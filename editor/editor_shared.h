#pragma once

#include <QColor>
#include <QImage>
#include <QStringList>
#include <QString>
#include <QVector>

#include <cstdint>

enum class ClipMediaType {
    Unknown,
    Image,
    Video,
    Audio,
    Title,
};

enum class ProxyFormat {
    ImageSequence,  // JPEG frames in a directory (default — best compatibility)
    H264,           // H.264 in MOV/MP4 (small files, needs sequential decode)
    MJPEG,          // Motion JPEG in MOV (intra-frame, Linux only)
};

enum class MediaSourceKind {
    File,
    ImageSequence,
};

struct TimelineClip {
    struct TransformKeyframe {
        int64_t frame = 0;
        qreal translationX = 0.0;
        qreal translationY = 0.0;
        qreal rotation = 0.0;
        qreal scaleX = 1.0;
        qreal scaleY = 1.0;
        bool linearInterpolation = true;
    };

    struct GradingKeyframe {
        int64_t frame = 0;
        qreal brightness = 0.0;
        qreal contrast = 1.0;
        qreal saturation = 1.0;
        qreal opacity = 1.0;
        bool linearInterpolation = true;
    };

    struct TitleKeyframe {
        int64_t frame = 0;
        QString text;
        qreal translationX = 0.0;
        qreal translationY = 0.0;
        qreal fontSize = 48.0;
        qreal opacity = 1.0;
        #ifdef __APPLE__
        QString fontFamily = QStringLiteral("Helvetica Neue");
#else
        QString fontFamily = QStringLiteral("DejaVu Sans");
#endif
        bool bold = true;
        bool italic = false;
        QColor color = QColor(QStringLiteral("#ffffff"));
        bool linearInterpolation = true;
    };

    struct TranscriptOverlaySettings {
        bool enabled = false;
        bool autoScroll = false;
        qreal translationX = 0.0;
        qreal translationY = 640.0;
        qreal boxWidth = 900.0;
        qreal boxHeight = 220.0;
        int maxLines = 2;
        int maxCharsPerLine = 28;
        #ifdef __APPLE__
        QString fontFamily = QStringLiteral("Helvetica Neue");
#else
        QString fontFamily = QStringLiteral("DejaVu Sans");
#endif
        int fontPointSize = 42;
        bool bold = true;
        bool italic = false;
        QColor textColor = QColor(QStringLiteral("#ffffff"));
    };

    QString id;
    QString filePath;
    QString proxyPath;
    QString label;
    ClipMediaType mediaType = ClipMediaType::Unknown;
    MediaSourceKind sourceKind = MediaSourceKind::File;
    bool hasAudio = false;
    int64_t sourceDurationFrames = 0;
    int64_t sourceInFrame = 0;
    int64_t sourceInSubframeSamples = 0;
    int64_t startFrame = 0;
    int64_t startSubframeSamples = 0;
    int64_t durationFrames = 90;
    int trackIndex = 0;
    qreal playbackRate = 1.0;
    bool videoEnabled = true;
    bool audioEnabled = true;
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
    QVector<GradingKeyframe> gradingKeyframes;
    QVector<TitleKeyframe> titleKeyframes;
    TranscriptOverlaySettings transcriptOverlay;
    int fadeSamples = 250;  // Crossfade with previous audio clip (0 = no fade)
    bool locked = false;    // When true, prevents temporal adjustments
};

struct TimelineTrack {
    QString name;
    int height = 44;
};

struct ExportRangeSegment {
    int64_t startFrame = 0;
    int64_t endFrame = 0;
};

enum class RenderSyncAction {
    DuplicateFrame,
    SkipFrame,
};

struct RenderSyncMarker {
    QString clipId;
    int64_t frame = 0;
    RenderSyncAction action = RenderSyncAction::DuplicateFrame;
    int count = 1;
};

struct MediaProbeResult {
    ClipMediaType mediaType = ClipMediaType::Unknown;
    MediaSourceKind sourceKind = MediaSourceKind::File;
    bool hasAudio = false;
    bool hasVideo = false;
    bool hasAlpha = false;
    int64_t durationFrames = 120;
    QString codecName;
    QSize frameSize;
};

struct TranscriptWord {
    int64_t startFrame = 0;
    int64_t endFrame = 0;
    QString text;
};

struct TranscriptSection {
    int64_t startFrame = 0;
    int64_t endFrame = 0;
    QString text;
    QVector<TranscriptWord> words;
};

struct TranscriptOverlayLine {
    QStringList words;
    int activeWord = -1;
};

struct TranscriptOverlayLayout {
    QVector<TranscriptOverlayLine> lines;
    bool truncatedTop = false;
    bool truncatedBottom = false;
};

#ifdef __APPLE__
inline const QString kDefaultFontFamily = QStringLiteral("Helvetica Neue");
#else
inline const QString kDefaultFontFamily = QStringLiteral("DejaVu Sans");
#endif

constexpr int kTimelineFps = 30;
constexpr int kAudioSampleRate = 48000;
constexpr int64_t kSamplesPerFrame = kAudioSampleRate / kTimelineFps;
constexpr int64_t kAudioNudgeSamples = (kAudioSampleRate * 25) / 1000;

QString clipMediaTypeToString(ClipMediaType type);
ClipMediaType clipMediaTypeFromString(const QString& value);
QString clipMediaTypeLabel(ClipMediaType type);
QString mediaSourceKindToString(MediaSourceKind kind);
MediaSourceKind mediaSourceKindFromString(const QString& value);
QString mediaSourceKindLabel(MediaSourceKind kind);

QString renderSyncActionToString(RenderSyncAction action);
RenderSyncAction renderSyncActionFromString(const QString& value);
QString renderSyncActionLabel(RenderSyncAction action);

bool clipHasVisuals(const TimelineClip& clip);
bool clipIsAudioOnly(const TimelineClip& clip);
bool clipVisualPlaybackEnabled(const TimelineClip& clip);
bool clipAudioPlaybackEnabled(const TimelineClip& clip);

int64_t frameToSamples(int64_t frame);
qreal samplesToFramePosition(int64_t samples);
int64_t clipTimelineStartSamples(const TimelineClip& clip);
int64_t clipSourceInSamples(const TimelineClip& clip);
void normalizeSubframeTiming(int64_t& frame, int64_t& subframeSamples);
void normalizeClipTiming(TimelineClip& clip);

QString transformInterpolationLabel(bool linearInterpolation);
qreal sanitizeScaleValue(qreal value);
void normalizeClipTransformKeyframes(TimelineClip& clip);
void normalizeClipGradingKeyframes(TimelineClip& clip);
void normalizeClipTitleKeyframes(TimelineClip& clip);
TimelineClip::TransformKeyframe evaluateClipKeyframeOffsetAtFrame(const TimelineClip& clip, int64_t timelineFrame);
TimelineClip::TransformKeyframe evaluateClipTransformAtFrame(const TimelineClip& clip, int64_t timelineFrame);
TimelineClip::TransformKeyframe evaluateClipTransformAtPosition(const TimelineClip& clip, qreal timelineFramePosition);
TimelineClip::GradingKeyframe evaluateClipGradingAtFrame(const TimelineClip& clip, int64_t timelineFrame);
TimelineClip::GradingKeyframe evaluateClipGradingAtPosition(const TimelineClip& clip, qreal timelineFramePosition);
int64_t adjustedClipLocalFrameAtTimelineFrame(const TimelineClip& clip,
                                              int64_t localTimelineFrame,
                                              const QVector<RenderSyncMarker>& markers);
int64_t sourceFrameForClipAtTimelinePosition(const TimelineClip& clip,
                                             qreal timelineFramePosition,
                                             const QVector<RenderSyncMarker>& markers);
int64_t sourceSampleForClipAtTimelineSample(const TimelineClip& clip,
                                            int64_t timelineSample,
                                            const QVector<RenderSyncMarker>& markers);

MediaProbeResult probeMediaFile(const QString& filePath, int64_t fallbackFrames = 120);
QImage applyClipGrade(const QImage& source, const TimelineClip& clip);
QImage applyClipGrade(const QImage& source, const TimelineClip::GradingKeyframe& grade);
QString playbackProxyPathForClip(const TimelineClip& clip);
QString playbackMediaPathForClip(const TimelineClip& clip);
QString interactivePreviewMediaPathForClip(const TimelineClip& clip);
bool isImageSequencePath(const QString& path);
QStringList imageSequenceFramePaths(const QString& path);
QString imageSequenceDisplayLabel(const QString& path);
QString transcriptPathForClipFile(const QString& filePath);
QString transcriptEditablePathForClipFile(const QString& filePath);
QString transcriptWorkingPathForClipFile(const QString& filePath);
bool ensureEditableTranscriptForClipFile(const QString& filePath, QString* editablePathOut = nullptr);
QVector<TranscriptSection> loadTranscriptSections(const QString& transcriptPath);
QString wrappedTranscriptSectionText(const QString& text, int maxCharsPerLine, int maxLines);
TranscriptOverlayLayout layoutTranscriptSection(const TranscriptSection& section,
                                               int64_t sourceFrame,
                                               int maxCharsPerLine,
                                               int maxLines,
                                               bool autoScroll);
QString transcriptOverlayHtml(const TranscriptOverlayLayout& layout,
                              const QColor& textColor,
                              const QColor& highlightTextColor,
                              const QColor& highlightFillColor);
