#include "async_decoder.h"

#include <QDebug>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QHash>
#include <QThread>
#include <QThreadStorage>

#include <limits>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libavutil/time.h>
#include <libswscale/swscale.h>
}

namespace editor {

namespace {
qint64 decodeTraceMs() {
    static QElapsedTimer timer;
    static bool started = false;
    if (!started) {
        timer.start();
        started = true;
    }
    return timer.elapsed();
}

QString shortPath(const QString& path) {
    return QFileInfo(path).fileName();
}

void decodeTrace(const QString& stage, const QString& detail = QString()) {
    static QHash<QString, qint64> lastLogByStage;
    const qint64 now = decodeTraceMs();
    if (stage.startsWith(QStringLiteral("AsyncDecoder::requestFrame")) ||
        stage.startsWith(QStringLiteral("DecoderWorker::processRequest.begin")) ||
        stage.startsWith(QStringLiteral("DecoderContext::decodeFrame.begin")) ||
        stage.startsWith(QStringLiteral("DecoderContext::decodeFrame.seek")) ||
        stage.startsWith(QStringLiteral("DecoderContext::decodeFrame.end"))) {
        const qint64 last = lastLogByStage.value(stage, std::numeric_limits<qint64>::min());
        if (now - last < 250) {
            return;
        }
        lastLogByStage.insert(stage, now);
    }
    qDebug().noquote() << QStringLiteral("[DECODE %1 ms] %2%3")
                              .arg(now, 6)
                              .arg(stage)
                              .arg(detail.isEmpty() ? QString() : QStringLiteral(" | ") + detail);
}
}

// ============================================================================
// FFmpeg Helper Functions
// ============================================================================

static QString avErrToString(int errnum) {
    char errbuf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(errnum, errbuf, AV_ERROR_MAX_STRING_SIZE);
    return QString::fromUtf8(errbuf);
}

static AVPixelFormat get_hw_format(AVCodecContext* ctx, const AVPixelFormat* pix_fmts) {
    const AVPixelFormat preferred =
        static_cast<AVPixelFormat>(reinterpret_cast<intptr_t>(ctx->opaque));
    for (const AVPixelFormat* p = pix_fmts; *p != -1; p++) {  // -1 = AV_PIX_FMT_NONE
        if (*p == preferred) {
            return *p;
        }
    }

    // Fall back to the first software format the decoder offers.
    return pix_fmts[0];
}

static int64_t ptsToFrameNumber(int64_t pts, const AVRational& timeBase, double fps) {
    if (pts == AV_NOPTS_VALUE || fps <= 0.0) {
        return -1;
    }
    const double seconds = pts * av_q2d(timeBase);
    return static_cast<int64_t>(seconds * fps + 0.5);
}

// ============================================================================
// DecoderContext Implementation
// ============================================================================

DecoderContext::DecoderContext(const QString& path) : m_path(path) {}

DecoderContext::~DecoderContext() {
    shutdown();
}

void DecoderContext::shutdown() {
    if (m_codecCtx) {
        avcodec_free_context(&m_codecCtx);
    }
    if (m_formatCtx) {
        avformat_close_input(&m_formatCtx);
    }
    if (m_hwDeviceCtx) {
        av_buffer_unref(&m_hwDeviceCtx);
    }
    m_hwDeviceCtx = nullptr;
    m_hwPixFmt = AV_PIX_FMT_NONE;
}

bool DecoderContext::initialize() {
    if (!openInput()) return false;
    if (!initCodec()) return false;
    
    updateAccessTime();
    return true;
}

bool DecoderContext::openInput() {
    int ret = avformat_open_input(&m_formatCtx, m_path.toUtf8().constData(), nullptr, nullptr);
    if (ret < 0) {
        qWarning() << "Failed to open input:" << m_path << avErrToString(ret);
        return false;
    }
    
    ret = avformat_find_stream_info(m_formatCtx, nullptr);
    if (ret < 0) {
        qWarning() << "Failed to find stream info:" << avErrToString(ret);
        return false;
    }
    
    // Find video stream
    for (unsigned i = 0; i < m_formatCtx->nb_streams; i++) {
        AVStream* stream = m_formatCtx->streams[i];
        if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            m_videoStreamIndex = i;
            
            // Fill info
            m_info.path = m_path;
            m_info.frameSize = QSize(stream->codecpar->width, stream->codecpar->height);
            
            // Calculate FPS
            AVRational framerate = av_guess_frame_rate(m_formatCtx, stream, nullptr);
            m_info.fps = framerate.num > 0 && framerate.den > 0 ? 
                        av_q2d(framerate) : 30.0;
            
            // Calculate duration in frames
            if (stream->duration != AV_NOPTS_VALUE) {
                double secs = stream->duration * av_q2d(stream->time_base);
                m_info.durationFrames = static_cast<int64_t>(secs * m_info.fps);
            } else if (m_formatCtx->duration > 0) {
                double secs = m_formatCtx->duration / (double)AV_TIME_BASE;
                m_info.durationFrames = static_cast<int64_t>(secs * m_info.fps);
            }
            
            m_info.bitrate = stream->codecpar->bit_rate;
            if (AVDictionaryEntry* alphaMode = av_dict_get(stream->metadata, "alpha_mode", nullptr, 0)) {
                m_streamHasAlphaTag = QByteArray(alphaMode->value) == "1";
                m_info.hasAlpha = m_streamHasAlphaTag;
            }
            m_info.isValid = true;
            break;
        }
    }
    
    if (m_videoStreamIndex < 0) {
        qWarning() << "No video stream found in:" << m_path;
        return false;
    }
    
    return true;
}

bool DecoderContext::initCodec() {
    AVStream* stream = m_formatCtx->streams[m_videoStreamIndex];
    const AVCodec* decoder = nullptr;
    if (stream->codecpar->codec_id == AV_CODEC_ID_VP9 && m_streamHasAlphaTag) {
        decoder = avcodec_find_decoder_by_name("libvpx-vp9");
        if (decoder) {
            qDebug() << "Using libvpx-vp9 for alpha-tagged VP9 stream:" << m_path;
        }
    }
    if (!decoder) {
        decoder = avcodec_find_decoder(stream->codecpar->codec_id);
    }
    
    if (!decoder) {
        qWarning() << "Decoder not found for codec_id:" << stream->codecpar->codec_id;
        return false;
    }
    
    m_info.codecName = QString::fromUtf8(decoder->name);
    
    m_codecCtx = avcodec_alloc_context3(decoder);
    if (!m_codecCtx) {
        qWarning() << "Failed to allocate codec context";
        return false;
    }
    
    int ret = avcodec_parameters_to_context(m_codecCtx, stream->codecpar);
    if (ret < 0) {
        qWarning() << "Failed to copy codec params:" << avErrToString(ret);
        return false;
    }

    // Timeline playback benefits more from deterministic frame-exact decoding
    // than from codec-internal parallelism. Keeping each decoder context
    // single-threaded also avoids hard-to-reproduce races in optimized builds.
    m_codecCtx->thread_count = 1;
    m_codecCtx->thread_type = 0;
    
    // Hardware decode is disabled for now.
    // The current worker-thread startup path is not robust across driver stacks.
    
    ret = avcodec_open2(m_codecCtx, decoder, nullptr);
    if (ret < 0) {
        qWarning() << "Failed to open codec:" << avErrToString(ret);
        return false;
    }
    
    return true;
}

bool DecoderContext::initHardwareAccel(const AVCodec* decoder) {
    // Try CUDA first (NVIDIA), then VAAPI (Intel/AMD Linux), then VDPAU
    static const AVHWDeviceType kPreferredDevices[] = {
        AV_HWDEVICE_TYPE_CUDA,
        AV_HWDEVICE_TYPE_VAAPI,
        AV_HWDEVICE_TYPE_VDPAU,
        AV_HWDEVICE_TYPE_DXVA2,
        AV_HWDEVICE_TYPE_D3D11VA,
        AV_HWDEVICE_TYPE_VIDEOTOOLBOX,
    };
    
    for (AVHWDeviceType type : kPreferredDevices) {
        const AVCodecHWConfig* selectedConfig = nullptr;
        for (int i = 0;; ++i) {
            const AVCodecHWConfig* config = avcodec_get_hw_config(decoder, i);
            if (!config) {
                break;
            }
            if (config->device_type == type &&
                (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) {
                selectedConfig = config;
                break;
            }
        }

        if (!selectedConfig) {
            continue;
        }

        AVBufferRef* hwCtx = nullptr;
        int ret = av_hwdevice_ctx_create(&hwCtx, type, nullptr, nullptr, 0);
        if (ret >= 0) {
            m_codecCtx->hw_device_ctx = av_buffer_ref(hwCtx);
            m_hwDeviceCtx = hwCtx;
            m_hwPixFmt = selectedConfig->pix_fmt;

            // Set callback to select hardware format
            m_codecCtx->get_format = get_hw_format;
            m_codecCtx->opaque = reinterpret_cast<void*>(static_cast<intptr_t>(m_hwPixFmt));

            qDebug() << "Using hardware acceleration:" << type << "for" << m_path;
            return true;
        }
    }

    m_codecCtx->get_format = nullptr;
    m_codecCtx->opaque = nullptr;
    m_hwPixFmt = AV_PIX_FMT_NONE;
    return false;
}

FrameHandle DecoderContext::decodeFrame(int64_t frameNumber) {
    decodeTrace(QStringLiteral("DecoderContext::decodeFrame.begin"),
                QStringLiteral("file=%1 target=%2 last=%3")
                    .arg(shortPath(m_path))
                    .arg(frameNumber)
                    .arg(m_lastDecodedFrame));
    if (m_eof) {
        m_eof = false;
    }
    
    updateAccessTime();
    
    // For accurate seeking, we need to seek if we're too far ahead or behind
    int64_t frameDelta = frameNumber - m_lastDecodedFrame;
    if (frameDelta < 0 || frameDelta > 30) {
        decodeTrace(QStringLiteral("DecoderContext::decodeFrame.seek"),
                    QStringLiteral("file=%1 target=%2 delta=%3")
                        .arg(shortPath(m_path))
                        .arg(frameNumber)
                        .arg(frameDelta));
        return seekAndDecode(frameNumber);
    }
    
    // Sequential decode
    AVFrame* frame = av_frame_alloc();
    AVPacket* packet = av_packet_alloc();
    FrameHandle result;
    
    AVStream* stream = m_formatCtx->streams[m_videoStreamIndex];
    
    while (av_read_frame(m_formatCtx, packet) >= 0) {
        if (packet->stream_index != m_videoStreamIndex) {
            av_packet_unref(packet);
            continue;
        }
        
        int ret = avcodec_send_packet(m_codecCtx, packet);
        av_packet_unref(packet);
        
        if (ret < 0) {
            continue;
        }
        
        while (ret >= 0) {
            ret = avcodec_receive_frame(m_codecCtx, frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                break;
            }
            if (ret < 0) {
                break;
            }
            
            // Calculate frame number from PTS
            int64_t pts = frame->best_effort_timestamp != AV_NOPTS_VALUE ? frame->best_effort_timestamp : frame->pts;
            int64_t currentFrame = ptsToFrameNumber(pts, stream->time_base, m_info.fps);
            if (currentFrame < 0) {
                currentFrame = frameNumber;
            }
            
            m_lastDecodedFrame = currentFrame;
            
            if (currentFrame >= frameNumber) {
                result = convertToFrame(frame, currentFrame);
                av_frame_unref(frame);
                goto done;
            }
            
            av_frame_unref(frame);
        }
    }
    
    m_eof = true;
    
done:
    av_frame_free(&frame);
    av_packet_free(&packet);
    decodeTrace(QStringLiteral("DecoderContext::decodeFrame.end"),
                QStringLiteral("file=%1 target=%2 null=%3 decoded=%4")
                    .arg(shortPath(m_path))
                    .arg(frameNumber)
                    .arg(result.isNull())
                    .arg(result.frameNumber()));
    return result;
}

FrameHandle DecoderContext::seekAndDecode(int64_t frameNumber) {
    const qint64 startedAt = decodeTraceMs();
    decodeTrace(QStringLiteral("DecoderContext::seekAndDecode.begin"),
                QStringLiteral("file=%1 target=%2")
                    .arg(shortPath(m_path))
                    .arg(frameNumber));
    if (!seekToKeyframe(frameNumber)) {
        decodeTrace(QStringLiteral("DecoderContext::seekAndDecode.seek-failed"),
                    QStringLiteral("file=%1 target=%2").arg(shortPath(m_path)).arg(frameNumber));
        return FrameHandle();
    }
    
    // Decode forward to exact frame
    AVFrame* frame = av_frame_alloc();
    AVPacket* packet = av_packet_alloc();
    FrameHandle result;
    
    AVStream* stream = m_formatCtx->streams[m_videoStreamIndex];
    
    while (av_read_frame(m_formatCtx, packet) >= 0) {
        if (packet->stream_index != m_videoStreamIndex) {
            av_packet_unref(packet);
            continue;
        }
        
        int ret = avcodec_send_packet(m_codecCtx, packet);
        av_packet_unref(packet);
        
        if (ret < 0) continue;
        
        while (ret >= 0) {
            ret = avcodec_receive_frame(m_codecCtx, frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if (ret < 0) break;
            
            int64_t pts = frame->best_effort_timestamp != AV_NOPTS_VALUE ? frame->best_effort_timestamp : frame->pts;
            int64_t currentFrame = ptsToFrameNumber(pts, stream->time_base, m_info.fps);
            if (currentFrame < 0) {
                currentFrame = frameNumber;
            }
            
            m_lastDecodedFrame = currentFrame;
            
            if (currentFrame >= frameNumber) {
                result = convertToFrame(frame, currentFrame);
                av_frame_unref(frame);
                goto done;
            }
            
            av_frame_unref(frame);
        }
    }
    
done:
    av_frame_free(&frame);
    av_packet_free(&packet);
    decodeTrace(QStringLiteral("DecoderContext::seekAndDecode.end"),
                QStringLiteral("file=%1 target=%2 null=%3 waitMs=%4 decoded=%5")
                    .arg(shortPath(m_path))
                    .arg(frameNumber)
                    .arg(result.isNull())
                    .arg(decodeTraceMs() - startedAt)
                    .arg(result.frameNumber()));
    return result;
}

bool DecoderContext::seekToKeyframe(int64_t targetFrame) {
    AVStream* stream = m_formatCtx->streams[m_videoStreamIndex];
    
    // Convert frame number into the video stream's time base.
    AVRational frameBase{1, static_cast<int>(qMax(1.0, m_info.fps))};
    int64_t targetTs = av_rescale_q(targetFrame, frameBase, stream->time_base);
    
    // Seek to nearest keyframe before target
    int ret = av_seek_frame(m_formatCtx, m_videoStreamIndex, targetTs, AVSEEK_FLAG_BACKWARD);
    if (ret < 0) {
        qWarning() << "Seek failed:" << avErrToString(ret);
        return false;
    }
    
    avcodec_flush_buffers(m_codecCtx);
    m_lastDecodedFrame = -1;
    
    return true;
}

FrameHandle DecoderContext::convertToFrame(AVFrame* avFrame, int64_t frameNumber) {
    QImage image = convertAVFrameToImage(avFrame);
    if (image.isNull()) {
        return FrameHandle();
    }

    // QPainter's alpha compositing path is most stable with premultiplied Qt
    // image formats. Normalize decoded video frames before caching them.
    if (image.format() != QImage::Format_ARGB32_Premultiplied) {
        image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    }
    
    FrameHandle handle = FrameHandle::createCpuFrame(image, frameNumber, m_path);
    return handle;
}

QImage DecoderContext::convertAVFrameToImage(AVFrame* frame) {
    AVFrame* swFrame = frame;
    AVFrame* tempFrame = nullptr;
    
    // If hardware frame, transfer to system memory
    if (frame->format == m_hwPixFmt && m_hwPixFmt != AV_PIX_FMT_NONE) {
        tempFrame = av_frame_alloc();
        if (av_hwframe_transfer_data(tempFrame, frame, 0) < 0) {
            av_frame_free(&tempFrame);
            return QImage();
        }
        swFrame = tempFrame;
    }
    
    // Setup swscale context
    AVPixelFormat sourceFormat = static_cast<AVPixelFormat>(swFrame->format);
    if (sourceFormat == AV_PIX_FMT_YUVJ420P) {
        sourceFormat = AV_PIX_FMT_YUV420P;
    }

    const AVPixFmtDescriptor* sourceDesc = av_pix_fmt_desc_get(sourceFormat);
    const bool sourceHasAlpha = sourceDesc && (sourceDesc->flags & AV_PIX_FMT_FLAG_ALPHA);
    if (!m_loggedSourceFormat) {
        m_loggedSourceFormat = true;
        decodeTrace(QStringLiteral("DecoderContext::convertAVFrameToImage.format"),
                    QStringLiteral("file=%1 fmt=%2 alphaTag=%3 alphaFmt=%4 linesize=[%5,%6,%7,%8]")
                        .arg(shortPath(m_path))
                        .arg(sourceDesc ? QString::fromUtf8(sourceDesc->name) : QStringLiteral("unknown"))
                        .arg(m_streamHasAlphaTag)
                        .arg(sourceHasAlpha)
                        .arg(swFrame->linesize[0])
                        .arg(swFrame->linesize[1])
                        .arg(swFrame->linesize[2])
                        .arg(swFrame->linesize[3]));
    }

    if (m_streamHasAlphaTag && !sourceHasAlpha) {
        if (!m_reportedAlphaMismatch) {
            m_reportedAlphaMismatch = true;
            qWarning().noquote() << QStringLiteral(
                "Alpha-tagged stream is decoding without alpha-capable pixel format; refusing frame conversion for %1 (fmt=%2)")
                .arg(shortPath(m_path))
                .arg(sourceDesc ? QString::fromUtf8(sourceDesc->name) : QStringLiteral("unknown"));
        }
        if (tempFrame) {
            av_frame_free(&tempFrame);
        }
        return QImage();
    }

    SwsContext* swsCtx = sws_getContext(
        swFrame->width, swFrame->height, sourceFormat,
        swFrame->width, swFrame->height, AV_PIX_FMT_RGBA,
        SWS_BILINEAR, nullptr, nullptr, nullptr
    );
    
    if (!swsCtx) {
        if (tempFrame) av_frame_free(&tempFrame);
        return QImage();
    }
    
    QImage image(swFrame->width, swFrame->height, QImage::Format_RGBA8888);
    if (image.isNull()) {
        sws_freeContext(swsCtx);
        if (tempFrame) av_frame_free(&tempFrame);
        return QImage();
    }

    AVFrame* rgbaFrame = av_frame_alloc();
    if (!rgbaFrame) {
        sws_freeContext(swsCtx);
        if (tempFrame) av_frame_free(&tempFrame);
        return QImage();
    }

    rgbaFrame->format = AV_PIX_FMT_RGBA;
    rgbaFrame->width = swFrame->width;
    rgbaFrame->height = swFrame->height;
    if (av_frame_get_buffer(rgbaFrame, 0) < 0) {
        av_frame_free(&rgbaFrame);
        sws_freeContext(swsCtx);
        if (tempFrame) av_frame_free(&tempFrame);
        return QImage();
    }

    if (sws_scale(swsCtx,
                  swFrame->data,
                  swFrame->linesize,
                  0,
                  swFrame->height,
                  rgbaFrame->data,
                  rgbaFrame->linesize) <= 0) {
        av_frame_free(&rgbaFrame);
        sws_freeContext(swsCtx);
        if (tempFrame) av_frame_free(&tempFrame);
        return QImage();
    }

    const int copyBytesPerRow = qMin(image.bytesPerLine(), rgbaFrame->linesize[0]);
    for (int y = 0; y < swFrame->height; ++y) {
        memcpy(image.scanLine(y), rgbaFrame->data[0] + (y * rgbaFrame->linesize[0]), copyBytesPerRow);
    }

    av_frame_free(&rgbaFrame);
    sws_freeContext(swsCtx);
    if (tempFrame) av_frame_free(&tempFrame);
    
    return image;
}

void DecoderContext::updateAccessTime() {
    m_lastAccessTime = QDateTime::currentMSecsSinceEpoch();
}

// ============================================================================
// DecodeQueue Implementation
// ============================================================================

bool DecodeQueue::enqueue(DecodeRequest req) {
    std::function<void(FrameHandle)> droppedCallback;
    QMutexLocker lock(&m_mutex);
    
    if (m_shutdown.load()) return false;
    
    if (m_queue.size() >= kMaxSize) {
        // Try to drop a lower priority request
        bool dropped = false;
        for (int i = static_cast<int>(m_queue.size()) - 1; i >= 0; --i) {
            if (m_queue[i].priority < req.priority) {
                droppedCallback = std::move(m_queue[i].callback);
                m_queue.erase(m_queue.begin() + i);
                dropped = true;
                break;
            }
        }
        if (!dropped) {
            return false;
        }
    }
    
    insertByPriority(req);
    m_condition.wakeOne();
    lock.unlock();
    if (droppedCallback) {
        droppedCallback(FrameHandle());
    }
    return true;
}

bool DecodeQueue::dequeue(DecodeRequest* out) {
    QMutexLocker lock(&m_mutex);
    
    while (m_queue.empty() && !m_shutdown.load()) {
        m_condition.wait(&m_mutex);
    }
    
    if (m_shutdown.load() && m_queue.empty()) {
        return false;
    }
    
    *out = std::move(m_queue.front());
    m_queue.pop_front();
    return true;
}

bool DecodeQueue::tryDequeue(DecodeRequest* out, int timeoutMs) {
    QMutexLocker lock(&m_mutex);
    
    if (m_queue.empty() && !m_shutdown.load()) {
        m_condition.wait(&m_mutex, timeoutMs);
    }
    
    if (m_queue.empty()) {
        return false;
    }
    
    *out = std::move(m_queue.front());
    m_queue.pop_front();
    return true;
}

void DecodeQueue::clear() {
    QMutexLocker lock(&m_mutex);
    m_queue.clear();
}

void DecodeQueue::removeForFile(const QString& path) {
    QVector<std::function<void(FrameHandle)>> callbacks;
    QMutexLocker lock(&m_mutex);
    for (int i = static_cast<int>(m_queue.size()) - 1; i >= 0; --i) {
        if (m_queue[i].filePath == path) {
            if (m_queue[i].callback) {
                callbacks.push_back(std::move(m_queue[i].callback));
            }
            m_queue.erase(m_queue.begin() + i);
        }
    }
    lock.unlock();
    for (const auto& callback : callbacks) {
        if (callback) {
            callback(FrameHandle());
        }
    }
}

void DecodeQueue::removeForFileBefore(const QString& path, int64_t frameNumber) {
    QVector<std::function<void(FrameHandle)>> callbacks;
    QMutexLocker lock(&m_mutex);
    for (int i = static_cast<int>(m_queue.size()) - 1; i >= 0; --i) {
        if (m_queue[i].filePath == path && m_queue[i].frameNumber < frameNumber) {
            if (m_queue[i].callback) {
                callbacks.push_back(std::move(m_queue[i].callback));
            }
            m_queue.erase(m_queue.begin() + i);
        }
    }
    lock.unlock();
    for (const auto& callback : callbacks) {
        if (callback) {
            callback(FrameHandle());
        }
    }
}

int DecodeQueue::size() const {
    QMutexLocker lock(&m_mutex);
    return m_queue.size();
}

bool DecodeQueue::isEmpty() const {
    QMutexLocker lock(&m_mutex);
    return m_queue.empty();
}

void DecodeQueue::shutdown() {
    QMutexLocker lock(&m_mutex);
    m_shutdown.store(true);
    m_condition.wakeAll();
}

void DecodeQueue::insertByPriority(const DecodeRequest& req) {
    // Insertion sort by priority (higher first)
    int i = 0;
    for (; i < static_cast<int>(m_queue.size()); ++i) {
        if (m_queue[i].priority < req.priority) {
            break;
        }
    }
    m_queue.insert(m_queue.begin() + i, req);
}

// ============================================================================
// DecoderWorker Implementation
// ============================================================================

DecoderWorker::DecoderWorker(DecodeQueue* queue, MemoryBudget* budget, QObject* parent)
    : QObject(parent), m_queue(queue), m_budget(budget) {}

DecoderWorker::~DecoderWorker() {
    stop();
    
    // Clean up decoder contexts
    QMutexLocker lock(&m_contextsMutex);
    for (auto it = m_contexts.begin(); it != m_contexts.end(); ++it) {
        delete it.value();
    }
    m_contexts.clear();
}

void DecoderWorker::start() {
    if (m_thread) return;
    
    m_thread = new QThread();
    moveToThread(m_thread);
    connect(m_thread, &QThread::started, this, &DecoderWorker::run);
    m_running.store(true);
    m_thread->start();
}

void DecoderWorker::stop() {
    m_running.store(false);
    if (m_thread) {
        m_thread->quit();
        m_thread->wait(5000);
        delete m_thread;
        m_thread = nullptr;
    }
    
    QMutexLocker lock(&m_contextsMutex);
    m_contexts.clear();
}

void DecoderWorker::run() {
    while (m_running.load()) {
        DecodeRequest req;
        if (!m_queue->dequeue(&req)) {
            continue;
        }
        
        if (req.isExpired()) {
            m_dropCount++;
            if (req.callback) {
                req.callback(FrameHandle());
            }
            continue;
        }
        
        processRequest(req);
    }
}

void DecoderWorker::processRequest(const DecodeRequest& req) {
    const qint64 startedAt = decodeTraceMs();
    decodeTrace(QStringLiteral("DecoderWorker::processRequest.begin"),
                QStringLiteral("seq=%1 file=%2 frame=%3 priority=%4 queue=%5 thread=%6")
                    .arg(req.sequenceId)
                    .arg(shortPath(req.filePath))
                    .arg(req.frameNumber)
                    .arg(req.priority)
                    .arg(m_queue ? m_queue->size() : -1)
                    .arg(reinterpret_cast<quintptr>(QThread::currentThreadId()), 0, 16));
    DecoderContext* ctx = getOrCreateContext(req.filePath);
    if (!ctx) {
        m_dropCount++;
        if (req.callback) {
            req.callback(FrameHandle());
        }
        emit decodeError(req.filePath, "Failed to create decoder context");
        return;
    }
    
    FrameHandle frame = ctx->decodeFrame(req.frameNumber);
    
    if (frame.isNull()) {
        m_dropCount++;
    } else {
        m_decodeCount++;
        
        // Track memory
        if (m_budget) {
            size_t mem = frame.memoryUsage();
            m_budget->allocateCpu(mem, MemoryBudget::Priority::Normal);
        }
    }
    
    if (req.callback) {
        req.callback(frame);
    }
    
    emit frameDecoded(frame);
    decodeTrace(QStringLiteral("DecoderWorker::processRequest.end"),
                QStringLiteral("seq=%1 file=%2 frame=%3 null=%4 waitMs=%5")
                    .arg(req.sequenceId)
                    .arg(shortPath(req.filePath))
                    .arg(req.frameNumber)
                    .arg(frame.isNull())
                    .arg(decodeTraceMs() - startedAt));
}

DecoderContext* DecoderWorker::getOrCreateContext(const QString& path) {
    QMutexLocker lock(&m_contextsMutex);
    
    auto it = m_contexts.find(path);
    if (it != m_contexts.end()) {
        return it.value();
    }
    
    // Evict oldest if at limit
    if (m_contexts.size() >= kMaxContexts) {
        evictOldestContext();
    }
    
    DecoderContext* ctx = new DecoderContext(path);
    if (!ctx->initialize()) {
        delete ctx;
        return nullptr;
    }
    
    m_contexts[path] = ctx;
    return ctx;
}

void DecoderWorker::evictOldestContext() {
    qint64 oldestTime = INT64_MAX;
    QString oldestPath;
    
    for (auto it = m_contexts.begin(); it != m_contexts.end(); ++it) {
        if (it.value()->lastAccessTime() < oldestTime) {
            oldestTime = it.value()->lastAccessTime();
            oldestPath = it.key();
        }
    }
    
    if (!oldestPath.isEmpty()) {
        delete m_contexts.take(oldestPath);
    }
}

// ============================================================================
// AsyncDecoder Implementation
// ============================================================================

AsyncDecoder::AsyncDecoder(QObject* parent) : QObject(parent) {
    m_budget = new MemoryBudget(this);
    setupTrimCallback();
}

AsyncDecoder::~AsyncDecoder() {
    shutdown();
}

bool AsyncDecoder::initialize() {
    // A single worker preserves decoder context locality for timeline playback.
    // With multiple workers, sequential frame requests for one clip bounce across
    // independent FFmpeg contexts and degrade into repeated seek-heavy decoding.
    const int workerCount = 1;
    
    for (int i = 0; i < workerCount; ++i) {
        auto* worker = new DecoderWorker(&m_queue, m_budget);
        worker->start();
        m_workers.append(worker);
    }
    
    qDebug() << "AsyncDecoder initialized with" << workerCount << "workers";
    return true;
}

void AsyncDecoder::shutdown() {
    m_queue.shutdown();
    
    for (auto* worker : m_workers) {
        worker->stop();
        delete worker;
    }
    m_workers.clear();
}

uint64_t AsyncDecoder::requestFrame(const QString& path, 
                                    int64_t frameNumber,
                                    int priority,
                                    int timeoutMs,
                                    std::function<void(FrameHandle)> callback) {
    uint64_t seqId = m_nextSequenceId++;
    
    DecodeRequest req;
    req.sequenceId = seqId;
    req.filePath = path;
    req.frameNumber = frameNumber;
    req.priority = priority;
    req.deadline = QDeadlineTimer(timeoutMs);
    req.callback = callback;
    req.submittedAt = QDateTime::currentMSecsSinceEpoch();

    decodeTrace(QStringLiteral("AsyncDecoder::requestFrame"),
                QStringLiteral("seq=%1 file=%2 frame=%3 priority=%4 timeoutMs=%5 pending=%6")
                    .arg(seqId)
                    .arg(shortPath(path))
                    .arg(frameNumber)
                    .arg(priority)
                    .arg(timeoutMs)
                    .arg(m_queue.size()));
    
    if (!m_queue.enqueue(req)) {
        // Queue full, return immediately with null
        decodeTrace(QStringLiteral("AsyncDecoder::requestFrame.queue-full"),
                    QStringLiteral("seq=%1 file=%2 frame=%3")
                        .arg(seqId)
                        .arg(shortPath(path))
                        .arg(frameNumber));
        if (callback) {
            callback(FrameHandle());
        }
    }
    
    emit queuePressureChanged(m_queue.size());
    return seqId;
}

void AsyncDecoder::cancelForFile(const QString& path) {
    m_queue.removeForFile(path);
}

void AsyncDecoder::cancelForFileBefore(const QString& path, int64_t frameNumber) {
    m_queue.removeForFileBefore(path, frameNumber);
}

void AsyncDecoder::cancelAll() {
    m_queue.clear();
}

VideoStreamInfo AsyncDecoder::getVideoInfo(const QString& path) {
    QMutexLocker lock(&m_infoCacheMutex);
    
    auto it = m_infoCache.find(path);
    if (it != m_infoCache.end()) {
        return it.value();
    }
    
    // Probe file
    DecoderContext ctx(path);
    if (ctx.initialize()) {
        VideoStreamInfo info = ctx.info();
        m_infoCache[path] = info;
        return info;
    }
    
    return VideoStreamInfo();
}

void AsyncDecoder::preloadFile(const QString& path) {
    // Just create a decoder context to warm up
    getVideoInfo(path);
}

void AsyncDecoder::setupTrimCallback() {
    if (m_budget) {
        m_budget->setTrimCallback([this]() {
            trimCaches();
        });
    }
}

void AsyncDecoder::trimCaches() {
    // Called under memory pressure - could clear info cache, etc.
    QMutexLocker lock(&m_infoCacheMutex);
    // Keep only recent entries
    // (In production, implement proper LRU)
}

} // namespace editor
