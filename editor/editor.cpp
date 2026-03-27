#include "frame_handle.h"
#include "clip_serialization.h"
#include "transcript_engine.h"
#include "async_decoder.h"
#include "timeline_cache.h"
#include "memory_budget.h"
#include "gpu_compositor.h"
#include "control_server.h"
#include "debug_controls.h"
#include "editor_shared.h"
#include "audio_engine.h"
#include "timeline_widget.h"
#include "preview.h"
#include "render.h"
#include "inspector_pane.h"
#include "explorer_pane.h"
#include "editor_pane.h"

#include <QApplication>
#include <QColor>
#include <QCheckBox>
#include <QCommandLineParser>
#include <QCommandLineOption>
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
#include <QStandardPaths>
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

extern "C"
{
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

#include "playback_debug.h"

// ============================================================================
// EditorWindow - Main application window
// ============================================================================
class EditorWindow final : public QMainWindow
{
    Q_OBJECT
public:
    explicit EditorWindow(quint16 controlPort)
    {
        QElapsedTimer ctorTimer;
        ctorTimer.start();

        setWindowTitle(QStringLiteral("QRhi Editor - Professional"));
        resize(1500, 900);

        auto *central = new QWidget(this);
        auto *rootLayout = new QHBoxLayout(central);
        rootLayout->setContentsMargins(0, 0, 0, 0);
        rootLayout->setSpacing(0);

        auto *splitter = new QSplitter(Qt::Horizontal, central);
        splitter->setObjectName(QStringLiteral("layout.main_splitter"));
        splitter->setChildrenCollapsible(false);
        rootLayout->addWidget(splitter);

        qDebug() << "[STARTUP] Building explorer pane...";
        m_explorerPane = new ExplorerPane(this);
        splitter->addWidget(m_explorerPane);

        connect(m_explorerPane, &ExplorerPane::fileActivated, this, [this](const QString &path)
                { addFileToTimeline(path); });

        connect(m_explorerPane, &ExplorerPane::transcriptionRequested, this, [this](const QString &path, const QString &label)
                { openTranscriptionWindow(path, label); });

        connect(m_explorerPane, &ExplorerPane::stateChanged, this, [this]()
                {
            scheduleSaveState();
            pushHistorySnapshot(); });
        qDebug() << "[STARTUP] Explorer pane built in" << ctorTimer.elapsed() << "ms";

        qDebug() << "[STARTUP] Building editor pane...";
        QElapsedTimer editorPaneTimer;
        editorPaneTimer.start();
        splitter->addWidget(buildEditorPane());
        m_explorerPane->setPreviewWindow(m_preview);
        qDebug() << "[STARTUP] Editor pane built in" << editorPaneTimer.elapsed() << "ms";
        m_inspectorPane = new InspectorPane(this);
        splitter->addWidget(m_inspectorPane);

        // Wire up inspector pane widgets
        m_transcriptTable = m_inspectorPane->transcriptTable();
        m_transcriptInspectorClipLabel = m_inspectorPane->transcriptInspectorClipLabel();
        m_transcriptInspectorDetailsLabel = m_inspectorPane->transcriptInspectorDetailsLabel();
        m_clipInspectorClipLabel = m_inspectorPane->clipInspectorClipLabel();
        m_clipProxyUsageLabel = m_inspectorPane->clipProxyUsageLabel();
        m_clipPlaybackSourceLabel = m_inspectorPane->clipPlaybackSourceLabel();
        m_clipOriginalInfoLabel = m_inspectorPane->clipOriginalInfoLabel();
        m_clipProxyInfoLabel = m_inspectorPane->clipProxyInfoLabel();
        m_transcriptOverlayEnabledCheckBox = m_inspectorPane->transcriptOverlayEnabledCheckBox();
        m_transcriptMaxLinesSpin = m_inspectorPane->transcriptMaxLinesSpin();
        m_transcriptMaxCharsSpin = m_inspectorPane->transcriptMaxCharsSpin();
        m_transcriptAutoScrollCheckBox = m_inspectorPane->transcriptAutoScrollCheckBox();
        m_transcriptFollowCurrentWordCheckBox = m_inspectorPane->transcriptFollowCurrentWordCheckBox();
        m_transcriptOverlayXSpin = m_inspectorPane->transcriptOverlayXSpin();
        m_transcriptOverlayYSpin = m_inspectorPane->transcriptOverlayYSpin();
        m_transcriptOverlayWidthSpin = m_inspectorPane->transcriptOverlayWidthSpin();
        m_transcriptOverlayHeightSpin = m_inspectorPane->transcriptOverlayHeightSpin();
        m_transcriptFontFamilyCombo = m_inspectorPane->transcriptFontFamilyCombo();
        m_transcriptFontSizeSpin = m_inspectorPane->transcriptFontSizeSpin();
        m_transcriptBoldCheckBox = m_inspectorPane->transcriptBoldCheckBox();
        m_transcriptItalicCheckBox = m_inspectorPane->transcriptItalicCheckBox();
        m_syncTable = m_inspectorPane->syncTable();
        m_syncInspectorClipLabel = m_inspectorPane->syncInspectorClipLabel();
        m_syncInspectorDetailsLabel = m_inspectorPane->syncInspectorDetailsLabel();
        m_gradingPathLabel = m_inspectorPane->gradingPathLabel();
        m_brightnessSpin = m_inspectorPane->brightnessSpin();
        m_contrastSpin = m_inspectorPane->contrastSpin();
        m_saturationSpin = m_inspectorPane->saturationSpin();
        m_opacitySpin = m_inspectorPane->opacitySpin();
        m_keyframesInspectorClipLabel = m_inspectorPane->keyframesInspectorClipLabel();
        m_keyframesInspectorDetailsLabel = m_inspectorPane->keyframesInspectorDetailsLabel();
        m_audioInspectorClipLabel = m_inspectorPane->audioInspectorClipLabel();
        m_audioInspectorDetailsLabel = m_inspectorPane->audioInspectorDetailsLabel();
        m_videoTranslationXSpin = m_inspectorPane->videoTranslationXSpin();
        m_videoTranslationYSpin = m_inspectorPane->videoTranslationYSpin();
        m_videoRotationSpin = m_inspectorPane->videoRotationSpin();
        m_videoScaleXSpin = m_inspectorPane->videoScaleXSpin();
        m_videoScaleYSpin = m_inspectorPane->videoScaleYSpin();
        m_videoKeyframeTable = m_inspectorPane->videoKeyframeTable();
        m_videoInterpolationCombo = m_inspectorPane->videoInterpolationCombo();
        m_mirrorHorizontalCheckBox = m_inspectorPane->mirrorHorizontalCheckBox();
        m_mirrorVerticalCheckBox = m_inspectorPane->mirrorVerticalCheckBox();
        m_outputWidthSpin = m_inspectorPane->outputWidthSpin();
        m_outputHeightSpin = m_inspectorPane->outputHeightSpin();
        m_exportStartSpin = m_inspectorPane->exportStartSpin();
        m_exportEndSpin = m_inspectorPane->exportEndSpin();
        m_outputFormatCombo = m_inspectorPane->outputFormatCombo();
        m_transcriptPrependMsSpin = m_inspectorPane->transcriptPrependMsSpin();
        m_transcriptPostpendMsSpin = m_inspectorPane->transcriptPostpendMsSpin();
        m_speechFilterEnabledCheckBox = m_inspectorPane->speechFilterEnabledCheckBox();

        splitter->setStretchFactor(0, 0);
        splitter->setStretchFactor(1, 1);
        splitter->setStretchFactor(2, 0);
        splitter->setSizes({320, 900, 280});

        setCentralWidget(central);

        connect(&m_playbackTimer, &QTimer::timeout, this, &EditorWindow::advanceFrame);
        m_playbackTimer.setTimerType(Qt::PreciseTimer);
        m_playbackTimer.setInterval(16);
        auto *undoShortcut = new QShortcut(QKeySequence::Undo, this);
        connect(undoShortcut, &QShortcut::activated, this, [this]()
                { undoHistory(); });
        auto *splitShortcut = new QShortcut(QKeySequence(QStringLiteral("Ctrl+B")), this);
        connect(splitShortcut, &QShortcut::activated, this, [this]()
                {
            if (!m_timeline) {
                return;
            }
            if (m_timeline->splitSelectedClipAtFrame(m_timeline->currentFrame())) {
                m_inspectorPane->refresh();
            } });
        auto *deleteShortcut = new QShortcut(QKeySequence::Delete, this);
        connect(deleteShortcut, &QShortcut::activated, this, [this]()
                {
            if (!m_timeline) {
                return;
            }
            if (m_timeline->deleteSelectedClip()) {
                m_inspectorPane->refresh();
            } });
        auto *nudgeLeftShortcut = new QShortcut(QKeySequence(QStringLiteral("Alt+Left")), this);
        connect(nudgeLeftShortcut, &QShortcut::activated, this, [this]()
                {
            if (m_timeline) {
                m_timeline->nudgeSelectedClip(-1);
            } });
        auto *nudgeRightShortcut = new QShortcut(QKeySequence(QStringLiteral("Alt+Right")), this);
        connect(nudgeRightShortcut, &QShortcut::activated, this, [this]()
                {
            if (m_timeline) {
                m_timeline->nudgeSelectedClip(1);
            } });
        auto *playbackShortcut = new QShortcut(QKeySequence(Qt::Key_Space), this);
        connect(playbackShortcut, &QShortcut::activated, this, [this]()
                {
            if (m_timeline) {
                togglePlayback();
            } });
        m_mainThreadHeartbeatTimer.setInterval(100);
        connect(&m_mainThreadHeartbeatTimer, &QTimer::timeout, this, [this]()
                { m_lastMainThreadHeartbeatMs.store(nowMs()); });
        m_lastMainThreadHeartbeatMs.store(nowMs());
        m_mainThreadHeartbeatTimer.start();
        m_stateSaveTimer.setSingleShot(true);
        m_stateSaveTimer.setInterval(250);
        connect(&m_stateSaveTimer, &QTimer::timeout, this, [this]()
                { saveStateNow(); });

        m_fastCurrentFrame.store(0);
        m_fastPlaybackActive.store(false);

        m_controlServer = std::make_unique<ControlServer>(
            this,
            [this]()
            {
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
                    {QStringLiteral("last_playhead_advance_age_ms"), playheadMs > 0 ? now - playheadMs : -1}};
            },
            [this]()
            {
                return this->profilingSnapshot();
            },
            this);
        m_controlServer->start(controlPort);
        qDebug() << "[STARTUP] ControlServer started in" << ctorTimer.elapsed() << "ms";
        m_audioEngine = std::make_unique<AudioEngine>();

        auto connectGradeLive = [this](QDoubleSpinBox *spin)
        {
            connect(spin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double)
                    { applySelectedClipGradeFromInspector(false); });
            connect(spin, &QDoubleSpinBox::editingFinished, this, [this]()
                    { applySelectedClipGradeFromInspector(true); });
        };
        connectGradeLive(m_brightnessSpin);
        connectGradeLive(m_contrastSpin);
        connectGradeLive(m_saturationSpin);
        connectGradeLive(m_opacitySpin);

        auto connectVideoKeyframeLive = [this](QDoubleSpinBox *spin)
        {
            connect(spin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double)
                    { applySelectedVideoKeyframeFromInspector(false); });
            connect(spin, &QDoubleSpinBox::editingFinished, this, [this]()
                    { applySelectedVideoKeyframeFromInspector(true); });
        };
        connectVideoKeyframeLive(m_videoTranslationXSpin);
        connectVideoKeyframeLive(m_videoTranslationYSpin);
        connectVideoKeyframeLive(m_videoRotationSpin);
        connect(m_videoScaleXSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double)
                {
            syncScaleSpinPair(m_videoScaleXSpin, m_videoScaleYSpin);
            applySelectedVideoKeyframeFromInspector(false); });
        connect(m_videoScaleXSpin, &QDoubleSpinBox::editingFinished, this, [this]()
                { applySelectedVideoKeyframeFromInspector(true); });
        connect(m_videoScaleYSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double)
                {
            syncScaleSpinPair(m_videoScaleYSpin, m_videoScaleXSpin);
            applySelectedVideoKeyframeFromInspector(false); });
        connect(m_videoScaleYSpin, &QDoubleSpinBox::editingFinished, this, [this]()
                { applySelectedVideoKeyframeFromInspector(true); });
        connect(m_videoInterpolationCombo, &QComboBox::currentIndexChanged, this, [this](int)
                { applySelectedVideoKeyframeFromInspector(true); });
        connect(m_mirrorHorizontalCheckBox, &QCheckBox::toggled, this, [this](bool)
                { applySelectedVideoKeyframeFromInspector(true); });
        connect(m_mirrorVerticalCheckBox, &QCheckBox::toggled, this, [this](bool)
                { applySelectedVideoKeyframeFromInspector(true); });

        QTimer::singleShot(0, this, [this]()
                           {
            loadProjectsFromFolders();
            refreshProjectsList();
            loadState();
            m_inspectorPane->refresh(); });
    }

    ~EditorWindow() override
    {
        saveStateNow();
    }

protected:
    void closeEvent(QCloseEvent *event) override
    {
        saveStateNow();
        QMainWindow::closeEvent(event);
    }

    bool eventFilter(QObject *watched, QEvent *event) override
    {
        Q_UNUSED(watched)
        Q_UNUSED(event)
        return QMainWindow::eventFilter(watched, event);
    }

private slots:
    void loadState()
    {
        loadProjectsFromFolders();
        m_historyEntries = QJsonArray();
        m_historyIndex = -1;
        m_lastSavedState.clear();
        QJsonObject root;
        QFile historyFile(historyFilePath());
        if (historyFile.open(QIODevice::ReadOnly))
        {
            const QJsonObject historyRoot = QJsonDocument::fromJson(historyFile.readAll()).object();
            m_historyEntries = historyRoot.value(QStringLiteral("entries")).toArray();
            m_historyIndex = historyRoot.value(QStringLiteral("index")).toInt(m_historyEntries.size() - 1);
            if (!m_historyEntries.isEmpty())
            {
                m_historyIndex = qBound(0, m_historyIndex, m_historyEntries.size() - 1);
                root = m_historyEntries.at(m_historyIndex).toObject();
            }
        }

        if (root.isEmpty())
        {
            QFile file(stateFilePath());
            if (file.open(QIODevice::ReadOnly))
            {
                root = QJsonDocument::fromJson(file.readAll()).object();
            }
        }

        applyStateJson(root);
        if (m_historyEntries.isEmpty())
        {
            pushHistorySnapshot();
        }

        if (m_pendingSaveAfterLoad)
        {
            m_pendingSaveAfterLoad = false;
            scheduleSaveState();
        }
        else
        {
            scheduleSaveState();
        }
    }




    void openTranscriptionWindow(const QString &filePath, const QString &label)
    {
        const QFileInfo inputInfo(filePath);
        if (!inputInfo.exists() || !inputInfo.isFile())
        {
            QMessageBox::warning(this,
                                 QStringLiteral("Transcribe Failed"),
                                 QStringLiteral("The selected file does not exist:\n%1")
                                     .arg(QDir::toNativeSeparators(filePath)));
            return;
        }

        const QString scriptPath = QDir(QDir::currentPath()).absoluteFilePath(QStringLiteral("whisperx.sh"));
        if (!QFileInfo::exists(scriptPath))
        {
            QMessageBox::warning(this,
                                 QStringLiteral("Transcribe Failed"),
                                 QStringLiteral("whisperx.sh was not found at:\n%1")
                                     .arg(QDir::toNativeSeparators(scriptPath)));
            return;
        }

        auto *dialog = new QDialog(this);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setWindowTitle(QStringLiteral("Transcribe  %1").arg(label));
        dialog->resize(920, 560);

        auto *layout = new QVBoxLayout(dialog);
        layout->setContentsMargins(12, 12, 12, 12);
        layout->setSpacing(8);

        auto *title = new QLabel(QStringLiteral("whisperx.sh %1").arg(QDir::toNativeSeparators(filePath)), dialog);
        title->setWordWrap(true);
        layout->addWidget(title);

        auto *output = new QPlainTextEdit(dialog);
        output->setReadOnly(true);
        output->setLineWrapMode(QPlainTextEdit::NoWrap);
        output->setStyleSheet(QStringLiteral(
            "QPlainTextEdit { background: #05080c; color: #d6e2ee; border: 1px solid #24303c; "
            "border-radius: 8px; font-family: monospace; font-size: 12px; }"));
        layout->addWidget(output, 1);

        auto *inputRow = new QHBoxLayout;
        inputRow->setContentsMargins(0, 0, 0, 0);
        inputRow->setSpacing(8);
        auto *inputLabel = new QLabel(QStringLiteral("stdin"), dialog);
        auto *inputLine = new QLineEdit(dialog);
        inputLine->setPlaceholderText(QStringLiteral("Type input for whisperx.sh prompts, then press Send"));
        auto *sendButton = new QPushButton(QStringLiteral("Send"), dialog);
        auto *closeButton = new QPushButton(QStringLiteral("Close"), dialog);
        inputRow->addWidget(inputLabel);
        inputRow->addWidget(inputLine, 1);
        inputRow->addWidget(sendButton);
        inputRow->addWidget(closeButton);
        layout->addLayout(inputRow);

        auto *process = new QProcess(dialog);
        process->setProcessChannelMode(QProcess::MergedChannels);
        process->setWorkingDirectory(QDir::currentPath());

        const auto appendOutput = [output](const QString &text)
        {
            if (text.isEmpty())
            {
                return;
            }
            output->moveCursor(QTextCursor::End);
            output->insertPlainText(text);
            output->moveCursor(QTextCursor::End);
        };

        connect(process, &QProcess::readyReadStandardOutput, dialog, [process, appendOutput]()
                { appendOutput(QString::fromLocal8Bit(process->readAllStandardOutput())); });
        connect(process, &QProcess::started, dialog, [appendOutput, filePath]()
                { appendOutput(QStringLiteral("$ ./whisperx.sh \"%1\"\n").arg(QDir::toNativeSeparators(filePath))); });
        connect(process, &QProcess::errorOccurred, dialog, [appendOutput](QProcess::ProcessError error)
                { appendOutput(QStringLiteral("\n[process error] %1\n").arg(static_cast<int>(error))); });
        connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), dialog,
                [appendOutput](int exitCode, QProcess::ExitStatus exitStatus)
                {
                    appendOutput(QStringLiteral("\n[process finished] exitCode=%1 status=%2\n")
                                     .arg(exitCode)
                                     .arg(exitStatus == QProcess::NormalExit ? QStringLiteral("normal")
                                                                             : QStringLiteral("crashed")));
                });
        connect(sendButton, &QPushButton::clicked, dialog, [process, inputLine, appendOutput]()
                {
            const QString text = inputLine->text();
            if (text.isEmpty()) {
                return;
            }
            process->write(text.toUtf8());
            process->write("\n");
            appendOutput(QStringLiteral("> %1\n").arg(text));
            inputLine->clear(); });
        connect(inputLine, &QLineEdit::returnPressed, sendButton, &QPushButton::click);
        connect(closeButton, &QPushButton::clicked, dialog, &QDialog::close);
        connect(dialog, &QDialog::finished, dialog, [process](int)
                {
            if (process->state() != QProcess::NotRunning) {
                process->kill();
                process->waitForFinished(1000);
            } });

        process->start(QStringLiteral("/bin/bash"),
                       {scriptPath, QFileInfo(filePath).absoluteFilePath()});
        dialog->show();
    }

    QString defaultProxyOutputPath(const TimelineClip &clip) const
    {
        const QFileInfo sourceInfo(clip.filePath);
        return sourceInfo.dir().absoluteFilePath(
            QStringLiteral("%1.proxy.mov").arg(sourceInfo.completeBaseName()));
    }

    QString clipFileInfoSummary(const QString &filePath) const
    {
        if (filePath.isEmpty())
        {
            return QStringLiteral("Path: None");
        }

        const QFileInfo info(filePath);
        QStringList lines;
        lines << QStringLiteral("Path: %1").arg(QDir::toNativeSeparators(filePath));
        lines << QStringLiteral("Exists: %1").arg(info.exists() ? QStringLiteral("Yes") : QStringLiteral("No"));
        if (!info.exists())
        {
            return lines.join(QLatin1Char('\n'));
        }

        lines << QStringLiteral("Size: %1 MB").arg(
            QString::number(static_cast<double>(info.size()) / (1024.0 * 1024.0), 'f', 1));
        lines << QStringLiteral("Modified: %1").arg(info.lastModified().toString(Qt::ISODate));

        const MediaProbeResult probe = probeMediaFile(filePath);
        lines << QStringLiteral("Media Type: %1").arg(clipMediaTypeLabel(probe.mediaType));
        lines << QStringLiteral("Duration: %1 frames").arg(probe.durationFrames);
        lines << QStringLiteral("Audio: %1").arg(probe.hasAudio ? QStringLiteral("Yes") : QStringLiteral("No"));
        lines << QStringLiteral("Video: %1").arg(probe.hasVideo ? QStringLiteral("Yes") : QStringLiteral("No"));
        return lines.join(QLatin1Char('\n'));
    }

    void createProxyForClip(const QString &clipId)
    {
        if (!m_timeline)
        {
            return;
        }

        const TimelineClip *clip = nullptr;
        for (const TimelineClip &candidate : m_timeline->clips())
        {
            if (candidate.id == clipId)
            {
                clip = &candidate;
                break;
            }
        }
        if (!clip)
        {
            return;
        }
        if (clip->mediaType != ClipMediaType::Video)
        {
            QMessageBox::information(this,
                                     QStringLiteral("Create Proxy"),
                                     QStringLiteral("Proxy creation is currently available for video clips."));
            return;
        }

        const QString ffmpegPath = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
        if (ffmpegPath.isEmpty())
        {
            QMessageBox::warning(this,
                                 QStringLiteral("Create Proxy Failed"),
                                 QStringLiteral("ffmpeg was not found in PATH."));
            return;
        }

        const QString outputPath = defaultProxyOutputPath(*clip);
        if (QFileInfo::exists(outputPath))
        {
            const auto response = QMessageBox::question(
                this,
                QStringLiteral("Overwrite Proxy"),
                QStringLiteral("A proxy already exists:\n%1\n\nOverwrite it?")
                    .arg(QDir::toNativeSeparators(outputPath)),
                QMessageBox::Yes | QMessageBox::No,
                QMessageBox::No);
            if (response != QMessageBox::Yes)
            {
                return;
            }
        }

        auto *dialog = new QDialog(this);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setWindowTitle(QStringLiteral("Create Proxy  %1").arg(clip->label));
        dialog->resize(920, 560);

        auto *layout = new QVBoxLayout(dialog);
        layout->setContentsMargins(12, 12, 12, 12);
        layout->setSpacing(8);

        auto *title = new QLabel(QStringLiteral("ffmpeg proxy for %1").arg(QDir::toNativeSeparators(clip->filePath)), dialog);
        title->setWordWrap(true);
        layout->addWidget(title);

        auto *output = new QPlainTextEdit(dialog);
        output->setReadOnly(true);
        output->setLineWrapMode(QPlainTextEdit::NoWrap);
        output->setStyleSheet(QStringLiteral(
            "QPlainTextEdit { background: #05080c; color: #d6e2ee; border: 1px solid #24303c; "
            "border-radius: 8px; font-family: monospace; font-size: 12px; }"));
        layout->addWidget(output, 1);

        auto *buttonRow = new QHBoxLayout;
        buttonRow->setContentsMargins(0, 0, 0, 0);
        buttonRow->setSpacing(8);
        auto *closeButton = new QPushButton(QStringLiteral("Close"), dialog);
        buttonRow->addStretch(1);
        buttonRow->addWidget(closeButton);
        layout->addLayout(buttonRow);

        auto *process = new QProcess(dialog);
        process->setProcessChannelMode(QProcess::MergedChannels);
        process->setWorkingDirectory(QDir::currentPath());

        QStringList arguments = {
            QStringLiteral("-y"),
            QStringLiteral("-hide_banner"),
            QStringLiteral("-i"), QFileInfo(clip->filePath).absoluteFilePath(),
            QStringLiteral("-map"), QStringLiteral("0:v:0"),
            QStringLiteral("-map"), QStringLiteral("0:a?")};
        const bool alphaProxy = QFileInfo(clip->filePath).suffix().compare(QStringLiteral("webm"), Qt::CaseInsensitive) == 0;
        if (alphaProxy)
        {
            arguments << QStringLiteral("-c:v") << QStringLiteral("prores_ks")
                      << QStringLiteral("-profile:v") << QStringLiteral("4")
                      << QStringLiteral("-pix_fmt") << QStringLiteral("yuva444p10le")
                      << QStringLiteral("-vendor") << QStringLiteral("apl0")
                      << QStringLiteral("-bits_per_mb") << QStringLiteral("8000");
        }
        else
        {
            arguments << QStringLiteral("-c:v") << QStringLiteral("prores_ks")
                      << QStringLiteral("-profile:v") << QStringLiteral("3")
                      << QStringLiteral("-pix_fmt") << QStringLiteral("yuv422p10le");
        }
        arguments << QStringLiteral("-c:a") << QStringLiteral("pcm_s16le")
                  << QFileInfo(outputPath).absoluteFilePath();

        const auto appendOutput = [output](const QString &text)
        {
            if (text.isEmpty())
            {
                return;
            }
            output->moveCursor(QTextCursor::End);
            output->insertPlainText(text);
            output->moveCursor(QTextCursor::End);
        };

        connect(process, &QProcess::readyReadStandardOutput, dialog, [process, appendOutput]()
                { appendOutput(QString::fromLocal8Bit(process->readAllStandardOutput())); });
        connect(process, &QProcess::started, dialog, [appendOutput, ffmpegPath, arguments]()
                {
                    appendOutput(QStringLiteral("$ %1 %2\n")
                                     .arg(QDir::toNativeSeparators(ffmpegPath),
                                          arguments.join(QLatin1Char(' '))));
                });
        connect(process, &QProcess::errorOccurred, dialog, [appendOutput](QProcess::ProcessError error)
                { appendOutput(QStringLiteral("\n[process error] %1\n").arg(static_cast<int>(error))); });
        connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), dialog,
                [this, clipId, outputPath, appendOutput](int exitCode, QProcess::ExitStatus exitStatus)
                {
                    appendOutput(QStringLiteral("\n[process finished] exitCode=%1 status=%2\n")
                                     .arg(exitCode)
                                     .arg(exitStatus == QProcess::NormalExit ? QStringLiteral("normal")
                                                                             : QStringLiteral("crashed")));
                    if (exitStatus != QProcess::NormalExit || exitCode != 0 || !QFileInfo::exists(outputPath))
                    {
                        return;
                    }
                    if (!m_timeline->updateClipById(clipId, [outputPath](TimelineClip &updatedClip)
                                                   { updatedClip.proxyPath = outputPath; }))
                    {
                        return;
                    }
                    if (m_timeline->clipsChanged)
                    {
                        m_timeline->clipsChanged();
                    }
                });
        connect(closeButton, &QPushButton::clicked, dialog, &QDialog::close);
        connect(dialog, &QDialog::finished, dialog, [process](int)
                {
                    if (process->state() != QProcess::NotRunning)
                    {
                        process->kill();
                        process->waitForFinished(1000);
                    }
                });

        process->start(ffmpegPath, arguments);
        dialog->show();
    }

    QWidget *buildEditorPane()
    {
        auto *pane = new EditorPane;

        m_preview = pane->previewWindow();
        m_timeline = pane->timelineWidget();
        m_playButton = pane->playButton();
        m_seekSlider = pane->seekSlider();
        m_timecodeLabel = pane->timecodeLabel();
        m_audioMuteButton = pane->audioMuteButton();
        m_audioVolumeSlider = pane->audioVolumeSlider();
        m_audioNowPlayingLabel = pane->audioNowPlayingLabel();
        m_statusBadge = pane->statusBadge();
        m_previewInfo = pane->previewInfo();

        connect(pane, &EditorPane::playClicked, this, [this]()
                { togglePlayback(); });
        connect(pane, &EditorPane::startClicked, this, [this]()
                { setCurrentFrame(0); });
        connect(pane, &EditorPane::endClicked, this, [this]()
                { setCurrentFrame(m_timeline ? m_timeline->totalFrames() : 0); });
        connect(pane, &EditorPane::seekValueChanged, this, [this](int value)
                {
            if (m_ignoreSeekSignal) return;
            setCurrentFrame(value); });
        connect(pane, &EditorPane::audioMuteClicked, this, [this]()
                {
            const bool nextMuted = !m_preview->audioMuted();
            m_preview->setAudioMuted(nextMuted);
            if (m_audioEngine) {
                m_audioEngine->setMuted(nextMuted);
            }
            m_inspectorPane->refresh();
            scheduleSaveState(); });
        connect(pane, &EditorPane::audioVolumeChanged, this, [this](int value)
                {
            m_preview->setAudioVolume(value / 100.0);
            if (m_audioEngine) {
                m_audioEngine->setVolume(value / 100.0);
            }
            m_inspectorPane->refresh(); });

        m_timeline->seekRequested = [this](int64_t frame)
        {
            setCurrentFrame(frame);
        };
        m_timeline->clipsChanged = [this]()
        {
            syncSliderRange();
            m_preview->setClipCount(m_timeline->clips().size());
            m_preview->setTimelineClips(m_timeline->clips());
            m_preview->setExportRanges(effectivePlaybackRanges());
            m_preview->setRenderSyncMarkers(m_timeline->renderSyncMarkers());
            m_preview->setSelectedClipId(m_timeline->selectedClipId());
            if (m_audioEngine)
            {
                m_audioEngine->setTimelineClips(m_timeline->clips());
                m_audioEngine->setExportRanges(effectivePlaybackRanges());
            }
            refreshClipInspector();
            m_inspectorPane->refresh();
            scheduleSaveState();
            pushHistorySnapshot();
        };
        m_timeline->selectionChanged = [this]()
        {
            m_preview->setSelectedClipId(m_timeline->selectedClipId());
            refreshSyncInspector();
            refreshTranscriptInspector();
            refreshClipInspector();
            m_inspectorPane->refresh();
        };
        m_timeline->renderSyncMarkersChanged = [this]()
        {
            m_preview->setRenderSyncMarkers(m_timeline->renderSyncMarkers());
            refreshSyncInspector();
            m_inspectorPane->refresh();
            scheduleSaveState();
            pushHistorySnapshot();
        };
        m_timeline->exportRangeChanged = [this]()
        {
            m_inspectorPane->refresh();
            scheduleSaveState();
            pushHistorySnapshot();
        };
        m_timeline->gradingRequested = [this]()
        {
            focusGradingTab();
            m_inspectorPane->refresh();
        };
        m_timeline->transcribeRequested = [this](const QString &filePath, const QString &label)
        {
            openTranscriptionWindow(filePath, label);
        };
        m_timeline->createProxyRequested = [this](const QString &clipId)
        {
            createProxyForClip(clipId);
        };
        m_preview->selectionRequested = [this](const QString &clipId)
        {
            if (!m_timeline)
            {
                return;
            }
            m_timeline->setSelectedClipId(clipId);
        };
        m_preview->resizeRequested = [this](const QString &clipId, qreal scaleX, qreal scaleY, bool finalize)
        {
            if (!m_timeline)
            {
                return;
            }
            const int64_t currentFrame = m_timeline->currentFrame();
            const bool updated = m_timeline->updateClipById(clipId, [this, currentFrame, scaleX, scaleY](TimelineClip &clip)
                                                            {
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
                normalizeClipTransformKeyframes(clip); });
            if (!updated)
            {
                return;
            }
            m_preview->setTimelineClips(m_timeline->clips());
            m_preview->setRenderSyncMarkers(m_timeline->renderSyncMarkers());
            m_inspectorPane->refresh();
            scheduleSaveState();
            if (finalize)
            {
                pushHistorySnapshot();
            }
        };
        m_preview->moveRequested = [this](const QString &clipId, qreal translationX, qreal translationY, bool finalize)
        {
            if (!m_timeline)
            {
                return;
            }
            const int64_t currentFrame = m_timeline->currentFrame();
            const bool updated = m_timeline->updateClipById(clipId, [this, currentFrame, translationX, translationY](TimelineClip &clip)
                                                            {
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
                normalizeClipTransformKeyframes(clip); });
            if (!updated)
            {
                return;
            }
            m_preview->setTimelineClips(m_timeline->clips());
            m_preview->setRenderSyncMarkers(m_timeline->renderSyncMarkers());
            m_inspectorPane->refresh();
            scheduleSaveState();
            if (finalize)
            {
                pushHistorySnapshot();
            }
        };
        return pane;
    }

    void addFileToTimeline(const QString &filePath)
    {
        if (m_timeline)
        {
            m_timeline->addClipFromFile(filePath);
            m_preview->setTimelineClips(m_timeline->clips());
        }
    }

    void syncSliderRange()
    {
        const int64_t maxFrame = m_timeline->totalFrames();
        m_seekSlider->setRange(0, static_cast<int>(qMin<int64_t>(maxFrame, INT_MAX)));
    }

    void focusGradingTab()
    {
        if (m_inspectorTabs)
        {
            m_inspectorTabs->setCurrentIndex(0);
        }
    }

    bool showClipRelativeKeyframes() const
    {
        return !m_keyframeSpaceCheckBox || m_keyframeSpaceCheckBox->isChecked();
    }

    int64_t keyframeFrameForInspectorDisplay(const TimelineClip &clip, int64_t storedFrame) const
    {
        return showClipRelativeKeyframes() ? storedFrame : (clip.startFrame + storedFrame);
    }

    int64_t keyframeFrameFromInspectorDisplay(const TimelineClip &clip, int64_t displayedFrame) const
    {
        return showClipRelativeKeyframes() ? displayedFrame : (displayedFrame - clip.startFrame);
    }

    qreal inspectorScaleWithMirror(qreal magnitude, bool mirrored) const
    {
        const qreal sanitized = sanitizeScaleValue(std::abs(magnitude));
        return mirrored ? -sanitized : sanitized;
    }

    void syncScaleSpinPair(QDoubleSpinBox *changedSpin, QDoubleSpinBox *otherSpin)
    {
        if (m_syncingScaleControls || !m_lockVideoScaleCheckBox || !m_lockVideoScaleCheckBox->isChecked() ||
            !changedSpin || !otherSpin)
        {
            return;
        }
        m_syncingScaleControls = true;
        otherSpin->setValue(std::abs(changedSpin->value()));
        m_syncingScaleControls = false;
    }

    TimelineClip::TransformKeyframe keyframeForInspectorDisplay(const TimelineClip &clip,
                                                                const TimelineClip::TransformKeyframe &keyframe) const
    {
        if (showClipRelativeKeyframes())
        {
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

    TimelineClip::TransformKeyframe keyframeFromInspectorDisplay(const TimelineClip &clip,
                                                                 const TimelineClip::TransformKeyframe &displayed) const
    {
        if (showClipRelativeKeyframes())
        {
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

    int selectedVideoKeyframeIndex(const TimelineClip &clip) const
    {
        for (int i = 0; i < clip.transformKeyframes.size(); ++i)
        {
            if (clip.transformKeyframes[i].frame == m_selectedVideoKeyframeFrame)
            {
                return i;
            }
        }
        return -1;
    }

    QList<int64_t> selectedVideoKeyframeFrames(const TimelineClip &clip) const
    {
        QList<int64_t> frames;
        for (const TimelineClip::TransformKeyframe &keyframe : clip.transformKeyframes)
        {
            if (m_selectedVideoKeyframeFrames.contains(keyframe.frame))
            {
                frames.push_back(keyframe.frame);
            }
        }
        return frames;
    }

    bool hasRemovableVideoKeyframeSelection(const TimelineClip &clip) const
    {
        for (const int64_t frame : selectedVideoKeyframeFrames(clip))
        {
            if (frame > 0)
            {
                return true;
            }
        }
        return false;
    }

    int nearestVideoKeyframeIndex(const TimelineClip &clip) const
    {
        if (!m_timeline || clip.transformKeyframes.isEmpty())
        {
            return -1;
        }
        const int64_t localFrame =
            qBound<int64_t>(0, m_timeline->currentFrame() - clip.startFrame, qMax<int64_t>(0, clip.durationFrames - 1));
        int nearestIndex = 0;
        int64_t nearestDistance = std::numeric_limits<int64_t>::max();
        for (int i = 0; i < clip.transformKeyframes.size(); ++i)
        {
            const int64_t distance = std::abs(clip.transformKeyframes[i].frame - localFrame);
            if (distance < nearestDistance)
            {
                nearestDistance = distance;
                nearestIndex = i;
            }
        }
        return nearestIndex;
    }

    void applySelectedVideoKeyframeFromInspector(bool pushHistoryAfterChange = false)
    {
        if (m_updatingVideoInspector || !m_timeline)
        {
            return;
        }

        const QString clipId = m_timeline->selectedClipId();
        if (clipId.isEmpty() || m_selectedVideoKeyframeFrame < 0)
        {
            return;
        }

        const bool updated = m_timeline->updateClipById(clipId, [this](TimelineClip &clip)
                                                        {
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
            normalizeClipTransformKeyframes(clip); });

        if (!updated)
        {
            return;
        }

        m_preview->setTimelineClips(m_timeline->clips());
        m_inspectorPane->refresh();
        scheduleSaveState();
        if (pushHistoryAfterChange)
        {
            pushHistorySnapshot();
        }
    }

    void upsertSelectedClipVideoKeyframe()
    {
        if (!m_timeline)
        {
            return;
        }
        const TimelineClip *clip = m_timeline->selectedClip();
        if (!clip || !clipHasVisuals(*clip))
        {
            return;
        }

        const int64_t keyframeFrame =
            qBound<int64_t>(0, m_timeline->currentFrame() - clip->startFrame, qMax<int64_t>(0, clip->durationFrames - 1));
        m_selectedVideoKeyframeFrame = keyframeFrame;
        m_selectedVideoKeyframeFrames = {keyframeFrame};

        const bool updated = m_timeline->updateClipById(clip->id, [this, keyframeFrame](TimelineClip &editableClip)
                                                        {
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
            normalizeClipTransformKeyframes(editableClip); });

        if (!updated)
        {
            return;
        }

        m_preview->setTimelineClips(m_timeline->clips());
        m_inspectorPane->refresh();
        scheduleSaveState();
        pushHistorySnapshot();
    }

    void duplicateSelectedClipVideoKeyframes(int frameDelta)
    {
        if (!m_timeline || frameDelta == 0)
        {
            return;
        }
        const TimelineClip *selectedClip = m_timeline->selectedClip();
        if (!selectedClip || !clipHasVisuals(*selectedClip))
        {
            return;
        }

        const QList<int64_t> selectedFrames = selectedVideoKeyframeFrames(*selectedClip);
        if (selectedFrames.isEmpty())
        {
            return;
        }

        QSet<int64_t> sourceFrames;
        for (int64_t frame : selectedFrames)
        {
            sourceFrames.insert(frame);
        }
        const int64_t maxFrame = qMax<int64_t>(0, selectedClip->durationFrames - 1);
        QSet<int64_t> newFrames;
        const bool updated = m_timeline->updateClipById(selectedClip->id, [frameDelta, maxFrame, sourceFrames, &newFrames](TimelineClip &clip)
                                                        {
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
            } });

        if (!updated || newFrames.isEmpty())
        {
            return;
        }

        m_selectedVideoKeyframeFrames = newFrames;
        m_selectedVideoKeyframeFrame = *std::min_element(newFrames.begin(), newFrames.end());
        m_preview->setTimelineClips(m_timeline->clips());
        m_inspectorPane->refresh();
        scheduleSaveState();
        pushHistorySnapshot();
    }

    void removeSelectedClipVideoKeyframe()
    {
        if (!m_timeline || m_selectedVideoKeyframeFrames.isEmpty())
        {
            return;
        }
        const QString clipId = m_timeline->selectedClipId();
        if (clipId.isEmpty())
        {
            return;
        }

        const TimelineClip *selectedClip = m_timeline->selectedClip();
        if (!selectedClip)
        {
            return;
        }
        QList<int64_t> selectedFrames = selectedVideoKeyframeFrames(*selectedClip);
        selectedFrames.erase(std::remove_if(selectedFrames.begin(),
                                            selectedFrames.end(),
                                            [](int64_t frame)
                                            { return frame <= 0; }),
                             selectedFrames.end());
        if (selectedFrames.isEmpty())
        {
            m_inspectorPane->refresh();
            return;
        }

        const bool updated = m_timeline->updateClipById(clipId, [selectedFrames](TimelineClip &clip)
                                                        {
            clip.transformKeyframes.erase(
                std::remove_if(clip.transformKeyframes.begin(),
                               clip.transformKeyframes.end(),
                               [&selectedFrames](const TimelineClip::TransformKeyframe& keyframe) {
                                   return selectedFrames.contains(keyframe.frame);
                               }),
                clip.transformKeyframes.end());
            normalizeClipTransformKeyframes(clip); });

        if (!updated)
        {
            return;
        }

        m_selectedVideoKeyframeFrame = 0;
        m_selectedVideoKeyframeFrames = {0};
        m_preview->setTimelineClips(m_timeline->clips());
        m_inspectorPane->refresh();
        scheduleSaveState();
        pushHistorySnapshot();
    }

    void applySelectedClipGradeFromInspector(bool pushHistoryAfterChange = false)
    {
        if (!m_timeline)
        {
            return;
        }
        const QString clipId = m_timeline->selectedClipId();
        if (clipId.isEmpty())
        {
            return;
        }

        const bool updated = m_timeline->updateClipById(clipId, [this](TimelineClip &clip)
                                                        {
            clip.brightness = m_brightnessSpin->value();
            clip.contrast = m_contrastSpin->value();
            clip.saturation = m_saturationSpin->value();
            clip.opacity = m_opacitySpin->value(); });

        if (!updated)
        {
            return;
        }

        m_preview->setTimelineClips(m_timeline->clips());
        m_inspectorPane->refresh();
        scheduleSaveState();
        if (pushHistoryAfterChange)
        {
            pushHistorySnapshot();
        }
    }

    void applySelectedTranscriptOverlayFromInspector(bool pushHistoryAfterChange = false)
    {
        if (!m_timeline || m_updatingTranscriptInspector)
        {
            return;
        }
        const QString clipId = m_timeline->selectedClipId();
        if (clipId.isEmpty())
        {
            return;
        }

        const bool updated = m_timeline->updateClipById(clipId, [this](TimelineClip &clip)
                                                        {
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
                m_transcriptItalicCheckBox && m_transcriptItalicCheckBox->isChecked(); });
        if (!updated)
        {
            return;
        }

        if (m_preview)
        {
            m_preview->setTimelineClips(m_timeline->clips());
        }
        m_inspectorPane->refresh();
        scheduleSaveState();
        if (pushHistoryAfterChange)
        {
            pushHistorySnapshot();
        }
    }

    void setCurrentFrame(int64_t frame, bool syncAudio = true)
    {
        setCurrentPlaybackSample(frameToSamples(frame), syncAudio);
    }

    void setPlaybackActive(bool playing)
    {
        if (playing)
        {
            // Update export ranges on audio engine and cache for speech filter awareness
            const auto ranges = effectivePlaybackRanges();
            if (m_audioEngine)
            {
                m_audioEngine->setExportRanges(ranges);
            }
            if (m_audioEngine && m_audioEngine->hasPlayableAudio())
            {
                m_audioEngine->start(m_timeline->currentFrame());
            }
            if (m_preview)
            {
                m_preview->setExportRanges(ranges);
            }
            advanceFrame();
            m_playbackTimer.start();
            m_fastPlaybackActive.store(true);
            m_preview->setPlaybackState(true);
        }
        else
        {
            if (m_audioEngine)
            {
                m_audioEngine->stop();
            }
            m_playbackTimer.stop();
            m_fastPlaybackActive.store(false);
            m_preview->setPlaybackState(false);
        }
        m_inspectorPane->refresh();
        scheduleSaveState();
    }

    void togglePlayback()
    {
        setPlaybackActive(!m_playbackTimer.isActive());
    }

    void setCurrentPlaybackSample(int64_t samplePosition, bool syncAudio = true, bool duringPlayback = false)
    {
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
        if (!m_timeline || bounded != m_timeline->currentFrame())
        {
            m_lastPlayheadAdvanceMs.store(nowMs());
        }
        m_currentPlaybackSample = boundedSample;
        m_fastCurrentFrame.store(bounded);
        if (syncAudio && m_audioEngine && m_audioEngine->hasPlayableAudio())
        {
            m_audioEngine->seek(bounded);
        }
        m_timeline->setCurrentFrame(bounded);
        m_preview->setCurrentPlaybackSample(boundedSample);

        m_ignoreSeekSignal = true;
        m_seekSlider->setValue(static_cast<int>(qMin<int64_t>(bounded, INT_MAX)));
        m_ignoreSeekSignal = false;

        m_timecodeLabel->setText(frameToTimecode(bounded));

        // During playback, only update cheap transport labels and sync transcript table
        // instead of doing a full m_inspectorPane->refresh() which rebuilds all tables
        if (duringPlayback)
        {
            updateTransportLabels();
            syncTranscriptTableToPlayhead();
        }
        else
        {
            m_inspectorPane->refresh();
        }
        scheduleSaveState();
    }

    void syncTranscriptTableToPlayhead()
    {
        if (!m_timeline || !m_transcriptTable || m_updatingTranscriptInspector)
        {
            return;
        }

        // Skip all table updates when "Follow current word" is disabled
        if (!m_transcriptFollowCurrentWordCheckBox || !m_transcriptFollowCurrentWordCheckBox->isChecked())
        {
            return;
        }

        const TimelineClip *clip = m_timeline->selectedClip();
        if (!clip || clip->mediaType != ClipMediaType::Audio || m_loadedTranscriptPath.isEmpty())
        {
            m_transcriptTable->clearSelection();
            return;
        }

        const int64_t sourceFrame =
            sourceFrameForClipAtTimelinePosition(*clip,
                                                 samplesToFramePosition(m_currentPlaybackSample),
                                                 {});
        int matchingRow = -1;
        for (int row = 0; row < m_transcriptTable->rowCount(); ++row)
        {
            QTableWidgetItem *startItem = m_transcriptTable->item(row, 0);
            if (!startItem)
            {
                continue;
            }
            const int64_t startFrame = startItem->data(Qt::UserRole + 2).toLongLong();
            const int64_t endFrame = startItem->data(Qt::UserRole + 3).toLongLong();
            if (sourceFrame >= startFrame && sourceFrame <= endFrame)
            {
                matchingRow = row;
                break;
            }
        }

        if (matchingRow < 0)
        {
            m_transcriptTable->clearSelection();
            return;
        }

        if (!m_transcriptTable->selectionModel()->isRowSelected(matchingRow, QModelIndex()))
        {
            m_transcriptTable->setCurrentCell(matchingRow, 0);
            m_transcriptTable->selectRow(matchingRow);
        }
        // Scroll to the matching word
        if (QTableWidgetItem *item = m_transcriptTable->item(matchingRow, 0))
        {
            m_transcriptTable->scrollToItem(item, QAbstractItemView::PositionAtCenter);
        }
    }

    QString frameToTimecode(int64_t frame) const
    {
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
    void updateTransportLabels()
    {
        const bool playing = m_playbackTimer.isActive();
        const QString state = playing ? QStringLiteral("PLAYING") : QStringLiteral("PAUSED");
        const int clipCount = m_timeline ? m_timeline->clips().size() : 0;
        const QString activeAudio = m_preview ? m_preview->activeAudioClipLabel() : QString();

        m_statusBadge->setText(QStringLiteral("%1  |  %2 clips").arg(state).arg(clipCount));
        if (m_preview && m_timeline)
        {
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

    QJsonObject profilingSnapshot() const
    {
        const qint64 now = nowMs();
        QJsonObject snapshot{
            {QStringLiteral("playback_active"), m_playbackTimer.isActive()},
            {QStringLiteral("timeline_clip_count"), m_timeline ? m_timeline->clips().size() : 0},
            {QStringLiteral("current_frame"), m_timeline ? static_cast<qint64>(m_timeline->currentFrame()) : 0},
            {QStringLiteral("explorer_root"), m_explorerPane ? m_explorerPane->currentRootPath() : QString()},
            {QStringLiteral("debug"), debugControlsSnapshot()},
            {QStringLiteral("main_thread_heartbeat_ms"), m_lastMainThreadHeartbeatMs.load()},
            {QStringLiteral("last_playhead_advance_ms"), m_lastPlayheadAdvanceMs.load()},
            {QStringLiteral("main_thread_heartbeat_age_ms"), m_lastMainThreadHeartbeatMs.load() > 0 ? now - m_lastMainThreadHeartbeatMs.load() : -1},
            {QStringLiteral("last_playhead_advance_age_ms"), m_lastPlayheadAdvanceMs.load() > 0 ? now - m_lastPlayheadAdvanceMs.load() : -1}};

        if (m_preview)
        {
            snapshot[QStringLiteral("preview")] = m_preview->profilingSnapshot();
        }

        return snapshot;
    }


    // Project path helpers
    QString projectsDirPath() const
    {
        return QDir(QDir::currentPath()).filePath(QStringLiteral("projects"));
    }

    QString currentProjectMarkerPath() const
    {
        return QDir(projectsDirPath()).filePath(QStringLiteral(".current_project"));
    }

    QString currentProjectIdOrDefault() const
    {
        return m_currentProjectId.isEmpty() ? QStringLiteral("default") : m_currentProjectId;
    }

    QString projectPath(const QString &projectId) const
    {
        return QDir(projectsDirPath()).filePath(projectId.isEmpty() ? QStringLiteral("default") : projectId);
    }

    QString stateFilePathForProject(const QString &projectId) const
    {
        return QDir(projectPath(projectId)).filePath(QStringLiteral("state.json"));
    }

    QString historyFilePathForProject(const QString &projectId) const
    {
        return QDir(projectPath(projectId)).filePath(QStringLiteral("history.json"));
    }

    QString stateFilePath() const
    {
        return stateFilePathForProject(currentProjectIdOrDefault());
    }

    QString historyFilePath() const
    {
        return historyFilePathForProject(currentProjectIdOrDefault());
    }

    QString sanitizedProjectId(const QString &name) const
    {
        QString id = name.trimmed().toLower();
        for (QChar &ch : id)
        {
            if (!(ch.isLetterOrNumber() || ch == QLatin1Char('_') || ch == QLatin1Char('-')))
            {
                ch = QLatin1Char('-');
            }
        }
        while (id.contains(QStringLiteral("--")))
        {
            id.replace(QStringLiteral("--"), QStringLiteral("-"));
        }
        id.remove(QRegularExpression(QStringLiteral("^-+|-+$")));
        if (id.isEmpty())
        {
            id = QStringLiteral("project");
        }
        QString uniqueId = id;
        int suffix = 2;
        while (QFileInfo::exists(projectPath(uniqueId)))
        {
            uniqueId = QStringLiteral("%1-%2").arg(id).arg(suffix++);
        }
        return uniqueId;
    }

    void ensureProjectsDirectory() const
    {
        QDir().mkpath(projectsDirPath());
    }

    QStringList availableProjectIds() const
    {
        ensureProjectsDirectory();
        const QFileInfoList entries = QDir(projectsDirPath()).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::IgnoreCase);
        QStringList ids;
        ids.reserve(entries.size());
        for (const QFileInfo &entry : entries)
        {
            ids.push_back(entry.fileName());
        }
        return ids;
    }

    void ensureDefaultProjectExists() const
    {
        ensureProjectsDirectory();
        QDir().mkpath(projectPath(QStringLiteral("default")));
    }

    void loadProjectsFromFolders()
    {
        ensureDefaultProjectExists();
        QFile markerFile(currentProjectMarkerPath());
        if (markerFile.open(QIODevice::ReadOnly))
        {
            m_currentProjectId = QString::fromUtf8(markerFile.readAll()).trimmed();
        }
        const QStringList projectIds = availableProjectIds();
        if (projectIds.isEmpty())
        {
            m_currentProjectId = QStringLiteral("default");
            return;
        }
        if (m_currentProjectId.isEmpty() || !projectIds.contains(m_currentProjectId))
        {
            m_currentProjectId = projectIds.contains(QStringLiteral("default"))
                                     ? QStringLiteral("default")
                                     : projectIds.constFirst();
        }
        QSaveFile marker(currentProjectMarkerPath());
        if (marker.open(QIODevice::WriteOnly | QIODevice::Truncate))
        {
            const QByteArray payload = m_currentProjectId.toUtf8();
            if (marker.write(payload) == payload.size())
            {
                marker.commit();
            }
            else
            {
                marker.cancelWriting();
            }
        }
    }

    void saveCurrentProjectMarker()
    {
        ensureProjectsDirectory();
        QSaveFile file(currentProjectMarkerPath());
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        {
            return;
        }
        const QByteArray payload = currentProjectIdOrDefault().toUtf8();
        if (file.write(payload) != payload.size())
        {
            file.cancelWriting();
            return;
        }
        file.commit();
    }

    QString currentProjectName() const
    {
        return currentProjectIdOrDefault();
    }

    void refreshProjectsList()
    {
        if (!m_projectsList)
        {
            return;
        }
        loadProjectsFromFolders();
        m_updatingProjectsList = true;
        m_projectsList->clear();
        const QStringList projectIds = availableProjectIds();
        for (const QString &projectId : projectIds)
        {
            auto *item = new QListWidgetItem(projectId, m_projectsList);
            item->setData(Qt::UserRole, projectId);
            item->setToolTip(QDir::toNativeSeparators(projectPath(projectId)));
            if (projectId == currentProjectIdOrDefault())
            {
                item->setSelected(true);
            }
        }
        if (m_projectSectionLabel)
        {
            m_projectSectionLabel->setText(QStringLiteral("PROJECTS  %1").arg(currentProjectName()));
        }
        m_updatingProjectsList = false;
    }

    void switchToProject(const QString &projectId)
    {
        if (projectId.isEmpty() || projectId == currentProjectIdOrDefault())
        {
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
        m_inspectorPane->refresh();
    }

    void createProject()
    {
        bool accepted = false;
        const QString name = QInputDialog::getText(this,
                                                   QStringLiteral("New Project"),
                                                   QStringLiteral("Project name"),
                                                   QLineEdit::Normal,
                                                   QStringLiteral("Untitled Project"),
                                                   &accepted)
                                 .trimmed();
        if (!accepted || name.isEmpty())
        {
            return;
        }
        const QString projectId = sanitizedProjectId(name);
        QDir().mkpath(projectPath(projectId));
        switchToProject(projectId);
    }

    bool saveProjectPayload(const QString &projectId,
                                const QByteArray &statePayload,
                                const QByteArray &historyPayload)
    {
        ensureProjectsDirectory();
        QDir().mkpath(projectPath(projectId));

        QSaveFile stateFile(stateFilePathForProject(projectId));
        if (!stateFile.open(QIODevice::WriteOnly | QIODevice::Truncate))
        {
            return false;
        }
        if (stateFile.write(statePayload) != statePayload.size())
        {
            stateFile.cancelWriting();
            return false;
        }
        if (!stateFile.commit())
        {
            return false;
        }

        QSaveFile historyFile(historyFilePathForProject(projectId));
        if (!historyFile.open(QIODevice::WriteOnly | QIODevice::Truncate))
        {
            return false;
        }
        if (historyFile.write(historyPayload) != historyPayload.size())
        {
            historyFile.cancelWriting();
            return false;
        }
        return historyFile.commit();
    }

    void saveProjectAs()
    {
        if (!m_timeline)
        {
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
                                                   &accepted)
                                 .trimmed();
        if (!accepted || name.isEmpty())
        {
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

        if (!saveProjectPayload(newProjectId, statePayload, historyPayload))
        {
            QMessageBox::warning(this,
                                 QStringLiteral("Save Project As Failed"),
                                 QStringLiteral("Could not write the new project files."));
            return;
        }

        switchToProject(newProjectId);
    }

    void renameProject(const QString &projectId)
    {
        if (projectId.isEmpty() || !QFileInfo::exists(projectPath(projectId)))
        {
            return;
        }
        bool accepted = false;
        const QString name = QInputDialog::getText(this,
                                                   QStringLiteral("Rename Project"),
                                                   QStringLiteral("Project name"),
                                                   QLineEdit::Normal,
                                                   projectId,
                                                   &accepted)
                                 .trimmed();
        if (!accepted || name.isEmpty())
        {
            return;
        }
        const QString renamedProjectId = sanitizedProjectId(name);
        if (renamedProjectId == projectId)
        {
            return;
        }
        QDir projectsDir(projectsDirPath());
        if (!projectsDir.rename(projectId, renamedProjectId))
        {
            QMessageBox::warning(this,
                                 QStringLiteral("Rename Project Failed"),
                                 QStringLiteral("Could not rename the project folder."));
            return;
        }
        if (m_currentProjectId == projectId)
        {
            m_currentProjectId = renamedProjectId;
            saveCurrentProjectMarker();
        }
        refreshProjectsList();
    }

    QJsonObject buildStateJson() const
    {
        QJsonObject root;
        root[QStringLiteral("explorerRoot")] = m_explorerPane ? m_explorerPane->currentRootPath() : QString();
        root[QStringLiteral("explorerGalleryPath")] = m_explorerPane ? m_explorerPane->galleryPath() : QString();
        root[QStringLiteral("currentFrame")] = static_cast<qint64>(m_timeline ? m_timeline->currentFrame() : 0);
        root[QStringLiteral("playing")] = m_playbackTimer.isActive();
        root[QStringLiteral("selectedClipId")] = m_timeline ? m_timeline->selectedClipId() : QString();
        QJsonArray expandedFolders;
        if (m_explorerPane) {
            for (const QString &path : m_explorerPane->currentExpandedExplorerPaths()) {
                expandedFolders.push_back(path);
            }
        }
        root[QStringLiteral("explorerExpandedFolders")] = expandedFolders;
        root[QStringLiteral("outputWidth")] = m_outputWidthSpin ? m_outputWidthSpin->value() : 1080;
        root[QStringLiteral("outputHeight")] = m_outputHeightSpin ? m_outputHeightSpin->value() : 1920;
        root[QStringLiteral("outputFormat")] =
            m_outputFormatCombo ? m_outputFormatCombo->currentData().toString() : QStringLiteral("mp4");
        root[QStringLiteral("speechFilterEnabled")] =
            m_speechFilterEnabledCheckBox ? m_speechFilterEnabledCheckBox->isChecked() : false;
        root[QStringLiteral("transcriptPrependMs")] = m_transcriptPrependMs;
        root[QStringLiteral("transcriptPostpendMs")] = m_transcriptPostpendMs;
        root[QStringLiteral("selectedInspectorTab")] =
            m_inspectorTabs ? m_inspectorTabs->currentIndex() : 0;
        root[QStringLiteral("timelineZoom")] = m_timeline ? m_timeline->timelineZoom() : 4.0;
        root[QStringLiteral("timelineVerticalScroll")] =
            m_timeline ? m_timeline->verticalScrollOffset() : 0;
        root[QStringLiteral("exportStartFrame")] = m_timeline ? static_cast<qint64>(m_timeline->exportStartFrame()) : 0;
        root[QStringLiteral("exportEndFrame")] =
            m_timeline ? static_cast<qint64>(m_timeline->exportEndFrame()) : 300;
        QJsonArray exportRanges;
        if (m_timeline)
        {
            for (const ExportRangeSegment &range : m_timeline->exportRanges())
            {
                QJsonObject rangeObj;
                rangeObj[QStringLiteral("startFrame")] = static_cast<qint64>(range.startFrame);
                rangeObj[QStringLiteral("endFrame")] = static_cast<qint64>(range.endFrame);
                exportRanges.push_back(rangeObj);
            }
        }
        root[QStringLiteral("exportRanges")] = exportRanges;

        QJsonArray timeline;
        if (m_timeline)
        {
            for (const TimelineClip &clip : m_timeline->clips())
            {
                timeline.push_back(clipToJson(clip));
            }
        }
        root[QStringLiteral("timeline")] = timeline;
        QJsonArray renderSyncMarkers;
        if (m_timeline)
        {
            for (const RenderSyncMarker &marker : m_timeline->renderSyncMarkers())
            {
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
        if (m_timeline)
        {
            for (const TimelineTrack &track : m_timeline->tracks())
            {
                QJsonObject trackObj;
                trackObj[QStringLiteral("name")] = track.name;
                trackObj[QStringLiteral("height")] = track.height;
                tracks.push_back(trackObj);
            }
        }
        root[QStringLiteral("tracks")] = tracks;
        return root;
    }

    void scheduleSaveState()
    {
        if (m_loadingState || !m_timeline)
        {
            return;
        }
        m_stateSaveTimer.start();
    }

    void saveStateNow()
    {
        if (!m_timeline)
        {
            return;
        }
        if (m_loadingState)
        {
            m_pendingSaveAfterLoad = true;
            return;
        }

        m_stateSaveTimer.stop();
        ensureProjectsDirectory();
        QDir().mkpath(projectPath(currentProjectIdOrDefault()));

        const QByteArray serializedState =
            QJsonDocument(buildStateJson()).toJson(QJsonDocument::Indented);
        if (serializedState == m_lastSavedState)
        {
            return;
        }

        QSaveFile file(stateFilePath());
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        {
            return;
        }

        if (file.write(serializedState) != serializedState.size())
        {
            file.cancelWriting();
            return;
        }

        if (!file.commit())
        {
            return;
        }

        m_lastSavedState = serializedState;
    }

    void saveHistoryNow()
    {
        ensureProjectsDirectory();
        QDir().mkpath(projectPath(currentProjectIdOrDefault()));
        QJsonObject root;
        root[QStringLiteral("index")] = m_historyIndex;
        root[QStringLiteral("entries")] = m_historyEntries;

        QSaveFile file(historyFilePath());
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        {
            return;
        }

        const QByteArray payload = QJsonDocument(root).toJson(QJsonDocument::Indented);
        if (file.write(payload) != payload.size())
        {
            file.cancelWriting();
            return;
        }
        file.commit();
    }

    void pushHistorySnapshot()
    {
        if (m_loadingState || m_restoringHistory || !m_timeline)
        {
            return;
        }

        const QJsonObject snapshot = buildStateJson();
        if (m_historyIndex >= 0 && m_historyIndex < m_historyEntries.size() &&
            m_historyEntries.at(m_historyIndex).toObject() == snapshot)
        {
            return;
        }

        while (m_historyEntries.size() > m_historyIndex + 1)
        {
            m_historyEntries.removeAt(m_historyEntries.size() - 1);
        }
        m_historyEntries.append(snapshot);
        if (m_historyEntries.size() > 200)
        {
            m_historyEntries.removeAt(0);
        }
        m_historyIndex = m_historyEntries.size() - 1;
        saveHistoryNow();
    }

    void undoHistory()
    {
        if (m_historyIndex <= 0 || m_historyEntries.isEmpty())
        {
            return;
        }

        m_restoringHistory = true;
        m_historyIndex -= 1;
        applyStateJson(m_historyEntries.at(m_historyIndex).toObject());
        m_restoringHistory = false;
        saveHistoryNow();
        scheduleSaveState();
    }

    void applyStateJson(const QJsonObject &root)
    {
        m_loadingState = true;

        QString rootPath = root.value(QStringLiteral("explorerRoot")).toString(QDir::currentPath());
        QString galleryFolderPath = root.value(QStringLiteral("explorerGalleryPath")).toString();
        const int outputWidth = qMax(16, root.value(QStringLiteral("outputWidth")).toInt(1080));
        const int outputHeight = qMax(16, root.value(QStringLiteral("outputHeight")).toInt(1920));
        const QString outputFormat = root.value(QStringLiteral("outputFormat")).toString(QStringLiteral("mp4"));
        const bool speechFilterEnabled =
            root.value(QStringLiteral("speechFilterEnabled")).toBool(false);
        const int transcriptPrependMs = root.value(QStringLiteral("transcriptPrependMs")).toInt(0);
        const int transcriptPostpendMs = root.value(QStringLiteral("transcriptPostpendMs")).toInt(0);
        const int selectedInspectorTab = root.value(QStringLiteral("selectedInspectorTab")).toInt(0);
        const qreal timelineZoom = root.value(QStringLiteral("timelineZoom")).toDouble(4.0);
        const int timelineVerticalScroll = root.value(QStringLiteral("timelineVerticalScroll")).toInt(0);
        const int64_t exportStartFrame = root.value(QStringLiteral("exportStartFrame")).toVariant().toLongLong();
        const int64_t exportEndFrame = root.value(QStringLiteral("exportEndFrame")).toVariant().toLongLong();
        QVector<ExportRangeSegment> loadedExportRanges;
        const QJsonArray exportRanges = root.value(QStringLiteral("exportRanges")).toArray();
        loadedExportRanges.reserve(exportRanges.size());
        for (const QJsonValue &value : exportRanges)
        {
            if (!value.isObject())
            {
                continue;
            }
            const QJsonObject obj = value.toObject();
            ExportRangeSegment range;
            range.startFrame = qMax<int64_t>(0, obj.value(QStringLiteral("startFrame")).toVariant().toLongLong());
            range.endFrame = qMax<int64_t>(0, obj.value(QStringLiteral("endFrame")).toVariant().toLongLong());
            loadedExportRanges.push_back(range);
        }
        QStringList expandedExplorerPaths;
        for (const QJsonValue &value : root.value(QStringLiteral("explorerExpandedFolders")).toArray())
        {
            const QString path = value.toString();
            if (!path.isEmpty())
            {
                expandedExplorerPaths.push_back(path);
            }
        }
        QVector<TimelineClip> loadedClips;
        QVector<RenderSyncMarker> loadedRenderSyncMarkers;
        const int64_t currentFrame = root.value(QStringLiteral("currentFrame")).toVariant().toLongLong();
        const QString selectedClipId = root.value(QStringLiteral("selectedClipId")).toString();
        QVector<TimelineTrack> loadedTracks;

        const QJsonArray clips = root.value(QStringLiteral("timeline")).toArray();
        loadedClips.reserve(clips.size());
        for (const QJsonValue &value : clips)
        {
            if (!value.isObject())
                continue;
            TimelineClip clip = clipFromJson(value.toObject());
            if (clip.trackIndex < 0)
            {
                clip.trackIndex = loadedClips.size();
            }
            if (!clip.filePath.isEmpty())
            {
                loadedClips.push_back(clip);
            }
        }

        const QJsonArray tracks = root.value(QStringLiteral("tracks")).toArray();
        loadedTracks.reserve(tracks.size());
        for (int i = 0; i < tracks.size(); ++i)
        {
            const QJsonObject obj = tracks.at(i).toObject();
            TimelineTrack track;
            track.name = obj.value(QStringLiteral("name")).toString(QStringLiteral("Track %1").arg(i + 1));
            track.height = qMax(28, obj.value(QStringLiteral("height")).toInt(44));
            loadedTracks.push_back(track);
        }

        const QJsonArray renderSyncMarkers = root.value(QStringLiteral("renderSyncMarkers")).toArray();
        loadedRenderSyncMarkers.reserve(renderSyncMarkers.size());
        for (const QJsonValue &value : renderSyncMarkers)
        {
            if (!value.isObject())
            {
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
        if (m_explorerPane) {
            m_explorerPane->setInitialRootPath(resolvedRootPath);
            m_explorerPane->restoreExpandedExplorerPaths(expandedExplorerPaths);
        }
        if (m_outputWidthSpin)
        {
            QSignalBlocker block(m_outputWidthSpin);
            m_outputWidthSpin->setValue(outputWidth);
        }
        if (m_outputHeightSpin)
        {
            QSignalBlocker block(m_outputHeightSpin);
            m_outputHeightSpin->setValue(outputHeight);
        }
        if (m_outputFormatCombo)
        {
            QSignalBlocker block(m_outputFormatCombo);
            const int formatIndex = m_outputFormatCombo->findData(outputFormat);
            if (formatIndex >= 0)
            {
                m_outputFormatCombo->setCurrentIndex(formatIndex);
            }
        }
        if (m_speechFilterEnabledCheckBox)
        {
            QSignalBlocker block(m_speechFilterEnabledCheckBox);
            m_speechFilterEnabledCheckBox->setChecked(speechFilterEnabled);
        }
        m_transcriptPrependMs = transcriptPrependMs;
        m_transcriptPostpendMs = transcriptPostpendMs;
        if (m_transcriptPrependMsSpin)
        {
            QSignalBlocker block(m_transcriptPrependMsSpin);
            m_transcriptPrependMsSpin->setValue(m_transcriptPrependMs);
        }
        if (m_transcriptPostpendMsSpin)
        {
            QSignalBlocker block(m_transcriptPostpendMsSpin);
            m_transcriptPostpendMsSpin->setValue(m_transcriptPostpendMs);
        }
        if (m_inspectorTabs && m_inspectorTabs->count() > 0)
        {
            QSignalBlocker block(m_inspectorTabs);
            m_inspectorTabs->setCurrentIndex(qBound(0, selectedInspectorTab, m_inspectorTabs->count() - 1));
        }
        if (m_preview)
        {
            m_preview->setOutputSize(QSize(outputWidth, outputHeight));
        }

        m_timeline->setTracks(loadedTracks);
        m_timeline->setClips(loadedClips);
        m_timeline->setTimelineZoom(timelineZoom);
        m_timeline->setVerticalScrollOffset(timelineVerticalScroll);
        if (!loadedExportRanges.isEmpty())
        {
            m_timeline->setExportRanges(loadedExportRanges);
        }
        else
        {
            m_timeline->setExportRange(exportStartFrame, exportEndFrame > 0 ? exportEndFrame : m_timeline->totalFrames());
        }
        m_timeline->setRenderSyncMarkers(loadedRenderSyncMarkers);
        m_timeline->setSelectedClipId(selectedClipId);
        syncSliderRange();
        m_preview->setClipCount(m_timeline->clips().size());
        m_preview->setTimelineClips(m_timeline->clips());
        m_preview->setExportRanges(effectivePlaybackRanges());
        m_preview->setRenderSyncMarkers(m_timeline->renderSyncMarkers());
        m_preview->setSelectedClipId(selectedClipId);
        if (m_audioEngine)
        {
            m_audioEngine->setTimelineClips(m_timeline->clips());
            m_audioEngine->setExportRanges(effectivePlaybackRanges());
            m_audioEngine->seek(currentFrame);
        }
        setCurrentFrame(currentFrame);

        m_playbackTimer.stop();
        m_fastPlaybackActive.store(false);
        m_preview->setPlaybackState(false);
        if (m_audioEngine)
        {
            m_audioEngine->stop();
        }

        m_loadingState = false;
        QTimer::singleShot(0, this, [this, resolvedRootPath]()
                           {
            if (m_explorerPane) {
                m_explorerPane->setInitialRootPath(resolvedRootPath);
            }
            refreshProjectsList();
            m_inspectorPane->refresh(); });
    }

    void advanceFrame()
    {
        if (!m_timeline)
        {
            return;
        }

        if (m_audioEngine && m_audioEngine->hasPlayableAudio())
        {
            int64_t audioSample = qMax<int64_t>(0, m_audioEngine->currentSample());
            const qreal audioFramePosition = samplesToFramePosition(audioSample);
            const int64_t audioFrame = qBound<int64_t>(0,
                                                       static_cast<int64_t>(std::floor(audioFramePosition)),
                                                       m_timeline->totalFrames());

            if (audioFrame == m_timeline->currentFrame())
            {
                if (m_preview)
                {
                    m_preview->setCurrentPlaybackSample(audioSample);
                }
                return;
            }

            if (m_preview)
            {
                m_preview->preparePlaybackAdvanceSample(audioSample);
            }
            setCurrentPlaybackSample(audioSample, false, true);
            return;
        }

        const int64_t nextFrame = nextPlaybackFrame(m_timeline->currentFrame());
        if (m_preview)
        {
            m_preview->preparePlaybackAdvance(nextFrame);
        }
        setCurrentPlaybackSample(frameToSamples(nextFrame), false, true);
    }

    bool speechFilterPlaybackEnabled() const
    {
        return m_speechFilterEnabledCheckBox && m_speechFilterEnabledCheckBox->isChecked();
    }

    QVector<ExportRangeSegment> effectivePlaybackRanges() const
    {
        if (!m_timeline)
        {
            return {};
        }
        QVector<ExportRangeSegment> ranges = m_timeline->exportRanges();
        if (!speechFilterPlaybackEnabled())
        {
            return ranges;
        }
        return m_transcriptEngine.transcriptWordExportRanges(ranges,
                                                             m_timeline->clips(),
                                                             m_timeline->renderSyncMarkers(),
                                                             m_transcriptPrependMs,
                                                             m_transcriptPostpendMs);
    }

    int64_t nextPlaybackFrame(int64_t currentFrame) const
    {
        if (!m_timeline)
        {
            return 0;
        }

        const QVector<ExportRangeSegment> ranges = effectivePlaybackRanges();
        if (ranges.isEmpty())
        {
            const int64_t nextFrame = currentFrame + 1;
            return nextFrame > m_timeline->totalFrames() ? 0 : nextFrame;
        }

        for (const ExportRangeSegment &range : ranges)
        {
            if (currentFrame < range.startFrame)
            {
                return range.startFrame;
            }
            if (currentFrame >= range.startFrame && currentFrame < range.endFrame)
            {
                return currentFrame + 1;
            }
        }
        return ranges.constFirst().startFrame;
    }

    QString clipLabelForId(const QString &clipId) const
    {
        if (!m_timeline)
        {
            return clipId;
        }
        for (const TimelineClip &clip : m_timeline->clips())
        {
            if (clip.id == clipId)
            {
                return clip.label;
            }
        }
        return clipId;
    }

    QColor clipColorForId(const QString &clipId) const
    {
        if (!m_timeline)
        {
            return QColor(QStringLiteral("#24303c"));
        }
        for (const TimelineClip &clip : m_timeline->clips())
        {
            if (clip.id == clipId)
            {
                return clip.color;
            }
        }
        return QColor(QStringLiteral("#24303c"));
    }

    bool parseSyncActionText(const QString &text, RenderSyncAction *actionOut) const
    {
        const QString normalized = text.trimmed().toLower();
        if (normalized == QStringLiteral("duplicate") || normalized == QStringLiteral("dup"))
        {
            *actionOut = RenderSyncAction::DuplicateFrame;
            return true;
        }
        if (normalized == QStringLiteral("skip"))
        {
            *actionOut = RenderSyncAction::SkipFrame;
            return true;
        }
        return false;
    }

    void refreshSyncInspector()
    {
        m_syncInspectorClipLabel->setText(QStringLiteral("Sync"));
        m_updatingSyncInspector = true;
        m_syncTable->clearContents();
        m_syncTable->setRowCount(0);
        if (!m_timeline || m_timeline->renderSyncMarkers().isEmpty())
        {
            m_syncInspectorDetailsLabel->setText(QStringLiteral("No render sync markers in the timeline."));
            m_updatingSyncInspector = false;
            return;
        }

        const QVector<RenderSyncMarker> markers = m_timeline->renderSyncMarkers();
        m_syncInspectorDetailsLabel->setText(
            QStringLiteral("%1 sync markers across the timeline. Edit Frame, Count, or Action directly.")
                .arg(markers.size()));
        m_syncTable->setRowCount(markers.size());
        for (int i = 0; i < markers.size(); ++i)
        {
            const RenderSyncMarker &marker = markers[i];
            const QColor clipColor = clipColorForId(marker.clipId);
            const QColor rowBackground = QColor(clipColor.red(), clipColor.green(), clipColor.blue(), 72);
            const QColor rowForeground = QColor(QStringLiteral("#f4f7fb"));
            const QString clipLabel = clipLabelForId(marker.clipId);
            auto *clipItem = new QTableWidgetItem(QString());
            clipItem->setData(Qt::UserRole, marker.clipId);
            clipItem->setData(Qt::UserRole + 1, QVariant::fromValue(static_cast<qint64>(marker.frame)));
            clipItem->setFlags(clipItem->flags() & ~Qt::ItemIsEditable);
            clipItem->setToolTip(clipLabel);
            auto *frameItem = new QTableWidgetItem(QString::number(marker.frame));
            auto *countItem = new QTableWidgetItem(QString::number(marker.count));
            auto *actionItem = new QTableWidgetItem(
                marker.action == RenderSyncAction::DuplicateFrame ? QStringLiteral("Duplicate")
                                                                  : QStringLiteral("Skip"));
            for (QTableWidgetItem *item : {clipItem, frameItem, countItem, actionItem})
            {
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

    void refreshTranscriptInspector()
    {
        const TimelineClip *clip = m_timeline ? m_timeline->selectedClip() : nullptr;
        m_updatingTranscriptInspector = true;
        m_transcriptTable->clearContents();
        m_transcriptTable->setRowCount(0);
        m_loadedTranscriptPath.clear();
        m_loadedTranscriptDoc = QJsonDocument();

        if (!clip || clip->mediaType != ClipMediaType::Audio)
        {
            m_transcriptInspectorClipLabel->setText(QStringLiteral("No transcript selected"));
            m_transcriptInspectorDetailsLabel->setText(QStringLiteral("Select an audio clip with a WhisperX JSON transcript."));
            m_updatingTranscriptInspector = false;
            return;
        }

        const QString transcriptPath = transcriptPathForClipFile(clip->filePath);
        QFile transcriptFile(transcriptPath);
        if (!transcriptFile.open(QIODevice::ReadOnly))
        {
            m_transcriptInspectorClipLabel->setText(clip->label);
            m_transcriptInspectorDetailsLabel->setText(QStringLiteral("No transcript file found."));
            m_updatingTranscriptInspector = false;
            return;
        }

        QJsonParseError parseError;
        const QJsonDocument transcriptDoc = QJsonDocument::fromJson(transcriptFile.readAll(), &parseError);
        if (parseError.error != QJsonParseError::NoError || !transcriptDoc.isObject())
        {
            m_transcriptInspectorClipLabel->setText(clip->label);
            m_transcriptInspectorDetailsLabel->setText(QStringLiteral("Invalid transcript JSON file."));
            m_updatingTranscriptInspector = false;
            return;
        }

        m_loadedTranscriptPath = transcriptPath;
        m_loadedTranscriptDoc = transcriptDoc;

        const QJsonObject root = transcriptDoc.object();
        const QJsonArray segments = root.value(QStringLiteral("segments")).toArray();
        
        int totalWords = 0;
        for (const QJsonValue &segValue : segments)
        {
            const QJsonObject segment = segValue.toObject();
            const QJsonArray words = segment.value(QStringLiteral("words")).toArray();
            totalWords += words.size();
        }

        m_transcriptInspectorClipLabel->setText(clip->label);
        m_transcriptInspectorDetailsLabel->setText(
            QStringLiteral("%1 words across %2 segments").arg(totalWords).arg(segments.size()));

        int row = 0;
        for (const QJsonValue &segValue : segments)
        {
            const QJsonObject segment = segValue.toObject();
            const QJsonArray words = segment.value(QStringLiteral("words")).toArray();
            
            for (const QJsonValue &wordValue : words)
            {
                const QJsonObject word = wordValue.toObject();
                const QString text = word.value(QStringLiteral("word")).toString();
                const double startTime = word.value(QStringLiteral("start")).toDouble();
                const double endTime = word.value(QStringLiteral("end")).toDouble();
                const int64_t startFrame = static_cast<int64_t>(startTime * kTimelineFps);
                const int64_t endFrame = static_cast<int64_t>(endTime * kTimelineFps);

                m_transcriptTable->setRowCount(row + 1);
                
                auto *startItem = new QTableWidgetItem(frameToTimecode(startFrame));
                startItem->setData(Qt::UserRole, startTime);
                startItem->setData(Qt::UserRole + 1, endTime);
                startItem->setData(Qt::UserRole + 2, QVariant::fromValue(startFrame));
                startItem->setData(Qt::UserRole + 3, QVariant::fromValue(endFrame));
                startItem->setFlags(startItem->flags() & ~Qt::ItemIsEditable);
                
                auto *endItem = new QTableWidgetItem(frameToTimecode(endFrame));
                endItem->setFlags(endItem->flags() & ~Qt::ItemIsEditable);
                
                auto *textItem = new QTableWidgetItem(text);
                textItem->setFlags(textItem->flags() & ~Qt::ItemIsEditable);
                
                m_transcriptTable->setItem(row, 0, startItem);
                m_transcriptTable->setItem(row, 1, endItem);
                m_transcriptTable->setItem(row, 2, textItem);
                
                row++;
            }
        }
        m_updatingTranscriptInspector = false;
    }

    void refreshClipInspector()
    {
        const TimelineClip *clip = m_timeline ? m_timeline->selectedClip() : nullptr;
        if (!clip)
        {
            m_clipInspectorClipLabel->setText(QStringLiteral("No clip selected"));
            m_clipProxyUsageLabel->setText(QStringLiteral("Proxy In Use: No"));
            m_clipPlaybackSourceLabel->setText(QStringLiteral("Playback Source: None"));
            m_clipOriginalInfoLabel->setText(QStringLiteral("Original\nNo clip selected."));
            m_clipProxyInfoLabel->setText(QStringLiteral("Proxy\nNo proxy configured."));
            return;
        }

        const QString proxyPath = playbackProxyPathForClip(*clip);
        const QString playbackPath = playbackMediaPathForClip(*clip);
        m_clipInspectorClipLabel->setText(QStringLiteral("%1\n%2")
                                              .arg(clip->label, clipMediaTypeLabel(clip->mediaType)));
        m_clipProxyUsageLabel->setText(QStringLiteral("Proxy In Use: %1")
                                           .arg(playbackPath != clip->filePath ? QStringLiteral("Yes")
                                                                               : QStringLiteral("No")));
        m_clipPlaybackSourceLabel->setText(QStringLiteral("Playback Source\n%1")
                                               .arg(QDir::toNativeSeparators(playbackPath)));
        m_clipOriginalInfoLabel->setText(QStringLiteral("Original\n%1")
                                             .arg(clipFileInfoSummary(clip->filePath)));
        if (proxyPath.isEmpty())
        {
            const QString configuredProxyPath = clip->proxyPath.isEmpty()
                                                    ? defaultProxyOutputPath(*clip)
                                                    : clip->proxyPath;
            m_clipProxyInfoLabel->setText(QStringLiteral("Proxy\nConfigured: No\nPath: %1")
                                              .arg(QDir::toNativeSeparators(configuredProxyPath)));
        }
        else
        {
            m_clipProxyInfoLabel->setText(QStringLiteral("Proxy\n%1")
                                              .arg(clipFileInfoSummary(proxyPath)));
        }
    }
private:
    QToolButton *m_newProjectButton = nullptr;
    QToolButton *m_saveProjectAsButton = nullptr;
    QLabel *m_projectSectionLabel = nullptr;
    QListWidget *m_projectsList = nullptr;

    PreviewWindow *m_preview = nullptr;
    TimelineWidget *m_timeline = nullptr;

    QPushButton *m_playButton = nullptr;
    QSlider *m_seekSlider = nullptr;
    QLabel *m_timecodeLabel = nullptr;

    QToolButton *m_audioMuteButton = nullptr;
    QSlider *m_audioVolumeSlider = nullptr;
    QLabel *m_audioNowPlayingLabel = nullptr;

    QLabel *m_statusBadge = nullptr;
    QLabel *m_previewInfo = nullptr;

    QTabWidget *m_inspectorTabs = nullptr;

    QLabel *m_gradingClipLabel = nullptr;
    QLabel *m_gradingPathLabel = nullptr;

    QLabel *m_videoInspectorClipLabel = nullptr;
    QLabel *m_videoInspectorDetailsLabel = nullptr;

    QLabel *m_keyframesInspectorClipLabel = nullptr;
    QLabel *m_keyframesInspectorDetailsLabel = nullptr;

    QLabel *m_audioInspectorClipLabel = nullptr;
    QLabel *m_audioInspectorDetailsLabel = nullptr;
    QSpinBox *m_audioFadeSamplesSpin = nullptr;
    bool m_updatingAudioInspector = false;

    QLabel *m_transcriptInspectorClipLabel = nullptr;
    QLabel *m_transcriptInspectorDetailsLabel = nullptr;
    QLabel *m_clipInspectorClipLabel = nullptr;
    QLabel *m_clipProxyUsageLabel = nullptr;
    QLabel *m_clipPlaybackSourceLabel = nullptr;
    QLabel *m_clipOriginalInfoLabel = nullptr;
    QLabel *m_clipProxyInfoLabel = nullptr;

    QLabel *m_syncInspectorClipLabel = nullptr;
    QLabel *m_syncInspectorDetailsLabel = nullptr;

    QDoubleSpinBox *m_brightnessSpin = nullptr;
    QDoubleSpinBox *m_contrastSpin = nullptr;
    QDoubleSpinBox *m_saturationSpin = nullptr;
    QDoubleSpinBox *m_opacitySpin = nullptr;
    QCheckBox *m_bypassGradingCheckBox = nullptr;

    QSpinBox *m_outputWidthSpin = nullptr;
    QSpinBox *m_outputHeightSpin = nullptr;
    QSpinBox *m_exportStartSpin = nullptr;
    QSpinBox *m_exportEndSpin = nullptr;
    QComboBox *m_outputFormatCombo = nullptr;

    QDoubleSpinBox *m_videoTranslationXSpin = nullptr;
    QDoubleSpinBox *m_videoTranslationYSpin = nullptr;
    QDoubleSpinBox *m_videoRotationSpin = nullptr;
    QDoubleSpinBox *m_videoScaleXSpin = nullptr;
    QDoubleSpinBox *m_videoScaleYSpin = nullptr;
    QComboBox *m_videoInterpolationCombo = nullptr;

    QCheckBox *m_mirrorHorizontalCheckBox = nullptr;
    QCheckBox *m_mirrorVerticalCheckBox = nullptr;
    QCheckBox *m_lockVideoScaleCheckBox = nullptr;
    QCheckBox *m_keyframeSpaceCheckBox = nullptr;

    QCheckBox *m_transcriptOverlayEnabledCheckBox = nullptr;
    QSpinBox *m_transcriptMaxLinesSpin = nullptr;
    QSpinBox *m_transcriptMaxCharsSpin = nullptr;
    QCheckBox *m_transcriptAutoScrollCheckBox = nullptr;
    QCheckBox *m_transcriptFollowCurrentWordCheckBox = nullptr;

    QSpinBox *m_transcriptPrependMsSpin = nullptr;
    QSpinBox *m_transcriptPostpendMsSpin = nullptr;
    QCheckBox *m_speechFilterEnabledCheckBox = nullptr;
    int m_transcriptPrependMs = 0;
    int m_transcriptPostpendMs = 0;
    mutable TranscriptEngine m_transcriptEngine;

    QDoubleSpinBox *m_transcriptOverlayXSpin = nullptr;
    QDoubleSpinBox *m_transcriptOverlayYSpin = nullptr;
    QSpinBox *m_transcriptOverlayWidthSpin = nullptr;
    QSpinBox *m_transcriptOverlayHeightSpin = nullptr;

    QFontComboBox *m_transcriptFontFamilyCombo = nullptr;
    QSpinBox *m_transcriptFontSizeSpin = nullptr;
    QCheckBox *m_transcriptBoldCheckBox = nullptr;
    QCheckBox *m_transcriptItalicCheckBox = nullptr;

    QTableWidget *m_videoKeyframeTable = nullptr;
    QTableWidget *m_transcriptTable = nullptr;
    QTableWidget *m_syncTable = nullptr;

    QPushButton *m_addVideoKeyframeButton = nullptr;
    QPushButton *m_removeVideoKeyframeButton = nullptr;
    QPushButton *m_renderButton = nullptr;

    std::unique_ptr<ControlServer> m_controlServer;
    std::unique_ptr<AudioEngine> m_audioEngine;
    
    ExplorerPane *m_explorerPane = nullptr;
    InspectorPane *m_inspectorPane = nullptr;

    QTimer m_playbackTimer;
    QTimer m_mainThreadHeartbeatTimer;
    QTimer m_stateSaveTimer;

    bool m_ignoreSeekSignal = false;
    bool m_loadingState = false;
    bool m_updatingProjectsList = false;
    bool m_pendingSaveAfterLoad = false;
    bool m_restoringHistory = false;

    int64_t m_currentPlaybackSample = 0;

    QString m_currentProjectId;

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
int main(int argc, char **argv)
{
    QElapsedTimer startupTimer;
    startupTimer.start();

    qDebug() << "[STARTUP] main() started";
    QApplication app(argc, argv);
    qDebug() << "[STARTUP] QApplication created in" << startupTimer.elapsed() << "ms";
    QApplication::setApplicationName(QStringLiteral("JCut"));

    // Parse command line arguments for debug flags
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Video Editor with AI-powered features"));
    parser.addHelpOption();
    
    QCommandLineOption debugPlaybackOption(QStringLiteral("debug-playback"),
                                           QStringLiteral("Enable playback debug logging"));
    QCommandLineOption debugCacheOption(QStringLiteral("debug-cache"),
                                        QStringLiteral("Enable cache debug logging"));
    QCommandLineOption debugDecodeOption(QStringLiteral("debug-decode"),
                                         QStringLiteral("Enable decode debug logging"));
    QCommandLineOption debugAllOption(QStringLiteral("debug-all"),
                                      QStringLiteral("Enable all debug logging"));
    
    parser.addOption(debugPlaybackOption);
    parser.addOption(debugCacheOption);
    parser.addOption(debugDecodeOption);
    parser.addOption(debugAllOption);
    
    parser.process(app);
    
    // Apply debug flags from CLI (overrides env vars)
    if (parser.isSet(debugAllOption)) {
        editor::setDebugPlaybackEnabled(true);
        editor::setDebugCacheEnabled(true);
        editor::setDebugDecodeEnabled(true);
    } else {
        if (parser.isSet(debugPlaybackOption)) {
            editor::setDebugPlaybackEnabled(true);
        }
        if (parser.isSet(debugCacheOption)) {
            editor::setDebugCacheEnabled(true);
        }
        if (parser.isSet(debugDecodeOption)) {
            editor::setDebugDecodeEnabled(true);
        }
    }

    // Register metatypes
    qRegisterMetaType<editor::FrameHandle>();

    constexpr quint16 kDefaultControlPort = 40130;
    quint16 controlPort = kDefaultControlPort;
    bool ok = false;
    const uint envControlPort = qEnvironmentVariableIntValue("EDITOR_CONTROL_PORT", &ok);
    if (ok && envControlPort <= std::numeric_limits<quint16>::max())
    {
        controlPort = static_cast<quint16>(envControlPort);
    }

    EditorWindow window(controlPort);
    window.show();

    return app.exec();
}

#include "editor.moc"
