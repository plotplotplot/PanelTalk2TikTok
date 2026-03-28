#pragma once

#include "qt_compat.h"  // Qt 6.4/GCC 13 compatibility
#include "frame_handle.h"
#include "memory_budget.h"

#include <QDateTime>
#include <QDeadlineTimer>
#include <QHash>
#include <QImage>
#include <QMutex>
#include <QObject>
#include <QVector>
#include <functional>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

// FFmpeg forward declarations (actual includes in .cpp)
extern "C" {
struct AVCodecContext;
struct AVCodec;
struct AVFormatContext;
struct AVFrame;
struct AVPacket;
struct AVBufferRef;
struct AVCodecHWConfig;
struct SwsContext;
// AVPixelFormat is an enum - we store it as int in the header
}

namespace editor {

enum class DecodeRequestKind : int {
    Visible = 0,
    Prefetch = 1,
    Preload = 2,
};

// ============================================================================
// DecodeRequest - Single frame decode request
// ============================================================================
struct DecodeRequest {
    uint64_t sequenceId;
    QString filePath;
    int64_t frameNumber;
    int priority;  // Higher = more urgent (0-255)
    DecodeRequestKind kind = DecodeRequestKind::Visible;
    uint64_t generation = 0;
    QDeadlineTimer deadline;
    std::function<void(FrameHandle)> callback;
    qint64 submittedAt;
    
    bool isExpired() const { return deadline.hasExpired(); }
    int64_t ageMs() const { return QDateTime::currentMSecsSinceEpoch() - submittedAt; }
};

// ============================================================================
// VideoStreamInfo - Metadata about a video file
// ============================================================================
struct VideoStreamInfo {
    QString path;
    int64_t durationFrames = 0;
    double fps = 30.0;
    QSize frameSize;
    int64_t bitrate = 0;
    QString codecName;
    QString decodePath;
    QString interopPath;
    QString requestedDecodeMode;
    bool hasAudio = false;
    bool hasAlpha = false;
    bool hardwareAccelerated = false;
    bool isValid = false;
};

// ============================================================================
// DecoderContext - Per-file decoder state (lives on decoder thread)
// ============================================================================
class DecoderContext {
public:
    explicit DecoderContext(const QString& path);
    ~DecoderContext();
    
    // Initialize the decoder (open file, find streams, init codec)
    bool initialize();
    
    // Get stream info
    VideoStreamInfo info() const { return m_info; }
    
    // Decode specific frame (blocking)
    FrameHandle decodeFrame(int64_t frameNumber);
    QVector<FrameHandle> decodeThroughFrame(int64_t targetFrame);
    bool supportsSequenceBatchDecode() const { return m_isImageSequence && m_sequenceUsesWebp; }
    
    // Precise seek then decode
    FrameHandle seekAndDecode(int64_t frameNumber);
    
    // Check if hardware accelerated
    bool isHardwareAccelerated() const { return m_hwDeviceCtx != nullptr; }
    
    // Close and cleanup
    void shutdown();
    
    // Last access time for LRU eviction
    qint64 lastAccessTime() const { return m_lastAccessTime; }
    void updateAccessTime();
    
private:
    bool openInput();
    bool initCodec();
    bool initHardwareAccel(const AVCodec* decoder);
    bool seekToKeyframe(int64_t targetFrame);
    bool loadStillImage();
    bool loadImageSequence();
    QImage loadCachedSequenceFrameImage(int64_t frameNumber);
    void cacheSequenceFrameImage(int64_t frameNumber, const QImage& image);
    void trimSequenceFrameCache();
    QVector<FrameHandle> decodeForwardUntil(int64_t targetFrame, bool forceSeek);
    FrameHandle convertToFrame(AVFrame* avFrame, int64_t frameNumber);
    QImage convertAVFrameToImage(AVFrame* frame);
    
    QString m_path;
    VideoStreamInfo m_info;
    qint64 m_lastAccessTime = 0;
    
    // FFmpeg contexts
    AVFormatContext* m_formatCtx = nullptr;
    AVCodecContext* m_codecCtx = nullptr;
    int m_videoStreamIndex = -1;
    
    // Hardware acceleration
    AVBufferRef* m_hwDeviceCtx = nullptr;
    int m_hwPixFmt = -1;  // AV_PIX_FMT_NONE stored as int
    int m_swPixFmt = -1;  // AV_PIX_FMT_NONE stored as int
    bool m_streamHasAlphaTag = false;
    bool m_loggedSourceFormat = false;
    bool m_loggedAlphaProbe = false;
    bool m_reportedAlphaMismatch = false;
    bool m_isStillImage = false;
    bool m_isImageSequence = false;
    bool m_sequenceUsesWebp = false;
    QImage m_stillImage;
    QStringList m_sequenceFramePaths;
    QHash<int64_t, QImage> m_sequenceFrameCache;
    QHash<int64_t, quint64> m_sequenceFrameCacheUseOrder;
    size_t m_sequenceFrameCacheBytes = 0;
    quint64 m_sequenceFrameUseCounter = 0;
    SwsContext* m_swsCtx = nullptr;
    AVFrame* m_convertFrame = nullptr;
    int m_swsSourceFormat = -1;
    QSize m_swsSourceSize;
    QSize m_convertFrameSize;
    
    // Seek state
    int64_t m_lastDecodedFrame = -1;
    bool m_eof = false;
};

// ============================================================================
// AsyncDecoder - Main API for async frame decoding
// ============================================================================
class AsyncDecoder : public QObject {
    Q_OBJECT
public:
    explicit AsyncDecoder(QObject* parent = nullptr);
    ~AsyncDecoder();
    
    bool initialize();
    void shutdown();
    
    // Request a frame decode (non-blocking)
    // callback is invoked on the decoder thread - use QMetaObject::invokeMethod 
    // to get back to main thread if needed
    uint64_t requestFrame(const QString& path,
                          int64_t frameNumber,
                          int priority,
                          int timeoutMs,
                          std::function<void(FrameHandle)> callback);
    uint64_t requestFrame(const QString& path, 
                          int64_t frameNumber,
                          int priority,
                          int timeoutMs,
                          DecodeRequestKind kind,
                          std::function<void(FrameHandle)> callback);
    
    // Cancel pending requests
    void cancelForFile(const QString& path);
    void cancelForFileBefore(const QString& path, int64_t frameNumber);
    void cancelAll();
    
    // Get video info (cached)
    VideoStreamInfo getVideoInfo(const QString& path);
    
    // Preload a file (warm up decoder)
    void preloadFile(const QString& path);
    
    // Statistics
    int pendingRequestCount() const { return totalPendingRequests(); }
    int workerCount() const { return m_workerCount; }
    
    // Memory budget access
    MemoryBudget* memoryBudget() const { return m_budget; }
    
signals:
    void frameReady(FrameHandle frame);
    void error(QString path, QString message);
    void queuePressureChanged(int pendingCount);
    
private:
    struct LaneState;

    void setupTrimCallback();
    void trimCaches();
    int totalPendingRequests() const;
    int laneIndexForRequest(const QString& path, int64_t frameNumber) const;
    LaneState* laneForRequest(const QString& path, int64_t frameNumber) const;
    std::vector<LaneState*> lanesForPath(const QString& path) const;
    void startLane(LaneState* lane);
    void stopLane(LaneState* lane);
    void runLane(LaneState* lane);
    void insertByPriority(std::deque<DecodeRequest>& queue, const DecodeRequest& req);
    void collectSupersededRequests(const DecodeRequest& req,
                                   std::deque<DecodeRequest>& queue,
                                   QVector<std::function<void(FrameHandle)>>* droppedCallbacks);
    
    MemoryBudget* m_budget = nullptr;
    int m_workerCount = 0;
    std::vector<std::unique_ptr<LaneState>> m_lanes;
    bool m_initialized = false;
    bool m_shuttingDown = false;
    std::atomic<uint64_t> m_nextSequenceId{1};
    
    QMutex m_infoCacheMutex;
    QHash<QString, VideoStreamInfo> m_infoCache;
};

} // namespace editor
