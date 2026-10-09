/*
===========================================================================
This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Quake III Arena source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/

#include "tr_common.h"

#ifdef USE_WEBP
#include "src/webp/decode.h"

/*
=================
R_LoadWEBP

WebP images, lossy or lossless, with or without alpha
=================
*/
void R_LoadWEBP( const char *name, byte **pic, int *width, int *height )
{
	union {
		byte *b;
		void *v;
	} buffer;
	int length, w, h;
	byte *out;

	*pic = NULL;

	length = ri.FS_ReadFile( name, &buffer.v );
	if ( !buffer.b || length < 0 ) {
		return;
	}

	if ( !WebPGetInfo( buffer.b, length, &w, &h ) || w <= 0 || h <= 0 ) {
		ri.Printf( PRINT_WARNING, "WARNING: R_LoadWEBP: %s is not a valid WebP image\n", name );
		ri.FS_FreeFile( buffer.v );
		return;
	}

	out = ri.Malloc( w * h * 4 );
	if ( !WebPDecodeRGBAInto( buffer.b, length, out, w * h * 4, w * 4 ) ) {
		ri.Printf( PRINT_WARNING, "WARNING: R_LoadWEBP: failed to decode %s\n", name );
		ri.Free( out );
		ri.FS_FreeFile( buffer.v );
		return;
	}

	ri.FS_FreeFile( buffer.v );
	*pic = out;
	*width = w;
	*height = h;
}
#endif
