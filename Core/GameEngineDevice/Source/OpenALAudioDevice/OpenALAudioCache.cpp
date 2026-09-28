#include "OpenALAudioCache.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

#include "Common/AudioEventInfo.h"
#include "Common/AudioEventRTS.h"
#include "Common/file.h"
#include "Common/FileSystem.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
OpenALAudioFileCache::OpenALAudioFileCache() : m_maxSize(14*1024*1024), m_currentlyUsedSize(0)
{
}

Bool OpenALAudioFileCache::decodeFFmpeg(OpenAudioFile* file)
{
	std::vector<uint8_t> audioData;
	auto on_frame = [&audioData](AVFrame* frame, int stream_idx, int stream_type, void* user_data) {
		OpenAudioFile* file = static_cast<OpenAudioFile*>(user_data);
		if (stream_type != AVMEDIA_TYPE_AUDIO) {
			return;
		}

		// GeneralsX @performance Android port 28/09/2026 Grow, don't reallocate. This used to
		// reserve() exactly the size needed for each new frame, which defeats the vector's
		// geometric growth: every decoded frame reallocated and copied everything decoded so
		// far, quadratic in the length of the sound -- tens of milliseconds for a long one on
		// the old test phone, on the main thread. And the planar branch inserted one sample at
		// a time. Resize once per frame and write in place.
		const int frame_data_size = file->m_ffmpegFile->getSizeForSamples(frame->nb_samples);
		const size_t writeAt = audioData.size();
		audioData.resize(writeAt + frame_data_size);
		uint8_t* dst = audioData.data() + writeAt;

		if (av_sample_fmt_is_planar(static_cast<AVSampleFormat>(frame->format))) {
			// Convert planar audio to interleaved
			const int num_channels = file->m_ffmpegFile->getNumChannels();
			const int bytes_per_sample = file->m_ffmpegFile->getBytesPerSample();
			for (int sample = 0; sample < frame->nb_samples; ++sample) {
				for (int channel = 0; channel < num_channels; ++channel) {
					memcpy(dst, frame->data[channel] + sample * bytes_per_sample, bytes_per_sample);
					dst += bytes_per_sample;
				}
			}
		} else {
			// Directly copy interleaved audio
			memcpy(dst, frame->data[0], frame_data_size);
		}
		file->m_fileSize += frame_data_size;
		file->m_totalSamples += frame->nb_samples;
		};

	file->m_ffmpegFile->setFrameCallback(on_frame);
	file->m_ffmpegFile->setUserData(file);

	// Read all packets inside the file
	while (file->m_ffmpegFile->decodePacket()) {
	}

	// Fill the buffer with the audio data
	alBufferData(file->m_buffer, OpenALAudioManager::getALFormat(file->m_ffmpegFile->getNumChannels(), file->m_ffmpegFile->getBytesPerSample() * 8),
		audioData.data(), audioData.size(), file->m_ffmpegFile->getSampleRate());

	// Calculate the duration in MS
	file->m_duration = (file->m_totalSamples / (float)file->m_ffmpegFile->getSampleRate()) * 1000.0f;

	return true;
}

//-------------------------------------------------------------------------------------------------
// GeneralsX @performance Android port 28/09/2026 The game's sound effects are .wav files, and
// opening one through FFmpeg -- demuxer probe, stream-info probe, codec setup, packet loop --
// cost 6.8 ms on average and up to 116 ms per sound on the old test phone (logs-7: 74% of all
// audio time in a battle, all on the main thread, in the frame the sound starts). A WAV is a
// header and samples, and the two codecs it carries here are trivial: plain PCM goes to
// OpenAL as it is, IMA ADPCM is a table lookup per sample. Anything else -- another codec,
// WAVE_FORMAT_EXTENSIBLE, a malformed header -- returns FALSE and takes the FFmpeg path as
// before, so nothing that used to play can stop playing.
namespace {

inline UnsignedInt readLE16(const uint8_t* p) { return (UnsignedInt)p[0] | ((UnsignedInt)p[1] << 8); }
inline UnsignedInt readLE32(const uint8_t* p) { return readLE16(p) | (readLE16(p + 2) << 16); }

const int kImaIndexTable[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };
const int kImaStepTable[89] = {
	7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66,
	73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408,
	449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
	2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630,
	9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
	32767 };

struct ImaChannel { int predictor; int index; };

inline int16_t imaExpand(ImaChannel& c, unsigned nibble)
{
	const int step = kImaStepTable[c.index];
	int diff = step >> 3;
	if (nibble & 4) diff += step;
	if (nibble & 2) diff += step >> 1;
	if (nibble & 1) diff += step >> 2;
	c.predictor += (nibble & 8) ? -diff : diff;
	c.predictor = std::clamp(c.predictor, -32768, 32767);
	c.index = std::clamp(c.index + kImaIndexTable[nibble], 0, 88);
	return (int16_t)c.predictor;
}

// Microsoft IMA ADPCM: per block, a 4-byte header per channel (first sample, step index), then
// 4-byte groups per channel in turn, 8 samples each, low nibble first. A short last block holds
// as many whole groups as fit, the same count FFmpeg's decoder produces for it.
Bool decodeImaAdpcm(const uint8_t* data, size_t size, UnsignedInt channels, UnsignedInt blockAlign,
                    std::vector<int16_t>& out)
{
	if (blockAlign < 4 * channels + 4 * channels)
		return FALSE;
	out.reserve((size / blockAlign + 1) * ((blockAlign - 4 * channels) * 2 / channels + 1) * channels);
	for (size_t pos = 0; pos + 4 * channels <= size; pos += blockAlign) {
		const size_t blockSize = std::min<size_t>(blockAlign, size - pos);
		const uint8_t* block = data + pos;
		ImaChannel ch[2];
		for (UnsignedInt c = 0; c < channels; ++c) {
			ch[c].predictor = (int16_t)readLE16(block + 4 * c);
			ch[c].index = block[4 * c + 2];
			if (ch[c].index > 88)
				return FALSE;
			out.push_back((int16_t)ch[c].predictor);
		}
		const size_t groups = (blockSize - 4 * channels) / (4 * channels);
		const uint8_t* p = block + 4 * channels;
		for (size_t g = 0; g < groups; ++g) {
			int16_t samples[2][8];
			for (UnsignedInt c = 0; c < channels; ++c) {
				for (int b = 0; b < 4; ++b) {
					const uint8_t byte = *p++;
					samples[c][2 * b] = imaExpand(ch[c], byte & 0x0F);
					samples[c][2 * b + 1] = imaExpand(ch[c], byte >> 4);
				}
			}
			for (int i = 0; i < 8; ++i)
				for (UnsignedInt c = 0; c < channels; ++c)
					out.push_back(samples[c][i]);
		}
	}
	return TRUE;
}

} // namespace

Bool OpenALAudioFileCache::decodeWavNative(const uint8_t* data, size_t size, OpenAudioFile* file)
{
	if (size < 12 || memcmp(data, "RIFF", 4) != 0 || memcmp(data + 8, "WAVE", 4) != 0)
		return FALSE;

	UnsignedInt format = 0, channels = 0, rate = 0, blockAlign = 0, bits = 0;
	const uint8_t* samples = nullptr;
	size_t samplesSize = 0;
	for (size_t pos = 12; pos + 8 <= size; ) {
		const uint8_t* chunk = data + pos;
		const size_t chunkSize = readLE32(chunk + 4);
		const size_t body = pos + 8;
		const size_t available = std::min(chunkSize, size - body);
		if (memcmp(chunk, "fmt ", 4) == 0 && available >= 16) {
			format = readLE16(chunk + 8);
			channels = readLE16(chunk + 10);
			rate = readLE32(chunk + 12);
			blockAlign = readLE16(chunk + 20);
			bits = readLE16(chunk + 22);
		} else if (memcmp(chunk, "data", 4) == 0) {
			samples = data + body;
			samplesSize = available;
			break;
		}
		pos = body + chunkSize + (chunkSize & 1);
	}
	if (samples == nullptr || rate == 0 || (channels != 1 && channels != 2))
		return FALSE;

	const UnsignedInt WAVE_FORMAT_PCM = 1;
	const UnsignedInt WAVE_FORMAT_IMA_ADPCM = 0x11;
	if (format == WAVE_FORMAT_PCM && (bits == 8 || bits == 16)) {
		const size_t frameBytes = channels * (bits / 8);
		samplesSize -= samplesSize % frameBytes;
		alBufferData(file->m_buffer, OpenALAudioManager::getALFormat(channels, bits), samples, (ALsizei)samplesSize, rate);
		file->m_totalSamples = (int)(samplesSize / frameBytes);
	} else if (format == WAVE_FORMAT_IMA_ADPCM && bits == 4 && blockAlign > 0) {
		std::vector<int16_t> pcm;
		if (!decodeImaAdpcm(samples, samplesSize, channels, blockAlign, pcm))
			return FALSE;
		alBufferData(file->m_buffer, OpenALAudioManager::getALFormat(channels, 16), pcm.data(),
			(ALsizei)(pcm.size() * sizeof(int16_t)), rate);
		file->m_totalSamples = (int)(pcm.size() / channels);
	} else {
		return FALSE;
	}

	file->m_channels = channels;
	file->m_freq = rate;
	file->m_duration = (file->m_totalSamples / (float)rate) * 1000.0f;
	return TRUE;
}

//-------------------------------------------------------------------------------------------------
OpenALAudioFileCache::~OpenALAudioFileCache()
{
	// Free all the samples that are open.
	OpenFilesHashIt it;
	for (it = m_openFiles.begin(); it != m_openFiles.end(); ++it) {
		if (it->second.m_openCount > 0) {
			DEBUG_CRASH(("Sample '%s' is still playing, and we're trying to quit.\n", it->second.m_eventInfo->m_audioName.str()));
		}

		releaseOpenAudioFile(&it->second);
		// Don't erase it from the map, cause it makes this whole process way more complicated, and 
		// we're about to go away anyways.
	}
}

//-------------------------------------------------------------------------------------------------
ALuint OpenALAudioFileCache::getBufferForFile(const OpenFileInfo &fileInfo)
{
	AudioEventRTS *eventToOpenFrom = fileInfo.event;

	AsciiString strToFind;
	if (eventToOpenFrom)
	{
		switch (eventToOpenFrom->getNextPlayPortion())
		{
		case PP_Attack:
			strToFind = eventToOpenFrom->getAttackFilename();
			break;
		case PP_Sound:
			strToFind = eventToOpenFrom->getFilename();
			break;
		case PP_Decay:
			strToFind = eventToOpenFrom->getDecayFilename();
			break;
		case PP_Done:
			return 0;
		}
	}
	else
	{
		if (fileInfo.filename)
		{
			strToFind = *fileInfo.filename;
		}
		else
		{
			DEBUG_CRASH(("No filename to open\n"));
			return 0;
		}
	}

	auto it = m_openFiles.find(strToFind);

	if (it != m_openFiles.end()) {
		++it->second.m_openCount;
		it->second.m_lastUse = ++m_useClock;
		++m_stats.hits;
		return it->second.m_buffer;
	}

	// GeneralsX @performance Android port 28/09/2026 Time every miss: opening, probing and
	// decoding a file happens right here on the main thread, in the frame that plays it.
	const std::chrono::steady_clock::time_point missStart = std::chrono::steady_clock::now();
	struct MissTimer
	{
		OpenALAudioCacheStats &stats;
		std::chrono::steady_clock::time_point start;
		~MissTimer()
		{
			const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
			++stats.misses;
			stats.decodeMs += ms;
			if (ms > stats.decodeMaxMs)
				stats.decodeMaxMs = ms;
		}
	} missTimer = { m_stats, missStart };

	// Couldn't find the file, so actually open it.
	File* file = TheFileSystem->openFile(strToFind.str());
	m_stats.openMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - missStart).count();
	if (!file) {
		DEBUG_ASSERTLOG(strToFind.isEmpty(), ("Missing Audio File: '%s'\n", strToFind.str()));
		return 0;
	}

	UnsignedInt fileSize = file->size();

	OpenAudioFile openedAudioFile;
	alGenBuffers(1, &openedAudioFile.m_buffer);
	openedAudioFile.m_eventInfo = eventToOpenFrom ? eventToOpenFrom->getAudioEventInfo() : NULL;

	// The built-in WAV reader first; FFmpeg only for what it does not handle.
	Bool decodedNatively = FALSE;
	{
		std::vector<uint8_t> bytes(fileSize);
		if (fileSize > 0 && file->read(bytes.data(), (Int)fileSize) == (Int)fileSize) {
			decodedNatively = decodeWavNative(bytes.data(), bytes.size(), &openedAudioFile);
		}
	}
	if (decodedNatively) {
		++m_stats.nativeWav;
		file->close();
	} else {
		file->seek(0, File::START);
		openedAudioFile.m_ffmpegFile = new FFmpegFile();

		// This transfer ownership of file
		if (!openedAudioFile.m_ffmpegFile->open(file)) {
			releaseOpenAudioFile(&openedAudioFile);
			return 0;
		}

		if (eventToOpenFrom && eventToOpenFrom->isPositionalAudio()) {
			if (openedAudioFile.m_ffmpegFile->getNumChannels() > 1) {
				// GeneralsX @bugfix Bender 09/05/2026 Keep multichannel positional assets loadable so playback can fall back to non-spatial output.
				DEBUG_LOG(("OpenAL positional fallback armed for multichannel audio '%s' (%d channels)\n",
					strToFind.str(), openedAudioFile.m_ffmpegFile->getNumChannels()));
			}
		}

		if (!decodeFFmpeg(&openedAudioFile)) {
			releaseOpenAudioFile(&openedAudioFile);
			return 0;
		}

		openedAudioFile.m_ffmpegFile->close();
	}

	openedAudioFile.m_fileSize = fileSize;
	m_currentlyUsedSize += openedAudioFile.m_fileSize;
	if (m_currentlyUsedSize > m_maxSize) {
		DEBUG_LOG(("Audio Cache is full, trying to free some space\n"));
		// We need to free some samples, or we're not going to be able to play this sound.
		if (!freeEnoughSpaceForSample(openedAudioFile)) {
			DEBUG_LOG(("Couldn't free enough space for sample\n"));
			++m_stats.dropped;
			m_currentlyUsedSize -= openedAudioFile.m_fileSize;
			releaseOpenAudioFile(&openedAudioFile);
			return 0;
		}
	}

	// GeneralsX @bugfix Android port 28/09/2026 The caller is about to play this buffer, so it
	// enters the cache with one user, as MilesAudioFileCache::openFile does (m_openCount = 1).
	// Left at 0, two things went wrong at once. While the sound played, freeEnoughSpaceForSample
	// took it for an unused entry and deleted its buffer under the playing source -- OpenAL
	// refuses that, so the PCM leaked and the cache only believed it had freed the space
	// (the al::base_exception traces from alDeleteBuffers in device logs). And when the sound
	// ended, closeBuffer() decremented 0, wrapping the unsigned count to 4294967295, so the
	// entry never counted as unused again. Once about 14 MB of sounds had played the cache was
	// full for good: every new sound was decoded in full with FFmpeg and then thrown away, on
	// the main thread, which on the old Mali test phone grew to 5-15 ms of every frame in battle.
	openedAudioFile.m_openCount = 1;
	openedAudioFile.m_lastUse = ++m_useClock;
	m_openFiles[strToFind] = openedAudioFile;
	return openedAudioFile.m_buffer;
}

//-------------------------------------------------------------------------------------------------
void OpenALAudioFileCache::closeBuffer(ALuint bufferToClose)
{
	if (!bufferToClose) {
		return;
	}

	OpenFilesHash::iterator it;
	for (it = m_openFiles.begin(); it != m_openFiles.end(); ++it) {
		if (it->second.m_buffer == bufferToClose) {
			--it->second.m_openCount;
			return;
		}
	}
}

float OpenALAudioFileCache::getBufferLength(ALuint handle)
{
	if (!handle) {
		return 0.0f;
	}

	for (auto it = m_openFiles.begin(); it != m_openFiles.end(); ++it) {
		if (it->second.m_buffer == handle) {
			return it->second.m_duration;
		}
	}

	return 0.0f;
}

//-------------------------------------------------------------------------------------------------
void OpenALAudioFileCache::setMaxSize(UnsignedInt size)
{
	// Protect the function, in case we're trying to use this value elsewhere.

	// Hardcoded to 14MiB for now, this is a workaround for the limit
	//  set by the default config files being 4MB and causing needless reloads.
	//m_maxSize = size;
}

//-------------------------------------------------------------------------------------------------
void OpenALAudioFileCache::releaseOpenAudioFile(OpenAudioFile* fileToRelease)
{
	if (fileToRelease->m_openCount > 0) {
		// This thing needs to be terminated IMMEDIATELY.
		TheAudio->closeAnySamplesUsingFile((const void*)(uintptr_t)fileToRelease->m_buffer);
	}

	if (fileToRelease->m_ffmpegFile) {
		// Free FFMPEG handles
		delete fileToRelease->m_ffmpegFile;
	}

	if (fileToRelease->m_buffer)
	{
		// Free the OpenAL buffer
		alDeleteBuffers(1, &fileToRelease->m_buffer);
	}
	fileToRelease->m_ffmpegFile = NULL;
	fileToRelease->m_buffer = 0;
	fileToRelease->m_eventInfo = NULL;
}

//-------------------------------------------------------------------------------------------------
Bool OpenALAudioFileCache::freeEnoughSpaceForSample(const OpenAudioFile& sampleThatNeedsSpace)
{

	Int spaceRequired = m_currentlyUsedSize - m_maxSize;
	Int runningTotal = 0;
	// GeneralsX @bugfix fbraz3 16/04/2026 Handle cache entries without AudioEventInfo (filename-only loads).
	const Int requestedPriority = sampleThatNeedsSpace.m_eventInfo
		? sampleThatNeedsSpace.m_eventInfo->m_priority
		: std::numeric_limits<Int>::min();

	std::list<AsciiString> filesToClose;
	// First, search for any samples that have ref counts of 0. They are low-hanging fruit, and 
	// should be considered immediately.
	//
	// GeneralsX @performance Android port 28/09/2026 Oldest first. The hash map's iteration
	// order is arbitrary, so this used to evict whichever idle sounds the hash happened to list
	// first -- in a battle, as often as not the gunfire that was about to play again, which then
	// had to be opened and decoded on the main thread once more. Least recently used goes first.
	std::vector< std::pair<UnsignedInt, OpenFilesHashIt> > idle;
	OpenFilesHashIt it;
	for (it = m_openFiles.begin(); it != m_openFiles.end(); ++it) {
		if (it->second.m_openCount == 0) {
			idle.push_back(std::make_pair(it->second.m_lastUse, it));
		}
	}
	std::sort(idle.begin(), idle.end(),
		[](const std::pair<UnsignedInt, OpenFilesHashIt> &a, const std::pair<UnsignedInt, OpenFilesHashIt> &b) {
			return a.first < b.first;
		});
	for (size_t i = 0; i < idle.size() && runningTotal < spaceRequired; ++i) {
		// This is said low-hanging fruit.
		filesToClose.push_back(idle[i].second->first);
		runningTotal += idle[i].second->second.m_fileSize;
	}

	// If we don't have enough space yet, then search through the events who have a count of 1 or more
	// and who are lower priority than this sound.
	// Mical said that at this point, sounds shouldn't care if other sounds are interruptable or not.
	// Kill any files of lower priority necessary to clear our the buffer.
	if (runningTotal < spaceRequired) {
		for (it = m_openFiles.begin(); it != m_openFiles.end(); ++it) {
			if (it->second.m_openCount > 0) {
				const Int candidatePriority = it->second.m_eventInfo
					? it->second.m_eventInfo->m_priority
					: std::numeric_limits<Int>::min();
				if (candidatePriority < requestedPriority) {
					filesToClose.push_back(it->first);
					runningTotal += it->second.m_fileSize;

					if (runningTotal >= spaceRequired) {
						break;
					}
				}
			}
		}
	}

	// We weren't able to find enough sounds to truncate. Therefore, this sound is not going to play.
	if (runningTotal < spaceRequired) {
		return FALSE;
	}

	std::list<AsciiString>::iterator ait;
	for (ait = filesToClose.begin(); ait != filesToClose.end(); ++ait) {
		OpenFilesHashIt itToErase = m_openFiles.find(*ait);
		if (itToErase != m_openFiles.end()) {
			releaseOpenAudioFile(&itToErase->second);
			m_currentlyUsedSize -= itToErase->second.m_fileSize;
			m_openFiles.erase(itToErase);
			++m_stats.evicted;
		}
	}

	return TRUE;
}