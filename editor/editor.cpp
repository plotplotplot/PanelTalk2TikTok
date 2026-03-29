#include "editor.h"
#include "keyframe_table_shared.h"
#include "clip_serialization.h"

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDialog>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProgressBar>
#include <QProgressDialog>
#include <QGridLayout>
#include <QPixmap>
#include <QSaveFile>
#include <QShortcut>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QStyle>
#include <QTextCursor>
#include <QTextStream>
#include <QTemporaryFile>
#include <QVBoxLayout>

using namespace editor;

#include "playback_debug.h"

// ============================================================================
// EditorWindow - Main application window
// ============================================================================
EditorWindow::EditorWindow(quint16 controlPort)
{
    QElapsedTimer ctorTimer;
    ctorTimer.start();

    setWindowTitle(QStringLiteral("JCut"));
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

    connect(m_explorerPane, &ExplorerPane::fileActivated, this, &EditorWindow::addFileToTimeline);
    connect(m_explorerPane, &ExplorerPane::transcriptionRequested, this, &EditorWindow::openTranscriptionWindow);
    connect(m_explorerPane, &ExplorerPane::stateChanged, this, [this]() {
        scheduleSaveState();
        pushHistorySnapshot();
    });
    qDebug() << "[STARTUP] Explorer pane built in" << ctorTimer.elapsed() << "ms";

    qDebug() << "[STARTUP] Building editor pane...";
    QElapsedTimer editorPaneTimer;
    editorPaneTimer.start();
    splitter->addWidget(buildEditorPane());
    m_explorerPane->setPreviewWindow(m_preview);
    qDebug() << "[STARTUP] Editor pane built in" << editorPaneTimer.elapsed() << "ms";
    
    m_inspectorPane = new InspectorPane(this);
    splitter->addWidget(m_inspectorPane);
    m_inspectorTabs = m_inspectorPane->tabs();

    // Wire up inspector pane widgets
    m_transcriptTable = m_inspectorPane->transcriptTable();
    m_transcriptInspectorClipLabel = m_inspectorPane->transcriptInspectorClipLabel();
    m_transcriptInspectorDetailsLabel = m_inspectorPane->transcriptInspectorDetailsLabel();
    m_clipInspectorClipLabel = m_inspectorPane->clipInspectorClipLabel();
    m_clipProxyUsageLabel = m_inspectorPane->clipProxyUsageLabel();
    m_clipPlaybackSourceLabel = m_inspectorPane->clipPlaybackSourceLabel();
    m_clipOriginalInfoLabel = m_inspectorPane->clipOriginalInfoLabel();
    m_clipProxyInfoLabel = m_inspectorPane->clipProxyInfoLabel();
    m_clipPlaybackRateSpin = m_inspectorPane->clipPlaybackRateSpin();
    m_trackInspectorLabel = m_inspectorPane->trackInspectorLabel();
    m_trackInspectorDetailsLabel = m_inspectorPane->trackInspectorDetailsLabel();
    m_trackNameEdit = m_inspectorPane->trackNameEdit();
    m_trackHeightSpin = m_inspectorPane->trackHeightSpin();
    m_trackVideoEnabledCheckBox = m_inspectorPane->trackVideoEnabledCheckBox();
    m_trackAudioEnabledCheckBox = m_inspectorPane->trackAudioEnabledCheckBox();
    m_trackCrossfadeSecondsSpin = m_inspectorPane->trackCrossfadeSecondsSpin();
    m_trackCrossfadeButton = m_inspectorPane->trackCrossfadeButton();
    m_previewHideOutsideOutputCheckBox = m_inspectorPane->previewHideOutsideOutputCheckBox();
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
    if (m_syncTable) {
        connect(m_syncTable, &QTableWidget::itemSelectionChanged,
                this, &EditorWindow::onSyncTableSelectionChanged);
        connect(m_syncTable, &QTableWidget::itemChanged,
                this, &EditorWindow::onSyncTableItemChanged);
        connect(m_syncTable, &QTableWidget::itemDoubleClicked,
                this, &EditorWindow::onSyncTableItemDoubleClicked);
        m_syncTable->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(m_syncTable, &QWidget::customContextMenuRequested,
                this, &EditorWindow::onSyncTableCustomContextMenu);
    }
    m_gradingPathLabel = m_inspectorPane->gradingPathLabel();
    m_brightnessSpin = m_inspectorPane->brightnessSpin();
    m_contrastSpin = m_inspectorPane->contrastSpin();
    m_saturationSpin = m_inspectorPane->saturationSpin();
    m_opacitySpin = m_inspectorPane->opacitySpin();
    m_gradingKeyframeTable = m_inspectorPane->gradingKeyframeTable();
    m_gradingAutoScrollCheckBox = m_inspectorPane->gradingAutoScrollCheckBox();
    m_gradingFollowCurrentCheckBox = m_inspectorPane->gradingFollowCurrentCheckBox();
    m_gradingKeyAtPlayheadButton = m_inspectorPane->gradingKeyAtPlayheadButton();
    m_gradingFadeInButton = m_inspectorPane->gradingFadeInButton();
    m_gradingFadeOutButton = m_inspectorPane->gradingFadeOutButton();
    m_keyframesInspectorClipLabel = m_inspectorPane->keyframesInspectorClipLabel();
    m_keyframesInspectorDetailsLabel = m_inspectorPane->keyframesInspectorDetailsLabel();
    m_keyframesAutoScrollCheckBox = m_inspectorPane->keyframesAutoScrollCheckBox();
    m_keyframesFollowCurrentCheckBox = m_inspectorPane->keyframesFollowCurrentCheckBox();
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
    m_lockVideoScaleCheckBox = m_inspectorPane->lockVideoScaleCheckBox();
    m_keyframeSpaceCheckBox = m_inspectorPane->keyframeSpaceCheckBox();
    m_addVideoKeyframeButton = m_inspectorPane->addVideoKeyframeButton();
    m_removeVideoKeyframeButton = m_inspectorPane->removeVideoKeyframeButton();
    m_outputWidthSpin = m_inspectorPane->outputWidthSpin();
    m_outputHeightSpin = m_inspectorPane->outputHeightSpin();
    m_exportStartSpin = m_inspectorPane->exportStartSpin();
    m_exportEndSpin = m_inspectorPane->exportEndSpin();
    m_outputFormatCombo = m_inspectorPane->outputFormatCombo();
    m_outputRangeSummaryLabel = m_inspectorPane->outputRangeSummaryLabel();
    m_renderUseProxiesCheckBox = m_inspectorPane->renderUseProxiesCheckBox();
    m_renderButton = m_inspectorPane->renderButton();
    m_profileSummaryTable = m_inspectorPane->profileSummaryTable();
    m_profileBenchmarkButton = m_inspectorPane->profileBenchmarkButton();
    m_transcriptPrependMsSpin = m_inspectorPane->transcriptPrependMsSpin();
    m_transcriptPostpendMsSpin = m_inspectorPane->transcriptPostpendMsSpin();
    m_speechFilterEnabledCheckBox = m_inspectorPane->speechFilterEnabledCheckBox();
    m_speechFilterFadeSamplesSpin = m_inspectorPane->speechFilterFadeSamplesSpin();

    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setStretchFactor(2, 0);
    splitter->setSizes({320, 900, 280});

    setCentralWidget(central);

    // Editor pane connections
    connect(&m_playbackTimer, &QTimer::timeout, this, &EditorWindow::advanceFrame);
    m_playbackTimer.setTimerType(Qt::PreciseTimer);
    m_playbackTimer.setInterval(16);
    
    // Shortcuts
    auto *undoShortcut = new QShortcut(QKeySequence::Undo, this);
    connect(undoShortcut, &QShortcut::activated, this, [this]() {
        if (shouldBlockGlobalEditorShortcuts()) {
            return;
        }
        undoHistory();
    });
    auto *splitShortcut = new QShortcut(QKeySequence(QStringLiteral("Ctrl+B")), this);
    connect(splitShortcut, &QShortcut::activated, this, [this]() {
        if (shouldBlockGlobalEditorShortcuts()) {
            return;
        }
        if (m_timeline && m_timeline->splitSelectedClipAtFrame(m_timeline->currentFrame())) {
            m_inspectorPane->refresh();
        }
    });
    auto *deleteShortcut = new QShortcut(QKeySequence::Delete, this);
    connect(deleteShortcut, &QShortcut::activated, this, [this]() {
        if (focusInTranscriptTable() || focusInKeyframeTable() || focusInGradingTable() ||
            shouldBlockGlobalEditorShortcuts()) {
            return;
        }
        if (m_timeline && m_timeline->deleteSelectedClip()) {
            m_inspectorPane->refresh();
        }
    });
    auto *nudgeLeftShortcut = new QShortcut(QKeySequence(QStringLiteral("Alt+Left")), this);
    connect(nudgeLeftShortcut, &QShortcut::activated, this, [this]() {
        if (shouldBlockGlobalEditorShortcuts()) {
            return;
        }
        if (m_timeline) m_timeline->nudgeSelectedClip(-1);
    });
    auto *nudgeRightShortcut = new QShortcut(QKeySequence(QStringLiteral("Alt+Right")), this);
    connect(nudgeRightShortcut, &QShortcut::activated, this, [this]() {
        if (shouldBlockGlobalEditorShortcuts()) {
            return;
        }
        if (m_timeline) m_timeline->nudgeSelectedClip(1);
    });
    auto *playbackShortcut = new QShortcut(QKeySequence(Qt::Key_Space), this);
    connect(playbackShortcut, &QShortcut::activated, this, [this]() {
        if (shouldBlockGlobalEditorShortcuts()) {
            return;
        }
        if (m_timeline) togglePlayback();
    });
        m_mainThreadHeartbeatTimer.setInterval(100);
    connect(&m_mainThreadHeartbeatTimer, &QTimer::timeout, this, [this]() { m_lastMainThreadHeartbeatMs.store(nowMs()); });
    m_lastMainThreadHeartbeatMs.store(nowMs());
    m_mainThreadHeartbeatTimer.start();

    m_stateSaveTimer.setSingleShot(true);
    m_stateSaveTimer.setInterval(250);
    connect(&m_stateSaveTimer, &QTimer::timeout, this, [this]() { saveStateNow(); });

    initializeDeferredTimelineSeek(&m_transcriptClickSeekTimer, &m_pendingTranscriptClickTimelineFrame);
    initializeDeferredTimelineSeek(&m_keyframeClickSeekTimer, &m_pendingKeyframeClickTimelineFrame);
    initializeDeferredTimelineSeek(&m_gradingClickSeekTimer, &m_pendingGradingClickTimelineFrame);
    initializeDeferredTimelineSeek(&m_syncClickSeekTimer, &m_pendingSyncClickTimelineFrame);

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
                {QStringLiteral("last_playhead_advance_age_ms"), playheadMs > 0 ? now - playheadMs : -1}};
        },
        [this]() { return profilingSnapshot(); },
        this);
    m_controlServer->start(controlPort);
    qDebug() << "[STARTUP] ControlServer started in" << ctorTimer.elapsed() << "ms";
    
    m_audioEngine = std::make_unique<AudioEngine>();
    m_audioEngine->setSpeechFilterFadeSamples(m_speechFilterFadeSamples);

    // Speech filter routing
    const auto refreshSpeechFilterRouting = [this](bool pushHistory = false) {
        const QVector<ExportRangeSegment> ranges = effectivePlaybackRanges();
        if (m_preview) m_preview->setExportRanges(ranges);
        if (m_audioEngine) {
            m_audioEngine->setExportRanges(ranges);
            m_audioEngine->setSpeechFilterFadeSamples(m_speechFilterFadeSamples);
        }
        m_inspectorPane->refresh();
        scheduleSaveState();
        if (pushHistory) pushHistorySnapshot();
    };

    connect(m_speechFilterEnabledCheckBox, &QCheckBox::toggled, this, [refreshSpeechFilterRouting](bool) { refreshSpeechFilterRouting(true); });
    connect(m_transcriptPrependMsSpin, qOverload<int>(&QSpinBox::valueChanged), this, [this, refreshSpeechFilterRouting](int value) {
        m_transcriptPrependMs = qMax(0, value);
        refreshSpeechFilterRouting(true);
    });
    connect(m_transcriptPostpendMsSpin, qOverload<int>(&QSpinBox::valueChanged), this, [this, refreshSpeechFilterRouting](int value) {
        m_transcriptPostpendMs = qMax(0, value);
        refreshSpeechFilterRouting(true);
    });
    connect(m_speechFilterFadeSamplesSpin, qOverload<int>(&QSpinBox::valueChanged), this, [this, refreshSpeechFilterRouting](int value) {
        m_speechFilterFadeSamples = qMax(0, value);
        refreshSpeechFilterRouting(true);
    });
    connect(m_clipPlaybackRateSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double value) {
        if (!m_timeline || !m_clipPlaybackRateSpin) {
            return;
        }
        const TimelineClip *clip = m_timeline->selectedClip();
        if (!clip || !clipHasVisuals(*clip)) {
            return;
        }
        const qreal playbackRate = qBound<qreal>(0.001, value, 4.0);
        if (qFuzzyCompare(clip->playbackRate, playbackRate)) {
            return;
        }
        if (!m_timeline->updateClipById(clip->id, [playbackRate](TimelineClip &editableClip) {
                editableClip.playbackRate = playbackRate;
                normalizeClipTiming(editableClip);
            })) {
            return;
        }
        if (m_preview) {
            m_preview->setTimelineClips(m_timeline->clips());
        }
        m_inspectorPane->refresh();
        scheduleSaveState();
        pushHistorySnapshot();
    });
    connect(m_trackNameEdit, &QLineEdit::editingFinished, this, [this]() {
        if (!m_timeline || !m_trackNameEdit) {
            return;
        }
        const int trackIndex = m_timeline->selectedTrackIndex();
        const TimelineTrack* track = m_timeline->selectedTrack();
        if (trackIndex < 0 || !track) {
            return;
        }
        const QString nextName = m_trackNameEdit->text().trimmed().isEmpty()
                                     ? QStringLiteral("Track %1").arg(trackIndex + 1)
                                     : m_trackNameEdit->text().trimmed();
        if (track->name == nextName) {
            return;
        }
        if (!m_timeline->updateTrackByIndex(trackIndex, [nextName](TimelineTrack& editableTrack) {
                editableTrack.name = nextName;
            })) {
            return;
        }
        refreshClipInspector();
    });
    connect(m_trackHeightSpin, qOverload<int>(&QSpinBox::valueChanged), this, [this](int value) {
        if (!m_timeline || !m_trackHeightSpin) {
            return;
        }
        const int trackIndex = m_timeline->selectedTrackIndex();
        const TimelineTrack* track = m_timeline->selectedTrack();
        if (trackIndex < 0 || !track || track->height == value) {
            return;
        }
        if (!m_timeline->updateTrackByIndex(trackIndex, [value](TimelineTrack& editableTrack) {
                editableTrack.height = value;
            })) {
            return;
        }
        refreshClipInspector();
    });
    connect(m_trackVideoEnabledCheckBox, &QCheckBox::toggled, this, [this](bool checked) {
        if (!m_timeline || !m_trackVideoEnabledCheckBox) {
            return;
        }
        const int trackIndex = m_timeline->selectedTrackIndex();
        if (trackIndex < 0) {
            return;
        }
        if (m_timeline->updateTrackVisualEnabled(trackIndex, checked)) {
            refreshClipInspector();
        }
    });
    connect(m_trackAudioEnabledCheckBox, &QCheckBox::toggled, this, [this](bool checked) {
        if (!m_timeline || !m_trackAudioEnabledCheckBox) {
            return;
        }
        const int trackIndex = m_timeline->selectedTrackIndex();
        if (trackIndex < 0) {
            return;
        }
        if (m_timeline->updateTrackAudioEnabled(trackIndex, checked)) {
            refreshClipInspector();
        }
    });
    connect(m_trackCrossfadeButton, &QPushButton::clicked, this, [this]() {
        if (!m_timeline || !m_trackCrossfadeButton || !m_trackCrossfadeSecondsSpin) {
            return;
        }
        const int trackIndex = m_timeline->selectedTrackIndex();
        if (trackIndex < 0) {
            return;
        }
        if (m_timeline->crossfadeTrack(trackIndex, m_trackCrossfadeSecondsSpin->value())) {
            refreshClipInspector();
        }
    });
    connect(m_previewHideOutsideOutputCheckBox, &QCheckBox::toggled, this, [this](bool checked) {
        if (m_preview) {
            m_preview->setHideOutsideOutputWindow(checked);
        }
        scheduleSaveState();
        pushHistorySnapshot();
    });

    // Instantiate and wire up tab modules
    m_outputTab = std::make_unique<OutputTab>(
        OutputTab::Widgets{
            m_outputWidthSpin, m_outputHeightSpin,
            m_exportStartSpin, m_exportEndSpin,
            m_outputFormatCombo, m_outputRangeSummaryLabel, m_renderUseProxiesCheckBox, m_renderButton},
        OutputTab::Dependencies{
            [this]() { return m_timeline != nullptr; },
            [this]() { return m_timeline && !m_timeline->clips().isEmpty(); },
            [this]() -> int64_t { return m_timeline ? m_timeline->totalFrames() : 0; },
            [this]() -> int64_t { return m_timeline ? m_timeline->exportStartFrame() : 0; },
            [this]() -> int64_t { return m_timeline ? m_timeline->exportEndFrame() : 0; },
            [this]() { return effectivePlaybackRanges(); },
            [this](int64_t startFrame, int64_t endFrame) { if (m_timeline) m_timeline->setExportRange(startFrame, endFrame); },
            [this](const QSize& size) { if (m_preview) m_preview->setOutputSize(size); },
            [this]() { setPlaybackActive(false); },
            [this]() { return m_timeline ? m_timeline->clips() : QVector<TimelineClip>{}; },
            [this]() { return m_timeline ? m_timeline->renderSyncMarkers() : QVector<RenderSyncMarker>{}; },
            [this](const RenderRequest& request) { renderTimelineFromOutputRequest(request); },
            [this]() { return m_lastRenderOutputPath; },
            [this](const QString& path) {
                m_lastRenderOutputPath = path;
                scheduleSaveState();
            },
            [this]() { scheduleSaveState(); },
            [this]() { pushHistorySnapshot(); }});
    m_outputTab->wire();

    m_profileTab = std::make_unique<ProfileTab>(
        ProfileTab::Widgets{m_profileSummaryTable, m_profileBenchmarkButton},
        ProfileTab::Dependencies{
            [this]() { return profilingSnapshot(); },
            [this](TimelineClip* clipOut) { return profileBenchmarkClip(clipOut); },
            [this](const TimelineClip& clip) { return playbackMediaPathForClip(clip); },
            [this]() { m_inspectorPane->refresh(); }});
    m_profileTab->wire();

    m_transcriptTab = std::make_unique<TranscriptTab>(
        TranscriptTab::Widgets{
            m_transcriptInspectorClipLabel, m_transcriptInspectorDetailsLabel,
            m_transcriptTable, m_transcriptOverlayEnabledCheckBox,
            m_transcriptMaxLinesSpin, m_transcriptMaxCharsSpin,
            m_transcriptAutoScrollCheckBox, m_transcriptFollowCurrentWordCheckBox,
            m_transcriptOverlayXSpin, m_transcriptOverlayYSpin,
            m_transcriptOverlayWidthSpin, m_transcriptOverlayHeightSpin,
            m_transcriptFontFamilyCombo, m_transcriptFontSizeSpin,
            m_transcriptBoldCheckBox, m_transcriptItalicCheckBox,
            m_transcriptPrependMsSpin, m_transcriptPostpendMsSpin,
            m_speechFilterEnabledCheckBox, m_speechFilterFadeSamplesSpin},
        TranscriptTab::Dependencies{
            [this]() { return m_timeline ? m_timeline->selectedClip() : nullptr; },
            [this](const QString& id, const std::function<void(TimelineClip&)>& updater) {
                return m_timeline->updateClipById(id, updater);
            },
            [this]() { scheduleSaveState(); },
            [this]() { pushHistorySnapshot(); },
            [this]() { m_inspectorPane->refresh(); },
            [this]() { m_preview->setTimelineClips(m_timeline->clips()); },
            [this]() { return effectivePlaybackRanges(); },
            [this](int64_t frame) { setCurrentFrame(frame); }});
    m_transcriptTab->wire();

    // Connect transcript tab speech filter settings to editor
    connect(m_transcriptTab.get(), &TranscriptTab::speechFilterSettingsChanged, this, [this, refreshSpeechFilterRouting]() {
        refreshSpeechFilterRouting(true);
    });

        m_gradingTab = std::make_unique<GradingTab>(
        GradingTab::Widgets{
            m_gradingPathLabel, m_brightnessSpin, m_contrastSpin,
            m_saturationSpin, m_opacitySpin, m_gradingKeyframeTable,
            m_gradingAutoScrollCheckBox, m_gradingFollowCurrentCheckBox,
            m_gradingKeyAtPlayheadButton, m_gradingFadeInButton, m_gradingFadeOutButton},
        GradingTab::Dependencies{
            [this]() { return m_timeline ? m_timeline->selectedClip() : nullptr; },
            [this]() { return m_timeline ? m_timeline->selectedClip() : nullptr; },
            [this](const QString& id, const std::function<void(TimelineClip&)>& updater) {
                return m_timeline->updateClipById(id, updater);
            },
            [this](const TimelineClip& clip) { return clip.filePath; },
            [this](const TimelineClip& clip) { return clipHasVisuals(clip); },
            [this]() { scheduleSaveState(); },
            [this]() { pushHistorySnapshot(); },
            [this]() { m_inspectorPane->refresh(); },
            [this]() { m_preview->setTimelineClips(m_timeline->clips()); },
            [this]() -> int64_t { return m_timeline ? m_timeline->currentFrame() : 0; },
            [this]() -> int64_t { return m_timeline && m_timeline->selectedClip() ? m_timeline->selectedClip()->startFrame : 0; },
            [this]() -> QString { return m_timeline ? m_timeline->selectedClipId() : QString(); },
            [this](int64_t frame) { setCurrentFrame(frame); },
            {},
            {}});
    m_gradingTab->wire();

    m_videoKeyframeTab = std::make_unique<VideoKeyframeTab>(
        VideoKeyframeTab::Widgets{
            m_keyframesInspectorClipLabel, m_keyframesInspectorDetailsLabel,
            m_videoKeyframeTable, m_videoTranslationXSpin, m_videoTranslationYSpin,
            m_videoRotationSpin, m_videoScaleXSpin, m_videoScaleYSpin,
            m_videoInterpolationCombo, m_mirrorHorizontalCheckBox,
            m_mirrorVerticalCheckBox, m_lockVideoScaleCheckBox,
            m_keyframeSpaceCheckBox, m_keyframesAutoScrollCheckBox,
            m_keyframesFollowCurrentCheckBox, m_addVideoKeyframeButton, m_removeVideoKeyframeButton},
        VideoKeyframeTab::Dependencies{
            [this]() { return m_timeline ? m_timeline->selectedClip() : nullptr; },
            [this]() { return m_timeline ? m_timeline->selectedClip() : nullptr; },
            [this](const QString& id, const std::function<void(TimelineClip&)>& updater) {
                return m_timeline->updateClipById(id, updater);
            },
            [this](const TimelineClip& clip) { return clip.filePath; },
            [this](const TimelineClip& clip) { return clipHasVisuals(clip); },
            [this]() { scheduleSaveState(); },
            [this]() { pushHistorySnapshot(); },
            [this]() { m_inspectorPane->refresh(); },
            [this]() { m_preview->setTimelineClips(m_timeline->clips()); },
            [this]() -> int64_t { return m_timeline ? m_timeline->currentFrame() : 0; },
            [this]() -> int64_t { return m_timeline && m_timeline->selectedClip() ? m_timeline->selectedClip()->startFrame : 0; },
            [this]() -> QString { return m_timeline ? m_timeline->selectedClipId() : QString(); },
            [this](int64_t frame) { setCurrentFrame(frame); },
            {},
            {}});
    m_videoKeyframeTab->wire();

    // Connect inspector pane refresh to tabs
    connect(m_inspectorPane, &InspectorPane::refreshRequested, this, [this]() {
        m_gradingTab->refresh();
        refreshSyncInspector();
        m_transcriptTab->refresh();
        refreshClipInspector();
        m_videoKeyframeTab->refresh();
        m_outputTab->refresh();
        m_profileTab->refresh();
    });

    QTimer::singleShot(0, this, [this]() {
        loadProjectsFromFolders();
        refreshProjectsList();
        loadState();
        m_inspectorPane->refresh();
    });
}

EditorWindow::~EditorWindow()
{
    saveStateNow();
}

void EditorWindow::closeEvent(QCloseEvent *event)
{
    saveStateNow();
    QMainWindow::closeEvent(event);
}

bool EditorWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (m_transcriptTable &&
        (watched == m_transcriptTable || watched == m_transcriptTable->viewport()) &&
        event->type() == QEvent::KeyPress)
    {
        auto *keyEvent = static_cast<QKeyEvent *>(event);
        if (keyEvent->key() == Qt::Key_Delete && !(keyEvent->modifiers() & Qt::KeyboardModifierMask))
        {
            m_transcriptTab->deleteSelectedRows();
            return true;
        }
    }
    if (m_videoKeyframeTable &&
        (watched == m_videoKeyframeTable || watched == m_videoKeyframeTable->viewport()) &&
        event->type() == QEvent::KeyPress)
    {
        auto *keyEvent = static_cast<QKeyEvent *>(event);
        if (keyEvent->key() == Qt::Key_Delete && !(keyEvent->modifiers() & Qt::KeyboardModifierMask))
        {
            m_videoKeyframeTab->removeSelectedKeyframes();
            return true;
        }
    }
    if (m_gradingKeyframeTable &&
        (watched == m_gradingKeyframeTable || watched == m_gradingKeyframeTable->viewport()) &&
        event->type() == QEvent::KeyPress)
    {
        auto *keyEvent = static_cast<QKeyEvent *>(event);
        if (keyEvent->key() == Qt::Key_Delete && !(keyEvent->modifiers() & Qt::KeyboardModifierMask))
        {
            m_gradingTab->removeSelectedKeyframes();
            return true;
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

void EditorWindow::openTranscriptionWindow(const QString &filePath, const QString &label)
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

    auto *autoScrollBox = new QCheckBox(QStringLiteral("Auto-scroll"), dialog);
    autoScrollBox->setChecked(true);
    layout->addWidget(autoScrollBox);

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

    const auto appendOutput = [output, autoScrollBox](const QString &text)
    {
        if (text.isEmpty()) return;
        if (autoScrollBox->isChecked()) output->moveCursor(QTextCursor::End);
        output->insertPlainText(text);
        if (autoScrollBox->isChecked()) output->moveCursor(QTextCursor::End);
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
                                 .arg(exitStatus == QProcess::NormalExit ? QStringLiteral("normal") : QStringLiteral("crashed")));
            });
    connect(sendButton, &QPushButton::clicked, dialog, [process, inputLine, appendOutput]()
            {
                const QString text = inputLine->text();
                if (text.isEmpty()) return;
                process->write(text.toUtf8());
                process->write("\n");
                appendOutput(QStringLiteral("> %1\n").arg(text));
                inputLine->clear();
            });
    connect(inputLine, &QLineEdit::returnPressed, sendButton, &QPushButton::click);
    connect(closeButton, &QPushButton::clicked, dialog, &QDialog::close);
    connect(dialog, &QDialog::finished, dialog, [process](int)
            {
                if (process->state() != QProcess::NotRunning) {
                    process->kill();
                    process->waitForFinished(1000);
                }
            });

    process->start(QStringLiteral("/bin/bash"), {scriptPath, QFileInfo(filePath).absoluteFilePath()});
    dialog->show();
}

QString EditorWindow::defaultProxyOutputPath(const TimelineClip &clip, const MediaProbeResult *knownProbe) const
{
    const QFileInfo sourceInfo(clip.filePath);
    MediaProbeResult fallbackProbe;
    const MediaProbeResult &probe = knownProbe ? *knownProbe : fallbackProbe;
    const QString suffix = QStringLiteral("mov");
    return sourceInfo.dir().absoluteFilePath(
        QStringLiteral("%1.proxy.%2").arg(sourceInfo.completeBaseName(), suffix));
}

QString EditorWindow::clipFileInfoSummary(const QString &filePath, const MediaProbeResult *knownProbe) const
{
    if (filePath.isEmpty()) return QStringLiteral("Path: None");

    const QFileInfo info(filePath);
    QStringList lines;
    lines << QStringLiteral("Path: %1").arg(QDir::toNativeSeparators(filePath));
    lines << QStringLiteral("Exists: %1").arg(info.exists() ? QStringLiteral("Yes") : QStringLiteral("No"));
    if (!info.exists()) return lines.join(QLatin1Char('\n'));

    lines << QStringLiteral("Size: %1 MB").arg(
        QString::number(static_cast<double>(info.size()) / (1024.0 * 1024.0), 'f', 1));
    lines << QStringLiteral("Modified: %1").arg(info.lastModified().toString(Qt::ISODate));
    MediaProbeResult fallbackProbe;
    const MediaProbeResult &probe = knownProbe ? *knownProbe : fallbackProbe;
    lines << QStringLiteral("Media Type: %1").arg(clipMediaTypeLabel(probe.mediaType));
    lines << QStringLiteral("Duration: %1 frames").arg(probe.durationFrames);
    lines << QStringLiteral("Audio: %1").arg(probe.hasAudio ? QStringLiteral("Yes") : QStringLiteral("No"));
    lines << QStringLiteral("Video: %1").arg(probe.hasVideo ? QStringLiteral("Yes") : QStringLiteral("No"));
    return lines.join(QLatin1Char('\n'));
}
void EditorWindow::createProxyForClip(const QString &clipId)
{
    if (!m_timeline) return;

    const TimelineClip *clip = nullptr;
    for (const TimelineClip &candidate : m_timeline->clips())
    {
        if (candidate.id == clipId)
        {
            clip = &candidate;
            break;
        }
    }
    if (!clip) return;
    if (clip->mediaType != ClipMediaType::Video)
    {
        QMessageBox::information(this, QStringLiteral("Create Proxy"),
                                 QStringLiteral("Proxy creation is currently available for video clips."));
        return;
    }

    const QString ffmpegPath = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (ffmpegPath.isEmpty())
    {
        QMessageBox::warning(this, QStringLiteral("Create Proxy Failed"),
                             QStringLiteral("ffmpeg was not found in PATH."));
        return;
    }

    const bool imageSequenceProxy = isImageSequencePath(clip->filePath);
    const QStringList sequenceFrames = imageSequenceProxy ? imageSequenceFramePaths(clip->filePath) : QStringList{};
    if (imageSequenceProxy && sequenceFrames.isEmpty())
    {
        QMessageBox::warning(this, QStringLiteral("Create Proxy Failed"),
                             QStringLiteral("No sequence frames were found for this clip."));
        return;
    }

    const QString probePath = imageSequenceProxy ? sequenceFrames.constFirst() : clip->filePath;
    const MediaProbeResult sourceProbe = probeMediaFile(probePath, clip->durationFrames);
    const QString existingProxyPath = playbackProxyPathForClip(*clip);
    const QString outputPath = defaultProxyOutputPath(*clip, &sourceProbe);
    const QString overwriteTarget = !existingProxyPath.isEmpty() ? existingProxyPath : outputPath;
    
    if (QFileInfo::exists(overwriteTarget))
    {
        const auto response = QMessageBox::question(
            this,
            QStringLiteral("Overwrite Proxy"),
            QStringLiteral("A proxy already exists:\n%1\n\nOverwrite it?")
                .arg(QDir::toNativeSeparators(overwriteTarget)),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);
        if (response != QMessageBox::Yes) return;
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

    auto *autoScrollBox = new QCheckBox(QStringLiteral("Auto-scroll"), dialog);
    autoScrollBox->setChecked(true);
    layout->addWidget(autoScrollBox);

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

    std::unique_ptr<QTemporaryFile> sequenceListFile;
    QStringList arguments = {
        QStringLiteral("-y"),
        QStringLiteral("-hide_banner")};

    if (imageSequenceProxy)
    {
        sequenceListFile = std::make_unique<QTemporaryFile>(QDir::temp().filePath(QStringLiteral("editor_sequence_proxy_XXXXXX.txt")));
        sequenceListFile->setAutoRemove(false);
        if (!sequenceListFile->open())
        {
            QMessageBox::warning(this, QStringLiteral("Create Proxy Failed"),
                                 QStringLiteral("Unable to create a temporary ffmpeg input list for the image sequence."));
            return;
        }

        QTextStream stream(sequenceListFile.get());
        for (const QString &framePath : sequenceFrames)
        {
            QString escapedFramePath = QFileInfo(framePath).absoluteFilePath();
            escapedFramePath.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
            stream << "file '" << escapedFramePath << "'\n";
            stream << "duration " << QString::number(1.0 / static_cast<double>(kTimelineFps), 'f', 6) << '\n';
        }
        if (!sequenceFrames.isEmpty())
        {
            QString escapedFramePath = QFileInfo(sequenceFrames.constLast()).absoluteFilePath();
            escapedFramePath.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
            stream << "file '" << escapedFramePath << "'\n";
        }
        stream.flush();
        sequenceListFile->flush();

        arguments << QStringLiteral("-f") << QStringLiteral("concat")
                  << QStringLiteral("-safe") << QStringLiteral("0")
                  << QStringLiteral("-i") << sequenceListFile->fileName();
    }
    else
    {
        arguments << QStringLiteral("-i") << QFileInfo(clip->filePath).absoluteFilePath()
                  << QStringLiteral("-map") << QStringLiteral("0:v:0")
                  << QStringLiteral("-map") << QStringLiteral("0:a?");
    }
    
    const bool alphaProxy = sourceProbe.hasAlpha;
    if (alphaProxy)
    {
        arguments << QStringLiteral("-c:v") << QStringLiteral("png")
                  << QStringLiteral("-pix_fmt") << QStringLiteral("rgba");
    }
    else
    {
        arguments << QStringLiteral("-vf") << QStringLiteral("scale='min(1280,iw)':-2")
                  << QStringLiteral("-c:v") << QStringLiteral("mjpeg")
                  << QStringLiteral("-q:v") << QStringLiteral("3")
                  << QStringLiteral("-pix_fmt") << QStringLiteral("yuvj420p");
    }
    if (!imageSequenceProxy)
    {
        arguments << QStringLiteral("-c:a") << QStringLiteral("pcm_s16le")
                  << QStringLiteral("-b:a") << QStringLiteral("1536k");
    }
    arguments << QFileInfo(outputPath).absoluteFilePath();

    const auto appendOutput = [output, autoScrollBox](const QString &text)
    {
        if (text.isEmpty()) return;
        if (autoScrollBox->isChecked()) output->moveCursor(QTextCursor::End);
        output->insertPlainText(text);
        if (autoScrollBox->isChecked()) output->moveCursor(QTextCursor::End);
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
            [this, clipId, outputPath, existingProxyPath, appendOutput, sequenceListPath = sequenceListFile ? sequenceListFile->fileName() : QString()](int exitCode, QProcess::ExitStatus exitStatus)
            {
                appendOutput(QStringLiteral("\n[process finished] exitCode=%1 status=%2\n")
                                 .arg(exitCode)
                                 .arg(exitStatus == QProcess::NormalExit ? QStringLiteral("normal") : QStringLiteral("crashed")));
                if (!sequenceListPath.isEmpty())
                {
                    QFile::remove(sequenceListPath);
                }
                if (exitStatus != QProcess::NormalExit || exitCode != 0 || !QFileInfo::exists(outputPath))
                {
                    return;
                }
                if (!existingProxyPath.isEmpty() &&
                    existingProxyPath != outputPath &&
                    QFileInfo::exists(existingProxyPath))
                {
                    QFile::remove(existingProxyPath);
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
    connect(dialog, &QDialog::finished, dialog, [process, sequenceListPath = sequenceListFile ? sequenceListFile->fileName() : QString()](int)
            {
                if (process->state() != QProcess::NotRunning)
                {
                    process->kill();
                    process->waitForFinished(1000);
                }
                if (!sequenceListPath.isEmpty())
                {
                    QFile::remove(sequenceListPath);
                }
            });

    process->start(ffmpegPath, arguments);
    dialog->show();
}

void EditorWindow::deleteProxyForClip(const QString &clipId)
{
    if (!m_timeline) return;

    const TimelineClip *clip = nullptr;
    for (const TimelineClip &candidate : m_timeline->clips())
    {
        if (candidate.id == clipId)
        {
            clip = &candidate;
            break;
        }
    }
    if (!clip) return;

    const QString proxyPath = playbackProxyPathForClip(*clip);
    if (proxyPath.isEmpty())
    {
        QMessageBox::information(this, QStringLiteral("Delete Proxy"),
                                 QStringLiteral("No proxy exists for this clip."));
        return;
    }

    const auto response = QMessageBox::question(
        this,
        QStringLiteral("Delete Proxy"),
        QStringLiteral("Delete this proxy?\n%1").arg(QDir::toNativeSeparators(proxyPath)),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (response != QMessageBox::Yes) return;

    QFile::remove(proxyPath);
    if (!m_timeline->updateClipById(clipId, [](TimelineClip &updatedClip)
                                   { updatedClip.proxyPath.clear(); }))
    {
        return;
    }
    if (m_timeline->clipsChanged)
    {
        m_timeline->clipsChanged();
    }
}

QWidget *EditorWindow::buildEditorPane()
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

    connect(pane, &EditorPane::playClicked, this, [this]() { togglePlayback(); });
    connect(pane, &EditorPane::startClicked, this, [this]() { setCurrentFrame(0); });
    connect(pane, &EditorPane::endClicked, this, [this]() { setCurrentFrame(m_timeline ? m_timeline->totalFrames() : 0); });
    connect(pane, &EditorPane::seekValueChanged, this, [this](int value) {
        if (m_ignoreSeekSignal) return;
        setCurrentFrame(value);
    });
    connect(pane, &EditorPane::audioMuteClicked, this, [this]() {
        const bool nextMuted = !m_preview->audioMuted();
        m_preview->setAudioMuted(nextMuted);
        if (m_audioEngine) m_audioEngine->setMuted(nextMuted);
        m_inspectorPane->refresh();
        scheduleSaveState();
    });
    connect(pane, &EditorPane::audioVolumeChanged, this, [this](int value) {
        m_preview->setAudioVolume(value / 100.0);
        if (m_audioEngine) m_audioEngine->setVolume(value / 100.0);
        m_inspectorPane->refresh();
    });

    m_timeline->seekRequested = [this](int64_t frame) { setCurrentFrame(frame); };
    m_timeline->clipsChanged = [this]() {
        syncSliderRange();
        m_preview->beginBulkUpdate();
        m_preview->setClipCount(m_timeline->clips().size());
        m_preview->setTimelineClips(m_timeline->clips());
        m_preview->setExportRanges(effectivePlaybackRanges());
        m_preview->setRenderSyncMarkers(m_timeline->renderSyncMarkers());
        m_preview->setSelectedClipId(m_timeline->selectedClipId());
        m_preview->endBulkUpdate();
        if (m_audioEngine) {
            m_audioEngine->setTimelineClips(m_timeline->clips());
            m_audioEngine->setExportRanges(effectivePlaybackRanges());
            m_audioEngine->setRenderSyncMarkers(m_timeline->renderSyncMarkers());
            m_audioEngine->setSpeechFilterFadeSamples(m_speechFilterFadeSamples);
        }
        refreshClipInspector();
        m_inspectorPane->refresh();
        scheduleSaveState();
        pushHistorySnapshot();
    };
    m_timeline->selectionChanged = [this]() {
        m_preview->setSelectedClipId(m_timeline->selectedClipId());
        refreshSyncInspector();
        m_transcriptTab->refresh();
        refreshClipInspector();
        m_outputTab->refresh();
        m_profileTab->refresh();
        m_gradingTab->refresh();
        m_videoKeyframeTab->refresh();
        m_inspectorPane->refresh();
    };
    m_timeline->renderSyncMarkersChanged = [this]() {
        m_preview->setRenderSyncMarkers(m_timeline->renderSyncMarkers());
        if (m_audioEngine) {
            m_audioEngine->setRenderSyncMarkers(m_timeline->renderSyncMarkers());
        }
        refreshSyncInspector();
        m_inspectorPane->refresh();
        scheduleSaveState();
        pushHistorySnapshot();
    };
    m_timeline->exportRangeChanged = [this]() {
        m_outputTab->refresh();
        m_inspectorPane->refresh();
        scheduleSaveState();
        pushHistorySnapshot();
    };
    m_timeline->gradingRequested = [this]() {
        focusGradingTab();
        m_inspectorPane->refresh();
    };
    m_timeline->transcribeRequested = [this](const QString &filePath, const QString &label) {
        openTranscriptionWindow(filePath, label);
    };
    m_timeline->createProxyRequested = [this](const QString &clipId) {
        createProxyForClip(clipId);
    };
    m_timeline->deleteProxyRequested = [this](const QString &clipId) {
        deleteProxyForClip(clipId);
    };
    m_preview->selectionRequested = [this](const QString &clipId) {
        if (m_timeline) m_timeline->setSelectedClipId(clipId);
    };
        m_preview->resizeRequested = [this](const QString &clipId, qreal scaleX, qreal scaleY, bool finalize) {
        if (!m_timeline) return;
        const int64_t currentFrame = m_timeline->currentFrame();
        const bool updated = m_timeline->updateClipById(clipId, [this, currentFrame, scaleX, scaleY](TimelineClip &clip) {
            if (clip.mediaType == ClipMediaType::Audio && clip.transcriptOverlay.enabled) {
                clip.transcriptOverlay.boxWidth = qMax<qreal>(80.0, scaleX);
                clip.transcriptOverlay.boxHeight = qMax<qreal>(40.0, scaleY);
                return;
            }
            if (!clipHasVisuals(clip)) return;
            const TimelineClip::TransformKeyframe offset = evaluateClipKeyframeOffsetAtFrame(clip, currentFrame);
            const int64_t keyframeFrame = qBound<int64_t>(0, currentFrame - clip.startFrame, qMax<int64_t>(0, clip.durationFrames - 1));
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
        if (!updated) return;
        m_preview->setTimelineClips(m_timeline->clips());
        m_preview->setRenderSyncMarkers(m_timeline->renderSyncMarkers());
        m_inspectorPane->refresh();
        scheduleSaveState();
        if (finalize) pushHistorySnapshot();
    };
    m_preview->moveRequested = [this](const QString &clipId, qreal translationX, qreal translationY, bool finalize) {
        if (!m_timeline) return;
        const int64_t currentFrame = m_timeline->currentFrame();
        const bool updated = m_timeline->updateClipById(clipId, [this, currentFrame, translationX, translationY](TimelineClip &clip) {
            if (clip.mediaType == ClipMediaType::Audio && clip.transcriptOverlay.enabled) {
                clip.transcriptOverlay.translationX = translationX;
                clip.transcriptOverlay.translationY = translationY;
                return;
            }
            if (!clipHasVisuals(clip)) return;
            const TimelineClip::TransformKeyframe offset = evaluateClipKeyframeOffsetAtFrame(clip, currentFrame);
            const int64_t keyframeFrame = qBound<int64_t>(0, currentFrame - clip.startFrame, qMax<int64_t>(0, clip.durationFrames - 1));
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
        if (!updated) return;
        m_preview->setTimelineClips(m_timeline->clips());
        m_preview->setRenderSyncMarkers(m_timeline->renderSyncMarkers());
        m_inspectorPane->refresh();
        scheduleSaveState();
        if (finalize) pushHistorySnapshot();
    };
    return pane;
}

void EditorWindow::addFileToTimeline(const QString &filePath)
{
    if (m_timeline) {
        m_timeline->addClipFromFile(filePath);
        m_preview->setTimelineClips(m_timeline->clips());
    }
}

void EditorWindow::syncSliderRange()
{
    const int64_t maxFrame = m_timeline->totalFrames();
    m_seekSlider->setRange(0, static_cast<int>(qMin<int64_t>(maxFrame, INT_MAX)));
}

void EditorWindow::focusGradingTab()
{
    if (m_inspectorTabs) {
        m_inspectorTabs->setCurrentIndex(0);
    }
}

void EditorWindow::updateTransportLabels()
{
    const bool playing = playbackActive();
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
    m_playButton->setIcon(style()->standardIcon(playing ? QStyle::SP_MediaPause : QStyle::SP_MediaPlay));
    m_audioMuteButton->setText(m_preview && m_preview->audioMuted() ? QStringLiteral("Unmute") : QStringLiteral("Mute"));
    m_audioNowPlayingLabel->setText(activeAudio.isEmpty() ? QStringLiteral("Audio idle") : QStringLiteral("Audio  %1").arg(activeAudio));
}

QString EditorWindow::frameToTimecode(int64_t frame) const
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

QJsonObject EditorWindow::profilingSnapshot() const
{
    const qint64 now = nowMs();
    QJsonObject snapshot{
        {QStringLiteral("playback_active"), m_playbackTimer.isActive()},
        {QStringLiteral("timeline_clip_count"), m_timeline ? m_timeline->clips().size() : 0},
        {QStringLiteral("current_frame"), m_timeline ? static_cast<qint64>(m_timeline->currentFrame()) : 0},
        {QStringLiteral("absolute_playback_sample"), static_cast<qint64>(m_absolutePlaybackSample)},
        {QStringLiteral("filtered_playback_sample"), static_cast<qint64>(m_filteredPlaybackSample)},
        {QStringLiteral("explorer_root"), m_explorerPane ? m_explorerPane->currentRootPath() : QString()},
        {QStringLiteral("debug"), debugControlsSnapshot()},
        {QStringLiteral("main_thread_heartbeat_ms"), m_lastMainThreadHeartbeatMs.load()},
        {QStringLiteral("last_playhead_advance_ms"), m_lastPlayheadAdvanceMs.load()},
        {QStringLiteral("main_thread_heartbeat_age_ms"), m_lastMainThreadHeartbeatMs.load() > 0 ? now - m_lastMainThreadHeartbeatMs.load() : -1},
        {QStringLiteral("last_playhead_advance_age_ms"), m_lastPlayheadAdvanceMs.load() > 0 ? now - m_lastPlayheadAdvanceMs.load() : -1}};

    if (m_preview) {
        snapshot[QStringLiteral("preview")] = m_preview->profilingSnapshot();
    }

    snapshot[QStringLiteral("export")] = QJsonObject{
        {QStringLiteral("active"), m_renderInProgress},
        {QStringLiteral("live"), m_liveRenderProfile},
        {QStringLiteral("last"), m_lastRenderProfile}};

    return snapshot;
}

void EditorWindow::syncTranscriptTableToPlayhead()
{
    if (!m_timeline || !m_transcriptTable || m_updatingTranscriptInspector) return;
    if (!m_transcriptFollowCurrentWordCheckBox || !m_transcriptFollowCurrentWordCheckBox->isChecked()) return;

    const TimelineClip *clip = m_timeline->selectedClip();
    if (!clip || clip->mediaType != ClipMediaType::Audio) {
        m_transcriptTable->clearSelection();
        return;
    }

    const int64_t sourceFrame = sourceFrameForClipAtTimelinePosition(*clip, samplesToFramePosition(m_absolutePlaybackSample), {});
    if (m_transcriptTab) {
        m_transcriptTab->syncTableToPlayhead(m_absolutePlaybackSample, sourceFrame);
    }
}

void EditorWindow::syncKeyframeTableToPlayhead()
{
    if (m_videoKeyframeTab) {
        m_videoKeyframeTab->syncTableToPlayhead();
    }
}

void EditorWindow::syncGradingTableToPlayhead()
{
    if (m_gradingTab) {
        m_gradingTab->syncTableToPlayhead();
    }
}

bool EditorWindow::focusInTranscriptTable() const
{
    QWidget *focus = QApplication::focusWidget();
    return m_transcriptTable && focus && (focus == m_transcriptTable || m_transcriptTable->isAncestorOf(focus));
}

bool EditorWindow::focusInKeyframeTable() const
{
    QWidget *focus = QApplication::focusWidget();
    return m_videoKeyframeTable && focus && (focus == m_videoKeyframeTable || m_videoKeyframeTable->isAncestorOf(focus));
}

bool EditorWindow::focusInGradingTable() const
{
    QWidget *focus = QApplication::focusWidget();
    return m_gradingKeyframeTable && focus && (focus == m_gradingKeyframeTable || m_gradingKeyframeTable->isAncestorOf(focus));
}

bool EditorWindow::focusInEditableInput() const
{
    QWidget *focus = QApplication::focusWidget();
    if (!focus) return false;
    
    if (qobject_cast<QLineEdit *>(focus) ||
        qobject_cast<QTextEdit *>(focus) ||
        qobject_cast<QPlainTextEdit *>(focus) ||
        qobject_cast<QAbstractSpinBox *>(focus))
    {
        return true;
    }
    if (auto *combo = qobject_cast<QComboBox *>(focus))
    {
        if (combo->isEditable()) return true;
    }
    for (QWidget *parent = focus->parentWidget(); parent; parent = parent->parentWidget())
    {
        if (qobject_cast<QLineEdit *>(parent) ||
            qobject_cast<QTextEdit *>(parent) ||
            qobject_cast<QPlainTextEdit *>(parent) ||
            qobject_cast<QAbstractSpinBox *>(parent) ||
            qobject_cast<QAbstractItemView *>(parent))
        {
            return true;
        }
    }
    return false;
}

bool EditorWindow::shouldBlockGlobalEditorShortcuts() const
{
    return focusInEditableInput();
}

void EditorWindow::initializeDeferredTimelineSeek(QTimer *timer, int64_t *pendingFrame)
{
    if (!timer || !pendingFrame) return;
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, this, [this, pendingFrame]()
    {
        if (!m_timeline || !pendingFrame || *pendingFrame < 0) return;
        setCurrentPlaybackSample(frameToSamples(*pendingFrame), false, true);
        *pendingFrame = -1;
    });
}

void EditorWindow::scheduleDeferredTimelineSeek(QTimer *timer, int64_t *pendingFrame, int64_t timelineFrame)
{
    if (!timer || !pendingFrame) return;
    *pendingFrame = timelineFrame;
    timer->start(QApplication::doubleClickInterval());
}

void EditorWindow::cancelDeferredTimelineSeek(QTimer *timer, int64_t *pendingFrame)
{
    if (timer) timer->stop();
    if (pendingFrame) *pendingFrame = -1;
}

void EditorWindow::undoHistory()
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

void EditorWindow::applyStateJson(const QJsonObject &root)
{
    m_loadingState = true;

    QString rootPath = root.value(QStringLiteral("explorerRoot")).toString(QDir::currentPath());
    QString galleryFolderPath = root.value(QStringLiteral("explorerGalleryPath")).toString();
    const int outputWidth = qMax(16, root.value(QStringLiteral("outputWidth")).toInt(1080));
    const int outputHeight = qMax(16, root.value(QStringLiteral("outputHeight")).toInt(1920));
    const QString outputFormat = root.value(QStringLiteral("outputFormat")).toString(QStringLiteral("mp4"));
    const QString lastRenderOutputPath = root.value(QStringLiteral("lastRenderOutputPath")).toString();
    const bool renderUseProxies = root.value(QStringLiteral("renderUseProxies")).toBool(false);
    const bool previewHideOutsideOutput = root.value(QStringLiteral("previewHideOutsideOutput")).toBool(false);
    const bool speechFilterEnabled = root.value(QStringLiteral("speechFilterEnabled")).toBool(false);
    const int transcriptPrependMs = root.value(QStringLiteral("transcriptPrependMs")).toInt(0);
    const int transcriptPostpendMs = root.value(QStringLiteral("transcriptPostpendMs")).toInt(0);
    const int speechFilterFadeSamples = root.value(QStringLiteral("speechFilterFadeSamples")).toInt(250);
    const bool transcriptFollowCurrentWord = root.value(QStringLiteral("transcriptFollowCurrentWord")).toBool(true);
    const bool gradingFollowCurrent = root.value(QStringLiteral("gradingFollowCurrent")).toBool(true);
    const bool gradingAutoScroll = root.value(QStringLiteral("gradingAutoScroll")).toBool(true);
    const bool keyframesFollowCurrent = root.value(QStringLiteral("keyframesFollowCurrent")).toBool(true);
    const bool keyframesAutoScroll = root.value(QStringLiteral("keyframesAutoScroll")).toBool(true);
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
        if (!value.isObject()) continue;
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
        if (!path.isEmpty()) expandedExplorerPaths.push_back(path);
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
        if (!value.isObject()) continue;
        TimelineClip clip = clipFromJson(value.toObject());
        if (clip.trackIndex < 0) clip.trackIndex = loadedClips.size();
        if (!clip.filePath.isEmpty()) loadedClips.push_back(clip);
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
        if (!value.isObject()) continue;
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
    
    if (m_outputWidthSpin) { QSignalBlocker block(m_outputWidthSpin); m_outputWidthSpin->setValue(outputWidth); }
    if (m_outputHeightSpin) { QSignalBlocker block(m_outputHeightSpin); m_outputHeightSpin->setValue(outputHeight); }
    if (m_outputFormatCombo) {
        QSignalBlocker block(m_outputFormatCombo);
        const int formatIndex = m_outputFormatCombo->findData(outputFormat);
        if (formatIndex >= 0) m_outputFormatCombo->setCurrentIndex(formatIndex);
    }
    m_lastRenderOutputPath = lastRenderOutputPath;
    if (m_renderUseProxiesCheckBox) {
        QSignalBlocker block(m_renderUseProxiesCheckBox);
        m_renderUseProxiesCheckBox->setChecked(renderUseProxies);
    }
    if (m_previewHideOutsideOutputCheckBox) {
        QSignalBlocker block(m_previewHideOutsideOutputCheckBox);
        m_previewHideOutsideOutputCheckBox->setChecked(previewHideOutsideOutput);
    }
    if (m_speechFilterEnabledCheckBox) { QSignalBlocker block(m_speechFilterEnabledCheckBox); m_speechFilterEnabledCheckBox->setChecked(speechFilterEnabled); }
    
    m_transcriptPrependMs = transcriptPrependMs;
    m_transcriptPostpendMs = transcriptPostpendMs;
    m_speechFilterFadeSamples = qMax(0, speechFilterFadeSamples);
    
    if (m_transcriptPrependMsSpin) { QSignalBlocker block(m_transcriptPrependMsSpin); m_transcriptPrependMsSpin->setValue(m_transcriptPrependMs); }
    if (m_transcriptPostpendMsSpin) { QSignalBlocker block(m_transcriptPostpendMsSpin); m_transcriptPostpendMsSpin->setValue(m_transcriptPostpendMs); }
    if (m_speechFilterFadeSamplesSpin) { QSignalBlocker block(m_speechFilterFadeSamplesSpin); m_speechFilterFadeSamplesSpin->setValue(m_speechFilterFadeSamples); }
    
    if (m_transcriptFollowCurrentWordCheckBox) { QSignalBlocker block(m_transcriptFollowCurrentWordCheckBox); m_transcriptFollowCurrentWordCheckBox->setChecked(transcriptFollowCurrentWord); }
    if (m_gradingFollowCurrentCheckBox) { QSignalBlocker block(m_gradingFollowCurrentCheckBox); m_gradingFollowCurrentCheckBox->setChecked(gradingFollowCurrent); }
    if (m_gradingAutoScrollCheckBox) { QSignalBlocker block(m_gradingAutoScrollCheckBox); m_gradingAutoScrollCheckBox->setChecked(gradingAutoScroll); }
    if (m_keyframesFollowCurrentCheckBox) { QSignalBlocker block(m_keyframesFollowCurrentCheckBox); m_keyframesFollowCurrentCheckBox->setChecked(keyframesFollowCurrent); }
    if (m_keyframesAutoScrollCheckBox) { QSignalBlocker block(m_keyframesAutoScrollCheckBox); m_keyframesAutoScrollCheckBox->setChecked(keyframesAutoScroll); }
    
    if (m_inspectorTabs && m_inspectorTabs->count() > 0) {
        QSignalBlocker block(m_inspectorTabs);
        m_inspectorTabs->setCurrentIndex(qBound(0, selectedInspectorTab, m_inspectorTabs->count() - 1));
    }
    
    if (m_preview) {
        m_preview->setOutputSize(QSize(outputWidth, outputHeight));
        m_preview->setHideOutsideOutputWindow(previewHideOutsideOutput);
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
    
    m_preview->beginBulkUpdate();
    m_preview->setClipCount(m_timeline->clips().size());
    m_preview->setTimelineClips(m_timeline->clips());
    m_preview->setExportRanges(effectivePlaybackRanges());
    m_preview->setRenderSyncMarkers(m_timeline->renderSyncMarkers());
    m_preview->setSelectedClipId(selectedClipId);
    m_preview->endBulkUpdate();
    
    if (m_audioEngine) {
        m_audioEngine->setTimelineClips(m_timeline->clips());
        m_audioEngine->setExportRanges(effectivePlaybackRanges());
        m_audioEngine->setRenderSyncMarkers(m_timeline->renderSyncMarkers());
        m_audioEngine->setSpeechFilterFadeSamples(m_speechFilterFadeSamples);
        m_audioEngine->seek(currentFrame);
    }
    
    setCurrentFrame(currentFrame);

    m_playbackTimer.stop();
    m_fastPlaybackActive.store(false);
    m_preview->setPlaybackState(false);
    if (m_audioEngine) {
        m_audioEngine->stop();
    }
    updateTransportLabels();

    m_loadingState = false;
    QTimer::singleShot(0, this, [this, resolvedRootPath]() {
        if (m_explorerPane) {
            m_explorerPane->setInitialRootPath(resolvedRootPath);
        }
        refreshProjectsList();
        m_inspectorPane->refresh();
    });
}

void EditorWindow::advanceFrame()
{
    if (!m_timeline) return;

    if (m_audioEngine && m_audioEngine->hasPlayableAudio()) {
        int64_t audioSample = qMax<int64_t>(0, m_audioEngine->currentSample());
        const qreal audioFramePosition = samplesToFramePosition(audioSample);
        const int64_t audioFrame = qBound<int64_t>(0, static_cast<int64_t>(std::floor(audioFramePosition)), m_timeline->totalFrames());

        if (audioFrame == m_timeline->currentFrame()) {
            if (m_preview) m_preview->setCurrentPlaybackSample(audioSample);
            return;
        }

        if (m_preview) m_preview->preparePlaybackAdvanceSample(audioSample);
        setCurrentPlaybackSample(audioSample, false, true);
        return;
    }

    const int64_t nextFrame = nextPlaybackFrame(m_timeline->currentFrame());
    if (m_preview) m_preview->preparePlaybackAdvance(nextFrame);
    setCurrentPlaybackSample(frameToSamples(nextFrame), false, true);
}

bool EditorWindow::speechFilterPlaybackEnabled() const
{
    return m_speechFilterEnabledCheckBox && m_speechFilterEnabledCheckBox->isChecked();
}

int64_t EditorWindow::filteredPlaybackSampleForAbsoluteSample(int64_t absoluteSample) const
{
    const QVector<ExportRangeSegment> ranges = effectivePlaybackRanges();
    if (ranges.isEmpty()) return qMax<int64_t>(0, absoluteSample);

    int64_t filteredSample = 0;
    for (const ExportRangeSegment &range : ranges) {
        const int64_t rangeStartSample = frameToSamples(range.startFrame);
        const int64_t rangeEndSampleExclusive = frameToSamples(range.endFrame + 1);
        if (absoluteSample <= rangeStartSample) return filteredSample;
        if (absoluteSample < rangeEndSampleExclusive) return filteredSample + (absoluteSample - rangeStartSample);
        filteredSample += (rangeEndSampleExclusive - rangeStartSample);
    }
    return filteredSample;
}

QVector<ExportRangeSegment> EditorWindow::effectivePlaybackRanges() const
{
    if (!m_timeline) return {};
    QVector<ExportRangeSegment> ranges = m_timeline->exportRanges();
    if (!speechFilterPlaybackEnabled()) return ranges;
    return m_transcriptEngine.transcriptWordExportRanges(ranges,
                                                         m_timeline->clips(),
                                                         m_timeline->renderSyncMarkers(),
                                                         m_transcriptPrependMs,
                                                         m_transcriptPostpendMs);
}
int64_t EditorWindow::nextPlaybackFrame(int64_t currentFrame) const
{
    if (!m_timeline) return 0;

    const QVector<ExportRangeSegment> ranges = effectivePlaybackRanges();
    if (ranges.isEmpty()) {
        const int64_t nextFrame = currentFrame + 1;
        return nextFrame > m_timeline->totalFrames() ? 0 : nextFrame;
    }

    for (const ExportRangeSegment &range : ranges) {
        if (currentFrame < range.startFrame) return range.startFrame;
        if (currentFrame >= range.startFrame && currentFrame < range.endFrame) return currentFrame + 1;
    }
    return ranges.constFirst().startFrame;
}

QString EditorWindow::clipLabelForId(const QString &clipId) const
{
    if (!m_timeline) return clipId;
    for (const TimelineClip &clip : m_timeline->clips()) {
        if (clip.id == clipId) return clip.label;
    }
    return clipId;
}

QColor EditorWindow::clipColorForId(const QString &clipId) const
{
    if (!m_timeline) return QColor(QStringLiteral("#24303c"));
    for (const TimelineClip &clip : m_timeline->clips()) {
        if (clip.id == clipId) return clip.color;
    }
    return QColor(QStringLiteral("#24303c"));
}


bool EditorWindow::parseSyncActionText(const QString &text, RenderSyncAction *actionOut) const
{
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

void EditorWindow::refreshSyncInspector()
{
    m_syncInspectorClipLabel->setText(QStringLiteral("Sync"));
    m_updatingSyncInspector = true;
    const QSet<int64_t> selectedFrames =
        editor::collectSelectedFrameRoles(m_syncTable);
    m_syncTable->clearContents();
    m_syncTable->setRowCount(0);

    const TimelineClip* selectedClip = m_timeline ? m_timeline->selectedClip() : nullptr;
    if (!selectedClip) {
        m_syncInspectorDetailsLabel->setText(QStringLiteral("Select a clip to inspect its sync markers."));
        m_updatingSyncInspector = false;
        return;
    }

    m_syncInspectorClipLabel->setText(QStringLiteral("Sync\n%1").arg(selectedClip->label));

    QVector<RenderSyncMarker> markers;
    if (m_timeline) {
        const QVector<RenderSyncMarker> allMarkers = m_timeline->renderSyncMarkers();
        markers.reserve(allMarkers.size());
        for (const RenderSyncMarker& marker : allMarkers) {
            if (marker.clipId == selectedClip->id) {
                markers.push_back(marker);
            }
        }
    }

    if (markers.isEmpty()) {
        m_syncInspectorDetailsLabel->setText(QStringLiteral("No render sync markers for the selected clip."));
        m_updatingSyncInspector = false;
        return;
    }

    m_syncInspectorDetailsLabel->setText(
        QStringLiteral("%1 sync markers for the selected clip. Edit Frame, Count, or Action directly.")
            .arg(markers.size()));
    m_syncTable->setRowCount(markers.size());
    
    for (int i = 0; i < markers.size(); ++i) {
        const RenderSyncMarker &marker = markers[i];
        const QColor clipColor = clipColorForId(marker.clipId);
        const QColor rowBackground = QColor(clipColor.red(), clipColor.green(), clipColor.blue(), 72);
        const QColor rowForeground = QColor(QStringLiteral("#f4f7fb"));
        const QString clipLabel = clipLabelForId(marker.clipId);
        
        auto *clipItem = new QTableWidgetItem(QString());
        clipItem->setFlags(clipItem->flags() & ~Qt::ItemIsEditable);
        clipItem->setToolTip(clipLabel);
        
        auto *frameItem = new QTableWidgetItem(QString::number(marker.frame));
        auto *countItem = new QTableWidgetItem(QString::number(marker.count));
        auto *actionItem = new QTableWidgetItem(
            marker.action == RenderSyncAction::DuplicateFrame ? QStringLiteral("Duplicate") : QStringLiteral("Skip"));

        for (QTableWidgetItem *item : {clipItem, frameItem, countItem, actionItem}) {
            item->setData(Qt::UserRole, QVariant::fromValue(static_cast<qint64>(marker.frame)));
            item->setData(Qt::UserRole + 1, marker.clipId);
            item->setBackground(rowBackground);
            item->setForeground(rowForeground);
        }
        
        m_syncTable->setItem(i, 0, clipItem);
        m_syncTable->setItem(i, 1, frameItem);
        m_syncTable->setItem(i, 2, countItem);
        m_syncTable->setItem(i, 3, actionItem);
    }
    editor::restoreSelectionByFrameRole(m_syncTable, selectedFrames);
    m_updatingSyncInspector = false;
}

void EditorWindow::onSyncTableSelectionChanged()
{
    if (m_updatingSyncInspector || !m_syncTable) {
        return;
    }
    const int64_t primaryFrame = editor::primarySelectedFrameRole(m_syncTable);
    if (primaryFrame < 0) {
        return;
    }
    scheduleDeferredTimelineSeek(&m_syncClickSeekTimer,
                                 &m_pendingSyncClickTimelineFrame,
                                 primaryFrame);
}

void EditorWindow::onSyncTableItemChanged(QTableWidgetItem* item)
{
    if (m_updatingSyncInspector || !item || !m_timeline || !m_syncTable) {
        return;
    }

    const int row = item->row();
    if (row < 0 || row >= m_syncTable->rowCount()) {
        return;
    }

    auto tableText = [this, row](int column) -> QString {
        QTableWidgetItem* tableItem = m_syncTable->item(row, column);
        return tableItem ? tableItem->text().trimmed() : QString();
    };

    const QString clipId = m_syncTable->item(row, 0)
                               ? m_syncTable->item(row, 0)->data(Qt::UserRole + 1).toString()
                               : QString();
    const int64_t originalFrame = m_syncTable->item(row, 0)
                                      ? m_syncTable->item(row, 0)->data(Qt::UserRole).toLongLong()
                                      : -1;
    if (clipId.isEmpty() || originalFrame < 0) {
        refreshSyncInspector();
        return;
    }

    bool ok = false;
    RenderSyncMarker edited;
    edited.clipId = clipId;
    edited.frame = tableText(1).toLongLong(&ok);
    if (!ok) { refreshSyncInspector(); return; }
    edited.count = tableText(2).toInt(&ok);
    if (!ok) { refreshSyncInspector(); return; }
    edited.count = qMax(1, edited.count);
    if (!parseSyncActionText(tableText(3), &edited.action)) {
        refreshSyncInspector();
        return;
    }

    QVector<RenderSyncMarker> markers = m_timeline->renderSyncMarkers();
    bool replaced = false;
    for (RenderSyncMarker& marker : markers) {
        if (marker.clipId == clipId && marker.frame == originalFrame) {
            marker = edited;
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        refreshSyncInspector();
        return;
    }

    std::sort(markers.begin(), markers.end(), [](const RenderSyncMarker& a, const RenderSyncMarker& b) {
        if (a.frame == b.frame) {
            return a.clipId < b.clipId;
        }
        return a.frame < b.frame;
    });
    m_timeline->setRenderSyncMarkers(markers);
    refreshSyncInspector();
    scheduleSaveState();
}

void EditorWindow::onSyncTableItemDoubleClicked(QTableWidgetItem* item)
{
    Q_UNUSED(item);
    cancelDeferredTimelineSeek(&m_syncClickSeekTimer, &m_pendingSyncClickTimelineFrame);
}

void EditorWindow::onSyncTableCustomContextMenu(const QPoint& pos)
{
    if (!m_syncTable || !m_timeline) {
        return;
    }

    int row = -1;
    QTableWidgetItem* item = editor::ensureContextRowSelected(m_syncTable, pos, &row);
    if (!item) {
        return;
    }

    const QString clipId = item->data(Qt::UserRole + 1).toString();
    const int64_t frame = item->data(Qt::UserRole).toLongLong();
    if (clipId.isEmpty() || frame < 0) {
        return;
    }

    QMenu menu;
    QAction* copyToCurrentPlayheadAction = menu.addAction(QStringLiteral("Copy to Current Playhead"));
    copyToCurrentPlayheadAction->setEnabled(frame != m_timeline->currentFrame());
    QAction* deleteAction = menu.addAction(QStringLiteral("Delete"));
    QAction* chosen = menu.exec(m_syncTable->viewport()->mapToGlobal(pos));
    if (!chosen) {
        return;
    }

    if (chosen == deleteAction) {
        QVector<RenderSyncMarker> markers = m_timeline->renderSyncMarkers();
        const auto newEnd = std::remove_if(markers.begin(),
                                           markers.end(),
                                           [&](const RenderSyncMarker& marker) {
                                               return marker.clipId == clipId && marker.frame == frame;
                                           });
        if (newEnd == markers.end()) {
            return;
        }
        markers.erase(newEnd, markers.end());
        m_timeline->setRenderSyncMarkers(markers);
        refreshSyncInspector();
        scheduleSaveState();
        return;
    }

    if (chosen != copyToCurrentPlayheadAction || !copyToCurrentPlayheadAction->isEnabled()) {
        return;
    }

    QVector<RenderSyncMarker> markers = m_timeline->renderSyncMarkers();
    RenderSyncMarker sourceMarker;
    bool foundSource = false;
    for (const RenderSyncMarker& marker : markers) {
        if (marker.clipId == clipId && marker.frame == frame) {
            sourceMarker = marker;
            foundSource = true;
            break;
        }
    }
    if (!foundSource) {
        return;
    }

    sourceMarker.frame = m_timeline->currentFrame();
    bool replaced = false;
    for (RenderSyncMarker& marker : markers) {
        if (marker.clipId == sourceMarker.clipId && marker.frame == sourceMarker.frame) {
            marker = sourceMarker;
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        markers.push_back(sourceMarker);
    }
    std::sort(markers.begin(), markers.end(), [](const RenderSyncMarker& a, const RenderSyncMarker& b) {
        if (a.frame == b.frame) {
            return a.clipId < b.clipId;
        }
        return a.frame < b.frame;
    });
    m_timeline->setRenderSyncMarkers(markers);
    refreshSyncInspector();
    scheduleSaveState();
}

void EditorWindow::refreshClipInspector()
{
    const TimelineClip *clip = m_timeline ? m_timeline->selectedClip() : nullptr;
    const TimelineTrack *track = m_timeline ? m_timeline->selectedTrack() : nullptr;
    const int selectedTrackIndex = m_timeline ? m_timeline->selectedTrackIndex() : -1;

    auto disableTrackControls = [this]() {
        if (m_trackNameEdit) {
            QSignalBlocker blocker(m_trackNameEdit);
            m_trackNameEdit->setText(QString());
            m_trackNameEdit->setEnabled(false);
        }
        if (m_trackHeightSpin) {
            QSignalBlocker blocker(m_trackHeightSpin);
            m_trackHeightSpin->setValue(44);
            m_trackHeightSpin->setEnabled(false);
        }
        if (m_trackVideoEnabledCheckBox) {
            QSignalBlocker blocker(m_trackVideoEnabledCheckBox);
            m_trackVideoEnabledCheckBox->setChecked(false);
            m_trackVideoEnabledCheckBox->setEnabled(false);
        }
        if (m_trackAudioEnabledCheckBox) {
            QSignalBlocker blocker(m_trackAudioEnabledCheckBox);
            m_trackAudioEnabledCheckBox->setChecked(false);
            m_trackAudioEnabledCheckBox->setEnabled(false);
        }
        if (m_trackCrossfadeSecondsSpin) {
            m_trackCrossfadeSecondsSpin->setEnabled(false);
        }
        if (m_trackCrossfadeButton) {
            m_trackCrossfadeButton->setEnabled(false);
        }
    };

    if (!clip) {
        m_clipInspectorClipLabel->setText(track ? QStringLiteral("No clip selected") : QStringLiteral("No clip or track selected"));
        m_clipProxyUsageLabel->setText(QStringLiteral("Proxy In Use: No"));
        m_clipPlaybackSourceLabel->setText(QStringLiteral("Playback Source: None"));
        m_clipOriginalInfoLabel->setText(QStringLiteral("Original\nNo clip selected."));
        m_clipProxyInfoLabel->setText(QStringLiteral("Proxy\nNo proxy configured."));
        if (m_clipPlaybackRateSpin) {
            QSignalBlocker block(m_clipPlaybackRateSpin);
            m_clipPlaybackRateSpin->setValue(1.0);
            m_clipPlaybackRateSpin->setEnabled(false);
        }
    } else {
        const QString proxyPath = playbackProxyPathForClip(*clip);
        const QString playbackPath = playbackMediaPathForClip(*clip);
        
        MediaProbeResult originalProbe;
        originalProbe.mediaType = clip->mediaType;
        originalProbe.sourceKind = clip->sourceKind;
        originalProbe.hasAudio = clip->hasAudio;
        originalProbe.hasVideo = clipHasVisuals(*clip);
        originalProbe.durationFrames = clip->sourceDurationFrames > 0 ? clip->sourceDurationFrames : clip->durationFrames;
        
        m_clipInspectorClipLabel->setText(QStringLiteral("%1\n%2")
            .arg(clip->label,
                 QStringLiteral("%1 | %2")
                     .arg(clipMediaTypeLabel(clip->mediaType),
                          mediaSourceKindLabel(clip->sourceKind))));
        m_clipProxyUsageLabel->setText(QStringLiteral("Proxy In Use: %1")
            .arg(playbackPath != clip->filePath ? QStringLiteral("Yes") : QStringLiteral("No")));
        m_clipPlaybackSourceLabel->setText(QStringLiteral("Playback Source\n%1")
            .arg(QDir::toNativeSeparators(playbackPath)));
        if (m_clipPlaybackRateSpin) {
            QSignalBlocker block(m_clipPlaybackRateSpin);
            m_clipPlaybackRateSpin->setValue(clip->playbackRate);
            m_clipPlaybackRateSpin->setEnabled(clipHasVisuals(*clip));
            m_clipPlaybackRateSpin->setToolTip(
                clip->hasAudio
                    ? QStringLiteral("Visual retime control. Audio playback is not time-stretched.")
                    : QStringLiteral("Playback speed multiplier for this clip."));
        }
        m_clipOriginalInfoLabel->setText(QStringLiteral("Original\n%1")
            .arg(clipFileInfoSummary(clip->filePath, &originalProbe)));
        
        if (proxyPath.isEmpty()) {
            const QString configuredProxyPath = clip->proxyPath.isEmpty()
                ? defaultProxyOutputPath(*clip)
                : clip->proxyPath;
            m_clipProxyInfoLabel->setText(QStringLiteral("Proxy\nConfigured: No\nPath: %1")
                .arg(QDir::toNativeSeparators(configuredProxyPath)));
        } else {
            m_clipProxyInfoLabel->setText(QStringLiteral("Proxy\n%1")
                .arg(clipFileInfoSummary(proxyPath)));
        }
    }

    if (!track || selectedTrackIndex < 0 || clip) {
        if (m_trackInspectorLabel) {
            m_trackInspectorLabel->setText(QStringLiteral("No track selected"));
        }
        if (m_trackInspectorDetailsLabel) {
            m_trackInspectorDetailsLabel->setText(QStringLiteral("Select a track header to edit track-wide properties."));
        }
        disableTrackControls();
        return;
    }

    int clipCount = 0;
    int visualCount = 0;
    int audioCount = 0;
    bool allVisualEnabled = true;
    bool allAudioEnabled = true;
    for (const TimelineClip& timelineClip : m_timeline->clips()) {
        if (timelineClip.trackIndex != selectedTrackIndex) {
            continue;
        }
        ++clipCount;
        if (clipHasVisuals(timelineClip)) {
            ++visualCount;
            allVisualEnabled = allVisualEnabled && timelineClip.videoEnabled;
        }
        if (timelineClip.hasAudio) {
            ++audioCount;
            allAudioEnabled = allAudioEnabled && timelineClip.audioEnabled;
        }
    }

    if (m_trackInspectorLabel) {
        m_trackInspectorLabel->setText(QStringLiteral("Track %1\n%2")
                                           .arg(selectedTrackIndex + 1)
                                           .arg(track->name));
    }
    if (m_trackInspectorDetailsLabel) {
        m_trackInspectorDetailsLabel->setText(QStringLiteral("%1 clips | %2 visual | %3 audio")
                                                  .arg(clipCount)
                                                  .arg(visualCount)
                                                  .arg(audioCount));
    }
    if (m_trackNameEdit) {
        QSignalBlocker blocker(m_trackNameEdit);
        m_trackNameEdit->setText(track->name);
        m_trackNameEdit->setEnabled(true);
    }
    if (m_trackHeightSpin) {
        QSignalBlocker blocker(m_trackHeightSpin);
        m_trackHeightSpin->setValue(track->height);
        m_trackHeightSpin->setEnabled(true);
    }
    if (m_trackVideoEnabledCheckBox) {
        QSignalBlocker blocker(m_trackVideoEnabledCheckBox);
        m_trackVideoEnabledCheckBox->setChecked(visualCount > 0 ? allVisualEnabled : false);
        m_trackVideoEnabledCheckBox->setEnabled(visualCount > 0);
    }
    if (m_trackAudioEnabledCheckBox) {
        QSignalBlocker blocker(m_trackAudioEnabledCheckBox);
        m_trackAudioEnabledCheckBox->setChecked(audioCount > 0 ? allAudioEnabled : false);
        m_trackAudioEnabledCheckBox->setEnabled(audioCount > 0);
    }
    if (m_trackCrossfadeSecondsSpin) {
        m_trackCrossfadeSecondsSpin->setEnabled(clipCount > 1);
    }
    if (m_trackCrossfadeButton) {
        m_trackCrossfadeButton->setEnabled(clipCount > 1);
    }
}

void EditorWindow::refreshOutputInspector()
{
    if (m_outputTab) {
        m_outputTab->refresh();
    }
}

void EditorWindow::applyOutputRangeFromInspector()
{
    if (m_outputTab) {
        m_outputTab->applyRangeFromInspector();
    }
}

void EditorWindow::renderFromOutputInspector()
{
    if (m_outputTab) {
        m_outputTab->renderFromInspector();
    }
}

void EditorWindow::renderTimelineFromOutputRequest(const RenderRequest &request)
{
    RenderRequest effectiveRequest = request;
    if (effectiveRequest.useProxyMedia)
    {
        for (TimelineClip &clip : effectiveRequest.clips)
        {
            const QString proxyPath = playbackProxyPathForClip(clip);
            if (!proxyPath.isEmpty())
            {
                clip.filePath = proxyPath;
            }
        }
    }

    int64_t totalFramesToRender = 0;
    for (const ExportRangeSegment &range : std::as_const(effectiveRequest.exportRanges))
    {
        totalFramesToRender += qMax<int64_t>(0, range.endFrame - range.startFrame + 1);
    }
    if (totalFramesToRender <= 0)
    {
        totalFramesToRender = qMax<int64_t>(1, effectiveRequest.exportEndFrame - effectiveRequest.exportStartFrame + 1);
    }

    QDialog progressDialog(this);
    progressDialog.setWindowTitle(QStringLiteral("Render Export"));
    progressDialog.setWindowModality(Qt::ApplicationModal);
    progressDialog.setMinimumWidth(560);
    progressDialog.setStyleSheet(QStringLiteral(
        "QDialog { background: #f6f3ee; }"
        "QLabel { color: #1f2430; font-size: 13px; }"
        "QProgressBar { border: 1px solid #c9c2b8; border-radius: 6px; text-align: center; background: #ffffff; min-height: 20px; }"
        "QProgressBar::chunk { background: #2f7d67; border-radius: 5px; }"
        "QPushButton { min-width: 96px; padding: 6px 14px; }"));
    auto *progressLayout = new QVBoxLayout(&progressDialog);
    progressLayout->setContentsMargins(16, 16, 16, 16);
    progressLayout->setSpacing(10);

    auto *renderStatusLabel = new QLabel(QStringLiteral("Preparing render..."), &progressDialog);
    renderStatusLabel->setWordWrap(true);
    renderStatusLabel->setAlignment(Qt::AlignCenter);
    progressLayout->addWidget(renderStatusLabel);

    auto *showRenderPreviewCheckBox = new QCheckBox(QStringLiteral("Show Visual Preview"), &progressDialog);
    showRenderPreviewCheckBox->setChecked(true);
    progressLayout->addWidget(showRenderPreviewCheckBox, 0, Qt::AlignLeft);

    auto *renderPreviewLabel = new QLabel(&progressDialog);
    renderPreviewLabel->setAlignment(Qt::AlignCenter);
    renderPreviewLabel->setMinimumSize(360, 202);
    renderPreviewLabel->setStyleSheet(QStringLiteral(
        "QLabel { background: #11151c; color: #d9e1ea; border: 1px solid #c9c2b8; border-radius: 6px; }"));
    renderPreviewLabel->setText(QStringLiteral("Waiting for first rendered frame..."));
    progressLayout->addWidget(renderPreviewLabel);

    auto *renderProgressBar = new QProgressBar(&progressDialog);
    renderProgressBar->setRange(0, static_cast<int>(qMin<int64_t>(totalFramesToRender, std::numeric_limits<int>::max())));
    renderProgressBar->setValue(0);
    progressLayout->addWidget(renderProgressBar);

    auto *buttonRow = new QHBoxLayout;
    buttonRow->addStretch(1);
    auto *cancelRenderButton = new QPushButton(QStringLiteral("Cancel"), &progressDialog);
    buttonRow->addWidget(cancelRenderButton);
    progressLayout->addLayout(buttonRow);

    bool renderCancelled = false;
    QObject::connect(cancelRenderButton, &QPushButton::clicked, &progressDialog, [&renderCancelled, cancelRenderButton]() {
        renderCancelled = true;
        cancelRenderButton->setEnabled(false);
    });
    QObject::connect(showRenderPreviewCheckBox, &QCheckBox::toggled, &progressDialog, [renderPreviewLabel](bool checked) {
        renderPreviewLabel->setVisible(checked);
    });
    progressDialog.show();

    const QString outputPath = effectiveRequest.outputPath;
    const auto formatEta = [](qint64 remainingMs) -> QString
    {
        if (remainingMs <= 0)
        {
            return QStringLiteral("calculating...");
        }
        const qint64 totalSeconds = remainingMs / 1000;
        const qint64 hours = totalSeconds / 3600;
        const qint64 minutes = (totalSeconds % 3600) / 60;
        const qint64 seconds = totalSeconds % 60;
        if (hours > 0)
        {
            return QStringLiteral("%1h %2m %3s").arg(hours).arg(minutes).arg(seconds);
        }
        if (minutes > 0)
        {
            return QStringLiteral("%1m %2s").arg(minutes).arg(seconds);
        }
        return QStringLiteral("%1s").arg(seconds);
    };
    const auto stageSummary = [](qint64 stageMs, int64_t completedFrames) -> QString
    {
        if (stageMs <= 0 || completedFrames <= 0)
        {
            return QStringLiteral("0 ms");
        }
        return QStringLiteral("%1 ms total (%2 ms/frame)")
            .arg(stageMs)
            .arg(QString::number(static_cast<double>(stageMs) / static_cast<double>(completedFrames), 'f', 2));
    };
    const auto renderProfileFromProgress = [&formatEta](const RenderProgress &progress) -> QJsonObject
    {
        const qint64 completedFrames = qMax<int64_t>(1, progress.framesCompleted);
        const double fps = progress.elapsedMs > 0
                               ? (1000.0 * static_cast<double>(progress.framesCompleted)) / static_cast<double>(progress.elapsedMs)
                               : 0.0;
        return QJsonObject{
            {QStringLiteral("status"), QStringLiteral("running")},
            {QStringLiteral("output_path"), QString()},
            {QStringLiteral("frames_completed"), static_cast<qint64>(progress.framesCompleted)},
            {QStringLiteral("total_frames"), static_cast<qint64>(progress.totalFrames)},
            {QStringLiteral("segment_index"), progress.segmentIndex},
            {QStringLiteral("segment_count"), progress.segmentCount},
            {QStringLiteral("timeline_frame"), static_cast<qint64>(progress.timelineFrame)},
            {QStringLiteral("segment_start_frame"), static_cast<qint64>(progress.segmentStartFrame)},
            {QStringLiteral("segment_end_frame"), static_cast<qint64>(progress.segmentEndFrame)},
            {QStringLiteral("using_gpu"), progress.usingGpu},
            {QStringLiteral("using_hardware_encode"), progress.usingHardwareEncode},
            {QStringLiteral("encoder_label"), progress.encoderLabel},
            {QStringLiteral("elapsed_ms"), progress.elapsedMs},
            {QStringLiteral("estimated_remaining_ms"), progress.estimatedRemainingMs},
            {QStringLiteral("eta_text"), formatEta(progress.estimatedRemainingMs)},
            {QStringLiteral("fps"), fps},
            {QStringLiteral("render_stage_ms"), progress.renderStageMs},
            {QStringLiteral("render_decode_stage_ms"), progress.renderDecodeStageMs},
            {QStringLiteral("render_texture_stage_ms"), progress.renderTextureStageMs},
            {QStringLiteral("render_composite_stage_ms"), progress.renderCompositeStageMs},
            {QStringLiteral("render_nv12_stage_ms"), progress.renderNv12StageMs},
            {QStringLiteral("gpu_readback_ms"), progress.gpuReadbackMs},
            {QStringLiteral("overlay_stage_ms"), progress.overlayStageMs},
            {QStringLiteral("convert_stage_ms"), progress.convertStageMs},
            {QStringLiteral("encode_stage_ms"), progress.encodeStageMs},
            {QStringLiteral("audio_stage_ms"), progress.audioStageMs},
            {QStringLiteral("max_frame_render_stage_ms"), progress.maxFrameRenderStageMs},
            {QStringLiteral("max_frame_decode_stage_ms"), progress.maxFrameDecodeStageMs},
            {QStringLiteral("max_frame_texture_stage_ms"), progress.maxFrameTextureStageMs},
            {QStringLiteral("max_frame_readback_stage_ms"), progress.maxFrameReadbackStageMs},
            {QStringLiteral("max_frame_convert_stage_ms"), progress.maxFrameConvertStageMs},
            {QStringLiteral("skipped_clips"), progress.skippedClips},
            {QStringLiteral("skipped_clip_reason_counts"), progress.skippedClipReasonCounts},
            {QStringLiteral("render_stage_table"), progress.renderStageTable},
            {QStringLiteral("worst_frame_table"), progress.worstFrameTable},
            {QStringLiteral("render_stage_per_frame_ms"), static_cast<double>(progress.renderStageMs) / static_cast<double>(completedFrames)},
            {QStringLiteral("render_decode_stage_per_frame_ms"), static_cast<double>(progress.renderDecodeStageMs) / static_cast<double>(completedFrames)},
            {QStringLiteral("render_texture_stage_per_frame_ms"), static_cast<double>(progress.renderTextureStageMs) / static_cast<double>(completedFrames)},
            {QStringLiteral("render_composite_stage_per_frame_ms"), static_cast<double>(progress.renderCompositeStageMs) / static_cast<double>(completedFrames)},
            {QStringLiteral("render_nv12_stage_per_frame_ms"), static_cast<double>(progress.renderNv12StageMs) / static_cast<double>(completedFrames)},
            {QStringLiteral("gpu_readback_per_frame_ms"), static_cast<double>(progress.gpuReadbackMs) / static_cast<double>(completedFrames)},
            {QStringLiteral("overlay_stage_per_frame_ms"), static_cast<double>(progress.overlayStageMs) / static_cast<double>(completedFrames)},
            {QStringLiteral("convert_stage_per_frame_ms"), static_cast<double>(progress.convertStageMs) / static_cast<double>(completedFrames)},
            {QStringLiteral("encode_stage_per_frame_ms"), static_cast<double>(progress.encodeStageMs) / static_cast<double>(completedFrames)}};
    };
    const auto renderProfileFromResult = [&formatEta, &outputPath](const RenderResult &result) -> QJsonObject
    {
        const qint64 completedFrames = qMax<int64_t>(1, result.framesRendered);
        const double fps = result.elapsedMs > 0
                               ? (1000.0 * static_cast<double>(result.framesRendered)) / static_cast<double>(result.elapsedMs)
                               : 0.0;
        return QJsonObject{
            {QStringLiteral("status"), result.success ? QStringLiteral("completed")
                                                      : (result.cancelled ? QStringLiteral("cancelled")
                                                                          : QStringLiteral("failed"))},
            {QStringLiteral("output_path"), QDir::toNativeSeparators(outputPath)},
            {QStringLiteral("frames_completed"), static_cast<qint64>(result.framesRendered)},
            {QStringLiteral("total_frames"), static_cast<qint64>(result.framesRendered)},
            {QStringLiteral("segment_index"), 0},
            {QStringLiteral("segment_count"), 0},
            {QStringLiteral("segment_start_frame"), static_cast<qint64>(0)},
            {QStringLiteral("segment_end_frame"), static_cast<qint64>(0)},
            {QStringLiteral("using_gpu"), result.usedGpu},
            {QStringLiteral("using_hardware_encode"), result.usedHardwareEncode},
            {QStringLiteral("encoder_label"), result.encoderLabel},
            {QStringLiteral("elapsed_ms"), result.elapsedMs},
            {QStringLiteral("estimated_remaining_ms"), static_cast<qint64>(0)},
            {QStringLiteral("eta_text"), formatEta(0)},
            {QStringLiteral("fps"), fps},
            {QStringLiteral("render_stage_ms"), result.renderStageMs},
            {QStringLiteral("render_decode_stage_ms"), result.renderDecodeStageMs},
            {QStringLiteral("render_texture_stage_ms"), result.renderTextureStageMs},
            {QStringLiteral("render_composite_stage_ms"), result.renderCompositeStageMs},
            {QStringLiteral("render_nv12_stage_ms"), result.renderNv12StageMs},
            {QStringLiteral("gpu_readback_ms"), result.gpuReadbackMs},
            {QStringLiteral("overlay_stage_ms"), result.overlayStageMs},
            {QStringLiteral("convert_stage_ms"), result.convertStageMs},
            {QStringLiteral("encode_stage_ms"), result.encodeStageMs},
            {QStringLiteral("audio_stage_ms"), result.audioStageMs},
            {QStringLiteral("max_frame_render_stage_ms"), result.maxFrameRenderStageMs},
            {QStringLiteral("max_frame_decode_stage_ms"), result.maxFrameDecodeStageMs},
            {QStringLiteral("max_frame_texture_stage_ms"), result.maxFrameTextureStageMs},
            {QStringLiteral("max_frame_readback_stage_ms"), result.maxFrameReadbackStageMs},
            {QStringLiteral("max_frame_convert_stage_ms"), result.maxFrameConvertStageMs},
            {QStringLiteral("skipped_clips"), result.skippedClips},
            {QStringLiteral("skipped_clip_reason_counts"), result.skippedClipReasonCounts},
            {QStringLiteral("render_stage_table"), result.renderStageTable},
            {QStringLiteral("worst_frame_table"), result.worstFrameTable},
            {QStringLiteral("render_stage_per_frame_ms"), static_cast<double>(result.renderStageMs) / static_cast<double>(completedFrames)},
            {QStringLiteral("render_decode_stage_per_frame_ms"), static_cast<double>(result.renderDecodeStageMs) / static_cast<double>(completedFrames)},
            {QStringLiteral("render_texture_stage_per_frame_ms"), static_cast<double>(result.renderTextureStageMs) / static_cast<double>(completedFrames)},
            {QStringLiteral("render_composite_stage_per_frame_ms"), static_cast<double>(result.renderCompositeStageMs) / static_cast<double>(completedFrames)},
            {QStringLiteral("render_nv12_stage_per_frame_ms"), static_cast<double>(result.renderNv12StageMs) / static_cast<double>(completedFrames)},
            {QStringLiteral("gpu_readback_per_frame_ms"), static_cast<double>(result.gpuReadbackMs) / static_cast<double>(completedFrames)},
            {QStringLiteral("overlay_stage_per_frame_ms"), static_cast<double>(result.overlayStageMs) / static_cast<double>(completedFrames)},
            {QStringLiteral("convert_stage_per_frame_ms"), static_cast<double>(result.convertStageMs) / static_cast<double>(completedFrames)},
            {QStringLiteral("encode_stage_per_frame_ms"), static_cast<double>(result.encodeStageMs) / static_cast<double>(completedFrames)},
            {QStringLiteral("message"), result.message}};
    };
    m_renderInProgress = true;
    m_liveRenderProfile = QJsonObject{
        {QStringLiteral("status"), QStringLiteral("starting")},
        {QStringLiteral("output_path"), QDir::toNativeSeparators(outputPath)},
        {QStringLiteral("frames_completed"), static_cast<qint64>(0)},
        {QStringLiteral("total_frames"), static_cast<qint64>(totalFramesToRender)}};
    refreshProfileInspector();

        const RenderResult result = renderTimelineToFile(
        effectiveRequest,
            [this, &progressDialog, renderStatusLabel, renderProgressBar, renderPreviewLabel, showRenderPreviewCheckBox, &renderCancelled, formatEta, stageSummary, renderProfileFromProgress, outputPath](const RenderProgress &progress)
        {
            renderProgressBar->setMaximum(qMax(1, static_cast<int>(qMin<int64_t>(progress.totalFrames, std::numeric_limits<int>::max()))));
            renderProgressBar->setValue(static_cast<int>(qMin<int64_t>(progress.framesCompleted, std::numeric_limits<int>::max())));
            const QString rendererMode = progress.usingGpu ? QStringLiteral("GPU render") : QStringLiteral("CPU render");
            const QString encoderMode = progress.usingHardwareEncode
                                            ? QStringLiteral("Hardware encode")
                                            : QStringLiteral("Software encode");
            const QString encoderLabel = progress.encoderLabel.isEmpty()
                                             ? QStringLiteral("unknown")
                                             : progress.encoderLabel;
            m_liveRenderProfile = renderProfileFromProgress(progress);
            m_liveRenderProfile[QStringLiteral("output_path")] = QDir::toNativeSeparators(outputPath);
            refreshProfileInspector();
            const QString metricsTable = QStringLiteral(
                "<table cellspacing='0' cellpadding='2' style='margin: 0 auto;'>"
                "<tr>"
                "<td align='right'><b>Render</b></td><td>%1</td>"
                "<td align='right'><b>Decode</b></td><td>%2</td>"
                "<td align='right'><b>Texture</b></td><td>%3</td>"
                "</tr>"
                "<tr>"
                "<td align='right'><b>Composite</b></td><td>%4</td>"
                "<td align='right'><b>GPU NV12</b></td><td>%5</td>"
                "<td align='right'><b>Readback</b></td><td>%6</td>"
                "</tr>"
                "<tr>"
                "<td align='right'><b>Convert</b></td><td>%7</td>"
                "<td align='right'><b>Encode</b></td><td>%8</td>"
                "<td></td><td></td>"
                "</tr>"
                "</table>")
                .arg(stageSummary(progress.renderStageMs, progress.framesCompleted))
                .arg(stageSummary(progress.renderDecodeStageMs, progress.framesCompleted))
                .arg(stageSummary(progress.renderTextureStageMs, progress.framesCompleted))
                .arg(stageSummary(progress.renderCompositeStageMs, progress.framesCompleted))
                .arg(stageSummary(progress.renderNv12StageMs, progress.framesCompleted))
                .arg(stageSummary(progress.gpuReadbackMs, progress.framesCompleted))
                .arg(stageSummary(progress.convertStageMs, progress.framesCompleted))
                .arg(stageSummary(progress.encodeStageMs, progress.framesCompleted));
            renderStatusLabel->setText(
                QStringLiteral("<b>Rendering frame %1 of %2</b><br>"
                               "Segment %3/%4: %5-%6<br>"
                               "%7 | %8 (%9)<br>"
                               "ETA: %10<br>%11")
                    .arg(progress.framesCompleted + 1)
                    .arg(qMax<int64_t>(1, progress.totalFrames))
                    .arg(progress.segmentIndex)
                    .arg(progress.segmentCount)
                    .arg(progress.segmentStartFrame)
                    .arg(progress.segmentEndFrame)
                    .arg(rendererMode)
                    .arg(encoderMode)
                    .arg(encoderLabel)
                    .arg(formatEta(progress.estimatedRemainingMs))
                    .arg(metricsTable));
            if (showRenderPreviewCheckBox->isChecked() && !progress.previewFrame.isNull())
            {
                const QPixmap pixmap = QPixmap::fromImage(progress.previewFrame).scaled(
                    renderPreviewLabel->size(),
                    Qt::KeepAspectRatio,
                    Qt::SmoothTransformation);
                renderPreviewLabel->setPixmap(pixmap);
                renderPreviewLabel->setText(QString());
            }
            QCoreApplication::processEvents();
            return !renderCancelled;
        });
    renderProgressBar->setValue(renderProgressBar->maximum());
    progressDialog.close();
    m_renderInProgress = false;
    m_lastRenderProfile = renderProfileFromResult(result);
    m_liveRenderProfile = QJsonObject{};
    refreshProfileInspector();

    if (result.success)
    {
        QMessageBox::information(this, QStringLiteral("Render Complete"), result.message);
        return;
    }

    const QString message = result.message.isEmpty()
                                ? QStringLiteral("Render failed.")
                                : result.message;
    QMessageBox::warning(this,
                         result.cancelled ? QStringLiteral("Render Cancelled") : QStringLiteral("Render Failed"),
                         message);
}

void EditorWindow::refreshProfileInspector()
{
    if (m_profileTab) {
        m_profileTab->refresh();
    }
}

void EditorWindow::runDecodeBenchmarkFromProfile()
{
    if (m_profileTab) {
        m_profileTab->runDecodeBenchmark();
    }
}

bool EditorWindow::profileBenchmarkClip(TimelineClip *out) const
{
    if (!out) return false;
    if (!m_timeline) return false;
    const TimelineClip *selected = m_timeline->selectedClip();
    if (selected && clipHasVisuals(*selected)) {
        *out = *selected;
        return true;
    }
    const QVector<TimelineClip> clips = m_timeline->clips();
    for (const TimelineClip &clip : clips) {
        if (clipHasVisuals(clip)) {
            *out = clip;
            return true;
        }
    }
    return false;
}

QStringList EditorWindow::availableHardwareDeviceTypes() const
{
    QStringList types;
    for (AVHWDeviceType type = av_hwdevice_iterate_types(AV_HWDEVICE_TYPE_NONE);
         type != AV_HWDEVICE_TYPE_NONE;
         type = av_hwdevice_iterate_types(type)) {
        if (const char *name = av_hwdevice_get_type_name(type)) {
            types.push_back(QString::fromLatin1(name));
        }
    }
    return types;
}

void EditorWindow::setCurrentPlaybackSample(int64_t samplePosition, bool syncAudio, bool duringPlayback)
{
    const int64_t boundedSample = qBound<int64_t>(0, samplePosition, frameToSamples(m_timeline->totalFrames()));
    const qreal framePosition = samplesToFramePosition(boundedSample);
    const int64_t bounded = qBound<int64_t>(0, static_cast<int64_t>(std::floor(framePosition)), m_timeline->totalFrames());
    
    playbackTrace(QStringLiteral("EditorWindow::setCurrentFrame"),
                  QStringLiteral("requestedSample=%1 boundedSample=%2 frame=%3")
                      .arg(samplePosition).arg(boundedSample).arg(framePosition, 0, 'f', 3));
    
    if (!m_timeline || bounded != m_timeline->currentFrame()) {
        m_lastPlayheadAdvanceMs.store(nowMs());
    }
    
    m_absolutePlaybackSample = boundedSample;
    m_filteredPlaybackSample = filteredPlaybackSampleForAbsoluteSample(boundedSample);
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
    
    if (duringPlayback) {
        updateTransportLabels();
        syncTranscriptTableToPlayhead();
        syncKeyframeTableToPlayhead();
        syncGradingTableToPlayhead();
    } else {
        m_inspectorPane->refresh();
        syncKeyframeTableToPlayhead();
        syncGradingTableToPlayhead();
    }
    scheduleSaveState();
}

void EditorWindow::setCurrentFrame(int64_t frame, bool syncAudio)
{
    setCurrentPlaybackSample(frameToSamples(frame), syncAudio);
}

void EditorWindow::setPlaybackActive(bool playing)
{
    if (playing == playbackActive()) {
        updateTransportLabels();
        return;
    }

    if (playing) {
        const auto ranges = effectivePlaybackRanges();
        if (m_audioEngine) {
            m_audioEngine->setExportRanges(ranges);
            m_audioEngine->setSpeechFilterFadeSamples(m_speechFilterFadeSamples);
        }
        if (m_audioEngine && m_audioEngine->hasPlayableAudio()) {
            m_audioEngine->start(m_timeline->currentFrame());
        }
        if (m_preview) {
            m_preview->setExportRanges(ranges);
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
    updateTransportLabels();
    m_inspectorPane->refresh();
    scheduleSaveState();
}

void EditorWindow::togglePlayback()
{
    setPlaybackActive(!playbackActive());
}

bool EditorWindow::playbackActive() const
{
    return m_fastPlaybackActive.load();
}

namespace {

bool zeroCopyPreferredEnvironmentDetected() {
#if defined(Q_OS_LINUX)
    return QFile::exists(QStringLiteral("/proc/driver/nvidia/version")) ||
           QFile::exists(QStringLiteral("/sys/module/nvidia"));
#else
    return false;
#endif
}

}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("QRhi Editor"));
    qRegisterMetaType<editor::FrameHandle>();

    if (!zeroCopyPreferredEnvironmentDetected()) {
        qWarning().noquote() << QStringLiteral(
            "[STARTUP][WARN] Preferred zero-copy decode path requires Linux + NVIDIA detection; "
            "falling back to hardware CPU-upload or software decode.");
    }

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("PanelVid2TikTok editor"));
    parser.addHelpOption();
    QCommandLineOption debugPlaybackOption(QStringLiteral("debug-playback"),
                                           QStringLiteral("Enable playback debug logging"));
    QCommandLineOption debugCacheOption(QStringLiteral("debug-cache"),
                                        QStringLiteral("Enable cache debug logging"));
    QCommandLineOption debugDecodeOption(QStringLiteral("debug-decode"),
                                         QStringLiteral("Enable decode debug logging"));
    QCommandLineOption debugAllOption(QStringLiteral("debug-all"),
                                      QStringLiteral("Enable all debug logging"));
    QCommandLineOption controlPortOption(
        QStringList{QStringLiteral("control-port")},
        QStringLiteral("Control server port."),
        QStringLiteral("port"));
    parser.addOption(debugPlaybackOption);
    parser.addOption(debugCacheOption);
    parser.addOption(debugDecodeOption);
    parser.addOption(debugAllOption);
    parser.addOption(controlPortOption);
    parser.process(app);

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

    bool portOk = false;
    quint16 controlPort = 40130;
    const QString optionValue = parser.value(controlPortOption);
    if (!optionValue.isEmpty()) {
        const uint parsed = optionValue.toUInt(&portOk);
        if (portOk && parsed <= std::numeric_limits<quint16>::max()) {
            controlPort = static_cast<quint16>(parsed);
        }
    } else {
        const QString envValue = qEnvironmentVariable("EDITOR_CONTROL_PORT");
        const uint parsed = envValue.toUInt(&portOk);
        if (portOk && parsed <= std::numeric_limits<quint16>::max()) {
            controlPort = static_cast<quint16>(parsed);
        }
    }

    EditorWindow window(controlPort);
    window.show();
    return app.exec();
}
