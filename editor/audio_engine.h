#pragma once

#include "editor_shared.h"

#include <QByteArray>
#include <QDebug>
#include <QFile>
#include <QHash>
#include <QSet>
#include <QVector>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

extern "C" {
#ifdef EDITOR_HAS_ALSA
#include <alsa/asoundlib.h>
#endif
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libavutil/avutil.h>
#include <libswresample/swresample.h>
}

#ifdef EDITOR_HAS_ALSA

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

    bool initialize() {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        if (m_initialized) {
            return true;
        }

        int err = snd_pcm_open(&m_pcm, "default", SND_PCM_STREAM_PLAYBACK, 0);
        if (err < 0) {
            qWarning() << "Failed to open ALSA playback device:" << snd_strerror(err);
            return false;
        }

        err = snd_pcm_set_params(m_pcm,
                                 SND_PCM_FORMAT_S16_LE,
                                 SND_PCM_ACCESS_RW_INTERLEAVED,
                                 m_channelCount,
                                 m_sampleRate,
                                 1,
                                 100000);
        if (err < 0) {
            qWarning() << "Failed to configure ALSA playback device:" << snd_strerror(err);
            snd_pcm_close(m_pcm);
            m_pcm = nullptr;
            return false;
        }

        m_running = true;
        m_decodeWorker = std::thread([this]() { decodeLoop(); });
        m_mixWorker = std::thread([this]() { mixLoop(); });
        m_outputWorker = std::thread([this]() { outputLoop(); });
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
        {
            std::lock_guard<std::mutex> queueLock(m_queueMutex);
            m_pcmQueue.clear();
        }
        m_stateCondition.notify_all();
        m_decodeCondition.notify_all();
        m_queueCondition.notify_all();
        if (m_decodeWorker.joinable()) {
            m_decodeWorker.join();
        }
        if (m_mixWorker.joinable()) {
            m_mixWorker.join();
        }
        if (m_outputWorker.joinable()) {
            m_outputWorker.join();
        }
        {
            std::lock_guard<std::mutex> pcmLock(m_pcmMutex);
            if (m_pcm) {
                snd_pcm_drop(m_pcm);
                snd_pcm_close(m_pcm);
                m_pcm = nullptr;
            }
        }
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
            m_playing = true;
            scheduleDecodesLocked(m_timelineClips);
        }
        {
            std::lock_guard<std::mutex> queueLock(m_queueMutex);
            m_pcmQueue.clear();
        }
        m_stateCondition.notify_all();
        m_decodeCondition.notify_one();
        m_queueCondition.notify_all();
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            m_playing = false;
        }
        {
            std::lock_guard<std::mutex> queueLock(m_queueMutex);
            m_pcmQueue.clear();
        }
        {
            std::lock_guard<std::mutex> pcmLock(m_pcmMutex);
            if (m_pcm) {
                snd_pcm_drop(m_pcm);
                snd_pcm_prepare(m_pcm);
            }
        }
        m_queueCondition.notify_all();
    }

    void seek(int64_t frame) {
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            m_timelineSampleCursor = timelineFrameToSamples(frame);
            m_audioClockSample.store(m_timelineSampleCursor, std::memory_order_release);
        }
        {
            std::lock_guard<std::mutex> queueLock(m_queueMutex);
            m_pcmQueue.clear();
        }
        {
            std::lock_guard<std::mutex> pcmLock(m_pcmMutex);
            if (m_pcm) {
                snd_pcm_drop(m_pcm);
                snd_pcm_prepare(m_pcm);
            }
        }
        m_stateCondition.notify_all();
        m_queueCondition.notify_all();
    }

    bool hasPlayableAudio() const {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        for (const TimelineClip& clip : m_timelineClips) {
            if (clip.hasAudio) {
                return true;
            }
        }
        return false;
    }

    int64_t currentSample() const {
        const int64_t submittedSample = m_audioClockSample.load(std::memory_order_acquire);
        snd_pcm_sframes_t delayFrames = 0;
        {
            std::lock_guard<std::mutex> pcmLock(m_pcmMutex);
            if (!m_pcm || snd_pcm_delay(m_pcm, &delayFrames) < 0) {
                return qMax<int64_t>(0, submittedSample);
            }
        }
        return qMax<int64_t>(0, submittedSample - qMax<int64_t>(0, static_cast<int64_t>(delayFrames)));
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
        bool muted = false;
        qreal volume = 0.8;
    };

    int64_t timelineFrameToSamples(int64_t frame) const {
        return frameToSamples(frame);
    }

    int64_t samplesToTimelineFrame(int64_t samples) const {
        return qMax<int64_t>(0, static_cast<int64_t>(std::floor((static_cast<double>(samples) * kTimelineFps) / m_sampleRate)));
    }

    void scheduleDecodesLocked(const QVector<TimelineClip>& clips) {
        for (const TimelineClip& clip : clips) {
            if (!clip.hasAudio || clip.filePath.isEmpty()) {
                continue;
            }
            if (m_audioCache.contains(clip.filePath) || m_pendingDecodeSet.contains(clip.filePath)) {
                continue;
            }
            m_pendingDecodePaths.push_back(clip.filePath);
            m_pendingDecodeSet.insert(clip.filePath);
        }
    }

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

    void mixChunk(const MixContext& context, float* output, int frames, int64_t chunkStartSample) {
        std::fill(output, output + frames * m_channelCount, 0.0f);

        for (const TimelineClip& clip : context.clips) {
            if (!clip.hasAudio) {
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
            const int64_t clipEndSample = clipStartSample + qMin<int64_t>(clip.durationFrames * m_sampleRate / kTimelineFps,
                                                                           clipAvailableSamples);
            const int64_t chunkEndSample = chunkStartSample + frames;
            if (chunkEndSample <= clipStartSample || chunkStartSample >= clipEndSample) {
                continue;
            }

            const int64_t mixStart = qMax<int64_t>(chunkStartSample, clipStartSample);
            const int64_t mixEnd = qMin<int64_t>(chunkEndSample, clipEndSample);
            for (int64_t samplePos = mixStart; samplePos < mixEnd; ++samplePos) {
                const int outFrame = static_cast<int>(samplePos - chunkStartSample);
                const int inFrame = static_cast<int>(sourceInSample + (samplePos - clipStartSample));
                const int outIndex = outFrame * m_channelCount;
                const int inIndex = inFrame * m_channelCount;
                output[outIndex] += audio.samples[inIndex];
                output[outIndex + 1] += audio.samples[inIndex + 1];
            }
        }

        const float gain = context.muted ? 0.0f : static_cast<float>(context.volume);
        for (int i = 0; i < frames * m_channelCount; ++i) {
            output[i] = qBound(-1.0f, output[i] * gain, 1.0f);
        }
    }

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
            MixContext context;
            int64_t chunkStartSample = 0;
            {
                std::unique_lock<std::mutex> lock(m_stateMutex);
                m_stateCondition.wait(lock, [this]() {
                    return !m_running || m_playing;
                });
                if (!m_running) {
                    break;
                }
                lock.unlock();
            }

            {
                std::unique_lock<std::mutex> queueLock(m_queueMutex);
                m_queueCondition.wait(queueLock, [this]() {
                    return !m_running || !m_playing || queuedFramesLocked() <= m_mixLowWaterFrames;
                });
                if (!m_running) {
                    break;
                }
                if (!m_playing) {
                    continue;
                }
            }

            {
                std::lock_guard<std::mutex> lock(m_stateMutex);
                if (!m_playing) {
                    continue;
                }
                chunkStartSample = m_timelineSampleCursor;
                m_timelineSampleCursor += m_periodFrames;
                context.clips = m_timelineClips;
                context.muted = m_muted;
                context.volume = m_volume;
            }

            mixChunk(context, mixBuffer.data(), m_periodFrames, chunkStartSample);

            for (int i = 0; i < pcmBuffer.size(); ++i) {
                pcmBuffer[i] = static_cast<int16_t>(mixBuffer[i] * 32767.0f);
            }

            {
                std::lock_guard<std::mutex> queueLock(m_queueMutex);
                for (int16_t sample : pcmBuffer) {
                    if (m_pcmQueue.size() >= m_maxQueuedSamples) {
                        break;
                    }
                    m_pcmQueue.push_back(sample);
                }
            }
            m_queueCondition.notify_all();
        }
    }

    int queuedFramesLocked() const {
        return static_cast<int>(m_pcmQueue.size() / m_channelCount);
    }

    void outputLoop() {
        QVector<int16_t> pcmBuffer(m_periodFrames * m_channelCount);
        QVector<int16_t> silenceBuffer(m_periodFrames * m_channelCount, 0);

        while (true) {
            bool playing = false;
            {
                std::lock_guard<std::mutex> lock(m_stateMutex);
                if (!m_running) {
                    break;
                }
                playing = m_playing;
            }

            if (!playing) {
                std::unique_lock<std::mutex> lock(m_stateMutex);
                m_stateCondition.wait(lock, [this]() { return !m_running || m_playing; });
                if (!m_running) {
                    break;
                }
                continue;
            }

            bool hadAudio = false;
            {
                std::unique_lock<std::mutex> queueLock(m_queueMutex);
                if (m_pcmQueue.size() < pcmBuffer.size()) {
                    m_queueCondition.wait_for(queueLock, std::chrono::milliseconds(10), [this, &pcmBuffer]() {
                        return !m_running || !m_playing || m_pcmQueue.size() >= pcmBuffer.size();
                    });
                }
                if (!m_running) {
                    break;
                }
                if (m_pcmQueue.size() >= pcmBuffer.size()) {
                    for (int i = 0; i < pcmBuffer.size(); ++i) {
                        pcmBuffer[i] = m_pcmQueue.front();
                        m_pcmQueue.pop_front();
                    }
                    hadAudio = true;
                }
            }

            const int16_t* writePtr = hadAudio ? pcmBuffer.constData() : silenceBuffer.constData();
            int framesRemaining = m_periodFrames;
            while (framesRemaining > 0 && m_running) {
                snd_pcm_sframes_t written = 0;
                {
                    std::lock_guard<std::mutex> pcmLock(m_pcmMutex);
                    written = snd_pcm_writei(m_pcm, writePtr, framesRemaining);
                }
                if (written == -EPIPE) {
                    m_underrunCount.fetch_add(1, std::memory_order_relaxed);
                    std::lock_guard<std::mutex> pcmLock(m_pcmMutex);
                    snd_pcm_prepare(m_pcm);
                    continue;
                }
                if (written < 0) {
                    int recovered = 0;
                    {
                        std::lock_guard<std::mutex> pcmLock(m_pcmMutex);
                        recovered = snd_pcm_recover(m_pcm, static_cast<int>(written), 1);
                    }
                    if (recovered < 0) {
                        m_underrunCount.fetch_add(1, std::memory_order_relaxed);
                        std::lock_guard<std::mutex> pcmLock(m_pcmMutex);
                        snd_pcm_prepare(m_pcm);
                    }
                    continue;
                }
                framesRemaining -= static_cast<int>(written);
                writePtr += written * m_channelCount;
            }

            if (!hadAudio) {
                m_underrunCount.fetch_add(1, std::memory_order_relaxed);
            }
            m_audioClockSample.fetch_add(m_periodFrames, std::memory_order_release);
            m_queueCondition.notify_all();
        }
    }

    mutable std::mutex m_stateMutex;
    mutable std::mutex m_queueMutex;
    std::condition_variable m_stateCondition;
    std::condition_variable m_decodeCondition;
    std::condition_variable m_queueCondition;
    QVector<TimelineClip> m_timelineClips;
    QHash<QString, AudioClipCacheEntry> m_audioCache;
    std::deque<QString> m_pendingDecodePaths;
    QSet<QString> m_pendingDecodeSet;
    std::deque<int16_t> m_pcmQueue;
    std::thread m_decodeWorker;
    std::thread m_mixWorker;
    std::thread m_outputWorker;
    snd_pcm_t* m_pcm = nullptr;
    mutable std::mutex m_pcmMutex;
    bool m_initialized = false;
    bool m_running = false;
    bool m_playing = false;
    bool m_muted = false;
    qreal m_volume = 0.8;
    int64_t m_timelineSampleCursor = 0;
    std::atomic<int64_t> m_audioClockSample{0};
    std::atomic<int> m_underrunCount{0};
    static constexpr int m_sampleRate = 48000;
    static constexpr int m_channelCount = 2;
    static constexpr int m_periodFrames = 1024;
    static constexpr int m_mixLowWaterFrames = 2048;
    static constexpr int m_maxQueuedFrames = 8192;
    static constexpr size_t m_maxQueuedSamples = static_cast<size_t>(m_maxQueuedFrames * m_channelCount);
};

#else // !EDITOR_HAS_ALSA

// Stub AudioEngine for platforms without ALSA (macOS, Windows).
// Preserves the full public API so all call sites compile unchanged.
class AudioEngine {
public:
    AudioEngine() = default;
    ~AudioEngine() { shutdown(); }

    void setTimelineClips(const QVector<TimelineClip>&) {}
    bool initialize() { return false; }
    void shutdown() {}
    void setMuted(bool) {}
    void setVolume(qreal) {}
    bool muted() const { return false; }
    int volumePercent() const { return 0; }
    void start(int64_t) {}
    void stop() {}
    void seek(int64_t) {}
    bool hasPlayableAudio() const { return false; }
    int64_t currentSample() const { return 0; }
    int64_t currentFrame() const { return 0; }
};

#endif // EDITOR_HAS_ALSA
