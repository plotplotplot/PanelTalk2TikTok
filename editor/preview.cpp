#include "preview.h"
#include "frame_handle.h"
#include "async_decoder.h"
#include "timeline_cache.h"
#include "playback_frame_pipeline.h"
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
#include <cmath>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_cuda.h>
}

#include <cuda.h>
#include <cudaGL.h>

using namespace editor;

namespace {
    qint64 nowMs()
    {
        return QDateTime::currentMSecsSinceEpoch();
    }

    qint64 playbackTraceMs()
    {
        static QElapsedTimer timer;
        static bool started = false;
        if (!started) {
            timer.start();
            started = true;
        }
        return timer.elapsed();
    }

    void playbackTrace(const QString& event, const QString& detail)
    {
        if (editor::debugPlaybackLevel() < editor::DebugLogLevel::Info) {
            return;
        }
        qDebug() << "[PLAYBACK]" << playbackTraceMs() << event << "-" << detail;
    }

    void playbackWarnTrace(const QString& event, const QString& detail)
    {
        if (!editor::debugPlaybackWarnEnabled()) {
            return;
        }
        qDebug() << "[PLAYBACK][WARN]" << playbackTraceMs() << event << "-" << detail;
    }

    bool frameUsesCudaZeroCopyCandidate(const FrameHandle& frame)
    {
        return frame.hasHardwareFrame() && frame.hardwareSwPixelFormat() == AV_PIX_FMT_NV12;
    }

    bool uploadCudaNv12FrameToTextures(const FrameHandle& frame, GLuint textureId, GLuint uvTextureId)
    {
        if (!textureId || !uvTextureId || !frameUsesCudaZeroCopyCandidate(frame)) {
            return false;
        }

        const AVFrame* hwFrame = frame.hardwareFrame();
        if (!hwFrame || !hwFrame->hw_frames_ctx) {
            return false;
        }

        auto* framesContext = reinterpret_cast<AVHWFramesContext*>(hwFrame->hw_frames_ctx->data);
        if (!framesContext || !framesContext->device_ctx) {
            return false;
        }
        auto* deviceContext = framesContext->device_ctx;
        if (!deviceContext || deviceContext->type != AV_HWDEVICE_TYPE_CUDA) {
            return false;
        }
        auto* cudaDeviceContext = reinterpret_cast<AVCUDADeviceContext*>(deviceContext->hwctx);
        if (!cudaDeviceContext) {
            return false;
        }

        if (cuCtxPushCurrent(cudaDeviceContext->cuda_ctx) != CUDA_SUCCESS) {
            return false;
        }

        auto restoreContext = []() {
            CUcontext popped = nullptr;
            cuCtxPopCurrent(&popped);
        };

        const int width = hwFrame->width;
        const int height = hwFrame->height;
        if (width <= 0 || height <= 0) {
            restoreContext();
            return false;
        }

        CUgraphicsResource yResource = nullptr;
        CUgraphicsResource uvResource = nullptr;
        CUresult result = cuGraphicsGLRegisterImage(&yResource, textureId, GL_TEXTURE_2D,
                                                    CU_GRAPHICS_REGISTER_FLAGS_WRITE_DISCARD);
        if (result != CUDA_SUCCESS) {
            restoreContext();
            return false;
        }
        result = cuGraphicsGLRegisterImage(&uvResource, uvTextureId, GL_TEXTURE_2D,
                                           CU_GRAPHICS_REGISTER_FLAGS_WRITE_DISCARD);
        if (result != CUDA_SUCCESS) {
            cuGraphicsUnregisterResource(yResource);
            restoreContext();
            return false;
        }

        CUgraphicsResource resources[2] = {yResource, uvResource};
        result = cuGraphicsMapResources(2, resources, cudaDeviceContext->stream);
        if (result != CUDA_SUCCESS) {
            cuGraphicsUnregisterResource(uvResource);
            cuGraphicsUnregisterResource(yResource);
            restoreContext();
            return false;
        }

        CUarray yArray = nullptr;
        CUarray uvArray = nullptr;
        result = cuGraphicsSubResourceGetMappedArray(&yArray, yResource, 0, 0);
        if (result == CUDA_SUCCESS) {
            result = cuGraphicsSubResourceGetMappedArray(&uvArray, uvResource, 0, 0);
        }

        if (result == CUDA_SUCCESS) {
            CUDA_MEMCPY2D copy = {};
            copy.srcMemoryType = CU_MEMORYTYPE_DEVICE;
            copy.dstMemoryType = CU_MEMORYTYPE_ARRAY;

            copy.srcDevice = reinterpret_cast<CUdeviceptr>(hwFrame->data[0]);
            copy.srcPitch = static_cast<size_t>(hwFrame->linesize[0]);
            copy.dstArray = yArray;
            copy.WidthInBytes = static_cast<size_t>(width);
            copy.Height = static_cast<size_t>(height);
            result = cuMemcpy2DAsync(&copy, cudaDeviceContext->stream);

            if (result == CUDA_SUCCESS) {
                CUDA_MEMCPY2D uvCopy = {};
                uvCopy.srcMemoryType = CU_MEMORYTYPE_DEVICE;
                uvCopy.dstMemoryType = CU_MEMORYTYPE_ARRAY;
                uvCopy.srcDevice = reinterpret_cast<CUdeviceptr>(hwFrame->data[1]);
                uvCopy.srcPitch = static_cast<size_t>(hwFrame->linesize[1]);
                uvCopy.dstArray = uvArray;
                uvCopy.WidthInBytes = static_cast<size_t>(width);
                uvCopy.Height = static_cast<size_t>(height / 2);
                result = cuMemcpy2DAsync(&uvCopy, cudaDeviceContext->stream);
            }
        }

        if (result == CUDA_SUCCESS) {
            result = cuStreamSynchronize(cudaDeviceContext->stream);
        }

        cuGraphicsUnmapResources(2, resources, cudaDeviceContext->stream);
        cuGraphicsUnregisterResource(uvResource);
        cuGraphicsUnregisterResource(yResource);
        restoreContext();

        if (result != CUDA_SUCCESS) {
            return false;
        }

        return true;
    }

    struct PlaybackStaleRunState {
        bool active = false;
        int64_t startRequestedFrame = -1;
        int64_t startSelectedFrame = -1;
        int64_t worstRequestedFrame = -1;
        int64_t worstSelectedFrame = -1;
        int64_t worstDelta = 0;
        qint64 startTraceMs = 0;
    };

    void playbackFrameSelectionTrace(const QString& stage,
                                     const TimelineClip& clip,
                                     int64_t requestedFrame,
                                     const FrameHandle& exactFrame,
                                     const FrameHandle& selectedFrame,
                                     qreal playheadFramePosition)
    {
        static QHash<QString, PlaybackStaleRunState> staleRunsByClip;

        const int64_t exactFrameNumber = exactFrame.isNull() ? -1 : exactFrame.frameNumber();
        const int64_t selectedFrameNumber = selectedFrame.isNull() ? -1 : selectedFrame.frameNumber();
        const int64_t delta = selectedFrame.isNull() ? -1 : selectedFrameNumber - requestedFrame;
        const bool singleFrame = clip.mediaType == ClipMediaType::Image;
        const bool anomaly =
            !singleFrame &&
            (selectedFrame.isNull() ||
             delta < 0 ||
             delta > 1);

        if (editor::debugPlaybackWarnOnlyEnabled() && !anomaly) {
            auto staleIt = staleRunsByClip.find(clip.id);
            if (staleIt != staleRunsByClip.end() && staleIt->active) {
                const PlaybackStaleRunState state = staleIt.value();
                playbackWarnTrace(QStringLiteral("PreviewWindow::stale-run.end"),
                                  QStringLiteral("clip=%1 file=%2 durationMs=%3 startRequested=%4 startSelected=%5 recoveredRequested=%6 recoveredSelected=%7 worstRequested=%8 worstSelected=%9 worstDelta=%10 playhead=%11")
                                      .arg(clip.id)
                                      .arg(QFileInfo(clip.filePath).fileName())
                                      .arg(playbackTraceMs() - state.startTraceMs)
                                      .arg(state.startRequestedFrame)
                                      .arg(state.startSelectedFrame)
                                      .arg(requestedFrame)
                                      .arg(selectedFrameNumber)
                                      .arg(state.worstRequestedFrame)
                                      .arg(state.worstSelectedFrame)
                                      .arg(state.worstDelta)
                                      .arg(playheadFramePosition, 0, 'f', 3));
                staleRunsByClip.remove(clip.id);
            }
            return;
        }
        if (!editor::debugPlaybackVerboseEnabled() && !editor::debugPlaybackWarnOnlyEnabled()) {
            return;
        }

        const QString detail =
            QStringLiteral("clip=%1 file=%2 singleFrame=%3 requested=%4 exact=%5 selected=%6 delta=%7 playhead=%8")
                .arg(clip.id)
                .arg(QFileInfo(clip.filePath).fileName())
                .arg(singleFrame ? 1 : 0)
                .arg(requestedFrame)
                .arg(exactFrameNumber)
                .arg(selectedFrameNumber)
                .arg(delta)
                .arg(playheadFramePosition, 0, 'f', 3);
        if (editor::debugPlaybackWarnOnlyEnabled()) {
            PlaybackStaleRunState& state = staleRunsByClip[clip.id];
            if (!state.active) {
                state.active = true;
                state.startRequestedFrame = requestedFrame;
                state.startSelectedFrame = selectedFrameNumber;
                state.worstRequestedFrame = requestedFrame;
                state.worstSelectedFrame = selectedFrameNumber;
                state.worstDelta = delta;
                state.startTraceMs = playbackTraceMs();
                playbackWarnTrace(QStringLiteral("PreviewWindow::stale-run.start"), detail);
                return;
            }
            if (delta < state.worstDelta) {
                state.worstRequestedFrame = requestedFrame;
                state.worstSelectedFrame = selectedFrameNumber;
                state.worstDelta = delta;
            }
            return;
        }
        playbackTrace(stage, detail);
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
// PreviewWindow Implementation
// ============================================================================

PreviewWindow::PreviewWindow(QWidget* parent)
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
    m_frameRequestTimer.setSingleShot(true);
    m_frameRequestTimer.setInterval(0);
    connect(&m_frameRequestTimer, &QTimer::timeout, this, [this]() {
        if (!m_frameRequestsArmed || !isVisible() || m_bulkUpdateDepth > 0 || !m_pendingFrameRequest) {
            return;
        }
        m_pendingFrameRequest = false;
        requestFramesForCurrentPosition();
    });
}

PreviewWindow::~PreviewWindow() {
    if (m_cache) {
        m_cache->stopPrefetching();
    }
    if (context()) {
        makeCurrent();
        releaseGlResources();
        doneCurrent();
    }
}

void PreviewWindow::setPlaybackState(bool playing) {
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
    if (m_playbackPipeline) {
        m_playbackPipeline->setPlaybackActive(playing);
    }
    if (!playing) {
        m_lastPresentedFrames.clear();
    }
}

void PreviewWindow::setCurrentFrame(int64_t frame) {
    playbackTrace(QStringLiteral("PreviewWindow::setCurrentFrame"),
                  QStringLiteral("frame=%1 visible=%2 cache=%3")
                      .arg(frame)
                      .arg(isVisible())
                      .arg(m_cache != nullptr));
    setCurrentPlaybackSample(frameToSamples(frame));
}

void PreviewWindow::setCurrentPlaybackSample(int64_t samplePosition) {
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
    if (m_playbackPipeline) {
        m_playbackPipeline->setPlayheadFrame(displayFrame);
    }
    if (m_bulkUpdateDepth > 0) {
        m_pendingFrameRequest = true;
    } else if (isVisible() && m_frameRequestsArmed) {
        m_pendingFrameRequest = true;
        scheduleFrameRequest();
    } else if (isVisible()) {
        m_pendingFrameRequest = true;
    }
    scheduleRepaint();
}

void PreviewWindow::setClipCount(int count) {
    m_clipCount = count;
    scheduleRepaint();
}

void PreviewWindow::setSelectedClipId(const QString& clipId) {
    if (m_selectedClipId == clipId) {
        return;
    }
    m_selectedClipId = clipId;
    scheduleRepaint();
}

void PreviewWindow::setTimelineClips(const QVector<TimelineClip>& clips) {
    playbackTrace(QStringLiteral("PreviewWindow::setTimelineClips"),
                  QStringLiteral("clips=%1 cache=%2").arg(clips.size()).arg(m_cache != nullptr));
    m_clips = clips;
    m_transcriptSectionsCache.clear();
    QSet<QString> visualClipIds;
    for (const auto& clip : clips) {
        if (clipHasVisuals(clip)) {
            visualClipIds.insert(clip.id);
        }
    }
    for (auto it = m_lastPresentedFrames.begin(); it != m_lastPresentedFrames.end();) {
        if (!visualClipIds.contains(it.key())) {
            it = m_lastPresentedFrames.erase(it);
        } else {
            ++it;
        }
    }
    if (m_playbackPipeline) {
        m_playbackPipeline->setTimelineClips(clips);
    }
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
            m_cache->registerClip(clip);
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
    
    if (m_bulkUpdateDepth > 0) {
        m_pendingFrameRequest = true;
    } else if (m_frameRequestsArmed) {
        m_pendingFrameRequest = true;
        scheduleFrameRequest();
    } else {
        m_pendingFrameRequest = true;
    }
    scheduleRepaint();
}

void PreviewWindow::setRenderSyncMarkers(const QVector<RenderSyncMarker>& markers) {
    m_renderSyncMarkers = markers;
    if (m_cache) {
        m_cache->setRenderSyncMarkers(markers);
    }
    if (m_playbackPipeline) {
        m_playbackPipeline->setRenderSyncMarkers(markers);
    }
    if (m_bulkUpdateDepth > 0) {
        m_pendingFrameRequest = true;
    } else if (m_frameRequestsArmed) {
        m_pendingFrameRequest = true;
        scheduleFrameRequest();
    } else {
        m_pendingFrameRequest = true;
    }
    scheduleRepaint();
}

void PreviewWindow::beginBulkUpdate() {
    ++m_bulkUpdateDepth;
}

void PreviewWindow::endBulkUpdate() {
    if (m_bulkUpdateDepth <= 0) {
        m_bulkUpdateDepth = 0;
        return;
    }
    --m_bulkUpdateDepth;
    if (m_bulkUpdateDepth == 0 && m_pendingFrameRequest) {
        if (isVisible() && m_frameRequestsArmed) {
            scheduleFrameRequest();
        }
        scheduleRepaint();
    }
}

void PreviewWindow::setExportRanges(const QVector<ExportRangeSegment>& ranges) {
    if (m_cache) {
        m_cache->setExportRanges(ranges);
    }
}

QString PreviewWindow::backendName() const {
    return usingCpuFallback() ? QStringLiteral("CPU Preview Fallback")
                              : QStringLiteral("OpenGL Shader Preview");
}

void PreviewWindow::setAudioMuted(bool muted) {
    m_audioMuted = muted;
}

void PreviewWindow::setAudioVolume(qreal volume) {
    m_audioVolume = qBound<qreal>(0.0, volume, 1.0);
}

void PreviewWindow::setOutputSize(const QSize& size) {
    const QSize sanitized(qMax(16, size.width()), qMax(16, size.height()));
    if (m_outputSize == sanitized) {
        return;
    }
    m_outputSize = sanitized;
    scheduleRepaint();
}

void PreviewWindow::setBypassGrading(bool bypass) {
    if (m_bypassGrading == bypass) {
        return;
    }
    m_bypassGrading = bypass;
    scheduleRepaint();
}

bool PreviewWindow::bypassGrading() const {
    return m_bypassGrading;
}

bool PreviewWindow::audioMuted() const {
    return m_audioMuted;
}

int PreviewWindow::audioVolumePercent() const {
    return qRound(m_audioVolume * 100.0);
}

QString PreviewWindow::activeAudioClipLabel() const {
    for (const TimelineClip& clip : m_clips) {
        if (clip.hasAudio && isSampleWithinClip(clip, m_currentSample)) {
            return clip.label;
        }
    }
    return QString();
}

bool PreviewWindow::preparePlaybackAdvance(int64_t targetFrame) {
    return preparePlaybackAdvanceSample(frameToSamples(targetFrame));
}

bool PreviewWindow::preparePlaybackAdvanceSample(int64_t targetSample) {
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
        const bool usePlaybackPipeline = false;
        const bool ready = usePlaybackPipeline
                               ? m_playbackPipeline->isFrameBuffered(clip.id, localFrame)
                               : m_cache->isFrameCached(clip.id, localFrame);
        if (ready) {
            continue;
        }

        if (!m_playing && m_cache->isVisibleRequestPending(clip.id, localFrame)) {
            continue;
        }

        if (usePlaybackPipeline) {
            m_playbackPipeline->requestFramesForSample(targetSample,
                [this]() { QMetaObject::invokeMethod(this, [this]() { scheduleRepaint(); }, Qt::QueuedConnection); });
            break;
        }

        m_cache->requestFrame(clip.id, localFrame,
                              [this](FrameHandle frame) {
                                  Q_UNUSED(frame)
                                  QMetaObject::invokeMethod(this, [this]() { scheduleRepaint(); }, Qt::QueuedConnection);
                              });
    }

    return true;
}

QJsonObject PreviewWindow::profilingSnapshot() const {
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

    if (m_playbackPipeline) {
        snapshot[QStringLiteral("playback_pipeline")] = QJsonObject{
            {QStringLiteral("active"), m_playing},
            {QStringLiteral("buffered_frames"), m_playbackPipeline->bufferedFrameCount()},
            {QStringLiteral("pending_visible_requests"), m_playbackPipeline->pendingVisibleRequestCount()},
            {QStringLiteral("dropped_presentation_frames"), m_playbackPipeline->droppedPresentationFrameCount()}
        };
    }

    if (!m_lastFrameSelectionStats.isEmpty()) {
        QJsonObject frameSelection = m_lastFrameSelectionStats;
        if (m_decoder) {
            QJsonArray enrichedClips;
            const QJsonArray clips = frameSelection.value(QStringLiteral("clips")).toArray();
            for (const QJsonValue& value : clips) {
                if (!value.isObject()) {
                    continue;
                }
                QJsonObject clipObject = value.toObject();
                const QString clipId = clipObject.value(QStringLiteral("id")).toString();
                auto clipIt = std::find_if(m_clips.begin(), m_clips.end(), [&clipId](const TimelineClip& clip) {
                    return clip.id == clipId;
                });
                if (clipIt != m_clips.end()) {
                    const QString decodePath = interactivePreviewMediaPathForClip(*clipIt);
                    if (!decodePath.isEmpty()) {
                        const VideoStreamInfo info = m_decoder->getVideoInfo(decodePath);
                        if (info.isValid) {
                            clipObject[QStringLiteral("decode_path")] = info.decodePath;
                            clipObject[QStringLiteral("interop_path")] = info.interopPath;
                            clipObject[QStringLiteral("decode_mode_requested")] = info.requestedDecodeMode;
                            clipObject[QStringLiteral("hardware_accelerated")] = info.hardwareAccelerated;
                            clipObject[QStringLiteral("codec")] = info.codecName;
                        }
                    }
                }
                enrichedClips.append(clipObject);
            }
            frameSelection[QStringLiteral("clips")] = enrichedClips;
        }
        snapshot[QStringLiteral("frame_selection")] = frameSelection;
    }

    return snapshot;
}

void PreviewWindow::paintEvent(QPaintEvent* event) {
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

void PreviewWindow::initializeGL() {
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
        uniform sampler2D u_texture_uv;
        uniform float u_texture_mode;
        uniform float u_brightness;
        uniform float u_contrast;
        uniform float u_saturation;
        uniform float u_opacity;
        varying vec2 v_texCoord;

        void main() {
            vec4 color;
            float sourceAlpha;
            vec3 rgb;
            if (u_texture_mode > 0.5) {
                float y = texture2D(u_texture, v_texCoord).r;
                vec2 uv = texture2D(u_texture_uv, v_texCoord).rg - vec2(0.5, 0.5);
                rgb = vec3(
                    y + 1.4020 * uv.y,
                    y - 0.344136 * uv.x - 0.714136 * uv.y,
                    y + 1.7720 * uv.x
                );
                rgb = clamp(rgb, 0.0, 1.0);
                sourceAlpha = 1.0;
            } else {
                color = texture2D(u_texture, v_texCoord);
                sourceAlpha = color.a;
                rgb = color.rgb;
                if (sourceAlpha > 0.0001) {
                    rgb /= sourceAlpha;
                } else {
                    rgb = vec3(0.0);
                }
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

void PreviewWindow::resizeGL(int w, int h) {
    Q_UNUSED(w)
    Q_UNUSED(h)
}

void PreviewWindow::showEvent(QShowEvent* event) {
    QOpenGLWidget::showEvent(event);
    QTimer::singleShot(0, this, [this]() {
        m_frameRequestsArmed = true;
        m_pendingFrameRequest = true;
        scheduleFrameRequest();
    });
}

void PreviewWindow::paintGL() {
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

void PreviewWindow::mousePressEvent(QMouseEvent* event) {
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

void PreviewWindow::mouseMoveEvent(QMouseEvent* event) {
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

void PreviewWindow::mouseReleaseEvent(QMouseEvent* event) {
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

void PreviewWindow::wheelEvent(QWheelEvent* event) {
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

QRect PreviewWindow::previewCanvasBaseRect() const {
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

QRect PreviewWindow::scaledCanvasRect(const QRect& baseRect) const {
    const QSize scaledSize(qMax(1, qRound(baseRect.width() * m_previewZoom)),
                           qMax(1, qRound(baseRect.height() * m_previewZoom)));
    const QPoint center = baseRect.center();
    return QRect(qRound(center.x() - scaledSize.width() / 2.0 + m_previewPanOffset.x()),
                 qRound(center.y() - scaledSize.height() / 2.0 + m_previewPanOffset.y()),
                 scaledSize.width(),
                 scaledSize.height());
}

QPointF PreviewWindow::previewCanvasScale(const QRect& targetRect) const {
    const QSize output = m_outputSize.isValid() ? m_outputSize : QSize(1080, 1920);
    return QPointF(targetRect.width() / qMax<qreal>(1.0, output.width()),
                   targetRect.height() / qMax<qreal>(1.0, output.height()));
}

bool PreviewWindow::clipShowsTranscriptOverlay(const TimelineClip& clip) const {
    return clip.mediaType == ClipMediaType::Audio && clip.transcriptOverlay.enabled;
}

const QVector<TranscriptSection>& PreviewWindow::transcriptSectionsForClip(const TimelineClip& clip) const {
    const QString key = clip.filePath;
    auto it = m_transcriptSectionsCache.find(key);
    if (it == m_transcriptSectionsCache.end()) {
        it = m_transcriptSectionsCache.insert(key, loadTranscriptSections(transcriptWorkingPathForClipFile(clip.filePath)));
    }
    return it.value();
}

TranscriptOverlayLayout PreviewWindow::transcriptOverlayLayoutForClip(const TimelineClip& clip) const {
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

QRectF PreviewWindow::transcriptOverlayRectForTarget(const TimelineClip& clip, const QRect& targetRect) const {
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

QSizeF PreviewWindow::transcriptOverlaySizeForSelectedClip() const {
    const PreviewOverlayInfo info = m_overlayInfo.value(m_selectedClipId);
    const QRect compositeRect = scaledCanvasRect(previewCanvasBaseRect());
    const QPointF previewScale = previewCanvasScale(compositeRect);
    return QSizeF(info.bounds.width() / qMax<qreal>(0.0001, previewScale.x()),
                  info.bounds.height() / qMax<qreal>(0.0001, previewScale.y()));
}

void PreviewWindow::drawTranscriptOverlay(QPainter* painter, const TimelineClip& clip, const QRect& targetRect) {
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

bool PreviewWindow::usingCpuFallback() const {
    return !context() || !isValid() || !m_shaderProgram;
}

void PreviewWindow::ensurePipeline() {
    if (m_cache) {
        return;
    }

    playbackTrace(QStringLiteral("PreviewWindow::ensurePipeline.begin"),
                  QStringLiteral("clips=%1 frame=%2").arg(m_clips.size()).arg(m_currentFramePosition, 0, 'f', 3));

    m_decoder = std::make_unique<AsyncDecoder>(this);
    m_decoder->initialize();
    if (MemoryBudget* budget = m_decoder->memoryBudget()) {
        budget->setMaxCpuMemory(384 * 1024 * 1024);
    }

    m_cache = std::make_unique<TimelineCache>(m_decoder.get(),
                                              m_decoder->memoryBudget(),
                                              this);
    m_playbackPipeline = std::make_unique<PlaybackFramePipeline>(m_decoder.get(), this);
    m_cache->setMaxMemory(384 * 1024 * 1024);
    m_cache->setLookaheadFrames(18);
    m_cache->setPlaybackSpeed(1.0);
    m_cache->setPlaybackState(m_playing ?
        TimelineCache::PlaybackState::Playing :
        TimelineCache::PlaybackState::Stopped);
    m_cache->setPlayheadFrame(m_currentFrame);
    m_playbackPipeline->setPlaybackActive(m_playing);
    m_playbackPipeline->setPlayheadFrame(m_currentFrame);
    m_playbackPipeline->setTimelineClips(m_clips);
    m_playbackPipeline->setRenderSyncMarkers(m_renderSyncMarkers);
    m_registeredClips.clear();
    for (const TimelineClip& clip : m_clips) {
        if (!clipHasVisuals(clip)) {
            continue;
        }
        m_cache->registerClip(clip);
        m_registeredClips.insert(clip.id);
    }
    m_cache->setRenderSyncMarkers(m_renderSyncMarkers);
    m_cache->startPrefetching();
    playbackTrace(QStringLiteral("PreviewWindow::ensurePipeline.end"),
                  QStringLiteral("workers=%1").arg(m_decoder ? m_decoder->workerCount() : 0));
}

void PreviewWindow::releaseGlResources() {
    for (auto it = m_textureCache.begin(); it != m_textureCache.end(); ++it) {
        if (it.value().textureId != 0) {
            glDeleteTextures(1, &it.value().textureId);
            it.value().textureId = 0;
        }
        if (it.value().auxTextureId != 0) {
            glDeleteTextures(1, &it.value().auxTextureId);
            it.value().auxTextureId = 0;
        }
    }
    m_textureCache.clear();
    if (m_quadBuffer.isCreated()) {
        m_quadBuffer.destroy();
    }
    m_shaderProgram.reset();
}

QString PreviewWindow::textureCacheKey(const FrameHandle& frame) const {
    return QStringLiteral("%1|%2").arg(frame.sourcePath()).arg(frame.frameNumber());
}

bool PreviewWindow::uploadCudaNv12FrameToTextures(const FrameHandle& frame, TextureCacheEntry& entry) {
    if (!frameUsesCudaZeroCopyCandidate(frame)) {
        return false;
    }

    const QSize frameSize = frame.size();
    if (!frameSize.isValid()) {
        return false;
    }

    const int width = frameSize.width();
    const int height = frameSize.height();
    const int uvWidth = qMax(1, (width + 1) / 2);
    const int uvHeight = qMax(1, (height + 1) / 2);
    const bool needsAllocation =
        entry.textureId == 0 ||
        entry.auxTextureId == 0 ||
        entry.size != frameSize ||
        !entry.usesYuvTextures;

    if (needsAllocation) {
        if (entry.textureId != 0) {
            glDeleteTextures(1, &entry.textureId);
            entry.textureId = 0;
        }
        if (entry.auxTextureId != 0) {
            glDeleteTextures(1, &entry.auxTextureId);
            entry.auxTextureId = 0;
        }

        glGenTextures(1, &entry.textureId);
        glBindTexture(GL_TEXTURE_2D, entry.textureId);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, width, height, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);

        glGenTextures(1, &entry.auxTextureId);
        glBindTexture(GL_TEXTURE_2D, entry.auxTextureId);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, uvWidth, uvHeight, 0, GL_RG, GL_UNSIGNED_BYTE, nullptr);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    if (!::uploadCudaNv12FrameToTextures(frame, entry.textureId, entry.auxTextureId)) {
        return false;
    }

    entry.size = frameSize;
    entry.usesYuvTextures = true;
    return true;
}

GLuint PreviewWindow::textureForFrame(const FrameHandle& frame) {
    if (frame.isNull()) {
        return 0;
    }

    const QString key = textureCacheKey(frame);
    const qint64 decodeTimestamp = frame.data() ? frame.data()->decodeTimestamp : 0;
    TextureCacheEntry entry = m_textureCache.value(key);
    if (entry.textureId != 0 && entry.decodeTimestamp == decodeTimestamp) {
        entry.lastUsedMs = nowMs();
        m_textureCache.insert(key, entry);
        return entry.textureId;
    }

    if (entry.textureId != 0) {
        glDeleteTextures(1, &entry.textureId);
        entry.textureId = 0;
    }
    if (entry.auxTextureId != 0) {
        glDeleteTextures(1, &entry.auxTextureId);
        entry.auxTextureId = 0;
    }
    entry.usesYuvTextures = false;

    if (frameUsesCudaZeroCopyCandidate(frame) && uploadCudaNv12FrameToTextures(frame, entry)) {
        entry.decodeTimestamp = decodeTimestamp;
        entry.lastUsedMs = nowMs();
        m_textureCache.insert(key, entry);
        trimTextureCache();
        return entry.textureId;
    }

    if (!frame.hasCpuImage()) {
        return 0;
    }

    QImage uploadImage = frame.cpuImage();
    if (uploadImage.format() != QImage::Format_ARGB32_Premultiplied) {
        uploadImage = uploadImage.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    }

    GLuint textureId = 0;
    glGenTextures(1, &textureId);
    if (textureId == 0) {
        return 0;
    }
    glBindTexture(GL_TEXTURE_2D, textureId);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D,
                 0,
                 GL_RGBA8,
                 uploadImage.width(),
                 uploadImage.height(),
                 0,
                 GL_BGRA,
                 GL_UNSIGNED_BYTE,
                 uploadImage.constBits());
    glBindTexture(GL_TEXTURE_2D, 0);

    entry.textureId = textureId;
    entry.auxTextureId = 0;
    entry.decodeTimestamp = decodeTimestamp;
    entry.lastUsedMs = nowMs();
    entry.size = uploadImage.size();
    entry.usesYuvTextures = false;
    m_textureCache.insert(key, entry);
    trimTextureCache();
    return textureId;
}

void PreviewWindow::trimTextureCache() {
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
        if (entry.textureId != 0) {
            glDeleteTextures(1, &entry.textureId);
        }
        if (entry.auxTextureId != 0) {
            glDeleteTextures(1, &entry.auxTextureId);
        }
    }
}

bool PreviewWindow::isSampleWithinClip(const TimelineClip& clip, int64_t samplePosition) const {
    const int64_t clipStartSample = clipTimelineStartSamples(clip);
    const int64_t clipEndSample = clipStartSample + frameToSamples(clip.durationFrames);
    return samplePosition >= clipStartSample && samplePosition < clipEndSample;
}

int64_t PreviewWindow::sourceSampleForPlaybackSample(const TimelineClip& clip, int64_t samplePosition) const {
    return qMax<int64_t>(0, clipSourceInSamples(clip) + (samplePosition - clipTimelineStartSamples(clip)));
}

int64_t PreviewWindow::sourceFrameForSample(const TimelineClip& clip, int64_t samplePosition) const {
    return sourceFrameForClipAtTimelinePosition(
        clip,
        samplesToFramePosition(samplePosition),
        m_renderSyncMarkers);
}

QRectF PreviewWindow::renderFrameLayerGL(const QRect& targetRect, const TimelineClip& clip, const FrameHandle& frame) {
    if (!m_shaderProgram) {
        return QRectF();
    }

    const QString cacheKey = textureCacheKey(frame);
    const GLuint textureId = textureForFrame(frame);
    if (textureId == 0) {
        return QRectF();
    }
    const TextureCacheEntry entry = m_textureCache.value(cacheKey);

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

    const TimelineClip::GradingKeyframe grade =
        m_bypassGrading ? TimelineClip::GradingKeyframe{} : evaluateClipGradingAtPosition(clip, m_currentFramePosition);
    const qreal brightness = grade.brightness;
    const qreal contrast = grade.contrast;
    const qreal saturation = grade.saturation;
    const qreal opacity = grade.opacity;

    m_shaderProgram->bind();
    m_shaderProgram->setUniformValue("u_mvp", projection * model);
    m_shaderProgram->setUniformValue("u_brightness", GLfloat(brightness));
    m_shaderProgram->setUniformValue("u_contrast", GLfloat(contrast));
    m_shaderProgram->setUniformValue("u_saturation", GLfloat(saturation));
    m_shaderProgram->setUniformValue("u_opacity", GLfloat(opacity));
    m_shaderProgram->setUniformValue("u_texture", 0);
    m_shaderProgram->setUniformValue("u_texture_uv", 1);
    m_shaderProgram->setUniformValue("u_texture_mode", entry.usesYuvTextures ? 1.0f : 0.0f);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, textureId);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, entry.usesYuvTextures ? entry.auxTextureId : 0);
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
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    m_quadBuffer.release();
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

void PreviewWindow::renderCompositedPreviewGL(const QRect& compositeRect,
                               const QList<TimelineClip>& activeClips,
                               bool& drewAnyFrame,
                               bool& waitingForFrame) {
    m_overlayInfo.clear();
    m_paintOrder.clear();
    int usedPlaybackPipelineCount = 0;
    int presentationCount = 0;
    int exactCount = 0;
    int bestCount = 0;
    int heldCount = 0;
    int nullCount = 0;
    int skippedZeroOpacityCount = 0;
    QJsonArray clipSelections;
    for (const TimelineClip& clip : activeClips) {
        if (!clipHasVisuals(clip)) {
            continue;
        }
        if (!m_bypassGrading) {
            const TimelineClip::GradingKeyframe grade =
                evaluateClipGradingAtPosition(clip, m_currentFramePosition);
            if (grade.opacity <= 0.0001) {
                ++skippedZeroOpacityCount;
                clipSelections.append(QJsonObject{
                    {QStringLiteral("id"), clip.id},
                    {QStringLiteral("label"), clip.label},
                    {QStringLiteral("media_type"), clipMediaTypeLabel(clip.mediaType)},
                    {QStringLiteral("source_kind"), mediaSourceKindLabel(clip.sourceKind)},
                    {QStringLiteral("playback_pipeline"), false},
                    {QStringLiteral("local_frame"), static_cast<qint64>(sourceFrameForSample(clip, m_currentSample))},
                    {QStringLiteral("selection"), QStringLiteral("skipped_zero_opacity")},
                    {QStringLiteral("frame_storage"), QStringLiteral("none")}
                });
                continue;
            }
        }
        const int64_t localFrame = sourceFrameForSample(clip, m_currentSample);
        const bool usePlaybackPipeline = false;
        QString selection = QStringLiteral("none");
        const FrameHandle exactFrame = usePlaybackPipeline
                                           ? m_playbackPipeline->getFrame(clip.id, localFrame)
                                           : (m_cache ? m_cache->getCachedFrame(clip.id, localFrame) : FrameHandle());
        FrameHandle frame;
        if (usePlaybackPipeline) {
            ++usedPlaybackPipelineCount;
            frame = m_playbackPipeline->getPresentationFrame(clip.id, localFrame);
            if (!frame.isNull()) {
                ++presentationCount;
                selection = QStringLiteral("presentation");
            }
        } else {
            frame = exactFrame.isNull() && m_cache
                        ? (m_playing
                               ? m_cache->getLatestCachedFrame(clip.id, localFrame)
                               : m_cache->getBestCachedFrame(clip.id, localFrame))
                        : exactFrame;
            if (!frame.isNull()) {
                if (!exactFrame.isNull() && frame == exactFrame) {
                    ++exactCount;
                    selection = QStringLiteral("exact");
                } else {
                    ++bestCount;
                    selection = m_playing ? QStringLiteral("latest") : QStringLiteral("best");
                }
            }
        }
        if (usePlaybackPipeline && frame.isNull()) {
            frame = !exactFrame.isNull() ? exactFrame
                                         : m_playbackPipeline->getBestFrame(clip.id, localFrame);
            if (!frame.isNull()) {
                if (!exactFrame.isNull() && frame == exactFrame) {
                    ++exactCount;
                    selection = QStringLiteral("exact");
                } else {
                    ++bestCount;
                    selection = QStringLiteral("best");
                }
            }
        }
        if (usePlaybackPipeline) {
            if (!frame.isNull()) {
                m_lastPresentedFrames.insert(clip.id, frame);
            } else {
                frame = m_lastPresentedFrames.value(clip.id);
                if (!frame.isNull()) {
                    ++heldCount;
                    selection = QStringLiteral("held");
                }
            }
        }
        playbackFrameSelectionTrace(QStringLiteral("PreviewWindow::renderCompositedPreviewGL.select"),
                                    clip,
                                    localFrame,
                                    exactFrame,
                                    frame,
                                    m_currentFramePosition);
        if (frame.isNull()) {
            ++nullCount;
            selection = QStringLiteral("null");
            waitingForFrame = true;
            clipSelections.append(QJsonObject{
                {QStringLiteral("id"), clip.id},
                {QStringLiteral("label"), clip.label},
                {QStringLiteral("media_type"), clipMediaTypeLabel(clip.mediaType)},
                {QStringLiteral("source_kind"), mediaSourceKindLabel(clip.sourceKind)},
                {QStringLiteral("playback_pipeline"), usePlaybackPipeline},
                {QStringLiteral("local_frame"), static_cast<qint64>(localFrame)},
                {QStringLiteral("selection"), selection},
                {QStringLiteral("frame_storage"), QStringLiteral("none")}
            });
            continue;
        }
        clipSelections.append(QJsonObject{
            {QStringLiteral("id"), clip.id},
            {QStringLiteral("label"), clip.label},
            {QStringLiteral("media_type"), clipMediaTypeLabel(clip.mediaType)},
            {QStringLiteral("source_kind"), mediaSourceKindLabel(clip.sourceKind)},
            {QStringLiteral("playback_pipeline"), usePlaybackPipeline},
            {QStringLiteral("local_frame"), static_cast<qint64>(localFrame)},
            {QStringLiteral("frame_number"), static_cast<qint64>(frame.frameNumber())},
            {QStringLiteral("selection"), selection},
            {QStringLiteral("frame_storage"),
             frame.hasHardwareFrame() ? QStringLiteral("hardware")
                                      : (frame.hasCpuImage() ? QStringLiteral("cpu")
                                                             : QStringLiteral("unknown"))}
        });

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
    m_lastFrameSelectionStats = QJsonObject{
        {QStringLiteral("path"), QStringLiteral("gl")},
        {QStringLiteral("active_visual_clips"), activeClips.size()},
        {QStringLiteral("use_playback_pipeline_clips"), usedPlaybackPipelineCount},
        {QStringLiteral("presentation"), presentationCount},
        {QStringLiteral("exact"), exactCount},
        {QStringLiteral("best"), bestCount},
        {QStringLiteral("held"), heldCount},
        {QStringLiteral("null"), nullCount},
        {QStringLiteral("skipped_zero_opacity"), skippedZeroOpacityCount},
        {QStringLiteral("clips"), clipSelections}
    };
}

void PreviewWindow::drawCompositedPreviewOverlay(QPainter* painter,
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

void PreviewWindow::drawBackground(QPainter* painter) {
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

QList<TimelineClip> PreviewWindow::getActiveClips() const {
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

void PreviewWindow::requestFramesForCurrentPosition() {
    static constexpr int kMaxVisibleBacklog = 4;
    QVector<const TimelineClip*> activeClips;
    activeClips.reserve(m_clips.size());
    for (const TimelineClip& clip : m_clips) {
        if (!clipHasVisuals(clip)) {
            continue;
        }
        if (isSampleWithinClip(clip, m_currentSample)) {
            if (!m_bypassGrading) {
                const TimelineClip::GradingKeyframe grade =
                    evaluateClipGradingAtPosition(clip, m_currentFramePosition);
                if (grade.opacity <= 0.0001) {
                    playbackTrace(QStringLiteral("PreviewWindow::requestFramesForCurrentPosition.skip"),
                                  QStringLiteral("reason=zero-opacity clip=%1 frame=%2")
                                      .arg(clip.id)
                                      .arg(m_currentFramePosition, 0, 'f', 3));
                    continue;
                }
            }
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

    if (!m_playing && m_cache->pendingVisibleRequestCount() >= kMaxVisibleBacklog) {
        playbackTrace(QStringLiteral("PreviewWindow::requestFramesForCurrentPosition.skip"),
                      QStringLiteral("reason=visible-backlog count=%1")
                          .arg(m_cache->pendingVisibleRequestCount()));
        return;
    }

    for (const TimelineClip* clip : activeClips) {
        const int64_t localFrame = sourceFrameForSample(*clip, m_currentSample);
        const bool usePlaybackPipeline = false;
        const bool usePlaybackBuffer =
            m_playing &&
            !usePlaybackPipeline &&
            m_cache;
        const bool cached = usePlaybackPipeline
                                ? m_playbackPipeline->isFrameBuffered(clip->id, localFrame)
                                : (usePlaybackBuffer
                                       ? (m_cache->isPlaybackFrameBuffered(clip->id, localFrame) ||
                                          m_cache->isFrameCached(clip->id, localFrame))
                                       : m_cache->isFrameCached(clip->id, localFrame));
        playbackTrace(QStringLiteral("PreviewWindow::visible-request"),
                      QStringLiteral("clip=%1 localFrame=%2 cached=%3")
                          .arg(clip->id)
                          .arg(localFrame)
                          .arg(cached));

        // Request if not cached
        if (!cached && !usePlaybackPipeline && !m_cache->isVisibleRequestPending(clip->id, localFrame)) {
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

    if (m_playing && m_playbackPipeline) {
        m_lastFrameRequestMs = nowMs();
        m_playbackPipeline->requestFramesForSample(
            m_currentSample,
            [this]() {
                m_lastFrameReadyMs = nowMs();
                QMetaObject::invokeMethod(this, [this]() { scheduleRepaint(); }, Qt::QueuedConnection);
            });
    }
}

void PreviewWindow::scheduleRepaint() {
    m_lastRepaintScheduleMs = nowMs();
    if (!m_repaintTimer.isActive()) {
        m_repaintTimer.start();
    }
}

void PreviewWindow::scheduleFrameRequest() {
    if (!m_frameRequestsArmed || !isVisible() || m_bulkUpdateDepth > 0) {
        return;
    }
    if (!m_frameRequestTimer.isActive()) {
        m_frameRequestTimer.start();
    }
}

void PreviewWindow::drawCompositedPreview(QPainter* painter, const QRect& safeRect,
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
    int usedPlaybackPipelineCount = 0;
    int presentationCount = 0;
    int exactCount = 0;
    int bestCount = 0;
    int heldCount = 0;
    int nullCount = 0;
    QJsonArray clipSelections;

    for (const TimelineClip& clip : activeClips) {
        const int64_t localFrame = sourceFrameForSample(clip, m_currentSample);
        const bool usePlaybackPipeline = false;
        const bool usePlaybackBuffer =
            m_playing &&
            !usePlaybackPipeline &&
            m_cache;
        QString selection = QStringLiteral("none");
        const FrameHandle exactFrame = usePlaybackPipeline
                                           ? m_playbackPipeline->getFrame(clip.id, localFrame)
                                           : (usePlaybackBuffer
                                                  ? m_cache->getPlaybackFrame(clip.id, localFrame)
                                                  : (m_cache ? m_cache->getCachedFrame(clip.id, localFrame) : FrameHandle()));
        FrameHandle frame;
        if (usePlaybackPipeline) {
            ++usedPlaybackPipelineCount;
            frame = m_playbackPipeline->getPresentationFrame(clip.id, localFrame);
            if (!frame.isNull()) {
                ++presentationCount;
                selection = QStringLiteral("presentation");
            }
        } else {
            frame = exactFrame.isNull() && m_cache
                        ? (usePlaybackBuffer
                               ? m_cache->getLatestPlaybackFrame(clip.id, localFrame)
                               : (m_playing
                                      ? m_cache->getLatestCachedFrame(clip.id, localFrame)
                                      : m_cache->getBestCachedFrame(clip.id, localFrame)))
                        : exactFrame;
            if (!frame.isNull()) {
                if (!exactFrame.isNull() && frame == exactFrame) {
                    ++exactCount;
                    selection = QStringLiteral("exact");
                } else {
                    ++bestCount;
                    selection = m_playing ? QStringLiteral("latest") : QStringLiteral("best");
                }
            }
        }
        if (usePlaybackPipeline && frame.isNull()) {
            frame = !exactFrame.isNull() ? exactFrame
                                         : m_playbackPipeline->getBestFrame(clip.id, localFrame);
            if (!frame.isNull()) {
                if (!exactFrame.isNull() && frame == exactFrame) {
                    ++exactCount;
                    selection = QStringLiteral("exact");
                } else {
                    ++bestCount;
                    selection = QStringLiteral("best");
                }
            }
        }
        if (usePlaybackPipeline) {
            if (!frame.isNull()) {
                m_lastPresentedFrames.insert(clip.id, frame);
            } else {
                frame = m_lastPresentedFrames.value(clip.id);
                if (!frame.isNull()) {
                    ++heldCount;
                    selection = QStringLiteral("held");
                }
            }
        }
        playbackFrameSelectionTrace(QStringLiteral("PreviewWindow::drawCompositedPreview.select"),
                                    clip,
                                    localFrame,
                                    exactFrame,
                                    frame,
                                    m_currentFramePosition);
        if (frame.isNull()) {
            ++nullCount;
            selection = QStringLiteral("null");
            waitingForFrame = true;
            clipSelections.append(QJsonObject{
                {QStringLiteral("id"), clip.id},
                {QStringLiteral("label"), clip.label},
                {QStringLiteral("media_type"), clipMediaTypeLabel(clip.mediaType)},
                {QStringLiteral("source_kind"), mediaSourceKindLabel(clip.sourceKind)},
                {QStringLiteral("playback_pipeline"), usePlaybackPipeline},
                {QStringLiteral("local_frame"), static_cast<qint64>(localFrame)},
                {QStringLiteral("selection"), selection}
            });
            continue;
        }
        clipSelections.append(QJsonObject{
            {QStringLiteral("id"), clip.id},
            {QStringLiteral("label"), clip.label},
            {QStringLiteral("media_type"), clipMediaTypeLabel(clip.mediaType)},
            {QStringLiteral("source_kind"), mediaSourceKindLabel(clip.sourceKind)},
            {QStringLiteral("playback_pipeline"), usePlaybackPipeline},
            {QStringLiteral("local_frame"), static_cast<qint64>(localFrame)},
            {QStringLiteral("frame_number"), static_cast<qint64>(frame.frameNumber())},
            {QStringLiteral("selection"), selection}
        });
        drawFrameLayer(painter, compositeRect, clip, frame);
        drewAnyFrame = true;
    }

    m_lastFrameSelectionStats = QJsonObject{
        {QStringLiteral("path"), QStringLiteral("cpu")},
        {QStringLiteral("active_visual_clips"), activeClips.size()},
        {QStringLiteral("use_playback_pipeline_clips"), usedPlaybackPipelineCount},
        {QStringLiteral("presentation"), presentationCount},
        {QStringLiteral("exact"), exactCount},
        {QStringLiteral("best"), bestCount},
        {QStringLiteral("held"), heldCount},
        {QStringLiteral("null"), nullCount},
        {QStringLiteral("clips"), clipSelections}
    };

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

void PreviewWindow::drawEmptyState(QPainter* painter, const QRect& safeRect) {
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

void PreviewWindow::drawFrameLayer(QPainter* painter, const QRect& targetRect,
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

void PreviewWindow::drawFramePlaceholder(QPainter* painter, const QRect& targetRect,
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

void PreviewWindow::drawAudioPlaceholder(QPainter* painter, const QRect& safeRect,
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

void PreviewWindow::drawAudioBadge(QPainter* painter, const QRect& targetRect,
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

QRect PreviewWindow::fitRect(const QSize& source, const QRect& bounds) const {
    if (source.isEmpty() || bounds.isEmpty()) {
        return bounds;
    }
    
    QSize scaled = source;
    scaled.scale(bounds.size(), Qt::KeepAspectRatio);
    const QPoint topLeft(bounds.center().x() - scaled.width() / 2,
                         bounds.center().y() - scaled.height() / 2);
    return QRect(topLeft, scaled);
}

void PreviewWindow::drawPreviewChrome(QPainter* painter, const QRect& safeRect, int activeClipCount) const {
    Q_UNUSED(painter)
    Q_UNUSED(safeRect)
    Q_UNUSED(activeClipCount)
}

QString PreviewWindow::clipIdAtPosition(const QPointF& position) const {
    for (int i = m_paintOrder.size() - 1; i >= 0; --i) {
        const QString& clipId = m_paintOrder[i];
        if (m_overlayInfo.value(clipId).bounds.contains(position)) {
            return clipId;
        }
    }
    return QString();
}

TimelineClip::TransformKeyframe PreviewWindow::evaluateTransformForSelectedClip() const {
    for (const TimelineClip& clip : m_clips) {
        if (clip.id == m_selectedClipId) {
            return evaluateClipTransformAtPosition(clip, m_currentFramePosition);
        }
    }
    return TimelineClip::TransformKeyframe();
}

void PreviewWindow::updatePreviewCursor(const QPointF& position) {
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
