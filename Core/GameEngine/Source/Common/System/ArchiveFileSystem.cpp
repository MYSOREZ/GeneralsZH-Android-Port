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
//                Copyright (C) 2001 - All Rights Reserved
//
//----------------------------------------------------------------------------
//
// Project:   Generals
//
// Module:    Game Engine Common
//
// File name: ArchiveFileSystem.cpp
//
// Created:   11/26/01 TR
//
//----------------------------------------------------------------------------

//----------------------------------------------------------------------------
//         Includes
//----------------------------------------------------------------------------

#include "PreRTS.h"
#include "Common/ArchiveFile.h"
#include "Common/ArchiveFileSystem.h"
#include "Common/GlobalData.h"
#include "Common/LocalFileSystem.h"
#include "Common/AsciiString.h"
#include "Common/PerfTimer.h"


//----------------------------------------------------------------------------
//         Externals
//----------------------------------------------------------------------------



//----------------------------------------------------------------------------
//         Defines
//----------------------------------------------------------------------------



//----------------------------------------------------------------------------
//         Private Types
//----------------------------------------------------------------------------


//----------------------------------------------------------------------------
//         Private Data
//----------------------------------------------------------------------------



//----------------------------------------------------------------------------
//         Public Data
//----------------------------------------------------------------------------

ArchiveFileSystem *TheArchiveFileSystem = nullptr;


//----------------------------------------------------------------------------
//         Private Prototypes
//----------------------------------------------------------------------------



//----------------------------------------------------------------------------
//         Private Functions
//----------------------------------------------------------------------------

#if RTS_ZEROHOUR
// Presence turns the community data patch off without removing it -- the
// launcher writes and deletes this, and loadMods() below is the only reader.
static const char* const kCommunityPatchDisableMarker = "gx_no_community_patch.txt";
#endif

static AsciiString getBaseFilename(const AsciiString& path)
{
	const char* str = path.str();
	const char* p1 = strrchr(str, '\\');
	const char* p2 = strrchr(str, '/');
	const char* sep = (p1 == nullptr) ? p2 : ((p2 == nullptr) ? p1 : ((p1 > p2) ? p1 : p2));
	return sep ? AsciiString(sep + 1) : path;
}



//----------------------------------------------------------------------------
//         Public Functions
//----------------------------------------------------------------------------

//------------------------------------------------------
// ArchivedFileInfo
//------------------------------------------------------
ArchiveFileSystem::ArchiveFileSystem()
{
}

ArchiveFileSystem::~ArchiveFileSystem()
{
	ArchiveFileMap::iterator iter = m_archiveFileMap.begin();
	while (iter != m_archiveFileMap.end()) {
		ArchiveFile *file = iter->second;
		delete file;
		iter++;
	}
}

void ArchiveFileSystem::loadIntoDirectoryTree(ArchiveFile *archiveFile, Bool overwrite, Bool sortedByName)
{

	FilenameList filenameList;

	archiveFile->getFileListInDirectory("", "", "*", filenameList, TRUE);

	FilenameListIter it = filenameList.begin();

	while (it != filenameList.end())
	{
		ArchivedDirectoryInfo *dirInfo = &m_rootDirectory;

		AsciiString path;
		AsciiString token;
		AsciiString tokenizer = *it;
		tokenizer.toLower();
		Bool infoInPath = tokenizer.nextToken(&token, "\\/");

		while (infoInPath && (!token.find('.') || tokenizer.find('.')))
		{
			path.concat(token);
			path.concat('\\');

			ArchivedDirectoryInfoMap::iterator tempiter = dirInfo->m_directories.find(token);
			if (tempiter == dirInfo->m_directories.end())
			{
				dirInfo = &(dirInfo->m_directories[token]);
				dirInfo->m_path = path;
				dirInfo->m_directoryName = token;
			}
			else
			{
				dirInfo = &tempiter->second;
			}

			infoInPath = tokenizer.nextToken(&token, "\\/");
		}

		ArchivedFileLocationMap::iterator fileIt;
		if (sortedByName)
		{
			// Insert by case-insensitive archive filename, matching game folder load
			// order where the alphabetically first archive wins.
			const AsciiString baseName = getBaseFilename(archiveFile->getName());
			std::pair<ArchivedFileLocationMap::iterator, ArchivedFileLocationMap::iterator> range = dirInfo->m_files.equal_range(token);
			fileIt = range.first;
			while (fileIt != range.second && getBaseFilename(fileIt->second->getName()).compareNoCase(baseName) <= 0)
				++fileIt;
		}
		else if (overwrite)
		{
			// When overwriting, try place the new value at the beginning of the key list.
			fileIt = dirInfo->m_files.find(token);
		}
		else
		{
			// Append to the end of the key list.
			fileIt = dirInfo->m_files.end();
		}

		dirInfo->m_files.insert(fileIt, std::make_pair(token, archiveFile));

#if defined(DEBUG_LOGGING) && ENABLE_FILESYSTEM_LOGGING
		{
			const stl::const_range<ArchivedFileLocationMap> range = stl::get_range(dirInfo->m_files, token, 0);
			if (range.distance() >= 2)
			{
				ArchivedFileLocationMap::const_iterator rangeIt0;
				ArchivedFileLocationMap::const_iterator rangeIt1;

				if (overwrite)
				{
					rangeIt0 = range.begin;
					rangeIt1 = std::next(rangeIt0);

					DEBUG_LOG(("ArchiveFileSystem::loadIntoDirectoryTree - adding file %s, archived in %s, overwriting same file in %s",
						it->str(),
						rangeIt0->second->getName().str(),
						rangeIt1->second->getName().str()
					));
				}
				else
				{
					rangeIt1 = std::prev(range.end);
					rangeIt0 = std::prev(rangeIt1);

					DEBUG_LOG(("ArchiveFileSystem::loadIntoDirectoryTree - adding file %s, archived in %s, overwritten by same file in %s",
						it->str(),
						rangeIt1->second->getName().str(),
						rangeIt0->second->getName().str()
					));
				}
			}
			else
			{
				DEBUG_LOG(("ArchiveFileSystem::loadIntoDirectoryTree - adding file %s, archived in %s", it->str(), archiveFile->getName().str()));
			}
		}
#endif

		it++;
	}
}

void ArchiveFileSystem::loadMods()
{
#if RTS_ZEROHOUR
	// GeneralsX @bugfix Android port 13/09/2026 Mount the GeneralsOnline community
	// data patch, which is why this port could not join a single PC-hosted lobby.
	//
	// The lobby gate compares INI checksums, and the two never matched: this port
	// reports 4272612339, every PC lobby reports 2180732466. It was tempting to
	// read that as "the PC players are modded" -- GeneralsOnline's own source even
	// calls 4272612339 VANILLA_INI_CRC -- but it is the other way round. The PC
	// client loads an extra archive nobody installs by hand: the community patch
	// is a data pack it downloads into the user-data folder and mounts on every
	// launch unless explicitly disabled, so 2180732466 is simply what a normal
	// GeneralsOnline install computes, and matching it is not a workaround.
	//
	// Mounted sorted-by-name rather than as an overwrite, which is what the PC
	// client does and what the "500_900_" prefix is for: the patch takes priority
	// over the retail archives because digits sort ahead of letters, and later
	// data packs slot in by number rather than by mount order.
	//
	// The path is the user-data folder, unchanged from the PC client's -- which
	// on this port is already a plain, visible directory laid out exactly like
	// Windows' Documents leaf (see SDL3Main.cpp and BuildUserDataPathFromRegistry).
	// So this needs no new location and invents no convention: the same relative
	// path under the folder that already holds Options.ini, save games and Maps.
	//
	// Presence is the switch. Without the file this does nothing and the port
	// behaves as before; with it, the INI checksum should land on the PC's value
	// -- and land on it honestly, because the simulation is then reading the same
	// unit data, which is the whole point of the checksum.
	{
		AsciiString userData = TheGlobalData->getPath_UserData();
		if (userData.isNotEmpty() && !userData.endsWith("/") && !userData.endsWith("\\"))
			userData.concat('/');

		AsciiString patchPath;
		AsciiString disableMarkerPath;

		if (userData.isNotEmpty())
		{
			patchPath = userData;
			patchPath.concat("GeneralsOnlineGameData/500_900_CommunityPatch_CoreINI.big");

			disableMarkerPath = userData;
			disableMarkerPath.concat(kCommunityPatchDisableMarker);
		}

		// The PC client has a settings switch for this (DataPacks_UseCommunityPatch)
		// and a command-line override; a marker file is this port's equivalent, and
		// it sits beside the patch so turning the data off never means deleting it.
		const Bool disabled = disableMarkerPath.isNotEmpty()
			&& TheLocalFileSystem->doesFileExist(disableMarkerPath.str());

		if (disabled)
		{
			fprintf(stderr, "[gxbig] community patch disabled by %s; INI stays retail\n",
				disableMarkerPath.str());
		}
		else if (patchPath.isNotEmpty() && TheLocalFileSystem->doesFileExist(patchPath.str()))
		{
			ArchiveFile* archiveFile = openArchiveFile(patchPath.str());
			if (archiveFile != nullptr)
			{
				loadIntoDirectoryTree(archiveFile, FALSE, TRUE);
				m_archiveFileMap[patchPath] = archiveFile;
				fprintf(stderr, "[gxbig] community patch mounted: %s\n", patchPath.str());
			}
			else
			{
				fprintf(stderr, "[gxbig] community patch found but could not be opened: %s\n", patchPath.str());
			}
		}
		else
		{
			fprintf(stderr, "[gxbig] no community patch archive at %s; INI stays retail\n",
				patchPath.isEmpty() ? "<no user data dir>" : patchPath.str());
		}
		fflush(stderr);
	}
#endif

	if (TheGlobalData->m_modBIG.isNotEmpty())
	{
		ArchiveFile *archiveFile = openArchiveFile(TheGlobalData->m_modBIG.str());

		if (archiveFile != nullptr) {
			DEBUG_LOG(("ArchiveFileSystem::loadMods - loading %s into the directory tree.", TheGlobalData->m_modBIG.str()));
			loadIntoDirectoryTree(archiveFile, TRUE);
			m_archiveFileMap[TheGlobalData->m_modBIG] = archiveFile;
			DEBUG_LOG(("ArchiveFileSystem::loadMods - %s inserted into the archive file map.", TheGlobalData->m_modBIG.str()));
		}
		else
		{
			DEBUG_LOG(("ArchiveFileSystem::loadMods - could not openArchiveFile(%s)", TheGlobalData->m_modBIG.str()));
		}
	}

	if (TheGlobalData->m_modDir.isNotEmpty())
	{
		MAYBE_UNUSED Bool ret = loadBigFilesFromDirectory(TheGlobalData->m_modDir, "*.big", TRUE);
		(void)ret;
		DEBUG_ASSERTLOG(ret, ("loadBigFilesFromDirectory(%s) returned FALSE!", TheGlobalData->m_modDir.str()));
	}
}

Bool ArchiveFileSystem::doesFileExist(const Char *filename, FileInstance instance) const
{
	ArchivedDirectoryInfoResult result = const_cast<ArchiveFileSystem*>(this)->getArchivedDirectoryInfo(filename);

	if (!result.valid())
		return false;

	stl::const_range<ArchivedFileLocationMap> range = stl::get_range(result.dirInfo->m_files, result.lastToken, instance);

	return range.valid();
}

ArchivedDirectoryInfo* ArchiveFileSystem::friend_getArchivedDirectoryInfo(const Char* directory)
{
	ArchivedDirectoryInfoResult result = getArchivedDirectoryInfo(directory);

	return result.dirInfo;
}

ArchiveFileSystem::ArchivedDirectoryInfoResult ArchiveFileSystem::getArchivedDirectoryInfo(const Char* directory)
{
	ArchivedDirectoryInfoResult result;
	ArchivedDirectoryInfo* dirInfo = &m_rootDirectory;

	AsciiString token;
	AsciiString tokenizer = directory;
	tokenizer.toLower();
	Bool infoInPath = tokenizer.nextToken(&token, "\\/");

	while (infoInPath && (!token.find('.') || tokenizer.find('.')))
	{
		ArchivedDirectoryInfoMap::iterator tempiter = dirInfo->m_directories.find(token);
		if (tempiter != dirInfo->m_directories.end())
		{
			dirInfo = &tempiter->second;
			infoInPath = tokenizer.nextToken(&token, "\\/");
		}
		else
		{
			// the directory doesn't exist
			result.dirInfo = nullptr;
			result.lastToken = AsciiString::TheEmptyString;
			return result;
		}
	}

	result.dirInfo = dirInfo;
	result.lastToken = token;
	return result;
}

File * ArchiveFileSystem::openFile(const Char *filename, Int access, FileInstance instance)
{
	ArchiveFile* archive = getArchiveFile(filename, instance);

	if (archive == nullptr)
		return nullptr;

	return archive->openFile(filename, access);
}

Bool ArchiveFileSystem::getFileInfo(const AsciiString& filename, FileInfo *fileInfo, FileInstance instance) const
{
	if (fileInfo == nullptr) {
		return FALSE;
	}

	if (filename.isEmpty()) {
		return FALSE;
	}

	ArchiveFile* archive = getArchiveFile(filename, instance);

	if (archive == nullptr)
		return FALSE;

	return archive->getFileInfo(filename, fileInfo);
}

ArchiveFile* ArchiveFileSystem::getArchiveFile(const AsciiString& filename, FileInstance instance) const
{
	ArchivedDirectoryInfoResult result = const_cast<ArchiveFileSystem*>(this)->getArchivedDirectoryInfo(filename.str());

	if (!result.valid())
		return nullptr;

	stl::const_range<ArchivedFileLocationMap> range = stl::get_range(result.dirInfo->m_files, result.lastToken, instance);

	if (!range.valid())
		return nullptr;
	
	return range.get()->second;
}

void ArchiveFileSystem::getFileListInDirectory(const AsciiString& currentDirectory, const AsciiString& originalDirectory, const AsciiString& searchName, FilenameList &filenameList, Bool searchSubdirectories) const
{
	ArchiveFileMap::const_iterator it = m_archiveFileMap.begin();
	while (it != m_archiveFileMap.end()) {
		it->second->getFileListInDirectory(currentDirectory, originalDirectory, searchName, filenameList, searchSubdirectories);
		it++;
	}
}
