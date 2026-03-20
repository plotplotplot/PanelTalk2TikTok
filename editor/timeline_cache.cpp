#include "timeline_cache.h"

#include <QDebug>
#include <QDateTime>
#include <QElapsedTimer>
#include <QHash>
#include <QPointer>
#include <algorithm>
#include <limits>

namespace editor {

namespace {
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
    static QHash<QString, qint64> lastLogByStage;
    const qint64 now = cacheTraceMs();
    if (stage.startsWith(QStringLiteral("TimelineCache::onPrefetchTimer")) ||
        stage.startsWith(QStringLiteral("TimelineCache::prefetch.skip")) ||
        stage.startsWith(QStringLiteral("TimelineCache::requestFrame.miss")) ||
        stage.startsWith(QStringLiteral("TimelineCache::requestFrame.dispatch"))) {
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
}

// ============================================================================
// ClipCache Implementation
// ============================================================================

ClipCache::ClipCache(const QString& path, int64_t duration)
    : m_path(path), m_duration(duration) {}

void ClipCache::insert(int64_t frameNumber, const FrameHandle& frame) {
    QMutexLocker lock(&m_mutex);
    
    // Remove old frame if exists
    auto it = m_frames.find(frameNumber);
    if (it != m_frames.end()) {
        m_memoryUsage -= it.value().frame.memoryUsage();
    }
    
    CachedFrame cf;
    cf.frame = frame;
    cf.lastAccessTime = QDateTime::currentMSecsSinceEpoch();
    cf.accessCount = 1;
    
    m_frames[frameNumber] = cf;
    m_memoryUsage += frame.memoryUsage();
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

bool ClipCache::contains(int64_t frameNumber) const {
    QMutexLocker lock(&m_mutex);
    return m_frames.contains(frameNumber);
}

void ClipCache::remove(int64_t frameNumber) {
    QMutexLocker lock(&m_mutex);
    
    auto it = m_frames.find(frameNumber);
    if (it != m_frames.end()) {
        m_memoryUsage -= it.value().frame.memoryUsage();
        m_frames.erase(it);
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
            m_memoryUsage -= it.value().frame.memoryUsage();
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
    m_prefetchTimer.setInterval(33);  // ~30fps
    connect(&m_prefetchTimer, &QTimer::timeout, this, &TimelineCache::onPrefetchTimer);

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
    if (m_budget && releasedMemory > 0) {
        m_budget->deallocateCpu(releasedMemory);
    }
}

void TimelineCache::setMaxMemory(size_t bytes) {
    if (m_budget) {
        m_budget->setMaxCpuMemory(bytes / 2);
        m_budget->setMaxGpuMemory(bytes / 2);
    }
}

void TimelineCache::setPlayheadFrame(int64_t frame) {
    m_playhead.store(frame);

    if (m_state.load() == PlaybackState::Playing) {
        dropStaleRequestsForPlayhead(frame);
    }
}

void TimelineCache::registerClip(const QString& id, const QString& path, 
                                 int64_t startFrame, int64_t duration) {
    QMutexLocker lock(&m_clipsMutex);
    
    ClipInfo info;
    info.id = id;
    info.path = path;
    info.startFrame = startFrame;
    info.duration = duration;
    
    m_clips[id] = info;
    m_caches[id] = new ClipCache(path, duration);
}

void TimelineCache::unregisterClip(const QString& id) {
    QMutexLocker lock(&m_clipsMutex);
    
    m_clips.remove(id);
    ClipCache* cache = m_caches.take(id);
    const size_t releasedMemory = cache ? cache->memoryUsage() : 0;
    delete cache;
    lock.unlock();
    if (m_budget && releasedMemory > 0) {
        m_budget->deallocateCpu(releasedMemory);
    }
}

void TimelineCache::clearClips() {
    QMutexLocker lock(&m_clipsMutex);
    
    m_clips.clear();
    size_t releasedMemory = 0;
    for (ClipCache* cache : m_caches) {
        releasedMemory += cache->memoryUsage();
        delete cache;
    }
    m_caches.clear();
    lock.unlock();
    if (m_budget && releasedMemory > 0) {
        m_budget->deallocateCpu(releasedMemory);
    }
}

void TimelineCache::requestFrame(const QString& clipId, int64_t frameNumber, 
                                 std::function<void(FrameHandle)> callback) {
    m_requests++;
    const qint64 requestedAt = cacheTraceMs();
    
    // Check cache first
    FrameHandle cached = getCachedFrame(clipId, frameNumber);
    if (!cached.isNull()) {
        m_hits++;
        cacheTrace(QStringLiteral("TimelineCache::requestFrame.hit"),
                   QStringLiteral("clip=%1 frame=%2").arg(clipId).arg(frameNumber));
        callback(cached);
        return;
    }
    cacheTrace(QStringLiteral("TimelineCache::requestFrame.miss"),
               QStringLiteral("clip=%1 frame=%2").arg(clipId).arg(frameNumber));

    const QString key = requestKey(clipId, frameNumber);
    {
        QMutexLocker pendingLock(&m_pendingMutex);
        auto existing = m_pendingVisibleRequests.find(key);
        if (existing != m_pendingVisibleRequests.end()) {
            cacheTrace(QStringLiteral("TimelineCache::requestFrame.dedup"),
                       QStringLiteral("clip=%1 frame=%2 listeners=%3")
                           .arg(clipId)
                           .arg(frameNumber)
                           .arg(existing->callbacks.size()));
            return;
        }
    }
    
    QMutexLocker lock(&m_clipsMutex);
    auto it = m_clips.find(clipId);
    if (it == m_clips.end()) {
        cacheTrace(QStringLiteral("TimelineCache::requestFrame.missing-clip"),
                   QStringLiteral("clip=%1 frame=%2").arg(clipId).arg(frameNumber));
        callback(FrameHandle());
        return;
    }
    
    const ClipInfo& info = it.value();
    lock.unlock();
    
    // Request from decoder
    if (m_decoder) {
        int priority = calculatePriority(frameNumber);
        QPointer<TimelineCache> self(this);
        const std::shared_ptr<std::atomic<bool>> aliveToken = m_aliveToken;
        cacheTrace(QStringLiteral("TimelineCache::requestFrame.dispatch"),
                   QStringLiteral("clip=%1 frame=%2 priority=%3")
                       .arg(clipId)
                       .arg(frameNumber)
                       .arg(priority));

        {
            QMutexLocker pendingLock(&m_pendingMutex);
            PendingVisibleRequest pending;
            pending.callbacks.push_back(std::move(callback));
            m_pendingVisibleRequests.insert(key, std::move(pending));
            m_pendingPrefetchRequests.remove(key);
        }
        
        m_decoder->requestFrame(info.path, frameNumber, priority, 10000,
            [self, aliveToken, clipId, frameNumber, requestedAt, key](FrameHandle frame) {
                if (!aliveToken->load() || !self) {
                    return;
                }
                QMetaObject::invokeMethod(self, [self, aliveToken, clipId, frameNumber, requestedAt, key, frame]() {
                    if (!aliveToken->load() || !self) {
                        return;
                    }
                    if (!frame.isNull()) {
                        auto* cache = self->getOrCreateClipCache(clipId);
                        if (cache) {
                            cache->insert(frameNumber, frame);
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
                    }
                    cacheTrace(QStringLiteral("TimelineCache::requestFrame.complete"),
                               QStringLiteral("clip=%1 frame=%2 null=%3 waitMs=%4")
                                   .arg(clipId)
                                   .arg(frameNumber)
                                   .arg(frame.isNull())
                                   .arg(cacheTraceMs() - requestedAt));
                    for (const auto& cb : callbacks) {
                        if (cb) {
                            cb(frame);
                        }
                    }
                }, Qt::QueuedConnection);
            });
    }
}

FrameHandle TimelineCache::getCachedFrame(const QString& clipId, int64_t frameNumber) {
    QMutexLocker lock(&m_clipsMutex);
    
    auto it = m_caches.find(clipId);
    if (it == m_caches.end()) {
        return FrameHandle();
    }
    
    return it.value()->get(frameNumber);
}

FrameHandle TimelineCache::getBestCachedFrame(const QString& clipId, int64_t frameNumber) {
    QMutexLocker lock(&m_clipsMutex);

    auto it = m_caches.find(clipId);
    if (it == m_caches.end()) {
        return FrameHandle();
    }

    return it.value()->getBest(frameNumber);
}

bool TimelineCache::isFrameCached(const QString& clipId, int64_t frameNumber) const {
    QMutexLocker lock(&m_clipsMutex);
    
    auto it = m_caches.find(clipId);
    if (it == m_caches.end()) {
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
    QMutexLocker pendingLock(&m_pendingMutex);
    return m_pendingVisibleRequests.contains(requestKey(clipId, frameNumber));
}

void TimelineCache::clearCache() {
    QMutexLocker lock(&m_clipsMutex);
    
    size_t releasedMemory = 0;
    for (ClipCache* cache : m_caches) {
        releasedMemory += cache->memoryUsage();
        delete cache;
    }
    m_caches.clear();
    lock.unlock();
    if (m_budget && releasedMemory > 0) {
        m_budget->deallocateCpu(releasedMemory);
    }
}

void TimelineCache::trimCache() {
    // Evict oldest frames globally
    evictOldestFrames(m_budget ? m_budget->maxCpuMemory() / 2 : 256 * 1024 * 1024);
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
        if (isFrameCached(clipId, f)) continue;
        
        m_decoder->requestFrame(info.path, f, 5, 30000, 
            [this, clipId, f](FrameHandle frame) {
                QMetaObject::invokeMethod(this, [this, clipId, f, frame]() {
                    if (!frame.isNull()) {
                        auto* cache = getOrCreateClipCache(clipId);
                        if (cache) {
                            cache->insert(f, frame);
                        }
                    }
                }, Qt::QueuedConnection);
            });
    }
}

void TimelineCache::onPrefetchTimer() {
    if (m_state.load() != PlaybackState::Playing) return;
}

void TimelineCache::onFrameDecoded(FrameHandle frame) {
    // Handled in individual callbacks
    Q_UNUSED(frame)
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
            if (playheadFrame < info.startFrame || playheadFrame >= info.startFrame + info.duration) {
                continue;
            }
            activeLocalFrames.insert(it.key(), playheadFrame - info.startFrame);
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

void TimelineCache::schedulePredictiveLoads() {
    static constexpr int kMaxPrefetchQueueDepth = 8;
    static constexpr int kMaxInflightPrefetch = 4;
    static constexpr int kMaxPrefetchPerTick = 2;

    int64_t playhead = m_playhead.load();
    Direction dir = m_direction.load();
    double speed = m_speed.load();

    if (!m_decoder) {
        return;
    }

    {
        QMutexLocker pendingLock(&m_pendingMutex);
        if (!m_pendingVisibleRequests.isEmpty()) {
            cacheTrace(QStringLiteral("TimelineCache::prefetch.skip"),
                       QStringLiteral("reason=visible-pending count=%1")
                           .arg(m_pendingVisibleRequests.size()));
            return;
        }
    }

    if (m_decoder->pendingRequestCount() >= kMaxPrefetchQueueDepth ||
        m_inflightPrefetches.load() >= kMaxInflightPrefetch) {
        cacheTrace(QStringLiteral("TimelineCache::prefetch.skip"),
                   QStringLiteral("reason=queue-pressure pending=%1 inflight=%2")
                       .arg(m_decoder->pendingRequestCount())
                       .arg(m_inflightPrefetches.load()));
        return;
    }
    
    // Calculate lookahead based on speed
    int lookahead = static_cast<int>(m_lookaheadFrames * qMax(1.0, speed));
    int scheduledThisTick = 0;
    
    QMutexLocker lock(&m_clipsMutex);
    
    for (auto it = m_clips.begin(); it != m_clips.end(); ++it) {
        const QString& id = it.key();
        const ClipInfo& info = it.value();
        
        // Check if clip is near playhead
        if (playhead < info.startFrame - lookahead || 
            playhead > info.startFrame + info.duration + lookahead) {
            continue;  // Clip not visible
        }
        
        int64_t localFrame = playhead - info.startFrame;
        if (localFrame < 0 || localFrame >= info.duration) continue;
        
        // Schedule loads for upcoming frames
        int step = dir == Direction::Forward ? 1 : -1;
        for (int i = 1; i <= lookahead; ++i) {
            int64_t targetFrame = localFrame + (i * step);
            if (targetFrame < 0 || targetFrame >= info.duration) continue;
            if (scheduledThisTick >= kMaxPrefetchPerTick) return;
            if (m_decoder->pendingRequestCount() >= kMaxPrefetchQueueDepth ||
                m_inflightPrefetches.load() >= kMaxInflightPrefetch) {
                return;
            }
            
            // Skip if already cached
            ClipCache* cache = m_caches.value(id);
            if (cache && cache->contains(targetFrame)) continue;

            const QString key = requestKey(id, targetFrame);
            {
                QMutexLocker pendingLock(&m_pendingMutex);
                if (m_pendingVisibleRequests.contains(key) || m_pendingPrefetchRequests.contains(key)) {
                    continue;
                }
                m_pendingPrefetchRequests.insert(key);
                m_inflightPrefetches.fetch_add(1);
            }
            
            // Request with lower priority
            int priority = 20 - i;  // Decreasing priority
            
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
            
            m_decoder->requestFrame(info.path, targetFrame, priority, 5000,
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

ClipCache* TimelineCache::getOrCreateClipCache(const QString& clipId) {
    QMutexLocker lock(&m_clipsMutex);
    
    auto it = m_caches.find(clipId);
    if (it != m_caches.end()) {
        return it.value();
    }
    
    auto clipIt = m_clips.find(clipId);
    if (clipIt == m_clips.end()) return nullptr;
    
    ClipCache* cache = new ClipCache(clipIt->path, clipIt->duration);
    m_caches[clipId] = cache;
    
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
            if (m_budget && entry.memory > 0) {
                m_budget->deallocateCpu(entry.memory);
            }
            emit frameEvicted(entry.clipId, entry.frameNumber);
        }
    }
}

} // namespace editor
