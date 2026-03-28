#include "inspector_pane.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFontComboBox>
#include <QFormLayout>
#include <QFrame>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QScrollArea>
#include <QSplitter>
#include <QSpinBox>
#include <QTabBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QPushButton>
#include <QSize>
#include <QStyle>
#include <QVBoxLayout>

InspectorPane::InspectorPane(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(buildPane());
}

QWidget *InspectorPane::buildPane()
{
    auto *pane = new QFrame;
    pane->setMinimumWidth(320);
    pane->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);

    auto *layout = new QVBoxLayout(pane);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(8);

    m_inspectorTabs = new QTabWidget(pane);
    m_inspectorTabs->addTab(buildGradingTab(), QStringLiteral("Grade"));
    m_inspectorTabs->addTab(buildSyncTab(), QStringLiteral("Sync"));
    m_inspectorTabs->addTab(buildKeyframesTab(), QStringLiteral("Keyframes"));
    m_inspectorTabs->addTab(buildTranscriptTab(), QStringLiteral("Transcript"));
    m_inspectorTabs->addTab(buildClipTab(), QStringLiteral("Properties"));
    m_inspectorTabs->addTab(buildPreviewTab(), QStringLiteral("Preview"));
    m_inspectorTabs->addTab(buildOutputTab(), QStringLiteral("Output"));
    m_inspectorTabs->addTab(buildProfileTab(), QStringLiteral("System"));
    configureInspectorTabs();

    layout->addWidget(m_inspectorTabs);
    return pane;
}

void InspectorPane::configureInspectorTabs()
{
    if (!m_inspectorTabs) {
        return;
    }

    auto *bar = m_inspectorTabs->tabBar();
    m_inspectorTabs->setTabPosition(QTabWidget::East);
    m_inspectorTabs->setDocumentMode(true);
    bar->setExpanding(false);
    bar->setUsesScrollButtons(false);
    bar->setIconSize(QSize(18, 18));
    bar->setDrawBase(false);

    struct TabSpec {
        int index;
        QStyle::StandardPixmap icon;
        const char* tooltip;
    };

    const TabSpec specs[] = {
        {0, QStyle::SP_DriveDVDIcon, "Grade: clip color, opacity, and grading keyframes"},
        {1, QStyle::SP_BrowserReload, "Sync: render sync markers for the selected clip"},
        {2, QStyle::SP_FileDialogDetailedView, "Keyframes: transform keyframes for the selected clip"},
        {3, QStyle::SP_FileDialogContentsView, "Transcript: transcript editing and speech filter controls"},
        {4, QStyle::SP_FileDialogInfoView, "Properties: clip and track properties"},
        {5, QStyle::SP_MediaPlay, "Preview: editor preview display controls"},
        {6, QStyle::SP_DialogSaveButton, "Output: render settings and export"},
        {7, QStyle::SP_ComputerIcon, "System: playback, decoder, cache, and benchmark information"},
    };

    for (const TabSpec& spec : specs) {
        m_inspectorTabs->setTabIcon(spec.index, style()->standardIcon(spec.icon));
        m_inspectorTabs->setTabText(spec.index, QString());
        bar->setTabToolTip(spec.index, QString::fromUtf8(spec.tooltip));
    }

    bar->setStyleSheet(QStringLiteral(
        "QTabBar::tab {"
        " width: 34px;"
        " height: 34px;"
        " margin: 2px 0;"
        " padding: 0px;"
        " }"
        "QTabBar::tab:selected {"
        " background: palette(base);"
        " border: 1px solid palette(mid);"
        " border-right: 0px;"
        " border-top-left-radius: 6px;"
        " border-bottom-left-radius: 6px;"
        " }"
        "QTabBar::tab:!selected {"
        " background: palette(button);"
        " border: 1px solid palette(midlight);"
        " border-right: 0px;"
        " border-top-left-radius: 6px;"
        " border-bottom-left-radius: 6px;"
        " opacity: 0.8;"
        " }"));
}

QWidget *InspectorPane::buildGradingTab()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);

    m_gradingPathLabel = new QLabel(QStringLiteral("No visual clip selected"), page);
    m_gradingPathLabel->setWordWrap(true);
    layout->addWidget(m_gradingPathLabel);

    auto *form = new QFormLayout;
    m_brightnessSpin = new QDoubleSpinBox(page);
    m_contrastSpin = new QDoubleSpinBox(page);
    m_saturationSpin = new QDoubleSpinBox(page);
    m_opacitySpin = new QDoubleSpinBox(page);

    for (QDoubleSpinBox *spin : {m_brightnessSpin, m_contrastSpin, m_saturationSpin, m_opacitySpin})
    {
        spin->setRange(-10.0, 10.0);
        spin->setDecimals(3);
        spin->setSingleStep(0.05);
    }
    m_opacitySpin->setRange(0.0, 1.0);
    m_opacitySpin->setValue(1.0);

    form->addRow(QStringLiteral("Brightness"), m_brightnessSpin);
    form->addRow(QStringLiteral("Contrast"), m_contrastSpin);
    form->addRow(QStringLiteral("Saturation"), m_saturationSpin);
    form->addRow(QStringLiteral("Opacity"), m_opacitySpin);

    m_gradingAutoScrollCheckBox = new QCheckBox(QStringLiteral("Auto Scroll"), page);
    m_gradingFollowCurrentCheckBox = new QCheckBox(QStringLiteral("Follow Current Keyframe"), page);
    m_gradingAutoScrollCheckBox->setChecked(true);
    m_gradingFollowCurrentCheckBox->setChecked(true);
    m_gradingKeyAtPlayheadButton = new QPushButton(QStringLiteral("Key At Playhead"), page);
    m_gradingFadeInButton = new QPushButton(QStringLiteral("Fade In From Playhead"), page);
    m_gradingFadeOutButton = new QPushButton(QStringLiteral("Fade Out From Playhead"), page);

    m_gradingKeyframeTable = new QTableWidget(page);
    m_gradingKeyframeTable->setColumnCount(6);
    m_gradingKeyframeTable->setHorizontalHeaderLabels({QStringLiteral("Frame"),
                                                       QStringLiteral("Bright"),
                                                       QStringLiteral("Contrast"),
                                                       QStringLiteral("Sat"),
                                                       QStringLiteral("Opacity"),
                                                       QStringLiteral("Interp")});
    m_gradingKeyframeTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_gradingKeyframeTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_gradingKeyframeTable->setEditTriggers(QAbstractItemView::DoubleClicked |
                                            QAbstractItemView::EditKeyPressed);
    m_gradingKeyframeTable->verticalHeader()->setVisible(false);
    m_gradingKeyframeTable->horizontalHeader()->setStretchLastSection(true);
    m_gradingKeyframeTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);

    layout->addLayout(form);
    layout->addWidget(m_gradingAutoScrollCheckBox);
    layout->addWidget(m_gradingFollowCurrentCheckBox);
    layout->addWidget(m_gradingKeyAtPlayheadButton);
    layout->addWidget(m_gradingFadeInButton);
    layout->addWidget(m_gradingFadeOutButton);
    layout->addWidget(m_gradingKeyframeTable, 1);
    return page;
}

QWidget *InspectorPane::buildSyncTab()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);

    m_syncInspectorClipLabel = new QLabel(QStringLiteral("Sync"), page);
    m_syncInspectorDetailsLabel = new QLabel(QStringLiteral("No render sync markers in the timeline."), page);
    m_syncInspectorDetailsLabel->setWordWrap(true);

    m_syncTable = new QTableWidget(page);
    m_syncTable->setColumnCount(4);
    m_syncTable->setHorizontalHeaderLabels(
        {QStringLiteral("Clip"), QStringLiteral("Frame"), QStringLiteral("Count"), QStringLiteral("Action")});
    m_syncTable->horizontalHeader()->setStretchLastSection(true);

    layout->addWidget(m_syncInspectorClipLabel);
    layout->addWidget(m_syncInspectorDetailsLabel);
    layout->addWidget(m_syncTable, 1);

    return page;
}

QWidget *InspectorPane::buildKeyframesTab()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);

    m_keyframesInspectorClipLabel = new QLabel(QStringLiteral("No visual clip selected"), page);
    m_keyframesInspectorDetailsLabel = new QLabel(QStringLiteral("Select a visual clip to inspect its keyframes."), page);
    m_keyframesInspectorDetailsLabel->setWordWrap(true);

    auto *form = new QFormLayout;
    m_videoTranslationXSpin = new QDoubleSpinBox(page);
    m_videoTranslationYSpin = new QDoubleSpinBox(page);
    m_videoRotationSpin = new QDoubleSpinBox(page);
    m_videoScaleXSpin = new QDoubleSpinBox(page);
    m_videoScaleYSpin = new QDoubleSpinBox(page);
    m_videoInterpolationCombo = new QComboBox(page);
    m_mirrorHorizontalCheckBox = new QCheckBox(QStringLiteral("Mirror Horizontal"), page);
    m_mirrorVerticalCheckBox = new QCheckBox(QStringLiteral("Mirror Vertical"), page);
    m_lockVideoScaleCheckBox = new QCheckBox(QStringLiteral("Lock Scale"), page);
    m_keyframeSpaceCheckBox = new QCheckBox(QStringLiteral("Clip-Relative Frames"), page);
    m_addVideoKeyframeButton = new QPushButton(QStringLiteral("Add Keyframe"), page);
    m_removeVideoKeyframeButton = new QPushButton(QStringLiteral("Remove Keyframe"), page);

    m_videoInterpolationCombo->addItem(QStringLiteral("Step"));
    m_videoInterpolationCombo->addItem(QStringLiteral("Linear"));
    m_lockVideoScaleCheckBox->setChecked(false);
    m_keyframeSpaceCheckBox->setChecked(true);

    for (QDoubleSpinBox *spin : {
             m_videoTranslationXSpin, m_videoTranslationYSpin, m_videoRotationSpin,
             m_videoScaleXSpin, m_videoScaleYSpin})
    {
        spin->setDecimals(3);
        spin->setRange(-100000.0, 100000.0);
    }
    m_videoScaleXSpin->setValue(1.0);
    m_videoScaleYSpin->setValue(1.0);

    form->addRow(QStringLiteral("Translate X"), m_videoTranslationXSpin);
    form->addRow(QStringLiteral("Translate Y"), m_videoTranslationYSpin);
    form->addRow(QStringLiteral("Rotation"), m_videoRotationSpin);
    form->addRow(QStringLiteral("Scale X"), m_videoScaleXSpin);
    form->addRow(QStringLiteral("Scale Y"), m_videoScaleYSpin);
    form->addRow(QStringLiteral("Interpolation"), m_videoInterpolationCombo);

    auto *buttonRow = new QHBoxLayout;
    buttonRow->addWidget(m_addVideoKeyframeButton);
    buttonRow->addWidget(m_removeVideoKeyframeButton);

    m_keyframesAutoScrollCheckBox = new QCheckBox(QStringLiteral("Auto Scroll"), page);
    m_keyframesFollowCurrentCheckBox = new QCheckBox(QStringLiteral("Follow Current Keyframe"), page);
    m_keyframesAutoScrollCheckBox->setChecked(true);
    m_keyframesFollowCurrentCheckBox->setChecked(true);

    m_videoKeyframeTable = new QTableWidget(page);
    m_videoKeyframeTable->setColumnCount(7);
    m_videoKeyframeTable->setHorizontalHeaderLabels({QStringLiteral("Frame"),
                                                     QStringLiteral("X"),
                                                     QStringLiteral("Y"),
                                                     QStringLiteral("Rot"),
                                                     QStringLiteral("Scale X"),
                                                     QStringLiteral("Scale Y"),
                                                     QStringLiteral("Interp")});
    m_videoKeyframeTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_videoKeyframeTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_videoKeyframeTable->setEditTriggers(QAbstractItemView::DoubleClicked |
                                          QAbstractItemView::EditKeyPressed);
    m_videoKeyframeTable->verticalHeader()->setVisible(false);
    m_videoKeyframeTable->horizontalHeader()->setStretchLastSection(true);
    m_videoKeyframeTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);

    layout->addWidget(m_keyframesInspectorClipLabel);
    layout->addWidget(m_keyframesInspectorDetailsLabel);
    layout->addLayout(form);
    layout->addWidget(m_lockVideoScaleCheckBox);
    layout->addWidget(m_keyframeSpaceCheckBox);
    layout->addWidget(m_keyframesAutoScrollCheckBox);
    layout->addWidget(m_keyframesFollowCurrentCheckBox);
    layout->addWidget(m_mirrorHorizontalCheckBox);
    layout->addWidget(m_mirrorVerticalCheckBox);
    layout->addLayout(buttonRow);
    layout->addWidget(m_videoKeyframeTable, 1);

    return page;
}

QWidget *InspectorPane::buildTranscriptTab()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);

    auto *splitter = new QSplitter(Qt::Vertical, page);
    splitter->setChildrenCollapsible(false);

    auto *settingsContainer = new QWidget(splitter);
    auto *settingsLayout = new QVBoxLayout(settingsContainer);
    settingsLayout->setContentsMargins(8, 8, 8, 8);

    m_transcriptInspectorClipLabel = new QLabel(QStringLiteral("No transcript selected"), settingsContainer);
    m_transcriptInspectorDetailsLabel = new QLabel(QStringLiteral("Select a clip with a WhisperX JSON transcript."), settingsContainer);
    m_transcriptInspectorDetailsLabel->setWordWrap(true);

    auto *form = new QFormLayout;
    m_transcriptOverlayEnabledCheckBox = new QCheckBox(QStringLiteral("Enable Overlay"), settingsContainer);
    m_transcriptMaxLinesSpin = new QSpinBox(settingsContainer);
    m_transcriptMaxCharsSpin = new QSpinBox(settingsContainer);
    m_transcriptAutoScrollCheckBox = new QCheckBox(QStringLiteral("Auto Scroll"), settingsContainer);
    m_transcriptFollowCurrentWordCheckBox = new QCheckBox(QStringLiteral("Follow Current Word"), settingsContainer);
    m_transcriptOverlayXSpin = new QDoubleSpinBox(settingsContainer);
    m_transcriptOverlayYSpin = new QDoubleSpinBox(settingsContainer);
    m_transcriptOverlayWidthSpin = new QSpinBox(settingsContainer);
    m_transcriptOverlayHeightSpin = new QSpinBox(settingsContainer);
    m_transcriptFontFamilyCombo = new QFontComboBox(settingsContainer);
    m_transcriptFontSizeSpin = new QSpinBox(settingsContainer);
    m_transcriptBoldCheckBox = new QCheckBox(QStringLiteral("Bold"), settingsContainer);
    m_transcriptItalicCheckBox = new QCheckBox(QStringLiteral("Italic"), settingsContainer);

    m_transcriptMaxLinesSpin->setRange(1, 20);
    m_transcriptMaxCharsSpin->setRange(1, 200);
    m_transcriptOverlayWidthSpin->setRange(1, 10000);
    m_transcriptOverlayHeightSpin->setRange(1, 10000);
    m_transcriptFontSizeSpin->setRange(8, 256);

    form->addRow(QStringLiteral("Overlay"), m_transcriptOverlayEnabledCheckBox);
    form->addRow(QStringLiteral("Max Lines"), m_transcriptMaxLinesSpin);
    form->addRow(QStringLiteral("Max Chars"), m_transcriptMaxCharsSpin);
    form->addRow(QStringLiteral("Auto Scroll"), m_transcriptAutoScrollCheckBox);
    form->addRow(QStringLiteral("Follow Word"), m_transcriptFollowCurrentWordCheckBox);
    form->addRow(QStringLiteral("X"), m_transcriptOverlayXSpin);
    form->addRow(QStringLiteral("Y"), m_transcriptOverlayYSpin);
    form->addRow(QStringLiteral("Width"), m_transcriptOverlayWidthSpin);
    form->addRow(QStringLiteral("Height"), m_transcriptOverlayHeightSpin);
    form->addRow(QStringLiteral("Font"), m_transcriptFontFamilyCombo);
    form->addRow(QStringLiteral("Font Size"), m_transcriptFontSizeSpin);
    form->addRow(QStringLiteral("Bold"), m_transcriptBoldCheckBox);
    form->addRow(QStringLiteral("Italic"), m_transcriptItalicCheckBox);

    auto *speechSectionLabel = new QLabel(QStringLiteral("Speech Filter"), settingsContainer);
    speechSectionLabel->setStyleSheet(QStringLiteral("font-weight: 600; color: #8fa3b8;"));

    auto *speechForm = new QFormLayout;
    m_speechFilterEnabledCheckBox = new QCheckBox(QStringLiteral("Enable Speech Filter"), settingsContainer);
    m_transcriptPrependMsSpin = new QSpinBox(settingsContainer);
    m_transcriptPostpendMsSpin = new QSpinBox(settingsContainer);
    m_speechFilterFadeSamplesSpin = new QSpinBox(settingsContainer);

    m_transcriptPrependMsSpin->setRange(0, 10000);
    m_transcriptPrependMsSpin->setValue(0);
    m_transcriptPrependMsSpin->setSuffix(QStringLiteral(" ms"));
    m_transcriptPrependMsSpin->setToolTip(QStringLiteral("Milliseconds to add before each word"));

    m_transcriptPostpendMsSpin->setRange(0, 10000);
    m_transcriptPostpendMsSpin->setValue(0);
    m_transcriptPostpendMsSpin->setSuffix(QStringLiteral(" ms"));
    m_transcriptPostpendMsSpin->setToolTip(QStringLiteral("Milliseconds to add after each word"));

    m_speechFilterFadeSamplesSpin->setRange(0, 5000);
    m_speechFilterFadeSamplesSpin->setValue(250);
    m_speechFilterFadeSamplesSpin->setSuffix(QStringLiteral(" samples"));
    m_speechFilterFadeSamplesSpin->setToolTip(QStringLiteral("Crossfade duration at speech boundaries (0 = no fade)"));

    speechForm->addRow(QStringLiteral("Speech Filter"), m_speechFilterEnabledCheckBox);
    speechForm->addRow(QStringLiteral("Prepend Time"), m_transcriptPrependMsSpin);
    speechForm->addRow(QStringLiteral("Postpend Time"), m_transcriptPostpendMsSpin);
    speechForm->addRow(QStringLiteral("Fade Length"), m_speechFilterFadeSamplesSpin);

    m_transcriptTable = new QTableWidget(splitter);
    m_transcriptTable->setColumnCount(3);
    m_transcriptTable->setHorizontalHeaderLabels(
        {QStringLiteral("Start"), QStringLiteral("End"), QStringLiteral("Text")});
    m_transcriptTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_transcriptTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_transcriptTable->setEditTriggers(QAbstractItemView::DoubleClicked |
                                       QAbstractItemView::EditKeyPressed);
    m_transcriptTable->verticalHeader()->setVisible(false);
    m_transcriptTable->horizontalHeader()->setStretchLastSection(true);

    settingsLayout->addWidget(m_transcriptInspectorClipLabel);
    settingsLayout->addWidget(m_transcriptInspectorDetailsLabel);
    settingsLayout->addLayout(form);
    settingsLayout->addWidget(speechSectionLabel);
    settingsLayout->addLayout(speechForm);
    settingsLayout->addStretch(1);

    auto *settingsScroll = new QScrollArea(page);
    settingsScroll->setWidgetResizable(true);
    settingsScroll->setFrameShape(QFrame::NoFrame);
    settingsScroll->setWidget(settingsContainer);

    splitter->addWidget(settingsScroll);
    splitter->addWidget(m_transcriptTable);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({360, 420});

    layout->addWidget(splitter);

    return page;
}

QWidget *InspectorPane::buildOutputTab()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);

    m_outputRangeSummaryLabel = new QLabel(QStringLiteral("Timeline export range: 00:00:00:00 -> 00:00:10:00"), page);
    m_outputRangeSummaryLabel->setWordWrap(true);
    m_outputRangeSummaryLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);

    auto *form = new QFormLayout;
    m_outputWidthSpin = new QSpinBox(page);
    m_outputHeightSpin = new QSpinBox(page);
    m_exportStartSpin = new QSpinBox(page);
    m_exportEndSpin = new QSpinBox(page);
    m_outputFormatCombo = new QComboBox(page);

    m_outputWidthSpin->setRange(16, 7680);
    m_outputWidthSpin->setValue(1080);
    m_outputHeightSpin->setRange(16, 4320);
    m_outputHeightSpin->setValue(1920);
    m_exportStartSpin->setRange(0, 999999);
    m_exportEndSpin->setRange(0, 999999);

    m_outputFormatCombo->addItem(QStringLiteral("MP4"), QStringLiteral("mp4"));
    m_outputFormatCombo->addItem(QStringLiteral("MOV"), QStringLiteral("mov"));
    m_outputFormatCombo->addItem(QStringLiteral("WebM"), QStringLiteral("webm"));
    m_renderUseProxiesCheckBox = new QCheckBox(QStringLiteral("Use Proxies For Render"), page);

    form->addRow(QStringLiteral("Output Width"), m_outputWidthSpin);
    form->addRow(QStringLiteral("Output Height"), m_outputHeightSpin);
    form->addRow(QStringLiteral("Export Start Frame"), m_exportStartSpin);
    form->addRow(QStringLiteral("Export End Frame"), m_exportEndSpin);
    form->addRow(QStringLiteral("Output Format"), m_outputFormatCombo);

    m_renderButton = new QPushButton(QStringLiteral("Render"), page);

    layout->addWidget(m_outputRangeSummaryLabel);
    layout->addLayout(form);
    layout->addWidget(m_renderUseProxiesCheckBox);
    layout->addWidget(m_renderButton);
    layout->addStretch(1);

    return page;
}

QWidget *InspectorPane::buildPreviewTab()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);

    auto *summary = new QLabel(QStringLiteral("Preview controls affect only the editor preview."), page);
    summary->setWordWrap(true);

    m_previewHideOutsideOutputCheckBox =
        new QCheckBox(QStringLiteral("Hide Content Outside Output Window"), page);
    m_previewHideOutsideOutputCheckBox->setChecked(false);
    m_previewHideOutsideOutputCheckBox->setToolTip(
        QStringLiteral("Clip the preview to the current output frame so off-frame content is hidden."));

    layout->addWidget(summary);
    layout->addWidget(m_previewHideOutsideOutputCheckBox);
    layout->addStretch(1);
    return page;
}

QWidget *InspectorPane::buildClipTab()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    auto *form = new QFormLayout;

    m_clipInspectorClipLabel = new QLabel(QStringLiteral("No clip selected"), page);
    m_clipProxyUsageLabel = new QLabel(QStringLiteral("Playback Source: None"), page);
    m_clipPlaybackSourceLabel = new QLabel(QStringLiteral("Proxy In Use: No"), page);
    m_clipOriginalInfoLabel = new QLabel(QStringLiteral("Original\nNo clip selected."), page);
    m_clipProxyInfoLabel = new QLabel(QStringLiteral("Proxy\nNo proxy configured."), page);
    m_clipPlaybackRateSpin = new QDoubleSpinBox(page);
    m_clipPlaybackRateSpin->setDecimals(4);
    m_clipPlaybackRateSpin->setRange(0.001, 4.0);
    m_clipPlaybackRateSpin->setSingleStep(0.0001);
    m_clipPlaybackRateSpin->setValue(1.0);
    m_clipPlaybackRateSpin->setToolTip(
        QStringLiteral("Adjust clip playback speed with fine precision. "
                       "Example: 0.9999 for a slight slow-down."));
    m_clipPlaybackRateSpin->setValue(1.0);
    auto *trackSectionLabel = new QLabel(QStringLiteral("Track"), page);
    trackSectionLabel->setStyleSheet(QStringLiteral("font-weight: 600; color: #8fa3b8;"));
    m_trackInspectorLabel = new QLabel(QStringLiteral("No track selected"), page);
    m_trackInspectorDetailsLabel = new QLabel(QStringLiteral("Select a track header to edit track-wide properties."), page);
    m_trackNameEdit = new QLineEdit(page);
    m_trackHeightSpin = new QSpinBox(page);
    m_trackHeightSpin->setRange(28, 240);
    m_trackVideoEnabledCheckBox = new QCheckBox(QStringLiteral("Track Video Enabled"), page);
    m_trackAudioEnabledCheckBox = new QCheckBox(QStringLiteral("Track Audio Enabled"), page);
    m_trackCrossfadeSecondsSpin = new QDoubleSpinBox(page);
    m_trackCrossfadeSecondsSpin->setDecimals(2);
    m_trackCrossfadeSecondsSpin->setRange(0.01, 30.0);
    m_trackCrossfadeSecondsSpin->setSingleStep(0.05);
    m_trackCrossfadeSecondsSpin->setValue(0.50);
    m_trackCrossfadeButton = new QPushButton(QStringLiteral("Crossfade Consecutive Clips"), page);
    auto *audioSectionLabel = new QLabel(QStringLiteral("Audio"), page);
    audioSectionLabel->setStyleSheet(QStringLiteral("font-weight: 600; color: #8fa3b8;"));
    m_audioInspectorClipLabel = new QLabel(QStringLiteral("No audio clip selected"), page);
    m_audioInspectorDetailsLabel = new QLabel(QStringLiteral("Select an audio clip to inspect playback details."), page);

    for (QLabel *label : {m_clipInspectorClipLabel, m_clipProxyUsageLabel, m_clipPlaybackSourceLabel,
                          m_clipOriginalInfoLabel, m_clipProxyInfoLabel,
                          m_trackInspectorLabel, m_trackInspectorDetailsLabel,
                          m_audioInspectorClipLabel, m_audioInspectorDetailsLabel})
    {
        label->setWordWrap(true);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    }

    layout->addWidget(m_clipInspectorClipLabel);
    layout->addWidget(m_clipProxyUsageLabel);
    layout->addWidget(m_clipPlaybackSourceLabel);
    form->addRow(QStringLiteral("Playback Speed"), m_clipPlaybackRateSpin);
    layout->addLayout(form);
    layout->addWidget(m_clipOriginalInfoLabel);
    layout->addWidget(m_clipProxyInfoLabel);
    auto *trackForm = new QFormLayout;
    trackForm->addRow(QStringLiteral("Track Name"), m_trackNameEdit);
    trackForm->addRow(QStringLiteral("Track Height"), m_trackHeightSpin);
    trackForm->addRow(QStringLiteral("Crossfade"), m_trackCrossfadeSecondsSpin);
    layout->addWidget(trackSectionLabel);
    layout->addWidget(m_trackInspectorLabel);
    layout->addWidget(m_trackInspectorDetailsLabel);
    layout->addLayout(trackForm);
    layout->addWidget(m_trackVideoEnabledCheckBox);
    layout->addWidget(m_trackAudioEnabledCheckBox);
    layout->addWidget(m_trackCrossfadeButton);
    layout->addWidget(audioSectionLabel);
    layout->addWidget(m_audioInspectorClipLabel);
    layout->addWidget(m_audioInspectorDetailsLabel);
    layout->addStretch(1);

    return page;
}

QWidget *InspectorPane::buildProfileTab()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);

    m_profileSummaryTable = new QTableWidget(page);
    m_profileSummaryTable->setColumnCount(2);
    m_profileSummaryTable->setHorizontalHeaderLabels({QStringLiteral("Property"), QStringLiteral("Value")});
    m_profileSummaryTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_profileSummaryTable->setSelectionMode(QAbstractItemView::NoSelection);
    m_profileSummaryTable->setFocusPolicy(Qt::NoFocus);
    m_profileSummaryTable->verticalHeader()->setVisible(false);
    m_profileSummaryTable->horizontalHeader()->setStretchLastSection(true);
    m_profileSummaryTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_profileSummaryTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);

    m_profileBenchmarkButton = new QPushButton(QStringLiteral("Run Decode Benchmark"), page);

    layout->addWidget(m_profileSummaryTable, 1);
    layout->addWidget(m_profileBenchmarkButton);
    return page;
}

void InspectorPane::refresh()
{
    emit refreshRequested();
}
