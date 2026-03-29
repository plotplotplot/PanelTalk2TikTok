#include "async_decoder.h"
#include "debug_controls.h"
#include "editor_shared.h"

#include <QDebug>
#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QImageReader>
#include <QGuiApplication>
#include <QFile>
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
constexpr size_t kMaxSequenceFrameCacheBytes = 192 * 1024 * 1024;
constexpr int kMaxSequenceFrameCacheEntries = 24;
constexpr int kWebpSequenceBatchAhead = 6;
constexpr int64_t kImageSequenceLaneShardSize = 8;

#if defined(__SANITIZE_ADDRESS__)
constexpr bool kAsanBuild = true;
#else
constexpr bool kAsanBuild = false;
#endif

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

bool linuxNvidiaDetected() {
#if defined(Q_OS_LINUX)
    return QFile::exists(QStringLiteral("/proc/driver/nvidia/version")) ||
           QFile::exists(QStringLiteral("/sys/module/nvidia"));
#else
    return false;
#endif
}

bool zeroCopyInteropSupportedForCurrentBuild() {
#if defined(Q_OS_LINUX)
    return linuxNvidiaDetected();
#else
    return false;
#endif
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
        if (image.format() != QImage::Format_ARGB32_Premultiplied) {
            image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        }
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
    if (!decodedImage.isNull() && decodedImage.format() != QImage::Format_ARGB32_Premultiplied) {
        decodedImage = decodedImage.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    }
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

void invokeRequestCallback(std::function<void(FrameHandle)> callback, FrameHandle frame) {
    if (!callback) {
        return;
    }

    QCoreApplication* app = QCoreApplication::instance();
    if (!app) {
        callback(frame);
        return;
    }

    QMetaObject::invokeMethod(app,
                              [callback = std::move(callback), frame]() mutable {
                                  if (callback) {
                                      callback(frame);
                                  }
                              },
                              Qt::QueuedConnection);
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
    m_sequenceFramePaths.clear();
    m_sequenceFrameCache.clear();
    m_sequenceFrameCacheUseOrder.clear();
    m_sequenceFrameCacheBytes = 0;
    m_sequenceFrameUseCounter = 0;
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
    m_isImageSequence = true;
    m_sequenceFramePaths = framePaths;
    m_sequenceUsesWebp = QFileInfo(framePaths.constFirst()).suffix().compare(QStringLiteral("webp"), Qt::CaseInsensitive) == 0;
    m_stillImage = image;
    cacheSequenceFrameImage(0, image);
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

QImage DecoderContext::loadCachedSequenceFrameImage(int64_t frameNumber) {
    if (m_sequenceFramePaths.isEmpty()) {
        return QImage();
    }

    const int64_t boundedFrame =
        qBound<int64_t>(0, frameNumber, static_cast<int64_t>(m_sequenceFramePaths.size() - 1));
    auto cached = m_sequenceFrameCache.find(boundedFrame);
    if (cached != m_sequenceFrameCache.end()) {
        m_sequenceFrameCacheUseOrder[boundedFrame] = ++m_sequenceFrameUseCounter;
        return cached.value();
    }

    QImage image = loadSequenceFrameImage(m_sequenceFramePaths, boundedFrame);
    if (image.isNull()) {
        return QImage();
    }

    if (m_sequenceUsesWebp) {
        cacheSequenceFrameImage(boundedFrame, image);
    }
    return image;
}

void DecoderContext::cacheSequenceFrameImage(int64_t frameNumber, const QImage& image) {
    if (!m_sequenceUsesWebp || image.isNull()) {
        return;
    }

    const size_t imageBytes = image.sizeInBytes();
    if (imageBytes == 0 || imageBytes > kMaxSequenceFrameCacheBytes) {
        return;
    }

    auto existing = m_sequenceFrameCache.find(frameNumber);
    if (existing != m_sequenceFrameCache.end()) {
        m_sequenceFrameCacheBytes -= existing.value().sizeInBytes();
    }

    m_sequenceFrameCache.insert(frameNumber, image);
    m_sequenceFrameCacheUseOrder.insert(frameNumber, ++m_sequenceFrameUseCounter);
    m_sequenceFrameCacheBytes += imageBytes;
    trimSequenceFrameCache();
}

void DecoderContext::trimSequenceFrameCache() {
    while (m_sequenceFrameCache.size() > kMaxSequenceFrameCacheEntries ||
           m_sequenceFrameCacheBytes > kMaxSequenceFrameCacheBytes) {
        int64_t oldestFrame = -1;
        quint64 oldestUse = std::numeric_limits<quint64>::max();
        for (auto it = m_sequenceFrameCacheUseOrder.cbegin(); it != m_sequenceFrameCacheUseOrder.cend(); ++it) {
            if (it.value() < oldestUse) {
                oldestUse = it.value();
                oldestFrame = it.key();
            }
        }
        if (oldestFrame < 0) {
            break;
        }

        auto cacheIt = m_sequenceFrameCache.find(oldestFrame);
        if (cacheIt != m_sequenceFrameCache.end()) {
            m_sequenceFrameCacheBytes -= cacheIt.value().sizeInBytes();
            m_sequenceFrameCache.erase(cacheIt);
        }
        m_sequenceFrameCacheUseOrder.remove(oldestFrame);
    }
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
    const DecodePreference decodePreference = debugDecodePreference();
    const bool zeroCopyPreferred =
        decodePreference == DecodePreference::Auto ||
        decodePreference == DecodePreference::HardwareZeroCopy;
    const bool zeroCopySupported =
        zeroCopyPreferred &&
        !headlessOffscreen &&
        !m_streamHasAlphaTag &&
        zeroCopyInteropSupportedForCurrentBuild();
    const bool allowHardware =
        decodePreference != DecodePreference::Software &&
        !headlessOffscreen &&
        !m_streamHasAlphaTag;
    const bool hardwareEnabled = allowHardware && initHardwareAccel(decoder);
    const bool softwareProResAsanWorkaround =
        kAsanBuild &&
        !hardwareEnabled &&
        stream->codecpar->codec_id == AV_CODEC_ID_PRORES;

    if (hardwareEnabled) {
        // Let FFmpeg choose sensible threading for hardware-backed decode.
        m_codecCtx->thread_count = 0;
        m_codecCtx->thread_type = FF_THREAD_FRAME;
    } else if (softwareProResAsanWorkaround) {
        // ASan currently trips inside libavcodec's internal ProRes worker
        // threads. Keep our app threading intact, but serialize this codec's
        // internal decode path in sanitizer builds so we can debug our code.
        m_codecCtx->thread_count = 1;
        m_codecCtx->thread_type = 0;
    } else {
        // Let software decoders use their normal parallel paths. Forcing
        // single-threaded decode here is not worth the stability cost, and
        // ProRes in particular behaves better with FFmpeg's default threading.
        m_codecCtx->thread_count = qBound(2, QThread::idealThreadCount(), 8);
        m_codecCtx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
        m_codecCtx->flags2 |= AV_CODEC_FLAG2_FAST;
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

    m_info.requestedDecodeMode = decodePreferenceToString(decodePreference);
    m_info.hardwareAccelerated = hardwareEnabled;
    m_info.decodePath = hardwareEnabled ? QStringLiteral("hardware")
                                        : QStringLiteral("software");
    m_info.interopPath = hardwareEnabled
                             ? (zeroCopySupported ? QStringLiteral("cuda_gl_nv12_copy_candidate")
                                                  : QStringLiteral("hardware_cpu_upload"))
                             : QStringLiteral("software");

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
        QImage image = loadCachedSequenceFrameImage(frameNumber);
        if (image.isNull()) {
            return FrameHandle();
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
        updateAccessTime();
        QVector<FrameHandle> frames;
        const int64_t maxFrame =
            static_cast<int64_t>(m_sequenceFramePaths.size() - 1);
        const int64_t boundedTarget = qBound<int64_t>(0, targetFrame, maxFrame);
        const int64_t endFrame = m_sequenceUsesWebp
                                     ? qMin<int64_t>(maxFrame, boundedTarget + kWebpSequenceBatchAhead)
                                     : boundedTarget;
        frames.reserve(static_cast<int>(endFrame - boundedTarget + 1));
        for (int64_t frameNumber = boundedTarget; frameNumber <= endFrame; ++frameNumber) {
            QImage image = loadCachedSequenceFrameImage(frameNumber);
            if (image.isNull()) {
                continue;
            }
            frames.push_back(FrameHandle::createCpuFrame(image, frameNumber, m_path));
        }
        m_lastDecodedFrame = boundedTarget;
        return frames;
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
    if (avFrame->format == m_hwPixFmt &&
        m_hwPixFmt != AV_PIX_FMT_NONE &&
        (debugDecodePreference() == DecodePreference::HardwareZeroCopy ||
         debugDecodePreference() == DecodePreference::Auto) &&
        avFrame->hw_frames_ctx) {
        auto* framesContext = reinterpret_cast<AVHWFramesContext*>(avFrame->hw_frames_ctx->data);
        const int swPixelFormat = framesContext ? framesContext->sw_format : AV_PIX_FMT_NONE;
        if (swPixelFormat == AV_PIX_FMT_NV12) {
            FrameHandle hardwareHandle =
                FrameHandle::createHardwareFrame(avFrame, frameNumber, m_path, swPixelFormat);
            if (!hardwareHandle.isNull()) {
                return hardwareHandle;
            }
        }
    }

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
// AsyncDecoder Implementation
// ============================================================================

struct AsyncDecoder::LaneState {
    struct FileDecodeState {
        std::atomic<int64_t> cancelBeforeFrame{std::numeric_limits<int64_t>::min()};
        std::atomic<uint64_t> generation{0};
        std::unique_ptr<DecoderContext> context;
    };

    explicit LaneState(int laneIndex)
        : index(laneIndex) {}

    int index = 0;
    mutable std::mutex mutex;
    std::condition_variable condition;
    std::deque<DecodeRequest> queue;
    QHash<QString, std::shared_ptr<FileDecodeState>> fileStates;
    std::unique_ptr<std::thread> thread;
    bool shuttingDown = false;
    bool running = false;
    int activeRequests = 0;
    static constexpr int kMaxContexts = 4;
};

AsyncDecoder::AsyncDecoder(QObject* parent) : QObject(parent) {
    m_budget = new MemoryBudget(this);
    setupTrimCallback();
}

AsyncDecoder::~AsyncDecoder() {
    shutdown();
}

bool AsyncDecoder::initialize() {
    if (m_initialized) {
        return true;
    }

    m_workerCount = qBound(2, QThread::idealThreadCount(), 6);
    m_lanes.reserve(m_workerCount);
    m_shuttingDown = false;
    for (int i = 0; i < m_workerCount; ++i) {
        auto lane = std::make_unique<LaneState>(i);
        startLane(lane.get());
        m_lanes.push_back(std::move(lane));
    }
    m_initialized = true;

    qDebug() << "AsyncDecoder initialized with" << m_workerCount << "workers";
    return true;
}

void AsyncDecoder::shutdown() {
    m_shuttingDown = true;
    for (const auto& lane : m_lanes) {
        stopLane(lane.get());
    }
    m_lanes.clear();
    m_initialized = false;
    m_workerCount = 0;
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
    LaneState* lane = laneForRequest(path, frameNumber);
    if (!lane || m_shuttingDown) {
        if (callback) {
            callback(FrameHandle());
        }
        return 0;
    }

    const uint64_t seqId = m_nextSequenceId++;
    DecodeRequest req;
    req.sequenceId = seqId;
    req.filePath = path;
    req.frameNumber = frameNumber;
    req.priority = priority;
    req.kind = kind;
    req.deadline = QDeadlineTimer(timeoutMs);
    req.callback = callback;
    req.submittedAt = QDateTime::currentMSecsSinceEpoch();

    QVector<std::function<void(FrameHandle)>> droppedCallbacks;
    int pendingBefore = 0;
    int pendingAfter = 0;
    bool accepted = false;

    {
        std::unique_lock<std::mutex> lock(lane->mutex);
        auto stateIt = lane->fileStates.find(path);
        if (stateIt == lane->fileStates.end()) {
            stateIt = lane->fileStates.insert(path, std::make_shared<LaneState::FileDecodeState>());
        }
        const std::shared_ptr<LaneState::FileDecodeState> state = stateIt.value();
        if (frameNumber < state->cancelBeforeFrame.load()) {
            state->cancelBeforeFrame.store(std::numeric_limits<int64_t>::min());
        }
        req.generation = state->generation.load();
        pendingBefore = static_cast<int>(lane->queue.size()) + lane->activeRequests;

        collectSupersededRequests(req, lane->queue, &droppedCallbacks);

        constexpr int kMaxPendingRequests = 128;
        const int visibleReserve = qBound(0, debugVisibleQueueReserve(), kMaxPendingRequests - 1);
        const int nonVisibleLimit = kMaxPendingRequests - visibleReserve;
        if (req.kind != DecodeRequestKind::Visible &&
            static_cast<int>(lane->queue.size()) >= nonVisibleLimit) {
            accepted = false;
        } else {
            if (static_cast<int>(lane->queue.size()) >= kMaxPendingRequests) {
                bool dropped = false;
                for (auto it = lane->queue.end(); it != lane->queue.begin();) {
                    --it;
                    const bool kindFavored =
                        req.kind == DecodeRequestKind::Visible && it->kind != DecodeRequestKind::Visible;
                    if (kindFavored || it->priority < req.priority) {
                        if (it->callback) {
                            droppedCallbacks.push_back(std::move(it->callback));
                        }
                        lane->queue.erase(it);
                        dropped = true;
                        break;
                    }
                }
                if (!dropped) {
                    accepted = false;
                } else {
                    insertByPriority(lane->queue, req);
                    accepted = true;
                }
            } else {
                insertByPriority(lane->queue, req);
                accepted = true;
            }
        }
        pendingAfter = static_cast<int>(lane->queue.size()) + lane->activeRequests;
        if (accepted) {
            lane->condition.notify_one();
        }
    }

    decodeTrace(QStringLiteral("AsyncDecoder::requestFrame"),
                QStringLiteral("seq=%1 file=%2 frame=%3 priority=%4 kind=%5 timeoutMs=%6 pending=%7 lane=%8")
                    .arg(seqId)
                    .arg(shortPath(path))
                    .arg(frameNumber)
                    .arg(priority)
                    .arg(static_cast<int>(kind))
                    .arg(timeoutMs)
                    .arg(pendingBefore)
                    .arg(lane->index));

    for (const auto& droppedCallback : droppedCallbacks) {
        if (droppedCallback) {
            invokeRequestCallback(droppedCallback, FrameHandle());
        }
    }

    if (!accepted) {
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
    const std::vector<LaneState*> lanes = lanesForPath(path);
    if (lanes.empty()) {
        return;
    }

    QVector<std::function<void(FrameHandle)>> callbacks;
    for (LaneState* lane : lanes) {
        if (!lane) {
            continue;
        }
        std::unique_lock<std::mutex> lock(lane->mutex);
        auto stateIt = lane->fileStates.find(path);
        if (stateIt == lane->fileStates.end()) {
            stateIt = lane->fileStates.insert(path, std::make_shared<LaneState::FileDecodeState>());
        }
        stateIt.value()->generation.fetch_add(1);
        stateIt.value()->cancelBeforeFrame.store(std::numeric_limits<int64_t>::min());

        for (auto it = lane->queue.begin(); it != lane->queue.end();) {
            if (it->filePath == path) {
                if (it->callback) {
                    callbacks.push_back(std::move(it->callback));
                }
                it = lane->queue.erase(it);
            } else {
                ++it;
            }
        }
    }

    for (const auto& callback : callbacks) {
        if (callback) {
            invokeRequestCallback(callback, FrameHandle());
        }
    }
    emit queuePressureChanged(totalPendingRequests());
}

void AsyncDecoder::cancelForFileBefore(const QString& path, int64_t frameNumber) {
    const std::vector<LaneState*> lanes = lanesForPath(path);
    if (lanes.empty()) {
        return;
    }

    QVector<std::function<void(FrameHandle)>> callbacks;
    for (LaneState* lane : lanes) {
        if (!lane) {
            continue;
        }
        std::unique_lock<std::mutex> lock(lane->mutex);
        auto stateIt = lane->fileStates.find(path);
        if (stateIt == lane->fileStates.end()) {
            stateIt = lane->fileStates.insert(path, std::make_shared<LaneState::FileDecodeState>());
        }
        const auto& state = stateIt.value();
        state->cancelBeforeFrame.store(qMax(state->cancelBeforeFrame.load(), frameNumber));

        for (auto it = lane->queue.begin(); it != lane->queue.end();) {
            if (it->filePath == path && it->frameNumber < frameNumber) {
                if (it->callback) {
                    callbacks.push_back(std::move(it->callback));
                }
                it = lane->queue.erase(it);
            } else {
                ++it;
            }
        }
    }

    for (const auto& callback : callbacks) {
        if (callback) {
            invokeRequestCallback(callback, FrameHandle());
        }
    }
    emit queuePressureChanged(totalPendingRequests());
}

void AsyncDecoder::cancelAll() {
    QVector<std::function<void(FrameHandle)>> callbacks;
    for (const auto& lane : m_lanes) {
        std::unique_lock<std::mutex> lock(lane->mutex);
        for (auto& state : lane->fileStates) {
            state->generation.fetch_add(1);
            state->cancelBeforeFrame.store(std::numeric_limits<int64_t>::min());
        }
        for (DecodeRequest& request : lane->queue) {
            if (request.callback) {
                callbacks.push_back(std::move(request.callback));
            }
        }
        lane->queue.clear();
    }

    for (const auto& callback : callbacks) {
        if (callback) {
            invokeRequestCallback(callback, FrameHandle());
        }
    }
    emit queuePressureChanged(totalPendingRequests());
}

VideoStreamInfo AsyncDecoder::getVideoInfo(const QString& path) {
    QMutexLocker lock(&m_infoCacheMutex);
    const QString requestedDecodeMode = decodePreferenceToString(debugDecodePreference());

    auto it = m_infoCache.find(path);
    if (it != m_infoCache.end() && it.value().requestedDecodeMode == requestedDecodeMode) {
        return it.value();
    }

    DecoderContext ctx(path);
    if (ctx.initialize()) {
        VideoStreamInfo info = ctx.info();
        info.requestedDecodeMode = requestedDecodeMode;
        m_infoCache[path] = info;
        return info;
    }

    return VideoStreamInfo();
}

void AsyncDecoder::preloadFile(const QString& path) {
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
    QMutexLocker lock(&m_infoCacheMutex);
}

int AsyncDecoder::totalPendingRequests() const {
    int total = 0;
    for (const auto& lane : m_lanes) {
        std::unique_lock<std::mutex> lock(lane->mutex);
        total += static_cast<int>(lane->queue.size()) + lane->activeRequests;
    }
    return total;
}

int AsyncDecoder::laneIndexForRequest(const QString& path, int64_t frameNumber) const {
    if (m_lanes.empty()) {
        return -1;
    }
    const uint laneCount = static_cast<uint>(m_lanes.size());
    const uint baseHash = qHash(path);
    if (!isImageSequencePath(path) || laneCount == 1) {
        return static_cast<int>(baseHash % laneCount);
    }

    const int64_t shard = qMax<int64_t>(0, frameNumber) / kImageSequenceLaneShardSize;
    return static_cast<int>((baseHash + static_cast<uint>(shard)) % laneCount);
}

AsyncDecoder::LaneState* AsyncDecoder::laneForRequest(const QString& path, int64_t frameNumber) const {
    const int index = laneIndexForRequest(path, frameNumber);
    if (index < 0 || index >= m_lanes.size()) {
        return nullptr;
    }
    return m_lanes[index].get();
}

std::vector<AsyncDecoder::LaneState*> AsyncDecoder::lanesForPath(const QString& path) const {
    std::vector<LaneState*> lanes;
    if (m_lanes.empty()) {
        return lanes;
    }
    if (!isImageSequencePath(path)) {
        if (LaneState* lane = laneForRequest(path, 0)) {
            lanes.push_back(lane);
        }
        return lanes;
    }

    lanes.reserve(m_lanes.size());
    for (const auto& lane : m_lanes) {
        lanes.push_back(lane.get());
    }
    return lanes;
}

void AsyncDecoder::startLane(LaneState* lane) {
    if (!lane || lane->thread) {
        return;
    }
    lane->running = true;
    lane->thread = std::make_unique<std::thread>([this, lane]() {
        runLane(lane);
    });
}

void AsyncDecoder::stopLane(LaneState* lane) {
    if (!lane) {
        return;
    }
    {
        std::unique_lock<std::mutex> lock(lane->mutex);
        lane->shuttingDown = true;
        lane->condition.notify_all();
    }
    if (lane->thread && lane->thread->joinable()) {
        lane->thread->join();
    }
    lane->thread.reset();
    lane->running = false;
}

void AsyncDecoder::runLane(LaneState* lane) {
    while (true) {
        DecodeRequest request;
        std::shared_ptr<LaneState::FileDecodeState> state;
        {
            std::unique_lock<std::mutex> lock(lane->mutex);
            lane->condition.wait(lock, [lane]() {
                return lane->shuttingDown || !lane->queue.empty();
            });
            if (lane->shuttingDown) {
                return;
            }

            request = std::move(lane->queue.front());
            lane->queue.pop_front();
            ++lane->activeRequests;

            auto stateIt = lane->fileStates.find(request.filePath);
            if (stateIt == lane->fileStates.end()) {
                stateIt = lane->fileStates.insert(request.filePath, std::make_shared<LaneState::FileDecodeState>());
            }
            state = stateIt.value();
        }

        const qint64 startedAt = decodeTraceMs();
        decodeTrace(QStringLiteral("AsyncDecoder::runLane.begin"),
                    QStringLiteral("lane=%1 seq=%2 file=%3 frame=%4 priority=%5 thread=%6")
                        .arg(lane->index)
                        .arg(request.sequenceId)
                        .arg(shortPath(request.filePath))
                        .arg(request.frameNumber)
                        .arg(request.priority)
                        .arg(reinterpret_cast<quintptr>(QThread::currentThreadId()), 0, 16));

        FrameHandle frame;
        QVector<FrameHandle> decodedFrames;
        QString errorMessage;
        const bool cancelled =
            request.generation != state->generation.load() ||
            request.frameNumber < state->cancelBeforeFrame.load();

        if (!request.isExpired() && !cancelled) {
            if (!state->context) {
                state->context = std::make_unique<DecoderContext>(request.filePath);
                if (!state->context->initialize()) {
                    errorMessage = QStringLiteral("Failed to create decoder context");
                    state->context.reset();
                }
            }
            if (state->context) {
                if (state->context->supportsSequenceBatchDecode() &&
                    request.kind != DecodeRequestKind::Preload) {
                    decodedFrames = state->context->decodeThroughFrame(request.frameNumber);
                    for (const FrameHandle& decodedFrame : decodedFrames) {
                        if (!decodedFrame.isNull() && decodedFrame.frameNumber() == request.frameNumber) {
                            frame = decodedFrame;
                            break;
                        }
                    }
                    if (frame.isNull() && !decodedFrames.isEmpty()) {
                        frame = decodedFrames.constFirst();
                    }
                } else {
                    frame = state->context->decodeFrame(request.frameNumber);
                }
            }
        }

        if (request.isExpired() ||
            request.generation != state->generation.load() ||
            request.frameNumber < state->cancelBeforeFrame.load()) {
            frame = FrameHandle();
        }

        invokeRequestCallback(std::move(request.callback), frame);
        QMetaObject::invokeMethod(this,
                                  [this, frame, decodedFrames, path = request.filePath, errorMessage]() {
                                      if (!decodedFrames.isEmpty()) {
                                          for (const FrameHandle& decodedFrame : decodedFrames) {
                                              emit frameReady(decodedFrame);
                                          }
                                      } else {
                                          emit frameReady(frame);
                                      }
                                      if (!errorMessage.isEmpty()) {
                                          emit error(path, errorMessage);
                                      }
                                  },
                                  Qt::QueuedConnection);

        decodeTrace(QStringLiteral("AsyncDecoder::runLane.end"),
                    QStringLiteral("lane=%1 seq=%2 file=%3 frame=%4 null=%5 waitMs=%6")
                        .arg(lane->index)
                        .arg(request.sequenceId)
                        .arg(shortPath(request.filePath))
                        .arg(request.frameNumber)
                        .arg(frame.isNull())
                        .arg(decodeTraceMs() - startedAt));

        {
            std::unique_lock<std::mutex> lock(lane->mutex);
            lane->activeRequests = qMax(0, lane->activeRequests - 1);

            if (lane->fileStates.size() > LaneState::kMaxContexts) {
                qint64 oldestTime = std::numeric_limits<qint64>::max();
                QString oldestPath;
                for (auto it = lane->fileStates.begin(); it != lane->fileStates.end(); ++it) {
                    if (!it.value()->context) {
                        oldestPath = it.key();
                        break;
                    }
                    const qint64 accessTime = it.value()->context->lastAccessTime();
                    if (accessTime < oldestTime) {
                        oldestTime = accessTime;
                        oldestPath = it.key();
                    }
                }
                if (!oldestPath.isEmpty()) {
                    lane->fileStates.remove(oldestPath);
                }
            }
        }
        emit queuePressureChanged(totalPendingRequests());
    }
}

void AsyncDecoder::insertByPriority(std::deque<DecodeRequest>& queue, const DecodeRequest& req) {
    auto insertAt = queue.begin();
    for (; insertAt != queue.end(); ++insertAt) {
        if (insertAt->priority < req.priority) {
            break;
        }
    }
    queue.insert(insertAt, req);
}

void AsyncDecoder::collectSupersededRequests(const DecodeRequest& req,
                                             std::deque<DecodeRequest>& queue,
                                             QVector<std::function<void(FrameHandle)>>* droppedCallbacks) {
    if (!droppedCallbacks) {
        return;
    }

    for (int i = static_cast<int>(queue.size()) - 1; i >= 0; --i) {
        const DecodeRequest& queued = queue[static_cast<size_t>(i)];
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
        if (queue[static_cast<size_t>(i)].callback) {
            droppedCallbacks->push_back(std::move(queue[static_cast<size_t>(i)].callback));
        }
        queue.erase(queue.begin() + i);
    }
}

} // namespace editor
