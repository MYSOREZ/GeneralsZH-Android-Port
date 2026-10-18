/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

// GeneralsX @feature Android port 27/09/2026, shared and extended 05/10/2026 -- see
// W3DTouchButtonImages.h.
//
// The game has arrows of the right style only as 14x7 scroll-bar pieces, which turn into a smear
// when a 60x48 button stretches them, and shipping a texture would mean writing into the player's
// game folder. So the pictures are drawn here -- the way W3DRadar builds its images -- as a few
// convex polygons in cyan with a dark rim and a drop shadow, on the dark steel of a command
// button, with 4x4 supersampling so the edges stay smooth at the ~2.3x scale a command button is
// shown at on a phone.

#include <stdio.h>

#include "Lib/BaseType.h"
#include "Common/GameMemory.h"
#include "GameClient/Image.h"
#include "WW3D2/surfaceclass.h"
#include "WW3D2/texture.h"
#include "W3DDevice/GameClient/W3DTouchButtonImages.h"

#include <math.h>

namespace
{
	enum { TEX = 64, CELL_W = 60, CELL_H = 48, SS = 4, MAX_VERTS = 4, MAX_POLYS = 7 };

	struct Poly
	{
		Int count;
		Real x[ MAX_VERTS ];
		Real y[ MAX_VERTS ];
	};

	struct Icon
	{
		const char *name;
		Int count;
		Poly polys[ MAX_POLYS ];
	};

	// A command button is 60x48; every shape keeps clear of the 1-pixel frame and the 2-pixel
	// shadow.
	const Icon kIcons[] =
	{
		// "More orders": one triangle pointing down.
		{ "GXBuilderPageMore", 1, {
			{ 3, { 30, 13, 47 }, { 39, 9, 9 } } } },
		// "Back": one triangle pointing up.
		{ "GXBuilderPageBack", 1, {
			{ 3, { 30, 13, 47 }, { 9, 39, 39 } } } },
		// Scatter: four arrowheads flying out of a centre point.
		{ "GXScatter", 5, {
			{ 4, { 26, 34, 34, 26 }, { 20, 20, 28, 28 } },
			{ 3, { 30, 21, 39 }, { 4, 15, 15 } },
			{ 3, { 30, 39, 21 }, { 44, 33, 33 } },
			{ 3, { 7, 19, 19 }, { 24, 15, 33 } },
			{ 3, { 53, 41, 41 }, { 24, 33, 15 } } } },
		// Force attack: four arrowheads closing on a centre point -- a crosshair. Generals has no
		// SCCAttack picture (the base game's cursors are animated files), and its mapped image name
		// drew as a purple square there (owner, a29).
		{ "GXForceAttack", 5, {
			{ 4, { 28, 32, 32, 28 }, { 22, 22, 26, 26 } },
			{ 3, { 30, 37, 23 }, { 18, 5, 5 } },
			{ 3, { 30, 23, 37 }, { 30, 43, 43 } },
			{ 3, { 22, 9, 9 }, { 24, 31, 17 } },
			{ 3, { 38, 51, 51 }, { 24, 17, 31 } } } },
		// Formation: two ranks of three, with the heading above them.
		{ "GXFormation", 7, {
			{ 3, { 30, 21, 39 }, { 4, 12, 12 } },
			{ 4, { 13, 21, 21, 13 }, { 17, 17, 25, 25 } },
			{ 4, { 26, 34, 34, 26 }, { 17, 17, 25, 25 } },
			{ 4, { 39, 47, 47, 39 }, { 17, 17, 25, 25 } },
			{ 4, { 13, 21, 21, 13 }, { 31, 31, 39, 39 } },
			{ 4, { 26, 34, 34, 26 }, { 31, 31, 39, 39 } },
			{ 4, { 39, 47, 47, 39 }, { 31, 31, 39, 39 } } } },
	};

	// How far (px,py) is inside the convex polygon, shifted by (dx,dy); negative outside.
	Real insidePoly( const Poly &p, Real px, Real py, Real dx, Real dy )
	{
		// Winding sign so that "inside" is positive for every edge.
		Real area = 0;
		for( Int e = 0; e < p.count; ++e )
		{
			const Int n = ( e + 1 ) % p.count;
			area += p.x[ e ] * p.y[ n ] - p.x[ n ] * p.y[ e ];
		}
		const Real sign = area > 0 ? -1.0f : 1.0f;

		Real d = 1e9f;
		for( Int e = 0; e < p.count; ++e )
		{
			const Int n = ( e + 1 ) % p.count;
			const Real ax = p.x[ e ] + dx, ay = p.y[ e ] + dy;
			const Real ex = p.x[ n ] - p.x[ e ], ey = p.y[ n ] - p.y[ e ];
			const Real len = sqrtf( ex * ex + ey * ey );
			d = min( d, sign * ( ( px - ax ) * ey - ( py - ay ) * ex ) / len );
		}
		return d;
	}

	// The deepest the point is inside any of the icon's polygons.
	Real insideIcon( const Icon &icon, Real px, Real py, Real dx, Real dy )
	{
		Real d = -1e9f;
		for( Int i = 0; i < icon.count; ++i )
			d = max( d, insidePoly( icon.polys[ i ], px, py, dx, dy ) );
		return d;
	}

	void registerIcon( const Icon &icon )
	{
		if( TheMappedImageCollection->findImageByName( icon.name ) != nullptr )
			return;

		TextureClass *texture = MSGNEW("TextureClass") TextureClass( TEX, TEX, WW3D_FORMAT_A8R8G8B8, MIP_LEVELS_1 );
		SurfaceClass *surface = texture ? texture->Get_Surface_Level() : nullptr;
		if( surface == nullptr || surface->Get_Bytes_Per_Pixel() != 4 )
		{
			fprintf( stderr, "[touchmodes] could not create the %s texture\n", icon.name );
			REF_PTR_RELEASE( surface );
			REF_PTR_RELEASE( texture );
			return;
		}

		int pitch = 0;
		UnsignedByte *bits = (UnsignedByte *)surface->Lock( &pitch );
		if( bits == nullptr )
		{
			REF_PTR_RELEASE( surface );
			REF_PTR_RELEASE( texture );
			return;
		}

		for( Int y = 0; y < TEX; ++y )
		{
			UnsignedInt *row = (UnsignedInt *)( bits + y * pitch );
			for( Int x = 0; x < TEX; ++x )
			{
				if( x >= CELL_W || y >= CELL_H )
				{
					row[ x ] = 0;
					continue;
				}
				Real r = 0, g = 0, b = 0;
				for( Int sy = 0; sy < SS; ++sy )
				{
					for( Int sx = 0; sx < SS; ++sx )
					{
						const Real px = x + ( sx + 0.5f ) / SS;
						const Real py = y + ( sy + 0.5f ) / SS;

						// steel background, lighter at the top
						const Real shade = 62.0f - 34.0f * py / CELL_H;
						Real cr = shade - 22.0f, cg = shade - 8.0f, cb = shade + 30.0f;

						// drop shadow
						if( insideIcon( icon, px, py, 2.0f, 2.0f ) >= 0 )
						{
							const Real a = 170.0f / 255.0f;
							cr *= 1 - a; cg *= 1 - a; cb *= 1 - a;
						}

						// shape: dark rim one pixel wide, cyan fill
						const Real d = insideIcon( icon, px, py, 0, 0 );
						if( d >= 1.0f )
						{
							cr = 90; cg = 215; cb = 255;
						}
						else if( d >= 0 )
						{
							cr = 30; cg = 110; cb = 170;
						}

						// button frame
						if( px < 1.0f || py < 1.0f || px >= CELL_W - 1.0f || py >= CELL_H - 1.0f )
						{
							cr = 120; cg = 140; cb = 190;
						}

						r += cr; g += cg; b += cb;
					}
				}
				const Real n = (Real)( SS * SS );
				const UnsignedInt R = (UnsignedInt)clamp( 0.0f, r / n, 255.0f );
				const UnsignedInt G = (UnsignedInt)clamp( 0.0f, g / n, 255.0f );
				const UnsignedInt B = (UnsignedInt)clamp( 0.0f, b / n, 255.0f );
				row[ x ] = 0xFF000000u | ( R << 16 ) | ( G << 8 ) | B;
			}
		}
		surface->Unlock();
		REF_PTR_RELEASE( surface );

		Image *image = newInstance(Image);
		image->setName( icon.name );
		image->setStatus( IMAGE_STATUS_RAW_TEXTURE );
		image->setRawTextureData( texture );	// the image keeps the reference
		Region2D uv;
		uv.lo.x = 0.0f;
		uv.lo.y = 0.0f;
		uv.hi.x = (Real)CELL_W / TEX;
		uv.hi.y = (Real)CELL_H / TEX;
		image->setUV( &uv );
		image->setTextureWidth( TEX );
		image->setTextureHeight( TEX );
		ICoord2D size;
		size.x = CELL_W;
		size.y = CELL_H;
		image->setImageSize( &size );
		TheMappedImageCollection->addImage( image );
	}
}

// GeneralsX @feature Android port 18/10/2026 The army column's button face (GroupPanel.cpp).
//
// Owner, a30: the column's flat rectangles looked old-fashioned and unlike a game's buttons. This is
// one face in three states -- rest, hilite, pressed -- drawn like the icons above: a rounded plate
// of dark gunmetal, lighter at the top with a gloss band, a thin dark line inside a two-pixel trim
// in the warm gold of the game's own frames, opaque (the column has its own backing plate). The
// column stretches it over each button.
namespace
{
	enum { FACE_TEX_W = 128, FACE_TEX_H = 64, FACE_W = 128, FACE_H = 56 };

	struct FaceStyle
	{
		const char *name;
		Real top[ 3 ], bottom[ 3 ];
		Real alpha;
	};

	const FaceStyle kFaces[] =
	{
		{ "GXPanelButton",        { 58, 68, 80 },  { 20, 26, 34 }, 1.0f },
		{ "GXPanelButtonHilite",  { 82, 98, 116 }, { 32, 42, 54 }, 1.0f },
		{ "GXPanelButtonPressed", { 18, 24, 32 },  { 46, 56, 68 }, 1.0f },
	};

	// Signed distance to a rounded rectangle [0,w]x[0,h] with corner radius r; positive inside.
	Real insideRounded( Real px, Real py, Real w, Real h, Real r )
	{
		const Real cx = max( r, min( px, w - r ) );
		const Real cy = max( r, min( py, h - r ) );
		const Real dx = px - cx, dy = py - cy;
		if( px >= r && px <= w - r )
			return min( py, h - py );
		if( py >= r && py <= h - r )
			return min( px, w - px );
		return r - sqrtf( dx * dx + dy * dy );
	}

	void registerFace( const FaceStyle &face )
	{
		if( TheMappedImageCollection->findImageByName( face.name ) != nullptr )
			return;

		TextureClass *texture = MSGNEW("TextureClass") TextureClass( FACE_TEX_W, FACE_TEX_H, WW3D_FORMAT_A8R8G8B8, MIP_LEVELS_1 );
		SurfaceClass *surface = texture ? texture->Get_Surface_Level() : nullptr;
		if( surface == nullptr || surface->Get_Bytes_Per_Pixel() != 4 )
		{
			fprintf( stderr, "[touchmodes] could not create the %s texture\n", face.name );
			REF_PTR_RELEASE( surface );
			REF_PTR_RELEASE( texture );
			return;
		}
		int pitch = 0;
		UnsignedByte *bits = (UnsignedByte *)surface->Lock( &pitch );
		if( bits == nullptr )
		{
			REF_PTR_RELEASE( surface );
			REF_PTR_RELEASE( texture );
			return;
		}

		const Real radius = 9.0f;
		for( Int y = 0; y < FACE_TEX_H; ++y )
		{
			UnsignedInt *row = (UnsignedInt *)( bits + y * pitch );
			for( Int x = 0; x < FACE_TEX_W; ++x )
			{
				if( y >= FACE_H )
				{
					row[ x ] = 0;
					continue;
				}
				Real r = 0, g = 0, b = 0, a = 0;
				for( Int sy = 0; sy < SS; ++sy )
				{
					for( Int sx = 0; sx < SS; ++sx )
					{
						const Real px = x + ( sx + 0.5f ) / SS;
						const Real py = y + ( sy + 0.5f ) / SS;
						const Real d = insideRounded( px, py, (Real)FACE_W, (Real)FACE_H, radius );
						if( d < 0 )
							continue;
						const Real t = py / FACE_H;
						Real cr, cg, cb, ca;
						if( d < 2.0f )
						{
							// the gold trim, brighter along the top
							const Real k = 1.0f - 0.45f * t;
							cr = 205 * k; cg = 176 * k; cb = 112 * k; ca = 1.0f;
						}
						else if( d < 3.0f )
						{
							cr = 8; cg = 11; cb = 15; ca = 1.0f;
						}
						else
						{
							cr = face.top[ 0 ] + ( face.bottom[ 0 ] - face.top[ 0 ] ) * t;
							cg = face.top[ 1 ] + ( face.bottom[ 1 ] - face.top[ 1 ] ) * t;
							cb = face.top[ 2 ] + ( face.bottom[ 2 ] - face.top[ 2 ] ) * t;
							// gloss on the upper part, fading out
							if( t < 0.45f )
							{
								const Real gloss = 22.0f * ( 1.0f - t / 0.45f );
								cr += gloss; cg += gloss; cb += gloss;
							}
							ca = face.alpha;
						}
						r += cr * ca; g += cg * ca; b += cb * ca; a += ca;
					}
				}
				const Real n = (Real)( SS * SS );
				if( a <= 0.0f )
				{
					row[ x ] = 0;
					continue;
				}
				// colour un-premultiplied by the covered samples' alpha, alpha by coverage
				const UnsignedInt R = (UnsignedInt)clamp( 0.0f, r / a, 255.0f );
				const UnsignedInt G = (UnsignedInt)clamp( 0.0f, g / a, 255.0f );
				const UnsignedInt B = (UnsignedInt)clamp( 0.0f, b / a, 255.0f );
				const UnsignedInt A = (UnsignedInt)clamp( 0.0f, 255.0f * a / n, 255.0f );
				row[ x ] = ( A << 24 ) | ( R << 16 ) | ( G << 8 ) | B;
			}
		}
		surface->Unlock();
		REF_PTR_RELEASE( surface );

		Image *image = newInstance(Image);
		image->setName( face.name );
		image->setStatus( IMAGE_STATUS_RAW_TEXTURE );
		image->setRawTextureData( texture );
		Region2D uv;
		uv.lo.x = 0.0f;
		uv.lo.y = 0.0f;
		uv.hi.x = (Real)FACE_W / FACE_TEX_W;
		uv.hi.y = (Real)FACE_H / FACE_TEX_H;
		image->setUV( &uv );
		image->setTextureWidth( FACE_TEX_W );
		image->setTextureHeight( FACE_TEX_H );
		ICoord2D size;
		size.x = FACE_W;
		size.y = FACE_H;
		image->setImageSize( &size );
		TheMappedImageCollection->addImage( image );
	}
}

void W3DRegisterTouchButtonImages()
{
	if( TheMappedImageCollection == nullptr )
		return;
	for( size_t i = 0; i < ARRAY_SIZE( kIcons ); ++i )
		registerIcon( kIcons[ i ] );
	for( size_t i = 0; i < ARRAY_SIZE( kFaces ); ++i )
		registerFace( kFaces[ i ] );
}
