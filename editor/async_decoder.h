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
#include <QThread>
#include <QWaitCondition>
#include <functional>
#include <atomic>
#include <deque>
#include <memory>

// FFmpeg forward declarations (actual includes in .cpp)
extern "C" {
struct AVCodecContext;
struct AVCodec;
struct AVFormatContext;
struct AVFrame;
struct AVPacket;
struct AVBufferRef;
struct AVCodecHWConfig;
// AVPixelFormat is an enum - we store it as int in the header
}

namespace editor {

// ============================================================================
// DecodeRequest - Single frame decode request
// ============================================================================
struct DecodeRequest {
    uint64_t sequenceId;
    QString filePath;
    int64_t frameNumber;
    int priority;  // Higher = more urgent (0-255)
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
    bool hasAudio = false;
    bool hasAlpha = false;
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
    bool m_reportedAlphaMismatch = false;
    bool m_isStillImage = false;
    QImage m_stillImage;
    
    // Seek state
    int64_t m_lastDecodedFrame = -1;
    bool m_eof = false;
};

// ============================================================================
// DecodeQueue - Thread-safe priority queue
// ============================================================================
class DecodeQueue {
public:
    static constexpr int kMaxSize = 128;
    
    bool enqueue(DecodeRequest req);
    bool dequeue(DecodeRequest* out);
    bool tryDequeue(DecodeRequest* out, int timeoutMs);
    
    void clear();
    void removeForFile(const QString& path);
    void removeForFileBefore(const QString& path, int64_t frameNumber);
    int size() const;
    bool isEmpty() const;
    
    void shutdown();
    bool isShutdown() const { return m_shutdown.load(); }
    
private:
    mutable QMutex m_mutex;
    QWaitCondition m_condition;
    std::deque<DecodeRequest> m_queue;
    std::atomic<bool> m_shutdown{false};
    
    // Insert by priority (higher priority = earlier in queue)
    void insertByPriority(const DecodeRequest& req);
};

// ============================================================================
// DecoderWorker - Runs on background thread, processes decode requests
// ============================================================================
class DecoderWorker : public QObject {
    Q_OBJECT
public:
    explicit DecoderWorker(DecodeQueue* queue, MemoryBudget* budget, QObject* parent = nullptr);
    ~DecoderWorker();
    
    void start();
    void stop();
    bool isRunning() const { return m_running.load(); }
    
    // Statistics
    int decodedFrameCount() const { return m_decodeCount.load(); }
    int droppedFrameCount() const { return m_dropCount.load(); }
    
signals:
    void frameDecoded(FrameHandle frame);
    void decodeError(QString path, QString error);
    
public slots:
    void run();
    
private:
    DecoderContext* getOrCreateContext(const QString& path);
    void evictOldestContext();
    void processRequest(const DecodeRequest& req);
    
    DecodeQueue* m_queue = nullptr;
    MemoryBudget* m_budget = nullptr;
    QThread* m_thread = nullptr;
    std::atomic<bool> m_running{false};
    std::atomic<int> m_decodeCount{0};
    std::atomic<int> m_dropCount{0};
    
    QMutex m_contextsMutex;
    QHash<QString, DecoderContext*> m_contexts;
    static constexpr int kMaxContexts = 4;  // Max concurrent files per worker
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
    
    // Cancel pending requests
    void cancelForFile(const QString& path);
    void cancelForFileBefore(const QString& path, int64_t frameNumber);
    void cancelAll();
    
    // Get video info (cached)
    VideoStreamInfo getVideoInfo(const QString& path);
    
    // Preload a file (warm up decoder)
    void preloadFile(const QString& path);
    
    // Statistics
    int pendingRequestCount() const { return m_queue.size(); }
    int workerCount() const { return m_workers.size(); }
    
    // Memory budget access
    MemoryBudget* memoryBudget() const { return m_budget; }
    
signals:
    void frameReady(FrameHandle frame);
    void error(QString path, QString message);
    void queuePressureChanged(int pendingCount);
    
private:
    void setupTrimCallback();
    void trimCaches();
    
    DecodeQueue m_queue;
    MemoryBudget* m_budget = nullptr;
    QVector<DecoderWorker*> m_workers;
    std::atomic<uint64_t> m_nextSequenceId{1};
    
    QMutex m_infoCacheMutex;
    QHash<QString, VideoStreamInfo> m_infoCache;
};

} // namespace editor
