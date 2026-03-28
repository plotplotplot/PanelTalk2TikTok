#include "timeline_cache.h"
#include "debug_controls.h"
#include "media_pipeline_shared.h"

#include <QDebug>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QHash>
#include <QPointer>
#include <algorithm>
#include <cmath>
#include <limits>

namespace editor {

namespace {
constexpr int64_t kVisibleDecodeKeepWindow = 2;
constexpr int64_t kObsoleteVisibleFrameSlack = 0;
qint64 cacheTraceMs() {
    static QElapsedTimer timer;
    static bool started = false;
    if (!started) {
        timer.start();
        started = true;
    }
    return timer.elapsed();
}

void cacheTrace(const QString& stage, const QString& detail = QString()) {
    if (debugCacheLevel() < DebugLogLevel::Info) {
        return;
    }
    static QHash<QString, qint64> lastLogByStage;
    const qint64 now = cacheTraceMs();
    if (!debugCacheVerboseEnabled() &&
        (stage.startsWith(QStringLiteral("TimelineCache::onPrefetchTimer")) ||
         stage.startsWith(QStringLiteral("TimelineCache::prefetch.skip")) ||
         stage.startsWith(QStringLiteral("TimelineCache::requestFrame.miss")) ||
         stage.startsWith(QStringLiteral("TimelineCache::requestFrame.dispatch")))) {
        const qint64 last = lastLogByStage.value(stage, std::numeric_limits<qint64>::min());
        if (now - last < 250) {
            return;
        }
        lastLogByStage.insert(stage, now);
    }
    qDebug().noquote() << QStringLiteral("[CACHE %1 ms] %2%3")
                              .arg(now, 6)
                              .arg(stage)
                              .arg(detail.isEmpty() ? QString() : QStringLiteral(" | ") + detail);
}

void cacheWarnTrace(const QString& stage, const QString& detail = QString()) {
    if (!debugCacheWarnEnabled()) {
        return;
    }
    const qint64 now = cacheTraceMs();
    qDebug().noquote() << QStringLiteral("[CACHE][WARN] %1 %2%3")
                              .arg(now, 6)
                              .arg(stage)
                              .arg(detail.isEmpty() ? QString() : QStringLiteral(" | ") + detail);
}

bool isSingleFramePath(const QString& path) {
    static const QSet<QString> suffixes = {
        QStringLiteral("png"),
        QStringLiteral("jpg"),
        QStringLiteral("jpeg"),
        QStringLiteral("bmp"),
        QStringLiteral("gif"),
        QStringLiteral("webp"),
        QStringLiteral("tif"),
        QStringLiteral("tiff")
    };
    return suffixes.contains(QFileInfo(path).suffix().toLower());
}
}

void TimelineCache::PlaybackBuffer::clear() {
    QMutexLocker lock(&m_mutex);
    m_frames.clear();
}

void TimelineCache::PlaybackBuffer::insert(int64_t frameNumber, const FrameHandle& frame) {
    if (frame.isNull() || frame.hasHardwareFrame()) {
        return;
    }
    QMutexLocker lock(&m_mutex);
    PlaybackFrameInfo info;
    info.frame = frame;
    info.insertedAt = QDateTime::currentMSecsSinceEpoch();
    m_frames.insert(frameNumber, info);
    trimLocked();
}

FrameHandle TimelineCache::PlaybackBuffer::get(int64_t frameNumber) {
    QMutexLocker lock(&m_mutex);
    auto it = m_frames.find(frameNumber);
    if (it == m_frames.end()) {
        return FrameHandle();
    }
    return it.value().frame;
}

FrameHandle TimelineCache::PlaybackBuffer::getBest(int64_t frameNumber) {
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

FrameHandle TimelineCache::PlaybackBuffer::getLatestAtOrBefore(int64_t frameNumber) {
    QMutexLocker lock(&m_mutex);
    auto best = m_frames.end();
    for (auto it = m_frames.begin(); it != m_frames.end(); ++it) {
        if (it.key() <= frameNumber && (best == m_frames.end() || it.key() > best.key())) {
            best = it;
        }
    }
    if (best != m_frames.end()) {
        return best.value().frame;
    }

    auto earliestAhead = m_frames.end();
    for (auto it = m_frames.begin(); it != m_frames.end(); ++it) {
        if (it.key() > frameNumber && (earliestAhead == m_frames.end() || it.key() < earliestAhead.key())) {
            earliestAhead = it;
        }
    }
    return earliestAhead == m_frames.end() ? FrameHandle() : earliestAhead.value().frame;
}

bool TimelineCache::PlaybackBuffer::contains(int64_t frameNumber) const {
    QMutexLocker lock(&m_mutex);
    return m_frames.contains(frameNumber);
}

void TimelineCache::PlaybackBuffer::trimLocked() {
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

// ============================================================================
// ClipCache Implementation
// ============================================================================

ClipCache::ClipCache(const QString& path, int64_t duration, MemoryBudget* budget)
    : m_path(path), m_duration(duration), m_budget(budget) {}

ClipCache::~ClipCache() {
    if (m_budget && m_memoryUsage > 0) {
        size_t cpuBytes = 0;
        size_t gpuBytes = 0;
        {
            QMutexLocker lock(&m_mutex);
            for (auto it = m_frames.cbegin(); it != m_frames.cend(); ++it) {
                cpuBytes += it.value().frame.cpuMemoryUsage();
                gpuBytes += it.value().frame.gpuMemoryUsage();
            }
        }
        if (cpuBytes > 0) {
            m_budget->deallocateCpu(cpuBytes);
        }
        if (gpuBytes > 0) {
            m_budget->deallocateGpu(gpuBytes);
        }
    }
}

ClipCache::FrameMemoryUse ClipCache::frameMemoryUse(const FrameHandle& frame) const {
    return FrameMemoryUse{
        frame.cpuMemoryUsage(),
        frame.gpuMemoryUsage()
    };
}

void ClipCache::insert(int64_t frameNumber, const FrameHandle& frame) {
    FrameMemoryUse replacedUse;
    FrameMemoryUse insertedUse;
    QMutexLocker lock(&m_mutex);
    
    // Remove old frame if exists
    auto it = m_frames.find(frameNumber);
    if (it != m_frames.end()) {
        replacedUse = frameMemoryUse(it.value().frame);
        m_memoryUsage -= (replacedUse.cpuBytes + replacedUse.gpuBytes);
    }
    
    CachedFrame cf;
    cf.frame = frame;
    cf.lastAccessTime = QDateTime::currentMSecsSinceEpoch();
    cf.accessCount = 1;
    
    m_frames[frameNumber] = cf;
    insertedUse = frameMemoryUse(frame);
    m_memoryUsage += (insertedUse.cpuBytes + insertedUse.gpuBytes);
    lock.unlock();

    if (m_budget && replacedUse.cpuBytes > 0) {
        m_budget->deallocateCpu(replacedUse.cpuBytes);
    }
    if (m_budget && replacedUse.gpuBytes > 0) {
        m_budget->deallocateGpu(replacedUse.gpuBytes);
    }
    if (m_budget && insertedUse.cpuBytes > 0) {
        m_budget->allocateCpu(insertedUse.cpuBytes, MemoryBudget::Priority::Normal);
    }
    if (m_budget && insertedUse.gpuBytes > 0) {
        m_budget->allocateGpu(insertedUse.gpuBytes, MemoryBudget::Priority::Normal);
    }
}

FrameHandle ClipCache::get(int64_t frameNumber) {
    QMutexLocker lock(&m_mutex);
    
    auto it = m_frames.find(frameNumber);
    if (it == m_frames.end()) {
        return FrameHandle();
    }
    
    it.value().lastAccessTime = QDateTime::currentMSecsSinceEpoch();
    it.value().accessCount++;
    
    return it.value().frame;
}

FrameHandle ClipCache::getBest(int64_t frameNumber) {
    QMutexLocker lock(&m_mutex);

    auto exact = m_frames.find(frameNumber);
    if (exact != m_frames.end()) {
        exact.value().lastAccessTime = QDateTime::currentMSecsSinceEpoch();
        exact.value().accessCount++;
        return exact.value().frame;
    }

    qint64 bestDistance = std::numeric_limits<qint64>::max();
    auto best = m_frames.end();
    for (auto it = m_frames.begin(); it != m_frames.end(); ++it) {
        const qint64 distance = qAbs(it.key() - frameNumber);
        if (distance < bestDistance || (distance == bestDistance && it.key() < best.key())) {
            bestDistance = distance;
            best = it;
        }
    }

    if (best == m_frames.end()) {
        return FrameHandle();
    }

    best.value().lastAccessTime = QDateTime::currentMSecsSinceEpoch();
    best.value().accessCount++;
    return best.value().frame;
}

FrameHandle ClipCache::getLatestAtOrBefore(int64_t frameNumber) {
    QMutexLocker lock(&m_mutex);

    auto best = m_frames.end();
    for (auto it = m_frames.begin(); it != m_frames.end(); ++it) {
        if (it.key() <= frameNumber && (best == m_frames.end() || it.key() > best.key())) {
            best = it;
        }
    }
    if (best != m_frames.end()) {
        best.value().lastAccessTime = QDateTime::currentMSecsSinceEpoch();
        best.value().accessCount++;
        return best.value().frame;
    }

    auto earliestAhead = m_frames.end();
    for (auto it = m_frames.begin(); it != m_frames.end(); ++it) {
        if (it.key() > frameNumber && (earliestAhead == m_frames.end() || it.key() < earliestAhead.key())) {
            earliestAhead = it;
        }
    }
    if (earliestAhead == m_frames.end()) {
        return FrameHandle();
    }
    earliestAhead.value().lastAccessTime = QDateTime::currentMSecsSinceEpoch();
    earliestAhead.value().accessCount++;
    return earliestAhead.value().frame;
}

bool ClipCache::contains(int64_t frameNumber) const {
    QMutexLocker lock(&m_mutex);
    return m_frames.contains(frameNumber);
}

void ClipCache::remove(int64_t frameNumber) {
    FrameMemoryUse releasedUse;
    QMutexLocker lock(&m_mutex);
    
    auto it = m_frames.find(frameNumber);
    if (it != m_frames.end()) {
        releasedUse = frameMemoryUse(it.value().frame);
        m_memoryUsage -= (releasedUse.cpuBytes + releasedUse.gpuBytes);
        m_frames.erase(it);
    }
    lock.unlock();

    if (m_budget && releasedUse.cpuBytes > 0) {
        m_budget->deallocateCpu(releasedUse.cpuBytes);
    }
    if (m_budget && releasedUse.gpuBytes > 0) {
        m_budget->deallocateGpu(releasedUse.gpuBytes);
    }
}

void ClipCache::evictToFit(size_t maxMemory) {
    QMutexLocker lock(&m_mutex);
    
    if (m_memoryUsage <= maxMemory) return;
    
    // Sort by last access time (oldest first)
    QList<int64_t> keys;
    for (auto it = m_frames.begin(); it != m_frames.end(); ++it) {
        keys.append(it.key());
    }
    
    std::sort(keys.begin(), keys.end(), [this](int64_t a, int64_t b) {
        return m_frames[a].lastAccessTime < m_frames[b].lastAccessTime;
    });
    
    // Evict oldest until under budget
    for (int64_t key : keys) {
        if (m_memoryUsage <= maxMemory) break;
        
        auto it = m_frames.find(key);
        if (it != m_frames.end()) {
            const FrameMemoryUse use = frameMemoryUse(it.value().frame);
            m_memoryUsage -= (use.cpuBytes + use.gpuBytes);
            if (m_budget && use.cpuBytes > 0) {
                m_budget->deallocateCpu(use.cpuBytes);
            }
            if (m_budget && use.gpuBytes > 0) {
                m_budget->deallocateGpu(use.gpuBytes);
            }
            m_frames.erase(it);
        }
    }
}

QList<int64_t> ClipCache::cachedFrames() const {
    QMutexLocker lock(&m_mutex);
    return m_frames.keys();
}

QList<CacheEntryInfo> ClipCache::entries() const {
    QMutexLocker lock(&m_mutex);

    QList<CacheEntryInfo> result;
    result.reserve(m_frames.size());
    for (auto it = m_frames.cbegin(); it != m_frames.cend(); ++it) {
        result.append({it.key(), it.value().lastAccessTime, it.value().frame.memoryUsage()});
    }
    return result;
}

// ============================================================================
// TimelineCache Implementation
// ============================================================================

TimelineCache::TimelineCache(AsyncDecoder* decoder, MemoryBudget* budget, QObject* parent)
    : QObject(parent), m_decoder(decoder), m_budget(budget) {
    m_prefetchTimer.setInterval(16);  // ~60fps scheduling cadence
    connect(&m_prefetchTimer, &QTimer::timeout, this, &TimelineCache::onPrefetchTimer);
    if (m_decoder) {
        connect(m_decoder, &AsyncDecoder::frameReady, this, &TimelineCache::onFrameDecoded);
    }

    if (m_budget) {
        connect(m_budget, &MemoryBudget::trimRequested, this, &TimelineCache::onMemoryPressure);
    }
}

TimelineCache::~TimelineCache() {
    m_aliveToken->store(false);
    stopPrefetching();
    size_t releasedMemory = 0;
    for (ClipCache* cache : m_caches) {
        releasedMemory += cache->memoryUsage();
        delete cache;
    }
    m_caches.clear();
    for (PlaybackBuffer* buffer : m_playbackBuffers) {
        delete buffer;
    }
    m_playbackBuffers.clear();
    Q_UNUSED(releasedMemory)
}

void TimelineCache::setMaxMemory(size_t bytes) {
    if (m_budget) {
        m_budget->setMaxCpuMemory((bytes * 3) / 4);
        m_budget->setMaxGpuMemory(bytes / 4);
    }
}

void TimelineCache::setPlayheadFrame(int64_t frame) {
    m_playhead.store(frame);

    if (m_state.load() == PlaybackState::Playing) {
        dropStaleRequestsForPlayhead(frame);
    }
}

void TimelineCache::setPlaybackState(PlaybackState state) {
    const PlaybackState previous = m_state.exchange(state);
    if (previous == state) {
        return;
    }

    if (state == PlaybackState::Playing) {
        return;
    }

    QMutexLocker lock(&m_clipsMutex);
    for (PlaybackBuffer* buffer : m_playbackBuffers) {
        if (buffer) {
            buffer->clear();
        }
    }
}

void TimelineCache::setExportRanges(const QVector<ExportRangeSegment>& ranges) {
    QMutexLocker lock(&m_exportRangesMutex);
    m_exportRanges = ranges;
}

void TimelineCache::setRenderSyncMarkers(const QVector<RenderSyncMarker>& markers) {
    QMutexLocker lock(&m_exportRangesMutex);
    m_renderSyncMarkers = markers;
}

// Given a frame, return the next valid frame considering export ranges (speech filter gaps)
// Returns -1 if no valid frame exists in the requested direction
static int64_t nextValidFrame(int64_t currentFrame, int step, const QVector<ExportRangeSegment>& ranges) {
    if (ranges.isEmpty()) {
        return currentFrame + step;
    }
    
    if (step > 0) {
        // Forward direction - find next frame that falls within any export range
        for (const auto& range : ranges) {
            if (currentFrame < range.startFrame) {
                // We're before this range - jump to its start
                return range.startFrame;
            }
            if (currentFrame >= range.startFrame && currentFrame < range.endFrame) {
                // Inside a range - advance normally
                return currentFrame + 1;
            }
            // currentFrame >= range.endFrame: past this range, continue to next
        }
        // Past all ranges
        return -1;
    } else {
        // Backward direction
        for (int i = ranges.size() - 1; i >= 0; --i) {
            const auto& range = ranges[i];
            if (currentFrame > range.endFrame) {
                // We're after this range - jump to its end
                return range.endFrame;
            }
            if (currentFrame > range.startFrame && currentFrame <= range.endFrame) {
                // Inside a range - advance normally
                return currentFrame - 1;
            }
            // currentFrame <= range.startFrame: before this range, continue to previous
        }
        // Before all ranges
        return -1;
    }
}

void TimelineCache::registerClip(const TimelineClip& clip) {
    QMutexLocker lock(&m_clipsMutex);
    
    ClipInfo info;
    info.clip = clip;
    info.decodePath = interactivePreviewMediaPathForClip(clip);
    info.isSingleFrame = isSingleFramePath(info.decodePath.isEmpty() ? clip.filePath : info.decodePath);
    
    m_clips[clip.id] = info;
    m_caches[clip.id] = new ClipCache(info.decodePath.isEmpty() ? clip.filePath : info.decodePath,
                                      clip.durationFrames,
                                      m_budget);
    m_playbackBuffers[clip.id] = new PlaybackBuffer();
}

void TimelineCache::registerClip(const QString& id, const QString& path,
                                 int64_t startFrame, int64_t duration) {
    TimelineClip clip;
    clip.id = id;
    clip.filePath = path;
    clip.startFrame = startFrame;
    clip.durationFrames = duration;
    clip.sourceDurationFrames = duration;
    registerClip(clip);
}

void TimelineCache::unregisterClip(const QString& id) {
    QMutexLocker lock(&m_clipsMutex);
    
    m_clips.remove(id);
    ClipCache* cache = m_caches.take(id);
    PlaybackBuffer* playbackBuffer = m_playbackBuffers.take(id);
    delete cache;
    delete playbackBuffer;
    lock.unlock();

    QMutexLocker pendingLock(&m_pendingMutex);
    m_latestVisibleTargets.remove(id);
}

void TimelineCache::clearClips() {
    QMutexLocker lock(&m_clipsMutex);
    
    m_clips.clear();
    for (ClipCache* cache : m_caches) {
        delete cache;
    }
    m_caches.clear();
    for (PlaybackBuffer* buffer : m_playbackBuffers) {
        delete buffer;
    }
    m_playbackBuffers.clear();
    lock.unlock();

    QMutexLocker pendingLock(&m_pendingMutex);
    m_latestVisibleTargets.clear();
}

void TimelineCache::requestFrame(const QString& clipId, int64_t frameNumber, 
                                 std::function<void(FrameHandle)> callback) {
    m_requests++;
    const qint64 requestedAt = cacheTraceMs();
    const int64_t normalizedFrame = normalizeFrameNumber(clipId, frameNumber);
    
    // Check cache first
    FrameHandle cached = m_state.load() == PlaybackState::Playing
                             ? getPlaybackFrame(clipId, normalizedFrame)
                             : getCachedFrame(clipId, normalizedFrame);
    if (!cached.isNull()) {
        m_hits++;
        cacheTrace(QStringLiteral("TimelineCache::requestFrame.hit"),
                   QStringLiteral("clip=%1 frame=%2 normalized=%3")
                       .arg(clipId)
                       .arg(frameNumber)
                       .arg(normalizedFrame));
        callback(cached);
        return;
    }
    cacheTrace(QStringLiteral("TimelineCache::requestFrame.miss"),
               QStringLiteral("clip=%1 frame=%2 normalized=%3")
                   .arg(clipId)
                   .arg(frameNumber)
                   .arg(normalizedFrame));
    if (debugCacheWarnOnlyEnabled()) {
        cacheWarnTrace(QStringLiteral("TimelineCache::visible-miss"),
                       QStringLiteral("clip=%1 frame=%2 normalized=%3")
                           .arg(clipId)
                           .arg(frameNumber)
                           .arg(normalizedFrame));
    }

    QMutexLocker lock(&m_clipsMutex);
    auto it = m_clips.find(clipId);
    if (it == m_clips.end()) {
        cacheTrace(QStringLiteral("TimelineCache::requestFrame.missing-clip"),
                   QStringLiteral("clip=%1 frame=%2").arg(clipId).arg(normalizedFrame));
        callback(FrameHandle());
        return;
    }
    
    const ClipInfo info = it.value();
    lock.unlock();
    const int64_t canonicalFrame = normalizeFrameNumber(info, normalizedFrame);
    if (info.decodePath.isEmpty()) {
        callback(FrameHandle());
        return;
    }
    const QString key = requestKey(clipId, canonicalFrame);

    {
        QMutexLocker pendingLock(&m_pendingMutex);
        m_latestVisibleTargets.insert(clipId, canonicalFrame);
        auto existing = m_pendingVisibleRequests.find(key);
        if (existing != m_pendingVisibleRequests.end()) {
            existing->callbacks.push_back(std::move(callback));
            cacheTrace(QStringLiteral("TimelineCache::requestFrame.dedup"),
                       QStringLiteral("clip=%1 frame=%2 normalized=%3 listeners=%4")
                           .arg(clipId)
                           .arg(frameNumber)
                           .arg(canonicalFrame)
                           .arg(existing->callbacks.size()));
            return;
        }

        PendingVisibleRequest pending;
        pending.callbacks.push_back(std::move(callback));
        m_pendingVisibleRequests.insert(key, std::move(pending));
        m_pendingPrefetchRequests.remove(key);
    }
    
    // Request from decoder
    if (m_decoder) {
        if (m_state.load() == PlaybackState::Playing && !info.isSingleFrame) {
            const int64_t keepFromFrame = qMax<int64_t>(0, canonicalFrame - kVisibleDecodeKeepWindow);
            m_decoder->cancelForFileBefore(info.decodePath, keepFromFrame);
        }

        int priority = calculatePriority(canonicalFrame);
        QPointer<TimelineCache> self(this);
        const std::shared_ptr<std::atomic<bool>> aliveToken = m_aliveToken;
        cacheTrace(QStringLiteral("TimelineCache::requestFrame.dispatch"),
                   QStringLiteral("clip=%1 frame=%2 normalized=%3 priority=%4")
                       .arg(clipId)
                       .arg(frameNumber)
                       .arg(canonicalFrame)
                       .arg(priority));
        
        const uint64_t seqId = m_decoder->requestFrame(info.decodePath, canonicalFrame, priority, 10000,
            DecodeRequestKind::Visible,
            [self, aliveToken, clipId, canonicalFrame, requestedAt, key](FrameHandle frame) {
                if (!aliveToken->load() || !self) {
                    return;
                }
                QMetaObject::invokeMethod(self, [self, aliveToken, clipId, canonicalFrame, requestedAt, key, frame]() {
                    if (!aliveToken->load() || !self) {
                        return;
                    }
                    FrameHandle deliveredFrame = frame;

                    int64_t latestVisibleTarget = canonicalFrame;
                    bool obsoleteVisibleCompletion = false;
                    bool obsoleteVisibleRequest = false;
                    {
                        QMutexLocker pendingLock(&self->m_pendingMutex);
                        latestVisibleTarget = self->m_latestVisibleTargets.value(clipId, canonicalFrame);
                        obsoleteVisibleRequest =
                            self->m_state.load() == PlaybackState::Playing &&
                            canonicalFrame + kObsoleteVisibleFrameSlack < latestVisibleTarget;
                        obsoleteVisibleCompletion =
                            obsoleteVisibleRequest &&
                            !deliveredFrame.isNull() &&
                            canonicalFrame + kObsoleteVisibleFrameSlack < latestVisibleTarget;
                    }

                    if (obsoleteVisibleCompletion) {
                        cacheTrace(QStringLiteral("TimelineCache::requestFrame.obsolete-complete"),
                                   QStringLiteral("clip=%1 frame=%2 latest=%3 waitMs=%4")
                                       .arg(clipId)
                                       .arg(canonicalFrame)
                                       .arg(latestVisibleTarget)
                                       .arg(cacheTraceMs() - requestedAt));
                        deliveredFrame = FrameHandle();
                    }

                    if (!deliveredFrame.isNull()) {
                        if (self->m_state.load() == PlaybackState::Playing) {
                            auto bufferIt = self->m_playbackBuffers.find(clipId);
                            if (bufferIt != self->m_playbackBuffers.end() && bufferIt.value()) {
                                bufferIt.value()->insert(canonicalFrame, deliveredFrame);
                            }
                        }
                        auto* cache = self->getOrCreateClipCache(clipId);
                        if (cache) {
                            cache->insert(canonicalFrame, deliveredFrame);
                        }
                    }

                    QVector<std::function<void(FrameHandle)>> callbacks;
                    {
                        QMutexLocker pendingLock(&self->m_pendingMutex);
                        auto it = self->m_pendingVisibleRequests.find(key);
                        if (it != self->m_pendingVisibleRequests.end()) {
                            callbacks = std::move(it->callbacks);
                            self->m_pendingVisibleRequests.erase(it);
                        }
                        if (canonicalFrame >= latestVisibleTarget) {
                            self->m_latestVisibleTargets.remove(clipId);
                        }
                    }
                    cacheTrace(QStringLiteral("TimelineCache::requestFrame.complete"),
                               QStringLiteral("clip=%1 frame=%2 null=%3 waitMs=%4")
                                   .arg(clipId)
                                   .arg(canonicalFrame)
                                   .arg(deliveredFrame.isNull())
                                   .arg(cacheTraceMs() - requestedAt));
                    const qint64 waitMs = cacheTraceMs() - requestedAt;
                    if (debugCacheWarnOnlyEnabled()) {
                        if (deliveredFrame.isNull() && obsoleteVisibleRequest) {
                            cacheWarnTrace(QStringLiteral("TimelineCache::visible-cancelled"),
                                           QStringLiteral("clip=%1 frame=%2 latest=%3 waitMs=%4 listeners=%5")
                                               .arg(clipId)
                                               .arg(canonicalFrame)
                                               .arg(latestVisibleTarget)
                                               .arg(waitMs)
                                               .arg(callbacks.size()));
                        } else if (deliveredFrame.isNull() || waitMs > 33) {
                            cacheWarnTrace(QStringLiteral("TimelineCache::visible-complete"),
                                           QStringLiteral("clip=%1 frame=%2 null=%3 waitMs=%4 listeners=%5")
                                               .arg(clipId)
                                               .arg(canonicalFrame)
                                               .arg(deliveredFrame.isNull())
                                               .arg(waitMs)
                                               .arg(callbacks.size()));
                        }
                    }
                    for (const auto& cb : callbacks) {
                        if (cb) {
                            cb(deliveredFrame);
                        }
                    }
                }, Qt::QueuedConnection);
            });
        if (seqId == 0 && debugCacheWarnOnlyEnabled()) {
            cacheWarnTrace(QStringLiteral("TimelineCache::visible-rejected"),
                           QStringLiteral("clip=%1 frame=%2 normalized=%3 priority=%4 pending=%5")
                               .arg(clipId)
                               .arg(frameNumber)
                               .arg(canonicalFrame)
                               .arg(priority)
                               .arg(m_decoder->pendingRequestCount()));
        }

        scheduleImmediateLeadPrefetch(info, canonicalFrame);
    }
}

FrameHandle TimelineCache::getCachedFrame(const QString& clipId, int64_t frameNumber) {
    frameNumber = normalizeFrameNumber(clipId, frameNumber);
    QMutexLocker lock(&m_clipsMutex);
    
    auto it = m_caches.find(clipId);
    if (it == m_caches.end()) {
        return FrameHandle();
    }
    
    return it.value()->get(frameNumber);
}

FrameHandle TimelineCache::getBestCachedFrame(const QString& clipId, int64_t frameNumber) {
    frameNumber = normalizeFrameNumber(clipId, frameNumber);
    QMutexLocker lock(&m_clipsMutex);

    auto it = m_caches.find(clipId);
    if (it == m_caches.end()) {
        return FrameHandle();
    }

    return it.value()->getBest(frameNumber);
}

FrameHandle TimelineCache::getLatestCachedFrame(const QString& clipId, int64_t frameNumber) {
    frameNumber = normalizeFrameNumber(clipId, frameNumber);
    QMutexLocker lock(&m_clipsMutex);

    auto it = m_caches.find(clipId);
    if (it == m_caches.end()) {
        return FrameHandle();
    }

    return it.value()->getLatestAtOrBefore(frameNumber);
}

FrameHandle TimelineCache::getPlaybackFrame(const QString& clipId, int64_t frameNumber) {
    frameNumber = normalizeFrameNumber(clipId, frameNumber);
    QMutexLocker lock(&m_clipsMutex);

    auto it = m_playbackBuffers.find(clipId);
    if (it == m_playbackBuffers.end()) {
        return FrameHandle();
    }

    return it.value()->get(frameNumber);
}

FrameHandle TimelineCache::getBestPlaybackFrame(const QString& clipId, int64_t frameNumber) {
    frameNumber = normalizeFrameNumber(clipId, frameNumber);
    QMutexLocker lock(&m_clipsMutex);

    auto it = m_playbackBuffers.find(clipId);
    if (it == m_playbackBuffers.end()) {
        return FrameHandle();
    }

    return it.value()->getBest(frameNumber);
}

FrameHandle TimelineCache::getLatestPlaybackFrame(const QString& clipId, int64_t frameNumber) {
    frameNumber = normalizeFrameNumber(clipId, frameNumber);
    QMutexLocker lock(&m_clipsMutex);

    auto it = m_playbackBuffers.find(clipId);
    if (it == m_playbackBuffers.end()) {
        return FrameHandle();
    }

    return it.value()->getLatestAtOrBefore(frameNumber);
}

bool TimelineCache::isFrameCached(const QString& clipId, int64_t frameNumber) const {
    frameNumber = normalizeFrameNumber(clipId, frameNumber);
    QMutexLocker lock(&m_clipsMutex);
    
    auto it = m_caches.find(clipId);
    if (it == m_caches.end()) {
        return false;
    }
    
    return it.value()->contains(frameNumber);
}

bool TimelineCache::isPlaybackFrameBuffered(const QString& clipId, int64_t frameNumber) const {
    frameNumber = normalizeFrameNumber(clipId, frameNumber);
    QMutexLocker lock(&m_clipsMutex);

    auto it = m_playbackBuffers.find(clipId);
    if (it == m_playbackBuffers.end()) {
        return false;
    }

    return it.value()->contains(frameNumber);
}

void TimelineCache::startPrefetching() {
    m_prefetchTimer.start();
}

void TimelineCache::stopPrefetching() {
    m_prefetchTimer.stop();
}

int TimelineCache::totalCachedFrames() const {
    QMutexLocker lock(&m_clipsMutex);
    
    int total = 0;
    for (const auto& cache : m_caches) {
        total += cache->size();
    }
    return total;
}

size_t TimelineCache::totalMemoryUsage() const {
    size_t total = 0;
    
    QMutexLocker lock(&m_clipsMutex);
    for (ClipCache* cache : m_caches) {
        total += cache->memoryUsage();
    }
    
    return total;
}

double TimelineCache::cacheHitRate() const {
    int req = m_requests.load();
    if (req == 0) return 0.0;
    return static_cast<double>(m_hits.load()) / req;
}

int TimelineCache::pendingVisibleRequestCount() const {
    QMutexLocker pendingLock(&m_pendingMutex);
    return m_pendingVisibleRequests.size();
}

bool TimelineCache::isVisibleRequestPending(const QString& clipId, int64_t frameNumber) const {
    frameNumber = normalizeFrameNumber(clipId, frameNumber);
    QMutexLocker pendingLock(&m_pendingMutex);
    return m_pendingVisibleRequests.contains(requestKey(clipId, frameNumber));
}

void TimelineCache::clearCache() {
    QMutexLocker lock(&m_clipsMutex);
    
    for (ClipCache* cache : m_caches) {
        delete cache;
    }
    m_caches.clear();
    for (PlaybackBuffer* buffer : m_playbackBuffers) {
        delete buffer;
    }
    m_playbackBuffers.clear();
}

void TimelineCache::trimCache() {
    const bool alreadyTrimming = m_trimInProgress.exchange(true);
    if (alreadyTrimming) {
        cacheTrace(QStringLiteral("TimelineCache::trimCache.skip"),
                   QStringLiteral("reason=reentrant"));
        return;
    }

    // Evict oldest frames globally
    const size_t targetMemory = m_budget ? static_cast<size_t>(m_budget->maxCpuMemory() * 0.7)
                                         : (192 * 1024 * 1024);
    evictOldestFrames(targetMemory);
    m_trimInProgress.store(false);
}

void TimelineCache::preloadRange(const QString& clipId, int64_t startFrame, int64_t endFrame) {
    QMutexLocker lock(&m_clipsMutex);
    
    auto it = m_clips.find(clipId);
    if (it == m_clips.end()) return;
    
    const ClipInfo& info = it.value();
    lock.unlock();
    
    // Request frames at interval (don't load every frame)
    const int kStep = 5;  // Load every 5th frame
    
    for (int64_t f = startFrame; f < endFrame; f += kStep) {
        const int64_t normalizedFrame = normalizeFrameNumber(info, f);
        if (isFrameCached(clipId, normalizedFrame)) continue;
        
        if (info.decodePath.isEmpty()) {
            continue;
        }
        m_decoder->requestFrame(info.decodePath, normalizedFrame, 5, 30000,
            DecodeRequestKind::Preload,
            [this, clipId, normalizedFrame](FrameHandle frame) {
                QMetaObject::invokeMethod(this, [this, clipId, normalizedFrame, frame]() {
                    if (!frame.isNull()) {
                        auto* cache = getOrCreateClipCache(clipId);
                        if (cache) {
                            cache->insert(normalizedFrame, frame);
                        }
                    }
                }, Qt::QueuedConnection);
            });

        if (info.isSingleFrame) {
            break;
        }
    }
}

void TimelineCache::onPrefetchTimer() {
    if (m_state.load() != PlaybackState::Playing) {
        return;
    }
    schedulePredictiveLoads();
}

void TimelineCache::onFrameDecoded(FrameHandle frame) {
    if (frame.isNull()) {
        return;
    }

    const QString sourcePath = frame.sourcePath();
    if (sourcePath.isEmpty()) {
        return;
    }

    QVector<QPair<QString, int64_t>> targets;
    {
        QMutexLocker lock(&m_clipsMutex);
        for (auto it = m_clips.cbegin(); it != m_clips.cend(); ++it) {
            const ClipInfo& info = it.value();
            if (info.isSingleFrame || info.decodePath != sourcePath) {
                continue;
            }
            targets.push_back(qMakePair(it.key(), normalizeFrameNumber(info, frame.frameNumber())));
        }
    }

    for (const auto& target : targets) {
        if (m_state.load() == PlaybackState::Playing) {
            QMutexLocker lock(&m_clipsMutex);
            auto it = m_playbackBuffers.find(target.first);
            if (it != m_playbackBuffers.end() && it.value()) {
                it.value()->insert(target.second, frame);
            }
        }

        ClipCache* cache = getOrCreateClipCache(target.first);
        if (cache) {
            cache->insert(target.second, frame);
        }
        emit frameLoaded(target.first, target.second, frame);
    }
}

void TimelineCache::onMemoryPressure() {
    trimCache();
}

void TimelineCache::dropStaleRequestsForPlayhead(int64_t playheadFrame) {
    QHash<QString, int64_t> activeLocalFrames;
    {
        QMutexLocker lock(&m_clipsMutex);
        for (auto it = m_clips.cbegin(); it != m_clips.cend(); ++it) {
            const ClipInfo& info = it.value();
            if (playheadFrame < info.clip.startFrame ||
                playheadFrame >= info.clip.startFrame + info.clip.durationFrames) {
                continue;
            }
            const int64_t activeSourceFrame =
                sourceFrameForClipAtTimelinePosition(info.clip,
                                                     static_cast<qreal>(playheadFrame),
                                                     m_renderSyncMarkers);
            activeLocalFrames.insert(it.key(), normalizeFrameNumber(info, activeSourceFrame));
        }
    }

    if (activeLocalFrames.isEmpty()) {
        return;
    }

    QVector<QVector<std::function<void(FrameHandle)>>> callbacksToCancel;
    {
        QMutexLocker pendingLock(&m_pendingMutex);
        for (auto it = m_pendingVisibleRequests.begin(); it != m_pendingVisibleRequests.end();) {
            const QString key = it.key();
            const int separator = key.indexOf(QLatin1Char(':'));
            if (separator <= 0) {
                ++it;
                continue;
            }

            const QString clipId = key.left(separator);
            const auto activeIt = activeLocalFrames.find(clipId);
            if (activeIt == activeLocalFrames.end()) {
                ++it;
                continue;
            }

            bool ok = false;
            const int64_t pendingFrame = key.mid(separator + 1).toLongLong(&ok);
            if (!ok || pendingFrame >= activeIt.value()) {
                ++it;
                continue;
            }

            callbacksToCancel.push_back(it->callbacks);
            it = m_pendingVisibleRequests.erase(it);
        }

        for (auto it = m_pendingPrefetchRequests.begin(); it != m_pendingPrefetchRequests.end();) {
            const QString key = *it;
            const int separator = key.indexOf(QLatin1Char(':'));
            if (separator <= 0) {
                ++it;
                continue;
            }

            const QString clipId = key.left(separator);
            const auto activeIt = activeLocalFrames.find(clipId);
            if (activeIt == activeLocalFrames.end()) {
                ++it;
                continue;
            }

            bool ok = false;
            const int64_t pendingFrame = key.mid(separator + 1).toLongLong(&ok);
            if (!ok || pendingFrame >= activeIt.value()) {
                ++it;
                continue;
            }

            it = m_pendingPrefetchRequests.erase(it);
            m_inflightPrefetches.fetch_sub(1);
        }
    }

    for (const auto& callbacks : callbacksToCancel) {
        for (const auto& cb : callbacks) {
            if (cb) {
                cb(FrameHandle());
            }
        }
    }

    // Do not mutate the decoder queue here. Stale requests are dropped at the
    // cache layer by removing their pending callbacks above; any already-queued
    // decode can finish harmlessly and will find no waiting visible listener.
}

void TimelineCache::scheduleImmediateLeadPrefetch(const ClipInfo& info, int64_t canonicalFrame) {
    const int leadPrefetchCount = debugLeadPrefetchEnabled() ? debugLeadPrefetchCount() : 0;
    if (!m_decoder || info.isSingleFrame || m_state.load() != PlaybackState::Playing || leadPrefetchCount <= 0) {
        return;
    }

    QPointer<TimelineCache> self(this);
    const std::shared_ptr<std::atomic<bool>> aliveToken = m_aliveToken;
    for (int offset = 1; offset <= leadPrefetchCount; ++offset) {
        const int64_t targetFrame = normalizeFrameNumber(info, canonicalFrame + offset);
        const QString key = requestKey(info.clip.id, targetFrame);

        {
            QMutexLocker pendingLock(&m_pendingMutex);
            if (m_pendingVisibleRequests.contains(key) || m_pendingPrefetchRequests.contains(key)) {
                continue;
            }
        }

        if (isFrameCached(info.clip.id, targetFrame)) {
            continue;
        }

        {
            QMutexLocker pendingLock(&m_pendingMutex);
            if (m_pendingVisibleRequests.contains(key) || m_pendingPrefetchRequests.contains(key)) {
                continue;
            }
            m_pendingPrefetchRequests.insert(key);
            m_inflightPrefetches.fetch_add(1);
        }

        const int priority = qMax(65, calculatePriority(canonicalFrame) - (offset * 5));
        cacheTrace(QStringLiteral("TimelineCache::lead-prefetch.dispatch"),
                   QStringLiteral("clip=%1 frame=%2 priority=%3")
                       .arg(info.clip.id)
                       .arg(targetFrame)
                       .arg(priority));

        if (info.decodePath.isEmpty()) {
            continue;
        }
        m_decoder->requestFrame(info.decodePath, targetFrame, priority, 5000,
            DecodeRequestKind::Prefetch,
            [self, aliveToken, clipId = info.clip.id, targetFrame, key](FrameHandle frame) {
                if (!aliveToken->load() || !self) {
                    return;
                }
                QMetaObject::invokeMethod(self, [self, aliveToken, clipId, targetFrame, key, frame]() {
                    if (!aliveToken->load() || !self) {
                        return;
                    }
                    {
                        QMutexLocker pendingLock(&self->m_pendingMutex);
                        self->m_pendingPrefetchRequests.remove(key);
                        self->m_inflightPrefetches.fetch_sub(1);
                    }
                    if (!frame.isNull()) {
                        if (self->m_state.load() == PlaybackState::Playing) {
                            QMutexLocker lock(&self->m_clipsMutex);
                            auto bufferIt = self->m_playbackBuffers.find(clipId);
                            if (bufferIt != self->m_playbackBuffers.end() && bufferIt.value()) {
                                bufferIt.value()->insert(targetFrame, frame);
                            }
                        }
                        ClipCache* cache = self->getOrCreateClipCache(clipId);
                        if (cache) {
                            cache->insert(targetFrame, frame);
                        }
                    }
                    cacheTrace(QStringLiteral("TimelineCache::lead-prefetch.complete"),
                               QStringLiteral("clip=%1 frame=%2 null=%3")
                                   .arg(clipId)
                                   .arg(targetFrame)
                                   .arg(frame.isNull()));
                }, Qt::QueuedConnection);
            });
    }
}

void TimelineCache::schedulePredictiveLoads() {
    const int maxPrefetchQueueDepth = debugPrefetchMaxQueueDepth();
    const int maxInflightPrefetch = debugPrefetchMaxInflight();
    const int maxPrefetchPerTick = debugPrefetchMaxPerTick();

    int64_t playhead = m_playhead.load();
    Direction dir = m_direction.load();
    double speed = m_speed.load();

    if (!m_decoder) {
        return;
    }

    const int pendingVisible = pendingVisibleRequestCount();
    if (pendingVisible > debugPrefetchSkipVisiblePendingThreshold()) {
        cacheTrace(QStringLiteral("TimelineCache::prefetch.skip"),
                   QStringLiteral("reason=visible-pending count=%1").arg(pendingVisible));
        return;
    }

    if (m_decoder->pendingRequestCount() >= maxPrefetchQueueDepth ||
        m_inflightPrefetches.load() >= maxInflightPrefetch) {
        cacheTrace(QStringLiteral("TimelineCache::prefetch.skip"),
                   QStringLiteral("reason=queue-pressure pending=%1 inflight=%2")
                       .arg(m_decoder->pendingRequestCount())
                       .arg(m_inflightPrefetches.load()));
        return;
    }
    
    // Spend more on forward visibility so image-sequence playback and speech-
    // filtered jumps have frames ready when the playhead arrives.
    int lookahead = qBound(6,
                           static_cast<int>(std::ceil(qMax(1.0, speed) * (pendingVisible > 0 ? 6.0 : 10.0))),
                           qMin(m_lookaheadFrames, pendingVisible > 0 ? 12 : 20));
    int scheduledThisTick = 0;

    QMutexLocker lock(&m_clipsMutex);

    // Get export ranges for speech filter awareness
    QVector<ExportRangeSegment> exportRanges;
    {
        QMutexLocker rangesLock(&m_exportRangesMutex);
        exportRanges = m_exportRanges;
    }

    const int step = dir == Direction::Forward ? 1 : -1;
    QVector<editor::SequencePrefetchClip> sequenceClips;
    sequenceClips.reserve(m_clips.size());
    for (auto it = m_clips.cbegin(); it != m_clips.cend(); ++it) {
        sequenceClips.push_back(editor::SequencePrefetchClip{it.value().clip, it.value().decodePath});
    }
    const QVector<int64_t> futureTimelineFrames =
        editor::collectLookaheadTimelineFrames(playhead, lookahead, step, exportRanges);
    for (int i = 0; i < futureTimelineFrames.size() && scheduledThisTick < maxPrefetchPerTick; ++i) {
        const int64_t currentTimelineFrame = futureTimelineFrames[i];
        QVector<ClipInfo> activeClips;
        activeClips.reserve(m_clips.size());
        for (auto it = m_clips.cbegin(); it != m_clips.cend(); ++it) {
            const ClipInfo& info = it.value();
            if (currentTimelineFrame < info.clip.startFrame ||
                currentTimelineFrame >= info.clip.startFrame + info.clip.durationFrames) {
                continue;
            }
            activeClips.push_back(info);
        }

        std::sort(activeClips.begin(), activeClips.end(),
                  [currentTimelineFrame](const ClipInfo& a, const ClipInfo& b) {
                      const bool aSequence = a.clip.sourceKind == MediaSourceKind::ImageSequence;
                      const bool bSequence = b.clip.sourceKind == MediaSourceKind::ImageSequence;
                      if (aSequence != bSequence) {
                          return aSequence;
                      }
                      const int64_t aDistance = qAbs(currentTimelineFrame - a.clip.startFrame);
                      const int64_t bDistance = qAbs(currentTimelineFrame - b.clip.startFrame);
                      if (aDistance == bDistance) {
                          return a.clip.id < b.clip.id;
                      }
                      return aDistance < bDistance;
                  });

        const QVector<editor::SequencePrefetchRequest> requests =
            editor::collectSequencePrefetchRequestsAtTimelineFrame(sequenceClips,
                                                                   static_cast<qreal>(currentTimelineFrame),
                                                                   m_renderSyncMarkers,
                                                                   false,
                                                                   qMax(20, 44 - ((i + 1) * 2)));
        QSet<QString> scheduledSequenceClipIds;

        for (const editor::SequencePrefetchRequest& prefetch : requests) {
            const QString& id = prefetch.clipId;
            const int64_t targetFrame = prefetch.sourceFrame;
            if (m_decoder->pendingRequestCount() >= maxPrefetchQueueDepth ||
                m_inflightPrefetches.load() >= maxInflightPrefetch) {
                return;
            }

            ClipCache* cache = m_caches.value(id);
            if (cache && cache->contains(targetFrame)) {
                continue;
            }

            const QString key = requestKey(id, targetFrame);
            {
                QMutexLocker pendingLock(&m_pendingMutex);
                if (m_pendingVisibleRequests.contains(key) || m_pendingPrefetchRequests.contains(key)) {
                    continue;
                }
                m_pendingPrefetchRequests.insert(key);
                m_inflightPrefetches.fetch_add(1);
            }
            scheduledSequenceClipIds.insert(id);

            lock.unlock();
            m_prefetches++;
            scheduledThisTick++;
            QPointer<TimelineCache> self(this);
            const std::shared_ptr<std::atomic<bool>> aliveToken = m_aliveToken;
            cacheTrace(QStringLiteral("TimelineCache::prefetch.dispatch"),
                       QStringLiteral("clip=%1 frame=%2 priority=%3")
                           .arg(id)
                           .arg(targetFrame)
                           .arg(prefetch.priority));

            if (prefetch.decodePath.isEmpty()) {
                continue;
            }
            m_decoder->requestFrame(prefetch.decodePath, targetFrame, prefetch.priority, 5000,
                DecodeRequestKind::Prefetch,
                [self, aliveToken, id, targetFrame, key](FrameHandle frame) {
                    if (!aliveToken->load() || !self) {
                        return;
                    }
                    QMetaObject::invokeMethod(self, [self, aliveToken, id, targetFrame, key, frame]() {
                        if (!aliveToken->load() || !self) {
                            return;
                        }
                        {
                            QMutexLocker pendingLock(&self->m_pendingMutex);
                            self->m_pendingPrefetchRequests.remove(key);
                            self->m_inflightPrefetches.fetch_sub(1);
                        }
                        if (!frame.isNull()) {
                            ClipCache* cache = self->getOrCreateClipCache(id);
                            if (cache) {
                                cache->insert(targetFrame, frame);
                            }
                        }
                        cacheTrace(QStringLiteral("TimelineCache::prefetch.complete"),
                                   QStringLiteral("clip=%1 frame=%2 null=%3")
                                       .arg(id)
                                       .arg(targetFrame)
                                       .arg(frame.isNull()));
                    }, Qt::QueuedConnection);
                });
            lock.relock();
            if (scheduledThisTick >= maxPrefetchPerTick) {
                break;
            }
        }

        for (const ClipInfo& info : activeClips) {
            if (scheduledThisTick >= maxPrefetchPerTick) {
                break;
            }
            if (info.clip.sourceKind == MediaSourceKind::ImageSequence ||
                scheduledSequenceClipIds.contains(info.clip.id)) {
                continue;
            }

            const QString& id = info.clip.id;
            const int64_t targetFrame =
                sourceFrameForClipAtTimelinePosition(info.clip,
                                                     static_cast<qreal>(currentTimelineFrame),
                                                     m_renderSyncMarkers);
            if (m_decoder->pendingRequestCount() >= maxPrefetchQueueDepth ||
                m_inflightPrefetches.load() >= maxInflightPrefetch) {
                return;
            }

            ClipCache* cache = m_caches.value(id);
            if (cache && cache->contains(targetFrame)) {
                continue;
            }

            const QString key = requestKey(id, targetFrame);
            {
                QMutexLocker pendingLock(&m_pendingMutex);
                if (m_pendingVisibleRequests.contains(key) || m_pendingPrefetchRequests.contains(key)) {
                    continue;
                }
                m_pendingPrefetchRequests.insert(key);
                m_inflightPrefetches.fetch_add(1);
            }

            const int priority = qMax(12, 30 - (i + 1));
            lock.unlock();
            m_prefetches++;
            scheduledThisTick++;
            QPointer<TimelineCache> self(this);
            const std::shared_ptr<std::atomic<bool>> aliveToken = m_aliveToken;
            cacheTrace(QStringLiteral("TimelineCache::prefetch.dispatch"),
                       QStringLiteral("clip=%1 frame=%2 priority=%3")
                           .arg(id)
                           .arg(targetFrame)
                           .arg(priority));

            if (info.decodePath.isEmpty()) {
                continue;
            }
            m_decoder->requestFrame(info.decodePath, targetFrame, priority, 5000,
                DecodeRequestKind::Prefetch,
                [self, aliveToken, id, targetFrame, key](FrameHandle frame) {
                    if (!aliveToken->load() || !self) {
                        return;
                    }
                    QMetaObject::invokeMethod(self, [self, aliveToken, id, targetFrame, key, frame]() {
                        if (!aliveToken->load() || !self) {
                            return;
                        }
                        {
                            QMutexLocker pendingLock(&self->m_pendingMutex);
                            self->m_pendingPrefetchRequests.remove(key);
                            self->m_inflightPrefetches.fetch_sub(1);
                        }
                        if (!frame.isNull()) {
                            ClipCache* cache = self->getOrCreateClipCache(id);
                            if (cache) {
                                cache->insert(targetFrame, frame);
                            }
                        }
                        cacheTrace(QStringLiteral("TimelineCache::prefetch.complete"),
                                   QStringLiteral("clip=%1 frame=%2 null=%3")
                                       .arg(id)
                                       .arg(targetFrame)
                                       .arg(frame.isNull()));
                    }, Qt::QueuedConnection);
                });
            lock.relock();
        }
    }
}

int TimelineCache::calculatePriority(int64_t frameNumber) const {
    int64_t playhead = m_playhead.load();
    int64_t delta = qAbs(frameNumber - playhead);
    
    // Current frame = highest priority
    if (delta == 0) return 100;
    if (delta <= 5) return 80;
    if (delta <= 15) return 60;
    if (delta <= 30) return 40;
    return 20;
}

QString TimelineCache::requestKey(const QString& clipId, int64_t frameNumber) const {
    return clipId + QLatin1Char(':') + QString::number(frameNumber);
}

int64_t TimelineCache::normalizeFrameNumber(const QString& clipId, int64_t frameNumber) const {
    QMutexLocker lock(&m_clipsMutex);
    const auto it = m_clips.find(clipId);
    if (it == m_clips.end()) {
        return frameNumber;
    }
    return normalizeFrameNumber(it.value(), frameNumber);
}

int64_t TimelineCache::normalizeFrameNumber(const ClipInfo& info, int64_t frameNumber) const {
    if (info.isSingleFrame) {
        return 0;
    }
    return frameNumber;
}

ClipCache* TimelineCache::getOrCreateClipCache(const QString& clipId) {
    QMutexLocker lock(&m_clipsMutex);
    
    auto it = m_caches.find(clipId);
    if (it != m_caches.end()) {
        return it.value();
    }
    
    auto clipIt = m_clips.find(clipId);
    if (clipIt == m_clips.end()) return nullptr;
    
    ClipCache* cache = new ClipCache(clipIt->decodePath.isEmpty() ? clipIt->clip.filePath : clipIt->decodePath,
                                     clipIt->clip.durationFrames,
                                     m_budget);
    m_caches[clipId] = cache;
    if (!m_playbackBuffers.contains(clipId)) {
        m_playbackBuffers[clipId] = new PlaybackBuffer();
    }
    
    return cache;
}

void TimelineCache::evictOldestFrames(size_t targetMemory) {
    QMutexLocker lock(&m_clipsMutex);
    
    struct FrameEntry {
        QString clipId;
        int64_t frameNumber;
        qint64 accessTime;
        size_t memory;
    };
    
    QList<FrameEntry> entries;
    size_t current = 0;
    
    for (auto it = m_caches.begin(); it != m_caches.end(); ++it) {
        const QString& clipId = it.key();
        ClipCache* cache = it.value();

        for (const CacheEntryInfo& info : cache->entries()) {
            entries.append({clipId, info.frameNumber, info.lastAccessTime, info.memoryUsage});
            current += info.memoryUsage;
        }
    }
    
    // Sort by access time (oldest first)
    std::sort(entries.begin(), entries.end(), 
        [](const FrameEntry& a, const FrameEntry& b) {
            return a.accessTime < b.accessTime;
        });
    
    // Evict until under budget
    for (const auto& entry : entries) {
        if (current <= targetMemory) break;
        
        auto it = m_caches.find(entry.clipId);
        if (it != m_caches.end()) {
            it.value()->remove(entry.frameNumber);
            current -= entry.memory;
            emit frameEvicted(entry.clipId, entry.frameNumber);
        }
    }
}

} // namespace editor
