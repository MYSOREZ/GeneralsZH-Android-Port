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

 /***********************************************************************************************
 ***              C O N F I D E N T I A L  ---  W E S T W O O D  S T U D I O S               ***
 ***********************************************************************************************
 *                                                                                             *
 *                 Project Name : WW3D                                                         *
 *                                                                                             *
 *                     $Archive:: /Commando/Code/ww3d2/render2dsentence.cpp                   $*
 *                                                                                             *
 *                       $Author:: Patrick                  $*
 *                                                                                             *
 *								$Modtime:: 8/29/01 11:16a                                             $*
 *                                                                                             *
 *                    $Revision:: 13                                                          $*
 *                                                                                             *
 *---------------------------------------------------------------------------------------------*
 * Functions:                                                                                  *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#include "render2dsentence.h"
#include "surfaceclass.h"
#include "texture.h"
#include "wwprofile.h"
#include "wwmemlog.h"
#include "dx8wrapper.h"
#include "GXTrace.h"
#if defined(__ANDROID__)
// GeneralsX @perf Android port 09/05/2026 - draw-category / UI-timing hooks
#include "d3d8gles.h"
#include <chrono>
#include <vector>
#endif



////////////////////////////////////////////////////////////////////////////////////
//	Local constants
////////////////////////////////////////////////////////////////////////////////////
#define no_TEST_PLACEMENT 1	 // Shows alignment markers for text.

#define TEXTURE_OFFSET 2

////////////////////////////////////////////////////////////////////////////////////
//
//	Right-to-left text (Arabic, Persian, Hebrew)
//
// GeneralsX @feature Android port 27/09/2026 The layout below places glyphs strictly left to
// right, one glyph per code point, which is only correct for scripts that neither join nor run
// right to left. Arabic needs both, so RTL text is handled in two steps around the unchanged
// layout:
//   1. Prepare_RTL_Text (before layout): Arabic letters are replaced with their contextual
//      presentation forms (isolated/final/initial/medial, lam-alef ligatures), and each word's
//      characters are put in visual order. Latin/digit runs inside a word keep their order;
//      brackets inside RTL runs are mirrored; a hotkey "&X" pair stays atomic.
//   2. Mirror_RTL_Lines (after layout): word order within each laid-out line is reversed by
//      mirroring the chunks' screen positions, so the first word ends up on the right. A run of
//      consecutive Latin words (a unit name, "(&K)", a number) is moved as one block and keeps
//      its internal order. Line breaking is therefore still done in logical order.
////////////////////////////////////////////////////////////////////////////////////
namespace
{
struct ArabicForms
{
	WCHAR	base;
	WCHAR	isolated;
	WCHAR	final_form;
	WCHAR	initial;
	WCHAR	medial;
};

// Presentation forms, U+FE70-FEFF (Arabic) and U+FB50-FDFF (Persian letters). A zero initial/medial
// form means the letter only joins to the preceding one.
static const ArabicForms kArabicForms[] = {
	{ 0x0621, 0xFE80, 0,      0,      0      },
	{ 0x0622, 0xFE81, 0xFE82, 0,      0      },
	{ 0x0623, 0xFE83, 0xFE84, 0,      0      },
	{ 0x0624, 0xFE85, 0xFE86, 0,      0      },
	{ 0x0625, 0xFE87, 0xFE88, 0,      0      },
	{ 0x0626, 0xFE89, 0xFE8A, 0xFE8B, 0xFE8C },
	{ 0x0627, 0xFE8D, 0xFE8E, 0,      0      },
	{ 0x0628, 0xFE8F, 0xFE90, 0xFE91, 0xFE92 },
	{ 0x0629, 0xFE93, 0xFE94, 0,      0      },
	{ 0x062A, 0xFE95, 0xFE96, 0xFE97, 0xFE98 },
	{ 0x062B, 0xFE99, 0xFE9A, 0xFE9B, 0xFE9C },
	{ 0x062C, 0xFE9D, 0xFE9E, 0xFE9F, 0xFEA0 },
	{ 0x062D, 0xFEA1, 0xFEA2, 0xFEA3, 0xFEA4 },
	{ 0x062E, 0xFEA5, 0xFEA6, 0xFEA7, 0xFEA8 },
	{ 0x062F, 0xFEA9, 0xFEAA, 0,      0      },
	{ 0x0630, 0xFEAB, 0xFEAC, 0,      0      },
	{ 0x0631, 0xFEAD, 0xFEAE, 0,      0      },
	{ 0x0632, 0xFEAF, 0xFEB0, 0,      0      },
	{ 0x0633, 0xFEB1, 0xFEB2, 0xFEB3, 0xFEB4 },
	{ 0x0634, 0xFEB5, 0xFEB6, 0xFEB7, 0xFEB8 },
	{ 0x0635, 0xFEB9, 0xFEBA, 0xFEBB, 0xFEBC },
	{ 0x0636, 0xFEBD, 0xFEBE, 0xFEBF, 0xFEC0 },
	{ 0x0637, 0xFEC1, 0xFEC2, 0xFEC3, 0xFEC4 },
	{ 0x0638, 0xFEC5, 0xFEC6, 0xFEC7, 0xFEC8 },
	{ 0x0639, 0xFEC9, 0xFECA, 0xFECB, 0xFECC },
	{ 0x063A, 0xFECD, 0xFECE, 0xFECF, 0xFED0 },
	{ 0x0641, 0xFED1, 0xFED2, 0xFED3, 0xFED4 },
	{ 0x0642, 0xFED5, 0xFED6, 0xFED7, 0xFED8 },
	{ 0x0643, 0xFED9, 0xFEDA, 0xFEDB, 0xFEDC },
	{ 0x0644, 0xFEDD, 0xFEDE, 0xFEDF, 0xFEE0 },
	{ 0x0645, 0xFEE1, 0xFEE2, 0xFEE3, 0xFEE4 },
	{ 0x0646, 0xFEE5, 0xFEE6, 0xFEE7, 0xFEE8 },
	{ 0x0647, 0xFEE9, 0xFEEA, 0xFEEB, 0xFEEC },
	{ 0x0648, 0xFEED, 0xFEEE, 0,      0      },
	{ 0x0649, 0xFEEF, 0xFEF0, 0,      0      },
	{ 0x064A, 0xFEF1, 0xFEF2, 0xFEF3, 0xFEF4 },
	{ 0x067E, 0xFB56, 0xFB57, 0xFB58, 0xFB59 },
	{ 0x0686, 0xFB7A, 0xFB7B, 0xFB7C, 0xFB7D },
	{ 0x0698, 0xFB8A, 0xFB8B, 0,      0      },
	{ 0x06A9, 0xFB8E, 0xFB8F, 0xFB90, 0xFB91 },
	{ 0x06AF, 0xFB92, 0xFB93, 0xFB94, 0xFB95 },
	{ 0x06CC, 0xFBFC, 0xFBFD, 0xFBFE, 0xFBFF },
};

static const ArabicForms *Find_Arabic_Forms( WCHAR ch )
{
	for ( const ArabicForms &forms : kArabicForms ) {
		if ( forms.base == ch ) {
			return &forms;
		}
	}
	return nullptr;
}

// The base letter a presentation form was made from, or 0. Used when no face has the form.
static WCHAR Arabic_Presentation_Base( WCHAR ch )
{
	for ( const ArabicForms &forms : kArabicForms ) {
		if ( ch == forms.isolated || ch == forms.final_form
			|| ( forms.initial != 0 && ( ch == forms.initial || ch == forms.medial ) ) ) {
			return forms.base;
		}
	}
	return 0;
}

enum JoiningType { JOIN_NONE, JOIN_RIGHT, JOIN_DUAL, JOIN_CAUSING, JOIN_TRANSPARENT };

static JoiningType Get_Joining_Type( WCHAR ch )
{
	if ( ( ch >= 0x064B && ch <= 0x065F ) || ch == 0x0670 || ( ch >= 0x06D6 && ch <= 0x06ED ) ) {
		return JOIN_TRANSPARENT;
	}
	if ( ch == 0x0640 || ch == 0x200D ) {
		return JOIN_CAUSING;
	}
	const ArabicForms *forms = Find_Arabic_Forms( ch );
	if ( forms == nullptr || forms->final_form == 0 ) {
		return JOIN_NONE;
	}
	return forms->initial != 0 ? JOIN_DUAL : JOIN_RIGHT;
}

static bool Is_Strong_RTL( WCHAR ch )
{
	if ( ( ch >= 0x0660 && ch <= 0x066C ) || ( ch >= 0x06F0 && ch <= 0x06F9 ) ) {
		return false;	// Arabic-Indic digits and the percent/decimal/thousands signs keep number order
	}
	return ( ch >= 0x0590 && ch <= 0x08FF ) || ( ch >= 0xFB1D && ch <= 0xFDFF ) || ( ch >= 0xFE70 && ch <= 0xFEFF );
}

static bool Is_Strong_LTR( WCHAR ch )
{
	if ( ( ch >= L'A' && ch <= L'Z' ) || ( ch >= L'a' && ch <= L'z' ) || ( ch >= L'0' && ch <= L'9' ) ) {
		return true;
	}
	if ( ( ch >= 0x0660 && ch <= 0x0669 ) || ( ch >= 0x06F0 && ch <= 0x06F9 ) ) {
		return true;
	}
	if ( ch < 0x00C0 || ch == 0x00D7 || ch == 0x00F7 || ( ch >= 0x2000 && ch <= 0x2BFF ) || ( ch >= 0x3000 && ch <= 0x303F ) ) {
		return false;
	}
	return !Is_Strong_RTL( ch );
}

static WCHAR Mirror_Bracket( WCHAR ch )
{
	switch ( ch ) {
		case L'(': return L')';
		case L')': return L'(';
		case L'[': return L']';
		case L']': return L'[';
		case L'{': return L'}';
		case L'}': return L'{';
		case L'<': return L'>';
		case L'>': return L'<';
		case 0x00AB: return 0x00BB;
		case 0x00BB: return 0x00AB;
		default: return ch;
	}
}

// Contextual shaping, in logical order. ZWJ/ZWNJ only steer the joining and are dropped.
static void Shape_Arabic( const WCHAR *text, std::vector<WCHAR> &out )
{
	const size_t len = wcslen( text );
	out.clear();
	out.reserve( len + 1 );

	auto neighbour_type = [&]( size_t i, int step ) -> JoiningType {
		for ( long j = (long)i + step; j >= 0 && j < (long)len; j += step ) {
			JoiningType type = Get_Joining_Type( text[j] );
			if ( type != JOIN_TRANSPARENT ) {
				return type;
			}
		}
		return JOIN_NONE;
	};

	for ( size_t i = 0; i < len; i++ ) {
		const WCHAR ch = text[i];
		if ( ch == 0x200C || ch == 0x200D ) {
			continue;
		}
		const JoiningType type = Get_Joining_Type( ch );
		// Harakat (fatha, shadda, tanween, ...) are dropped: the layout gives every code point its
		// own cell and advance, so a mark would open a gap in the middle of the word instead of
		// sitting on its letter. Arabic reads correctly without them.
		if ( type == JOIN_TRANSPARENT ) {
			continue;
		}
		if ( type != JOIN_RIGHT && type != JOIN_DUAL ) {
			out.push_back( ch );
			continue;
		}

		const JoiningType prev = neighbour_type( i, -1 );
		const bool joins_prev = ( prev == JOIN_DUAL || prev == JOIN_CAUSING );

		// Lam followed by an alef becomes a single ligature that only joins to the right.
		if ( ch == 0x0644 && i + 1 < len ) {
			WCHAR ligature = 0;
			switch ( text[i + 1] ) {
				case 0x0622: ligature = 0xFEF5; break;
				case 0x0623: ligature = 0xFEF7; break;
				case 0x0625: ligature = 0xFEF9; break;
				case 0x0627: ligature = 0xFEFB; break;
				default: break;
			}
			if ( ligature != 0 ) {
				out.push_back( joins_prev ? (WCHAR)( ligature + 1 ) : ligature );
				i++;
				continue;
			}
		}

		const JoiningType next = neighbour_type( i, +1 );
		const bool joins_next = ( type == JOIN_DUAL )
			&& ( next == JOIN_DUAL || next == JOIN_RIGHT || next == JOIN_CAUSING );

		const ArabicForms *forms = Find_Arabic_Forms( ch );
		WCHAR shaped = forms->isolated;
		if ( joins_prev && joins_next ) {
			shaped = forms->medial;
		} else if ( joins_prev ) {
			shaped = forms->final_form;
		} else if ( joins_next ) {
			shaped = forms->initial;
		}
		out.push_back( shaped );
	}
	out.push_back( 0 );
}

// Puts one word (no spaces or newlines) into visual order, see the block comment above.
static void Reorder_RTL_Word( WCHAR *word, size_t len )
{
	struct Unit { size_t start; size_t count; int dir; };	// dir: 0 neutral, 1 LTR, 2 RTL
	std::vector<Unit> units;
	bool any_rtl = false;
	for ( size_t i = 0; i < len; ) {
		Unit unit = { i, 1, 0 };
		if ( word[i] == L'&' && i + 1 < len ) {
			unit.count = 2;
		}
		const WCHAR key = word[i + unit.count - 1];
		unit.dir = Is_Strong_RTL( key ) ? 2 : ( Is_Strong_LTR( key ) ? 1 : 0 );
		any_rtl = any_rtl || unit.dir == 2;
		units.push_back( unit );
		i += unit.count;
	}
	if ( !any_rtl ) {
		return;
	}

	// A neutral takes the LTR direction only between two LTR units, otherwise the paragraph's.
	for ( size_t u = 0; u < units.size(); u++ ) {
		if ( units[u].dir != 0 ) {
			continue;
		}
		int before = 0;
		int after = 0;
		for ( size_t k = u; k-- > 0; ) {
			if ( units[k].dir != 0 ) { before = units[k].dir; break; }
		}
		for ( size_t k = u + 1; k < units.size(); k++ ) {
			if ( units[k].dir != 0 ) { after = units[k].dir; break; }
		}
		units[u].dir = ( before == 1 && after == 1 ) ? 3 : 2;	// 3: resolved LTR
	}

	std::vector<WCHAR> visual;
	visual.reserve( len );
	size_t run_end = units.size();
	while ( run_end > 0 ) {
		const bool rtl = units[run_end - 1].dir == 2;
		size_t run_start = run_end - 1;
		while ( run_start > 0 && ( units[run_start - 1].dir == 2 ) == rtl ) {
			run_start--;
		}
		if ( rtl ) {
			for ( size_t u = run_end; u-- > run_start; ) {
				for ( size_t c = 0; c < units[u].count; c++ ) {
					visual.push_back( units[u].count == 1 ? Mirror_Bracket( word[units[u].start] ) : word[units[u].start + c] );
				}
			}
		} else {
			for ( size_t u = run_start; u < run_end; u++ ) {
				for ( size_t c = 0; c < units[u].count; c++ ) {
					visual.push_back( word[units[u].start + c] );
				}
			}
		}
		run_end = run_start;
	}
	for ( size_t i = 0; i < len; i++ ) {
		word[i] = visual[i];
	}
}

static bool Contains_RTL( const WCHAR *text )
{
	for ( ; *text != 0; text++ ) {
		if ( Is_Strong_RTL( *text ) ) {
			return true;
		}
	}
	return false;
}

// Returns the text to lay out: `text` itself when it has no RTL characters, otherwise the
// shaped, word-reordered copy in `buffer`.
static const WCHAR *Prepare_RTL_Text( const WCHAR *text, std::vector<WCHAR> &buffer, bool *is_rtl )
{
	*is_rtl = ( text != nullptr ) && Contains_RTL( text );
	if ( !*is_rtl ) {
		return text;
	}
	Shape_Arabic( text, buffer );
	WCHAR *chars = buffer.data();
	size_t word_start = 0;
	for ( size_t i = 0; ; i++ ) {
		const WCHAR ch = chars[i];
		if ( ch == 0 || ch == L' ' || ch == L'\n' ) {
			if ( i > word_start ) {
				Reorder_RTL_Word( chars + word_start, i - word_start );
			}
			word_start = i + 1;
			if ( ch == 0 ) {
				break;
			}
		}
	}
	return chars;
}
}
////////////////////////////////////////////////////////////////////////////////////
//
//	Render2DSentenceClass
//
////////////////////////////////////////////////////////////////////////////////////
Render2DSentenceClass::Render2DSentenceClass () :
	Font (nullptr),
	Location (0.0F,0.0F),
	Cursor (0.0F,0.0F),
	TextureOffset (0, 0),
	TextureStartX (0),
	LastCharOverhang (0),
	PendingLeadPad (0),
	ChunkHasRTL (false),
	ChunkHasLTR (false),
	CurSurface (nullptr),
	CurrTextureSize (0),
	MonoSpaced (false),
	IsClippedEnabled (false),
	ClipRect (0, 0, 0, 0),
	BaseLocation (0, 0),
	LockedPtr (nullptr),
	LockedStride (0),
	LockedBytesPerPixel (2),
	TextureSizeHint (0),
	WrapWidth (0),
	Centered (false),
	DrawExtents (0, 0, 0, 0),
	ParseHotKey( false ),
	useHardWordWrap( false)
{
	Shader = Render2DClass::Get_Default_Shader ();
}


////////////////////////////////////////////////////////////////////////////////////
// GeneralsX @perf Android port 09/05/2026 The glyph-atlas recycle pool used to
// be a per-instance member, and Build_Textures() destroyed whatever it did not
// immediately reclaim. That only ever helped one case: a single long-lived
// sentence object rebuilding text that needs exactly as many atlas pages as
// before. It cannot help the case that actually dominates in gameplay --
// SHORT-LIVED sentence objects (damage numbers, tooltips, unit labels, build
// progress) constantly being constructed and destroyed. Each one allocated its
// own atlas texture and destroyed it moments later, and a texture freed by one
// object was invisible to every other. A real device log for an ordinary
// gameplay session still showed 187 of 191 total texture creations coming from
// Build_Textures (162 of them 64x64, 29 of them 128x128), with 364 deletions --
// which lines up with the frame-time dips to 3-6 fps that do NOT correlate with
// draw-call count.
//
// So the pool is shared across all Render2DSentenceClass instances, and
// survives Build_Textures() instead of being drained at the end of it. Sizing:
// entries are A4R4G4B4, so 64x64 is 8 KB and 128x128 is 32 KB -- a 32-entry cap
// is a few hundred KB worst case, far cheaper than the churn it replaces.
//
// The pool is heap-allocated and deliberately never destroyed. This codebase
// has been bitten before by global destructors running at exit (see AGENTS.md's
// "Exit semantics" note -- Windows ExitProcess skips them, POSIX does not, and
// pool allocators crash in that window). Letting process teardown reclaim it is
// the safe choice; Flush_Recycled_Textures() below is the explicit release path
// for device-reset/shutdown.
static const int GENERALSX_MAX_RECYCLED_GLYPH_TEXTURES = 32;

static DynamicVectorClass<TextureClass *> &GeneralsX_Get_Glyph_Texture_Pool ()
{
	static DynamicVectorClass<TextureClass *> *pool = new DynamicVectorClass<TextureClass *>;
	return *pool;
}

// GeneralsX @perf Android port 09/05/2026 Glyph-pool instrumentation. The pool
// above was added to stop the ~200-textures-in-a-few-seconds churn traced here
// by the [texchurn] diagnostic, and it did NOT: a later device log still showed
// created=784 deleted=630 live=154. Two rounds of code-reading produced
// plausible-sounding explanations that all turned out to be wrong (Get_Width()
// returning 0 -- disproven, TextureBaseClass's ctor sets it; the pool being
// flushed by _Invalidate_Textures -- disproven, that only runs on device
// reset). So stop guessing and measure the pool's OUTPUT: hits, misses, what
// size the miss wanted, and what was actually sitting in the pool when it
// missed. See docs/WORKDIR/lessons/LESSON-d3d-vs-gl-rasterization-conventions.md
// for why this project debugs outputs and not inferred inputs.
// MEASURED RESULT (real device, full session through menus and a skirmish):
//   hit=20428 miss=351 salvaged=20432 salvage_full=237 salvage_dup=0 pool=4
// i.e. the pool works -- 98.3% of atlas pages are recycled, and every miss the
// dump caught wanted a 64x64 page while the pool held only 128x128/256x256
// ones, which is an ordinary size mismatch and not a broken lookup. So the
// earlier "created=784 deleted=630" reading was NOT this pool failing; that
// counter is g_texturesCreated in the GLES backend, which counts every GL
// texture object in the process, glyph atlases included but far from alone.
// Keep the counters: they are ~free and turn "is the pool working?" back into
// one grep instead of another round of hypotheses.
static long g_glyphPoolHits			= 0;
static long g_glyphPoolMisses			= 0;
static long g_glyphPoolSalvaged		= 0;
static long g_glyphPoolSalvageFull	= 0;
static long g_glyphPoolSalvageDup	= 0;

void GeneralsX_Report_Glyph_Pool (const char *why)
{
	DynamicVectorClass<TextureClass *> &pool = GeneralsX_Get_Glyph_Texture_Pool ();
	char sizes[192];
	int used = 0;
	sizes[0] = 0;
	for (int i = 0; i < pool.Count () && used < (int)sizeof (sizes) - 16; i ++) {
		used += snprintf (sizes + used, sizeof (sizes) - used, "%s%dx%d/%d",
			(i ? "," : ""), pool[i]->Get_Width (), pool[i]->Get_Height (),
			(int)pool[i]->Get_Texture_Format ());
	}
	fprintf (stderr, "[glyphpool] %s hit=%ld miss=%ld salvaged=%ld salvage_full=%ld "
		"salvage_dup=%ld pool=%d [%s]\n",
		why, g_glyphPoolHits, g_glyphPoolMisses, g_glyphPoolSalvaged,
		g_glyphPoolSalvageFull, g_glyphPoolSalvageDup, pool.Count (), sizes);
}

void Render2DSentenceClass::Flush_Recycled_Textures ()
{
	DynamicVectorClass<TextureClass *> &pool = GeneralsX_Get_Glyph_Texture_Pool ();
	while (pool.Count () > 0) {
		TextureClass *leftover = pool[0];
		pool.Delete (0);
		REF_PTR_RELEASE (leftover);
	}
}


//
//	~Render2DSentenceClass
//
////////////////////////////////////////////////////////////////////////////////////
Render2DSentenceClass::~Render2DSentenceClass ()
{
	REF_PTR_RELEASE (Font);
	Reset ();

	// GeneralsX @perf Android port 09/05/2026 Reset() salvaged this object's
	// textures into the SHARED pool above, which outlives this object on
	// purpose -- other sentence objects reuse them. Nothing to release here.
	// (This used to drain a per-instance pool; see the pool's comment.)
}


////////////////////////////////////////////////////////////////////////////////////
//
//	Set_Font
//
////////////////////////////////////////////////////////////////////////////////////
void
Render2DSentenceClass::Set_Font (FontCharsClass *font)
{
	Reset ();
	REF_PTR_SET (Font, font);
}


////////////////////////////////////////////////////////////////////////////////////
//
//	Reset_Polys
//
////////////////////////////////////////////////////////////////////////////////////
void
Render2DSentenceClass::Reset_Polys ()
{
	for (int index = 0; index < Renderers.Count (); index ++) {
		Renderers[index].Renderer->Reset ();
	}
}


////////////////////////////////////////////////////////////////////////////////////
//
//	Reset
//
////////////////////////////////////////////////////////////////////////////////////
void
Render2DSentenceClass::Reset ()
{
	//
	//	Make sure we unlock the current surface (if necessary)
	//
	if (LockedPtr != nullptr) {
		CurSurface->Unlock ();
		LockedPtr = nullptr;
	}

	//
	//	Release our hold on the current surface
	//
	REF_PTR_RELEASE (CurSurface);

	//
	//	Free each renderer, salvaging its texture into RecycledTextures
	//	first (see the member's comment in render2dsentence.h) so
	//	Build_Textures() can reuse it instead of always allocating a fresh
	//	GL texture object for the content that's about to replace this.
	//
	while (Renderers.Count () > 0) {
		TextureClass *salvaged = Renderers[0].Renderer->Peek_Texture ();
		if (salvaged != nullptr) {
			// GeneralsX @perf Android port 09/05/2026 Salvage into the SHARED
			// pool (see its comment above) so any sentence object can reclaim
			// it, not just this one -- short-lived label objects are the main
			// churn source and never reuse their own textures. Past the cap,
			// release instead of growing without bound.
			DynamicVectorClass<TextureClass *> &pool = GeneralsX_Get_Glyph_Texture_Pool ();
			// Several renderers of one sentence share a single atlas page, so
			// the same TextureClass comes back around this loop more than
			// once. Adding it twice would let Build_Textures hand the SAME
			// page to two different pending surfaces in one pass, and the
			// second _Copy_DX8_Rects would overwrite the first one's glyphs.
			bool already_pooled = false;
			for (int pool_index = 0; pool_index < pool.Count (); pool_index ++) {
				if (pool[pool_index] == salvaged) {
					already_pooled = true;
					break;
				}
			}
			if (already_pooled) {
				g_glyphPoolSalvageDup ++;
			} else if (pool.Count () < GENERALSX_MAX_RECYCLED_GLYPH_TEXTURES) {
				salvaged->Add_Ref ();
				pool.Add (salvaged);
				g_glyphPoolSalvaged ++;
			} else {
				g_glyphPoolSalvageFull ++;
			}
		}
		delete Renderers[0].Renderer;
		Renderers.Delete(0);
	}

	Cursor.Set (0, 0);
	MonoSpaced = false;
	ParseHotKey = false;

	Release_Pending_Surfaces ();
	Reset_Sentence_Data ();
}


////////////////////////////////////////////////////////////////////////////////////
//
//	Make_Additive
//
////////////////////////////////////////////////////////////////////////////////////
void
Render2DSentenceClass::Make_Additive ()
{
	Shader.Set_Dst_Blend_Func (ShaderClass::DSTBLEND_ONE);
	Shader.Set_Src_Blend_Func (ShaderClass::SRCBLEND_ONE);
	Shader.Set_Primary_Gradient (ShaderClass::GRADIENT_MODULATE);
	Shader.Set_Secondary_Gradient (ShaderClass::SECONDARY_GRADIENT_DISABLE);

	Set_Shader (Shader);
}


////////////////////////////////////////////////////////////////////////////////////
//
//	Make_Additive
//
////////////////////////////////////////////////////////////////////////////////////
void
Render2DSentenceClass::Set_Shader (ShaderClass shader)
{
	Shader = shader;

	//
	//	Change each renderer's shader
	//
	for (int i = 0; i < Renderers.Count (); i ++) {
		ShaderClass *curr_shader = Renderers[i].Renderer->Get_Shader ();
		(*curr_shader) = Shader;
	}
}


////////////////////////////////////////////////////////////////////////////////////
//
//	Render
//
////////////////////////////////////////////////////////////////////////////////////
void
Render2DSentenceClass::Render ()
{
	//
	//	Build any textures that are pending
	//
	GX_TRACE("Render2DSentenceClass::Render: about to Build_Textures pending=%d\n", PendingSurfaces.Count());
	Build_Textures ();
	GX_TRACE("Render2DSentenceClass::Render: Build_Textures returned, renderers=%d\n", Renderers.Count());

	//
	//	Ask each renderer to draw its contents
	//
	for (int i = 0; i < Renderers.Count (); i ++) {
		GX_TRACE("Render2DSentenceClass::Render: about to Renderers[%d]->Render()\n", i);
		Renderers[i].Renderer->Render ();
		GX_TRACE("Render2DSentenceClass::Render: Renderers[%d]->Render() returned\n", i);
	}
}


////////////////////////////////////////////////////////////////////////////////////
//
//	Set_Base_Location
//
////////////////////////////////////////////////////////////////////////////////////
void
Render2DSentenceClass::Set_Base_Location (const Vector2 &loc)
{
	Vector2 dif		= loc - BaseLocation;
	BaseLocation	= loc;
	for (int i = 0; i < Renderers.Count (); i ++) {
		Renderers[i].Renderer->Move (dif);
	}
}


////////////////////////////////////////////////////////////////////////////////////
//
//	Set_Location
//
////////////////////////////////////////////////////////////////////////////////////
void
Render2DSentenceClass::Set_Location (const Vector2 &loc)
{
	Location	= loc;
}


////////////////////////////////////////////////////////////////////////////////////
//
//	Get_Text_Extents
//
////////////////////////////////////////////////////////////////////////////////////
Vector2
Render2DSentenceClass::Get_Text_Extents (const WCHAR *text)
{
	// TheSuperHackers @bugfix Guard against a null Font: this can legitimately
	// happen when font resolution/loading failed upstream (e.g. an unavailable
	// localized font), and this method is reachable directly (e.g. for layout
	// measurement) without going through Build_Sentence()'s null check first.
	if (Font == nullptr || text == nullptr)
		return Vector2 (0, 0);

	Vector2 extent (0, Font->Get_Char_Height());

	// GeneralsX @feature Android port 27/09/2026 Measure the shaped text: lam-alef ligatures and
	// contextual forms have different advances from the logical letters.
	std::vector<WCHAR> rtl_buffer;
	bool is_rtl = false;
	text = Prepare_RTL_Text(text, rtl_buffer, &is_rtl);

	while (*text) {
		WCHAR ch = *text++;

		if ( ch != (WCHAR)'\n' ) {
			extent.X += Font->Get_Char_Spacing( ch );
		}
	}

	return extent;
}


////////////////////////////////////////////////////////////////////////////////////
//
//	Get_Formatted_Text_Extents
//
////////////////////////////////////////////////////////////////////////////////////
Vector2
Render2DSentenceClass::Get_Formatted_Text_Extents (const WCHAR *text)
{
	// TheSuperHackers @bugfix Guard against a null Font (see Get_Text_Extents).
	if (Font == nullptr || text == nullptr)
		return Vector2 (0, 0);

	std::vector<WCHAR> rtl_buffer;
	bool is_rtl = false;
	text = Prepare_RTL_Text(text, rtl_buffer, &is_rtl);
	return Build_Sentence_Not_Centered(text, nullptr, nullptr, true);
}


////////////////////////////////////////////////////////////////////////////////////
//
//	Reset_Sentence_Data
//
////////////////////////////////////////////////////////////////////////////////////
void
Render2DSentenceClass::Reset_Sentence_Data ()
{
	//
	//	Release our hold on each texture used in the sentence
	//
	for (int index = 0; index < SentenceData.Count (); index ++) {
		REF_PTR_RELEASE (SentenceData[index].Surface);
	}

	if (SentenceData.Count()>0) {
		SentenceData.Delete_All ();
	}
}


////////////////////////////////////////////////////////////////////////////////////
//
//	Release_Pending_Surfaces
//
////////////////////////////////////////////////////////////////////////////////////
void
Render2DSentenceClass::Release_Pending_Surfaces ()
{
	//
	//	Release our hold on each pending surface
	//
	for (int index = 0; index < PendingSurfaces.Count (); index ++) {
		SurfaceClass *curr_surface = PendingSurfaces[index].Surface;
		REF_PTR_RELEASE (curr_surface);
	}

	if (PendingSurfaces.Count()>0) PendingSurfaces.Delete_All ();
}


////////////////////////////////////////////////////////////////////////////////////
//
//	Build_Textures
//
////////////////////////////////////////////////////////////////////////////////////
void
Render2DSentenceClass::Build_Textures ()
{
#if defined(__ANDROID__)
	struct GxUiTimer {
		std::chrono::steady_clock::time_point t0;
		int bucket;
		GxUiTimer(int b) : t0(std::chrono::steady_clock::now()), bucket(b) {}
		~GxUiTimer() {
			d3d8gles_AddUiTiming(bucket,
				std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count());
		}
	} gxUiTimer(D3D8GLES_UITIME_TEXT_TEXTURE);
#endif

	WWMEMLOG(MEM_TEXTURE);

	//
	//	Make sure we unlock the current surface
	//
	if (LockedPtr != nullptr) {
		CurSurface->Unlock ();
		LockedPtr = nullptr;
	}

	//
	//	Release our hold on the current surface
	//
	REF_PTR_RELEASE (CurSurface);
	TextureOffset.Set (0, 0);
	TextureStartX = 0;
	LastCharOverhang = 0;

	//
	//	Convert all pending surfaces to textures
	//
	for (int index = 0; index < PendingSurfaces.Count (); index ++) {
		PendingSurfaceStruct &surface_info = PendingSurfaces[index];
		SurfaceClass *curr_surface = surface_info.Surface;

		//
		//	Get the dimensions of the surface
		//
		SurfaceClass::SurfaceDescription desc;
		curr_surface->Get_Description (desc);

		//
		//	Reuse a texture Reset() salvaged into RecycledTextures (see that
		//	function and the member's comment in render2dsentence.h) when
		//	one matches what we need, instead of always allocating a fresh
		//	GL texture object. This function runs every time on-screen text
		//	is rebuilt (any 2D sentence/label content change) -- on a real
		//	device that showed up as ~200 GL texture create+destroy cycles
		//	within a few seconds during ordinary gameplay, all unnamed
		//	64x64-ish A4R4G4B4 textures (see the [texchurn] diagnostic that
		//	traced them here). Renderers[0]->Peek_Texture() itself is
		//	useless here -- by this point Reset() has already deleted every
		//	old renderer and PendingSurfaces' renderers are freshly
		//	constructed with no texture of their own yet.
		//
		TextureClass *new_texture = nullptr;
		DynamicVectorClass<TextureClass *> &pool = GeneralsX_Get_Glyph_Texture_Pool ();
		for (int pool_index = 0; pool_index < pool.Count (); pool_index ++) {
			TextureClass *candidate = pool[pool_index];
			// Atlas pages are always square (allocated below as Width x Width),
			// so both dimensions are checked against desc.Width by design.
			if (candidate->Get_Width () == (int)desc.Width &&
				candidate->Get_Height () == (int)desc.Width &&
				candidate->Get_Texture_Format () == WW3D_FORMAT_A4R4G4B4) {
				new_texture = candidate;
				pool.Delete (pool_index);
				g_glyphPoolHits ++;
				GX_TRACE("Build_Textures: reusing recycled TextureClass=%p width=%u\n",
					(void*)new_texture, desc.Width);
				break;
			}
		}
		if (new_texture == nullptr) {
			g_glyphPoolMisses ++;
			// Print the pool's contents at the moment of the miss -- the whole
			// point of this diagnostic is to see WHY nothing matched, not just
			// that nothing did. Throttled so it does not become the bottleneck
			// it is measuring.
			if ((g_glyphPoolMisses % 25) == 1) {
				char miss_why[64];
				snprintf (miss_why, sizeof (miss_why), "miss want=%ux%u/%d",
					desc.Width, desc.Width, (int)WW3D_FORMAT_A4R4G4B4);
				GeneralsX_Report_Glyph_Pool (miss_why);
			}
			GX_TRACE("Build_Textures: about to create TextureClass width=%u\n", desc.Width);
			new_texture = W3DNEW TextureClass (desc.Width, desc.Width, WW3D_FORMAT_A4R4G4B4, MIP_LEVELS_1);
			GX_TRACE("Build_Textures: TextureClass created=%p\n", (void*)new_texture);
		}
		SurfaceClass *texture_surface = new_texture->Get_Surface_Level ();

		new_texture->Get_Filter().Set_U_Addr_Mode(TextureFilterClass::TEXTURE_ADDRESS_CLAMP);
		new_texture->Get_Filter().Set_V_Addr_Mode(TextureFilterClass::TEXTURE_ADDRESS_CLAMP);
		new_texture->Get_Filter().Set_Min_Filter(TextureFilterClass::FILTER_TYPE_NONE);
		new_texture->Get_Filter().Set_Mag_Filter(TextureFilterClass::FILTER_TYPE_NONE);
		new_texture->Get_Filter().Set_Mip_Mapping(TextureFilterClass::FILTER_TYPE_NONE);

		//
		//	Copy the contents of the texture from the surface
		//
		GX_TRACE("Build_Textures: about to _Copy_DX8_Rects src=%p dst=%p\n",
			(void*)curr_surface->Peek_D3D_Surface(), (void*)texture_surface->Peek_D3D_Surface());
		DX8Wrapper::_Copy_DX8_Rects (curr_surface->Peek_D3D_Surface (), nullptr, 0, texture_surface->Peek_D3D_Surface (), nullptr);
		GX_TRACE("Build_Textures: _Copy_DX8_Rects returned\n");
		REF_PTR_RELEASE (texture_surface);

		//
		//	Assign this texture to any renderers that need it
		//
		for (int renderer_index = 0; renderer_index < surface_info.Renderers.Count (); renderer_index ++) {
			Render2DClass *renderer = surface_info.Renderers[renderer_index];
			renderer->Set_Texture (new_texture);
		}

		//
		//	Release our hold on the objects
		//
		REF_PTR_RELEASE (new_texture);
		REF_PTR_RELEASE (curr_surface);
	}

	//
	//	Reset the list
	//
	if (PendingSurfaces.Count()>0) {
		PendingSurfaces.Delete_All ();
	}

	// GeneralsX @perf Android port 09/05/2026 Unclaimed entries deliberately
	// STAY in the shared pool now (bounded by GENERALSX_MAX_RECYCLED_GLYPH_TEXTURES
	// at insertion time). Destroying them here, as this used to, is what kept
	// the churn alive: pages freed by one sentence object were thrown away
	// before any other object could claim them. Flush_Recycled_Textures() is
	// the explicit release path.
}


////////////////////////////////////////////////////////////////////////////////////
//
//	Draw_Sentence
//
////////////////////////////////////////////////////////////////////////////////////
void
Render2DSentenceClass::Draw_Sentence (uint32 color)
{
	Render2DClass *curr_renderer	= nullptr;
	SurfaceClass *curr_surface		= nullptr;

	DrawExtents.Set (0, 0, 0, 0);

	int offset = 0;
	//
	//	Loop over all the parts of the sentence
	//
	for (int index = 0; index < SentenceData.Count (); index ++) {
		SentenceDataStruct &data = SentenceData[index];

		//
		//	Has the surface changed?
		//
		if (data.Surface != curr_surface) {
			curr_surface = data.Surface;

			//
			//	Try to find a renderer that uses the same "texture"
			//
			bool found = false;
			for (int renderer_index = 0; renderer_index < Renderers.Count (); renderer_index ++) {
				if (Renderers[renderer_index].Surface == curr_surface) {
					found = true;
					curr_renderer = Renderers[renderer_index].Renderer;
					break;
				}
			}

			//
			//	Create a new renderer if we couldn't find an appropriate one
			//
			if (found == false) {

				//
				//	Allocate a new renderer
				//
				curr_renderer = W3DNEW Render2DClass;
				curr_renderer->Set_Coordinate_Range (Render2DClass::Get_Screen_Resolution ());
				ShaderClass *curr_shader = curr_renderer->Get_Shader ();
				(*curr_shader) = Shader;

				//
				//	Add it to our list
				//
				RendererDataStruct render_info;
				render_info.Renderer	= curr_renderer;
				render_info.Surface	= curr_surface;
				Renderers.Add (render_info);

				//
				//	Now, add this renderer to the surface pending list
				//
				for (int surface_index = 0; surface_index < PendingSurfaces.Count (); surface_index ++) {
					PendingSurfaceStruct &surface_info = PendingSurfaces[surface_index];
					if (surface_info.Surface == curr_surface) {
						surface_info.Renderers.Add (curr_renderer);
					}
				}
			}
		}

		//
		//	Get the dimensions of the surface
		//
		SurfaceClass::SurfaceDescription desc;
		curr_surface->Get_Description (desc);

		//
		//	Add a quad that contains this sentence chunk
		//
		RectClass screen_rect	= data.ScreenRect;
		screen_rect					+= Location;
		RectClass uv_rect			= data.UVRect;

		//
		//	Clip the quad (as necessary)
		//
		bool add_quad = true;
		if (IsClippedEnabled) {

			//
			//	Check for completely clipped
			//
			if (	screen_rect.Right <= ClipRect.Left ||
					screen_rect.Bottom <= ClipRect.Top)
			{
				add_quad = false;
			} else {

				//
				//	Clip the polygons to the specified area
				//
				RectClass clipped_rect;
				clipped_rect.Left		= max (screen_rect.Left, ClipRect.Left);
				clipped_rect.Right	= min (screen_rect.Right, ClipRect.Right);
				clipped_rect.Top		= max (screen_rect.Top, ClipRect.Top);
				clipped_rect.Bottom	= min (screen_rect.Bottom, ClipRect.Bottom);

				//
				//	Clip the texture to the specified area
				//
				RectClass clipped_uv_rect;
				float percent				= ((clipped_rect.Left - screen_rect.Left) / screen_rect.Width ());
				clipped_uv_rect.Left		= uv_rect.Left + (uv_rect.Width () * percent);

				percent						= ((clipped_rect.Right - screen_rect.Left) / screen_rect.Width ());
				clipped_uv_rect.Right	= uv_rect.Left + (uv_rect.Width () * percent);

				percent						= ((clipped_rect.Top - screen_rect.Top) / screen_rect.Height ());
				clipped_uv_rect.Top		= uv_rect.Top + (uv_rect.Height () * percent);

				percent						= ((clipped_rect.Bottom - screen_rect.Top) / screen_rect.Height ());
				clipped_uv_rect.Bottom	= uv_rect.Top + (uv_rect.Height () * percent);

				//
				//	Use the clipped rectangles to render
				//
				screen_rect = clipped_rect;
				uv_rect		= clipped_uv_rect;

				if (screen_rect.Right <= screen_rect.Left ||
						screen_rect.Bottom <= screen_rect.Top)
				{
					add_quad = false;
				}
			}
		}

		if (add_quad) {
			//uv_rect.Bottom += 0.5f;
			uv_rect *=  1.0F / ((float)desc.Width);
#ifdef TEST_PLACEMENT
			screen_rect.Left += offset*3;
			screen_rect.Right += offset*3;
#endif
			offset++;
			curr_renderer->Add_Quad (screen_rect, uv_rect, color);

			//
			//	Add this rectangle to the total draw extents
			//
			if (DrawExtents.Width () == 0) {
				DrawExtents = screen_rect;
			} else {
				DrawExtents += screen_rect;
			}
		}
	}
}


////////////////////////////////////////////////////////////////////////////////////
//
//	Record_Sentence_Chunk
//
////////////////////////////////////////////////////////////////////////////////////
void
Render2DSentenceClass::Record_Sentence_Chunk (bool joins_next)
{
	//
	//	Do we have anything to store?
	//
	int width = TextureOffset.I - TextureStartX;
	if (width > 0) {
		float char_height = Font->Get_Char_Height ();

		//
		//	Build a structure that contains enough information
		// to hold this portion of the sentence
		//
		SentenceDataStruct sentence_data;
		sentence_data.Surface = CurSurface;
		sentence_data.Surface->Add_Ref ();
		// GeneralsX @bugfix Android port 30/07/2026 Blit_Char writes a cell of
		// Get_Char_Width() columns, but we only stepped TextureOffset.I by the
		// advance, so the last glyph of this chunk spills LastCharOverhang
		// pixels past TextureOffset.I. Inside a chunk that overlap is harmless
		// (the next glyph is blitted over it, which is what PixelOverlap is
		// for), but at the chunk edge it falls outside the rect below and the
		// glyph loses its tail -- while Cursor.X still advanced by the full
		// advance, which reads on screen as a gap in the middle of a word.
		// Widen both rects by the overhang so the tail is drawn; Cursor.X is
		// deliberately still advanced by `width` only, so the next chunk
		// overlaps this one by exactly the overhang.
		int overhang = LastCharOverhang;

		sentence_data.ScreenRect.Left		= Cursor.X;
		sentence_data.ScreenRect.Right	= Cursor.X + width + overhang;
		sentence_data.ScreenRect.Top		= Cursor.Y;
		sentence_data.ScreenRect.Bottom	= Cursor.Y + char_height;
		sentence_data.UVRect.Left			= TextureStartX;
		sentence_data.UVRect.Top			= TextureOffset.J;
		sentence_data.UVRect.Right			= TextureOffset.I + overhang;
		sentence_data.UVRect.Bottom		= TextureOffset.J + char_height;
		sentence_data.LeadPad			= min (PendingLeadPad, (float)width);
		sentence_data.JoinsNext			= joins_next;
		sentence_data.HasRTL				= ChunkHasRTL;
		sentence_data.HasLTR				= ChunkHasLTR;

		//
		//	Add this information to our list
		//
		SentenceData.Add (sentence_data);
	}
	PendingLeadPad = 0;
	ChunkHasRTL = false;
	ChunkHasLTR = false;
}


////////////////////////////////////////////////////////////////////////////////////
//
//	Note_Chunk_Char
//
////////////////////////////////////////////////////////////////////////////////////
void
Render2DSentenceClass::Note_Chunk_Char (WCHAR ch)
{
	ChunkHasRTL = ChunkHasRTL || Is_Strong_RTL (ch);
	ChunkHasLTR = ChunkHasLTR || Is_Strong_LTR (ch);
}


////////////////////////////////////////////////////////////////////////////////////
//
//	Is_RTL_Text
//
// GeneralsX @feature Android port 27/09/2026 Same test Build_Sentence uses to decide whether a
// sentence is shaped and mirrored, so a caller's alignment always agrees with the layout.
////////////////////////////////////////////////////////////////////////////////////
bool
Render2DSentenceClass::Is_RTL_Text (const WCHAR *text)
{
	return text != nullptr && Contains_RTL (text);
}

////////////////////////////////////////////////////////////////////////////////////
//
//	Mirror_RTL_Lines
//
// GeneralsX @feature Android port 27/09/2026 Reverses the word order of every laid-out line of
// an RTL sentence (see the block comment at the top of the file). Each line is mirrored inside
// [0, widest line], which keeps centred lines centred, right-aligns wrapped paragraphs, and leaves
// the sentence's extents unchanged. Chunks of one word split at the texture edge move together;
// consecutive LTR words move as one block so they keep their order. A blitted leading space is
// cut off its chunk first, otherwise it would end up between the wrong two words.
////////////////////////////////////////////////////////////////////////////////////
void
Render2DSentenceClass::Mirror_RTL_Lines (int *hkX, int *hkY)
{
	const int count = SentenceData.Count ();
	if (count == 0) {
		return;
	}

	std::vector<RectClass> original (count);
	float span = 0;
	for (int index = 0; index < count; index ++) {
		SentenceDataStruct &data = SentenceData[index];
		original[index] = data.ScreenRect;
		if (data.LeadPad > 0) {
			data.ScreenRect.Left	+= data.LeadPad;
			data.UVRect.Left		+= data.LeadPad;
		}
		span = max (span, data.ScreenRect.Right);
	}

	std::vector<float> delta (count, 0.0F);
	int line_start = 0;
	while (line_start < count) {
		int line_end = line_start + 1;
		while (line_end < count && SentenceData[line_end].ScreenRect.Top == SentenceData[line_start].ScreenRect.Top) {
			line_end ++;
		}

		//	Words: chunks joined across a texture edge. dir: 1 LTR, 2 RTL, 0 neutral.
		struct Word { int first; int last; int dir; };
		std::vector<Word> words;
		for (int index = line_start; index < line_end; index ++) {
			Word word = { index, index, 0 };
			while (SentenceData[word.last].JoinsNext && word.last + 1 < line_end) {
				word.last ++;
			}
			bool has_rtl = false;
			bool has_ltr = false;
			for (int k = word.first; k <= word.last; k ++) {
				has_rtl = has_rtl || SentenceData[k].HasRTL;
				has_ltr = has_ltr || SentenceData[k].HasLTR;
			}
			word.dir = has_rtl ? 2 : (has_ltr ? 1 : 0);
			words.push_back (word);
			index = word.last;
		}
		for (size_t w = 0; w < words.size (); w ++) {
			if (words[w].dir != 0) {
				continue;
			}
			bool ltr_before = false;
			bool ltr_after = false;
			for (size_t k = w; k-- > 0; ) {
				if (words[k].dir != 0) { ltr_before = (words[k].dir == 1); break; }
			}
			for (size_t k = w + 1; k < words.size (); k ++) {
				if (words[k].dir != 0) { ltr_after = (words[k].dir == 1); break; }
			}
			words[w].dir = (ltr_before && ltr_after) ? 3 : 2;
		}

		//	Blocks: an RTL word on its own, or a run of LTR words.
		for (size_t w = 0; w < words.size (); ) {
			size_t w_end = w + 1;
			if (words[w].dir != 2) {
				while (w_end < words.size () && words[w_end].dir != 2) {
					w_end ++;
				}
			}
			const int first = words[w].first;
			const int last = words[w_end - 1].last;
			float left = SentenceData[first].ScreenRect.Left;
			float right = SentenceData[first].ScreenRect.Right;
			for (int k = first; k <= last; k ++) {
				left = min (left, SentenceData[k].ScreenRect.Left);
				right = max (right, SentenceData[k].ScreenRect.Right);
			}
			const float shift = span - right - left;
			for (int k = first; k <= last; k ++) {
				delta[k] = shift;
			}
			w = w_end;
		}
		line_start = line_end;
	}

	for (int index = 0; index < count; index ++) {
		SentenceData[index].ScreenRect.Left		+= delta[index];
		SentenceData[index].ScreenRect.Right	+= delta[index];
	}

	//	The hotkey letter is drawn separately at (hkX, hkY); move it with the chunk it was in.
	if (hkX != nullptr && hkY != nullptr) {
		for (int index = 0; index < count; index ++) {
			if (original[index].Top == (float)*hkY && (float)*hkX >= original[index].Left && (float)*hkX < original[index].Right) {
				*hkX += (int)delta[index];
				break;
			}
		}
	}
}


////////////////////////////////////////////////////////////////////////////////////
//
//	Allocate_New_Surface
//
////////////////////////////////////////////////////////////////////////////////////
void
Render2DSentenceClass::Allocate_New_Surface (const WCHAR *text, bool justCalcExtents)
{
	if (!justCalcExtents)
	{
		//
		//	Unlock the last surface (if necessary)
		//
		if (LockedPtr != nullptr) {
			CurSurface->Unlock ();
			LockedPtr = nullptr;
		}
	}

	//
	// Calculate the width of the text
	//
	int text_width = 0;
	for (int index = 0; text[index] != 0; index ++) {
		text_width += Font->Get_Char_Spacing (text[index]);
	}

	int char_height = Font->Get_Char_Height ();

	//
	//	Find the best texture size for the remaining text
	//
	CurrTextureSize = 256;
	int best_tex_mem_usage = 999999999;
	for (int pow2 = 6; pow2 <= 8; pow2 ++) {

		int size					= 1 << pow2;
		int row_count			= (text_width / size) + 1;
		int rows_per_texture	= size / (char_height + 1);

		//
		//	Can we even fit one character on this texture?
		//
		if (rows_per_texture > 0) {

			//
			//	How many textures (at this size) would it take to render
			// the remaining text?
			//
			int texture_count	= row_count / rows_per_texture;
			texture_count		= max (texture_count, 1);

			//
			//	Is this the best usage of texture memory we've found yet?
			//
			int texture_mem_usage = (texture_count * size * size);
			if (texture_mem_usage < best_tex_mem_usage) {
				CurrTextureSize		= size;
				best_tex_mem_usage	= texture_mem_usage;
			}
		}
	}

	//
	//	Use whichever is larger, the hint or the calculated size
	//
	CurrTextureSize = max (TextureSizeHint, CurrTextureSize);

	if (!justCalcExtents)
	{
		//
		//	Release our extra hold on the old surface
		//
		REF_PTR_RELEASE (CurSurface);

		//
		//	Create the new surface
		//
		GX_TRACE("Allocate_New_Surface: about to create SurfaceClass size=%d\n", CurrTextureSize);
		CurSurface = NEW_REF (SurfaceClass, (CurrTextureSize, CurrTextureSize, WW3D_FORMAT_A4R4G4B4));
		GX_TRACE("Allocate_New_Surface: SurfaceClass created=%p\n", (void*)CurSurface);
		WWASSERT (CurSurface != nullptr);
		CurSurface->Add_Ref ();

		//
		//	Add this surface to our list
		//
		PendingSurfaceStruct surface_info;
		surface_info.Surface = CurSurface;
		PendingSurfaces.Add (surface_info);
	}

	//
	//	Reset to the upper left corner
	//
	TextureOffset.Set (0, 0);
	TextureStartX = 0;
	LastCharOverhang = 0;
}

float FindStartingXPos( const WCHAR *text )
{

	return 1;
}
////////////////////////////////////////////////////////////////////////////////////
//
//	Build_Sentence_Centered
//
////////////////////////////////////////////////////////////////////////////////////
void	Render2DSentenceClass::Build_Sentence_Centered (const WCHAR *text, int *hkX, int *hkY)
{
	// TheSuperHackers @bugfix Guard against a null Font (see Get_Text_Extents):
	// this function is reachable directly from Build_Sentence(), which already
	// checks for null, but also indirectly (e.g. by future callers), so check
	// again here defensively.
	if (Font == nullptr || text == nullptr)
		return;

	float char_height = Font->Get_Char_Height ();
	int		wordWidth = 0;
	int notCenteredHotkeyX = 0;
	int notCenteredHotkeyY = 0;
	Vector2 extent = Build_Sentence_Not_Centered(text,&notCenteredHotkeyX, &notCenteredHotkeyY, TRUE); //Get_Formatted_Text_Extents(text);

	//
	//	Start fresh
	//
	Reset_Sentence_Data ();
	PendingLeadPad = 0;
	ChunkHasRTL = false;
	ChunkHasLTR = false;
	Cursor.Set (0, 0);

	//
	//	Ensure we have a surface to start with
	//
	if (CurSurface == nullptr) {
		Allocate_New_Surface (text);
	}



	//
	//	Loop over all the characters in the string
	//
	bool end = false;
	const WCHAR *word;
	int word_width	= 0;
	int line_width	= 0;
	int charCount = 0;
	int wordCount = 0;
	int hotKeyPosX = 0;
	int hotKeyPosY = 0;
	bool calcHotKeyX = false;
	bool dontBlit = false;
	while (!end)
	{
		//
		// Re-init everything for the next line
		//
		word	= text;
		word_width	= 0;
		line_width	= 0;
		charCount = 0;
		wordCount = 0;
		//
		//first find the length of the line till we wrap
		//
		while ( 1 )
		{
			//
			// read a word
			//
			int charWidth = 0;
			while ((*word != 0) && (*word > L' ') && (*word != L'\n')) {
				if( ParseHotKey && (*word == L'&') && (*word+1 != 0) && (*word+1 > L' ') && (*word+1 != L'\n'))
				{
					int offset = 0;
					if (word_width != 0 )
					{
						const WCHAR *word_back = word;
						*word_back--;
						if (*word_back == L' ')
						{
							line_width -= word_width;
							offset =-1;
						}
					}
					*word++;
					calcHotKeyX = true;
				}

				charWidth = Font->Get_Char_Spacing (*word++);
				word_width += charWidth;
				wordCount++;

				if (WrapWidth > 0 && word_width >= WrapWidth && useHardWordWrap)
					break;
			}
			//
			// If this word is unworthy to be on the current line, decrement the space and break
			//
			if(WrapWidth > 0 && (line_width + word_width >= WrapWidth))
			{
				//
				//Take care of the case that the word is too big for the allocated space...
				//If that's the case, drop out and process the word anyway
				//
				if(charCount == 0)
				{
					charCount +=wordCount - 1;
					line_width += word_width - charWidth;
					if(*word == 0)
						end = true;
					break;
				}
				charCount--;
				break;
			}
			//
			// if we reached the end of the text, set the values and break, also set the end flag
			//
			if( *word == 0 )
			{
				charCount +=wordCount;
				line_width += word_width;
				end = true;
				break;
			}
			//
			// otherwise, increment the counts
			//
			charCount +=wordCount + 1;
			line_width += word_width;
			//
			// We were some a new line character break and process
			//
			if(*word != L' ')
				break;
			//
			// add the space to our width
			//
			word_width = Font->Get_Char_Spacing (*word++);
			wordCount = 0;
			line_width += word_width;
		}
		//
		// we now hold the length of the line and it's width lets set our cursor position to center it
		//
		Cursor.X = (int)((extent.X - line_width) / 2);
		if(Cursor.X < 0)
			Cursor.X = 0;
		if(calcHotKeyX)
		{
			calcHotKeyX = false;
			hotKeyPosX = Cursor.X + notCenteredHotkeyX;
		}

		for(int i = 0; i <= charCount; i++) {
			WCHAR ch = *text++;
			dontBlit = false;
			//
			//	Determine how much horizontal space this character requires
			//
			if(ParseHotKey && (ch == L'&') && (*text != 0) && (*text > L' ') && (*text != L'\n'))
			{
				ch = *text++;
				dontBlit = true;
			}
			float char_spacing = Font->Get_Char_Spacing (ch);

			bool exceeded_texture_width	= ((TextureOffset.I + Font->Get_Char_Width (ch)) >= CurrTextureSize);
			bool encountered_break_char	= (ch == L' ' || ch == L'\n' || ch == 0);

			//
			//	Do we need to record this portion of the sentence to its own chunk?
			//
			if (exceeded_texture_width || encountered_break_char) {
				Record_Sentence_Chunk (!encountered_break_char);

				//
				//	Adjust the positions
				//
				Cursor.X			+= (TextureOffset.I - TextureStartX);
				TextureStartX	= TextureOffset.I;

				//
				//	Adjust the output coordinates
				//
				if (ch == L' ') {
					Cursor.X += char_spacing;
				} else if ((ch == 0 )|| (ch == L'\n')) {
					break;
				}

				//
				//	Did the text extend past the edge of the texture?
				//
				if (exceeded_texture_width) {
					TextureStartX		= 0;
					TextureOffset.I	= TextureStartX;
					TextureOffset.J	+= char_height;

					//
					//	Did the text extent completely off the texture?
					//
					if ((TextureOffset.J + char_height) >= CurrTextureSize) {
						Allocate_New_Surface (text);
					}
				}
			}
			//
			//	Adjust the output coordinates
			//
			if (ch != L'\n' && ch != L' ') {

				//
				//	Ensure the surface is locked
				//
				if (LockedPtr == nullptr) {
					LockedPtr = CurSurface->Lock (&LockedStride);
					WWASSERT (LockedPtr != nullptr);
					// GeneralsX @bugfix Android port 31/07/2026 CurSurface was created
					// with WW3D_FORMAT_A4R4G4B4, but Patches/dxvk-mali-g76-4444-format.patch
					// maps that D3D9 format to a real 32-bit VK_FORMAT_B8G8R8A8_UNORM on
					// this device (Mali-G76 can't create the packed 16-bit format DXVK
					// would otherwise use), so the surface's real bytes-per-pixel no
					// longer matches Get_Bytes_Per_Pixel()'s WW3D_FORMAT_A4R4G4B4 table
					// entry (still 2). Comparing the pitch DXVK actually returned
					// against the known pixel width (rather than assuming an exact
					// stride == width * bpp, which Vulkan does not guarantee for a
					// linear-tiled image -- a driver is free to pad each row) keeps
					// this correct regardless of which format substitution, if any,
					// is active for the current device.
					LockedBytesPerPixel = (LockedStride >= CurrTextureSize * 4) ? 4 : 2;
				}

				//
				//	Check to ensure the text will fit on this texture
				//
				WWASSERT (((TextureOffset.I + Font->Get_Char_Width (ch)) <= CurrTextureSize) && ((TextureOffset.J + char_height) < CurrTextureSize));

				//
				//	Blit the character to the surface
				//
				if(!dontBlit)
					Font->Blit_Char (ch, LockedPtr, LockedStride, LockedBytesPerPixel, TextureOffset.I, TextureOffset.J);

				if (dontBlit) {
					// we don't blit for a hot key character.  So add extra spacing.
					char_spacing += Font->Get_Extra_Overlap();
					// Brutal hack #27 Gamma - Bolded M's are just a problem.	jba.
					if (ch=='M') {
						char_spacing++;
					}
				}

				// GeneralsX @bugfix Android port 30/07/2026 see Record_Sentence_Chunk.
				LastCharOverhang = dontBlit ? 0
					: max( 0, (int)(Font->Get_Char_Width (ch) - char_spacing) );

				Note_Chunk_Char (ch);
				TextureOffset.I += char_spacing;
			}
		}
		//
		// reset our cursor and add a line of text to the cursor position
		//
		Cursor.X = 0;
		Cursor.Y += char_height;
		line_width = 0;
		}

		if(hkX)
			*hkX = hotKeyPosX;
		if(hkX)
			*hkY = hotKeyPosY;
}
////////////////////////////////////////////////////////////////////////////////////
//
//	Build_Sentence_NotCentered
//
////////////////////////////////////////////////////////////////////////////////////
Vector2	Render2DSentenceClass::Build_Sentence_Not_Centered (const WCHAR *text, int *hkX, int *hkY, bool justCalcExtents)
{
	// TheSuperHackers @bugfix Guard against a null Font (see Get_Text_Extents):
	// Get_Formatted_Text_Extents() calls this directly, bypassing the null
	// check in Build_Sentence(), so a font that failed to load could otherwise
	// reach Font->Get_Char_Height()/Get_Char_Spacing() below with Font==nullptr.
	if (Font == nullptr || text == nullptr)
		return Vector2 (0, 0);

	Vector2 cursor = Cursor;
	int textureStartX = TextureStartX;
	float maxX = 0;

	int hotKeyPosX = 0;
	int hotKeyPosY = 0;
	bool calcHotKeyX = false;
	bool dontBlit = false;
	Vector2i textureOffset = TextureOffset;


	//
	//	Start fresh
	//
	if (!justCalcExtents)
	{
		Reset_Sentence_Data ();
		PendingLeadPad = 0;
		ChunkHasRTL = false;
		ChunkHasLTR = false;
	}
	Cursor.Set (0, 0);

	//
	//	Ensure we have a surface to start with
	//
	if (CurSurface == nullptr) {
		Allocate_New_Surface (text, justCalcExtents);
	}

	TextureOffset.Set (TEXTURE_OFFSET, 0);
	TextureStartX = TEXTURE_OFFSET;

	float char_height = Font->Get_Char_Height ();

	//
	//	Loop over all the characters in the string
	//
	while (text != nullptr) {
		WCHAR ch = *text++;
		dontBlit = false;
		//
		//	Determine how much horizontal space this character requires
		//
		if(ParseHotKey && (ch == L'&') && (*text != 0) && (*text > L' ') && (*text != L'\n'))
		{
				hotKeyPosY = Cursor.Y;
			if (calcHotKeyX)
				hotKeyPosX = 0;
			else
				hotKeyPosX = Cursor.X + TextureOffset.I -TextureStartX;//TextureOffset.I;

			ch = *text++;
			dontBlit = true;
		}
		float char_spacing = Font->Get_Char_Spacing (ch);

		bool exceeded_texture_width	= ((TextureOffset.I + Font->Get_Char_Width (ch)) >= CurrTextureSize);
		bool encountered_break_char	= (ch == L' ' || ch == L'\n' || ch == 0);
		bool wordBiggerThenLine = ((useHardWordWrap) && ( WrapWidth != 0 ) &&((Cursor.X + TextureOffset.I -TextureStartX + char_spacing) >= WrapWidth));
		//
		//	Do we need to record this portion of the sentence to its own chunk?
		//
		if (exceeded_texture_width || encountered_break_char|| wordBiggerThenLine) {
			if (!justCalcExtents)
			{
				Record_Sentence_Chunk (exceeded_texture_width && !encountered_break_char && !wordBiggerThenLine);
			}
			// GeneralsX @feature Android port 27/09/2026 The space that ended this chunk is blitted
			// at the start of the next one; Mirror_RTL_Lines cuts it off there.
			if (!justCalcExtents)
			{
				PendingLeadPad = (ch == L' ') ? char_spacing : 0;
			}

			//
			//	Adjust the positions
			//
			Cursor.X			+= (TextureOffset.I - TextureStartX);
			maxX = max(maxX, Cursor.X);
			TextureStartX	= TextureOffset.I;

			//
			//	Adjust the output coordinates
			//
			if (ch == L' ') {
				//Cursor.X += char_spacing;
				//maxX = max(maxX, Cursor.X);

				//
				// Check to see if we need to wrap on this word-break
				//
				if (WrapWidth > 0) {

					//
					//	Find the length of the next word
					//
					const WCHAR *word	= text;
					float word_width	= char_spacing;
					while ((*word != 0) && (*word > L' ')) {
						if(ParseHotKey && (*word == L'&') && (*word+1 != 0) && (*word+1 > L' ') && (*word+1 != L'\n'))
							*word++;
						word_width += Font->Get_Char_Spacing (*word++);
					}

					//
					//	Should we wrap the next word?
					//
					if ((Cursor.X + word_width) >= WrapWidth) {
						Cursor.X = 0;
						Cursor.Y += char_height;
						calcHotKeyX = true;
					}
				}

			} else if (ch == L'\n') {
				Cursor.X = 0;
				Cursor.Y += char_height;
			} else if (ch == 0) {
				break;
			} else if (wordBiggerThenLine){ // we've entered this loop because we're greater then the wordwrap so we need to force a wordwrap
				Cursor.X = 0;
				Cursor.Y += char_height;
			}


			//
			//	Did the text extend past the edge of the texture?
			//
			if (exceeded_texture_width) {
				TextureStartX		= TEXTURE_OFFSET;
				TextureOffset.I	= TextureStartX;
				TextureOffset.J	+= char_height;

				//
				//	Did the text extent completely off the texture?
				//
				if ((TextureOffset.J + char_height) >= CurrTextureSize) {
					Allocate_New_Surface (text, justCalcExtents);
				}
			}
		}

		if (ch != L'\n' ) {

			//
			//	Ensure the surface is locked
			//
			if (!justCalcExtents)
			{
				if (LockedPtr == nullptr) {
					LockedPtr = CurSurface->Lock (&LockedStride);
					WWASSERT (LockedPtr != nullptr);
					// GeneralsX @bugfix Android port 31/07/2026 CurSurface was created
					// with WW3D_FORMAT_A4R4G4B4, but Patches/dxvk-mali-g76-4444-format.patch
					// maps that D3D9 format to a real 32-bit VK_FORMAT_B8G8R8A8_UNORM on
					// this device (Mali-G76 can't create the packed 16-bit format DXVK
					// would otherwise use), so the surface's real bytes-per-pixel no
					// longer matches Get_Bytes_Per_Pixel()'s WW3D_FORMAT_A4R4G4B4 table
					// entry (still 2). Comparing the pitch DXVK actually returned
					// against the known pixel width (rather than assuming an exact
					// stride == width * bpp, which Vulkan does not guarantee for a
					// linear-tiled image -- a driver is free to pad each row) keeps
					// this correct regardless of which format substitution, if any,
					// is active for the current device.
					LockedBytesPerPixel = (LockedStride >= CurrTextureSize * 4) ? 4 : 2;
				}
			}

			//
			//	Check to ensure the text will fit on this texture
			//
			WWASSERT (((TextureOffset.I + Font->Get_Char_Width (ch)) <= CurrTextureSize) && ((TextureOffset.J + char_height) < CurrTextureSize));

			//
			//	Blit the character to the surface
			//
			if (!justCalcExtents && !dontBlit )
			{
				Font->Blit_Char (ch, LockedPtr, LockedStride, LockedBytesPerPixel, TextureOffset.I, TextureOffset.J);
			}

			// GeneralsX @bugfix Android port 30/07/2026 see Record_Sentence_Chunk.
			LastCharOverhang = dontBlit ? 0
				: max( 0, (int)(Font->Get_Char_Width (ch) - char_spacing) );

			if (!justCalcExtents)
			{
				Note_Chunk_Char (ch);
			}
			TextureOffset.I += char_spacing;
		}
	}

	Vector2 extent;
	extent.X = maxX + Font->Get_Extra_Overlap();
	extent.Y = Cursor.Y + char_height;

	Cursor = cursor;
	TextureOffset = textureOffset;
	TextureStartX = textureStartX;

	if(hkX)
		*hkX = hotKeyPosX;
	if(hkX)
		*hkY = hotKeyPosY;

	return extent;
}


////////////////////////////////////////////////////////////////////////////////////
//
//	Build_Sentence
//
////////////////////////////////////////////////////////////////////////////////////
void
Render2DSentenceClass::Build_Sentence (const WCHAR *text, int *hkX, int *hkY)
{
#if defined(__ANDROID__)
	struct GxUiTimer {
		std::chrono::steady_clock::time_point t0;
		int bucket;
		GxUiTimer(int b) : t0(std::chrono::steady_clock::now()), bucket(b) {}
		~GxUiTimer() {
			d3d8gles_AddUiTiming(bucket,
				std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count());
		}
	} gxUiTimer(D3D8GLES_UITIME_TEXT_RASTER);
#endif

	if (text == nullptr) {
		return ;
	}

	if (Font == nullptr)
		return;

	// GeneralsX @feature Android port 27/09/2026 RTL text is shaped and put in visual word order
	// before layout, and its lines mirrored after it (see Mirror_RTL_Lines).
	std::vector<WCHAR> rtl_buffer;
	bool is_rtl = false;
	text = Prepare_RTL_Text(text, rtl_buffer, &is_rtl);

	if(Centered && (WrapWidth > 0 || wcschr(text,L'\n')))
		Build_Sentence_Centered(text, hkX, hkY);
	else
		Build_Sentence_Not_Centered(text, hkX, hkY);

	if (is_rtl)
		Mirror_RTL_Lines(hkX, hkY);

}


////////////////////////////////////////////////////////////////////////////////////
//
//	FontCharsClass
//
////////////////////////////////////////////////////////////////////////////////////
FontCharsClass::FontCharsClass () :
#ifdef _WIN32
	OldGDIFont(	nullptr ),
	OldGDIBitmap( nullptr ),
	GDIFont( nullptr ),
	GDIBitmap( nullptr ),
	GDIBitmapBits ( nullptr ),
	MemDC( nullptr ),
#endif
#if defined(SAGE_USE_FREETYPE) && !defined(_WIN32)
	FTLibrary( nullptr ),
	FTFace( nullptr ),
	FTFallbackFaceCount( 0 ),
	FTFallbackFacesLoaded( false ),
#endif
	CurrPixelOffset( 0 ),
	PointSize( 0 ),
	CharHeight( 0 ),
	UnicodeCharArray( nullptr ),
	FirstUnicodeChar( 0xFFFF ),
	LastUnicodeChar( 0 ),
	IsBold (false)
{
	AlternateUnicodeFont = nullptr;
	::memset( ASCIICharArray, 0, sizeof (ASCIICharArray) );
#if defined(SAGE_USE_FREETYPE) && !defined(_WIN32)
	::memset( FTFallbackFaces, 0, sizeof (FTFallbackFaces) );
#endif
}


////////////////////////////////////////////////////////////////////////////////////
//
//	~FontCharsClass
//
////////////////////////////////////////////////////////////////////////////////////
FontCharsClass::~FontCharsClass ()
{
	while ( BufferList.Count() ) {
		delete BufferList[0];
		BufferList.Delete(0);
	}

#if defined(SAGE_USE_FREETYPE) && !defined(_WIN32)
	Free_Freetype_Font();
#else
	Free_GDI_Font();
#endif
	Free_Character_Arrays();
}


////////////////////////////////////////////////////////////////////////////////////
//
//	Get_Char_Data
//
////////////////////////////////////////////////////////////////////////////////////
const FontCharsClassCharDataStruct *
FontCharsClass::Get_Char_Data (WCHAR ch)
{
	// GeneralsX @bugfix BenderAI/felipebraz 14/03/2026 Normalize to 16-bit code units to match legacy font array indexing on non-Windows wchar_t
	const uint16 normalized_char = static_cast<uint16>(ch);
	const WCHAR glyph = static_cast<WCHAR>(normalized_char);

	const FontCharsClassCharDataStruct *retval = nullptr;

	if ( normalized_char < 256 )
	{
		retval = ASCIICharArray[normalized_char];
	}
 	else if ( AlternateUnicodeFont && this != AlternateUnicodeFont )
	{
		// GeneralsX @bugfix fbraz 03/06/2026 Log ALL Cyrillic delegations for diagnostics
		if (ch >= 0x0400 && ch <= 0x04FF) {
			GX_TRACE("[GX-ISSUE144] Get_Char_Data delegate U+%04X from=%s to=%s\n",
				(unsigned int)ch,
				GDIFontName.str(),
				AlternateUnicodeFont->GDIFontName.str());
		}
		return AlternateUnicodeFont->Get_Char_Data( glyph );
	}
	else
	{
		Grow_Unicode_Array( glyph );
		retval = UnicodeCharArray[normalized_char - FirstUnicodeChar];
	}

	//
	//	If the character wasn't found, then add it to our list
	//  TheSuperHackers @feature FreeType port 10/02/2026 Dispatch to FreeType on Linux
	//
	if ( retval == nullptr ) {
#if defined(SAGE_USE_FREETYPE) && !defined(_WIN32)
		retval = Store_Freetype_Char( glyph );
#else
		retval = Store_GDI_Char( glyph );
#endif
	}

	WWASSERT( retval->Value == glyph );
	return retval;
}


////////////////////////////////////////////////////////////////////////////////////
//
//	Get_Char_Width
//
////////////////////////////////////////////////////////////////////////////////////
int
FontCharsClass::Get_Char_Width (WCHAR ch)
{
	const FontCharsClassCharDataStruct	* data = Get_Char_Data( ch );
	if ( data != nullptr ) {
		return data->Width;
	}

	return 0;
}


////////////////////////////////////////////////////////////////////////////////////
//
//	Get_Char_Spacing
//
////////////////////////////////////////////////////////////////////////////////////
int
FontCharsClass::Get_Char_Spacing (WCHAR ch)
{
	const FontCharsClassCharDataStruct	* data = Get_Char_Data( ch );
	if ( data != nullptr ) {
		// GeneralsX @bugfix Android port 30/07/2026 prefer the real
		// typographic advance when the backend recorded one. The legacy
		// formula below derives spacing from the atlas cell width, but the
		// FreeType backend has to widen that cell whenever a glyph's bitmap
		// overhangs its advance -- so deriving spacing from it inflated the
		// step by each glyph's overhang, producing ragged gaps inside words
		// ("S OL O PL AY" instead of "SOLO PLAY").
		if ( data->Advance > 0 ) {
			return data->Advance;
		}
		if ( data->Width != 0 ) {
			return data->Width - PixelOverlap - CharOverhang;
		}
	}

	return 0;
}


////////////////////////////////////////////////////////////////////////////////////
//
//	Blit_Char
//
////////////////////////////////////////////////////////////////////////////////////
void
FontCharsClass::Blit_Char (WCHAR ch, void *dest_ptr, int dest_stride, int dest_bytes_per_pixel, int x, int y)
{
	const FontCharsClassCharDataStruct	* data = Get_Char_Data( ch );
	if ( data != nullptr && data->Width != 0 ) {

		// GeneralsX @bugfix Android port 31/07/2026 data->Buffer (the glyph
		// cache Store_Freetype_Char/Store_GDI_Char wrote into) is always
		// packed 16-bit A4R4G4B4 -- that is this engine's own private cache
		// format and is untouched by any D3D/DXVK format substitution. The
		// destination surface is a different story: on a device where DXVK
		// had to remap WW3D_FORMAT_A4R4G4B4 to a real 32-bit format (see the
		// call site), each source texel must be unpacked and re-expanded
		// into 8-bit-per-channel BGRA8 instead of written verbatim.
		uint8 *dest_row = static_cast<uint8*>(dest_ptr) + dest_stride * y + dest_bytes_per_pixel * x;
		uint16 *src_ptr	= data->Buffer;

		if (dest_bytes_per_pixel == 4) {
			for ( int row = 0; row < CharHeight; row ++ ) {
				uint32 *dest32 = reinterpret_cast<uint32*>(dest_row);
				for ( int col = 0; col < data->Width; col ++ ) {
					uint16 packed = *src_ptr++;
					// Unpack this engine's 4-bit-alpha/12-bit-color cache
					// format and replicate each nibble into a full byte
					// (0-15 -> 0-255 evenly) to build a BGRA8 word matching
					// VK_FORMAT_B8G8R8A8_UNORM's byte order.
					unsigned a4 = (packed >> 12) & 0xF;
					unsigned r4 = (packed >> 8)  & 0xF;
					unsigned g4 = (packed >> 4)  & 0xF;
					unsigned b4 =  packed        & 0xF;
					uint32 curData = ((a4 << 4 | a4) << 24)
					               | ((r4 << 4 | r4) << 16)
					               | ((g4 << 4 | g4) << 8)
					               |  (b4 << 4 | b4);
					if (col < PixelOverlap) {
						curData |= dest32[col];
					}
					dest32[col] = curData;
				}
				dest_row += dest_stride;
			}
		} else {
			for ( int row = 0; row < CharHeight; row ++ ) {
				uint16 *dest16 = reinterpret_cast<uint16*>(dest_row);
				for ( int col = 0; col < data->Width; col ++ ) {
					uint16 curData = *src_ptr++;
					if (col < PixelOverlap) {
						curData |= dest16[col];
					}
					dest16[col] = curData;
				}
				dest_row += dest_stride;
			}
		}
	}
}


#ifdef _WIN32

////////////////////////////////////////////////////////////////////////////////////
//
//	Store_GDI_Char
//
// GeneralsX @build fbraz 11/02/2026 - Windows-only GDI text rendering
////////////////////////////////////////////////////////////////////////////////////
const FontCharsClassCharDataStruct *
FontCharsClass::Store_GDI_Char (WCHAR ch)
{
	int width	= PointSize * 2;
	int height	= PointSize * 2;

	//
	//	Draw the character into the memory DC
	//
	RECT rect = { 0, 0, width, height };
	int xOrigin = 0;
	if (ch == 'W') {
		xOrigin = 1;
	}
	::ExtTextOutW( MemDC, xOrigin, 0, ETO_OPAQUE, &rect, &ch, 1, nullptr);

	//
	//	Get the size of the character we just drew
	//
	SIZE char_size = { 0 };
	::GetTextExtentPoint32W( MemDC, &ch, 1, &char_size );
	char_size.cx += PixelOverlap + xOrigin;
	//
	//	Get a pointer to the surface that this character should use
	//
	Update_Current_Buffer( char_size.cx );
	uint16* curr_buffer_p = BufferList[BufferList.Count () - 1]->Buffer;
	curr_buffer_p += CurrPixelOffset;

	//
	//	Copy the BMP contents to the buffer
	//
	int stride = (((width * 3) + 3) & ~3);
	for (int row = 0; row < char_size.cy; row ++) {

		//
		//	Compute the indices into the BMP and surface
		//
		int index = (row * stride);

		//
		//	Loop over each column
		//
		for (int col = 0; col < char_size.cx; col ++) {

			//
			//	Get the pixel color at this location
			//
			uint8 pixel_value = GDIBitmapBits[index];
			index += 3;
#ifdef TEST_PLACEMENT
 			if (row==CharHeight-1&&col==0) {
 				pixel_value = 0xff;
 			}
 			if (row==CharHeight-2&&col==1) {
 				pixel_value = 0xff;
 			}
 			if (row==0&&col==0) {
 				pixel_value = 0xff;
 			}
 			if (row==1&&col==1) {
 				pixel_value = 0xff;
 			}
 			if (row==CharHeight-1&&col==char_size.cx-1-PixelOverlap) {
 				pixel_value = 0xff;
 			}
 			if (row==CharHeight-2&&col==char_size.cx-2-PixelOverlap) {
 				pixel_value = 0xff;
 			}
 			if (row==0&&col==char_size.cx-1-PixelOverlap) {
 				pixel_value = 0xff;
 			}
 			if (row==1&&col==char_size.cx-2-PixelOverlap) {
 				pixel_value = 0xff;
 			}
 			if (pixel_value == 0x00) {
 				pixel_value = 0x40;
 			}
#endif

			uint16 pixel_color = 0;
			if (pixel_value != 0) {
				pixel_color = 0x0FFF;
			}

			//
			//	Convert the pixel intensity from 8bit to 4bit and
			// store it in our buffer
			//
			uint8 alpha_value	= ((pixel_value >> 4) & 0xF);
			*curr_buffer_p++	= pixel_color | (alpha_value << 12);
		}
	}

	//
	//	Save information about this character in our list
	//
	FontCharsClassCharDataStruct *char_data	= W3DNEW FontCharsClassCharDataStruct;
	char_data->Value				= ch;
	char_data->Width				= char_size.cx;
	char_data->Buffer				= BufferList[BufferList.Count () - 1]->Buffer + CurrPixelOffset;

	//
	//	Insert this character into our array
	//
	if ( ch < 256 ) {
		ASCIICharArray[ch] = char_data;
	} else {
		UnicodeCharArray[ch - FirstUnicodeChar] = char_data;
	}

	//
	//	Advance the character position
	//
	CurrPixelOffset += ((char_size.cx+PixelOverlap) * CharHeight);

	//
	//	Return the index of the entry we just added
	//
	return char_data;
}


////////////////////////////////////////////////////////////////////////////////////
//
//	Create_GDI_Font
//
////////////////////////////////////////////////////////////////////////////////////
bool
FontCharsClass::Create_GDI_Font (const char *font_name)
{
	HDC screen_dc = ::GetDC ((HWND)WW3D::Get_Window());

	const char *fontToUseForGenerals = "Arial";
	bool doingGenerals = false;
	if (strcmp(font_name, "Generals")==0) {
		font_name = fontToUseForGenerals;
		doingGenerals = true;
	}

	//
	//	Calculate the height of the font in logical units
	//
	const int dotsPerInch = 96; // always use 96.	jba.
	int font_height = -MulDiv (PointSize, dotsPerInch, 72);

	int fontWidth = 0; // use font default.
	if (doingGenerals) {
		//fontWidth = -font_height*0.35f; //2 pixels tighter.
		fontWidth = -font_height*0.40f; // one pixel tighter
	}
	PixelOverlap = (-font_height)/8;

	// Sanity check in case of perversion. :)
	if (PixelOverlap<0) PixelOverlap = 0;
	if (PixelOverlap>4) PixelOverlap = 4;
	//
	//	Create the Windows font
	//
	DWORD bold		= IsBold ? FW_BOLD : FW_NORMAL;
	DWORD italic	= 0;
	GDIFont			= ::CreateFont (font_height, fontWidth, 0, 0, bold, italic,
								FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
								CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
								VARIABLE_PITCH, font_name);

	//
	// Set-up the fields of the BITMAPINFOHEADER
	//	Note: Top-down DIBs use negative height in Win32.
	//
	BITMAPINFOHEADER bitmap_info = { 0 };
	bitmap_info.biSize				= sizeof (BITMAPINFOHEADER);
	bitmap_info.biWidth				= PointSize * 2;
	bitmap_info.biHeight				= -(PointSize * 2);
	bitmap_info.biPlanes				= 1;
	bitmap_info.biBitCount			= 24;
	bitmap_info.biCompression		= BI_RGB;
	bitmap_info.biSizeImage			= ((PointSize * PointSize * 4) * 3);
	bitmap_info.biXPelsPerMeter	= 0;
	bitmap_info.biYPelsPerMeter	= 0;
	bitmap_info.biClrUsed			= 0;
	bitmap_info.biClrImportant		= 0;

	//
	// Create a bitmap that we can access the bits directly of
	//
	GDIBitmap	= ::CreateDIBSection (	screen_dc,
													(const BITMAPINFO *)&bitmap_info,
													DIB_RGB_COLORS,
													(void **)&GDIBitmapBits,
													nullptr,
													0L);

	//
	//	Create a device context we can select the font and bitmap into
	//
	MemDC = ::CreateCompatibleDC (screen_dc);

	//
	// Release our temporary screen DC
	//
	::ReleaseDC ((HWND)WW3D::Get_Window(), screen_dc);

	//
	//	Now select the BMP and font into the DC
	//
	OldGDIBitmap	= (HBITMAP)::SelectObject (MemDC, GDIBitmap);
	OldGDIFont		= (HFONT)::SelectObject (MemDC, GDIFont);
	::SetBkColor (MemDC, RGB (0, 0, 0));
	::SetTextColor (MemDC, RGB (255, 255, 255));

	//
	//	Lookup the pixel height of the font
	//
	TEXTMETRIC text_metric = { 0 };
	::GetTextMetrics (MemDC, &text_metric);
	CharHeight = text_metric.tmHeight;
	CharAscent = text_metric.tmAscent;
	CharOverhang = text_metric.tmOverhang;
	if (doingGenerals) {
		CharOverhang = 0;
	}

	return GDIFont != nullptr && GDIBitmap != nullptr;
}


////////////////////////////////////////////////////////////////////////////////////
//
//	Free_GDI_Font
//
////////////////////////////////////////////////////////////////////////////////////
void
FontCharsClass::Free_GDI_Font ()
{
	//
	//	Select the old font back into the DC and delete
	// our font object
	//
	if ( GDIFont != nullptr ) {
		::SelectObject( MemDC, OldGDIFont );
		::DeleteObject( GDIFont );
		GDIFont = nullptr;
	}

	//
	//	Select the old bitmap back into the DC and delete
	// our bitmap object
	//
	if ( GDIBitmap != nullptr ) {
		::SelectObject( MemDC, OldGDIBitmap );
		::DeleteObject( GDIBitmap );
		GDIBitmap = nullptr;
	}

	//
	//	Delete our memory DC
	//
	if ( MemDC != nullptr ) {
		::DeleteDC( MemDC );
		MemDC = nullptr;
	}
}

#endif // _WIN32

////////////////////////////////////////////////////////////////////////////////////
//
//	Update_Current_Buffer (Platform-independent text buffer management)
//
// GeneralsX @build fbraz 11/02/2026 - Used by both Windows GDI and Linux FreeType
////////////////////////////////////////////////////////////////////////////////////
void
FontCharsClass::Update_Current_Buffer (int char_width)
{
	//
	//	Check to see if we need to allocate a new buffer
	//
	bool needs_new_buffer = (BufferList.Count () == 0);
	if (needs_new_buffer == false) {

		//
		//	Would we extend past this buffer?
		//
		if ( (CurrPixelOffset + (char_width * CharHeight)) > CHAR_BUFFER_LEN ) {
			needs_new_buffer = true;
		}
	}

	//
	//	Do we need to create a new surface?
	//
	if (needs_new_buffer)
	{
		FontCharsBuffer* new_buffer = W3DNEW FontCharsBuffer;
		BufferList.Add( new_buffer );
		CurrPixelOffset = 0;
	}

	return ;
}

#if defined(SAGE_USE_FREETYPE) && !defined(_WIN32)
#include <strings.h>
#include <unistd.h>

#if (defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE) || defined(__ANDROID__)

#include <cctype>
#include <cstdio>
#include <unistd.h>

////////////////////////////////////////////////////////////////////////////////////
//
//	Locate_Font_FontConfig (iOS/Android)
//
// iOS and Android have no fontconfig and no user-accessible system font files
// (Android ships fonts in /system/fonts but offers no name-resolution API to
// native code). Fonts are resolved from a "fonts" directory below the current
// working directory (the app's game-data folder). The requested face name
// is normalized (lowercase, spaces stripped) and tried as <name>.ttf/.otf/.ttc;
// arial.ttf serves as the universal fallback since the game UI is Arial-based.
////////////////////////////////////////////////////////////////////////////////////
const char *
FontCharsClass::Locate_Font_FontConfig (const char *font_name)
{
	char normalized[128];
	int n = 0;
	for ( const char *p = font_name; *p != '\0' && n < (int)sizeof(normalized) - 1; ++p ) {
		if ( *p == ' ' ) {
			continue;
		}
		normalized[n++] = (char)tolower( (unsigned char)*p );
	}
	normalized[n] = '\0';

	// GeneralsX @bugfix Android port 05/10/2026 Issue #36: a second place to look. The launcher
	// copies the bundled fonts into the game folder, and that copy fails silently when the
	// folder cannot be written (Android 16 custom ROM: no fonts/ folder, every label blank). The
	// app's own external files dir always holds them -- the launcher unpacks the APK's copies
	// there on every start, no permission needed -- and SDL3Main.cpp names it in
	// GENERALSX_BUNDLED_FONTS_DIR. The game folder is still searched first, so a player's own
	// replacement font keeps winning.
	const char *roots[2] = { "fonts", getenv( "GENERALSX_BUNDLED_FONTS_DIR" ) };
	static const char *extensions[] = { ".ttf", ".otf", ".ttc" };
	char candidate[512];
	for ( size_t r = 0; r < sizeof(roots) / sizeof(roots[0]); ++r ) {
		if ( roots[r] == nullptr || roots[r][0] == '\0' ) {
			continue;
		}
		for ( size_t i = 0; i < sizeof(extensions) / sizeof(extensions[0]); ++i ) {
			snprintf( candidate, sizeof(candidate), "%s/%s%s", roots[r], normalized, extensions[i] );
			if ( access( candidate, R_OK ) == 0 ) {
				FreetypeFontPath = candidate;
				return FreetypeFontPath;
			}
		}

		// Fall back to the Arial-equivalent face shipped with the app
		snprintf( candidate, sizeof(candidate), "%s/arial.ttf", roots[r] );
		if ( access( candidate, R_OK ) == 0 ) {
			FreetypeFontPath = candidate;
			return FreetypeFontPath;
		}
	}

	return nullptr;
}

#else // !(TARGET_OS_IPHONE || __ANDROID__)

////////////////////////////////////////////////////////////////////////////////////
//
//	Locate_Font_FontConfig
//
// TheSuperHackers @feature FreeType port 10/02/2026 Locate system font using Fontconfig
////////////////////////////////////////////////////////////////////////////////////
const char *
FontCharsClass::Locate_Font_FontConfig (const char *font_name)
{
	//
	//	Initialize Fontconfig library
	//
	FcConfig *config = FcInitLoadConfigAndFonts();
	if ( config == nullptr ) {
		return nullptr;
	}

	//
	//	Create a pattern for the requested font
	//
	FcPattern *pattern = FcNameParse( (const FcChar8*)font_name );
	if ( pattern == nullptr ) {
		FcConfigDestroy( config );
		return nullptr;
	}

	//
	//	Configure the pattern
	//
	FcConfigSubstitute( config, pattern, FcMatchPattern );
	FcDefaultSubstitute( pattern );

	//
	//	Find the best match
	//
	FcResult result = FcResultNoMatch;
	FcPattern *font = FcFontMatch( config, pattern, &result );

	const char *font_path = nullptr;
	if ( font != nullptr && result == FcResultMatch ) {
		//
		//	Extract the font file path
		//
		FcChar8 *file_path = nullptr;
		if ( FcPatternGetString( font, FC_FILE, 0, &file_path ) == FcResultMatch ) {
			FreetypeFontPath = (const char*)file_path;
			font_path = FreetypeFontPath;
		}

		FcPatternDestroy( font );
	}

	FcPatternDestroy( pattern );
	FcConfigDestroy( config );

	return font_path;
}

#endif // !(TARGET_OS_IPHONE || __ANDROID__)


////////////////////////////////////////////////////////////////////////////////////
//
//	Create_Freetype_Font
//
// GeneralsX @build fbraz 11/02/2026 BenderAI -  Initialize FreeType font (fighter19 pattern)
////////////////////////////////////////////////////////////////////////////////////
bool
FontCharsClass::Create_Freetype_Font (const char *font_name)
{
	//
	//	Initialize FreeType library
	//
	FT_Error error = FT_Init_FreeType( &FTLibrary );
	if ( error != 0 ) {
		return false;
	}

	//
	//	Handle "Generals" font mapping to Arial
	//
	bool doingGenerals = false;
	if ( strcmp( font_name, "Generals" ) == 0 ) {
		font_name = "Arial";
		doingGenerals = true;
	}

	//
	//	Calculate font height in pixels (96 DPI standard)
	//
	const int dotsPerInch = 96;
	int font_height = FT_MulDiv( PointSize, dotsPerInch, 72 );

	//
	//	Locate the font file using Fontconfig
	//
	const char *font_path = Locate_Font_FontConfig( font_name );
	if ( font_path == nullptr ) {
		FT_Done_FreeType( FTLibrary );
		FTLibrary = nullptr;
		return false;
	}

	//
	//	Load the font face
	//
	error = FT_New_Face( FTLibrary, font_path, 0, &FTFace );
	if ( error != 0 ) {
		FT_Done_FreeType( FTLibrary );
		FTLibrary = nullptr;
		return false;
	}

	//
	//	Set the font size (using pixel sizes for simplicity)
	//
	error = FT_Set_Pixel_Sizes( FTFace, 0, font_height );
	if ( error != 0 ) {
		FT_Done_Face( FTFace );
		FT_Done_FreeType( FTLibrary );
		FTFace = nullptr;
		FTLibrary = nullptr;
		return false;
	}

	//
	//	Calculate font metrics (Wine-compatible, same as fighter19)
	//
	if ( FT_IS_SCALABLE( FTFace ) ) {
		CharAscent = FT_MulFix( FTFace->ascender, FTFace->size->metrics.y_scale ) >> 6;
		int descent = -FT_MulFix( FTFace->descender, FTFace->size->metrics.y_scale ) >> 6;
		CharHeight = CharAscent + descent;
		CharOverhang = 0;
	} else {
		//
		//	Non-scalable fonts not supported
		//
		FT_Done_Face( FTFace );
		FT_Done_FreeType( FTLibrary );
		FTFace = nullptr;
		FTLibrary = nullptr;
		return false;
	}

	// GeneralsX @bugfix fbraz 03/06/2026 Log FreeType font details for Cyrillic font issue
	{
		GX_TRACE("[GX-ISSUE144] Freetype path=%s name=%s family=%s num_glyphs=%ld has_Cyrillic_Caps=%s\n",
			font_path,
			font_name,
			FTFace->family_name ? FTFace->family_name : "<null>",
			FTFace->num_glyphs,
			FT_Get_Char_Index(FTFace, 0x0410) != 0 ? "YES" : "NO");
	}

	if ( doingGenerals ) {
		CharOverhang = 0;
	}

	//
	//	Calculate pixel overlap (same logic as GDI version)
	//
	PixelOverlap = (-font_height) / 8;
	if ( PixelOverlap < 0 ) PixelOverlap = 0;
	if ( PixelOverlap > 4 ) PixelOverlap = 4;

	return true;
}


////////////////////////////////////////////////////////////////////////////////////
//
//	Store_Freetype_Char
//
// GeneralsX @build fbraz 11/02/2026 BenderAI - FreeType character rendering (fighter19 pattern)
////////////////////////////////////////////////////////////////////////////////////
const FontCharsClassCharDataStruct *
FontCharsClass::Store_Freetype_Char (WCHAR ch)
{
	GX_TRACE("Store_Freetype_Char: enter ch=U+%04X font=%s FTFace=%p\n", (unsigned int)ch, GDIFontName.str(), (void*)FTFace);

	//
	//	Get the glyph index for the character
	//
	FT_UInt glyph_index = FT_Get_Char_Index( FTFace, ch );
	GX_TRACE("Store_Freetype_Char: FT_Get_Char_Index returned glyph_index=%u\n", (unsigned int)glyph_index);

	// GeneralsX @bugfix Android port 27/09/2026 A code point the face has no glyph for used to be
	// rendered as glyph 0 (an empty box). The game text for zh/ko/ar/fa is almost entirely such
	// code points, because the bundled faces are Latin/Cyrillic only, so look the glyph up in the
	// fallback faces (system CJK/Arabic fonts) and render it from whichever face has it.
	FT_Face face = FTFace;
	if ( glyph_index == 0 && ch >= 0x80 ) {
		FT_UInt fallback_index = 0;
		FT_Face fallback = Find_Freetype_Fallback_Face( ch, &fallback_index );
		if ( fallback != nullptr ) {
			face = fallback;
			glyph_index = fallback_index;
		} else if ( Arabic_Presentation_Base( ch ) == 0 ) {
			// Said once per code point, so a box on screen can be traced to the character a
			// fallback font is still missing for.
			static uint8 s_reported[0x10000 / 8];
			const unsigned int code = (unsigned int)ch & 0xFFFFu;
			if ( ( s_reported[code >> 3] & ( 1u << ( code & 7 ) ) ) == 0 ) {
				s_reported[code >> 3] |= (uint8)( 1u << ( code & 7 ) );
				fprintf( stderr, "[font] no glyph for U+%04X in %s or any fallback face\n", code, GDIFontName.str() );
			}
		} else {
			// GeneralsX @feature Android port 27/09/2026 A face with Arabic letters but without
			// the presentation-forms block: draw the unjoined letter rather than a box.
			const WCHAR base = Arabic_Presentation_Base( ch );
			glyph_index = FT_Get_Char_Index( FTFace, base );
			if ( glyph_index == 0 ) {
				fallback = Find_Freetype_Fallback_Face( base, &fallback_index );
				if ( fallback != nullptr ) {
					face = fallback;
					glyph_index = fallback_index;
				}
			}
		}
	}

	// GeneralsX @bugfix fbraz 03/06/2026 Log ALL Cyrillic character rendering attempts
	if (ch >= 0x0400 && ch <= 0x04FF) {
		GX_TRACE("[GX-ISSUE144] Store_Freetype_Char U+%04X glyph_idx=%u font=%s\n",
			(unsigned int)ch,
			(unsigned int)glyph_index,
			GDIFontName.str());
	}

	//
	//	Load the glyph (without rendering yet)
	//
	FT_Error error = FT_Load_Glyph( face, glyph_index, FT_LOAD_DEFAULT );
	GX_TRACE("Store_Freetype_Char: FT_Load_Glyph returned error=%d\n", (int)error);
	if ( error != 0 ) {
		return nullptr;
	}

	//
	//	Convert to an anti-aliased bitmap
	//
	error = FT_Render_Glyph( face->glyph, FT_RENDER_MODE_NORMAL );
	GX_TRACE("Store_Freetype_Char: FT_Render_Glyph returned error=%d\n", (int)error);
	if ( error != 0 ) {
		return nullptr;
	}

	FT_GlyphSlot glyph = face->glyph;
	GX_TRACE("Store_Freetype_Char: glyph slot=%p bitmap.width=%u bitmap.rows=%u advance.x=%ld\n",
		(void*)glyph, glyph ? glyph->bitmap.width : 0u, glyph ? glyph->bitmap.rows : 0u, glyph ? (long)glyph->advance.x : 0L);

	//
	//	Calculate X position (special case for 'W')
	//
	int x_pos = 0;
	if ( ch == 'W' ) {
		x_pos = 1;
	}

	//
	//	Calculate character width (advance + overlap)
	//
	unsigned int char_width = glyph->advance.x >> 6;

	//
	//	Sometimes bitmap is wider than advancement (fix it)
	//
	if ( char_width < glyph->bitmap.width + glyph->bitmap_left ) {
		char_width = glyph->bitmap.width + glyph->bitmap_left;
	}
	char_width += PixelOverlap + x_pos;

	//
	//	Get a pointer to the buffer for this character (allocates if needed)
	//
	Update_Current_Buffer( char_width );
	uint16 *curr_buffer_p = BufferList[BufferList.Count() - 1]->Buffer;
	curr_buffer_p += CurrPixelOffset;

	//
	//	Calculate bitmap offsets (match GDI baseline)
	//
	int x_offset = glyph->bitmap_left;
	int descent = CharHeight - CharAscent;
	int y_offset = (CharHeight - glyph->bitmap_top) - descent;

	//
	//	Prevent invalid buffer access
	//
	if ( x_offset < 0 ) x_offset = 0;
	if ( y_offset < 0 ) y_offset = 0;

	//
	//	Copy FreeType bitmap to our buffer (convert 8-bit gray → 16-bit format)
	//
	// GeneralsX @bugfix Android port 14/07/2026 y_offset/x_offset are only
	// clamped to >= 0 above, never to the buffer's actual reserved region
	// (char_width * CharHeight uint16s, carved out of a fixed 32768-entry
	// FontCharsBuffer in Update_Current_Buffer()). A glyph whose real bitmap
	// overshoots the font's nominal ascender/descent (bitmap_top taller than
	// CharAscent, or bitmap.rows taller than CharHeight -- both plausible for
	// large bold display fonts) pushes dst_index past this glyph's slice and
	// silently corrupts whatever comes next in the buffer, up to and including
	// memory past the struct's own fixed array -- a heap write with no
	// exception and no signal until something unrelated reads the corrupted
	// memory later. Skip any row/col that would land outside this glyph's
	// own [0, char_width*CharHeight) region instead of writing blindly.
	for ( unsigned int row = 0; row < glyph->bitmap.rows; row++ ) {
		int dst_row = y_offset + (int)row;
		if ( dst_row < 0 || dst_row >= CharHeight )
			continue;

		int src_index = row * glyph->bitmap.pitch;
		int dst_index = dst_row * char_width;

		for ( unsigned int col = 0; col < glyph->bitmap.width; col++ ) {
			int dst_col = x_offset + (int)col;
			if ( dst_col < 0 || dst_col >= char_width )
				continue;

			//
			//	Get 8-bit grayscale pixel
			//
			uint8 pixel_value = glyph->bitmap.buffer[src_index + col];

			uint16 pixel_color = 0;
			if ( pixel_value != 0 ) {
				pixel_color = 0x0FFF;	// White (12-bit RGB444)
			}

			//
			//	Format: 4-bit alpha (top nibble) + 12-bit color (bottom bits)
			//	SAME FORMAT AS GDI IMPLEMENTATION
			//
			uint8 alpha_value = (pixel_value >> 4) & 0xF;
			curr_buffer_p[dst_index + dst_col] = pixel_color | (alpha_value << 12);
		}
	}

	//
	//	Save information about this character
	//
	FontCharsClassCharDataStruct *char_data = W3DNEW FontCharsClassCharDataStruct;
	char_data->Value = ch;
	char_data->Width = (short)char_width;
	// GeneralsX @bugfix Android port 30/07/2026 record the true advance
	// separately from the atlas cell width above; see Get_Char_Spacing.
	// Guard against a zero advance (some glyphs legitimately have none, and
	// zero would mean "unset" to Get_Char_Spacing) by leaving it unset so the
	// legacy formula still applies.
	char_data->Advance = (short)(glyph->advance.x >> 6);
	char_data->Buffer = BufferList[BufferList.Count() - 1]->Buffer + CurrPixelOffset;

	//
	//	Insert into character array (ASCII or Unicode)
	//
	if ( ch < 256 ) {
		ASCIICharArray[ch] = char_data;
	} else {
		UnicodeCharArray[ch - FirstUnicodeChar] = char_data;
	}

	//
	//	Advance the pixel offset for next character
	//
	CurrPixelOffset += (char_width * CharHeight);

	//
	//	Return the character data
	//
	return char_data;
}


////////////////////////////////////////////////////////////////////////////////////
//
//	Freetype fallback font files
//
// GeneralsX @bugfix Android port 27/09/2026 Candidate faces for glyphs the base face lacks.
// Android ships its CJK/Arabic fonts in /system/fonts under stable file names; the Pan-CJK
// collection's face 2 is Simplified Chinese, whose glyph set also covers Hangul and kana. A
// fonts/fallback.* file in the game-data folder is tried first so a player can supply a face.
// On fontconfig platforms the same families are resolved by name, and a candidate is only
// accepted when fontconfig returns that family rather than its generic substitute.
////////////////////////////////////////////////////////////////////////////////////
namespace
{
struct FallbackFontFile
{
	const char *	path;
	int				face_index;
};

static const FallbackFontFile kBundledFallbackFiles[] = {
	{ "fonts/fallback.ttf", 0 },
	{ "fonts/fallback.otf", 0 },
	{ "fonts/fallback.ttc", 0 },
};

#if defined(__ANDROID__)
static const FallbackFontFile kSystemFallbackFiles[] = {
	{ "/system/fonts/NotoSansCJK-Regular.ttc", 2 },
	{ "/system/fonts/NotoSansSC-Regular.otf", 0 },
	{ "/system/fonts/NotoSansKR-Regular.otf", 0 },
	{ "/system/fonts/DroidSansFallbackFull.ttf", 0 },
	{ "/system/fonts/DroidSansFallback.ttf", 0 },
	{ "/system/fonts/NotoNaskhArabic-Regular.ttf", 0 },
	{ "/system/fonts/NotoNaskhArabicUI-Regular.ttf", 0 },
	{ "/system/fonts/NotoSansArabic-Regular.ttf", 0 },
	// GeneralsX @bugfix Android port 27/09/2026 Symbols. The GeneralsOnline lobby prefixes every
	// room name with "[region][shield]", and the shield drew as an empty box here while the PC
	// shows it: Windows' GDI links a missing glyph to its symbol fonts, and this list had none.
	{ "/system/fonts/NotoSansSymbols-Regular-Subsetted.ttf", 0 },
	{ "/system/fonts/NotoSansSymbols-Regular-Subsetted2.ttf", 0 },
	{ "/system/fonts/NotoSansSymbols2-Regular.ttf", 0 },
	{ "/system/fonts/DroidSans.ttf", 0 },
};
#endif

#if !(defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE) && !defined(__ANDROID__)
static const char *kFontconfigFallbackFamilies[] = {
	"Noto Sans CJK SC",
	"Noto Sans CJK KR",
	"WenQuanYi Zen Hei",
	"Droid Sans Fallback",
	"Noto Naskh Arabic",
	"Noto Sans Arabic",
	"Noto Sans Symbols",
	"Noto Sans Symbols2",
	"DejaVu Sans",
};
#endif
}

void
FontCharsClass::Load_Freetype_Fallback_Faces (void)
{
	FTFallbackFacesLoaded = true;
	if ( FTLibrary == nullptr ) {
		return;
	}

	const int font_height = FT_MulDiv( PointSize, 96, 72 );

	auto add_face = [&]( const char *path, int face_index ) {
		if ( FTFallbackFaceCount >= MAX_FT_FALLBACK_FACES ) {
			return;
		}
		if ( FreetypeFontPath.Get_Length() > 0 && strcmp( path, FreetypeFontPath.Peek_Buffer() ) == 0 ) {
			return;
		}
		FT_Face face = nullptr;
		FT_Error error = FT_New_Face( FTLibrary, path, face_index, &face );
		if ( error != 0 && face_index != 0 ) {
			error = FT_New_Face( FTLibrary, path, 0, &face );
		}
		if ( error != 0 ) {
			return;
		}
		if ( !FT_IS_SCALABLE( face ) || FT_Set_Pixel_Sizes( face, 0, font_height ) != 0 ) {
			FT_Done_Face( face );
			return;
		}
		GX_TRACE("[fontfallback] %s face %d family=%s glyphs=%ld for font=%s\n",
			path, face_index, face->family_name ? face->family_name : "<null>", face->num_glyphs, GDIFontName.str());
		FTFallbackFaces[FTFallbackFaceCount++] = face;
	};

	for ( const FallbackFontFile &file : kBundledFallbackFiles ) {
		if ( access( file.path, R_OK ) == 0 ) {
			add_face( file.path, file.face_index );
		}
	}

#if defined(__ANDROID__)
	for ( const FallbackFontFile &file : kSystemFallbackFiles ) {
		if ( access( file.path, R_OK ) == 0 ) {
			add_face( file.path, file.face_index );
		}
	}
#elif !(defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE)
	FcConfig *config = FcInitLoadConfigAndFonts();
	if ( config != nullptr ) {
		for ( const char *family : kFontconfigFallbackFamilies ) {
			FcPattern *pattern = FcNameParse( (const FcChar8*)family );
			if ( pattern == nullptr ) {
				continue;
			}
			FcConfigSubstitute( config, pattern, FcMatchPattern );
			FcDefaultSubstitute( pattern );
			FcResult result = FcResultNoMatch;
			FcPattern *match = FcFontMatch( config, pattern, &result );
			if ( match != nullptr && result == FcResultMatch ) {
				FcChar8 *matched_family = nullptr;
				FcChar8 *file_path = nullptr;
				int face_index = 0;
				FcPatternGetInteger( match, FC_INDEX, 0, &face_index );
				if ( FcPatternGetString( match, FC_FAMILY, 0, &matched_family ) == FcResultMatch
					&& strcasecmp( (const char*)matched_family, family ) == 0
					&& FcPatternGetString( match, FC_FILE, 0, &file_path ) == FcResultMatch ) {
					add_face( (const char*)file_path, face_index );
				}
			}
			if ( match != nullptr ) {
				FcPatternDestroy( match );
			}
			FcPatternDestroy( pattern );
		}
		FcConfigDestroy( config );
	}
#endif

	if ( FTFallbackFaceCount == 0 ) {
		GX_TRACE("[fontfallback] no fallback face found for font=%s; glyphs outside it render empty\n", GDIFontName.str());
	}
}

FT_Face
FontCharsClass::Find_Freetype_Fallback_Face (WCHAR ch, FT_UInt *glyph_index)
{
	if ( !FTFallbackFacesLoaded ) {
		Load_Freetype_Fallback_Faces();
	}

	for ( int i = 0; i < FTFallbackFaceCount; i++ ) {
		FT_UInt index = FT_Get_Char_Index( FTFallbackFaces[i], ch );
		if ( index != 0 ) {
			*glyph_index = index;
			return FTFallbackFaces[i];
		}
	}
	return nullptr;
}


////////////////////////////////////////////////////////////////////////////////////
//
//	Free_Freetype_Font
//
// GeneralsX @build fbraz 11/02/2026 BenderAI - Cleanup FreeType resources
////////////////////////////////////////////////////////////////////////////////////
void
FontCharsClass::Free_Freetype_Font (void)
{
	//
	//	Free the FreeType face
	//
	if ( FTFace != nullptr ) {
		FT_Done_Face( FTFace );
		FTFace = nullptr;
	}

	for ( int i = 0; i < FTFallbackFaceCount; i++ ) {
		FT_Done_Face( FTFallbackFaces[i] );
		FTFallbackFaces[i] = nullptr;
	}
	FTFallbackFaceCount = 0;
	FTFallbackFacesLoaded = false;

	//
	//	Free the FreeType library
	//
	if ( FTLibrary != nullptr ) {
		FT_Done_FreeType( FTLibrary );
		FTLibrary = nullptr;
	}
}

#endif // SAGE_USE_FREETYPE && !_WIN32


////////////////////////////////////////////////////////////////////////////////////
//
//	Initialize_GDI_Font
//
////////////////////////////////////////////////////////////////////////////////////
bool
FontCharsClass::Initialize_GDI_Font (const char *font_name, int point_size, bool is_bold)
{
	//
	//	Build a unique name from the font name and its size
	//
	Name.Format ("%s%d", font_name, point_size);

	//
	//	Remember these settings
	//
	GDIFontName	= font_name;
	PointSize	= point_size;
	IsBold		= is_bold;

	//
	//	Create the actual font object (platform-specific)
	//  TheSuperHackers @feature FreeType port 10/02/2026 Dispatch to FreeType on Linux
	//
#if defined(SAGE_USE_FREETYPE) && !defined(_WIN32)
	return Create_Freetype_Font (font_name);
#else
	return Create_GDI_Font (font_name);
#endif
}


////////////////////////////////////////////////////////////////////////////////////
//
//	Is_Font
//
////////////////////////////////////////////////////////////////////////////////////
bool
FontCharsClass::Is_Font (const char *font_name, int point_size, bool is_bold)
{
	bool retval = false;

	//
	//	Check to see if both the name and height matches...
	//
	if (	(GDIFontName.Compare_No_Case (font_name) == 0) &&
			(point_size == PointSize) &&
			(is_bold == IsBold))
	{
		retval = true;
	}

	return retval;
}


////////////////////////////////////////////////////////////////////////////////////
//
//	Grow_Unicode_Array
//
////////////////////////////////////////////////////////////////////////////////////
void
FontCharsClass::Grow_Unicode_Array (WCHAR ch)
{
	//
	//	Don't do anything if character is in the ASCII range
	//
	if ( ch < 256 ) {
		return ;
	}

	//
	//	Don't do anything if character is in the currently allocated range
	//
	if ( ch >= FirstUnicodeChar && ch <= LastUnicodeChar ) {
		return ;
	}

	uint16 first_index	= min( FirstUnicodeChar, static_cast<uint16>(ch) );
	uint16 last_index		= max( LastUnicodeChar, static_cast<uint16>(ch) );
	uint16 count			= (last_index - first_index) + 1;

	//
	//	Allocate enough memory to hold the new cells
	//
	FontCharsClassCharDataStruct **new_array = W3DNEWARRAY FontCharsClassCharDataStruct *[count];
	::memset (new_array, 0, sizeof (FontCharsClassCharDataStruct *) * count);

	//
	//	Copy the contents of the old array into the new array
	//
	if ( UnicodeCharArray != nullptr ) {
		int start_offset	= (FirstUnicodeChar - first_index);
		int old_count		= (LastUnicodeChar - FirstUnicodeChar) + 1;
		::memcpy (&new_array[start_offset], UnicodeCharArray, sizeof (FontCharsClassCharDataStruct *) * old_count);

		//
		//	Delete the old array
		//
		delete [] UnicodeCharArray;
		UnicodeCharArray = nullptr;
	}

	FirstUnicodeChar	= first_index;
	LastUnicodeChar	= last_index;
	UnicodeCharArray	= new_array;
}


////////////////////////////////////////////////////////////////////////////////////
//
//	Free_Character_Arrays
//
////////////////////////////////////////////////////////////////////////////////////
void
FontCharsClass::Free_Character_Arrays ()
{
	if ( UnicodeCharArray != nullptr ) {

		int count = (LastUnicodeChar - FirstUnicodeChar) + 1;

		//
		//	Delete each member of the unicode array
		//
		for (int index = 0; index < count; index ++) {
			delete UnicodeCharArray[index];
			UnicodeCharArray[index] = nullptr;
		}

		//
		//	Delete the array itself
		//
		delete [] UnicodeCharArray;
		UnicodeCharArray = nullptr;
	}

	//
	//	Delete each member of the ascii character array
	//
	for (int index = 0; index < 256; index ++) {
		delete ASCIICharArray[index];
		ASCIICharArray[index] = nullptr;
	}
}
