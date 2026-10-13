/*
===========================================================================
WebXR (immersive VR) under Emscripten.

The page owns the XRSession. On each XR animation frame it runs one engine
frame, which first reads the headset and controller state from it (laid out as
below). The client then draws the screen once per eye into the canvas, and the
page copies each one into its half of the XR framebuffer. While an eye is being
drawn, webxrView tells the renderer how to turn the game's view into that eye.
===========================================================================
*/

#ifndef TR_WEBXR_H
#define TR_WEBXR_H

#ifdef __EMSCRIPTEN__

// headset state layout, shared with the page (client/xr.js)
#define WEBXR_IN_FRAME		0	// 1 while inside an XR animation frame
#define WEBXR_IN_HEAD_QUAT	1	// x y z w, reference space
#define WEBXR_IN_HEAD_POS	5	// x y z, meters, reference space
#define WEBXR_IN_EYE		8	// per eye: tan left/right/down/up, then the
#define WEBXR_IN_EYE_SIZE	7	//   eye's offset from the head, head frame, meters
#define WEBXR_IN_STICKS		22	// left x y, right x y; -1..1, y down positive
#define WEBXR_IN_BUTTONS	26	// WEBXR_BTN_* bits
#define WEBXR_IN_SIZE		32

typedef struct {
	qboolean	active;		// an eye is being drawn
	float		tan[4];		// frustum tangents: left, right, down, up
	float		headAngles[3];	// pitch yaw roll of the head, reference space
	vec3_t		headShift;	// head position, reference space turned Quake-wise (forward left up), game units
	vec3_t		eyeOffset;	// eye relative to the head, forward left up, game units
	int			hud[4];		// where 2D goes: x y w h, GL window coordinates
	float		baseFov;	// cg_fov: a narrower refdef is a scope zooming in
} webxrView_t;

extern webxrView_t webxrView;

#endif

#endif
