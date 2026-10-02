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
		// Slots (NAME prefix) stacked bottom to top in a column: those that would leave the top of
		// the screen continue in a second column to the left, again from the bottom; the frame
		// window (frameName) widens to hold it, so taps there still reach the buttons.
		const char *wrapSlotPrefix;
		const char *frameName;
		// An excluded window that marks where something grows to (the main menu's faction emblem
		// grows into WinGrowMarker when a side is picked): moved into the free space beside the
		// scaled content, shrunk to fit, so the grown emblem does not cover the menu.
		const char *growMarkerName;
		// How far from the screen's edges the scaled content stays (fraction). 0 = the default 2%,
		// where content that already reaches past it may stay there; a value given here is strict:
		// everything goes inside, as the main menu inside its frame (its logo stuck out at the top).
		Real margin;
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
		{ "GenPowersShortcutBarUS.wnd",     1.0f, 1.0f, TRUE, { nullptr }, nullptr, "ControlBar.wnd", nullptr, "ButtonParent", "GenPowersShortcutBarParent" },
		{ "GenPowersShortcutBarChina.wnd",  1.0f, 1.0f, TRUE, { nullptr }, nullptr, "ControlBar.wnd", nullptr, "ButtonParent", "GenPowersShortcutBarParent" },
		{ "GenPowersShortcutBarGLA.wnd",    1.0f, 1.0f, TRUE, { nullptr }, nullptr, "ControlBar.wnd", nullptr, "ButtonParent", "GenPowersShortcutBarParent" },
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
		// At most 60% of the screen's width (x1.35): the side emblem grows into the room left of
		// it -- at x1.5 it kept 40% of its size, at 52% (x1.18) the menu looked too small; the
		// owner picked the middle.
		// The main menu's buttons with the logo above them (scaled apart, the buttons grew into the
		// logo), not the rest of its decoration: the clock and the download buttons stay where they
		// are, and the faction emblems the single player menu shows along the bottom go when the
		// grown menu reaches down over them (they overlapped its lower buttons, owner's photo).
		{ "MainMenu.wnd",                   0.60f, 1.0f, TRUE,
			{ "WinFaction*", "WinGrowMarker", "GreenDot", "Clock", "ButtonGetMapPack",
			  "ButtonGetUpdate", "ShellMenuScheme", nullptr }, nullptr, nullptr, "WinFaction", nullptr, nullptr, "WinGrowMarker", 0.06f },
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
		std::vector<Int> scaled;  // per window with a rectangle, in file order: 0 keep, 1 scale, 2 move off screen,
		                          // 3 moved decoration, 4 wrapped slot, 5 wrapping frame
		std::vector<Real> offset; // decision 3, per window: old centre x, y, new centre x, y, scale (fractions of the screen)
		Int next;
		GXUiScale::Transform transform;
	};

	const GXUiScale::Transform kIdentity = { FALSE, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f };
	std::map<std::string, GXUiScale::Transform> s_transforms;
	std::vector<Parsing> s_parsing;

	// The axis transform for content [lo, hi] of an axis `size` long: scale k around the edge the
	// content sits on (or its centre), then shift it back inside [0, size].
	void axisTransform( Real lo, Real hi, Real size, Real k, Real margin, Bool strict, Real *outK, Real *outAdd )
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
		// Edges already outside the margin before scaling are allowed to stay where they were.
		const Real minLo = strict ? margin * size : fminf( margin * size, lo );
		const Real maxHi = strict ? size - margin * size : fmaxf( size - margin * size, hi );
		if( newLo < minLo )
			add += minLo - newLo;
		else if( newHi > maxHi )
			add -= newHi - maxHi;
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
		std::vector<const Win *> rectWins;  // the window behind each decision, in the same order
		Int growMarker = -1;
		Real growRect[ 4 ] = { 0, 0, 0, 0 };
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
			rectWins.push_back( &w );
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
				if( rule->growMarkerName && w.name == rule->growMarkerName )
				{
					growMarker = (Int)p.scaled.size();
					growRect[ 0 ] = w.lo[ 0 ] / w.res[ 0 ]; growRect[ 1 ] = w.lo[ 1 ] / w.res[ 1 ];
					growRect[ 2 ] = w.hi[ 0 ] / w.res[ 0 ]; growRect[ 3 ] = w.hi[ 1 ] / w.res[ 1 ];
				}
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
			// The control bar sits on the screen's edge by design: no margin for content that
			// already touches it.
			const Bool strict = rule->margin > 0.0f;
			// A strict margin is the rule's own at the top and bottom; the frame's sides are
			// thinner, 3% (6% moved the main menu visibly left of where it sits).
			const Real marginAxis[ 2 ] = { strict ? fminf( rule->margin, 0.03f ) : 0.02f, strict ? rule->margin : 0.02f };
			Real axisK[ 2 ], axisAdd[ 2 ], fit[ 2 ];
			for( Int a = 0; a < 2; ++a )
			{
				box[ a ][ 0 ] = fmaxf( box[ a ][ 0 ], 0.0f );
				box[ a ][ 1 ] = fminf( box[ a ][ 1 ], 1.0f );
				const Real extent = fmaxf( box[ a ][ 1 ] - box[ a ][ 0 ], 0.001f );
				// The margin only on sides the content does not already touch (the control bar sits
				// on the bottom edge and keeps its full height cap).
				const Real margin = marginAxis[ a ];
				const Real room = strict ? 1.0f - 2.0f * margin
					: fmaxf( box[ a ][ 1 ], 1.0f - margin ) - fminf( box[ a ][ 0 ], margin );
				fit[ a ] = fmaxf( 1.0f, fminf( k, fminf( maxFrac[ a ], room ) / extent ) );
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
				// Content that wraps into columns (the power bar) may be taller than the room above;
				// anything else has to fit there.
				const Real extent = fmaxf( box[ 1 ][ 1 ] - box[ 1 ][ 0 ], 0.001f );
				if( !rule->wrapSlotPrefix )
					fit[ 1 ] = fmaxf( 1.0f, fminf( fit[ 1 ], restBottom / extent ) );
			}
			if( rule->uniform )
				fit[ 0 ] = fit[ 1 ] = fminf( fit[ 0 ], fit[ 1 ] );
			for( Int a = 0; a < 2; ++a )
				axisTransform( box[ a ][ 0 ], box[ a ][ 1 ], 1.0f, fit[ a ], marginAxis[ a ], strict, &axisK[ a ], &axisAdd[ a ] );
			if( restBottom >= 0.0f )
			{
				// No clamp at the top: clamping there undid the whole shift for the power bar (its frame
				// starts at y 3 of 600), which then stayed on the grown control bar's star tab.
				axisAdd[ 1 ] = restBottom - box[ 1 ][ 1 ] * axisK[ 1 ];
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
			// Slots that would leave the top of the screen wrap into further columns to the left.
			if( t.active && rule->wrapSlotPrefix )
			{
				const size_t prefixLen = strlen( rule->wrapSlotPrefix );
				struct Slot { size_t index; Int depth; Real r[ 4 ]; };
				std::vector<Slot> slots;
				for( size_t i = 0; i < rectWins.size(); ++i )
				{
					const Win &w = *rectWins[ i ];
					if( w.name.compare( 0, prefixLen, rule->wrapSlotPrefix ) == 0 )
					{
						Slot sl = { i, w.depth, { w.lo[ 0 ] / w.res[ 0 ], w.lo[ 1 ] / w.res[ 1 ], w.hi[ 0 ] / w.res[ 0 ], w.hi[ 1 ] / w.res[ 1 ] } };
						slots.push_back( sl );
					}
				}
				// Bottom first.
				for( size_t a = 0; a < slots.size(); ++a )
					for( size_t b = a + 1; b < slots.size(); ++b )
						if( slots[ b ].r[ 3 ] > slots[ a ].r[ 3 ] )
						{
							const Slot tmp = slots[ a ];
							slots[ a ] = slots[ b ];
							slots[ b ] = tmp;
						}
				if( slots.size() >= 2 )
				{
					const Real k = t.ky;
					const Real pitch = ( slots[ 0 ].r[ 1 ] - slots[ 1 ].r[ 1 ] ) * k;
					const Real colW = ( slots[ 0 ].r[ 2 ] - slots[ 0 ].r[ 0 ] ) * t.kx + 0.004f;
					const Real top0 = slots[ 0 ].r[ 1 ] * k + t.addY;
					const Int perCol = pitch > 0.0f ? 1 + (Int)floorf( ( top0 - 0.01f ) / pitch ) : (Int)slots.size();
					const Real c0x = ( slots[ 0 ].r[ 0 ] + slots[ 0 ].r[ 2 ] ) * 0.5f * t.kx + t.addX;
					const Real c0y = ( slots[ 0 ].r[ 1 ] + slots[ 0 ].r[ 3 ] ) * 0.5f * k + t.addY;
					p.offset.assign( p.scaled.size() * 5, 0.0f );
					Int columns = 1;
					for( size_t n = 0; n < slots.size(); ++n )
					{
						const Int col = perCol > 0 ? (Int)n / perCol : 0;
						const Int row = perCol > 0 ? (Int)n % perCol : (Int)n;
						if( col + 1 > columns ) columns = col + 1;
						const Real ocx = ( slots[ n ].r[ 0 ] + slots[ n ].r[ 2 ] ) * 0.5f;
						const Real ocy = ( slots[ n ].r[ 1 ] + slots[ n ].r[ 3 ] ) * 0.5f;
						// The slot and every window inside it, moved as one.
						for( size_t i = slots[ n ].index; i < rectWins.size(); ++i )
						{
							if( i != slots[ n ].index && rectWins[ i ]->depth <= slots[ n ].depth )
								break;
							p.scaled[ i ] = 4;
							Real *o = &p.offset[ i * 5 ];
							o[ 0 ] = ocx;
							o[ 1 ] = ocy;
							o[ 2 ] = c0x - col * colW;
							o[ 3 ] = c0y - row * pitch;
							o[ 4 ] = k;
						}
					}
					// The frame: from the leftmost column to the right edge of its own, and no
					// higher than the top of the screen.
					if( rule->frameName )
						for( size_t i = 0; i < rectWins.size(); ++i )
							if( rectWins[ i ]->name == rule->frameName )
							{
								p.scaled[ i ] = 5;
								Real *o = &p.offset[ i * 5 ];
								o[ 0 ] = ( columns - 1 ) * colW;  // widen to the left by this
								o[ 1 ] = 0.0f;                     // and keep the top on screen
							}
					fprintf( stderr, "[GX-UISCALE] %s: %d slots, %d per column, %d column(s)\n", baseName( layoutFile ),
						(Int)slots.size(), perCol, columns );
				}
			}
			// Decoration the grown content now covers leaves the screen -- all of the set, so that
			// not half of a row of emblems is left standing.
			const Real sx0 = box[ 0 ][ 0 ] * t.kx + t.addX;
			// Covered means a scaled window itself now reaches into one of them (by more than 1% of
			// the screen) -- not the content's bounding box, which takes in empty space and the
			// tallest of the submenus and moved the emblems away at 110% with room to spare.
			Bool covered = FALSE;
			for( size_t m = 0; m < moveAway.size() && !covered; ++m )
			{
				// The pictures as they normally stand: their Medium copies, the size a picture grows
				// to while highlighted, reach higher and touched the panel's bottom at 110% (by 2.5%
				// of the screen) though nothing covered the pictures themselves.
				if( !moveAway[ m ].plain )
					continue;
				const Real *r = moveAway[ m ].r;
				for( size_t i = 0; i < rectWins.size() && !covered; ++i )
				{
					if( p.scaled[ i ] != 1 )
						continue;
					const Win &w = *rectWins[ i ];
					const Real wx0 = w.lo[ 0 ] / w.res[ 0 ] * t.kx + t.addX, wx1 = w.hi[ 0 ] / w.res[ 0 ] * t.kx + t.addX;
					const Real wy0 = w.lo[ 1 ] / w.res[ 1 ] * t.ky + t.addY, wy1 = w.hi[ 1 ] / w.res[ 1 ] * t.ky + t.addY;
					const Real ox = fminf( r[ 2 ], wx1 ) - fmaxf( r[ 0 ], wx0 );
					const Real oy = fminf( r[ 3 ], wy1 ) - fmaxf( r[ 1 ], wy0 );
					if( ox > 0.01f && oy > 0.01f )
						covered = TRUE;
				}
			}
			if( p.offset.size() != p.scaled.size() * 5 )
				p.offset.assign( p.scaled.size() * 5, 0.0f );
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
				// Inside the menu's frame (its ruler runs about 8% in from the top and bottom: at
				// 3%..97% the emblems touched it, owner's photo), each shrunk to fit its slot.
				const Real top = 0.11f, bottom = 0.89f;
				const Real freeRight = sx0 - 0.01f;
				const Real slot = groups.empty() ? 0.0f : ( bottom - top ) / (Real)groups.size();
				Real plainH = 0.0f;
				for( size_t m = 0; m < moveAway.size(); ++m )
					if( moveAway[ m ].plain )
						plainH = fmaxf( plainH, moveAway[ m ].r[ 3 ] - moveAway[ m ].r[ 1 ] );
				const Real shrink = plainH > 0.0f ? fminf( 1.0f, slot * 0.88f / plainH ) : 1.0f;
				const Bool fits = !groups.empty() && freeRight >= maxW * shrink + 0.04f;
				for( size_t k2 = 0; k2 < order.size(); ++k2 )
				{
					const size_t g = order[ k2 ];
					const Real newCx = fminf( freeRight * 0.5f, 0.08f + maxW * shrink * 0.5f );
					const Real newCy = top + slot * ( (Real)k2 + 0.5f );
					for( size_t m = 0; m < moveAway.size(); ++m )
					{
						if( moveAway[ m ].group != groups[ g ] )
							continue;
						const size_t idx = moveAway[ m ].index;
						if( fits )
						{
							p.scaled[ idx ] = 3;
							Real *o = &p.offset[ idx * 5 ];
							o[ 0 ] = groupCx[ g ];
							o[ 1 ] = groupCy[ g ];
							o[ 2 ] = newCx;
							o[ 3 ] = newCy;
							o[ 4 ] = shrink;
						}
						else
							p.scaled[ idx ] = 2;
					}
				}
				fprintf( stderr, "[GX-UISCALE] %s: %d decoration groups %s\n", baseName( layoutFile ),
					(Int)groups.size(), fits ? "rearranged as a column on the left" : "moved off the screen (no room)" );
			}
			// The grow target beside the scaled content, shrunk to the room there.
			if( t.active && growMarker >= 0 )
			{
				// Between the frame's left rule (about 7% in) and the menu panel itself -- not the
				// content box, which reaches further left through the load-game buttons: centred
				// on that, the emblem sat left of the free space and over the frame (owner's photos).
				Real panelLeft = sx0;
				for( size_t i = 0; i < rectWins.size(); ++i )
					if( rectWins[ i ]->name.compare( 0, 9, "MapBorder" ) == 0 && p.scaled[ i ] == 1 )
						panelLeft = fmaxf( panelLeft, ( rectWins[ i ]->lo[ 0 ] / rectWins[ i ]->res[ 0 ] ) * t.kx + t.addX );
				const Real left = 0.08f, right = panelLeft - 0.02f, top = 0.12f, bottom = 0.88f;
				const Real gw = growRect[ 2 ] - growRect[ 0 ], gh = growRect[ 3 ] - growRect[ 1 ];
				if( right - left > 0.05f && gw > 0.0f && gh > 0.0f )
				{
					const Real shrink = fminf( 1.0f, fminf( ( right - left ) / gw, ( bottom - top ) / gh ) );
					Real *o = &p.offset[ growMarker * 5 ];
					o[ 0 ] = ( growRect[ 0 ] + growRect[ 2 ] ) * 0.5f;
					o[ 1 ] = ( growRect[ 1 ] + growRect[ 3 ] ) * 0.5f;
					o[ 2 ] = ( left + right ) * 0.5f;
					o[ 3 ] = ( top + bottom ) * 0.5f;
					o[ 4 ] = shrink;
					p.scaled[ growMarker ] = 3;
				}
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
					for( Int c = 0; c < 5; ++c )
						p.offset[ i * 5 + c ] = p.offset[ from * 5 + c ];
				}
			}
			Int scaledCount = 0;
			for( size_t i = 0; i < p.scaled.size(); ++i )
				scaledCount += ( p.scaled[ i ] == 1 || p.scaled[ i ] == 4 || p.scaled[ i ] == 5 ) ? 1 : 0;
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
	if( p.scaled[ index ] == 5 )
	{
		// A wrapping layout's frame: scaled as usual, widened left over its extra columns, kept
		// below the top of the screen.
		const Transform &t = p.transform;
		const Real *o = &p.offset[ index * 5 ];
		*loX = (Int)floorf( t.mapX( (Real)*loX ) - o[ 0 ] * TheDisplay->getWidth() + 0.5f );
		*hiX = (Int)floorf( t.mapX( (Real)*hiX ) + 0.5f );
		{ const Int y0 = (Int)floorf( t.mapY( (Real)*loY ) + 0.5f ); *loY = y0 > 0 ? y0 : 0; }
		*hiY = (Int)floorf( t.mapY( (Real)*hiY ) + 0.5f );
		return;
	}
	if( p.scaled[ index ] == 3 || p.scaled[ index ] == 4 )
	{
		// Moved to a new centre and shrunk around it, with the group it belongs to.
		const Real *o = &p.offset[ index * 5 ];
		const Real w = (Real)TheDisplay->getWidth(), h = (Real)TheDisplay->getHeight();
		*loX = (Int)floorf( ( o[ 2 ] + ( *loX / w - o[ 0 ] ) * o[ 4 ] ) * w + 0.5f );
		*hiX = (Int)floorf( ( o[ 2 ] + ( *hiX / w - o[ 0 ] ) * o[ 4 ] ) * w + 0.5f );
		*loY = (Int)floorf( ( o[ 3 ] + ( *loY / h - o[ 1 ] ) * o[ 4 ] ) * h + 0.5f );
		*hiY = (Int)floorf( ( o[ 3 ] + ( *hiY / h - o[ 1 ] ) * o[ 4 ] ) * h + 0.5f );
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
	if( !p.transform.active || index < 0 || index >= (Int)p.scaled.size() || ( p.scaled[ index ] != 1 && p.scaled[ index ] != 4 ) )
		return size;
	return (Int)floorf( size * p.transform.fontK + 0.5f );
}

} // namespace GXUiScale
