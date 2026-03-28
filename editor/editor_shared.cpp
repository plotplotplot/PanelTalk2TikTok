#include "editor_shared.h"

#include <QDir>
#include <QCollator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <cmath>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/pixdesc.h>
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

QStringList collectSequenceFrames(const QString& path) {
    static QMutex cacheMutex;
    static QHash<QString, QStringList> cachedFramesByKey;

    const QFileInfo dirInfo(path);
    if (!dirInfo.exists() || !dirInfo.isDir()) {
        return {};
    }

    const QString cacheKey = dirInfo.absoluteFilePath() + QLatin1Char('|') +
                             QString::number(dirInfo.lastModified().toMSecsSinceEpoch());
    {
        QMutexLocker locker(&cacheMutex);
        const auto it = cachedFramesByKey.constFind(cacheKey);
        if (it != cachedFramesByKey.cend()) {
            return it.value();
        }
    }

    const QDir dir(dirInfo.absoluteFilePath());
    QFileInfoList entries = dir.entryInfoList(QDir::Files | QDir::NoDotAndDotDot, QDir::Name | QDir::IgnoreCase);
    if (entries.size() < 2) {
        return {};
    }

    QHash<QString, QFileInfoList> bySuffix;
    for (const QFileInfo& entry : entries) {
        const QString suffix = entry.suffix().toLower();
        if (!isImageSuffix(suffix)) {
            continue;
        }
        bySuffix[suffix].push_back(entry);
    }

    QFileInfoList bestGroup;
    for (auto it = bySuffix.begin(); it != bySuffix.end(); ++it) {
        if (it.value().size() > bestGroup.size()) {
            bestGroup = it.value();
        }
    }
    if (bestGroup.size() < 2) {
        return {};
    }

    static const QRegularExpression kDigitsPattern(QStringLiteral("(\\d+)"));
    int numberedCount = 0;
    for (const QFileInfo& entry : bestGroup) {
        if (kDigitsPattern.match(entry.completeBaseName()).hasMatch()) {
            ++numberedCount;
        }
    }
    if (numberedCount < 2 || (numberedCount * 2) < bestGroup.size()) {
        return {};
    }

    QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    std::sort(bestGroup.begin(), bestGroup.end(), [&collator](const QFileInfo& a, const QFileInfo& b) {
        return collator.compare(a.fileName(), b.fileName()) < 0;
    });

    QStringList frames;
    frames.reserve(bestGroup.size());
    for (const QFileInfo& entry : bestGroup) {
        frames.push_back(entry.absoluteFilePath());
    }
    {
        QMutexLocker locker(&cacheMutex);
        cachedFramesByKey.insert(cacheKey, frames);
    }
    return frames;
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

QString mediaSourceKindToString(MediaSourceKind kind) {
    switch (kind) {
    case MediaSourceKind::ImageSequence:
        return QStringLiteral("image_sequence");
    case MediaSourceKind::File:
    default:
        return QStringLiteral("file");
    }
}

MediaSourceKind mediaSourceKindFromString(const QString& value) {
    if (value == QStringLiteral("image_sequence")) return MediaSourceKind::ImageSequence;
    return MediaSourceKind::File;
}

QString mediaSourceKindLabel(MediaSourceKind kind) {
    switch (kind) {
    case MediaSourceKind::ImageSequence:
        return QStringLiteral("Image Sequence");
    case MediaSourceKind::File:
    default:
        return QStringLiteral("File");
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
    return clip.mediaType == ClipMediaType::Image ||
           clip.mediaType == ClipMediaType::Video ||
           clip.sourceKind == MediaSourceKind::ImageSequence;
}

bool clipIsAudioOnly(const TimelineClip& clip) {
    return clip.mediaType == ClipMediaType::Audio;
}

bool clipVisualPlaybackEnabled(const TimelineClip& clip) {
    return clipHasVisuals(clip) && clip.videoEnabled;
}

bool clipAudioPlaybackEnabled(const TimelineClip& clip) {
    return clip.hasAudio && clip.audioEnabled;
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
    clip.playbackRate = qBound<qreal>(0.001, clip.playbackRate, 1000.0);
}

QString transformInterpolationLabel(bool linearInterpolation) {
    return linearInterpolation ? QStringLiteral("Linear") : QStringLiteral("Step");
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
    if (clipHasVisuals(clip)) {
        if (normalized.isEmpty()) {
            TimelineClip::TransformKeyframe keyframe;
            keyframe.frame = 0;
            normalized.push_back(keyframe);
        } else if (normalized.constFirst().frame > 0) {
            TimelineClip::TransformKeyframe firstKeyframe = normalized.constFirst();
            firstKeyframe.frame = 0;
            normalized.push_front(firstKeyframe);
        } else {
            normalized.first().frame = 0;
        }
    }
    clip.transformKeyframes = normalized;
}

void normalizeClipGradingKeyframes(TimelineClip& clip) {
    std::sort(clip.gradingKeyframes.begin(), clip.gradingKeyframes.end(),
              [](const TimelineClip::GradingKeyframe& a, const TimelineClip::GradingKeyframe& b) {
                  return a.frame < b.frame;
              });

    QVector<TimelineClip::GradingKeyframe> normalized;
    normalized.reserve(clip.gradingKeyframes.size());
    const int64_t maxFrame = qMax<int64_t>(0, clip.durationFrames - 1);
    for (TimelineClip::GradingKeyframe keyframe : clip.gradingKeyframes) {
        keyframe.frame = qBound<int64_t>(0, keyframe.frame, maxFrame);
        if (!normalized.isEmpty() && normalized.constLast().frame == keyframe.frame) {
            normalized.last() = keyframe;
        } else {
            normalized.push_back(keyframe);
        }
    }

    if (clipHasVisuals(clip)) {
        if (normalized.isEmpty()) {
            TimelineClip::GradingKeyframe keyframe;
            keyframe.frame = 0;
            keyframe.brightness = clip.brightness;
            keyframe.contrast = clip.contrast;
            keyframe.saturation = clip.saturation;
            keyframe.opacity = clip.opacity;
            normalized.push_back(keyframe);
        } else if (normalized.constFirst().frame > 0) {
            TimelineClip::GradingKeyframe firstKeyframe = normalized.constFirst();
            firstKeyframe.frame = 0;
            normalized.push_front(firstKeyframe);
        } else {
            normalized.first().frame = 0;
        }
    }

    clip.gradingKeyframes = normalized;
    if (!clip.gradingKeyframes.isEmpty()) {
        clip.brightness = clip.gradingKeyframes.constFirst().brightness;
        clip.contrast = clip.gradingKeyframes.constFirst().contrast;
        clip.saturation = clip.gradingKeyframes.constFirst().saturation;
        clip.opacity = clip.gradingKeyframes.constFirst().opacity;
    }
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
            if (!current.linearInterpolation || current.frame <= previous.frame) {
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
            state.linearInterpolation = current.linearInterpolation;
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
                if (!current.linearInterpolation || current.frame <= previous.frame) {
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
                    state.linearInterpolation = current.linearInterpolation;
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

TimelineClip::GradingKeyframe evaluateClipGradingAtFrame(const TimelineClip& clip, int64_t timelineFrame) {
    TimelineClip::GradingKeyframe state;
    state.brightness = clip.brightness;
    state.contrast = clip.contrast;
    state.saturation = clip.saturation;
    state.opacity = clip.opacity;
    if (clip.gradingKeyframes.isEmpty()) {
        return state;
    }

    const int64_t localFrame = qBound<int64_t>(0, timelineFrame - clip.startFrame, qMax<int64_t>(0, clip.durationFrames - 1));
    if (localFrame <= clip.gradingKeyframes.constFirst().frame) {
        return clip.gradingKeyframes.constFirst();
    }

    for (int i = 1; i < clip.gradingKeyframes.size(); ++i) {
        const TimelineClip::GradingKeyframe& previous = clip.gradingKeyframes[i - 1];
        const TimelineClip::GradingKeyframe& current = clip.gradingKeyframes[i];
        if (localFrame < current.frame) {
            if (!current.linearInterpolation || current.frame <= previous.frame) {
                return previous;
            }
            const qreal t = static_cast<qreal>(localFrame - previous.frame) /
                            static_cast<qreal>(current.frame - previous.frame);
            state.frame = localFrame;
            state.brightness = previous.brightness + ((current.brightness - previous.brightness) * t);
            state.contrast = previous.contrast + ((current.contrast - previous.contrast) * t);
            state.saturation = previous.saturation + ((current.saturation - previous.saturation) * t);
            state.opacity = previous.opacity + ((current.opacity - previous.opacity) * t);
            state.linearInterpolation = current.linearInterpolation;
            return state;
        }
        if (localFrame == current.frame) {
            return current;
        }
    }

    return clip.gradingKeyframes.constLast();
}

TimelineClip::GradingKeyframe evaluateClipGradingAtPosition(const TimelineClip& clip, qreal timelineFramePosition) {
    TimelineClip::GradingKeyframe state;
    state.brightness = clip.brightness;
    state.contrast = clip.contrast;
    state.saturation = clip.saturation;
    state.opacity = clip.opacity;
    if (clip.gradingKeyframes.isEmpty()) {
        return state;
    }

    const qreal maxFrame = static_cast<qreal>(qMax<int64_t>(0, clip.durationFrames - 1));
    const qreal localFrame = qBound<qreal>(0.0, timelineFramePosition - static_cast<qreal>(clip.startFrame), maxFrame);
    if (localFrame <= static_cast<qreal>(clip.gradingKeyframes.constFirst().frame)) {
        return clip.gradingKeyframes.constFirst();
    }

    state = clip.gradingKeyframes.constLast();
    for (int i = 1; i < clip.gradingKeyframes.size(); ++i) {
        const TimelineClip::GradingKeyframe& previous = clip.gradingKeyframes[i - 1];
        const TimelineClip::GradingKeyframe& current = clip.gradingKeyframes[i];
        if (localFrame < static_cast<qreal>(current.frame)) {
            if (!current.linearInterpolation || current.frame <= previous.frame) {
                return previous;
            }
            const qreal t = (localFrame - static_cast<qreal>(previous.frame)) /
                            static_cast<qreal>(current.frame - previous.frame);
            state.frame = qRound64(localFrame);
            state.brightness = previous.brightness + ((current.brightness - previous.brightness) * t);
            state.contrast = previous.contrast + ((current.contrast - previous.contrast) * t);
            state.saturation = previous.saturation + ((current.saturation - previous.saturation) * t);
            state.opacity = previous.opacity + ((current.opacity - previous.opacity) * t);
            state.linearInterpolation = current.linearInterpolation;
            return state;
        }
        if (qFuzzyCompare(localFrame + 1.0, static_cast<qreal>(current.frame) + 1.0)) {
            return current;
        }
    }
    return state;
}

int64_t adjustedClipLocalFrameAtTimelineFrame(const TimelineClip& clip,
                                              int64_t localTimelineFrame,
                                              const QVector<RenderSyncMarker>& markers) {
    int64_t adjustedLocalFrame = qMax<int64_t>(0, localTimelineFrame);
    int duplicateCarry = 0;
    for (const RenderSyncMarker& marker : markers) {
        if (marker.clipId != clip.id) {
            continue;
        }
        const int64_t markerLocalFrame = marker.frame - clip.startFrame;
        if (markerLocalFrame < 0 || markerLocalFrame >= localTimelineFrame) {
            continue;
        }
        if (duplicateCarry > 0) {
            adjustedLocalFrame -= 1;
            duplicateCarry -= 1;
            continue;
        }
        if (marker.action == RenderSyncAction::DuplicateFrame) {
            adjustedLocalFrame -= 1;
            duplicateCarry = qMax(0, marker.count - 1);
        } else if (marker.action == RenderSyncAction::SkipFrame) {
            adjustedLocalFrame += marker.count;
        }
    }
    return adjustedLocalFrame;
}

int64_t sourceFrameForClipAtTimelinePosition(const TimelineClip& clip,
                                             qreal timelineFramePosition,
                                             const QVector<RenderSyncMarker>& markers) {
    const qreal maxFrame = static_cast<qreal>(qMax<int64_t>(0, clip.durationFrames - 1));
    const qreal localTimelineFramePosition =
        qBound<qreal>(0.0, timelineFramePosition - static_cast<qreal>(clip.startFrame), maxFrame);
    const int64_t steppedLocalTimelineFrame =
        qMax<int64_t>(0, static_cast<int64_t>(std::floor(localTimelineFramePosition)));
    const int64_t adjustedLocalFrame =
        adjustedClipLocalFrameAtTimelineFrame(clip, steppedLocalTimelineFrame, markers);
    const qreal sourceFrameOffset =
        std::floor(static_cast<qreal>(adjustedLocalFrame) * qMax<qreal>(0.001, clip.playbackRate));
    return qMax<int64_t>(0,
                         qMin<int64_t>(qMax<int64_t>(0, clip.sourceDurationFrames - 1),
                                       clip.sourceInFrame + static_cast<int64_t>(sourceFrameOffset)));
}

MediaProbeResult probeMediaFile(const QString& filePath, int64_t fallbackFrames) {
    MediaProbeResult result;
    result.durationFrames = fallbackFrames;

    if (filePath.trimmed().isEmpty()) {
        return result;
    }

    const QFileInfo info(filePath);
    if (!info.exists()) {
        return result;
    }
    if (info.exists() && info.isDir()) {
        const QStringList sequenceFrames = collectSequenceFrames(filePath);
        if (!sequenceFrames.isEmpty()) {
            result.mediaType = ClipMediaType::Video;
            result.sourceKind = MediaSourceKind::ImageSequence;
            result.hasVideo = true;
            result.durationFrames = sequenceFrames.size();
            result.codecName = QStringLiteral("image_sequence");
            QImage firstImage(sequenceFrames.constFirst());
            result.hasAlpha = !firstImage.isNull() && firstImage.hasAlphaChannel();
        }
        return result;
    }
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
                if (result.codecName.isEmpty()) {
                    result.codecName = QString::fromUtf8(avcodec_get_name(stream->codecpar->codec_id));
                }
                const AVPixFmtDescriptor* descriptor =
                    av_pix_fmt_desc_get(static_cast<AVPixelFormat>(stream->codecpar->format));
                if (descriptor && (descriptor->flags & AV_PIX_FMT_FLAG_ALPHA)) {
                    result.hasAlpha = true;
                }
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

QImage applyClipGrade(const QImage& source, const TimelineClip::GradingKeyframe& grade) {
    const bool needsGrade =
        !qFuzzyIsNull(grade.brightness) ||
        !qFuzzyCompare(grade.contrast, 1.0) ||
        !qFuzzyCompare(grade.saturation, 1.0) ||
        !qFuzzyCompare(grade.opacity, 1.0);
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

            int r = clampChannel(qRound(((color.red() - 127.5) * grade.contrast) + 127.5 + grade.brightness * 255.0));
            int g = clampChannel(qRound(((color.green() - 127.5) * grade.contrast) + 127.5 + grade.brightness * 255.0));
            int b = clampChannel(qRound(((color.blue() - 127.5) * grade.contrast) + 127.5 + grade.brightness * 255.0));

            QColor adjusted(r, g, b, color.alpha());
            adjusted.getHslF(&h, &s, &l, &a);
            s = qBound(0.0f, static_cast<float>(s * grade.saturation), 1.0f);
            a = qBound(0.0f, static_cast<float>(a * grade.opacity), 1.0f);
            adjusted.setHslF(h, s, l, a);
            row[x] = adjusted.rgba();
        }
    }
    return graded.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

QImage applyClipGrade(const QImage& source, const TimelineClip& clip) {
    return applyClipGrade(source, evaluateClipGradingAtFrame(clip, clip.startFrame));
}

QString playbackProxyPathForClip(const TimelineClip& clip) {
    if (!clip.proxyPath.isEmpty() && QFileInfo::exists(clip.proxyPath)) {
        return clip.proxyPath;
    }
    if (clip.filePath.isEmpty() || !clipHasVisuals(clip)) {
        return QString();
    }

    const QFileInfo sourceInfo(clip.filePath);
    if (!sourceInfo.exists()) {
        return QString();
    }

    const QString baseName = sourceInfo.completeBaseName();
    const QDir sourceDir = sourceInfo.dir();
    const QStringList candidateNames = {
        baseName + QStringLiteral(".proxy.mov"),
        baseName + QStringLiteral(".proxy.mp4"),
        baseName + QStringLiteral(".proxy.mkv"),
        baseName + QStringLiteral(".proxy.webm"),
        baseName + QStringLiteral("_proxy.mov"),
        baseName + QStringLiteral("_proxy.mp4"),
        baseName + QStringLiteral("_proxy.mkv"),
        baseName + QStringLiteral("_proxy.webm")
    };

    for (const QString& candidateName : candidateNames) {
        const QString candidatePath = sourceDir.filePath(candidateName);
        if (QFileInfo::exists(candidatePath)) {
            return candidatePath;
        }
    }

    const QDir proxiesDir(sourceDir.filePath(QStringLiteral("proxies")));
    if (proxiesDir.exists()) {
        const QStringList proxySuffixes = {
            QStringLiteral(".mov"),
            QStringLiteral(".mp4"),
            QStringLiteral(".mkv"),
            QStringLiteral(".webm")
        };
        for (const QString& suffix : proxySuffixes) {
            const QString candidatePath = proxiesDir.filePath(baseName + suffix);
            if (QFileInfo::exists(candidatePath)) {
                return candidatePath;
            }
        }
    }

    return QString();
}

QString playbackMediaPathForClip(const TimelineClip& clip) {
    const QString proxyPath = playbackProxyPathForClip(clip);
    return proxyPath.isEmpty() ? clip.filePath : proxyPath;
}

QString interactivePreviewMediaPathForClip(const TimelineClip& clip) {
    const auto interactivePathAllowed = [durationFrames = clip.durationFrames](const QString& path) {
        if (path.isEmpty()) {
            return false;
        }
        const MediaProbeResult probe = probeMediaFile(path, durationFrames);
        const QString suffix = QFileInfo(path).suffix().toLower();
        const bool disallowAlphaProresMov =
            probe.mediaType == ClipMediaType::Video &&
            probe.hasAlpha &&
            probe.codecName == QStringLiteral("prores") &&
            suffix == QStringLiteral("mov");
        return !disallowAlphaProresMov;
    };

    const QString proxyPath = playbackProxyPathForClip(clip);
    if (!proxyPath.isEmpty()) {
        return interactivePathAllowed(proxyPath) ? proxyPath : QString();
    }
    if (!clipHasVisuals(clip) || clip.filePath.isEmpty()) {
        return clip.filePath;
    }

    return interactivePathAllowed(clip.filePath) ? clip.filePath : QString();
}

bool isImageSequencePath(const QString& path) {
    return !collectSequenceFrames(path).isEmpty();
}

QStringList imageSequenceFramePaths(const QString& path) {
    return collectSequenceFrames(path);
}

QString imageSequenceDisplayLabel(const QString& path) {
    return QFileInfo(path).fileName();
}

QString transcriptPathForClipFile(const QString& filePath) {
    const QFileInfo info(filePath);
    return info.dir().filePath(info.completeBaseName() + QStringLiteral(".json"));
}

QString transcriptEditablePathForClipFile(const QString& filePath) {
    const QFileInfo info(filePath);
    return info.dir().filePath(info.completeBaseName() + QStringLiteral("_editable.json"));
}

QString transcriptWorkingPathForClipFile(const QString& filePath) {
    const QString editablePath = transcriptEditablePathForClipFile(filePath);
    if (QFileInfo::exists(editablePath)) {
        return editablePath;
    }
    return transcriptPathForClipFile(filePath);
}

bool ensureEditableTranscriptForClipFile(const QString& filePath, QString* editablePathOut) {
    const QString editablePath = transcriptEditablePathForClipFile(filePath);
    if (editablePathOut) {
        *editablePathOut = editablePath;
    }
    if (QFileInfo::exists(editablePath)) {
        return true;
    }

    const QString originalPath = transcriptPathForClipFile(filePath);
    if (!QFileInfo::exists(originalPath)) {
        return false;
    }
    QFile::remove(editablePath);
    return QFile::copy(originalPath, editablePath);
}

QVector<TranscriptSection> loadTranscriptSections(const QString& transcriptPath) {
    QFile transcriptFile(transcriptPath);
    if (!transcriptFile.open(QIODevice::ReadOnly)) {
        return {};
    }

    QJsonParseError parseError;
    const QJsonDocument transcriptDoc = QJsonDocument::fromJson(transcriptFile.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !transcriptDoc.isObject()) {
        return {};
    }

    QVector<TranscriptWord> words;
    const QJsonArray segments = transcriptDoc.object().value(QStringLiteral("segments")).toArray();
    for (const QJsonValue& segmentValue : segments) {
        const QJsonArray segmentWords = segmentValue.toObject().value(QStringLiteral("words")).toArray();
        for (const QJsonValue& wordValue : segmentWords) {
            const QJsonObject wordObj = wordValue.toObject();
            const QString text = wordObj.value(QStringLiteral("word")).toString().trimmed();
            if (text.isEmpty()) {
                continue;
            }
            const double startSeconds = wordObj.value(QStringLiteral("start")).toDouble(-1.0);
            const double endSeconds = wordObj.value(QStringLiteral("end")).toDouble(-1.0);
            if (startSeconds < 0.0 || endSeconds < startSeconds) {
                continue;
            }
            const int64_t startFrame =
                qMax<int64_t>(0, static_cast<int64_t>(std::floor(startSeconds * kTimelineFps)));
            const int64_t endFrame =
                qMax<int64_t>(startFrame, static_cast<int64_t>(std::ceil(endSeconds * kTimelineFps)) - 1);
            words.push_back({startFrame, endFrame, text});
        }
    }

    QVector<TranscriptSection> sections;
    TranscriptSection current;
    const QRegularExpression punctuationPattern(QStringLiteral("[\\.!\\?;:]$"));
    for (const TranscriptWord& word : std::as_const(words)) {
        if (current.text.isEmpty()) {
            current.startFrame = word.startFrame;
            current.endFrame = word.endFrame;
            current.text = word.text;
            current.words.push_back(word);
        } else {
            current.endFrame = word.endFrame;
            current.text += QStringLiteral(" ") + word.text;
            current.words.push_back(word);
        }
        if (punctuationPattern.match(word.text).hasMatch()) {
            sections.push_back(current);
            current = TranscriptSection();
        }
    }

    if (!current.text.isEmpty()) {
        sections.push_back(current);
    }
    return sections;
}

QString wrappedTranscriptSectionText(const QString& text, int maxCharsPerLine, int maxLines) {
    const int charsPerLine = qMax(1, maxCharsPerLine);
    const int linesAllowed = qMax(1, maxLines);
    const QStringList words = text.simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (words.isEmpty()) {
        return QString();
    }

    QStringList lines;
    QString currentLine;
    int consumedWords = 0;
    for (const QString& word : words) {
        const QString candidate = currentLine.isEmpty() ? word : currentLine + QStringLiteral(" ") + word;
        if (candidate.size() <= charsPerLine || currentLine.isEmpty()) {
            currentLine = candidate;
            ++consumedWords;
            continue;
        }
        lines.push_back(currentLine);
        if (lines.size() >= linesAllowed) {
            break;
        }
        currentLine = word;
        ++consumedWords;
    }
    if (lines.size() < linesAllowed && !currentLine.isEmpty()) {
        lines.push_back(currentLine);
    }
    if (lines.isEmpty()) {
        return QString();
    }
    return lines.join(QLatin1Char('\n'));
}

TranscriptOverlayLayout layoutTranscriptSection(const TranscriptSection& section,
                                               int64_t sourceFrame,
                                               int maxCharsPerLine,
                                               int maxLines,
                                               bool autoScroll) {
    TranscriptOverlayLayout layout;
    if (section.words.isEmpty()) {
        return layout;
    }

    const int charsPerLine = qMax(1, maxCharsPerLine);
    const int linesAllowed = qMax(1, maxLines);
    int activeWordIndex = -1;
    for (int i = 0; i < section.words.size(); ++i) {
        const TranscriptWord& word = section.words.at(i);
        if (sourceFrame >= word.startFrame && sourceFrame <= word.endFrame) {
            activeWordIndex = i;
            break;
        }
    }

    QVector<TranscriptOverlayLine> allLines;
    TranscriptOverlayLine currentLine;
    int currentLength = 0;
    for (int i = 0; i < section.words.size(); ++i) {
        const QString wordText = section.words.at(i).text.simplified();
        if (wordText.isEmpty()) {
            continue;
        }

        const int candidateLength = currentLine.words.isEmpty()
                                        ? wordText.size()
                                        : currentLength + 1 + wordText.size();
        if (!currentLine.words.isEmpty() && candidateLength > charsPerLine) {
            allLines.push_back(currentLine);
            currentLine = TranscriptOverlayLine();
            currentLength = 0;
        }

        currentLine.words.push_back(wordText);
        if (i == activeWordIndex) {
            currentLine.activeWord = currentLine.words.size() - 1;
        }
        currentLength = currentLine.words.join(QStringLiteral(" ")).size();
    }
    if (!currentLine.words.isEmpty()) {
        allLines.push_back(currentLine);
    }
    if (allLines.isEmpty()) {
        return layout;
    }

    int activeLineIndex = -1;
    for (int i = 0; i < allLines.size(); ++i) {
        if (allLines.at(i).activeWord >= 0) {
            activeLineIndex = i;
            break;
        }
    }

    int startLine = 0;
    if (autoScroll && activeLineIndex >= 0 && allLines.size() > linesAllowed) {
        startLine = qBound(0, activeLineIndex - (linesAllowed - 1), allLines.size() - linesAllowed);
    }

    const int endLine = qMin(allLines.size(), startLine + linesAllowed);
    for (int i = startLine; i < endLine; ++i) {
        layout.lines.push_back(allLines.at(i));
    }
    layout.truncatedTop = startLine > 0;
    layout.truncatedBottom = endLine < allLines.size();
    return layout;
}

QString transcriptOverlayHtml(const TranscriptOverlayLayout& layout,
                              const QColor& textColor,
                              const QColor& highlightTextColor,
                              const QColor& highlightFillColor) {
    if (layout.lines.isEmpty()) {
        return QString();
    }

    QStringList htmlLines;
    htmlLines.reserve(layout.lines.size());
    for (int lineIndex = 0; lineIndex < layout.lines.size(); ++lineIndex) {
        const TranscriptOverlayLine& line = layout.lines.at(lineIndex);
        QStringList htmlWords;
        htmlWords.reserve(line.words.size());
        for (int wordIndex = 0; wordIndex < line.words.size(); ++wordIndex) {
            QString wordHtml = line.words.at(wordIndex).toHtmlEscaped();
            if (wordIndex == line.activeWord) {
                wordHtml = QStringLiteral(
                               "<span style=\"background:%1;color:%2;border-radius:0.28em;padding:0.02em 0.18em;\">%3</span>")
                               .arg(highlightFillColor.name(QColor::HexArgb),
                                    highlightTextColor.name(QColor::HexArgb),
                                    wordHtml);
            }
            htmlWords.push_back(wordHtml);
        }

        QString lineHtml = htmlWords.join(QStringLiteral(" "));
        htmlLines.push_back(lineHtml);
    }

    return QStringLiteral("<div style=\"color:%1;text-align:center;\">%2</div>")
        .arg(textColor.name(QColor::HexArgb), htmlLines.join(QStringLiteral("<br/>")));
}
