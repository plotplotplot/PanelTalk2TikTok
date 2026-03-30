#pragma once

#include "editor_shared.h"

#include <QByteArray>
#include <QDebug>
#include <QFile>
#include <QHash>
#include <QSet>
#include <QVector>

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libavutil/avutil.h>
#include <libswresample/swresample.h>
}

#include <RtAudio.h>

// Lock-free SPSC ring buffer for int16 audio samples.
// Single producer (mix thread) writes, single consumer (RtAudio callback) reads.
struct AudioRingBuffer {
    static constexpr size_t kCapacity = 32768; // power of 2

    size_t available() const {
        return m_writePos.load(std::memory_order_acquire) -
               m_readPos.load(std::memory_order_relaxed);
    }

    size_t space() const { return kCapacity - available(); }

    size_t write(const int16_t* data, size_t count) {
        const size_t avail = space();
        count = std::min(count, avail);
        const size_t wp = m_writePos.load(std::memory_order_relaxed);
        for (size_t i = 0; i < count; ++i)
            m_buffer[(wp + i) & (kCapacity - 1)] = data[i];
        m_writePos.store(wp + count, std::memory_order_release);
        return count;
    }

    size_t read(int16_t* data, size_t count) {
        const size_t avail = available();
        count = std::min(count, avail);
        const size_t rp = m_readPos.load(std::memory_order_relaxed);
        for (size_t i = 0; i < count; ++i)
            data[i] = m_buffer[(rp + i) & (kCapacity - 1)];
        m_readPos.store(rp + count, std::memory_order_release);
        return count;
    }

    void clear() {
        // Set readPos = writePos so the consumer sees an empty buffer.
        // This is safe even if the consumer is concurrently reading: it will
        // see available() == 0 on the next call. Resetting both to 0 would
        // race because the consumer could see writePos=0 with readPos still
        // at the old value, computing a negative (wrapped) available count.
        m_readPos.store(m_writePos.load(std::memory_order_acquire), std::memory_order_release);
    }

private:
    std::array<int16_t, kCapacity> m_buffer{};
    std::atomic<size_t> m_readPos{0};
    std::atomic<size_t> m_writePos{0};
};

class AudioEngine {
public:
    AudioEngine() = default;
    ~AudioEngine() { shutdown(); }

    void setTimelineClips(const QVector<TimelineClip>& clips) {
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            m_timelineClips = clips;
            scheduleDecodesLocked(clips);
        }
        m_decodeCondition.notify_one();
    }

    void setExportRanges(const QVector<ExportRangeSegment>& ranges) {
        std::lock_guard<std::mutex> lock(m_exportRangesMutex);
        m_exportRanges = ranges;
    }

    void setRenderSyncMarkers(const QVector<RenderSyncMarker>& markers) {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_renderSyncMarkers = markers;
    }

    void setSpeechFilterFadeSamples(int samples) {
        m_speechFilterFadeSamples.store(qMax(0, samples), std::memory_order_release);
    }

    bool initialize() {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        if (m_initialized) {
            return true;
        }

        try {
            m_rtaudio = std::make_unique<rt::audio::RtAudio>();
        } catch (const std::exception& e) {
            qWarning() << "RtAudio creation failed:" << e.what();
            return false;
        }

        if (m_rtaudio->getDeviceCount() == 0) {
            qWarning() << "No audio output devices found";
            m_rtaudio.reset();
            return false;
        }

        rt::audio::RtAudio::StreamParameters params;
        params.deviceId = m_rtaudio->getDefaultOutputDevice();
        params.nChannels = m_channelCount;

        unsigned int bufferFrames = m_periodFrames;
        auto err = m_rtaudio->openStream(
            &params, nullptr,
            rt::audio::RTAUDIO_SINT16, m_sampleRate, &bufferFrames,
            &AudioEngine::rtAudioCallback, this);
        if (err != rt::audio::RTAUDIO_NO_ERROR) {
            qWarning() << "RtAudio openStream failed";
            m_rtaudio.reset();
            return false;
        }

        m_running = true;
        m_decodeWorker = std::thread([this]() { decodeLoop(); });
        m_mixWorker = std::thread([this]() { mixLoop(); });
        m_initialized = true;
        return true;
    }

    void shutdown() {
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            if (!m_initialized) {
                return;
            }
            m_running = false;
            m_playing = false;
        }
        m_stateCondition.notify_all();
        m_decodeCondition.notify_all();
        m_mixCondition.notify_all();
        if (m_decodeWorker.joinable()) {
            m_decodeWorker.join();
        }
        if (m_mixWorker.joinable()) {
            m_mixWorker.join();
        }
        if (m_rtaudio) {
            if (m_rtaudio->isStreamRunning()) {
                m_rtaudio->stopStream();
            }
            if (m_rtaudio->isStreamOpen()) {
                m_rtaudio->closeStream();
            }
            m_rtaudio.reset();
        }
        m_ringBuffer.clear();
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_initialized = false;
    }

    void setMuted(bool muted) {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_muted = muted;
    }

    void setVolume(qreal volume) {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_volume = qBound<qreal>(0.0, volume, 1.0);
    }

    bool muted() const {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        return m_muted;
    }

    int volumePercent() const {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        return qRound(m_volume * 100.0);
    }

    void start(int64_t startFrame) {
        if (!initialize()) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            m_timelineSampleCursor = timelineFrameToSamples(startFrame);
            m_audioClockSample.store(m_timelineSampleCursor, std::memory_order_release);
            m_ringBufferEndSample.store(m_timelineSampleCursor, std::memory_order_release);
            m_playing = true;
            scheduleDecodesLocked(m_timelineClips);
        }
        m_ringBuffer.clear();
        m_stateCondition.notify_all();
        m_decodeCondition.notify_one();
        m_mixCondition.notify_all();
        if (m_rtaudio && !m_rtaudio->isStreamRunning()) {
            m_rtaudio->startStream();
        }
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            m_playing = false;
        }
        if (m_rtaudio && m_rtaudio->isStreamRunning()) {
            m_rtaudio->stopStream();
        }
        m_ringBuffer.clear();
        m_mixCondition.notify_all();
    }

    void seek(int64_t frame) {
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            const int64_t sample = timelineFrameToSamples(frame);
            m_timelineSampleCursor = sample;
            m_audioClockSample.store(sample, std::memory_order_release);
            m_ringBufferEndSample.store(sample, std::memory_order_release);
        }
        m_ringBuffer.clear();
        m_stateCondition.notify_all();
        m_mixCondition.notify_all();
    }

    bool hasPlayableAudio() const {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        for (const TimelineClip& clip : m_timelineClips) {
            if (clipAudioPlaybackEnabled(clip)) {
                return true;
            }
        }
        return false;
    }

    int64_t currentSample() const {
        const int64_t submitted = m_audioClockSample.load(std::memory_order_acquire);
        long latencyFrames = 0;
        if (m_rtaudio && m_rtaudio->isStreamOpen()) {
            latencyFrames = m_rtaudio->getStreamLatency();
        }
        return qMax<int64_t>(0, submitted - qMax<long>(0, latencyFrames));
    }

    int64_t currentFrame() const {
        return samplesToTimelineFrame(currentSample());
    }

private:
    struct AudioClipCacheEntry {
        QVector<float> samples;
        int sampleRate = 48000;
        int channelCount = 2;
        bool valid = false;
    };

    struct MixContext {
        QVector<TimelineClip> clips;
        QVector<ExportRangeSegment> exportRanges;
        QVector<RenderSyncMarker> renderSyncMarkers;
        bool muted = false;
        qreal volume = 0.8;
    };

    // --- RtAudio callback (called from OS audio thread) ---

    static int rtAudioCallback(void* outputBuffer, void* /*inputBuffer*/,
                               unsigned int nFrames, double /*streamTime*/,
                               rt::audio::RtAudioStreamStatus /*status*/, void* userData) {
        auto* engine = static_cast<AudioEngine*>(userData);
        auto* out = static_cast<int16_t*>(outputBuffer);
        const size_t samplesNeeded = static_cast<size_t>(nFrames) * engine->m_channelCount;
        const size_t read = engine->m_ringBuffer.read(out, samplesNeeded);

        if (read > 0) {
            engine->m_audioClockSample.store(
                engine->m_ringBufferEndSample.load(std::memory_order_acquire),
                std::memory_order_release);
        }
        // Fill remainder with silence on underrun
        if (read < samplesNeeded) {
            std::memset(out + read, 0, (samplesNeeded - read) * sizeof(int16_t));
            engine->m_underrunCount.fetch_add(1, std::memory_order_relaxed);
        }
        // Signal mix thread that buffer was consumed. Use an atomic flag
        // instead of condition_variable::notify_one() because this runs on
        // the OS realtime audio thread — notify_one can acquire an internal
        // mutex on some implementations, causing priority inversion.
        engine->m_bufferConsumed.store(true, std::memory_order_release);
        return 0;
    }

    // --- Sample math ---

    int64_t timelineFrameToSamples(int64_t frame) const {
        return frameToSamples(frame);
    }

    int64_t samplesToTimelineFrame(int64_t samples) const {
        return qMax<int64_t>(0, static_cast<int64_t>(
            std::floor((static_cast<double>(samples) * kTimelineFps) / m_sampleRate)));
    }

    int64_t nextPlayableSampleAtOrAfter(int64_t samplePos,
                                        const QVector<ExportRangeSegment>& ranges) const {
        if (ranges.isEmpty()) {
            return qMax<int64_t>(0, samplePos);
        }
        for (const ExportRangeSegment& range : ranges) {
            const int64_t rangeStartSample = timelineFrameToSamples(range.startFrame);
            const int64_t rangeEndSampleExclusive = timelineFrameToSamples(range.endFrame + 1);
            if (samplePos < rangeStartSample) {
                return rangeStartSample;
            }
            if (samplePos >= rangeStartSample && samplePos < rangeEndSampleExclusive) {
                return samplePos;
            }
        }
        return qMax<int64_t>(0, samplePos);
    }

    // --- Decode scheduling ---

    void scheduleDecodesLocked(const QVector<TimelineClip>& clips) {
        for (const TimelineClip& clip : clips) {
            if (!clipAudioPlaybackEnabled(clip) || clip.filePath.isEmpty()) {
                continue;
            }
            if (m_audioCache.contains(clip.filePath) || m_pendingDecodeSet.contains(clip.filePath)) {
                continue;
            }
            m_pendingDecodePaths.push_back(clip.filePath);
            m_pendingDecodeSet.insert(clip.filePath);
        }
    }

    // --- Gain calculations ---

    float calculateRangeFadeGain(int64_t samplePos, const QVector<ExportRangeSegment>& ranges,
                                 int fadeSamples) const {
        if (ranges.isEmpty() || fadeSamples <= 0) {
            return 1.0f;
        }

        const ExportRangeSegment* currentRange = nullptr;
        for (const auto& range : ranges) {
            const int64_t rangeStartSample = timelineFrameToSamples(range.startFrame);
            const int64_t rangeEndSampleExclusive = timelineFrameToSamples(range.endFrame + 1);
            if (samplePos >= rangeStartSample && samplePos < rangeEndSampleExclusive) {
                currentRange = &range;
                break;
            }
        }

        if (!currentRange) {
            return 0.0f;
        }

        const int64_t rangeStartSample = timelineFrameToSamples(currentRange->startFrame);
        const int64_t rangeEndSampleExclusive = timelineFrameToSamples(currentRange->endFrame + 1);
        const int64_t samplesFromStart = samplePos - rangeStartSample;
        const int64_t samplesToEnd = rangeEndSampleExclusive - samplePos;

        float gain = 1.0f;
        if (samplesFromStart < fadeSamples) {
            gain = qMin(gain,
                        static_cast<float>(samplesFromStart) / static_cast<float>(fadeSamples));
        }
        if (samplesToEnd < fadeSamples) {
            gain = qMin(gain,
                        static_cast<float>(samplesToEnd) / static_cast<float>(fadeSamples));
        }
        return qBound(0.0f, gain, 1.0f);
    }

    float calculateClipCrossfadeGain(int64_t samplePos, const TimelineClip& clip,
                                      int64_t clipStartSample, int64_t clipEndSample,
                                      int fadeSamples) const {
        if (fadeSamples <= 0) {
            return 1.0f;
        }

        float gain = 1.0f;
        const int64_t samplesFromStart = samplePos - clipStartSample;
        if (samplesFromStart >= 0 && samplesFromStart < fadeSamples) {
            gain *= static_cast<float>(samplesFromStart) / static_cast<float>(fadeSamples);
        }
        const int64_t samplesToEnd = clipEndSample - samplePos;
        if (samplesToEnd >= 0 && samplesToEnd < fadeSamples) {
            gain *= static_cast<float>(samplesToEnd) / static_cast<float>(fadeSamples);
        }
        return qBound(0.0f, gain, 1.0f);
    }

    // --- FFmpeg full-file decode ---

    AudioClipCacheEntry decodeClipAudio(const QString& path) {
        AudioClipCacheEntry cache;

        AVFormatContext* formatCtx = nullptr;
        if (avformat_open_input(&formatCtx, QFile::encodeName(path).constData(), nullptr, nullptr) < 0) {
            return cache;
        }

        if (avformat_find_stream_info(formatCtx, nullptr) < 0) {
            avformat_close_input(&formatCtx);
            return cache;
        }

        int audioStreamIndex = -1;
        for (unsigned i = 0; i < formatCtx->nb_streams; ++i) {
            if (formatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                audioStreamIndex = static_cast<int>(i);
                break;
            }
        }
        if (audioStreamIndex < 0) {
            avformat_close_input(&formatCtx);
            return cache;
        }

        AVStream* stream = formatCtx->streams[audioStreamIndex];
        const AVCodec* decoder = avcodec_find_decoder(stream->codecpar->codec_id);
        if (!decoder) {
            avformat_close_input(&formatCtx);
            return cache;
        }

        AVCodecContext* codecCtx = avcodec_alloc_context3(decoder);
        if (!codecCtx) {
            avformat_close_input(&formatCtx);
            return cache;
        }

        if (avcodec_parameters_to_context(codecCtx, stream->codecpar) < 0 ||
            avcodec_open2(codecCtx, decoder, nullptr) < 0) {
            avcodec_free_context(&codecCtx);
            avformat_close_input(&formatCtx);
            return cache;
        }

        SwrContext* swr = swr_alloc();
        AVChannelLayout outLayout;
        av_channel_layout_default(&outLayout, m_channelCount);
        av_opt_set_chlayout(swr, "in_chlayout", &codecCtx->ch_layout, 0);
        av_opt_set_chlayout(swr, "out_chlayout", &outLayout, 0);
        av_opt_set_int(swr, "in_sample_rate", codecCtx->sample_rate, 0);
        av_opt_set_int(swr, "out_sample_rate", m_sampleRate, 0);
        av_opt_set_sample_fmt(swr, "in_sample_fmt", codecCtx->sample_fmt, 0);
        av_opt_set_sample_fmt(swr, "out_sample_fmt", AV_SAMPLE_FMT_FLT, 0);
        if (!swr || swr_init(swr) < 0) {
            av_channel_layout_uninit(&outLayout);
            swr_free(&swr);
            avcodec_free_context(&codecCtx);
            avformat_close_input(&formatCtx);
            return cache;
        }

        AVPacket* packet = av_packet_alloc();
        AVFrame* frame = av_frame_alloc();
        QByteArray converted;

        auto appendConverted = [&](AVFrame* decoded) {
            const int outSamples = swr_get_out_samples(swr, decoded->nb_samples);
            if (outSamples <= 0) {
                return;
            }
            uint8_t* outData = nullptr;
            int outLineSize = 0;
            if (av_samples_alloc(&outData, &outLineSize, m_channelCount, outSamples, AV_SAMPLE_FMT_FLT, 0) < 0) {
                return;
            }
            const int convertedSamples = swr_convert(swr, &outData, outSamples,
                                                     const_cast<const uint8_t**>(decoded->extended_data),
                                                     decoded->nb_samples);
            if (convertedSamples > 0) {
                const int byteCount = convertedSamples * m_channelCount * static_cast<int>(sizeof(float));
                converted.append(reinterpret_cast<const char*>(outData), byteCount);
            }
            av_freep(&outData);
        };

        while (av_read_frame(formatCtx, packet) >= 0) {
            if (packet->stream_index != audioStreamIndex) {
                av_packet_unref(packet);
                continue;
            }
            if (avcodec_send_packet(codecCtx, packet) >= 0) {
                while (avcodec_receive_frame(codecCtx, frame) >= 0) {
                    appendConverted(frame);
                    av_frame_unref(frame);
                }
            }
            av_packet_unref(packet);
        }

        avcodec_send_packet(codecCtx, nullptr);
        while (avcodec_receive_frame(codecCtx, frame) >= 0) {
            appendConverted(frame);
            av_frame_unref(frame);
        }

        const int outSamples = swr_get_out_samples(swr, 0);
        if (outSamples > 0) {
            uint8_t* outData = nullptr;
            int outLineSize = 0;
            if (av_samples_alloc(&outData, &outLineSize, m_channelCount, outSamples, AV_SAMPLE_FMT_FLT, 0) >= 0) {
                const int flushed = swr_convert(swr, &outData, outSamples, nullptr, 0);
                if (flushed > 0) {
                    converted.append(reinterpret_cast<const char*>(outData),
                                     flushed * m_channelCount * static_cast<int>(sizeof(float)));
                }
                av_freep(&outData);
            }
        }

        const int sampleCount = converted.size() / static_cast<int>(sizeof(float));
        cache.samples.resize(sampleCount);
        std::memcpy(cache.samples.data(), converted.constData(), converted.size());
        cache.valid = !cache.samples.isEmpty();

        av_frame_free(&frame);
        av_packet_free(&packet);
        av_channel_layout_uninit(&outLayout);
        swr_free(&swr);
        avcodec_free_context(&codecCtx);
        avformat_close_input(&formatCtx);
        return cache;
    }

    AudioClipCacheEntry clipCacheForPathCopy(const QString& path) const {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        return m_audioCache.value(path);
    }

    QVector<ExportRangeSegment> exportRangesCopy() const {
        std::lock_guard<std::mutex> lock(m_exportRangesMutex);
        return m_exportRanges;
    }

    // --- Mix engine ---

    void mixChunk(const MixContext& context, float* output, int frames, int64_t chunkStartSample) {
        std::fill(output, output + frames * m_channelCount, 0.0f);

        for (const TimelineClip& clip : context.clips) {
            if (!clipAudioPlaybackEnabled(clip)) {
                continue;
            }
            const AudioClipCacheEntry audio = clipCacheForPathCopy(clip.filePath);
            if (!audio.valid) {
                continue;
            }

            const int64_t clipStartSample = clipTimelineStartSamples(clip);
            const int64_t sourceInSample = clipSourceInSamples(clip);
            const int64_t clipAvailableSamples = (audio.samples.size() / m_channelCount) - sourceInSample;
            if (clipAvailableSamples <= 0) {
                continue;
            }
            const int64_t clipEndSample = clipStartSample + qMin<int64_t>(
                clip.durationFrames * m_sampleRate / kTimelineFps, clipAvailableSamples);
            const int64_t chunkEndSample = chunkStartSample + frames;
            if (chunkEndSample <= clipStartSample || chunkStartSample >= clipEndSample) {
                continue;
            }

            const int64_t mixStart = qMax<int64_t>(chunkStartSample, clipStartSample);
            const int64_t mixEnd = qMin<int64_t>(chunkEndSample, clipEndSample);

            for (int64_t samplePos = mixStart; samplePos < mixEnd; ++samplePos) {
                const int outFrame = static_cast<int>(samplePos - chunkStartSample);
                const int64_t inFrame =
                    sourceSampleForClipAtTimelineSample(clip, samplePos, context.renderSyncMarkers);
                if (inFrame < 0 || inFrame >= (audio.samples.size() / m_channelCount)) {
                    continue;
                }
                const int outIndex = outFrame * m_channelCount;
                const int inIndex = static_cast<int>(inFrame * m_channelCount);

                float gain = 1.0f;
                if (!context.exportRanges.isEmpty()) {
                    gain *= calculateRangeFadeGain(samplePos, context.exportRanges,
                        m_speechFilterFadeSamples.load(std::memory_order_acquire));
                }
                gain *= calculateClipCrossfadeGain(samplePos, clip, clipStartSample, clipEndSample,
                    clip.fadeSamples > 0 ? clip.fadeSamples : m_defaultFadeSamples);

                output[outIndex] += audio.samples[inIndex] * gain;
                output[outIndex + 1] += audio.samples[inIndex + 1] * gain;
            }
        }

        const float masterGain = context.muted ? 0.0f : static_cast<float>(context.volume);
        for (int i = 0; i < frames * m_channelCount; ++i) {
            output[i] = qBound(-1.0f, output[i] * masterGain, 1.0f);
        }
    }

    // --- Worker threads ---

    void decodeLoop() {
        while (true) {
            QString nextPath;
            {
                std::unique_lock<std::mutex> lock(m_stateMutex);
                m_decodeCondition.wait(lock, [this]() {
                    return !m_running || !m_pendingDecodePaths.empty();
                });
                if (!m_running) {
                    break;
                }
                nextPath = m_pendingDecodePaths.front();
                m_pendingDecodePaths.pop_front();
            }

            AudioClipCacheEntry decoded = decodeClipAudio(nextPath);

            {
                std::lock_guard<std::mutex> lock(m_stateMutex);
                m_pendingDecodeSet.remove(nextPath);
                m_audioCache.insert(nextPath, decoded);
            }
            m_stateCondition.notify_all();
        }
    }

    void mixLoop() {
        QVector<float> mixBuffer(m_periodFrames * m_channelCount);
        QVector<int16_t> pcmBuffer(m_periodFrames * m_channelCount);

        while (true) {
            // Wait until playing
            {
                std::unique_lock<std::mutex> lock(m_stateMutex);
                m_stateCondition.wait(lock, [this]() {
                    return !m_running || m_playing;
                });
                if (!m_running) {
                    break;
                }
            }

            // Wait until ring buffer needs more data
            {
                std::unique_lock<std::mutex> lock(m_mixMutex);
                m_mixCondition.wait(lock, [this]() {
                    return !m_running || !m_playing ||
                           m_ringBuffer.available() < static_cast<size_t>(m_mixLowWaterSamples);
                });
                if (!m_running) {
                    break;
                }
                if (!m_playing) {
                    continue;
                }
            }

            MixContext context;
            int64_t chunkStartSample = 0;
            {
                std::lock_guard<std::mutex> lock(m_stateMutex);
                if (!m_playing) {
                    continue;
                }
                context.clips = m_timelineClips;
                context.exportRanges = exportRangesCopy();
                context.renderSyncMarkers = m_renderSyncMarkers;
                chunkStartSample = nextPlayableSampleAtOrAfter(m_timelineSampleCursor, context.exportRanges);
                m_timelineSampleCursor = chunkStartSample + m_periodFrames;
                context.muted = m_muted;
                context.volume = m_volume;
            }

            mixChunk(context, mixBuffer.data(), m_periodFrames, chunkStartSample);

            for (int i = 0; i < pcmBuffer.size(); ++i) {
                pcmBuffer[i] = static_cast<int16_t>(mixBuffer[i] * 32767.0f);
            }

            m_ringBuffer.write(pcmBuffer.constData(), static_cast<size_t>(pcmBuffer.size()));
            m_ringBufferEndSample.store(chunkStartSample + m_periodFrames, std::memory_order_release);
        }
    }

    // --- Member variables ---

    mutable std::mutex m_stateMutex;
    mutable std::mutex m_exportRangesMutex;
    std::mutex m_mixMutex;
    std::condition_variable m_stateCondition;
    std::condition_variable m_decodeCondition;
    std::condition_variable m_mixCondition;

    QVector<TimelineClip> m_timelineClips;
    QVector<RenderSyncMarker> m_renderSyncMarkers;
    QVector<ExportRangeSegment> m_exportRanges;
    QHash<QString, AudioClipCacheEntry> m_audioCache;
    std::deque<QString> m_pendingDecodePaths;
    QSet<QString> m_pendingDecodeSet;

    std::thread m_decodeWorker;
    std::thread m_mixWorker;

    std::unique_ptr<rt::audio::RtAudio> m_rtaudio;
    AudioRingBuffer m_ringBuffer;
    std::atomic<int64_t> m_ringBufferEndSample{0};

    bool m_initialized = false;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_playing{false};
    bool m_muted = false;
    qreal m_volume = 0.8;
    int64_t m_timelineSampleCursor = 0;
    std::atomic<int64_t> m_audioClockSample{0};
    std::atomic<int> m_underrunCount{0};

    static constexpr int m_sampleRate = 48000;
    static constexpr int m_channelCount = 2;
    static constexpr int m_periodFrames = 1024;
    static constexpr int m_mixLowWaterSamples = 2048 * 2; // samples (frames * channels)
    static constexpr int m_defaultFadeSamples = 250;
    std::atomic<int> m_speechFilterFadeSamples{m_defaultFadeSamples};
};
