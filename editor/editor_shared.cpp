#include "editor_shared.h"

#include <QFile>
#include <QFileInfo>
#include <QSet>

#include <algorithm>
#include <cmath>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
}

namespace {
bool isImageSuffix(const QString& suffix) {
    static const QSet<QString> kImageSuffixes = {
        QStringLiteral("png"),
        QStringLiteral("jpg"),
        QStringLiteral("jpeg"),
        QStringLiteral("webp"),
    };
    return kImageSuffixes.contains(suffix);
}

int clampChannel(int value) {
    return qBound(0, value, 255);
}
}

QString clipMediaTypeToString(ClipMediaType type) {
    switch (type) {
    case ClipMediaType::Image:
        return QStringLiteral("image");
    case ClipMediaType::Video:
        return QStringLiteral("video");
    case ClipMediaType::Audio:
        return QStringLiteral("audio");
    case ClipMediaType::Unknown:
    default:
        return QStringLiteral("unknown");
    }
}

ClipMediaType clipMediaTypeFromString(const QString& value) {
    if (value == QStringLiteral("image")) return ClipMediaType::Image;
    if (value == QStringLiteral("video")) return ClipMediaType::Video;
    if (value == QStringLiteral("audio")) return ClipMediaType::Audio;
    return ClipMediaType::Unknown;
}

QString clipMediaTypeLabel(ClipMediaType type) {
    switch (type) {
    case ClipMediaType::Image:
        return QStringLiteral("Image");
    case ClipMediaType::Video:
        return QStringLiteral("Video");
    case ClipMediaType::Audio:
        return QStringLiteral("Audio");
    case ClipMediaType::Unknown:
    default:
        return QStringLiteral("Unknown");
    }
}

QString renderSyncActionToString(RenderSyncAction action) {
    switch (action) {
    case RenderSyncAction::DuplicateFrame:
        return QStringLiteral("duplicate");
    case RenderSyncAction::SkipFrame:
        return QStringLiteral("skip");
    default:
        return QStringLiteral("duplicate");
    }
}

RenderSyncAction renderSyncActionFromString(const QString& value) {
    if (value == QStringLiteral("skip")) {
        return RenderSyncAction::SkipFrame;
    }
    return RenderSyncAction::DuplicateFrame;
}

QString renderSyncActionLabel(RenderSyncAction action) {
    switch (action) {
    case RenderSyncAction::DuplicateFrame:
        return QStringLiteral("Duplicate");
    case RenderSyncAction::SkipFrame:
        return QStringLiteral("Skip");
    default:
        return QStringLiteral("Duplicate");
    }
}

bool clipHasVisuals(const TimelineClip& clip) {
    return clip.mediaType == ClipMediaType::Image || clip.mediaType == ClipMediaType::Video;
}

bool clipIsAudioOnly(const TimelineClip& clip) {
    return clip.mediaType == ClipMediaType::Audio;
}

int64_t frameToSamples(int64_t frame) {
    return qMax<int64_t>(0, frame * kSamplesPerFrame);
}

qreal samplesToFramePosition(int64_t samples) {
    return static_cast<qreal>(samples) / static_cast<qreal>(kSamplesPerFrame);
}

int64_t clipTimelineStartSamples(const TimelineClip& clip) {
    return frameToSamples(clip.startFrame) + clip.startSubframeSamples;
}

int64_t clipSourceInSamples(const TimelineClip& clip) {
    return frameToSamples(clip.sourceInFrame) + clip.sourceInSubframeSamples;
}

void normalizeSubframeTiming(int64_t& frame, int64_t& subframeSamples) {
    if (subframeSamples >= kSamplesPerFrame) {
        frame += subframeSamples / kSamplesPerFrame;
        subframeSamples %= kSamplesPerFrame;
    }
    while (subframeSamples < 0 && frame > 0) {
        --frame;
        subframeSamples += kSamplesPerFrame;
    }
    if (frame <= 0) {
        frame = 0;
        subframeSamples = qMax<int64_t>(0, subframeSamples);
    }
}

void normalizeClipTiming(TimelineClip& clip) {
    normalizeSubframeTiming(clip.startFrame, clip.startSubframeSamples);
    normalizeSubframeTiming(clip.sourceInFrame, clip.sourceInSubframeSamples);
}

QString transformInterpolationLabel(bool interpolated) {
    return interpolated ? QStringLiteral("Interpolated") : QStringLiteral("Sudden");
}

qreal sanitizeScaleValue(qreal value) {
    if (std::abs(value) < 0.01) {
        return value < 0.0 ? -0.01 : 0.01;
    }
    return value;
}

void normalizeClipTransformKeyframes(TimelineClip& clip) {
    clip.baseScaleX = sanitizeScaleValue(clip.baseScaleX);
    clip.baseScaleY = sanitizeScaleValue(clip.baseScaleY);
    std::sort(clip.transformKeyframes.begin(), clip.transformKeyframes.end(),
              [](const TimelineClip::TransformKeyframe& a, const TimelineClip::TransformKeyframe& b) {
                  return a.frame < b.frame;
              });

    QVector<TimelineClip::TransformKeyframe> normalized;
    normalized.reserve(clip.transformKeyframes.size());
    const int64_t maxFrame = qMax<int64_t>(0, clip.durationFrames - 1);
    for (TimelineClip::TransformKeyframe keyframe : clip.transformKeyframes) {
        keyframe.frame = qBound<int64_t>(0, keyframe.frame, maxFrame);
        keyframe.scaleX = sanitizeScaleValue(keyframe.scaleX);
        keyframe.scaleY = sanitizeScaleValue(keyframe.scaleY);
        if (!normalized.isEmpty() && normalized.constLast().frame == keyframe.frame) {
            normalized.last() = keyframe;
        } else {
            normalized.push_back(keyframe);
        }
    }
    clip.transformKeyframes = normalized;
}

TimelineClip::TransformKeyframe evaluateClipKeyframeOffsetAtFrame(const TimelineClip& clip, int64_t timelineFrame) {
    TimelineClip::TransformKeyframe state;
    if (clip.transformKeyframes.isEmpty()) {
        return state;
    }

    const int64_t localFrame = qBound<int64_t>(0, timelineFrame - clip.startFrame, qMax<int64_t>(0, clip.durationFrames - 1));
    if (localFrame <= clip.transformKeyframes.constFirst().frame) {
        return clip.transformKeyframes.constFirst();
    }

    for (int i = 1; i < clip.transformKeyframes.size(); ++i) {
        const TimelineClip::TransformKeyframe& previous = clip.transformKeyframes[i - 1];
        const TimelineClip::TransformKeyframe& current = clip.transformKeyframes[i];
        if (localFrame < current.frame) {
            if (!current.interpolated || current.frame <= previous.frame) {
                return previous;
            }
            const qreal t = static_cast<qreal>(localFrame - previous.frame) /
                            static_cast<qreal>(current.frame - previous.frame);
            state.frame = localFrame;
            state.translationX = previous.translationX + ((current.translationX - previous.translationX) * t);
            state.translationY = previous.translationY + ((current.translationY - previous.translationY) * t);
            state.rotation = previous.rotation + ((current.rotation - previous.rotation) * t);
            state.scaleX = previous.scaleX + ((current.scaleX - previous.scaleX) * t);
            state.scaleY = previous.scaleY + ((current.scaleY - previous.scaleY) * t);
            state.interpolated = current.interpolated;
            return state;
        }
        if (localFrame == current.frame) {
            return current;
        }
    }

    return clip.transformKeyframes.constLast();
}

TimelineClip::TransformKeyframe evaluateClipTransformAtFrame(const TimelineClip& clip, int64_t timelineFrame) {
    TimelineClip::TransformKeyframe effective = evaluateClipKeyframeOffsetAtFrame(clip, timelineFrame);
    effective.translationX += clip.baseTranslationX;
    effective.translationY += clip.baseTranslationY;
    effective.rotation += clip.baseRotation;
    effective.scaleX = sanitizeScaleValue(clip.baseScaleX * effective.scaleX);
    effective.scaleY = sanitizeScaleValue(clip.baseScaleY * effective.scaleY);
    return effective;
}

TimelineClip::TransformKeyframe evaluateClipTransformAtPosition(const TimelineClip& clip, qreal timelineFramePosition) {
    TimelineClip::TransformKeyframe state;
    if (clip.transformKeyframes.isEmpty()) {
        state.translationX = clip.baseTranslationX;
        state.translationY = clip.baseTranslationY;
        state.rotation = clip.baseRotation;
        state.scaleX = sanitizeScaleValue(clip.baseScaleX);
        state.scaleY = sanitizeScaleValue(clip.baseScaleY);
        return state;
    }

    const qreal maxFrame = static_cast<qreal>(qMax<int64_t>(0, clip.durationFrames - 1));
    const qreal localFrame = qBound<qreal>(0.0, timelineFramePosition - static_cast<qreal>(clip.startFrame), maxFrame);
    if (localFrame <= static_cast<qreal>(clip.transformKeyframes.constFirst().frame)) {
        state = clip.transformKeyframes.constFirst();
    } else {
        state = clip.transformKeyframes.constLast();
        for (int i = 1; i < clip.transformKeyframes.size(); ++i) {
            const TimelineClip::TransformKeyframe& previous = clip.transformKeyframes[i - 1];
            const TimelineClip::TransformKeyframe& current = clip.transformKeyframes[i];
            if (localFrame < static_cast<qreal>(current.frame)) {
                if (!current.interpolated || current.frame <= previous.frame) {
                    state = previous;
                } else {
                    const qreal t = (localFrame - static_cast<qreal>(previous.frame)) /
                                    static_cast<qreal>(current.frame - previous.frame);
                    state.frame = qRound64(localFrame);
                    state.translationX = previous.translationX + ((current.translationX - previous.translationX) * t);
                    state.translationY = previous.translationY + ((current.translationY - previous.translationY) * t);
                    state.rotation = previous.rotation + ((current.rotation - previous.rotation) * t);
                    state.scaleX = previous.scaleX + ((current.scaleX - previous.scaleX) * t);
                    state.scaleY = previous.scaleY + ((current.scaleY - previous.scaleY) * t);
                    state.interpolated = current.interpolated;
                }
                break;
            }
            if (qFuzzyCompare(localFrame + 1.0, static_cast<qreal>(current.frame) + 1.0)) {
                state = current;
                break;
            }
        }
    }

    state.translationX += clip.baseTranslationX;
    state.translationY += clip.baseTranslationY;
    state.rotation += clip.baseRotation;
    state.scaleX = sanitizeScaleValue(clip.baseScaleX * state.scaleX);
    state.scaleY = sanitizeScaleValue(clip.baseScaleY * state.scaleY);
    return state;
}

MediaProbeResult probeMediaFile(const QString& filePath, int64_t fallbackFrames) {
    MediaProbeResult result;
    result.durationFrames = fallbackFrames;

    const QFileInfo info(filePath);
    const QString suffix = info.suffix().toLower();
    if (isImageSuffix(suffix)) {
        result.mediaType = ClipMediaType::Image;
        return result;
    }

    AVFormatContext* formatCtx = nullptr;
    const QByteArray pathBytes = QFile::encodeName(filePath);
    if (avformat_open_input(&formatCtx, pathBytes.constData(), nullptr, nullptr) < 0) {
        return result;
    }

    if (avformat_find_stream_info(formatCtx, nullptr) >= 0) {
        double durationSeconds = 0.0;
        if (formatCtx->duration > 0) {
            durationSeconds = formatCtx->duration / static_cast<double>(AV_TIME_BASE);
        }

        for (unsigned i = 0; i < formatCtx->nb_streams; ++i) {
            const AVStream* stream = formatCtx->streams[i];
            if (!stream || !stream->codecpar) {
                continue;
            }
            if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                result.hasVideo = true;
            } else if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                result.hasAudio = true;
            }
            if (durationSeconds <= 0.0 && stream->duration != AV_NOPTS_VALUE) {
                const double streamSeconds = stream->duration * av_q2d(stream->time_base);
                if (streamSeconds > durationSeconds) {
                    durationSeconds = streamSeconds;
                }
            }
        }

        if (durationSeconds > 0.0) {
            result.durationFrames = qMax<int64_t>(1, qRound64(durationSeconds * kTimelineFps));
        }
    }

    avformat_close_input(&formatCtx);

    if (result.hasVideo) {
        result.mediaType = ClipMediaType::Video;
    } else if (result.hasAudio) {
        result.mediaType = ClipMediaType::Audio;
    }

    return result;
}

QImage applyClipGrade(const QImage& source, const TimelineClip& clip) {
    const bool needsGrade =
        !qFuzzyIsNull(clip.brightness) ||
        !qFuzzyCompare(clip.contrast, 1.0) ||
        !qFuzzyCompare(clip.saturation, 1.0) ||
        !qFuzzyCompare(clip.opacity, 1.0);
    if (source.isNull() || !needsGrade) {
        return source;
    }

    QImage graded = source.convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < graded.height(); ++y) {
        QRgb* row = reinterpret_cast<QRgb*>(graded.scanLine(y));
        for (int x = 0; x < graded.width(); ++x) {
            QColor color = QColor::fromRgba(row[x]);
            float h = 0.0f;
            float s = 0.0f;
            float l = 0.0f;
            float a = 0.0f;
            color.getHslF(&h, &s, &l, &a);

            int r = clampChannel(qRound(((color.red() - 127.5) * clip.contrast) + 127.5 + clip.brightness * 255.0));
            int g = clampChannel(qRound(((color.green() - 127.5) * clip.contrast) + 127.5 + clip.brightness * 255.0));
            int b = clampChannel(qRound(((color.blue() - 127.5) * clip.contrast) + 127.5 + clip.brightness * 255.0));

            QColor adjusted(r, g, b, color.alpha());
            adjusted.getHslF(&h, &s, &l, &a);
            s = qBound(0.0f, static_cast<float>(s * clip.saturation), 1.0f);
            a = qBound(0.0f, static_cast<float>(a * clip.opacity), 1.0f);
            adjusted.setHslF(h, s, l, a);
            row[x] = adjusted.rgba();
        }
    }
    return graded.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}
