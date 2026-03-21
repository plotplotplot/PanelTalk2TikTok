#include "frame_handle.h"
#include "async_decoder.h"
#include "timeline_cache.h"
#include "memory_budget.h"
#include "gpu_compositor.h"
#include "control_server.h"
#include "editor_shared.h"
#include "audio_engine.h"
#include "timeline_widget.h"

#include <QApplication>
#include <QColor>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDragEnterEvent>
#include <QDoubleSpinBox>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileIconProvider>
#include <QFile>
#include <QFileSystemModel>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QInputDialog>
#include <QListWidget>
#include <QMainWindow>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QOffscreenSurface>
#include <QPainter>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QRandomGenerator>
#include <QResizeEvent>
#include <QSaveFile>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QStyle>
#include <QTabWidget>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QStackedLayout>
#include <QStackedWidget>
#include <QPointer>
#include <QElapsedTimer>

#include <QtGui/private/qrhi_p.h>
#include <QtGui/private/qrhigles2_p.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <cmath>
#include <chrono>
#include <mutex>
#include <deque>
#include <thread>

extern "C" {
#include <alsa/asoundlib.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
#include <libavutil/samplefmt.h>
#include <libavutil/avutil.h>
#include <libswresample/swresample.h>
}

using namespace editor;

namespace {
qint64 nowMs() {
    return QDateTime::currentMSecsSinceEpoch();
}

qint64 playbackTraceMs() {
    static QElapsedTimer timer;
    static bool started = false;
    if (!started) {
        timer.start();
        started = true;
    }
    return timer.elapsed();
}

bool playbackTraceEnabled() {
    static const bool enabled = qEnvironmentVariableIntValue("EDITOR_DEBUG_PLAYBACK") == 1;
    return enabled;
}

void playbackTrace(const QString& stage, const QString& detail = QString()) {
    if (!playbackTraceEnabled()) {
        return;
    }
    static QHash<QString, qint64> lastLogByStage;
    const qint64 now = playbackTraceMs();
    if (stage.startsWith(QStringLiteral("PreviewWindow::requestFramesForCurrentPosition")) ||
        stage.startsWith(QStringLiteral("EditorWindow::advanceFrame")) ||
        stage.startsWith(QStringLiteral("PreviewWindow::setCurrentFrame")) ||
        stage.startsWith(QStringLiteral("PreviewWindow::visible-request"))) {
        const qint64 last = lastLogByStage.value(stage, std::numeric_limits<qint64>::min());
        if (now - last < 250) {
            return;
        }
        lastLogByStage.insert(stage, now);
    }
    qDebug().noquote() << QStringLiteral("[PLAYBACK %1 ms] %2%3")
                              .arg(now, 6)
                              .arg(stage)
                              .arg(detail.isEmpty() ? QString() : QStringLiteral(" | ") + detail);
}

}

// ============================================================================
// PreviewRenderer - GPU-accelerated preview rendering
// ============================================================================
class PreviewRenderer {
public:
    explicit PreviewRenderer() = default;
    ~PreviewRenderer() { release(); }
    
    bool initialize() {
        if (m_initialized) return true;
        
        QElapsedTimer initTimer;
        initTimer.start();

        if (qEnvironmentVariableIntValue("EDITOR_FORCE_NULL_RHI") == 1) {
            qDebug() << "[STARTUP] Forcing QRhi Null backend";
            QRhiInitParams nullParams;
            m_rhi.reset(QRhi::create(QRhi::Null, &nullParams, QRhi::Flags()));
            if (!m_rhi) {
                qWarning() << "Failed to initialize forced Null QRhi backend";
                return false;
            }
            m_backendName = QString::fromLatin1(m_rhi->backendName()) + QStringLiteral(" (forced)");

            qDebug() << "[STARTUP] Initializing GPUCompositor...";
            QElapsedTimer compositorTimer;
            compositorTimer.start();
            m_compositor = std::make_unique<GPUCompositor>(m_rhi.get());
            if (!m_compositor->initialize()) {
                qWarning() << "Failed to initialize GPU compositor";
            }
            qDebug() << "[STARTUP] GPUCompositor initialized in" << compositorTimer.elapsed() << "ms";

            m_initialized = true;
            qDebug() << "[STARTUP] PreviewRenderer::initialize() total:" << initTimer.elapsed() << "ms";
            return true;
        }
        
        // Create offscreen surface for RHI
        m_fallbackSurface = std::make_unique<QOffscreenSurface>();
        m_fallbackSurface->setFormat(QSurfaceFormat::defaultFormat());
        m_fallbackSurface->create();
        qDebug() << "[STARTUP] Offscreen surface created in" << initTimer.elapsed() << "ms";
        
        if (!m_fallbackSurface->isValid()) {
            qWarning() << "Failed to create fallback surface";
            return false;
        }
        
        // Try OpenGL ES 2 backend
        qDebug() << "[STARTUP] Creating QRhi OpenGL context...";
        QElapsedTimer rhiTimer;
        rhiTimer.start();
        QRhiGles2InitParams params;
        params.format = QSurfaceFormat::defaultFormat();
        params.fallbackSurface = m_fallbackSurface.get();
        
        m_rhi.reset(QRhi::create(QRhi::OpenGLES2, &params, QRhi::Flags()));
        qDebug() << "[STARTUP] QRhi::create(OpenGLES2) took" << rhiTimer.elapsed() << "ms";
        
        if (m_rhi) {
            m_backendName = QString::fromLatin1(m_rhi->backendName());
            qDebug() << "PreviewRenderer: Using backend:" << m_backendName;
        } else {
            // Fall back to Null backend
            QRhiInitParams nullParams;
            m_rhi.reset(QRhi::create(QRhi::Null, &nullParams, QRhi::Flags()));
            
            if (m_rhi) {
                m_backendName = QString::fromLatin1(m_rhi->backendName()) + QStringLiteral(" (fallback)");
            } else {
                qWarning() << "Failed to initialize any RHI backend";
                return false;
            }
        }
        
        // Create compositor
        qDebug() << "[STARTUP] Initializing GPUCompositor...";
        QElapsedTimer compositorTimer;
        compositorTimer.start();
        m_compositor = std::make_unique<GPUCompositor>(m_rhi.get());
        if (!m_compositor->initialize()) {
            qWarning() << "Failed to initialize GPU compositor";
            // Continue without compositor - we'll use CPU fallback
        }
        qDebug() << "[STARTUP] GPUCompositor initialized in" << compositorTimer.elapsed() << "ms";
        
        m_initialized = true;
        qDebug() << "[STARTUP] PreviewRenderer::initialize() total:" << initTimer.elapsed() << "ms";
        return true;
    }
    
    void release() {
        m_compositor.reset();
        m_rhi.reset();
        m_fallbackSurface.reset();
        m_initialized = false;
    }
    
    QRhi* rhi() const { return m_rhi.get(); }
    GPUCompositor* compositor() const { return m_compositor.get(); }
    QString backendName() const { return m_backendName; }
    bool isInitialized() const { return m_initialized; }
    
private:
    std::unique_ptr<QOffscreenSurface> m_fallbackSurface;
    std::unique_ptr<QRhi> m_rhi;
    std::unique_ptr<GPUCompositor> m_compositor;
    QString m_backendName = QStringLiteral("not initialized");
    bool m_initialized = false;
};

// ============================================================================
// PreviewWindow - Video preview widget with professional pipeline
// ============================================================================
class PreviewWindow final : public QWidget {
    Q_OBJECT
public:
    PreviewWindow(QWidget* parent = nullptr)
        : QWidget(parent)
        , m_renderer(std::make_unique<PreviewRenderer>())
    {
        setMinimumSize(640, 360);
        setAutoFillBackground(true);
        setMouseTracking(true);
        m_lastPaintMs = nowMs();
        m_repaintTimer.setSingleShot(true);
        m_repaintTimer.setInterval(16);
        connect(&m_repaintTimer, &QTimer::timeout, this, [this]() {
            update();
        });
    }
    
    ~PreviewWindow() override {
        if (m_cache) {
            m_cache->stopPrefetching();
        }
        m_renderer->release();
    }
    
    void setPlaybackState(bool playing) {
        playbackTrace(QStringLiteral("PreviewWindow::setPlaybackState"),
                      QStringLiteral("playing=%1 clips=%2 cache=%3")
                          .arg(playing)
                          .arg(m_clips.size())
                          .arg(m_cache != nullptr));
        m_playing = playing;
        if (playing && !m_clips.isEmpty()) {
            ensurePipeline();
        }
        if (m_cache) {
            m_cache->setPlaybackState(playing ?
                TimelineCache::PlaybackState::Playing :
                TimelineCache::PlaybackState::Stopped);
        }
    }
    
    void setCurrentFrame(int64_t frame) {
        playbackTrace(QStringLiteral("PreviewWindow::setCurrentFrame"),
                      QStringLiteral("frame=%1 visible=%2 cache=%3")
                          .arg(frame)
                          .arg(isVisible())
                          .arg(m_cache != nullptr));
        m_currentFrame = frame;
        if (m_cache) {
            m_cache->setPlayheadFrame(frame);
        }
        if (isVisible()) {
            requestFramesForCurrentPosition();
        }
        scheduleRepaint();
    }
    
    void setClipCount(int count) {
        m_clipCount = count;
        scheduleRepaint();
    }

    void setSelectedClipId(const QString& clipId) {
        if (m_selectedClipId == clipId) {
            return;
        }
        m_selectedClipId = clipId;
        scheduleRepaint();
    }
    
    void setTimelineClips(const QVector<TimelineClip>& clips) {
        playbackTrace(QStringLiteral("PreviewWindow::setTimelineClips"),
                      QStringLiteral("clips=%1 cache=%2").arg(clips.size()).arg(m_cache != nullptr));
        m_clips = clips;
        if (!m_cache) {
            m_registeredClips.clear();
            scheduleRepaint();
            return;
        }
        
        // Update cache with new clips
        QSet<QString> registeredIds;
        for (const auto& clip : clips) {
            if (!clipHasVisuals(clip)) {
                continue;
            }
            registeredIds.insert(clip.id);
            if (!m_registeredClips.contains(clip.id)) {
                m_cache->registerClip(clip.id, clip.filePath, 
                                     clip.startFrame, clip.durationFrames);
                m_registeredClips.insert(clip.id);
            }
        }
        
        // Unregister removed clips
        for (const QString& id : m_registeredClips) {
            if (!registeredIds.contains(id)) {
                m_cache->unregisterClip(id);
            }
        }
        m_registeredClips = registeredIds;
        
        requestFramesForCurrentPosition();
        scheduleRepaint();
    }
    
    QString backendName() const {
        return m_renderer->backendName();
    }

    void setAudioMuted(bool muted) {
        m_audioMuted = muted;
    }

    void setAudioVolume(qreal volume) {
        m_audioVolume = qBound<qreal>(0.0, volume, 1.0);
    }

    void setOutputSize(const QSize& size) {
        const QSize sanitized(qMax(16, size.width()), qMax(16, size.height()));
        if (m_outputSize == sanitized) {
            return;
        }
        m_outputSize = sanitized;
        scheduleRepaint();
    }

    void setBypassGrading(bool bypass) {
        if (m_bypassGrading == bypass) {
            return;
        }
        m_bypassGrading = bypass;
        scheduleRepaint();
    }

    bool bypassGrading() const {
        return m_bypassGrading;
    }

    bool audioMuted() const {
        return m_audioMuted;
    }

    int audioVolumePercent() const {
        return qRound(m_audioVolume * 100.0);
    }

    QString activeAudioClipLabel() const {
        for (const TimelineClip& clip : m_clips) {
            if (clip.hasAudio &&
                m_currentFrame >= clip.startFrame &&
                m_currentFrame < clip.startFrame + clip.durationFrames) {
                return clip.label;
            }
        }
        return QString();
    }

    bool preparePlaybackAdvance(int64_t targetFrame) {
        if (m_clips.isEmpty()) {
            return true;
        }

        ensurePipeline();
        if (!m_cache) {
            return false;
        }

        static constexpr int kMaxVisibleBacklog = 2;
        bool hasActiveClip = false;

        for (const TimelineClip& clip : m_clips) {
            if (!clipHasVisuals(clip)) {
                continue;
            }
            if (targetFrame < clip.startFrame || targetFrame >= clip.startFrame + clip.durationFrames) {
                continue;
            }

            hasActiveClip = true;
            const int64_t localFrame = clip.sourceInFrame + (targetFrame - clip.startFrame);
            if (m_cache->isFrameCached(clip.id, localFrame)) {
                continue;
            }

            if (m_cache->isVisibleRequestPending(clip.id, localFrame)) {
                continue;
            }

            if (m_cache->pendingVisibleRequestCount() >= kMaxVisibleBacklog) {
                continue;
            }

            m_cache->requestFrame(clip.id, localFrame,
                [this](FrameHandle frame) {
                    Q_UNUSED(frame)
                    QMetaObject::invokeMethod(this, [this]() { scheduleRepaint(); }, Qt::QueuedConnection);
                });
        }

        return true;
    }

    QJsonObject profilingSnapshot() const {
        const qint64 now = nowMs();
        QJsonObject snapshot{
            {QStringLiteral("backend"), m_renderer->backendName()},
            {QStringLiteral("playing"), m_playing},
            {QStringLiteral("current_frame"), static_cast<qint64>(m_currentFrame)},
            {QStringLiteral("clip_count"), m_clips.size()},
            {QStringLiteral("pipeline_initialized"), m_cache != nullptr},
            {QStringLiteral("last_frame_request_ms"), m_lastFrameRequestMs},
            {QStringLiteral("last_frame_ready_ms"), m_lastFrameReadyMs},
            {QStringLiteral("last_paint_ms"), m_lastPaintMs},
            {QStringLiteral("last_repaint_schedule_ms"), m_lastRepaintScheduleMs},
            {QStringLiteral("last_frame_request_age_ms"), m_lastFrameRequestMs > 0 ? now - m_lastFrameRequestMs : -1},
            {QStringLiteral("last_frame_ready_age_ms"), m_lastFrameReadyMs > 0 ? now - m_lastFrameReadyMs : -1},
            {QStringLiteral("last_paint_age_ms"), m_lastPaintMs > 0 ? now - m_lastPaintMs : -1},
            {QStringLiteral("repaint_timer_active"), m_repaintTimer.isActive()},
            {QStringLiteral("bypass_grading"), m_bypassGrading}
        };

        if (m_decoder) {
            snapshot[QStringLiteral("decoder")] = QJsonObject{
                {QStringLiteral("worker_count"), m_decoder->workerCount()},
                {QStringLiteral("pending_requests"), m_decoder->pendingRequestCount()}
            };

            if (MemoryBudget* budget = m_decoder->memoryBudget()) {
                snapshot[QStringLiteral("memory_budget")] = QJsonObject{
                    {QStringLiteral("cpu_usage"), static_cast<qint64>(budget->currentCpuUsage())},
                    {QStringLiteral("gpu_usage"), static_cast<qint64>(budget->currentGpuUsage())},
                    {QStringLiteral("cpu_pressure"), budget->cpuPressure()},
                    {QStringLiteral("gpu_pressure"), budget->gpuPressure()},
                    {QStringLiteral("cpu_max"), static_cast<qint64>(budget->maxCpuMemory())},
                    {QStringLiteral("gpu_max"), static_cast<qint64>(budget->maxGpuMemory())}
                };
            }
        }

        if (m_cache) {
            snapshot[QStringLiteral("cache")] = QJsonObject{
                {QStringLiteral("hit_rate"), m_cache->cacheHitRate()},
                {QStringLiteral("total_memory_usage"), static_cast<qint64>(m_cache->totalMemoryUsage())},
                {QStringLiteral("total_cached_frames"), m_cache->totalCachedFrames()},
                {QStringLiteral("pending_visible_requests"), m_cache->pendingVisibleRequestCount()}
            };
        }

        return snapshot;
    }

protected:
    void showEvent(QShowEvent* event) override {
        QWidget::showEvent(event);
        if (!m_renderer->isInitialized()) {
            m_renderer->initialize();
        }
        QTimer::singleShot(0, this, [this]() {
            requestFramesForCurrentPosition();
        });
    }
    
    void paintEvent(QPaintEvent* event) override {
        Q_UNUSED(event)
        m_lastPaintMs = nowMs();
        
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        
        // Draw background
        drawBackground(&painter);
        
        // Get active clips
        QList<TimelineClip> activeClips = getActiveClips();
        
        // Draw composited preview
        const QRect safeRect = rect().adjusted(24, 24, -24, -24);
        drawCompositedPreview(&painter, safeRect, activeClips);
        
        // Draw chrome
        drawPreviewChrome(&painter, safeRect, activeClips.size());
    }

    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton) {
            QWidget::mousePressEvent(event);
            return;
        }

        const PreviewOverlayInfo selectedInfo = m_overlayInfo.value(m_selectedClipId);
        if (!m_selectedClipId.isEmpty()) {
            if (selectedInfo.cornerHandle.contains(event->position())) {
                m_dragMode = PreviewDragMode::ResizeBoth;
            } else if (selectedInfo.rightHandle.contains(event->position())) {
                m_dragMode = PreviewDragMode::ResizeX;
            } else if (selectedInfo.bottomHandle.contains(event->position())) {
                m_dragMode = PreviewDragMode::ResizeY;
            } else if (selectedInfo.bounds.contains(event->position())) {
                m_dragMode = PreviewDragMode::Move;
            }
            if (m_dragMode != PreviewDragMode::None) {
                m_dragOriginPos = event->position();
                m_dragOriginTransform = evaluateTransformForSelectedClip();
                m_dragOriginBounds = selectedInfo.bounds;
                event->accept();
                return;
            }
        }

        const QString hitClipId = clipIdAtPosition(event->position());
        if (!hitClipId.isEmpty()) {
            m_selectedClipId = hitClipId;
            if (selectionRequested) {
                selectionRequested(hitClipId);
            }
            updatePreviewCursor(event->position());
            update();
            event->accept();
            return;
        }

        QWidget::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        if (m_dragMode != PreviewDragMode::None && (event->buttons() & Qt::LeftButton) &&
            !m_selectedClipId.isEmpty() && m_dragOriginBounds.width() > 1.0 && m_dragOriginBounds.height() > 1.0) {
            if (m_dragMode == PreviewDragMode::Move) {
                if (moveRequested) {
                    moveRequested(m_selectedClipId,
                                  m_dragOriginTransform.translationX + ((event->position().x() - m_dragOriginPos.x()) / m_previewZoom),
                                  m_dragOriginTransform.translationY + ((event->position().y() - m_dragOriginPos.y()) / m_previewZoom),
                                  false);
                }
                event->accept();
                return;
            }
            qreal scaleX = m_dragOriginTransform.scaleX;
            qreal scaleY = m_dragOriginTransform.scaleY;
            if (m_dragMode == PreviewDragMode::ResizeX || m_dragMode == PreviewDragMode::ResizeBoth) {
                scaleX = sanitizeScaleValue(
                    m_dragOriginTransform.scaleX *
                    ((m_dragOriginBounds.width() + (event->position().x() - m_dragOriginPos.x())) /
                     m_dragOriginBounds.width()));
            }
            if (m_dragMode == PreviewDragMode::ResizeY || m_dragMode == PreviewDragMode::ResizeBoth) {
                scaleY = sanitizeScaleValue(
                    m_dragOriginTransform.scaleY *
                    ((m_dragOriginBounds.height() + (event->position().y() - m_dragOriginPos.y())) /
                     m_dragOriginBounds.height()));
            }
            if (m_dragMode == PreviewDragMode::ResizeBoth) {
                const qreal factorX =
                    (m_dragOriginBounds.width() + (event->position().x() - m_dragOriginPos.x())) /
                    m_dragOriginBounds.width();
                const qreal factorY =
                    (m_dragOriginBounds.height() + (event->position().y() - m_dragOriginPos.y())) /
                    m_dragOriginBounds.height();
                const qreal uniformFactor = std::abs(factorX) >= std::abs(factorY) ? factorX : factorY;
                scaleX = sanitizeScaleValue(m_dragOriginTransform.scaleX * uniformFactor);
                scaleY = sanitizeScaleValue(m_dragOriginTransform.scaleY * uniformFactor);
            }
            if (resizeRequested) {
                resizeRequested(m_selectedClipId, scaleX, scaleY, false);
            }
            event->accept();
            return;
        }

        updatePreviewCursor(event->position());
        QWidget::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton && m_dragMode != PreviewDragMode::None) {
            const TimelineClip::TransformKeyframe transform = evaluateTransformForSelectedClip();
            if (m_dragMode == PreviewDragMode::Move) {
                if (moveRequested) {
                    moveRequested(m_selectedClipId, transform.translationX, transform.translationY, true);
                }
            } else if (resizeRequested) {
                resizeRequested(m_selectedClipId, transform.scaleX, transform.scaleY, true);
            }
            m_dragMode = PreviewDragMode::None;
            m_dragOriginBounds = QRectF();
            updatePreviewCursor(event->position());
            event->accept();
            return;
        }
        QWidget::mouseReleaseEvent(event);
    }

    void wheelEvent(QWheelEvent* event) override {
        if (event->angleDelta().y() == 0) {
            QWidget::wheelEvent(event);
            return;
        }
        const QRect baseRect = previewCanvasBaseRect();
        const QRect oldRect = scaledCanvasRect(baseRect);
        const qreal factor = event->angleDelta().y() > 0 ? 1.1 : (1.0 / 1.1);
        const qreal nextZoom = qBound<qreal>(0.25, m_previewZoom * factor, 4.0);
        const QPointF anchor =
            QPointF((event->position().x() - oldRect.left()) / qMax(1.0, static_cast<qreal>(oldRect.width())),
                    (event->position().y() - oldRect.top()) / qMax(1.0, static_cast<qreal>(oldRect.height())));
        m_previewZoom = nextZoom;
        const QSizeF newSize(baseRect.width() * m_previewZoom, baseRect.height() * m_previewZoom);
        const QPointF centeredTopLeft(baseRect.center().x() - (newSize.width() / 2.0),
                                      baseRect.center().y() - (newSize.height() / 2.0));
        const QPointF anchoredTopLeft(event->position().x() - (anchor.x() * newSize.width()),
                                      event->position().y() - (anchor.y() * newSize.height()));
        m_previewPanOffset = anchoredTopLeft - centeredTopLeft;
        scheduleRepaint();
        event->accept();
    }

private:
    struct PreviewOverlayInfo {
        QRectF bounds;
        QRectF rightHandle;
        QRectF bottomHandle;
        QRectF cornerHandle;
    };

    enum class PreviewDragMode {
        None,
        Move,
        ResizeX,
        ResizeY,
        ResizeBoth,
    };

    QRect previewCanvasBaseRect() const {
        const QRect available = rect().adjusted(36, 36, -36, -36);
        if (!available.isValid()) {
            return available;
        }
        QSize fitted = (m_outputSize.isValid() ? m_outputSize : QSize(1080, 1920));
        fitted.scale(available.size(), Qt::KeepAspectRatio);
        const QPoint topLeft(available.center().x() - fitted.width() / 2,
                             available.center().y() - fitted.height() / 2);
        return QRect(topLeft, fitted);
    }

    QRect scaledCanvasRect(const QRect& baseRect) const {
        const QSize scaledSize(qMax(1, qRound(baseRect.width() * m_previewZoom)),
                               qMax(1, qRound(baseRect.height() * m_previewZoom)));
        const QPoint center = baseRect.center();
        return QRect(qRound(center.x() - scaledSize.width() / 2.0 + m_previewPanOffset.x()),
                     qRound(center.y() - scaledSize.height() / 2.0 + m_previewPanOffset.y()),
                     scaledSize.width(),
                     scaledSize.height());
    }

    void ensurePipeline() {
        if (m_cache) {
            return;
        }

        playbackTrace(QStringLiteral("PreviewWindow::ensurePipeline.begin"),
                      QStringLiteral("clips=%1 frame=%2").arg(m_clips.size()).arg(m_currentFrame));

        m_decoder = std::make_unique<AsyncDecoder>(this);
        m_decoder->initialize();

        m_cache = std::make_unique<TimelineCache>(m_decoder.get(),
                                                  m_decoder->memoryBudget(),
                                                  this);
        m_cache->setMaxMemory(512 * 1024 * 1024);
        m_cache->setPlaybackState(m_playing ?
            TimelineCache::PlaybackState::Playing :
            TimelineCache::PlaybackState::Stopped);
        m_cache->setPlayheadFrame(m_currentFrame);
        m_registeredClips.clear();
        for (const TimelineClip& clip : m_clips) {
            if (!clipHasVisuals(clip)) {
                continue;
            }
                m_cache->registerClip(clip.id, clip.filePath, clip.startFrame, clip.durationFrames);
            m_registeredClips.insert(clip.id);
        }
        m_cache->startPrefetching();
        playbackTrace(QStringLiteral("PreviewWindow::ensurePipeline.end"),
                      QStringLiteral("workers=%1").arg(m_decoder ? m_decoder->workerCount() : 0));
    }

    void drawBackground(QPainter* painter) {
        const float phase = static_cast<float>(m_currentFrame % 180) / 179.0f;
        const float clipFactor = qBound(0.0f, static_cast<float>(m_clipCount) / 8.0f, 1.0f);
        const float motion = m_playing ? phase : 0.25f;
        
        QLinearGradient gradient(rect().topLeft(), rect().bottomRight());
        gradient.setColorAt(0.0, QColor::fromRgbF(0.08f + 0.22f * motion,
                                                  0.10f + 0.18f * clipFactor,
                                                  0.13f + 0.35f * (1.0f - motion),
                                                  1.0f));
        gradient.setColorAt(1.0, QColor::fromRgbF(0.14f + 0.10f * clipFactor,
                                                  0.07f + 0.08f * motion,
                                                  0.09f + 0.25f * clipFactor,
                                                  1.0f));
        painter->fillRect(rect(), gradient);
    }
    
    QList<TimelineClip> getActiveClips() const {
        QList<TimelineClip> active;
        for (const TimelineClip& clip : m_clips) {
            if (m_currentFrame >= clip.startFrame && 
                m_currentFrame < clip.startFrame + clip.durationFrames) {
                active.push_back(clip);
            }
        }
        std::sort(active.begin(), active.end(), 
            [](const TimelineClip& a, const TimelineClip& b) {
                if (a.trackIndex == b.trackIndex) {
                    return a.startFrame < b.startFrame;
                }
                return a.trackIndex < b.trackIndex;
            });
        return active;
    }
    
    void requestFramesForCurrentPosition() {
        static constexpr int kMaxVisibleBacklog = 2;
        QVector<const TimelineClip*> activeClips;
        activeClips.reserve(m_clips.size());
        for (const TimelineClip& clip : m_clips) {
            if (!clipHasVisuals(clip)) {
                continue;
            }
            if (m_currentFrame >= clip.startFrame &&
                m_currentFrame < clip.startFrame + clip.durationFrames) {
                activeClips.push_back(&clip);
            }
        }

        if (activeClips.isEmpty()) {
            playbackTrace(QStringLiteral("PreviewWindow::requestFramesForCurrentPosition"),
                          QStringLiteral("no-active-clips frame=%1").arg(m_currentFrame));
            return;
        }

        ensurePipeline();
        if (!m_cache) {
            playbackTrace(QStringLiteral("PreviewWindow::requestFramesForCurrentPosition"),
                          QStringLiteral("cache-unavailable frame=%1").arg(m_currentFrame));
            return;
        }

        playbackTrace(QStringLiteral("PreviewWindow::requestFramesForCurrentPosition"),
                      QStringLiteral("active=%1 frame=%2 pending=%3")
                          .arg(activeClips.size())
                          .arg(m_currentFrame)
                          .arg(m_decoder ? m_decoder->pendingRequestCount() : 0));

        if (m_cache->pendingVisibleRequestCount() >= kMaxVisibleBacklog) {
            playbackTrace(QStringLiteral("PreviewWindow::requestFramesForCurrentPosition.skip"),
                          QStringLiteral("reason=visible-backlog count=%1")
                              .arg(m_cache->pendingVisibleRequestCount()));
            return;
        }

        for (const TimelineClip* clip : activeClips) {
            const int64_t localFrame = clip->sourceInFrame + (m_currentFrame - clip->startFrame);
            const bool cached = m_cache->isFrameCached(clip->id, localFrame);
            playbackTrace(QStringLiteral("PreviewWindow::visible-request"),
                          QStringLiteral("clip=%1 localFrame=%2 cached=%3")
                              .arg(clip->id)
                              .arg(localFrame)
                              .arg(cached));

            // Request if not cached
            if (!cached && !m_cache->isVisibleRequestPending(clip->id, localFrame)) {
                m_lastFrameRequestMs = nowMs();
                const qint64 requestedAt = playbackTraceMs();
                m_cache->requestFrame(clip->id, localFrame,
                    [this, clipId = clip->id, localFrame, requestedAt](FrameHandle frame) {
                        if (!frame.isNull()) {
                            m_lastFrameReadyMs = nowMs();
                        }
                        playbackTrace(QStringLiteral("PreviewWindow::visible-request.callback"),
                                      QStringLiteral("clip=%1 localFrame=%2 null=%3 waitMs=%4")
                                          .arg(clipId)
                                          .arg(localFrame)
                                          .arg(frame.isNull())
                                          .arg(playbackTraceMs() - requestedAt));
                        // Frame loaded - trigger repaint
                        QMetaObject::invokeMethod(this, [this]() {
                            scheduleRepaint();
                        }, Qt::QueuedConnection);
                    });
            }
        }
    }

    void scheduleRepaint() {
        m_lastRepaintScheduleMs = nowMs();
        if (!m_repaintTimer.isActive()) {
            m_repaintTimer.start();
        }
    }
    
    void drawCompositedPreview(QPainter* painter, const QRect& safeRect, 
                               const QList<TimelineClip>& activeClips) {
        painter->save();
        m_overlayInfo.clear();
        m_paintOrder.clear();
        
        // Draw background panel
        painter->setPen(QPen(QColor(255, 255, 255, 40), 1.5));
        painter->setBrush(QColor(255, 255, 255, 18));
        painter->drawRoundedRect(safeRect, 18, 18);
        
        if (activeClips.isEmpty()) {
            QList<TimelineClip> activeAudioClips;
            for (const TimelineClip& clip : m_clips) {
                if (clipIsAudioOnly(clip) &&
                    m_currentFrame >= clip.startFrame &&
                    m_currentFrame < clip.startFrame + clip.durationFrames) {
                    activeAudioClips.push_back(clip);
                }
            }
            if (!activeAudioClips.isEmpty()) {
                drawAudioPlaceholder(painter, safeRect, activeAudioClips);
            } else {
                drawEmptyState(painter, safeRect);
            }
            painter->restore();
            return;
        }

        const QRect compositeRect = scaledCanvasRect(previewCanvasBaseRect());
        painter->fillRect(compositeRect, Qt::black);
        bool drewAnyFrame = false;
        bool waitingForFrame = false;

        for (const TimelineClip& clip : activeClips) {
            const int64_t localFrame = clip.sourceInFrame + (m_currentFrame - clip.startFrame);
            const FrameHandle frame = m_cache ? m_cache->getBestCachedFrame(clip.id, localFrame) : FrameHandle();
            if (frame.isNull()) {
                waitingForFrame = true;
                continue;
            }
            drawFrameLayer(painter, compositeRect, clip, frame);
            drewAnyFrame = true;
        }

        if (!drewAnyFrame) {
            const TimelineClip& primaryClip = activeClips.constFirst();
            drawFramePlaceholder(painter, compositeRect, primaryClip,
                                 waitingForFrame
                                     ? QStringLiteral("Frame loading...")
                                     : QStringLiteral("No composited frame available"));
        } else if (waitingForFrame) {
            painter->save();
            painter->setPen(Qt::NoPen);
            painter->setBrush(QColor(9, 12, 16, 170));
            const QRect badgeRect(compositeRect.left() + 16, compositeRect.top() + 16, 150, 28);
            painter->drawRoundedRect(badgeRect, 10, 10);
            painter->setPen(QColor(QStringLiteral("#edf3f8")));
            painter->drawText(badgeRect.adjusted(12, 0, -12, 0),
                              Qt::AlignLeft | Qt::AlignVCenter,
                              QStringLiteral("Overlay loading..."));
            painter->restore();
        }

        QList<TimelineClip> activeAudioClips;
        for (const TimelineClip& clip : m_clips) {
            if (clipIsAudioOnly(clip) &&
                m_currentFrame >= clip.startFrame &&
                m_currentFrame < clip.startFrame + clip.durationFrames) {
                activeAudioClips.push_back(clip);
            }
        }
        if (!activeAudioClips.isEmpty()) {
            drawAudioBadge(painter, compositeRect, activeAudioClips);
        }
        
        painter->restore();
    }
    
    void drawEmptyState(QPainter* painter, const QRect& safeRect) {
        painter->setPen(QColor(QStringLiteral("#f5f8fb")));
        QFont titleFont = painter->font();
        titleFont.setPointSize(titleFont.pointSize() + 4);
        titleFont.setBold(true);
        painter->setFont(titleFont);
        painter->drawText(safeRect.adjusted(20, 18, -20, -20),
                          Qt::AlignTop | Qt::AlignLeft,
                          QStringLiteral("Preview"));
        
        QFont bodyFont = painter->font();
        bodyFont.setBold(false);
        bodyFont.setPointSize(qMax(10, bodyFont.pointSize() - 2));
        painter->setFont(bodyFont);
        painter->setPen(QColor(QStringLiteral("#d2dbe4")));
        painter->drawText(safeRect.adjusted(20, 58, -20, -20),
                          Qt::AlignTop | Qt::AlignLeft,
                          QStringLiteral("No active clips at this frame.\nFrame %1\nQRhi backend: %2\nGrading: %3")
                              .arg(m_currentFrame)
                              .arg(m_renderer->backendName())
                              .arg(m_bypassGrading ? QStringLiteral("bypassed") : QStringLiteral("on")));
    }
    
    void drawFrameLayer(QPainter* painter, const QRect& targetRect,
                        const TimelineClip& clip, const FrameHandle& frame) {
        painter->save();
        painter->setClipRect(targetRect);

        if (!frame.isNull() && frame.hasCpuImage()) {
            const QImage img = m_bypassGrading ? frame.cpuImage() : applyClipGrade(frame.cpuImage(), clip);
            const QRect fitted = fitRect(img.size(), targetRect);
            const TimelineClip::TransformKeyframe transform = evaluateClipTransformAtFrame(clip, m_currentFrame);
            painter->translate(fitted.center().x() + (transform.translationX * m_previewZoom),
                               fitted.center().y() + (transform.translationY * m_previewZoom));
            painter->rotate(transform.rotation);
            painter->scale(transform.scaleX, transform.scaleY);
            const QRectF drawRect(-fitted.width() / 2.0,
                                  -fitted.height() / 2.0,
                                  fitted.width(),
                                  fitted.height());
            painter->drawImage(drawRect, img);

            QTransform overlayTransform;
            overlayTransform.translate(fitted.center().x() + (transform.translationX * m_previewZoom),
                                       fitted.center().y() + (transform.translationY * m_previewZoom));
            overlayTransform.rotate(transform.rotation);
            overlayTransform.scale(transform.scaleX, transform.scaleY);
            const QRectF bounds = overlayTransform.mapRect(drawRect);
            PreviewOverlayInfo info;
            info.bounds = bounds;
            constexpr qreal kHandleSize = 12.0;
            info.rightHandle = QRectF(bounds.right() - kHandleSize,
                                      bounds.center().y() - kHandleSize,
                                      kHandleSize,
                                      kHandleSize * 2.0);
            info.bottomHandle = QRectF(bounds.center().x() - kHandleSize,
                                       bounds.bottom() - kHandleSize,
                                       kHandleSize * 2.0,
                                       kHandleSize);
            info.cornerHandle = QRectF(bounds.right() - kHandleSize * 1.5,
                                       bounds.bottom() - kHandleSize * 1.5,
                                       kHandleSize * 1.5,
                                       kHandleSize * 1.5);
            m_overlayInfo.insert(clip.id, info);
            m_paintOrder.push_back(clip.id);

            painter->resetTransform();
            painter->setClipping(false);
            if (clip.id == m_selectedClipId) {
                painter->setPen(QPen(QColor(QStringLiteral("#fff4c2")), 2.0));
                painter->setBrush(Qt::NoBrush);
                painter->drawRect(bounds);
                painter->setBrush(QColor(QStringLiteral("#fff4c2")));
                painter->drawRect(info.rightHandle);
                painter->drawRect(info.bottomHandle);
                painter->drawRect(info.cornerHandle);
            }
        }

        painter->setPen(QPen(QColor(255, 255, 255, 36), 1.0));
        painter->setBrush(Qt::NoBrush);
        painter->drawRoundedRect(targetRect.adjusted(0, 0, -1, -1), 12, 12);

        const QRect badgeRect(targetRect.left() + 16,
                              targetRect.top() + 16 + clip.trackIndex * 32,
                              220,
                              26);
        painter->setPen(Qt::NoPen);
        painter->setBrush(QColor(7, 11, 17, 176));
        painter->drawRoundedRect(badgeRect, 10, 10);
        painter->setPen(QColor(QStringLiteral("#eef4fa")));
        painter->drawText(badgeRect.adjusted(12, 0, -12, 0),
                          Qt::AlignLeft | Qt::AlignVCenter,
                          QStringLiteral("Track %1  %2")
                              .arg(clip.trackIndex + 1)
                              .arg(clip.label));

        painter->restore();
    }

    void drawFramePlaceholder(QPainter* painter, const QRect& targetRect,
                              const TimelineClip& clip, const QString& message) {
        painter->save();
        painter->fillRect(targetRect, clip.color.darker(160));
        painter->setPen(QColor(255, 255, 255, 48));
        painter->drawRect(targetRect.adjusted(0, 0, -1, -1));
        painter->setPen(QColor(QStringLiteral("#f2f6fa")));
        painter->drawText(targetRect.adjusted(16, 16, -16, -16),
                          Qt::AlignCenter | Qt::TextWordWrap,
                          QStringLiteral("Track %1\n%2\n%3")
                              .arg(clip.trackIndex + 1)
                              .arg(clip.label)
                              .arg(message));
        painter->restore();
    }

    void drawAudioPlaceholder(QPainter* painter, const QRect& safeRect,
                              const QList<TimelineClip>& activeAudioClips) {
        painter->save();
        painter->setPen(QPen(QColor(255, 255, 255, 40), 1.5));
        painter->setBrush(QColor(255, 255, 255, 18));
        painter->drawRoundedRect(safeRect, 18, 18);

        const QRect panel = safeRect.adjusted(12, 12, -12, -12);
        QLinearGradient gradient(panel.topLeft(), panel.bottomRight());
        gradient.setColorAt(0.0, QColor(QStringLiteral("#13222d")));
        gradient.setColorAt(1.0, QColor(QStringLiteral("#0a1218")));
        painter->fillRect(panel, gradient);

        QFont titleFont = painter->font();
        titleFont.setBold(true);
        titleFont.setPointSize(titleFont.pointSize() + 5);
        painter->setFont(titleFont);
        painter->setPen(QColor(QStringLiteral("#eef5fb")));
        painter->drawText(panel.adjusted(20, 22, -20, -20),
                          Qt::AlignTop | Qt::AlignLeft,
                          QStringLiteral("Audio Monitor"));

        QFont bodyFont = painter->font();
        bodyFont.setBold(false);
        bodyFont.setPointSize(qMax(10, bodyFont.pointSize() - 2));
        painter->setFont(bodyFont);
        painter->setPen(QColor(QStringLiteral("#c8d5e0")));
        painter->drawText(panel.adjusted(20, 60, -20, -20),
                          Qt::AlignTop | Qt::AlignLeft,
                          QStringLiteral("Active audio clip: %1\nTransport audio: %2")
                              .arg(activeAudioClips.constFirst().label)
                              .arg(m_playing ? QStringLiteral("live") : QStringLiteral("paused")));

        const QRect waveRect = panel.adjusted(24, 120, -24, -36);
        painter->setPen(Qt::NoPen);
        for (int x = waveRect.left(); x < waveRect.right(); x += 10) {
            const int idx = (x - waveRect.left()) / 10;
            const qreal phase = static_cast<qreal>((idx * 13 + m_currentFrame) % 100) / 99.0;
            const int barHeight = qMax(12, qRound((0.2 + std::sin(phase * 6.28318) * 0.4 + 0.4) * waveRect.height()));
            const QRect barRect(x, waveRect.center().y() - barHeight / 2, 6, barHeight);
            painter->setBrush(QColor(QStringLiteral("#58c4dd")));
            painter->drawRoundedRect(barRect, 3, 3);
        }
        painter->restore();
    }

    void drawAudioBadge(QPainter* painter, const QRect& targetRect,
                        const QList<TimelineClip>& activeAudioClips) {
        painter->save();
        const QRect badgeRect(targetRect.left() + 16, targetRect.bottom() - 46, 240, 30);
        painter->setPen(Qt::NoPen);
        painter->setBrush(QColor(7, 11, 17, 176));
        painter->drawRoundedRect(badgeRect, 10, 10);
        painter->setPen(QColor(QStringLiteral("#dff8ff")));
        painter->drawText(badgeRect.adjusted(12, 0, -12, 0),
                          Qt::AlignLeft | Qt::AlignVCenter,
                          QStringLiteral("Audio  %1").arg(activeAudioClips.constFirst().label));
        painter->restore();
    }
    
    QRect fitRect(const QSize& source, const QRect& bounds) const {
        if (source.isEmpty() || bounds.isEmpty()) {
            return bounds;
        }
        
        QSize scaled = source;
        scaled.scale(bounds.size(), Qt::KeepAspectRatio);
        const QPoint topLeft(bounds.center().x() - scaled.width() / 2,
                             bounds.center().y() - scaled.height() / 2);
        return QRect(topLeft, scaled);
    }
    
    void drawPreviewChrome(QPainter* painter, const QRect& safeRect, int activeClipCount) const {
        painter->save();
        painter->setPen(QColor(QStringLiteral("#f5f8fb")));
        QFont titleFont = painter->font();
        titleFont.setPointSize(titleFont.pointSize() + 1);
        titleFont.setBold(true);
        painter->setFont(titleFont);
        painter->drawText(safeRect.adjusted(20, 16, -20, -20),
                          Qt::AlignTop | Qt::AlignLeft,
                          QStringLiteral("Preview  |  Active clips %1").arg(activeClipCount));
        
        QFont bodyFont = painter->font();
        bodyFont.setBold(false);
        bodyFont.setPointSize(qMax(9, bodyFont.pointSize() - 2));
        painter->setFont(bodyFont);
        painter->setPen(QColor(QStringLiteral("#d2dbe4")));
        painter->drawText(safeRect.adjusted(20, 40, -20, -20),
                          Qt::AlignTop | Qt::AlignLeft,
                          QStringLiteral("Frame %1  |  QRhi backend: %2  |  Grading: %3")
                              .arg(m_currentFrame)
                              .arg(m_renderer->backendName())
                              .arg(m_bypassGrading ? QStringLiteral("bypassed") : QStringLiteral("on")));
        painter->restore();
    }

    QString clipIdAtPosition(const QPointF& position) const {
        for (int i = m_paintOrder.size() - 1; i >= 0; --i) {
            const QString& clipId = m_paintOrder[i];
            if (m_overlayInfo.value(clipId).bounds.contains(position)) {
                return clipId;
            }
        }
        return QString();
    }

    TimelineClip::TransformKeyframe evaluateTransformForSelectedClip() const {
        for (const TimelineClip& clip : m_clips) {
            if (clip.id == m_selectedClipId) {
                return evaluateClipTransformAtFrame(clip, m_currentFrame);
            }
        }
        return TimelineClip::TransformKeyframe();
    }

    void updatePreviewCursor(const QPointF& position) {
        const PreviewOverlayInfo info = m_overlayInfo.value(m_selectedClipId);
        if (!m_selectedClipId.isEmpty()) {
            if (info.cornerHandle.contains(position)) {
                setCursor(Qt::SizeFDiagCursor);
                return;
            }
            if (info.rightHandle.contains(position)) {
                setCursor(Qt::SizeHorCursor);
                return;
            }
            if (info.bottomHandle.contains(position)) {
                setCursor(Qt::SizeVerCursor);
                return;
            }
            if (info.bounds.contains(position)) {
                setCursor(m_dragMode == PreviewDragMode::Move ? Qt::ClosedHandCursor : Qt::OpenHandCursor);
                return;
            }
        }
        unsetCursor();
    }

    std::unique_ptr<PreviewRenderer> m_renderer;
    std::unique_ptr<AsyncDecoder> m_decoder;
    std::unique_ptr<TimelineCache> m_cache;
    
    bool m_playing = false;
    bool m_audioMuted = false;
    qreal m_audioVolume = 0.8;
    bool m_bypassGrading = false;
    int64_t m_currentFrame = 0;
    int m_clipCount = 0;
    QVector<TimelineClip> m_clips;
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
    QVector<QString> m_paintOrder;
    PreviewDragMode m_dragMode = PreviewDragMode::None;
    QPointF m_dragOriginPos;
    QRectF m_dragOriginBounds;
    TimelineClip::TransformKeyframe m_dragOriginTransform;

public:
    std::function<void(const QString&)> selectionRequested;
    std::function<void(const QString&, qreal, qreal, bool)> resizeRequested;
    std::function<void(const QString&, qreal, qreal, bool)> moveRequested;
};


// ============================================================================
// EditorWindow - Main application window
// ============================================================================
class EditorWindow final : public QMainWindow {
    Q_OBJECT
public:
    explicit EditorWindow(quint16 controlPort) {
        QElapsedTimer ctorTimer;
        ctorTimer.start();
        
        setWindowTitle(QStringLiteral("QRhi Editor - Professional"));
        resize(1500, 900);

        auto* central = new QWidget(this);
        auto* rootLayout = new QHBoxLayout(central);
        rootLayout->setContentsMargins(0, 0, 0, 0);
        rootLayout->setSpacing(0);

        auto* splitter = new QSplitter(Qt::Horizontal, central);
        splitter->setObjectName(QStringLiteral("layout.main_splitter"));
        splitter->setChildrenCollapsible(false);
        rootLayout->addWidget(splitter);

        qDebug() << "[STARTUP] Building explorer pane...";
        splitter->addWidget(buildExplorerPane());
        qDebug() << "[STARTUP] Explorer pane built in" << ctorTimer.elapsed() << "ms";
        
        qDebug() << "[STARTUP] Building editor pane...";
        QElapsedTimer editorPaneTimer;
        editorPaneTimer.start();
        splitter->addWidget(buildEditorPane());
        qDebug() << "[STARTUP] Editor pane built in" << editorPaneTimer.elapsed() << "ms";
        splitter->addWidget(buildInspectorPane());
        
        splitter->setStretchFactor(0, 0);
        splitter->setStretchFactor(1, 1);
        splitter->setStretchFactor(2, 0);
        splitter->setSizes({320, 900, 280});

        setCentralWidget(central);

        connect(&m_playbackTimer, &QTimer::timeout, this, &EditorWindow::advanceFrame);
        m_playbackTimer.setInterval(33);
        auto* undoShortcut = new QShortcut(QKeySequence::Undo, this);
        connect(undoShortcut, &QShortcut::activated, this, [this]() { undoHistory(); });
        auto* splitShortcut = new QShortcut(QKeySequence(QStringLiteral("Ctrl+B")), this);
        connect(splitShortcut, &QShortcut::activated, this, [this]() {
            if (!m_timeline) {
                return;
            }
            if (m_timeline->splitSelectedClipAtFrame(m_timeline->currentFrame())) {
                refreshInspector();
            }
        });
        auto* deleteShortcut = new QShortcut(QKeySequence::Delete, this);
        connect(deleteShortcut, &QShortcut::activated, this, [this]() {
            if (!m_timeline) {
                return;
            }
            if (m_timeline->deleteSelectedClip()) {
                refreshInspector();
            }
        });
        auto* nudgeLeftShortcut = new QShortcut(QKeySequence(QStringLiteral("Alt+Left")), this);
        connect(nudgeLeftShortcut, &QShortcut::activated, this, [this]() {
            if (m_timeline) {
                m_timeline->nudgeSelectedClip(-1);
            }
        });
        auto* nudgeRightShortcut = new QShortcut(QKeySequence(QStringLiteral("Alt+Right")), this);
        connect(nudgeRightShortcut, &QShortcut::activated, this, [this]() {
            if (m_timeline) {
                m_timeline->nudgeSelectedClip(1);
            }
        });
        m_mainThreadHeartbeatTimer.setInterval(100);
        connect(&m_mainThreadHeartbeatTimer, &QTimer::timeout, this, [this]() {
            m_lastMainThreadHeartbeatMs.store(nowMs());
        });
        m_lastMainThreadHeartbeatMs.store(nowMs());
        m_mainThreadHeartbeatTimer.start();
        m_stateSaveTimer.setSingleShot(true);
        m_stateSaveTimer.setInterval(250);
        connect(&m_stateSaveTimer, &QTimer::timeout, this, [this]() {
            saveStateNow();
        });

        m_fastCurrentFrame.store(0);
        m_fastPlaybackActive.store(false);

        m_controlServer = std::make_unique<ControlServer>(
            this,
            [this]() {
                const qint64 now = nowMs();
                const qint64 heartbeatMs = m_lastMainThreadHeartbeatMs.load();
                const qint64 playheadMs = m_lastPlayheadAdvanceMs.load();
                return QJsonObject{
                    {QStringLiteral("ok"), true},
                    {QStringLiteral("pid"), static_cast<qint64>(QCoreApplication::applicationPid())},
                    {QStringLiteral("current_frame"), m_fastCurrentFrame.load()},
                    {QStringLiteral("playback_active"), m_fastPlaybackActive.load()},
                    {QStringLiteral("main_thread_heartbeat_ms"), heartbeatMs},
                    {QStringLiteral("main_thread_heartbeat_age_ms"), heartbeatMs > 0 ? now - heartbeatMs : -1},
                    {QStringLiteral("last_playhead_advance_ms"), playheadMs},
                    {QStringLiteral("last_playhead_advance_age_ms"), playheadMs > 0 ? now - playheadMs : -1}
                };
            },
            [this]() {
                return this->profilingSnapshot();
            },
            this);
        m_controlServer->start(controlPort);
        qDebug() << "[STARTUP] ControlServer started in" << ctorTimer.elapsed() << "ms";
        m_audioEngine = std::make_unique<AudioEngine>();

        auto connectGradeLive = [this](QDoubleSpinBox* spin) {
            connect(spin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double) {
                applySelectedClipGradeFromInspector(false);
            });
            connect(spin, &QDoubleSpinBox::editingFinished, this, [this]() {
                applySelectedClipGradeFromInspector(true);
            });
        };
        connectGradeLive(m_brightnessSpin);
        connectGradeLive(m_contrastSpin);
        connectGradeLive(m_saturationSpin);
        connectGradeLive(m_opacitySpin);

        auto connectVideoKeyframeLive = [this](QDoubleSpinBox* spin) {
            connect(spin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double) {
                applySelectedVideoKeyframeFromInspector(false);
            });
            connect(spin, &QDoubleSpinBox::editingFinished, this, [this]() {
                applySelectedVideoKeyframeFromInspector(true);
            });
        };
        connectVideoKeyframeLive(m_videoTranslationXSpin);
        connectVideoKeyframeLive(m_videoTranslationYSpin);
        connectVideoKeyframeLive(m_videoRotationSpin);
        connectVideoKeyframeLive(m_videoScaleXSpin);
        connectVideoKeyframeLive(m_videoScaleYSpin);
        connect(m_videoInterpolationCombo, &QComboBox::currentIndexChanged, this, [this](int) {
            applySelectedVideoKeyframeFromInspector(true);
        });

        QTimer::singleShot(0, this, [this]() {
            loadState();
            refreshInspector();
        });
    }
    
    ~EditorWindow() override {
        saveStateNow();
    }

protected:
    void closeEvent(QCloseEvent* event) override {
        saveStateNow();
        QMainWindow::closeEvent(event);
    }

    bool eventFilter(QObject* watched, QEvent* event) override {
        if (watched == (m_tree ? m_tree->viewport() : nullptr)) {
            if (event->type() == QEvent::Leave) {
                hideExplorerHoverPreview();
            } else if (event->type() == QEvent::MouseMove && m_tree && m_fsModel) {
                const auto* mouseEvent = static_cast<QMouseEvent*>(event);
                const QModelIndex index = m_tree->indexAt(mouseEvent->pos());
                if (!index.isValid()) {
                    hideExplorerHoverPreview();
                } else {
                    const QFileInfo info = m_fsModel->fileInfo(index);
                    if (!info.exists() || !info.isFile()) {
                        hideExplorerHoverPreview();
                    }
                }
            }
        } else if (watched == (m_galleryList ? m_galleryList->viewport() : nullptr)) {
            if (event->type() == QEvent::Leave) {
                hideExplorerHoverPreview();
            } else if (event->type() == QEvent::MouseMove && m_galleryList) {
                const auto* mouseEvent = static_cast<QMouseEvent*>(event);
                QListWidgetItem* item = m_galleryList->itemAt(mouseEvent->pos());
                if (!item) {
                    hideExplorerHoverPreview();
                } else {
                    const QFileInfo info(item->data(Qt::UserRole).toString());
                    if (!info.exists() || !info.isFile()) {
                        hideExplorerHoverPreview();
                    }
                }
            }
        }
        return QMainWindow::eventFilter(watched, event);
    }

private slots:
    void advanceFrame() {
        if (m_audioEngine && m_audioEngine->hasPlayableAudio()) {
            const int64_t audioFrame = qBound<int64_t>(0, m_audioEngine->currentFrame(), m_timeline->totalFrames());
            if (m_preview && !m_preview->preparePlaybackAdvance(audioFrame)) {
                playbackTrace(QStringLiteral("EditorWindow::advanceFrame.wait"),
                              QStringLiteral("audio=%1").arg(audioFrame));
                return;
            }
            playbackTrace(QStringLiteral("EditorWindow::advanceFrame"),
                          QStringLiteral("clock=audio frame=%1").arg(audioFrame));
            setCurrentFrame(audioFrame, false);
            return;
        }

        const int64_t nextFrame = m_timeline->currentFrame() + 1;
        const int64_t wrapped = nextFrame > m_timeline->totalFrames() ? 0 : nextFrame;
        if (m_preview && !m_preview->preparePlaybackAdvance(wrapped)) {
            playbackTrace(QStringLiteral("EditorWindow::advanceFrame.wait"),
                          QStringLiteral("next=%1").arg(wrapped));
            return;
        }
        playbackTrace(QStringLiteral("EditorWindow::advanceFrame"),
                      QStringLiteral("clock=video current=%1 next=%2").arg(m_timeline->currentFrame()).arg(wrapped));
        setCurrentFrame(wrapped, false);
    }

private:
    QString stateFilePath() const {
        return QDir(QDir::currentPath()).filePath(QStringLiteral("editor_state.json"));
    }

    QString historyFilePath() const {
        return QDir(QDir::currentPath()).filePath(QStringLiteral("editor_history.json"));
    }

    QJsonObject buildStateJson() const {
        QJsonObject root;
        root[QStringLiteral("explorerRoot")] = m_currentRootPath;
        root[QStringLiteral("explorerGalleryPath")] = m_galleryFolderPath;
        root[QStringLiteral("currentFrame")] = static_cast<qint64>(m_timeline ? m_timeline->currentFrame() : 0);
        root[QStringLiteral("playing")] = m_playbackTimer.isActive();
        root[QStringLiteral("selectedClipId")] = m_timeline ? m_timeline->selectedClipId() : QString();
        QJsonArray expandedFolders;
        for (const QString& path : currentExpandedExplorerPaths()) {
            expandedFolders.push_back(path);
        }
        root[QStringLiteral("explorerExpandedFolders")] = expandedFolders;
        root[QStringLiteral("outputWidth")] = m_outputWidthSpin ? m_outputWidthSpin->value() : 1080;
        root[QStringLiteral("outputHeight")] = m_outputHeightSpin ? m_outputHeightSpin->value() : 1920;
        root[QStringLiteral("outputFormat")] =
            m_outputFormatCombo ? m_outputFormatCombo->currentData().toString() : QStringLiteral("mp4");

        QJsonArray timeline;
        if (m_timeline) {
            for (const TimelineClip& clip : m_timeline->clips()) {
                timeline.push_back(clipToJson(clip));
            }
        }
        root[QStringLiteral("timeline")] = timeline;
        QJsonArray renderSyncMarkers;
        if (m_timeline) {
            for (const RenderSyncMarker& marker : m_timeline->renderSyncMarkers()) {
                QJsonObject markerObj;
                markerObj[QStringLiteral("frame")] = static_cast<qint64>(marker.frame);
                markerObj[QStringLiteral("action")] = renderSyncActionToString(marker.action);
                renderSyncMarkers.push_back(markerObj);
            }
        }
        root[QStringLiteral("renderSyncMarkers")] = renderSyncMarkers;

        QJsonArray tracks;
        if (m_timeline) {
            for (const TimelineTrack& track : m_timeline->tracks()) {
                QJsonObject trackObj;
                trackObj[QStringLiteral("name")] = track.name;
                trackObj[QStringLiteral("height")] = track.height;
                tracks.push_back(trackObj);
            }
        }
        root[QStringLiteral("tracks")] = tracks;
        return root;
    }

    void scheduleSaveState() {
        if (m_loadingState || !m_timeline) {
            return;
        }
        m_stateSaveTimer.start();
    }

    void saveStateNow() {
        if (!m_timeline) {
            return;
        }
        if (m_loadingState) {
            m_pendingSaveAfterLoad = true;
            return;
        }

        m_stateSaveTimer.stop();

        const QByteArray serializedState =
            QJsonDocument(buildStateJson()).toJson(QJsonDocument::Indented);
        if (serializedState == m_lastSavedState) {
            return;
        }

        QSaveFile file(stateFilePath());
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            return;
        }

        if (file.write(serializedState) != serializedState.size()) {
            file.cancelWriting();
            return;
        }

        if (!file.commit()) {
            return;
        }

        m_lastSavedState = serializedState;
    }

    void saveHistoryNow() {
        QJsonObject root;
        root[QStringLiteral("index")] = m_historyIndex;
        root[QStringLiteral("entries")] = m_historyEntries;

        QSaveFile file(historyFilePath());
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            return;
        }

        const QByteArray payload = QJsonDocument(root).toJson(QJsonDocument::Indented);
        if (file.write(payload) != payload.size()) {
            file.cancelWriting();
            return;
        }
        file.commit();
    }

    void pushHistorySnapshot() {
        if (m_loadingState || m_restoringHistory || !m_timeline) {
            return;
        }

        const QJsonObject snapshot = buildStateJson();
        if (m_historyIndex >= 0 && m_historyIndex < m_historyEntries.size() &&
            m_historyEntries.at(m_historyIndex).toObject() == snapshot) {
            return;
        }

        while (m_historyEntries.size() > m_historyIndex + 1) {
            m_historyEntries.removeAt(m_historyEntries.size() - 1);
        }
        m_historyEntries.append(snapshot);
        if (m_historyEntries.size() > 200) {
            m_historyEntries.removeAt(0);
        }
        m_historyIndex = m_historyEntries.size() - 1;
        saveHistoryNow();
    }

    void applyStateJson(const QJsonObject& root) {
        m_loadingState = true;

        QString rootPath = root.value(QStringLiteral("explorerRoot")).toString(QDir::currentPath());
        m_galleryFolderPath = root.value(QStringLiteral("explorerGalleryPath")).toString();
        const int outputWidth = qMax(16, root.value(QStringLiteral("outputWidth")).toInt(1080));
        const int outputHeight = qMax(16, root.value(QStringLiteral("outputHeight")).toInt(1920));
        const QString outputFormat = root.value(QStringLiteral("outputFormat")).toString(QStringLiteral("mp4"));
        m_expandedExplorerPaths.clear();
        for (const QJsonValue& value : root.value(QStringLiteral("explorerExpandedFolders")).toArray()) {
            const QString path = value.toString();
            if (!path.isEmpty()) {
                m_expandedExplorerPaths.push_back(path);
            }
        }
        QVector<TimelineClip> loadedClips;
        QVector<RenderSyncMarker> loadedRenderSyncMarkers;
        const int64_t currentFrame = root.value(QStringLiteral("currentFrame")).toVariant().toLongLong();
        const QString selectedClipId = root.value(QStringLiteral("selectedClipId")).toString();
        QVector<TimelineTrack> loadedTracks;

        const QJsonArray clips = root.value(QStringLiteral("timeline")).toArray();
        loadedClips.reserve(clips.size());
        for (const QJsonValue& value : clips) {
            if (!value.isObject()) continue;
            TimelineClip clip = clipFromJson(value.toObject());
            if (clip.trackIndex < 0) {
                clip.trackIndex = loadedClips.size();
            }
            if (!clip.filePath.isEmpty()) {
                loadedClips.push_back(clip);
            }
        }

        const QJsonArray tracks = root.value(QStringLiteral("tracks")).toArray();
        loadedTracks.reserve(tracks.size());
        for (int i = 0; i < tracks.size(); ++i) {
            const QJsonObject obj = tracks.at(i).toObject();
            TimelineTrack track;
            track.name = obj.value(QStringLiteral("name")).toString(QStringLiteral("Track %1").arg(i + 1));
            track.height = qMax(28, obj.value(QStringLiteral("height")).toInt(44));
            loadedTracks.push_back(track);
        }

        const QJsonArray renderSyncMarkers = root.value(QStringLiteral("renderSyncMarkers")).toArray();
        loadedRenderSyncMarkers.reserve(renderSyncMarkers.size());
        for (const QJsonValue& value : renderSyncMarkers) {
            if (!value.isObject()) {
                continue;
            }
            const QJsonObject obj = value.toObject();
            RenderSyncMarker marker;
            marker.frame = qMax<int64_t>(0, obj.value(QStringLiteral("frame")).toVariant().toLongLong());
            marker.action = renderSyncActionFromString(obj.value(QStringLiteral("action")).toString());
            loadedRenderSyncMarkers.push_back(marker);
        }

        const QString resolvedRootPath = QDir(rootPath).absolutePath();
        m_currentRootPath = resolvedRootPath;
        if (m_rootPathLabel) {
            m_rootPathLabel->setText(resolvedRootPath);
        }
        if (m_outputWidthSpin) {
            QSignalBlocker block(m_outputWidthSpin);
            m_outputWidthSpin->setValue(outputWidth);
        }
        if (m_outputHeightSpin) {
            QSignalBlocker block(m_outputHeightSpin);
            m_outputHeightSpin->setValue(outputHeight);
        }
        if (m_outputFormatCombo) {
            QSignalBlocker block(m_outputFormatCombo);
            const int formatIndex = m_outputFormatCombo->findData(outputFormat);
            if (formatIndex >= 0) {
                m_outputFormatCombo->setCurrentIndex(formatIndex);
            }
        }
        if (m_preview) {
            m_preview->setOutputSize(QSize(outputWidth, outputHeight));
        }

        m_timeline->setTracks(loadedTracks);
        m_timeline->setClips(loadedClips);
        m_timeline->setRenderSyncMarkers(loadedRenderSyncMarkers);
        m_timeline->setSelectedClipId(selectedClipId);
        syncSliderRange();
        m_preview->setClipCount(m_timeline->clips().size());
        m_preview->setTimelineClips(m_timeline->clips());
        m_preview->setSelectedClipId(selectedClipId);
        if (m_audioEngine) {
            m_audioEngine->setTimelineClips(m_timeline->clips());
            m_audioEngine->seek(currentFrame);
        }
        setCurrentFrame(currentFrame);

        m_playbackTimer.stop();
        m_fastPlaybackActive.store(false);
        m_preview->setPlaybackState(false);
        if (m_audioEngine) {
            m_audioEngine->stop();
        }

        m_loadingState = false;
        QTimer::singleShot(0, this, [this, resolvedRootPath]() {
            setExplorerRootPath(resolvedRootPath, false);
            refreshInspector();
        });
    }

    void undoHistory() {
        if (m_historyIndex <= 0 || m_historyEntries.isEmpty()) {
            return;
        }

        m_restoringHistory = true;
        m_historyIndex -= 1;
        applyStateJson(m_historyEntries.at(m_historyIndex).toObject());
        m_restoringHistory = false;
        saveHistoryNow();
        scheduleSaveState();
    }
    
    void setExplorerRootPath(const QString& path, bool saveAfterChange = true) {
        if (!m_fsModel || !m_tree) {
            return;
        }
        
        QString resolvedPath = path;
        if (resolvedPath.isEmpty() || !QFileInfo::exists(resolvedPath) || !QFileInfo(resolvedPath).isDir()) {
            resolvedPath = QDir::currentPath();
        }
        
        m_currentRootPath = QDir(resolvedPath).absolutePath();
        const QModelIndex rootIndex = m_fsModel->setRootPath(m_currentRootPath);
        m_tree->setRootIndex(rootIndex);
        hideExplorerHoverPreview();
        restoreExpandedExplorerPaths();
        if (!m_galleryFolderPath.isEmpty()) {
            setExplorerGalleryPath(m_galleryFolderPath, false);
        }
        
        if (m_rootPathLabel) {
            m_rootPathLabel->setText(m_currentRootPath);
        }
        
        if (saveAfterChange) {
            scheduleSaveState();
            pushHistorySnapshot();
        }
    }

    QStringList currentExpandedExplorerPaths() const {
        QStringList expanded;
        if (!m_tree || !m_fsModel) {
            return expanded;
        }
        const QModelIndex rootIndex = m_tree->rootIndex();
        std::function<void(const QModelIndex&)> collect = [&](const QModelIndex& parent) {
            const int rowCount = m_fsModel->rowCount(parent);
            for (int row = 0; row < rowCount; ++row) {
                const QModelIndex child = m_fsModel->index(row, 0, parent);
                if (!child.isValid()) {
                    continue;
                }
                const QFileInfo info = m_fsModel->fileInfo(child);
                if (!info.isDir()) {
                    continue;
                }
                if (m_tree->isExpanded(child)) {
                    expanded.push_back(info.absoluteFilePath());
                    collect(child);
                }
            }
        };
        collect(rootIndex);
        return expanded;
    }

    void restoreExpandedExplorerPaths() {
        if (!m_tree || !m_fsModel) {
            return;
        }
        for (const QString& path : m_expandedExplorerPaths) {
            const QModelIndex index = m_fsModel->index(path);
            if (index.isValid()) {
                m_tree->expand(index);
            }
        }
    }

    void populateExplorerGallery(const QString& folderPath) {
        if (!m_galleryList) {
            return;
        }
        m_galleryList->clear();
        QFileIconProvider iconProvider;
        QDir dir(folderPath);
        const QFileInfoList entries = dir.entryInfoList(QDir::NoDotAndDotDot | QDir::AllEntries,
                                                        QDir::DirsFirst | QDir::Name | QDir::IgnoreCase);
        for (const QFileInfo& info : entries) {
            auto* item = new QListWidgetItem(iconProvider.icon(info), info.fileName(), m_galleryList);
            item->setData(Qt::UserRole, info.absoluteFilePath());
            item->setToolTip(QDir::toNativeSeparators(info.absoluteFilePath()));
            if (info.isDir()) {
                item->setText(QStringLiteral("[%1]").arg(info.fileName()));
            }
        }
        if (m_galleryTitleLabel) {
            m_galleryTitleLabel->setText(QDir::toNativeSeparators(folderPath));
        }
    }

    void setExplorerGalleryPath(const QString& folderPath, bool saveAfterChange = true) {
        if (!m_explorerStack || !m_galleryList) {
            return;
        }
        if (folderPath.isEmpty() || !QFileInfo(folderPath).isDir()) {
            m_galleryFolderPath.clear();
            m_explorerStack->setCurrentIndex(0);
            hideExplorerHoverPreview();
            if (saveAfterChange) {
                scheduleSaveState();
            }
            return;
        }
        m_galleryFolderPath = QDir(folderPath).absolutePath();
        populateExplorerGallery(m_galleryFolderPath);
        m_explorerStack->setCurrentIndex(1);
        if (saveAfterChange) {
            scheduleSaveState();
            pushHistorySnapshot();
        }
    }

    QPixmap previewPixmapForFile(const QString& filePath) const {
        const QFileInfo info(filePath);
        if (!info.exists() || !info.isFile()) {
            return QPixmap();
        }

        const MediaProbeResult probe = probeMediaFile(filePath);
        if (probe.mediaType == ClipMediaType::Image) {
            QImage image(filePath);
            return image.isNull() ? QPixmap() : QPixmap::fromImage(image);
        }
        if (probe.mediaType != ClipMediaType::Video) {
            return QPixmap();
        }

        AVFormatContext* formatCtx = nullptr;
        if (avformat_open_input(&formatCtx, QFile::encodeName(filePath).constData(), nullptr, nullptr) < 0) {
            return QPixmap();
        }
        if (avformat_find_stream_info(formatCtx, nullptr) < 0) {
            avformat_close_input(&formatCtx);
            return QPixmap();
        }

        int videoStreamIndex = -1;
        for (unsigned i = 0; i < formatCtx->nb_streams; ++i) {
            if (formatCtx->streams[i] && formatCtx->streams[i]->codecpar &&
                formatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                videoStreamIndex = static_cast<int>(i);
                break;
            }
        }
        if (videoStreamIndex < 0) {
            avformat_close_input(&formatCtx);
            return QPixmap();
        }

        AVStream* stream = formatCtx->streams[videoStreamIndex];
        const AVCodec* decoder = avcodec_find_decoder(stream->codecpar->codec_id);
        if (!decoder) {
            avformat_close_input(&formatCtx);
            return QPixmap();
        }

        AVCodecContext* codecCtx = avcodec_alloc_context3(decoder);
        if (!codecCtx) {
            avformat_close_input(&formatCtx);
            return QPixmap();
        }
        if (avcodec_parameters_to_context(codecCtx, stream->codecpar) < 0 ||
            avcodec_open2(codecCtx, decoder, nullptr) < 0) {
            avcodec_free_context(&codecCtx);
            avformat_close_input(&formatCtx);
            return QPixmap();
        }

        AVPacket* packet = av_packet_alloc();
        AVFrame* frame = av_frame_alloc();
        QPixmap pixmap;
        auto decodeFrame = [&](AVFrame* decodedFrame) {
            if (decodedFrame->width <= 0 || decodedFrame->height <= 0) {
                return;
            }
            SwsContext* sws = sws_getContext(decodedFrame->width,
                                             decodedFrame->height,
                                             static_cast<AVPixelFormat>(decodedFrame->format),
                                             decodedFrame->width,
                                             decodedFrame->height,
                                             AV_PIX_FMT_RGBA,
                                             SWS_BILINEAR,
                                             nullptr,
                                             nullptr,
                                             nullptr);
            if (!sws) {
                return;
            }
            QImage image(decodedFrame->width, decodedFrame->height, QImage::Format_RGBA8888);
            uint8_t* destData[4] = { image.bits(), nullptr, nullptr, nullptr };
            int destLinesize[4] = { static_cast<int>(image.bytesPerLine()), 0, 0, 0 };
            sws_scale(sws,
                      decodedFrame->data,
                      decodedFrame->linesize,
                      0,
                      decodedFrame->height,
                      destData,
                      destLinesize);
            sws_freeContext(sws);
            pixmap = QPixmap::fromImage(image.copy());
        };

        while (av_read_frame(formatCtx, packet) >= 0 && pixmap.isNull()) {
            if (packet->stream_index == videoStreamIndex && avcodec_send_packet(codecCtx, packet) >= 0) {
                while (avcodec_receive_frame(codecCtx, frame) >= 0) {
                    decodeFrame(frame);
                    av_frame_unref(frame);
                    if (!pixmap.isNull()) {
                        break;
                    }
                }
            }
            av_packet_unref(packet);
        }

        av_frame_free(&frame);
        av_packet_free(&packet);
        avcodec_free_context(&codecCtx);
        avformat_close_input(&formatCtx);
        return pixmap;
    }

    void hideExplorerHoverPreview() {
        if (m_explorerHoverPreview) {
            m_explorerHoverPreview->hide();
        }
    }

    void showExplorerHoverPreview(const QString& filePath, const QPoint& globalPos) {
        const QPixmap source = previewPixmapForFile(filePath);
        if (source.isNull()) {
            hideExplorerHoverPreview();
            return;
        }
        if (!m_explorerHoverPreview) {
            m_explorerHoverPreview = new QLabel(nullptr, Qt::ToolTip);
            m_explorerHoverPreview->setObjectName(QStringLiteral("explorerHoverPreview"));
            m_explorerHoverPreview->setAlignment(Qt::AlignCenter);
            m_explorerHoverPreview->setStyleSheet(
                QStringLiteral("QLabel#explorerHoverPreview { background: #05080c; color: #edf2f7; border: 1px solid #24303c; border-radius: 10px; padding: 8px; }"));
        }
        const QPixmap scaled = source.scaled(QSize(280, 180), Qt::KeepAspectRatio, Qt::SmoothTransformation);
        m_explorerHoverPreview->setPixmap(scaled);
        m_explorerHoverPreview->resize(scaled.size() + QSize(16, 16));
        m_explorerHoverPreview->move(globalPos + QPoint(18, 18));
        m_explorerHoverPreview->show();
    }
    
    void chooseExplorerRoot() {
        const QString startPath = m_currentRootPath.isEmpty() ? QDir::currentPath() : m_currentRootPath;
        const QString selected = QFileDialog::getExistingDirectory(this,
            QStringLiteral("Select Media Folder"), startPath,
            QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);

        if (!selected.isEmpty()) {
            setExplorerRootPath(selected, true);
        }
    }
    
    QJsonObject clipToJson(const TimelineClip& clip) const {
        QJsonObject obj;
        obj[QStringLiteral("id")] = clip.id;
        obj[QStringLiteral("filePath")] = clip.filePath;
        obj[QStringLiteral("label")] = clip.label;
        obj[QStringLiteral("mediaType")] = clipMediaTypeToString(clip.mediaType);
        obj[QStringLiteral("hasAudio")] = clip.hasAudio;
        obj[QStringLiteral("sourceDurationFrames")] = static_cast<qint64>(clip.sourceDurationFrames);
        obj[QStringLiteral("sourceInFrame")] = static_cast<qint64>(clip.sourceInFrame);
        obj[QStringLiteral("sourceInSubframeSamples")] = static_cast<qint64>(clip.sourceInSubframeSamples);
        obj[QStringLiteral("startFrame")] = static_cast<qint64>(clip.startFrame);
        obj[QStringLiteral("startSubframeSamples")] = static_cast<qint64>(clip.startSubframeSamples);
        obj[QStringLiteral("durationFrames")] = static_cast<qint64>(clip.durationFrames);
        obj[QStringLiteral("trackIndex")] = clip.trackIndex;
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
        for (const TimelineClip::TransformKeyframe& keyframe : clip.transformKeyframes) {
            QJsonObject keyframeObj;
            keyframeObj[QStringLiteral("frame")] = static_cast<qint64>(keyframe.frame);
            keyframeObj[QStringLiteral("translationX")] = keyframe.translationX;
            keyframeObj[QStringLiteral("translationY")] = keyframe.translationY;
            keyframeObj[QStringLiteral("rotation")] = keyframe.rotation;
            keyframeObj[QStringLiteral("scaleX")] = keyframe.scaleX;
            keyframeObj[QStringLiteral("scaleY")] = keyframe.scaleY;
            keyframeObj[QStringLiteral("interpolated")] = keyframe.interpolated;
            keyframes.push_back(keyframeObj);
        }
        obj[QStringLiteral("transformKeyframes")] = keyframes;
        return obj;
    }
    
    TimelineClip clipFromJson(const QJsonObject& obj) const {
        TimelineClip clip;
        clip.id = obj.value(QStringLiteral("id")).toString();
        clip.filePath = obj.value(QStringLiteral("filePath")).toString();
        clip.label = obj.value(QStringLiteral("label")).toString(QFileInfo(clip.filePath).fileName());
        clip.mediaType = clipMediaTypeFromString(obj.value(QStringLiteral("mediaType")).toString());
        clip.hasAudio = obj.value(QStringLiteral("hasAudio")).toBool(false);
        clip.sourceDurationFrames = obj.value(QStringLiteral("sourceDurationFrames")).toVariant().toLongLong();
        clip.sourceInFrame = obj.value(QStringLiteral("sourceInFrame")).toVariant().toLongLong();
        clip.sourceInSubframeSamples = obj.value(QStringLiteral("sourceInSubframeSamples")).toVariant().toLongLong();
        clip.startFrame = obj.value(QStringLiteral("startFrame")).toVariant().toLongLong();
        clip.startSubframeSamples = obj.value(QStringLiteral("startSubframeSamples")).toVariant().toLongLong();
        clip.durationFrames = obj.value(QStringLiteral("durationFrames")).toVariant().toLongLong();
        clip.trackIndex = obj.value(QStringLiteral("trackIndex")).toInt(-1);
        if (clip.durationFrames == 0) clip.durationFrames = 120;
        if (clip.mediaType == ClipMediaType::Unknown && !clip.filePath.isEmpty()) {
            const MediaProbeResult probe = probeMediaFile(clip.filePath, clip.durationFrames);
            clip.mediaType = probe.mediaType;
            clip.hasAudio = probe.hasAudio;
            clip.durationFrames = probe.durationFrames;
            clip.sourceDurationFrames = probe.durationFrames;
        }
        if (clip.sourceDurationFrames <= 0) {
            clip.sourceDurationFrames = clip.sourceInFrame + clip.durationFrames;
        }
        normalizeClipTiming(clip);
        clip.color = QColor(obj.value(QStringLiteral("color")).toString());
        if (!clip.color.isValid()) {
            clip.color = QColor::fromHsv(static_cast<int>(qHash(clip.filePath) % 360), 160, 220, 220);
        }
        if (clip.mediaType == ClipMediaType::Audio) {
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
        for (const QJsonValue& value : keyframes) {
            if (!value.isObject()) {
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
            keyframe.interpolated = keyframeObj.value(QStringLiteral("interpolated")).toBool(true);
            clip.transformKeyframes.push_back(keyframe);
        }
        normalizeClipTransformKeyframes(clip);
        return clip;
    }
    
    void loadState() {
        QJsonObject root;
        QFile historyFile(historyFilePath());
        if (historyFile.open(QIODevice::ReadOnly)) {
            const QJsonObject historyRoot = QJsonDocument::fromJson(historyFile.readAll()).object();
            m_historyEntries = historyRoot.value(QStringLiteral("entries")).toArray();
            m_historyIndex = historyRoot.value(QStringLiteral("index")).toInt(m_historyEntries.size() - 1);
            if (!m_historyEntries.isEmpty()) {
                m_historyIndex = qBound(0, m_historyIndex, m_historyEntries.size() - 1);
                root = m_historyEntries.at(m_historyIndex).toObject();
            }
        }

        if (root.isEmpty()) {
            QFile file(stateFilePath());
            if (file.open(QIODevice::ReadOnly)) {
                root = QJsonDocument::fromJson(file.readAll()).object();
            }
        }

        applyStateJson(root);
        if (m_historyEntries.isEmpty()) {
            pushHistorySnapshot();
        }

        if (m_pendingSaveAfterLoad) {
            m_pendingSaveAfterLoad = false;
            scheduleSaveState();
        } else {
            scheduleSaveState();
        }
    }
    
    QWidget* buildExplorerPane() {
        auto* pane = new QFrame;
        pane->setFrameShape(QFrame::NoFrame);
        pane->setMinimumWidth(260);
        pane->setStyleSheet(
            QStringLiteral("QFrame { background: #11161c; color: #e8edf2; }"
                          "QLabel { color: #dce5ee; font-weight: 600; letter-spacing: 0.08em; }"
                          "QPushButton#folderPicker { background: transparent; border: none; color: #dce5ee; font-weight: 700; letter-spacing: 0.08em; padding: 0; text-align: left; }"
                          "QPushButton#folderPicker:hover { color: #ffffff; }"
                          "QToolButton#explorerRefresh { background: transparent; border: 1px solid #24303c; border-radius: 8px; color: #dce5ee; padding: 4px 8px; }"
                          "QToolButton#explorerRefresh:hover { background: #1a2330; color: #ffffff; }"
                          "QLabel#rootPath { color: #8ea0b2; font-size: 11px; letter-spacing: 0; }"
                          "QTreeView { background: transparent; border: none; color: #dbe2ea; }"
                          "QTreeView::item { padding: 4px 0; }"
                          "QTreeView::item:selected { background: #213042; color: #f7fbff; }"));
        
        auto* layout = new QVBoxLayout(pane);
        layout->setContentsMargins(14, 14, 14, 14);
        layout->setSpacing(10);

        auto* headerRow = new QHBoxLayout;
        headerRow->setContentsMargins(0, 0, 0, 0);
        headerRow->setSpacing(8);
        m_folderPickerButton = new QPushButton(QStringLiteral("FILES"));
        m_folderPickerButton->setObjectName(QStringLiteral("folderPicker"));
        m_folderPickerButton->setCursor(Qt::PointingHandCursor);
        headerRow->addWidget(m_folderPickerButton);
        headerRow->addStretch(1);
        m_refreshExplorerButton = new QToolButton(pane);
        m_refreshExplorerButton->setObjectName(QStringLiteral("explorerRefresh"));
        m_refreshExplorerButton->setText(QStringLiteral("Refresh"));
        m_refreshExplorerButton->setCursor(Qt::PointingHandCursor);
        headerRow->addWidget(m_refreshExplorerButton);
        layout->addLayout(headerRow);
        
        m_rootPathLabel = new QLabel;
        m_rootPathLabel->setObjectName(QStringLiteral("rootPath"));
        m_rootPathLabel->setWordWrap(true);
        layout->addWidget(m_rootPathLabel);
        
        m_fsModel = new QFileSystemModel(this);
        m_fsModel->setFilter(QDir::AllDirs | QDir::Files | QDir::NoDotAndDotDot);
        
        m_tree = new QTreeView;
        m_tree->setObjectName(QStringLiteral("explorer.tree"));
        m_tree->setModel(m_fsModel);
        m_tree->setAlternatingRowColors(false);
        m_tree->setAnimated(true);
        m_tree->setIndentation(18);
        m_tree->setHeaderHidden(true);
        m_tree->setDragEnabled(true);
        m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
        m_tree->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_tree->setTextElideMode(Qt::ElideRight);
        m_tree->setMouseTracking(true);
        m_tree->viewport()->installEventFilter(this);
        m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
        m_tree->hideColumn(1);
        m_tree->hideColumn(2);
        m_tree->hideColumn(3);
        m_tree->header()->setStretchLastSection(true);

        m_explorerStack = new QStackedWidget(pane);
        m_explorerStack->addWidget(m_tree);

        auto* galleryPage = new QWidget(m_explorerStack);
        auto* galleryLayout = new QVBoxLayout(galleryPage);
        galleryLayout->setContentsMargins(0, 0, 0, 0);
        galleryLayout->setSpacing(8);
        auto* galleryHeader = new QHBoxLayout;
        galleryHeader->setContentsMargins(0, 0, 0, 0);
        galleryHeader->setSpacing(8);
        m_galleryBackButton = new QToolButton(galleryPage);
        m_galleryBackButton->setText(QStringLiteral("Tree"));
        m_galleryBackButton->setCursor(Qt::PointingHandCursor);
        galleryHeader->addWidget(m_galleryBackButton);
        m_galleryTitleLabel = new QLabel(QStringLiteral("Gallery"), galleryPage);
        m_galleryTitleLabel->setObjectName(QStringLiteral("rootPath"));
        m_galleryTitleLabel->setWordWrap(true);
        galleryHeader->addWidget(m_galleryTitleLabel, 1);
        galleryLayout->addLayout(galleryHeader);
        m_galleryList = new QListWidget(galleryPage);
        m_galleryList->setViewMode(QListView::IconMode);
        m_galleryList->setResizeMode(QListView::Adjust);
        m_galleryList->setMovement(QListView::Static);
        m_galleryList->setSpacing(12);
        m_galleryList->setIconSize(QSize(72, 72));
        m_galleryList->setWordWrap(true);
        m_galleryList->setUniformItemSizes(false);
        m_galleryList->setMouseTracking(true);
        m_galleryList->viewport()->installEventFilter(this);
        galleryLayout->addWidget(m_galleryList, 1);
        m_explorerStack->addWidget(galleryPage);
        layout->addWidget(m_explorerStack, 1);
        
        connect(m_tree, &QTreeView::doubleClicked, this, [this](const QModelIndex& index) {
            if (!m_fsModel) return;
            const QFileInfo info = m_fsModel->fileInfo(index);
            if (info.exists() && info.isFile()) {
                addFileToTimeline(info.absoluteFilePath());
            }
        });
        connect(m_tree, &QTreeView::entered, this, [this](const QModelIndex& index) {
            if (!m_fsModel || !m_tree) {
                return;
            }
            const QFileInfo info = m_fsModel->fileInfo(index);
            if (!info.exists() || !info.isFile()) {
                hideExplorerHoverPreview();
                return;
            }
            const MediaProbeResult probe = probeMediaFile(info.absoluteFilePath());
            if (probe.mediaType != ClipMediaType::Image && probe.mediaType != ClipMediaType::Video) {
                hideExplorerHoverPreview();
                return;
            }
            showExplorerHoverPreview(info.absoluteFilePath(),
                                     m_tree->viewport()->mapToGlobal(m_tree->visualRect(index).bottomRight()));
        });
        connect(m_tree, &QAbstractItemView::viewportEntered, this, [this]() {
            hideExplorerHoverPreview();
        });
        m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(m_tree, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
            if (!m_fsModel || !m_tree) {
                return;
            }
            const QModelIndex index = m_tree->indexAt(pos);
            if (!index.isValid()) {
                return;
            }
            const QFileInfo info = m_fsModel->fileInfo(index);
            if (!info.exists() || !info.isDir()) {
                return;
            }
            QMenu menu(this);
            QAction* galleryAction = menu.addAction(QStringLiteral("Open In Gallery"));
            QAction* selected = menu.exec(m_tree->viewport()->mapToGlobal(pos));
            if (selected == galleryAction) {
                setExplorerGalleryPath(info.absoluteFilePath(), true);
            }
        });
        connect(m_tree, &QTreeView::expanded, this, [this](const QModelIndex&) {
            m_expandedExplorerPaths = currentExpandedExplorerPaths();
            scheduleSaveState();
        });
        connect(m_tree, &QTreeView::collapsed, this, [this](const QModelIndex&) {
            m_expandedExplorerPaths = currentExpandedExplorerPaths();
            scheduleSaveState();
        });
        connect(m_folderPickerButton, &QPushButton::clicked, this, [this]() { chooseExplorerRoot(); });
        connect(m_refreshExplorerButton, &QToolButton::clicked, this, [this]() {
            m_expandedExplorerPaths = currentExpandedExplorerPaths();
            if (m_currentRootPath.isEmpty()) {
                setExplorerRootPath(QDir::currentPath(), false);
            } else {
                setExplorerRootPath(m_currentRootPath, false);
            }
        });
        connect(m_galleryBackButton, &QToolButton::clicked, this, [this]() {
            setExplorerGalleryPath(QString(), true);
        });
        connect(m_galleryList, &QListWidget::itemEntered, this, [this](QListWidgetItem* item) {
            if (!item || !m_galleryList) {
                hideExplorerHoverPreview();
                return;
            }
            const QString path = item->data(Qt::UserRole).toString();
            const QFileInfo info(path);
            if (!info.exists() || !info.isFile()) {
                hideExplorerHoverPreview();
                return;
            }
            const MediaProbeResult probe = probeMediaFile(path);
            if (probe.mediaType != ClipMediaType::Image && probe.mediaType != ClipMediaType::Video) {
                hideExplorerHoverPreview();
                return;
            }
            showExplorerHoverPreview(path,
                                     m_galleryList->viewport()->mapToGlobal(m_galleryList->visualItemRect(item).bottomRight()));
        });
        connect(m_galleryList, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* item) {
            if (!item) {
                return;
            }
            const QString path = item->data(Qt::UserRole).toString();
            const QFileInfo info(path);
            if (info.isDir()) {
                setExplorerGalleryPath(path, true);
            } else if (info.exists() && info.isFile()) {
                addFileToTimeline(path);
            }
        });
        connect(m_galleryList, &QListWidget::itemSelectionChanged, this, [this]() {
            if (m_galleryList && m_galleryList->selectedItems().isEmpty()) {
                hideExplorerHoverPreview();
            }
        });
        connect(m_galleryList, &QAbstractItemView::viewportEntered, this, [this]() {
            hideExplorerHoverPreview();
        });
        
        return pane;
    }
    
    QWidget* buildEditorPane() {
        auto* pane = new QWidget;
        pane->setStyleSheet(
            QStringLiteral("QWidget { background: #0c1015; color: #edf2f7; }"
                          "QPushButton, QToolButton { background: #1b2430; border: 1px solid #2e3b4a; border-radius: 7px; padding: 8px 12px; }"
                          "QPushButton:hover, QToolButton:hover { background: #233142; }"
                          "QSlider::groove:horizontal { background: #24303c; height: 6px; border-radius: 3px; }"
                          "QSlider::handle:horizontal { background: #ff6f61; width: 14px; margin: -5px 0; border-radius: 7px; }"));
        
        auto* layout = new QVBoxLayout(pane);
        layout->setContentsMargins(18, 18, 18, 18);
        layout->setSpacing(0);

        auto* verticalSplitter = new QSplitter(Qt::Vertical, pane);
        verticalSplitter->setObjectName(QStringLiteral("layout.editor_splitter"));
        verticalSplitter->setChildrenCollapsible(false);
        layout->addWidget(verticalSplitter, 1);
        
        auto* previewFrame = new QFrame;
        previewFrame->setMinimumHeight(240);
        previewFrame->setFrameShape(QFrame::NoFrame);
        previewFrame->setStyleSheet(QStringLiteral("QFrame { background: #05080c; border: 1px solid #202934; border-radius: 14px; }"));
        auto* previewLayout = new QVBoxLayout(previewFrame);
        previewLayout->setContentsMargins(0, 0, 0, 0);
        previewLayout->setSpacing(0);
        
        m_preview = new PreviewWindow;
        m_preview->setObjectName(QStringLiteral("preview.window"));
        m_preview->setFocusPolicy(Qt::StrongFocus);
        m_preview->setMinimumSize(640, 360);
        m_preview->setOutputSize(QSize(1080, 1920));
        
        auto* overlay = new QWidget;
        overlay->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        overlay->setStyleSheet(QStringLiteral("background: transparent;"));
        auto* overlayLayout = new QVBoxLayout(overlay);
        overlayLayout->setContentsMargins(18, 14, 18, 14);
        overlayLayout->setSpacing(6);
        
        auto* badgeRow = new QHBoxLayout;
        badgeRow->setContentsMargins(0, 0, 0, 0);
        m_statusBadge = new QLabel;
        m_statusBadge->setObjectName(QStringLiteral("overlay.status_badge"));
        m_statusBadge->setStyleSheet(QStringLiteral("QLabel { background: rgba(7, 11, 17, 0.72); color: #f2f7fb; border-radius: 10px; padding: 8px 12px; font-weight: 600; }"));
        badgeRow->addWidget(m_statusBadge, 0, Qt::AlignLeft);
        badgeRow->addStretch(1);
        overlayLayout->addLayout(badgeRow);
        
        overlayLayout->addStretch(1);
        
        m_previewInfo = new QLabel;
        m_previewInfo->setObjectName(QStringLiteral("overlay.preview_info"));
        m_previewInfo->setStyleSheet(QStringLiteral("QLabel { background: rgba(7, 11, 17, 0.72); color: #dce6ef; border-radius: 10px; padding: 10px 12px; }"));
        m_previewInfo->setWordWrap(true);
        overlayLayout->addWidget(m_previewInfo, 0, Qt::AlignLeft | Qt::AlignBottom);
        
        auto* stack = new QStackedLayout;
        stack->setStackingMode(QStackedLayout::StackAll);
        stack->addWidget(m_preview);
        stack->addWidget(overlay);
        previewLayout->addLayout(stack);
        verticalSplitter->addWidget(previewFrame);

        auto* timelinePane = new QWidget;
        timelinePane->setMinimumHeight(220);
        auto* timelineLayout = new QVBoxLayout(timelinePane);
        timelineLayout->setContentsMargins(0, 14, 0, 0);
        timelineLayout->setSpacing(14);
        
        auto* transport = new QWidget;
        auto* transportLayout = new QHBoxLayout(transport);
        transportLayout->setContentsMargins(0, 0, 0, 0);
        transportLayout->setSpacing(10);
        
        m_playButton = new QPushButton(style()->standardIcon(QStyle::SP_MediaPlay), QStringLiteral("Play"));
        m_playButton->setObjectName(QStringLiteral("transport.play"));
        auto* pauseButton = new QPushButton(style()->standardIcon(QStyle::SP_MediaPause), QStringLiteral("Pause"));
        pauseButton->setObjectName(QStringLiteral("transport.pause"));
        auto* startButton = new QToolButton;
        auto* endButton = new QToolButton;
        startButton->setObjectName(QStringLiteral("transport.start"));
        endButton->setObjectName(QStringLiteral("transport.end"));
        startButton->setIcon(style()->standardIcon(QStyle::SP_MediaSkipBackward));
        endButton->setIcon(style()->standardIcon(QStyle::SP_MediaSkipForward));
        
        m_seekSlider = new QSlider(Qt::Horizontal);
        m_seekSlider->setObjectName(QStringLiteral("transport.seek"));
        m_seekSlider->setRange(0, 300);
        
        m_timecodeLabel = new QLabel;
        m_timecodeLabel->setObjectName(QStringLiteral("transport.timecode"));
        m_timecodeLabel->setMinimumWidth(96);

        m_audioMuteButton = new QToolButton;
        m_audioMuteButton->setObjectName(QStringLiteral("transport.audio_mute"));
        m_audioMuteButton->setText(QStringLiteral("Mute"));
        m_audioVolumeSlider = new QSlider(Qt::Horizontal);
        m_audioVolumeSlider->setObjectName(QStringLiteral("transport.audio_volume"));
        m_audioVolumeSlider->setRange(0, 100);
        m_audioVolumeSlider->setValue(80);
        m_audioVolumeSlider->setFixedWidth(110);
        m_audioNowPlayingLabel = new QLabel(QStringLiteral("Audio idle"));
        m_audioNowPlayingLabel->setObjectName(QStringLiteral("transport.audio_status"));
        m_audioNowPlayingLabel->setMinimumWidth(180);
        
        fprintf(stderr, "[DEBUG] Adding transport widgets...\n");
        fflush(stderr);
        transportLayout->addWidget(startButton);
        transportLayout->addWidget(m_playButton);
        transportLayout->addWidget(pauseButton);
        transportLayout->addWidget(endButton);
        transportLayout->addWidget(m_seekSlider, 1);
        transportLayout->addWidget(m_timecodeLabel);
        transportLayout->addWidget(m_audioMuteButton);
        transportLayout->addWidget(m_audioVolumeSlider);
        transportLayout->addWidget(m_audioNowPlayingLabel);
        timelineLayout->addWidget(transport, 0);
        fprintf(stderr, "[DEBUG] Transport widgets added\n");
        fflush(stderr);
        
        fprintf(stderr, "[DEBUG] Creating TimelineWidget...\n");
        fflush(stderr);
        m_timeline = new TimelineWidget;
        fprintf(stderr, "[DEBUG] TimelineWidget created\n");
        fflush(stderr);
        m_timeline->setObjectName(QStringLiteral("timeline.widget"));
        timelineLayout->addWidget(m_timeline, 1);
        verticalSplitter->addWidget(timelinePane);
        verticalSplitter->setStretchFactor(0, 3);
        verticalSplitter->setStretchFactor(1, 2);
        verticalSplitter->setSizes({540, 320});
        
        connect(m_playButton, &QPushButton::clicked, this, [this]() {
            if (m_audioEngine && m_audioEngine->hasPlayableAudio()) {
                m_audioEngine->start(m_timeline->currentFrame());
            }
            m_playbackTimer.start();
            m_fastPlaybackActive.store(true);
            m_preview->setPlaybackState(true);
            refreshInspector();
            scheduleSaveState();
        });
        connect(pauseButton, &QPushButton::clicked, this, [this]() {
            if (m_audioEngine) {
                m_audioEngine->stop();
            }
            m_playbackTimer.stop();
            m_fastPlaybackActive.store(false);
            m_preview->setPlaybackState(false);
            refreshInspector();
            scheduleSaveState();
        });
        connect(startButton, &QToolButton::clicked, this, [this]() {
            setCurrentFrame(0);
        });
        connect(endButton, &QToolButton::clicked, this, [this]() {
            setCurrentFrame(m_timeline->totalFrames());
        });
        connect(m_seekSlider, &QSlider::valueChanged, this, [this](int value) {
            if (m_ignoreSeekSignal) return;
            setCurrentFrame(value);
        });
        connect(m_audioMuteButton, &QToolButton::clicked, this, [this]() {
            const bool nextMuted = !m_preview->audioMuted();
            m_preview->setAudioMuted(nextMuted);
            if (m_audioEngine) {
                m_audioEngine->setMuted(nextMuted);
            }
            refreshInspector();
            scheduleSaveState();
        });
        connect(m_audioVolumeSlider, &QSlider::valueChanged, this, [this](int value) {
            m_preview->setAudioVolume(value / 100.0);
            if (m_audioEngine) {
                m_audioEngine->setVolume(value / 100.0);
            }
            refreshInspector();
        });
        
        m_timeline->seekRequested = [this](int64_t frame) {
            setCurrentFrame(frame);
        };
        m_timeline->clipsChanged = [this]() {
            syncSliderRange();
            m_preview->setClipCount(m_timeline->clips().size());
            m_preview->setTimelineClips(m_timeline->clips());
            m_preview->setSelectedClipId(m_timeline->selectedClipId());
            if (m_audioEngine) {
                m_audioEngine->setTimelineClips(m_timeline->clips());
            }
            refreshInspector();
            scheduleSaveState();
            pushHistorySnapshot();
        };
        m_timeline->selectionChanged = [this]() {
            m_preview->setSelectedClipId(m_timeline->selectedClipId());
            refreshInspector();
        };
        m_timeline->renderSyncMarkersChanged = [this]() {
            refreshInspector();
            scheduleSaveState();
            pushHistorySnapshot();
        };
        m_timeline->gradingRequested = [this]() {
            focusGradingTab();
            refreshInspector();
        };
        m_preview->selectionRequested = [this](const QString& clipId) {
            if (!m_timeline) {
                return;
            }
            m_timeline->setSelectedClipId(clipId);
        };
        m_preview->resizeRequested = [this](const QString& clipId, qreal scaleX, qreal scaleY, bool finalize) {
            if (!m_timeline) {
                return;
            }
            const int64_t currentFrame = m_timeline->currentFrame();
            const bool updated = m_timeline->updateClipById(clipId, [this, currentFrame, scaleX, scaleY](TimelineClip& clip) {
                if (!clipHasVisuals(clip)) {
                    return;
                }
                const TimelineClip::TransformKeyframe offset = evaluateClipKeyframeOffsetAtFrame(clip, currentFrame);
                const qreal offsetScaleX = sanitizeScaleValue(offset.scaleX);
                const qreal offsetScaleY = sanitizeScaleValue(offset.scaleY);
                if (m_keyframeModeCheckBox && !m_keyframeModeCheckBox->isChecked()) {
                    clip.baseScaleX = sanitizeScaleValue(scaleX / offsetScaleX);
                    clip.baseScaleY = sanitizeScaleValue(scaleY / offsetScaleY);
                } else {
                    const int64_t keyframeFrame =
                        qBound<int64_t>(0, currentFrame - clip.startFrame, qMax<int64_t>(0, clip.durationFrames - 1));
                    TimelineClip::TransformKeyframe keyframe = offset;
                    keyframe.frame = keyframeFrame;
                    keyframe.scaleX = sanitizeScaleValue(scaleX / sanitizeScaleValue(clip.baseScaleX));
                    keyframe.scaleY = sanitizeScaleValue(scaleY / sanitizeScaleValue(clip.baseScaleY));
                    bool replaced = false;
                    for (TimelineClip::TransformKeyframe& existing : clip.transformKeyframes) {
                        if (existing.frame == keyframeFrame) {
                            existing = keyframe;
                            replaced = true;
                            break;
                        }
                    }
                    if (!replaced) {
                        clip.transformKeyframes.push_back(keyframe);
                    }
                }
                normalizeClipTransformKeyframes(clip);
            });
            if (!updated) {
                return;
            }
            m_preview->setTimelineClips(m_timeline->clips());
            refreshInspector();
            scheduleSaveState();
            if (finalize) {
                pushHistorySnapshot();
            }
        };
        m_preview->moveRequested = [this](const QString& clipId, qreal translationX, qreal translationY, bool finalize) {
            if (!m_timeline) {
                return;
            }
            const int64_t currentFrame = m_timeline->currentFrame();
            const bool updated = m_timeline->updateClipById(clipId, [this, currentFrame, translationX, translationY](TimelineClip& clip) {
                if (!clipHasVisuals(clip)) {
                    return;
                }
                const TimelineClip::TransformKeyframe offset = evaluateClipKeyframeOffsetAtFrame(clip, currentFrame);
                if (m_keyframeModeCheckBox && !m_keyframeModeCheckBox->isChecked()) {
                    clip.baseTranslationX = translationX - offset.translationX;
                    clip.baseTranslationY = translationY - offset.translationY;
                } else {
                    const int64_t keyframeFrame =
                        qBound<int64_t>(0, currentFrame - clip.startFrame, qMax<int64_t>(0, clip.durationFrames - 1));
                    TimelineClip::TransformKeyframe keyframe = offset;
                    keyframe.frame = keyframeFrame;
                    keyframe.translationX = translationX - clip.baseTranslationX;
                    keyframe.translationY = translationY - clip.baseTranslationY;
                    bool replaced = false;
                    for (TimelineClip::TransformKeyframe& existing : clip.transformKeyframes) {
                        if (existing.frame == keyframeFrame) {
                            existing = keyframe;
                            replaced = true;
                            break;
                        }
                    }
                    if (!replaced) {
                        clip.transformKeyframes.push_back(keyframe);
                    }
                }
                normalizeClipTransformKeyframes(clip);
            });
            if (!updated) {
                return;
            }
            m_preview->setTimelineClips(m_timeline->clips());
            refreshInspector();
            scheduleSaveState();
            if (finalize) {
                pushHistorySnapshot();
            }
        };
        fprintf(stderr, "[DEBUG] buildEditorPane() returning\n");
        fflush(stderr);
        return pane;
    }

    QWidget* buildInspectorPane() {
        auto* pane = new QFrame;
        pane->setFrameShape(QFrame::NoFrame);
        pane->setMinimumWidth(300);
        pane->setStyleSheet(
            QStringLiteral("QFrame { background: #11161c; color: #e8edf2; }"
                           "QLabel#inspectorTitle { color: #dce5ee; font-weight: 700; letter-spacing: 0.08em; }"
                           "QLabel#inspectorMeta { color: #91a3b4; }"
                           "QDoubleSpinBox, QSpinBox, QComboBox { background: #1b2430; border: 1px solid #2e3b4a; border-radius: 6px; padding: 6px; color: #edf2f7; }"
                           "QTabWidget::pane { border: 1px solid #222c36; border-radius: 8px; }"
                           "QTabBar::tab { background: #17202a; color: #f4f7fb; padding: 8px 12px; margin-right: 2px; }"
                           "QTabBar::tab:selected { background: #24303c; color: #ffffff; }"));

        auto* layout = new QVBoxLayout(pane);
        layout->setContentsMargins(14, 14, 14, 14);
        layout->setSpacing(10);

        m_inspectorTabs = new QTabWidget(pane);
        m_inspectorTabs->setObjectName(QStringLiteral("inspector.tabs"));

        auto* gradingTab = new QWidget(m_inspectorTabs);
        auto* gradingLayout = new QVBoxLayout(gradingTab);
        gradingLayout->setContentsMargins(12, 12, 12, 12);
        gradingLayout->setSpacing(10);

        m_gradingClipLabel = new QLabel(QStringLiteral("No clip selected"), gradingTab);
        m_gradingClipLabel->setObjectName(QStringLiteral("inspectorTitle"));
        m_gradingPathLabel = new QLabel(QStringLiteral("Select a clip in the timeline to grade it."), gradingTab);
        m_gradingPathLabel->setObjectName(QStringLiteral("inspectorMeta"));
        m_gradingPathLabel->setWordWrap(true);
        gradingLayout->addWidget(m_gradingClipLabel);
        gradingLayout->addWidget(m_gradingPathLabel);

        auto* form = new QFormLayout;
        m_brightnessSpin = new QDoubleSpinBox(gradingTab);
        m_brightnessSpin->setRange(-1.0, 1.0);
        m_brightnessSpin->setSingleStep(0.05);
        m_brightnessSpin->setDecimals(2);
        form->addRow(QStringLiteral("Brightness"), m_brightnessSpin);

        m_contrastSpin = new QDoubleSpinBox(gradingTab);
        m_contrastSpin->setRange(0.0, 3.0);
        m_contrastSpin->setSingleStep(0.05);
        m_contrastSpin->setDecimals(2);
        form->addRow(QStringLiteral("Contrast"), m_contrastSpin);

        m_saturationSpin = new QDoubleSpinBox(gradingTab);
        m_saturationSpin->setRange(0.0, 3.0);
        m_saturationSpin->setSingleStep(0.05);
        m_saturationSpin->setDecimals(2);
        form->addRow(QStringLiteral("Saturation"), m_saturationSpin);

        m_opacitySpin = new QDoubleSpinBox(gradingTab);
        m_opacitySpin->setRange(0.0, 1.0);
        m_opacitySpin->setSingleStep(0.05);
        m_opacitySpin->setDecimals(2);
        form->addRow(QStringLiteral("Opacity"), m_opacitySpin);
        gradingLayout->addLayout(form);
        m_bypassGradingCheckBox = new QCheckBox(QStringLiteral("Bypass Preview Grading"), gradingTab);
        gradingLayout->addWidget(m_bypassGradingCheckBox);
        gradingLayout->addStretch(1);

        m_inspectorTabs->addTab(gradingTab, QStringLiteral("Grading"));

        auto* videoTab = new QWidget(m_inspectorTabs);
        auto* videoLayout = new QVBoxLayout(videoTab);
        videoLayout->setContentsMargins(12, 12, 12, 12);
        videoLayout->setSpacing(10);
        m_videoInspectorClipLabel = new QLabel(QStringLiteral("Video output"), videoTab);
        m_videoInspectorClipLabel->setObjectName(QStringLiteral("inspectorTitle"));
        m_videoInspectorDetailsLabel = new QLabel(QStringLiteral("Configure output dimensions and format for rendering."), videoTab);
        m_videoInspectorDetailsLabel->setObjectName(QStringLiteral("inspectorMeta"));
        m_videoInspectorDetailsLabel->setWordWrap(true);
        videoLayout->addWidget(m_videoInspectorClipLabel);
        videoLayout->addWidget(m_videoInspectorDetailsLabel);

        auto* videoForm = new QFormLayout;
        m_outputWidthSpin = new QSpinBox(videoTab);
        m_outputWidthSpin->setRange(16, 7680);
        m_outputWidthSpin->setSingleStep(16);
        m_outputWidthSpin->setValue(1080);
        videoForm->addRow(QStringLiteral("Width"), m_outputWidthSpin);

        m_outputHeightSpin = new QSpinBox(videoTab);
        m_outputHeightSpin->setRange(16, 7680);
        m_outputHeightSpin->setSingleStep(16);
        m_outputHeightSpin->setValue(1920);
        videoForm->addRow(QStringLiteral("Height"), m_outputHeightSpin);

        m_outputFormatCombo = new QComboBox(videoTab);
        m_outputFormatCombo->addItem(QStringLiteral("MP4 (H.264)"), QStringLiteral("mp4"));
        m_outputFormatCombo->addItem(QStringLiteral("MOV (ProRes)"), QStringLiteral("mov"));
        m_outputFormatCombo->addItem(QStringLiteral("MKV (FFV1)"), QStringLiteral("mkv"));
        m_outputFormatCombo->addItem(QStringLiteral("WebM (VP9)"), QStringLiteral("webm"));
        videoForm->addRow(QStringLiteral("Output Format"), m_outputFormatCombo);

        m_videoTranslationXSpin = new QDoubleSpinBox(videoTab);
        m_videoTranslationXSpin->setRange(-4000.0, 4000.0);
        m_videoTranslationXSpin->setDecimals(1);
        m_videoTranslationXSpin->setSingleStep(10.0);
        videoForm->addRow(QStringLiteral("Translate X"), m_videoTranslationXSpin);

        m_videoTranslationYSpin = new QDoubleSpinBox(videoTab);
        m_videoTranslationYSpin->setRange(-4000.0, 4000.0);
        m_videoTranslationYSpin->setDecimals(1);
        m_videoTranslationYSpin->setSingleStep(10.0);
        videoForm->addRow(QStringLiteral("Translate Y"), m_videoTranslationYSpin);

        m_videoRotationSpin = new QDoubleSpinBox(videoTab);
        m_videoRotationSpin->setRange(-3600.0, 3600.0);
        m_videoRotationSpin->setDecimals(1);
        m_videoRotationSpin->setSingleStep(5.0);
        videoForm->addRow(QStringLiteral("Rotation"), m_videoRotationSpin);

        m_videoScaleXSpin = new QDoubleSpinBox(videoTab);
        m_videoScaleXSpin->setRange(-20.0, 20.0);
        m_videoScaleXSpin->setDecimals(3);
        m_videoScaleXSpin->setSingleStep(0.05);
        m_videoScaleXSpin->setValue(1.0);
        videoForm->addRow(QStringLiteral("Scale X"), m_videoScaleXSpin);

        m_videoScaleYSpin = new QDoubleSpinBox(videoTab);
        m_videoScaleYSpin->setRange(-20.0, 20.0);
        m_videoScaleYSpin->setDecimals(3);
        m_videoScaleYSpin->setSingleStep(0.05);
        m_videoScaleYSpin->setValue(1.0);
        videoForm->addRow(QStringLiteral("Scale Y"), m_videoScaleYSpin);

        m_videoInterpolationCombo = new QComboBox(videoTab);
        m_videoInterpolationCombo->addItem(QStringLiteral("Interpolated"));
        m_videoInterpolationCombo->addItem(QStringLiteral("Sudden"));
        videoForm->addRow(QStringLiteral("To This Keyframe"), m_videoInterpolationCombo);
        videoLayout->addLayout(videoForm);

        m_addVideoKeyframeButton = new QPushButton(QStringLiteral("Add/Update Keyframe"), videoTab);
        videoLayout->addWidget(m_addVideoKeyframeButton, 0, Qt::AlignLeft);

        m_renderButton = new QPushButton(QStringLiteral("Render"), videoTab);
        m_renderButton->setObjectName(QStringLiteral("video.render"));
        videoLayout->addWidget(m_renderButton, 0, Qt::AlignLeft);
        videoLayout->addStretch(1);
        m_inspectorTabs->addTab(videoTab, QStringLiteral("Video"));

        auto* keyframesTab = new QWidget(m_inspectorTabs);
        auto* keyframesLayout = new QVBoxLayout(keyframesTab);
        keyframesLayout->setContentsMargins(12, 12, 12, 12);
        keyframesLayout->setSpacing(10);
        m_keyframesInspectorClipLabel = new QLabel(QStringLiteral("No visual clip selected"), keyframesTab);
        m_keyframesInspectorClipLabel->setObjectName(QStringLiteral("inspectorTitle"));
        m_keyframesInspectorDetailsLabel = new QLabel(QStringLiteral("Select a visual clip to inspect its keyframes."), keyframesTab);
        m_keyframesInspectorDetailsLabel->setObjectName(QStringLiteral("inspectorMeta"));
        m_keyframesInspectorDetailsLabel->setWordWrap(true);
        keyframesLayout->addWidget(m_keyframesInspectorClipLabel);
        keyframesLayout->addWidget(m_keyframesInspectorDetailsLabel);
        m_keyframeModeCheckBox = new QCheckBox(QStringLiteral("Keyframe Mode"), keyframesTab);
        m_keyframeModeCheckBox->setChecked(true);
        keyframesLayout->addWidget(m_keyframeModeCheckBox);
        m_videoKeyframeList = new QListWidget(keyframesTab);
        m_videoKeyframeList->setSelectionMode(QAbstractItemView::ExtendedSelection);
        keyframesLayout->addWidget(m_videoKeyframeList, 1);
        m_removeVideoKeyframeButton = new QPushButton(QStringLiteral("Delete Keyframe"), keyframesTab);
        keyframesLayout->addWidget(m_removeVideoKeyframeButton, 0, Qt::AlignLeft);
        m_inspectorTabs->addTab(keyframesTab, QStringLiteral("Keyframes"));

        auto* audioTab = new QWidget(m_inspectorTabs);
        auto* audioLayout = new QVBoxLayout(audioTab);
        audioLayout->setContentsMargins(12, 12, 12, 12);
        audioLayout->setSpacing(10);
        m_audioInspectorClipLabel = new QLabel(QStringLiteral("No audio clip selected"), audioTab);
        m_audioInspectorClipLabel->setObjectName(QStringLiteral("inspectorTitle"));
        m_audioInspectorDetailsLabel = new QLabel(QStringLiteral("Select an audio clip to inspect playback details."), audioTab);
        m_audioInspectorDetailsLabel->setObjectName(QStringLiteral("inspectorMeta"));
        m_audioInspectorDetailsLabel->setWordWrap(true);
        audioLayout->addWidget(m_audioInspectorClipLabel);
        audioLayout->addWidget(m_audioInspectorDetailsLabel);
        audioLayout->addStretch(1);
        m_inspectorTabs->addTab(audioTab, QStringLiteral("Audio"));
        layout->addWidget(m_inspectorTabs, 1);

        connect(m_outputWidthSpin, qOverload<int>(&QSpinBox::valueChanged), this, [this](int) {
            if (m_preview) {
                m_preview->setOutputSize(QSize(m_outputWidthSpin->value(),
                                               m_outputHeightSpin ? m_outputHeightSpin->value() : 1920));
            }
            scheduleSaveState();
            refreshInspector();
        });
        connect(m_outputHeightSpin, qOverload<int>(&QSpinBox::valueChanged), this, [this](int) {
            if (m_preview) {
                m_preview->setOutputSize(QSize(m_outputWidthSpin ? m_outputWidthSpin->value() : 1080,
                                               m_outputHeightSpin->value()));
            }
            scheduleSaveState();
            refreshInspector();
        });
        connect(m_outputFormatCombo, &QComboBox::currentIndexChanged, this, [this](int) {
            scheduleSaveState();
            refreshInspector();
        });
        connect(m_bypassGradingCheckBox, &QCheckBox::toggled, this, [this](bool checked) {
            if (m_preview) {
                m_preview->setBypassGrading(checked);
            }
            refreshInspector();
        });
        connect(m_videoKeyframeList, &QListWidget::itemSelectionChanged, this, [this]() {
            if (m_updatingVideoInspector) {
                return;
            }
            m_selectedVideoKeyframeFrames.clear();
            const QList<QListWidgetItem*> selectedItems = m_videoKeyframeList->selectedItems();
            for (QListWidgetItem* item : selectedItems) {
                if (item) {
                    m_selectedVideoKeyframeFrames.insert(item->data(Qt::UserRole).toLongLong());
                }
            }

            QListWidgetItem* currentItem = m_videoKeyframeList->currentItem();
            if (currentItem && m_selectedVideoKeyframeFrames.contains(currentItem->data(Qt::UserRole).toLongLong())) {
                m_selectedVideoKeyframeFrame = currentItem->data(Qt::UserRole).toLongLong();
            } else if (!selectedItems.isEmpty()) {
                m_selectedVideoKeyframeFrame = selectedItems.constFirst()->data(Qt::UserRole).toLongLong();
            } else {
                m_selectedVideoKeyframeFrame = -1;
            }
            refreshInspector();
        });
        connect(m_videoKeyframeList, &QListWidget::currentItemChanged, this, [this](QListWidgetItem* current, QListWidgetItem*) {
            if (m_updatingVideoInspector) {
                return;
            }
            if (!current) {
                return;
            }
            const int64_t frame = current->data(Qt::UserRole).toLongLong();
            if (!m_selectedVideoKeyframeFrames.contains(frame)) {
                return;
            }
            m_selectedVideoKeyframeFrame = frame;
            refreshInspector();
        });
        connect(m_keyframeModeCheckBox, &QCheckBox::toggled, this, [this](bool) {
            refreshInspector();
        });
        connect(m_addVideoKeyframeButton, &QPushButton::clicked, this, [this]() {
            upsertSelectedClipVideoKeyframe();
        });
        connect(m_removeVideoKeyframeButton, &QPushButton::clicked, this, [this]() {
            removeSelectedClipVideoKeyframe();
        });
        connect(m_renderButton, &QPushButton::clicked, this, [this]() {
            const QString extension =
                m_outputFormatCombo ? m_outputFormatCombo->currentData().toString() : QStringLiteral("mp4");
            const QString selectedPath = QFileDialog::getSaveFileName(
                this,
                QStringLiteral("Render Output"),
                QDir(m_currentRootPath.isEmpty() ? QDir::currentPath() : m_currentRootPath)
                    .filePath(QStringLiteral("render_output.%1").arg(extension)),
                QStringLiteral("Video Files (*.%1)").arg(extension));
            if (selectedPath.isEmpty()) {
                return;
            }
            QString renderSyncSummary = QStringLiteral("none");
            if (m_timeline && !m_timeline->renderSyncMarkers().isEmpty()) {
                QStringList items;
                for (const RenderSyncMarker& marker : m_timeline->renderSyncMarkers()) {
                    items.append(QStringLiteral("%1 @ %2")
                                     .arg(renderSyncActionLabel(marker.action))
                                     .arg(frameToTimecode(marker.frame)));
                }
                renderSyncSummary = items.join(QStringLiteral(", "));
            }
            QMessageBox::information(
                this,
                QStringLiteral("Render Queued"),
                QStringLiteral("Render settings\n\nOutput: %1\nResolution: %2x%3\nFormat: %4\nTimeline clips: %5\nRender sync marks: %6")
                    .arg(QDir::toNativeSeparators(selectedPath))
                    .arg(m_outputWidthSpin ? m_outputWidthSpin->value() : 1080)
                    .arg(m_outputHeightSpin ? m_outputHeightSpin->value() : 1920)
                    .arg(m_outputFormatCombo ? m_outputFormatCombo->currentText() : QStringLiteral("MP4 (H.264)"))
                    .arg(m_timeline ? m_timeline->clips().size() : 0)
                    .arg(renderSyncSummary));
        });
        return pane;
    }
    
    void addFileToTimeline(const QString& filePath) {
        if (m_timeline) {
            m_timeline->addClipFromFile(filePath);
            m_preview->setTimelineClips(m_timeline->clips());
        }
    }
    
    void syncSliderRange() {
        const int64_t maxFrame = m_timeline->totalFrames();
        m_seekSlider->setRange(0, static_cast<int>(qMin<int64_t>(maxFrame, INT_MAX)));
    }

    void focusGradingTab() {
        if (m_inspectorTabs) {
            m_inspectorTabs->setCurrentIndex(0);
        }
    }

    int selectedVideoKeyframeIndex(const TimelineClip& clip) const {
        for (int i = 0; i < clip.transformKeyframes.size(); ++i) {
            if (clip.transformKeyframes[i].frame == m_selectedVideoKeyframeFrame) {
                return i;
            }
        }
        return -1;
    }

    QList<int64_t> selectedVideoKeyframeFrames(const TimelineClip& clip) const {
        QList<int64_t> frames;
        for (const TimelineClip::TransformKeyframe& keyframe : clip.transformKeyframes) {
            if (m_selectedVideoKeyframeFrames.contains(keyframe.frame)) {
                frames.push_back(keyframe.frame);
            }
        }
        return frames;
    }

    int nearestVideoKeyframeIndex(const TimelineClip& clip) const {
        if (!m_timeline || clip.transformKeyframes.isEmpty()) {
            return -1;
        }
        const int64_t localFrame =
            qBound<int64_t>(0, m_timeline->currentFrame() - clip.startFrame, qMax<int64_t>(0, clip.durationFrames - 1));
        int nearestIndex = 0;
        int64_t nearestDistance = std::numeric_limits<int64_t>::max();
        for (int i = 0; i < clip.transformKeyframes.size(); ++i) {
            const int64_t distance = std::abs(clip.transformKeyframes[i].frame - localFrame);
            if (distance < nearestDistance) {
                nearestDistance = distance;
                nearestIndex = i;
            }
        }
        return nearestIndex;
    }

    void applySelectedVideoKeyframeFromInspector(bool pushHistoryAfterChange = false) {
        if (m_updatingVideoInspector || !m_timeline) {
            return;
        }

        const QString clipId = m_timeline->selectedClipId();
        if (clipId.isEmpty() || m_selectedVideoKeyframeFrame < 0) {
            return;
        }

        const bool updated = m_timeline->updateClipById(clipId, [this](TimelineClip& clip) {
            const int index = selectedVideoKeyframeIndex(clip);
            if (index < 0) {
                return;
            }
            TimelineClip::TransformKeyframe& keyframe = clip.transformKeyframes[index];
            keyframe.translationX = m_videoTranslationXSpin->value();
            keyframe.translationY = m_videoTranslationYSpin->value();
            keyframe.rotation = m_videoRotationSpin->value();
            keyframe.scaleX = m_videoScaleXSpin->value();
            keyframe.scaleY = m_videoScaleYSpin->value();
            keyframe.interpolated = m_videoInterpolationCombo->currentIndex() == 0;
            normalizeClipTransformKeyframes(clip);
        });

        if (!updated) {
            return;
        }

        m_preview->setTimelineClips(m_timeline->clips());
        refreshInspector();
        scheduleSaveState();
        if (pushHistoryAfterChange) {
            pushHistorySnapshot();
        }
    }

    void upsertSelectedClipVideoKeyframe() {
        if (!m_timeline) {
            return;
        }
        const TimelineClip* clip = m_timeline->selectedClip();
        if (!clip || !clipHasVisuals(*clip)) {
            return;
        }

        const int64_t keyframeFrame =
            qBound<int64_t>(0, m_timeline->currentFrame() - clip->startFrame, qMax<int64_t>(0, clip->durationFrames - 1));
        m_selectedVideoKeyframeFrame = keyframeFrame;
        m_selectedVideoKeyframeFrames = {keyframeFrame};

        const bool updated = m_timeline->updateClipById(clip->id, [this, keyframeFrame](TimelineClip& editableClip) {
            TimelineClip::TransformKeyframe keyframe;
            keyframe.frame = keyframeFrame;
            keyframe.translationX = m_videoTranslationXSpin->value();
            keyframe.translationY = m_videoTranslationYSpin->value();
            keyframe.rotation = m_videoRotationSpin->value();
            keyframe.scaleX = m_videoScaleXSpin->value();
            keyframe.scaleY = m_videoScaleYSpin->value();
            keyframe.interpolated = m_videoInterpolationCombo->currentIndex() == 0;

            bool replaced = false;
            for (TimelineClip::TransformKeyframe& existing : editableClip.transformKeyframes) {
                if (existing.frame == keyframeFrame) {
                    existing = keyframe;
                    replaced = true;
                    break;
                }
            }
            if (!replaced) {
                editableClip.transformKeyframes.push_back(keyframe);
            }
            normalizeClipTransformKeyframes(editableClip);
        });

        if (!updated) {
            return;
        }

        m_preview->setTimelineClips(m_timeline->clips());
        refreshInspector();
        scheduleSaveState();
        pushHistorySnapshot();
    }

    void removeSelectedClipVideoKeyframe() {
        if (!m_timeline || m_selectedVideoKeyframeFrames.isEmpty()) {
            return;
        }
        const QString clipId = m_timeline->selectedClipId();
        if (clipId.isEmpty()) {
            return;
        }

        const TimelineClip* selectedClip = m_timeline->selectedClip();
        if (!selectedClip) {
            return;
        }
        const QList<int64_t> selectedFrames = selectedVideoKeyframeFrames(*selectedClip);
        if (selectedFrames.isEmpty()) {
            return;
        }

        const bool updated = m_timeline->updateClipById(clipId, [selectedFrames](TimelineClip& clip) {
            clip.transformKeyframes.erase(
                std::remove_if(clip.transformKeyframes.begin(),
                               clip.transformKeyframes.end(),
                               [&selectedFrames](const TimelineClip::TransformKeyframe& keyframe) {
                                   return selectedFrames.contains(keyframe.frame);
                               }),
                clip.transformKeyframes.end());
            normalizeClipTransformKeyframes(clip);
        });

        if (!updated) {
            return;
        }

        m_selectedVideoKeyframeFrame = -1;
        m_selectedVideoKeyframeFrames.clear();
        m_preview->setTimelineClips(m_timeline->clips());
        refreshInspector();
        scheduleSaveState();
        pushHistorySnapshot();
    }

    void applySelectedClipGradeFromInspector(bool pushHistoryAfterChange = false) {
        if (!m_timeline) {
            return;
        }
        const QString clipId = m_timeline->selectedClipId();
        if (clipId.isEmpty()) {
            return;
        }

        const bool updated = m_timeline->updateClipById(clipId, [this](TimelineClip& clip) {
            clip.brightness = m_brightnessSpin->value();
            clip.contrast = m_contrastSpin->value();
            clip.saturation = m_saturationSpin->value();
            clip.opacity = m_opacitySpin->value();
        });

        if (!updated) {
            return;
        }

        m_preview->setTimelineClips(m_timeline->clips());
        refreshInspector();
        scheduleSaveState();
        if (pushHistoryAfterChange) {
            pushHistorySnapshot();
        }
    }
    
    void setCurrentFrame(int64_t frame, bool syncAudio = true) {
        const int64_t bounded = qBound<int64_t>(0, frame, m_timeline->totalFrames());
        playbackTrace(QStringLiteral("EditorWindow::setCurrentFrame"),
                      QStringLiteral("requested=%1 bounded=%2").arg(frame).arg(bounded));
        if (!m_timeline || bounded != m_timeline->currentFrame()) {
            m_lastPlayheadAdvanceMs.store(nowMs());
        }
        m_fastCurrentFrame.store(bounded);
        if (syncAudio && m_audioEngine && m_audioEngine->hasPlayableAudio()) {
            m_audioEngine->seek(bounded);
        }
        m_timeline->setCurrentFrame(bounded);
        m_preview->setCurrentFrame(bounded);
        
        m_ignoreSeekSignal = true;
        m_seekSlider->setValue(static_cast<int>(qMin<int64_t>(bounded, INT_MAX)));
        m_ignoreSeekSignal = false;
        
        m_timecodeLabel->setText(frameToTimecode(bounded));
        refreshInspector();
        scheduleSaveState();
    }
    
    QString frameToTimecode(int64_t frame) const {
        const int fps = 30;
        const int64_t totalSeconds = frame / fps;
        const int64_t minutes = totalSeconds / 60;
        const int64_t seconds = totalSeconds % 60;
        const int64_t frames = frame % fps;
        
        return QStringLiteral("%1:%2:%3")
            .arg(minutes, 2, 10, QLatin1Char('0'))
            .arg(seconds, 2, 10, QLatin1Char('0'))
            .arg(frames, 2, 10, QLatin1Char('0'));
    }
    
    void refreshInspector() {
        const bool playing = m_playbackTimer.isActive();
        const QString state = playing ? QStringLiteral("PLAYING") : QStringLiteral("PAUSED");
        const int clipCount = m_timeline ? m_timeline->clips().size() : 0;
        const QString activeAudio = m_preview ? m_preview->activeAudioClipLabel() : QString();
        
        m_statusBadge->setText(QStringLiteral("%1  |  %2 clips").arg(state).arg(clipCount));
        if (m_preview && m_timeline) {
            m_preview->setSelectedClipId(m_timeline->selectedClipId());
        }
        m_previewInfo->setText(QStringLiteral("Professional pipeline with libavcodec\nBackend: %1\nSeek: %2\nAudio: %3\nGrading: %4")
                                   .arg(m_preview ? m_preview->backendName() : QStringLiteral("unknown"))
                                   .arg(frameToTimecode(m_timeline ? m_timeline->currentFrame() : 0))
                                   .arg(activeAudio.isEmpty() ? QStringLiteral("idle") : activeAudio)
                                   .arg(m_preview && m_preview->bypassGrading() ? QStringLiteral("bypassed") : QStringLiteral("on")));
        m_playButton->setText(playing ? QStringLiteral("Playing") : QStringLiteral("Play"));
        m_audioMuteButton->setText(m_preview && m_preview->audioMuted()
                                       ? QStringLiteral("Unmute")
                                       : QStringLiteral("Mute"));
        m_audioNowPlayingLabel->setText(activeAudio.isEmpty()
                                            ? QStringLiteral("Audio idle")
                                            : QStringLiteral("Audio  %1").arg(activeAudio));
        if (m_bypassGradingCheckBox) {
            QSignalBlocker block(m_bypassGradingCheckBox);
            m_bypassGradingCheckBox->setChecked(m_preview && m_preview->bypassGrading());
        }

        const TimelineClip* clip = m_timeline ? m_timeline->selectedClip() : nullptr;
        const bool enabled = clip != nullptr && clipHasVisuals(*clip);
        m_brightnessSpin->setEnabled(enabled);
        m_contrastSpin->setEnabled(enabled);
        m_saturationSpin->setEnabled(enabled);
        m_opacitySpin->setEnabled(enabled);
        m_videoTranslationXSpin->setEnabled(enabled);
        m_videoTranslationYSpin->setEnabled(enabled);
        m_videoRotationSpin->setEnabled(enabled);
        m_videoScaleXSpin->setEnabled(enabled);
        m_videoScaleYSpin->setEnabled(enabled);
        m_videoInterpolationCombo->setEnabled(enabled);
        m_addVideoKeyframeButton->setEnabled(enabled);
        m_videoKeyframeList->setEnabled(enabled);

        if (!clip) {
            m_gradingClipLabel->setText(QStringLiteral("No clip selected"));
            m_gradingPathLabel->setText(QStringLiteral("Select a clip in the timeline to grade it live."));
            m_videoInspectorClipLabel->setText(QStringLiteral("Video output"));
            m_videoInspectorDetailsLabel->setText(
                QStringLiteral("Render %1 clips at %2x%3 as %4.")
                    .arg(m_timeline ? m_timeline->clips().size() : 0)
                    .arg(m_outputWidthSpin ? m_outputWidthSpin->value() : 1080)
                    .arg(m_outputHeightSpin ? m_outputHeightSpin->value() : 1920)
                    .arg(m_outputFormatCombo ? m_outputFormatCombo->currentText() : QStringLiteral("MP4 (H.264)")));
            m_keyframesInspectorClipLabel->setText(QStringLiteral("No visual clip selected"));
            m_keyframesInspectorDetailsLabel->setText(QStringLiteral("Select a visual clip to inspect its keyframes."));
            m_keyframeModeCheckBox->setEnabled(false);
            m_audioInspectorClipLabel->setText(QStringLiteral("No audio clip selected"));
            m_audioInspectorDetailsLabel->setText(QStringLiteral("Select an audio clip to inspect playback details."));
            m_updatingVideoInspector = true;
            m_videoKeyframeList->clear();
            m_removeVideoKeyframeButton->setEnabled(false);
            {
                QSignalBlocker videoBlock1(m_videoTranslationXSpin);
                QSignalBlocker videoBlock2(m_videoTranslationYSpin);
                QSignalBlocker videoBlock3(m_videoRotationSpin);
                QSignalBlocker videoBlock4(m_videoScaleXSpin);
                QSignalBlocker videoBlock5(m_videoScaleYSpin);
                QSignalBlocker videoBlock6(m_videoInterpolationCombo);
                m_videoTranslationXSpin->setValue(0.0);
                m_videoTranslationYSpin->setValue(0.0);
                m_videoRotationSpin->setValue(0.0);
                m_videoScaleXSpin->setValue(1.0);
                m_videoScaleYSpin->setValue(1.0);
                m_videoInterpolationCombo->setCurrentIndex(0);
            }
            m_updatingVideoInspector = false;
            m_selectedVideoKeyframeFrame = -1;
            m_selectedVideoKeyframeFrames.clear();
            {
                QSignalBlocker block1(m_brightnessSpin);
                QSignalBlocker block2(m_contrastSpin);
                QSignalBlocker block3(m_saturationSpin);
                QSignalBlocker block4(m_opacitySpin);
                m_brightnessSpin->setValue(0.0);
                m_contrastSpin->setValue(1.0);
                m_saturationSpin->setValue(1.0);
                m_opacitySpin->setValue(1.0);
            }
            return;
        }

        m_gradingClipLabel->setText(QStringLiteral("Track %1  %2").arg(clip->trackIndex + 1).arg(clip->label));
        m_videoInspectorClipLabel->setText(QStringLiteral("Output  %1x%2")
                                               .arg(m_outputWidthSpin ? m_outputWidthSpin->value() : 1080)
                                               .arg(m_outputHeightSpin ? m_outputHeightSpin->value() : 1920));
        m_videoInspectorDetailsLabel->setText(
            QStringLiteral("Selected clip: %1\nOutput format: %2\nTimeline length: %3")
                .arg(clip->label)
                .arg(m_outputFormatCombo ? m_outputFormatCombo->currentText() : QStringLiteral("MP4 (H.264)"))
                .arg(frameToTimecode(m_timeline ? m_timeline->totalFrames() : 0)));
        m_keyframesInspectorClipLabel->setText(QStringLiteral("Keyframes  %1").arg(clip->label));
        m_keyframeModeCheckBox->setEnabled(enabled);
        m_updatingVideoInspector = true;
        m_videoKeyframeList->clear();
        if (clip->transformKeyframes.isEmpty()) {
            m_keyframesInspectorDetailsLabel->setText(
                QStringLiteral("Mode: %1\nNo keyframes yet at this clip.\nBase scale: %2 x %3")
                    .arg(m_keyframeModeCheckBox->isChecked() ? QStringLiteral("Keyframing") : QStringLiteral("Base Sizing"))
                    .arg(clip->baseScaleX, 0, 'f', 3)
                    .arg(clip->baseScaleY, 0, 'f', 3));
            m_selectedVideoKeyframeFrame = -1;
            m_selectedVideoKeyframeFrames.clear();
        } else {
            const int nearestIndex = nearestVideoKeyframeIndex(*clip);
            QSet<int64_t> validSelectedFrames;
            for (const TimelineClip::TransformKeyframe& keyframe : clip->transformKeyframes) {
                if (m_selectedVideoKeyframeFrames.contains(keyframe.frame)) {
                    validSelectedFrames.insert(keyframe.frame);
                }
            }
            m_selectedVideoKeyframeFrames = validSelectedFrames;
            if (selectedVideoKeyframeIndex(*clip) < 0 && nearestIndex >= 0) {
                m_selectedVideoKeyframeFrame = clip->transformKeyframes[nearestIndex].frame;
            }
            if (m_selectedVideoKeyframeFrame >= 0 && m_selectedVideoKeyframeFrames.isEmpty()) {
                m_selectedVideoKeyframeFrames.insert(m_selectedVideoKeyframeFrame);
            }
            const int64_t localFrame =
                m_timeline
                    ? qBound<int64_t>(0, m_timeline->currentFrame() - clip->startFrame, qMax<int64_t>(0, clip->durationFrames - 1))
                    : 0;
            m_keyframesInspectorDetailsLabel->setText(
                QStringLiteral("Mode: %1\nCurrent clip frame: %2\nNearest keyframe: %3\nBase scale: %4 x %5")
                    .arg(m_keyframeModeCheckBox->isChecked() ? QStringLiteral("Keyframing") : QStringLiteral("Base Sizing"))
                    .arg(frameToTimecode(localFrame))
                    .arg(nearestIndex >= 0 ? frameToTimecode(clip->transformKeyframes[nearestIndex].frame)
                                           : QStringLiteral("none"))
                    .arg(clip->baseScaleX, 0, 'f', 3)
                    .arg(clip->baseScaleY, 0, 'f', 3));
            for (int i = 0; i < clip->transformKeyframes.size(); ++i) {
                const TimelineClip::TransformKeyframe& keyframe = clip->transformKeyframes[i];
                const bool nearest = i == nearestIndex;
                const QString label =
                    QStringLiteral("%1%2  |  X %3 Y %4  |  R %5  |  SX %6 SY %7  |  %8")
                        .arg(nearest ? QStringLiteral("* ") : QString())
                        .arg(frameToTimecode(keyframe.frame))
                        .arg(keyframe.translationX, 0, 'f', 1)
                        .arg(keyframe.translationY, 0, 'f', 1)
                        .arg(keyframe.rotation, 0, 'f', 1)
                        .arg(keyframe.scaleX, 0, 'f', 3)
                        .arg(keyframe.scaleY, 0, 'f', 3)
                        .arg(transformInterpolationLabel(keyframe.interpolated));
                auto* item = new QListWidgetItem(label, m_videoKeyframeList);
                item->setData(Qt::UserRole, QVariant::fromValue(static_cast<qint64>(keyframe.frame)));
                item->setSelected(m_selectedVideoKeyframeFrames.contains(keyframe.frame));
                if (nearest) {
                    item->setBackground(QColor(QStringLiteral("#2a3948")));
                    item->setForeground(QColor(QStringLiteral("#ffffff")));
                }
            }
        }
        int selectedRow = -1;
        for (int i = 0; i < m_videoKeyframeList->count(); ++i) {
            QListWidgetItem* item = m_videoKeyframeList->item(i);
            if (item && item->data(Qt::UserRole).toLongLong() == m_selectedVideoKeyframeFrame) {
                selectedRow = i;
                break;
            }
        }
        if (selectedRow >= 0) {
            m_videoKeyframeList->setCurrentRow(selectedRow);
        }
        const int keyframeIndex = selectedVideoKeyframeIndex(*clip);
        m_removeVideoKeyframeButton->setEnabled(enabled && !m_selectedVideoKeyframeFrames.isEmpty());
        {
            QSignalBlocker videoBlock1(m_videoTranslationXSpin);
            QSignalBlocker videoBlock2(m_videoTranslationYSpin);
            QSignalBlocker videoBlock3(m_videoRotationSpin);
            QSignalBlocker videoBlock4(m_videoScaleXSpin);
            QSignalBlocker videoBlock5(m_videoScaleYSpin);
            QSignalBlocker videoBlock6(m_videoInterpolationCombo);
            if (keyframeIndex >= 0) {
                const TimelineClip::TransformKeyframe& keyframe = clip->transformKeyframes[keyframeIndex];
                m_videoTranslationXSpin->setValue(keyframe.translationX);
                m_videoTranslationYSpin->setValue(keyframe.translationY);
                m_videoRotationSpin->setValue(keyframe.rotation);
                m_videoScaleXSpin->setValue(keyframe.scaleX);
                m_videoScaleYSpin->setValue(keyframe.scaleY);
                m_videoInterpolationCombo->setCurrentIndex(keyframe.interpolated ? 0 : 1);
            } else {
                m_videoTranslationXSpin->setValue(0.0);
                m_videoTranslationYSpin->setValue(0.0);
                m_videoRotationSpin->setValue(0.0);
                m_videoScaleXSpin->setValue(1.0);
                m_videoScaleYSpin->setValue(1.0);
                m_videoInterpolationCombo->setCurrentIndex(0);
            }
        }
        m_updatingVideoInspector = false;
        if (clipHasVisuals(*clip)) {
            m_gradingPathLabel->setText(QDir::toNativeSeparators(clip->filePath));
        } else {
            m_gradingPathLabel->setText(QStringLiteral("Audio clips do not use visual grading controls."));
        }
        if (clipIsAudioOnly(*clip)) {
            m_audioInspectorClipLabel->setText(QStringLiteral("Track %1  %2").arg(clip->trackIndex + 1).arg(clip->label));
            m_audioInspectorDetailsLabel->setText(
                QStringLiteral("Path: %1\nDuration: %2\nTransport volume: %3%%\nMuted: %4")
                    .arg(QDir::toNativeSeparators(clip->filePath))
                    .arg(frameToTimecode(clip->durationFrames))
                    .arg(m_preview ? m_preview->audioVolumePercent() : 0)
                    .arg(m_preview && m_preview->audioMuted() ? QStringLiteral("yes") : QStringLiteral("no")));
        } else {
            m_audioInspectorClipLabel->setText(QStringLiteral("No audio clip selected"));
            m_audioInspectorDetailsLabel->setText(QStringLiteral("Select an audio clip to inspect playback details."));
        }
        {
            QSignalBlocker block1(m_brightnessSpin);
            QSignalBlocker block2(m_contrastSpin);
            QSignalBlocker block3(m_saturationSpin);
            QSignalBlocker block4(m_opacitySpin);
            m_brightnessSpin->setValue(clip->brightness);
            m_contrastSpin->setValue(clip->contrast);
            m_saturationSpin->setValue(clip->saturation);
            m_opacitySpin->setValue(clip->opacity);
        }
    }

    QJsonObject profilingSnapshot() const {
        const qint64 now = nowMs();
        QJsonObject snapshot{
            {QStringLiteral("playback_active"), m_playbackTimer.isActive()},
            {QStringLiteral("timeline_clip_count"), m_timeline ? m_timeline->clips().size() : 0},
            {QStringLiteral("current_frame"), m_timeline ? static_cast<qint64>(m_timeline->currentFrame()) : 0},
            {QStringLiteral("explorer_root"), m_currentRootPath},
            {QStringLiteral("main_thread_heartbeat_ms"), m_lastMainThreadHeartbeatMs.load()},
            {QStringLiteral("last_playhead_advance_ms"), m_lastPlayheadAdvanceMs.load()},
            {QStringLiteral("main_thread_heartbeat_age_ms"), m_lastMainThreadHeartbeatMs.load() > 0 ? now - m_lastMainThreadHeartbeatMs.load() : -1},
            {QStringLiteral("last_playhead_advance_age_ms"), m_lastPlayheadAdvanceMs.load() > 0 ? now - m_lastPlayheadAdvanceMs.load() : -1}
        };

        if (m_preview) {
            snapshot[QStringLiteral("preview")] = m_preview->profilingSnapshot();
        }

        return snapshot;
    }
    
    QFileSystemModel* m_fsModel = nullptr;
    QTreeView* m_tree = nullptr;
    QStackedWidget* m_explorerStack = nullptr;
    QListWidget* m_galleryList = nullptr;
    QLabel* m_explorerHoverPreview = nullptr;
    QPushButton* m_folderPickerButton = nullptr;
    QToolButton* m_refreshExplorerButton = nullptr;
    QToolButton* m_galleryBackButton = nullptr;
    QLabel* m_rootPathLabel = nullptr;
    QLabel* m_galleryTitleLabel = nullptr;
    PreviewWindow* m_preview = nullptr;
    TimelineWidget* m_timeline = nullptr;
    QPushButton* m_playButton = nullptr;
    QSlider* m_seekSlider = nullptr;
    QLabel* m_timecodeLabel = nullptr;
    QToolButton* m_audioMuteButton = nullptr;
    QSlider* m_audioVolumeSlider = nullptr;
    QLabel* m_audioNowPlayingLabel = nullptr;
    QLabel* m_statusBadge = nullptr;
    QLabel* m_previewInfo = nullptr;
    QTabWidget* m_inspectorTabs = nullptr;
    QLabel* m_gradingClipLabel = nullptr;
    QLabel* m_gradingPathLabel = nullptr;
    QLabel* m_videoInspectorClipLabel = nullptr;
    QLabel* m_videoInspectorDetailsLabel = nullptr;
    QLabel* m_keyframesInspectorClipLabel = nullptr;
    QLabel* m_keyframesInspectorDetailsLabel = nullptr;
    QLabel* m_audioInspectorClipLabel = nullptr;
    QLabel* m_audioInspectorDetailsLabel = nullptr;
    QDoubleSpinBox* m_brightnessSpin = nullptr;
    QDoubleSpinBox* m_contrastSpin = nullptr;
    QDoubleSpinBox* m_saturationSpin = nullptr;
    QDoubleSpinBox* m_opacitySpin = nullptr;
    QCheckBox* m_bypassGradingCheckBox = nullptr;
    QSpinBox* m_outputWidthSpin = nullptr;
    QSpinBox* m_outputHeightSpin = nullptr;
    QComboBox* m_outputFormatCombo = nullptr;
    QDoubleSpinBox* m_videoTranslationXSpin = nullptr;
    QDoubleSpinBox* m_videoTranslationYSpin = nullptr;
    QDoubleSpinBox* m_videoRotationSpin = nullptr;
    QDoubleSpinBox* m_videoScaleXSpin = nullptr;
    QDoubleSpinBox* m_videoScaleYSpin = nullptr;
    QComboBox* m_videoInterpolationCombo = nullptr;
    QCheckBox* m_keyframeModeCheckBox = nullptr;
    QListWidget* m_videoKeyframeList = nullptr;
    QPushButton* m_addVideoKeyframeButton = nullptr;
    QPushButton* m_removeVideoKeyframeButton = nullptr;
    QPushButton* m_renderButton = nullptr;
    std::unique_ptr<ControlServer> m_controlServer;
    std::unique_ptr<AudioEngine> m_audioEngine;
    QTimer m_playbackTimer;
    QTimer m_mainThreadHeartbeatTimer;
    QTimer m_stateSaveTimer;
    bool m_ignoreSeekSignal = false;
    bool m_loadingState = false;
    bool m_pendingSaveAfterLoad = false;
    bool m_restoringHistory = false;
    QString m_currentRootPath;
    QString m_galleryFolderPath;
    QStringList m_expandedExplorerPaths;
    QByteArray m_lastSavedState;
    QJsonArray m_historyEntries;
    int m_historyIndex = -1;
    bool m_updatingVideoInspector = false;
    int64_t m_selectedVideoKeyframeFrame = -1;
    QSet<int64_t> m_selectedVideoKeyframeFrames;
    std::atomic<qint64> m_fastCurrentFrame{0};
    std::atomic<bool> m_fastPlaybackActive{false};
    std::atomic<qint64> m_lastMainThreadHeartbeatMs{0};
    std::atomic<qint64> m_lastPlayheadAdvanceMs{0};
};

// ============================================================================
// Main Entry Point
// ============================================================================
int main(int argc, char** argv) {
    QElapsedTimer startupTimer;
    startupTimer.start();
    
    qDebug() << "[STARTUP] main() started";
    QApplication app(argc, argv);
    qDebug() << "[STARTUP] QApplication created in" << startupTimer.elapsed() << "ms";
    QApplication::setApplicationName(QStringLiteral("QRhi Editor Professional"));
    
    // Register metatypes
    qRegisterMetaType<editor::FrameHandle>();

    constexpr quint16 kDefaultControlPort = 40130;
    quint16 controlPort = kDefaultControlPort;
    bool ok = false;
    const uint envControlPort = qEnvironmentVariableIntValue("EDITOR_CONTROL_PORT", &ok);
    if (ok && envControlPort <= std::numeric_limits<quint16>::max()) {
        controlPort = static_cast<quint16>(envControlPort);
    }

    EditorWindow window(controlPort);
    window.show();
    
    return app.exec();
}

#include "editor.moc"
