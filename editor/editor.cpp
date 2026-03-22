#include "frame_handle.h"
#include "async_decoder.h"
#include "timeline_cache.h"
#include "memory_budget.h"
#include "gpu_compositor.h"
#include "control_server.h"
#include "debug_controls.h"
#include "editor_shared.h"
#include "audio_engine.h"
#include "timeline_widget.h"
#include "render.h"

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
#include <QFontComboBox>
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
#include <QListWidgetItem>
#include <QMainWindow>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QOffscreenSurface>
#include <QOpenGLBuffer>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QOpenGLPixelTransferOptions>
#include <QOpenGLShaderProgram>
#include <QOpenGLTexture>
#include <QOpenGLWidget>
#include <QPainter>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProcessEnvironment>
#include <QProgressBar>
#include <QPushButton>
#include <QRandomGenerator>
#include <QRegularExpression>
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
#include <QTableWidget>
#include <QTextDocument>
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
    return debugPlaybackEnabled();
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
class PreviewWindow final : public QOpenGLWidget, protected QOpenGLFunctions {
    Q_OBJECT
public:
    PreviewWindow(QWidget* parent = nullptr)
        : QOpenGLWidget(parent)
        , m_quadBuffer(QOpenGLBuffer::VertexBuffer)
    {
        setMinimumSize(640, 360);
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
        if (context()) {
            makeCurrent();
            releaseGlResources();
            doneCurrent();
        }
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
        setCurrentPlaybackSample(frameToSamples(frame));
    }

    void setCurrentPlaybackSample(int64_t samplePosition) {
        const int64_t sanitizedSample = qMax<int64_t>(0, samplePosition);
        const qreal framePosition = samplesToFramePosition(sanitizedSample);
        const int64_t displayFrame = qMax<int64_t>(0, static_cast<int64_t>(std::floor(framePosition)));
        playbackTrace(QStringLiteral("PreviewWindow::setCurrentPlaybackSample"),
                      QStringLiteral("sample=%1 frame=%2 visible=%3 cache=%4")
                          .arg(sanitizedSample)
                          .arg(framePosition, 0, 'f', 3)
                          .arg(isVisible())
                          .arg(m_cache != nullptr));
        m_currentSample = sanitizedSample;
        m_currentFramePosition = framePosition;
        m_currentFrame = displayFrame;
        if (m_cache) {
            m_cache->setPlayheadFrame(displayFrame);
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
        m_transcriptSectionsCache.clear();
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

    void setRenderSyncMarkers(const QVector<RenderSyncMarker>& markers) {
        m_renderSyncMarkers = markers;
        requestFramesForCurrentPosition();
        scheduleRepaint();
    }
    
    QString backendName() const {
        return usingCpuFallback() ? QStringLiteral("CPU Preview Fallback")
                                  : QStringLiteral("OpenGL Shader Preview");
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
            if (clip.hasAudio && isSampleWithinClip(clip, m_currentSample)) {
                return clip.label;
            }
        }
        return QString();
    }

    bool preparePlaybackAdvance(int64_t targetFrame) {
        return preparePlaybackAdvanceSample(frameToSamples(targetFrame));
    }

    bool preparePlaybackAdvanceSample(int64_t targetSample) {
        if (m_clips.isEmpty()) {
            return true;
        }

        ensurePipeline();
        if (!m_cache) {
            return false;
        }

        static constexpr int kMaxVisibleBacklog = 4;
        bool hasActiveClip = false;

        for (const TimelineClip& clip : m_clips) {
            if (!clipHasVisuals(clip)) {
                continue;
            }
            if (!isSampleWithinClip(clip, targetSample)) {
                continue;
            }

            hasActiveClip = true;
            const int64_t localFrame = sourceFrameForSample(clip, targetSample);
            if (m_cache->isFrameCached(clip.id, localFrame)) {
                continue;
            }

            if (m_cache->isVisibleRequestPending(clip.id, localFrame)) {
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
            {QStringLiteral("backend"), backendName()},
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
    void paintEvent(QPaintEvent* event) override {
        if (!usingCpuFallback()) {
            QOpenGLWidget::paintEvent(event);
            return;
        }

        if (!QWidget::paintEngine()) {
            return;
        }

        Q_UNUSED(event)
        m_lastPaintMs = nowMs();

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        drawBackground(&painter);
        const QList<TimelineClip> activeClips = getActiveClips();
        const QRect safeRect = rect().adjusted(24, 24, -24, -24);
        drawCompositedPreview(&painter, safeRect, activeClips);
        drawPreviewChrome(&painter, safeRect, activeClips.size());
    }

    void initializeGL() override {
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
            uniform float u_brightness;
            uniform float u_contrast;
            uniform float u_saturation;
            uniform float u_opacity;
            varying vec2 v_texCoord;

            void main() {
                vec4 color = texture2D(u_texture, v_texCoord);
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
            qWarning() << "Failed to build preview shader program" << m_shaderProgram->log();
            m_shaderProgram.reset();
            return;
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

        connect(context(), &QOpenGLContext::aboutToBeDestroyed, this, [this]() {
            makeCurrent();
            releaseGlResources();
            doneCurrent();
        }, Qt::DirectConnection);
    }

    void resizeGL(int w, int h) override {
        Q_UNUSED(w)
        Q_UNUSED(h)
    }

    void showEvent(QShowEvent* event) override {
        QOpenGLWidget::showEvent(event);
        QTimer::singleShot(0, this, [this]() {
            requestFramesForCurrentPosition();
        });
    }
    
    void paintGL() override {
        m_lastPaintMs = nowMs();

        glDisable(GL_DEPTH_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

        const float phase = static_cast<float>(m_currentFrame % 180) / 179.0f;
        const float clipFactor = qBound(0.0f, static_cast<float>(m_clipCount) / 8.0f, 1.0f);
        const float motion = m_playing ? phase : 0.25f;
        glViewport(0, 0, width() * devicePixelRatio(), height() * devicePixelRatio());
        glClearColor(0.08f + 0.12f * motion,
                     0.08f + 0.10f * clipFactor,
                     0.10f + 0.16f * (1.0f - motion),
                     1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        QList<TimelineClip> activeClips = getActiveClips();
        const QRect safeRect = rect().adjusted(24, 24, -24, -24);
        const QRect compositeRect = scaledCanvasRect(previewCanvasBaseRect());
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        bool drewAnyFrame = false;
        bool waitingForFrame = false;
        renderCompositedPreviewGL(compositeRect, activeClips, drewAnyFrame, waitingForFrame);
        drawCompositedPreviewOverlay(&painter, safeRect, compositeRect, activeClips, drewAnyFrame, waitingForFrame);
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
            const PreviewOverlayInfo selectedInfo = m_overlayInfo.value(m_selectedClipId);
            if (m_dragMode == PreviewDragMode::Move) {
                if (moveRequested) {
                    const QRect compositeRect = scaledCanvasRect(previewCanvasBaseRect());
                    const QPointF previewScale = previewCanvasScale(compositeRect);
                    moveRequested(m_selectedClipId,
                                  m_dragOriginTransform.translationX +
                                      ((event->position().x() - m_dragOriginPos.x()) /
                                       qMax<qreal>(0.0001, previewScale.x())),
                                  m_dragOriginTransform.translationY +
                                      ((event->position().y() - m_dragOriginPos.y()) /
                                       qMax<qreal>(0.0001, previewScale.y())),
                                  false);
                }
                event->accept();
                return;
            }
            qreal scaleX = m_dragOriginTransform.scaleX;
            qreal scaleY = m_dragOriginTransform.scaleY;
            if (selectedInfo.kind == PreviewOverlayKind::TranscriptOverlay) {
                const QRect compositeRect = scaledCanvasRect(previewCanvasBaseRect());
                const QPointF previewScale = previewCanvasScale(compositeRect);
                qreal width = transcriptOverlaySizeForSelectedClip().width();
                qreal height = transcriptOverlaySizeForSelectedClip().height();
                if (m_dragMode == PreviewDragMode::ResizeX || m_dragMode == PreviewDragMode::ResizeBoth) {
                    width = qMax<qreal>(80.0, width + ((event->position().x() - m_dragOriginPos.x()) /
                                                       qMax<qreal>(0.0001, previewScale.x())));
                }
                if (m_dragMode == PreviewDragMode::ResizeY || m_dragMode == PreviewDragMode::ResizeBoth) {
                    height = qMax<qreal>(40.0, height + ((event->position().y() - m_dragOriginPos.y()) /
                                                         qMax<qreal>(0.0001, previewScale.y())));
                }
                if (resizeRequested) {
                    resizeRequested(m_selectedClipId, width, height, false);
                }
                event->accept();
                return;
            }
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
            const PreviewOverlayInfo selectedInfo = m_overlayInfo.value(m_selectedClipId);
            const TimelineClip::TransformKeyframe transform = evaluateTransformForSelectedClip();
            if (m_dragMode == PreviewDragMode::Move) {
                if (moveRequested) {
                    if (selectedInfo.kind == PreviewOverlayKind::TranscriptOverlay) {
                        for (const TimelineClip& clip : m_clips) {
                            if (clip.id == m_selectedClipId) {
                                moveRequested(m_selectedClipId,
                                              clip.transcriptOverlay.translationX,
                                              clip.transcriptOverlay.translationY,
                                              true);
                                break;
                            }
                        }
                    } else {
                        moveRequested(m_selectedClipId, transform.translationX, transform.translationY, true);
                    }
                }
            } else if (resizeRequested) {
                if (selectedInfo.kind == PreviewOverlayKind::TranscriptOverlay) {
                    const QSizeF size = transcriptOverlaySizeForSelectedClip();
                    resizeRequested(m_selectedClipId, size.width(), size.height(), true);
                } else {
                    resizeRequested(m_selectedClipId, transform.scaleX, transform.scaleY, true);
                }
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

    QPointF previewCanvasScale(const QRect& targetRect) const {
        const QSize output = m_outputSize.isValid() ? m_outputSize : QSize(1080, 1920);
        return QPointF(targetRect.width() / qMax<qreal>(1.0, output.width()),
                       targetRect.height() / qMax<qreal>(1.0, output.height()));
    }

    bool clipShowsTranscriptOverlay(const TimelineClip& clip) const {
        return clip.mediaType == ClipMediaType::Audio && clip.transcriptOverlay.enabled;
    }

    const QVector<TranscriptSection>& transcriptSectionsForClip(const TimelineClip& clip) const {
        const QString key = clip.filePath;
        auto it = m_transcriptSectionsCache.find(key);
        if (it == m_transcriptSectionsCache.end()) {
            it = m_transcriptSectionsCache.insert(key, loadTranscriptSections(transcriptPathForClipFile(clip.filePath)));
        }
        return it.value();
    }

    TranscriptOverlayLayout transcriptOverlayLayoutForClip(const TimelineClip& clip) const {
        if (!clipShowsTranscriptOverlay(clip)) {
            return {};
        }
        const QVector<TranscriptSection>& sections = transcriptSectionsForClip(clip);
        if (sections.isEmpty()) {
            return {};
        }
        const int64_t sourceFrame = sourceFrameForSample(clip, m_currentSample);
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

    QRectF transcriptOverlayRectForTarget(const TimelineClip& clip, const QRect& targetRect) const {
        const QPointF previewScale = previewCanvasScale(targetRect);
        const QSizeF size(qMax<qreal>(40.0, clip.transcriptOverlay.boxWidth * previewScale.x()),
                          qMax<qreal>(20.0, clip.transcriptOverlay.boxHeight * previewScale.y()));
        const QPointF center(targetRect.center().x() + (clip.transcriptOverlay.translationX * previewScale.x()),
                             targetRect.center().y() + (clip.transcriptOverlay.translationY * previewScale.y()));
        return QRectF(center.x() - (size.width() / 2.0),
                      center.y() - (size.height() / 2.0),
                      size.width(),
                      size.height());
    }

    QSizeF transcriptOverlaySizeForSelectedClip() const {
        const PreviewOverlayInfo info = m_overlayInfo.value(m_selectedClipId);
        const QRect compositeRect = scaledCanvasRect(previewCanvasBaseRect());
        const QPointF previewScale = previewCanvasScale(compositeRect);
        return QSizeF(info.bounds.width() / qMax<qreal>(0.0001, previewScale.x()),
                      info.bounds.height() / qMax<qreal>(0.0001, previewScale.y()));
    }

    void drawTranscriptOverlay(QPainter* painter, const TimelineClip& clip, const QRect& targetRect) {
        const TranscriptOverlayLayout overlayLayout = transcriptOverlayLayoutForClip(clip);
        if (overlayLayout.lines.isEmpty()) {
            return;
        }
        const QRectF bounds = transcriptOverlayRectForTarget(clip, targetRect);
        const QRectF textBounds = bounds.adjusted(18.0, 14.0, -18.0, -14.0);
        const QColor highlightFillColor(QStringLiteral("#fff2a8"));
        const QColor highlightTextColor(QStringLiteral("#181818"));
        const QString shadowHtml = transcriptOverlayHtml(overlayLayout,
                                                         QColor(0, 0, 0, 200),
                                                         QColor(0, 0, 0, 200),
                                                         QColor(0, 0, 0, 0));
        const QString textHtml = transcriptOverlayHtml(overlayLayout,
                                                       clip.transcriptOverlay.textColor,
                                                       highlightTextColor,
                                                       highlightFillColor);
        if (textHtml.isEmpty()) {
            return;
        }
        painter->save();
        painter->setPen(Qt::NoPen);
        painter->setBrush(QColor(0, 0, 0, 120));
        painter->drawRoundedRect(bounds, 14.0, 14.0);
        QFont font(clip.transcriptOverlay.fontFamily);
        font.setPixelSize(qMax(8, qRound(clip.transcriptOverlay.fontPointSize * previewCanvasScale(targetRect).y())));
        font.setBold(clip.transcriptOverlay.bold);
        font.setItalic(clip.transcriptOverlay.italic);
        QTextDocument shadowDoc;
        shadowDoc.setDefaultFont(font);
        shadowDoc.setDocumentMargin(0.0);
        shadowDoc.setTextWidth(textBounds.width());
        shadowDoc.setHtml(shadowHtml);
        QTextDocument textDoc;
        textDoc.setDefaultFont(font);
        textDoc.setDocumentMargin(0.0);
        textDoc.setTextWidth(textBounds.width());
        textDoc.setHtml(textHtml);
        const qreal textY = textBounds.top() + qMax<qreal>(0.0, (textBounds.height() - textDoc.size().height()) / 2.0);
        painter->translate(textBounds.left() + 3.0, textY + 3.0);
        shadowDoc.drawContents(painter);
        painter->translate(-3.0, -3.0);
        textDoc.drawContents(painter);
        painter->restore();

        PreviewOverlayInfo info;
        info.kind = PreviewOverlayKind::TranscriptOverlay;
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
    }

    bool usingCpuFallback() const {
        return !context() || !isValid() || !m_shaderProgram;
    }

    void ensurePipeline() {
        if (m_cache) {
            return;
        }

        playbackTrace(QStringLiteral("PreviewWindow::ensurePipeline.begin"),
                      QStringLiteral("clips=%1 frame=%2").arg(m_clips.size()).arg(m_currentFramePosition, 0, 'f', 3));

        m_decoder = std::make_unique<AsyncDecoder>(this);
        m_decoder->initialize();

        m_cache = std::make_unique<TimelineCache>(m_decoder.get(),
                                                  m_decoder->memoryBudget(),
                                                  this);
        m_cache->setMaxMemory(256 * 1024 * 1024);
        m_cache->setLookaheadFrames(18);
        m_cache->setPlaybackSpeed(1.0);
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

    void releaseGlResources() {
        for (auto it = m_textureCache.begin(); it != m_textureCache.end(); ++it) {
            delete it.value().texture;
            it.value().texture = nullptr;
        }
        m_textureCache.clear();
        if (m_quadBuffer.isCreated()) {
            m_quadBuffer.destroy();
        }
        m_shaderProgram.reset();
    }

    QString textureCacheKey(const FrameHandle& frame) const {
        return QStringLiteral("%1|%2").arg(frame.sourcePath()).arg(frame.frameNumber());
    }

    QOpenGLTexture* textureForFrame(const FrameHandle& frame) {
        if (frame.isNull() || !frame.hasCpuImage()) {
            return nullptr;
        }

        const QString key = textureCacheKey(frame);
        const qint64 decodeTimestamp = frame.data() ? frame.data()->decodeTimestamp : 0;
        TextureCacheEntry entry = m_textureCache.value(key);
        if (entry.texture && entry.decodeTimestamp == decodeTimestamp) {
            entry.lastUsedMs = nowMs();
            m_textureCache.insert(key, entry);
            return entry.texture;
        }

        if (entry.texture) {
            delete entry.texture;
            entry.texture = nullptr;
        }

        QImage uploadImage = frame.cpuImage();
        if (uploadImage.format() != QImage::Format_ARGB32_Premultiplied) {
            uploadImage = uploadImage.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        }

        auto* texture = new QOpenGLTexture(QOpenGLTexture::Target2D);
        texture->setFormat(QOpenGLTexture::RGBA8_UNorm);
        texture->setSize(uploadImage.width(), uploadImage.height());
        texture->allocateStorage(QOpenGLTexture::RGBA, QOpenGLTexture::UInt8);
        QOpenGLPixelTransferOptions uploadOptions;
        uploadOptions.setAlignment(4);
        texture->setData(QOpenGLTexture::BGRA,
                         QOpenGLTexture::UInt8,
                         uploadImage.constBits(),
                         &uploadOptions);
        if (!texture->isCreated()) {
            delete texture;
            return nullptr;
        }
        texture->setMinMagFilters(QOpenGLTexture::Linear, QOpenGLTexture::Linear);
        texture->setWrapMode(QOpenGLTexture::ClampToEdge);

        entry.texture = texture;
        entry.decodeTimestamp = decodeTimestamp;
        entry.lastUsedMs = nowMs();
        entry.size = uploadImage.size();
        m_textureCache.insert(key, entry);
        trimTextureCache();
        return texture;
    }

    void trimTextureCache() {
        static constexpr int kMaxTextureCacheEntries = 180;
        if (m_textureCache.size() <= kMaxTextureCacheEntries) {
            return;
        }

        QVector<QString> keys = m_textureCache.keys().toVector();
        std::sort(keys.begin(), keys.end(), [this](const QString& a, const QString& b) {
            return m_textureCache.value(a).lastUsedMs < m_textureCache.value(b).lastUsedMs;
        });

        const int removeCount = m_textureCache.size() - kMaxTextureCacheEntries;
        for (int i = 0; i < removeCount; ++i) {
            TextureCacheEntry entry = m_textureCache.take(keys[i]);
            delete entry.texture;
        }
    }

    bool isSampleWithinClip(const TimelineClip& clip, int64_t samplePosition) const {
        const int64_t clipStartSample = clipTimelineStartSamples(clip);
        const int64_t clipEndSample = clipStartSample + frameToSamples(clip.durationFrames);
        return samplePosition >= clipStartSample && samplePosition < clipEndSample;
    }

    int64_t sourceSampleForPlaybackSample(const TimelineClip& clip, int64_t samplePosition) const {
        return qMax<int64_t>(0, clipSourceInSamples(clip) + (samplePosition - clipTimelineStartSamples(clip)));
    }

    int64_t sourceFrameForSample(const TimelineClip& clip, int64_t samplePosition) const {
        return sourceFrameForClipAtTimelinePosition(
            clip,
            samplesToFramePosition(samplePosition),
            m_renderSyncMarkers);
    }

    QRectF renderFrameLayerGL(const QRect& targetRect, const TimelineClip& clip, const FrameHandle& frame) {
        if (!m_shaderProgram) {
            return QRectF();
        }

        QOpenGLTexture* texture = textureForFrame(frame);
        if (!texture) {
            return QRectF();
        }

        const QRect fitted = fitRect(frame.size(), targetRect);
        const TimelineClip::TransformKeyframe transform = evaluateClipTransformAtPosition(clip, m_currentFramePosition);
        const QPointF previewScale = previewCanvasScale(targetRect);
        const QPointF center(fitted.center().x() + (transform.translationX * previewScale.x()),
                             fitted.center().y() + (transform.translationY * previewScale.y()));

        QMatrix4x4 projection;
        projection.ortho(0.0f, static_cast<float>(width()),
                         static_cast<float>(height()), 0.0f,
                         -1.0f, 1.0f);

        QMatrix4x4 model;
        model.translate(center.x(), center.y());
        model.rotate(transform.rotation, 0.0f, 0.0f, 1.0f);
        model.scale(fitted.width() * transform.scaleX, fitted.height() * transform.scaleY, 1.0f);

        const qreal brightness = m_bypassGrading ? 0.0 : clip.brightness;
        const qreal contrast = m_bypassGrading ? 1.0 : clip.contrast;
        const qreal saturation = m_bypassGrading ? 1.0 : clip.saturation;
        const qreal opacity = m_bypassGrading ? 1.0 : clip.opacity;

        m_shaderProgram->bind();
        m_shaderProgram->setUniformValue("u_mvp", projection * model);
        m_shaderProgram->setUniformValue("u_brightness", GLfloat(brightness));
        m_shaderProgram->setUniformValue("u_contrast", GLfloat(contrast));
        m_shaderProgram->setUniformValue("u_saturation", GLfloat(saturation));
        m_shaderProgram->setUniformValue("u_opacity", GLfloat(opacity));
        m_shaderProgram->setUniformValue("u_texture", 0);

        glActiveTexture(GL_TEXTURE0);
        texture->bind();
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
        texture->release();
        m_shaderProgram->release();

        QTransform overlayTransform;
        overlayTransform.translate(center.x(), center.y());
        overlayTransform.rotate(transform.rotation);
        overlayTransform.scale(transform.scaleX, transform.scaleY);
        return overlayTransform.mapRect(QRectF(-fitted.width() / 2.0,
                                               -fitted.height() / 2.0,
                                               fitted.width(),
                                               fitted.height()));
    }

    void renderCompositedPreviewGL(const QRect& compositeRect,
                                   const QList<TimelineClip>& activeClips,
                                   bool& drewAnyFrame,
                                   bool& waitingForFrame) {
        m_overlayInfo.clear();
        m_paintOrder.clear();
        for (const TimelineClip& clip : activeClips) {
            if (!clipHasVisuals(clip)) {
                continue;
            }
            const int64_t localFrame = sourceFrameForSample(clip, m_currentSample);
            const FrameHandle frame = m_cache ? m_cache->getBestCachedFrame(clip.id, localFrame) : FrameHandle();
            if (frame.isNull()) {
                waitingForFrame = true;
                continue;
            }

            const QRectF bounds = renderFrameLayerGL(compositeRect, clip, frame);
            if (!bounds.isEmpty()) {
                PreviewOverlayInfo info;
                info.kind = PreviewOverlayKind::VisualClip;
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
            }
            drewAnyFrame = true;
        }
    }

    void drawCompositedPreviewOverlay(QPainter* painter,
                                      const QRect& safeRect,
                                      const QRect& compositeRect,
                                      const QList<TimelineClip>& activeClips,
                                      bool drewAnyFrame,
                                      bool waitingForFrame) {
        painter->save();
        painter->setPen(QPen(QColor(255, 255, 255, 40), 1.5));
        painter->setBrush(QColor(255, 255, 255, 18));
        painter->drawRoundedRect(safeRect, 18, 18);

        if (activeClips.isEmpty()) {
            QList<TimelineClip> activeAudioClips;
            for (const TimelineClip& clip : m_clips) {
                if (clipIsAudioOnly(clip) && isSampleWithinClip(clip, m_currentSample)) {
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

        painter->setPen(QPen(QColor(255, 255, 255, 36), 1.0));
        painter->setBrush(Qt::NoBrush);
        painter->drawRoundedRect(compositeRect.adjusted(0, 0, -1, -1), 12, 12);

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

        for (const TimelineClip& clip : activeClips) {
            if (clipShowsTranscriptOverlay(clip)) {
                drawTranscriptOverlay(painter, clip, compositeRect);
            }
        }

        for (const TimelineClip& clip : activeClips) {
            const PreviewOverlayInfo info = m_overlayInfo.value(clip.id);
            if (clip.id == m_selectedClipId && info.bounds.isValid()) {
                painter->setPen(QPen(QColor(QStringLiteral("#fff4c2")), 2.0));
                painter->setBrush(Qt::NoBrush);
                painter->drawRect(info.bounds);
                painter->setBrush(QColor(QStringLiteral("#fff4c2")));
                painter->drawRect(info.rightHandle);
                painter->drawRect(info.bottomHandle);
                painter->drawRect(info.cornerHandle);
            }
        }

        QList<TimelineClip> activeAudioClips;
        for (const TimelineClip& clip : m_clips) {
            if (clipIsAudioOnly(clip) && isSampleWithinClip(clip, m_currentSample)) {
                activeAudioClips.push_back(clip);
            }
        }
        if (!activeAudioClips.isEmpty()) {
            drawAudioBadge(painter, compositeRect, activeAudioClips);
        }
        painter->restore();
    }

    void drawBackground(QPainter* painter) {
        const float phase = std::fmod(static_cast<float>(m_currentFramePosition), 180.0f) / 179.0f;
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
            if (isSampleWithinClip(clip, m_currentSample)) {
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
        static constexpr int kMaxVisibleBacklog = 4;
        QVector<const TimelineClip*> activeClips;
        activeClips.reserve(m_clips.size());
        for (const TimelineClip& clip : m_clips) {
            if (!clipHasVisuals(clip)) {
                continue;
            }
            if (isSampleWithinClip(clip, m_currentSample)) {
                activeClips.push_back(&clip);
            }
        }

        if (activeClips.isEmpty()) {
            playbackTrace(QStringLiteral("PreviewWindow::requestFramesForCurrentPosition"),
                          QStringLiteral("no-active-clips frame=%1").arg(m_currentFramePosition, 0, 'f', 3));
            return;
        }

        ensurePipeline();
        if (!m_cache) {
            playbackTrace(QStringLiteral("PreviewWindow::requestFramesForCurrentPosition"),
                          QStringLiteral("cache-unavailable frame=%1").arg(m_currentFramePosition, 0, 'f', 3));
            return;
        }

        playbackTrace(QStringLiteral("PreviewWindow::requestFramesForCurrentPosition"),
                      QStringLiteral("active=%1 frame=%2 pending=%3")
                          .arg(activeClips.size())
                          .arg(m_currentFramePosition, 0, 'f', 3)
                          .arg(m_decoder ? m_decoder->pendingRequestCount() : 0));

        if (m_cache->pendingVisibleRequestCount() >= kMaxVisibleBacklog) {
            playbackTrace(QStringLiteral("PreviewWindow::requestFramesForCurrentPosition.skip"),
                          QStringLiteral("reason=visible-backlog count=%1")
                              .arg(m_cache->pendingVisibleRequestCount()));
            return;
        }

        for (const TimelineClip* clip : activeClips) {
            const int64_t localFrame = sourceFrameForSample(*clip, m_currentSample);
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
                if (clipIsAudioOnly(clip) && isSampleWithinClip(clip, m_currentSample)) {
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
            const int64_t localFrame = sourceFrameForSample(clip, m_currentSample);
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
            if (clipIsAudioOnly(clip) && isSampleWithinClip(clip, m_currentSample)) {
                activeAudioClips.push_back(clip);
            }
        }
        if (!activeAudioClips.isEmpty()) {
            drawAudioBadge(painter, compositeRect, activeAudioClips);
        }
        for (const TimelineClip& clip : activeClips) {
            if (clipShowsTranscriptOverlay(clip)) {
                drawTranscriptOverlay(painter, clip, compositeRect);
            }
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
                              .arg(m_currentFramePosition, 0, 'f', 3)
                              .arg(backendName())
                              .arg(m_bypassGrading ? QStringLiteral("bypassed") : QStringLiteral("on")));
    }
    
    void drawFrameLayer(QPainter* painter, const QRect& targetRect,
                        const TimelineClip& clip, const FrameHandle& frame) {
        painter->save();
        painter->setClipRect(targetRect);

        if (!frame.isNull() && frame.hasCpuImage()) {
            const QImage img = m_bypassGrading ? frame.cpuImage() : applyClipGrade(frame.cpuImage(), clip);
            const QRect fitted = fitRect(img.size(), targetRect);
            const TimelineClip::TransformKeyframe transform =
                evaluateClipTransformAtPosition(clip, m_currentFramePosition);
            const QPointF previewScale = previewCanvasScale(targetRect);
            painter->translate(fitted.center().x() + (transform.translationX * previewScale.x()),
                               fitted.center().y() + (transform.translationY * previewScale.y()));
            painter->rotate(transform.rotation);
            painter->scale(transform.scaleX, transform.scaleY);
            const QRectF drawRect(-fitted.width() / 2.0,
                                  -fitted.height() / 2.0,
                                  fitted.width(),
                                  fitted.height());
            painter->drawImage(drawRect, img);

            QTransform overlayTransform;
            overlayTransform.translate(fitted.center().x() + (transform.translationX * previewScale.x()),
                                       fitted.center().y() + (transform.translationY * previewScale.y()));
            overlayTransform.rotate(transform.rotation);
            overlayTransform.scale(transform.scaleX, transform.scaleY);
            const QRectF bounds = overlayTransform.mapRect(drawRect);
            PreviewOverlayInfo info;
            info.kind = PreviewOverlayKind::VisualClip;
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
            const qreal phase = std::fmod(static_cast<qreal>(idx * 13) + m_currentFramePosition, 100.0) / 99.0;
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
        Q_UNUSED(painter)
        Q_UNUSED(safeRect)
        Q_UNUSED(activeClipCount)
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
                return evaluateClipTransformAtPosition(clip, m_currentFramePosition);
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

    std::unique_ptr<AsyncDecoder> m_decoder;
    std::unique_ptr<TimelineCache> m_cache;
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
        m_playbackTimer.setTimerType(Qt::PreciseTimer);
        m_playbackTimer.setInterval(16);
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
        auto* playbackShortcut = new QShortcut(QKeySequence(Qt::Key_Space), this);
        connect(playbackShortcut, &QShortcut::activated, this, [this]() {
            if (m_timeline) {
                togglePlayback();
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
        connect(m_videoScaleXSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double) {
            syncScaleSpinPair(m_videoScaleXSpin, m_videoScaleYSpin);
            applySelectedVideoKeyframeFromInspector(false);
        });
        connect(m_videoScaleXSpin, &QDoubleSpinBox::editingFinished, this, [this]() {
            applySelectedVideoKeyframeFromInspector(true);
        });
        connect(m_videoScaleYSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double) {
            syncScaleSpinPair(m_videoScaleYSpin, m_videoScaleXSpin);
            applySelectedVideoKeyframeFromInspector(false);
        });
        connect(m_videoScaleYSpin, &QDoubleSpinBox::editingFinished, this, [this]() {
            applySelectedVideoKeyframeFromInspector(true);
        });
        connect(m_videoInterpolationCombo, &QComboBox::currentIndexChanged, this, [this](int) {
            applySelectedVideoKeyframeFromInspector(true);
        });
        connect(m_mirrorHorizontalCheckBox, &QCheckBox::toggled, this, [this](bool) {
            applySelectedVideoKeyframeFromInspector(true);
        });
        connect(m_mirrorVerticalCheckBox, &QCheckBox::toggled, this, [this](bool) {
            applySelectedVideoKeyframeFromInspector(true);
        });

        QTimer::singleShot(0, this, [this]() {
            loadProjectsFromFolders();
            refreshProjectsList();
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
        // Audio is always the master clock when available
        if (m_audioEngine && m_audioEngine->hasPlayableAudio()) {
            int64_t audioSample = qMax<int64_t>(0, m_audioEngine->currentSample());
            qreal audioFramePosition = samplesToFramePosition(audioSample);
            int64_t audioFrame = qBound<int64_t>(0,
                                                static_cast<int64_t>(std::floor(audioFramePosition)),
                                                m_timeline->totalFrames());

            // When speech filter is active, check if audio is in a gap and seek it forward
            if (speechFilterPlaybackEnabled()) {
                const QVector<ExportRangeSegment> ranges = effectivePlaybackRanges();
                if (!ranges.isEmpty()) {
                    // Check if current audio position is in a gap (between word ranges)
                    bool inGap = true;
                    int64_t nextWordStart = -1;
                    for (const auto& range : ranges) {
                        if (audioFrame >= range.startFrame && audioFrame <= range.endFrame) {
                            // Audio is inside a word range - normal playback
                            inGap = false;
                            break;
                        }
                        if (audioFrame < range.startFrame) {
                            // Audio is before this range - we're in a gap
                            nextWordStart = range.startFrame;
                            break;
                        }
                    }

                    if (inGap && nextWordStart >= 0) {
                        // Audio is in a gap - seek it to the next word
                        playbackTrace(QStringLiteral("EditorWindow::advanceFrame.speechFilterSkip"),
                                      QStringLiteral("from=%1 to=%2").arg(audioFrame).arg(nextWordStart));
                        m_audioEngine->seek(nextWordStart);
                        audioSample = m_audioEngine->currentSample();
                        audioFramePosition = samplesToFramePosition(audioSample);
                        audioFrame = qBound<int64_t>(0,
                                                    static_cast<int64_t>(std::floor(audioFramePosition)),
                                                    m_timeline->totalFrames());
                    } else if (inGap) {
                        // Past all words - loop back to start
                        m_audioEngine->seek(ranges.constFirst().startFrame);
                        audioSample = m_audioEngine->currentSample();
                        audioFramePosition = samplesToFramePosition(audioSample);
                        audioFrame = qBound<int64_t>(0,
                                                    static_cast<int64_t>(std::floor(audioFramePosition)),
                                                    m_timeline->totalFrames());
                    }
                }
            }

            if (audioFrame == m_timeline->currentFrame()) {
                if (m_preview) {
                    m_preview->setCurrentPlaybackSample(audioSample);
                }
                return;
            }
            if (m_preview) {
                const bool ready = m_preview->preparePlaybackAdvanceSample(audioSample);
                if (!ready) {
                    playbackTrace(QStringLiteral("EditorWindow::advanceFrame.catchup"),
                                  QStringLiteral("audioSample=%1 audioFrame=%2")
                                      .arg(audioSample)
                                      .arg(audioFramePosition, 0, 'f', 3));
                }
            }
            playbackTrace(QStringLiteral("EditorWindow::advanceFrame"),
                          QStringLiteral("clock=audio sample=%1 frame=%2").arg(audioSample).arg(audioFramePosition, 0, 'f', 3));
            // Audio is master - don't sync audio to itself, just update video/timeline
            setCurrentPlaybackSample(audioSample, false, /*duringPlayback=*/true);
            return;
        }

        // No audio engine - video is master (fallback)
        const int64_t nextFrame = nextPlaybackFrame(m_timeline->currentFrame());
        if (m_preview) {
            const bool ready = m_preview->preparePlaybackAdvance(nextFrame);
            if (!ready) {
                playbackTrace(QStringLiteral("EditorWindow::advanceFrame.catchup"),
                              QStringLiteral("next=%1").arg(nextFrame));
            }
        }
        playbackTrace(QStringLiteral("EditorWindow::advanceFrame"),
                      QStringLiteral("clock=video current=%1 next=%2").arg(m_timeline->currentFrame()).arg(nextFrame));
        setCurrentPlaybackSample(frameToSamples(nextFrame), false, /*duringPlayback=*/true);
    }

private:
    bool speechFilterPlaybackEnabled() const {
        return m_exportOnlyTranscriptWordsCheckBox && m_exportOnlyTranscriptWordsCheckBox->isChecked();
    }

    QVector<ExportRangeSegment> effectivePlaybackRanges() const {
        if (!m_timeline) {
            return {};
        }
        QVector<ExportRangeSegment> ranges = m_timeline->exportRanges();
        if (!speechFilterPlaybackEnabled()) {
            return ranges;
        }
        return transcriptWordExportRanges(ranges, m_timeline->clips(), m_timeline->renderSyncMarkers());
    }

    int64_t nextPlaybackFrame(int64_t currentFrame) const {
        if (!m_timeline) {
            return 0;
        }

        const QVector<ExportRangeSegment> ranges = effectivePlaybackRanges();
        if (ranges.isEmpty()) {
            const int64_t nextFrame = currentFrame + 1;
            return nextFrame > m_timeline->totalFrames() ? 0 : nextFrame;
        }

        for (const ExportRangeSegment& range : ranges) {
            if (currentFrame < range.startFrame) {
                // Jump to the start of the next word range
                return range.startFrame;
            }
            if (currentFrame >= range.startFrame && currentFrame < range.endFrame) {
                // Inside a word range - advance normally
                return currentFrame + 1;
            }
            // If currentFrame == range.endFrame, continue to find next range
        }
        // Finished all ranges - loop back to start
        return ranges.constFirst().startFrame;
    }

    QString projectsDirPath() const {
        return QDir(QDir::currentPath()).filePath(QStringLiteral("projects"));
    }

    QString currentProjectMarkerPath() const {
        return QDir(projectsDirPath()).filePath(QStringLiteral(".current_project"));
    }

    QString currentProjectIdOrDefault() const {
        return m_currentProjectId.isEmpty() ? QStringLiteral("default") : m_currentProjectId;
    }

    QString projectPath(const QString& projectId) const {
        return QDir(projectsDirPath()).filePath(projectId.isEmpty() ? QStringLiteral("default") : projectId);
    }

    QString stateFilePathForProject(const QString& projectId) const {
        return QDir(projectPath(projectId)).filePath(QStringLiteral("state.json"));
    }

    QString historyFilePathForProject(const QString& projectId) const {
        return QDir(projectPath(projectId)).filePath(QStringLiteral("history.json"));
    }

    QString stateFilePath() const {
        return stateFilePathForProject(currentProjectIdOrDefault());
    }

    QString historyFilePath() const {
        return historyFilePathForProject(currentProjectIdOrDefault());
    }

    QString sanitizedProjectId(const QString& name) const {
        QString id = name.trimmed().toLower();
        for (QChar& ch : id) {
            if (!(ch.isLetterOrNumber() || ch == QLatin1Char('_') || ch == QLatin1Char('-'))) {
                ch = QLatin1Char('-');
            }
        }
        while (id.contains(QStringLiteral("--"))) {
            id.replace(QStringLiteral("--"), QStringLiteral("-"));
        }
        id.remove(QRegularExpression(QStringLiteral("^-+|-+$")));
        if (id.isEmpty()) {
            id = QStringLiteral("project");
        }
        QString uniqueId = id;
        int suffix = 2;
        while (QFileInfo::exists(projectPath(uniqueId))) {
            uniqueId = QStringLiteral("%1-%2").arg(id).arg(suffix++);
        }
        return uniqueId;
    }

    void ensureProjectsDirectory() const {
        QDir().mkpath(projectsDirPath());
    }

    QStringList availableProjectIds() const {
        ensureProjectsDirectory();
        const QFileInfoList entries = QDir(projectsDirPath()).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot,
                                                                            QDir::Name | QDir::IgnoreCase);
        QStringList ids;
        ids.reserve(entries.size());
        for (const QFileInfo& entry : entries) {
            ids.push_back(entry.fileName());
        }
        return ids;
    }

    void ensureDefaultProjectExists() const {
        ensureProjectsDirectory();
        QDir().mkpath(projectPath(QStringLiteral("default")));
    }

    void loadProjectsFromFolders() {
        ensureDefaultProjectExists();
        QFile markerFile(currentProjectMarkerPath());
        if (markerFile.open(QIODevice::ReadOnly)) {
            m_currentProjectId = QString::fromUtf8(markerFile.readAll()).trimmed();
        }
        const QStringList projectIds = availableProjectIds();
        if (projectIds.isEmpty()) {
            m_currentProjectId = QStringLiteral("default");
            return;
        }
        if (m_currentProjectId.isEmpty() || !projectIds.contains(m_currentProjectId)) {
            m_currentProjectId = projectIds.contains(QStringLiteral("default"))
                ? QStringLiteral("default")
                : projectIds.constFirst();
        }
        QSaveFile marker(currentProjectMarkerPath());
        if (marker.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            const QByteArray payload = m_currentProjectId.toUtf8();
            if (marker.write(payload) == payload.size()) {
                marker.commit();
            } else {
                marker.cancelWriting();
            }
        }
    }

    void saveCurrentProjectMarker() {
        ensureProjectsDirectory();
        QSaveFile file(currentProjectMarkerPath());
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            return;
        }
        const QByteArray payload = currentProjectIdOrDefault().toUtf8();
        if (file.write(payload) != payload.size()) {
            file.cancelWriting();
            return;
        }
        file.commit();
    }

    QString currentProjectName() const {
        return currentProjectIdOrDefault();
    }

    void refreshProjectsList() {
        if (!m_projectsList) {
            return;
        }
        loadProjectsFromFolders();
        m_updatingProjectsList = true;
        m_projectsList->clear();
        const QStringList projectIds = availableProjectIds();
        for (const QString& projectId : projectIds) {
            auto* item = new QListWidgetItem(projectId, m_projectsList);
            item->setData(Qt::UserRole, projectId);
            item->setToolTip(QDir::toNativeSeparators(projectPath(projectId)));
            if (projectId == currentProjectIdOrDefault()) {
                item->setSelected(true);
            }
        }
        if (m_projectSectionLabel) {
            m_projectSectionLabel->setText(QStringLiteral("PROJECTS  %1").arg(currentProjectName()));
        }
        m_updatingProjectsList = false;
    }

    void switchToProject(const QString& projectId) {
        if (projectId.isEmpty() || projectId == currentProjectIdOrDefault()) {
            refreshProjectsList();
            return;
        }
        saveStateNow();
        saveHistoryNow();
        m_currentProjectId = projectId;
        m_lastSavedState.clear();
        m_historyEntries = QJsonArray();
        m_historyIndex = -1;
        saveCurrentProjectMarker();
        loadState();
        refreshProjectsList();
        refreshInspector();
    }

    void createProject() {
        bool accepted = false;
        const QString name = QInputDialog::getText(this,
                                                   QStringLiteral("New Project"),
                                                   QStringLiteral("Project name"),
                                                   QLineEdit::Normal,
                                                   QStringLiteral("Untitled Project"),
                                                   &accepted).trimmed();
        if (!accepted || name.isEmpty()) {
            return;
        }
        const QString projectId = sanitizedProjectId(name);
        QDir().mkpath(projectPath(projectId));
        switchToProject(projectId);
    }

    bool saveProjectPayload(const QString& projectId,
                            const QByteArray& statePayload,
                            const QByteArray& historyPayload) {
        ensureProjectsDirectory();
        QDir().mkpath(projectPath(projectId));

        QSaveFile stateFile(stateFilePathForProject(projectId));
        if (!stateFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            return false;
        }
        if (stateFile.write(statePayload) != statePayload.size()) {
            stateFile.cancelWriting();
            return false;
        }
        if (!stateFile.commit()) {
            return false;
        }

        QSaveFile historyFile(historyFilePathForProject(projectId));
        if (!historyFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            return false;
        }
        if (historyFile.write(historyPayload) != historyPayload.size()) {
            historyFile.cancelWriting();
            return false;
        }
        return historyFile.commit();
    }

    void saveProjectAs() {
        if (!m_timeline) {
            return;
        }

        bool accepted = false;
        const QString name = QInputDialog::getText(this,
                                                   QStringLiteral("Save Project As"),
                                                   QStringLiteral("Project name"),
                                                   QLineEdit::Normal,
                                                   currentProjectName() == QStringLiteral("Default Project")
                                                       ? QStringLiteral("Untitled Project")
                                                       : currentProjectName(),
                                                   &accepted).trimmed();
        if (!accepted || name.isEmpty()) {
            return;
        }

        saveStateNow();
        saveHistoryNow();

        const QString newProjectId = sanitizedProjectId(name);
        const QByteArray statePayload = QJsonDocument(buildStateJson()).toJson(QJsonDocument::Indented);
        QJsonObject historyRoot;
        historyRoot[QStringLiteral("index")] = m_historyIndex;
        historyRoot[QStringLiteral("entries")] = m_historyEntries;
        const QByteArray historyPayload = QJsonDocument(historyRoot).toJson(QJsonDocument::Indented);

        if (!saveProjectPayload(newProjectId, statePayload, historyPayload)) {
            QMessageBox::warning(this,
                                 QStringLiteral("Save Project As Failed"),
                                 QStringLiteral("Could not write the new project files."));
            return;
        }

        switchToProject(newProjectId);
    }

    void renameProject(const QString& projectId) {
        if (projectId.isEmpty() || !QFileInfo::exists(projectPath(projectId))) {
            return;
        }
        bool accepted = false;
        const QString name = QInputDialog::getText(this,
                                                   QStringLiteral("Rename Project"),
                                                   QStringLiteral("Project name"),
                                                   QLineEdit::Normal,
                                                   projectId,
                                                   &accepted).trimmed();
        if (!accepted || name.isEmpty()) {
            return;
        }
        const QString renamedProjectId = sanitizedProjectId(name);
        if (renamedProjectId == projectId) {
            return;
        }
        QDir projectsDir(projectsDirPath());
        if (!projectsDir.rename(projectId, renamedProjectId)) {
            QMessageBox::warning(this,
                                 QStringLiteral("Rename Project Failed"),
                                 QStringLiteral("Could not rename the project folder."));
            return;
        }
        if (m_currentProjectId == projectId) {
            m_currentProjectId = renamedProjectId;
            saveCurrentProjectMarker();
        }
        refreshProjectsList();
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
        root[QStringLiteral("exportOnlyTranscriptWords")] =
            m_exportOnlyTranscriptWordsCheckBox ? m_exportOnlyTranscriptWordsCheckBox->isChecked() : false;
        root[QStringLiteral("selectedInspectorTab")] =
            m_inspectorTabs ? m_inspectorTabs->currentIndex() : 0;
        root[QStringLiteral("timelineZoom")] = m_timeline ? m_timeline->timelineZoom() : 4.0;
        root[QStringLiteral("timelineVerticalScroll")] =
            m_timeline ? m_timeline->verticalScrollOffset() : 0;
        root[QStringLiteral("exportStartFrame")] = m_timeline ? static_cast<qint64>(m_timeline->exportStartFrame()) : 0;
        root[QStringLiteral("exportEndFrame")] =
            m_timeline ? static_cast<qint64>(m_timeline->exportEndFrame()) : 300;
        QJsonArray exportRanges;
        if (m_timeline) {
            for (const ExportRangeSegment& range : m_timeline->exportRanges()) {
                QJsonObject rangeObj;
                rangeObj[QStringLiteral("startFrame")] = static_cast<qint64>(range.startFrame);
                rangeObj[QStringLiteral("endFrame")] = static_cast<qint64>(range.endFrame);
                exportRanges.push_back(rangeObj);
            }
        }
        root[QStringLiteral("exportRanges")] = exportRanges;

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
                markerObj[QStringLiteral("clipId")] = marker.clipId;
                markerObj[QStringLiteral("frame")] = static_cast<qint64>(marker.frame);
                markerObj[QStringLiteral("action")] = renderSyncActionToString(marker.action);
                markerObj[QStringLiteral("count")] = marker.count;
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
        ensureProjectsDirectory();
        QDir().mkpath(projectPath(currentProjectIdOrDefault()));

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
        ensureProjectsDirectory();
        QDir().mkpath(projectPath(currentProjectIdOrDefault()));
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
        const bool exportOnlyTranscriptWords =
            root.value(QStringLiteral("exportOnlyTranscriptWords")).toBool(false);
        const int selectedInspectorTab = root.value(QStringLiteral("selectedInspectorTab")).toInt(0);
        const qreal timelineZoom = root.value(QStringLiteral("timelineZoom")).toDouble(4.0);
        const int timelineVerticalScroll = root.value(QStringLiteral("timelineVerticalScroll")).toInt(0);
        const int64_t exportStartFrame = root.value(QStringLiteral("exportStartFrame")).toVariant().toLongLong();
        const int64_t exportEndFrame = root.value(QStringLiteral("exportEndFrame")).toVariant().toLongLong();
        QVector<ExportRangeSegment> loadedExportRanges;
        const QJsonArray exportRanges = root.value(QStringLiteral("exportRanges")).toArray();
        loadedExportRanges.reserve(exportRanges.size());
        for (const QJsonValue& value : exportRanges) {
            if (!value.isObject()) {
                continue;
            }
            const QJsonObject obj = value.toObject();
            ExportRangeSegment range;
            range.startFrame = qMax<int64_t>(0, obj.value(QStringLiteral("startFrame")).toVariant().toLongLong());
            range.endFrame = qMax<int64_t>(0, obj.value(QStringLiteral("endFrame")).toVariant().toLongLong());
            loadedExportRanges.push_back(range);
        }
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
            marker.clipId = obj.value(QStringLiteral("clipId")).toString();
            marker.frame = qMax<int64_t>(0, obj.value(QStringLiteral("frame")).toVariant().toLongLong());
            marker.action = renderSyncActionFromString(obj.value(QStringLiteral("action")).toString());
            marker.count = qMax(1, obj.value(QStringLiteral("count")).toInt(1));
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
        if (m_exportOnlyTranscriptWordsCheckBox) {
            QSignalBlocker block(m_exportOnlyTranscriptWordsCheckBox);
            m_exportOnlyTranscriptWordsCheckBox->setChecked(exportOnlyTranscriptWords);
        }
        if (m_inspectorTabs && m_inspectorTabs->count() > 0) {
            QSignalBlocker block(m_inspectorTabs);
            m_inspectorTabs->setCurrentIndex(qBound(0, selectedInspectorTab, m_inspectorTabs->count() - 1));
        }
        if (m_preview) {
            m_preview->setOutputSize(QSize(outputWidth, outputHeight));
        }

        m_timeline->setTracks(loadedTracks);
        m_timeline->setClips(loadedClips);
        m_timeline->setTimelineZoom(timelineZoom);
        m_timeline->setVerticalScrollOffset(timelineVerticalScroll);
        if (!loadedExportRanges.isEmpty()) {
            m_timeline->setExportRanges(loadedExportRanges);
        } else {
            m_timeline->setExportRange(exportStartFrame, exportEndFrame > 0 ? exportEndFrame : m_timeline->totalFrames());
        }
        m_timeline->setRenderSyncMarkers(loadedRenderSyncMarkers);
        m_timeline->setSelectedClipId(selectedClipId);
        syncSliderRange();
        m_preview->setClipCount(m_timeline->clips().size());
        m_preview->setTimelineClips(m_timeline->clips());
        m_preview->setRenderSyncMarkers(m_timeline->renderSyncMarkers());
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
            refreshProjectsList();
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

    void showExplorerHoverPreview(const QString& filePath) {
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
        QSize targetSize(280, 180);
        if (m_preview) {
            const QSize previewSize = m_preview->size() - QSize(32, 32);
            if (previewSize.width() > 0 && previewSize.height() > 0) {
                targetSize = previewSize;
            }
        }
        const QPixmap scaled = source.scaled(targetSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        m_explorerHoverPreview->setPixmap(scaled);
        m_explorerHoverPreview->resize(scaled.size() + QSize(16, 16));
        QPoint previewAnchor(80, 80);
        if (m_preview) {
            const QRect previewRect = m_preview->rect();
            previewAnchor = m_preview->mapToGlobal(
                QPoint(qMax(24, (previewRect.width() - m_explorerHoverPreview->width()) / 2),
                       qMax(24, previewRect.height() / 8)));
        }
        m_explorerHoverPreview->move(previewAnchor);
        m_explorerHoverPreview->show();
        m_explorerHoverPreview->raise();
    }

    void openTranscriptionWindow(const QString& filePath, const QString& label) {
        const QFileInfo inputInfo(filePath);
        if (!inputInfo.exists() || !inputInfo.isFile()) {
            QMessageBox::warning(this,
                                 QStringLiteral("Transcribe Failed"),
                                 QStringLiteral("The selected file does not exist:\n%1")
                                     .arg(QDir::toNativeSeparators(filePath)));
            return;
        }

        const QString scriptPath = QDir(QDir::currentPath()).absoluteFilePath(QStringLiteral("whisperx.sh"));
        if (!QFileInfo::exists(scriptPath)) {
            QMessageBox::warning(this,
                                 QStringLiteral("Transcribe Failed"),
                                 QStringLiteral("whisperx.sh was not found at:\n%1")
                                     .arg(QDir::toNativeSeparators(scriptPath)));
            return;
        }

        auto* dialog = new QDialog(this);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setWindowTitle(QStringLiteral("Transcribe  %1").arg(label));
        dialog->resize(920, 560);

        auto* layout = new QVBoxLayout(dialog);
        layout->setContentsMargins(12, 12, 12, 12);
        layout->setSpacing(8);

        auto* title = new QLabel(QStringLiteral("whisperx.sh %1").arg(QDir::toNativeSeparators(filePath)), dialog);
        title->setWordWrap(true);
        layout->addWidget(title);

        auto* output = new QPlainTextEdit(dialog);
        output->setReadOnly(true);
        output->setLineWrapMode(QPlainTextEdit::NoWrap);
        output->setStyleSheet(QStringLiteral(
            "QPlainTextEdit { background: #05080c; color: #d6e2ee; border: 1px solid #24303c; "
            "border-radius: 8px; font-family: monospace; font-size: 12px; }"));
        layout->addWidget(output, 1);

        auto* inputRow = new QHBoxLayout;
        inputRow->setContentsMargins(0, 0, 0, 0);
        inputRow->setSpacing(8);
        auto* inputLabel = new QLabel(QStringLiteral("stdin"), dialog);
        auto* inputLine = new QLineEdit(dialog);
        inputLine->setPlaceholderText(QStringLiteral("Type input for whisperx.sh prompts, then press Send"));
        auto* sendButton = new QPushButton(QStringLiteral("Send"), dialog);
        auto* closeButton = new QPushButton(QStringLiteral("Close"), dialog);
        inputRow->addWidget(inputLabel);
        inputRow->addWidget(inputLine, 1);
        inputRow->addWidget(sendButton);
        inputRow->addWidget(closeButton);
        layout->addLayout(inputRow);

        auto* process = new QProcess(dialog);
        process->setProcessChannelMode(QProcess::MergedChannels);
        process->setWorkingDirectory(QDir::currentPath());

        const auto appendOutput = [output](const QString& text) {
            if (text.isEmpty()) {
                return;
            }
            output->moveCursor(QTextCursor::End);
            output->insertPlainText(text);
            output->moveCursor(QTextCursor::End);
        };

        connect(process, &QProcess::readyReadStandardOutput, dialog, [process, appendOutput]() {
            appendOutput(QString::fromLocal8Bit(process->readAllStandardOutput()));
        });
        connect(process, &QProcess::started, dialog, [appendOutput, filePath]() {
            appendOutput(QStringLiteral("$ ./whisperx.sh \"%1\"\n").arg(QDir::toNativeSeparators(filePath)));
        });
        connect(process, &QProcess::errorOccurred, dialog, [appendOutput](QProcess::ProcessError error) {
            appendOutput(QStringLiteral("\n[process error] %1\n").arg(static_cast<int>(error)));
        });
        connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), dialog,
                [appendOutput](int exitCode, QProcess::ExitStatus exitStatus) {
                    appendOutput(QStringLiteral("\n[process finished] exitCode=%1 status=%2\n")
                                     .arg(exitCode)
                                     .arg(exitStatus == QProcess::NormalExit ? QStringLiteral("normal")
                                                                             : QStringLiteral("crashed")));
                });
        connect(sendButton, &QPushButton::clicked, dialog, [process, inputLine, appendOutput]() {
            const QString text = inputLine->text();
            if (text.isEmpty()) {
                return;
            }
            process->write(text.toUtf8());
            process->write("\n");
            appendOutput(QStringLiteral("> %1\n").arg(text));
            inputLine->clear();
        });
        connect(inputLine, &QLineEdit::returnPressed, sendButton, &QPushButton::click);
        connect(closeButton, &QPushButton::clicked, dialog, &QDialog::close);
        connect(dialog, &QDialog::finished, dialog, [process](int) {
            if (process->state() != QProcess::NotRunning) {
                process->kill();
                process->waitForFinished(1000);
            }
        });

        process->start(QStringLiteral("/bin/bash"),
                       {scriptPath, QFileInfo(filePath).absoluteFilePath()});
        dialog->show();
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

    QString transcriptPathForClip(const TimelineClip& clip) const {
        return transcriptPathForClipFile(clip.filePath);
    }

    QString secondsToTranscriptTime(double seconds) const {
        const qint64 millis = qMax<qint64>(0, qRound64(seconds * 1000.0));
        const qint64 totalSeconds = millis / 1000;
        const qint64 minutes = totalSeconds / 60;
        const qint64 secs = totalSeconds % 60;
        const qint64 ms = millis % 1000;
        return QStringLiteral("%1:%2.%3")
            .arg(minutes, 2, 10, QLatin1Char('0'))
            .arg(secs, 2, 10, QLatin1Char('0'))
            .arg(ms, 3, 10, QLatin1Char('0'));
    }

    bool parseTranscriptTime(const QString& text, double* secondsOut) const {
        bool ok = false;
        const double numericValue = text.toDouble(&ok);
        if (ok) {
            *secondsOut = qMax(0.0, numericValue);
            return true;
        }

        const QString trimmed = text.trimmed();
        const QStringList minuteParts = trimmed.split(QLatin1Char(':'));
        if (minuteParts.size() != 2) {
            return false;
        }
        const int minutes = minuteParts[0].toInt(&ok);
        if (!ok) {
            return false;
        }
        const double secValue = minuteParts[1].toDouble(&ok);
        if (!ok) {
            return false;
        }
        *secondsOut = qMax(0.0, minutes * 60.0 + secValue);
        return true;
    }

    bool saveTranscriptJson(const QString& path, const QJsonDocument& doc) const {
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            return false;
        }
        const QByteArray payload = doc.toJson(QJsonDocument::Indented);
        if (file.write(payload) != payload.size()) {
            file.cancelWriting();
            return false;
        }
        return file.commit();
    }

    int64_t adjustedLocalFrameForClip(const TimelineClip& clip,
                                      int64_t localTimelineFrame,
                                      const QVector<RenderSyncMarker>& markers) const {
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

    void appendMergedExportFrame(QVector<ExportRangeSegment>& ranges, int64_t frame) const {
        if (ranges.isEmpty() || frame > ranges.constLast().endFrame + 1) {
            ranges.push_back(ExportRangeSegment{frame, frame});
            return;
        }
        ranges.last().endFrame = qMax(ranges.last().endFrame, frame);
    }

    // Cache for transcript word ranges to avoid re-parsing JSON on every frame
    mutable QHash<QString, QVector<ExportRangeSegment>> m_transcriptWordRangesCache;
    mutable qint64 m_transcriptWordRangesCacheVersion = -1;

    QVector<ExportRangeSegment> transcriptWordExportRanges(const QVector<ExportRangeSegment>& baseRanges,
                                                           const QVector<TimelineClip>& clips,
                                                           const QVector<RenderSyncMarker>& markers) const {
        // Check if cache is still valid (based on timeline clip count and version)
        const qint64 currentVersion = m_timeline ? m_timeline->clips().size() : 0;
        if (m_transcriptWordRangesCacheVersion == currentVersion && !m_transcriptWordRangesCache.isEmpty()) {
            // Return cached ranges (they're already mapped to timeline frames)
            QVector<ExportRangeSegment> result;
            for (auto it = m_transcriptWordRangesCache.constBegin(); it != m_transcriptWordRangesCache.constEnd(); ++it) {
                result.append(it.value());
            }
            // Merge overlapping ranges from different clips
            std::sort(result.begin(), result.end(),
                      [](const ExportRangeSegment& a, const ExportRangeSegment& b) {
                          return a.startFrame < b.startFrame;
                      });
            QVector<ExportRangeSegment> merged;
            for (const auto& range : result) {
                if (merged.isEmpty() || range.startFrame > merged.last().endFrame + 1) {
                    merged.append(range);
                } else {
                    merged.last().endFrame = qMax(merged.last().endFrame, range.endFrame);
                }
            }
            return merged;
        }

        m_transcriptWordRangesCache.clear();
        m_transcriptWordRangesCacheVersion = currentVersion;

        QVector<ExportRangeSegment> resolvedBaseRanges = baseRanges;
        if (resolvedBaseRanges.isEmpty()) {
            int64_t endFrame = 0;
            for (const TimelineClip& clip : clips) {
                endFrame = qMax(endFrame, clip.startFrame + clip.durationFrames);
            }
            resolvedBaseRanges.push_back(ExportRangeSegment{0, endFrame});
        }

        QVector<ExportRangeSegment> allTranscriptRanges;

        for (const TimelineClip& clip : clips) {
            if (clip.durationFrames <= 0) {
                continue;
            }

            QFile transcriptFile(transcriptPathForClip(clip));
            if (!transcriptFile.open(QIODevice::ReadOnly)) {
                continue;
            }

            QJsonParseError parseError;
            const QJsonDocument transcriptDoc =
                QJsonDocument::fromJson(transcriptFile.readAll(), &parseError);
            if (parseError.error != QJsonParseError::NoError || !transcriptDoc.isObject()) {
                continue;
            }

            // Build source word ranges from transcript
            QVector<ExportRangeSegment> sourceWordRanges;
            const QJsonArray segments = transcriptDoc.object().value(QStringLiteral("segments")).toArray();
            for (const QJsonValue& segmentValue : segments) {
                const QJsonArray words = segmentValue.toObject().value(QStringLiteral("words")).toArray();
                for (const QJsonValue& wordValue : words) {
                    const QJsonObject wordObj = wordValue.toObject();
                    if (wordObj.value(QStringLiteral("word")).toString().trimmed().isEmpty()) {
                        continue;
                    }

                    double startSeconds = wordObj.value(QStringLiteral("start")).toDouble(-1.0);
                    const double endSeconds = wordObj.value(QStringLiteral("end")).toDouble(-1.0);
                    if (startSeconds < 0.0 || endSeconds < startSeconds) {
                        continue;
                    }

                    // Apply runtime prepend offset (in seconds)
                    const double prependSeconds = m_transcriptPrependMs / 1000.0;
                    startSeconds = qMax(0.0, startSeconds + prependSeconds);
                    // Clamp to not overlap with previous word (handled by sorting/merging later)

                    const int64_t startFrame =
                        qMax<int64_t>(0, static_cast<int64_t>(std::floor(startSeconds * kTimelineFps)));
                    const int64_t endFrame =
                        qMax<int64_t>(startFrame, static_cast<int64_t>(std::ceil(endSeconds * kTimelineFps)) - 1);
                    sourceWordRanges.push_back(ExportRangeSegment{startFrame, endFrame});
                }
            }

            if (sourceWordRanges.isEmpty()) {
                continue;
            }

            // Sort and merge overlapping word ranges at source level
            std::sort(sourceWordRanges.begin(), sourceWordRanges.end(),
                      [](const ExportRangeSegment& a, const ExportRangeSegment& b) {
                          if (a.startFrame == b.startFrame) {
                              return a.endFrame < b.endFrame;
                          }
                          return a.startFrame < b.startFrame;
                      });
            QVector<ExportRangeSegment> mergedSourceWordRanges;
            for (const ExportRangeSegment& wordRange : std::as_const(sourceWordRanges)) {
                if (mergedSourceWordRanges.isEmpty() ||
                    wordRange.startFrame > mergedSourceWordRanges.constLast().endFrame + 1) {
                    mergedSourceWordRanges.push_back(wordRange);
                } else {
                    mergedSourceWordRanges.last().endFrame =
                        qMax(mergedSourceWordRanges.last().endFrame, wordRange.endFrame);
                }
            }

            // Map source word ranges to timeline ranges efficiently (O(n) instead of O(n*m))
            QVector<ExportRangeSegment> clipTimelineRanges;

            for (const ExportRangeSegment& baseRange : resolvedBaseRanges) {
                const int64_t clipStart = qMax<int64_t>(clip.startFrame, baseRange.startFrame);
                const int64_t clipEnd =
                    qMin<int64_t>(clip.startFrame + clip.durationFrames - 1, baseRange.endFrame);
                if (clipEnd < clipStart) {
                    continue;
                }

                // For each source word range, map it to timeline frames
                for (const ExportRangeSegment& wordRange : std::as_const(mergedSourceWordRanges)) {
                    // Map source word range to timeline
                    // sourceFrame = clip.sourceInFrame + adjustedLocalFrameForClip(clip, localTimelineFrame, markers)
                    // So we need to find timeline frames where sourceFrame is within wordRange

                    // This is a simplified mapping that assumes linear time mapping
                    // For more complex mappings with markers, we'd need inverse mapping
                    const int64_t wordStartInSource = wordRange.startFrame;
                    const int64_t wordEndInSource = wordRange.endFrame;

                    // Map source range to timeline range (clip-relative)
                    int64_t localStart = wordStartInSource - clip.sourceInFrame;
                    int64_t localEnd = wordEndInSource - clip.sourceInFrame;

                    // Clamp to clip duration
                    localStart = qMax<int64_t>(0, localStart);
                    localEnd = qMin<int64_t>(clip.durationFrames - 1, localEnd);

                    if (localEnd < localStart) {
                        continue;
                    }

                    // Map to absolute timeline frames
                    int64_t timelineStart = clip.startFrame + localStart;
                    int64_t timelineEnd = clip.startFrame + localEnd;

                    // Intersect with base range
                    timelineStart = qMax(timelineStart, clipStart);
                    timelineEnd = qMin(timelineEnd, clipEnd);

                    if (timelineEnd >= timelineStart) {
                        clipTimelineRanges.push_back(ExportRangeSegment{timelineStart, timelineEnd});
                    }
                }
            }

            // Cache ranges for this clip
            if (!clipTimelineRanges.isEmpty()) {
                m_transcriptWordRangesCache[clip.id] = clipTimelineRanges;
                allTranscriptRanges.append(clipTimelineRanges);
            }
        }

        // Sort and merge all ranges from all clips
        std::sort(allTranscriptRanges.begin(), allTranscriptRanges.end(),
                  [](const ExportRangeSegment& a, const ExportRangeSegment& b) {
                      return a.startFrame < b.startFrame;
                  });
        QVector<ExportRangeSegment> merged;
        for (const auto& range : allTranscriptRanges) {
            if (merged.isEmpty() || range.startFrame > merged.last().endFrame + 1) {
                merged.append(range);
            } else {
                merged.last().endFrame = qMax(merged.last().endFrame, range.endFrame);
            }
        }
        return merged;
    }

    QString clipLabelForId(const QString& clipId) const {
        if (!m_timeline) {
            return clipId;
        }
        for (const TimelineClip& clip : m_timeline->clips()) {
            if (clip.id == clipId) {
                return clip.label;
            }
        }
        return clipId;
    }

    QColor clipColorForId(const QString& clipId) const {
        if (!m_timeline) {
            return QColor(QStringLiteral("#24303c"));
        }
        for (const TimelineClip& clip : m_timeline->clips()) {
            if (clip.id == clipId) {
                return clip.color;
            }
        }
        return QColor(QStringLiteral("#24303c"));
    }

    bool parseSyncActionText(const QString& text, RenderSyncAction* actionOut) const {
        const QString normalized = text.trimmed().toLower();
        if (normalized == QStringLiteral("duplicate") || normalized == QStringLiteral("dup")) {
            *actionOut = RenderSyncAction::DuplicateFrame;
            return true;
        }
        if (normalized == QStringLiteral("skip")) {
            *actionOut = RenderSyncAction::SkipFrame;
            return true;
        }
        return false;
    }

    void refreshSyncInspector() {
        m_syncInspectorClipLabel->setText(QStringLiteral("Sync"));
        m_updatingSyncInspector = true;
        m_syncTable->clearContents();
        m_syncTable->setRowCount(0);
        if (!m_timeline || m_timeline->renderSyncMarkers().isEmpty()) {
            m_syncInspectorDetailsLabel->setText(QStringLiteral("No render sync markers in the timeline."));
            m_updatingSyncInspector = false;
            return;
        }

        const QVector<RenderSyncMarker> markers = m_timeline->renderSyncMarkers();
        m_syncInspectorDetailsLabel->setText(
            QStringLiteral("%1 sync markers across the timeline. Edit Frame, Count, or Action directly.")
                .arg(markers.size()));
        m_syncTable->setRowCount(markers.size());
        for (int i = 0; i < markers.size(); ++i) {
            const RenderSyncMarker& marker = markers[i];
            const QColor clipColor = clipColorForId(marker.clipId);
            const QColor rowBackground = QColor(clipColor.red(), clipColor.green(), clipColor.blue(), 72);
            const QColor rowForeground = QColor(QStringLiteral("#f4f7fb"));
            const QString clipLabel = clipLabelForId(marker.clipId);
            auto* clipItem = new QTableWidgetItem(QString());
            clipItem->setData(Qt::UserRole, marker.clipId);
            clipItem->setData(Qt::UserRole + 1, QVariant::fromValue(static_cast<qint64>(marker.frame)));
            clipItem->setFlags(clipItem->flags() & ~Qt::ItemIsEditable);
            clipItem->setToolTip(clipLabel);
            auto* frameItem = new QTableWidgetItem(QString::number(marker.frame));
            auto* countItem = new QTableWidgetItem(QString::number(marker.count));
            auto* actionItem = new QTableWidgetItem(
                marker.action == RenderSyncAction::DuplicateFrame ? QStringLiteral("Duplicate")
                                                                  : QStringLiteral("Skip"));
            for (QTableWidgetItem* item : {clipItem, frameItem, countItem, actionItem}) {
                item->setBackground(rowBackground);
                item->setForeground(rowForeground);
            }
            m_syncTable->setItem(i, 0, clipItem);
            m_syncTable->setItem(i, 1, frameItem);
            m_syncTable->setItem(i, 2, countItem);
            m_syncTable->setItem(i, 3, actionItem);
        }
        m_updatingSyncInspector = false;
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
        normalizeClipTransformKeyframes(clip);
        return clip;
    }
    
    void loadState() {
        loadProjectsFromFolders();
        m_historyEntries = QJsonArray();
        m_historyIndex = -1;
        m_lastSavedState.clear();
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
                          "QToolButton#projectAction { background: transparent; border: 1px solid #24303c; border-radius: 8px; color: #dce5ee; padding: 4px 8px; }"
                          "QToolButton#projectAction:hover { background: #1a2330; color: #ffffff; }"
                          "QLabel#rootPath { color: #8ea0b2; font-size: 11px; letter-spacing: 0; }"
                          "QTreeView { background: transparent; border: none; color: #dbe2ea; }"
                          "QTreeView::item { padding: 4px 0; }"
                          "QTreeView::item:selected { background: #213042; color: #f7fbff; }"
                          "QListWidget#projectsList { background: transparent; border: 1px solid #24303c; border-radius: 8px; color: #dbe2ea; }"
                          "QListWidget#projectsList::item { padding: 6px 8px; }"
                          "QListWidget#projectsList::item:selected { background: #213042; color: #f7fbff; }"));
        
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

        auto* projectsHeader = new QHBoxLayout;
        projectsHeader->setContentsMargins(0, 0, 0, 0);
        projectsHeader->setSpacing(8);
        m_projectSectionLabel = new QLabel(QStringLiteral("PROJECTS"), pane);
        projectsHeader->addWidget(m_projectSectionLabel);
        projectsHeader->addStretch(1);
        m_saveProjectAsButton = new QToolButton(pane);
        m_saveProjectAsButton->setObjectName(QStringLiteral("projectAction"));
        m_saveProjectAsButton->setText(QStringLiteral("Save As"));
        m_saveProjectAsButton->setCursor(Qt::PointingHandCursor);
        projectsHeader->addWidget(m_saveProjectAsButton);
        m_newProjectButton = new QToolButton(pane);
        m_newProjectButton->setObjectName(QStringLiteral("projectAction"));
        m_newProjectButton->setText(QStringLiteral("New Project"));
        m_newProjectButton->setCursor(Qt::PointingHandCursor);
        projectsHeader->addWidget(m_newProjectButton);
        layout->addLayout(projectsHeader);

        m_projectsList = new QListWidget(pane);
        m_projectsList->setObjectName(QStringLiteral("projectsList"));
        m_projectsList->setSelectionMode(QAbstractItemView::SingleSelection);
        m_projectsList->setContextMenuPolicy(Qt::CustomContextMenu);
        m_projectsList->setMaximumHeight(160);
        layout->addWidget(m_projectsList, 0);
        
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
            showExplorerHoverPreview(info.absoluteFilePath());
        });
        connect(m_tree, &QAbstractItemView::viewportEntered, this, [this]() {
            hideExplorerHoverPreview();
        });
        connect(m_newProjectButton, &QToolButton::clicked, this, [this]() {
            createProject();
        });
        connect(m_saveProjectAsButton, &QToolButton::clicked, this, [this]() {
            saveProjectAs();
        });
        connect(m_projectsList, &QListWidget::itemSelectionChanged, this, [this]() {
            if (m_updatingProjectsList || !m_projectsList) {
                return;
            }
            QListWidgetItem* item = m_projectsList->currentItem();
            if (!item) {
                return;
            }
            switchToProject(item->data(Qt::UserRole).toString());
        });
        connect(m_projectsList, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* item) {
            if (!item) {
                return;
            }
            renameProject(item->data(Qt::UserRole).toString());
        });
        connect(m_projectsList, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
            if (!m_projectsList) {
                return;
            }
            QListWidgetItem* item = m_projectsList->itemAt(pos);
            if (!item) {
                return;
            }
            QMenu menu(m_projectsList);
            QAction* saveAsAction = menu.addAction(QStringLiteral("Save Project As..."));
            QAction* renameAction = menu.addAction(QStringLiteral("Rename"));
            QAction* chosen = menu.exec(m_projectsList->viewport()->mapToGlobal(pos));
            if (chosen == saveAsAction) {
                saveProjectAs();
            } else if (chosen == renameAction) {
                renameProject(item->data(Qt::UserRole).toString());
            }
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
            showExplorerHoverPreview(path);
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
        
        connect(m_playButton, &QPushButton::clicked, this, [this]() { togglePlayback(); });
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
            m_preview->setRenderSyncMarkers(m_timeline->renderSyncMarkers());
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
            m_preview->setRenderSyncMarkers(m_timeline->renderSyncMarkers());
            refreshInspector();
            scheduleSaveState();
            pushHistorySnapshot();
        };
        m_timeline->exportRangeChanged = [this]() {
            refreshInspector();
            scheduleSaveState();
            pushHistorySnapshot();
        };
        m_timeline->gradingRequested = [this]() {
            focusGradingTab();
            refreshInspector();
        };
        m_timeline->transcribeRequested = [this](const QString& filePath, const QString& label) {
            openTranscriptionWindow(filePath, label);
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
                if (clip.mediaType == ClipMediaType::Audio && clip.transcriptOverlay.enabled) {
                    clip.transcriptOverlay.boxWidth = qMax<qreal>(80.0, scaleX);
                    clip.transcriptOverlay.boxHeight = qMax<qreal>(40.0, scaleY);
                    return;
                }
                if (!clipHasVisuals(clip)) {
                    return;
                }
                const TimelineClip::TransformKeyframe offset = evaluateClipKeyframeOffsetAtFrame(clip, currentFrame);
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
                normalizeClipTransformKeyframes(clip);
            });
            if (!updated) {
                return;
            }
            m_preview->setTimelineClips(m_timeline->clips());
            m_preview->setRenderSyncMarkers(m_timeline->renderSyncMarkers());
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
                if (clip.mediaType == ClipMediaType::Audio && clip.transcriptOverlay.enabled) {
                    clip.transcriptOverlay.translationX = translationX;
                    clip.transcriptOverlay.translationY = translationY;
                    return;
                }
                if (!clipHasVisuals(clip)) {
                    return;
                }
                const TimelineClip::TransformKeyframe offset = evaluateClipKeyframeOffsetAtFrame(clip, currentFrame);
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
                normalizeClipTransformKeyframes(clip);
            });
            if (!updated) {
                return;
            }
            m_preview->setTimelineClips(m_timeline->clips());
            m_preview->setRenderSyncMarkers(m_timeline->renderSyncMarkers());
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
                           "QCheckBox { color: #e8edf2; spacing: 8px; }"
                           "QCheckBox::indicator { width: 16px; height: 16px; border: 1px solid #4b6074; border-radius: 4px; background: #17202a; }"
                           "QCheckBox::indicator:checked { background: #5da2ff; border-color: #5da2ff; }"
                           "QCheckBox::indicator:disabled { background: #141a20; border-color: #26323e; }"
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
        m_videoInspectorClipLabel = new QLabel(QStringLiteral("Output"), videoTab);
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
        m_exportOnlyTranscriptWordsCheckBox =
            new QCheckBox(QStringLiteral("Only export frames with transcript words"), videoTab);
        videoForm->addRow(QStringLiteral("Speech Filter"), m_exportOnlyTranscriptWordsCheckBox);
        m_exportStartSpin = new QSpinBox(videoTab);
        m_exportStartSpin->setRange(0, INT_MAX);
        videoForm->addRow(QStringLiteral("Export Start"), m_exportStartSpin);

        m_exportEndSpin = new QSpinBox(videoTab);
        m_exportEndSpin->setRange(0, INT_MAX);
        videoForm->addRow(QStringLiteral("Export End"), m_exportEndSpin);
        videoLayout->addLayout(videoForm);

        m_renderButton = new QPushButton(QStringLiteral("Render"), videoTab);
        m_renderButton->setObjectName(QStringLiteral("video.render"));
        videoLayout->addWidget(m_renderButton, 0, Qt::AlignLeft);
        videoLayout->addStretch(1);
        m_inspectorTabs->addTab(videoTab, QStringLiteral("Output"));

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
        m_keyframeSpaceCheckBox = new QCheckBox(QStringLiteral("Show Clip-Relative Values"), keyframesTab);
        m_keyframeSpaceCheckBox->setChecked(true);
        keyframesLayout->addWidget(m_keyframeSpaceCheckBox);
        auto* keyframeForm = new QFormLayout;
        m_videoTranslationXSpin = new QDoubleSpinBox(keyframesTab);
        m_videoTranslationXSpin->setRange(-4000.0, 4000.0);
        m_videoTranslationXSpin->setDecimals(1);
        m_videoTranslationXSpin->setSingleStep(10.0);
        keyframeForm->addRow(QStringLiteral("Translate X"), m_videoTranslationXSpin);

        m_videoTranslationYSpin = new QDoubleSpinBox(keyframesTab);
        m_videoTranslationYSpin->setRange(-4000.0, 4000.0);
        m_videoTranslationYSpin->setDecimals(1);
        m_videoTranslationYSpin->setSingleStep(10.0);
        keyframeForm->addRow(QStringLiteral("Translate Y"), m_videoTranslationYSpin);

        m_videoRotationSpin = new QDoubleSpinBox(keyframesTab);
        m_videoRotationSpin->setRange(-3600.0, 3600.0);
        m_videoRotationSpin->setDecimals(1);
        m_videoRotationSpin->setSingleStep(5.0);
        keyframeForm->addRow(QStringLiteral("Rotation"), m_videoRotationSpin);

        m_videoScaleXSpin = new QDoubleSpinBox(keyframesTab);
        m_videoScaleXSpin->setRange(-20.0, 20.0);
        m_videoScaleXSpin->setDecimals(3);
        m_videoScaleXSpin->setSingleStep(0.05);
        m_videoScaleXSpin->setValue(1.0);
        keyframeForm->addRow(QStringLiteral("Scale X"), m_videoScaleXSpin);

        m_videoScaleYSpin = new QDoubleSpinBox(keyframesTab);
        m_videoScaleYSpin->setRange(-20.0, 20.0);
        m_videoScaleYSpin->setDecimals(3);
        m_videoScaleYSpin->setSingleStep(0.05);
        m_videoScaleYSpin->setValue(1.0);
        keyframeForm->addRow(QStringLiteral("Scale Y"), m_videoScaleYSpin);
        m_mirrorHorizontalCheckBox = new QCheckBox(QStringLiteral("Mirror Horizontal"), keyframesTab);
        keyframeForm->addRow(QStringLiteral(""), m_mirrorHorizontalCheckBox);

        m_mirrorVerticalCheckBox = new QCheckBox(QStringLiteral("Mirror Vertical"), keyframesTab);
        keyframeForm->addRow(QStringLiteral(""), m_mirrorVerticalCheckBox);

        m_lockVideoScaleCheckBox = new QCheckBox(QStringLiteral("Lock X / Y Scale"), keyframesTab);
        keyframeForm->addRow(QStringLiteral(""), m_lockVideoScaleCheckBox);

        m_videoInterpolationCombo = new QComboBox(keyframesTab);
        m_videoInterpolationCombo->addItem(QStringLiteral("Interpolated"));
        m_videoInterpolationCombo->addItem(QStringLiteral("Sudden"));
        keyframeForm->addRow(QStringLiteral("To This Keyframe"), m_videoInterpolationCombo);
        keyframesLayout->addLayout(keyframeForm);
        m_addVideoKeyframeButton = new QPushButton(QStringLiteral("Add/Update Keyframe"), keyframesTab);
        keyframesLayout->addWidget(m_addVideoKeyframeButton, 0, Qt::AlignLeft);
        m_videoKeyframeTable = new QTableWidget(keyframesTab);
        m_videoKeyframeTable->setColumnCount(7);
        m_videoKeyframeTable->setHorizontalHeaderLabels(
            {QStringLiteral("Frame"), QStringLiteral("X"), QStringLiteral("Y"), QStringLiteral("Rot"),
             QStringLiteral("Scale X"), QStringLiteral("Scale Y"), QStringLiteral("Mode")});
        m_videoKeyframeTable->setSelectionBehavior(QAbstractItemView::SelectRows);
        m_videoKeyframeTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
        m_videoKeyframeTable->setEditTriggers(QAbstractItemView::DoubleClicked |
                                              QAbstractItemView::EditKeyPressed |
                                              QAbstractItemView::SelectedClicked);
        m_videoKeyframeTable->setContextMenuPolicy(Qt::CustomContextMenu);
        m_videoKeyframeTable->verticalHeader()->setVisible(false);
        m_videoKeyframeTable->horizontalHeader()->setStretchLastSection(true);
        m_videoKeyframeTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
        keyframesLayout->addWidget(m_videoKeyframeTable, 1);
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
        
        // Fade samples control for crossfade with previous clip
        auto* fadeLayout = new QHBoxLayout();
        fadeLayout->addWidget(new QLabel(QStringLiteral("Fade (samples):"), audioTab));
        m_audioFadeSamplesSpin = new QSpinBox(audioTab);
        m_audioFadeSamplesSpin->setRange(0, 10000);
        m_audioFadeSamplesSpin->setValue(250);
        m_audioFadeSamplesSpin->setSingleStep(10);
        m_audioFadeSamplesSpin->setToolTip(QStringLiteral("Crossfade with previous audio clip (0 = no fade)"));
        fadeLayout->addWidget(m_audioFadeSamplesSpin);
        fadeLayout->addStretch();
        audioLayout->addLayout(fadeLayout);
        
        connect(m_audioFadeSamplesSpin, qOverload<int>(&QSpinBox::valueChanged), this, [this](int value) {
            if (!m_timeline || m_updatingAudioInspector) {
                return;
            }
            const TimelineClip* clip = m_timeline->selectedClip();
            if (!clip || !clipIsAudioOnly(*clip)) {
                return;
            }
            m_timeline->updateClipById(clip->id, [value](TimelineClip& c) {
                c.fadeSamples = value;
            });
            scheduleSaveState();
            pushHistorySnapshot();
        });
        
        audioLayout->addStretch(1);
        m_inspectorTabs->addTab(audioTab, QStringLiteral("Audio"));

        auto* transcriptTab = new QWidget(m_inspectorTabs);
        auto* transcriptLayout = new QVBoxLayout(transcriptTab);
        transcriptLayout->setContentsMargins(12, 12, 12, 12);
        transcriptLayout->setSpacing(10);
        m_transcriptInspectorClipLabel = new QLabel(QStringLiteral("No transcript selected"), transcriptTab);
        m_transcriptInspectorClipLabel->setObjectName(QStringLiteral("inspectorTitle"));
        m_transcriptInspectorDetailsLabel = new QLabel(QStringLiteral("Select a clip with a WhisperX JSON transcript."), transcriptTab);
        m_transcriptInspectorDetailsLabel->setObjectName(QStringLiteral("inspectorMeta"));
        m_transcriptInspectorDetailsLabel->setWordWrap(true);
        transcriptLayout->addWidget(m_transcriptInspectorClipLabel);
        transcriptLayout->addWidget(m_transcriptInspectorDetailsLabel);
        auto* transcriptOverlayForm = new QFormLayout;
        m_transcriptOverlayEnabledCheckBox = new QCheckBox(QStringLiteral("Show Transcript Overlay"), transcriptTab);
        transcriptOverlayForm->addRow(QStringLiteral("Overlay"), m_transcriptOverlayEnabledCheckBox);
        m_transcriptMaxLinesSpin = new QSpinBox(transcriptTab);
        m_transcriptMaxLinesSpin->setRange(1, 6);
        transcriptOverlayForm->addRow(QStringLiteral("Line Count"), m_transcriptMaxLinesSpin);
        m_transcriptMaxCharsSpin = new QSpinBox(transcriptTab);
        m_transcriptMaxCharsSpin->setRange(4, 80);
        transcriptOverlayForm->addRow(QStringLiteral("Max Chars/Line"), m_transcriptMaxCharsSpin);
        m_transcriptAutoScrollCheckBox = new QCheckBox(QStringLiteral("Auto-scroll current word"), transcriptTab);
        transcriptOverlayForm->addRow(QStringLiteral(""), m_transcriptAutoScrollCheckBox);
        m_transcriptOverlayXSpin = new QDoubleSpinBox(transcriptTab);
        m_transcriptOverlayXSpin->setRange(-4000.0, 4000.0);
        m_transcriptOverlayXSpin->setDecimals(1);
        transcriptOverlayForm->addRow(QStringLiteral("Position X"), m_transcriptOverlayXSpin);
        m_transcriptOverlayYSpin = new QDoubleSpinBox(transcriptTab);
        m_transcriptOverlayYSpin->setRange(-4000.0, 4000.0);
        m_transcriptOverlayYSpin->setDecimals(1);
        transcriptOverlayForm->addRow(QStringLiteral("Position Y"), m_transcriptOverlayYSpin);
        m_transcriptOverlayWidthSpin = new QSpinBox(transcriptTab);
        m_transcriptOverlayWidthSpin->setRange(80, 4000);
        transcriptOverlayForm->addRow(QStringLiteral("Box Width"), m_transcriptOverlayWidthSpin);
        m_transcriptOverlayHeightSpin = new QSpinBox(transcriptTab);
        m_transcriptOverlayHeightSpin->setRange(40, 2000);
        transcriptOverlayForm->addRow(QStringLiteral("Box Height"), m_transcriptOverlayHeightSpin);
        m_transcriptFontFamilyCombo = new QFontComboBox(transcriptTab);
        transcriptOverlayForm->addRow(QStringLiteral("Font"), m_transcriptFontFamilyCombo);
        m_transcriptFontSizeSpin = new QSpinBox(transcriptTab);
        m_transcriptFontSizeSpin->setRange(8, 240);
        transcriptOverlayForm->addRow(QStringLiteral("Font Size"), m_transcriptFontSizeSpin);
        m_transcriptBoldCheckBox = new QCheckBox(QStringLiteral("Bold"), transcriptTab);
        transcriptOverlayForm->addRow(QStringLiteral(""), m_transcriptBoldCheckBox);
        m_transcriptItalicCheckBox = new QCheckBox(QStringLiteral("Italic"), transcriptTab);
        transcriptOverlayForm->addRow(QStringLiteral(""), m_transcriptItalicCheckBox);
        transcriptLayout->addLayout(transcriptOverlayForm);
        m_transcriptTable = new QTableWidget(transcriptTab);
        m_transcriptTable->setColumnCount(3);
        m_transcriptTable->setHorizontalHeaderLabels(
            {QStringLiteral("Start"), QStringLiteral("End"), QStringLiteral("Text")});
        m_transcriptTable->setSelectionBehavior(QAbstractItemView::SelectRows);
        m_transcriptTable->setSelectionMode(QAbstractItemView::ExtendedSelection); // Allow Shift+Ctrl multi-select
        m_transcriptTable->setEditTriggers(QAbstractItemView::DoubleClicked |
                                           QAbstractItemView::EditKeyPressed |
                                           QAbstractItemView::SelectedClicked);
        
        // Delete key handler for transcript words
        auto* transcriptDeleteShortcut = new QShortcut(QKeySequence::Delete, m_transcriptTable);
        connect(transcriptDeleteShortcut, &QShortcut::activated, this, [this]() {
            if (!m_transcriptTable || m_updatingTranscriptInspector || 
                m_loadedTranscriptPath.isEmpty() || m_loadedTranscriptDoc.isNull()) {
                return;
            }
            
            // Get selected rows (sorted in descending order to delete from bottom up)
            QList<int> rowsToDelete;
            for (int row = 0; row < m_transcriptTable->rowCount(); ++row) {
                if (m_transcriptTable->selectionModel()->isRowSelected(row, QModelIndex())) {
                    rowsToDelete.append(row);
                }
            }
            if (rowsToDelete.isEmpty()) {
                return;
            }
            
            // Sort descending so we can delete without index shifting issues
            std::sort(rowsToDelete.begin(), rowsToDelete.end(), std::greater<int>());
            
            QJsonObject root = m_loadedTranscriptDoc.object();
            QJsonArray segments = root.value(QStringLiteral("segments")).toArray();
            
            // Track which words to delete from which segments
            QHash<int, QList<int>> wordsToDeleteBySegment; // segmentIndex -> list of wordIndices
            
            for (int row : rowsToDelete) {
                QTableWidgetItem* item = m_transcriptTable->item(row, 0);
                if (!item) continue;
                int segmentIndex = item->data(Qt::UserRole).toInt();
                int wordIndex = item->data(Qt::UserRole + 1).toInt();
                wordsToDeleteBySegment[segmentIndex].append(wordIndex);
            }
            
            // Delete words from each segment
            bool modified = false;
            for (auto it = wordsToDeleteBySegment.begin(); it != wordsToDeleteBySegment.end(); ++it) {
                int segmentIndex = it.key();
                QList<int> wordIndices = it.value();
                
                if (segmentIndex < 0 || segmentIndex >= segments.size()) continue;
                
                QJsonObject segmentObj = segments[segmentIndex].toObject();
                QJsonArray words = segmentObj.value(QStringLiteral("words")).toArray();
                
                // Sort descending to delete from end first
                std::sort(wordIndices.begin(), wordIndices.end(), std::greater<int>());
                
                for (int wordIndex : wordIndices) {
                    if (wordIndex >= 0 && wordIndex < words.size()) {
                        words.removeAt(wordIndex);
                        modified = true;
                    }
                }
                
                // Update segment
                segmentObj[QStringLiteral("words")] = words;
                segments[segmentIndex] = segmentObj;
            }
            
            if (!modified) {
                return;
            }
            
            root[QStringLiteral("segments")] = segments;
            m_loadedTranscriptDoc = QJsonDocument(root);
            
            if (!saveTranscriptJson(m_loadedTranscriptPath, m_loadedTranscriptDoc)) {
                QMessageBox::warning(this,
                                     QStringLiteral("Transcript Save Failed"),
                                     QStringLiteral("Could not save transcript changes."));
            }
            
            // Refresh the table
            refreshInspector();
        });
        m_transcriptTable->setTextElideMode(Qt::ElideNone);
        m_transcriptTable->setWordWrap(true);
        m_transcriptTable->verticalHeader()->setVisible(false);
        m_transcriptTable->verticalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
        m_transcriptTable->horizontalHeader()->setStretchLastSection(true);
        m_transcriptTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        m_transcriptTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        m_transcriptFollowCurrentWordCheckBox = new QCheckBox(QStringLiteral("Follow current word in table"), transcriptTab);
        m_transcriptFollowCurrentWordCheckBox->setChecked(true);
        transcriptLayout->addWidget(m_transcriptFollowCurrentWordCheckBox);
        
        // Runtime prepend offset (not saved to ground truth)
        auto* prependLayout = new QHBoxLayout();
        prependLayout->addWidget(new QLabel(QStringLiteral("Prepend (ms):"), transcriptTab));
        m_transcriptPrependMsSpin = new QSpinBox(transcriptTab);
        m_transcriptPrependMsSpin->setRange(-1000, 1000);
        m_transcriptPrependMsSpin->setValue(0);
        m_transcriptPrependMsSpin->setSingleStep(10);
        m_transcriptPrependMsSpin->setToolTip(QStringLiteral("Runtime offset applied to word start times (not saved to file)"));
        prependLayout->addWidget(m_transcriptPrependMsSpin);
        prependLayout->addStretch();
        transcriptLayout->addLayout(prependLayout);
        
        connect(m_transcriptPrependMsSpin, qOverload<int>(&QSpinBox::valueChanged), this, [this]() {
            // Just update the cached value - applied at runtime
            m_transcriptPrependMs = m_transcriptPrependMsSpin->value();
            // Rebuild ranges with new offset
            m_transcriptWordRangesCache.clear();
        });
        
        transcriptLayout->addWidget(m_transcriptTable, 1);
        m_inspectorTabs->addTab(transcriptTab, QStringLiteral("Transcript"));

        auto* syncTab = new QWidget(m_inspectorTabs);
        auto* syncLayout = new QVBoxLayout(syncTab);
        syncLayout->setContentsMargins(12, 12, 12, 12);
        syncLayout->setSpacing(10);
        m_syncInspectorClipLabel = new QLabel(QStringLiteral("Sync"), syncTab);
        m_syncInspectorClipLabel->setObjectName(QStringLiteral("inspectorTitle"));
        m_syncInspectorDetailsLabel = new QLabel(QStringLiteral("Timeline render sync markers."), syncTab);
        m_syncInspectorDetailsLabel->setObjectName(QStringLiteral("inspectorMeta"));
        m_syncInspectorDetailsLabel->setWordWrap(true);
        syncLayout->addWidget(m_syncInspectorClipLabel);
        syncLayout->addWidget(m_syncInspectorDetailsLabel);
        m_syncTable = new QTableWidget(syncTab);
        m_syncTable->setColumnCount(4);
        m_syncTable->setHorizontalHeaderLabels(
            {QStringLiteral("Clip"), QStringLiteral("Frame"), QStringLiteral("Count"), QStringLiteral("Action")});
        m_syncTable->setSelectionBehavior(QAbstractItemView::SelectRows);
        m_syncTable->setSelectionMode(QAbstractItemView::SingleSelection);
        m_syncTable->setEditTriggers(QAbstractItemView::DoubleClicked |
                                     QAbstractItemView::EditKeyPressed |
                                     QAbstractItemView::SelectedClicked);
        m_syncTable->verticalHeader()->setVisible(false);
        m_syncTable->horizontalHeader()->setStretchLastSection(true);
        m_syncTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        m_syncTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        m_syncTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
        syncLayout->addWidget(m_syncTable, 1);
        m_inspectorTabs->addTab(syncTab, QStringLiteral("Sync"));
        layout->addWidget(m_inspectorTabs, 1);
        connect(m_inspectorTabs, &QTabWidget::currentChanged, this, [this](int) {
            scheduleSaveState();
        });

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
        connect(m_exportOnlyTranscriptWordsCheckBox, &QCheckBox::toggled, this, [this](bool) {
            if (m_playbackTimer.isActive()) {
                setPlaybackActive(false);
                setPlaybackActive(true);
            }
            scheduleSaveState();
            refreshInspector();
        });
        connect(m_exportStartSpin, qOverload<int>(&QSpinBox::valueChanged), this, [this](int value) {
            if (!m_timeline || m_loadingState) {
                return;
            }
            m_timeline->setExportRange(value, m_timeline->exportEndFrame());
            refreshInspector();
        });
        connect(m_exportEndSpin, qOverload<int>(&QSpinBox::valueChanged), this, [this](int value) {
            if (!m_timeline || m_loadingState) {
                return;
            }
            m_timeline->setExportRange(m_timeline->exportStartFrame(), value);
            refreshInspector();
        });
        connect(m_bypassGradingCheckBox, &QCheckBox::toggled, this, [this](bool checked) {
            if (m_preview) {
                m_preview->setBypassGrading(checked);
            }
            refreshInspector();
        });
        // Single click on a row to seek to that word's start time
        connect(m_transcriptTable, &QTableWidget::cellClicked, this, [this](int row, int column) {
            Q_UNUSED(column)
            if (!m_transcriptTable || row < 0 || !m_timeline) {
                return;
            }
            // Don't seek if we're in edit mode (editing text)
            if (m_transcriptTable->isPersistentEditorOpen(m_transcriptTable->currentIndex())) {
                return;
            }
            QTableWidgetItem* startItem = m_transcriptTable->item(row, 0);
            if (!startItem) {
                return;
            }
            const int64_t startFrame = startItem->data(Qt::UserRole + 2).toLongLong();
            if (startFrame >= 0) {
                // Map source frame to timeline position for the selected clip
                const TimelineClip* clip = m_timeline->selectedClip();
                if (clip && clip->mediaType == ClipMediaType::Audio) {
                    // Calculate timeline frame from source frame
                    const int64_t localFrame = startFrame - clip->sourceInFrame;
                    const int64_t timelineFrame = clip->startFrame + localFrame;
                    setCurrentFrame(timelineFrame);
                }
            }
        });
        connect(m_transcriptTable, &QTableWidget::cellChanged, this, [this](int row, int column) {
            if (m_updatingTranscriptInspector || !m_transcriptTable || row < 0 || column < 0 ||
                m_loadedTranscriptPath.isEmpty() || m_loadedTranscriptDoc.isNull()) {
                return;
            }

            QTableWidgetItem* item = m_transcriptTable->item(row, column);
            if (!item) {
                return;
            }
            QTableWidgetItem* anchorItem = m_transcriptTable->item(row, 0);
            if (!anchorItem) {
                return;
            }
            const int segmentIndex = anchorItem->data(Qt::UserRole).toInt();
            const int wordIndex = anchorItem->data(Qt::UserRole + 1).toInt();

            QJsonObject root = m_loadedTranscriptDoc.object();
            QJsonArray segments = root.value(QStringLiteral("segments")).toArray();
            if (segmentIndex < 0 || segmentIndex >= segments.size()) {
                refreshInspector();
                return;
            }
            QJsonObject segmentObj = segments[segmentIndex].toObject();
            QJsonArray words = segmentObj.value(QStringLiteral("words")).toArray();
            if (wordIndex < 0 || wordIndex >= words.size()) {
                refreshInspector();
                return;
            }
            QJsonObject wordObj = words[wordIndex].toObject();

            if (column == 0 || column == 1) {
                double seconds = 0.0;
                if (!parseTranscriptTime(item->text(), &seconds)) {
                    refreshInspector();
                    return;
                }
                wordObj[column == 0 ? QStringLiteral("start") : QStringLiteral("end")] = seconds;
            } else if (column == 2) {
                wordObj[QStringLiteral("word")] = item->text();
            }

            words[wordIndex] = wordObj;
            segmentObj[QStringLiteral("words")] = words;
            segments[segmentIndex] = segmentObj;
            root[QStringLiteral("segments")] = segments;
            m_loadedTranscriptDoc = QJsonDocument(root);
            if (!saveTranscriptJson(m_loadedTranscriptPath, m_loadedTranscriptDoc)) {
                QMessageBox::warning(this,
                                     QStringLiteral("Transcript Save Failed"),
                                     QStringLiteral("Could not save transcript changes to:\n%1")
                                         .arg(QDir::toNativeSeparators(m_loadedTranscriptPath)));
            }
            if (m_preview && m_timeline) {
                m_preview->setTimelineClips(m_timeline->clips());
            }
            refreshInspector();
        });
        auto connectTranscriptOverlayLive = [this](auto* widget, auto signal) {
            connect(widget, signal, this, [this](auto) {
                applySelectedTranscriptOverlayFromInspector(false);
            });
        };
        connect(m_transcriptOverlayEnabledCheckBox, &QCheckBox::toggled, this, [this](bool) {
            applySelectedTranscriptOverlayFromInspector(true);
        });
        connectTranscriptOverlayLive(m_transcriptMaxLinesSpin, qOverload<int>(&QSpinBox::valueChanged));
        connectTranscriptOverlayLive(m_transcriptMaxCharsSpin, qOverload<int>(&QSpinBox::valueChanged));
        connect(m_transcriptAutoScrollCheckBox, &QCheckBox::toggled, this, [this](bool) {
            applySelectedTranscriptOverlayFromInspector(false);
        });
        connectTranscriptOverlayLive(m_transcriptOverlayXSpin, qOverload<double>(&QDoubleSpinBox::valueChanged));
        connectTranscriptOverlayLive(m_transcriptOverlayYSpin, qOverload<double>(&QDoubleSpinBox::valueChanged));
        connectTranscriptOverlayLive(m_transcriptOverlayWidthSpin, qOverload<int>(&QSpinBox::valueChanged));
        connectTranscriptOverlayLive(m_transcriptOverlayHeightSpin, qOverload<int>(&QSpinBox::valueChanged));
        connect(m_transcriptFontFamilyCombo, &QFontComboBox::currentFontChanged, this, [this](const QFont&) {
            applySelectedTranscriptOverlayFromInspector(false);
        });
        connectTranscriptOverlayLive(m_transcriptFontSizeSpin, qOverload<int>(&QSpinBox::valueChanged));
        connect(m_transcriptBoldCheckBox, &QCheckBox::toggled, this, [this](bool) {
            applySelectedTranscriptOverlayFromInspector(false);
        });
        connect(m_transcriptItalicCheckBox, &QCheckBox::toggled, this, [this](bool) {
            applySelectedTranscriptOverlayFromInspector(false);
        });
        connect(m_syncTable, &QTableWidget::cellChanged, this, [this](int row, int column) {
            if (m_updatingSyncInspector || !m_timeline || !m_syncTable || row < 0 || column < 0) {
                return;
            }
            QTableWidgetItem* clipItem = m_syncTable->item(row, 0);
            QTableWidgetItem* frameItem = m_syncTable->item(row, 1);
            QTableWidgetItem* countItem = m_syncTable->item(row, 2);
            QTableWidgetItem* actionItem = m_syncTable->item(row, 3);
            if (!clipItem || !frameItem || !countItem || !actionItem) {
                return;
            }

            const QString clipId = clipItem->data(Qt::UserRole).toString();
            const int64_t originalFrame = clipItem->data(Qt::UserRole + 1).toLongLong();
            bool frameOk = false;
            bool countOk = false;
            const int64_t editedFrame = frameItem->text().toLongLong(&frameOk);
            const int editedCount = countItem->text().toInt(&countOk);
            RenderSyncAction action = RenderSyncAction::DuplicateFrame;
            const bool actionOk = parseSyncActionText(actionItem->text(), &action);
            if (clipId.isEmpty() || !frameOk || !countOk || editedCount < 1 || !actionOk) {
                refreshInspector();
                return;
            }

            QVector<RenderSyncMarker> markers = m_timeline->renderSyncMarkers();
            bool updated = false;
            for (RenderSyncMarker& marker : markers) {
                if (marker.clipId == clipId && marker.frame == originalFrame) {
                    marker.frame = qMax<int64_t>(0, editedFrame);
                    marker.count = qMax(1, editedCount);
                    marker.action = action;
                    updated = true;
                    break;
                }
            }
            if (!updated) {
                refreshInspector();
                return;
            }

            m_timeline->setRenderSyncMarkers(markers);
        });
        connect(m_videoKeyframeTable, &QTableWidget::itemSelectionChanged, this, [this]() {
            if (m_updatingVideoInspector) {
                return;
            }
            m_selectedVideoKeyframeFrames.clear();
            const QList<QTableWidgetSelectionRange> ranges = m_videoKeyframeTable->selectedRanges();
            for (const QTableWidgetSelectionRange& range : ranges) {
                for (int row = range.topRow(); row <= range.bottomRow(); ++row) {
                    QTableWidgetItem* item = m_videoKeyframeTable->item(row, 0);
                    if (item) {
                        m_selectedVideoKeyframeFrames.insert(item->data(Qt::UserRole).toLongLong());
                    }
                }
            }
            QTableWidgetItem* currentItem = m_videoKeyframeTable->currentItem();
            if (currentItem) {
                currentItem = m_videoKeyframeTable->item(currentItem->row(), 0);
            }
            if (currentItem && m_selectedVideoKeyframeFrames.contains(currentItem->data(Qt::UserRole).toLongLong())) {
                m_selectedVideoKeyframeFrame = currentItem->data(Qt::UserRole).toLongLong();
            } else if (!m_selectedVideoKeyframeFrames.isEmpty()) {
                m_selectedVideoKeyframeFrame = *m_selectedVideoKeyframeFrames.begin();
            } else {
                m_selectedVideoKeyframeFrame = -1;
            }
            const TimelineClip* clip = m_timeline ? m_timeline->selectedClip() : nullptr;
            if (clip && m_selectedVideoKeyframeFrame >= 0) {
                setCurrentFrame(clip->startFrame + m_selectedVideoKeyframeFrame);
                return;
            }
            refreshInspector();
        });
        connect(m_videoKeyframeTable, &QTableWidget::currentItemChanged, this, [this](QTableWidgetItem* current, QTableWidgetItem*) {
            if (m_updatingVideoInspector) {
                return;
            }
            if (!current) {
                return;
            }
            QTableWidgetItem* frameItem = m_videoKeyframeTable->item(current->row(), 0);
            if (!frameItem) {
                return;
            }
            const int64_t frame = frameItem->data(Qt::UserRole).toLongLong();
            if (!m_selectedVideoKeyframeFrames.contains(frame)) {
                return;
            }
            m_selectedVideoKeyframeFrame = frame;
        });
        connect(m_videoKeyframeTable, &QTableWidget::cellChanged, this, [this](int row, int) {
            if (m_updatingVideoInspector || !m_timeline || row < 0) {
                return;
            }
            const QString clipId = m_timeline->selectedClipId();
            if (clipId.isEmpty()) {
                return;
            }
            QTableWidgetItem* frameItem = m_videoKeyframeTable->item(row, 0);
            if (!frameItem) {
                return;
            }
            const int64_t originalFrame = frameItem->data(Qt::UserRole).toLongLong();
            bool ok = false;
            const int64_t editedFrame = frameItem->text().toLongLong(&ok);
            if (!ok) {
                refreshInspector();
                return;
            }
            const bool updated = m_timeline->updateClipById(clipId, [this, row, originalFrame, editedFrame](TimelineClip& clip) {
                int rowIndex = -1;
                for (int i = 0; i < clip.transformKeyframes.size(); ++i) {
                    if (clip.transformKeyframes[i].frame == originalFrame) {
                        rowIndex = i;
                        break;
                    }
                }
                if (rowIndex < 0) {
                    return;
                }
                TimelineClip::TransformKeyframe& keyframe = clip.transformKeyframes[rowIndex];
                keyframe.frame = (originalFrame == 0)
                    ? 0
                    : qMax<int64_t>(0, keyframeFrameFromInspectorDisplay(clip, editedFrame));
                TimelineClip::TransformKeyframe displayed = keyframe;
                displayed.translationX = m_videoKeyframeTable->item(row, 1)->text().toDouble();
                displayed.translationY = m_videoKeyframeTable->item(row, 2)->text().toDouble();
                displayed.rotation = m_videoKeyframeTable->item(row, 3)->text().toDouble();
                displayed.scaleX = m_videoKeyframeTable->item(row, 4)->text().toDouble();
                displayed.scaleY = m_videoKeyframeTable->item(row, 5)->text().toDouble();
                const TimelineClip::TransformKeyframe stored = keyframeFromInspectorDisplay(clip, displayed);
                keyframe.translationX = stored.translationX;
                keyframe.translationY = stored.translationY;
                keyframe.rotation = stored.rotation;
                keyframe.scaleX = stored.scaleX;
                keyframe.scaleY = stored.scaleY;
                const QString mode = m_videoKeyframeTable->item(row, 6)->text().trimmed().toLower();
                keyframe.interpolated = mode != QStringLiteral("sudden");
                normalizeClipTransformKeyframes(clip);
            });
            if (!updated) {
                refreshInspector();
                return;
            }
            const int64_t selectedFrame =
                (originalFrame == 0)
                    ? 0
                    : qMax<int64_t>(0, keyframeFrameFromInspectorDisplay(*m_timeline->selectedClip(), editedFrame));
            m_selectedVideoKeyframeFrame = selectedFrame;
            m_selectedVideoKeyframeFrames.remove(originalFrame);
            m_selectedVideoKeyframeFrames.insert(m_selectedVideoKeyframeFrame);
            m_preview->setTimelineClips(m_timeline->clips());
            refreshInspector();
            scheduleSaveState();
            pushHistorySnapshot();
        });
        connect(m_videoKeyframeTable, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
            if (!m_videoKeyframeTable || !m_timeline) {
                return;
            }
            const QModelIndex index = m_videoKeyframeTable->indexAt(pos);
            if (!index.isValid()) {
                return;
            }
            if (!m_videoKeyframeTable->selectionModel()->isRowSelected(index.row(), QModelIndex())) {
                m_videoKeyframeTable->selectRow(index.row());
            }

            QMenu menu(m_videoKeyframeTable);
            QAction* duplicateAboveAction = menu.addAction(QStringLiteral("Duplicate Above (-1 frame)"));
            QAction* duplicateBelowAction = menu.addAction(QStringLiteral("Duplicate Below (+1 frame)"));
            menu.addSeparator();
            QAction* deleteAction = menu.addAction(QStringLiteral("Delete"));
            const TimelineClip* clip = m_timeline->selectedClip();
            if (!clip || !hasRemovableVideoKeyframeSelection(*clip)) {
                deleteAction->setEnabled(false);
            }

            QAction* chosen = menu.exec(m_videoKeyframeTable->viewport()->mapToGlobal(pos));
            if (chosen == duplicateAboveAction) {
                duplicateSelectedClipVideoKeyframes(-1);
            } else if (chosen == duplicateBelowAction) {
                duplicateSelectedClipVideoKeyframes(1);
            } else if (chosen == deleteAction) {
                removeSelectedClipVideoKeyframe();
            }
        });
        connect(m_keyframeSpaceCheckBox, &QCheckBox::toggled, this, [this](bool) {
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
            RenderRequest request;
            request.outputPath = selectedPath;
            request.outputFormat = extension;
            request.outputSize = QSize(m_outputWidthSpin ? m_outputWidthSpin->value() : 1080,
                                       m_outputHeightSpin ? m_outputHeightSpin->value() : 1920);
            request.clips = m_timeline ? m_timeline->clips() : QVector<TimelineClip>();
            request.renderSyncMarkers = m_timeline ? m_timeline->renderSyncMarkers() : QVector<RenderSyncMarker>();
            request.exportRanges = m_timeline ? m_timeline->exportRanges() : QVector<ExportRangeSegment>();
            request.exportStartFrame = m_timeline ? m_timeline->exportStartFrame() : 0;
            request.exportEndFrame = m_timeline ? m_timeline->exportEndFrame() : 0;
            if (m_exportOnlyTranscriptWordsCheckBox && m_exportOnlyTranscriptWordsCheckBox->isChecked()) {
                request.exportRanges =
                    transcriptWordExportRanges(request.exportRanges, request.clips, request.renderSyncMarkers);
                if (request.exportRanges.isEmpty()) {
                    QMessageBox::information(
                        this,
                        QStringLiteral("Nothing To Render"),
                        QStringLiteral("No transcript words were found inside the current export range."));
                    return;
                }
                request.exportStartFrame = request.exportRanges.constFirst().startFrame;
                request.exportEndFrame = request.exportRanges.constLast().endFrame;
            }

            auto* progressDialog = new QDialog(this);
            progressDialog->setWindowTitle(QStringLiteral("Rendering"));
            progressDialog->setModal(true);
            progressDialog->setMinimumWidth(520);
            progressDialog->setAttribute(Qt::WA_DeleteOnClose, false);
            auto* progressLayout = new QVBoxLayout(progressDialog);
            progressLayout->setContentsMargins(18, 18, 18, 18);
            progressLayout->setSpacing(10);

            auto* titleLabel = new QLabel(QStringLiteral("Rendering Video"), progressDialog);
            titleLabel->setStyleSheet(QStringLiteral("font-weight: 700; font-size: 16px;"));
            auto* pathLabel = new QLabel(QDir::toNativeSeparators(selectedPath), progressDialog);
            pathLabel->setWordWrap(true);
            auto* pipelineLabel = new QLabel(QStringLiteral("Pipeline: Detecting..."), progressDialog);
            pipelineLabel->setStyleSheet(QStringLiteral("font-weight: 600; color: #fff4c2;"));
            auto* statusLabel = new QLabel(QStringLiteral("Preparing render..."), progressDialog);
            statusLabel->setWordWrap(true);
            auto* detailLabel = new QLabel(QStringLiteral("Scanning export ranges..."), progressDialog);
            detailLabel->setWordWrap(true);
            auto* previewLabel = new QLabel(progressDialog);
            previewLabel->setAlignment(Qt::AlignCenter);
            previewLabel->setMinimumSize(240, 426);
            previewLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
            previewLabel->setStyleSheet(QStringLiteral(
                "QLabel { background: #05080c; border: 1px solid #202934; border-radius: 12px; color: #7f8b99; }"));
            previewLabel->setText(QStringLiteral("Render preview will appear here"));
            auto* progressBar = new QProgressBar(progressDialog);
            progressBar->setRange(0, 1000);
            progressBar->setValue(0);
            auto* cancelButton = new QPushButton(QStringLiteral("Cancel"), progressDialog);
            progressLayout->addWidget(titleLabel);
            progressLayout->addWidget(pathLabel);
            progressLayout->addWidget(pipelineLabel);
            progressLayout->addWidget(statusLabel);
            progressLayout->addWidget(detailLabel);
            progressLayout->addWidget(previewLabel, 1);
            progressLayout->addWidget(progressBar);
            progressLayout->addWidget(cancelButton, 0, Qt::AlignRight);

            bool cancelRequested = false;
            connect(cancelButton, &QPushButton::clicked, progressDialog, [&cancelRequested, cancelButton, statusLabel]() {
                cancelRequested = true;
                cancelButton->setEnabled(false);
                statusLabel->setText(QStringLiteral("Cancelling render..."));
            });

            progressDialog->show();
            progressDialog->raise();
            progressDialog->activateWindow();
            QApplication::processEvents();

            QApplication::setOverrideCursor(Qt::WaitCursor);
            const RenderResult renderResult = renderTimelineToFile(
                request,
                [this, &cancelRequested, progressDialog, progressBar, pipelineLabel, statusLabel, detailLabel, previewLabel](const RenderProgress& progress) {
                    if (cancelRequested) {
                        return false;
                    }
                    pipelineLabel->setText(progress.usingGpu
                                               ? QStringLiteral("Pipeline: GPU Offscreen OpenGL")
                                               : QStringLiteral("Pipeline: CPU Fallback"));
                    const int percent = progress.totalFrames > 0
                        ? qBound(0, qRound((static_cast<double>(progress.framesCompleted) / static_cast<double>(progress.totalFrames)) * 1000.0), 1000)
                        : 0;
                    progressBar->setValue(percent);
                    statusLabel->setText(
                        QStringLiteral("Frame %1 of %2  (%3%)")
                            .arg(progress.framesCompleted + 1)
                            .arg(qMax<int64_t>(1, progress.totalFrames))
                            .arg(QString::number(percent / 10.0, 'f', 1)));
                    detailLabel->setText(
                        QStringLiteral("Segment %1 of %2  |  Timeline frame %3  |  Segment %4 -> %5")
                            .arg(progress.segmentIndex)
                            .arg(qMax(1, progress.segmentCount))
                            .arg(progress.timelineFrame)
                            .arg(progress.segmentStartFrame)
                            .arg(progress.segmentEndFrame));
                    if (!progress.previewFrame.isNull() && previewLabel) {
                        const QSize targetSize = previewLabel->contentsRect().size().expandedTo(QSize(1, 1));
                        const QPixmap pixmap = QPixmap::fromImage(
                            progress.previewFrame.scaled(targetSize, Qt::KeepAspectRatio, Qt::SmoothTransformation));
                        previewLabel->setPixmap(pixmap);
                    }
                    if (progressDialog) {
                        progressDialog->update();
                    }
                    QApplication::processEvents();
                    return !cancelRequested;
                });
            QApplication::restoreOverrideCursor();
            progressDialog->close();
            progressDialog->deleteLater();

            if (!renderResult.success) {
                if (renderResult.cancelled) {
                    QMessageBox::information(this,
                                             QStringLiteral("Render Cancelled"),
                                             renderResult.message);
                    return;
                }
                QMessageBox::warning(this,
                                     QStringLiteral("Render Failed"),
                                     renderResult.message);
                return;
            }

            QMessageBox::information(this,
                                     QStringLiteral("Render Complete"),
                                     renderResult.message +
                                         QStringLiteral("\nPipeline: %1\n\nAudio included when timeline clips provide it.")
                                             .arg(renderResult.usedGpu
                                                      ? QStringLiteral("GPU Offscreen OpenGL")
                                                      : QStringLiteral("CPU Fallback")));
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

    bool showClipRelativeKeyframes() const {
        return !m_keyframeSpaceCheckBox || m_keyframeSpaceCheckBox->isChecked();
    }

    int64_t keyframeFrameForInspectorDisplay(const TimelineClip& clip, int64_t storedFrame) const {
        return showClipRelativeKeyframes() ? storedFrame : (clip.startFrame + storedFrame);
    }

    int64_t keyframeFrameFromInspectorDisplay(const TimelineClip& clip, int64_t displayedFrame) const {
        return showClipRelativeKeyframes() ? displayedFrame : (displayedFrame - clip.startFrame);
    }

    qreal inspectorScaleWithMirror(qreal magnitude, bool mirrored) const {
        const qreal sanitized = sanitizeScaleValue(std::abs(magnitude));
        return mirrored ? -sanitized : sanitized;
    }

    void syncScaleSpinPair(QDoubleSpinBox* changedSpin, QDoubleSpinBox* otherSpin) {
        if (m_syncingScaleControls || !m_lockVideoScaleCheckBox || !m_lockVideoScaleCheckBox->isChecked() ||
            !changedSpin || !otherSpin) {
            return;
        }
        m_syncingScaleControls = true;
        otherSpin->setValue(std::abs(changedSpin->value()));
        m_syncingScaleControls = false;
    }

    TimelineClip::TransformKeyframe keyframeForInspectorDisplay(const TimelineClip& clip,
                                                                const TimelineClip::TransformKeyframe& keyframe) const {
        if (showClipRelativeKeyframes()) {
            return keyframe;
        }
        TimelineClip::TransformKeyframe displayed = keyframe;
        displayed.translationX = clip.baseTranslationX + keyframe.translationX;
        displayed.translationY = clip.baseTranslationY + keyframe.translationY;
        displayed.rotation = clip.baseRotation + keyframe.rotation;
        displayed.scaleX = sanitizeScaleValue(clip.baseScaleX) * sanitizeScaleValue(keyframe.scaleX);
        displayed.scaleY = sanitizeScaleValue(clip.baseScaleY) * sanitizeScaleValue(keyframe.scaleY);
        return displayed;
    }

    TimelineClip::TransformKeyframe keyframeFromInspectorDisplay(const TimelineClip& clip,
                                                                 const TimelineClip::TransformKeyframe& displayed) const {
        if (showClipRelativeKeyframes()) {
            return displayed;
        }
        TimelineClip::TransformKeyframe stored = displayed;
        stored.translationX = displayed.translationX - clip.baseTranslationX;
        stored.translationY = displayed.translationY - clip.baseTranslationY;
        stored.rotation = displayed.rotation - clip.baseRotation;
        stored.scaleX = sanitizeScaleValue(displayed.scaleX / sanitizeScaleValue(clip.baseScaleX));
        stored.scaleY = sanitizeScaleValue(displayed.scaleY / sanitizeScaleValue(clip.baseScaleY));
        return stored;
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

    bool hasRemovableVideoKeyframeSelection(const TimelineClip& clip) const {
        for (const int64_t frame : selectedVideoKeyframeFrames(clip)) {
            if (frame > 0) {
                return true;
            }
        }
        return false;
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
            TimelineClip::TransformKeyframe displayed = keyframe;
            displayed.translationX = m_videoTranslationXSpin->value();
            displayed.translationY = m_videoTranslationYSpin->value();
            displayed.rotation = m_videoRotationSpin->value();
            displayed.scaleX = inspectorScaleWithMirror(m_videoScaleXSpin->value(),
                                                        m_mirrorHorizontalCheckBox && m_mirrorHorizontalCheckBox->isChecked());
            displayed.scaleY = inspectorScaleWithMirror(m_videoScaleYSpin->value(),
                                                        m_mirrorVerticalCheckBox && m_mirrorVerticalCheckBox->isChecked());
            const TimelineClip::TransformKeyframe stored = keyframeFromInspectorDisplay(clip, displayed);
            keyframe.translationX = stored.translationX;
            keyframe.translationY = stored.translationY;
            keyframe.rotation = stored.rotation;
            keyframe.scaleX = stored.scaleX;
            keyframe.scaleY = stored.scaleY;
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
            keyframe.scaleX = inspectorScaleWithMirror(m_videoScaleXSpin->value(),
                                                       m_mirrorHorizontalCheckBox && m_mirrorHorizontalCheckBox->isChecked());
            keyframe.scaleY = inspectorScaleWithMirror(m_videoScaleYSpin->value(),
                                                       m_mirrorVerticalCheckBox && m_mirrorVerticalCheckBox->isChecked());
            keyframe = keyframeFromInspectorDisplay(editableClip, keyframe);
            keyframe.frame = keyframeFrame;
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

    void duplicateSelectedClipVideoKeyframes(int frameDelta) {
        if (!m_timeline || frameDelta == 0) {
            return;
        }
        const TimelineClip* selectedClip = m_timeline->selectedClip();
        if (!selectedClip || !clipHasVisuals(*selectedClip)) {
            return;
        }

        const QList<int64_t> selectedFrames = selectedVideoKeyframeFrames(*selectedClip);
        if (selectedFrames.isEmpty()) {
            return;
        }

        QSet<int64_t> sourceFrames;
        for (int64_t frame : selectedFrames) {
            sourceFrames.insert(frame);
        }
        const int64_t maxFrame = qMax<int64_t>(0, selectedClip->durationFrames - 1);
        QSet<int64_t> newFrames;
        const bool updated = m_timeline->updateClipById(selectedClip->id, [frameDelta, maxFrame, sourceFrames, &newFrames](TimelineClip& clip) {
            bool changed = false;
            const QVector<TimelineClip::TransformKeyframe> originalKeyframes = clip.transformKeyframes;
            for (const TimelineClip::TransformKeyframe& keyframe : originalKeyframes) {
                if (!sourceFrames.contains(keyframe.frame)) {
                    continue;
                }
                const int64_t newFrame = qBound<int64_t>(0, keyframe.frame + frameDelta, maxFrame);
                TimelineClip::TransformKeyframe duplicate = keyframe;
                duplicate.frame = newFrame;
                bool replaced = false;
                for (TimelineClip::TransformKeyframe& existing : clip.transformKeyframes) {
                    if (existing.frame == newFrame) {
                        existing = duplicate;
                        replaced = true;
                        changed = true;
                        break;
                    }
                }
                if (!replaced) {
                    clip.transformKeyframes.push_back(duplicate);
                    changed = true;
                }
                newFrames.insert(newFrame);
            }
            if (changed) {
                normalizeClipTransformKeyframes(clip);
            }
        });

        if (!updated || newFrames.isEmpty()) {
            return;
        }

        m_selectedVideoKeyframeFrames = newFrames;
        m_selectedVideoKeyframeFrame = *std::min_element(newFrames.begin(), newFrames.end());
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
        QList<int64_t> selectedFrames = selectedVideoKeyframeFrames(*selectedClip);
        selectedFrames.erase(std::remove_if(selectedFrames.begin(),
                                            selectedFrames.end(),
                                            [](int64_t frame) { return frame <= 0; }),
                             selectedFrames.end());
        if (selectedFrames.isEmpty()) {
            refreshInspector();
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

        m_selectedVideoKeyframeFrame = 0;
        m_selectedVideoKeyframeFrames = {0};
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

    void applySelectedTranscriptOverlayFromInspector(bool pushHistoryAfterChange = false) {
        if (!m_timeline || m_updatingTranscriptInspector) {
            return;
        }
        const QString clipId = m_timeline->selectedClipId();
        if (clipId.isEmpty()) {
            return;
        }

        const bool updated = m_timeline->updateClipById(clipId, [this](TimelineClip& clip) {
            clip.transcriptOverlay.enabled =
                m_transcriptOverlayEnabledCheckBox && m_transcriptOverlayEnabledCheckBox->isChecked();
            clip.transcriptOverlay.maxLines = m_transcriptMaxLinesSpin ? m_transcriptMaxLinesSpin->value() : 2;
            clip.transcriptOverlay.maxCharsPerLine =
                m_transcriptMaxCharsSpin ? m_transcriptMaxCharsSpin->value() : 28;
            clip.transcriptOverlay.autoScroll =
                m_transcriptAutoScrollCheckBox && m_transcriptAutoScrollCheckBox->isChecked();
            clip.transcriptOverlay.translationX =
                m_transcriptOverlayXSpin ? m_transcriptOverlayXSpin->value() : 0.0;
            clip.transcriptOverlay.translationY =
                m_transcriptOverlayYSpin ? m_transcriptOverlayYSpin->value() : 640.0;
            clip.transcriptOverlay.boxWidth =
                m_transcriptOverlayWidthSpin ? m_transcriptOverlayWidthSpin->value() : 900.0;
            clip.transcriptOverlay.boxHeight =
                m_transcriptOverlayHeightSpin ? m_transcriptOverlayHeightSpin->value() : 220.0;
            clip.transcriptOverlay.fontFamily =
                m_transcriptFontFamilyCombo ? m_transcriptFontFamilyCombo->currentFont().family()
                                            : QStringLiteral("DejaVu Sans");
            clip.transcriptOverlay.fontPointSize =
                m_transcriptFontSizeSpin ? m_transcriptFontSizeSpin->value() : 42;
            clip.transcriptOverlay.bold = m_transcriptBoldCheckBox && m_transcriptBoldCheckBox->isChecked();
            clip.transcriptOverlay.italic =
                m_transcriptItalicCheckBox && m_transcriptItalicCheckBox->isChecked();
        });
        if (!updated) {
            return;
        }

        if (m_preview) {
            m_preview->setTimelineClips(m_timeline->clips());
        }
        refreshInspector();
        scheduleSaveState();
        if (pushHistoryAfterChange) {
            pushHistorySnapshot();
        }
    }
    
    void setCurrentFrame(int64_t frame, bool syncAudio = true) {
        setCurrentPlaybackSample(frameToSamples(frame), syncAudio);
    }

    void setPlaybackActive(bool playing) {
        if (playing) {
            if (m_audioEngine && m_audioEngine->hasPlayableAudio()) {
                m_audioEngine->start(m_timeline->currentFrame());
            }
            advanceFrame();
            m_playbackTimer.start();
            m_fastPlaybackActive.store(true);
            m_preview->setPlaybackState(true);
        } else {
            if (m_audioEngine) {
                m_audioEngine->stop();
            }
            m_playbackTimer.stop();
            m_fastPlaybackActive.store(false);
            m_preview->setPlaybackState(false);
        }
        refreshInspector();
        scheduleSaveState();
    }

    void togglePlayback() {
        setPlaybackActive(!m_playbackTimer.isActive());
    }

    void setCurrentPlaybackSample(int64_t samplePosition, bool syncAudio = true, bool duringPlayback = false) {
        const int64_t boundedSample = qBound<int64_t>(0, samplePosition, frameToSamples(m_timeline->totalFrames()));
        const qreal framePosition = samplesToFramePosition(boundedSample);
        const int64_t bounded = qBound<int64_t>(0,
                                               static_cast<int64_t>(std::floor(framePosition)),
                                               m_timeline->totalFrames());
        playbackTrace(QStringLiteral("EditorWindow::setCurrentFrame"),
                      QStringLiteral("requestedSample=%1 boundedSample=%2 frame=%3")
                          .arg(samplePosition)
                          .arg(boundedSample)
                          .arg(framePosition, 0, 'f', 3));
        if (!m_timeline || bounded != m_timeline->currentFrame()) {
            m_lastPlayheadAdvanceMs.store(nowMs());
        }
        m_currentPlaybackSample = boundedSample;
        m_fastCurrentFrame.store(bounded);
        if (syncAudio && m_audioEngine && m_audioEngine->hasPlayableAudio()) {
            m_audioEngine->seek(bounded);
        }
        m_timeline->setCurrentFrame(bounded);
        m_preview->setCurrentPlaybackSample(boundedSample);
        
        m_ignoreSeekSignal = true;
        m_seekSlider->setValue(static_cast<int>(qMin<int64_t>(bounded, INT_MAX)));
        m_ignoreSeekSignal = false;
        
        m_timecodeLabel->setText(frameToTimecode(bounded));
        
        // During playback, only update cheap transport labels and sync transcript table
        // instead of doing a full refreshInspector() which rebuilds all tables
        if (duringPlayback) {
            updateTransportLabels();
            syncTranscriptTableToPlayhead();
        } else {
            refreshInspector();
        }
        scheduleSaveState();
    }

    void syncTranscriptTableToPlayhead() {
        if (!m_timeline || !m_transcriptTable || m_updatingTranscriptInspector) {
            return;
        }

        // Skip all table updates when "Follow current word" is disabled
        if (!m_transcriptFollowCurrentWordCheckBox || !m_transcriptFollowCurrentWordCheckBox->isChecked()) {
            return;
        }

        const TimelineClip* clip = m_timeline->selectedClip();
        if (!clip || clip->mediaType != ClipMediaType::Audio || m_loadedTranscriptPath.isEmpty()) {
            m_transcriptTable->clearSelection();
            return;
        }

        const int64_t sourceFrame =
            sourceFrameForClipAtTimelinePosition(*clip,
                                                 samplesToFramePosition(m_currentPlaybackSample),
                                                 {});
        int matchingRow = -1;
        for (int row = 0; row < m_transcriptTable->rowCount(); ++row) {
            QTableWidgetItem* startItem = m_transcriptTable->item(row, 0);
            if (!startItem) {
                continue;
            }
            const int64_t startFrame = startItem->data(Qt::UserRole + 2).toLongLong();
            const int64_t endFrame = startItem->data(Qt::UserRole + 3).toLongLong();
            if (sourceFrame >= startFrame && sourceFrame <= endFrame) {
                matchingRow = row;
                break;
            }
        }

        if (matchingRow < 0) {
            m_transcriptTable->clearSelection();
            return;
        }

        if (!m_transcriptTable->selectionModel()->isRowSelected(matchingRow, QModelIndex())) {
            m_transcriptTable->setCurrentCell(matchingRow, 0);
            m_transcriptTable->selectRow(matchingRow);
        }
        // Scroll to the matching word
        if (QTableWidgetItem* item = m_transcriptTable->item(matchingRow, 0)) {
            m_transcriptTable->scrollToItem(item, QAbstractItemView::PositionAtCenter);
        }
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
    
    // Lightweight update for transport labels - safe to call during playback
    void updateTransportLabels() {
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
        m_playButton->setText(playing ? QStringLiteral("Pause") : QStringLiteral("Play"));
        m_playButton->setIcon(style()->standardIcon(playing ? QStyle::SP_MediaPause
                                                            : QStyle::SP_MediaPlay));
        m_audioMuteButton->setText(m_preview && m_preview->audioMuted()
                                       ? QStringLiteral("Unmute")
                                       : QStringLiteral("Mute"));
        m_audioNowPlayingLabel->setText(activeAudio.isEmpty()
                                            ? QStringLiteral("Audio idle")
                                            : QStringLiteral("Audio  %1").arg(activeAudio));
    }

    void refreshInspector() {
        const bool playing = m_playbackTimer.isActive();
        
        // Update cheap transport labels
        updateTransportLabels();
        if (m_bypassGradingCheckBox) {
            QSignalBlocker block(m_bypassGradingCheckBox);
            m_bypassGradingCheckBox->setChecked(m_preview && m_preview->bypassGrading());
        }
        if (m_timeline && m_exportStartSpin && m_exportEndSpin) {
            const int totalFrames = static_cast<int>(qMin<int64_t>(m_timeline->totalFrames(), INT_MAX));
            QSignalBlocker block1(m_exportStartSpin);
            QSignalBlocker block2(m_exportEndSpin);
            m_exportStartSpin->setRange(0, totalFrames);
            m_exportEndSpin->setRange(0, totalFrames);
            m_exportStartSpin->setValue(static_cast<int>(qMin<int64_t>(m_timeline->exportStartFrame(), totalFrames)));
            m_exportEndSpin->setValue(static_cast<int>(qMin<int64_t>(m_timeline->exportEndFrame(), totalFrames)));
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
        m_mirrorHorizontalCheckBox->setEnabled(enabled);
        m_mirrorVerticalCheckBox->setEnabled(enabled);
        m_lockVideoScaleCheckBox->setEnabled(enabled);
        m_addVideoKeyframeButton->setEnabled(enabled);
        m_videoKeyframeTable->setEnabled(enabled);
        m_keyframeSpaceCheckBox->setEnabled(enabled);
        m_transcriptTable->setEnabled(clip != nullptr);
        m_syncTable->setEnabled(m_timeline != nullptr);
        refreshSyncInspector();

        if (!clip) {
            m_gradingClipLabel->setText(QStringLiteral("No clip selected"));
            m_gradingPathLabel->setText(QStringLiteral("Select a clip in the timeline to grade it live."));
            m_videoInspectorClipLabel->setText(QStringLiteral("Output"));
            m_videoInspectorDetailsLabel->setText(
                QStringLiteral("Render %1 clips at %2x%3 as %4.\nExport range: %5 -> %6\nSpeech filter: %7")
                    .arg(m_timeline ? m_timeline->clips().size() : 0)
                    .arg(m_outputWidthSpin ? m_outputWidthSpin->value() : 1080)
                    .arg(m_outputHeightSpin ? m_outputHeightSpin->value() : 1920)
                    .arg(m_outputFormatCombo ? m_outputFormatCombo->currentText() : QStringLiteral("MP4 (H.264)"))
                    .arg(frameToTimecode(m_timeline ? m_timeline->exportStartFrame() : 0))
                    .arg(frameToTimecode(m_timeline ? m_timeline->exportEndFrame() : 0))
                    .arg(m_exportOnlyTranscriptWordsCheckBox && m_exportOnlyTranscriptWordsCheckBox->isChecked()
                             ? QStringLiteral("Only frames with transcript words")
                             : QStringLiteral("Off")));
            m_keyframesInspectorClipLabel->setText(QStringLiteral("No visual clip selected"));
            m_keyframesInspectorDetailsLabel->setText(QStringLiteral("Select a visual clip to inspect its keyframes."));
            m_keyframeSpaceCheckBox->setEnabled(false);
            m_audioInspectorClipLabel->setText(QStringLiteral("No audio clip selected"));
            m_audioInspectorDetailsLabel->setText(QStringLiteral("Select an audio clip to inspect playback details."));
            m_transcriptInspectorClipLabel->setText(QStringLiteral("No transcript selected"));
            m_transcriptInspectorDetailsLabel->setText(QStringLiteral("Select a clip with a WhisperX JSON transcript."));
            if (m_transcriptOverlayEnabledCheckBox) {
                QSignalBlocker block1(m_transcriptOverlayEnabledCheckBox);
                QSignalBlocker block2(m_transcriptMaxLinesSpin);
                QSignalBlocker block3(m_transcriptMaxCharsSpin);
                QSignalBlocker block4(m_transcriptAutoScrollCheckBox);
                QSignalBlocker block5(m_transcriptOverlayXSpin);
                QSignalBlocker block6(m_transcriptOverlayYSpin);
                QSignalBlocker block7(m_transcriptOverlayWidthSpin);
                QSignalBlocker block8(m_transcriptOverlayHeightSpin);
                QSignalBlocker block9(m_transcriptFontFamilyCombo);
                QSignalBlocker block10(m_transcriptFontSizeSpin);
                QSignalBlocker block11(m_transcriptBoldCheckBox);
                QSignalBlocker block12(m_transcriptItalicCheckBox);
                m_transcriptOverlayEnabledCheckBox->setChecked(false);
                m_transcriptMaxLinesSpin->setValue(2);
                m_transcriptMaxCharsSpin->setValue(28);
                m_transcriptAutoScrollCheckBox->setChecked(false);
                m_transcriptOverlayXSpin->setValue(0.0);
                m_transcriptOverlayYSpin->setValue(640.0);
                m_transcriptOverlayWidthSpin->setValue(900);
                m_transcriptOverlayHeightSpin->setValue(220);
                m_transcriptFontFamilyCombo->setCurrentFont(QFont(QStringLiteral("DejaVu Sans")));
                m_transcriptFontSizeSpin->setValue(42);
                m_transcriptBoldCheckBox->setChecked(true);
                m_transcriptItalicCheckBox->setChecked(false);
                for (QWidget* widget : {static_cast<QWidget*>(m_transcriptOverlayEnabledCheckBox),
                                        static_cast<QWidget*>(m_transcriptMaxLinesSpin),
                                        static_cast<QWidget*>(m_transcriptMaxCharsSpin),
                                        static_cast<QWidget*>(m_transcriptAutoScrollCheckBox),
                                        static_cast<QWidget*>(m_transcriptOverlayXSpin),
                                        static_cast<QWidget*>(m_transcriptOverlayYSpin),
                                        static_cast<QWidget*>(m_transcriptOverlayWidthSpin),
                                        static_cast<QWidget*>(m_transcriptOverlayHeightSpin),
                                        static_cast<QWidget*>(m_transcriptFontFamilyCombo),
                                        static_cast<QWidget*>(m_transcriptFontSizeSpin),
                                        static_cast<QWidget*>(m_transcriptBoldCheckBox),
                                        static_cast<QWidget*>(m_transcriptItalicCheckBox)}) {
                    if (widget) {
                        widget->setEnabled(false);
                    }
                }
            }
            m_updatingVideoInspector = true;
            m_videoKeyframeTable->clearContents();
            m_videoKeyframeTable->setRowCount(0);
            m_removeVideoKeyframeButton->setEnabled(false);
            m_updatingTranscriptInspector = true;
            m_transcriptTable->clearContents();
            m_transcriptTable->setRowCount(0);
            m_updatingTranscriptInspector = false;
            m_loadedTranscriptDoc = QJsonDocument();
            m_loadedTranscriptPath.clear();
            {
                QSignalBlocker videoBlock1(m_videoTranslationXSpin);
                QSignalBlocker videoBlock2(m_videoTranslationYSpin);
                QSignalBlocker videoBlock3(m_videoRotationSpin);
                QSignalBlocker videoBlock4(m_videoScaleXSpin);
                QSignalBlocker videoBlock5(m_videoScaleYSpin);
                QSignalBlocker videoBlock6(m_videoInterpolationCombo);
                QSignalBlocker videoBlock7(m_mirrorHorizontalCheckBox);
                QSignalBlocker videoBlock8(m_mirrorVerticalCheckBox);
                m_videoTranslationXSpin->setValue(0.0);
                m_videoTranslationYSpin->setValue(0.0);
                m_videoRotationSpin->setValue(0.0);
                m_videoScaleXSpin->setValue(1.0);
                m_videoScaleYSpin->setValue(1.0);
                m_videoInterpolationCombo->setCurrentIndex(0);
                m_mirrorHorizontalCheckBox->setChecked(false);
                m_mirrorVerticalCheckBox->setChecked(false);
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
            QStringLiteral("Selected clip: %1\nOutput format: %2\nExport range: %3 -> %4\nSpeech filter: %5\nTimeline length: %6")
                .arg(clip->label)
                .arg(m_outputFormatCombo ? m_outputFormatCombo->currentText() : QStringLiteral("MP4 (H.264)"))
                .arg(frameToTimecode(m_timeline ? m_timeline->exportStartFrame() : 0))
                .arg(frameToTimecode(m_timeline ? m_timeline->exportEndFrame() : 0))
                .arg(m_exportOnlyTranscriptWordsCheckBox && m_exportOnlyTranscriptWordsCheckBox->isChecked()
                         ? QStringLiteral("Only frames with transcript words")
                         : QStringLiteral("Off"))
                .arg(frameToTimecode(m_timeline ? m_timeline->totalFrames() : 0)));
        m_keyframesInspectorClipLabel->setText(QStringLiteral("Keyframes  %1").arg(clip->label));
        m_keyframeSpaceCheckBox->setEnabled(enabled);
        m_updatingVideoInspector = true;
        m_videoKeyframeTable->clearContents();
        m_videoKeyframeTable->setRowCount(0);
        if (clip->transformKeyframes.isEmpty()) {
            m_keyframesInspectorDetailsLabel->setText(
                QStringLiteral("Values: %1\nNo keyframes available for this clip.\nBase scale: %2 x %3")
                    .arg(showClipRelativeKeyframes() ? QStringLiteral("Clip-relative") : QStringLiteral("Project-relative"))
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
            const int64_t displayedCurrentFrame = keyframeFrameForInspectorDisplay(*clip, localFrame);
            const int64_t displayedNearestFrame =
                nearestIndex >= 0 ? keyframeFrameForInspectorDisplay(*clip, clip->transformKeyframes[nearestIndex].frame) : -1;
            QSet<int64_t> boundaryFrames;
            int lowerBoundaryIndex = -1;
            int upperBoundaryIndex = -1;
            for (int i = 0; i < clip->transformKeyframes.size(); ++i) {
                const int64_t frame = clip->transformKeyframes[i].frame;
                if (frame <= localFrame) {
                    lowerBoundaryIndex = i;
                }
                if (frame >= localFrame) {
                    upperBoundaryIndex = i;
                    break;
                }
            }
            if (lowerBoundaryIndex < 0 && !clip->transformKeyframes.isEmpty()) {
                lowerBoundaryIndex = 0;
            }
            if (upperBoundaryIndex < 0 && !clip->transformKeyframes.isEmpty()) {
                upperBoundaryIndex = clip->transformKeyframes.size() - 1;
            }
            if (lowerBoundaryIndex >= 0) {
                boundaryFrames.insert(clip->transformKeyframes[lowerBoundaryIndex].frame);
            }
            if (upperBoundaryIndex >= 0) {
                boundaryFrames.insert(clip->transformKeyframes[upperBoundaryIndex].frame);
            }
            m_keyframesInspectorDetailsLabel->setText(
                QStringLiteral("Values: %1\nCurrent clip frame: %2\nNearest keyframe: %3\nBase scale: %4 x %5")
                    .arg(showClipRelativeKeyframes() ? QStringLiteral("Clip-relative") : QStringLiteral("Project-relative"))
                    .arg(frameToTimecode(displayedCurrentFrame))
                    .arg(nearestIndex >= 0 ? frameToTimecode(displayedNearestFrame)
                                           : QStringLiteral("none"))
                    .arg(clip->baseScaleX, 0, 'f', 3)
                    .arg(clip->baseScaleY, 0, 'f', 3));
            m_videoKeyframeTable->setRowCount(clip->transformKeyframes.size());
            for (int i = 0; i < clip->transformKeyframes.size(); ++i) {
                const TimelineClip::TransformKeyframe displayedKeyframe =
                    keyframeForInspectorDisplay(*clip, clip->transformKeyframes[i]);
                const int64_t displayedFrame =
                    keyframeFrameForInspectorDisplay(*clip, clip->transformKeyframes[i].frame);
                const bool nearest = i == nearestIndex;
                const QStringList values = {
                    QString::number(displayedFrame),
                    QString::number(displayedKeyframe.translationX, 'f', 1),
                    QString::number(displayedKeyframe.translationY, 'f', 1),
                    QString::number(displayedKeyframe.rotation, 'f', 1),
                    QString::number(displayedKeyframe.scaleX, 'f', 3),
                    QString::number(displayedKeyframe.scaleY, 'f', 3),
                    displayedKeyframe.interpolated ? QStringLiteral("Interpolated") : QStringLiteral("Sudden")
                };
                for (int column = 0; column < values.size(); ++column) {
                    auto* item = new QTableWidgetItem(values[column]);
                    item->setData(Qt::UserRole, QVariant::fromValue(static_cast<qint64>(clip->transformKeyframes[i].frame)));
                    if (column == 0 && clip->transformKeyframes[i].frame == 0) {
                        item->setFlags(item->flags() & ~Qt::ItemIsEditable);
                        item->setToolTip(QStringLiteral("The first keyframe is fixed at frame 0."));
                    }
                    if (column == 6) {
                        item->setToolTip(QStringLiteral("Use Interpolated or Sudden"));
                    }
                    if (boundaryFrames.contains(clip->transformKeyframes[i].frame)) {
                        item->setBackground(QColor(QStringLiteral("#d97a1f")));
                        item->setForeground(QColor(QStringLiteral("#ffffff")));
                    } else if (nearest) {
                        item->setBackground(QColor(QStringLiteral("#2a3948")));
                        item->setForeground(QColor(QStringLiteral("#ffffff")));
                    }
                    m_videoKeyframeTable->setItem(i, column, item);
                }
                if (boundaryFrames.contains(clip->transformKeyframes[i].frame) || nearest) {
                    for (int column = 0; column < m_videoKeyframeTable->columnCount(); ++column) {
                        if (QTableWidgetItem* item = m_videoKeyframeTable->item(i, column)) {
                            if (boundaryFrames.contains(clip->transformKeyframes[i].frame)) {
                                item->setBackground(QColor(QStringLiteral("#d97a1f")));
                                item->setForeground(QColor(QStringLiteral("#ffffff")));
                            } else {
                                item->setBackground(QColor(QStringLiteral("#2a3948")));
                                item->setForeground(QColor(QStringLiteral("#ffffff")));
                            }
                        }
                    }
                }
            }
        }
        int selectedRow = -1;
        for (int i = 0; i < m_videoKeyframeTable->rowCount(); ++i) {
            QTableWidgetItem* item = m_videoKeyframeTable->item(i, 0);
            if (item && item->data(Qt::UserRole).toLongLong() == m_selectedVideoKeyframeFrame) {
                selectedRow = i;
            }
            if (item && m_selectedVideoKeyframeFrames.contains(item->data(Qt::UserRole).toLongLong())) {
                m_videoKeyframeTable->selectRow(i);
            }
        }
        if (selectedRow >= 0) {
            m_videoKeyframeTable->setCurrentCell(selectedRow, 0);
        }
        const int keyframeIndex = selectedVideoKeyframeIndex(*clip);
        m_removeVideoKeyframeButton->setEnabled(enabled && hasRemovableVideoKeyframeSelection(*clip));
        {
            QSignalBlocker videoBlock1(m_videoTranslationXSpin);
            QSignalBlocker videoBlock2(m_videoTranslationYSpin);
            QSignalBlocker videoBlock3(m_videoRotationSpin);
            QSignalBlocker videoBlock4(m_videoScaleXSpin);
            QSignalBlocker videoBlock5(m_videoScaleYSpin);
            QSignalBlocker videoBlock6(m_videoInterpolationCombo);
            QSignalBlocker videoBlock7(m_mirrorHorizontalCheckBox);
            QSignalBlocker videoBlock8(m_mirrorVerticalCheckBox);
            if (keyframeIndex >= 0) {
                const TimelineClip::TransformKeyframe displayedKeyframe =
                    keyframeForInspectorDisplay(*clip, clip->transformKeyframes[keyframeIndex]);
                m_videoTranslationXSpin->setValue(displayedKeyframe.translationX);
                m_videoTranslationYSpin->setValue(displayedKeyframe.translationY);
                m_videoRotationSpin->setValue(displayedKeyframe.rotation);
                m_videoScaleXSpin->setValue(std::abs(displayedKeyframe.scaleX));
                m_videoScaleYSpin->setValue(std::abs(displayedKeyframe.scaleY));
                m_mirrorHorizontalCheckBox->setChecked(displayedKeyframe.scaleX < 0.0);
                m_mirrorVerticalCheckBox->setChecked(displayedKeyframe.scaleY < 0.0);
                m_videoInterpolationCombo->setCurrentIndex(displayedKeyframe.interpolated ? 0 : 1);
            } else {
                m_videoTranslationXSpin->setValue(0.0);
                m_videoTranslationYSpin->setValue(0.0);
                m_videoRotationSpin->setValue(0.0);
                m_videoScaleXSpin->setValue(1.0);
                m_videoScaleYSpin->setValue(1.0);
                m_videoInterpolationCombo->setCurrentIndex(0);
                m_mirrorHorizontalCheckBox->setChecked(false);
                m_mirrorVerticalCheckBox->setChecked(false);
            }
        }
        m_updatingVideoInspector = false;

        const QString transcriptPath = transcriptPathForClip(*clip);
        const QFileInfo transcriptInfo(transcriptPath);
        m_transcriptInspectorClipLabel->setText(QStringLiteral("Transcript  %1").arg(clip->label));
        m_updatingTranscriptInspector = true;
        m_transcriptTable->clearContents();
        m_transcriptTable->setRowCount(0);
        m_loadedTranscriptDoc = QJsonDocument();
        m_loadedTranscriptPath.clear();
        if (!transcriptInfo.exists() || !transcriptInfo.isFile()) {
            m_transcriptInspectorDetailsLabel->setText(
                QStringLiteral("No transcript JSON found.\nExpected: %1")
                    .arg(QDir::toNativeSeparators(transcriptPath)));
        } else {
            QFile transcriptFile(transcriptPath);
            if (!transcriptFile.open(QIODevice::ReadOnly)) {
                m_transcriptInspectorDetailsLabel->setText(
                    QStringLiteral("Could not open transcript JSON:\n%1")
                        .arg(QDir::toNativeSeparators(transcriptPath)));
            } else {
                QJsonParseError parseError;
                const QJsonDocument transcriptDoc =
                    QJsonDocument::fromJson(transcriptFile.readAll(), &parseError);
                if (parseError.error != QJsonParseError::NoError || !transcriptDoc.isObject()) {
                    m_transcriptInspectorDetailsLabel->setText(
                        QStringLiteral("Invalid transcript JSON:\n%1")
                            .arg(parseError.errorString()));
                } else {
                    m_loadedTranscriptDoc = transcriptDoc;
                    m_loadedTranscriptPath = transcriptPath;
                    const QJsonArray segments = transcriptDoc.object().value(QStringLiteral("segments")).toArray();
                    int row = 0;
                    const double prependSeconds = m_transcriptPrependMs / 1000.0;
                    for (int segmentIndex = 0; segmentIndex < segments.size(); ++segmentIndex) {
                        const QJsonObject segmentObj = segments[segmentIndex].toObject();
                        const QJsonArray words = segmentObj.value(QStringLiteral("words")).toArray();
                        for (int wordIndex = 0; wordIndex < words.size(); ++wordIndex) {
                            const QJsonObject wordObj = words[wordIndex].toObject();
                            m_transcriptTable->insertRow(row);
                            // Ground truth times
                            const double gtStart = wordObj.value(QStringLiteral("start")).toDouble();
                            const double gtEnd = wordObj.value(QStringLiteral("end")).toDouble();
                            // Display times with runtime offset applied
                            const double displayStart = qMax(0.0, gtStart + prependSeconds);
                            auto* startItem = new QTableWidgetItem(secondsToTranscriptTime(displayStart));
                            auto* endItem = new QTableWidgetItem(secondsToTranscriptTime(gtEnd));
                            auto* textItem = new QTableWidgetItem(wordObj.value(QStringLiteral("word")).toString());
                            // Use display-adjusted frame for playhead sync
                            const int64_t startFrame = qMax<int64_t>(
                                0, static_cast<int64_t>(std::floor(displayStart * kTimelineFps)));
                            const int64_t endFrame = qMax<int64_t>(
                                startFrame,
                                static_cast<int64_t>(std::ceil(gtEnd * kTimelineFps)) - 1);
                            startItem->setData(Qt::UserRole, segmentIndex);
                            startItem->setData(Qt::UserRole + 1, wordIndex);
                            startItem->setData(Qt::UserRole + 2, QVariant::fromValue<qlonglong>(startFrame));
                            startItem->setData(Qt::UserRole + 3, QVariant::fromValue<qlonglong>(endFrame));
                            endItem->setData(Qt::UserRole, segmentIndex);
                            endItem->setData(Qt::UserRole + 1, wordIndex);
                            endItem->setData(Qt::UserRole + 2, QVariant::fromValue<qlonglong>(startFrame));
                            endItem->setData(Qt::UserRole + 3, QVariant::fromValue<qlonglong>(endFrame));
                            textItem->setData(Qt::UserRole, segmentIndex);
                            textItem->setData(Qt::UserRole + 1, wordIndex);
                            textItem->setData(Qt::UserRole + 2, QVariant::fromValue<qlonglong>(startFrame));
                            textItem->setData(Qt::UserRole + 3, QVariant::fromValue<qlonglong>(endFrame));
                            textItem->setToolTip(textItem->text());
                            m_transcriptTable->setItem(row, 0, startItem);
                            m_transcriptTable->setItem(row, 1, endItem);
                            m_transcriptTable->setItem(row, 2, textItem);
                            ++row;
                        }
                    }
                    m_transcriptInspectorDetailsLabel->setText(
                        QStringLiteral("%1 words from %2")
                            .arg(m_transcriptTable->rowCount())
                            .arg(QDir::toNativeSeparators(transcriptPath)));
                }
            }
        }
        m_updatingTranscriptInspector = false;
        const bool transcriptOverlayAvailable =
            clip->mediaType == ClipMediaType::Audio && !m_loadedTranscriptPath.isEmpty();
        for (QWidget* widget : {static_cast<QWidget*>(m_transcriptOverlayEnabledCheckBox),
                                static_cast<QWidget*>(m_transcriptMaxLinesSpin),
                                static_cast<QWidget*>(m_transcriptMaxCharsSpin),
                                static_cast<QWidget*>(m_transcriptAutoScrollCheckBox),
                                static_cast<QWidget*>(m_transcriptOverlayXSpin),
                                static_cast<QWidget*>(m_transcriptOverlayYSpin),
                                static_cast<QWidget*>(m_transcriptOverlayWidthSpin),
                                static_cast<QWidget*>(m_transcriptOverlayHeightSpin),
                                static_cast<QWidget*>(m_transcriptFontFamilyCombo),
                                static_cast<QWidget*>(m_transcriptFontSizeSpin),
                                static_cast<QWidget*>(m_transcriptBoldCheckBox),
                                static_cast<QWidget*>(m_transcriptItalicCheckBox)}) {
            if (widget) {
                widget->setEnabled(transcriptOverlayAvailable);
            }
        }
        if (transcriptOverlayAvailable) {
            QSignalBlocker block1(m_transcriptOverlayEnabledCheckBox);
            QSignalBlocker block2(m_transcriptMaxLinesSpin);
            QSignalBlocker block3(m_transcriptMaxCharsSpin);
            QSignalBlocker block4(m_transcriptAutoScrollCheckBox);
            QSignalBlocker block5(m_transcriptOverlayXSpin);
            QSignalBlocker block6(m_transcriptOverlayYSpin);
            QSignalBlocker block7(m_transcriptOverlayWidthSpin);
            QSignalBlocker block8(m_transcriptOverlayHeightSpin);
            QSignalBlocker block9(m_transcriptFontFamilyCombo);
            QSignalBlocker block10(m_transcriptFontSizeSpin);
            QSignalBlocker block11(m_transcriptBoldCheckBox);
            QSignalBlocker block12(m_transcriptItalicCheckBox);
            m_transcriptOverlayEnabledCheckBox->setChecked(clip->transcriptOverlay.enabled);
            m_transcriptMaxLinesSpin->setValue(clip->transcriptOverlay.maxLines);
            m_transcriptMaxCharsSpin->setValue(clip->transcriptOverlay.maxCharsPerLine);
            m_transcriptAutoScrollCheckBox->setChecked(clip->transcriptOverlay.autoScroll);
            m_transcriptOverlayXSpin->setValue(clip->transcriptOverlay.translationX);
            m_transcriptOverlayYSpin->setValue(clip->transcriptOverlay.translationY);
            m_transcriptOverlayWidthSpin->setValue(qRound(clip->transcriptOverlay.boxWidth));
            m_transcriptOverlayHeightSpin->setValue(qRound(clip->transcriptOverlay.boxHeight));
            m_transcriptFontFamilyCombo->setCurrentFont(QFont(clip->transcriptOverlay.fontFamily));
            m_transcriptFontSizeSpin->setValue(clip->transcriptOverlay.fontPointSize);
            m_transcriptBoldCheckBox->setChecked(clip->transcriptOverlay.bold);
            m_transcriptItalicCheckBox->setChecked(clip->transcriptOverlay.italic);
            m_transcriptInspectorDetailsLabel->setText(
                m_transcriptInspectorDetailsLabel->text() +
                QStringLiteral("\nOverlay: %1, %2 lines, %3 chars/line")
                    .arg(clip->transcriptOverlay.enabled ? QStringLiteral("on") : QStringLiteral("off"))
                    .arg(clip->transcriptOverlay.maxLines)
                    .arg(clip->transcriptOverlay.maxCharsPerLine));
        }
        syncTranscriptTableToPlayhead();
        if (clipHasVisuals(*clip)) {
            m_gradingPathLabel->setText(QDir::toNativeSeparators(clip->filePath));
        } else {
            m_gradingPathLabel->setText(QStringLiteral("Audio clips do not use visual grading controls."));
        }
        const bool isAudioClip = clipIsAudioOnly(*clip);
        m_audioFadeSamplesSpin->setEnabled(isAudioClip);
        if (isAudioClip) {
            m_audioInspectorClipLabel->setText(QStringLiteral("Track %1  %2").arg(clip->trackIndex + 1).arg(clip->label));
            m_audioInspectorDetailsLabel->setText(
                QStringLiteral("Path: %1\nDuration: %2\nTransport volume: %3%%\nMuted: %4\nFade: %5 samples")
                    .arg(QDir::toNativeSeparators(clip->filePath))
                    .arg(frameToTimecode(clip->durationFrames))
                    .arg(m_preview ? m_preview->audioVolumePercent() : 0)
                    .arg(m_preview && m_preview->audioMuted() ? QStringLiteral("yes") : QStringLiteral("no"))
                    .arg(clip->fadeSamples));
            m_updatingAudioInspector = true;
            m_audioFadeSamplesSpin->setValue(clip->fadeSamples);
            m_updatingAudioInspector = false;
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
            {QStringLiteral("debug"), debugControlsSnapshot()},
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
    QToolButton* m_newProjectButton = nullptr;
    QToolButton* m_saveProjectAsButton = nullptr;
    QLabel* m_rootPathLabel = nullptr;
    QLabel* m_galleryTitleLabel = nullptr;
    QLabel* m_projectSectionLabel = nullptr;
    QListWidget* m_projectsList = nullptr;
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
    QSpinBox* m_audioFadeSamplesSpin = nullptr;
    bool m_updatingAudioInspector = false;
    QLabel* m_transcriptInspectorClipLabel = nullptr;
    QLabel* m_transcriptInspectorDetailsLabel = nullptr;
    QLabel* m_syncInspectorClipLabel = nullptr;
    QLabel* m_syncInspectorDetailsLabel = nullptr;
    QDoubleSpinBox* m_brightnessSpin = nullptr;
    QDoubleSpinBox* m_contrastSpin = nullptr;
    QDoubleSpinBox* m_saturationSpin = nullptr;
    QDoubleSpinBox* m_opacitySpin = nullptr;
    QCheckBox* m_bypassGradingCheckBox = nullptr;
    QSpinBox* m_outputWidthSpin = nullptr;
    QSpinBox* m_outputHeightSpin = nullptr;
    QSpinBox* m_exportStartSpin = nullptr;
    QSpinBox* m_exportEndSpin = nullptr;
    QComboBox* m_outputFormatCombo = nullptr;
    QCheckBox* m_exportOnlyTranscriptWordsCheckBox = nullptr;
    QDoubleSpinBox* m_videoTranslationXSpin = nullptr;
    QDoubleSpinBox* m_videoTranslationYSpin = nullptr;
    QDoubleSpinBox* m_videoRotationSpin = nullptr;
    QDoubleSpinBox* m_videoScaleXSpin = nullptr;
    QDoubleSpinBox* m_videoScaleYSpin = nullptr;
    QComboBox* m_videoInterpolationCombo = nullptr;
    QCheckBox* m_mirrorHorizontalCheckBox = nullptr;
    QCheckBox* m_mirrorVerticalCheckBox = nullptr;
    QCheckBox* m_lockVideoScaleCheckBox = nullptr;
    QCheckBox* m_keyframeSpaceCheckBox = nullptr;
    QCheckBox* m_transcriptOverlayEnabledCheckBox = nullptr;
    QSpinBox* m_transcriptMaxLinesSpin = nullptr;
    QSpinBox* m_transcriptMaxCharsSpin = nullptr;
    QCheckBox* m_transcriptAutoScrollCheckBox = nullptr;
    QCheckBox* m_transcriptFollowCurrentWordCheckBox = nullptr;
    QSpinBox* m_transcriptPrependMsSpin = nullptr;
    int m_transcriptPrependMs = 0;  // Runtime offset (not persisted)
    QDoubleSpinBox* m_transcriptOverlayXSpin = nullptr;
    QDoubleSpinBox* m_transcriptOverlayYSpin = nullptr;
    QSpinBox* m_transcriptOverlayWidthSpin = nullptr;
    QSpinBox* m_transcriptOverlayHeightSpin = nullptr;
    QFontComboBox* m_transcriptFontFamilyCombo = nullptr;
    QSpinBox* m_transcriptFontSizeSpin = nullptr;
    QCheckBox* m_transcriptBoldCheckBox = nullptr;
    QCheckBox* m_transcriptItalicCheckBox = nullptr;
    QTableWidget* m_videoKeyframeTable = nullptr;
    QTableWidget* m_transcriptTable = nullptr;
    QTableWidget* m_syncTable = nullptr;
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
    bool m_updatingProjectsList = false;
    bool m_pendingSaveAfterLoad = false;
    bool m_restoringHistory = false;
    int64_t m_currentPlaybackSample = 0;
    QString m_currentRootPath;
    QString m_galleryFolderPath;
    QString m_currentProjectId;
    QStringList m_expandedExplorerPaths;
    QByteArray m_lastSavedState;
    QJsonArray m_historyEntries;
    int m_historyIndex = -1;
    bool m_updatingVideoInspector = false;
    bool m_updatingTranscriptInspector = false;
    bool m_updatingSyncInspector = false;
    bool m_syncingScaleControls = false;
    int64_t m_selectedVideoKeyframeFrame = -1;
    QSet<int64_t> m_selectedVideoKeyframeFrames;
    QString m_loadedTranscriptPath;
    QJsonDocument m_loadedTranscriptDoc;
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
