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

///////// StdLocalFileSystem.cpp /////////////////////////
// Stephan Vedder, April 2025
////////////////////////////////////////////////////////////

#include "Common/AsciiString.h"
#include "Common/GameMemory.h"
#include "Common/PerfTimer.h"
#include "StdDevice/Common/StdLocalFileSystem.h"
#include "StdDevice/Common/StdLocalFile.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

#ifndef _WIN32
// GeneralsX @bugfix felipebraz 23/03/2026 Asset root fallback path for loose file lookups.
// On Linux/macOS the game binary's cwd and the data directory (asset root, CNC_GENERALS_ZH_PATH) are separate.
// The StdBIGFileSystem sets this after resolving the primary asset directory so that relative paths like
// "Data\Scripts\SkirmishScripts.scb" can be found in the asset root when the cwd lookup fails.
static std::filesystem::path s_assetFallbackPath;
#endif

StdLocalFileSystem::StdLocalFileSystem() : LocalFileSystem()
{
}

StdLocalFileSystem::~StdLocalFileSystem() {
}

#ifndef _WIN32
// GeneralsX @performance Android port 28/09/2026 Remember which relative names are not loose
// files. FileSystem::openFile() asks the local file system before the .big archives for every
// file, and on a case-sensitive system a miss is not one failed open: it is a stat of the name,
// a stat under the asset root, then a component-by-component case-insensitive search that lists
// directories -- twice, once from the asset root and once from the working directory. On
// Android those directories are in shared storage, behind FUSE, where each of those calls is
// slow, and nearly every asset lives in a .big, so the search always fails. Device logs showed
// it as the bulk of a sound's first-play cost (8-15 ms, up to 85 ms, for a .wav whose decode is
// well under a millisecond), repeated for every sound the cache had evicted.
//
// Only read lookups of RELATIVE names are remembered -- the install directory, which the game
// never writes to while running. Its user files (saves, replays, options, downloaded maps) are
// addressed by absolute paths and are never cached. Any write through this file system clears
// the whole set anyway, as a backstop.
static std::mutex s_missingMutex;
static std::unordered_set<std::string> s_missingRelative;

// GeneralsX @performance Android port 28/09/2026 ...and list each directory once. The negative
// set above only helps from a name's second lookup on; the first one still paid the full
// search, and a battle's opening seconds are nothing but first lookups (logs-9: 10-15 ms of
// directory listing per new sound, most of a miss). The case-insensitive search needs exactly
// one thing per directory -- its entry names -- so keep them. A lookup is then a walk through
// in-memory tables with no system call at all, except the one listing per directory ever
// visited. Guarded by s_missingMutex and cleared with it.
struct ListedDirectory
{
	std::unordered_set<std::string> exact;                  ///< entry names as on disk
	std::unordered_map<std::string, std::string> folded;    ///< lowercased name -> name on disk
};
static std::unordered_map<std::string, ListedDirectory> s_listedDirectories;

static std::string foldCase(const std::string &name)
{
	std::string folded(name);
	std::transform(folded.begin(), folded.end(), folded.begin(), [](unsigned char c) { return (char)std::tolower(c); });
	return folded;
}

static const ListedDirectory &listDirectory(const std::filesystem::path &dir)
{
	std::unordered_map<std::string, ListedDirectory>::iterator it = s_listedDirectories.find(dir.string());
	if (it != s_listedDirectories.end()) {
		return it->second;
	}
	ListedDirectory &listed = s_listedDirectories[dir.string()];
	std::error_code ec;
	for (std::filesystem::directory_iterator entry(dir, ec), end; !ec && entry != end; entry.increment(ec)) {
		const std::string name = entry->path().filename().string();
		listed.exact.insert(name);
		listed.folded.emplace(foldCase(name), name);
	}
	return listed;
}

// The same answer the case-insensitive search in resolveFilenameFromWindowsPath() gives for a
// read -- an exact-case entry first, else the first entry that matches ignoring case -- taken
// from the listings. FALSE when a component is missing, or for "." and ".." components, which
// are left to the full search.
static bool findInListings(const std::filesystem::path &base, const std::filesystem::path &relative, std::filesystem::path &found)
{
	std::filesystem::path current = base;
	for (const auto &part : relative) {
		const std::string name = part.string();
		if (name.empty() || name == "." || name == "..") {
			return false;
		}
		const ListedDirectory &listed = listDirectory(current);
		if (listed.exact.find(name) != listed.exact.end()) {
			current /= name;
			continue;
		}
		std::unordered_map<std::string, std::string>::const_iterator match = listed.folded.find(foldCase(name));
		if (match == listed.folded.end()) {
			return false;
		}
		current /= match->second;
	}
	found = current;
	return true;
}

static void forgetMissingFiles()
{
	std::lock_guard<std::mutex> lock(s_missingMutex);
	s_missingRelative.clear();
	s_listedDirectories.clear();
}
#endif

//DECLARE_PERF_TIMER(StdLocalFileSystem_openFile)
static std::filesystem::path resolveFilenameFromWindowsPath(const Char *filename, Int access);

static std::filesystem::path fixFilenameFromWindowsPath(const Char *filename, Int access)
{
#ifndef _WIN32
	if (access & File::WRITE) {
		forgetMissingFiles();
		return resolveFilenameFromWindowsPath(filename, access);
	}

	const bool relative = filename[0] != '/' && filename[0] != '\\';
	if (relative) {
		std::lock_guard<std::mutex> lock(s_missingMutex);
		if (s_missingRelative.find(filename) != s_missingRelative.end()) {
			return std::filesystem::path();
		}

		// The working directory first, as the full search does, then the asset root.
		std::string slashed(filename);
		std::replace(slashed.begin(), slashed.end(), '\\', '/');
		const std::filesystem::path relativePath(slashed);
		bool usable = true;
		for (const auto &part : relativePath) {
			const std::string name = part.string();
			if (name.empty() || name == "." || name == "..") {
				usable = false;
				break;
			}
		}
		if (usable) {
			std::error_code ec;
			const std::filesystem::path cwd = std::filesystem::current_path(ec);
			std::filesystem::path found;
			if (!ec && findInListings(cwd, relativePath, found)) {
				return found;
			}
			if (!s_assetFallbackPath.empty() && findInListings(s_assetFallbackPath, relativePath, found)) {
				return found;
			}
			s_missingRelative.insert(filename);
			return std::filesystem::path();
		}
	}

	std::filesystem::path resolved = resolveFilenameFromWindowsPath(filename, access);
	if (relative) {
		std::error_code ec;
		if (resolved.empty() || !std::filesystem::exists(resolved, ec)) {
			std::lock_guard<std::mutex> lock(s_missingMutex);
			s_missingRelative.insert(filename);
			return std::filesystem::path();
		}
	}
	return resolved;
#else
	return resolveFilenameFromWindowsPath(filename, access);
#endif
}

static std::filesystem::path resolveFilenameFromWindowsPath(const Char *filename, Int access)
{
	std::string fixedFilename(filename);

#ifndef _WIN32
	// Replace backslashes with forward slashes on unix
	std::replace(fixedFilename.begin(), fixedFilename.end(), '\\', '/');
#endif

	// Convert the filename to a std::filesystem::path and pass that
	std::filesystem::path path(std::move(fixedFilename));

#ifndef _WIN32
	// check if the file exists to see if fixup is required
	// if it's not found try to match disregarding case sensitivity
	// For cases where a write is happening, we should check if the parent path exists, if so, let it through, since the file may not exist yet.
	std::error_code ec;
	if (!std::filesystem::exists(path, ec) &&
		((!(access & File::WRITE)) || ((access & File::WRITE) && !std::filesystem::exists(path.parent_path(), ec))))
	{
		// GeneralsX @bugfix felipebraz 23/03/2026 Before attempting expensive case-insensitive cwd traversal,
		// check if the relative path resolves directly from the asset root (e.g. CNC_GENERALS_ZH_PATH).
		// On Windows cwd == install dir so this is never needed; on Linux/macOS they are separate.
		if (!s_assetFallbackPath.empty() && path.is_relative()) {
			std::filesystem::path assetRootPath = s_assetFallbackPath / path;
			std::error_code ecAsset;
			const bool writeAndParentExists = (access & File::WRITE) && std::filesystem::exists(assetRootPath.parent_path(), ecAsset);
			if (std::filesystem::exists(assetRootPath, ecAsset) || writeAndParentExists) {
				return assetRootPath;
			}

			#ifdef __linux__
			// GeneralsX @bugfix BenderAI 11/05/2026 Linux: resolve case-insensitive paths from asset root.
			// Some cursor files are lowercase on disk (e.g. sccpointer.ani) while INI references mixed-case names.
			// The existing case-insensitive traversal below only checks cwd, not the asset root fallback.
			std::filesystem::path assetRootFixed = s_assetFallbackPath;
			std::filesystem::path assetRootCurrent = s_assetFallbackPath;
			bool assetRootFound = true;
			for (const auto& p : path)
			{
				std::filesystem::path pathFixedPart;
				std::error_code ecAssetCase;
				if (std::filesystem::exists(assetRootCurrent / p, ecAssetCase))
				{
					pathFixedPart = p;
				}
				else
				{
					for (auto& entry : std::filesystem::directory_iterator(assetRootCurrent, ecAssetCase))
					{
						if (strcasecmp(entry.path().filename().string().c_str(), p.string().c_str()) == 0)
						{
							pathFixedPart = entry.path().filename();
							break;
						}
					}
				}

				if (pathFixedPart.empty())
				{
					assetRootFound = false;
					break;
				}

				assetRootFixed /= pathFixedPart;
				assetRootCurrent /= pathFixedPart;
			}

			if (assetRootFound)
			{
				std::error_code ecAssetFixed;
				const bool writeAndParentExistsFixed = (access & File::WRITE)
					&& std::filesystem::exists(assetRootFixed.parent_path(), ecAssetFixed);
				if (std::filesystem::exists(assetRootFixed, ecAssetFixed) || writeAndParentExistsFixed)
				{
					return assetRootFixed;
				}
			}
			#endif
		}
		// Traverse path to try and match case-insensitively
		std::filesystem::path parent = path.parent_path();

		std::filesystem::path pathFixed;
		std::filesystem::path pathCurrent;
		// GeneralsX @build felipebraz 20/06/2025 const auto& required because libc++ std::filesystem::path iterator yields temporaries (non-const lvalue reference would fail on Apple clang)
		for (const auto& p : path)
		{
			std::filesystem::path pathFixedPart;
			if (pathCurrent.empty())
			{
				// Load the first part of the path
				pathFixed /= p;
				pathCurrent /= p;
				continue;
			}

			if (std::filesystem::exists(pathCurrent / p, ec))
			{
				pathFixedPart = p;
			}
			else if (std::filesystem::exists(pathFixed / p, ec))
			{
				pathFixedPart = p;
			}
			else
			{
				// Check if the subpath exists using case-insensitive comparison
				for (auto& entry : std::filesystem::directory_iterator(pathFixed, ec))
				{
					if (strcasecmp(entry.path().filename().string().c_str(), p.string().c_str()) == 0)
					{
						pathFixedPart = entry.path().filename();
						break;
					}
				}
			}

			if (pathFixedPart.empty())
			{
				// Required to allow creation of new files
				if (!(access & File::WRITE))
				{
					DEBUG_LOG(("StdLocalFileSystem::fixFilenameFromWindowsPath - Error finding file %s", filename.string().c_str()));
					DEBUG_LOG(("StdLocalFileSystem::fixFilenameFromWindowsPath - Got so far %s", pathCurrent.string().c_str()));

					return std::filesystem::path();
				}

				// Use the last known good path
				pathFixed = p;
			}

			// Copy of the current path to mirror the current depth
			pathFixed /= pathFixedPart;
			pathCurrent /= p;
		}
		path = pathFixed;
	}
#endif

	return path;
}

File * StdLocalFileSystem::openFile(const Char *filename, Int access, size_t bufferSize)
{
	//USE_PERF_TIMER(StdLocalFileSystem_openFile)

	// sanity check
	if (strlen(filename) <= 0) {
		return nullptr;
	}

	std::filesystem::path path = fixFilenameFromWindowsPath(filename, access);

	if (path.empty()) {
		return nullptr;
	}

	if (access & File::WRITE) {
		// if opening the file for writing, we need to make sure the directory is there
		// before we try to create the file.
		std::filesystem::path dir = path.parent_path();
		std::error_code ec;
		if (!std::filesystem::exists(dir, ec) || ec) {
			if(!std::filesystem::create_directories(dir, ec) || ec) {
				DEBUG_LOG(("StdLocalFileSystem::openFile - Error creating directory %s", dir.string().c_str()));
				return nullptr;
			}
		}
	}

	StdLocalFile *file = newInstance( StdLocalFile );

	if (file->open(path.string().c_str(), access, bufferSize) == FALSE) {
		deleteInstance(file);
		file = nullptr;
	} else {
		file->deleteOnClose();
	}

// this will also need to play nice with the STREAMING type that I added, if we ever enable this

// srj sez: this speeds up INI loading, but makes BIG files unusable.
// don't enable it without further tweaking.
//
// unless you like running really slowly.
//	if (!(access&File::WRITE)) {
//		// Return a ramfile.
//		RAMFile *ramFile = newInstance( RAMFile );
//		if (ramFile->open(file)) {
//			file->close(); // is deleteonclose, so should delete.
//			ramFile->deleteOnClose();
//			return ramFile;
//		}	else {
//			ramFile->close();
//			deleteInstance(ramFile);
//		}
//	}

	return file;
}

void StdLocalFileSystem::update()
{
}

void StdLocalFileSystem::init()
{
}

void StdLocalFileSystem::reset()
{
}

//DECLARE_PERF_TIMER(StdLocalFileSystem_doesFileExist)
Bool StdLocalFileSystem::doesFileExist(const Char *filename) const
{
	std::filesystem::path path = fixFilenameFromWindowsPath(filename, 0);
	if(path.empty()) {
		return FALSE;
	}

	std::error_code ec;
	return std::filesystem::exists(path, ec);
}

void StdLocalFileSystem::getFileListInDirectory(const AsciiString& currentDirectory, const AsciiString& originalDirectory, const AsciiString& searchName, FilenameList & filenameList, Bool searchSubdirectories) const
{

	AsciiString asciisearch;
	asciisearch = originalDirectory;
	asciisearch.concat(currentDirectory);
	// GeneralsX @bugfix Android port 06/09/2026 The extension was compared byte
	// for byte. Windows, where this engine grew up, does not care about the case
	// of a filename; Android's filesystem does. So a game copy whose archives
	// are named "TerrainZH.BIG" rather than "TerrainZH.big" had those archives
	// silently skipped here -- not reported missing, just never listed -- and
	// everything inside them turned into the magenta missing-texture placeholder
	// at draw time, with nothing anywhere saying why. Compare case-insensitively,
	// which is what the caller ("*.big") has always meant.
	std::string searchExt = std::filesystem::path(searchName.str()).extension().string();
	std::transform(searchExt.begin(), searchExt.end(), searchExt.begin(),
		[](unsigned char c) { return (char)std::tolower(c); });
	if (asciisearch.isEmpty()) {
		asciisearch = ".";
	}

	std::string fixedDirectory(asciisearch.str());

#ifndef _WIN32
	// Replace backslashes with forward slashes on unix
	std::replace(fixedDirectory.begin(), fixedDirectory.end(), '\\', '/');
#endif

	Bool done = FALSE;
	std::error_code ec;

	auto iter = std::filesystem::directory_iterator(fixedDirectory.c_str(), ec);
	// The default iterator constructor creates an end iterator
	done = iter == std::filesystem::directory_iterator();

	if (ec) {
		DEBUG_LOG(("StdLocalFileSystem::getFileListInDirectory - Error opening directory %s", fixedDirectory.c_str()));
		return;
	}

	while (!done)	{
		std::string filenameStr = iter->path().filename().string();
		std::string entryExt = iter->path().extension().string();
		std::transform(entryExt.begin(), entryExt.end(), entryExt.begin(),
			[](unsigned char c) { return (char)std::tolower(c); });
		if (!iter->is_directory() && entryExt == searchExt &&
			(strcmp(filenameStr.c_str(), ".") != 0 && strcmp(filenameStr.c_str(), "..") != 0)) {
			// if we haven't already, add this filename to the list.
			// a stl set should only allow one copy of each filename
			AsciiString newFilename = iter->path().string().c_str();
			if (filenameList.find(newFilename) == filenameList.end()) {
				filenameList.insert(newFilename);
			}
		}

		iter++;
		done = iter == std::filesystem::directory_iterator();
	}

	if (searchSubdirectories) {
		auto iter = std::filesystem::directory_iterator(fixedDirectory, ec);

		if (ec) {
			DEBUG_LOG(("StdLocalFileSystem::getFileListInDirectory - Error opening subdirectory %s", fixedDirectory.c_str()));
			return;
		}

		// The default iterator constructor creates an end iterator
		done = iter == std::filesystem::directory_iterator();

		while (!done) {
			std::string filenameStr = iter->path().filename().string();
			if(iter->is_directory() &&
				(strcmp(filenameStr.c_str(), ".") != 0 && strcmp(filenameStr.c_str(), "..") != 0)) {
				AsciiString tempsearchstr(filenameStr.c_str());

				// recursively add files in subdirectories if required.
				getFileListInDirectory(tempsearchstr, originalDirectory, searchName, filenameList, searchSubdirectories);
			}

			iter++;
			done = iter == std::filesystem::directory_iterator();
		}
	}
}

Bool StdLocalFileSystem::getFileInfo(const AsciiString& filename, FileInfo *fileInfo) const
{
	std::filesystem::path path = fixFilenameFromWindowsPath(filename.str(), 0);

	if(path.empty()) {
		return FALSE;
	}

	std::error_code ec;
	auto file_size = std::filesystem::file_size(path, ec);
	if (ec)
	{
		return FALSE;
	}

	auto write_time = std::filesystem::last_write_time(path, ec);
	if (ec)
	{
		return FALSE;
	}

	// TODO: fix this to be win compatible (time since 1601)
	auto time = write_time.time_since_epoch().count();
	fileInfo->timestampHigh = time >> 32;
	fileInfo->timestampLow = time & UINT32_MAX;
	fileInfo->sizeHigh      = file_size >> 32;
	fileInfo->sizeLow  = file_size & UINT32_MAX;

	return TRUE;
}

Bool StdLocalFileSystem::createDirectory(AsciiString directory)
{
	bool result = FALSE;

	std::string fixedDirectory(directory.str());

#ifndef _WIN32
	forgetMissingFiles();
	// Replace backslashes with forward slashes on unix
	std::replace(fixedDirectory.begin(), fixedDirectory.end(), '\\', '/');
#endif

	if ((!fixedDirectory.empty()) && (fixedDirectory.length() < _MAX_DIR)) {
		// Convert to host path
		std::filesystem::path path(std::move(fixedDirectory));

		std::error_code ec;
		result = std::filesystem::create_directory(path, ec);
		if (ec) {
			result = FALSE;
		}
	}
	return result;
}

AsciiString StdLocalFileSystem::normalizePath(const AsciiString& filePath) const
{
	std::string nonNormalized(filePath.str());
#ifndef _WIN32
	// Replace backslashes with forward slashes on non-Windows platforms
	// GeneralsX @bugfix BenderAI 13/02/2026 Fixed typo: unNormalized → nonNormalized
	std::replace(nonNormalized.begin(), nonNormalized.end(), '\\', '/');
#endif
	std::filesystem::path pathNonNormalized(nonNormalized);
	return AsciiString(pathNonNormalized.lexically_normal().string().c_str());
}

#ifndef _WIN32
// GeneralsX @bugfix felipebraz 23/03/2026 Receive the asset root path from StdBIGFileSystem after it resolves
// CNC_GENERALS_ZH_PATH. Used as a fallback in fixFilenameFromWindowsPath so that loose data files
// (e.g. Data\Scripts\SkirmishScripts.scb) can be found even when cwd != asset root directory.
void StdLocalFileSystem::setAssetRootPath(const AsciiString& path)
{
	std::string p(path.str());
	std::replace(p.begin(), p.end(), '\\', '/');
	s_assetFallbackPath = std::filesystem::path(std::move(p));
	DEBUG_LOG(("StdLocalFileSystem::setAssetRootPath - asset fallback path set to '%s'", s_assetFallbackPath.string().c_str()));
}
#endif
