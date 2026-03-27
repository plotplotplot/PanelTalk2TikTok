#include "async_decoder.h"
#include "debug_controls.h"
#include "editor_shared.h"

#include <QDebug>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QImageReader>
#include <QGuiApplication>
#include <QPainter>
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
constexpr int64_t kMaxSequentialDecodeGap = 90;
constexpr int64_t kPlaybackBatchFrameSlack = 2;

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

bool isStillImagePath(const QString& path) {
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

QImage loadSingleImageFile(const QString& framePath);

QImage loadSequenceFrameImage(const QStringList& framePaths, int64_t frameNumber) {
    if (framePaths.isEmpty()) {
        return QImage();
    }
    const int index = qBound(0, static_cast<int>(frameNumber), framePaths.size() - 1);
    const QString framePath = framePaths.at(index);
    return loadSingleImageFile(framePath);
}

QImage loadSingleImageFile(const QString& framePath) {
    if (framePath.isEmpty()) {
        return QImage();
    }

    QImageReader reader(framePath);
    reader.setAutoTransform(true);
    QImage image = reader.read();
    if (!image.isNull()) {
        return image;
    }

    AVFormatContext* formatCtx = nullptr;
    const QByteArray pathBytes = QFile::encodeName(framePath);
    const QString suffix = QFileInfo(framePath).suffix().toLower();
    const AVInputFormat* inputFormat = nullptr;
    if (suffix == QStringLiteral("webp")) {
        inputFormat = av_find_input_format("webp_pipe");
    } else {
        inputFormat = av_find_input_format("image2");
    }
    if (avformat_open_input(&formatCtx, pathBytes.constData(), inputFormat, nullptr) < 0) {
        return QImage();
    }

    if (avformat_find_stream_info(formatCtx, nullptr) < 0) {
        avformat_close_input(&formatCtx);
        return QImage();
    }

    int videoStreamIndex = -1;
    for (unsigned i = 0; i < formatCtx->nb_streams; ++i) {
        if (formatCtx->streams[i] &&
            formatCtx->streams[i]->codecpar &&
            formatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            videoStreamIndex = static_cast<int>(i);
            break;
        }
    }
    if (videoStreamIndex < 0) {
        avformat_close_input(&formatCtx);
        return QImage();
    }

    AVStream* stream = formatCtx->streams[videoStreamIndex];
    const AVCodec* decoder = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!decoder) {
        avformat_close_input(&formatCtx);
        return QImage();
    }

    AVCodecContext* codecCtx = avcodec_alloc_context3(decoder);
    if (!codecCtx) {
        avformat_close_input(&formatCtx);
        return QImage();
    }
    if (avcodec_parameters_to_context(codecCtx, stream->codecpar) < 0 ||
        avcodec_open2(codecCtx, decoder, nullptr) < 0) {
        avcodec_free_context(&codecCtx);
        avformat_close_input(&formatCtx);
        return QImage();
    }

    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    SwsContext* swsCtx = nullptr;
    QImage decodedImage;

    while (packet && frame && av_read_frame(formatCtx, packet) >= 0 && decodedImage.isNull()) {
        if (packet->stream_index == videoStreamIndex && avcodec_send_packet(codecCtx, packet) >= 0) {
            while (avcodec_receive_frame(codecCtx, frame) >= 0) {
                swsCtx = sws_getCachedContext(swsCtx,
                                              frame->width,
                                              frame->height,
                                              static_cast<AVPixelFormat>(frame->format),
                                              frame->width,
                                              frame->height,
                                              AV_PIX_FMT_RGBA,
                                              SWS_BILINEAR,
                                              nullptr,
                                              nullptr,
                                              nullptr);
                if (!swsCtx) {
                    av_frame_unref(frame);
                    break;
                }

                QImage rgba(frame->width, frame->height, QImage::Format_RGBA8888);
                if (rgba.isNull()) {
                    av_frame_unref(frame);
                    break;
                }

                uint8_t* destData[4] = { rgba.bits(), nullptr, nullptr, nullptr };
                int destLinesize[4] = { static_cast<int>(rgba.bytesPerLine()), 0, 0, 0 };
                if (sws_scale(swsCtx,
                              frame->data,
                              frame->linesize,
                              0,
                              frame->height,
                              destData,
                              destLinesize) > 0) {
                    decodedImage = rgba;
                }
                av_frame_unref(frame);
                if (!decodedImage.isNull()) {
                    break;
                }
            }
        }
        av_packet_unref(packet);
    }

    if (swsCtx) {
        sws_freeContext(swsCtx);
    }
    if (frame) {
        av_frame_free(&frame);
    }
    if (packet) {
        av_packet_free(&packet);
    }
    avcodec_free_context(&codecCtx);
    avformat_close_input(&formatCtx);
    return decodedImage;
}

void decodeTrace(const QString& stage, const QString& detail = QString()) {
    if (debugDecodeLevel() < DebugLogLevel::Info) {
        return;
    }
    static QHash<QString, qint64> lastLogByStage;
    const qint64 now = decodeTraceMs();
    if (!debugDecodeVerboseEnabled() &&
        (stage.startsWith(QStringLiteral("AsyncDecoder::requestFrame")) ||
         stage.startsWith(QStringLiteral("DecoderWorker::processRequest.begin")) ||
         stage.startsWith(QStringLiteral("DecoderContext::decodeFrame.begin")) ||
         stage.startsWith(QStringLiteral("DecoderContext::decodeFrame.seek")) ||
         stage.startsWith(QStringLiteral("DecoderContext::decodeFrame.end")))) {
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

// get_alpha_compatible_format - Select a pixel format that supports alpha
// when the stream is tagged with alpha_mode. This is needed for VP8/VP9
// streams where the default decoder output (yuv420p) doesn't have alpha.
static AVPixelFormat get_alpha_compatible_format(AVCodecContext* ctx, const AVPixelFormat* pix_fmts) {
    Q_UNUSED(ctx)
    
    // First, try to find a format with alpha support
    for (const AVPixelFormat* p = pix_fmts; *p != -1; p++) {
        const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(*p);
        if (desc && (desc->flags & AV_PIX_FMT_FLAG_ALPHA)) {
            return *p;
        }
    }
    
    // Fall back to the first available format
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
    if (m_swsCtx) {
        sws_freeContext(m_swsCtx);
        m_swsCtx = nullptr;
    }
    if (m_convertFrame) {
        av_frame_free(&m_convertFrame);
    }
    m_hwDeviceCtx = nullptr;
    m_hwPixFmt = AV_PIX_FMT_NONE;
    m_swsSourceFormat = AV_PIX_FMT_NONE;
    m_swsSourceSize = QSize();
    m_convertFrameSize = QSize();
}

bool DecoderContext::initialize() {
    if (isImageSequencePath(m_path)) {
        if (!loadImageSequence()) {
            return false;
        }
        updateAccessTime();
        return true;
    }
    if (isStillImagePath(m_path)) {
        if (!loadStillImage()) {
            return false;
        }
        updateAccessTime();
        return true;
    }

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

bool DecoderContext::loadStillImage() {
    QImage image = loadSingleImageFile(m_path);
    if (image.isNull()) {
        qWarning() << "Failed to load image:" << m_path;
        return false;
    }

    if (image.format() != QImage::Format_ARGB32_Premultiplied) {
        image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    }

    m_isStillImage = true;
    m_stillImage = image;
    m_info.path = m_path;
    m_info.durationFrames = 1;
    m_info.fps = 30.0;
    m_info.frameSize = image.size();
    m_info.codecName = QStringLiteral("still-image");
    m_info.hasAlpha = image.hasAlphaChannel();
    m_info.isValid = true;
    m_lastDecodedFrame = 0;
    m_eof = false;
    return true;
}

bool DecoderContext::loadImageSequence() {
    const QStringList framePaths = imageSequenceFramePaths(m_path);
    if (framePaths.isEmpty()) {
        return false;
    }

    QImage image = loadSequenceFrameImage(framePaths, 0);
    if (image.isNull()) {
        qWarning() << "Failed to load first image sequence frame:" << m_path;
        return false;
    }
    if (image.format() != QImage::Format_ARGB32_Premultiplied) {
        image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    }

    m_isImageSequence = true;
    m_sequenceFramePaths = framePaths;
    m_stillImage = image;
    m_info.path = m_path;
    m_info.durationFrames = framePaths.size();
    m_info.fps = 30.0;
    m_info.frameSize = image.size();
    m_info.codecName = QStringLiteral("image_sequence");
    m_info.hasAlpha = image.hasAlphaChannel();
    m_info.isValid = true;
    m_lastDecodedFrame = 0;
    m_eof = false;
    return true;
}

bool DecoderContext::initCodec() {
    AVStream* stream = m_formatCtx->streams[m_videoStreamIndex];
    const AVCodec* decoder = nullptr;
    const bool requiresSoftwareAlphaPath =
        stream->codecpar->codec_id == AV_CODEC_ID_VP9 && m_streamHasAlphaTag;
    if (requiresSoftwareAlphaPath) {
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

    // Professional editors typically use hardware decode for opaque playback
    // streams, but keep software decode for alpha-tagged formats where
    // hardware paths often drop or mangle transparency.
    const bool headlessOffscreen =
        qEnvironmentVariable("QT_QPA_PLATFORM") == QStringLiteral("offscreen");
    const bool hardwareEnabled =
        !headlessOffscreen &&
        !m_streamHasAlphaTag &&
        initHardwareAccel(decoder);
    if (hardwareEnabled) {
        // Let FFmpeg choose sensible threading for hardware-backed decode.
        m_codecCtx->thread_count = 0;
        m_codecCtx->thread_type = FF_THREAD_FRAME;
    } else if (requiresSoftwareAlphaPath) {
        // Alpha WebM preview needs software decode, but professional editors
        // still run that path with codec-level parallelism for realtime use.
        m_codecCtx->thread_count = qBound(2, QThread::idealThreadCount(), 8);
        m_codecCtx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
        m_codecCtx->flags2 |= AV_CODEC_FLAG2_FAST;
    } else {
        // Timeline playback benefits from deterministic frame-exact decoding
        // when we stay on the software path.
        m_codecCtx->thread_count = 1;
        m_codecCtx->thread_type = 0;
    }

    // For alpha-tagged streams, request a pixel format that supports alpha.
    // This is necessary because some decoders (e.g., VP8/VP9) default to
    // non-alpha formats like yuv420p even when the stream contains alpha.
    if (m_streamHasAlphaTag) {
        m_codecCtx->get_format = get_alpha_compatible_format;
    }
    
    ret = avcodec_open2(m_codecCtx, decoder, nullptr);
    if (ret < 0) {
        qWarning() << "Failed to open codec:" << avErrToString(ret);
        return false;
    }

    qDebug() << "Decoder path for" << m_path << ":" << (hardwareEnabled ? "hardware" : "software");
    
    return true;
}

bool DecoderContext::initHardwareAccel(const AVCodec* decoder) {
    // Professional playback stacks probe platform-appropriate hardware paths
    // and avoid display-dependent backends in headless environments.
    static const AVHWDeviceType kPreferredDevices[] = {
        AV_HWDEVICE_TYPE_CUDA,
        AV_HWDEVICE_TYPE_VAAPI,
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

        const char* deviceName = nullptr;
#if defined(Q_OS_LINUX)
        QByteArray devicePath;
        if (type == AV_HWDEVICE_TYPE_VAAPI) {
            static const char* kRenderNodes[] = {
                "/dev/dri/renderD128",
                "/dev/dri/renderD129",
                "/dev/dri/renderD130",
            };
            for (const char* candidate : kRenderNodes) {
                if (QFile::exists(QString::fromLatin1(candidate))) {
                    devicePath = QByteArray(candidate);
                    deviceName = devicePath.constData();
                    break;
                }
            }
            if (!deviceName) {
                continue;
            }
        }
#endif

        AVBufferRef* hwCtx = nullptr;
        int ret = av_hwdevice_ctx_create(&hwCtx, type, deviceName, nullptr, 0);
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
    if (m_isImageSequence) {
        updateAccessTime();
        QImage image = loadSequenceFrameImage(m_sequenceFramePaths, frameNumber);
        if (image.isNull()) {
            return FrameHandle();
        }
        if (image.format() != QImage::Format_ARGB32_Premultiplied) {
            image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        }
        m_lastDecodedFrame = qBound<int64_t>(0, frameNumber, static_cast<int64_t>(m_sequenceFramePaths.size() - 1));
        return FrameHandle::createCpuFrame(image, m_lastDecodedFrame, m_path);
    }
    if (m_isStillImage) {
        updateAccessTime();
        return FrameHandle::createCpuFrame(m_stillImage, 0, m_path);
    }

    decodeTrace(QStringLiteral("DecoderContext::decodeFrame.begin"),
                QStringLiteral("file=%1 target=%2 last=%3")
                    .arg(shortPath(m_path))
                    .arg(frameNumber)
                    .arg(m_lastDecodedFrame));
    FrameHandle result = seekAndDecode(frameNumber);
    decodeTrace(QStringLiteral("DecoderContext::decodeFrame.end"),
                QStringLiteral("file=%1 target=%2 null=%3 decoded=%4")
                    .arg(shortPath(m_path))
                    .arg(frameNumber)
                    .arg(result.isNull())
                    .arg(result.frameNumber()));
    return result;
}

QVector<FrameHandle> DecoderContext::decodeThroughFrame(int64_t targetFrame) {
    if (m_isImageSequence) {
        FrameHandle frame = decodeFrame(targetFrame);
        return frame.isNull() ? QVector<FrameHandle>() : QVector<FrameHandle>{frame};
    }
    if (m_isStillImage) {
        updateAccessTime();
        return {FrameHandle::createCpuFrame(m_stillImage, 0, m_path)};
    }

    if (m_eof) {
        m_eof = false;
    }

    updateAccessTime();

    const int64_t frameDelta = targetFrame - m_lastDecodedFrame;
    const bool forceSeek = m_lastDecodedFrame < 0 || frameDelta < 0 || frameDelta > kMaxSequentialDecodeGap;
    if (forceSeek) {
        decodeTrace(QStringLiteral("DecoderContext::decodeFrame.seek"),
                    QStringLiteral("file=%1 target=%2 delta=%3")
                        .arg(shortPath(m_path))
                        .arg(targetFrame)
                        .arg(frameDelta));
    }
    return decodeForwardUntil(targetFrame, forceSeek);
}

FrameHandle DecoderContext::seekAndDecode(int64_t frameNumber) {
    if (m_isImageSequence) {
        return decodeFrame(frameNumber);
    }
    if (m_isStillImage) {
        updateAccessTime();
        return FrameHandle::createCpuFrame(m_stillImage, 0, m_path);
    }

    const qint64 startedAt = decodeTraceMs();
    decodeTrace(QStringLiteral("DecoderContext::seekAndDecode.begin"),
                QStringLiteral("file=%1 target=%2")
                    .arg(shortPath(m_path))
                    .arg(frameNumber));
    const QVector<FrameHandle> frames = decodeForwardUntil(frameNumber, true);
    FrameHandle result;
    for (const FrameHandle& frame : frames) {
        if (!frame.isNull() && frame.frameNumber() >= frameNumber) {
            result = frame;
            break;
        }
    }
    decodeTrace(QStringLiteral("DecoderContext::seekAndDecode.end"),
                QStringLiteral("file=%1 target=%2 null=%3 waitMs=%4 decoded=%5")
                    .arg(shortPath(m_path))
                    .arg(frameNumber)
                    .arg(result.isNull())
                    .arg(decodeTraceMs() - startedAt)
                    .arg(result.frameNumber()));
    return result;
}

QVector<FrameHandle> DecoderContext::decodeForwardUntil(int64_t targetFrame, bool forceSeek) {
    QVector<FrameHandle> decodedFrames;
    if (m_isStillImage) {
        decodedFrames.push_back(FrameHandle::createCpuFrame(m_stillImage, 0, m_path));
        return decodedFrames;
    }

    if (forceSeek && !seekToKeyframe(targetFrame)) {
        decodeTrace(QStringLiteral("DecoderContext::seekAndDecode.seek-failed"),
                    QStringLiteral("file=%1 target=%2").arg(shortPath(m_path)).arg(targetFrame));
        return decodedFrames;
    }

    AVFrame* frame = av_frame_alloc();
    AVPacket* packet = av_packet_alloc();
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
                currentFrame = targetFrame;
            }
            
            m_lastDecodedFrame = currentFrame;
            decodedFrames.push_back(convertToFrame(frame, currentFrame));
            
            av_frame_unref(frame);
            if (currentFrame >= targetFrame) {
                goto done;
            }
        }
    }
    m_eof = true;
    
done:
    av_frame_free(&frame);
    av_packet_free(&packet);
    return decodedFrames;
}

bool DecoderContext::seekToKeyframe(int64_t targetFrame) {
    AVStream* stream = m_formatCtx->streams[m_videoStreamIndex];
    
    const double targetSeconds = targetFrame / qMax(1.0, m_info.fps);
    const int64_t targetUsec = qMax<int64_t>(0, qRound64(targetSeconds * AV_TIME_BASE));
    const int64_t targetTs = av_rescale_q(targetUsec, AVRational{1, AV_TIME_BASE}, stream->time_base);
    
    // Seek to the nearest decodable position at or before the target.
    const int ret = avformat_seek_file(m_formatCtx,
                                       m_videoStreamIndex,
                                       std::numeric_limits<int64_t>::min(),
                                       targetTs,
                                       targetTs,
                                       AVSEEK_FLAG_BACKWARD);
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

    // Note: We used to reject frames here when m_streamHasAlphaTag && !sourceHasAlpha,
    // but this caused transparent WebM files to fail decoding. The get_alpha_compatible_format
    // callback (set in initCodec) now requests alpha-capable formats from the decoder.
    // If we still get a non-alpha format, we proceed anyway as the alpha data may still
    // be present in the frame (e.g., VP8/VP9 with alpha in a separate plane).
    if (m_streamHasAlphaTag && !sourceHasAlpha && !m_reportedAlphaMismatch) {
        m_reportedAlphaMismatch = true;
        qWarning().noquote() << QStringLiteral(
            "Alpha-tagged stream is decoding without alpha-capable pixel format; attempting conversion anyway for %1 (fmt=%2)")
            .arg(shortPath(m_path))
            .arg(sourceDesc ? QString::fromUtf8(sourceDesc->name) : QStringLiteral("unknown"));
    }

    if (!m_swsCtx ||
        m_swsSourceFormat != sourceFormat ||
        m_swsSourceSize.width() != swFrame->width ||
        m_swsSourceSize.height() != swFrame->height) {
        m_swsCtx = sws_getCachedContext(m_swsCtx,
                                        swFrame->width,
                                        swFrame->height,
                                        sourceFormat,
                                        swFrame->width,
                                        swFrame->height,
                                        AV_PIX_FMT_RGBA,
                                        SWS_BILINEAR,
                                        nullptr,
                                        nullptr,
                                        nullptr);
        m_swsSourceFormat = sourceFormat;
        m_swsSourceSize = QSize(swFrame->width, swFrame->height);
    }

    if (!m_swsCtx) {
        if (tempFrame) av_frame_free(&tempFrame);
        return QImage();
    }
    
    if (!m_convertFrame ||
        m_convertFrameSize.width() != swFrame->width ||
        m_convertFrameSize.height() != swFrame->height ||
        m_convertFrame->format != AV_PIX_FMT_RGBA) {
        if (m_convertFrame) {
            av_frame_free(&m_convertFrame);
        }
        m_convertFrame = av_frame_alloc();
        if (!m_convertFrame) {
            if (tempFrame) av_frame_free(&tempFrame);
            return QImage();
        }
        m_convertFrame->format = AV_PIX_FMT_RGBA;
        m_convertFrame->width = swFrame->width;
        m_convertFrame->height = swFrame->height;
        if (av_frame_get_buffer(m_convertFrame, 32) < 0) {
            av_frame_free(&m_convertFrame);
            if (tempFrame) av_frame_free(&tempFrame);
            return QImage();
        }
        m_convertFrameSize = QSize(swFrame->width, swFrame->height);
    }

    if (av_frame_make_writable(m_convertFrame) < 0) {
        if (tempFrame) av_frame_free(&tempFrame);
        return QImage();
    }

    if (sws_scale(m_swsCtx,
                  swFrame->data,
                  swFrame->linesize,
                  0,
                  swFrame->height,
                  m_convertFrame->data,
                  m_convertFrame->linesize) <= 0) {
        if (tempFrame) av_frame_free(&tempFrame);
        return QImage();
    }

    QImage image(swFrame->width, swFrame->height, QImage::Format_RGBA8888);
    if (image.isNull()) {
        if (tempFrame) av_frame_free(&tempFrame);
        return QImage();
    }

    const int copyBytesPerRow = qMin(static_cast<int>(image.bytesPerLine()), m_convertFrame->linesize[0]);
    for (int y = 0; y < swFrame->height; ++y) {
        memcpy(image.scanLine(y), m_convertFrame->data[0] + (y * m_convertFrame->linesize[0]), copyBytesPerRow);
    }

    if (m_streamHasAlphaTag && !m_loggedAlphaProbe && !image.isNull()) {
        m_loggedAlphaProbe = true;
        const auto samplePixel = [&image](int x, int y) {
            const int sx = qBound(0, x, image.width() - 1);
            const int sy = qBound(0, y, image.height() - 1);
            const uchar* px = image.constScanLine(sy) + (sx * 4);
            return QStringLiteral("(%1,%2)=[r=%3 g=%4 b=%5 a=%6]")
                .arg(sx)
                .arg(sy)
                .arg(px[0])
                .arg(px[1])
                .arg(px[2])
                .arg(px[3]);
        };

        qDebug().noquote() << QStringLiteral(
            "[ALPHA] file=%1 size=%2x%3 samples: %4 %5 %6 %7 %8")
            .arg(shortPath(m_path))
            .arg(image.width())
            .arg(image.height())
            .arg(samplePixel(0, 0))
            .arg(samplePixel(image.width() / 2, image.height() / 2))
            .arg(samplePixel(image.width() - 1, 0))
            .arg(samplePixel(0, image.height() - 1))
            .arg(samplePixel(image.width() - 1, image.height() - 1));
        }

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
    QVector<std::function<void(FrameHandle)>> droppedCallbacks;
    QMutexLocker lock(&m_mutex);
    
    if (m_shutdown.load()) return false;

    const int visibleReserve = qBound(0, debugVisibleQueueReserve(), kMaxSize - 1);
    const int nonVisibleLimit = kMaxSize - visibleReserve;
    if (req.kind != DecodeRequestKind::Visible && static_cast<int>(m_queue.size()) >= nonVisibleLimit) {
        return false;
    }

    collectSupersededRequests(req, &droppedCallbacks);
    
    if (m_queue.size() >= kMaxSize) {
        // Try to drop a lower priority request
        bool dropped = false;
        for (int i = static_cast<int>(m_queue.size()) - 1; i >= 0; --i) {
            const bool kindFavored =
                req.kind == DecodeRequestKind::Visible && m_queue[i].kind != DecodeRequestKind::Visible;
            if (kindFavored || m_queue[i].priority < req.priority) {
                if (m_queue[i].callback) {
                    droppedCallbacks.push_back(std::move(m_queue[i].callback));
                }
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
    for (const auto& droppedCallback : droppedCallbacks) {
        if (droppedCallback) {
            droppedCallback(FrameHandle());
        }
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

QVector<DecodeRequest> DecodeQueue::takeSameFileVisibleRequests(const QString& path,
                                                                int64_t minimumFrameNumber,
                                                                int maxRequests) {
    QVector<DecodeRequest> requests;
    if (maxRequests <= 0) {
        return requests;
    }

    QMutexLocker lock(&m_mutex);
    for (auto it = m_queue.begin(); it != m_queue.end() && requests.size() < maxRequests;) {
        if (it->filePath != path ||
            it->kind != DecodeRequestKind::Visible ||
            it->frameNumber < minimumFrameNumber) {
            ++it;
            continue;
        }
        requests.push_back(std::move(*it));
        it = m_queue.erase(it);
    }
    return requests;
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

void DecodeQueue::collectSupersededRequests(const DecodeRequest& req,
                                            QVector<std::function<void(FrameHandle)>>* droppedCallbacks) {
    if (!droppedCallbacks) {
        return;
    }

    // During forward playback, newer same-file requests supersede older queued
    // targets. Keeping the newest target lets the decoder stay close to the
    // active frontier instead of spending queue time on already-obsolete work.
    for (int i = static_cast<int>(m_queue.size()) - 1; i >= 0; --i) {
        const DecodeRequest& queued = m_queue[i];
        if (queued.filePath != req.filePath) {
            continue;
        }
        if (queued.frameNumber >= req.frameNumber) {
            continue;
        }
        if (queued.priority > req.priority) {
            continue;
        }
        if (queued.kind == DecodeRequestKind::Visible && req.kind != DecodeRequestKind::Visible) {
            continue;
        }
        if (queued.callback) {
            droppedCallbacks->push_back(std::move(m_queue[i].callback));
        }
        m_queue.erase(m_queue.begin() + i);
    }
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
    }

    if (req.callback) {
        req.callback(frame);
    }

    emit frameDecoded(frame);
    decodeTrace(QStringLiteral("DecoderWorker::processRequest.end"),
                QStringLiteral("seq=%1 file=%2 frame=%3 kind=%4 null=%5 waitMs=%6")
                    .arg(req.sequenceId)
                    .arg(shortPath(req.filePath))
                    .arg(req.frameNumber)
                    .arg(static_cast<int>(req.kind))
                    .arg(frame.isNull())
                    .arg(decodeTraceMs() - startedAt));
}

void DecoderWorker::processVisibleBatch(const QVector<DecodeRequest>& requests, DecoderContext* ctx) {
    if (!ctx) {
        return;
    }

    int64_t highestTarget = -1;
    for (const DecodeRequest& request : requests) {
        if (!request.isExpired()) {
            highestTarget = qMax(highestTarget, request.frameNumber);
        }
    }
    if (highestTarget < 0) {
        for (const DecodeRequest& request : requests) {
            m_dropCount++;
            if (request.callback) {
                request.callback(FrameHandle());
            }
        }
        return;
    }

    const QVector<FrameHandle> decodedFrames = ctx->decodeThroughFrame(highestTarget);
    QHash<int64_t, FrameHandle> decodedByFrame;
    QVector<int64_t> decodedFrameNumbers;
    for (const FrameHandle& frame : decodedFrames) {
        if (frame.isNull()) {
            continue;
        }
        decodedByFrame.insert(frame.frameNumber(), frame);
        decodedFrameNumbers.push_back(frame.frameNumber());
        emit frameDecoded(frame);
    }
    std::sort(decodedFrameNumbers.begin(), decodedFrameNumbers.end());

    for (const DecodeRequest& request : requests) {
        if (request.isExpired()) {
            m_dropCount++;
            if (request.callback) {
                request.callback(FrameHandle());
            }
            continue;
        }

        FrameHandle deliveredFrame = decodedByFrame.value(request.frameNumber);
        if (deliveredFrame.isNull()) {
            int64_t bestFrameNumber = std::numeric_limits<int64_t>::max();
            int64_t bestDistance = std::numeric_limits<int64_t>::max();
            for (int64_t decodedFrameNumber : decodedFrameNumbers) {
                const int64_t distance = qAbs(decodedFrameNumber - request.frameNumber);
                if (distance > kPlaybackBatchFrameSlack) {
                    continue;
                }
                if (bestFrameNumber == std::numeric_limits<int64_t>::max() ||
                    distance < bestDistance ||
                    (distance == bestDistance && decodedFrameNumber > bestFrameNumber)) {
                    bestFrameNumber = decodedFrameNumber;
                    bestDistance = distance;
                }
            }
            if (bestFrameNumber != std::numeric_limits<int64_t>::max()) {
                deliveredFrame = decodedByFrame.value(bestFrameNumber);
                decodeTrace(QStringLiteral("DecoderWorker::processVisibleBatch.nearest"),
                            QStringLiteral("file=%1 requested=%2 delivered=%3 distance=%4")
                                .arg(shortPath(request.filePath))
                                .arg(request.frameNumber)
                                .arg(bestFrameNumber)
                                .arg(bestDistance));
            }
        }
        if (deliveredFrame.isNull()) {
            m_dropCount++;
        } else {
            m_decodeCount++;
        }

        if (request.callback) {
            request.callback(deliveredFrame);
        }
    }
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
    if (!m_workers.isEmpty()) {
        return true;
    }

    // Shard requests by file path so each file stays on one worker/context,
    // while different files can decode in parallel.
    const int workerCount = qBound(1, QThread::idealThreadCount(), 4);
    
    for (int i = 0; i < workerCount; ++i) {
        auto* queue = new DecodeQueue();
        auto* worker = new DecoderWorker(queue, m_budget);
        connect(worker, &DecoderWorker::frameDecoded, this, &AsyncDecoder::frameReady, Qt::QueuedConnection);
        connect(worker, &DecoderWorker::decodeError, this, &AsyncDecoder::error, Qt::QueuedConnection);
        m_queues.append(queue);
        worker->start();
        m_workers.append(worker);
    }
    
    qDebug() << "AsyncDecoder initialized with" << workerCount << "workers";
    return true;
}

void AsyncDecoder::shutdown() {
    for (DecodeQueue* queue : m_queues) {
        if (queue) {
            queue->shutdown();
        }
    }
    
    for (auto* worker : m_workers) {
        worker->stop();
        delete worker;
    }
    m_workers.clear();

    qDeleteAll(m_queues);
    m_queues.clear();
}

uint64_t AsyncDecoder::requestFrame(const QString& path,
                                    int64_t frameNumber,
                                    int priority,
                                    int timeoutMs,
                                    std::function<void(FrameHandle)> callback) {
    return requestFrame(path, frameNumber, priority, timeoutMs, DecodeRequestKind::Visible, std::move(callback));
}

uint64_t AsyncDecoder::requestFrame(const QString& path,
                                    int64_t frameNumber,
                                    int priority,
                                    int timeoutMs,
                                    DecodeRequestKind kind,
                                    std::function<void(FrameHandle)> callback) {
    uint64_t seqId = m_nextSequenceId++;
    
    DecodeRequest req;
    req.sequenceId = seqId;
    req.filePath = path;
    req.frameNumber = frameNumber;
    req.priority = priority;
    req.kind = kind;
    req.deadline = QDeadlineTimer(timeoutMs);
    req.callback = callback;
    req.submittedAt = QDateTime::currentMSecsSinceEpoch();
    DecodeQueue* queue = queueForPath(path);
    const int pendingBefore = totalPendingRequests();

    decodeTrace(QStringLiteral("AsyncDecoder::requestFrame"),
                QStringLiteral("seq=%1 file=%2 frame=%3 priority=%4 kind=%5 timeoutMs=%6 pending=%7")
                    .arg(seqId)
                    .arg(shortPath(path))
                    .arg(frameNumber)
                    .arg(priority)
                    .arg(static_cast<int>(kind))
                    .arg(timeoutMs)
                    .arg(pendingBefore));
    
    if (!queue || !queue->enqueue(req)) {
        // Queue full, return immediately with null
        decodeTrace(QStringLiteral("AsyncDecoder::requestFrame.queue-full"),
                    QStringLiteral("seq=%1 file=%2 frame=%3 kind=%4")
                        .arg(seqId)
                        .arg(shortPath(path))
                        .arg(frameNumber)
                        .arg(static_cast<int>(kind)));
        if (callback) {
            callback(FrameHandle());
        }
        return 0;
    }
    
    emit queuePressureChanged(totalPendingRequests());
    return seqId;
}

void AsyncDecoder::cancelForFile(const QString& path) {
    for (DecodeQueue* queue : m_queues) {
        if (queue) {
            queue->removeForFile(path);
        }
    }
}

void AsyncDecoder::cancelForFileBefore(const QString& path, int64_t frameNumber) {
    for (DecodeQueue* queue : m_queues) {
        if (queue) {
            queue->removeForFileBefore(path, frameNumber);
        }
    }
}

void AsyncDecoder::cancelAll() {
    for (DecodeQueue* queue : m_queues) {
        if (queue) {
            queue->clear();
        }
    }
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

int AsyncDecoder::totalPendingRequests() const {
    int total = 0;
    for (DecodeQueue* queue : m_queues) {
        if (queue) {
            total += queue->size();
        }
    }
    return total;
}

int AsyncDecoder::queueIndexForPath(const QString& path) const {
    if (m_queues.isEmpty()) {
        return -1;
    }
    return static_cast<int>(qHash(path) % static_cast<uint>(m_queues.size()));
}

DecodeQueue* AsyncDecoder::queueForPath(const QString& path) const {
    const int index = queueIndexForPath(path);
    if (index < 0 || index >= m_queues.size()) {
        return nullptr;
    }
    return m_queues[index];
}

} // namespace editor
