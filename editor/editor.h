#pragma once

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
#include "output_tab.h"
#include "profile_tab.h"
#include "transcript_tab.h"
#include "grading_tab.h"
#include "video_keyframe_tab.h"

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
#include <QProgressDialog>
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
#include <QTextEdit>
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
#include <libavutil/hwcontext.h>
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
    explicit EditorWindow(quint16 controlPort);
    ~EditorWindow() override;

protected:
    void closeEvent(QCloseEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    void loadState();
    void openTranscriptionWindow(const QString &filePath, const QString &label);
    void createProxyForClip(const QString &clipId);
    void deleteProxyForClip(const QString &clipId);
    void addFileToTimeline(const QString &filePath);
    void setCurrentFrame(int64_t frame, bool syncAudio = true);
    void setPlaybackActive(bool playing);
    void togglePlayback();
    void advanceFrame();
    void scheduleSaveState();
    void saveStateNow();
    void saveHistoryNow();
    void pushHistorySnapshot();
    void undoHistory();
    void applyStateJson(const QJsonObject &root);
    void renderTimelineFromOutputRequest(const RenderRequest &request);

private:
    // Core UI components
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

    // Inspector pane widgets (used by tabs)
    QTableWidget *m_videoKeyframeTable = nullptr;
    QTableWidget *m_transcriptTable = nullptr;
    QTableWidget *m_syncTable = nullptr;
    QTableWidget *m_gradingKeyframeTable = nullptr;
    QTableWidget *m_profileSummaryTable = nullptr;

    QLabel *m_transcriptInspectorClipLabel = nullptr;
    QLabel *m_transcriptInspectorDetailsLabel = nullptr;
    QLabel *m_clipInspectorClipLabel = nullptr;
    QLabel *m_clipProxyUsageLabel = nullptr;
    QLabel *m_clipPlaybackSourceLabel = nullptr;
    QLabel *m_clipOriginalInfoLabel = nullptr;
    QLabel *m_clipProxyInfoLabel = nullptr;
    QLabel *m_syncInspectorClipLabel = nullptr;
    QLabel *m_syncInspectorDetailsLabel = nullptr;
    QLabel *m_keyframesInspectorClipLabel = nullptr;
    QLabel *m_keyframesInspectorDetailsLabel = nullptr;
    QLabel *m_audioInspectorClipLabel = nullptr;
    QLabel *m_audioInspectorDetailsLabel = nullptr;
    QLabel *m_gradingPathLabel = nullptr;

    QDoubleSpinBox *m_brightnessSpin = nullptr;
    QDoubleSpinBox *m_contrastSpin = nullptr;
    QDoubleSpinBox *m_saturationSpin = nullptr;
    QDoubleSpinBox *m_opacitySpin = nullptr;
    QDoubleSpinBox *m_videoTranslationXSpin = nullptr;
    QDoubleSpinBox *m_videoTranslationYSpin = nullptr;
    QDoubleSpinBox *m_videoRotationSpin = nullptr;
    QDoubleSpinBox *m_videoScaleXSpin = nullptr;
    QDoubleSpinBox *m_videoScaleYSpin = nullptr;

    QComboBox *m_videoInterpolationCombo = nullptr;
    QComboBox *m_outputFormatCombo = nullptr;

    QCheckBox *m_mirrorHorizontalCheckBox = nullptr;
    QCheckBox *m_mirrorVerticalCheckBox = nullptr;
    QCheckBox *m_lockVideoScaleCheckBox = nullptr;
    QCheckBox *m_keyframeSpaceCheckBox = nullptr;
    QCheckBox *m_transcriptOverlayEnabledCheckBox = nullptr;
    QCheckBox *m_transcriptAutoScrollCheckBox = nullptr;
    QCheckBox *m_transcriptFollowCurrentWordCheckBox = nullptr;
    QCheckBox *m_gradingAutoScrollCheckBox = nullptr;
    QCheckBox *m_gradingFollowCurrentCheckBox = nullptr;
    QCheckBox *m_keyframesAutoScrollCheckBox = nullptr;
    QCheckBox *m_keyframesFollowCurrentCheckBox = nullptr;
    QCheckBox *m_speechFilterEnabledCheckBox = nullptr;

    QSpinBox *m_transcriptMaxLinesSpin = nullptr;
    QSpinBox *m_transcriptMaxCharsSpin = nullptr;
    QSpinBox *m_transcriptOverlayWidthSpin = nullptr;
    QSpinBox *m_transcriptOverlayHeightSpin = nullptr;
    QSpinBox *m_transcriptFontSizeSpin = nullptr;
    QSpinBox *m_transcriptPrependMsSpin = nullptr;
    QSpinBox *m_transcriptPostpendMsSpin = nullptr;
    QSpinBox *m_speechFilterFadeSamplesSpin = nullptr;
    QSpinBox *m_outputWidthSpin = nullptr;
    QSpinBox *m_outputHeightSpin = nullptr;
    QSpinBox *m_exportStartSpin = nullptr;
    QSpinBox *m_exportEndSpin = nullptr;

    QDoubleSpinBox *m_transcriptOverlayXSpin = nullptr;
    QDoubleSpinBox *m_transcriptOverlayYSpin = nullptr;

    QFontComboBox *m_transcriptFontFamilyCombo = nullptr;

    QCheckBox *m_transcriptBoldCheckBox = nullptr;
    QCheckBox *m_transcriptItalicCheckBox = nullptr;

    QPushButton *m_gradingKeyAtPlayheadButton = nullptr;
    QPushButton *m_addVideoKeyframeButton = nullptr;
    QPushButton *m_removeVideoKeyframeButton = nullptr;
    QPushButton *m_renderButton = nullptr;
    QPushButton *m_profileBenchmarkButton = nullptr;

    QLabel *m_outputRangeSummaryLabel = nullptr;

    // Tab modules
    std::unique_ptr<OutputTab> m_outputTab;
    std::unique_ptr<ProfileTab> m_profileTab;
    std::unique_ptr<TranscriptTab> m_transcriptTab;
    std::unique_ptr<GradingTab> m_gradingTab;
    std::unique_ptr<VideoKeyframeTab> m_videoKeyframeTab;

    // Core engine components
    std::unique_ptr<ControlServer> m_controlServer;
    std::unique_ptr<AudioEngine> m_audioEngine;
    
    ExplorerPane *m_explorerPane = nullptr;
    InspectorPane *m_inspectorPane = nullptr;

    // Timers
    QTimer m_playbackTimer;
    QTimer m_mainThreadHeartbeatTimer;
    QTimer m_stateSaveTimer;

    // State flags
    bool m_ignoreSeekSignal = false;
    bool m_loadingState = false;
    bool m_updatingProjectsList = false;
    bool m_pendingSaveAfterLoad = false;
    bool m_restoringHistory = false;
    bool m_updatingTranscriptInspector = false;
    bool m_updatingSyncInspector = false;

    // Playback state
    int64_t m_absolutePlaybackSample = 0;
    int64_t m_filteredPlaybackSample = 0;
    
    // Deferred seek timers
    QTimer m_transcriptClickSeekTimer;
    int64_t m_pendingTranscriptClickTimelineFrame = -1;
    QTimer m_keyframeClickSeekTimer;
    int64_t m_pendingKeyframeClickTimelineFrame = -1;
    QTimer m_gradingClickSeekTimer;
    int64_t m_pendingGradingClickTimelineFrame = -1;

    // Project management
    QString m_currentProjectId;
    QByteArray m_lastSavedState;
    QJsonArray m_historyEntries;
    int m_historyIndex = -1;

    // Speech filter settings (used by transcript tab)
    int m_transcriptPrependMs = 0;
    int m_transcriptPostpendMs = 0;
    int m_speechFilterFadeSamples = 250;
    mutable TranscriptEngine m_transcriptEngine;

    // Render state
    QJsonObject m_lastDecodeBenchmark;
    bool m_renderInProgress = false;
    QJsonObject m_liveRenderProfile;
    QJsonObject m_lastRenderProfile;

    // Selection state (used by grading and video keyframe tabs)
    int64_t m_selectedVideoKeyframeFrame = -1;
    QSet<int64_t> m_selectedVideoKeyframeFrames;
    int64_t m_selectedGradingKeyframeFrame = -1;
    QSet<int64_t> m_selectedGradingKeyframeFrames;

    // Transcript state
    QString m_loadedTranscriptPath;
    QJsonDocument m_loadedTranscriptDoc;

    // Atomic state for thread-safe access
    std::atomic<qint64> m_fastCurrentFrame{0};
    std::atomic<bool> m_fastPlaybackActive{false};
    std::atomic<qint64> m_lastMainThreadHeartbeatMs{0};
    std::atomic<qint64> m_lastPlayheadAdvanceMs{0};

    // Private helper methods
    QWidget *buildEditorPane();
    void syncSliderRange();
    void focusGradingTab();
    void updateTransportLabels();
    QString frameToTimecode(int64_t frame) const;
    QJsonObject profilingSnapshot() const;
    QJsonObject buildStateJson() const;
    QString defaultProxyOutputPath(const TimelineClip &clip, const MediaProbeResult *knownProbe = nullptr) const;
    QString clipFileInfoSummary(const QString &filePath, const MediaProbeResult *knownProbe = nullptr) const;
    QString clipLabelForId(const QString &clipId) const;
    QColor clipColorForId(const QString &clipId) const;
    bool parseSyncActionText(const QString &text, RenderSyncAction *actionOut) const;
    void refreshSyncInspector();
    void refreshClipInspector();
    void refreshOutputInspector();
    void refreshProfileInspector();
    void applyOutputRangeFromInspector();
    void renderFromOutputInspector();
    void runDecodeBenchmarkFromProfile();
    bool profileBenchmarkClip(TimelineClip *out) const;
    QStringList availableHardwareDeviceTypes() const;
    bool playbackActive() const;
    bool speechFilterPlaybackEnabled() const;
    int64_t filteredPlaybackSampleForAbsoluteSample(int64_t absoluteSample) const;
    QVector<ExportRangeSegment> effectivePlaybackRanges() const;
    int64_t nextPlaybackFrame(int64_t currentFrame) const;
    void setCurrentPlaybackSample(int64_t samplePosition, bool syncAudio = true, bool duringPlayback = false);
    void syncTranscriptTableToPlayhead();
    void syncKeyframeTableToPlayhead();
    void syncGradingTableToPlayhead();
    bool focusInTranscriptTable() const;
    bool focusInKeyframeTable() const;
    bool focusInGradingTable() const;
    bool focusInEditableInput() const;
    bool shouldBlockGlobalEditorShortcuts() const;
    void initializeDeferredTimeline() const;
        void initializeDeferredTimelineSeek(QTimer *timer, int64_t *pendingFrame);
    void scheduleDeferredTimelineSeek(QTimer *timer, int64_t *pendingFrame, int64_t timelineFrame);
    void cancelDeferredTimelineSeek(QTimer *timer, int64_t *pendingFrame);
    
    // Project management helpers
    QString projectsDirPath() const;
    QString currentProjectMarkerPath() const;
    QString currentProjectIdOrDefault() const;
    QString projectPath(const QString &projectId) const;
    QString stateFilePathForProject(const QString &projectId) const;
    QString historyFilePathForProject(const QString &projectId) const;
    QString stateFilePath() const;
    QString historyFilePath() const;
    QString sanitizedProjectId(const QString &name) const;
    void ensureProjectsDirectory() const;
    QStringList availableProjectIds() const;
    void ensureDefaultProjectExists() const;
    void loadProjectsFromFolders();
    void saveCurrentProjectMarker();
    QString currentProjectName() const;
    void refreshProjectsList();
    void switchToProject(const QString &projectId);
    void createProject();
    bool saveProjectPayload(const QString &projectId, const QByteArray &statePayload, const QByteArray &historyPayload);
    void saveProjectAs();
    void renameProject(const QString &projectId);
};
