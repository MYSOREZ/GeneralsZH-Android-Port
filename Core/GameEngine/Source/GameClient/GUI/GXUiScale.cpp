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
#include "Common/FileSystem.h"
#include "Common/file.h"

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
		// Another layout whose transform this one takes as it is, instead of its own: a panel that
		// is part of that layout on screen and must stay where it sits on it.
		const char *sharedWith;
		// A layout this one sits on top of (the control bar): its content's bottom edge goes where
		// that layout's transform puts the same screen height, and it grows upward from there.
		const char *restsOn;
		// Windows (NAME prefix) of the exclude list that are moved off the screen when the scaled
		// content would cover them: decoration that has no room left (the main menu's faction
		// emblems, under the grown single player menu).
		const char *moveAwayPrefix;
	};

	// "WinFactionUSMedium" -> "WinFactionUS": a window and the Small / Medium copies the menu's
	// transitions grow it through belong together.
	std::string factionGroup( const std::string &name )
	{
		static const char *suffixes[] = { "Small", "Medium" };
		for( Int i = 0; i < 2; ++i )
		{
			const size_t n = strlen( suffixes[ i ] );
			if( name.size() > n && name.compare( name.size() - n, n, suffixes[ i ] ) == 0 )
				return name.substr( 0, name.size() - n );
		}
		return name;
	}

	const Rule kRules[] =
	{
		// In game.
		{ "ControlBar.wnd",                 1.0f, 0.40f, FALSE, { nullptr } },
		// This port's control group row (Window\\GroupPanel.wnd) rides on the control bar.
		{ "GroupPanel.wnd",                 1.0f, 1.0f, FALSE, { nullptr }, "ControlBar.wnd" },
		{ "ControlBarPopupDescription.wnd", 1.0f, 1.0f, TRUE, { nullptr } },
		// The generals' power buttons stack up from just above the control bar; scaled around their
		// own box they slid down onto it (owner's photo, 150%).
		{ "GenPowersShortcutBarUS.wnd",     1.0f, 1.0f, TRUE, { nullptr }, nullptr, "ControlBar.wnd" },
		{ "GenPowersShortcutBarChina.wnd",  1.0f, 1.0f, TRUE, { nullptr }, nullptr, "ControlBar.wnd" },
		{ "GenPowersShortcutBarGLA.wnd",    1.0f, 1.0f, TRUE, { nullptr }, nullptr, "ControlBar.wnd" },
		{ "GeneralsExpPoints.wnd",          1.0f, 1.0f, TRUE, { nullptr } },
		{ "InGameChat.wnd",                 1.0f, 1.0f, TRUE, { nullptr }, nullptr, "ControlBar.wnd" },
		{ "Diplomacy.wnd",                  1.0f, 1.0f, TRUE, { nullptr } },
		{ "QuitMenu.wnd",                   1.0f, 1.0f, TRUE, { nullptr } },
		{ "QuitMessageBox.wnd",             1.0f, 1.0f, TRUE, { nullptr } },
		{ "QuitNoSave.wnd",                 1.0f, 1.0f, TRUE, { nullptr } },
		{ "PopupSaveLoad.wnd",              1.0f, 1.0f, TRUE, { nullptr } },
		// Shell and both.
		{ "OptionsMenu.wnd",                1.0f, 1.0f, TRUE, { nullptr } },
		{ "MessageBox.wnd",                 1.0f, 1.0f, TRUE, { nullptr } },
		{ "DifficultySelect.wnd",           1.0f, 1.0f, TRUE, { nullptr } },
		// The main menu's buttons with the logo above them (scaled apart, the buttons grew into the
		// logo), not the rest of its decoration: the clock and the download buttons stay where they
		// are, and the faction emblems the single player menu shows along the bottom go when the
		// grown menu reaches down over them (they overlapped its lower buttons, owner's photo).
		{ "MainMenu.wnd",                   1.0f, 1.0f, TRUE,
			{ "WinFaction*", "WinGrowMarker", "GreenDot", "Clock", "ButtonGetMapPack",
			  "ButtonGetUpdate", "ShellMenuScheme", nullptr }, nullptr, nullptr, "WinFaction" },
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
		std::vector<Int> scaled;  // per window with a rectangle, in file order: 0 keep, 1 scale, 2 move off screen, 3 move by offset
		std::vector<Real> offset; // decision 3: x, y per window (fractions of the screen)
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

} // namespace GXUiScale

static const GXUiScale::Transform &ensureLayoutTransform( const char *layoutFile );

// Which windows of a layout scale, and its own transform, from the layout's text.
static void analyzeLayout( const Rule *rule, const char *layoutFile, const char *text, Int length, Parsing &p )
{
	const Real k = GXUiScale::userScale();
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
				// The three parts of SCREENRECT, wherever they are: the game's own layouts put each
				// on a line of its own, this port's GroupPanel.wnd all three on one (which this used
				// to miss, so the group row never scaled -- logs-50, handle still at y 734).
				Int a = 0, b = 0;
				const char *t = nullptr;
				if( ( t = strstr( l.c_str(), "UPPERLEFT:" ) ) != nullptr && sscanf( t, "UPPERLEFT: %d %d", &a, &b ) == 2 )
				{
					w.lo[ 0 ] = (Real)a; w.lo[ 1 ] = (Real)b;
				}
				if( ( t = strstr( l.c_str(), "BOTTOMRIGHT:" ) ) != nullptr && sscanf( t, "BOTTOMRIGHT: %d %d", &a, &b ) == 2 )
				{
					w.hi[ 0 ] = (Real)a; w.hi[ 1 ] = (Real)b;
				}
				if( ( t = strstr( l.c_str(), "CREATIONRESOLUTION:" ) ) != nullptr && sscanf( t, "CREATIONRESOLUTION: %d %d", &a, &b ) == 2 && a > 0 && b > 0 )
				{
					w.res[ 0 ] = (Real)a; w.res[ 1 ] = (Real)b;
					w.hasRect = TRUE;
				}
				if( l.compare( 0, 8, "NAME = \"" ) == 0 && w.name.empty() )
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
		struct MoveAway { size_t index; Real r[ 4 ]; std::string group; Bool plain; };
		std::vector<MoveAway> moveAway;
		// Windows inside an excluded one follow it if it moves: decision of index `inherit[ i ]`.
		std::vector<Int> inherit;
		Int excludedIndex = -1;
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
				inherit.push_back( excludedIndex );
				p.scaled.push_back( 0 );
				continue;
			}
			excludedDepth = 0;
			excludedIndex = -1;
			if( excluded( *rule, w.name ) )
			{
				excludedDepth = w.depth;
				excludedIndex = (Int)p.scaled.size();
				if( rule->moveAwayPrefix && w.name.compare( 0, strlen( rule->moveAwayPrefix ), rule->moveAwayPrefix ) == 0 )
				{
					MoveAway m = { p.scaled.size(), { w.lo[ 0 ] / w.res[ 0 ], w.lo[ 1 ] / w.res[ 1 ], w.hi[ 0 ] / w.res[ 0 ], w.hi[ 1 ] / w.res[ 1 ] },
						factionGroup( w.name ), factionGroup( w.name ) == w.name };
					moveAway.push_back( m );
				}
				inherit.push_back( -1 );
				p.scaled.push_back( 0 );
				continue;
			}
			inherit.push_back( -1 );
			const Real fx0 = w.lo[ 0 ] / w.res[ 0 ], fx1 = w.hi[ 0 ] / w.res[ 0 ];
			const Real fy0 = w.lo[ 1 ] / w.res[ 1 ], fy1 = w.hi[ 1 ] / w.res[ 1 ];
			if( fx1 - fx0 >= 0.95f && fy1 - fy0 >= 0.95f )
			{
				p.scaled.push_back( 0 );
				continue;
			}
			p.scaled.push_back( 1 );
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
			// Resting on another layout: the bottom edge goes where that layout puts it, and the
			// content may only grow as far as the room above it.
			Real restBottom = -1.0f;
			if( rule->restsOn )
			{
				const GXUiScale::Transform &base = ensureLayoutTransform( rule->restsOn );
				// A little above it: the bar's own art (the general's star tab) reaches a few pixels
				// over its windows, and a power button resting exactly on the line covered it.
				restBottom = base.active ? box[ 1 ][ 1 ] * base.ky + base.addY - 0.015f : box[ 1 ][ 1 ];
				const Real extent = fmaxf( box[ 1 ][ 1 ] - box[ 1 ][ 0 ], 0.001f );
				fit[ 1 ] = fmaxf( 1.0f, fminf( fit[ 1 ], restBottom / extent ) );
			}
			if( rule->uniform )
				fit[ 0 ] = fit[ 1 ] = fminf( fit[ 0 ], fit[ 1 ] );
			for( Int a = 0; a < 2; ++a )
				axisTransform( box[ a ][ 0 ], box[ a ][ 1 ], 1.0f, fit[ a ], &axisK[ a ], &axisAdd[ a ] );
			if( restBottom >= 0.0f )
			{
				axisAdd[ 1 ] = restBottom - box[ 1 ][ 1 ] * axisK[ 1 ];
				if( box[ 1 ][ 0 ] * axisK[ 1 ] + axisAdd[ 1 ] < 0.0f )
					axisAdd[ 1 ] = -box[ 1 ][ 0 ] * axisK[ 1 ];
			}
			GXUiScale::Transform &t = p.transform;
			t.active = axisK[ 0 ] > 1.001f || axisK[ 1 ] > 1.001f || fabsf( axisAdd[ 0 ] ) > 0.0005f || fabsf( axisAdd[ 1 ] ) > 0.0005f;
			t.kx = axisK[ 0 ];
			t.ky = axisK[ 1 ];
			t.addX = axisAdd[ 0 ];
			t.addY = axisAdd[ 1 ];
			// Text grows with the layout's height, as far as its width -- already stretched by the
			// screen being wider than 4:3 -- leaves room for.
			const Real aspectRoom = ( (Real)TheDisplay->getWidth() / 800.0f ) / ( (Real)TheDisplay->getHeight() / 600.0f );
			t.fontK = fmaxf( 1.0f, fminf( t.ky, t.kx * fmaxf( aspectRoom, 1.0f ) ) );
			// Decoration the grown content now covers leaves the screen -- all of the set, so that
			// not half of a row of emblems is left standing.
			const Real sx0 = box[ 0 ][ 0 ] * t.kx + t.addX, sx1 = box[ 0 ][ 1 ] * t.kx + t.addX;
			const Real sy0 = box[ 1 ][ 0 ] * t.ky + t.addY, sy1 = box[ 1 ][ 1 ] * t.ky + t.addY;
			Bool covered = FALSE;
			for( size_t m = 0; m < moveAway.size(); ++m )
			{
				const Real *r = moveAway[ m ].r;
				if( r[ 0 ] < sx1 && r[ 2 ] > sx0 && r[ 1 ] < sy1 && r[ 3 ] > sy0 )
					covered = TRUE;
			}
			p.offset.assign( p.scaled.size() * 2, 0.0f );
			if( t.active && covered )
			{
				// Rearranged as a column in the free space left of the grown content, in their
				// original left-to-right order, each group (a picture and the Small / Medium copies
				// its hover effect grows through) moved as one so the effect still plays. Only if
				// no room is left there do they leave the screen.
				std::vector<std::string> groups;
				std::vector<Real> groupCx, groupCy;
				Real maxW = 0.0f, maxH = 0.0f;
				for( size_t m = 0; m < moveAway.size(); ++m )
				{
					const MoveAway &mw = moveAway[ m ];
					maxW = fmaxf( maxW, mw.r[ 2 ] - mw.r[ 0 ] );
					maxH = fmaxf( maxH, mw.r[ 3 ] - mw.r[ 1 ] );
					size_t g = 0;
					while( g < groups.size() && groups[ g ] != mw.group )
						++g;
					if( g == groups.size() )
					{
						groups.push_back( mw.group );
						groupCx.push_back( ( mw.r[ 0 ] + mw.r[ 2 ] ) * 0.5f );
						groupCy.push_back( ( mw.r[ 1 ] + mw.r[ 3 ] ) * 0.5f );
					}
					// A group's centre is its plain picture's (the one without a suffix).
					if( mw.plain )
					{
						groupCx[ g ] = ( mw.r[ 0 ] + mw.r[ 2 ] ) * 0.5f;
						groupCy[ g ] = ( mw.r[ 1 ] + mw.r[ 3 ] ) * 0.5f;
					}
				}
				std::vector<size_t> order( groups.size() );
				for( size_t g = 0; g < order.size(); ++g )
					order[ g ] = g;
				for( size_t a = 0; a < order.size(); ++a )
					for( size_t b = a + 1; b < order.size(); ++b )
						if( groupCx[ order[ b ] ] < groupCx[ order[ a ] ] )
						{
							const size_t tmp = order[ a ];
							order[ a ] = order[ b ];
							order[ b ] = tmp;
						}
				const Real freeRight = sx0 - 0.01f;
				const Real slot = groups.empty() ? 0.0f : 0.94f / (Real)groups.size();
				const Bool fits = !groups.empty() && freeRight >= maxW + 0.02f && slot * 1.15f >= maxH * 0.75f;
				for( size_t k2 = 0; k2 < order.size(); ++k2 )
				{
					const size_t g = order[ k2 ];
					const Real newCx = fminf( freeRight * 0.5f, 0.02f + maxW * 0.5f + 0.04f );
					const Real newCy = 0.03f + slot * ( (Real)k2 + 0.5f );
					for( size_t m = 0; m < moveAway.size(); ++m )
					{
						if( moveAway[ m ].group != groups[ g ] )
							continue;
						const size_t idx = moveAway[ m ].index;
						if( fits )
						{
							p.scaled[ idx ] = 3;
							p.offset[ idx * 2 ] = newCx - groupCx[ g ];
							p.offset[ idx * 2 + 1 ] = newCy - groupCy[ g ];
						}
						else
							p.scaled[ idx ] = 2;
					}
				}
				fprintf( stderr, "[GX-UISCALE] %s: %d decoration groups %s\n", baseName( layoutFile ),
					(Int)groups.size(), fits ? "rearranged as a column on the left" : "moved off the screen (no room)" );
			}
			// Windows inside a moved one go with it.
			for( size_t i = 0; i < p.scaled.size() && i < inherit.size(); ++i )
			{
				if( inherit[ i ] < 0 || p.scaled[ i ] != 0 )
					continue;
				const Int from = inherit[ i ];
				if( p.scaled[ from ] == 2 || p.scaled[ from ] == 3 )
				{
					p.scaled[ i ] = p.scaled[ from ];
					p.offset[ i * 2 ] = p.offset[ from * 2 ];
					p.offset[ i * 2 + 1 ] = p.offset[ from * 2 + 1 ];
				}
			}
			Int scaledCount = 0;
			for( size_t i = 0; i < p.scaled.size(); ++i )
				scaledCount += p.scaled[ i ] == 1 ? 1 : 0;
			if( t.active )
				fprintf( stderr, "[GX-UISCALE] %s: x%.2f y%.2f (asked %.2f), fonts x%.2f, %d of %d windows\n",
					baseName( layoutFile ), t.kx, t.ky, k, t.fontK,
					scaledCount, (Int)wins.size() );
		}
	}
}

// The transform of a layout that has not been loaded yet (one shared by another layout).
static const GXUiScale::Transform &ensureLayoutTransform( const char *layoutFile )
{
	std::map<std::string, GXUiScale::Transform>::const_iterator it = s_transforms.find( baseName( layoutFile ) );
	if( it != s_transforms.end() )
		return it->second;
	Parsing p;
	p.next = 0;
	p.transform = kIdentity;
	std::string path = std::string( "Window\\" ) + layoutFile;
	File *file = TheFileSystem ? TheFileSystem->openFile( path.c_str(), File::READ ) : nullptr;
	if( file )
	{
		const Int size = file->size();
		std::vector<char> text( size > 0 ? size : 1 );
		const Int got = size > 0 ? file->read( &text[ 0 ], size ) : 0;
		file->close();
		analyzeLayout( findRule( layoutFile ), layoutFile, &text[ 0 ], got, p );
	}
	s_transforms[ baseName( layoutFile ) ] = p.transform;
	return s_transforms[ baseName( layoutFile ) ];
}

namespace GXUiScale
{

void beginLayout( const char *layoutFile, const char *text, Int length )
{
	Parsing p;
	p.next = 0;
	p.transform = kIdentity;
	const Rule *rule = findRule( layoutFile );
	analyzeLayout( rule, layoutFile, text, length, p );
	if( rule && rule->sharedWith )
	{
		p.transform = ensureLayoutTransform( rule->sharedWith );
		if( p.transform.active )
			fprintf( stderr, "[GX-UISCALE] %s: follows %s (x%.2f y%.2f)\n", baseName( layoutFile ),
				rule->sharedWith, p.transform.kx, p.transform.ky );
	}
	s_transforms[ baseName( layoutFile ) ] = p.transform;
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
	if( !p.transform.active || index >= (Int)p.scaled.size() || p.scaled[ index ] == 0 )
		return;
	if( p.scaled[ index ] == 3 )
	{
		const Int dx = (Int)floorf( p.offset[ index * 2 ] * TheDisplay->getWidth() + 0.5f );
		const Int dy = (Int)floorf( p.offset[ index * 2 + 1 ] * TheDisplay->getHeight() + 0.5f );
		*loX += dx;
		*hiX += dx;
		*loY += dy;
		*hiY += dy;
		return;
	}
	if( p.scaled[ index ] == 2 )
	{
		// Below the bottom of the screen, where nothing that moves it relative to itself brings it back.
		const Int h = TheDisplay->getHeight() * 2;
		*loY += h;
		*hiY += h;
		return;
	}
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
	if( !p.transform.active || index < 0 || index >= (Int)p.scaled.size() || p.scaled[ index ] != 1 )
		return size;
	return (Int)floorf( size * p.transform.fontK + 0.5f );
}

} // namespace GXUiScale
