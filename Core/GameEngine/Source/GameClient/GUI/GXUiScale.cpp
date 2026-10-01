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


// GeneralsX @feature Android port 01/10/2026 Interface scale -- see GXUiScale.h.

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "GameClient/GXUiScale.h"
#include "GameClient/Display.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace
{
	// Which layouts are scaled, and how far. maxFracX/maxFracY cap the share of the screen the
	// layout's content may take after scaling (the control bar must leave the battlefield room);
	// exclude lists windows (by the part of NAME after the colon, a trailing '*' matching any rest)
	// that keep their place and size, with everything inside them.
	struct Rule
	{
		const char *file;
		Real maxFracX;
		Real maxFracY;
		Bool uniform;  // the same factor on both axes (anything but the full-width control bar)
		const char *exclude[ 12 ];
	};

	const Rule kRules[] =
	{
		// In game.
		{ "ControlBar.wnd",                 1.0f, 0.45f, FALSE, { nullptr } },
		{ "ControlBarPopupDescription.wnd", 1.0f, 1.0f, TRUE, { nullptr } },
		{ "GenPowersShortcutBarUS.wnd",     1.0f, 1.0f, TRUE, { nullptr } },
		{ "GenPowersShortcutBarChina.wnd",  1.0f, 1.0f, TRUE, { nullptr } },
		{ "GenPowersShortcutBarGLA.wnd",    1.0f, 1.0f, TRUE, { nullptr } },
		{ "GeneralsExpPoints.wnd",          1.0f, 1.0f, TRUE, { nullptr } },
		{ "InGameChat.wnd",                 1.0f, 1.0f, TRUE, { nullptr } },
		{ "Diplomacy.wnd",                  1.0f, 1.0f, TRUE, { nullptr } },
		{ "QuitMenu.wnd",                   1.0f, 1.0f, TRUE, { nullptr } },
		{ "QuitMessageBox.wnd",             1.0f, 1.0f, TRUE, { nullptr } },
		{ "QuitNoSave.wnd",                 1.0f, 1.0f, TRUE, { nullptr } },
		{ "PopupSaveLoad.wnd",              1.0f, 1.0f, TRUE, { nullptr } },
		// Shell and both.
		{ "OptionsMenu.wnd",                1.0f, 1.0f, TRUE, { nullptr } },
		{ "MessageBox.wnd",                 1.0f, 1.0f, TRUE, { nullptr } },
		{ "DifficultySelect.wnd",           1.0f, 1.0f, TRUE, { nullptr } },
		// The main menu's buttons, not its decoration: the logo, the faction pictures the single
		// player menu grows, the clock and the download buttons stay where they are.
		{ "MainMenu.wnd",                   1.0f, 1.0f, TRUE,
			{ "Logo", "WinFaction*", "WinGrowMarker", "GreenDot", "Clock", "ButtonGetMapPack",
			  "ButtonGetUpdate", "ShellMenuScheme", nullptr } },
	};

	const char *baseName( const char *path )
	{
		const char *b = path;
		for( const char *p = path; *p; ++p )
			if( *p == '\\' || *p == '/' )
				b = p + 1;
		return b;
	}

	const Rule *findRule( const char *layoutFile )
	{
		const char *b = baseName( layoutFile );
		for( size_t i = 0; i < sizeof( kRules ) / sizeof( kRules[ 0 ] ); ++i )
			if( strcasecmp( b, kRules[ i ].file ) == 0 )
				return &kRules[ i ];
		return nullptr;
	}

	Bool excluded( const Rule &rule, const std::string &name )
	{
		for( Int i = 0; i < 12 && rule.exclude[ i ]; ++i )
		{
			const char *e = rule.exclude[ i ];
			const size_t n = strlen( e );
			if( n > 0 && e[ n - 1 ] == '*' )
			{
				if( name.compare( 0, n - 1, e, n - 1 ) == 0 )
					return TRUE;
			}
			else if( name == e )
				return TRUE;
		}
		return FALSE;
	}

	struct Parsing
	{
		std::vector<Bool> scaled;  // per window, in file order
		Int next;
		GXUiScale::Transform transform;
	};

	const GXUiScale::Transform kIdentity = { FALSE, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f };
	std::map<std::string, GXUiScale::Transform> s_transforms;
	std::vector<Parsing> s_parsing;

	// The axis transform for content [lo, hi] of an axis `size` long: scale k around the edge the
	// content sits on (or its centre), then shift it back inside [0, size].
	void axisTransform( Real lo, Real hi, Real size, Real k, Real *outK, Real *outAdd )
	{
		const Real gapLo = lo;
		const Real gapHi = size - hi;
		Real anchor = ( lo + hi ) * 0.5f;
		if( gapHi <= size * 0.08f && gapHi < gapLo )
			anchor = hi;
		else if( gapLo <= size * 0.08f && gapLo < gapHi )
			anchor = lo;
		Real add = anchor * ( 1.0f - k );
		const Real newLo = lo * k + add;
		const Real newHi = hi * k + add;
		if( newLo < 0.0f )
			add -= newLo;
		else if( newHi > size )
			add -= newHi - size;
		*outK = k;
		*outAdd = add / size;
	}
}

namespace GXUiScale
{

Real Transform::mapX( Real x ) const
{
	return active ? x * kx + addX * (Real)TheDisplay->getWidth() : x;
}

Real Transform::mapY( Real y ) const
{
	return active ? y * ky + addY * (Real)TheDisplay->getHeight() : y;
}

Real userScale()
{
	static Real s_scale = -1.0f;
	if( s_scale < 0.0f )
	{
		s_scale = 1.0f;
		const char *env = getenv( "GX_UI_SCALE" );
		if( env )
		{
			const Int percent = atoi( env );
			if( percent > 100 && percent <= 300 )
				s_scale = percent / 100.0f;
		}
	}
	return s_scale;
}

const Transform &forLayout( const char *layoutFile )
{
	std::map<std::string, Transform>::const_iterator it = s_transforms.find( baseName( layoutFile ) );
	return it != s_transforms.end() ? it->second : kIdentity;
}

void beginLayout( const char *layoutFile, const char *text, Int length )
{
	Parsing p;
	p.next = 0;
	p.transform = kIdentity;

	const Rule *rule = findRule( layoutFile );
	const Real k = userScale();
	if( rule && k > 1.0f && text && length > 0 && TheDisplay )
	{
		// The layout's windows in file order: rectangle (creation coordinates), name, depth.
		struct Win { Real lo[ 2 ], hi[ 2 ], res[ 2 ]; std::string name; Int depth; Bool hasRect; };
		std::vector<Win> wins;
		Int depth = 0;
		const char *end = text + length;
		const char *line = text;
		while( line < end )
		{
			const char *eol = line;
			while( eol < end && *eol != '\n' )
				++eol;
			std::string l( line, eol - line );
			line = eol + 1;
			const size_t first = l.find_first_not_of( " \t\r" );
			if( first == std::string::npos )
				continue;
			l.erase( 0, first );
			while( !l.empty() && ( l.back() == '\r' || l.back() == ' ' || l.back() == '\t' ) )
				l.pop_back();
			if( l == "WINDOW" )
			{
				Win w = {};
				w.depth = ++depth;
				wins.push_back( w );
			}
			else if( l == "END" )
				--depth;
			else if( !wins.empty() )
			{
				Win &w = wins.back();
				Int a = 0, b = 0;
				if( sscanf( l.c_str(), "SCREENRECT = UPPERLEFT: %d %d", &a, &b ) == 2 )
				{
					w.lo[ 0 ] = (Real)a; w.lo[ 1 ] = (Real)b;
				}
				else if( sscanf( l.c_str(), "BOTTOMRIGHT: %d %d", &a, &b ) == 2 )
				{
					w.hi[ 0 ] = (Real)a; w.hi[ 1 ] = (Real)b;
				}
				else if( sscanf( l.c_str(), "CREATIONRESOLUTION: %d %d", &a, &b ) == 2 && a > 0 && b > 0 )
				{
					w.res[ 0 ] = (Real)a; w.res[ 1 ] = (Real)b;
					w.hasRect = TRUE;
				}
				else if( l.compare( 0, 8, "NAME = \"" ) == 0 && w.name.empty() )
				{
					std::string n = l.substr( 8 );
					const size_t q = n.find( '"' );
					if( q != std::string::npos )
						n.erase( q );
					const size_t colon = n.find( ':' );
					w.name = colon != std::string::npos ? n.substr( colon + 1 ) : n;
				}
			}
		}

		// What scales: everything but full-screen containers and the rule's exclusions (with
		// whatever they contain). The content box is what scales, normalized to the screen.
		// One decision per window that has a rectangle: parseScreenRect is called once for each of
		// those, in file order.
		p.scaled.clear();
		Real box[ 2 ][ 2 ] = { { 1.0f, 0.0f }, { 1.0f, 0.0f } };
		Bool any = FALSE;
		Int excludedDepth = 0;
		for( size_t i = 0; i < wins.size(); ++i )
		{
			const Win &w = wins[ i ];
			if( !w.hasRect )
				continue;
			if( excludedDepth > 0 && w.depth > excludedDepth )
			{
				p.scaled.push_back( FALSE );
				continue;
			}
			excludedDepth = 0;
			if( excluded( *rule, w.name ) )
			{
				excludedDepth = w.depth;
				p.scaled.push_back( FALSE );
				continue;
			}
			const Real fx0 = w.lo[ 0 ] / w.res[ 0 ], fx1 = w.hi[ 0 ] / w.res[ 0 ];
			const Real fy0 = w.lo[ 1 ] / w.res[ 1 ], fy1 = w.hi[ 1 ] / w.res[ 1 ];
			if( fx1 - fx0 >= 0.95f && fy1 - fy0 >= 0.95f )
			{
				p.scaled.push_back( FALSE );
				continue;
			}
			p.scaled.push_back( TRUE );
			any = TRUE;
			box[ 0 ][ 0 ] = fminf( box[ 0 ][ 0 ], fx0 ); box[ 0 ][ 1 ] = fmaxf( box[ 0 ][ 1 ], fx1 );
			box[ 1 ][ 0 ] = fminf( box[ 1 ][ 0 ], fy0 ); box[ 1 ][ 1 ] = fmaxf( box[ 1 ][ 1 ], fy1 );
		}

		if( any )
		{
			// Per axis: as much of the asked scale as fits.
			const Real maxFrac[ 2 ] = { rule->maxFracX, rule->maxFracY };
			Real axisK[ 2 ], axisAdd[ 2 ], fit[ 2 ];
			for( Int a = 0; a < 2; ++a )
			{
				box[ a ][ 0 ] = fmaxf( box[ a ][ 0 ], 0.0f );
				box[ a ][ 1 ] = fminf( box[ a ][ 1 ], 1.0f );
				const Real extent = fmaxf( box[ a ][ 1 ] - box[ a ][ 0 ], 0.001f );
				fit[ a ] = fmaxf( 1.0f, fminf( k, maxFrac[ a ] / extent ) );
			}
			if( rule->uniform )
				fit[ 0 ] = fit[ 1 ] = fminf( fit[ 0 ], fit[ 1 ] );
			for( Int a = 0; a < 2; ++a )
				axisTransform( box[ a ][ 0 ], box[ a ][ 1 ], 1.0f, fit[ a ], &axisK[ a ], &axisAdd[ a ] );
			Transform &t = p.transform;
			t.active = axisK[ 0 ] > 1.001f || axisK[ 1 ] > 1.001f;
			t.kx = axisK[ 0 ];
			t.ky = axisK[ 1 ];
			t.addX = axisAdd[ 0 ];
			t.addY = axisAdd[ 1 ];
			// Text grows with the layout's height, as far as its width -- already stretched by the
			// screen being wider than 4:3 -- leaves room for.
			const Real aspectRoom = ( (Real)TheDisplay->getWidth() / 800.0f ) / ( (Real)TheDisplay->getHeight() / 600.0f );
			t.fontK = fmaxf( 1.0f, fminf( t.ky, t.kx * fmaxf( aspectRoom, 1.0f ) ) );
			Int scaledCount = 0;
			for( size_t i = 0; i < p.scaled.size(); ++i )
				scaledCount += p.scaled[ i ] ? 1 : 0;
			if( t.active )
				fprintf( stderr, "[GX-UISCALE] %s: x%.2f y%.2f (asked %.2f), fonts x%.2f, %d of %d windows\n",
					baseName( layoutFile ), t.kx, t.ky, k, t.fontK,
					scaledCount, (Int)wins.size() );
		}
		s_transforms[ baseName( layoutFile ) ] = p.transform;
	}
	s_parsing.push_back( p );
}

void endLayout()
{
	if( !s_parsing.empty() )
		s_parsing.pop_back();
}

void mapNextWindowRect( Int *loX, Int *loY, Int *hiX, Int *hiY )
{
	if( s_parsing.empty() )
		return;
	Parsing &p = s_parsing.back();
	const Int index = p.next++;
	if( !p.transform.active || index >= (Int)p.scaled.size() || !p.scaled[ index ] )
		return;
	const Transform &t = p.transform;
	*loX = (Int)floorf( t.mapX( (Real)*loX ) + 0.5f );
	*hiX = (Int)floorf( t.mapX( (Real)*hiX ) + 0.5f );
	*loY = (Int)floorf( t.mapY( (Real)*loY ) + 0.5f );
	*hiY = (Int)floorf( t.mapY( (Real)*hiY ) + 0.5f );
}

Int scaleFontSize( Int size )
{
	if( s_parsing.empty() )
		return size;
	const Parsing &p = s_parsing.back();
	const Int index = p.next - 1;
	if( !p.transform.active || index < 0 || index >= (Int)p.scaled.size() || !p.scaled[ index ] )
		return size;
	return (Int)floorf( size * p.transform.fontK + 0.5f );
}

} // namespace GXUiScale
