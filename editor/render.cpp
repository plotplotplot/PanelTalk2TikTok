#include "render.h"

#include "async_decoder.h"
#include "debug_controls.h"
#include "gl_frame_texture_shared.h"
#include "media_pipeline_shared.h"
#include "render_cpu_fallback.h"

#include <QDir>
#include <QDateTime>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QOffscreenSurface>
#include <QOpenGLBuffer>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QPainter>
#include <QTextDocument>
#include <QSurfaceFormat>
#include <QElapsedTimer>
#include <QTimer>

#include <algorithm>
#include <memory>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libavutil/hwcontext.h>
#ifdef EDITOR_HAS_CUDA
#include <libavutil/hwcontext_cuda.h>
#endif
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#ifdef EDITOR_HAS_CUDA
#include <cuda.h>
#include <cudaGL.h>
#endif

namespace {
constexpr int kRenderAudioSampleRate = 48000;
constexpr int kRenderAudioChannels = 2;

struct VideoEncoderChoice {
    QString label;
    AVPixelFormat pixelFormat = AV_PIX_FMT_YUV420P;
};

struct RenderClipStageStats {
    QString id;
    QString label;
    int64_t frames = 0;
    qint64 decodeMs = 0;
    qint64 textureMs = 0;
    qint64 compositeMs = 0;
};

struct RenderFrameStageStats {
    int64_t timelineFrame = -1;
    int segmentIndex = 0;
    qint64 renderMs = 0;
    qint64 decodeMs = 0;
    qint64 textureMs = 0;
    qint64 readbackMs = 0;
    qint64 convertMs = 0;
};

struct RenderAsyncFrameKey {
    QString path;
    int64_t frameNumber = -1;

    bool operator==(const RenderAsyncFrameKey& other) const {
        return path == other.path && frameNumber == other.frameNumber;
    }
};

size_t qHash(const RenderAsyncFrameKey& key, size_t seed = 0) {
    return qHashMulti(seed, key.path, key.frameNumber);
}

bool isHardwareEncoderLabel(const QString& codecLabel) {
    const QString lowered = codecLabel.toLower();
    return lowered.contains(QStringLiteral("nvenc")) ||
           lowered.contains(QStringLiteral("qsv")) ||
           lowered.contains(QStringLiteral("vaapi")) ||
           lowered.contains(QStringLiteral("videotoolbox")) ||
           lowered.contains(QStringLiteral("amf")) ||
           lowered.contains(QStringLiteral("omx")) ||
           lowered.contains(QStringLiteral("mediacodec"));
}

void recordRenderSkip(QJsonArray* skippedClips,
                      QJsonObject* skippedReasonCounts,
                      const TimelineClip& clip,
                      const QString& reason,
                      int64_t timelineFrame,
                      int64_t localFrame = -1) {
    if (skippedClips) {
        QJsonObject obj{
            {QStringLiteral("id"), clip.id},
            {QStringLiteral("label"), clip.label},
            {QStringLiteral("reason"), reason},
            {QStringLiteral("timeline_frame"), static_cast<qint64>(timelineFrame)}
        };
        if (localFrame >= 0) {
            obj.insert(QStringLiteral("local_frame"), static_cast<qint64>(localFrame));
        }
        skippedClips->push_back(obj);
    }
    if (skippedReasonCounts) {
        skippedReasonCounts->insert(reason, skippedReasonCounts->value(reason).toInt(0) + 1);
    }
}

void accumulateClipStageStats(QHash<QString, RenderClipStageStats>* clipStageStats,
                              const TimelineClip& clip,
                              qint64 decodeMs,
                              qint64 textureMs,
                              qint64 compositeMs) {
    if (!clipStageStats) {
        return;
    }
    RenderClipStageStats& stats = (*clipStageStats)[clip.id];
    if (stats.id.isEmpty()) {
        stats.id = clip.id;
        stats.label = clip.label;
    }
    ++stats.frames;
    stats.decodeMs += decodeMs;
    stats.textureMs += textureMs;
    stats.compositeMs += compositeMs;
}

QJsonObject buildRenderStageTable(const QHash<QString, RenderClipStageStats>& clipStageStats,
                                  qint64 totalRenderStageMs,
                                  int64_t completedFrames) {
    QJsonArray columns{
        QStringLiteral("clip"),
        QStringLiteral("frames"),
        QStringLiteral("decode_ms"),
        QStringLiteral("decode_ms_per_frame"),
        QStringLiteral("texture_ms"),
        QStringLiteral("texture_ms_per_frame"),
        QStringLiteral("composite_ms"),
        QStringLiteral("composite_ms_per_frame"),
        QStringLiteral("stage_ms"),
        QStringLiteral("stage_ms_per_frame"),
        QStringLiteral("stage_share_pct")
    };

    QVector<RenderClipStageStats> rows = clipStageStats.values().toVector();
    std::sort(rows.begin(), rows.end(), [](const RenderClipStageStats& a, const RenderClipStageStats& b) {
        const qint64 aTotal = a.decodeMs + a.textureMs + a.compositeMs;
        const qint64 bTotal = b.decodeMs + b.textureMs + b.compositeMs;
        if (aTotal != bTotal) {
            return aTotal > bTotal;
        }
        return a.label < b.label;
    });

    QJsonArray jsonRows;
    for (const RenderClipStageStats& stats : rows) {
        const qint64 stageMs = stats.decodeMs + stats.textureMs + stats.compositeMs;
        const double frames = static_cast<double>(qMax<int64_t>(1, stats.frames));
        const double sharePct = totalRenderStageMs > 0
            ? (100.0 * static_cast<double>(stageMs) / static_cast<double>(totalRenderStageMs))
            : 0.0;
        jsonRows.push_back(QJsonObject{
            {QStringLiteral("id"), stats.id},
            {QStringLiteral("clip"), stats.label},
            {QStringLiteral("frames"), static_cast<qint64>(stats.frames)},
            {QStringLiteral("decode_ms"), stats.decodeMs},
            {QStringLiteral("decode_ms_per_frame"), static_cast<double>(stats.decodeMs) / frames},
            {QStringLiteral("texture_ms"), stats.textureMs},
            {QStringLiteral("texture_ms_per_frame"), static_cast<double>(stats.textureMs) / frames},
            {QStringLiteral("composite_ms"), stats.compositeMs},
            {QStringLiteral("composite_ms_per_frame"), static_cast<double>(stats.compositeMs) / frames},
            {QStringLiteral("stage_ms"), stageMs},
            {QStringLiteral("stage_ms_per_frame"), static_cast<double>(stageMs) / frames},
            {QStringLiteral("stage_share_pct"), sharePct}
        });
    }

    qint64 attributedStageMs = 0;
    for (const RenderClipStageStats& stats : rows) {
        attributedStageMs += stats.decodeMs + stats.textureMs + stats.compositeMs;
    }
    const qint64 overheadMs = qMax<qint64>(0, totalRenderStageMs - attributedStageMs);
    if (overheadMs > 0) {
        const double frames = static_cast<double>(qMax<int64_t>(1, completedFrames));
        const double sharePct = totalRenderStageMs > 0
            ? (100.0 * static_cast<double>(overheadMs) / static_cast<double>(totalRenderStageMs))
            : 0.0;
        jsonRows.push_back(QJsonObject{
            {QStringLiteral("id"), QStringLiteral("__frame_overhead__")},
            {QStringLiteral("clip"), QStringLiteral("__frame_overhead__")},
            {QStringLiteral("frames"), static_cast<qint64>(completedFrames)},
            {QStringLiteral("decode_ms"), static_cast<qint64>(0)},
            {QStringLiteral("decode_ms_per_frame"), 0.0},
            {QStringLiteral("texture_ms"), static_cast<qint64>(0)},
            {QStringLiteral("texture_ms_per_frame"), 0.0},
            {QStringLiteral("composite_ms"), overheadMs},
            {QStringLiteral("composite_ms_per_frame"), static_cast<double>(overheadMs) / frames},
            {QStringLiteral("stage_ms"), overheadMs},
            {QStringLiteral("stage_ms_per_frame"), static_cast<double>(overheadMs) / frames},
            {QStringLiteral("stage_share_pct"), sharePct}
        });
    }

    return QJsonObject{
        {QStringLiteral("columns"), columns},
        {QStringLiteral("rows"), jsonRows}
    };
}

void recordWorstFrame(QVector<RenderFrameStageStats>* worstFrames,
                      const RenderFrameStageStats& stats,
                      int maxEntries = 8) {
    if (!worstFrames) {
        return;
    }
    worstFrames->push_back(stats);
    std::sort(worstFrames->begin(), worstFrames->end(), [](const RenderFrameStageStats& a, const RenderFrameStageStats& b) {
        if (a.renderMs != b.renderMs) {
            return a.renderMs > b.renderMs;
        }
        return a.timelineFrame > b.timelineFrame;
    });
    if (worstFrames->size() > maxEntries) {
        worstFrames->resize(maxEntries);
    }
}

QJsonObject buildWorstFrameTable(const QVector<RenderFrameStageStats>& worstFrames) {
    QJsonArray columns{
        QStringLiteral("timeline_frame"),
        QStringLiteral("segment_index"),
        QStringLiteral("render_ms"),
        QStringLiteral("decode_ms"),
        QStringLiteral("texture_ms"),
        QStringLiteral("readback_ms"),
        QStringLiteral("convert_ms")
    };

    QJsonArray rows;
    for (const RenderFrameStageStats& stats : worstFrames) {
        rows.push_back(QJsonObject{
            {QStringLiteral("timeline_frame"), static_cast<qint64>(stats.timelineFrame)},
            {QStringLiteral("segment_index"), stats.segmentIndex},
            {QStringLiteral("render_ms"), stats.renderMs},
            {QStringLiteral("decode_ms"), stats.decodeMs},
            {QStringLiteral("texture_ms"), stats.textureMs},
            {QStringLiteral("readback_ms"), stats.readbackMs},
            {QStringLiteral("convert_ms"), stats.convertMs}
        });
    }

    return QJsonObject{
        {QStringLiteral("columns"), columns},
        {QStringLiteral("rows"), rows}
    };
}

void enqueueRenderSequenceLookahead(const RenderRequest& request,
                                    int64_t timelineFrame,
                                    const QVector<TimelineClip>& orderedClips,
                                    editor::AsyncDecoder* asyncDecoder,
                                    const QHash<RenderAsyncFrameKey, editor::FrameHandle>& asyncFrameCache) {
    if (!asyncDecoder || !editor::debugLeadPrefetchEnabled()) {
        return;
    }

    const int lookaheadFrames = qMax(editor::debugLeadPrefetchCount(),
                                     editor::debugPlaybackWindowAhead());
    QVector<editor::SequencePrefetchClip> sequenceClips;
    sequenceClips.reserve(orderedClips.size());
    for (const TimelineClip& clip : orderedClips) {
        sequenceClips.push_back(editor::SequencePrefetchClip{clip, clip.filePath});
    }
    const QVector<int64_t> futureTimelineFrames =
        editor::collectLookaheadTimelineFrames(timelineFrame,
                                               lookaheadFrames,
                                               1,
                                               {});
    for (int offset = 0; offset < futureTimelineFrames.size(); ++offset) {
        const int64_t futureTimelineFrame = futureTimelineFrames[offset];
        const int priority = qMax(8, 128 - (offset * 4));
        const QVector<editor::SequencePrefetchRequest> requests =
            editor::collectSequencePrefetchRequestsAtTimelineFrame(sequenceClips,
                                                                   static_cast<qreal>(futureTimelineFrame),
                                                                   request.renderSyncMarkers,
                                                                   false,
                                                                   priority);
        for (const editor::SequencePrefetchRequest& prefetch : requests) {
            const RenderAsyncFrameKey key{prefetch.decodePath, prefetch.sourceFrame};
            if (asyncFrameCache.contains(key)) {
                continue;
            }
            asyncDecoder->requestFrame(prefetch.decodePath,
                                       prefetch.sourceFrame,
                                       prefetch.priority,
                                       30000,
                                       editor::DecodeRequestKind::Prefetch,
                                       {});
        }
    }
}

void prewarmRenderSequenceSegment(const RenderRequest& request,
                                  int64_t segmentStartFrame,
                                  int64_t segmentEndFrame,
                                  const QVector<TimelineClip>& orderedClips,
                                  editor::AsyncDecoder* asyncDecoder,
                                  const QHash<RenderAsyncFrameKey, editor::FrameHandle>& asyncFrameCache) {
    if (!asyncDecoder || !editor::debugLeadPrefetchEnabled()) {
        return;
    }

    const int prewarmFrames = qMax(editor::debugPlaybackWindowAhead() * 2,
                                   editor::debugLeadPrefetchCount() * 4);
    const int lookaheadFrames = static_cast<int>(qMin<int64_t>(segmentEndFrame - segmentStartFrame + 1,
                                                               qMax<int64_t>(1, prewarmFrames)));
    QVector<editor::SequencePrefetchClip> sequenceClips;
    sequenceClips.reserve(orderedClips.size());
    for (const TimelineClip& clip : orderedClips) {
        sequenceClips.push_back(editor::SequencePrefetchClip{clip, clip.filePath});
    }
    const QVector<int64_t> prewarmTimelineFrames =
        editor::collectLookaheadTimelineFrames(segmentStartFrame - 1,
                                               lookaheadFrames,
                                               1,
                                               {});
    for (int64_t prewarmTimelineFrame : prewarmTimelineFrames) {
        const QVector<editor::SequencePrefetchRequest> requests =
            editor::collectSequencePrefetchRequestsAtTimelineFrame(sequenceClips,
                                                                   static_cast<qreal>(prewarmTimelineFrame),
                                                                   request.renderSyncMarkers,
                                                                   false,
                                                                   192);
        for (const editor::SequencePrefetchRequest& prefetch : requests) {
            const RenderAsyncFrameKey key{prefetch.decodePath, prefetch.sourceFrame};
            if (asyncFrameCache.contains(key)) {
                continue;
            }
            asyncDecoder->requestFrame(prefetch.decodePath,
                                       prefetch.sourceFrame,
                                       prefetch.priority,
                                       30000,
                                       editor::DecodeRequestKind::Prefetch,
                                       {});
        }
    }
}

editor::FrameHandle decodeRenderFrame(const QString& path,
                                      int64_t frameNumber,
                                      QHash<QString, editor::DecoderContext*>& decoders,
                                      editor::AsyncDecoder* asyncDecoder,
                                      QHash<RenderAsyncFrameKey, editor::FrameHandle>* asyncFrameCache) {
    if (isImageSequencePath(path) && asyncDecoder && asyncFrameCache) {
        const RenderAsyncFrameKey cacheKey{path, frameNumber};
        auto cachedIt = asyncFrameCache->find(cacheKey);
        if (cachedIt != asyncFrameCache->end()) {
            return cachedIt.value();
        }

        editor::FrameHandle result;
        bool completed = false;
        QEventLoop loop;
        QTimer timeoutTimer;
        timeoutTimer.setSingleShot(true);
        QObject::connect(&timeoutTimer, &QTimer::timeout, &loop, [&]() {
            completed = true;
            loop.quit();
        });

        asyncDecoder->requestFrame(path,
                                   frameNumber,
                                   255,
                                   30000,
                                   editor::DecodeRequestKind::Visible,
                                   [&](editor::FrameHandle frame) {
                                       result = frame;
                                       if (!result.isNull()) {
                                           asyncFrameCache->insert(cacheKey, result);
                                       }
                                       completed = true;
                                       loop.quit();
                                   });
        for (int64_t prefetchFrame = frameNumber + 1; prefetchFrame <= frameNumber + 6; ++prefetchFrame) {
            const RenderAsyncFrameKey prefetchKey{path, prefetchFrame};
            if (!asyncFrameCache->contains(prefetchKey)) {
                asyncDecoder->requestFrame(path,
                                           prefetchFrame,
                                           64,
                                           30000,
                                           editor::DecodeRequestKind::Prefetch,
                                           {});
            }
        }

        timeoutTimer.start(30000);
        while (!completed) {
            loop.exec(QEventLoop::ExcludeUserInputEvents);
        }

        if (!result.isNull()) {
            asyncFrameCache->insert(cacheKey, result);
        }
        return result;
    }

    auto it = decoders.find(path);
    if (it == decoders.end()) {
        editor::DecoderContext* ctx = new editor::DecoderContext(path);
        if (!ctx->initialize()) {
            delete ctx;
            return editor::FrameHandle();
        }
        it = decoders.insert(path, ctx);
    }
    return it.value()->decodeFrame(frameNumber);
}

QString avErrToString(int errnum) {
    char errbuf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(errnum, errbuf, AV_ERROR_MAX_STRING_SIZE);
    return QString::fromUtf8(errbuf);
}

QRect fitRect(const QSize& source, const QSize& bounds) {
    if (source.isEmpty() || bounds.isEmpty()) {
        return QRect(QPoint(0, 0), bounds);
    }

    QSize scaled = source;
    scaled.scale(bounds, Qt::KeepAspectRatio);
    const QPoint topLeft((bounds.width() - scaled.width()) / 2,
                         (bounds.height() - scaled.height()) / 2);
    return QRect(topLeft, scaled);
}

TranscriptOverlayLayout transcriptOverlayLayoutForFrame(const TimelineClip& clip,
                                                        int64_t timelineFrame,
                                                        const QVector<RenderSyncMarker>& markers,
                                                        QHash<QString, QVector<TranscriptSection>>& transcriptCache) {
    if (!(clip.mediaType == ClipMediaType::Audio && clip.transcriptOverlay.enabled)) {
        return {};
    }
    const QString cacheKey = clip.filePath;
    if (!transcriptCache.contains(cacheKey)) {
        transcriptCache.insert(cacheKey, loadTranscriptSections(transcriptWorkingPathForClipFile(clip.filePath)));
    }
    const QVector<TranscriptSection>& sections = transcriptCache.value(cacheKey);
    if (sections.isEmpty()) {
        return {};
    }
    const int64_t sourceFrame = sourceFrameForClipAtTimelinePosition(clip,
                                                                     static_cast<qreal>(timelineFrame),
                                                                     markers);
    for (const TranscriptSection& section : sections) {
        if (sourceFrame < section.startFrame) {
            return {};
        }
        if (sourceFrame <= section.endFrame) {
            return layoutTranscriptSection(section,
                                           sourceFrame,
                                           clip.transcriptOverlay.maxCharsPerLine,
                                           clip.transcriptOverlay.maxLines,
                                           clip.transcriptOverlay.autoScroll);
        }
    }
    return {};
}

void renderTranscriptOverlays(QImage* canvas,
                              const RenderRequest& request,
                              int64_t timelineFrame,
                              const QVector<TimelineClip>& orderedClips,
                              QHash<QString, QVector<TranscriptSection>>& transcriptCache) {
    if (!canvas || canvas->isNull()) {
        return;
    }

    QPainter painter(canvas);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    for (const TimelineClip& clip : orderedClips) {
        if (timelineFrame < clip.startFrame || timelineFrame >= clip.startFrame + clip.durationFrames) {
            continue;
        }
        const TranscriptOverlayLayout overlayLayout =
            transcriptOverlayLayoutForFrame(clip, timelineFrame, request.renderSyncMarkers, transcriptCache);
        if (overlayLayout.lines.isEmpty()) {
            continue;
        }

        const QRectF bounds((request.outputSize.width() / 2.0) + clip.transcriptOverlay.translationX -
                                (clip.transcriptOverlay.boxWidth / 2.0),
                            (request.outputSize.height() / 2.0) + clip.transcriptOverlay.translationY -
                                (clip.transcriptOverlay.boxHeight / 2.0),
                            clip.transcriptOverlay.boxWidth,
                            clip.transcriptOverlay.boxHeight);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0, 0, 0, 120));
        painter.drawRoundedRect(bounds, 14.0, 14.0);
        QFont font(clip.transcriptOverlay.fontFamily);
        font.setPixelSize(clip.transcriptOverlay.fontPointSize);
        font.setBold(clip.transcriptOverlay.bold);
        font.setItalic(clip.transcriptOverlay.italic);
        const QRectF textBounds = bounds.adjusted(18.0, 14.0, -18.0, -14.0);
        const QColor highlightFillColor(QStringLiteral("#fff2a8"));
        const QColor highlightTextColor(QStringLiteral("#181818"));
        QTextDocument shadowDoc;
        shadowDoc.setDefaultFont(font);
        shadowDoc.setDocumentMargin(0.0);
        shadowDoc.setTextWidth(textBounds.width());
        shadowDoc.setHtml(transcriptOverlayHtml(overlayLayout,
                                                QColor(0, 0, 0, 200),
                                                QColor(0, 0, 0, 200),
                                                QColor(0, 0, 0, 0)));
        QTextDocument textDoc;
        textDoc.setDefaultFont(font);
        textDoc.setDocumentMargin(0.0);
        textDoc.setTextWidth(textBounds.width());
        textDoc.setHtml(transcriptOverlayHtml(overlayLayout,
                                              clip.transcriptOverlay.textColor,
                                              highlightTextColor,
                                              highlightFillColor));
        const qreal textY = textBounds.top() + qMax<qreal>(0.0, (textBounds.height() - textDoc.size().height()) / 2.0);
        painter.save();
        painter.translate(textBounds.left() + 5.0, textY + 5.0);
        shadowDoc.drawContents(&painter);
        painter.translate(-5.0, -5.0);
        textDoc.drawContents(&painter);
        painter.restore();
    }
}

const AVCodec* codecForRequest(const QString& outputFormat, QString* codecLabel) {
    const QString format = outputFormat.toLower();
    if (format == QStringLiteral("mov")) {
        if (codecLabel) *codecLabel = QStringLiteral("prores_ks");
        if (const AVCodec* codec = avcodec_find_encoder_by_name("prores_ks")) {
            return codec;
        }
        return avcodec_find_encoder(AV_CODEC_ID_PRORES);
    }
    if (format == QStringLiteral("mkv")) {
        if (codecLabel) *codecLabel = QStringLiteral("ffv1");
        if (const AVCodec* codec = avcodec_find_encoder_by_name("ffv1")) {
            return codec;
        }
        return avcodec_find_encoder(AV_CODEC_ID_FFV1);
    }
    if (format == QStringLiteral("webm")) {
        if (codecLabel) *codecLabel = QStringLiteral("libvpx-vp9");
        if (const AVCodec* codec = avcodec_find_encoder_by_name("libvpx-vp9")) {
            return codec;
        }
        return avcodec_find_encoder(AV_CODEC_ID_VP9);
    }

    if (codecLabel) *codecLabel = QStringLiteral("libx264");
    if (const AVCodec* codec = avcodec_find_encoder_by_name("libx264")) {
        return codec;
    }
    return avcodec_find_encoder(AV_CODEC_ID_H264);
}

QVector<VideoEncoderChoice> videoEncoderChoicesForRequest(const QString& outputFormat) {
    const QString format = outputFormat.toLower();
    QVector<VideoEncoderChoice> choices;
    if (format == QStringLiteral("mp4")) {
#if defined(Q_OS_LINUX)
        choices.push_back({QStringLiteral("h264_nvenc"), AV_PIX_FMT_NV12});
        choices.push_back({QStringLiteral("h264_qsv"), AV_PIX_FMT_NV12});
        choices.push_back({QStringLiteral("h264_vaapi"), AV_PIX_FMT_NV12});
#endif
        choices.push_back({QStringLiteral("libx264"), AV_PIX_FMT_YUV420P});
    } else if (format == QStringLiteral("mov")) {
        choices.push_back({QStringLiteral("prores_ks"), AV_PIX_FMT_YUV422P10LE});
    } else if (format == QStringLiteral("webm")) {
        choices.push_back({QStringLiteral("libvpx-vp9"), AV_PIX_FMT_YUV420P});
    } else if (format == QStringLiteral("mkv")) {
        choices.push_back({QStringLiteral("ffv1"), AV_PIX_FMT_BGRA});
    }
    return choices;
}

const AVCodec* audioCodecForRequest(const QString& outputFormat, QString* codecLabel) {
    const QString format = outputFormat.toLower();
    if (format == QStringLiteral("webm")) {
        if (codecLabel) *codecLabel = QStringLiteral("libopus");
        if (const AVCodec* codec = avcodec_find_encoder_by_name("libopus")) {
            return codec;
        }
        return avcodec_find_encoder(AV_CODEC_ID_OPUS);
    }

    if (codecLabel) *codecLabel = QStringLiteral("aac");
    if (const AVCodec* codec = avcodec_find_encoder_by_name("aac")) {
        return codec;
    }
    return avcodec_find_encoder(AV_CODEC_ID_AAC);
}

AVPixelFormat pixelFormatForCodec(const AVCodec* codec, const QString& outputFormat) {
    const QString format = outputFormat.toLower();
    if (format == QStringLiteral("mov")) {
        return AV_PIX_FMT_YUV422P10LE;
    }
    if (format == QStringLiteral("mkv")) {
        return AV_PIX_FMT_BGRA;
    }
    Q_UNUSED(codec)
    return AV_PIX_FMT_YUV420P;
}

void configureCodecOptions(AVCodecContext* codecCtx, const QString& outputFormat, const QString& codecLabel = QString()) {
    const QString format = outputFormat.toLower();
    const QString loweredCodec = codecLabel.toLower();
    if (loweredCodec == QStringLiteral("h264_nvenc")) {
        av_opt_set(codecCtx->priv_data, "preset", "p5", 0);
        av_opt_set(codecCtx->priv_data, "rc", "vbr", 0);
        av_opt_set(codecCtx->priv_data, "cq", "19", 0);
        return;
    } else if (loweredCodec == QStringLiteral("h264_qsv")) {
        av_opt_set(codecCtx->priv_data, "preset", "medium", 0);
        av_opt_set(codecCtx->priv_data, "global_quality", "23", 0);
        return;
    } else if (loweredCodec == QStringLiteral("h264_vaapi")) {
        av_opt_set(codecCtx->priv_data, "rc_mode", "VBR", 0);
        av_opt_set(codecCtx->priv_data, "qp", "20", 0);
        return;
    }
    if (format == QStringLiteral("mp4")) {
        av_opt_set(codecCtx->priv_data, "preset", "veryfast", 0);
        av_opt_set(codecCtx->priv_data, "crf", "18", 0);
    } else if (format == QStringLiteral("mov")) {
        av_opt_set(codecCtx->priv_data, "profile", "3", 0);
    } else if (format == QStringLiteral("webm")) {
        av_opt_set(codecCtx->priv_data, "deadline", "realtime", 0);
        av_opt_set(codecCtx->priv_data, "cpu-used", "4", 0);
        av_opt_set(codecCtx->priv_data, "crf", "30", 0);
        av_opt_set(codecCtx->priv_data, "b", "0", 0);
    }
}

void configureAudioCodecOptions(AVCodecContext* codecCtx, const QString& outputFormat) {
    const QString format = outputFormat.toLower();
    if (format == QStringLiteral("webm")) {
        av_opt_set(codecCtx->priv_data, "application", "audio", 0);
    }
}

AVSampleFormat audioSampleFormatForCodec(const AVCodec* codec) {
    if (!codec || !codec->sample_fmts) {
        return AV_SAMPLE_FMT_FLTP;
    }

    const AVSampleFormat preferredFormats[] = {
        AV_SAMPLE_FMT_FLTP,
        AV_SAMPLE_FMT_S16P,
        AV_SAMPLE_FMT_S16,
        AV_SAMPLE_FMT_FLT,
    };
    for (AVSampleFormat preferred : preferredFormats) {
        for (const AVSampleFormat* fmt = codec->sample_fmts; *fmt != AV_SAMPLE_FMT_NONE; ++fmt) {
            if (*fmt == preferred) {
                return preferred;
            }
        }
    }
    return codec->sample_fmts[0];
}

int audioSampleRateForCodec(const AVCodec* codec) {
    if (!codec || !codec->supported_samplerates) {
        return kRenderAudioSampleRate;
    }

    for (const int* sampleRate = codec->supported_samplerates; *sampleRate != 0; ++sampleRate) {
        if (*sampleRate == kRenderAudioSampleRate) {
            return *sampleRate;
        }
    }
    return codec->supported_samplerates[0];
}

struct DecodedAudioClip {
    QVector<float> samples;
    bool valid = false;
};

struct AudioExportState {
    QVector<TimelineClip> clips;
    QVector<RenderSyncMarker> renderSyncMarkers;
    QHash<QString, DecodedAudioClip> cache;
    AVStream* stream = nullptr;
    AVCodecContext* codecCtx = nullptr;
    bool enabled = false;
};

DecodedAudioClip decodeClipAudio(const QString& path) {
    DecodedAudioClip cache;

    AVFormatContext* formatCtx = nullptr;
    if (avformat_open_input(&formatCtx, QFile::encodeName(path).constData(), nullptr, nullptr) < 0) {
        return cache;
    }

    if (avformat_find_stream_info(formatCtx, nullptr) < 0) {
        avformat_close_input(&formatCtx);
        return cache;
    }

    int audioStreamIndex = -1;
    for (unsigned i = 0; i < formatCtx->nb_streams; ++i) {
        if (formatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            audioStreamIndex = static_cast<int>(i);
            break;
        }
    }
    if (audioStreamIndex < 0) {
        avformat_close_input(&formatCtx);
        return cache;
    }

    AVStream* stream = formatCtx->streams[audioStreamIndex];
    const AVCodec* decoder = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!decoder) {
        avformat_close_input(&formatCtx);
        return cache;
    }

    AVCodecContext* codecCtx = avcodec_alloc_context3(decoder);
    if (!codecCtx) {
        avformat_close_input(&formatCtx);
        return cache;
    }

    if (avcodec_parameters_to_context(codecCtx, stream->codecpar) < 0 ||
        avcodec_open2(codecCtx, decoder, nullptr) < 0) {
        avcodec_free_context(&codecCtx);
        avformat_close_input(&formatCtx);
        return cache;
    }

    SwrContext* swr = swr_alloc();
    AVChannelLayout outLayout;
    av_channel_layout_default(&outLayout, kRenderAudioChannels);
    av_opt_set_chlayout(swr, "in_chlayout", &codecCtx->ch_layout, 0);
    av_opt_set_chlayout(swr, "out_chlayout", &outLayout, 0);
    av_opt_set_int(swr, "in_sample_rate", codecCtx->sample_rate, 0);
    av_opt_set_int(swr, "out_sample_rate", kRenderAudioSampleRate, 0);
    av_opt_set_sample_fmt(swr, "in_sample_fmt", codecCtx->sample_fmt, 0);
    av_opt_set_sample_fmt(swr, "out_sample_fmt", AV_SAMPLE_FMT_FLT, 0);
    if (!swr || swr_init(swr) < 0) {
        av_channel_layout_uninit(&outLayout);
        swr_free(&swr);
        avcodec_free_context(&codecCtx);
        avformat_close_input(&formatCtx);
        return cache;
    }

    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    QByteArray converted;

    auto appendConverted = [&](AVFrame* decoded) {
        const int outSamples = swr_get_out_samples(swr, decoded->nb_samples);
        if (outSamples <= 0) {
            return;
        }
        uint8_t* outData = nullptr;
        int outLineSize = 0;
        if (av_samples_alloc(&outData, &outLineSize, kRenderAudioChannels, outSamples, AV_SAMPLE_FMT_FLT, 0) < 0) {
            return;
        }
        const int convertedSamples = swr_convert(swr, &outData, outSamples,
                                                 const_cast<const uint8_t**>(decoded->extended_data),
                                                 decoded->nb_samples);
        if (convertedSamples > 0) {
            const int byteCount = convertedSamples * kRenderAudioChannels * static_cast<int>(sizeof(float));
            converted.append(reinterpret_cast<const char*>(outData), byteCount);
        }
        av_freep(&outData);
    };

    while (av_read_frame(formatCtx, packet) >= 0) {
        if (packet->stream_index != audioStreamIndex) {
            av_packet_unref(packet);
            continue;
        }
        if (avcodec_send_packet(codecCtx, packet) >= 0) {
            while (avcodec_receive_frame(codecCtx, frame) >= 0) {
                appendConverted(frame);
                av_frame_unref(frame);
            }
        }
        av_packet_unref(packet);
    }

    avcodec_send_packet(codecCtx, nullptr);
    while (avcodec_receive_frame(codecCtx, frame) >= 0) {
        appendConverted(frame);
        av_frame_unref(frame);
    }

    const int outSamples = swr_get_out_samples(swr, 0);
    if (outSamples > 0) {
        uint8_t* outData = nullptr;
        int outLineSize = 0;
        if (av_samples_alloc(&outData, &outLineSize, kRenderAudioChannels, outSamples, AV_SAMPLE_FMT_FLT, 0) >= 0) {
            const int flushed = swr_convert(swr, &outData, outSamples, nullptr, 0);
            if (flushed > 0) {
                converted.append(reinterpret_cast<const char*>(outData),
                                 flushed * kRenderAudioChannels * static_cast<int>(sizeof(float)));
            }
            av_freep(&outData);
        }
    }

    const int sampleCount = converted.size() / static_cast<int>(sizeof(float));
    cache.samples.resize(sampleCount);
    if (sampleCount > 0) {
        std::memcpy(cache.samples.data(), converted.constData(), converted.size());
        cache.valid = true;
    }

    av_frame_free(&frame);
    av_packet_free(&packet);
    av_channel_layout_uninit(&outLayout);
    swr_free(&swr);
    avcodec_free_context(&codecCtx);
    avformat_close_input(&formatCtx);
    return cache;
}

void mixAudioChunk(const QVector<TimelineClip>& clips,
                   const QVector<RenderSyncMarker>& renderSyncMarkers,
                   const QHash<QString, DecodedAudioClip>& audioCache,
                   float* output,
                   int frames,
                   int64_t chunkStartSample) {
    std::fill(output, output + frames * kRenderAudioChannels, 0.0f);

    for (const TimelineClip& clip : clips) {
        if (!clipAudioPlaybackEnabled(clip)) {
            continue;
        }
        const DecodedAudioClip audio = audioCache.value(clip.filePath);
        if (!audio.valid) {
            continue;
        }

        const int64_t clipStartSample = clipTimelineStartSamples(clip);
        const int64_t sourceInSample = clipSourceInSamples(clip);
        const int64_t clipAvailableSamples = (audio.samples.size() / kRenderAudioChannels) - sourceInSample;
        if (clipAvailableSamples <= 0) {
            continue;
        }
        const int64_t clipEndSample =
            clipStartSample + qMin<int64_t>(frameToSamples(clip.durationFrames), clipAvailableSamples);
        const int64_t chunkEndSample = chunkStartSample + frames;
        if (chunkEndSample <= clipStartSample || chunkStartSample >= clipEndSample) {
            continue;
        }

        const int64_t mixStart = qMax<int64_t>(chunkStartSample, clipStartSample);
        const int64_t mixEnd = qMin<int64_t>(chunkEndSample, clipEndSample);
        for (int64_t samplePos = mixStart; samplePos < mixEnd; ++samplePos) {
            const int outFrame = static_cast<int>(samplePos - chunkStartSample);
            const int64_t inFrame =
                sourceSampleForClipAtTimelineSample(clip, samplePos, renderSyncMarkers);
            if (inFrame < 0 || inFrame >= (audio.samples.size() / kRenderAudioChannels)) {
                continue;
            }
            const int outIndex = outFrame * kRenderAudioChannels;
            const int inIndex = static_cast<int>(inFrame * kRenderAudioChannels);
            output[outIndex] = qBound(-1.0f, output[outIndex] + audio.samples[inIndex], 1.0f);
            output[outIndex + 1] = qBound(-1.0f, output[outIndex + 1] + audio.samples[inIndex + 1], 1.0f);
        }
    }
}

bool encodeFrame(AVCodecContext* codecCtx,
                 AVStream* stream,
                 AVFormatContext* formatCtx,
                 AVFrame* frame,
                 QString* errorMessage);

bool initializeExportAudio(const RenderRequest& request,
                           AVFormatContext* formatCtx,
                           AudioExportState* state,
                           QString* errorMessage) {
    if (!state) {
        return true;
    }

    QVector<TimelineClip> audioClips;
    for (const TimelineClip& clip : request.clips) {
        if (clipAudioPlaybackEnabled(clip)) {
            audioClips.push_back(clip);
        }
    }
    if (audioClips.isEmpty()) {
        return true;
    }

    QHash<QString, DecodedAudioClip> audioCache;
    for (const TimelineClip& clip : audioClips) {
        if (audioCache.contains(clip.filePath)) {
            continue;
        }
        const DecodedAudioClip decoded = decodeClipAudio(clip.filePath);
        if (decoded.valid) {
            audioCache.insert(clip.filePath, decoded);
        }
    }

    bool hasDecodedAudio = false;
    for (const TimelineClip& clip : audioClips) {
        if (audioCache.value(clip.filePath).valid) {
            hasDecodedAudio = true;
            break;
        }
    }
    if (!hasDecodedAudio) {
        return true;
    }

    QString codecLabel;
    const AVCodec* audioCodec = audioCodecForRequest(request.outputFormat, &codecLabel);
    if (!audioCodec) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("No audio encoder available for format %1.").arg(request.outputFormat);
        }
        return false;
    }

    state->stream = avformat_new_stream(formatCtx, nullptr);
    if (!state->stream) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to create output audio stream.");
        }
        return false;
    }

    state->codecCtx = avcodec_alloc_context3(audioCodec);
    if (!state->codecCtx) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to allocate audio encoder context.");
        }
        return false;
    }

    const int sampleRate = audioSampleRateForCodec(audioCodec);
    const AVSampleFormat sampleFormat = audioSampleFormatForCodec(audioCodec);
    av_channel_layout_default(&state->codecCtx->ch_layout, kRenderAudioChannels);
    state->codecCtx->codec_id = audioCodec->id;
    state->codecCtx->codec_type = AVMEDIA_TYPE_AUDIO;
    state->codecCtx->sample_rate = sampleRate;
    state->codecCtx->sample_fmt = sampleFormat;
    state->codecCtx->time_base = AVRational{1, sampleRate};
    state->codecCtx->bit_rate = 192000;
    if (formatCtx->oformat->flags & AVFMT_GLOBALHEADER) {
        state->codecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
    configureAudioCodecOptions(state->codecCtx, request.outputFormat);

    if (avcodec_open2(state->codecCtx, audioCodec, nullptr) < 0) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to open audio encoder %1.").arg(codecLabel);
        }
        avcodec_free_context(&state->codecCtx);
        return false;
    }

    if (avcodec_parameters_from_context(state->stream->codecpar, state->codecCtx) < 0) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to copy audio encoder parameters.");
        }
        avcodec_free_context(&state->codecCtx);
        return false;
    }
    state->stream->time_base = state->codecCtx->time_base;
    state->clips = audioClips;
    state->cache = audioCache;
    state->renderSyncMarkers = request.renderSyncMarkers;
    state->enabled = true;
    return true;
}

bool encodeExportAudio(const QVector<ExportRangeSegment>& exportRanges,
                       const AudioExportState& state,
                       AVFormatContext* formatCtx,
                       QString* errorMessage) {
    if (!state.enabled || !state.codecCtx || !state.stream) {
        return true;
    }

    SwrContext* swr = swr_alloc();
    AVChannelLayout inputLayout;
    av_channel_layout_default(&inputLayout, kRenderAudioChannels);
    av_opt_set_chlayout(swr, "in_chlayout", &inputLayout, 0);
    av_opt_set_chlayout(swr, "out_chlayout", &state.codecCtx->ch_layout, 0);
    av_opt_set_int(swr, "in_sample_rate", kRenderAudioSampleRate, 0);
    av_opt_set_int(swr, "out_sample_rate", state.codecCtx->sample_rate, 0);
    av_opt_set_sample_fmt(swr, "in_sample_fmt", AV_SAMPLE_FMT_FLT, 0);
    av_opt_set_sample_fmt(swr, "out_sample_fmt", state.codecCtx->sample_fmt, 0);
    if (!swr || swr_init(swr) < 0) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to create audio resampler for export.");
        }
        av_channel_layout_uninit(&inputLayout);
        swr_free(&swr);
        return false;
    }

    AVAudioFifo* fifo = av_audio_fifo_alloc(state.codecCtx->sample_fmt,
                                            state.codecCtx->ch_layout.nb_channels,
                                            qMax(1, state.codecCtx->frame_size * 2));
    if (!fifo) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to allocate audio FIFO for export.");
        }
        av_channel_layout_uninit(&inputLayout);
        swr_free(&swr);
        return false;
    }

    AVFrame* audioFrame = av_frame_alloc();
    if (!audioFrame) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to allocate audio frame.");
        }
        av_audio_fifo_free(fifo);
        av_channel_layout_uninit(&inputLayout);
        swr_free(&swr);
        return false;
    }

    const int mixChunkFrames = 1024;
    QVector<float> mixBuffer(mixChunkFrames * kRenderAudioChannels);
    int64_t audioPts = 0;

    auto writeAvailableAudioFrames = [&](bool flushTail) -> bool {
        const int encoderFrameSamples = state.codecCtx->frame_size > 0 ? state.codecCtx->frame_size : 1024;
        while (av_audio_fifo_size(fifo) >= encoderFrameSamples ||
               (flushTail && av_audio_fifo_size(fifo) > 0)) {
            const int frameSamples = flushTail
                ? qMin(av_audio_fifo_size(fifo), encoderFrameSamples)
                : encoderFrameSamples;
            audioFrame->nb_samples = frameSamples;
            audioFrame->format = state.codecCtx->sample_fmt;
            audioFrame->sample_rate = state.codecCtx->sample_rate;
            av_channel_layout_copy(&audioFrame->ch_layout, &state.codecCtx->ch_layout);
            if (av_frame_get_buffer(audioFrame, 0) < 0) {
                if (errorMessage) {
                    *errorMessage = QStringLiteral("Failed to allocate encoded audio frame buffer.");
                }
                return false;
            }
            if (av_frame_make_writable(audioFrame) < 0) {
                if (errorMessage) {
                    *errorMessage = QStringLiteral("Failed to make encoded audio frame writable.");
                }
                return false;
            }
            if (av_audio_fifo_read(fifo, reinterpret_cast<void**>(audioFrame->data), frameSamples) < frameSamples) {
                if (errorMessage) {
                    *errorMessage = QStringLiteral("Failed to read mixed audio from FIFO.");
                }
                return false;
            }
            audioFrame->pts = audioPts;
            audioPts += frameSamples;
            if (!encodeFrame(state.codecCtx, state.stream, formatCtx, audioFrame, errorMessage)) {
                return false;
            }
            av_frame_unref(audioFrame);
            av_channel_layout_uninit(&audioFrame->ch_layout);
        }
        return true;
    };

    for (const ExportRangeSegment& range : exportRanges) {
        const int64_t exportStart = qMax<int64_t>(0, range.startFrame);
        const int64_t exportEnd = qMax(exportStart, range.endFrame);
        const int64_t segmentTotalSamples = frameToSamples(exportEnd - exportStart + 1);
        int64_t producedSamples = 0;
        while (producedSamples < segmentTotalSamples) {
            const int framesThisChunk =
                static_cast<int>(qMin<int64_t>(mixChunkFrames, segmentTotalSamples - producedSamples));
            const int64_t chunkStartSample = frameToSamples(exportStart) + producedSamples;
            mixAudioChunk(state.clips,
                          state.renderSyncMarkers,
                          state.cache,
                          mixBuffer.data(),
                          framesThisChunk,
                          chunkStartSample);

            const int estimatedOutSamples = swr_get_out_samples(swr, framesThisChunk);
            uint8_t** convertedData = nullptr;
            int convertedLineSize = 0;
            if (av_samples_alloc_array_and_samples(&convertedData,
                                                   &convertedLineSize,
                                                   state.codecCtx->ch_layout.nb_channels,
                                                   estimatedOutSamples,
                                                   state.codecCtx->sample_fmt,
                                                   0) < 0) {
                if (errorMessage) {
                    *errorMessage = QStringLiteral("Failed to allocate converted audio buffer.");
                }
                av_frame_free(&audioFrame);
                av_audio_fifo_free(fifo);
                av_channel_layout_uninit(&inputLayout);
                swr_free(&swr);
                return false;
            }

            const uint8_t* inputData[1] = {
                reinterpret_cast<const uint8_t*>(mixBuffer.constData())
            };
            const int convertedSamples =
                swr_convert(swr, convertedData, estimatedOutSamples, inputData, framesThisChunk);
            if (convertedSamples < 0) {
                if (errorMessage) {
                    *errorMessage = QStringLiteral("Failed to convert mixed audio for encoding.");
                }
                av_freep(&convertedData[0]);
                av_freep(&convertedData);
                av_frame_free(&audioFrame);
                av_audio_fifo_free(fifo);
                av_channel_layout_uninit(&inputLayout);
                swr_free(&swr);
                return false;
            }

            if (av_audio_fifo_realloc(fifo, av_audio_fifo_size(fifo) + convertedSamples) < 0 ||
                av_audio_fifo_write(fifo, reinterpret_cast<void**>(convertedData), convertedSamples) < convertedSamples) {
                if (errorMessage) {
                    *errorMessage = QStringLiteral("Failed to queue mixed audio for encoding.");
                }
                av_freep(&convertedData[0]);
                av_freep(&convertedData);
                av_frame_free(&audioFrame);
                av_audio_fifo_free(fifo);
                av_channel_layout_uninit(&inputLayout);
                swr_free(&swr);
                return false;
            }

            av_freep(&convertedData[0]);
            av_freep(&convertedData);

            if (!writeAvailableAudioFrames(false)) {
                av_frame_free(&audioFrame);
                av_audio_fifo_free(fifo);
                av_channel_layout_uninit(&inputLayout);
                swr_free(&swr);
                return false;
            }

            producedSamples += framesThisChunk;
        }
    }

    const int flushedSamples = swr_convert(swr, nullptr, 0, nullptr, 0);
    if (flushedSamples < 0) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to flush audio resampler.");
        }
        av_frame_free(&audioFrame);
        av_audio_fifo_free(fifo);
        av_channel_layout_uninit(&inputLayout);
        swr_free(&swr);
        return false;
    }

    if (!writeAvailableAudioFrames(true) ||
        !encodeFrame(state.codecCtx, state.stream, formatCtx, nullptr, errorMessage)) {
        av_frame_free(&audioFrame);
        av_audio_fifo_free(fifo);
        av_channel_layout_uninit(&inputLayout);
        swr_free(&swr);
        return false;
    }

    av_frame_free(&audioFrame);
    av_audio_fifo_free(fifo);
    av_channel_layout_uninit(&inputLayout);
    swr_free(&swr);
    return true;
}

QVector<TimelineClip> sortedVisualClips(const QVector<TimelineClip>& clips) {
    QVector<TimelineClip> visual;
    for (const TimelineClip& clip : clips) {
        if (clipVisualPlaybackEnabled(clip)) {
            visual.push_back(clip);
        }
    }
    std::sort(visual.begin(), visual.end(), [](const TimelineClip& a, const TimelineClip& b) {
        if (a.trackIndex == b.trackIndex) {
            return clipTimelineStartSamples(a) < clipTimelineStartSamples(b);
        }
        return a.trackIndex < b.trackIndex;
    });
    return visual;
}

class OffscreenGpuRenderer : protected QOpenGLFunctions {
public:
    OffscreenGpuRenderer()
        : m_quadBuffer(QOpenGLBuffer::VertexBuffer) {}

    ~OffscreenGpuRenderer() {
        releaseResources();
    }

    bool initialize(const QSize& outputSize, QString* errorMessage) {
        m_outputSize = outputSize;

        QSurfaceFormat format;
        format.setRenderableType(QSurfaceFormat::OpenGL);
        format.setProfile(QSurfaceFormat::CompatibilityProfile);
        format.setVersion(2, 0);

        m_surface = std::make_unique<QOffscreenSurface>();
        m_surface->setFormat(format);
        m_surface->create();
        if (!m_surface->isValid()) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Failed to create offscreen OpenGL surface.");
            }
            return false;
        }

        m_context = std::make_unique<QOpenGLContext>();
        m_context->setFormat(format);
        if (!m_context->create()) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Failed to create OpenGL context for render export.");
            }
            return false;
        }
        if (!m_context->makeCurrent(m_surface.get())) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Failed to activate OpenGL context for render export.");
            }
            return false;
        }

        initializeOpenGLFunctions();
        glDisable(GL_DEPTH_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

        static const char* kVertexShader = R"(
            attribute vec2 a_position;
            attribute vec2 a_texCoord;
            uniform mat4 u_mvp;
            varying vec2 v_texCoord;
            void main() {
                v_texCoord = a_texCoord;
                gl_Position = u_mvp * vec4(a_position, 0.0, 1.0);
            }
        )";

        static const char* kFragmentShader = R"(
            uniform sampler2D u_texture;
            uniform sampler2D u_texture_uv;
            uniform float u_texture_mode;
            uniform float u_brightness;
            uniform float u_contrast;
            uniform float u_saturation;
            uniform float u_opacity;
            varying vec2 v_texCoord;

            void main() {
                vec4 color;
                if (u_texture_mode > 0.5) {
                    float y = texture2D(u_texture, v_texCoord).r;
                    vec2 uv = texture2D(u_texture_uv, v_texCoord).rg - vec2(0.5, 0.5);
                    float r = y + (1.402 * uv.y);
                    float g = y - (0.344136 * uv.x) - (0.714136 * uv.y);
                    float b = y + (1.772 * uv.x);
                    color = vec4(clamp(vec3(r, g, b), 0.0, 1.0), 1.0);
                } else {
                    color = texture2D(u_texture, v_texCoord);
                }
                float sourceAlpha = color.a;
                vec3 rgb = color.rgb;
                if (sourceAlpha > 0.0001) {
                    rgb /= sourceAlpha;
                } else {
                    rgb = vec3(0.0);
                }
                rgb = ((rgb - 0.5) * u_contrast) + 0.5 + vec3(u_brightness);
                float luminance = dot(rgb, vec3(0.2126, 0.7152, 0.0722));
                rgb = mix(vec3(luminance), rgb, u_saturation);
                rgb = clamp(rgb, 0.0, 1.0);
                color.a = clamp(sourceAlpha * u_opacity, 0.0, 1.0);
                color.rgb = rgb * color.a;
                gl_FragColor = color;
            }
        )";

        m_shaderProgram = std::make_unique<QOpenGLShaderProgram>();
        if (!m_shaderProgram->addShaderFromSourceCode(QOpenGLShader::Vertex, kVertexShader) ||
            !m_shaderProgram->addShaderFromSourceCode(QOpenGLShader::Fragment, kFragmentShader) ||
            !m_shaderProgram->link()) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Failed to build offscreen render shader pipeline.");
            }
            return false;
        }

        static const GLfloat kQuadVertices[] = {
            -0.5f, -0.5f, 0.0f, 0.0f,
             0.5f, -0.5f, 1.0f, 0.0f,
            -0.5f,  0.5f, 0.0f, 1.0f,
             0.5f,  0.5f, 1.0f, 1.0f,
        };

        m_quadBuffer.create();
        m_quadBuffer.bind();
        m_quadBuffer.allocate(kQuadVertices, sizeof(kQuadVertices));
        m_quadBuffer.release();

        QOpenGLFramebufferObjectFormat fboFormat;
        fboFormat.setAttachment(QOpenGLFramebufferObject::CombinedDepthStencil);
        fboFormat.setInternalTextureFormat(GL_RGBA8);
        m_fbo = std::make_unique<QOpenGLFramebufferObject>(m_outputSize, fboFormat);
        if (!m_fbo->isValid()) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Failed to create offscreen framebuffer for render export.");
            }
            return false;
        }

        QOpenGLFramebufferObjectFormat yFboFormat;
        yFboFormat.setAttachment(QOpenGLFramebufferObject::NoAttachment);
        yFboFormat.setInternalTextureFormat(GL_R8);
        m_nv12YFbo = std::make_unique<QOpenGLFramebufferObject>(m_outputSize.width(), m_outputSize.height(), yFboFormat);
        if (!m_nv12YFbo->isValid()) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Failed to create NV12 luma framebuffer for render export.");
            }
            return false;
        }

        QOpenGLFramebufferObjectFormat uvFboFormat;
        uvFboFormat.setAttachment(QOpenGLFramebufferObject::NoAttachment);
        uvFboFormat.setInternalTextureFormat(GL_RG8);
        m_nv12UvFbo = std::make_unique<QOpenGLFramebufferObject>(qMax(1, m_outputSize.width() / 2),
                                                                 qMax(1, m_outputSize.height() / 2),
                                                                 uvFboFormat);
        if (!m_nv12UvFbo->isValid()) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Failed to create NV12 chroma framebuffer for render export.");
            }
            return false;
        }

        static const char* kNv12VertexShader = R"(
            attribute vec2 a_position;
            attribute vec2 a_texCoord;
            varying vec2 v_texCoord;
            void main() {
                v_texCoord = a_texCoord;
                gl_Position = vec4(a_position * 2.0, 0.0, 1.0);
            }
        )";

        static const char* kNv12YFragmentShader = R"(
            uniform sampler2D u_texture;
            varying vec2 v_texCoord;
            void main() {
                vec2 coord = vec2(v_texCoord.x, 1.0 - v_texCoord.y);
                vec3 rgb = texture2D(u_texture, coord).rgb;
                float y = dot(rgb, vec3(0.2578125, 0.50390625, 0.09765625)) + 0.0625;
                gl_FragColor = vec4(y, 0.0, 0.0, 1.0);
            }
        )";

        static const char* kNv12UvFragmentShader = R"(
            uniform sampler2D u_texture;
            uniform vec2 u_texel_size;
            varying vec2 v_texCoord;
            vec3 sampleRgb(vec2 coord) {
                return texture2D(u_texture, coord).rgb;
            }
            void main() {
                vec2 base = vec2(v_texCoord.x, 1.0 - v_texCoord.y);
                vec2 offset = u_texel_size * 0.5;
                vec3 rgb0 = sampleRgb(base + vec2(-offset.x, -offset.y));
                vec3 rgb1 = sampleRgb(base + vec2( offset.x, -offset.y));
                vec3 rgb2 = sampleRgb(base + vec2(-offset.x,  offset.y));
                vec3 rgb3 = sampleRgb(base + vec2( offset.x,  offset.y));
                vec3 rgb = (rgb0 + rgb1 + rgb2 + rgb3) * 0.25;
                float u = dot(rgb, vec3(-0.1484375, -0.2890625, 0.4375)) + 0.5;
                float v = dot(rgb, vec3(0.4375, -0.3671875, -0.0703125)) + 0.5;
                gl_FragColor = vec4(u, v, 0.0, 1.0);
            }
        )";

        m_nv12YShaderProgram = std::make_unique<QOpenGLShaderProgram>();
        if (!m_nv12YShaderProgram->addShaderFromSourceCode(QOpenGLShader::Vertex, kNv12VertexShader) ||
            !m_nv12YShaderProgram->addShaderFromSourceCode(QOpenGLShader::Fragment, kNv12YFragmentShader) ||
            !m_nv12YShaderProgram->link()) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Failed to build NV12 luma conversion shader.");
            }
            return false;
        }

        m_nv12UvShaderProgram = std::make_unique<QOpenGLShaderProgram>();
        if (!m_nv12UvShaderProgram->addShaderFromSourceCode(QOpenGLShader::Vertex, kNv12VertexShader) ||
            !m_nv12UvShaderProgram->addShaderFromSourceCode(QOpenGLShader::Fragment, kNv12UvFragmentShader) ||
            !m_nv12UvShaderProgram->link()) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Failed to build NV12 chroma conversion shader.");
            }
            return false;
        }

        return true;
    }

    QImage renderFrame(const RenderRequest& request,
                       int64_t timelineFrame,
                       QHash<QString, editor::DecoderContext*>& decoders,
                       editor::AsyncDecoder* asyncDecoder,
                       QHash<RenderAsyncFrameKey, editor::FrameHandle>* asyncFrameCache,
                       const QVector<TimelineClip>& orderedClips,
                       QHash<QString, RenderClipStageStats>* clipStageStats = nullptr,
                       qint64* decodeMs = nullptr,
                       qint64* textureMs = nullptr,
                       qint64* compositeMs = nullptr,
                       qint64* readbackMs = nullptr,
                       QJsonArray* skippedClips = nullptr,
                       QJsonObject* skippedReasonCounts = nullptr) {
        if (!m_context || !m_surface || !m_fbo || !m_shaderProgram) {
            return QImage();
        }
        if (!m_context->makeCurrent(m_surface.get())) {
            return QImage();
        }

        m_fbo->bind();
        glViewport(0, 0, m_outputSize.width(), m_outputSize.height());
        glDisable(GL_DEPTH_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        for (const TimelineClip& clip : orderedClips) {
            if (timelineFrame < clip.startFrame || timelineFrame >= clip.startFrame + clip.durationFrames) {
                continue;
            }

            const TimelineClip::GradingKeyframe grade =
                evaluateClipGradingAtPosition(clip, static_cast<qreal>(timelineFrame));
            if (grade.opacity <= 0.0001) {
                recordRenderSkip(skippedClips, skippedReasonCounts, clip, QStringLiteral("zero_opacity"), timelineFrame);
                continue;
            }

            const QString path = clip.filePath;
            const int64_t localFrame =
                sourceFrameForClipAtTimelinePosition(clip, static_cast<qreal>(timelineFrame), request.renderSyncMarkers);
            QElapsedTimer decodeTimer;
            decodeTimer.start();
            const editor::FrameHandle frame =
                decodeRenderFrame(path, localFrame, decoders, asyncDecoder, asyncFrameCache);
            const qint64 decodeElapsed = decodeTimer.elapsed();
            if (decodeMs) {
                *decodeMs += decodeElapsed;
            }
            if (frame.isNull() || (!frame.hasCpuImage() && !frameUsesCudaZeroCopyCandidate(frame))) {
                recordRenderSkip(skippedClips,
                                 skippedReasonCounts,
                                 clip,
                                 frame.isNull() ? QStringLiteral("frame_null_or_decoder_init_failed")
                                                : QStringLiteral("no_cpu_image"),
                                 timelineFrame,
                                 localFrame);
                continue;
            }

            QElapsedTimer textureTimer;
            textureTimer.start();
            editor::GlTextureCacheEntry* textureEntry = textureForFrame(frame);
            const qint64 textureElapsed = textureTimer.elapsed();
            if (textureMs) {
                *textureMs += textureElapsed;
            }
            if (!textureEntry || textureEntry->textureId == 0) {
                recordRenderSkip(skippedClips, skippedReasonCounts, clip, QStringLiteral("texture_upload_failed"), timelineFrame, localFrame);
                continue;
            }

            const QRect fitted = fitRect(frame.size(), m_outputSize);
            const TimelineClip::TransformKeyframe transform =
                evaluateClipTransformAtPosition(clip, static_cast<qreal>(timelineFrame));
            const QPointF center(fitted.center().x() + transform.translationX,
                                 fitted.center().y() + transform.translationY);

            QMatrix4x4 projection;
            projection.ortho(0.0f, static_cast<float>(m_outputSize.width()),
                             static_cast<float>(m_outputSize.height()), 0.0f,
                             -1.0f, 1.0f);

            QMatrix4x4 model;
            model.translate(center.x(), center.y());
            model.rotate(transform.rotation, 0.0f, 0.0f, 1.0f);
            model.scale(fitted.width() * transform.scaleX, fitted.height() * transform.scaleY, 1.0f);

            QElapsedTimer compositeTimer;
            compositeTimer.start();
            m_shaderProgram->bind();
            m_shaderProgram->setUniformValue("u_mvp", projection * model);
            m_shaderProgram->setUniformValue("u_brightness", GLfloat(grade.brightness));
            m_shaderProgram->setUniformValue("u_contrast", GLfloat(grade.contrast));
            m_shaderProgram->setUniformValue("u_saturation", GLfloat(grade.saturation));
            m_shaderProgram->setUniformValue("u_opacity", GLfloat(grade.opacity));
            m_shaderProgram->setUniformValue("u_texture", 0);
            m_shaderProgram->setUniformValue("u_texture_uv", 1);
            m_shaderProgram->setUniformValue("u_texture_mode", textureEntry->usesYuvTextures ? 1.0f : 0.0f);

            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, textureEntry->textureId);
            glActiveTexture(GL_TEXTURE1);
            if (textureEntry->usesYuvTextures && textureEntry->auxTextureId != 0) {
                glBindTexture(GL_TEXTURE_2D, textureEntry->auxTextureId);
            } else {
                glBindTexture(GL_TEXTURE_2D, 0);
            }
            m_quadBuffer.bind();
            const int positionLoc = m_shaderProgram->attributeLocation("a_position");
            const int texCoordLoc = m_shaderProgram->attributeLocation("a_texCoord");
            m_shaderProgram->enableAttributeArray(positionLoc);
            m_shaderProgram->enableAttributeArray(texCoordLoc);
            m_shaderProgram->setAttributeBuffer(positionLoc, GL_FLOAT, 0, 2, 4 * sizeof(GLfloat));
            m_shaderProgram->setAttributeBuffer(texCoordLoc, GL_FLOAT, 2 * sizeof(GLfloat), 2, 4 * sizeof(GLfloat));
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            m_shaderProgram->disableAttributeArray(positionLoc);
            m_shaderProgram->disableAttributeArray(texCoordLoc);
            m_quadBuffer.release();
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, 0);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, 0);
            const qint64 compositeElapsed = compositeTimer.elapsed();
            if (compositeMs) {
                *compositeMs += compositeElapsed;
            }
            accumulateClipStageStats(clipStageStats,
                                     clip,
                                     decodeElapsed,
                                     textureElapsed,
                                     compositeElapsed);
        }

        QImage image(m_outputSize, QImage::Format_ARGB32_Premultiplied);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        QElapsedTimer readbackTimer;
        readbackTimer.start();
        glReadPixels(0, 0, m_outputSize.width(), m_outputSize.height(), GL_BGRA, GL_UNSIGNED_BYTE, image.bits());
        if (readbackMs) {
            *readbackMs += readbackTimer.elapsed();
        }
        m_fbo->release();
        trimTextureCache();
        return image.mirrored();
    }

    bool convertLastFrameToNv12(AVFrame* frame,
                                qint64* nv12ConvertMs = nullptr,
                                qint64* readbackMs = nullptr) {
        if (!frame || !m_context || !m_surface || !m_fbo || !m_nv12YFbo || !m_nv12UvFbo ||
            !m_nv12YShaderProgram || !m_nv12UvShaderProgram) {
            return false;
        }
        if (!m_context->makeCurrent(m_surface.get())) {
            return false;
        }

        const GLuint sourceTextureId = m_fbo->texture();
        if (!sourceTextureId) {
            return false;
        }

        glDisable(GL_BLEND);
        glDisable(GL_DEPTH_TEST);

        QElapsedTimer nv12Timer;
        nv12Timer.start();

        m_nv12YFbo->bind();
        glViewport(0, 0, m_outputSize.width(), m_outputSize.height());
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        m_nv12YShaderProgram->bind();
        m_nv12YShaderProgram->setUniformValue("u_texture", 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, sourceTextureId);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        m_quadBuffer.bind();
        int positionLoc = m_nv12YShaderProgram->attributeLocation("a_position");
        int texCoordLoc = m_nv12YShaderProgram->attributeLocation("a_texCoord");
        m_nv12YShaderProgram->enableAttributeArray(positionLoc);
        m_nv12YShaderProgram->enableAttributeArray(texCoordLoc);
        m_nv12YShaderProgram->setAttributeBuffer(positionLoc, GL_FLOAT, 0, 2, 4 * sizeof(GLfloat));
        m_nv12YShaderProgram->setAttributeBuffer(texCoordLoc, GL_FLOAT, 2 * sizeof(GLfloat), 2, 4 * sizeof(GLfloat));
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        m_nv12YShaderProgram->disableAttributeArray(positionLoc);
        m_nv12YShaderProgram->disableAttributeArray(texCoordLoc);
        m_quadBuffer.release();
        m_nv12YShaderProgram->release();
        m_nv12YFbo->release();

        m_nv12UvFbo->bind();
        glViewport(0, 0, qMax(1, m_outputSize.width() / 2), qMax(1, m_outputSize.height() / 2));
        glClear(GL_COLOR_BUFFER_BIT);
        m_nv12UvShaderProgram->bind();
        m_nv12UvShaderProgram->setUniformValue("u_texture", 0);
        m_nv12UvShaderProgram->setUniformValue("u_texel_size",
                                               QVector2D(1.0f / qMax(1, m_outputSize.width()),
                                                         1.0f / qMax(1, m_outputSize.height())));
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, sourceTextureId);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        m_quadBuffer.bind();
        positionLoc = m_nv12UvShaderProgram->attributeLocation("a_position");
        texCoordLoc = m_nv12UvShaderProgram->attributeLocation("a_texCoord");
        m_nv12UvShaderProgram->enableAttributeArray(positionLoc);
        m_nv12UvShaderProgram->enableAttributeArray(texCoordLoc);
        m_nv12UvShaderProgram->setAttributeBuffer(positionLoc, GL_FLOAT, 0, 2, 4 * sizeof(GLfloat));
        m_nv12UvShaderProgram->setAttributeBuffer(texCoordLoc, GL_FLOAT, 2 * sizeof(GLfloat), 2, 4 * sizeof(GLfloat));
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        m_nv12UvShaderProgram->disableAttributeArray(positionLoc);
        m_nv12UvShaderProgram->disableAttributeArray(texCoordLoc);
        m_quadBuffer.release();
        m_nv12UvShaderProgram->release();
        m_nv12UvFbo->release();
        if (nv12ConvertMs) {
            *nv12ConvertMs += nv12Timer.elapsed();
        }

        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        QElapsedTimer readbackTimer;
        readbackTimer.start();
        m_nv12YFbo->bind();
        glPixelStorei(GL_PACK_ROW_LENGTH, frame->linesize[0]);
        glReadPixels(0, 0, frame->width, frame->height, GL_RED, GL_UNSIGNED_BYTE, frame->data[0]);
        m_nv12YFbo->release();
        m_nv12UvFbo->bind();
        glPixelStorei(GL_PACK_ROW_LENGTH, frame->linesize[1] / 2);
        glReadPixels(0, 0, qMax(1, frame->width / 2), qMax(1, frame->height / 2), GL_RG, GL_UNSIGNED_BYTE, frame->data[1]);
        m_nv12UvFbo->release();
        glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        if (readbackMs) {
            *readbackMs += readbackTimer.elapsed();
        }
        return true;
    }

private:
    void releaseResources() {
        if (!m_context || !m_surface) {
            return;
        }
        if (m_context->makeCurrent(m_surface.get())) {
            for (auto it = m_textureCache.begin(); it != m_textureCache.end(); ++it) {
                editor::destroyGlTextureEntry(&it.value());
            }
            m_textureCache.clear();
            for (auto it = m_reusableTextureCache.begin(); it != m_reusableTextureCache.end(); ++it) {
                editor::destroyGlTextureEntry(&it.value());
            }
            m_reusableTextureCache.clear();
            m_fbo.reset();
            m_nv12YFbo.reset();
            m_nv12UvFbo.reset();
            if (m_quadBuffer.isCreated()) {
                m_quadBuffer.destroy();
            }
            m_shaderProgram.reset();
            m_nv12YShaderProgram.reset();
            m_nv12UvShaderProgram.reset();
            m_context->doneCurrent();
        }
    }

    editor::GlTextureCacheEntry* textureForFrame(const editor::FrameHandle& frame) {
        if (editor::shouldUseReusableTextureCache(frame)) {
            const QString reusableKey = editor::reusableTextureCacheKey(frame);
            editor::GlTextureCacheEntry& reusableEntry = m_reusableTextureCache[reusableKey];
            if (!editor::uploadFrameToGlTextureEntry(frame, &reusableEntry)) {
                editor::destroyGlTextureEntry(&reusableEntry);
                return nullptr;
            }
            return &reusableEntry;
        }

        const QString key = editor::textureCacheKey(frame);
        const qint64 decodeTimestamp = frame.data() ? frame.data()->decodeTimestamp : 0;
        auto it = m_textureCache.find(key);
        if (it != m_textureCache.end() &&
            it->textureId != 0 &&
            it->decodeTimestamp == decodeTimestamp) {
            it->lastUsedMs = QDateTime::currentMSecsSinceEpoch();
            return &it.value();
        }
        editor::GlTextureCacheEntry entry = it != m_textureCache.end()
                                                ? it.value()
                                                : editor::GlTextureCacheEntry{};
        editor::destroyGlTextureEntry(&entry);
        if (!editor::uploadFrameToGlTextureEntry(frame, &entry)) {
            editor::destroyGlTextureEntry(&entry);
            return nullptr;
        }
        entry.decodeTimestamp = decodeTimestamp;
        m_textureCache.insert(key, entry);
        return &m_textureCache[key];
    }

    void trimTextureCache() {
        static constexpr int kMaxTextureCacheEntries = 180;
        editor::trimGlTextureCache(&m_textureCache, kMaxTextureCacheEntries);

        static constexpr int kMaxReusableTextureCacheEntries = 32;
        editor::trimGlTextureCache(&m_reusableTextureCache, kMaxReusableTextureCacheEntries);
    }

    QSize m_outputSize;
    std::unique_ptr<QOffscreenSurface> m_surface;
    std::unique_ptr<QOpenGLContext> m_context;
    std::unique_ptr<QOpenGLFramebufferObject> m_fbo;
    std::unique_ptr<QOpenGLFramebufferObject> m_nv12YFbo;
    std::unique_ptr<QOpenGLFramebufferObject> m_nv12UvFbo;
    std::unique_ptr<QOpenGLShaderProgram> m_shaderProgram;
    std::unique_ptr<QOpenGLShaderProgram> m_nv12YShaderProgram;
    std::unique_ptr<QOpenGLShaderProgram> m_nv12UvShaderProgram;
    QOpenGLBuffer m_quadBuffer;
    QHash<QString, editor::GlTextureCacheEntry> m_textureCache;
    QHash<QString, editor::GlTextureCacheEntry> m_reusableTextureCache;
};

QImage renderTimelineFrame(const RenderRequest& request,
                           int64_t timelineFrame,
                           QHash<QString, editor::DecoderContext*>& decoders,
                           editor::AsyncDecoder* asyncDecoder,
                           QHash<RenderAsyncFrameKey, editor::FrameHandle>* asyncFrameCache,
                           const QVector<TimelineClip>& orderedClips,
                           QHash<QString, RenderClipStageStats>* clipStageStats = nullptr,
                           QJsonArray* skippedClips = nullptr,
                           QJsonObject* skippedReasonCounts = nullptr) {
    QImage canvas(request.outputSize, QImage::Format_ARGB32_Premultiplied);
    canvas.fill(QColor(QStringLiteral("#000000")));

    QPainter painter(&canvas);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    for (const TimelineClip& clip : orderedClips) {
        if (timelineFrame < clip.startFrame || timelineFrame >= clip.startFrame + clip.durationFrames) {
            continue;
        }

        const TimelineClip::GradingKeyframe grade = evaluateClipGradingAtFrame(clip, timelineFrame);
        if (grade.opacity <= 0.0001) {
            recordRenderSkip(skippedClips, skippedReasonCounts, clip, QStringLiteral("zero_opacity"), timelineFrame);
            continue;
        }

        const QString path = clip.filePath;
        const int64_t localFrame =
            sourceFrameForClipAtTimelinePosition(clip, static_cast<qreal>(timelineFrame), request.renderSyncMarkers);
        QElapsedTimer decodeTimer;
        decodeTimer.start();
        const editor::FrameHandle frame =
            decodeRenderFrame(path, localFrame, decoders, asyncDecoder, asyncFrameCache);
        const qint64 decodeElapsed = decodeTimer.elapsed();
        if (frame.isNull()) {
            recordRenderSkip(skippedClips,
                             skippedReasonCounts,
                             clip,
                             QStringLiteral("frame_null_or_decoder_init_failed"),
                             timelineFrame,
                             localFrame);
            continue;
        }
        if (!frame.hasCpuImage()) {
            recordRenderSkip(skippedClips, skippedReasonCounts, clip, QStringLiteral("no_cpu_image"), timelineFrame, localFrame);
            continue;
        }

        const QImage graded = applyClipGrade(frame.cpuImage(), grade);
        const QRect fitted = fitRect(graded.size(), request.outputSize);
        const TimelineClip::TransformKeyframe transform =
            evaluateClipTransformAtPosition(clip, static_cast<qreal>(timelineFrame));

        QElapsedTimer compositeTimer;
        compositeTimer.start();
        painter.save();
        painter.translate(fitted.center().x() + transform.translationX,
                          fitted.center().y() + transform.translationY);
        painter.rotate(transform.rotation);
        painter.scale(transform.scaleX, transform.scaleY);
        const QRectF drawRect(-fitted.width() / 2.0,
                              -fitted.height() / 2.0,
                              fitted.width(),
                              fitted.height());
        painter.drawImage(drawRect, graded);
        painter.restore();
        accumulateClipStageStats(clipStageStats, clip, decodeElapsed, 0, compositeTimer.elapsed());
    }

    return canvas;
}

bool encodeFrame(AVCodecContext* codecCtx,
                 AVStream* stream,
                 AVFormatContext* formatCtx,
                 AVFrame* frame,
                 QString* errorMessage) {
    int ret = avcodec_send_frame(codecCtx, frame);
    if (ret < 0) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to send frame to encoder: %1").arg(avErrToString(ret));
        }
        return false;
    }

    AVPacket* packet = av_packet_alloc();
    if (!packet) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to allocate output packet.");
        }
        return false;
    }

    while (true) {
        ret = avcodec_receive_packet(codecCtx, packet);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        }
        if (ret < 0) {
            av_packet_free(&packet);
            if (errorMessage) {
                *errorMessage = QStringLiteral("Failed to receive encoded packet: %1").arg(avErrToString(ret));
            }
            return false;
        }

        av_packet_rescale_ts(packet, codecCtx->time_base, stream->time_base);
        packet->stream_index = stream->index;
        ret = av_interleaved_write_frame(formatCtx, packet);
        av_packet_unref(packet);
        if (ret < 0) {
            av_packet_free(&packet);
            if (errorMessage) {
                *errorMessage = QStringLiteral("Failed to write output packet: %1").arg(avErrToString(ret));
            }
            return false;
        }
    }

    av_packet_free(&packet);
    return true;
}
}

RenderResult renderTimelineToFile(const RenderRequest& request,
                                  const std::function<bool(const RenderProgress&)>& progressCallback) {
    RenderResult result;
    if (request.outputPath.isEmpty()) {
        result.message = QStringLiteral("No output path selected.");
        return result;
    }

    if (request.outputSize.width() <= 0 || request.outputSize.height() <= 0) {
        result.message = QStringLiteral("Invalid output size.");
        return result;
    }

    QVector<ExportRangeSegment> exportRanges = request.exportRanges;
    if (exportRanges.isEmpty()) {
        const int64_t exportStart = qMax<int64_t>(0, request.exportStartFrame);
        const int64_t exportEnd = qMax(exportStart, request.exportEndFrame);
        exportRanges.push_back(ExportRangeSegment{exportStart, exportEnd});
    }
    std::sort(exportRanges.begin(), exportRanges.end(), [](const ExportRangeSegment& a, const ExportRangeSegment& b) {
        if (a.startFrame == b.startFrame) {
            return a.endFrame < b.endFrame;
        }
        return a.startFrame < b.startFrame;
    });
    int64_t totalFramesToRender = 0;
    for (const ExportRangeSegment& range : exportRanges) {
        const int64_t exportStart = qMax<int64_t>(0, range.startFrame);
        const int64_t exportEnd = qMax(exportStart, range.endFrame);
        totalFramesToRender += (exportEnd - exportStart + 1);
    }
    const QVector<TimelineClip> orderedClips = sortedVisualClips(request.clips);
    QString gpuInitializationError;
    OffscreenGpuRenderer gpuRenderer;
    const bool useGpuRenderer = gpuRenderer.initialize(request.outputSize, &gpuInitializationError);
    result.usedGpu = useGpuRenderer;

    AVFormatContext* formatCtx = nullptr;
    const QByteArray outputPathBytes = QFile::encodeName(request.outputPath);
    if (avformat_alloc_output_context2(&formatCtx, nullptr, nullptr, outputPathBytes.constData()) < 0 || !formatCtx) {
        result.message = QStringLiteral("Failed to create output format context.");
        return result;
    }

    AVStream* stream = avformat_new_stream(formatCtx, nullptr);
    if (!stream) {
        avformat_free_context(formatCtx);
        result.message = QStringLiteral("Failed to create output stream.");
        return result;
    }

    QString codecLabel;
    const AVCodec* codec = nullptr;
    AVCodecContext* codecCtx = nullptr;
    QStringList attemptedEncoders;
    const QVector<VideoEncoderChoice> encoderChoices = videoEncoderChoicesForRequest(request.outputFormat);
    for (const VideoEncoderChoice &choice : encoderChoices) {
        const AVCodec* candidate = avcodec_find_encoder_by_name(choice.label.toUtf8().constData());
        if (!candidate) {
            attemptedEncoders.push_back(choice.label + QStringLiteral(" (unavailable)"));
            continue;
        }

        AVCodecContext* candidateCtx = avcodec_alloc_context3(candidate);
        if (!candidateCtx) {
            attemptedEncoders.push_back(choice.label + QStringLiteral(" (alloc failed)"));
            continue;
        }

        candidateCtx->codec_id = candidate->id;
        candidateCtx->codec_type = AVMEDIA_TYPE_VIDEO;
        candidateCtx->width = request.outputSize.width();
        candidateCtx->height = request.outputSize.height();
        candidateCtx->time_base = AVRational{1, kTimelineFps};
        candidateCtx->framerate = AVRational{kTimelineFps, 1};
        candidateCtx->gop_size = kTimelineFps;
        candidateCtx->max_b_frames = 0;
        candidateCtx->pix_fmt = choice.pixelFormat == AV_PIX_FMT_NONE
                                    ? pixelFormatForCodec(candidate, request.outputFormat)
                                    : choice.pixelFormat;
        candidateCtx->bit_rate = 8'000'000;

        if (formatCtx->oformat->flags & AVFMT_GLOBALHEADER) {
            candidateCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        }

        configureCodecOptions(candidateCtx, request.outputFormat, choice.label);
        if (avcodec_open2(candidateCtx, candidate, nullptr) >= 0) {
            codec = candidate;
            codecCtx = candidateCtx;
            codecLabel = choice.label;
            break;
        }

        attemptedEncoders.push_back(choice.label + QStringLiteral(" (open failed)"));
        avcodec_free_context(&candidateCtx);
    }

    if (!codec || !codecCtx) {
        codec = codecForRequest(request.outputFormat, &codecLabel);
        if (!codec) {
            avformat_free_context(formatCtx);
            result.message = QStringLiteral("No encoder available for format %1. Tried: %2")
                                 .arg(request.outputFormat, attemptedEncoders.join(QStringLiteral(", ")));
            return result;
        }

        codecCtx = avcodec_alloc_context3(codec);
        if (!codecCtx) {
            avformat_free_context(formatCtx);
            result.message = QStringLiteral("Failed to allocate encoder context.");
            return result;
        }

        codecCtx->codec_id = codec->id;
        codecCtx->codec_type = AVMEDIA_TYPE_VIDEO;
        codecCtx->width = request.outputSize.width();
        codecCtx->height = request.outputSize.height();
        codecCtx->time_base = AVRational{1, kTimelineFps};
        codecCtx->framerate = AVRational{kTimelineFps, 1};
        codecCtx->gop_size = kTimelineFps;
        codecCtx->max_b_frames = 0;
        codecCtx->pix_fmt = pixelFormatForCodec(codec, request.outputFormat);
        codecCtx->bit_rate = 8'000'000;

        if (formatCtx->oformat->flags & AVFMT_GLOBALHEADER) {
            codecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        }

        configureCodecOptions(codecCtx, request.outputFormat, codecLabel);

        if (avcodec_open2(codecCtx, codec, nullptr) < 0) {
            avcodec_free_context(&codecCtx);
            avformat_free_context(formatCtx);
            result.message = QStringLiteral("Failed to open encoder %1.").arg(codecLabel);
            return result;
        }
    }

    const bool usingHardwareEncode = isHardwareEncoderLabel(codecLabel);
    result.usedHardwareEncode = usingHardwareEncode;
    result.encoderLabel = codecLabel;

    if (avcodec_parameters_from_context(stream->codecpar, codecCtx) < 0) {
        avcodec_free_context(&codecCtx);
        avformat_free_context(formatCtx);
        result.message = QStringLiteral("Failed to copy encoder parameters.");
        return result;
    }
    stream->time_base = codecCtx->time_base;

    AudioExportState audioState;
    if (!initializeExportAudio(request, formatCtx, &audioState, &result.message)) {
        avcodec_free_context(&codecCtx);
        avformat_free_context(formatCtx);
        return result;
    }

    if (!(formatCtx->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&formatCtx->pb, outputPathBytes.constData(), AVIO_FLAG_WRITE) < 0) {
            if (audioState.codecCtx) {
                avcodec_free_context(&audioState.codecCtx);
            }
            avcodec_free_context(&codecCtx);
            avformat_free_context(formatCtx);
            result.message = QStringLiteral("Failed to open output file %1.").arg(QDir::toNativeSeparators(request.outputPath));
            return result;
        }
    }

    if (avformat_write_header(formatCtx, nullptr) < 0) {
        if (!(formatCtx->oformat->flags & AVFMT_NOFILE)) {
            avio_closep(&formatCtx->pb);
        }
        if (audioState.codecCtx) {
            avcodec_free_context(&audioState.codecCtx);
        }
        avcodec_free_context(&codecCtx);
        avformat_free_context(formatCtx);
        result.message = QStringLiteral("Failed to write output header.");
        return result;
    }

    const bool directNv12Conversion = codecCtx->pix_fmt == AV_PIX_FMT_NV12;
    SwsContext* swsCtx = nullptr;
    if (!directNv12Conversion) {
        swsCtx = sws_getContext(codecCtx->width,
                                codecCtx->height,
                                AV_PIX_FMT_BGRA,
                                codecCtx->width,
                                codecCtx->height,
                                codecCtx->pix_fmt,
                                SWS_BILINEAR,
                                nullptr,
                                nullptr,
                                nullptr);
        if (!swsCtx) {
            av_write_trailer(formatCtx);
            if (!(formatCtx->oformat->flags & AVFMT_NOFILE)) {
                avio_closep(&formatCtx->pb);
            }
            if (audioState.codecCtx) {
                avcodec_free_context(&audioState.codecCtx);
            }
            avcodec_free_context(&codecCtx);
            avformat_free_context(formatCtx);
            result.message = QStringLiteral("Failed to create render color converter.");
            return result;
        }
    }

    AVFrame* sourceFrame = directNv12Conversion ? nullptr : av_frame_alloc();
    AVFrame* encodedFrame = av_frame_alloc();
    if ((!directNv12Conversion && !sourceFrame) || !encodedFrame) {
        if (sourceFrame) av_frame_free(&sourceFrame);
        if (encodedFrame) av_frame_free(&encodedFrame);
        sws_freeContext(swsCtx);
        av_write_trailer(formatCtx);
        if (!(formatCtx->oformat->flags & AVFMT_NOFILE)) {
            avio_closep(&formatCtx->pb);
        }
        if (audioState.codecCtx) {
            avcodec_free_context(&audioState.codecCtx);
        }
        avcodec_free_context(&codecCtx);
        avformat_free_context(formatCtx);
        result.message = QStringLiteral("Failed to allocate render frames.");
        return result;
    }

    if (sourceFrame) {
        sourceFrame->format = AV_PIX_FMT_BGRA;
        sourceFrame->width = codecCtx->width;
        sourceFrame->height = codecCtx->height;
    }
    encodedFrame->format = codecCtx->pix_fmt;
    encodedFrame->width = codecCtx->width;
    encodedFrame->height = codecCtx->height;

    if ((!sourceFrame || av_frame_get_buffer(sourceFrame, 32) >= 0) &&
        av_frame_get_buffer(encodedFrame, 32) >= 0) {
    } else {
        av_frame_free(&sourceFrame);
        av_frame_free(&encodedFrame);
        sws_freeContext(swsCtx);
        av_write_trailer(formatCtx);
        if (!(formatCtx->oformat->flags & AVFMT_NOFILE)) {
            avio_closep(&formatCtx->pb);
        }
        if (audioState.codecCtx) {
            avcodec_free_context(&audioState.codecCtx);
        }
        avcodec_free_context(&codecCtx);
        avformat_free_context(formatCtx);
        result.message = QStringLiteral("Failed to allocate render frame buffers.");
        return result;
    }

    int64_t outputPts = 0;
    int64_t framesCompleted = 0;
    QHash<QString, editor::DecoderContext*> decoders;
    editor::AsyncDecoder asyncDecoder;
    asyncDecoder.initialize();
    QHash<RenderAsyncFrameKey, editor::FrameHandle> asyncFrameCache;
    QObject::connect(&asyncDecoder,
                     &editor::AsyncDecoder::frameReady,
                     [&](editor::FrameHandle frame) {
                         if (frame.isNull() || frame.sourcePath().isEmpty()) {
                             return;
                         }
                         asyncFrameCache.insert(RenderAsyncFrameKey{frame.sourcePath(), frame.frameNumber()}, frame);
                         while (asyncFrameCache.size() > 256) {
                             asyncFrameCache.erase(asyncFrameCache.begin());
                         }
                     });
    QHash<QString, QVector<TranscriptSection>> transcriptCache;
    QString errorMessage;
    QElapsedTimer totalTimer;
    totalTimer.start();
    qint64 totalRenderStageMs = 0;
    qint64 totalRenderDecodeStageMs = 0;
    qint64 totalRenderTextureStageMs = 0;
    qint64 totalRenderCompositeStageMs = 0;
    qint64 totalRenderNv12StageMs = 0;
    qint64 totalGpuReadbackMs = 0;
    qint64 totalOverlayStageMs = 0;
    qint64 totalConvertStageMs = 0;
    qint64 totalEncodeStageMs = 0;
    qint64 totalAudioStageMs = 0;
    qint64 maxFrameRenderStageMs = 0;
    qint64 maxFrameDecodeStageMs = 0;
    qint64 maxFrameTextureStageMs = 0;
    qint64 maxFrameReadbackStageMs = 0;
    qint64 maxFrameConvertStageMs = 0;
    QHash<QString, RenderClipStageStats> clipStageStats;
    QVector<RenderFrameStageStats> worstFrames;
    QJsonArray lastSkippedClips;
    QJsonObject skippedReasonCounts;

    for (int segmentIndex = 0; segmentIndex < exportRanges.size(); ++segmentIndex) {
        const ExportRangeSegment& range = exportRanges[segmentIndex];
        const int64_t exportStart = qMax<int64_t>(0, range.startFrame);
        const int64_t exportEnd = qMax(exportStart, range.endFrame);
        prewarmRenderSequenceSegment(request,
                                     exportStart,
                                     exportEnd,
                                     orderedClips,
                                     &asyncDecoder,
                                     asyncFrameCache);
        for (int64_t timelineFrame = exportStart; timelineFrame <= exportEnd; ++timelineFrame) {
            enqueueRenderSequenceLookahead(request,
                                          timelineFrame,
                                          orderedClips,
                                          &asyncDecoder,
                                          asyncFrameCache);
            if (progressCallback) {
                RenderProgress progress;
                progress.framesCompleted = framesCompleted;
                progress.totalFrames = totalFramesToRender;
                progress.segmentIndex = segmentIndex + 1;
                progress.segmentCount = exportRanges.size();
                progress.timelineFrame = timelineFrame;
                progress.segmentStartFrame = exportStart;
                progress.segmentEndFrame = exportEnd;
                progress.usingGpu = useGpuRenderer;
                progress.usingHardwareEncode = usingHardwareEncode;
                progress.encoderLabel = codecLabel;
                progress.elapsedMs = totalTimer.elapsed();
                progress.estimatedRemainingMs =
                    progress.framesCompleted > 0
                        ? (progress.elapsedMs * qMax<int64_t>(0, progress.totalFrames - progress.framesCompleted)) /
                              qMax<int64_t>(1, progress.framesCompleted)
                        : -1;
                progress.renderStageMs = totalRenderStageMs;
                progress.renderDecodeStageMs = totalRenderDecodeStageMs;
                progress.renderTextureStageMs = totalRenderTextureStageMs;
                progress.renderCompositeStageMs = totalRenderCompositeStageMs;
                progress.renderNv12StageMs = totalRenderNv12StageMs;
                progress.gpuReadbackMs = totalGpuReadbackMs;
                progress.overlayStageMs = totalOverlayStageMs;
                progress.convertStageMs = totalConvertStageMs;
                progress.encodeStageMs = totalEncodeStageMs;
                progress.audioStageMs = totalAudioStageMs;
                progress.maxFrameRenderStageMs = maxFrameRenderStageMs;
                progress.maxFrameDecodeStageMs = maxFrameDecodeStageMs;
                progress.maxFrameTextureStageMs = maxFrameTextureStageMs;
                progress.maxFrameReadbackStageMs = maxFrameReadbackStageMs;
                progress.maxFrameConvertStageMs = maxFrameConvertStageMs;
                progress.skippedClips = lastSkippedClips;
                progress.skippedClipReasonCounts = skippedReasonCounts;
                progress.renderStageTable = buildRenderStageTable(clipStageStats, totalRenderStageMs, framesCompleted);
                progress.worstFrameTable = buildWorstFrameTable(worstFrames);
                if (!progressCallback(progress)) {
                    result.cancelled = true;
                    errorMessage = QStringLiteral("Render cancelled.");
                    break;
                }
            }
            QElapsedTimer renderStageTimer;
            renderStageTimer.start();
            qint64 frameDecodeMs = 0;
            qint64 frameTextureMs = 0;
            qint64 frameCompositeMs = 0;
            qint64 frameReadbackMs = 0;
            QJsonArray frameSkippedClips;
            QImage rendered = useGpuRenderer
                ? gpuRenderer.renderFrame(request,
                                          timelineFrame,
                                          decoders,
                                          &asyncDecoder,
                                          &asyncFrameCache,
                                          orderedClips,
                                          &clipStageStats,
                                          &frameDecodeMs,
                                          &frameTextureMs,
                                          &frameCompositeMs,
                                          &frameReadbackMs,
                                          &frameSkippedClips,
                                          &skippedReasonCounts)
                : renderTimelineFrame(request,
                                      timelineFrame,
                                      decoders,
                                      &asyncDecoder,
                                      &asyncFrameCache,
                                      orderedClips,
                                      &clipStageStats,
                                      &frameSkippedClips,
                                      &skippedReasonCounts);
            lastSkippedClips = frameSkippedClips;
            totalRenderStageMs += renderStageTimer.elapsed();
            totalRenderDecodeStageMs += frameDecodeMs;
            totalRenderTextureStageMs += frameTextureMs;
            totalRenderCompositeStageMs += frameCompositeMs;
            totalGpuReadbackMs += frameReadbackMs;
            if (rendered.isNull()) {
                errorMessage = useGpuRenderer
                    ? QStringLiteral("Failed to render GPU timeline frame %1.").arg(timelineFrame)
                    : QStringLiteral("Failed to render timeline frame %1.").arg(timelineFrame);
                break;
            }

            QElapsedTimer overlayTimer;
            overlayTimer.start();
            renderTranscriptOverlays(&rendered, request, timelineFrame, orderedClips, transcriptCache);
            totalOverlayStageMs += overlayTimer.elapsed();

            if (progressCallback) {
                RenderProgress progress;
                progress.framesCompleted = framesCompleted;
                progress.totalFrames = totalFramesToRender;
                progress.segmentIndex = segmentIndex + 1;
                progress.segmentCount = exportRanges.size();
                progress.timelineFrame = timelineFrame;
                progress.segmentStartFrame = exportStart;
                progress.segmentEndFrame = exportEnd;
                progress.usingGpu = useGpuRenderer;
                progress.usingHardwareEncode = usingHardwareEncode;
                progress.encoderLabel = codecLabel;
                progress.elapsedMs = totalTimer.elapsed();
                progress.estimatedRemainingMs =
                    progress.framesCompleted > 0
                        ? (progress.elapsedMs * qMax<int64_t>(0, progress.totalFrames - progress.framesCompleted)) /
                              qMax<int64_t>(1, progress.framesCompleted)
                        : -1;
                progress.renderStageMs = totalRenderStageMs;
                progress.renderDecodeStageMs = totalRenderDecodeStageMs;
                progress.renderTextureStageMs = totalRenderTextureStageMs;
                progress.renderCompositeStageMs = totalRenderCompositeStageMs;
                progress.renderNv12StageMs = totalRenderNv12StageMs;
                progress.gpuReadbackMs = totalGpuReadbackMs;
                progress.overlayStageMs = totalOverlayStageMs;
                progress.convertStageMs = totalConvertStageMs;
                progress.encodeStageMs = totalEncodeStageMs;
                progress.audioStageMs = totalAudioStageMs;
                progress.maxFrameRenderStageMs = maxFrameRenderStageMs;
                progress.maxFrameDecodeStageMs = maxFrameDecodeStageMs;
                progress.maxFrameTextureStageMs = maxFrameTextureStageMs;
                progress.maxFrameReadbackStageMs = maxFrameReadbackStageMs;
                progress.maxFrameConvertStageMs = maxFrameConvertStageMs;
                progress.skippedClips = lastSkippedClips;
                progress.skippedClipReasonCounts = skippedReasonCounts;
                progress.renderStageTable = buildRenderStageTable(clipStageStats, totalRenderStageMs, framesCompleted);
                progress.worstFrameTable = buildWorstFrameTable(worstFrames);
                progress.previewFrame = rendered;
                if (!progressCallback(progress)) {
                    result.cancelled = true;
                    errorMessage = QStringLiteral("Render cancelled.");
                    break;
                }
            }

            if ((!sourceFrame || av_frame_make_writable(sourceFrame) >= 0) &&
                av_frame_make_writable(encodedFrame) >= 0) {
            } else {
                errorMessage = QStringLiteral("Failed to make render frame writable.");
                break;
            }

            QElapsedTimer convertTimer;
            convertTimer.start();
            if (directNv12Conversion) {
                const bool gpuNv12Converted =
                    useGpuRenderer && gpuRenderer.convertLastFrameToNv12(encodedFrame,
                                                                         &totalRenderNv12StageMs,
                                                                         &totalGpuReadbackMs);
                if (!gpuNv12Converted && !fillNv12FrameFromImage(rendered, encodedFrame)) {
                    errorMessage = QStringLiteral("Failed to convert rendered frame %1 to NV12 for encoding.").arg(timelineFrame);
                    break;
                }
            } else {
                const int copyBytesPerRow = qMin(static_cast<int>(rendered.bytesPerLine()), sourceFrame->linesize[0]);
                for (int y = 0; y < rendered.height(); ++y) {
                    memcpy(sourceFrame->data[0] + (y * sourceFrame->linesize[0]),
                           rendered.constScanLine(y),
                           copyBytesPerRow);
                }

                if (sws_scale(swsCtx,
                              sourceFrame->data,
                              sourceFrame->linesize,
                              0,
                              sourceFrame->height,
                              encodedFrame->data,
                              encodedFrame->linesize) <= 0) {
                    errorMessage = QStringLiteral("Failed to convert rendered frame %1 for encoding.").arg(timelineFrame);
                    break;
                }
            }
            const qint64 frameConvertMs = convertTimer.elapsed();
            totalConvertStageMs += frameConvertMs;

            encodedFrame->pts = outputPts++;
            QElapsedTimer encodeTimer;
            encodeTimer.start();
            if (!encodeFrame(codecCtx, stream, formatCtx, encodedFrame, &errorMessage)) {
                break;
            }
            totalEncodeStageMs += encodeTimer.elapsed();
            const qint64 frameRenderMs = renderStageTimer.elapsed();
            maxFrameRenderStageMs = qMax(maxFrameRenderStageMs, frameRenderMs);
            maxFrameDecodeStageMs = qMax(maxFrameDecodeStageMs, frameDecodeMs);
            maxFrameTextureStageMs = qMax(maxFrameTextureStageMs, frameTextureMs);
            maxFrameReadbackStageMs = qMax(maxFrameReadbackStageMs, frameReadbackMs);
            maxFrameConvertStageMs = qMax(maxFrameConvertStageMs, frameConvertMs);
            recordWorstFrame(&worstFrames,
                             RenderFrameStageStats{
                                 timelineFrame,
                                 segmentIndex + 1,
                                 frameRenderMs,
                                 frameDecodeMs,
                                 frameTextureMs,
                                 frameReadbackMs,
                                 frameConvertMs
                             });
            ++framesCompleted;
            if (!errorMessage.isEmpty()) {
                break;
            }
        }
        if (!errorMessage.isEmpty()) {
            break;
        }
    }

    if (errorMessage.isEmpty()) {
        encodeFrame(codecCtx, stream, formatCtx, nullptr, &errorMessage);
    }

    if (errorMessage.isEmpty() && audioState.enabled) {
        QElapsedTimer audioTimer;
        audioTimer.start();
        encodeExportAudio(exportRanges, audioState, formatCtx, &errorMessage);
        totalAudioStageMs += audioTimer.elapsed();
    }

    av_write_trailer(formatCtx);
    asyncDecoder.shutdown();
    qDeleteAll(decoders);
    av_frame_free(&sourceFrame);
    av_frame_free(&encodedFrame);
    sws_freeContext(swsCtx);
    if (!(formatCtx->oformat->flags & AVFMT_NOFILE)) {
        avio_closep(&formatCtx->pb);
    }
    if (audioState.codecCtx) {
        avcodec_free_context(&audioState.codecCtx);
    }
    avcodec_free_context(&codecCtx);
    avformat_free_context(formatCtx);

    if (!errorMessage.isEmpty()) {
        result.message = errorMessage;
        result.framesRendered = framesCompleted;
        result.elapsedMs = totalTimer.elapsed();
        result.renderStageMs = totalRenderStageMs;
        result.renderDecodeStageMs = totalRenderDecodeStageMs;
        result.renderTextureStageMs = totalRenderTextureStageMs;
        result.renderCompositeStageMs = totalRenderCompositeStageMs;
        result.renderNv12StageMs = totalRenderNv12StageMs;
        result.gpuReadbackMs = totalGpuReadbackMs;
        result.overlayStageMs = totalOverlayStageMs;
        result.convertStageMs = totalConvertStageMs;
        result.encodeStageMs = totalEncodeStageMs;
        result.audioStageMs = totalAudioStageMs;
        result.maxFrameRenderStageMs = maxFrameRenderStageMs;
        result.maxFrameDecodeStageMs = maxFrameDecodeStageMs;
        result.maxFrameTextureStageMs = maxFrameTextureStageMs;
        result.maxFrameReadbackStageMs = maxFrameReadbackStageMs;
        result.maxFrameConvertStageMs = maxFrameConvertStageMs;
        result.skippedClips = lastSkippedClips;
        result.skippedClipReasonCounts = skippedReasonCounts;
        result.renderStageTable = buildRenderStageTable(clipStageStats, totalRenderStageMs, framesCompleted);
        result.worstFrameTable = buildWorstFrameTable(worstFrames);
        return result;
    }

    result.success = true;
    result.framesRendered = framesCompleted;
    result.elapsedMs = totalTimer.elapsed();
    result.renderStageMs = totalRenderStageMs;
    result.renderDecodeStageMs = totalRenderDecodeStageMs;
    result.renderTextureStageMs = totalRenderTextureStageMs;
    result.renderCompositeStageMs = totalRenderCompositeStageMs;
    result.renderNv12StageMs = totalRenderNv12StageMs;
    result.gpuReadbackMs = totalGpuReadbackMs;
    result.overlayStageMs = totalOverlayStageMs;
    result.convertStageMs = totalConvertStageMs;
    result.encodeStageMs = totalEncodeStageMs;
    result.audioStageMs = totalAudioStageMs;
    result.maxFrameRenderStageMs = maxFrameRenderStageMs;
    result.maxFrameDecodeStageMs = maxFrameDecodeStageMs;
    result.maxFrameTextureStageMs = maxFrameTextureStageMs;
    result.maxFrameReadbackStageMs = maxFrameReadbackStageMs;
    result.maxFrameConvertStageMs = maxFrameConvertStageMs;
    result.skippedClips = lastSkippedClips;
    result.skippedClipReasonCounts = skippedReasonCounts;
    result.renderStageTable = buildRenderStageTable(clipStageStats, totalRenderStageMs, framesCompleted);
    result.worstFrameTable = buildWorstFrameTable(worstFrames);
    result.message = QStringLiteral("Rendered %1 video frames to %2%3")
                         .arg(framesCompleted)
                         .arg(QDir::toNativeSeparators(request.outputPath))
                         .arg(useGpuRenderer ? QString() : QStringLiteral("\nGPU export path unavailable, used CPU fallback: %1").arg(gpuInitializationError));
    return result;
}
