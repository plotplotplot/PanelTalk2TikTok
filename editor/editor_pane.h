#pragma once

#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QSplitter>
#include <QToolButton>
#include <QWidget>

class PreviewWindow;
class TimelineWidget;

class EditorPane final : public QWidget
{
    Q_OBJECT

public:
    explicit EditorPane(QWidget *parent = nullptr);

    PreviewWindow *previewWindow() const { return m_preview; }
    TimelineWidget *timelineWidget() const { return m_timeline; }

    QPushButton *playButton() const { return m_playButton; }
    QToolButton *audioMuteButton() const { return m_audioMuteButton; }
    QSlider *seekSlider() const { return m_seekSlider; }
    QSlider *audioVolumeSlider() const { return m_audioVolumeSlider; }
    QLabel *timecodeLabel() const { return m_timecodeLabel; }
    QLabel *audioNowPlayingLabel() const { return m_audioNowPlayingLabel; }
    QLabel *statusBadge() const { return m_statusBadge; }
    QLabel *previewInfo() const { return m_previewInfo; }

signals:
    void playClicked();
    void startClicked();
    void endClicked();
    void seekValueChanged(int value);
    void audioMuteClicked();
    void audioVolumeChanged(int value);

private:
    PreviewWindow *m_preview = nullptr;
    TimelineWidget *m_timeline = nullptr;

    QPushButton *m_playButton = nullptr;
    QToolButton *m_startButton = nullptr;
    QToolButton *m_endButton = nullptr;
    QSlider *m_seekSlider = nullptr;
    QLabel *m_timecodeLabel = nullptr;
    QToolButton *m_audioMuteButton = nullptr;
    QSlider *m_audioVolumeSlider = nullptr;
    QLabel *m_audioNowPlayingLabel = nullptr;
    QLabel *m_statusBadge = nullptr;
    QLabel *m_previewInfo = nullptr;
};
