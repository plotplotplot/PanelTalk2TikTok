#pragma once

#include "qt_compat.h"  // Qt 6.4/GCC 13 compatibility
#include "frame_handle.h"
#include "memory_budget.h"
#include "async_decoder.h"
#include "editor_shared.h"

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

struct PlaybackFrameInfo {
    FrameHandle frame;
    qint64 insertedAt = 0;
};

// ============================================================================
// ClipCache - Per-clip frame cache with LRU eviction
// ============================================================================
class ClipCache {
public:
    ClipCache(const QString& path, int64_t duration, MemoryBudget* budget);
    ~ClipCache();
    
    void insert(int64_t frameNumber, const FrameHandle& frame);
    FrameHandle get(int64_t frameNumber);
    FrameHandle getBest(int64_t frameNumber);
    FrameHandle getLatestAtOrBefore(int64_t frameNumber);
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
    struct FrameMemoryUse {
        size_t cpuBytes = 0;
        size_t gpuBytes = 0;
    };

    FrameMemoryUse frameMemoryUse(const FrameHandle& frame) const;

    QString m_path;
    int64_t m_duration;
    MemoryBudget* m_budget = nullptr;
    
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
    void setPlaybackState(PlaybackState state);
    void setDirection(Direction dir) { m_direction = dir; }
    void setPlaybackSpeed(double speed) { m_speed = speed; }
    
    // Export ranges for speech filter awareness (empty = all frames valid)
    void setExportRanges(const QVector<ExportRangeSegment>& ranges);
    void setRenderSyncMarkers(const QVector<RenderSyncMarker>& markers);
    
    // Playhead tracking
    void setPlayheadFrame(int64_t frame);
    int64_t playheadFrame() const { return m_playhead; }
    
    // Clip management
    void registerClip(const TimelineClip& clip);
    void registerClip(const QString& id, const QString& path, int64_t startFrame, int64_t duration);
    void unregisterClip(const QString& id);
    void clearClips();
    
    // Frame access (async)
    void requestFrame(const QString& clipId, int64_t frameNumber, 
                      std::function<void(FrameHandle)> callback);
    
    // Try to get frame from cache (synchronous)
    FrameHandle getCachedFrame(const QString& clipId, int64_t frameNumber);
    FrameHandle getBestCachedFrame(const QString& clipId, int64_t frameNumber);
    FrameHandle getLatestCachedFrame(const QString& clipId, int64_t frameNumber);
    FrameHandle getPlaybackFrame(const QString& clipId, int64_t frameNumber);
    FrameHandle getBestPlaybackFrame(const QString& clipId, int64_t frameNumber);
    FrameHandle getLatestPlaybackFrame(const QString& clipId, int64_t frameNumber);
    bool isFrameCached(const QString& clipId, int64_t frameNumber) const;
    bool isPlaybackFrameBuffered(const QString& clipId, int64_t frameNumber) const;
    
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
        TimelineClip clip;
        QString decodePath;
        bool isSingleFrame = false;
    };

    struct PendingVisibleRequest {
        QVector<std::function<void(FrameHandle)>> callbacks;
    };

    class PlaybackBuffer {
    public:
        void clear();
        void insert(int64_t frameNumber, const FrameHandle& frame);
        FrameHandle get(int64_t frameNumber);
        FrameHandle getBest(int64_t frameNumber);
        FrameHandle getLatestAtOrBefore(int64_t frameNumber);
        bool contains(int64_t frameNumber) const;

    private:
        void trimLocked();

        mutable QMutex m_mutex;
        QHash<int64_t, PlaybackFrameInfo> m_frames;
        static constexpr int kMaxFrames = 24;
    };

    QString requestKey(const QString& clipId, int64_t frameNumber) const;
    int64_t normalizeFrameNumber(const QString& clipId, int64_t frameNumber) const;
    int64_t normalizeFrameNumber(const ClipInfo& info, int64_t frameNumber) const;
    void dropStaleRequestsForPlayhead(int64_t playheadFrame);
    void scheduleImmediateLeadPrefetch(const ClipInfo& info, int64_t canonicalFrame);
    void schedulePredictiveLoads();
    int calculatePriority(int64_t frameNumber) const;
    ClipCache* getOrCreateClipCache(const QString& clipId);
    void evictOldestFrames(size_t targetMemory);
    
    AsyncDecoder* m_decoder = nullptr;
    MemoryBudget* m_budget = nullptr;
    
    mutable QMutex m_clipsMutex;
    QHash<QString, ClipInfo> m_clips;
    QHash<QString, ClipCache*> m_caches;
    QHash<QString, PlaybackBuffer*> m_playbackBuffers;
    
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
    QHash<QString, int64_t> m_latestVisibleTargets;
    
    // Export ranges for speech filter awareness
    mutable QMutex m_exportRangesMutex;
    QVector<ExportRangeSegment> m_exportRanges;
    QVector<RenderSyncMarker> m_renderSyncMarkers;
    std::atomic<int> m_inflightPrefetches{0};
    std::atomic<bool> m_trimInProgress{false};
    
    // Statistics
    std::atomic<int> m_requests{0};
    std::atomic<int> m_hits{0};
    std::atomic<int> m_prefetches{0};
    std::atomic<int> m_prefetchHits{0};
    std::shared_ptr<std::atomic<bool>> m_aliveToken = std::make_shared<std::atomic<bool>>(true);
};

} // namespace editor
