#include <QApplication>
#include <QColor>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDragEnterEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QList>
#include <QMainWindow>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QOffscreenSurface>
#include <QPainter>
#include <QPalette>
#include <QProcess>
#include <QPointer>
#include <QPushButton>
#include <QRandomGenerator>
#include <QResizeEvent>
#include <QSaveFile>
#include <QSlider>
#include <QSplitter>
#include <QStackedLayout>
#include <QStyle>
#include <QStyleOption>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>
#include <QMessageBox>
#include <QSurfaceFormat>
#include <QWheelEvent>
#include <QtGlobal>

// QRhi is exposed through Qt's private QtGui headers on this Ubuntu system.
#include <QtGui/private/qrhi_p.h>
#include <QtGui/private/qrhigles2_p.h>

#include <algorithm>
#include <functional>
#include <memory>
#include <cmath>
#include <deque>
#include <mutex>
#include <thread>

struct TimelineClip
{
    QString filePath;
    QString label;
    int startFrame = 0;
    int durationFrames = 90;
    QColor color;
};

class MediaFrameProvider
{
public:
    MediaFrameProvider()
        : m_state(std::make_shared<State>())
    {
        m_worker = std::thread([state = m_state, this]() { workerLoop(state); });
    }

    ~MediaFrameProvider()
    {
        {
            std::lock_guard<std::mutex> lock(m_state->mutex);
            m_state->stop = true;
        }
        m_state->condition.notify_all();
        if (m_worker.joinable()) {
            m_worker.join();
        }
    }

    void setFrameReadyCallback(std::function<void()> callback)
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        m_state->frameReadyCallback = std::move(callback);
    }

    QImage frameForClip(const TimelineClip &clip, int timelineFrame)
    {
        const QFileInfo info(clip.filePath);
        if (!info.exists() || !info.isFile()) {
            return {};
        }

        const QString suffix = info.suffix().toLower();
        if (isImageSuffix(suffix)) {
            return loadImageClip(info.absoluteFilePath());
        }
        if (isVideoSuffix(suffix)) {
            const int localFrame = qMax(0, timelineFrame - clip.startFrame);
            return loadVideoFrame(info.absoluteFilePath(), localFrame);
        }
        return {};
    }

    bool isImageFile(const QString &path) const
    {
        return isImageSuffix(QFileInfo(path).suffix().toLower());
    }

    bool isVideoFile(const QString &path) const
    {
        return isVideoSuffix(QFileInfo(path).suffix().toLower());
    }

    void preloadFrames(const QVector<TimelineClip> &clips, int timelineFrame, bool playing)
    {
        for (const TimelineClip &clip : clips) {
            if (!isVideoFile(clip.filePath)) {
                continue;
            }
            if (timelineFrame < clip.startFrame || timelineFrame >= clip.startFrame + clip.durationFrames) {
                continue;
            }

            const int localFrame = qMax(0, timelineFrame - clip.startFrame);
            queueVideoFrame(clip.filePath, localFrame);
            if (playing) {
                queueVideoFrame(clip.filePath, localFrame + 1);
                queueVideoFrame(clip.filePath, localFrame + 2);
                queueVideoFrame(clip.filePath, localFrame + 3);
            }
        }
    }

private:
    static constexpr int kPreviewFps = 30;
    static constexpr int kFrameQuantization = 2;
    static constexpr int kMaxCachedFrames = 96;
    static constexpr int kMaxQueuedFrames = 12;

    struct State
    {
        std::mutex mutex;
        std::condition_variable condition;
        bool stop = false;
        QHash<QString, QImage> imageCache;
        QHash<QString, QImage> videoFrameCache;
        QHash<QString, QImage> lastFrameByPath;
        QSet<QString> pendingKeys;
        std::deque<QString> decodeQueue;
        QHash<QString, QString> queuedPathByKey;
        QHash<QString, int> queuedFrameByKey;
        std::function<void()> frameReadyCallback;
    };

    bool isImageSuffix(const QString &suffix) const
    {
        static const QSet<QString> kImageSuffixes = {
            QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg"),
            QStringLiteral("webp"), QStringLiteral("bmp"), QStringLiteral("gif")
        };
        return kImageSuffixes.contains(suffix);
    }

    bool isVideoSuffix(const QString &suffix) const
    {
        static const QSet<QString> kVideoSuffixes = {
            QStringLiteral("mp4"), QStringLiteral("mov"), QStringLiteral("mkv"),
            QStringLiteral("webm"), QStringLiteral("avi"), QStringLiteral("m4v")
        };
        return kVideoSuffixes.contains(suffix);
    }

    QImage loadImageClip(const QString &path)
    {
        {
            std::lock_guard<std::mutex> lock(m_state->mutex);
            auto it = m_state->imageCache.find(path);
            if (it != m_state->imageCache.end()) {
                return it.value();
            }
        }

        QImageReader reader(path);
        reader.setAutoTransform(true);
        const QImage image = reader.read();
        if (!image.isNull()) {
            std::lock_guard<std::mutex> lock(m_state->mutex);
            m_state->imageCache.insert(path, image);
        }
        return image;
    }

    QImage loadVideoFrame(const QString &path, int localFrame)
    {
        const QString cacheKey = frameCacheKey(path, localFrame);
        {
            std::lock_guard<std::mutex> lock(m_state->mutex);
            auto hit = m_state->videoFrameCache.find(cacheKey);
            if (hit != m_state->videoFrameCache.end()) {
                return hit.value();
            }
            auto last = m_state->lastFrameByPath.find(path);
            if (last != m_state->lastFrameByPath.end()) {
                queueVideoFrame(path, localFrame);
                return last.value();
            }
        }

        queueVideoFrame(path, localFrame);
        return {};
    }

    QString frameCacheKey(const QString &path, int localFrame) const
    {
        const int quantizedFrame = qMax(0, (localFrame / kFrameQuantization) * kFrameQuantization);
        return QStringLiteral("%1|%2").arg(path).arg(quantizedFrame);
    }

    QString cacheFilePathForKey(const QString &cacheKey) const
    {
        const QString cacheDir = QDir(QApplication::applicationDirPath()).filePath(QStringLiteral("frame_cache"));
        if (!QDir().mkpath(cacheDir)) {
            qWarning() << "Failed to create cache directory:" << cacheDir;
        }
        const QByteArray hash = QCryptographicHash::hash(cacheKey.toUtf8(), QCryptographicHash::Sha1).toHex();
        return QDir(cacheDir).filePath(QString::fromLatin1(hash) + QStringLiteral(".png"));
    }

    void queueVideoFrame(const QString &path, int localFrame)
    {
        const QString cacheKey = frameCacheKey(path, localFrame);
        {
            std::lock_guard<std::mutex> lock(m_state->mutex);
            if (m_state->videoFrameCache.contains(cacheKey) || m_state->pendingKeys.contains(cacheKey)) {
                return;
            }
            if (static_cast<int>(m_state->decodeQueue.size()) >= kMaxQueuedFrames) {
                return;
            }
            m_state->pendingKeys.insert(cacheKey);
            m_state->queuedPathByKey.insert(cacheKey, path);
            m_state->queuedFrameByKey.insert(cacheKey, qMax(0, localFrame));
            m_state->decodeQueue.push_back(cacheKey);
        }
        m_state->condition.notify_one();
    }

    QImage extractVideoFrame(const QString &path, int localFrame)
    {
        // Validate FFmpeg is available before attempting decode
        static const bool ffmpegAvailable = []() {
            QProcess check;
            check.start(QStringLiteral("ffmpeg"), {QStringLiteral("-version")});
            if (!check.waitForFinished(2000)) {
                return false;
            }
            return check.exitStatus() == QProcess::NormalExit && check.exitCode() == 0;
        }();
        if (!ffmpegAvailable) {
            qWarning() << "FFmpeg not available for frame extraction";
            return {};
        }

        const QString cacheKey = frameCacheKey(path, localFrame);
        const QString cachePath = cacheFilePathForKey(cacheKey);
        
        // Generate frame if not cached
        if (!QFileInfo::exists(cachePath)) {
            const int quantizedFrame = qMax(0, (localFrame / kFrameQuantization) * kFrameQuantization);
            const double seconds = static_cast<double>(quantizedFrame) / static_cast<double>(kPreviewFps);
            
            QProcess process;
            process.setProgram(QStringLiteral("ffmpeg"));
            process.setArguments({
                QStringLiteral("-y"),
                QStringLiteral("-loglevel"), QStringLiteral("error"),
                QStringLiteral("-ss"), QString::number(seconds, 'f', 3),
                QStringLiteral("-i"), path,
                QStringLiteral("-frames:v"), QStringLiteral("1"),
                QStringLiteral("-vf"), QStringLiteral("scale='min(960,iw)':-2"),
                QStringLiteral("-pix_fmt"), QStringLiteral("rgb24"),
                cachePath
            });
            
            process.start();
            if (!process.waitForFinished(8000) || process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
                qWarning() << "FFmpeg failed to extract frame from" << path << "at frame" << localFrame
                           << "exit code:" << process.exitCode();
                return {};
            }
        }

        QImageReader reader(cachePath);
        reader.setAutoTransform(true);
        const QImage image = reader.read();
        if (image.isNull()) {
            qWarning() << "Failed to read cached frame from" << cachePath;
        }
        return image;
    }

    void workerLoop(const std::shared_ptr<State> &state)
    {
        while (true) {
            QString cacheKey;
            QString path;
            int localFrame = 0;
            {
                std::unique_lock<std::mutex> lock(state->mutex);
                state->condition.wait(lock, [&]() { return state->stop || !state->decodeQueue.empty(); });
                if (state->stop) {
                    return;
                }
                cacheKey = state->decodeQueue.front();
                state->decodeQueue.pop_front();
                path = state->queuedPathByKey.take(cacheKey);
                localFrame = state->queuedFrameByKey.take(cacheKey);
            }

            const QImage image = extractVideoFrame(path, localFrame);
            std::function<void()> callback;
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->pendingKeys.remove(cacheKey);
                if (!image.isNull()) {
                    state->videoFrameCache.insert(cacheKey, image);
                    state->lastFrameByPath.insert(path, image);
                    while (state->videoFrameCache.size() > kMaxCachedFrames) {
                        state->videoFrameCache.erase(state->videoFrameCache.begin());
                    }
                }
                callback = state->frameReadyCallback;
            }

            if (callback) {
                callback();
            }
        }
    }

    std::shared_ptr<State> m_state;
    std::thread m_worker;
};

class PreviewWindow final : public QWidget
{
public:
    PreviewWindow()
    {
        setMinimumSize(640, 360);
        setAutoFillBackground(true);
        m_mediaProvider.setFrameReadyCallback([guard = QPointer<PreviewWindow>(this)]() {
            if (!guard) {
                return;
            }
            QMetaObject::invokeMethod(guard, [guard]() {
                if (guard) {
                    guard->update();
                }
            }, Qt::QueuedConnection);
        });
        connect(&m_frameTimer, &QTimer::timeout, this, [this]() {
            ensureRhi();
            m_mediaProvider.preloadFrames(m_timelineClips, m_currentFrame, m_playing);
            update();
        });
        m_frameTimer.setInterval(33);
    }

    ~PreviewWindow() override
    {
        // Stop timer before cleanup to prevent callbacks during destruction
        m_frameTimer.stop();
        releaseRhi();
    }

    void setPlaybackState(bool playing)
    {
        m_playing = playing;
        if (m_playing) {
            if (!m_frameTimer.isActive()) {
                m_frameTimer.start();
            }
        } else {
            m_frameTimer.stop();
        }
    }

    void setCurrentFrame(int frame)
    {
        m_currentFrame = frame;
        m_mediaProvider.preloadFrames(m_timelineClips, m_currentFrame, m_playing);
        update();
    }

    void setClipCount(int count)
    {
        m_clipCount = count;
        update();
    }

    void setTimelineClips(const QVector<TimelineClip> &clips)
    {
        m_timelineClips = clips;
        m_mediaProvider.preloadFrames(m_timelineClips, m_currentFrame, m_playing);
        update();
    }

    QString backendName() const
    {
        return m_backendName;
    }

protected:
    void showEvent(QShowEvent *event) override
    {
        QWidget::showEvent(event);
        ensureRhi();
    }

    void paintEvent(QPaintEvent *event) override
    {
        ensureRhi();
        m_mediaProvider.preloadFrames(m_timelineClips, m_currentFrame, m_playing);

        Q_UNUSED(event);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

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
        painter.fillRect(rect(), gradient);

        const QRect safeRect = rect().adjusted(24, 24, -24, -24);
        const QList<TimelineClip> activeClips = activeClipsForCurrentFrame();
        drawCompositedPreview(&painter, safeRect, activeClips);
        drawPreviewChrome(&painter, safeRect, activeClips.size());
    }

private:
    void ensureRhi()
    {
        if (m_rhiInitialized) {
            return;
        }
        m_rhiInitialized = true;

        // Create fallback surface first - must exist before RHI creation
        m_fallbackSurface = std::make_unique<QOffscreenSurface>();
        m_fallbackSurface->setFormat(QSurfaceFormat::defaultFormat());
        m_fallbackSurface->create();

        if (!m_fallbackSurface->isValid()) {
            qWarning() << "Failed to create offscreen surface for RHI";
            m_backendName = QStringLiteral("surface-failed");
            return;
        }

        // Try OpenGL ES2 backend first
        QRhiGles2InitParams params;
        params.format = QSurfaceFormat::defaultFormat();
        params.fallbackSurface = m_fallbackSurface.get();

        m_rhi.reset(QRhi::create(QRhi::OpenGLES2, &params, QRhi::Flags()));
        if (m_rhi) {
            m_backendName = QString::fromLatin1(m_rhi->backendName());
            return;
        }

        // Fall back to Null backend (software rendering)
        QRhiInitParams nullParams;
        m_rhi.reset(QRhi::create(QRhi::Null, &nullParams, QRhi::Flags()));
        if (m_rhi) {
            m_backendName = QString::fromLatin1(m_rhi->backendName()) + QStringLiteral(" (fallback)");
        } else {
            m_backendName = QStringLiteral("unavailable");
            qWarning() << "Failed to initialize any RHI backend";
        }
    }

    void releaseRhi()
    {
        // Destroy RHI before the surface it depends on
        m_rhi.reset();
        m_fallbackSurface.reset();
        m_rhiInitialized = false;
        m_backendName = QStringLiteral("not initialized");
    }

    QList<TimelineClip> activeClipsForCurrentFrame() const
    {
        QList<TimelineClip> active;
        for (const TimelineClip &clip : m_timelineClips) {
            if (m_currentFrame >= clip.startFrame && m_currentFrame < clip.startFrame + clip.durationFrames) {
                active.push_back(clip);
            }
        }
        std::sort(active.begin(), active.end(), [](const TimelineClip &a, const TimelineClip &b) {
            return a.startFrame < b.startFrame;
        });
        return active;
    }

    QRect fitRect(const QSize &source, const QRect &bounds) const
    {
        if (source.isEmpty() || bounds.isEmpty()) {
            return bounds;
        }

        QSize scaled = source;
        scaled.scale(bounds.size(), Qt::KeepAspectRatio);
        const QPoint topLeft(bounds.center().x() - scaled.width() / 2,
                             bounds.center().y() - scaled.height() / 2);
        return QRect(topLeft, scaled);
    }

    void drawFrameOrPlaceholder(QPainter *painter,
                                const QRect &targetRect,
                                const TimelineClip &clip,
                                const QImage &frame,
                                bool overlay) const
    {
        painter->save();
        painter->setClipRect(targetRect);

        if (!frame.isNull()) {
            if (overlay) {
                painter->setOpacity(0.92);
            }
            painter->drawImage(fitRect(frame.size(), targetRect), frame);
            painter->setOpacity(1.0);
        } else {
            painter->fillRect(targetRect, clip.color.darker(160));
            painter->setPen(QColor(255, 255, 255, 48));
            painter->drawRect(targetRect.adjusted(0, 0, -1, -1));
            painter->setPen(QColor("#f2f6fa"));
            painter->drawText(targetRect.adjusted(16, 16, -16, -16),
                              Qt::AlignCenter | Qt::TextWordWrap,
                              QString("%1\n%2")
                                  .arg(clip.label)
                                  .arg(frame.isNull() ? "Frame unavailable" : ""));
        }

        painter->restore();
    }

    void drawCompositedPreview(QPainter *painter, const QRect &safeRect, const QList<TimelineClip> &activeClips)
    {
        if (!painter || !painter->isActive()) {
            qWarning() << "drawCompositedPreview called with invalid painter";
            return;
        }
        
        painter->save();
        painter->setPen(QPen(QColor(255, 255, 255, 40), 1.5));
        painter->setBrush(QColor(255, 255, 255, 18));
        painter->drawRoundedRect(safeRect, 18, 18);

        if (activeClips.isEmpty()) {
            painter->setPen(QColor("#f5f8fb"));
            QFont titleFont = painter->font();
            titleFont.setPointSize(titleFont.pointSize() + 4);
            titleFont.setBold(true);
            painter->setFont(titleFont);
            painter->drawText(safeRect.adjusted(20, 18, -20, -20),
                              Qt::AlignTop | Qt::AlignLeft,
                              "Preview");

            QFont bodyFont = painter->font();
            bodyFont.setBold(false);
            bodyFont.setPointSize(qMax(10, bodyFont.pointSize() - 2));
            painter->setFont(bodyFont);
            painter->setPen(QColor("#d2dbe4"));
            painter->drawText(safeRect.adjusted(20, 58, -20, -20),
                              Qt::AlignTop | Qt::AlignLeft,
                              QString("No active clips at this frame.\nFrame %1\nQRhi backend: %2")
                                  .arg(m_currentFrame)
                                  .arg(m_backendName));
            painter->restore();
            return;
        }

        // Composite base layer (bottom-most clip)
        const QRect baseRect = safeRect.adjusted(12, 12, -12, -12);
        const TimelineClip &baseClip = activeClips.constLast();
        const QImage baseFrame = m_mediaProvider.frameForClip(baseClip, m_currentFrame);
        drawFrameOrPlaceholder(painter, baseRect, baseClip, baseFrame, false);

        // Composite overlay layers (up to 3 overlays)
        const int overlayCount = qMin(static_cast<int>(activeClips.size()) - 1, 3);
        for (int i = 0; i < overlayCount; ++i) {
            const TimelineClip &clip = activeClips.at(i);
            const QImage frame = m_mediaProvider.frameForClip(clip, m_currentFrame);
            
            // Calculate overlay position (stacked on right side)
            const int overlayWidth = qMin(220, safeRect.width() / 3);
            const int overlayHeight = overlayWidth * 9 / 16;
            const int margin = 14;
            const QRect overlayRect(safeRect.right() - overlayWidth - 18,
                                    safeRect.top() + 18 + i * (overlayHeight + margin),
                                    overlayWidth,
                                    overlayHeight);
            
            // Draw overlay frame background
            painter->setPen(QPen(QColor(255, 255, 255, 72), 1.0));
            painter->setBrush(QColor(9, 12, 16, 180));
            painter->drawRoundedRect(overlayRect.adjusted(-6, -6, 6, 28), 12, 12);
            
            // Draw the frame content
            drawFrameOrPlaceholder(painter, overlayRect, clip, frame, true);
            
            // Draw label below the frame
            painter->setPen(QColor(QStringLiteral("#edf3f8")));
            painter->drawText(overlayRect.adjusted(8, overlayRect.height() + 6, -8, 22),
                              Qt::AlignLeft | Qt::AlignVCenter,
                              clip.label);
        }

        painter->restore();
    }

    void drawPreviewChrome(QPainter *painter, const QRect &safeRect, int activeClipCount) const
    {
        painter->save();
        painter->setPen(QColor("#f5f8fb"));
        QFont titleFont = painter->font();
        titleFont.setPointSize(titleFont.pointSize() + 1);
        titleFont.setBold(true);
        painter->setFont(titleFont);
        painter->drawText(safeRect.adjusted(20, 16, -20, -20),
                          Qt::AlignTop | Qt::AlignLeft,
                          QString("Preview  |  Active clips %1").arg(activeClipCount));

        QFont bodyFont = painter->font();
        bodyFont.setBold(false);
        bodyFont.setPointSize(qMax(9, bodyFont.pointSize() - 2));
        painter->setFont(bodyFont);
        painter->setPen(QColor("#d2dbe4"));
        painter->drawText(safeRect.adjusted(20, 40, -20, -20),
                          Qt::AlignTop | Qt::AlignLeft,
                          QString("Frame %1  |  QRhi backend: %2").arg(m_currentFrame).arg(m_backendName));
        painter->restore();
    }

    QTimer m_frameTimer;
    std::unique_ptr<QOffscreenSurface> m_fallbackSurface;
    std::unique_ptr<QRhi> m_rhi;
    bool m_rhiInitialized = false;
    bool m_playing = false;
    int m_currentFrame = 0;
    int m_clipCount = 0;
    QString m_backendName = "not initialized";
    QVector<TimelineClip> m_timelineClips;
    MediaFrameProvider m_mediaProvider;
};

class TimelineWidget final : public QWidget
{
public:
    explicit TimelineWidget(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setAcceptDrops(true);
        setMinimumHeight(150);
        setMouseTracking(true);
        setAutoFillBackground(true);

        QPalette pal = palette();
        pal.setColor(QPalette::Window, QColor("#0f1216"));
        setPalette(pal);
    }

    void setCurrentFrame(int frame)
    {
        m_currentFrame = qMax(0, frame);
        update();
    }

    int currentFrame() const
    {
        return m_currentFrame;
    }

    int totalFrames() const
    {
        int lastFrame = 300;
        for (const TimelineClip &clip : m_clips) {
            lastFrame = qMax(lastFrame, clip.startFrame + clip.durationFrames + 30);
        }
        return lastFrame;
    }

    void addClipFromFile(const QString &filePath, int startFrame = -1)
    {
        const QFileInfo info(filePath);
        if (!info.exists() || !info.isFile()) {
            return;
        }

        TimelineClip clip;
        clip.filePath = filePath;
        clip.label = info.fileName();
        clip.startFrame = startFrame >= 0 ? startFrame : totalFrames();
        clip.durationFrames = mediaDurationFrames(info);
        clip.color = colorForPath(filePath);

        m_clips.push_back(clip);
        sortClips();

        if (clipsChanged) {
            clipsChanged();
        }
        update();
    }

    const QVector<TimelineClip> &clips() const
    {
        return m_clips;
    }

    void setClips(const QVector<TimelineClip> &clips)
    {
        m_clips = clips;
        sortClips();
        update();
    }

    std::function<void(int)> seekRequested;
    std::function<void()> clipsChanged;

protected:
    void dragEnterEvent(QDragEnterEvent *event) override
    {
        if (hasFileUrls(event->mimeData())) {
            event->acceptProposedAction();
            return;
        }
        QWidget::dragEnterEvent(event);
    }

    void dragMoveEvent(QDragMoveEvent *event) override
    {
        if (hasFileUrls(event->mimeData())) {
            m_dropFrame = frameFromX(event->position().x());
            event->acceptProposedAction();
            update();
            return;
        }
        QWidget::dragMoveEvent(event);
    }

    void dragLeaveEvent(QDragLeaveEvent *event) override
    {
        m_dropFrame = -1;
        QWidget::dragLeaveEvent(event);
        update();
    }

    void dropEvent(QDropEvent *event) override
    {
        if (!hasFileUrls(event->mimeData())) {
            QWidget::dropEvent(event);
            return;
        }

        int insertFrame = frameFromX(event->position().x());
        for (const QUrl &url : event->mimeData()->urls()) {
            if (!url.isLocalFile()) {
                continue;
            }

            const QString filePath = url.toLocalFile();
            const QFileInfo info(filePath);
            if (!info.exists() || info.isDir()) {
                continue;
            }

            TimelineClip clip;
            clip.filePath = filePath;
            clip.label = info.fileName();
            clip.startFrame = insertFrame;
            clip.durationFrames = mediaDurationFrames(info);
            clip.color = colorForPath(filePath);

            m_clips.push_back(clip);
            insertFrame += clip.durationFrames + 6;
        }

        sortClips();

        m_dropFrame = -1;
        event->acceptProposedAction();
        if (clipsChanged) {
            clipsChanged();
        }
        update();
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton) {
            const int hitIndex = clipIndexAt(event->position().toPoint());
            if (hitIndex >= 0) {
                m_draggedClipIndex = hitIndex;
                m_dragOffsetFrames = frameFromX(event->position().x()) - m_clips[hitIndex].startFrame;
                m_currentFrame = m_clips[hitIndex].startFrame;
                update();
                return;
            }

            const int frame = frameFromX(event->position().x());
            m_currentFrame = frame;
            if (seekRequested) {
                seekRequested(frame);
            }
            update();
            return;
        }
        QWidget::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (m_draggedClipIndex >= 0 && (event->buttons() & Qt::LeftButton)) {
            TimelineClip &clip = m_clips[m_draggedClipIndex];
            const int newStartFrame = qMax(0, frameFromX(event->position().x()) - m_dragOffsetFrames);
            clip.startFrame = newStartFrame;
            m_currentFrame = newStartFrame;
            update();
            return;
        }

        QWidget::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && m_draggedClipIndex >= 0) {
            sortClips();
            m_draggedClipIndex = -1;
            m_dragOffsetFrames = 0;
            if (clipsChanged) {
                clipsChanged();
            }
            update();
            return;
        }

        QWidget::mouseReleaseEvent(event);
    }

    void contextMenuEvent(QContextMenuEvent *event) override
    {
        const int clipIndex = clipIndexAt(event->pos());
        if (clipIndex < 0) {
            QWidget::contextMenuEvent(event);
            return;
        }

        QMenu menu(this);
        QAction *deleteAction = menu.addAction("Delete");
        QAction *propertiesAction = menu.addAction("Properties");

        QAction *selected = menu.exec(event->globalPos());
        if (!selected) {
            return;
        }

        if (selected == deleteAction) {
            m_clips.removeAt(clipIndex);
            if (clipsChanged) {
                clipsChanged();
            }
            update();
            return;
        }

        if (selected == propertiesAction) {
            const TimelineClip &clip = m_clips[clipIndex];
            const QFileInfo info(clip.filePath);
            QMessageBox::information(
                this,
                "Clip Properties",
                QString("Name: %1\nPath: %2\nStart: %3\nDuration: %4 frames\nDuration: %5")
                    .arg(clip.label)
                    .arg(QDir::toNativeSeparators(clip.filePath))
                    .arg(timecodeForFrame(clip.startFrame))
                    .arg(clip.durationFrames)
                    .arg(timecodeForFrame(clip.durationFrames)));
            return;
        }
    }

    void wheelEvent(QWheelEvent *event) override
    {
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
            m_frameOffset = qMax(0, m_frameOffset - steps * panFrames);
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
            m_frameOffset = qMax(0, qRound(newOffset));
        }

        if (!qFuzzyCompare(oldPixelsPerFrame, m_pixelsPerFrame)) {
            update();
        }
        event->accept();
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.fillRect(rect(), QColor("#0f1216"));

        const QRect drawRect = rect().adjusted(16, 16, -16, -16);
        const QRect rulerRect(drawRect.left(), drawRect.top(), drawRect.width(), 28);
        const QRect trackRect(drawRect.left(), rulerRect.bottom() + 12, drawRect.width(), drawRect.height() - 40);

        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor("#171c22"));
        painter.drawRoundedRect(trackRect, 10, 10);

        painter.setPen(QColor("#6d7887"));
        for (int frame = 0; frame <= totalFrames(); frame += 30) {
            const int x = xFromFrame(frame);
            const bool major = (frame % 150) == 0;
            painter.setPen(major ? QColor("#8fa0b5") : QColor("#53606e"));
            painter.drawLine(x, rulerRect.bottom() - (major ? 18 : 10), x, trackRect.bottom() - 8);

            if (major) {
                painter.drawText(QRect(x + 4, rulerRect.top(), 56, rulerRect.height()),
                                 Qt::AlignLeft | Qt::AlignVCenter,
                                 timecodeForFrame(frame));
            }
        }

        int row = 0;
        for (const TimelineClip &clip : m_clips) {
            const int clipX = xFromFrame(clip.startFrame);
            const int clipW = qMax(40, widthForFrames(clip.durationFrames));
            const int clipY = trackRect.top() + 12 + row * 44;
            const QRect clipRect(clipX, clipY, clipW, 32);

            painter.setPen(QColor(255, 255, 255, 32));
            painter.setBrush(clip.color);
            painter.drawRoundedRect(clipRect, 7, 7);

            painter.setPen(QColor("#f4f7fb"));
            painter.drawText(clipRect.adjusted(10, 0, -10, 0),
                             Qt::AlignLeft | Qt::AlignVCenter,
                             painter.fontMetrics().elidedText(clip.label, Qt::ElideRight, clipRect.width() - 20));

            row = (row + 1) % 3;
        }

        if (m_dropFrame >= 0) {
            const int x = xFromFrame(m_dropFrame);
            painter.setPen(QPen(QColor("#f7b955"), 2, Qt::DashLine));
            painter.drawLine(x, trackRect.top() + 2, x, trackRect.bottom() - 2);
        }

        const int playheadX = xFromFrame(m_currentFrame);
        painter.setPen(QPen(QColor("#ff6f61"), 3));
        painter.drawLine(playheadX, rulerRect.top(), playheadX, trackRect.bottom());

        painter.setBrush(QColor("#ff6f61"));
        painter.setPen(Qt::NoPen);
        painter.drawRoundedRect(QRect(playheadX - 8, rulerRect.top(), 16, 12), 4, 4);
    }

private:
    static constexpr int kTimelineFps = 30;

    void sortClips()
    {
        std::sort(m_clips.begin(), m_clips.end(), [](const TimelineClip &a, const TimelineClip &b) {
            if (a.startFrame == b.startFrame) {
                return a.label < b.label;
            }
            return a.startFrame < b.startFrame;
        });
    }

    int clipIndexAt(const QPoint &pos) const
    {
        const QRect drawRect = rect().adjusted(16, 16, -16, -16);
        const QRect rulerRect(drawRect.left(), drawRect.top(), drawRect.width(), 28);
        const QRect trackRect(drawRect.left(), rulerRect.bottom() + 12, drawRect.width(), drawRect.height() - 40);

        int row = 0;
        for (int i = 0; i < m_clips.size(); ++i) {
            const TimelineClip &clip = m_clips[i];
            const int clipX = xFromFrame(clip.startFrame);
            const int clipW = qMax(40, widthForFrames(clip.durationFrames));
            const int clipY = trackRect.top() + 12 + row * 44;
            const QRect clipRect(clipX, clipY, clipW, 32);
            if (clipRect.contains(pos)) {
                return i;
            }
            row = (row + 1) % 3;
        }

        return -1;
    }

    int mediaDurationFrames(const QFileInfo &info) const
    {
        const QString suffix = info.suffix().toLower();
        if (suffix == "png" || suffix == "jpg" || suffix == "jpeg" || suffix == "webp") {
            return 90;
        }

        QProcess process;
        process.start("ffprobe",
                      {
                          "-v", "error",
                          "-show_entries", "format=duration",
                          "-of", "default=noprint_wrappers=1:nokey=1",
                          info.absoluteFilePath()
                      });

        if (process.waitForFinished(3000) && process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0) {
            bool ok = false;
            const double seconds = QString::fromUtf8(process.readAllStandardOutput()).trimmed().toDouble(&ok);
            if (ok && seconds > 0.0) {
                return qMax(1, qRound(seconds * kTimelineFps));
            }
        }

        return guessDurationFrames(info);
    }

    bool hasFileUrls(const QMimeData *mimeData) const
    {
        if (!mimeData || !mimeData->hasUrls()) {
            return false;
        }
        for (const QUrl &url : mimeData->urls()) {
            if (url.isLocalFile()) {
                return true;
            }
        }
        return false;
    }

    int guessDurationFrames(const QFileInfo &info) const
    {
        const QString suffix = info.suffix().toLower();
        if (suffix == "mp4" || suffix == "mov" || suffix == "mkv" || suffix == "webm") {
            return 180;
        }
        if (suffix == "wav" || suffix == "mp3" || suffix == "aac" || suffix == "flac") {
            return 150;
        }
        if (suffix == "png" || suffix == "jpg" || suffix == "jpeg" || suffix == "webp") {
            return 90;
        }
        return 120;
    }

    QColor colorForPath(const QString &path) const
    {
        const quint32 hash = qHash(path);
        return QColor::fromHsv(static_cast<int>(hash % 360), 160, 220, 220);
    }

    QString timecodeForFrame(int frame) const
    {
        const int seconds = frame / kTimelineFps;
        const int minutes = seconds / 60;
        const int secs = seconds % 60;
        return QStringLiteral("%1:%2")
            .arg(minutes, 2, 10, QLatin1Char('0'))
            .arg(secs, 2, 10, QLatin1Char('0'));
    }

    int frameFromX(qreal x) const
    {
        const int left = 16;
        const qreal normalized = qMax<qreal>(0.0, x - left);
        return m_frameOffset + static_cast<int>(normalized / m_pixelsPerFrame);
    }

    int xFromFrame(int frame) const
    {
        return 16 + widthForFrames(frame - m_frameOffset);
    }

    int widthForFrames(int frames) const
    {
        return static_cast<int>(frames * m_pixelsPerFrame);
    }

    QVector<TimelineClip> m_clips;
    int m_currentFrame = 0;
    int m_dropFrame = -1;
    int m_draggedClipIndex = -1;
    int m_dragOffsetFrames = 0;
    qreal m_pixelsPerFrame = 4.0;
    int m_frameOffset = 0;
};

class EditorWindow final : public QMainWindow
{
public:
    EditorWindow()
    {
        setWindowTitle("QRhi Editor");
        resize(1500, 900);

        auto *central = new QWidget(this);
        auto *rootLayout = new QHBoxLayout(central);
        rootLayout->setContentsMargins(0, 0, 0, 0);
        rootLayout->setSpacing(0);

        auto *splitter = new QSplitter(Qt::Horizontal, central);
        splitter->setChildrenCollapsible(false);
        rootLayout->addWidget(splitter);

        splitter->addWidget(buildExplorerPane());
        splitter->addWidget(buildEditorPane());
        splitter->setStretchFactor(0, 0);
        splitter->setStretchFactor(1, 1);
        splitter->setSizes({ 320, 1180 });

        setCentralWidget(central);

        connect(&m_playbackTimer, &QTimer::timeout, this, [this]() {
            const int nextFrame = m_timeline->currentFrame() + 1;
            const int wrapped = nextFrame > m_timeline->totalFrames() ? 0 : nextFrame;
            setCurrentFrame(wrapped);
        });
        m_playbackTimer.setInterval(33);

        loadState();
        refreshInspector();
    }

    ~EditorWindow() override
    {
        saveState();
    }

protected:
    void closeEvent(QCloseEvent *event) override
    {
        saveState();
        QMainWindow::closeEvent(event);
    }

private:
    QString stateFilePath() const
    {
        return QDir(QApplication::applicationDirPath()).filePath("editor_state.json");
    }

    void setExplorerRootPath(const QString &path, bool saveAfterChange = true)
    {
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
            saveState();
        }
    }

    void chooseExplorerRoot()
    {
        const QString startPath = m_currentRootPath.isEmpty() ? QDir::currentPath() : m_currentRootPath;
        const QString selected = QFileDialog::getExistingDirectory(this,
                                                                   "Select Media Folder",
                                                                   startPath,
                                                                   QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
        if (!selected.isEmpty()) {
            setExplorerRootPath(selected, true);
        }
    }

    QJsonObject clipToJson(const TimelineClip &clip) const
    {
        QJsonObject obj;
        obj["filePath"] = clip.filePath;
        obj["label"] = clip.label;
        obj["startFrame"] = clip.startFrame;
        obj["durationFrames"] = clip.durationFrames;
        obj["color"] = clip.color.name(QColor::HexArgb);
        return obj;
    }

    TimelineClip clipFromJson(const QJsonObject &obj) const
    {
        TimelineClip clip;
        clip.filePath = obj.value("filePath").toString();
        clip.label = obj.value("label").toString(QFileInfo(clip.filePath).fileName());
        clip.startFrame = obj.value("startFrame").toInt();
        clip.durationFrames = obj.value("durationFrames").toInt(120);
        clip.color = QColor(obj.value("color").toString());
        if (!clip.color.isValid()) {
            clip.color = QColor::fromHsv(static_cast<int>(qHash(clip.filePath) % 360), 160, 220, 220);
        }
        return clip;
    }

    void loadState()
    {
        m_loadingState = true;

        QString rootPath = QDir::currentPath();
        QVector<TimelineClip> loadedClips;
        int currentFrame = 0;
        bool playing = false;

        QFile file(stateFilePath());
        if (file.open(QIODevice::ReadOnly)) {
            const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
            const QJsonObject root = doc.object();

            rootPath = root.value("explorerRoot").toString(rootPath);
            currentFrame = root.value("currentFrame").toInt(0);
            playing = root.value("playing").toBool(false);

            const QJsonArray clips = root.value("timeline").toArray();
            loadedClips.reserve(clips.size());
            for (const QJsonValue &value : clips) {
                if (!value.isObject()) {
                    continue;
                }
                const TimelineClip clip = clipFromJson(value.toObject());
                if (!clip.filePath.isEmpty()) {
                    loadedClips.push_back(clip);
                }
            }
        }

        setExplorerRootPath(rootPath, false);
        m_timeline->setClips(loadedClips);
        syncSliderRange();
        m_preview->setClipCount(m_timeline->clips().size());
        m_preview->setTimelineClips(m_timeline->clips());
        setCurrentFrame(currentFrame);

        if (playing) {
            m_playbackTimer.start();
            m_preview->setPlaybackState(true);
        } else {
            m_playbackTimer.stop();
            m_preview->setPlaybackState(false);
        }

        m_loadingState = false;
        saveState();
    }

    void saveState() const
    {
        if (m_loadingState || !m_timeline) {
            return;
        }

        QJsonObject root;
        root["explorerRoot"] = m_currentRootPath;
        root["currentFrame"] = m_timeline->currentFrame();
        root["playing"] = m_playbackTimer.isActive();

        QJsonArray timeline;
        for (const TimelineClip &clip : m_timeline->clips()) {
            timeline.push_back(clipToJson(clip));
        }
        root["timeline"] = timeline;

        QSaveFile file(stateFilePath());
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            return;
        }

        file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
        file.commit();
    }

    QWidget *buildExplorerPane()
    {
        auto *pane = new QFrame;
        pane->setFrameShape(QFrame::NoFrame);
        pane->setMinimumWidth(260);
        pane->setStyleSheet(
            "QFrame { background: #11161c; color: #e8edf2; }"
            "QLabel { color: #dce5ee; font-weight: 600; letter-spacing: 0.08em; }"
            "QPushButton#folderPicker { background: transparent; border: none; color: #dce5ee; font-weight: 700; letter-spacing: 0.08em; padding: 0; text-align: left; }"
            "QPushButton#folderPicker:hover { color: #ffffff; }"
            "QLabel#rootPath { color: #8ea0b2; font-size: 11px; letter-spacing: 0; }"
            "QTreeView { background: transparent; border: none; color: #dbe2ea; }"
            "QTreeView::item { padding: 4px 0; }"
            "QTreeView::item:selected { background: #213042; color: #f7fbff; }");

        auto *layout = new QVBoxLayout(pane);
        layout->setContentsMargins(14, 14, 14, 14);
        layout->setSpacing(10);

        m_folderPickerButton = new QPushButton("FILES");
        m_folderPickerButton->setObjectName("folderPicker");
        m_folderPickerButton->setCursor(Qt::PointingHandCursor);
        layout->addWidget(m_folderPickerButton);

        m_rootPathLabel = new QLabel;
        m_rootPathLabel->setObjectName("rootPath");
        m_rootPathLabel->setWordWrap(true);
        layout->addWidget(m_rootPathLabel);

        m_fsModel = new QFileSystemModel(this);
        m_fsModel->setFilter(QDir::AllDirs | QDir::Files | QDir::NoDotAndDotDot);

        m_tree = new QTreeView;
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

        connect(m_tree, &QTreeView::doubleClicked, this, [this](const QModelIndex &index) {
            if (!m_fsModel) {
                return;
            }
            const QFileInfo info = m_fsModel->fileInfo(index);
            if (info.exists() && info.isFile()) {
                addFileToTimeline(info.absoluteFilePath());
            }
        });
        connect(m_folderPickerButton, &QPushButton::clicked, this, [this]() {
            chooseExplorerRoot();
        });

        return pane;
    }

    QWidget *buildEditorPane()
    {
        auto *pane = new QWidget;
        pane->setStyleSheet(
            "QWidget { background: #0c1015; color: #edf2f7; }"
            "QPushButton, QToolButton { background: #1b2430; border: 1px solid #2e3b4a; border-radius: 7px; padding: 8px 12px; }"
            "QPushButton:hover, QToolButton:hover { background: #233142; }"
            "QSlider::groove:horizontal { background: #24303c; height: 6px; border-radius: 3px; }"
            "QSlider::handle:horizontal { background: #ff6f61; width: 14px; margin: -5px 0; border-radius: 7px; }");

        auto *layout = new QVBoxLayout(pane);
        layout->setContentsMargins(18, 18, 18, 18);
        layout->setSpacing(14);

        auto *previewFrame = new QFrame;
        previewFrame->setMinimumHeight(360);
        previewFrame->setFrameShape(QFrame::NoFrame);
        previewFrame->setStyleSheet("QFrame { background: #05080c; border: 1px solid #202934; border-radius: 14px; }");
        auto *previewLayout = new QVBoxLayout(previewFrame);
        previewLayout->setContentsMargins(0, 0, 0, 0);
        previewLayout->setSpacing(0);

        m_preview = new PreviewWindow;
        m_preview->setFocusPolicy(Qt::StrongFocus);
        m_preview->setMinimumSize(640, 360);

        auto *overlay = new QWidget;
        overlay->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        overlay->setStyleSheet("background: transparent;");
        auto *overlayLayout = new QVBoxLayout(overlay);
        overlayLayout->setContentsMargins(18, 14, 18, 14);
        overlayLayout->setSpacing(6);

        auto *badgeRow = new QHBoxLayout;
        badgeRow->setContentsMargins(0, 0, 0, 0);
        m_statusBadge = new QLabel;
        m_statusBadge->setStyleSheet("QLabel { background: rgba(7, 11, 17, 0.72); color: #f2f7fb; border-radius: 10px; padding: 8px 12px; font-weight: 600; }");
        badgeRow->addWidget(m_statusBadge, 0, Qt::AlignLeft);
        badgeRow->addStretch(1);
        overlayLayout->addLayout(badgeRow);

        overlayLayout->addStretch(1);

        m_previewInfo = new QLabel;
        m_previewInfo->setStyleSheet("QLabel { background: rgba(7, 11, 17, 0.72); color: #dce6ef; border-radius: 10px; padding: 10px 12px; }");
        m_previewInfo->setWordWrap(true);
        overlayLayout->addWidget(m_previewInfo, 0, Qt::AlignLeft | Qt::AlignBottom);

        auto *stack = new QStackedLayout;
        stack->setStackingMode(QStackedLayout::StackAll);
        stack->addWidget(m_preview);
        stack->addWidget(overlay);
        previewLayout->addLayout(stack);
        layout->addWidget(previewFrame, 3);

        auto *transport = new QWidget;
        auto *transportLayout = new QHBoxLayout(transport);
        transportLayout->setContentsMargins(0, 0, 0, 0);
        transportLayout->setSpacing(10);

        m_playButton = new QPushButton(style()->standardIcon(QStyle::SP_MediaPlay), "Play");
        auto *pauseButton = new QPushButton(style()->standardIcon(QStyle::SP_MediaPause), "Pause");
        auto *startButton = new QToolButton;
        auto *endButton = new QToolButton;
        startButton->setIcon(style()->standardIcon(QStyle::SP_MediaSkipBackward));
        endButton->setIcon(style()->standardIcon(QStyle::SP_MediaSkipForward));

        m_seekSlider = new QSlider(Qt::Horizontal);
        m_seekSlider->setRange(0, 300);

        m_timecodeLabel = new QLabel;
        m_timecodeLabel->setMinimumWidth(96);

        transportLayout->addWidget(startButton);
        transportLayout->addWidget(m_playButton);
        transportLayout->addWidget(pauseButton);
        transportLayout->addWidget(endButton);
        transportLayout->addWidget(m_seekSlider, 1);
        transportLayout->addWidget(m_timecodeLabel);
        layout->addWidget(transport, 0);

        m_timeline = new TimelineWidget;
        layout->addWidget(m_timeline, 2);

        connect(m_playButton, &QPushButton::clicked, this, [this]() {
            m_playbackTimer.start();
            m_preview->setPlaybackState(true);
            refreshInspector();
            saveState();
        });
        connect(pauseButton, &QPushButton::clicked, this, [this]() {
            m_playbackTimer.stop();
            m_preview->setPlaybackState(false);
            refreshInspector();
            saveState();
        });
        connect(startButton, &QToolButton::clicked, this, [this]() {
            setCurrentFrame(0);
        });
        connect(endButton, &QToolButton::clicked, this, [this]() {
            setCurrentFrame(m_timeline->totalFrames());
        });
        connect(m_seekSlider, &QSlider::valueChanged, this, [this](int value) {
            if (m_ignoreSeekSignal) {
                return;
            }
            setCurrentFrame(value);
        });

        m_timeline->seekRequested = [this](int frame) {
            setCurrentFrame(frame);
        };
        m_timeline->clipsChanged = [this]() {
            syncSliderRange();
            m_preview->setClipCount(m_timeline->clips().size());
            m_preview->setTimelineClips(m_timeline->clips());
            refreshInspector();
            saveState();
        };

        return pane;
    }

    void addFileToTimeline(const QString &filePath)
    {
        if (m_timeline) {
            m_timeline->addClipFromFile(filePath);
            m_preview->setTimelineClips(m_timeline->clips());
        }
    }

    void syncSliderRange()
    {
        const int maxFrame = m_timeline->totalFrames();
        m_seekSlider->setRange(0, maxFrame);
    }

    void setCurrentFrame(int frame)
    {
        const int bounded = qBound(0, frame, m_timeline->totalFrames());
        m_timeline->setCurrentFrame(bounded);
        m_preview->setCurrentFrame(bounded);
        m_preview->setTimelineClips(m_timeline->clips());

        m_ignoreSeekSignal = true;
        m_seekSlider->setValue(bounded);
        m_ignoreSeekSignal = false;

        m_timecodeLabel->setText(frameToTimecode(bounded));
        refreshInspector();
        saveState();
    }

    QString frameToTimecode(int frame) const
    {
        const int fps = 30;
        const int totalSeconds = frame / fps;
        const int minutes = totalSeconds / 60;
        const int seconds = totalSeconds % 60;
        const int frames = frame % fps;

        return QStringLiteral("%1:%2:%3")
            .arg(minutes, 2, 10, QLatin1Char('0'))
            .arg(seconds, 2, 10, QLatin1Char('0'))
            .arg(frames, 2, 10, QLatin1Char('0'));
    }

    void refreshInspector()
    {
        const bool playing = m_playbackTimer.isActive();
        const QString state = playing ? "PLAYING" : "PAUSED";
        const int clipCount = m_timeline ? m_timeline->clips().size() : 0;

        m_statusBadge->setText(QString("%1  |  %2 clips").arg(state).arg(clipCount));
        m_previewInfo->setText(QString("Preview rendered with QRhi.\nBackend: %1\nSeek: %2\nDrag files from the left explorer onto the timeline below.")
                                   .arg(m_preview ? m_preview->backendName() : QStringLiteral("unknown"))
                                   .arg(frameToTimecode(m_timeline ? m_timeline->currentFrame() : 0)));
        m_playButton->setText(playing ? "Playing" : "Play");
    }

    QFileSystemModel *m_fsModel = nullptr;
    QTreeView *m_tree = nullptr;
    QPushButton *m_folderPickerButton = nullptr;
    QLabel *m_rootPathLabel = nullptr;
    PreviewWindow *m_preview = nullptr;
    TimelineWidget *m_timeline = nullptr;
    QPushButton *m_playButton = nullptr;
    QSlider *m_seekSlider = nullptr;
    QLabel *m_timecodeLabel = nullptr;
    QLabel *m_statusBadge = nullptr;
    QLabel *m_previewInfo = nullptr;
    QTimer m_playbackTimer;
    bool m_ignoreSeekSignal = false;
    bool m_loadingState = false;
    QString m_currentRootPath;
};

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QApplication::setApplicationName("QRhi Editor");

    EditorWindow window;
    window.show();

    return app.exec();
}
