#pragma once

#include "editor_shared.h"
#include "frame_handle.h"
#include "render.h"
#include "audio_engine.h"
#include "control_server.h"
#include "timeline_widget.h"
#include "preview.h"
#include "editor_pane.h"
#include "explorer_pane.h"
#include "inspector_pane.h"
#include "output_tab.h"
#include "profile_tab.h"
#include "transcript_tab.h"
#include "grading_tab.h"
#include "video_keyframe_tab.h"

#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFontComboBox>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <QToolButton>

#include <atomic>
#include <memory>

namespace editor {

class EditorWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit EditorWindow(quint16 controlPort);
    ~EditorWindow() override;

protected:
    void closeEvent(QCloseEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    QWidget *buildEditorPane();

    void loadState();
    void openTranscriptionWindow(const QString &filePath, const QString &label);

    QString defaultProxyOutputPath(const TimelineClip &clip,
                                   const MediaProbeResult *knownProbe = nullptr) const;
    QString clipFileInfoSummary(const QString &filePath,
                                const MediaProbeResult *knownProbe = nullptr) const;
    void createProxyForClip(const QString &clipId);
    void deleteProxyForClip(const QString &clipId);

    void addFileToTimeline(const QString &filePath);
    void syncSliderRange();
    void focusGradingTab();
    void updateTransportLabels();
    QString frameToTimecode(int64_t frame) const;
    QJsonObject profilingSnapshot() const;

    void syncTranscriptTableToPlayhead();
    void syncKeyframeTableToPlayhead();
    void syncGradingTableToPlayhead();

    bool focusInTranscriptTable() const;
    bool focusInKeyframeTable() const;
    bool focusInGradingTable() const;
    bool focusInEditableInput() const;
    bool shouldBlockGlobalEditorShortcuts() const;

    void initializeDeferredTimelineSeek(QTimer *timer, int64_t *pendingFrame);
    void scheduleDeferredTimelineSeek(QTimer *timer, int64_t *pendingFrame, int64_t timelineFrame);
    void cancelDeferredTimelineSeek(QTimer *timer, int64_t *pendingFrame);

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
    bool saveProjectPayload(const QString &projectId,
                            const QByteArray &statePayload,
                            const QByteArray &historyPayload);
    void saveProjectAs();
    void renameProject(const QString &projectId);
    QJsonObject buildStateJson() const;
    void scheduleSaveState();
    void saveStateNow();
    void saveHistoryNow();
    void pushHistorySnapshot();
    void undoHistory();
    void applyStateJson(const QJsonObject &root);

    void advanceFrame();
    bool speechFilterPlaybackEnabled() const;
    int64_t filteredPlaybackSampleForAbsoluteSample(int64_t absoluteSample) const;
    QVector<ExportRangeSegment> effectivePlaybackRanges() const;
    int64_t nextPlaybackFrame(int64_t currentFrame) const;
    QString clipLabelForId(const QString &clipId) const;
    QColor clipColorForId(const QString &clipId) const;
    bool parseSyncActionText(const QString &text, RenderSyncAction *actionOut) const;
    void refreshSyncInspector();
    void onSyncTableSelectionChanged();
    void onSyncTableItemChanged(QTableWidgetItem* item);
    void onSyncTableItemDoubleClicked(QTableWidgetItem* item);
    void onSyncTableCustomContextMenu(const QPoint& pos);
    void refreshClipInspector();
    void refreshOutputInspector();
    void applyOutputRangeFromInspector();
    void renderFromOutputInspector();
    void renderTimelineFromOutputRequest(const RenderRequest &request);
    void refreshProfileInspector();
    void runDecodeBenchmarkFromProfile();
    bool profileBenchmarkClip(TimelineClip *out) const;
    QStringList availableHardwareDeviceTypes() const;
    void setCurrentPlaybackSample(int64_t samplePosition, bool syncAudio = true, bool duringPlayback = false);
    void setCurrentFrame(int64_t frame, bool syncAudio = true);
    void setPlaybackActive(bool playing);
    void togglePlayback();
    bool playbackActive() const;

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
    QTableWidget *m_gradingKeyframeTable = nullptr;
    QCheckBox *m_gradingAutoScrollCheckBox = nullptr;
    QCheckBox *m_gradingFollowCurrentCheckBox = nullptr;
    QPushButton *m_gradingKeyAtPlayheadButton = nullptr;
    QPushButton *m_gradingFadeInButton = nullptr;
    QPushButton *m_gradingFadeOutButton = nullptr;

    QLabel *m_videoInspectorClipLabel = nullptr;
    QLabel *m_videoInspectorDetailsLabel = nullptr;

    QLabel *m_keyframesInspectorClipLabel = nullptr;
    QLabel *m_keyframesInspectorDetailsLabel = nullptr;
    QCheckBox *m_keyframesAutoScrollCheckBox = nullptr;
    QCheckBox *m_keyframesFollowCurrentCheckBox = nullptr;

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
    QDoubleSpinBox *m_clipPlaybackRateSpin = nullptr;
    QLabel *m_trackInspectorLabel = nullptr;
    QLabel *m_trackInspectorDetailsLabel = nullptr;
    QLineEdit *m_trackNameEdit = nullptr;
    QSpinBox *m_trackHeightSpin = nullptr;
    QCheckBox *m_trackVideoEnabledCheckBox = nullptr;
    QCheckBox *m_trackAudioEnabledCheckBox = nullptr;
    QDoubleSpinBox *m_trackCrossfadeSecondsSpin = nullptr;
    QPushButton *m_trackCrossfadeButton = nullptr;
    QCheckBox *m_previewHideOutsideOutputCheckBox = nullptr;
    QTableWidget *m_profileSummaryTable = nullptr;
    QPushButton *m_profileBenchmarkButton = nullptr;

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
    QLabel *m_outputRangeSummaryLabel = nullptr;
    QCheckBox *m_renderUseProxiesCheckBox = nullptr;

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
    QSpinBox *m_speechFilterFadeSamplesSpin = nullptr;
    int m_transcriptPrependMs = 0;
    int m_transcriptPostpendMs = 0;
    int m_speechFilterFadeSamples = 250;
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
    QString m_lastRenderOutputPath;

    std::unique_ptr<ControlServer> m_controlServer;
    std::unique_ptr<AudioEngine> m_audioEngine;
    std::unique_ptr<TranscriptTab> m_transcriptTab;
    std::unique_ptr<GradingTab> m_gradingTab;
    std::unique_ptr<VideoKeyframeTab> m_videoKeyframeTab;
    std::unique_ptr<OutputTab> m_outputTab;
    std::unique_ptr<ProfileTab> m_profileTab;

    ExplorerPane *m_explorerPane = nullptr;
    InspectorPane *m_inspectorPane = nullptr;
    EditorPane *m_editorPane = nullptr;

    QTimer m_playbackTimer;
    QTimer m_mainThreadHeartbeatTimer;
    QTimer m_stateSaveTimer;

    bool m_ignoreSeekSignal = false;
    bool m_loadingState = false;
    bool m_updatingProjectsList = false;
    bool m_pendingSaveAfterLoad = false;
    bool m_restoringHistory = false;

    int64_t m_absolutePlaybackSample = 0;
    int64_t m_filteredPlaybackSample = 0;
    QTimer m_transcriptClickSeekTimer;
    int64_t m_pendingTranscriptClickTimelineFrame = -1;
    QTimer m_keyframeClickSeekTimer;
    int64_t m_pendingKeyframeClickTimelineFrame = -1;
    QTimer m_gradingClickSeekTimer;
    int64_t m_pendingGradingClickTimelineFrame = -1;
    QTimer m_syncClickSeekTimer;
    int64_t m_pendingSyncClickTimelineFrame = -1;

    QString m_currentProjectId;

    QByteArray m_lastSavedState;
    QJsonArray m_historyEntries;
    int m_historyIndex = -1;
    QJsonObject m_lastDecodeBenchmark;
    bool m_renderInProgress = false;
    QJsonObject m_liveRenderProfile;
    QJsonObject m_lastRenderProfile;

    bool m_updatingTranscriptInspector = false;
    bool m_updatingSyncInspector = false;

    std::atomic<qint64> m_fastCurrentFrame{0};
    std::atomic<bool> m_fastPlaybackActive{false};
    std::atomic<qint64> m_lastMainThreadHeartbeatMs{0};
    std::atomic<qint64> m_lastPlayheadAdvanceMs{0};
};

} // namespace editor
