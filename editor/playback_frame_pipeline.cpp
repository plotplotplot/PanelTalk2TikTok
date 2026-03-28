#include "playback_frame_pipeline.h"
#include "debug_controls.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QPointer>
#include <algorithm>
#include <limits>

namespace editor {

namespace {
constexpr int64_t kVisibleDecodeKeepWindow = 8;
constexpr int64_t kObsoleteVisibleFrameSlack = 2;
constexpr int64_t kMaxPresentationPastFrameDelta = 6;
constexpr int64_t kMaxPresentationFutureFrameDelta = 1;

qint64 playbackPipelineTraceMs() {
    static QElapsedTimer timer;
    static bool started = false;
    if (!started) {
        timer.start();
        started = true;
    }
    return timer.elapsed();
}

void playbackPipelineTrace(const QString& stage, const QString& detail = QString()) {
    if (debugPlaybackLevel() < DebugLogLevel::Info) {
        return;
    }
    qDebug().noquote() << QStringLiteral("[PLAYBACK-PIPE %1 ms] %2%3")
                              .arg(playbackPipelineTraceMs(), 6)
                              .arg(stage)
                              .arg(detail.isEmpty() ? QString() : QStringLiteral(" | ") + detail);
}

bool isSingleFrameClip(const TimelineClip& clip) {
    return clip.mediaType == ClipMediaType::Image;
}
}

void PlaybackFramePipeline::PlaybackBuffer::clear() {
    QMutexLocker lock(&m_mutex);
    m_frames.clear();
}

void PlaybackFramePipeline::PlaybackBuffer::insert(int64_t frameNumber, const FrameHandle& frame) {
    if (frame.isNull()) {
        return;
    }
    QMutexLocker lock(&m_mutex);
    m_frames.insert(frameNumber, PlaybackFrameInfo{frame, QDateTime::currentMSecsSinceEpoch()});
    trimLocked();
}

FrameHandle PlaybackFramePipeline::PlaybackBuffer::get(int64_t frameNumber) const {
    QMutexLocker lock(&m_mutex);
    auto it = m_frames.find(frameNumber);
    return it == m_frames.end() ? FrameHandle() : it.value().frame;
}

FrameHandle PlaybackFramePipeline::PlaybackBuffer::getBest(int64_t frameNumber) const {
    QMutexLocker lock(&m_mutex);
    auto exact = m_frames.find(frameNumber);
    if (exact != m_frames.end()) {
        return exact.value().frame;
    }

    qint64 bestDistance = std::numeric_limits<qint64>::max();
    qint64 bestInsertedAt = std::numeric_limits<qint64>::min();
    auto best = m_frames.end();
    for (auto it = m_frames.begin(); it != m_frames.end(); ++it) {
        const qint64 distance = qAbs(it.key() - frameNumber);
        if (distance < bestDistance ||
            (distance == bestDistance && it.value().insertedAt > bestInsertedAt)) {
            bestDistance = distance;
            bestInsertedAt = it.value().insertedAt;
            best = it;
        }
    }
    return best == m_frames.end() ? FrameHandle() : best.value().frame;
}

FrameHandle PlaybackFramePipeline::PlaybackBuffer::getPresentation(int64_t frameNumber) const {
    QMutexLocker lock(&m_mutex);

    auto exact = m_frames.find(frameNumber);
    if (exact != m_frames.end()) {
        return exact.value().frame;
    }

    auto bestPast = m_frames.end();
    qint64 bestPastFrame = std::numeric_limits<qint64>::min();
    qint64 bestPastInsertedAt = std::numeric_limits<qint64>::min();
    auto bestFuture = m_frames.end();
    qint64 bestFutureDistance = std::numeric_limits<qint64>::max();

    for (auto it = m_frames.begin(); it != m_frames.end(); ++it) {
        if (it.key() <= frameNumber) {
            if (it.key() > bestPastFrame ||
                (it.key() == bestPastFrame && it.value().insertedAt > bestPastInsertedAt)) {
                bestPast = it;
                bestPastFrame = it.key();
                bestPastInsertedAt = it.value().insertedAt;
            }
            continue;
        }

        const qint64 futureDistance = it.key() - frameNumber;
        if (futureDistance > kMaxPresentationFutureFrameDelta) {
            continue;
        }
        if (futureDistance < bestFutureDistance) {
            bestFuture = it;
            bestFutureDistance = futureDistance;
        }
    }

    if (bestPast != m_frames.end() &&
        (frameNumber - bestPastFrame) <= kMaxPresentationPastFrameDelta) {
        return bestPast.value().frame;
    }
    return bestFuture == m_frames.end() ? FrameHandle() : bestFuture.value().frame;
}

bool PlaybackFramePipeline::PlaybackBuffer::contains(int64_t frameNumber) const {
    QMutexLocker lock(&m_mutex);
    return m_frames.contains(frameNumber);
}

int PlaybackFramePipeline::PlaybackBuffer::size() const {
    QMutexLocker lock(&m_mutex);
    return m_frames.size();
}

void PlaybackFramePipeline::PlaybackBuffer::trimLocked() {
    while (m_frames.size() > kMaxFrames) {
        auto oldest = m_frames.end();
        qint64 oldestInsertedAt = std::numeric_limits<qint64>::max();
        for (auto it = m_frames.begin(); it != m_frames.end(); ++it) {
            if (it.value().insertedAt < oldestInsertedAt) {
                oldestInsertedAt = it.value().insertedAt;
                oldest = it;
            }
        }
        if (oldest == m_frames.end()) {
            break;
        }
        m_frames.erase(oldest);
    }
}

PlaybackFramePipeline::PlaybackFramePipeline(AsyncDecoder* decoder, QObject* parent)
    : QObject(parent), m_decoder(decoder) {
    if (m_decoder) {
        connect(m_decoder, &AsyncDecoder::frameReady, this, &PlaybackFramePipeline::onFrameReady);
    }
}

PlaybackFramePipeline::~PlaybackFramePipeline() {
    for (PlaybackBuffer* buffer : m_buffers) {
        delete buffer;
    }
    m_buffers.clear();
}

void PlaybackFramePipeline::setTimelineClips(const QVector<TimelineClip>& clips) {
    QMutexLocker lock(&m_clipsMutex);

    QHash<QString, ClipInfo> nextClips;
    QHash<QString, PlaybackBuffer*> nextBuffers;
    for (const TimelineClip& clip : clips) {
        if (!clipHasVisuals(clip)) {
            continue;
        }
        ClipInfo info;
        info.clip = clip;
        info.playbackPath = interactivePreviewMediaPathForClip(clip);
        info.isSingleFrame = isSingleFrameClip(clip);
        nextClips.insert(clip.id, info);
        if (m_buffers.contains(clip.id)) {
            nextBuffers.insert(clip.id, m_buffers.take(clip.id));
        } else {
            nextBuffers.insert(clip.id, new PlaybackBuffer());
        }
    }

    for (PlaybackBuffer* buffer : m_buffers) {
        delete buffer;
    }

    m_clips = std::move(nextClips);
    m_buffers = std::move(nextBuffers);
}

void PlaybackFramePipeline::setRenderSyncMarkers(const QVector<RenderSyncMarker>& markers) {
    QMutexLocker lock(&m_markersMutex);
    m_renderSyncMarkers = markers;
}

void PlaybackFramePipeline::setPlaybackActive(bool active) {
    const bool previous = m_active.exchange(active);
    if (previous && !active) {
        clearBuffers();
    }
}

void PlaybackFramePipeline::setPlayheadFrame(int64_t playheadFrame) {
    m_playheadFrame.store(playheadFrame);
}

void PlaybackFramePipeline::requestFramesForSample(int64_t samplePosition,
                                                   const std::function<void()>& onVisibleFrameReady) {
    if (!m_decoder || !m_active.load()) {
        return;
    }

    QVector<ClipInfo> activeClips;
    QVector<RenderSyncMarker> markers;
    {
        QMutexLocker clipsLock(&m_clipsMutex);
        QMutexLocker markersLock(&m_markersMutex);
        markers = m_renderSyncMarkers;
        for (auto it = m_clips.cbegin(); it != m_clips.cend(); ++it) {
            const TimelineClip& clip = it.value().clip;
            const int64_t clipStartSample = frameToSamples(clip.startFrame) + clip.startSubframeSamples;
            const int64_t clipEndSample = clipStartSample + frameToSamples(clip.durationFrames);
            if (samplePosition >= clipStartSample && samplePosition < clipEndSample) {
                activeClips.push_back(it.value());
            }
        }
    }

    for (const ClipInfo& info : activeClips) {
        const int64_t requestedFrame = sourceFrameForClipAtTimelinePosition(
            info.clip, samplesToFramePosition(samplePosition), markers);
        const int64_t canonicalFrame = normalizeFrameNumber(info, requestedFrame);
        schedulePlaybackWindow(info, canonicalFrame, onVisibleFrameReady);
    }
}

FrameHandle PlaybackFramePipeline::getFrame(const QString& clipId, int64_t frameNumber) const {
    const int64_t normalizedFrame = normalizeFrameNumber(clipId, frameNumber);
    QMutexLocker lock(&m_clipsMutex);
    auto it = m_buffers.find(clipId);
    return (it == m_buffers.end() || !it.value()) ? FrameHandle() : it.value()->get(normalizedFrame);
}

FrameHandle PlaybackFramePipeline::getBestFrame(const QString& clipId, int64_t frameNumber) const {
    const int64_t normalizedFrame = normalizeFrameNumber(clipId, frameNumber);
    QMutexLocker lock(&m_clipsMutex);
    auto it = m_buffers.find(clipId);
    return (it == m_buffers.end() || !it.value()) ? FrameHandle() : it.value()->getBest(normalizedFrame);
}

FrameHandle PlaybackFramePipeline::getPresentationFrame(const QString& clipId, int64_t frameNumber) const {
    const int64_t normalizedFrame = normalizeFrameNumber(clipId, frameNumber);
    QMutexLocker lock(&m_clipsMutex);
    auto it = m_buffers.find(clipId);
    if (it == m_buffers.end() || !it.value()) {
        return FrameHandle();
    }

    const FrameHandle frame = it.value()->getPresentation(normalizedFrame);
    if (!frame.isNull() && frame.frameNumber() < normalizedFrame) {
        m_droppedPresentationFrames.fetch_add(static_cast<int>(normalizedFrame - frame.frameNumber()));
        playbackPipelineTrace(QStringLiteral("PlaybackFramePipeline::present.drop"),
                              QStringLiteral("clip=%1 requested=%2 presented=%3 delta=%4")
                                  .arg(clipId)
                                  .arg(normalizedFrame)
                                  .arg(frame.frameNumber())
                                  .arg(frame.frameNumber() - normalizedFrame));
    }
    return frame;
}

bool PlaybackFramePipeline::isFrameBuffered(const QString& clipId, int64_t frameNumber) const {
    const int64_t normalizedFrame = normalizeFrameNumber(clipId, frameNumber);
    QMutexLocker lock(&m_clipsMutex);
    auto it = m_buffers.find(clipId);
    return it != m_buffers.end() && it.value() && it.value()->contains(normalizedFrame);
}

int PlaybackFramePipeline::pendingVisibleRequestCount() const {
    QMutexLocker lock(&m_pendingMutex);
    return m_pendingVisibleRequests.size();
}

int PlaybackFramePipeline::bufferedFrameCount() const {
    QMutexLocker lock(&m_clipsMutex);
    int total = 0;
    for (auto it = m_buffers.cbegin(); it != m_buffers.cend(); ++it) {
        if (it.value()) {
            total += it.value()->size();
        }
    }
    return total;
}

void PlaybackFramePipeline::onFrameReady(FrameHandle frame) {
    if (!m_active.load() || frame.isNull()) {
        return;
    }

    const QString sourcePath = frame.sourcePath();
    if (sourcePath.isEmpty()) {
        return;
    }

    // Request-specific callbacks own insertion into playback buffers.
    // Avoid globally inserting every decoded frame for a matching file path,
    // which pollutes the presentation buffer with obsolete completions.
    emit frameAvailable();
}

QString PlaybackFramePipeline::requestKey(const QString& clipId, int64_t frameNumber) const {
    return clipId + QLatin1Char(':') + QString::number(frameNumber);
}

int64_t PlaybackFramePipeline::normalizeFrameNumber(const QString& clipId, int64_t frameNumber) const {
    QMutexLocker lock(&m_clipsMutex);
    const auto it = m_clips.find(clipId);
    return it == m_clips.end() ? frameNumber : normalizeFrameNumber(it.value(), frameNumber);
}

int64_t PlaybackFramePipeline::normalizeFrameNumber(const ClipInfo& info, int64_t frameNumber) const {
    return info.isSingleFrame ? 0 : frameNumber;
}

void PlaybackFramePipeline::clearBuffers() {
    {
        QMutexLocker lock(&m_clipsMutex);
        for (PlaybackBuffer* buffer : m_buffers) {
            if (buffer) {
                buffer->clear();
            }
        }
    }
    QMutexLocker pendingLock(&m_pendingMutex);
    m_pendingVisibleRequests.clear();
    m_pendingPrefetchRequests.clear();
    m_latestVisibleTargets.clear();
    m_droppedPresentationFrames.store(0);
}

void PlaybackFramePipeline::schedulePlaybackWindow(const ClipInfo& info,
                                                   int64_t canonicalFrame,
                                                   const std::function<void()>& onFrameReady) {
    const int playbackWindowAhead = debugPlaybackWindowAhead();
    if (!m_decoder || info.isSingleFrame || !m_active.load() || playbackWindowAhead <= 0) {
        return;
    }

    m_decoder->cancelForFileBefore(info.playbackPath,
                                   qMax<int64_t>(0, canonicalFrame - kVisibleDecodeKeepWindow));

    for (int offset = 0; offset <= playbackWindowAhead; ++offset) {
        const int64_t targetFrame = normalizeFrameNumber(info, canonicalFrame + offset);
        const QString key = requestKey(info.clip.id, targetFrame);
        if (isFrameBuffered(info.clip.id, targetFrame)) {
            continue;
        }
        {
            QMutexLocker pendingLock(&m_pendingMutex);
            if (m_pendingVisibleRequests.contains(key) || m_pendingPrefetchRequests.contains(key)) {
                continue;
            }
            if (offset == 0) {
                m_pendingVisibleRequests.insert(key);
                m_latestVisibleTargets.insert(info.clip.id, targetFrame);
            } else {
                m_pendingPrefetchRequests.insert(key);
            }
        }

        QPointer<PlaybackFramePipeline> self(this);
        const qint64 requestedAt = playbackPipelineTraceMs();
        const DecodeRequestKind kind = offset == 0 ? DecodeRequestKind::Visible : DecodeRequestKind::Prefetch;
        const int priority = offset == 0 ? 100 : qMax(65, 100 - (offset * 4));

        if (debugCacheWarnOnlyEnabled() && offset == 0) {
            qDebug().noquote() << QStringLiteral("[CACHE][WARN] %1 PlaybackFramePipeline::visible-miss | clip=%2 frame=%3 normalized=%4")
                                      .arg(playbackPipelineTraceMs(), 6)
                                      .arg(info.clip.id)
                                      .arg(canonicalFrame)
                                      .arg(targetFrame);
        }

        m_decoder->requestFrame(info.playbackPath,
                                targetFrame,
                                priority,
                                5000,
                                kind,
                                [self, clipId = info.clip.id, targetFrame, key, kind, requestedAt, onFrameReady](FrameHandle frame) {
                                    if (!self) {
                                        return;
                                    }
                                    QMetaObject::invokeMethod(self, [self, clipId, targetFrame, key, kind, requestedAt, onFrameReady, frame]() {
                                        if (!self) {
                                            return;
                                        }
                                        {
                                            QMutexLocker pendingLock(&self->m_pendingMutex);
                                            if (kind == DecodeRequestKind::Visible) {
                                                self->m_pendingVisibleRequests.remove(key);
                                                const int64_t latest = self->m_latestVisibleTargets.value(clipId, targetFrame);
                                                if (targetFrame >= latest) {
                                                    self->m_latestVisibleTargets.remove(clipId);
                                                }
                                            } else {
                                                self->m_pendingPrefetchRequests.remove(key);
                                            }
                                        }
                                        const int64_t playheadFrame = self->m_playheadFrame.load();
                                        const bool obsoleteForPresentation =
                                            !frame.isNull() &&
                                            targetFrame < qMax<int64_t>(0, playheadFrame - kObsoleteVisibleFrameSlack);

                                        if (!frame.isNull() && !obsoleteForPresentation) {
                                            QMutexLocker lock(&self->m_clipsMutex);
                                            auto it = self->m_buffers.find(clipId);
                                            if (it != self->m_buffers.end() && it.value()) {
                                                it.value()->insert(targetFrame, frame);
                                            }
                                        }

                                        if (debugCacheWarnOnlyEnabled() &&
                                            kind == DecodeRequestKind::Visible &&
                                            (frame.isNull() ||
                                             obsoleteForPresentation ||
                                             playbackPipelineTraceMs() - requestedAt > 33)) {
                                            qDebug().noquote() << QStringLiteral("[CACHE][WARN] %1 PlaybackFramePipeline::visible-complete | clip=%2 frame=%3 null=%4 waitMs=%5")
                                                                      .arg(playbackPipelineTraceMs(), 6)
                                                                      .arg(clipId)
                                                                      .arg(targetFrame)
                                                                      .arg(frame.isNull() || obsoleteForPresentation ? 1 : 0)
                                                                      .arg(playbackPipelineTraceMs() - requestedAt);
                                        }

                                        if (onFrameReady &&
                                            ((!frame.isNull() && !obsoleteForPresentation) ||
                                             kind == DecodeRequestKind::Visible)) {
                                            onFrameReady();
                                        }
                                    }, Qt::QueuedConnection);
                                });
    }
}

}  // namespace editor
