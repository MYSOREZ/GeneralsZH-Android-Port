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

////////////////////////////////////////////////////////////////////////////////
//																																						//
//  (c) 2001-2003 Electronic Arts Inc.																				//
//																																						//
////////////////////////////////////////////////////////////////////////////////

//----------------------------------------------------------------------------
//
//                       Westwood Studios Pacific.
//
//                       Confidential Information
//                Copyright(C) 2001 - All Rights Reserved
//
//----------------------------------------------------------------------------
//
// Project:   RTS3
//
// File name: GameText.cpp
//
// Created:   11/07/01
//
//----------------------------------------------------------------------------

//----------------------------------------------------------------------------
//         Includes
//----------------------------------------------------------------------------

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine
#include "Common/GXManagedMods.h"
#include <cstdlib>
#include <cctype>

#include "GameClient/GameText.h"
#include "Common/Language.h"
#include "Common/Registry.h"
#include "GameClient/LanguageFilter.h"
#include "Common/Debug.h"
#include "Common/UnicodeString.h"
#include "Common/AsciiString.h"
#include "Common/GlobalData.h"
#include "Common/file.h"
#include "Common/FileSystem.h"
#include "Common/ArchiveFileSystem.h"
#include "Common/ArchiveFile.h"
#include "Common/LocalFileSystem.h"
#include "Common/version.h"
#include <map>
#include <string>
#include <vector>




//----------------------------------------------------------------------------
//         Externals
//----------------------------------------------------------------------------

#if defined(RTS_DEBUG)
Bool g_useStringFile = TRUE;
#endif


//----------------------------------------------------------------------------
//         Defines
//----------------------------------------------------------------------------

#define CSF_ID ( ('C'<<24) | ('S'<<16) | ('F'<<8) | (' ') )
#define CSF_LABEL ( ('L'<<24) | ('B'<<16) | ('L'<<8) | (' ') )
#define CSF_STRING ( ('S'<<24) | ('T'<<16) | ('R'<<8) | (' ') )
#define CSF_STRINGWITHWAVE ( ('S'<<24) | ('T'<<16) | ('R'<<8) | ('W') )
#define CSF_VERSION 3

#define STRING_FILE 0
#define CSF_FILE 1
#define MAX_UITEXT_LENGTH (10*1024)
//----------------------------------------------------------------------------
//         Private Types
//----------------------------------------------------------------------------

//===============================
// StringInfo
//===============================

struct StringInfo
{
	AsciiString			label;
	UnicodeString		text;
	AsciiString			speech;
};

struct StringLookUp
{
	AsciiString		*label;
	StringInfo		*info;
};

//===============================
// CSFHeader
//===============================

struct CSFHeader
{
	Int id;
	Int version;
	Int num_labels;
	Int num_strings;
	Int skip;
	Int langid;

};

//===============================
// struct NoString
//===============================

struct NoString
{
	struct NoString *next;
	UnicodeString text;
};


//===============================
// GameTextManager
//===============================

class GameTextManager : public GameTextInterface
{
	public:

		GameTextManager();
		virtual ~GameTextManager() override;

		virtual void					init() override;						///< Initializes the text system
		virtual void					deinit();					///< Shuts down the text system
		virtual void					update() override {};			///< update text manager
		virtual void					reset() override;					///< Resets the text system

		virtual UnicodeString fetch( const Char *label, Bool *exists = nullptr ) override;		///< Returns the associated labeled unicode text
		virtual UnicodeString fetch( AsciiString label, Bool *exists = nullptr ) override;		///< Returns the associated labeled unicode text
		virtual UnicodeString fetchFormat( const Char *label, ... ) override;
		virtual UnicodeString fetchOrSubstitute( const Char *label, const WideChar *substituteText ) override;
		virtual UnicodeString fetchOrSubstituteFormat( const Char *label, const WideChar *substituteFormat, ... ) override;
		virtual UnicodeString fetchOrSubstituteFormatVA( const Char *label, const WideChar *substituteFormat, va_list args ) override;

		virtual AsciiStringVec& getStringsWithLabelPrefix(AsciiString label) override;

		virtual void					initMapStringFile( const AsciiString& filename ) override;

	protected:

		Int							m_textCount;
		Int							m_maxLabelLen;
		Char						m_buffer[MAX_UITEXT_LENGTH];
		Char						m_buffer2[MAX_UITEXT_LENGTH];
		Char						m_buffer3[MAX_UITEXT_LENGTH];
		WideChar				m_tbuffer[MAX_UITEXT_LENGTH*2];

		StringInfo			*m_stringInfo;
		StringLookUp		*m_stringLUT;
		StringInfo			*m_fallbackStringInfo;
		StringLookUp		*m_fallbackStringLUT;
		Int						m_fallbackTextCount;
		// GeneralsX @feature Android port 18/10/2026 Labels a mod added or changed, consulted before
		// the language pack (see init()).
		StringInfo			*m_overrideStringInfo;
		StringLookUp		*m_overrideStringLUT;
		Int						m_overrideTextCount;
		void						buildModTextLayers( Int format, const AsciiString& textLanguage );
		static StringLookUp *makeLUT( StringInfo *info, Int count );
		Bool						m_initialized;
#if defined(RTS_DEBUG)
		Bool						m_jabberWockie;
		Bool						m_munkee;
#endif
		NoString				*m_noStringList;
		Int							m_useStringFile;
		LanguageID			m_language;
		UnicodeString		m_failed;

		StringInfo			*m_mapStringInfo;
		StringLookUp		*m_mapStringLUT;
		Int							m_mapTextCount;

		/// m_asciiStringVec will be altered every time that getStringsWithLabelPrefix is called,
		/// so don't simply store a pointer to it.
		AsciiStringVec			m_asciiStringVec;

		void						stripSpaces ( WideChar *string );
		void						removeLeadingAndTrailing ( Char *m_buffer );
		void						readToEndOfQuote( File *file, Char *in, Char *out, Char *wavefile, Int maxBufLen );
		void						reverseWord ( Char *file, Char *lp );
		void						translateCopy( WideChar *outbuf, Char *inbuf );
		Bool						getStringCount( const Char *filename, Int& textCount );
		Bool						getCSFInfo ( const Char *filename, Int& textCount, LanguageID& language, FileInstance instance = 0 );
		Bool						parseCSF(  const Char *filename, StringInfo *stringInfo, Int textCount, Int& maxLabelLen, FileInstance instance = 0 );
		Bool						parseStringFile( const char *filename );
		Bool						parseMapStringFile( const char *filename );
		Bool						readLine( char *buffer, Int max, File *file );
		Char						readChar( File *file );
};

static int __cdecl			compareLUT ( const void *,  const void*);
//----------------------------------------------------------------------------
//         Private Data
//----------------------------------------------------------------------------

// GeneralsX @feature Android port 24/09/2026 Strings the installed game data does not have.
//
// Two sources of labels reach fetch() with no entry in any generals.csf:
//   - the Steam release's patch archives: PatchWindow.big adds a "Custom Mission" button
//     labelled GUI:CustomMission and a menu whose start button is GUI:StartCustomMission,
//     and the only text for them is PatchData.big's Data\Patch.str -- English, and read by
//     nothing but the executable that shipped with it;
//   - buttons this port adds itself (the touch force-attack button, issue #25).
//
// Translations of these labels belong in the language packs, like every other string:
// languages/<language>/generals.str carries them, one file per language
// (languages/README.md). What is here is only the English text, answered last, after every
// table the game loaded has missed -- so an English game, or an official non-English .csf
// that no pack replaces, still shows words instead of "MISSING".
//
// One label is not given text at all: the patch's "START GAME" is the skirmish menu's
// own "PLAY GAME", which already exists in every language the game ships in and in every
// pack made for it. Pointing at it is better than translating it a second time.
struct GXLabelAlias
{
	const char *label;
	const char *existingLabel;
};

static const GXLabelAlias s_gxLabelAliases[] =
{
	{ "GUI:StartCustomMission", "GUI:StartGame" },
};

struct GXEnglishDefault
{
	const char *label;
	const wchar_t *text;
};

static const GXEnglishDefault s_gxEnglishDefaults[] =
{
	{ "GUI:CustomMission",     L"CUSTOM MISSION" },
	{ "GX:ForceAttack",        L"Force Attack" },
	{ "GX:ToolTipForceAttack", L"Attack whatever you tap, even your own units or empty ground, until you press this again" },
	{ "GX:BuilderPageMore",        L"More Orders" },
	{ "GX:ToolTipBuilderPageMore", L"Show more orders: force attack, waypoints, scatter, formation" },
	{ "GX:BuilderPageBack",        L"Back" },
	{ "GX:ToolTipBuilderPageBack", L"Back to this unit's own buttons" },
	{ "GX:Scatter",                L"Scatter" },
	{ "GX:ToolTipScatter",         L"The selected units spread out from their centre, away from artillery and bombs" },
	{ "GX:Formation",              L"Formation" },
	{ "GX:ToolTipFormation",       L"The selected units keep their places and move together at the speed of the slowest. Press again to break the formation" },
	// GeneralsOnline buttons the original menus do not have (score screen, player info).
	{ "GX:ViewMatchOnline",    L"VIEW MATCH ONLINE" },
	{ "GX:Logout",             L"LOGOUT" },
};



//----------------------------------------------------------------------------
//         Public Data
//----------------------------------------------------------------------------

GameTextInterface *TheGameText = nullptr;

//----------------------------------------------------------------------------
//         Private Prototypes
//----------------------------------------------------------------------------



//----------------------------------------------------------------------------
//         Private Functions
//----------------------------------------------------------------------------



//----------------------------------------------------------------------------
//         Public Functions
//----------------------------------------------------------------------------

//============================================================================
// CreateGameTextInterface
//============================================================================

GameTextInterface* CreateGameTextInterface()
{
	return NEW GameTextManager;
}


//============================================================================
// GameTextManager::GameTextManager
//============================================================================

GameTextManager::GameTextManager()
:	m_textCount(0),
	m_maxLabelLen(0),
	m_stringInfo(nullptr),
	m_stringLUT(nullptr),
	m_fallbackStringInfo(nullptr),
	m_fallbackStringLUT(nullptr),
	m_fallbackTextCount(0),
	m_overrideStringInfo(nullptr),
	m_overrideStringLUT(nullptr),
	m_overrideTextCount(0),
	m_initialized(FALSE),
	m_noStringList(nullptr),
#if defined(RTS_DEBUG)
	m_jabberWockie(FALSE),
	m_munkee(FALSE),
	m_useStringFile(g_useStringFile),
#else
	m_useStringFile(TRUE),
#endif
	m_mapStringInfo(nullptr),
	m_mapStringLUT(nullptr),
	m_failed(L"***FATAL*** String Manager failed to initialize properly")
{
	for(Int i=0; i < MAX_UITEXT_LENGTH; i++)
	{
		m_buffer[i] = 0;
		m_buffer2[i] = 0;
		m_buffer3[i] = 0;
	}
}

//============================================================================
// GameTextManager::~GameTextManager
//============================================================================

GameTextManager::~GameTextManager()
{
	deinit();
}

//============================================================================
// GameTextManager::init
//============================================================================

extern const Char *g_strFile;
extern const Char *g_csfFile;

void GameTextManager::init()
{
	// GeneralsX @feature Android port 09/09/2026 The language of the TEXT, which is not
	// necessarily the language of the game.
	//
	// GetRegistryLanguage() answers "which localised build is this", and the whole engine
	// leans on it: header templates, fonts, the command map, and through those the menu
	// artwork. A language pack is none of that -- it is a string table, and pointing the
	// engine's language at it swapped Zero Hour's branding for the base Generals one, because
	// those were the assets that still resolved under the new name.
	//
	// So a pack sets GENERALSX_TEXT_LANGUAGE and only this function reads it. Without it,
	// nothing changes anywhere: the game's own language answers, exactly as before.
	AsciiString textLanguage;
	{
		const char *packLanguage = getenv("GENERALSX_TEXT_LANGUAGE");
		textLanguage = (packLanguage != nullptr && packLanguage[0] != '\0')
			? AsciiString(packLanguage)
			: GetRegistryLanguage();
	}

	AsciiString csfFile;
	csfFile.format(g_csfFile, textLanguage.str());

	// GeneralsX @feature Android port 09/09/2026 The .str path may now be per-language.
	//
	// g_strFile was a single fixed path, "data/Generals.str" -- one file, no language in it,
	// a development convenience that shipped installs never contained. The CSF path next to
	// it has always been per-language ("data/%s/generals.csf"), so this makes the two match:
	// if the platform's g_strFile carries a %s it is filled in with the current language, and
	// if it does not it is used verbatim, which leaves the Windows build and every tool
	// exactly as they were.
	AsciiString strFile;
	if( strstr( g_strFile, "%s" ) != nullptr )
		strFile.format( g_strFile, textLanguage.str() );
	else
		strFile = g_strFile;

	Int format;

	// GeneralsX @bugfix BenderAI 16/02/2026 - Debug CSF init
	fprintf(stderr, "[CSF] init() - START\n");

	if ( m_initialized )
	{
		return;
	}

	m_initialized = TRUE;

	m_maxLabelLen = 0;
#if defined(RTS_DEBUG)
	if(TheGlobalData)
	{
		m_jabberWockie = TheGlobalData->m_jabberOn;
		m_munkee = 	TheGlobalData->m_munkeeOn;
	}
#endif

	if ( m_useStringFile && getStringCount( strFile.str(), m_textCount ) )
	{
		format = STRING_FILE;
	}
	else if ( getCSFInfo ( csfFile.str(), m_textCount, m_language ) )
	{
		fprintf(stderr, "[CSF] init() - getCSFInfo OK, textCount=%d\n", m_textCount);
		format = CSF_FILE;
	}
	else
	{
		fprintf(stderr, "[CSF] init() - getCSFInfo FAILED\n");
		return;
	}

	if( m_textCount == 0 )
	{
		return;
	}

	//Allocate StringInfo Array

	m_stringInfo = NEW StringInfo[m_textCount];

	if( m_stringInfo == nullptr )
	{
		deinit();
		return;
	}

	if ( format == STRING_FILE )
	{
		if( parseStringFile( strFile.str() ) == FALSE )
		{
			deinit();
			return;
		}
	}
	else
	{
		fprintf(stderr, "[CSF] init() - Calling parseCSF()...\n");
		if ( !parseCSF ( csfFile.str(), m_stringInfo, m_textCount, m_maxLabelLen ) )
		{
			fprintf(stderr, "[CSF] init() - parseCSF FAILED\n");
			deinit();
			return;
		}
		fprintf(stderr, "[CSF] init() - parseCSF SUCCESS\n");
	}

	m_stringLUT = NEW StringLookUp[m_textCount];

	StringLookUp *lut = m_stringLUT;
	StringInfo *info = m_stringInfo;

	for ( Int i = 0; i < m_textCount; i++ )
	{
		lut->info = info;
		lut->label = &info->label;
		lut++;
		info++;
	}

	qsort( m_stringLUT, m_textCount, sizeof(StringLookUp), compareLUT  );

	// GeneralsX @bugfix BenderAI 22/05/2026 Load fallback CSF instance when a mod provides an incomplete table.
	// GeneralsX @feature Android port 18/10/2026 Generalised: every string table there is, mods'
	// and the game's, in the pack's language, the game's and English, and the labels a mod added
	// or changed above the language pack (buildModTextLayers).
	buildModTextLayers( format, textLanguage );
}

//============================================================================
// GameTextManager::buildModTextLayers
//============================================================================
// GeneralsX @feature Android port 18/10/2026 Mods and the language packs together.
//
// A language pack (.str) translates Zero Hour's own labels. A mod brings its own generals.csf --
// usually English -- with labels of its own (new units, buildings, tooltips) and some of the
// game's changed. With one fallback table in the game's language, a Russian install with an
// English mod showed MISSING for everything the mod added (4PDA, Shockwave screenshot).
//
// Every copy of data/<language>/generals.csf the file system has is read, for the pack's
// language, the game's and English, highest priority first. A copy is a mod's when it is a loose
// file or comes from an archive named with the leading '!' mods use (the same rule loadMods()
// uses for the community patch); the first copy that is not, is the game's. Then:
//   - override, consulted before the pack: every label a mod table has that the game's table of
//     the same language lacks or words differently -- what the mod added or changed, in the mod's
//     own words (and in the pack's language when the mod ships that language);
//   - fallback, after the pack: every label of every table, first copy wins -- so nothing the
//     game or a mod has ever shows MISSING.
// Without a pack (format == CSF_FILE) the primary table already is the top copy, mod or not, and
// only the fallback is built. A label the pack and the mod both have and the mod did not change
// stays translated.
namespace
{
	struct GXCsfTable
	{
		Bool isMod;
		std::vector<StringInfo> strings;
	};

	Bool gxIsModArchiveName( const AsciiString& path )
	{
		const char *name = path.str();
		const char *base = name;
		for( const char *c = name; *c; ++c )
			if( *c == '/' || *c == '\\' )
				base = c + 1;
		if( base[0] == '!' )
			return TRUE;
		// GeneralsX @feature Android port 18/10/2026 Also the archives of a -mod folder and of the
		// mods turned on in the launcher's Mods screen (GXManagedMods.h).
		if( GXManagedMods::contains( name ) )
			return TRUE;
		if( TheGlobalData != nullptr && TheGlobalData->m_modDir.isNotEmpty() )
		{
			// parseMod() ends the folder with '\\'; the archive paths under it use '/'.
			AsciiString dir = TheGlobalData->m_modDir;
			dir.removeLastChar();
			if( path.startsWithNoCase( dir.str() ) )
				return TRUE;
		}
		return TheGlobalData != nullptr && TheGlobalData->m_modBIG.isNotEmpty()
			&& path.compareNoCase( TheGlobalData->m_modBIG ) == 0;
	}
}

StringLookUp *GameTextManager::makeLUT( StringInfo *info, Int count )
{
	StringLookUp *lut = NEW StringLookUp[count];
	for( Int i = 0; i < count; ++i )
	{
		lut[i].info = &info[i];
		lut[i].label = &info[i].label;
	}
	qsort( lut, count, sizeof(StringLookUp), compareLUT );
	return lut;
}

void GameTextManager::buildModTextLayers( Int format, const AsciiString& textLanguage )
{
	std::vector<AsciiString> languages;
	const AsciiString candidates[3] = { textLanguage, GetRegistryLanguage(), AsciiString("english") };
	for( Int i = 0; i < 3; ++i )
	{
		Bool seen = candidates[i].isEmpty();
		for( size_t j = 0; j < languages.size() && !seen; ++j )
			seen = languages[j].compareNoCase( candidates[i] ) == 0;
		if( !seen )
			languages.push_back( candidates[i] );
	}

	const LanguageID originalLanguage = m_language;
	std::vector< std::vector<GXCsfTable> > tablesByLanguage;
	Int modTables = 0, gameTables = 0;
	for( size_t l = 0; l < languages.size(); ++l )
	{
		AsciiString path;
		path.format( g_csfFile, languages[l].str() );
		tablesByLanguage.push_back( std::vector<GXCsfTable>() );
		const Bool loose = TheLocalFileSystem != nullptr && TheLocalFileSystem->doesFileExist( path.str() );
		const Int archives = 16;
		for( Int k = -1; k < archives; ++k )
		{
			FileInstance instance;
			Bool isMod;
			if( k < 0 )
			{
				if( !loose )
					continue;
				instance = 0;
				isMod = TRUE;
			}
			else
			{
				ArchiveFile *archive = TheArchiveFileSystem ? TheArchiveFileSystem->getArchiveFile( path, (FileInstance)k ) : nullptr;
				if( archive == nullptr )
					break;
				instance = (FileInstance)( k + ( loose ? 1 : 0 ) );
				isMod = gxIsModArchiveName( archive->getName() );
			}
			const AsciiString source = k < 0 ? AsciiString("a loose file") : TheArchiveFileSystem->getArchiveFile( path, (FileInstance)k )->getName();
			Int count = 0;
			if( !getCSFInfo( path.str(), count, m_language, instance ) || count <= 0 )
				continue;
			GXCsfTable table;
			table.isMod = isMod;
			table.strings.resize( count );
			Int maxLabelLen = m_maxLabelLen;
			if( !parseCSF( path.str(), &table.strings[0], count, maxLabelLen, instance ) )
				continue;
			m_maxLabelLen = max( m_maxLabelLen, maxLabelLen );
			( isMod ? modTables : gameTables )++;
			tablesByLanguage.back().push_back( table );
			fprintf( stderr, "[GX-TEXT] %s table %s: %d labels from %s\n", isMod ? "mod" : "game",
				path.str(), count, source.str() );
		}
	}

	// GeneralsX @bugfix Android port 18/10/2026 Strings a mod keeps somewhere else.
	//
	// The retail engine looked first for one string file without a language in its path,
	// "data\Generals.str", and a mod could ship its text there; this port looks for
	// data/<language>/generals.str (the language packs) instead, so such a mod's labels were never
	// read and showed MISSING (Shockwave: 'INI:FactionChinaSpecialWeaponsGeneral' in the skirmish
	// lobby, with 0 mod tables in the log above). Every generals.csf and generals.str in a mod's
	// archives that the loop above did not read joins the English tables as a mod table, and every
	// string file a mod carries is named in the log, so the next MISSING says where to look.
	size_t englishIndex = 0;
	for( size_t l = 0; l < languages.size(); ++l )
		if( languages[l].compareNoCase( "english" ) == 0 )
			englishIndex = l;
	if( TheArchiveFileSystem != nullptr )
	{
		const char *patterns[2] = { "*.csf", "*.str" };
		for( Int p = 0; p < 2; ++p )
		{
			FilenameList files;
			TheArchiveFileSystem->getFileListInDirectory( AsciiString(""), AsciiString(""), AsciiString(patterns[p]), files, TRUE );
			for( FilenameList::const_iterator it = files.begin(); it != files.end(); ++it )
			{
				const AsciiString &name = *it;
				if( name.startsWithNoCase( "maps\\" ) || name.startsWithNoCase( "maps/" ) )
					continue;
				const char *base = name.str();
				for( const char *c = name.str(); *c; ++c )
					if( *c == '/' || *c == '\\' )
						base = c + 1;
				const Bool isGeneralsTable = stricmp( base, p == 0 ? "generals.csf" : "generals.str" ) == 0;
				Bool readAbove = FALSE;
				if( p == 0 )
				{
					for( size_t l = 0; l < languages.size() && !readAbove; ++l )
					{
						AsciiString path;
						path.format( g_csfFile, languages[l].str() );
						const char *a = path.str(), *b = name.str();
						while( *a && *b && ( tolower( (unsigned char)*a ) == tolower( (unsigned char)*b )
							|| ( ( *a == '/' || *a == '\\' ) && ( *b == '/' || *b == '\\' ) ) ) )
						{
							++a;
							++b;
						}
						readAbove = *a == '\0' && *b == '\0';
					}
				}
				const Bool loose = TheLocalFileSystem != nullptr && TheLocalFileSystem->doesFileExist( name.str() );
				for( Int k = 0; k < 16; ++k )
				{
					ArchiveFile *archive = TheArchiveFileSystem->getArchiveFile( name, (FileInstance)k );
					if( archive == nullptr )
						break;
					if( !gxIsModArchiveName( archive->getName() ) )
						continue;
					const Bool use = isGeneralsTable && !readAbove && ( p == 0 || ( k == 0 && !loose ) );
					fprintf( stderr, "[GX-TEXT] mod string file %s in %s%s\n", name.str(), archive->getName().str(),
						use ? "" : readAbove ? " (read above)" : isGeneralsTable ? " (shadowed, not read)" : " (not a name the game reads)" );
					if( !use )
						continue;
					GXCsfTable table;
					table.isMod = TRUE;
					Int count = 0;
					Bool ok = FALSE;
					if( p == 0 )
					{
						const FileInstance instance = (FileInstance)( k + ( loose ? 1 : 0 ) );
						Int maxLabelLen = m_maxLabelLen;
						if( getCSFInfo( name.str(), count, m_language, instance ) && count > 0 )
						{
							table.strings.resize( count );
							ok = parseCSF( name.str(), &table.strings[0], count, maxLabelLen, instance );
							m_maxLabelLen = max( m_maxLabelLen, maxLabelLen );
						}
					}
					else if( getStringCount( name.str(), count ) && count > 0 )
					{
						// parseStringFile fills m_stringInfo; lend it this table for the call.
						table.strings.resize( count );
						StringInfo *primary = m_stringInfo;
						m_stringInfo = &table.strings[0];
						ok = parseStringFile( name.str() );
						m_stringInfo = primary;
					}
					if( !ok )
						continue;
					++modTables;
					fprintf( stderr, "[GX-TEXT] mod table %s: %d labels from %s\n", name.str(), count, archive->getName().str() );
					tablesByLanguage[englishIndex].insert( tablesByLanguage[englishIndex].begin(), table );
				}
			}
		}
	}
	m_language = originalLanguage;

	// Override: what a mod added or changed, pack language first.
	std::map<std::string, const StringInfo*> overrides;
	if( format == STRING_FILE )
	{
		for( size_t l = 0; l < tablesByLanguage.size(); ++l )
		{
			const GXCsfTable *game = nullptr;
			for( size_t t = 0; t < tablesByLanguage[l].size() && game == nullptr; ++t )
				if( !tablesByLanguage[l][t].isMod )
					game = &tablesByLanguage[l][t];
			std::map<std::string, const UnicodeString*> gameText;
			if( game != nullptr )
				for( size_t i = 0; i < game->strings.size(); ++i )
					gameText[ std::string( game->strings[i].label.str() ) ] = &game->strings[i].text;
			for( size_t t = 0; t < tablesByLanguage[l].size(); ++t )
			{
				const GXCsfTable &table = tablesByLanguage[l][t];
				if( !table.isMod )
					continue;
				for( size_t i = 0; i < table.strings.size(); ++i )
				{
					const std::string label( table.strings[i].label.str() );
					if( label.empty() || overrides.count( label ) )
						continue;
					std::map<std::string, const UnicodeString*>::const_iterator g = gameText.find( label );
					if( g == gameText.end() || g->second->compare( table.strings[i].text ) != 0 )
						overrides[ label ] = &table.strings[i];
				}
			}
		}
	}

	// Fallback: everything, first copy wins.
	std::map<std::string, const StringInfo*> fallback;
	for( size_t l = 0; l < tablesByLanguage.size(); ++l )
		for( size_t t = 0; t < tablesByLanguage[l].size(); ++t )
			for( size_t i = 0; i < tablesByLanguage[l][t].strings.size(); ++i )
			{
				const StringInfo &info = tablesByLanguage[l][t].strings[i];
				if( !info.label.isEmpty() )
					fallback.insert( std::make_pair( std::string( info.label.str() ), &info ) );
			}

	if( !overrides.empty() )
	{
		m_overrideTextCount = (Int)overrides.size();
		m_overrideStringInfo = NEW StringInfo[m_overrideTextCount];
		Int i = 0;
		for( std::map<std::string, const StringInfo*>::const_iterator it = overrides.begin(); it != overrides.end(); ++it )
			m_overrideStringInfo[i++] = *it->second;
		m_overrideStringLUT = makeLUT( m_overrideStringInfo, m_overrideTextCount );
	}
	if( !fallback.empty() )
	{
		m_fallbackTextCount = (Int)fallback.size();
		m_fallbackStringInfo = NEW StringInfo[m_fallbackTextCount];
		Int i = 0;
		for( std::map<std::string, const StringInfo*>::const_iterator it = fallback.begin(); it != fallback.end(); ++it )
			m_fallbackStringInfo[i++] = *it->second;
		m_fallbackStringLUT = makeLUT( m_fallbackStringInfo, m_fallbackTextCount );
	}

	fprintf( stderr, "[GX-TEXT] primary=%s labels=%d; string tables: %d from mods, %d from the game; "
		"mod labels over the pack: %d; fallback labels: %d\n",
		format == STRING_FILE ? "language pack" : "game table", m_textCount, modTables, gameTables,
		m_overrideTextCount, m_fallbackTextCount );
	fflush( stderr );
}

//============================================================================
// GameTextManager::deinit
//============================================================================

void GameTextManager::deinit()
{

	delete [] m_stringInfo;
	m_stringInfo = nullptr;

	delete [] m_stringLUT;
	m_stringLUT = nullptr;

	delete [] m_fallbackStringInfo;
	m_fallbackStringInfo = nullptr;

	delete [] m_fallbackStringLUT;
	m_fallbackStringLUT = nullptr;

	delete [] m_overrideStringInfo;
	m_overrideStringInfo = nullptr;

	delete [] m_overrideStringLUT;
	m_overrideStringLUT = nullptr;
	m_overrideTextCount = 0;

	m_textCount = 0;
	m_fallbackTextCount = 0;

	NoString *noString = m_noStringList;

	DEBUG_LOG_RAW(("\n"));
	DEBUG_LOG(("*** Missing strings ***"));
	while ( noString )
	{
		DEBUG_LOG(("*** %ls ***", noString->text.str()));
		NoString *next = noString->next;
		delete noString;
		noString = next;
	}
	DEBUG_LOG(("*** End missing strings ***"));
	DEBUG_LOG_RAW(("\n"));

	m_noStringList = nullptr;

	m_initialized = FALSE;
}

//============================================================================
// GameTextManager::reset
//============================================================================

void GameTextManager::reset()
{
	delete [] m_mapStringInfo;
	m_mapStringInfo = nullptr;

	delete [] m_mapStringLUT;
	m_mapStringLUT = nullptr;
}


//============================================================================
// GameTextManager::stripSpaces
//============================================================================

// GeneralsX @bugfix Android port 09/09/2026 Whitespace means ASCII whitespace, and nothing
// above it.
//
// The four callers of this used iswspace() directly on a Char, which is signed: byte 0xA0
// arrives as -96, and asking a wide-character classifier about a negative value is undefined
// to begin with. On this platform it answers "yes" for 0xA0 and 0x85 -- and those are
// CONTINUATION bytes of perfectly ordinary UTF-8 letters. "Р" is D0 A0 and "х" is D1 85, so
// readToEndOfQuote turned their second byte into a space, the decoder then found a lead byte
// with no continuation after it and fell back to Latin-1, and the Russian menu read
// "ЗАГД УЗИТЬ" and "АВТОÐЫ" instead of "ЗАГРУЗИТЬ" and "АВТОРЫ".
//
// A byte at or above 0x80 is part of a multi-byte sequence and is never whitespace. Below
// that this behaves exactly as before, so ASCII text is unaffected.
static inline Bool isAsciiSpace( Char ch )
{
	const unsigned char b = (unsigned char)ch;
	return (b < 0x80) && (isspace( b ) != 0);
}

void GameTextManager::stripSpaces ( WideChar *string )
{
	WideChar *str, *ptr;
	WideChar ch, last = 0;
	Int skipall = TRUE;

	str = ptr = string;

	while ( (ch = *ptr++) != 0 )
	{
		if ( ch == ' '  )
		{
			if ( last == ' ' || skipall )
			{
				continue;
			}
		}

		if ( ch == '\n' || ch == '\t' )
		{
				// remove last space
				if ( last == ' ' )
				{
					str--;
				}

				skipall = TRUE;		// skip all spaces
				last = *str++ = ch;
				continue;
		}

		last = *str++ = ch;
		skipall = FALSE;
	}

	if ( last == ' ' )
	{
		str--;
	}

	*str = 0;
}

//============================================================================
// GameTextManager::removeLeadingAndTrailing
//============================================================================

void GameTextManager::removeLeadingAndTrailing ( Char *buffer )
{
	Char *first, *ptr;
	Char ch;

	ptr = first = buffer;

	while ( (ch = *first) != 0 && isAsciiSpace( ch ))
	{
			first++;
	}

	while ( (*ptr++ = *first++) != 0 );

	ptr -= 2;

	while ( (ptr > buffer) && (ch = *ptr) != 0 && isAsciiSpace( ch ) )
	{
		ptr--;
	}

	ptr++;
	*ptr = 0;
}

//============================================================================
// GameTextManager::readToEndOfQuote
//============================================================================

void GameTextManager::readToEndOfQuote( File *file, Char *in, Char *out, Char *wavefile, Int maxBufLen )
{
	Int slash = FALSE;
	Int state = 0;
	Int line_start = FALSE;
	Char ch;
	Int ccount = 0;
	Int len = 0;
	Int done = FALSE;

	while ( maxBufLen )
	{
		// get next Char

		if ( in )
		{
			if ( (ch = *in++) == 0 )
			{
				in = nullptr; // have exhausted the input m_buffer
				ch = readChar ( file );
			}
		}
		else
		{
			ch = readChar ( file );
		}

		if ( ch == EOF )
		{
			return ;
		}

		if ( ch == '\n' )
		{
			line_start = TRUE;
			slash = FALSE;
			ccount = 0;
			ch = ' ';
		}
		else if ( ch == '\\' && !slash)
		{
			slash = TRUE;
		}
		else if ( ch == '\\' && slash)
		{
			slash = FALSE;
		}
		else if ( ch == '"' && !slash )
		{
			break; // done
		}
		else
		{
			slash = FALSE;
		}

		if ( isAsciiSpace( ch ))
		{
			ch = ' ';
		}

		*out++ = ch;
		maxBufLen--;
	}

	*out = 0;

	while ( !done )
	{
		// get next Char

		if ( in )
		{
			if ( (ch = *in++) == 0 )
			{
				in = nullptr; // have exhausted the input m_buffer
				ch = readChar ( file );
			}
		}
		else
		{
			ch = readChar ( file );
		}

		if ( ch == '\n' || ch == EOF )
		{
			break;
		}

		switch ( state )
		{

			case 0:
				if ( isAsciiSpace( ch ) || ch == '=' )
				{
					break;
				}

				state = 1;
				FALLTHROUGH;
			case 1:
				if ( ( ch >= 'a' && ch <= 'z') || ( ch >= 'A' && ch <='Z') || (ch >= '0' && ch <= '9') || ch == '_' )
				{
					*wavefile++ = ch;
					len++;
					break;
				}
				state = 2;
				FALLTHROUGH;
			case 2:
				break;
		}
	}

	*wavefile = 0;

	if ( len )
	{
		if ( ( ch = *(wavefile-1)) >= '0' && ch <= '9' )
		{
			*wavefile++ = 'e';
			*wavefile = 0;
		}
	}

}


//============================================================================
// GameTextManager::reverseWord
//============================================================================

void GameTextManager::reverseWord ( Char *file, Char *lp )
{
	Int first = TRUE;
	Char f, l;
	Int ok = TRUE	;

	while ( ok )
	{
		if ( file >= lp )
		{
			return;
		}

		f = *file;
		l = *lp;

		if ( first )
		{
			if ( f >= 'A' && f <= 'Z' )
			{
				if ( l >= 'a' && l <= 'z' )
				{
					f = (f - 'A') + 'a';
					l = (l - 'a') + 'A';
				}
			}

			first = FALSE;
		}

		*lp-- = f;
		*file++ = l;

	}

}

//============================================================================
// GameTextManager::translateCopy
//============================================================================

// GeneralsX @feature Android port 09/09/2026 Decode one UTF-8 sequence.
//
// Returns the code point and, through extraBytes, how many bytes BEYOND the first it
// consumed -- the caller advances by one on its own. A byte that does not begin a valid,
// complete, correctly-continued sequence is returned as itself with extraBytes 0, which is
// exactly the old "mask to 0x00FF" behaviour, so a legacy Latin-1 .str file is unchanged.
//
// Everything above the Basic Multilingual Plane is folded to U+FFFD: the engine stores text
// as UTF-16 in a fixed-size WideChar buffer with no surrogate handling anywhere, so a
// surrogate pair would be counted and measured wrong all the way through the text system.
// No game string needs one.
static UnsignedInt decodeUtf8( const unsigned char *in, Int &extraBytes )
{
	extraBytes = 0;

	const unsigned char lead = in[0];
	if( lead < 0x80 )
		return lead;

	Int needed;
	UnsignedInt value;
	if( (lead & 0xE0) == 0xC0 )      { needed = 1; value = lead & 0x1F; }
	else if( (lead & 0xF0) == 0xE0 ) { needed = 2; value = lead & 0x0F; }
	else if( (lead & 0xF8) == 0xF0 ) { needed = 3; value = lead & 0x07; }
	else                             { return lead; }   // continuation byte or invalid lead

	for( Int i = 1; i <= needed; i++ )
	{
		const unsigned char cont = in[i];
		if( (cont & 0xC0) != 0x80 )
			return lead;              // truncated or malformed: treat the lead as Latin-1
		value = (value << 6) | (cont & 0x3F);
	}

	extraBytes = needed;

	if( value > 0xFFFF )
		return 0xFFFD;                // outside the BMP; see above

	return value;
}

void GameTextManager::translateCopy( WideChar *outbuf, Char *inbuf )
{
	Int slash = FALSE;

#if defined(RTS_DEBUG)
	if ( m_jabberWockie )
	{
		static Char buffer[MAX_UITEXT_LENGTH*2];
		Char *firstLetter = nullptr, *lastLetter;
		Char *b = buffer;
		Int formatWord = FALSE;
		Char ch;

		while ( (ch = *inbuf++) != 0 )
		{
			if ( ! (( ch >= 'a' && ch <= 'z') || ( ch >= 'A' && ch <= 'Z' )))
			{
				if ( firstLetter )
				{
					if ( !formatWord )
					{
						lastLetter = b-1;
						reverseWord ( firstLetter, lastLetter );
					}
					firstLetter = nullptr;
					formatWord = FALSE;
				}
				*b++ = ch;
				if ( ch == '\\' )
				{
					*b++ = *inbuf++;
				}
				if ( ch == '%' )
				{
					while ( (ch = *inbuf++) != 0 && !( (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')))
					{
						*b++ = ch;
					}
					*b++ = ch;
				}
			}
			else
			{
				if ( !firstLetter )
				{
					firstLetter = b;
				}

				*b++ = ch;

			}
		}

		if ( firstLetter )
		{
			lastLetter = b-1;
			reverseWord ( firstLetter, lastLetter );
		}

		*b++ = 0;
		inbuf = buffer;
	}
	else if( m_munkee )
	{
		wcscpy(outbuf, L"Munkee");
		return;
	}
#endif

	while( *inbuf != '\0' )
	{
		if( slash == TRUE )
		{
			slash = FALSE;

			switch( *inbuf )
			{
				// in case end of string is reached
				// should never happen!!!
				case '\0':
					return;

				case '\\':
					*outbuf++ = '\\';
					break;

				case '\'':
					*outbuf++ = '\'';
					break;

				case '\"':
					*outbuf++ = '\"';
					break;

				case '\?':
					*outbuf++ = '\?';
					break;

				case 't':
					*outbuf++ = '\t';
					break;

				case 'n':
					*outbuf++ = '\n';
					break;

				default:
					*outbuf++ = *inbuf & 0x00FF;
					break;
			}
		}
		else if( *inbuf != '\\' )
		{
			// GeneralsX @feature Android port 09/09/2026 Decode UTF-8 here, instead of
			// masking each byte to 0x00FF.
			//
			// That mask is what made .str files English-only. A byte became the code point
			// with the same value, so a .str file could carry Latin-1 and nothing else --
			// Cyrillic, Greek, anything outside the first 256 code points came out as
			// mojibake, and every translation had to be shipped as a compiled binary .csf
			// instead. Which is why community translations are .big archives that overwrite
			// Data\English rather than proper language folders: the plain-text format the
			// engine already reads could not hold their alphabet.
			//
			// Decoding UTF-8 makes that format usable by every language, and costs nothing
			// for the files that already exist: a pure-ASCII .str decodes byte for byte to
			// exactly what the mask produced. Only a malformed sequence falls back to the
			// old behaviour, so a genuine Latin-1 file still reads as it always did rather
			// than turning into replacement characters.
			Int extraBytes = 0;
			UnsignedInt codePoint = decodeUtf8( (const unsigned char *)inbuf, extraBytes );
			*outbuf++ = (WideChar)codePoint;
			inbuf += extraBytes;
		}
		else
			slash = TRUE;

		inbuf++;
	}
	*outbuf= 0;
}

//============================================================================
// GameTextManager::getStringCount
//============================================================================

Bool GameTextManager::getStringCount( const char *filename, Int& textCount )
{
	Int ok = TRUE;

	textCount = 0;

	File *file;
	file = TheFileSystem->openFile(filename, File::READ | File::TEXT);
	DEBUG_LOG(("Looking in %s for string file", filename));

	if ( file == nullptr )
	{
		return FALSE;
	}

	while(ok)
	{
		if( !readLine( m_buffer, sizeof( m_buffer) -1, file ) )
			break;
		removeLeadingAndTrailing ( m_buffer );

		if( m_buffer[0] == '"' )
		{
				Int len = strlen(m_buffer);
				m_buffer[ len ] = '\n';
				m_buffer[ len+1] = 0;
			readToEndOfQuote( file, &m_buffer[1], m_buffer2, m_buffer3, MAX_UITEXT_LENGTH );
		}
		else if( stricmp( m_buffer, "END") == 0 )
		{
			textCount++;
		}
	}

	textCount += 500;
	file->close();
	file = nullptr;
	return TRUE;
}

//============================================================================
// GameTextManager::getCSFInfo
//============================================================================

Bool GameTextManager::getCSFInfo ( const Char *filename, Int& textCount, LanguageID& language, FileInstance instance )
{
	CSFHeader header;
	Int ok = FALSE;
	File *file = TheFileSystem->openFile(filename, File::READ | File::BINARY, File::BUFFERSIZE, instance);
	DEBUG_LOG(("Looking in %s for compiled string file", filename));

	if ( file != nullptr )
	{
		if ( file->read( &header, sizeof ( header )) == sizeof ( header ) )
		{
			if ( header.id == CSF_ID )
			{
				textCount = header.num_labels;

				if ( header.version >= 2 )
				{
					language = (LanguageID) header.langid;
				}
				else
				{
					language = LANGUAGE_ID_US;
				}

				ok = TRUE;
			}
		}

		file->close();
		file = nullptr;
	}

	return ok;
}

//============================================================================
// GameTextManager::parseCSF
//============================================================================

Bool GameTextManager::parseCSF( const Char *filename, StringInfo *stringInfo, Int textCount, Int& maxLabelLen, FileInstance instance )
{
	File *file;
	Int id;
	Int len;
	Int listCount = 0;
	Bool ok = FALSE;
	CSFHeader header;

	// GeneralsX @bugfix BenderAI 16/02/2026 - Debug parseCSF
	fprintf(stderr, "[CSF] parseCSF() - START filename='%s'\n", filename);

	file = TheFileSystem->openFile(filename, File::READ | File::BINARY, File::BUFFERSIZE, instance);

	if ( file == nullptr )
	{
		fprintf(stderr, "[CSF] parseCSF() - openFile FAILED\n");
		return FALSE;
	}

	fprintf(stderr, "[CSF] parseCSF() - File opened\n");

	if (  file->read ( &header, sizeof ( CSFHeader)) != sizeof ( CSFHeader) )
	{
		fprintf(stderr, "[CSF] parseCSF() - read header FAILED\n");
		return FALSE;
	}

	// GeneralsX @bugfix BenderAI 17/02/2026 - Log header for debugging
	fprintf(stderr, "[CSF] parseCSF() - Header: id=%#x version=%d num_labels=%d num_strings=%d skip=%d langid=%d\n",
		header.id, header.version, header.num_labels, header.num_strings, header.skip, header.langid);

	// GeneralsX @bugfix BenderAI 17/02/2026 - Skip extra bytes after header
	// The 'skip' field indicates how many bytes to advance before reading first label
	if (header.skip > 0) {
		fprintf(stderr, "[CSF] parseCSF() - Skipping %d bytes as indicated by header.skip\n", header.skip);
		file->seek(header.skip, File::CURRENT);
	}

	fprintf(stderr, "[CSF] parseCSF() - Starting main loop (textCount=%d)...\n", textCount);

	while( file->read ( &id, sizeof (id)) == sizeof ( id) )
	{
		Int num;
		Int num_strings;

		if ( id != CSF_LABEL )
		{
			fprintf(stderr, "[CSF] parseCSF() - ERROR: Expected CSF_LABEL (%#x), got %#x at label %d\n", CSF_LABEL, id, listCount);
			goto quit;
		}

		file->read ( &num_strings, sizeof ( Int ));

		file->read ( &len, sizeof ( Int ) );

		if ( len )
		{
			file->read ( m_buffer, len );
		}

		m_buffer[len] = 0;

		stringInfo[listCount].label = m_buffer;


		if ( len > maxLabelLen )
		{
			maxLabelLen = len;
		}

		num = 0;

		while ( num < num_strings )
		{
		 	file->read ( &id, sizeof ( Int ) );

			if ( id != CSF_STRING && id != CSF_STRINGWITHWAVE )
			{
				fprintf(stderr, "[CSF] parseCSF() - ERROR: Expected CSF_STRING (%#x) or CSF_STRINGWITHWAVE (%#x), got %#x at label %d, string %d\n", 
					CSF_STRING, CSF_STRINGWITHWAVE, id, listCount, num);
				goto quit;
			}

		 	file->read ( &len, sizeof ( Int ) );

			if ( len )
			{
				// GeneralsX @bugfix BenderAI 17/02/2026 Linux wchar_t compatibility
				// CSF files store 16-bit wide chars (UCS-2), but Linux wchar_t is 32-bit (UCS-4)
#ifdef _WIN32
				file->read ( m_tbuffer, len*sizeof(WideChar) );
#else
				uint16_t convert_buffer[MAX_UITEXT_LENGTH*2];
				file->read ( convert_buffer, len*sizeof(uint16_t) );
				for (int i = 0; i < len; i++)
				{
					m_tbuffer[i] = convert_buffer[i];
				}
#endif
			}

			if ( num == 0 )
			{
				// only use the first string found
				m_tbuffer[len] = 0;

				{
					WideChar *ptr;

					ptr = m_tbuffer;

					while ( *ptr )
					{
						// GeneralsX @bugfix BenderAI 17/02/2026 Bitwise NOT for 16-bit values only
#ifdef _WIN32
						*ptr = ~*ptr;
#else
						// only negate the lower 16 bits (32-bit widechar on Linux)
						*ptr = ~*ptr & 0x0000FFFF;
#endif
						ptr++;
					}
				}

				stripSpaces ( m_tbuffer );
				stringInfo[listCount].text = m_tbuffer;
			}

			if ( id == CSF_STRINGWITHWAVE )
			{
			 	file->read ( &len, sizeof ( Int ) );
				if ( len )
				{
					file->read ( m_buffer, len );
				}
				m_buffer[len] = 0;

				if ( num == 0 && len )
				{
					// only use the first string found
					stringInfo[listCount].speech = m_buffer;
				}

			}

			num++;
		}

		listCount++;
		
		// GeneralsX @bugfix BenderAI 17/02/2026 Progress logging every 500 labels
		if (listCount % 500 == 0) {
			fprintf(stderr, "[CSF] parseCSF() - Progress: %d/%d labels processed\n", listCount, textCount);
		}
	}

	fprintf(stderr, "[CSF] parseCSF() - Main loop complete! Processed %d/%d labels\n", listCount, textCount);
	ok = TRUE;

quit:

	fprintf(stderr, "[CSF] parseCSF() - Reached quit label: ok=%s, listCount=%d/%d\n", 
		ok ? "TRUE" : "FALSE", listCount, textCount);

	file->close();
	file = nullptr;

	return ok;
}


//============================================================================
// GameTextManager::parseStringFile
//============================================================================

Bool GameTextManager::parseStringFile( const char *filename )
{
	Int listCount = 0;
	Int ok = TRUE;

	File *file = TheFileSystem->openFile(filename, File::READ | File::TEXT);

	if ( file == nullptr )
	{
		return FALSE;
	}

	while( ok )
	{
		Int len;
		if( !readLine( m_buffer, MAX_UITEXT_LENGTH, file ))
		{
			break;
		}

		removeLeadingAndTrailing ( m_buffer );

		if( ( *(unsigned short *)m_buffer == 0x2F2F) || !m_buffer[0])			//	0x2F2F is Hex for //
			continue;

		// make sure label is unique

		for ( Int i = 0; i < listCount; i++ )
		{
			if ( stricmp ( m_stringInfo[i].label.str(), m_buffer ) == 0)
			{
				DEBUG_CRASH ( ("String label '%s' multiply defined!", m_buffer ));
			}
		}

		m_stringInfo[listCount].label = m_buffer;
		len = strlen ( m_buffer );


		if ( len > m_maxLabelLen )
		{
			m_maxLabelLen = len;
		}

		Bool readString = FALSE;
		while( ok )
		{
			if (!readLine ( m_buffer, sizeof(m_buffer)-1, file ))
			{
				DEBUG_CRASH (("Unexpected end of string file"));
				ok = FALSE;
				goto quit;
			}

			removeLeadingAndTrailing ( m_buffer );

			if( m_buffer[0] == '"' )
			{
				len = strlen(m_buffer);
				m_buffer[ len ] = '\n';
				m_buffer[ len+1] = 0;
				readToEndOfQuote( file, &m_buffer[1], m_buffer2, m_buffer3, MAX_UITEXT_LENGTH );


				if ( readString )
				{
					// only one string per label allows
						DEBUG_CRASH ( ("String label '%s' has more than one string defined!", m_stringInfo[listCount].label.str()));
				}
				else
				{
					// Copy string into new home
					translateCopy( m_tbuffer, m_buffer2 );
					stripSpaces ( m_tbuffer );

					m_stringInfo[listCount].text = m_tbuffer ;
					m_stringInfo[listCount].speech = m_buffer3;
					readString = TRUE;
				}
			}
			else if ( stricmp ( m_buffer, "END" ) == 0)
			{
				break;
			}
		}

		listCount++;
	}

quit:

	file->close();
	file = nullptr;

	return ok;
}

//============================================================================
// GameTextManager::initMapStringFile
//============================================================================

void GameTextManager::initMapStringFile( const AsciiString& filename )
{
	m_mapTextCount = 0;
	getStringCount( filename.str(), m_mapTextCount );

	m_mapStringInfo = NEW StringInfo[m_mapTextCount];

	parseMapStringFile( filename.str() );

	m_mapStringLUT = NEW StringLookUp[m_mapTextCount];

	StringLookUp *lut = m_mapStringLUT;
	StringInfo *info = m_mapStringInfo;

	for ( Int i = 0; i < m_mapTextCount; i++ )
	{
		lut->info = info;
		lut->label = &info->label;
		lut++;
		info++;
	}

	qsort( m_mapStringLUT, m_mapTextCount, sizeof(StringLookUp), compareLUT  );
}

//============================================================================
// GameTextManager::parseMapStringFile
//============================================================================

Bool GameTextManager::parseMapStringFile( const char *filename )
{
	Int listCount = 0;
	Int ok = TRUE;

	File *file;

	file = TheFileSystem->openFile(filename, File::READ | File::TEXT);
	if ( file == nullptr )
	{
		return FALSE;
	}

	while( ok )
	{
		Int len;
		if( !readLine( m_buffer, MAX_UITEXT_LENGTH, file ))
		{
			break;
		}

		removeLeadingAndTrailing ( m_buffer );

		if( ( *(unsigned short *)m_buffer == 0x2F2F) || !m_buffer[0])			//	0x2F2F is Hex for //
			continue;

		// make sure label is unique

		for ( Int i = 0; i < listCount; i++ )
		{
			if ( stricmp ( m_mapStringInfo[i].label.str(), m_buffer ) == 0)
			{
				DEBUG_CRASH ( ("String label '%s' multiply defined!", m_buffer ));
			}
		}

		m_mapStringInfo[listCount].label = m_buffer;
		len = strlen ( m_buffer );


		if ( len > m_maxLabelLen )
		{
			m_maxLabelLen = len;
		}

		Bool readString = FALSE;
		while( ok )
		{
			if (!readLine ( m_buffer, sizeof(m_buffer)-1, file ))
			{
				DEBUG_CRASH (("Unexpected end of string file"));
				ok = FALSE;
				goto quit;
			}

			removeLeadingAndTrailing ( m_buffer );

			if( m_buffer[0] == '"' )
			{
				len = strlen(m_buffer);
				m_buffer[ len ] = '\n';
				m_buffer[ len+1] = 0;
				readToEndOfQuote( file, &m_buffer[1], m_buffer2, m_buffer3, MAX_UITEXT_LENGTH );


				if ( readString )
				{
					// only one string per label allowed
						DEBUG_CRASH ( ("String label '%s' has more than one string defined!", m_stringInfo[listCount].label.str()));
				}
				else
				{
					// Copy string into new home
					translateCopy( m_tbuffer, m_buffer2 );
					stripSpaces ( m_tbuffer );

					UnicodeString text = UnicodeString(m_tbuffer);
					if (TheLanguageFilter)
						TheLanguageFilter->filterLine(text);

					m_mapStringInfo[listCount].text = text;
					m_mapStringInfo[listCount].speech = m_buffer3;
					readString = TRUE;
				}
			}
			else if ( stricmp ( m_buffer, "END" ) == 0)
			{
				break;
			}
		}

		listCount++;
	}

quit:

	file->close();
	file = nullptr;

	return ok;
}

//============================================================================
// *GameTextManager::fetch
//============================================================================

UnicodeString GameTextManager::fetch( const Char *label, Bool *exists )
{
	DEBUG_ASSERTCRASH ( m_initialized, ("String Manager has not been m_initialized") );

	if( m_stringInfo == nullptr )
	{
		if( exists )
			*exists = FALSE;
		return m_failed;
	}

	StringLookUp *lookUp;
	StringLookUp key;
	AsciiString lb;
	lb = label;
	key.info = nullptr;
	key.label = &lb;

	// GeneralsX @feature Android port 18/10/2026 A mod's own text first (buildModTextLayers).
	lookUp = nullptr;
	if ( m_overrideStringLUT && m_overrideTextCount )
	{
		lookUp = (StringLookUp *) bsearch( &key, (void*) m_overrideStringLUT, m_overrideTextCount, sizeof(StringLookUp), compareLUT );
	}

	if ( lookUp == nullptr )
	{
		lookUp = (StringLookUp *) bsearch( &key, (void*) m_stringLUT, m_textCount, sizeof(StringLookUp), compareLUT );
	}

	if ( lookUp == nullptr && m_mapStringLUT && m_mapTextCount )
	{
		lookUp = (StringLookUp *) bsearch( &key, (void*) m_mapStringLUT, m_mapTextCount, sizeof(StringLookUp), compareLUT );
	}

	// GeneralsX @bugfix BenderAI 22/05/2026 Fallback to lower-priority CSF when override tables are incomplete.
	if ( lookUp == nullptr && m_fallbackStringLUT && m_fallbackTextCount )
	{
		lookUp = (StringLookUp *) bsearch( &key, (void*) m_fallbackStringLUT, m_fallbackTextCount, sizeof(StringLookUp), compareLUT );
	}

	if( lookUp == nullptr )
	{
		for( size_t i = 0; i < ARRAY_SIZE(s_gxLabelAliases); ++i )
		{
			if( strcmp( s_gxLabelAliases[i].label, label ) == 0 )
				return fetch( s_gxLabelAliases[i].existingLabel, exists );
		}

		for( size_t i = 0; i < ARRAY_SIZE(s_gxEnglishDefaults); ++i )
		{
			if( strcmp( s_gxEnglishDefaults[i].label, label ) == 0 )
			{
				if( exists )
					*exists = TRUE;
				return UnicodeString( s_gxEnglishDefaults[i].text );
			}
		}
	}

	if( lookUp == nullptr )
	{

		// string not found
		if( exists )
			*exists = FALSE;

		// See if we already have the missing string
		UnicodeString missingString;
		missingString.format(L"MISSING: '%hs'", label);

		NoString *noString = m_noStringList;

		while ( noString )
		{
			if (noString->text == missingString)
				return missingString;

			noString = noString->next;
		}

		//DEBUG_LOG(("*** MISSING:'%s' ***", label));
		// Remember file could have been altered at this point.
		noString = NEW NoString;
		noString->text = missingString;
		noString->next = m_noStringList;
		m_noStringList = noString;
		return noString->text;
	}
	if( exists )
		*exists = TRUE;
	return lookUp->info->text;
}

//============================================================================
// *GameTextManager::fetch
//============================================================================

UnicodeString GameTextManager::fetch( AsciiString label, Bool *exists )
{
	return fetch(label.str(), exists);
}

//============================================================================
// *GameTextManager::fetchFormat
//============================================================================

UnicodeString GameTextManager::fetchFormat( const Char *label, ... )
{
	Bool exists;
	UnicodeString str = fetch(label, &exists);
	if (exists)
	{
		UnicodeString strFormat;

		va_list args;
		va_start(args, label);
		strFormat.format_va(str.str(), args);
		va_end(args);

		str = strFormat;
	}
	return str;
}

//============================================================================
// GameTextManager::fetchOrSubstitute
//============================================================================

UnicodeString GameTextManager::fetchOrSubstitute( const Char *label, const WideChar *substituteText )
{
	Bool exists;
	UnicodeString str = fetch(label, &exists);
	if (!exists)
		str = substituteText;
	return str;
}

//============================================================================
// GameTextManager::fetchOrSubstituteFormat
//============================================================================

UnicodeString GameTextManager::fetchOrSubstituteFormat( const Char *label, const WideChar *substituteFormat, ... )
{
	va_list args;
	va_start(args, substituteFormat);
	UnicodeString str = fetchOrSubstituteFormatVA(label, substituteFormat, args);
	va_end(args);

	return str;
}

//============================================================================
// GameTextManager::fetchOrSubstituteFormatVA
//============================================================================

UnicodeString GameTextManager::fetchOrSubstituteFormatVA( const Char *label, const WideChar *substituteFormat, va_list args )
{
	Bool exists;
	UnicodeString str = fetch(label, &exists);
	if (exists)
	{
		UnicodeString strFormat;
		strFormat.format_va(strFormat.str(), args);
		str = strFormat;
	}
	else
	{
		str.format_va(substituteFormat, args);
	}

	return str;
}

//============================================================================
// GameTextManager::getStringsWithLabelPrefix
//============================================================================

AsciiStringVec& GameTextManager::getStringsWithLabelPrefix(AsciiString label)
{
	m_asciiStringVec.clear();
	if (m_stringLUT) {
		for (int i = 0; i < m_textCount; ++i) {
			if (strstr(m_stringLUT[i].label->str(), label.str()) == m_stringLUT[i].label->str()) {
				m_asciiStringVec.push_back(*m_stringLUT[i].label);
			}
		}
	}
	if (m_mapStringLUT) {
		for (int i = 0; i < m_mapTextCount; ++i) {
			if (strstr(m_mapStringLUT[i].label->str(), label.str()) == m_mapStringLUT[i].label->str()) {
				m_asciiStringVec.push_back(*m_mapStringLUT[i].label);
			}
		}
	}
	return m_asciiStringVec;
}

//============================================================================
// GameTextManager::readLine
//============================================================================

Bool	GameTextManager::readLine( char *buffer, Int max, File *file )
{
	Int ok = FALSE;

	while ( max && file->read( buffer, 1 ) == 1 )
	{
		ok = TRUE;

		if ( *buffer == '\n' )
		{
			break;
		}

		buffer++;
		max--;
	}

	*buffer = 0;

	return ok;
}

//============================================================================
// GameTextManager::readChar
//============================================================================

Char	GameTextManager::readChar( File *file )
{
	Char ch;

	if ( file->read( &ch, 1 ) == 1 )
	{
		return ch;
	}

	return 0;
}

//============================================================================
// compareLUT
//============================================================================

static int __cdecl compareLUT ( const void *i1,  const void*i2)
{
	StringLookUp *lut1 = (StringLookUp*) i1;
	StringLookUp *lut2 = (StringLookUp*) i2;

	return stricmp( lut1->label->str(), lut2->label->str());
}
