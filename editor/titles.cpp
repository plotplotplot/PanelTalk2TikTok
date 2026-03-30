#include "titles.h"

#include <QUuid>
#include <cmath>

EvaluatedTitle evaluateTitleAtLocalFrame(const TimelineClip& clip, int64_t localFrame)
{
    EvaluatedTitle result;
    if (clip.titleKeyframes.isEmpty()) {
        return result;
    }

    // Find the keyframe at or before localFrame (step interpolation for text,
    // linear interpolation for numeric properties when enabled).
    int beforeIdx = 0;
    int afterIdx = -1;
    for (int i = 0; i < clip.titleKeyframes.size(); ++i) {
        if (clip.titleKeyframes[i].frame <= localFrame) {
            beforeIdx = i;
        } else if (afterIdx < 0) {
            afterIdx = i;
        }
    }

    const auto& kf = clip.titleKeyframes[beforeIdx];
    result.text = kf.text;
    // Title clips use their own coordinate system in titleKeyframes directly.
    // moveRequested writes to titleKeyframes, not baseTranslation.
    result.x = kf.translationX;
    result.y = kf.translationY;
    result.fontSize = kf.fontSize;
    result.opacity = kf.opacity;
    result.fontFamily = kf.fontFamily;
    result.bold = kf.bold;
    result.italic = kf.italic;
    result.color = kf.color;
    result.valid = true;

    // Linear interpolation of numeric properties between keyframes
    if (afterIdx >= 0 && kf.linearInterpolation) {
        const auto& next = clip.titleKeyframes[afterIdx];
        const int64_t span = next.frame - kf.frame;
        if (span > 0) {
            const qreal t = static_cast<qreal>(localFrame - kf.frame) / static_cast<qreal>(span);
            result.x = kf.translationX + (next.translationX - kf.translationX) * t;
            result.y = kf.translationY + (next.translationY - kf.translationY) * t;
            result.fontSize = kf.fontSize + (next.fontSize - kf.fontSize) * t;
            result.opacity = kf.opacity + (next.opacity - kf.opacity) * t;
            // Text, font, bold, italic, color are NOT interpolated — they step at the keyframe
        }
    }

    return result;
}

TimelineClip createDefaultTitleClip(int64_t startFrame, int trackIndex, int64_t durationFrames)
{
    TimelineClip clip;
    clip.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    clip.mediaType = ClipMediaType::Title;
    clip.label = QStringLiteral("Title");
    clip.startFrame = startFrame;
    clip.trackIndex = trackIndex;
    clip.durationFrames = durationFrames;
    clip.sourceDurationFrames = durationFrames;
    clip.videoEnabled = true;
    clip.audioEnabled = false;
    clip.hasAudio = false;
    clip.color = QColor(QStringLiteral("#4a2d6b"));

    TimelineClip::TitleKeyframe defaultKeyframe;
    defaultKeyframe.frame = 0;
    defaultKeyframe.text = QStringLiteral("Title");
    defaultKeyframe.translationX = 0.0;
    defaultKeyframe.translationY = 0.0;
    defaultKeyframe.fontSize = 48.0;
    defaultKeyframe.opacity = 1.0;
    clip.titleKeyframes.push_back(defaultKeyframe);

    return clip;
}

void drawTitleOverlay(QPainter* painter, const QRect& canvasRect,
                      const EvaluatedTitle& title, const QSize& outputSize)
{
    if (!painter || !title.valid || title.text.isEmpty()) {
        return;
    }
    if (title.opacity <= 0.001) {
        return;
    }

    painter->save();

    // Scale from output-canvas coordinates to widget-pixel coordinates
    const qreal scaleX = outputSize.width() > 0
        ? static_cast<qreal>(canvasRect.width()) / outputSize.width() : 1.0;
    const qreal scaleY = outputSize.height() > 0
        ? static_cast<qreal>(canvasRect.height()) / outputSize.height() : 1.0;

    QFont font(title.fontFamily);
    font.setPointSizeF(title.fontSize * qMin(scaleX, scaleY));
    font.setBold(title.bold);
    font.setItalic(title.italic);
    painter->setFont(font);

    QColor textColor = title.color;
    textColor.setAlphaF(title.opacity);
    painter->setPen(textColor);

    // Position is relative to the canvas center, stored in output-canvas units.
    // Scale to widget pixels for rendering.
    const qreal centerX = canvasRect.center().x() + title.x * scaleX;
    const qreal centerY = canvasRect.center().y() + title.y * scaleY;

    const QFontMetricsF fm(font);
    const qreal textWidth = fm.horizontalAdvance(title.text);
    const qreal textHeight = fm.height();

    // Draw drop shadow
    QColor shadowColor(Qt::black);
    shadowColor.setAlphaF(title.opacity * 0.6);
    painter->setPen(shadowColor);
    painter->drawText(QPointF(centerX - textWidth / 2.0 + 2, centerY + textHeight / 4.0 + 2),
                      title.text);

    // Draw text
    painter->setPen(textColor);
    painter->drawText(QPointF(centerX - textWidth / 2.0, centerY + textHeight / 4.0),
                      title.text);

    painter->restore();
}
