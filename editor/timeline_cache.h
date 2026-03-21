#pragma once

#include "qt_compat.h"  // Qt 6.4/GCC 13 compatibility
#include "frame_handle.h"
#include "memory_budget.h"
#include "async_decoder.h"

#include <QObject>
#include <QHash>
#include <QCache>
#include <QTimer>
#include <QMutex>
#include <QSet>
#include <QVector>
#include <memory>
#include <functional>

namespace editor {

// ============================================================================
// CachedFrame - Frame storage with metadata
// ============================================================================
struct CachedFrame {
    FrameHandle frame;
    qint64 lastAccessTime = 0;
    int accessCount = 0;
    int priority = 0;  // Last requested priority
};

struct CacheEntryInfo {
    int64_t frameNumber = 0;
    qint64 lastAccessTime = 0;
    size_t memoryUsage = 0;
};

// ============================================================================
// ClipCache - Per-clip frame cache with LRU eviction
// ============================================================================
class ClipCache {
public:
    explicit ClipCache(const QString& path, int64_t duration);
    
    void insert(int64_t frameNumber, const FrameHandle& frame);
    FrameHandle get(int64_t frameNumber);
    FrameHandle getBest(int64_t frameNumber);
    bool contains(int64_t frameNumber) const;
    void remove(int64_t frameNumber);
    
    int size() const { return m_frames.size(); }
    size_t memoryUsage() const { return m_memoryUsage; }
    
    // Evict oldest entries until under budget
    void evictToFit(size_t maxMemory);
    
    // Get all cached frame numbers (for debugging)
    QList<int64_t> cachedFrames() const;
    QList<CacheEntryInfo> entries() const;
    
    QString path() const { return m_path; }
    int64_t duration() const { return m_duration; }
    
private:
    QString m_path;
    int64_t m_duration;
    
    mutable QMutex m_mutex;
    QHash<int64_t, CachedFrame> m_frames;
    size_t m_memoryUsage = 0;
};

// ============================================================================
// TimelineCache - Multi-clip predictive frame caching
// 
// Manages frame caches for all timeline clips with:
// - Predictive loading based on playhead direction
// - Memory budget enforcement
// - Priority-based frame requests
// ============================================================================
class TimelineCache : public QObject {
    Q_OBJECT

public:
    enum class PlaybackState {
        Stopped,
        Playing,
        Scrubbing,
        Exporting
    };
    
    enum class Direction {
        Forward,
        Backward
    };

    explicit TimelineCache(AsyncDecoder* decoder, MemoryBudget* budget, QObject* parent = nullptr);
    ~TimelineCache();
    
    // Configuration
    void setMaxMemory(size_t bytes);
    void setLookaheadFrames(int frames) { m_lookaheadFrames = frames; }
    void setPlaybackState(PlaybackState state) { m_state = state; }
    void setDirection(Direction dir) { m_direction = dir; }
    void setPlaybackSpeed(double speed) { m_speed = speed; }
    
    // Playhead tracking
    void setPlayheadFrame(int64_t frame);
    int64_t playheadFrame() const { return m_playhead; }
    
    // Clip management
    void registerClip(const QString& id, const QString& path, int64_t startFrame, int64_t duration);
    void unregisterClip(const QString& id);
    void clearClips();
    
    // Frame access (async)
    void requestFrame(const QString& clipId, int64_t frameNumber, 
                      std::function<void(FrameHandle)> callback);
    
    // Try to get frame from cache (synchronous)
    FrameHandle getCachedFrame(const QString& clipId, int64_t frameNumber);
    FrameHandle getBestCachedFrame(const QString& clipId, int64_t frameNumber);
    bool isFrameCached(const QString& clipId, int64_t frameNumber) const;
    
    // Preload control
    void startPrefetching();
    void stopPrefetching();
    
    // Cache statistics
    int totalCachedFrames() const;
    size_t totalMemoryUsage() const;
    double cacheHitRate() const;
    int pendingVisibleRequestCount() const;
    bool isVisibleRequestPending(const QString& clipId, int64_t frameNumber) const;
    
    // Manual cache management
    void clearCache();
    void trimCache();
    void preloadRange(const QString& clipId, int64_t startFrame, int64_t endFrame);

signals:
    void frameLoaded(const QString& clipId, int64_t frameNumber, FrameHandle frame);
    void frameEvicted(const QString& clipId, int64_t frameNumber);
    void cachePressureChanged(double pressure);
    void prefetchProgress(int loaded, int total);

private slots:
    void onPrefetchTimer();
    void onFrameDecoded(FrameHandle frame);
    void onMemoryPressure();

private:
    struct ClipInfo {
        QString id;
        QString path;
        int64_t startFrame = 0;
        int64_t duration = 0;
        bool isSingleFrame = false;
    };

    struct PendingVisibleRequest {
        QVector<std::function<void(FrameHandle)>> callbacks;
    };

    QString requestKey(const QString& clipId, int64_t frameNumber) const;
    int64_t normalizeFrameNumber(const QString& clipId, int64_t frameNumber) const;
    int64_t normalizeFrameNumber(const ClipInfo& info, int64_t frameNumber) const;
    void dropStaleRequestsForPlayhead(int64_t playheadFrame);
    void schedulePredictiveLoads();
    int calculatePriority(int64_t frameNumber) const;
    ClipCache* getOrCreateClipCache(const QString& clipId);
    void evictOldestFrames(size_t targetMemory);
    
    AsyncDecoder* m_decoder = nullptr;
    MemoryBudget* m_budget = nullptr;
    
    mutable QMutex m_clipsMutex;
    QHash<QString, ClipInfo> m_clips;
    QHash<QString, ClipCache*> m_caches;
    
    // Playhead state
    std::atomic<int64_t> m_playhead{0};
    std::atomic<PlaybackState> m_state{PlaybackState::Stopped};
    std::atomic<Direction> m_direction{Direction::Forward};
    std::atomic<double> m_speed{1.0};
    
    // Prefetching
    QTimer m_prefetchTimer;
    int m_lookaheadFrames = 30;
    mutable QMutex m_pendingMutex;
    QHash<QString, PendingVisibleRequest> m_pendingVisibleRequests;
    QSet<QString> m_pendingPrefetchRequests;
    std::atomic<int> m_inflightPrefetches{0};
    
    // Statistics
    std::atomic<int> m_requests{0};
    std::atomic<int> m_hits{0};
    std::atomic<int> m_prefetches{0};
    std::atomic<int> m_prefetchHits{0};
    std::shared_ptr<std::atomic<bool>> m_aliveToken = std::make_shared<std::atomic<bool>>(true);
};

} // namespace editor
