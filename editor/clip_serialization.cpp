#include "clip_serialization.h"

#include <QColor>
#include <QFileInfo>
#include <QJsonArray>

namespace editor
{

QJsonObject clipToJson(const TimelineClip &clip)
    {
        QJsonObject obj;
        obj[QStringLiteral("id")] = clip.id;
        obj[QStringLiteral("filePath")] = clip.filePath;
        obj[QStringLiteral("proxyPath")] = clip.proxyPath;
        obj[QStringLiteral("label")] = clip.label;
        obj[QStringLiteral("mediaType")] = clipMediaTypeToString(clip.mediaType);
        obj[QStringLiteral("sourceKind")] = mediaSourceKindToString(clip.sourceKind);
        obj[QStringLiteral("hasAudio")] = clip.hasAudio;
        obj[QStringLiteral("sourceDurationFrames")] = static_cast<qint64>(clip.sourceDurationFrames);
        obj[QStringLiteral("sourceInFrame")] = static_cast<qint64>(clip.sourceInFrame);
        obj[QStringLiteral("sourceInSubframeSamples")] = static_cast<qint64>(clip.sourceInSubframeSamples);
        obj[QStringLiteral("startFrame")] = static_cast<qint64>(clip.startFrame);
        obj[QStringLiteral("startSubframeSamples")] = static_cast<qint64>(clip.startSubframeSamples);
        obj[QStringLiteral("durationFrames")] = static_cast<qint64>(clip.durationFrames);
        obj[QStringLiteral("trackIndex")] = clip.trackIndex;
        obj[QStringLiteral("playbackRate")] = clip.playbackRate;
        obj[QStringLiteral("color")] = clip.color.name(QColor::HexArgb);
        obj[QStringLiteral("brightness")] = clip.brightness;
        obj[QStringLiteral("contrast")] = clip.contrast;
        obj[QStringLiteral("saturation")] = clip.saturation;
        obj[QStringLiteral("opacity")] = clip.opacity;
        obj[QStringLiteral("baseTranslationX")] = clip.baseTranslationX;
        obj[QStringLiteral("baseTranslationY")] = clip.baseTranslationY;
        obj[QStringLiteral("baseRotation")] = clip.baseRotation;
        obj[QStringLiteral("baseScaleX")] = clip.baseScaleX;
        obj[QStringLiteral("baseScaleY")] = clip.baseScaleY;
        QJsonArray keyframes;
        for (const TimelineClip::TransformKeyframe &keyframe : clip.transformKeyframes)
        {
            QJsonObject keyframeObj;
            keyframeObj[QStringLiteral("frame")] = static_cast<qint64>(keyframe.frame);
            keyframeObj[QStringLiteral("translationX")] = keyframe.translationX;
            keyframeObj[QStringLiteral("translationY")] = keyframe.translationY;
            keyframeObj[QStringLiteral("rotation")] = keyframe.rotation;
            keyframeObj[QStringLiteral("scaleX")] = keyframe.scaleX;
            keyframeObj[QStringLiteral("scaleY")] = keyframe.scaleY;
            keyframeObj[QStringLiteral("linearInterpolation")] = keyframe.linearInterpolation;
            keyframes.push_back(keyframeObj);
        }
        obj[QStringLiteral("transformKeyframes")] = keyframes;
        QJsonArray gradingKeyframes;
        for (const TimelineClip::GradingKeyframe &keyframe : clip.gradingKeyframes)
        {
            QJsonObject keyframeObj;
            keyframeObj[QStringLiteral("frame")] = static_cast<qint64>(keyframe.frame);
            keyframeObj[QStringLiteral("brightness")] = keyframe.brightness;
            keyframeObj[QStringLiteral("contrast")] = keyframe.contrast;
            keyframeObj[QStringLiteral("saturation")] = keyframe.saturation;
            keyframeObj[QStringLiteral("opacity")] = keyframe.opacity;
            keyframeObj[QStringLiteral("linearInterpolation")] = keyframe.linearInterpolation;
            gradingKeyframes.push_back(keyframeObj);
        }
        obj[QStringLiteral("gradingKeyframes")] = gradingKeyframes;
        QJsonObject transcriptOverlayObj;
        transcriptOverlayObj[QStringLiteral("enabled")] = clip.transcriptOverlay.enabled;
        transcriptOverlayObj[QStringLiteral("autoScroll")] = clip.transcriptOverlay.autoScroll;
        transcriptOverlayObj[QStringLiteral("translationX")] = clip.transcriptOverlay.translationX;
        transcriptOverlayObj[QStringLiteral("translationY")] = clip.transcriptOverlay.translationY;
        transcriptOverlayObj[QStringLiteral("boxWidth")] = clip.transcriptOverlay.boxWidth;
        transcriptOverlayObj[QStringLiteral("boxHeight")] = clip.transcriptOverlay.boxHeight;
        transcriptOverlayObj[QStringLiteral("maxLines")] = clip.transcriptOverlay.maxLines;
        transcriptOverlayObj[QStringLiteral("maxCharsPerLine")] = clip.transcriptOverlay.maxCharsPerLine;
        transcriptOverlayObj[QStringLiteral("fontFamily")] = clip.transcriptOverlay.fontFamily;
        transcriptOverlayObj[QStringLiteral("fontPointSize")] = clip.transcriptOverlay.fontPointSize;
        transcriptOverlayObj[QStringLiteral("bold")] = clip.transcriptOverlay.bold;
        transcriptOverlayObj[QStringLiteral("italic")] = clip.transcriptOverlay.italic;
        transcriptOverlayObj[QStringLiteral("textColor")] =
            clip.transcriptOverlay.textColor.name(QColor::HexArgb);
        obj[QStringLiteral("transcriptOverlay")] = transcriptOverlayObj;
        obj[QStringLiteral("fadeSamples")] = clip.fadeSamples;
        obj[QStringLiteral("locked")] = clip.locked;
        return obj;
    }

TimelineClip clipFromJson(const QJsonObject &obj)
    {
        TimelineClip clip;
        clip.id = obj.value(QStringLiteral("id")).toString();
        clip.filePath = obj.value(QStringLiteral("filePath")).toString();
        clip.proxyPath = obj.value(QStringLiteral("proxyPath")).toString();
        clip.label = obj.value(QStringLiteral("label")).toString(QFileInfo(clip.filePath).fileName());
        clip.mediaType = clipMediaTypeFromString(obj.value(QStringLiteral("mediaType")).toString());
        clip.sourceKind = mediaSourceKindFromString(obj.value(QStringLiteral("sourceKind")).toString());
        clip.hasAudio = obj.value(QStringLiteral("hasAudio")).toBool(false);
        clip.sourceDurationFrames = obj.value(QStringLiteral("sourceDurationFrames")).toVariant().toLongLong();
        clip.sourceInFrame = obj.value(QStringLiteral("sourceInFrame")).toVariant().toLongLong();
        clip.sourceInSubframeSamples = obj.value(QStringLiteral("sourceInSubframeSamples")).toVariant().toLongLong();
        clip.startFrame = obj.value(QStringLiteral("startFrame")).toVariant().toLongLong();
        clip.startSubframeSamples = obj.value(QStringLiteral("startSubframeSamples")).toVariant().toLongLong();
        clip.durationFrames = obj.value(QStringLiteral("durationFrames")).toVariant().toLongLong();
        clip.trackIndex = obj.value(QStringLiteral("trackIndex")).toInt(-1);
        clip.playbackRate = obj.value(QStringLiteral("playbackRate")).toDouble(1.0);
        if (clip.durationFrames == 0)
            clip.durationFrames = 120;
        if (clip.mediaType == ClipMediaType::Unknown && !clip.filePath.isEmpty())
        {
            const MediaProbeResult probe = probeMediaFile(clip.filePath, clip.durationFrames);
            clip.mediaType = probe.mediaType;
            clip.sourceKind = probe.sourceKind;
            clip.hasAudio = probe.hasAudio;
            clip.durationFrames = probe.durationFrames;
            clip.sourceDurationFrames = probe.durationFrames;
        }
        if (clip.sourceDurationFrames <= 0)
        {
            clip.sourceDurationFrames = clip.sourceInFrame + clip.durationFrames;
        }
        normalizeClipTiming(clip);
        clip.color = QColor(obj.value(QStringLiteral("color")).toString());
        if (!clip.color.isValid())
        {
            clip.color = QColor::fromHsv(static_cast<int>(qHash(clip.filePath) % 360), 160, 220, 220);
        }
        if (clip.mediaType == ClipMediaType::Audio)
        {
            clip.color = QColor(QStringLiteral("#2f7f93"));
        }
        clip.brightness = obj.value(QStringLiteral("brightness")).toDouble(0.0);
        clip.contrast = obj.value(QStringLiteral("contrast")).toDouble(1.0);
        clip.saturation = obj.value(QStringLiteral("saturation")).toDouble(1.0);
        clip.opacity = obj.value(QStringLiteral("opacity")).toDouble(1.0);
        clip.baseTranslationX = obj.value(QStringLiteral("baseTranslationX")).toDouble(0.0);
        clip.baseTranslationY = obj.value(QStringLiteral("baseTranslationY")).toDouble(0.0);
        clip.baseRotation = obj.value(QStringLiteral("baseRotation")).toDouble(0.0);
        clip.baseScaleX = obj.value(QStringLiteral("baseScaleX")).toDouble(1.0);
        clip.baseScaleY = obj.value(QStringLiteral("baseScaleY")).toDouble(1.0);
        const QJsonArray keyframes = obj.value(QStringLiteral("transformKeyframes")).toArray();
        for (const QJsonValue &value : keyframes)
        {
            if (!value.isObject())
            {
                continue;
            }
            const QJsonObject keyframeObj = value.toObject();
            TimelineClip::TransformKeyframe keyframe;
            keyframe.frame = keyframeObj.value(QStringLiteral("frame")).toVariant().toLongLong();
            keyframe.translationX = keyframeObj.value(QStringLiteral("translationX")).toDouble(0.0);
            keyframe.translationY = keyframeObj.value(QStringLiteral("translationY")).toDouble(0.0);
            keyframe.rotation = keyframeObj.value(QStringLiteral("rotation")).toDouble(0.0);
            keyframe.scaleX = keyframeObj.value(QStringLiteral("scaleX")).toDouble(1.0);
            keyframe.scaleY = keyframeObj.value(QStringLiteral("scaleY")).toDouble(1.0);
            if (keyframeObj.contains(QStringLiteral("linearInterpolation"))) {
                keyframe.linearInterpolation =
                    keyframeObj.value(QStringLiteral("linearInterpolation")).toBool(true);
            } else {
                // Backward compatibility with older saved projects.
                keyframe.linearInterpolation =
                    keyframeObj.value(QStringLiteral("interpolated")).toBool(true);
            }
            clip.transformKeyframes.push_back(keyframe);
        }
        const QJsonArray gradingKeyframes = obj.value(QStringLiteral("gradingKeyframes")).toArray();
        for (const QJsonValue &value : gradingKeyframes)
        {
            if (!value.isObject())
            {
                continue;
            }
            const QJsonObject keyframeObj = value.toObject();
            TimelineClip::GradingKeyframe keyframe;
            keyframe.frame = keyframeObj.value(QStringLiteral("frame")).toVariant().toLongLong();
            keyframe.brightness = keyframeObj.value(QStringLiteral("brightness")).toDouble(0.0);
            keyframe.contrast = keyframeObj.value(QStringLiteral("contrast")).toDouble(1.0);
            keyframe.saturation = keyframeObj.value(QStringLiteral("saturation")).toDouble(1.0);
            keyframe.opacity = keyframeObj.value(QStringLiteral("opacity")).toDouble(1.0);
            if (keyframeObj.contains(QStringLiteral("linearInterpolation"))) {
                keyframe.linearInterpolation =
                    keyframeObj.value(QStringLiteral("linearInterpolation")).toBool(true);
            } else {
                keyframe.linearInterpolation = true;
            }
            clip.gradingKeyframes.push_back(keyframe);
        }
        const QJsonObject transcriptOverlayObj = obj.value(QStringLiteral("transcriptOverlay")).toObject();
        clip.transcriptOverlay.enabled = transcriptOverlayObj.value(QStringLiteral("enabled")).toBool(false);
        clip.transcriptOverlay.autoScroll = transcriptOverlayObj.value(QStringLiteral("autoScroll")).toBool(false);
        clip.transcriptOverlay.translationX = transcriptOverlayObj.value(QStringLiteral("translationX")).toDouble(0.0);
        clip.transcriptOverlay.translationY = transcriptOverlayObj.value(QStringLiteral("translationY")).toDouble(640.0);
        clip.transcriptOverlay.boxWidth = transcriptOverlayObj.value(QStringLiteral("boxWidth")).toDouble(900.0);
        clip.transcriptOverlay.boxHeight = transcriptOverlayObj.value(QStringLiteral("boxHeight")).toDouble(220.0);
        clip.transcriptOverlay.maxLines = qMax(1, transcriptOverlayObj.value(QStringLiteral("maxLines")).toInt(2));
        clip.transcriptOverlay.maxCharsPerLine =
            qMax(1, transcriptOverlayObj.value(QStringLiteral("maxCharsPerLine")).toInt(28));
        clip.transcriptOverlay.fontFamily =
            transcriptOverlayObj.value(QStringLiteral("fontFamily")).toString(QStringLiteral("DejaVu Sans"));
        clip.transcriptOverlay.fontPointSize =
            qMax(8, transcriptOverlayObj.value(QStringLiteral("fontPointSize")).toInt(42));
        clip.transcriptOverlay.bold = transcriptOverlayObj.value(QStringLiteral("bold")).toBool(true);
        clip.transcriptOverlay.italic = transcriptOverlayObj.value(QStringLiteral("italic")).toBool(false);
        clip.transcriptOverlay.textColor =
            QColor(transcriptOverlayObj.value(QStringLiteral("textColor")).toString(QStringLiteral("#ffffffff")));
        clip.fadeSamples = qMax(0, obj.value(QStringLiteral("fadeSamples")).toInt(250));
        clip.locked = obj.value(QStringLiteral("locked")).toBool(false);
        normalizeClipTransformKeyframes(clip);
        normalizeClipGradingKeyframes(clip);
        return clip;
    }

} // namespace editor
