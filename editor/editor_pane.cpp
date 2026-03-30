#include "editor_pane.h"

#include "preview.h"
#include "timeline_widget.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QSlider>
#include <QStackedLayout>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>

EditorPane::EditorPane(QWidget *parent)
    : QWidget(parent)
{
    setStyleSheet(
        QStringLiteral(
            "QWidget { background: #0c1015; color: #edf2f7; }"
            "QPushButton, QToolButton { background: #1b2430; border: 1px solid #2e3b4a; border-radius: 7px; padding: 8px 12px; }"
            "QPushButton:hover, QToolButton:hover { background: #233142; }"
            "QSlider::groove:horizontal { background: #24303c; height: 6px; border-radius: 3px; }"
            "QSlider::handle:horizontal { background: #ff6f61; width: 14px; margin: -5px 0; border-radius: 7px; }"));

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(0);

    auto *verticalSplitter = new QSplitter(Qt::Vertical, this);
    verticalSplitter->setObjectName(QStringLiteral("layout.editor_splitter"));
    verticalSplitter->setChildrenCollapsible(false);
    verticalSplitter->setHandleWidth(6);
    verticalSplitter->setStyleSheet(QStringLiteral(
        "QSplitter::handle { background: #1e2a36; }"
        "QSplitter::handle:hover { background: #3a5068; }"));
    layout->addWidget(verticalSplitter, 1);

    auto *previewFrame = new QFrame;
    previewFrame->setMinimumHeight(240);
    previewFrame->setFrameShape(QFrame::NoFrame);
    previewFrame->setStyleSheet(QStringLiteral(
        "QFrame { background: #05080c; border: 1px solid #202934; border-radius: 14px; }"));

    auto *previewLayout = new QVBoxLayout(previewFrame);
    previewLayout->setContentsMargins(0, 0, 0, 0);
    previewLayout->setSpacing(0);

    m_preview = new PreviewWindow;
    m_preview->setObjectName(QStringLiteral("preview.window"));
    m_preview->setFocusPolicy(Qt::StrongFocus);
    m_preview->setMinimumSize(320, 180);
    m_preview->setOutputSize(QSize(1080, 1920));

    auto *overlay = new QWidget;
    overlay->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    overlay->setStyleSheet(QStringLiteral("background: transparent;"));

    auto *overlayLayout = new QVBoxLayout(overlay);
    overlayLayout->setContentsMargins(18, 14, 18, 14);
    overlayLayout->setSpacing(6);

    auto *badgeRow = new QHBoxLayout;
    badgeRow->setContentsMargins(0, 0, 0, 0);

    m_statusBadge = new QLabel;
    m_statusBadge->setObjectName(QStringLiteral("overlay.status_badge"));
    m_statusBadge->setStyleSheet(QStringLiteral(
        "QLabel { background: rgba(7, 11, 17, 0.72); color: #f2f7fb; border-radius: 10px; padding: 8px 12px; font-weight: 600; }"));
    badgeRow->addWidget(m_statusBadge, 0, Qt::AlignLeft);
    badgeRow->addStretch(1);
    overlayLayout->addLayout(badgeRow);

    overlayLayout->addStretch(1);

    m_previewInfo = new QLabel;
    m_previewInfo->setObjectName(QStringLiteral("overlay.preview_info"));
    m_previewInfo->setStyleSheet(QStringLiteral(
        "QLabel { background: rgba(7, 11, 17, 0.72); color: #dce6ef; border-radius: 10px; padding: 10px 12px; }"));
    m_previewInfo->setWordWrap(true);
    overlayLayout->addWidget(m_previewInfo, 0, Qt::AlignLeft | Qt::AlignBottom);

    auto *stack = new QStackedLayout;
    stack->setStackingMode(QStackedLayout::StackAll);
    stack->addWidget(m_preview);
    stack->addWidget(overlay);
    previewLayout->addLayout(stack);

    verticalSplitter->addWidget(previewFrame);

    auto *timelinePane = new QWidget;
    timelinePane->setMinimumHeight(220);

    auto *timelineLayout = new QVBoxLayout(timelinePane);
    timelineLayout->setContentsMargins(0, 14, 0, 0);
    timelineLayout->setSpacing(14);

    auto *transport = new QWidget;
    auto *transportLayout = new QHBoxLayout(transport);
    transportLayout->setContentsMargins(0, 0, 0, 0);
    transportLayout->setSpacing(10);

    m_playButton = new QPushButton(style()->standardIcon(QStyle::SP_MediaPlay), QStringLiteral("Play"));
    m_playButton->setObjectName(QStringLiteral("transport.play"));

    m_startButton = new QToolButton;
    m_endButton = new QToolButton;
    m_startButton->setObjectName(QStringLiteral("transport.start"));
    m_endButton->setObjectName(QStringLiteral("transport.end"));
    m_startButton->setIcon(style()->standardIcon(QStyle::SP_MediaSkipBackward));
    m_endButton->setIcon(style()->standardIcon(QStyle::SP_MediaSkipForward));

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
    m_audioNowPlayingLabel->setMinimumWidth(80);

    m_razorButton = new QToolButton(transport);
    m_razorButton->setObjectName(QStringLiteral("transport.razor"));
    m_razorButton->setText(QStringLiteral("Razor"));
    m_razorButton->setCheckable(true);
    m_razorButton->setToolTip(QStringLiteral("Razor tool (B) \u2014 click to split clips"));
    m_razorButton->setStyleSheet(QStringLiteral(
        "QToolButton:checked { background: #3a4d63; border-color: #a0e0ff; color: #a0e0ff; }"));

    transportLayout->addWidget(m_startButton);
    transportLayout->addWidget(m_playButton);
    transportLayout->addWidget(m_endButton);
    transportLayout->addSpacing(12);
    transportLayout->addWidget(m_razorButton);
    transportLayout->addWidget(m_seekSlider, 1);
    transportLayout->addWidget(m_timecodeLabel);
    transportLayout->addWidget(m_audioMuteButton);
    transportLayout->addWidget(m_audioVolumeSlider);
    transportLayout->addWidget(m_audioNowPlayingLabel);
    timelineLayout->addWidget(transport, 0);

    m_timeline = new TimelineWidget;
    m_timeline->setObjectName(QStringLiteral("timeline.widget"));
    timelineLayout->addWidget(m_timeline, 1);

    verticalSplitter->addWidget(timelinePane);
    verticalSplitter->setStretchFactor(0, 3);
    verticalSplitter->setStretchFactor(1, 2);
    verticalSplitter->setSizes({540, 320});

    connect(m_playButton, &QPushButton::clicked, this, &EditorPane::playClicked);
    connect(m_startButton, &QToolButton::clicked, this, &EditorPane::startClicked);
    connect(m_endButton, &QToolButton::clicked, this, &EditorPane::endClicked);
    connect(m_seekSlider, &QSlider::valueChanged, this, &EditorPane::seekValueChanged);
    connect(m_audioMuteButton, &QToolButton::clicked, this, &EditorPane::audioMuteClicked);
    connect(m_audioVolumeSlider, &QSlider::valueChanged, this, &EditorPane::audioVolumeChanged);
}
