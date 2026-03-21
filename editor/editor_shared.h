#pragma once

#include <QColor>
#include <QImage>
#include <QString>
#include <QVector>

#include <cstdint>

enum class ClipMediaType {
    Unknown,
    Image,
    Video,
    Audio,
};

struct TimelineClip {
    struct TransformKeyframe {
        int64_t frame = 0;
        qreal translationX = 0.0;
        qreal translationY = 0.0;
        qreal rotation = 0.0;
        qreal scaleX = 1.0;
        qreal scaleY = 1.0;
        bool interpolated = true;
    };

    QString id;
    QString filePath;
    QString label;
    ClipMediaType mediaType = ClipMediaType::Unknown;
    bool hasAudio = false;
    int64_t sourceDurationFrames = 0;
    int64_t sourceInFrame = 0;
    int64_t sourceInSubframeSamples = 0;
    int64_t startFrame = 0;
    int64_t startSubframeSamples = 0;
    int64_t durationFrames = 90;
    int trackIndex = 0;
    QColor color;
    qreal brightness = 0.0;
    qreal contrast = 1.0;
    qreal saturation = 1.0;
    qreal opacity = 1.0;
    qreal baseTranslationX = 0.0;
    qreal baseTranslationY = 0.0;
    qreal baseRotation = 0.0;
    qreal baseScaleX = 1.0;
    qreal baseScaleY = 1.0;
    QVector<TransformKeyframe> transformKeyframes;
};

struct TimelineTrack {
    QString name;
    int height = 44;
};

enum class RenderSyncAction {
    DuplicateFrame,
    SkipFrame,
};

struct RenderSyncMarker {
    int64_t frame = 0;
    RenderSyncAction action = RenderSyncAction::DuplicateFrame;
};

struct MediaProbeResult {
    ClipMediaType mediaType = ClipMediaType::Unknown;
    bool hasAudio = false;
    bool hasVideo = false;
    int64_t durationFrames = 120;
};

constexpr int kTimelineFps = 30;
constexpr int kAudioSampleRate = 48000;
constexpr int64_t kSamplesPerFrame = kAudioSampleRate / kTimelineFps;
constexpr int64_t kAudioNudgeSamples = (kAudioSampleRate * 25) / 1000;

QString clipMediaTypeToString(ClipMediaType type);
ClipMediaType clipMediaTypeFromString(const QString& value);
QString clipMediaTypeLabel(ClipMediaType type);

QString renderSyncActionToString(RenderSyncAction action);
RenderSyncAction renderSyncActionFromString(const QString& value);
QString renderSyncActionLabel(RenderSyncAction action);

bool clipHasVisuals(const TimelineClip& clip);
bool clipIsAudioOnly(const TimelineClip& clip);

int64_t frameToSamples(int64_t frame);
qreal samplesToFramePosition(int64_t samples);
int64_t clipTimelineStartSamples(const TimelineClip& clip);
int64_t clipSourceInSamples(const TimelineClip& clip);
void normalizeSubframeTiming(int64_t& frame, int64_t& subframeSamples);
void normalizeClipTiming(TimelineClip& clip);

QString transformInterpolationLabel(bool interpolated);
qreal sanitizeScaleValue(qreal value);
void normalizeClipTransformKeyframes(TimelineClip& clip);
TimelineClip::TransformKeyframe evaluateClipKeyframeOffsetAtFrame(const TimelineClip& clip, int64_t timelineFrame);
TimelineClip::TransformKeyframe evaluateClipTransformAtFrame(const TimelineClip& clip, int64_t timelineFrame);
TimelineClip::TransformKeyframe evaluateClipTransformAtPosition(const TimelineClip& clip, qreal timelineFramePosition);

MediaProbeResult probeMediaFile(const QString& filePath, int64_t fallbackFrames = 120);
QImage applyClipGrade(const QImage& source, const TimelineClip& clip);
