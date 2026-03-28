#pragma once

#include <QObject>
#include <QTableWidget>
#include <QLabel>
#include <QDoubleSpinBox>
#include <QCheckBox>
#include <QComboBox>
#include <QPushButton>
#include <QTimer>
#include <functional>

#include "editor_shared.h"

class VideoKeyframeTab : public QObject
{
    Q_OBJECT

public:
    struct Widgets
    {
        QLabel* keyframesInspectorClipLabel = nullptr;
        QLabel* keyframesInspectorDetailsLabel = nullptr;
        QTableWidget* videoKeyframeTable = nullptr;
        QDoubleSpinBox* videoTranslationXSpin = nullptr;
        QDoubleSpinBox* videoTranslationYSpin = nullptr;
        QDoubleSpinBox* videoRotationSpin = nullptr;
        QDoubleSpinBox* videoScaleXSpin = nullptr;
        QDoubleSpinBox* videoScaleYSpin = nullptr;
        QComboBox* videoInterpolationCombo = nullptr;
        QCheckBox* mirrorHorizontalCheckBox = nullptr;
        QCheckBox* mirrorVerticalCheckBox = nullptr;
        QCheckBox* lockVideoScaleCheckBox = nullptr;
        QCheckBox* keyframeSpaceCheckBox = nullptr;
        QCheckBox* keyframesAutoScrollCheckBox = nullptr;
        QCheckBox* keyframesFollowCurrentCheckBox = nullptr;
        QPushButton* addVideoKeyframeButton = nullptr;
        QPushButton* removeVideoKeyframeButton = nullptr;
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

    explicit VideoKeyframeTab(const Widgets& widgets, const Dependencies& deps, QObject* parent = nullptr);
    ~VideoKeyframeTab() override = default;

    void wire();
    void refresh();
    void applyKeyframeFromInspector(bool pushHistory = false);
    void upsertKeyframeAtPlayhead();
    void removeSelectedKeyframes();
    void duplicateSelectedKeyframes(int frameDelta);
    void duplicateSelectedKeyframesToFrame(int64_t targetFrame);
    bool insertInterpolatedKeyframeBetween(int64_t earlierFrame, int64_t laterFrame);
    void syncTableToPlayhead();
    void setSelectedKeyframeFrame(int64_t frame);
    int64_t selectedKeyframeFrame() const { return m_selectedKeyframeFrame; }
    const QSet<int64_t>& selectedKeyframeFrames() const { return m_selectedKeyframeFrames; }
    void promptMultiplySelectedKeyframeScale(bool scaleX);

signals:
    void keyframeApplied();
    void keyframeAdded();
    void keyframesRemoved();

private slots:
    void onTranslationXChanged(double value);
    void onTranslationYChanged(double value);
    void onRotationChanged(double value);
    void onScaleXChanged(double value);
    void onScaleYChanged(double value);
    void onTranslationXEditingFinished();
    void onTranslationYEditingFinished();
    void onRotationEditingFinished();
    void onScaleXEditingFinished();
    void onScaleYEditingFinished();
    void onInterpolationChanged(int index);
    void onMirrorHorizontalToggled(bool checked);
    void onMirrorVerticalToggled(bool checked);
    void onLockScaleToggled(bool checked);
    void onKeyframeSpaceToggled(bool checked);
    void onAutoScrollToggled(bool checked);
    void onFollowCurrentToggled(bool checked);
    void onAddKeyframeClicked();
    void onRemoveKeyframeClicked();
    void onTableSelectionChanged();
    void onTableItemChanged(QTableWidgetItem* item);
    void onTableItemClicked(QTableWidgetItem* item);
    void onTableItemDoubleClicked(QTableWidgetItem* item);
    void onTableHeaderClicked(int section);
    void onTableCustomContextMenu(const QPoint& pos);

private:
    struct TransformKeyframeDisplay
    {
        int64_t frame = 0;
        double translationX = 0.0;
        double translationY = 0.0;
        double rotation = 0.0;
        double scaleX = 1.0;
        double scaleY = 1.0;
        bool linearInterpolation = true;
    };

    QString videoInterpolationLabel(bool linearInterpolation) const;
    QString nextVideoInterpolationLabel(const QString& text) const;
    bool parseVideoInterpolationText(const QString& text, bool* linearInterpolationOut) const;
    bool showClipRelativeKeyframes() const;
    int64_t keyframeFrameForInspectorDisplay(const TimelineClip& clip, int64_t storedFrame) const;
    int64_t keyframeFrameFromInspectorDisplay(const TimelineClip& clip, int64_t displayedFrame) const;
    qreal inspectorScaleWithMirror(qreal magnitude, bool mirrored) const;
    void syncScaleSpinPair(QDoubleSpinBox* changedSpin, QDoubleSpinBox* otherSpin);
    TransformKeyframeDisplay keyframeForInspectorDisplay(const TimelineClip& clip, const TimelineClip::TransformKeyframe& keyframe) const;
    TimelineClip::TransformKeyframe keyframeFromInspectorDisplay(const TimelineClip& clip, const TransformKeyframeDisplay& displayed) const;
    int selectedKeyframeIndex(const TimelineClip& clip) const;
    QList<int64_t> selectedKeyframeFramesForClip(const TimelineClip& clip) const;
    int nearestKeyframeIndex(const TimelineClip& clip) const;
    bool hasRemovableKeyframeSelection(const TimelineClip& clip) const;
    TransformKeyframeDisplay evaluateDisplayedTransform(const TimelineClip& clip, int64_t localFrame) const;
    void updateSpinBoxesFromKeyframe(const TransformKeyframeDisplay& keyframe);
    void updateMirrorCheckboxesFromScale(double scaleX, double scaleY);
    void populateTable(const TimelineClip& clip);

    Widgets m_widgets;
    Dependencies m_deps;
    bool m_updating = false;
    bool m_syncingTableSelection = false;
    bool m_syncingScaleControls = false;
    int64_t m_selectedKeyframeFrame = -1;
    QSet<int64_t> m_selectedKeyframeFrames;
    QTimer m_deferredSeekTimer;
    int64_t m_pendingSeekTimelineFrame = -1;
};
