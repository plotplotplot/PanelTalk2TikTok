#pragma once

#include <QObject>
#include <QTableWidget>
#include <QLabel>
#include <QCheckBox>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QFontComboBox>
#include <QJsonDocument>
#include <functional>

#include "editor_shared.h"
#include "transcript_engine.h"

class TranscriptTab : public QObject
{
    Q_OBJECT

public:
    struct Widgets
    {
        QLabel* transcriptInspectorClipLabel = nullptr;
        QLabel* transcriptInspectorDetailsLabel = nullptr;
        QTableWidget* transcriptTable = nullptr;
        QCheckBox* transcriptOverlayEnabledCheckBox = nullptr;
        QSpinBox* transcriptMaxLinesSpin = nullptr;
        QSpinBox* transcriptMaxCharsSpin = nullptr;
        QCheckBox* transcriptAutoScrollCheckBox = nullptr;
        QCheckBox* transcriptFollowCurrentWordCheckBox = nullptr;
        QDoubleSpinBox* transcriptOverlayXSpin = nullptr;
        QDoubleSpinBox* transcriptOverlayYSpin = nullptr;
        QSpinBox* transcriptOverlayWidthSpin = nullptr;
        QSpinBox* transcriptOverlayHeightSpin = nullptr;
        QFontComboBox* transcriptFontFamilyCombo = nullptr;
        QSpinBox* transcriptFontSizeSpin = nullptr;
        QCheckBox* transcriptBoldCheckBox = nullptr;
        QCheckBox* transcriptItalicCheckBox = nullptr;
        QSpinBox* transcriptPrependMsSpin = nullptr;
        QSpinBox* transcriptPostpendMsSpin = nullptr;
        QCheckBox* speechFilterEnabledCheckBox = nullptr;
        QSpinBox* speechFilterFadeSamplesSpin = nullptr;
    };

    struct Dependencies
    {
        std::function<const TimelineClip*()> getSelectedClip;
        std::function<bool(const QString&, const std::function<void(TimelineClip&)>&)> updateClipById;
        std::function<void()> scheduleSaveState;
        std::function<void()> pushHistorySnapshot;
        std::function<void()> refreshInspector;
        std::function<void()> setPreviewTimelineClips;
        std::function<QVector<ExportRangeSegment>()> effectivePlaybackRanges;
        std::function<void(int64_t)> seekToTimelineFrame;
    };

    explicit TranscriptTab(const Widgets& widgets, const Dependencies& deps, QObject* parent = nullptr);
    ~TranscriptTab() override = default;

    void wire();
    void refresh();
    void applyOverlayFromInspector(bool pushHistory = false);
    void syncTableToPlayhead(int64_t absolutePlaybackSample, int64_t sourceFrame);
    void applyTableEdit(QTableWidgetItem* item);
    void deleteSelectedRows();
    int transcriptPrependMs() const { return m_transcriptPrependMs; }
    int transcriptPostpendMs() const { return m_transcriptPostpendMs; }
    int speechFilterFadeSamples() const { return m_speechFilterFadeSamples; }
    bool speechFilterEnabled() const { return m_speechFilterEnabled; }

signals:
    void speechFilterSettingsChanged();

private slots:
    void onTranscriptItemClicked(QTableWidgetItem* item);
    void onFollowCurrentWordToggled(bool checked);
    void onOverlaySettingChanged();
    void onPrependMsChanged(int value);
    void onPostpendMsChanged(int value);
    void onSpeechFilterEnabledToggled(bool enabled);
    void onSpeechFilterFadeSamplesChanged(int value);

private:
    struct TranscriptRow
    {
        int64_t startFrame = 0;
        int64_t endFrame = 0;
        QString text;
        bool isGap = false;
        int segmentIndex = -1;
        int wordIndex = -1;
    };

    void updateOverlayWidgetsFromClip(const TimelineClip& clip);
    void loadTranscriptFile(const TimelineClip& clip);
    QVector<TranscriptRow> parseTranscriptRows(const QJsonArray& segments, int prependMs, int postpendMs);
    void populateTable(const QVector<TranscriptRow>& rows);
    void adjustOverlappingRows(QVector<TranscriptRow>& rows);

    Widgets m_widgets;
    Dependencies m_deps;
    editor::TranscriptEngine m_transcriptEngine;
    bool m_updating = false;
    QString m_loadedTranscriptPath;
    QJsonDocument m_loadedTranscriptDoc;
    int m_transcriptPrependMs = 0;
    int m_transcriptPostpendMs = 0;
    int m_speechFilterFadeSamples = 250;
    bool m_speechFilterEnabled = false;
};
