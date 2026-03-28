#pragma once

#include <QWidget>

class QLabel;
class QTabWidget;
class QDoubleSpinBox;
class QSpinBox;
class QComboBox;
class QCheckBox;
class QFontComboBox;
class QTableWidget;
class QPushButton;
class QLineEdit;

class InspectorPane final : public QWidget
{
    Q_OBJECT

public:
    explicit InspectorPane(QWidget *parent = nullptr);

    QTabWidget *tabs() const { return m_inspectorTabs; }

    QDoubleSpinBox *brightnessSpin() const { return m_brightnessSpin; }
    QDoubleSpinBox *contrastSpin() const { return m_contrastSpin; }
    QDoubleSpinBox *saturationSpin() const { return m_saturationSpin; }
    QDoubleSpinBox *opacitySpin() const { return m_opacitySpin; }

    QDoubleSpinBox *videoTranslationXSpin() const { return m_videoTranslationXSpin; }
    QDoubleSpinBox *videoTranslationYSpin() const { return m_videoTranslationYSpin; }
    QDoubleSpinBox *videoRotationSpin() const { return m_videoRotationSpin; }
    QDoubleSpinBox *videoScaleXSpin() const { return m_videoScaleXSpin; }
    QDoubleSpinBox *videoScaleYSpin() const { return m_videoScaleYSpin; }
    QComboBox *videoInterpolationCombo() const { return m_videoInterpolationCombo; }
    QCheckBox *mirrorHorizontalCheckBox() const { return m_mirrorHorizontalCheckBox; }
    QCheckBox *mirrorVerticalCheckBox() const { return m_mirrorVerticalCheckBox; }
    QCheckBox *lockVideoScaleCheckBox() const { return m_lockVideoScaleCheckBox; }
    QCheckBox *keyframeSpaceCheckBox() const { return m_keyframeSpaceCheckBox; }

    QCheckBox *transcriptOverlayEnabledCheckBox() const { return m_transcriptOverlayEnabledCheckBox; }
    QSpinBox *transcriptMaxLinesSpin() const { return m_transcriptMaxLinesSpin; }
    QSpinBox *transcriptMaxCharsSpin() const { return m_transcriptMaxCharsSpin; }
    QCheckBox *transcriptAutoScrollCheckBox() const { return m_transcriptAutoScrollCheckBox; }
    QDoubleSpinBox *transcriptOverlayXSpin() const { return m_transcriptOverlayXSpin; }
    QDoubleSpinBox *transcriptOverlayYSpin() const { return m_transcriptOverlayYSpin; }
    QSpinBox *transcriptOverlayWidthSpin() const { return m_transcriptOverlayWidthSpin; }
    QSpinBox *transcriptOverlayHeightSpin() const { return m_transcriptOverlayHeightSpin; }
    QFontComboBox *transcriptFontFamilyCombo() const { return m_transcriptFontFamilyCombo; }
    QSpinBox *transcriptFontSizeSpin() const { return m_transcriptFontSizeSpin; }
    QCheckBox *transcriptBoldCheckBox() const { return m_transcriptBoldCheckBox; }
    QCheckBox *transcriptItalicCheckBox() const { return m_transcriptItalicCheckBox; }
    QCheckBox *transcriptFollowCurrentWordCheckBox() const { return m_transcriptFollowCurrentWordCheckBox; }

    QLabel *gradingPathLabel() const { return m_gradingPathLabel; }
    QTableWidget *gradingKeyframeTable() const { return m_gradingKeyframeTable; }
    QCheckBox *gradingAutoScrollCheckBox() const { return m_gradingAutoScrollCheckBox; }
    QCheckBox *gradingFollowCurrentCheckBox() const { return m_gradingFollowCurrentCheckBox; }
    QPushButton *gradingKeyAtPlayheadButton() const { return m_gradingKeyAtPlayheadButton; }
    QPushButton *gradingFadeInButton() const { return m_gradingFadeInButton; }
    QPushButton *gradingFadeOutButton() const { return m_gradingFadeOutButton; }
    QLabel *syncInspectorClipLabel() const { return m_syncInspectorClipLabel; }
    QLabel *syncInspectorDetailsLabel() const { return m_syncInspectorDetailsLabel; }
    QTableWidget *syncTable() const { return m_syncTable; }

    QLabel *keyframesInspectorClipLabel() const { return m_keyframesInspectorClipLabel; }
    QLabel *keyframesInspectorDetailsLabel() const { return m_keyframesInspectorDetailsLabel; }
    QTableWidget *videoKeyframeTable() const { return m_videoKeyframeTable; }
    QCheckBox *keyframesAutoScrollCheckBox() const { return m_keyframesAutoScrollCheckBox; }
    QCheckBox *keyframesFollowCurrentCheckBox() const { return m_keyframesFollowCurrentCheckBox; }
    QPushButton *addVideoKeyframeButton() const { return m_addVideoKeyframeButton; }
    QPushButton *removeVideoKeyframeButton() const { return m_removeVideoKeyframeButton; }

    QLabel *audioInspectorClipLabel() const { return m_audioInspectorClipLabel; }
    QLabel *audioInspectorDetailsLabel() const { return m_audioInspectorDetailsLabel; }

    QLabel *transcriptInspectorClipLabel() const { return m_transcriptInspectorClipLabel; }
    QLabel *transcriptInspectorDetailsLabel() const { return m_transcriptInspectorDetailsLabel; }
    QTableWidget *transcriptTable() const { return m_transcriptTable; }
    QLabel *clipInspectorClipLabel() const { return m_clipInspectorClipLabel; }
    QLabel *clipProxyUsageLabel() const { return m_clipProxyUsageLabel; }
    QLabel *clipPlaybackSourceLabel() const { return m_clipPlaybackSourceLabel; }
    QLabel *clipOriginalInfoLabel() const { return m_clipOriginalInfoLabel; }
    QLabel *clipProxyInfoLabel() const { return m_clipProxyInfoLabel; }
    QDoubleSpinBox *clipPlaybackRateSpin() const { return m_clipPlaybackRateSpin; }
    QLabel *trackInspectorLabel() const { return m_trackInspectorLabel; }
    QLabel *trackInspectorDetailsLabel() const { return m_trackInspectorDetailsLabel; }
    QLineEdit *trackNameEdit() const { return m_trackNameEdit; }
    QSpinBox *trackHeightSpin() const { return m_trackHeightSpin; }
    QCheckBox *trackVideoEnabledCheckBox() const { return m_trackVideoEnabledCheckBox; }
    QCheckBox *trackAudioEnabledCheckBox() const { return m_trackAudioEnabledCheckBox; }
    QDoubleSpinBox *trackCrossfadeSecondsSpin() const { return m_trackCrossfadeSecondsSpin; }
    QPushButton *trackCrossfadeButton() const { return m_trackCrossfadeButton; }
    QCheckBox *previewHideOutsideOutputCheckBox() const { return m_previewHideOutsideOutputCheckBox; }
    QTableWidget *profileSummaryTable() const { return m_profileSummaryTable; }
    QPushButton *profileBenchmarkButton() const { return m_profileBenchmarkButton; }

    QSpinBox *outputWidthSpin() const { return m_outputWidthSpin; }
    QSpinBox *outputHeightSpin() const { return m_outputHeightSpin; }
    QSpinBox *exportStartSpin() const { return m_exportStartSpin; }
    QSpinBox *exportEndSpin() const { return m_exportEndSpin; }
    QComboBox *outputFormatCombo() const { return m_outputFormatCombo; }
    QLabel *outputRangeSummaryLabel() const { return m_outputRangeSummaryLabel; }
    QCheckBox *renderUseProxiesCheckBox() const { return m_renderUseProxiesCheckBox; }
    QPushButton *renderButton() const { return m_renderButton; }

    QCheckBox *speechFilterEnabledCheckBox() const { return m_speechFilterEnabledCheckBox; }
    QSpinBox *transcriptPrependMsSpin() const { return m_transcriptPrependMsSpin; }
    QSpinBox *transcriptPostpendMsSpin() const { return m_transcriptPostpendMsSpin; }
    QSpinBox *speechFilterFadeSamplesSpin() const { return m_speechFilterFadeSamplesSpin; }

    void refresh();

signals:
    void refreshRequested();

private:
    QWidget *buildPane();
    QWidget *buildGradingTab();
    QWidget *buildSyncTab();
    QWidget *buildKeyframesTab();
    QWidget *buildTranscriptTab();
    QWidget *buildClipTab();
    QWidget *buildOutputTab();
    QWidget *buildPreviewTab();
    QWidget *buildProfileTab();

private:
    QTabWidget *m_inspectorTabs = nullptr;

    QLabel *m_gradingPathLabel = nullptr;
    QDoubleSpinBox *m_brightnessSpin = nullptr;
    QDoubleSpinBox *m_contrastSpin = nullptr;
    QDoubleSpinBox *m_saturationSpin = nullptr;
    QDoubleSpinBox *m_opacitySpin = nullptr;
    QTableWidget *m_gradingKeyframeTable = nullptr;
    QCheckBox *m_gradingAutoScrollCheckBox = nullptr;
    QCheckBox *m_gradingFollowCurrentCheckBox = nullptr;
    QPushButton *m_gradingKeyAtPlayheadButton = nullptr;
    QPushButton *m_gradingFadeInButton = nullptr;
    QPushButton *m_gradingFadeOutButton = nullptr;

    QLabel *m_syncInspectorClipLabel = nullptr;
    QLabel *m_syncInspectorDetailsLabel = nullptr;
    QTableWidget *m_syncTable = nullptr;

    QLabel *m_keyframesInspectorClipLabel = nullptr;
    QLabel *m_keyframesInspectorDetailsLabel = nullptr;
    QTableWidget *m_videoKeyframeTable = nullptr;
    QCheckBox *m_keyframesAutoScrollCheckBox = nullptr;
    QCheckBox *m_keyframesFollowCurrentCheckBox = nullptr;
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
    QPushButton *m_addVideoKeyframeButton = nullptr;
    QPushButton *m_removeVideoKeyframeButton = nullptr;

    QLabel *m_audioInspectorClipLabel = nullptr;
    QLabel *m_audioInspectorDetailsLabel = nullptr;

    QLabel *m_transcriptInspectorClipLabel = nullptr;
    QLabel *m_transcriptInspectorDetailsLabel = nullptr;
    QTableWidget *m_transcriptTable = nullptr;
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
    QCheckBox *m_transcriptOverlayEnabledCheckBox = nullptr;
    QSpinBox *m_transcriptMaxLinesSpin = nullptr;
    QSpinBox *m_transcriptMaxCharsSpin = nullptr;
    QCheckBox *m_transcriptAutoScrollCheckBox = nullptr;
    QDoubleSpinBox *m_transcriptOverlayXSpin = nullptr;
    QDoubleSpinBox *m_transcriptOverlayYSpin = nullptr;
    QSpinBox *m_transcriptOverlayWidthSpin = nullptr;
    QSpinBox *m_transcriptOverlayHeightSpin = nullptr;
    QFontComboBox *m_transcriptFontFamilyCombo = nullptr;
    QSpinBox *m_transcriptFontSizeSpin = nullptr;
    QCheckBox *m_transcriptBoldCheckBox = nullptr;
    QCheckBox *m_transcriptItalicCheckBox = nullptr;
    QCheckBox *m_transcriptFollowCurrentWordCheckBox = nullptr;

    QSpinBox *m_outputWidthSpin = nullptr;
    QSpinBox *m_outputHeightSpin = nullptr;
    QSpinBox *m_exportStartSpin = nullptr;
    QSpinBox *m_exportEndSpin = nullptr;
    QComboBox *m_outputFormatCombo = nullptr;
    QLabel *m_outputRangeSummaryLabel = nullptr;
    QCheckBox *m_renderUseProxiesCheckBox = nullptr;
    QPushButton *m_renderButton = nullptr;

    QCheckBox *m_speechFilterEnabledCheckBox = nullptr;
    QSpinBox *m_transcriptPrependMsSpin = nullptr;
    QSpinBox *m_transcriptPostpendMsSpin = nullptr;
    QSpinBox *m_speechFilterFadeSamplesSpin = nullptr;
};
