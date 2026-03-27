#pragma once

#include <QOpenGLWidget>
#include <QOpenGLFunctions>
#include <QOpenGLBuffer>
#include <QTimer>
#include <QHash>
#include <memory>
#include <functional>

// Include frame_handle first to avoid conflicts with forward declarations
#include "frame_handle.h"
#include "editor_shared.h"
#include "timeline_widget.h"
#include "async_decoder.h"
#include "timeline_cache.h"
#include "playback_frame_pipeline.h"

QT_BEGIN_NAMESPACE
class QOpenGLShaderProgram;
class QOpenGLTexture;
QT_END_NAMESPACE

using namespace editor;

class PreviewWindow final : public QOpenGLWidget, protected QOpenGLFunctions {
    Q_OBJECT
public:
    explicit PreviewWindow(QWidget* parent = nullptr);
    ~PreviewWindow() override;

    void setPlaybackState(bool playing);
    void setCurrentFrame(int64_t frame);
    void setCurrentPlaybackSample(int64_t samplePosition);
    void setClipCount(int count);
    void setSelectedClipId(const QString& clipId);
    void setTimelineClips(const QVector<TimelineClip>& clips);
    void setRenderSyncMarkers(const QVector<RenderSyncMarker>& markers);
    void setExportRanges(const QVector<ExportRangeSegment>& ranges);
    void beginBulkUpdate();
    void endBulkUpdate();
    QString backendName() const;
    void setAudioMuted(bool muted);
    void setAudioVolume(qreal volume);
    void setOutputSize(const QSize& size);
    void setBypassGrading(bool bypass);
    bool bypassGrading() const;
    bool audioMuted() const;
    int audioVolumePercent() const;
    QString activeAudioClipLabel() const;
    bool preparePlaybackAdvance(int64_t targetFrame);
    bool preparePlaybackAdvanceSample(int64_t targetSample);
    QJsonObject profilingSnapshot() const;

    std::function<void(const QString&)> selectionRequested;
    std::function<void(const QString&, qreal, qreal, bool)> resizeRequested;
    std::function<void(const QString&, qreal, qreal, bool)> moveRequested;

protected:
    void paintEvent(QPaintEvent* event) override;
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void showEvent(QShowEvent* event) override;
    void paintGL() override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    enum class PreviewOverlayKind {
        VisualClip,
        TranscriptOverlay,
    };

    struct PreviewOverlayInfo {
        PreviewOverlayKind kind = PreviewOverlayKind::VisualClip;
        QRectF bounds;
        QRectF rightHandle;
        QRectF bottomHandle;
        QRectF cornerHandle;
    };

    struct TextureCacheEntry {
        QOpenGLTexture* texture = nullptr;
        qint64 decodeTimestamp = 0;
        qint64 lastUsedMs = 0;
        QSize size;
    };

    enum class PreviewDragMode {
        None,
        Move,
        ResizeX,
        ResizeY,
        ResizeBoth,
    };

    QRect previewCanvasBaseRect() const;
    QRect scaledCanvasRect(const QRect& baseRect) const;
    QPointF previewCanvasScale(const QRect& targetRect) const;
    bool clipShowsTranscriptOverlay(const TimelineClip& clip) const;
    const QVector<TranscriptSection>& transcriptSectionsForClip(const TimelineClip& clip) const;
    TranscriptOverlayLayout transcriptOverlayLayoutForClip(const TimelineClip& clip) const;
    QRectF transcriptOverlayRectForTarget(const TimelineClip& clip, const QRect& targetRect) const;
    QSizeF transcriptOverlaySizeForSelectedClip() const;
    void drawTranscriptOverlay(QPainter* painter, const TimelineClip& clip, const QRect& targetRect);
    bool usingCpuFallback() const;
    void ensurePipeline();
    void releaseGlResources();
    QString textureCacheKey(const FrameHandle& frame) const;
    QOpenGLTexture* textureForFrame(const FrameHandle& frame);
    void trimTextureCache();
    bool isSampleWithinClip(const TimelineClip& clip, int64_t samplePosition) const;
    int64_t sourceSampleForPlaybackSample(const TimelineClip& clip, int64_t samplePosition) const;
    int64_t sourceFrameForSample(const TimelineClip& clip, int64_t samplePosition) const;
    QRectF renderFrameLayerGL(const QRect& targetRect, const TimelineClip& clip, const FrameHandle& frame);
    void renderCompositedPreviewGL(const QRect& compositeRect,
                                   const QList<TimelineClip>& activeClips,
                                   bool& drewAnyFrame,
                                   bool& waitingForFrame);
    void drawCompositedPreviewOverlay(QPainter* painter,
                                      const QRect& safeRect,
                                      const QRect& compositeRect,
                                      const QList<TimelineClip>& activeClips,
                                      bool drewAnyFrame,
                                      bool waitingForFrame);
    void drawBackground(QPainter* painter);
    QList<TimelineClip> getActiveClips() const;
    void requestFramesForCurrentPosition();
    void scheduleRepaint();
    void drawCompositedPreview(QPainter* painter, const QRect& safeRect,
                               const QList<TimelineClip>& activeClips);
    void drawEmptyState(QPainter* painter, const QRect& safeRect);
    void drawFrameLayer(QPainter* painter, const QRect& targetRect,
                        const TimelineClip& clip, const FrameHandle& frame);
    void drawFramePlaceholder(QPainter* painter, const QRect& targetRect,
                              const TimelineClip& clip, const QString& message);
    void drawAudioPlaceholder(QPainter* painter, const QRect& safeRect,
                              const QList<TimelineClip>& activeAudioClips);
    void drawAudioBadge(QPainter* painter, const QRect& targetRect,
                        const QList<TimelineClip>& activeAudioClips);
    QRect fitRect(const QSize& source, const QRect& bounds) const;
    void drawPreviewChrome(QPainter* painter, const QRect& safeRect, int activeClipCount) const;
    QString clipIdAtPosition(const QPointF& position) const;
    TimelineClip::TransformKeyframe evaluateTransformForSelectedClip() const;
    void updatePreviewCursor(const QPointF& position);

    std::unique_ptr<AsyncDecoder> m_decoder;
    std::unique_ptr<TimelineCache> m_cache;
    std::unique_ptr<PlaybackFramePipeline> m_playbackPipeline;
    std::unique_ptr<QOpenGLShaderProgram> m_shaderProgram;
    QOpenGLBuffer m_quadBuffer;

    bool m_playing = false;
    bool m_audioMuted = false;
    qreal m_audioVolume = 0.8;
    bool m_bypassGrading = false;
    int64_t m_currentFrame = 0;
    int64_t m_currentSample = 0;
    qreal m_currentFramePosition = 0.0;
    int m_clipCount = 0;
    QVector<TimelineClip> m_clips;
    QVector<RenderSyncMarker> m_renderSyncMarkers;
    QSet<QString> m_registeredClips;
    QTimer m_repaintTimer;
    qint64 m_lastFrameRequestMs = 0;
    qint64 m_lastFrameReadyMs = 0;
    qint64 m_lastPaintMs = 0;
    qint64 m_lastRepaintScheduleMs = 0;
    QString m_selectedClipId;
    QSize m_outputSize = QSize(1080, 1920);
    qreal m_previewZoom = 1.0;
    QPointF m_previewPanOffset;
    QHash<QString, PreviewOverlayInfo> m_overlayInfo;
    mutable QHash<QString, QVector<TranscriptSection>> m_transcriptSectionsCache;
    QHash<QString, TextureCacheEntry> m_textureCache;
    QVector<QString> m_paintOrder;
    int m_bulkUpdateDepth = 0;
    bool m_pendingFrameRequest = false;
    PreviewDragMode m_dragMode = PreviewDragMode::None;
    QPointF m_dragOriginPos;
    QRectF m_dragOriginBounds;
    TimelineClip::TransformKeyframe m_dragOriginTransform;
};
