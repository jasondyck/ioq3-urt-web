/*
===========================================================================
WebXR (immersive VR) under Emscripten; see renderercommon/tr_webxr.h.

The head steers the game: its yaw is added to the view angles each frame and
its pitch is the view pitch, so the player aims where they look and the crosshair
stays in the middle of the view. The renderer then adds the head's roll, its
small movements and each eye's offset. Controller buttons press the keys of the
default UrT layout, so the player's binds still apply.
===========================================================================
*/

#include "client.h"

#ifdef __EMSCRIPTEN__

#include <emscripten/emscripten.h>

#define WEBXR_BTN_RTRIGGER	(1<<0)
#define WEBXR_BTN_RGRIP		(1<<1)
#define WEBXR_BTN_A			(1<<2)
#define WEBXR_BTN_B			(1<<3)
#define WEBXR_BTN_RSTICK	(1<<4)
#define WEBXR_BTN_LTRIGGER	(1<<5)
#define WEBXR_BTN_LGRIP		(1<<6)
#define WEBXR_BTN_X			(1<<7)
#define WEBXR_BTN_Y			(1<<8)
#define WEBXR_BTN_LSTICK	(1<<9)
#define WEBXR_BTN_RSTICK_UP	(1<<10)	// stick pushed, made by CL_WebXRFrame
#define WEBXR_BTN_RSTICK_DOWN	(1<<11)

static const struct {
	int		bit;
	int		key;
} webxrKeys[] = {
	{ WEBXR_BTN_RTRIGGER, K_MOUSE1 },	// fire; clicks in menus
	{ WEBXR_BTN_RGRIP, K_MOUSE2 },		// zoom
	{ WEBXR_BTN_A, K_SPACE },			// jump
	{ WEBXR_BTN_B, 'c' },				// crouch
	{ WEBXR_BTN_RSTICK, K_TAB },		// scores
	{ WEBXR_BTN_LTRIGGER, 'e' },		// sprint
	{ WEBXR_BTN_LGRIP, K_CTRL },		// interact / pick up
	{ WEBXR_BTN_X, 'r' },				// reload
	{ WEBXR_BTN_Y, 'q' },				// bandage
	{ WEBXR_BTN_LSTICK, K_ESCAPE },		// menu
	{ WEBXR_BTN_RSTICK_UP, K_MWHEELUP },	// next weapon
	{ WEBXR_BTN_RSTICK_DOWN, K_MWHEELDOWN },
};

static float webxrInput[WEBXR_IN_SIZE];
webxrView_t webxrView;

static cvar_t *xr_worldScale;
static cvar_t *xr_snapTurn;
static cvar_t *xr_turnSpeed;
static cvar_t *xr_hudSize;
static cvar_t *xr_hudDepth;
static cvar_t *xr_leanRange;

static struct {
	qboolean	tracking;		// had a head pose last frame
	float		lastYaw;
	float		yaw;			// head turn not yet in a usercmd
	vec3_t		center;			// where head movement is measured from
	int			buttons;
	qboolean	snapped;		// waiting for the stick to come back after a snap turn
	int			lastTime;
	float		cursor[2];		// sub-pixel cursor movement
} xr;

// Copies the headset state from the page; all zero outside of an XR frame.
EM_JS( void, CL_WebXRPoll, ( float *input, int count ), {
	const a = HEAPF32.subarray( input >> 2, ( input >> 2 ) + count );
	a.fill( 0 );
	if ( Module.urtXR ) Module.urtXR.poll( a );
} );

EM_JS( void, CL_WebXRCopyEye, ( int eye ), {
	if ( Module.urtXR ) Module.urtXR.copyEye( eye );
} );

qboolean CL_WebXRActive( void )
{
	return webxrInput[WEBXR_IN_FRAME] != 0.0f;
}

void CL_WebXRInit( void )
{
	xr_worldScale = Cvar_Get( "xr_worldScale", "32", CVAR_ARCHIVE );	// game units per meter
	xr_snapTurn = Cvar_Get( "xr_snapTurn", "45", CVAR_ARCHIVE );		// degrees, 0 turns smoothly
	xr_turnSpeed = Cvar_Get( "xr_turnSpeed", "150", CVAR_ARCHIVE );	// degrees per second when smooth
	xr_hudSize = Cvar_Get( "xr_hudSize", "0.5", CVAR_ARCHIVE );		// half height of the HUD, tangent of the angle
	xr_hudDepth = Cvar_Get( "xr_hudDepth", "0", CVAR_ARCHIVE );		// meters, 0 = at infinity like the crosshair's target
	xr_leanRange = Cvar_Get( "xr_leanRange", "0.3", CVAR_ARCHIVE );	// meters the view follows the head before staying put
}

// Quake axes (forward, left, up) from a WebXR vector (x right, y up, z back).
static void CL_WebXRToQuake( const float *v, vec3_t out )
{
	out[0] = -v[2];
	out[1] = -v[0];
	out[2] = v[1];
}

// Pitch, yaw and roll of the head, as AnglesToAxis would take them.
static void CL_WebXRHeadAngles( vec3_t angles )
{
	const float *q = webxrInput + WEBXR_IN_HEAD_QUAT;
	float x = q[0], y = q[1], z = q[2], w = q[3];
	float back[3], left[3], up[3];
	vec3_t f, l, u;

	back[0] = 2 * ( x * z + y * w );
	back[1] = 2 * ( y * z - x * w );
	back[2] = 1 - 2 * ( x * x + y * y );
	left[0] = -( 1 - 2 * ( y * y + z * z ) );
	left[1] = -( 2 * ( x * y + z * w ) );
	left[2] = -( 2 * ( x * z - y * w ) );
	up[0] = 2 * ( x * y - z * w );
	up[1] = 1 - 2 * ( x * x + z * z );
	up[2] = 2 * ( y * z + x * w );

	CL_WebXRToQuake( back, f );
	VectorNegate( f, f );
	CL_WebXRToQuake( left, l );
	CL_WebXRToQuake( up, u );

	angles[PITCH] = RAD2DEG( asin( Com_Clamp( -1.0f, 1.0f, -f[2] ) ) );
	angles[YAW] = RAD2DEG( atan2( f[1], f[0] ) );
	angles[ROLL] = RAD2DEG( atan2( l[2], u[2] ) );
}

static void CL_WebXRKey( int key, qboolean down )
{
	Com_QueueEvent( Sys_Milliseconds( ), SE_KEY, key, down, 0, NULL );
}

static void CL_WebXRSetButtons( int buttons )
{
	int i, changed = buttons ^ xr.buttons;

	for ( i = 0; i < ARRAY_LEN( webxrKeys ); i++ ) {
		if ( changed & webxrKeys[i].bit ) {
			CL_WebXRKey( webxrKeys[i].key, ( buttons & webxrKeys[i].bit ) != 0 );
		}
	}
	xr.buttons = buttons;
}

/*
=================
CL_WebXRFrame

Called once per engine frame, before the events are run.
=================
*/
void CL_WebXRFrame( void )
{
	const float *sticks = webxrInput + WEBXR_IN_STICKS;
	const float *pos = webxrInput + WEBXR_IN_HEAD_POS;
	int buttons, now = Sys_Milliseconds( );
	float seconds, range, d[3], len;
	qboolean menu;

	CL_WebXRPoll( webxrInput, WEBXR_IN_SIZE );
	if ( !CL_WebXRActive( ) ) {
		if ( xr.tracking ) {
			CL_WebXRSetButtons( 0 );	// nothing stays held when the session ends
			xr.tracking = qfalse;
		}
		webxrView.active = qfalse;
		return;
	}

	seconds = xr.tracking ? ( now - xr.lastTime ) * 0.001f : 0.0f;
	xr.lastTime = now;

	CL_WebXRHeadAngles( webxrView.headAngles );

	// The view follows the head for leaning and ducking, up to xr_leanRange from
	// a center that is dragged along beyond it (walking around the room).
	range = xr_leanRange->value;
	if ( !xr.tracking ) {
		VectorCopy( pos, xr.center );
	}
	VectorSubtract( pos, xr.center, d );
	len = sqrt( d[0] * d[0] + d[2] * d[2] );
	if ( len > range ) {
		xr.center[0] += d[0] * ( 1.0f - range / len );
		xr.center[2] += d[2] * ( 1.0f - range / len );
	}
	if ( fabs( d[1] ) > range ) {
		xr.center[1] += d[1] - ( d[1] > 0 ? range : -range );
	}
	VectorSubtract( pos, xr.center, d );
	CL_WebXRToQuake( d, webxrView.headShift );
	VectorScale( webxrView.headShift, xr_worldScale->value, webxrView.headShift );

	// buttons, and the right stick flicked up/down to change weapons
	menu = ( Key_GetCatcher( ) & ( KEYCATCH_UI | KEYCATCH_CONSOLE ) ) != 0;
	buttons = (int)webxrInput[WEBXR_IN_BUTTONS];
	if ( !menu ) {
		if ( sticks[3] < -0.7f || ( ( xr.buttons & WEBXR_BTN_RSTICK_UP ) && sticks[3] < -0.3f ) )
			buttons |= WEBXR_BTN_RSTICK_UP;
		if ( sticks[3] > 0.7f || ( ( xr.buttons & WEBXR_BTN_RSTICK_DOWN ) && sticks[3] > 0.3f ) )
			buttons |= WEBXR_BTN_RSTICK_DOWN;
	}
	CL_WebXRSetButtons( buttons );

	// in menus, either stick moves the cursor
	if ( menu && seconds > 0 ) {
		float x = sticks[0] + sticks[2], y = sticks[1] + sticks[3];
		int dx, dy;

		if ( fabs( x ) < 0.15f ) x = 0;
		if ( fabs( y ) < 0.15f ) y = 0;
		xr.cursor[0] += x * fabs( x ) * 900.0f * seconds;
		xr.cursor[1] += y * fabs( y ) * 900.0f * seconds;
		dx = (int)xr.cursor[0];
		dy = (int)xr.cursor[1];
		xr.cursor[0] -= dx;
		xr.cursor[1] -= dy;
		if ( dx || dy ) {
			Com_QueueEvent( now, SE_MOUSE, dx, dy, 0, NULL );
		}
	}

	if ( xr.tracking && clc.state == CA_ACTIVE ) {
		xr.yaw += AngleSubtract( webxrView.headAngles[YAW], xr.lastYaw );
	} else {
		xr.yaw = 0;
		xr.snapped = qfalse;
	}
	xr.lastYaw = webxrView.headAngles[YAW];
	xr.tracking = qtrue;
}

/*
=================
CL_WebXRMove

Head and stick steering for the usercmd being built.
=================
*/
void CL_WebXRMove( usercmd_t *cmd, int msec )
{
	const float *sticks = webxrInput + WEBXR_IN_STICKS;
	float x, y, turn;

	if ( !CL_WebXRActive( ) || !xr.tracking ) {
		return;
	}

	cl.viewangles[YAW] += xr.yaw;
	xr.yaw = 0;
	// pitch isn't summed: the server's delta (e.g. from spawning) would offset it
	cl.viewangles[PITCH] = webxrView.headAngles[PITCH] - SHORT2ANGLE( cl.snap.ps.delta_angles[PITCH] );

	if ( Key_GetCatcher( ) & ( KEYCATCH_UI | KEYCATCH_CONSOLE ) ) {
		return;
	}

	// right stick turns
	turn = sticks[2];
	if ( xr_snapTurn->value > 0 ) {
		if ( !xr.snapped && fabs( turn ) > 0.7f ) {
			cl.viewangles[YAW] -= turn > 0 ? xr_snapTurn->value : -xr_snapTurn->value;
			xr.snapped = qtrue;
		} else if ( fabs( turn ) < 0.3f ) {
			xr.snapped = qfalse;
		}
	} else if ( fabs( turn ) > 0.15f ) {
		cl.viewangles[YAW] -= turn * xr_turnSpeed->value * msec * 0.001f;
	}

	// left stick walks, relative to where the player looks
	x = sticks[0];
	y = sticks[1];
	if ( x * x + y * y > 0.15f * 0.15f ) {
		cmd->rightmove = ClampChar( cmd->rightmove + (int)( x * 127 ) );
		cmd->forwardmove = ClampChar( cmd->forwardmove - (int)( y * 127 ) );
	}
}

/*
=================
CL_WebXRBeginEye

Sets up the renderer for one eye, drawn to the whole canvas.
=================
*/
void CL_WebXRBeginEye( int eye )
{
	const float *e = webxrInput + WEBXR_IN_EYE + eye * WEBXR_IN_EYE_SIZE;
	float l = e[0], r = e[1], b = e[2], t = e[3];
	float hh = xr_hudSize->value, hw = hh * 4.0f / 3.0f, cx = 0, cy = 0;
	float w = cls.glconfig.vidWidth, h = cls.glconfig.vidHeight;
	int x0, x1, y0, y1;

	Com_Memcpy( webxrView.tan, e, sizeof( webxrView.tan ) );
	CL_WebXRToQuake( e + 4, webxrView.eyeOffset );
	VectorScale( webxrView.eyeOffset, xr_worldScale->value, webxrView.eyeOffset );

	// 2D (HUD, menus) in a 4:3 window straight ahead of the head; with a depth
	// the eyes see it from slightly different angles
	if ( xr_hudDepth->value > 0 ) {
		cx = -e[4] / xr_hudDepth->value;
		cy = -e[5] / xr_hudDepth->value;
	}
	x0 = (int)( ( cx - hw - l ) / ( r - l ) * w );
	x1 = (int)( ( cx + hw - l ) / ( r - l ) * w );
	y0 = (int)( ( cy - hh - b ) / ( t - b ) * h );
	y1 = (int)( ( cy + hh - b ) / ( t - b ) * h );
	webxrView.hud[0] = x0;
	webxrView.hud[1] = y0;
	webxrView.hud[2] = x1 - x0;
	webxrView.hud[3] = y1 - y0;

	webxrView.baseFov = Cvar_VariableValue( "cg_fov" );
	webxrView.active = qtrue;
}

void CL_WebXREndEye( int eye )
{
	webxrView.active = qfalse;
	CL_WebXRCopyEye( eye );
}

#endif
