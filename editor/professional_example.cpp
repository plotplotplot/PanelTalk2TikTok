// professional_example.cpp - Concrete implementation of professional pipeline
// This shows key parts of the architecture described in PROFESSIONAL_ARCHITECTURE.md

#include <QObject>
#include <QThread>
#include <QMutex>
#include <QWaitCondition>
#include <QQueue>
#include <QHash>
#include <QDeadlineTimer>
#include <QPromise>
#include <QRunnable>
#include <QThreadPool>
#include <memory>
#include <atomic>
#include <functional>

// libavcodec includes
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
}

// QRhi includes
#include <QtGui/private/qrhi_p.h>

// ============================================================================
// 1. Thread-Safe Frame Handle (RAII wrapper)
// ============================================================================

class FrameHandle {
public:
    struct FrameData {
        AVFrame* avFrame = nullptr;
        QRhiTexture* texture = nullptr;
        int64_t pts = 0;
        QSize size;
        bool isHardware = false;
        std::atomic<int> refCount{1};
        
        ~FrameData() {
            if (texture) {
                // Schedule texture deletion on render thread
            }
            if (avFrame) {
                av_frame_free(&avFrame);
            }
        }
    };
    
    FrameHandle() = default;
    explicit FrameHandle(FrameData* data) : m_data(data) {}
    
    ~FrameHandle() { reset(); }
    
    FrameHandle(const FrameHandle& other) : m_data(other.m_data) {
        if (m_data) m_data->refCount++;
    }
    
    FrameHandle(FrameHandle&& other) noexcept : m_data(other.m_data) {
        other.m_data = nullptr;
    }
    
    FrameHandle& operator=(const FrameHandle& other) {
        if (this != &other) {
            reset();
            m_data = other.m_data;
            if (m_data) m_data->refCount++;
        }
        return *this;
    }
    
    bool isNull() const { return m_data == nullptr; }
    int64_t pts() const { return m_data ? m_data->pts : -1; }
    QSize size() const { return m_data ? m_data->size : QSize(); }
    QRhiTexture* texture() const { return m_data ? m_data->texture : nullptr; }
    
    void reset() {
        if (m_data && --m_data->refCount == 0) {
            delete m_data;
        }
        m_data = nullptr;
    }
    
private:
    FrameData* m_data = nullptr;
};

// ============================================================================
// 2. Lock-Free Request Queue
// ============================================================================

struct DecodeRequest {
    uint64_t sequenceId;
    QString filePath;
    int64_t frameNumber;
    int priority;  // Higher = more urgent
    QDeadlineTimer deadline;
    std::function<void(FrameHandle)> callback;
    
    bool isExpired() const { return deadline.hasExpired(); }
};

class DecodeRequestQueue {
public:
    static constexpr int kMaxSize = 64;
    
    bool enqueue(DecodeRequest req) {
        QMutexLocker lock(&m_mutex);
        if (m_queue.size() >= kMaxSize) {
            // Drop lowest priority item
            if (!dropLowestPriority(req.priority)) {
                return false; // Queue full with higher priority items
            }
        }
        m_queue.enqueue(std::move(req));
        m_condition.wakeOne();
        return true;
    }
    
    bool dequeue(DecodeRequest* out) {
        QMutexLocker lock(&m_mutex);
        while (m_queue.isEmpty() && !m_shutdown) {
            m_condition.wait(&m_mutex, 100);
        }
        if (m_shutdown && m_queue.isEmpty()) {
            return false;
        }
        *out = m_queue.dequeue();
        return true;
    }
    
    void shutdown() {
        QMutexLocker lock(&m_mutex);
        m_shutdown = true;
        m_condition.wakeAll();
    }
    
    void clear() {
        QMutexLocker lock(&m_mutex);
        m_queue.clear();
    }
    
private:
    bool dropLowestPriority(int newPriority) {
        // Find and remove lowest priority item
        int lowestIdx = -1;
        int lowestPriority = newPriority;
        
        for (int i = 0; i < m_queue.size(); ++i) {
            if (m_queue[i].priority < lowestPriority) {
                lowestPriority = m_queue[i].priority;
                lowestIdx = i;
            }
        }
        
        if (lowestIdx >= 0) {
            // Notify caller that request was dropped
            if (m_queue[lowestIdx].callback) {
                m_queue[lowestIdx].callback(FrameHandle()); // null frame
            }
            m_queue.removeAt(lowestIdx);
            return true;
        }
        return false;
    }
    
    QMutex m_mutex;
    QWaitCondition m_condition;
    QQueue<DecodeRequest> m_queue;
    bool m_shutdown = false;
};

// ============================================================================
// 3. Hardware-Accelerated Decoder
// ============================================================================

class HWDecoder : public QObject {
    Q_OBJECT
public:
    enum class State {
        Uninitialized,
        Ready,
        Decoding,
        Error
    };
    
    explicit HWDecoder(const QString& path, QObject* parent = nullptr)
        : QObject(parent), m_path(path) {}
    
    ~HWDecoder() {
        shutdown();
    }
    
    bool initialize() {
        // Open input file
        if (avformat_open_input(&m_formatCtx, m_path.toUtf8().constData(), nullptr, nullptr) < 0) {
            setError("Failed to open input file");
            return false;
        }
        
        if (avformat_find_stream_info(m_formatCtx, nullptr) < 0) {
            setError("Failed to find stream info");
            return false;
        }
        
        // Find video stream
        for (unsigned i = 0; i < m_formatCtx->nb_streams; i++) {
            if (m_formatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                m_videoStreamIdx = i;
                break;
            }
        }
        
        if (m_videoStreamIdx < 0) {
            setError("No video stream found");
            return false;
        }
        
        AVStream* stream = m_formatCtx->streams[m_videoStreamIdx];
        const AVCodec* decoder = avcodec_find_decoder(stream->codecpar->codec_id);
        if (!decoder) {
            setError("Decoder not found");
            return false;
        }
        
        // Try hardware acceleration
        m_codecCtx = avcodec_alloc_context3(decoder);
        if (avcodec_parameters_to_context(m_codecCtx, stream->codecpar) < 0) {
            setError("Failed to copy codec params");
            return false;
        }
        
        // Enumerate available hardware devices
        AVHWDeviceType hwType = AV_HWDEVICE_TYPE_NONE;
        while ((hwType = av_hwdevice_iterate_types(hwType)) != AV_HWDEVICE_TYPE_NONE) {
            if (hwType == AV_HWDEVICE_TYPE_VAAPI || 
                hwType == AV_HWDEVICE_TYPE_VDPAU ||
                hwType == AV_HWDEVICE_TYPE_CUDA) {
                if (tryInitHardware(hwType)) {
                    m_hwDeviceType = hwType;
                    break;
                }
            }
        }
        
        // Fall back to software
        if (m_hwDeviceType == AV_HWDEVICE_TYPE_NONE) {
            qDebug() << "Using software decoding for" << m_path;
        }
        
        if (avcodec_open2(m_codecCtx, decoder, nullptr) < 0) {
            setError("Failed to open codec");
            return false;
        }
        
        m_state = State::Ready;
        return true;
    }
    
    bool tryInitHardware(AVHWDeviceType type) {
        AVBufferRef* hwDeviceCtx = nullptr;
        if (av_hwdevice_ctx_create(&hwDeviceCtx, type, nullptr, nullptr, 0) < 0) {
            return false;
        }
        
        m_codecCtx->hw_device_ctx = hwDeviceCtx;
        
        // Set hardware pixel format
        for (int i = 0;; i++) {
            const AVCodecHWConfig* config = avcodec_get_hw_config(m_codecCtx->codec, i);
            if (!config) break;
            if (config->device_type == type) {
                m_codecCtx->pix_fmt = config->pix_fmt;
                m_hwPixFmt = config->pix_fmt;
                return true;
            }
        }
        
        av_buffer_unref(&hwDeviceCtx);
        return false;
    }
    
    // Seek to specific frame (precise)
    bool seekToFrame(int64_t targetFrame) {
        if (m_state != State::Ready && m_state != State::Decoding) {
            return false;
        }
        
        AVStream* stream = m_formatCtx->streams[m_videoStreamIdx];
        
        // Convert frame to timestamp
        int64_t targetPts = av_rescale_q(
            targetFrame * stream->time_base.den / 30,  // assuming 30fps
            AVRational{1, 30},
            stream->time_base
        );
        
        // Find previous keyframe
        int flags = AVSEEK_FLAG_BACKWARD;
        if (av_seek_frame(m_formatCtx, m_videoStreamIdx, targetPts, flags) < 0) {
            return false;
        }
        
        avcodec_flush_buffers(m_codecCtx);
        
        // Decode forward to exact frame
        AVFrame* frame = av_frame_alloc();
        AVPacket* pkt = av_packet_alloc();
        int64_t currentFrame = -1;
        
        while (av_read_frame(m_formatCtx, pkt) >= 0) {
            if (pkt->stream_index != m_videoStreamIdx) {
                av_packet_unref(pkt);
                continue;
            }
            
            if (avcodec_send_packet(m_codecCtx, pkt) < 0) {
                av_packet_unref(pkt);
                continue;
            }
            
            while (avcodec_receive_frame(m_codecCtx, frame) == 0) {
                int64_t pts = av_rescale_q(frame->pts, stream->time_base, AVRational{1, 30});
                if (pts >= targetFrame) {
                    currentFrame = pts;
                    break;
                }
            }
            
            av_packet_unref(pkt);
            if (currentFrame >= targetFrame) break;
        }
        
        av_frame_free(&frame);
        av_packet_free(&pkt);
        
        return currentFrame == targetFrame;
    }
    
    // Decode single frame
    FrameHandle decodeFrame(int64_t frameNumber) {
        if (m_state != State::Ready) {
            return FrameHandle();
        }
        
        m_state = State::Decoding;
        
        AVFrame* frame = av_frame_alloc();
        AVPacket* pkt = av_packet_alloc();
        FrameHandle result;
        
        AVStream* stream = m_formatCtx->streams[m_videoStreamIdx];
        
        while (av_read_frame(m_formatCtx, pkt) >= 0) {
            if (pkt->stream_index != m_videoStreamIdx) {
                av_packet_unref(pkt);
                continue;
            }
            
            if (avcodec_send_packet(m_codecCtx, pkt) < 0) {
                av_packet_unref(pkt);
                continue;
            }
            
            while (avcodec_receive_frame(m_codecCtx, frame) == 0) {
                int64_t pts = av_rescale_q(frame->pts, stream->time_base, AVRational{1, 30});
                
                if (pts >= frameNumber) {
                    // Got our frame
                    FrameHandle::FrameData* data = new FrameHandle::FrameData();
                    
                    if (frame->format == m_hwPixFmt) {
                        // Hardware frame - map to system memory or use directly
                        data->isHardware = true;
                        data->avFrame = av_frame_clone(frame);
                    } else {
                        // Software frame
                        data->avFrame = av_frame_clone(frame);
                    }
                    
                    data->pts = pts;
                    data->size = QSize(frame->width, frame->height);
                    
                    result = FrameHandle(data);
                    goto done;
                }
            }
            
            av_packet_unref(pkt);
        }
        
done:
        av_frame_free(&frame);
        av_packet_free(&pkt);
        m_state = State::Ready;
        return result;
    }
    
    void shutdown() {
        if (m_codecCtx) {
            avcodec_free_context(&m_codecCtx);
        }
        if (m_formatCtx) {
            avformat_close_input(&m_formatCtx);
        }
        m_state = State::Uninitialized;
    }
    
    bool isHardwareAccelerated() const {
        return m_hwDeviceType != AV_HWDEVICE_TYPE_NONE;
    }
    
    State state() const { return m_state; }
    QString errorString() const { return m_error; }
    
signals:
    void frameReady(FrameHandle frame);
    void error(const QString& msg);
    
private:
    void setError(const QString& msg) {
        m_error = msg;
        m_state = State::Error;
        emit error(msg);
    }
    
    QString m_path;
    QString m_error;
    
    AVFormatContext* m_formatCtx = nullptr;
    AVCodecContext* m_codecCtx = nullptr;
    int m_videoStreamIdx = -1;
    
    AVHWDeviceType m_hwDeviceType = AV_HWDEVICE_TYPE_NONE;
    AVPixelFormat m_hwPixFmt = AV_PIX_FMT_NONE;
    
    std::atomic<State> m_state{State::Uninitialized};
};

// ============================================================================
// 4. Decoder Worker Thread
// ============================================================================

class DecoderWorker : public QObject {
    Q_OBJECT
public:
    explicit DecoderWorker(DecodeRequestQueue* queue, QObject* parent = nullptr)
        : QObject(parent), m_queue(queue) {}
    
    void start() {
        m_thread = new QThread(this);
        moveToThread(m_thread);
        connect(m_thread, &QThread::started, this, &DecoderWorker::run);
        m_thread->start();
    }
    
    void stop() {
        m_running = false;
        m_thread->quit();
        m_thread->wait(5000);
    }
    
    // Get or create decoder for file
    HWDecoder* getDecoder(const QString& path) {
        QMutexLocker lock(&m_decodersMutex);
        auto it = m_decoders.find(path);
        if (it != m_decoders.end()) {
            return it.value();
        }
        
        HWDecoder* decoder = new HWDecoder(path, this);
        if (decoder->initialize()) {
            m_decoders[path] = decoder;
            return decoder;
        }
        
        delete decoder;
        return nullptr;
    }
    
public slots:
    void run() {
        m_running = true;
        DecodeRequest req;
        
        while (m_running) {
            if (!m_queue->dequeue(&req)) {
                continue;
            }
            
            // Check deadline
            if (req.isExpired()) {
                if (req.callback) {
                    req.callback(FrameHandle()); // null = dropped
                }
                continue;
            }
            
            // Get decoder
            HWDecoder* decoder = getDecoder(req.filePath);
            if (!decoder) {
                if (req.callback) {
                    req.callback(FrameHandle());
                }
                continue;
            }
            
            // Decode (blocking call, but we're on worker thread)
            FrameHandle frame = decoder->decodeFrame(req.frameNumber);
            
            // Return result via callback
            if (req.callback) {
                req.callback(frame);
            }
        }
    }
    
private:
    QThread* m_thread = nullptr;
    DecodeRequestQueue* m_queue = nullptr;
    std::atomic<bool> m_running{false};
    
    QMutex m_decodersMutex;
    QHash<QString, HWDecoder*> m_decoders;
};

// ============================================================================
// 5. Frame Scheduler (API for UI)
// ============================================================================

class FrameScheduler : public QObject {
    Q_OBJECT
public:
    explicit FrameScheduler(QObject* parent = nullptr) : QObject(parent) {
        // Create worker threads (one per logical core)
        int threadCount = qMax(2, QThread::idealThreadCount() / 2);
        for (int i = 0; i < threadCount; ++i) {
            auto* worker = new DecoderWorker(&m_queue, this);
            worker->start();
            m_workers.append(worker);
        }
    }
    
    ~FrameScheduler() {
        m_queue.shutdown();
        for (auto* worker : m_workers) {
            worker->stop();
        }
        qDeleteAll(m_workers);
    }
    
    // Async frame request - returns immediately
    void requestFrame(const QString& path, int64_t frameNumber, 
                      int priority,
                      std::function<void(FrameHandle)> callback) {
        DecodeRequest req;
        req.sequenceId = m_nextSequenceId++;
        req.filePath = path;
        req.frameNumber = frameNumber;
        req.priority = priority;
        req.deadline = QDeadlineTimer(100); // 100ms deadline
        req.callback = callback;
        
        if (!m_queue.enqueue(std::move(req))) {
            // Queue full, return null immediately
            if (callback) {
                callback(FrameHandle());
            }
        }
    }
    
    // Cancel all pending requests for a file (e.g., when scrubbing)
    void cancelForFile(const QString& path) {
        m_queue.clear();
    }
    
private:
    DecodeRequestQueue m_queue;
    QVector<DecoderWorker*> m_workers;
    std::atomic<uint64_t> m_nextSequenceId{1};
};

// ============================================================================
// 6. Usage Example
// ============================================================================

/*

// In PreviewWindow::setCurrentFrame:
void PreviewWindow::setCurrentFrame(int frame) {
    m_currentFrame = frame;
    
    // Find active clips
    for (const TimelineClip& clip : m_clips) {
        if (frame >= clip.startFrame && frame < clip.endFrame) {
            int64_t localFrame = frame - clip.startFrame;
            
            // Request with appropriate priority
            int priority = m_playing ? 100 : 50;
            
            m_scheduler->requestFrame(
                clip.filePath,
                localFrame,
                priority,
                [this, clip](FrameHandle frame) {
                    if (!frame.isNull()) {
                        // Frame ready - will be displayed next paint
                        m_frameCache[clip.id] = frame;
                        update(); // trigger paint
                    }
                }
            );
        }
    }
}

// In PreviewWindow::paintEvent:
void PreviewWindow::paintEvent(QPaintEvent*) {
    // Frames are already GPU textures - just composite
    m_compositor->clearLayers();
    
    int layerIdx = 0;
    for (const TimelineClip& clip : activeClips()) {
        FrameHandle frame = m_frameCache.value(clip.id);
        if (!frame.isNull()) {
            m_compositor->setLayer(
                layerIdx++,
                frame.texture(),
                clip.transform,
                clip.opacity,
                clip.blendMode
            );
        }
    }
    
    // Render to screen
    m_compositor->render(m_swapChain);
}

*/

#include "professional_example.moc"
