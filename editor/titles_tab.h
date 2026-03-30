#pragma once

#include "editor_shared.h"

#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QFontComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QObject>
#include <QPushButton>
#include <QSet>
#include <QTableWidget>

#include <functional>

class TitlesTab final : public QObject
{
    Q_OBJECT

public:
    struct Widgets {
        QLabel *titlesInspectorClipLabel = nullptr;
        QLabel *titlesInspectorDetailsLabel = nullptr;
        QTableWidget *titleKeyframeTable = nullptr;
        QLineEdit *titleTextEdit = nullptr;
        QDoubleSpinBox *titleXSpin = nullptr;
        QDoubleSpinBox *titleYSpin = nullptr;
        QDoubleSpinBox *titleFontSizeSpin = nullptr;
        QDoubleSpinBox *titleOpacitySpin = nullptr;
        QFontComboBox *titleFontCombo = nullptr;
        QCheckBox *titleBoldCheck = nullptr;
        QCheckBox *titleItalicCheck = nullptr;
        QCheckBox *titleAutoScrollCheck = nullptr;
        QPushButton *addTitleKeyframeButton = nullptr;
        QPushButton *removeTitleKeyframeButton = nullptr;
        QPushButton *centerHorizontalButton = nullptr;
        QPushButton *centerVerticalButton = nullptr;
    };

    struct Dependencies {
        std::function<const TimelineClip *()> getSelectedClip;
        std::function<const TimelineClip *()> getSelectedClipConst;
        std::function<bool(const QString &, const std::function<void(TimelineClip &)> &)> updateClipById;
        std::function<QString(const TimelineClip &)> getClipFilePath;
        std::function<bool(const TimelineClip &)> clipHasVisuals;
        std::function<void()> scheduleSaveState;
        std::function<void()> pushHistorySnapshot;
        std::function<void()> refreshInspector;
        std::function<void()> setPreviewTimelineClips;
        std::function<int64_t()> getCurrentTimelineFrame;
        std::function<int64_t()> getSelectedClipStartFrame;
        std::function<QString()> getSelectedClipId;
        std::function<void(int64_t)> seekToTimelineFrame;
        std::function<void(QTableWidgetItem *)> onKeyframeItemChanged;
        std::function<void()> onKeyframeSelectionChanged;
    };

    TitlesTab(const Widgets &widgets, const Dependencies &deps, QObject *parent = nullptr);

    void wire();
    void refresh();
    void syncTableToPlayhead();
    void upsertKeyframeAtPlayhead();
    void removeSelectedKeyframes();
    void centerHorizontal();
    void centerVertical();

    int64_t selectedKeyframeFrame() const { return m_selectedKeyframeFrame; }
    QSet<int64_t> selectedKeyframeFrames() const { return m_selectedKeyframeFrames; }
    void setSelectedKeyframeFrame(int64_t frame) { m_selectedKeyframeFrame = frame; }

private:
    struct TitleKeyframeDisplay {
        int64_t frame = 0;
        QString text;
        double translationX = 0.0;
        double translationY = 0.0;
        double fontSize = 48.0;
        double opacity = 1.0;
        QString fontFamily = kDefaultFontFamily;
        bool bold = true;
        bool italic = false;
        bool linearInterpolation = true;
    };

    void populateTable(const TimelineClip &clip);
    TitleKeyframeDisplay evaluateDisplayedTitle(const TimelineClip &clip, int64_t localFrame) const;
    void updateWidgetsFromKeyframe(const TitleKeyframeDisplay &display);
    void applyKeyframeFromInspector();
    int selectedKeyframeIndex(const TimelineClip &clip) const;
    int nearestKeyframeIndex(const TimelineClip &clip, int64_t localFrame) const;
    bool hasRemovableKeyframeSelection() const;

    void onTableItemChanged(QTableWidgetItem *item);
    void onTableSelectionChanged();
    void onTableItemClicked(QTableWidgetItem *item);

    Widgets m_widgets;
    Dependencies m_deps;
    bool m_updating = false;
    bool m_syncingTableSelection = false;
    int64_t m_selectedKeyframeFrame = -1;
    QSet<int64_t> m_selectedKeyframeFrames;
};
