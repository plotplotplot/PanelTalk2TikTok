#pragma once

#include <QObject>
#include <QTableWidget>
#include <QLabel>
#include <QDoubleSpinBox>
#include <QCheckBox>
#include <QPushButton>
#include <functional>

#include "editor_shared.h"

class GradingTab : public QObject
{
    Q_OBJECT

public:
    struct Widgets
    {
        QLabel* gradingPathLabel = nullptr;
        QDoubleSpinBox* brightnessSpin = nullptr;
        QDoubleSpinBox* contrastSpin = nullptr;
        QDoubleSpinBox* saturationSpin = nullptr;
        QDoubleSpinBox* opacitySpin = nullptr;
        QTableWidget* gradingKeyframeTable = nullptr;
        QCheckBox* gradingAutoScrollCheckBox = nullptr;
        QCheckBox* gradingFollowCurrentCheckBox = nullptr;
        QPushButton* gradingKeyAtPlayheadButton = nullptr;
        QPushButton* gradingFadeInButton = nullptr;
        QPushButton* gradingFadeOutButton = nullptr;
    };

    struct Dependencies
    {
        std::function<const TimelineClip*()> getSelectedClip;
        std::function<const TimelineClip*()> getSelectedClipConst;
        std::function<bool(const QString&, const std::function<void(TimelineClip&)>&)> updateClipById;
        std::function<QString(const TimelineClip&)> getClipFilePath;
        std::function<bool(const TimelineClip&)> clipHasVisuals;
        std::function<void()> scheduleSaveState;
        std::function<void()> pushHistorySnapshot;
        std::function<void()> refreshInspector;
        std::function<void()> setPreviewTimelineClips;
        std::function<int64_t()> getCurrentTimelineFrame;
        std::function<int64_t()> getSelectedClipStartFrame;
        std::function<QString()> getSelectedClipId;
        std::function<void(int64_t)> seekToTimelineFrame;
        std::function<void(QTableWidgetItem*)> onKeyframeItemChanged;
        std::function<void()> onKeyframeSelectionChanged;
    };

    explicit GradingTab(const Widgets& widgets, const Dependencies& deps, QObject* parent = nullptr);
    ~GradingTab() override = default;

    void wire();
    void refresh();
    void applyGradeFromInspector(bool pushHistory = false);
    void upsertKeyframeAtPlayhead();
    void fadeInFromPlayhead();
    void fadeOutFromPlayhead();
    void removeSelectedKeyframes();
    void syncTableToPlayhead();
    void setSelectedKeyframeFrame(int64_t frame);
    int64_t selectedKeyframeFrame() const { return m_selectedKeyframeFrame; }
    const QSet<int64_t>& selectedKeyframeFrames() const { return m_selectedKeyframeFrames; }

signals:
    void gradeApplied();
    void keyframeAdded();
    void keyframesRemoved();

private slots:
    void onBrightnessChanged(double value);
    void onContrastChanged(double value);
    void onSaturationChanged(double value);
    void onOpacityChanged(double value);
    void onBrightnessEditingFinished();
    void onContrastEditingFinished();
    void onSaturationEditingFinished();
    void onOpacityEditingFinished();
    void onAutoScrollToggled(bool checked);
    void onFollowCurrentToggled(bool checked);
    void onKeyAtPlayheadClicked();
    void onFadeInClicked();
    void onFadeOutClicked();
    void onTableSelectionChanged();
    void onTableItemChanged(QTableWidgetItem* item);
    void onTableItemClicked(QTableWidgetItem* item);
    void onTableCustomContextMenu(const QPoint& pos);

private:
    struct GradingKeyframeDisplay
    {
        int64_t frame = 0;
        double brightness = 0.0;
        double contrast = 1.0;
        double saturation = 1.0;
        double opacity = 1.0;
        bool linearInterpolation = true;
    };

    QString videoInterpolationLabel(bool linearInterpolation) const;
    QString nextVideoInterpolationLabel(const QString& text) const;
    bool parseVideoInterpolationText(const QString& text, bool* linearInterpolationOut) const;
    int selectedKeyframeIndex(const TimelineClip& clip) const;
    QList<int64_t> selectedKeyframeFramesForClip(const TimelineClip& clip) const;
    int nearestKeyframeIndex(const TimelineClip& clip) const;
    bool hasRemovableKeyframeSelection(const TimelineClip& clip) const;
    GradingKeyframeDisplay evaluateDisplayedGrading(const TimelineClip& clip, int64_t localFrame) const;
    void updateSpinBoxesFromKeyframe(const GradingKeyframeDisplay& keyframe);
    void populateTable(const TimelineClip& clip);
    void applyOpacityFadeFromPlayhead(bool fadeIn);

    Widgets m_widgets;
    Dependencies m_deps;
    bool m_updating = false;
    bool m_syncingTableSelection = false;
    int64_t m_selectedKeyframeFrame = -1;
    QSet<int64_t> m_selectedKeyframeFrames;
};
