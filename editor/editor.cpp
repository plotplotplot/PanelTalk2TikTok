#include "frame_handle.h"
#include "async_decoder.h"
#include "timeline_cache.h"
#include "memory_budget.h"
#include "gpu_compositor.h"
#include "control_server.h"

#include <QApplication>
#include <QColor>
#include <QDateTime>
#include <QDir>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDragEnterEvent>
#include <QDoubleSpinBox>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
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
#include <QSlider>
#include <QSplitter>
#include <QStyle>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QStackedLayout>
#include <QPointer>
#include <QElapsedTimer>

#include <QtGui/private/qrhi_p.h>
#include <QtGui/private/qrhigles2_p.h>

#include <algorithm>
#include <atomic>
#include <limits>
#include <memory>
#include <cmath>

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

void playbackTrace(const QString& stage, const QString& detail = QString()) {
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
// TimelineClip - Timeline clip data structure
// ============================================================================
struct TimelineClip {
    QString id;
    QString filePath;
    QString label;
    int64_t startFrame = 0;
    int64_t durationFrames = 90;
    int trackIndex = 0;
    QColor color;
    qreal brightness = 0.0;
    qreal contrast = 1.0;
    qreal saturation = 1.0;
    qreal opacity = 1.0;
};

namespace {
int clampChannel(int value) {
    return qBound(0, value, 255);
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
            QColor color = QColor::fromRgb(row[x]);
            qreal h = 0.0;
            qreal s = 0.0;
            qreal l = 0.0;
            qreal a = 0.0;
            color.getHslF(&h, &s, &l, &a);

            int r = clampChannel(qRound(((color.red() - 127.5) * clip.contrast) + 127.5 + clip.brightness * 255.0));
            int g = clampChannel(qRound(((color.green() - 127.5) * clip.contrast) + 127.5 + clip.brightness * 255.0));
            int b = clampChannel(qRound(((color.blue() - 127.5) * clip.contrast) + 127.5 + clip.brightness * 255.0));

            QColor adjusted(r, g, b, color.alpha());
            adjusted.getHslF(&h, &s, &l, &a);
            s = qBound(0.0, s * clip.saturation, 1.0);
            a = qBound(0.0, a * clip.opacity, 1.0);
            adjusted.setHslF(h, s, l, a);
            row[x] = adjusted.rgba();
        }
    }
    return graded.convertToFormat(QImage::Format_ARGB32_Premultiplied);
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
            if (targetFrame < clip.startFrame || targetFrame >= clip.startFrame + clip.durationFrames) {
                continue;
            }

            hasActiveClip = true;
            const int64_t localFrame = targetFrame - clip.startFrame;
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
            {QStringLiteral("repaint_timer_active"), m_repaintTimer.isActive()}
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

private:
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
                return a.trackIndex > b.trackIndex;
            });
        return active;
    }
    
    void requestFramesForCurrentPosition() {
        static constexpr int kMaxVisibleBacklog = 2;
        QVector<const TimelineClip*> activeClips;
        activeClips.reserve(m_clips.size());
        for (const TimelineClip& clip : m_clips) {
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
            const int64_t localFrame = m_currentFrame - clip->startFrame;
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
        
        // Draw background panel
        painter->setPen(QPen(QColor(255, 255, 255, 40), 1.5));
        painter->setBrush(QColor(255, 255, 255, 18));
        painter->drawRoundedRect(safeRect, 18, 18);
        
        if (activeClips.isEmpty()) {
            drawEmptyState(painter, safeRect);
            painter->restore();
            return;
        }

        const QRect compositeRect = safeRect.adjusted(12, 12, -12, -12);
        bool drewAnyFrame = false;
        bool waitingForFrame = false;

        for (const TimelineClip& clip : activeClips) {
            const int64_t localFrame = m_currentFrame - clip.startFrame;
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
                          QStringLiteral("No active clips at this frame.\nFrame %1\nQRhi backend: %2")
                              .arg(m_currentFrame)
                              .arg(m_renderer->backendName()));
    }
    
    void drawFrameLayer(QPainter* painter, const QRect& targetRect,
                        const TimelineClip& clip, const FrameHandle& frame) {
        painter->save();
        painter->setClipRect(targetRect);

        if (!frame.isNull() && frame.hasCpuImage()) {
            const QImage img = applyClipGrade(frame.cpuImage(), clip);
            const QRect fitted = fitRect(img.size(), targetRect);
            painter->drawImage(fitted, img);
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
                          QStringLiteral("Frame %1  |  QRhi backend: %2")
                              .arg(m_currentFrame)
                              .arg(m_renderer->backendName()));
        painter->restore();
    }
    
    std::unique_ptr<PreviewRenderer> m_renderer;
    std::unique_ptr<AsyncDecoder> m_decoder;
    std::unique_ptr<TimelineCache> m_cache;
    
    bool m_playing = false;
    int64_t m_currentFrame = 0;
    int m_clipCount = 0;
    QVector<TimelineClip> m_clips;
    QSet<QString> m_registeredClips;
    QTimer m_repaintTimer;
    qint64 m_lastFrameRequestMs = 0;
    qint64 m_lastFrameReadyMs = 0;
    qint64 m_lastPaintMs = 0;
    qint64 m_lastRepaintScheduleMs = 0;
};

// ============================================================================
// TimelineWidget - Timeline editing widget
// ============================================================================
class TimelineWidget final : public QWidget {
    Q_OBJECT
public:
    explicit TimelineWidget(QWidget* parent = nullptr) : QWidget(parent) {
        setAcceptDrops(true);
        setMinimumHeight(150);
        setMouseTracking(true);
        setAutoFillBackground(true);
        
        QPalette pal = palette();
        pal.setColor(QPalette::Window, QColor(QStringLiteral("#0f1216")));
        setPalette(pal);
    }
    
    void setCurrentFrame(int64_t frame) {
        m_currentFrame = qMax<int64_t>(0, frame);
        update();
    }
    
    int64_t currentFrame() const { return m_currentFrame; }
    
    int64_t totalFrames() const {
        int64_t lastFrame = 300;
        for (const TimelineClip& clip : m_clips) {
            lastFrame = qMax(lastFrame, clip.startFrame + clip.durationFrames + 30);
        }
        return lastFrame;
    }
    
    void addClipFromFile(const QString& filePath, int64_t startFrame = -1);
    
    const QVector<TimelineClip>& clips() const { return m_clips; }
    
    void setClips(const QVector<TimelineClip>& clips) {
        m_clips = clips;
        normalizeTrackIndices();
        sortClips();
        update();
    }
    
    std::function<void(int64_t)> seekRequested;
    std::function<void()> clipsChanged;

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void paintEvent(QPaintEvent*) override;

private:
    static constexpr int kTimelineFps = 30;
    
    void sortClips();
    int clipIndexAt(const QPoint& pos) const;
    int trackIndexAt(const QPoint& pos) const;
    int64_t frameFromX(qreal x) const;
    int xFromFrame(int64_t frame) const;
    int widthForFrames(int64_t frames) const;
    QString timecodeForFrame(int64_t frame) const;
    int64_t mediaDurationFrames(const QFileInfo& info) const;
    bool hasFileUrls(const QMimeData* mimeData) const;
    int64_t guessDurationFrames(const QString& suffix) const;
    QColor colorForPath(const QString& path) const;
    
    int trackCount() const;
    int nextTrackIndex() const;
    void normalizeTrackIndices();
    QRect drawRect() const;
    QRect rulerRect() const;
    QRect trackRect() const;
    QRect timelineContentRect() const;
    QRect trackLabelRect(int trackIndex) const;
    QRect clipRectFor(const TimelineClip& clip) const;
    
    QVector<TimelineClip> m_clips;
    int64_t m_currentFrame = 0;
    int64_t m_dropFrame = -1;
    int m_draggedClipIndex = -1;
    int64_t m_dragOffsetFrames = 0;
    int m_draggedTrackIndex = -1;
    int m_trackDropIndex = -1;
    qreal m_pixelsPerFrame = 4.0;
    int64_t m_frameOffset = 0;
};

namespace {
constexpr int kTimelineOuterMargin = 16;
constexpr int kTimelineRulerHeight = 28;
constexpr int kTimelineTrackGap = 12;
constexpr int kTimelineTrackRowHeight = 44;
constexpr int kTimelineClipHeight = 32;
constexpr int kTimelineLabelWidth = 52;
constexpr int kTimelineLabelGap = 12;
}

void TimelineWidget::sortClips() {
    std::sort(m_clips.begin(), m_clips.end(), 
        [](const TimelineClip& a, const TimelineClip& b) {
            if (a.trackIndex == b.trackIndex) {
                if (a.startFrame == b.startFrame) {
                    return a.label < b.label;
                }
                return a.startFrame < b.startFrame;
            }
            return a.trackIndex < b.trackIndex;
        });
}

int TimelineWidget::trackCount() const {
    int maxTrack = -1;
    for (const TimelineClip& clip : m_clips) {
        maxTrack = qMax(maxTrack, clip.trackIndex);
    }
    return qMax(1, maxTrack + 1);
}

int TimelineWidget::nextTrackIndex() const {
    return trackCount();
}

void TimelineWidget::normalizeTrackIndices() {
    QSet<int> tracks;
    for (const TimelineClip& clip : m_clips) {
        tracks.insert(clip.trackIndex);
    }

    QList<int> sortedTracks = tracks.values();
    std::sort(sortedTracks.begin(), sortedTracks.end());

    QHash<int, int> remap;
    for (int i = 0; i < sortedTracks.size(); ++i) {
        remap.insert(sortedTracks[i], i);
    }

    for (TimelineClip& clip : m_clips) {
        clip.trackIndex = remap.value(clip.trackIndex, 0);
    }
}

QRect TimelineWidget::drawRect() const {
    return rect().adjusted(kTimelineOuterMargin, kTimelineOuterMargin,
                           -kTimelineOuterMargin, -kTimelineOuterMargin);
}

QRect TimelineWidget::rulerRect() const {
    const QRect draw = drawRect();
    return QRect(draw.left(), draw.top(), draw.width(), kTimelineRulerHeight);
}

QRect TimelineWidget::trackRect() const {
    const QRect draw = drawRect();
    const QRect ruler = rulerRect();
    return QRect(draw.left(), ruler.bottom() + kTimelineTrackGap, draw.width(),
                 draw.height() - (kTimelineRulerHeight + kTimelineTrackGap));
}

QRect TimelineWidget::timelineContentRect() const {
    const QRect tracks = trackRect();
    return QRect(tracks.left() + kTimelineLabelWidth + kTimelineLabelGap,
                 tracks.top(),
                 qMax(0, tracks.width() - kTimelineLabelWidth - kTimelineLabelGap),
                 tracks.height());
}

QRect TimelineWidget::trackLabelRect(int trackIndex) const {
    const QRect tracks = trackRect();
    return QRect(tracks.left() + 6,
                 tracks.top() + 12 + trackIndex * kTimelineTrackRowHeight,
                 kTimelineLabelWidth - 12,
                 kTimelineClipHeight);
}

QRect TimelineWidget::clipRectFor(const TimelineClip& clip) const {
    const int clipX = xFromFrame(clip.startFrame);
    const int clipW = qMax(40, widthForFrames(clip.durationFrames));
    const int clipY = trackRect().top() + 12 + clip.trackIndex * kTimelineTrackRowHeight;
    return QRect(clipX, clipY, clipW, kTimelineClipHeight);
}

int TimelineWidget::trackIndexAt(const QPoint& pos) const {
    for (int i = 0; i < trackCount(); ++i) {
        if (trackLabelRect(i).contains(pos)) {
            return i;
        }
    }
    return -1;
}

int TimelineWidget::clipIndexAt(const QPoint& pos) const {
    for (int i = 0; i < m_clips.size(); ++i) {
        if (clipRectFor(m_clips[i]).contains(pos)) {
            return i;
        }
    }
    return -1;
}

int64_t TimelineWidget::frameFromX(qreal x) const {
    const int left = timelineContentRect().left();
    const qreal normalized = qMax<qreal>(0.0, x - left);
    return m_frameOffset + static_cast<int64_t>(normalized / m_pixelsPerFrame);
}

int TimelineWidget::xFromFrame(int64_t frame) const {
    return timelineContentRect().left() + widthForFrames(frame - m_frameOffset);
}

int TimelineWidget::widthForFrames(int64_t frames) const {
    return static_cast<int>(frames * m_pixelsPerFrame);
}

QString TimelineWidget::timecodeForFrame(int64_t frame) const {
    const int64_t seconds = frame / kTimelineFps;
    const int64_t minutes = seconds / 60;
    const int64_t secs = seconds % 60;
    return QStringLiteral("%1:%2")
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(secs, 2, 10, QLatin1Char('0'));
}

int64_t TimelineWidget::mediaDurationFrames(const QFileInfo& info) const {
    // Will be updated with async decoder info
    QString suffix = info.suffix().toLower();
    return guessDurationFrames(suffix);
}

bool TimelineWidget::hasFileUrls(const QMimeData* mimeData) const {
    if (!mimeData || !mimeData->hasUrls()) return false;
    for (const QUrl& url : mimeData->urls()) {
        if (url.isLocalFile()) return true;
    }
    return false;
}

int64_t TimelineWidget::guessDurationFrames(const QString& suffix) const {
    static const QHash<QString, int64_t> kDurations = {
        {QStringLiteral("mp4"), 180},
        {QStringLiteral("mov"), 180},
        {QStringLiteral("mkv"), 180},
        {QStringLiteral("webm"), 180},
        {QStringLiteral("png"), 90},
        {QStringLiteral("jpg"), 90},
        {QStringLiteral("jpeg"), 90},
        {QStringLiteral("webp"), 90},
    };
    return kDurations.value(suffix, 120);
}

QColor TimelineWidget::colorForPath(const QString& path) const {
    const quint32 hash = qHash(path);
    return QColor::fromHsv(static_cast<int>(hash % 360), 160, 220, 220);
}

void TimelineWidget::addClipFromFile(const QString& filePath, int64_t startFrame) {
    const QFileInfo info(filePath);
    if (!info.exists() || !info.isFile()) return;
    
    TimelineClip clip;
    clip.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    clip.filePath = filePath;
    clip.label = info.fileName();
    clip.startFrame = startFrame >= 0 ? startFrame : totalFrames();
    clip.durationFrames = mediaDurationFrames(info);
    clip.trackIndex = nextTrackIndex();
    clip.color = colorForPath(filePath);
    
    m_clips.push_back(clip);
    normalizeTrackIndices();
    sortClips();
    
    if (clipsChanged) clipsChanged();
    update();
}

void TimelineWidget::dragEnterEvent(QDragEnterEvent* event) {
    if (hasFileUrls(event->mimeData())) {
        event->acceptProposedAction();
        return;
    }
    QWidget::dragEnterEvent(event);
}

void TimelineWidget::dragMoveEvent(QDragMoveEvent* event) {
    if (hasFileUrls(event->mimeData())) {
        m_dropFrame = frameFromX(event->position().x());
        event->acceptProposedAction();
        update();
        return;
    }
    QWidget::dragMoveEvent(event);
}

void TimelineWidget::dragLeaveEvent(QDragLeaveEvent* event) {
    m_dropFrame = -1;
    QWidget::dragLeaveEvent(event);
    update();
}

void TimelineWidget::dropEvent(QDropEvent* event) {
    if (!hasFileUrls(event->mimeData())) {
        QWidget::dropEvent(event);
        return;
    }
    
    int64_t insertFrame = frameFromX(event->position().x());
    
    for (const QUrl& url : event->mimeData()->urls()) {
        if (!url.isLocalFile()) continue;
        
        const QString filePath = url.toLocalFile();
        const QFileInfo info(filePath);
        if (!info.exists() || info.isDir()) continue;
        
        TimelineClip clip;
        clip.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        clip.filePath = filePath;
        clip.label = info.fileName();
        clip.startFrame = insertFrame;
        clip.durationFrames = mediaDurationFrames(info);
        clip.trackIndex = nextTrackIndex();
        clip.color = colorForPath(filePath);
        
        m_clips.push_back(clip);
        insertFrame += clip.durationFrames + 6;
    }
    
    normalizeTrackIndices();
    sortClips();
    m_dropFrame = -1;
    event->acceptProposedAction();
    
    if (clipsChanged) clipsChanged();
    update();
}

void TimelineWidget::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        const int trackHit = trackIndexAt(event->position().toPoint());
        if (trackHit >= 0) {
            m_draggedTrackIndex = trackHit;
            m_trackDropIndex = trackHit;
            update();
            return;
        }
        const int hitIndex = clipIndexAt(event->position().toPoint());
        if (hitIndex >= 0) {
            m_draggedClipIndex = hitIndex;
            m_dragOffsetFrames = frameFromX(event->position().x()) - m_clips[hitIndex].startFrame;
            m_currentFrame = m_clips[hitIndex].startFrame;
            update();
            return;
        }
        
        const int64_t frame = frameFromX(event->position().x());
        m_currentFrame = frame;
        if (seekRequested) seekRequested(frame);
        update();
        return;
    }
    QWidget::mousePressEvent(event);
}

void TimelineWidget::mouseMoveEvent(QMouseEvent* event) {
    if (m_draggedTrackIndex >= 0 && (event->buttons() & Qt::LeftButton)) {
        const QRect tracks = trackRect();
        const int relativeY = event->position().toPoint().y() - (tracks.top() + 12);
        const int proposed = qBound(0, relativeY / kTimelineTrackRowHeight, trackCount() - 1);
        if (proposed != m_trackDropIndex) {
            m_trackDropIndex = proposed;
            update();
        }
        return;
    }
    if (m_draggedClipIndex >= 0 && (event->buttons() & Qt::LeftButton)) {
        TimelineClip& clip = m_clips[m_draggedClipIndex];
        const int64_t newStartFrame = qMax<int64_t>(0, frameFromX(event->position().x()) - m_dragOffsetFrames);
        clip.startFrame = newStartFrame;
        m_currentFrame = newStartFrame;
        update();
        return;
    }
    QWidget::mouseMoveEvent(event);
}

void TimelineWidget::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && m_draggedTrackIndex >= 0) {
        const int fromTrack = m_draggedTrackIndex;
        const int toTrack = qBound(0, m_trackDropIndex, trackCount() - 1);
        if (fromTrack != toTrack) {
            for (TimelineClip& clip : m_clips) {
                if (clip.trackIndex == fromTrack) {
                    clip.trackIndex = toTrack;
                } else if (fromTrack < toTrack && clip.trackIndex > fromTrack && clip.trackIndex <= toTrack) {
                    clip.trackIndex -= 1;
                } else if (fromTrack > toTrack && clip.trackIndex >= toTrack && clip.trackIndex < fromTrack) {
                    clip.trackIndex += 1;
                }
            }
            normalizeTrackIndices();
            sortClips();
            if (clipsChanged) clipsChanged();
        }
        m_draggedTrackIndex = -1;
        m_trackDropIndex = -1;
        update();
        return;
    }
    if (event->button() == Qt::LeftButton && m_draggedClipIndex >= 0) {
        normalizeTrackIndices();
        sortClips();
        m_draggedClipIndex = -1;
        m_dragOffsetFrames = 0;
        if (clipsChanged) clipsChanged();
        update();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void TimelineWidget::contextMenuEvent(QContextMenuEvent* event) {
    const int clipIndex = clipIndexAt(event->pos());
    if (clipIndex < 0) {
        QWidget::contextMenuEvent(event);
        return;
    }
    
    QMenu menu(this);
    QAction* deleteAction = menu.addAction(QStringLiteral("Delete"));
    QAction* propertiesAction = menu.addAction(QStringLiteral("Properties"));
    
    QAction* selected = menu.exec(event->globalPos());
    if (!selected) return;
    
    if (selected == deleteAction) {
        m_clips.removeAt(clipIndex);
        if (clipsChanged) clipsChanged();
        update();
        return;
    }
    
    if (selected == propertiesAction) {
        const TimelineClip& clip = m_clips[clipIndex];
        QMessageBox::information(this, QStringLiteral("Clip Properties"),
            QStringLiteral("Name: %1\nPath: %2\nStart: %3\nDuration: %4 frames")
                .arg(clip.label)
                .arg(QDir::toNativeSeparators(clip.filePath))
                .arg(timecodeForFrame(clip.startFrame))
                .arg(clip.durationFrames));
        return;
    }
}

void TimelineWidget::wheelEvent(QWheelEvent* event) {
    const QPoint numDegrees = event->angleDelta() / 8;
    if (numDegrees.isNull()) {
        QWidget::wheelEvent(event);
        return;
    }
    
    const int steps = numDegrees.y() / 15;
    if (steps == 0) {
        QWidget::wheelEvent(event);
        return;
    }
    
    if (event->modifiers() & Qt::ControlModifier) {
        const int visibleFrames = qMax(1, static_cast<int>(width() / m_pixelsPerFrame));
        const int panFrames = qMax(1, visibleFrames / 12);
        m_frameOffset = qMax<int64_t>(0, m_frameOffset - steps * panFrames);
        update();
        event->accept();
        return;
    }
    
    const qreal oldPixelsPerFrame = m_pixelsPerFrame;
    const qreal cursorFrame = frameFromX(event->position().x());
    const qreal zoomFactor = steps > 0 ? 1.15 : (1.0 / 1.15);
    m_pixelsPerFrame = qBound(0.25, m_pixelsPerFrame * std::pow(zoomFactor, std::abs(steps)), 24.0);
    
    const qreal localX = event->position().x() - 16.0;
    if (m_pixelsPerFrame > 0.0) {
        const qreal newOffset = cursorFrame - qMax<qreal>(0.0, localX) / m_pixelsPerFrame;
        m_frameOffset = qMax<int64_t>(0, qRound(newOffset));
    }
    
    if (!qFuzzyCompare(oldPixelsPerFrame, m_pixelsPerFrame)) {
        update();
    }
    event->accept();
}

void TimelineWidget::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.fillRect(rect(), QColor(QStringLiteral("#0f1216")));
    
    const QRect draw = drawRect();
    const QRect ruler = rulerRect();
    const QRect tracks = trackRect();
    const QRect content = timelineContentRect();
    
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(QStringLiteral("#171c22")));
    painter.drawRoundedRect(tracks, 10, 10);
    
    painter.setPen(QColor(QStringLiteral("#6d7887")));
    for (int64_t frame = 0; frame <= totalFrames(); frame += 30) {
        const int x = xFromFrame(frame);
        const bool major = (frame % 150) == 0;
        painter.setPen(major ? QColor(QStringLiteral("#8fa0b5")) : QColor(QStringLiteral("#53606e")));
        painter.drawLine(x, ruler.bottom() - (major ? 18 : 10), x, tracks.bottom() - 8);
        
        if (major) {
            painter.drawText(QRect(x + 4, ruler.top(), 56, ruler.height()),
                            Qt::AlignLeft | Qt::AlignVCenter,
                            timecodeForFrame(frame));
        }
    }
    
    painter.setPen(QColor(QStringLiteral("#24303c")));
    for (int track = 0; track < trackCount(); ++track) {
        const QRect labelRect = trackLabelRect(track);
        const bool dragged = track == m_draggedTrackIndex;
        const bool target = track == m_trackDropIndex && m_draggedTrackIndex >= 0;
        painter.setBrush(dragged ? QColor(QStringLiteral("#ff6f61"))
                                 : (target ? QColor(QStringLiteral("#32465f"))
                                           : QColor(QStringLiteral("#202a34"))));
        painter.drawRoundedRect(labelRect, 8, 8);
        painter.setPen(QColor(QStringLiteral("#eef4fa")));
        painter.drawText(labelRect, Qt::AlignCenter, QString::number(track + 1));
        painter.setPen(QColor(QStringLiteral("#24303c")));
        const int dividerY = labelRect.bottom() + ((kTimelineTrackRowHeight - kTimelineClipHeight) / 2);
        painter.drawLine(content.left() - 8, dividerY, tracks.right() - 10, dividerY);
    }
    
    for (const TimelineClip& clip : m_clips) {
        const QRect clipRect = clipRectFor(clip);
        
        painter.setPen(QColor(255, 255, 255, 32));
        painter.setBrush(clip.color);
        painter.drawRoundedRect(clipRect, 7, 7);
        
        painter.setPen(QColor(QStringLiteral("#f4f7fb")));
        painter.drawText(clipRect.adjusted(10, 0, -10, 0),
                        Qt::AlignLeft | Qt::AlignVCenter,
                        painter.fontMetrics().elidedText(clip.label, Qt::ElideRight, 
                                                         clipRect.width() - 20));
    }
    
    if (m_dropFrame >= 0) {
        const int x = xFromFrame(m_dropFrame);
        painter.setPen(QPen(QColor(QStringLiteral("#f7b955")), 2, Qt::DashLine));
        painter.drawLine(x, tracks.top() + 2, x, tracks.bottom() - 2);
    }
    
    const int playheadX = xFromFrame(m_currentFrame);
    painter.setPen(QPen(QColor(QStringLiteral("#ff6f61")), 3));
    painter.drawLine(playheadX, ruler.top(), playheadX, tracks.bottom());
    
    painter.setBrush(QColor(QStringLiteral("#ff6f61")));
    painter.setPen(Qt::NoPen);
    painter.drawRoundedRect(QRect(playheadX - 8, ruler.top(), 16, 12), 4, 4);
}

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
        
        splitter->setStretchFactor(0, 0);
        splitter->setStretchFactor(1, 1);
        splitter->setSizes({320, 1180});

        setCentralWidget(central);

        connect(&m_playbackTimer, &QTimer::timeout, this, &EditorWindow::advanceFrame);
        m_playbackTimer.setInterval(33);
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

private slots:
    void advanceFrame() {
        const int64_t nextFrame = m_timeline->currentFrame() + 1;
        const int64_t wrapped = nextFrame > m_timeline->totalFrames() ? 0 : nextFrame;
        if (m_preview && !m_preview->preparePlaybackAdvance(wrapped)) {
            playbackTrace(QStringLiteral("EditorWindow::advanceFrame.wait"),
                          QStringLiteral("next=%1").arg(wrapped));
            return;
        }
        playbackTrace(QStringLiteral("EditorWindow::advanceFrame"),
                      QStringLiteral("current=%1 next=%2").arg(m_timeline->currentFrame()).arg(wrapped));
        setCurrentFrame(wrapped);
    }

private:
    QString stateFilePath() const {
        return QDir(QDir::currentPath()).filePath(QStringLiteral("editor_state.json"));
    }

    QJsonObject buildStateJson() const {
        QJsonObject root;
        root[QStringLiteral("explorerRoot")] = m_currentRootPath;
        root[QStringLiteral("currentFrame")] = static_cast<qint64>(m_timeline ? m_timeline->currentFrame() : 0);
        root[QStringLiteral("playing")] = m_playbackTimer.isActive();

        QJsonArray timeline;
        if (m_timeline) {
            for (const TimelineClip& clip : m_timeline->clips()) {
                timeline.push_back(clipToJson(clip));
            }
        }
        root[QStringLiteral("timeline")] = timeline;
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
        
        if (m_rootPathLabel) {
            m_rootPathLabel->setText(m_currentRootPath);
        }
        
        if (saveAfterChange) {
            scheduleSaveState();
        }
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
        obj[QStringLiteral("startFrame")] = static_cast<qint64>(clip.startFrame);
        obj[QStringLiteral("durationFrames")] = static_cast<qint64>(clip.durationFrames);
        obj[QStringLiteral("trackIndex")] = clip.trackIndex;
        obj[QStringLiteral("color")] = clip.color.name(QColor::HexArgb);
        return obj;
    }
    
    TimelineClip clipFromJson(const QJsonObject& obj) const {
        TimelineClip clip;
        clip.id = obj.value(QStringLiteral("id")).toString();
        clip.filePath = obj.value(QStringLiteral("filePath")).toString();
        clip.label = obj.value(QStringLiteral("label")).toString(QFileInfo(clip.filePath).fileName());
        clip.startFrame = obj.value(QStringLiteral("startFrame")).toVariant().toLongLong();
        clip.durationFrames = obj.value(QStringLiteral("durationFrames")).toVariant().toLongLong();
        clip.trackIndex = obj.value(QStringLiteral("trackIndex")).toInt(-1);
        if (clip.durationFrames == 0) clip.durationFrames = 120;
        clip.color = QColor(obj.value(QStringLiteral("color")).toString());
        if (!clip.color.isValid()) {
            clip.color = QColor::fromHsv(static_cast<int>(qHash(clip.filePath) % 360), 160, 220, 220);
        }
        return clip;
    }
    
    void loadState() {
        m_loadingState = true;
        
        QString rootPath = QDir::currentPath();
        QVector<TimelineClip> loadedClips;
        int64_t currentFrame = 0;
        bool playing = false;
        
        QFile file(stateFilePath());
        if (file.open(QIODevice::ReadOnly)) {
            const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
            const QJsonObject root = doc.object();
            
            rootPath = root.value(QStringLiteral("explorerRoot")).toString(rootPath);
            currentFrame = root.value(QStringLiteral("currentFrame")).toVariant().toLongLong();
            playing = root.value(QStringLiteral("playing")).toBool(false);
            
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
        }
        
        const QString resolvedRootPath = QDir(rootPath).absolutePath();
        m_currentRootPath = resolvedRootPath;
        if (m_rootPathLabel) {
            m_rootPathLabel->setText(resolvedRootPath);
        }

        m_timeline->setClips(loadedClips);
        syncSliderRange();
        m_preview->setClipCount(m_timeline->clips().size());
        m_preview->setTimelineClips(m_timeline->clips());
        setCurrentFrame(currentFrame);
        
        Q_UNUSED(playing)
        m_playbackTimer.stop();
        m_fastPlaybackActive.store(false);
        m_preview->setPlaybackState(false);
        
        m_loadingState = false;
        QTimer::singleShot(0, this, [this, resolvedRootPath]() {
            setExplorerRootPath(resolvedRootPath, false);
        });
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
                          "QLabel#rootPath { color: #8ea0b2; font-size: 11px; letter-spacing: 0; }"
                          "QTreeView { background: transparent; border: none; color: #dbe2ea; }"
                          "QTreeView::item { padding: 4px 0; }"
                          "QTreeView::item:selected { background: #213042; color: #f7fbff; }"));
        
        auto* layout = new QVBoxLayout(pane);
        layout->setContentsMargins(14, 14, 14, 14);
        layout->setSpacing(10);
        
        m_folderPickerButton = new QPushButton(QStringLiteral("FILES"));
        m_folderPickerButton->setObjectName(QStringLiteral("folderPicker"));
        m_folderPickerButton->setCursor(Qt::PointingHandCursor);
        layout->addWidget(m_folderPickerButton);
        
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
        m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
        m_tree->hideColumn(1);
        m_tree->hideColumn(2);
        m_tree->hideColumn(3);
        m_tree->header()->setStretchLastSection(true);
        layout->addWidget(m_tree, 1);
        
        connect(m_tree, &QTreeView::doubleClicked, this, [this](const QModelIndex& index) {
            if (!m_fsModel) return;
            const QFileInfo info = m_fsModel->fileInfo(index);
            if (info.exists() && info.isFile()) {
                addFileToTimeline(info.absoluteFilePath());
            }
        });
        connect(m_folderPickerButton, &QPushButton::clicked, this, [this]() { chooseExplorerRoot(); });
        
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
        layout->setSpacing(14);
        
        auto* previewFrame = new QFrame;
        previewFrame->setMinimumHeight(360);
        previewFrame->setFrameShape(QFrame::NoFrame);
        previewFrame->setStyleSheet(QStringLiteral("QFrame { background: #05080c; border: 1px solid #202934; border-radius: 14px; }"));
        auto* previewLayout = new QVBoxLayout(previewFrame);
        previewLayout->setContentsMargins(0, 0, 0, 0);
        previewLayout->setSpacing(0);
        
        m_preview = new PreviewWindow;
        m_preview->setObjectName(QStringLiteral("preview.window"));
        m_preview->setFocusPolicy(Qt::StrongFocus);
        m_preview->setMinimumSize(640, 360);
        
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
        layout->addWidget(previewFrame, 3);
        
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
        
        fprintf(stderr, "[DEBUG] Adding transport widgets...\n");
        fflush(stderr);
        transportLayout->addWidget(startButton);
        transportLayout->addWidget(m_playButton);
        transportLayout->addWidget(pauseButton);
        transportLayout->addWidget(endButton);
        transportLayout->addWidget(m_seekSlider, 1);
        transportLayout->addWidget(m_timecodeLabel);
        layout->addWidget(transport, 0);
        fprintf(stderr, "[DEBUG] Transport widgets added\n");
        fflush(stderr);
        
        fprintf(stderr, "[DEBUG] Creating TimelineWidget...\n");
        fflush(stderr);
        m_timeline = new TimelineWidget;
        fprintf(stderr, "[DEBUG] TimelineWidget created\n");
        fflush(stderr);
        m_timeline->setObjectName(QStringLiteral("timeline.widget"));
        layout->addWidget(m_timeline, 2);
        
        connect(m_playButton, &QPushButton::clicked, this, [this]() {
            m_playbackTimer.start();
            m_fastPlaybackActive.store(true);
            m_preview->setPlaybackState(true);
            refreshInspector();
            scheduleSaveState();
        });
        connect(pauseButton, &QPushButton::clicked, this, [this]() {
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
        
        m_timeline->seekRequested = [this](int64_t frame) {
            setCurrentFrame(frame);
        };
        m_timeline->clipsChanged = [this]() {
            syncSliderRange();
            m_preview->setClipCount(m_timeline->clips().size());
            m_preview->setTimelineClips(m_timeline->clips());
            refreshInspector();
            scheduleSaveState();
        };
        fprintf(stderr, "[DEBUG] buildEditorPane() returning\n");
        fflush(stderr);
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
    
    void setCurrentFrame(int64_t frame) {
        const int64_t bounded = qBound<int64_t>(0, frame, m_timeline->totalFrames());
        playbackTrace(QStringLiteral("EditorWindow::setCurrentFrame"),
                      QStringLiteral("requested=%1 bounded=%2").arg(frame).arg(bounded));
        if (!m_timeline || bounded != m_timeline->currentFrame()) {
            m_lastPlayheadAdvanceMs.store(nowMs());
        }
        m_fastCurrentFrame.store(bounded);
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
        
        m_statusBadge->setText(QStringLiteral("%1  |  %2 clips").arg(state).arg(clipCount));
        m_previewInfo->setText(QStringLiteral("Professional pipeline with libavcodec\nBackend: %1\nSeek: %2\nDrag files from explorer to timeline")
                                   .arg(m_preview ? m_preview->backendName() : QStringLiteral("unknown"))
                                   .arg(frameToTimecode(m_timeline ? m_timeline->currentFrame() : 0)));
        m_playButton->setText(playing ? QStringLiteral("Playing") : QStringLiteral("Play"));
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
    QPushButton* m_folderPickerButton = nullptr;
    QLabel* m_rootPathLabel = nullptr;
    PreviewWindow* m_preview = nullptr;
    TimelineWidget* m_timeline = nullptr;
    QPushButton* m_playButton = nullptr;
    QSlider* m_seekSlider = nullptr;
    QLabel* m_timecodeLabel = nullptr;
    QLabel* m_statusBadge = nullptr;
    QLabel* m_previewInfo = nullptr;
    std::unique_ptr<ControlServer> m_controlServer;
    QTimer m_playbackTimer;
    QTimer m_mainThreadHeartbeatTimer;
    QTimer m_stateSaveTimer;
    bool m_ignoreSeekSignal = false;
    bool m_loadingState = false;
    bool m_pendingSaveAfterLoad = false;
    QString m_currentRootPath;
    QByteArray m_lastSavedState;
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
