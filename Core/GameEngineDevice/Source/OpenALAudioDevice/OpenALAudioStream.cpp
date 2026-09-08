#include "OpenALAudioDevice/OpenALAudioStream.h"
#include "OpenALAudioDevice/OpenALAudioManager.h"
#include <AL/alext.h>
#include <chrono>
#include <cstdio>

// GeneralsX @bugfix Android port 08/09/2026 Wall clock for the EOF probe below.
static unsigned long gxNowMs()
{
    using namespace std::chrono;
    return (unsigned long)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

OpenALAudioStream::OpenALAudioStream()
{ 
    alGenSources(1, &m_source);
    alGenBuffers(AL_STREAM_BUFFER_COUNT, m_buffers);

    // GeneralsX @bugfix BenderAI 22/04/2026 Force video stream source to non-positional direct playback.
    alSourcei(m_source, AL_SOURCE_RELATIVE, AL_TRUE);
    alSource3f(m_source, AL_POSITION, 0.0f, 0.0f, 0.0f);
    alSource3f(m_source, AL_VELOCITY, 0.0f, 0.0f, 0.0f);
    alSourcef(m_source, AL_ROLLOFF_FACTOR, 0.0f);
    alSourcef(m_source, AL_GAIN, 1.0f);
    alSourcei(m_source, AL_LOOPING, AL_FALSE);
#ifdef AL_DIRECT_CHANNELS_SOFT
    alSourcei(m_source, AL_DIRECT_CHANNELS_SOFT, AL_TRUE);
#endif
#ifdef AL_SOURCE_SPATIALIZE_SOFT
    alSourcei(m_source, AL_SOURCE_SPATIALIZE_SOFT, AL_FALSE);
#endif

    DEBUG_LOG(("OpenALAudioStream created: %i\n", m_source));
}

OpenALAudioStream::~OpenALAudioStream()
{
    DEBUG_LOG(("OpenALAudioStream freed: %i\n", m_source));
    // Unbind the buffers first
    alSourceStop(m_source);
    alSourcei(m_source, AL_BUFFER, 0);
    alDeleteSources(1, &m_source);
    // Now delete the buffers
    alDeleteBuffers(AL_STREAM_BUFFER_COUNT, m_buffers);
}

bool OpenALAudioStream::bufferData(uint8_t *data, size_t data_size, ALenum format, int samplerate)
{
    DEBUG_LOG(("Buffering %zu bytes of data (samplerate: %i, format: %i)\n", data_size, samplerate, format));
    ALint num_queued;
    alGetSourcei(m_source, AL_BUFFERS_QUEUED, &num_queued);
    if (num_queued >= AL_STREAM_BUFFER_COUNT) {
        DEBUG_LOG(("Having too many buffers already queued: %i", num_queued));
        return false;
    }

    ALuint &current_buffer = m_buffers[m_current_buffer_idx];
    // GeneralsX @bugfix BenderAI 22/04/2026 Detect and reject invalid OpenAL buffer/queue operations.
    while (alGetError() != AL_NO_ERROR) {}
    alBufferData(current_buffer, format, data, data_size, samplerate);
    ALenum err = alGetError();
    if (err != AL_NO_ERROR) {
        DEBUG_LOG(("OpenALAudioStream::bufferData alBufferData failed: err=0x%x format=0x%x size=%zu rate=%d\n",
            (unsigned int)err, (unsigned int)format, data_size, samplerate));
        return false;
    }

    alSourceQueueBuffers(m_source, 1, &current_buffer);
    err = alGetError();
    if (err != AL_NO_ERROR) {
        DEBUG_LOG(("OpenALAudioStream::bufferData alSourceQueueBuffers failed: err=0x%x source=%u buffer=%u\n",
            (unsigned int)err, (unsigned int)m_source, (unsigned int)current_buffer));
        return false;
    }

    m_current_buffer_idx++;

    if (m_current_buffer_idx >= AL_STREAM_BUFFER_COUNT)
        m_current_buffer_idx = 0;

    return true;
}

void OpenALAudioStream::update()
{
    ALint sourceState;
    alGetSourcei(m_source, AL_SOURCE_STATE, &sourceState);

    ALint num_queued;
    alGetSourcei(m_source, AL_BUFFERS_QUEUED, &num_queued);

    // GeneralsX @bugfix 14/06/2026 EOF probe — runs BEFORE the restart-on-stopped guard below.
    // If the source has stopped having fully played everything queued (no unplayed buffers left),
    // probe the source stream once: at true EOF, latch m_endOfData so the guard does NOT restart
    // it. Without this, a one-shot voice line that ends while its queue is still over the refill
    // threshold (so the periodic refill/EOF check below hasn't run) gets restarted, REPLAYING its
    // already-played buffers as a repeating 'chip' until the next line. If data IS still available
    // this is a genuine underrun and m_endOfData stays false so the normal refill+restart recovers.
    {
        ALint processedNow = 0;
        alGetSourcei(m_source, AL_BUFFERS_PROCESSED, &processedNow);
        if (sourceState == AL_STOPPED && num_queued > 0 && processedNow >= num_queued
            && !m_endOfData && m_requireDataCallback) {
            ALint queuedBefore = num_queued;
            bool moreData = m_requireDataCallback();
            ALint queuedAfter = 0;
            alGetSourcei(m_source, AL_BUFFERS_QUEUED, &queuedAfter);
            fprintf(stderr, "[GX-AUDIO] probe src=%u queued=%d processed=%d moreData=%d queuedAfter=%d stalled=%d\n",
                    (unsigned)m_source, (int)num_queued, (int)processedNow, (int)moreData,
                    (int)queuedAfter, (int)m_stalledProbes);
            fflush(stderr);
            if (!moreData) {
                fprintf(stderr, "[GX-AUDIO] EOF latched: decoder said no more data (src=%u)\n", (unsigned)m_source);
                fflush(stderr);
                m_endOfData = true;   // definitive EOF from the decoder
            }
            else if (queuedAfter <= queuedBefore) {
                // GeneralsX @bugfix 04/07/2026 The decoder claims more data is coming but
                // produced none. Decode here is synchronous, so a persistently failing
                // packet never heals — and without this, the restart guard below replays
                // the already-played queue forever: the audible "chirping" loop after a
                // voice line, which also pins the stream un-stopped and holds the
                // disallow-speech flag (silencing subsequent EVA). Three consecutive
                // no-growth probes = the stream is done; latch EOF and let it stop.
                // GeneralsX @bugfix Android port 08/09/2026 Three failures mean "the decoder is
                // stuck" only if they happened in quick succession. Reported: the music on the
                // loading screen arriving in fragments and restarting from the top every time.
                // That is this rule misfiring. Loading pumps the audio rarely and starves the
                // refill, so a perfectly healthy stream shows the same signature -- dry source,
                // callback says "more coming", nothing decoded yet -- a few hundred milliseconds
                // apart, three times. EOF was latched, processPlayingList released the stream,
                // and the music event started again from the beginning.
                //
                // A genuinely stuck decoder fails on consecutive fast updates; a starved one
                // fails on updates that are far apart. So only count a probe as consecutive if
                // it follows the last one closely, and otherwise start the count over.
                const unsigned long nowMs = gxNowMs();
                const bool followsLastClosely =
                    (m_lastProbeMs != 0) && (nowMs - m_lastProbeMs <= 100);
                m_stalledProbes = followsLastClosely ? (m_stalledProbes + 1) : 1;
                m_lastProbeMs = nowMs;
                if (m_stalledProbes >= 3) {
                    fprintf(stderr, "[GX-AUDIO] EOF latched: 3 stalled probes (src=%u, gap=%lums)\n",
                            (unsigned)m_source, followsLastClosely ? (nowMs - m_lastProbeMs) : 0UL);
                    fflush(stderr);
                    m_endOfData = true;
                }
            }
            else {
                m_stalledProbes = 0;
                m_lastProbeMs = 0;
            }
        }
    }

    // GeneralsX @bugfix BenderAI 22/04/2026 Restart before unqueue to avoid dropping freshly queued
    // briefing buffers when OpenAL reports AL_STOPPED with processed buffers.
    // GeneralsX @bugfix 14/06/2026 ...but NOT once the stream is at true EOF: a finished one-shot
    // speech (taunt) must be allowed to reach a stable AL_STOPPED so its disallowSpeech flag clears.
    // GeneralsX @bugfix Android port 08/09/2026 Only restart if there is something here that
    // has NOT been played yet. "num_queued > 0" counts buffers that are still attached to the
    // source, and a source that ran dry still has all of its ALREADY-PLAYED buffers attached,
    // because they are unqueued further down -- after this. So restarting a dry source replayed
    // the last second of audio, drained again, replayed again: reported as the loading music
    // stuttering "like Morse code" and starting over every time, once starved streams stopped
    // being destroyed and this loop got to run repeatedly.
    //
    // Requiring an unprocessed buffer keeps the case this restart exists for -- a source that
    // stopped while freshly queued, never-played data was waiting on it -- and drops the case
    // where the only thing left to "resume" is the past.
    ALint processedNow = 0;
    alGetSourcei(m_source, AL_BUFFERS_PROCESSED, &processedNow);
    if ((sourceState == AL_STOPPED || sourceState == AL_INITIAL || sourceState == AL_PAUSED)
        && num_queued > processedNow && !m_endOfData && !m_paused) {
        play();
        alGetSourcei(m_source, AL_SOURCE_STATE, &sourceState);
    }

    ALint processedBeforeUnqueue = 0;
    alGetSourcei(m_source, AL_BUFFERS_PROCESSED, &processedBeforeUnqueue);
    DEBUG_LOG(("%i buffers have been processed\n", processedBeforeUnqueue));

    // GeneralsX @bugfix BenderAI 22/04/2026 Only unqueue processed data in active playback states.
    ALint processedToUnqueue = ((sourceState == AL_PLAYING || sourceState == AL_PAUSED) ? processedBeforeUnqueue : 0);
    while (processedToUnqueue > 0) {
        ALuint buffer;
        alSourceUnqueueBuffers(m_source, 1, &buffer);
        processedToUnqueue--;
    }

    // GeneralsX @bugfix 14/06/2026 At true EOF the source has stopped with its final buffers
    // still queued-but-processed; the state-gated unqueue above skips them. Reap them here so
    // num_queued can reach 0, letting processPlayingList detect the finished one-shot as stopped
    // (which clears disallowSpeech the frame the audio ends, so back-to-back taunts play).
    if (m_endOfData) {
        ALint processedAtEof = 0;
        alGetSourcei(m_source, AL_BUFFERS_PROCESSED, &processedAtEof);
        while (processedAtEof > 0) {
            ALuint buffer;
            alSourceUnqueueBuffers(m_source, 1, &buffer);
            processedAtEof--;
        }
    }

    alGetSourcei(m_source, AL_BUFFERS_QUEUED, &num_queued);
    DEBUG_LOG(("Having %i buffers queued\n", num_queued));

    if (num_queued < AL_STREAM_BUFFER_COUNT / 2 && m_requireDataCallback && !m_endOfData) {
        // GeneralsX @bugfix BenderAI 22/04/2026 Do not fake queue growth when callback fails to enqueue data.
        // Ask for more data to be buffered.
        // Only fill up to the half, because some formats can output
        // more than one buffer per decoded frame.
        while (num_queued < AL_STREAM_BUFFER_COUNT / 2) {
            // GeneralsX @bugfix 14/06/2026 callback returns FALSE at true end-of-file (no more
            // data will ever come). Latch m_endOfData so we stop restarting the drained source
            // and let it finish. A transient decode error still returns TRUE, so a stutter/underrun
            // mid-line is NOT mistaken for the end and the existing recovery path runs unchanged.
            if (!m_requireDataCallback()) {
                m_endOfData = true;
                break;
            }

            ALint refreshedQueued = 0;
            alGetSourcei(m_source, AL_BUFFERS_QUEUED, &refreshedQueued);
            if (refreshedQueued <= num_queued) {
                break;
            }
            num_queued = refreshedQueued;
            m_stalledProbes = 0;  // GeneralsX @bugfix 04/07/2026 healthy refill: stalls must be CONSECUTIVE to latch EOF, not accumulated over the stream's lifetime
        }
    }

    // GeneralsX @bugfix fbraz3 27/04/2026 Restart after refill when a generic speech stream
    // began the frame with an empty queue; otherwise processPlayingList() can release it as
    // stopped before the newly buffered narrator audio ever starts playing.
    // GeneralsX @bugfix 14/06/2026 As above, do not restart a source that has reached true EOF.
    alGetSourcei(m_source, AL_SOURCE_STATE, &sourceState);
    if ((sourceState == AL_STOPPED || sourceState == AL_INITIAL || sourceState == AL_PAUSED) && num_queued > 0 && !m_endOfData && !m_paused) {
        play();
    }
}

void OpenALAudioStream::reset()
{
    m_paused = false;  // GeneralsX @bugfix Android port 08/09/2026 a reset stream is not a paused one
    DEBUG_LOG(("Resetting stream\n"));
    // alSourceStop() marks all queued buffers as processed so they can be
    // unqueued. alSourceRewind() transitions to AL_INITIAL but does NOT move
    // unprocessed buffers to processed state, so the subsequent
    // alSourcei(AL_BUFFER, 0) would fail with AL_INVALID_OPERATION if any
    // buffers were still pending.
    alSourceStop(m_source);
    ALint num_queued;
    alGetSourcei(m_source, AL_BUFFERS_QUEUED, &num_queued);
    while (num_queued > 0) {
        ALuint buf;
        alSourceUnqueueBuffers(m_source, 1, &buf);
        num_queued--;
    }
    m_current_buffer_idx = 0;
    m_endOfData = false;  // GeneralsX @bugfix 14/06/2026 streams are reused (handleToKill/replace); clear EOF latch
    m_stalledProbes = 0;
}

bool OpenALAudioStream::isPlaying()
{
    ALint state;
    alGetSourcei(m_source, AL_SOURCE_STATE, &state);
    return state == AL_PLAYING;
}