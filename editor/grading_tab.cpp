#include "grading_tab.h"
#include "clip_serialization.h"
#include "editor_shared.h"

#include <QMenu>
#include <QHeaderView>
#include <QSignalBlocker>
#include <QMessageBox>
#include <QInputDialog>
#include <QApplication>
#include <QDir>
#include <QColor>
#include <cmath>

GradingTab::GradingTab(const Widgets& widgets, const Dependencies& deps, QObject* parent)
    : QObject(parent)
    , m_widgets(widgets)
    , m_deps(deps)
{
}

void GradingTab::wire()
{
    if (m_widgets.brightnessSpin) {
        connect(m_widgets.brightnessSpin, qOverload<double>(&QDoubleSpinBox::valueChanged),
                this, &GradingTab::onBrightnessChanged);
        connect(m_widgets.brightnessSpin, &QDoubleSpinBox::editingFinished,
                this, &GradingTab::onBrightnessEditingFinished);
    }
    if (m_widgets.contrastSpin) {
        connect(m_widgets.contrastSpin, qOverload<double>(&QDoubleSpinBox::valueChanged),
                this, &GradingTab::onContrastChanged);
        connect(m_widgets.contrastSpin, &QDoubleSpinBox::editingFinished,
                this, &GradingTab::onContrastEditingFinished);
    }
    if (m_widgets.saturationSpin) {
        connect(m_widgets.saturationSpin, qOverload<double>(&QDoubleSpinBox::valueChanged),
                this, &GradingTab::onSaturationChanged);
        connect(m_widgets.saturationSpin, &QDoubleSpinBox::editingFinished,
                this, &GradingTab::onSaturationEditingFinished);
    }
    if (m_widgets.opacitySpin) {
        connect(m_widgets.opacitySpin, qOverload<double>(&QDoubleSpinBox::valueChanged),
                this, &GradingTab::onOpacityChanged);
        connect(m_widgets.opacitySpin, &QDoubleSpinBox::editingFinished,
                this, &GradingTab::onOpacityEditingFinished);
    }
    if (m_widgets.gradingAutoScrollCheckBox) {
        connect(m_widgets.gradingAutoScrollCheckBox, &QCheckBox::toggled,
                this, &GradingTab::onAutoScrollToggled);
    }
    if (m_widgets.gradingFollowCurrentCheckBox) {
        connect(m_widgets.gradingFollowCurrentCheckBox, &QCheckBox::toggled,
                this, &GradingTab::onFollowCurrentToggled);
    }
    if (m_widgets.gradingKeyAtPlayheadButton) {
        connect(m_widgets.gradingKeyAtPlayheadButton, &QPushButton::clicked,
                this, &GradingTab::onKeyAtPlayheadClicked);
    }
    if (m_widgets.gradingFadeInButton) {
        connect(m_widgets.gradingFadeInButton, &QPushButton::clicked,
                this, &GradingTab::onFadeInClicked);
    }
    if (m_widgets.gradingFadeOutButton) {
        connect(m_widgets.gradingFadeOutButton, &QPushButton::clicked,
                this, &GradingTab::onFadeOutClicked);
    }
    if (m_widgets.gradingKeyframeTable) {
        connect(m_widgets.gradingKeyframeTable, &QTableWidget::itemSelectionChanged,
                this, &GradingTab::onTableSelectionChanged);
        connect(m_widgets.gradingKeyframeTable, &QTableWidget::itemChanged,
                this, &GradingTab::onTableItemChanged);
        connect(m_widgets.gradingKeyframeTable, &QTableWidget::itemClicked,
                this, &GradingTab::onTableItemClicked);
        connect(m_widgets.gradingKeyframeTable, &QTableWidget::itemDoubleClicked,
                this, [this](QTableWidgetItem*) {});
        m_widgets.gradingKeyframeTable->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(m_widgets.gradingKeyframeTable, &QWidget::customContextMenuRequested,
                this, &GradingTab::onTableCustomContextMenu);
    }
}

void GradingTab::refresh()
{
    if (!m_widgets.gradingPathLabel || !m_widgets.brightnessSpin || !m_widgets.contrastSpin ||
        !m_widgets.saturationSpin || !m_widgets.opacitySpin || !m_widgets.gradingKeyframeTable) {
        return;
    }

    const TimelineClip* clip = m_deps.getSelectedClip();
    m_updating = true;

    QSignalBlocker brightnessBlock(m_widgets.brightnessSpin);
    QSignalBlocker contrastBlock(m_widgets.contrastSpin);
    QSignalBlocker saturationBlock(m_widgets.saturationSpin);
    QSignalBlocker opacityBlock(m_widgets.opacitySpin);
    QSignalBlocker tableBlocker(m_widgets.gradingKeyframeTable);

    m_widgets.gradingKeyframeTable->clearContents();
    m_widgets.gradingKeyframeTable->setRowCount(0);

    if (!clip || !m_deps.clipHasVisuals(*clip)) {
        m_widgets.gradingPathLabel->setText(QStringLiteral("No visual clip selected"));
        m_widgets.gradingPathLabel->setToolTip(QString());
        m_widgets.brightnessSpin->setValue(0.0);
        m_widgets.contrastSpin->setValue(1.0);
        m_widgets.saturationSpin->setValue(1.0);
        m_widgets.opacitySpin->setValue(1.0);
        m_selectedKeyframeFrame = -1;
        m_selectedKeyframeFrames.clear();
        m_updating = false;
        return;
    }

    const QString nativePath = QDir::toNativeSeparators(m_deps.getClipFilePath(*clip));
    const QString sourceLabel = QStringLiteral("%1 | %2")
                                    .arg(clipMediaTypeLabel(clip->mediaType),
                                         mediaSourceKindLabel(clip->sourceKind));
    m_widgets.gradingPathLabel->setText(QStringLiteral("%1\n%2").arg(clip->label, sourceLabel));
    m_widgets.gradingPathLabel->setToolTip(nativePath);

    populateTable(*clip);

    if (m_selectedKeyframeFrame < 0) {
        const int selectedIndex = clip->gradingKeyframes.isEmpty() ? 0 : nearestKeyframeIndex(*clip);
        m_selectedKeyframeFrame = selectedIndex >= 0 && selectedIndex < clip->gradingKeyframes.size()
                                     ? clip->gradingKeyframes[selectedIndex].frame
                                     : 0;
        m_selectedKeyframeFrames = {m_selectedKeyframeFrame};
    } else if (m_selectedKeyframeFrames.isEmpty()) {
        m_selectedKeyframeFrames = {m_selectedKeyframeFrame};
    }

    GradingKeyframeDisplay displayed = evaluateDisplayedGrading(*clip, clip->startFrame);
    const int selectedIndex = selectedKeyframeIndex(*clip);
    if (selectedIndex >= 0) {
        const auto& keyframe = clip->gradingKeyframes[selectedIndex];
        displayed.frame = keyframe.frame;
        displayed.brightness = keyframe.brightness;
        displayed.contrast = keyframe.contrast;
        displayed.saturation = keyframe.saturation;
        displayed.opacity = keyframe.opacity;
        displayed.linearInterpolation = keyframe.linearInterpolation;
    }
    updateSpinBoxesFromKeyframe(displayed);

    for (int row = 0; row < m_widgets.gradingKeyframeTable->rowCount(); ++row) {
        QTableWidgetItem* item = m_widgets.gradingKeyframeTable->item(row, 0);
        if (!item) continue;
        const int64_t frame = item->data(Qt::UserRole).toLongLong();
        if (m_selectedKeyframeFrames.contains(frame)) {
            m_widgets.gradingKeyframeTable->selectRow(row);
        }
    }

    m_updating = false;
}

void GradingTab::applyGradeFromInspector(bool pushHistory)
{
    if (m_updating) return;

    const TimelineClip* selectedClip = m_deps.getSelectedClip();
    if (!selectedClip || !m_deps.clipHasVisuals(*selectedClip)) return;

    int64_t targetFrame = m_selectedKeyframeFrame;
    if (targetFrame < 0) {
        targetFrame = qBound<int64_t>(0,
                                      m_deps.getCurrentTimelineFrame() - selectedClip->startFrame,
                                      qMax<int64_t>(0, selectedClip->durationFrames - 1));
    }

    const bool updated = m_deps.updateClipById(selectedClip->id, [this, targetFrame](TimelineClip& clip) {
        TimelineClip::GradingKeyframe keyframe;
        keyframe.frame = targetFrame;
        keyframe.brightness = m_widgets.brightnessSpin->value();
        keyframe.contrast = m_widgets.contrastSpin->value();
        keyframe.saturation = m_widgets.saturationSpin->value();
        keyframe.opacity = m_widgets.opacitySpin->value();
        keyframe.linearInterpolation = true;

        bool replaced = false;
        for (TimelineClip::GradingKeyframe& existing : clip.gradingKeyframes) {
            if (existing.frame == targetFrame) {
                keyframe.linearInterpolation = existing.linearInterpolation;
                existing = keyframe;
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            clip.gradingKeyframes.push_back(keyframe);
        }
        normalizeClipGradingKeyframes(clip);
    });

    if (!updated) return;

    m_selectedKeyframeFrame = targetFrame;
    m_selectedKeyframeFrames = {targetFrame};
    m_deps.setPreviewTimelineClips();
    m_deps.refreshInspector();
    m_deps.scheduleSaveState();
    if (pushHistory) {
        m_deps.pushHistorySnapshot();
    }
    emit gradeApplied();
}

void GradingTab::upsertKeyframeAtPlayhead()
{
    const TimelineClip* clip = m_deps.getSelectedClip();
    if (!clip || !m_deps.clipHasVisuals(*clip)) return;

    m_selectedKeyframeFrame = qBound<int64_t>(0,
                                              m_deps.getCurrentTimelineFrame() - clip->startFrame,
                                              qMax<int64_t>(0, clip->durationFrames - 1));
    m_selectedKeyframeFrames = {m_selectedKeyframeFrame};
    applyGradeFromInspector(true);
    emit keyframeAdded();
}

void GradingTab::fadeInFromPlayhead()
{
    applyOpacityFadeFromPlayhead(true);
}

void GradingTab::fadeOutFromPlayhead()
{
    applyOpacityFadeFromPlayhead(false);
}

void GradingTab::syncTableToPlayhead()
{
    if (!m_widgets.gradingKeyframeTable || m_updating) return;

    if (!m_widgets.gradingFollowCurrentCheckBox || !m_widgets.gradingFollowCurrentCheckBox->isChecked()) {
        return;
    }

    const TimelineClip* clip = m_deps.getSelectedClip();
    if (!clip || !m_deps.clipHasVisuals(*clip) || m_widgets.gradingKeyframeTable->rowCount() <= 0) {
        m_widgets.gradingKeyframeTable->clearSelection();
        return;
    }

    QWidget* focus = QApplication::focusWidget();
    const bool editingGrading = m_widgets.gradingKeyframeTable &&
                                focus &&
                                m_widgets.gradingKeyframeTable->isAncestorOf(focus) &&
                                (qobject_cast<QLineEdit*>(focus) ||
                                 qobject_cast<QAbstractSpinBox*>(focus));
    if (editingGrading) {
        return;
    }

    const int64_t localFrame = qBound<int64_t>(0,
                                               m_deps.getCurrentTimelineFrame() - clip->startFrame,
                                               qMax<int64_t>(0, clip->durationFrames - 1));

    int matchingRow = -1;
    int64_t matchingFrame = -1;
    for (int row = 0; row < m_widgets.gradingKeyframeTable->rowCount(); ++row) {
        QTableWidgetItem* item = m_widgets.gradingKeyframeTable->item(row, 0);
        if (!item) continue;
        const int64_t frame = item->data(Qt::UserRole).toLongLong();
        if (frame <= localFrame && frame >= matchingFrame) {
            matchingFrame = frame;
            matchingRow = row;
        }
    }
    if (matchingRow < 0) {
        matchingRow = 0;
    }

    if (!m_widgets.gradingKeyframeTable->selectionModel()->isRowSelected(matchingRow, QModelIndex())) {
        m_syncingTableSelection = true;
        m_widgets.gradingKeyframeTable->setCurrentCell(matchingRow, 0);
        m_widgets.gradingKeyframeTable->selectRow(matchingRow);
        m_syncingTableSelection = false;
    }

    if (m_widgets.gradingAutoScrollCheckBox && m_widgets.gradingAutoScrollCheckBox->isChecked()) {
        if (QTableWidgetItem* item = m_widgets.gradingKeyframeTable->item(matchingRow, 0)) {
            m_widgets.gradingKeyframeTable->scrollToItem(item, QAbstractItemView::PositionAtCenter);
        }
    }
}

void GradingTab::setSelectedKeyframeFrame(int64_t frame)
{
    m_selectedKeyframeFrame = frame;
    m_selectedKeyframeFrames = {frame};
}

void GradingTab::onBrightnessChanged(double value)
{
    Q_UNUSED(value);
    if (m_updating) return;
    applyGradeFromInspector(false);
}

void GradingTab::onContrastChanged(double value)
{
    Q_UNUSED(value);
    if (m_updating) return;
    applyGradeFromInspector(false);
}

void GradingTab::onSaturationChanged(double value)
{
    Q_UNUSED(value);
    if (m_updating) return;
    applyGradeFromInspector(false);
}

void GradingTab::onOpacityChanged(double value)
{
    Q_UNUSED(value);
    if (m_updating) return;
    applyGradeFromInspector(false);
}

void GradingTab::onAutoScrollToggled(bool checked)
{
    Q_UNUSED(checked);
    if (m_updating) return;
    m_deps.scheduleSaveState();
}

void GradingTab::onFollowCurrentToggled(bool checked)
{
    Q_UNUSED(checked);
    if (m_updating) return;
    if (m_widgets.gradingFollowCurrentCheckBox && m_widgets.gradingFollowCurrentCheckBox->isChecked()) {
        syncTableToPlayhead();
    }
    m_deps.scheduleSaveState();
}

void GradingTab::onKeyAtPlayheadClicked()
{
    upsertKeyframeAtPlayhead();
}

void GradingTab::onFadeInClicked()
{
    fadeInFromPlayhead();
}

void GradingTab::onFadeOutClicked()
{
    fadeOutFromPlayhead();
}

void GradingTab::onTableSelectionChanged()
{
    if (m_updating || m_syncingTableSelection) return;

    QList<QTableWidgetItem*> selectedItems = m_widgets.gradingKeyframeTable->selectedItems();
    if (selectedItems.isEmpty()) return;

    QSet<int64_t> selectedFrames;
    int64_t primaryFrame = -1;
    for (QTableWidgetItem* item : selectedItems) {
        const QVariant frameData = item->data(Qt::UserRole);
        if (!frameData.isValid()) continue;
        const int64_t frame = frameData.toLongLong();
        selectedFrames.insert(frame);
        if (primaryFrame < 0 || frame < primaryFrame) {
            primaryFrame = frame;
        }
    }

    if (primaryFrame < 0) return;

    m_selectedKeyframeFrame = primaryFrame;
    m_selectedKeyframeFrames = selectedFrames;
    refresh();

    if (m_deps.onKeyframeSelectionChanged) {
        m_deps.onKeyframeSelectionChanged();
    }
}

void GradingTab::onTableItemChanged(QTableWidgetItem* changedItem)
{
    if (m_updating || !changedItem || !m_deps.onKeyframeItemChanged) {
        if (m_deps.onKeyframeItemChanged) {
            m_deps.onKeyframeItemChanged(changedItem);
        }
        return;
    }

    const TimelineClip* selectedClip = m_deps.getSelectedClip();
    if (!selectedClip || !m_deps.clipHasVisuals(*selectedClip)) return;

    const int row = changedItem->row();
    if (row < 0 || row >= m_widgets.gradingKeyframeTable->rowCount()) return;

    auto tableText = [this, row](int column) -> QString {
        QTableWidgetItem* item = m_widgets.gradingKeyframeTable->item(row, column);
        return item ? item->text().trimmed() : QString();
    };

    bool ok = false;
    TimelineClip::GradingKeyframe edited;
    edited.frame = tableText(0).toLongLong(&ok);
    if (!ok) { refresh(); return; }
    edited.brightness = tableText(1).toDouble(&ok);
    if (!ok) { refresh(); return; }
    edited.contrast = tableText(2).toDouble(&ok);
    if (!ok) { refresh(); return; }
    edited.saturation = tableText(3).toDouble(&ok);
    if (!ok) { refresh(); return; }
    edited.opacity = tableText(4).toDouble(&ok);
    if (!ok) { refresh(); return; }
    if (!parseVideoInterpolationText(tableText(5), &edited.linearInterpolation)) {
        refresh();
        return;
    }

    edited.frame = qBound<int64_t>(0, edited.frame, qMax<int64_t>(0, selectedClip->durationFrames - 1));
    const int64_t originalFrame = changedItem->data(Qt::UserRole).toLongLong();

    const bool updated = m_deps.updateClipById(selectedClip->id, [edited, originalFrame](TimelineClip& clip) {
        bool replaced = false;
        for (TimelineClip::GradingKeyframe& existing : clip.gradingKeyframes) {
            if (existing.frame == originalFrame) {
                existing = edited;
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            clip.gradingKeyframes.push_back(edited);
        }
        normalizeClipGradingKeyframes(clip);
    });

    if (!updated) {
        refresh();
        return;
    }

    m_selectedKeyframeFrame = edited.frame;
    m_selectedKeyframeFrames = {edited.frame};
    m_deps.setPreviewTimelineClips();
    refresh();
    m_deps.scheduleSaveState();
    m_deps.pushHistorySnapshot();
}

void GradingTab::onTableItemClicked(QTableWidgetItem* item)
{
    if (m_updating || !item) return;
    if (item->column() != 5) return;
    item->setText(nextVideoInterpolationLabel(item->text()));
}

QString GradingTab::videoInterpolationLabel(bool linearInterpolation) const
{
    return linearInterpolation ? QStringLiteral("Linear") : QStringLiteral("Step");
}

QString GradingTab::nextVideoInterpolationLabel(const QString& text) const
{
    bool linearInterpolation = true;
    if (!parseVideoInterpolationText(text, &linearInterpolation)) {
        linearInterpolation = true;
    }
    return videoInterpolationLabel(!linearInterpolation);
}

bool GradingTab::parseVideoInterpolationText(const QString& text, bool* linearInterpolationOut) const
{
    const QString normalized = text.trimmed().toLower();
    if (normalized.isEmpty() || normalized == QStringLiteral("step")) {
        *linearInterpolationOut = false;
        return true;
    }
    if (normalized == QStringLiteral("linear") || normalized == QStringLiteral("smooth")) {
        *linearInterpolationOut = true;
        return true;
    }
    return false;
}

int GradingTab::selectedKeyframeIndex(const TimelineClip& clip) const
{
    for (int i = 0; i < clip.gradingKeyframes.size(); ++i) {
        if (clip.gradingKeyframes[i].frame == m_selectedKeyframeFrame) {
            return i;
        }
    }
    return -1;
}

QList<int64_t> GradingTab::selectedKeyframeFramesForClip(const TimelineClip& clip) const
{
    QList<int64_t> frames;
    for (const TimelineClip::GradingKeyframe& keyframe : clip.gradingKeyframes) {
        if (m_selectedKeyframeFrames.contains(keyframe.frame)) {
            frames.push_back(keyframe.frame);
        }
    }
    return frames;
}

int GradingTab::nearestKeyframeIndex(const TimelineClip& clip) const
{
    if (!m_deps.getSelectedClip() || clip.gradingKeyframes.isEmpty()) {
        return -1;
    }
    const int64_t localFrame = qBound<int64_t>(0,
                                               m_deps.getCurrentTimelineFrame() - clip.startFrame,
                                               qMax<int64_t>(0, clip.durationFrames - 1));
    int nearestIndex = 0;
    int64_t nearestDistance = std::numeric_limits<int64_t>::max();
    for (int i = 0; i < clip.gradingKeyframes.size(); ++i) {
        const int64_t distance = std::abs(clip.gradingKeyframes[i].frame - localFrame);
        if (distance < nearestDistance) {
            nearestDistance = distance;
            nearestIndex = i;
        }
    }
    return nearestIndex;
}

bool GradingTab::hasRemovableKeyframeSelection(const TimelineClip& clip) const
{
    for (const int64_t frame : selectedKeyframeFramesForClip(clip)) {
        if (frame > 0) {
            return true;
        }
    }
    return false;
}

GradingTab::GradingKeyframeDisplay GradingTab::evaluateDisplayedGrading(const TimelineClip& clip, int64_t localFrame) const
{
    GradingKeyframeDisplay result;
    result.brightness = 0.0;
    result.contrast = 1.0;
    result.saturation = 1.0;
    result.opacity = 1.0;
    result.linearInterpolation = true;

    if (clip.gradingKeyframes.isEmpty()) {
        return result;
    }

    // Find the keyframe at or before localFrame
    int beforeIndex = -1;
    for (int i = clip.gradingKeyframes.size() - 1; i >= 0; --i) {
        if (clip.gradingKeyframes[i].frame <= localFrame) {
            beforeIndex = i;
            break;
        }
    }

    if (beforeIndex < 0) {
        // Use first keyframe
        const auto& kf = clip.gradingKeyframes[0];
        result.brightness = kf.brightness;
        result.contrast = kf.contrast;
        result.saturation = kf.saturation;
        result.opacity = kf.opacity;
        result.linearInterpolation = kf.linearInterpolation;
        return result;
    }

    const auto& before = clip.gradingKeyframes[beforeIndex];
    
    // If this is the last keyframe or we're exactly at it
    if (beforeIndex == clip.gradingKeyframes.size() - 1 || before.frame == localFrame) {
        result.brightness = before.brightness;
        result.contrast = before.contrast;
        result.saturation = before.saturation;
        result.opacity = before.opacity;
        result.linearInterpolation = before.linearInterpolation;
        return result;
    }

    // Find the next keyframe
    int afterIndex = beforeIndex + 1;
    const auto& after = clip.gradingKeyframes[afterIndex];

    // If not interpolating, just use the before keyframe
    if (!before.linearInterpolation) {
        result.brightness = before.brightness;
        result.contrast = before.contrast;
        result.saturation = before.saturation;
        result.opacity = before.opacity;
        result.linearInterpolation = before.linearInterpolation;
        return result;
    }

    // Interpolate
    const int64_t range = after.frame - before.frame;
    if (range <= 0) {
        result.brightness = before.brightness;
        result.contrast = before.contrast;
        result.saturation = before.saturation;
        result.opacity = before.opacity;
        return result;
    }

    const double t = static_cast<double>(localFrame - before.frame) / static_cast<double>(range);
    result.brightness = before.brightness + (after.brightness - before.brightness) * t;
    result.contrast = before.contrast + (after.contrast - before.contrast) * t;
    result.saturation = before.saturation + (after.saturation - before.saturation) * t;
    result.opacity = before.opacity + (after.opacity - before.opacity) * t;
    result.linearInterpolation = after.linearInterpolation;

    return result;
}

void GradingTab::updateSpinBoxesFromKeyframe(const GradingKeyframeDisplay& keyframe)
{
    QSignalBlocker brightnessBlock(m_widgets.brightnessSpin);
    QSignalBlocker contrastBlock(m_widgets.contrastSpin);
    QSignalBlocker saturationBlock(m_widgets.saturationSpin);
    QSignalBlocker opacityBlock(m_widgets.opacitySpin);

    m_widgets.brightnessSpin->setValue(keyframe.brightness);
    m_widgets.contrastSpin->setValue(keyframe.contrast);
    m_widgets.saturationSpin->setValue(keyframe.saturation);
    m_widgets.opacitySpin->setValue(keyframe.opacity);
}

void GradingTab::populateTable(const TimelineClip& clip)
{
    QList<int64_t> frames;
    if (clip.gradingKeyframes.isEmpty()) {
        frames.push_back(0);
    } else {
        frames.reserve(clip.gradingKeyframes.size());
        for (const TimelineClip::GradingKeyframe& keyframe : clip.gradingKeyframes) {
            frames.push_back(keyframe.frame);
        }
        std::sort(frames.begin(), frames.end());
    }

    m_widgets.gradingKeyframeTable->setRowCount(frames.size());
    
    for (int row = 0; row < frames.size(); ++row) {
        const int64_t frame = frames[row];
        TimelineClip::GradingKeyframe displayedFrame;
        
        if (clip.gradingKeyframes.isEmpty()) {
            displayedFrame = evaluateClipGradingAtFrame(clip, clip.startFrame);
            displayedFrame.frame = 0;
        } else {
            displayedFrame = clip.gradingKeyframes[row];
        }

        const QStringList rowValues = {
            QString::number(frame),
            QString::number(displayedFrame.brightness, 'f', 3),
            QString::number(displayedFrame.contrast, 'f', 3),
            QString::number(displayedFrame.saturation, 'f', 3),
            QString::number(displayedFrame.opacity, 'f', 3),
            videoInterpolationLabel(displayedFrame.linearInterpolation)
        };

        for (int column = 0; column < rowValues.size(); ++column) {
            auto* item = new QTableWidgetItem(rowValues[column]);
            item->setData(Qt::UserRole, QVariant::fromValue(static_cast<qint64>(frame)));
            m_widgets.gradingKeyframeTable->setItem(row, column, item);
        }
    }
}

void GradingTab::applyOpacityFadeFromPlayhead(bool fadeIn)
{
    const TimelineClip* clip = m_deps.getSelectedClip();
    if (!clip || !m_deps.clipHasVisuals(*clip)) {
        return;
    }

    const int64_t localStartFrame = qBound<int64_t>(0,
                                                    m_deps.getCurrentTimelineFrame() - clip->startFrame,
                                                    qMax<int64_t>(0, clip->durationFrames - 1));
    const int64_t localEndFrame = qMax<int64_t>(0, clip->durationFrames - 1);
    if (localStartFrame >= localEndFrame) {
        QMessageBox::information(nullptr,
                                 QStringLiteral("Opacity Fade"),
                                 QStringLiteral("Move the playhead before the end of the clip to create a fade."));
        return;
    }

    const GradingKeyframeDisplay startDisplay = evaluateDisplayedGrading(*clip, clip->startFrame + localStartFrame);
    const GradingKeyframeDisplay endDisplay = evaluateDisplayedGrading(*clip, clip->startFrame + localEndFrame);
    const double targetVisibleOpacity = qBound(0.0, qMax(startDisplay.opacity, endDisplay.opacity), 1.0);

    const bool updated = m_deps.updateClipById(clip->id, [&](TimelineClip& updatedClip) {
        auto upsertFrame = [](QVector<TimelineClip::GradingKeyframe>& keyframes,
                              const TimelineClip::GradingKeyframe& keyframe) {
            for (TimelineClip::GradingKeyframe& existing : keyframes) {
                if (existing.frame == keyframe.frame) {
                    existing = keyframe;
                    return;
                }
            }
            keyframes.push_back(keyframe);
        };

        TimelineClip::GradingKeyframe startKeyframe;
        startKeyframe.frame = localStartFrame;
        startKeyframe.brightness = startDisplay.brightness;
        startKeyframe.contrast = startDisplay.contrast;
        startKeyframe.saturation = startDisplay.saturation;
        startKeyframe.opacity = fadeIn ? 0.0 : qBound(0.0, startDisplay.opacity, 1.0);
        startKeyframe.linearInterpolation = true;

        TimelineClip::GradingKeyframe endKeyframe;
        endKeyframe.frame = localEndFrame;
        endKeyframe.brightness = endDisplay.brightness;
        endKeyframe.contrast = endDisplay.contrast;
        endKeyframe.saturation = endDisplay.saturation;
        endKeyframe.opacity = fadeIn ? targetVisibleOpacity : 0.0;
        endKeyframe.linearInterpolation = true;

        upsertFrame(updatedClip.gradingKeyframes, startKeyframe);
        upsertFrame(updatedClip.gradingKeyframes, endKeyframe);
        normalizeClipGradingKeyframes(updatedClip);
    });

    if (!updated) {
        return;
    }

    m_selectedKeyframeFrame = localStartFrame;
    m_selectedKeyframeFrames = {localStartFrame, localEndFrame};
    m_deps.setPreviewTimelineClips();
    m_deps.refreshInspector();
    m_deps.scheduleSaveState();
    m_deps.pushHistorySnapshot();
}

void GradingTab::removeSelectedKeyframes()
{
    if (m_selectedKeyframeFrames.isEmpty()) return;

    const QString clipId = m_deps.getSelectedClipId();
    if (clipId.isEmpty()) return;

    const TimelineClip* selectedClip = m_deps.getSelectedClip();
    if (!selectedClip) return;

    QList<int64_t> selectedFrames = selectedKeyframeFramesForClip(*selectedClip);
    selectedFrames.erase(std::remove_if(selectedFrames.begin(),
                                        selectedFrames.end(),
                                        [](int64_t frame) { return frame <= 0; }),
                         selectedFrames.end());

    if (selectedFrames.isEmpty()) {
        refresh();
        return;
    }

    const bool updated = m_deps.updateClipById(clipId, [selectedFrames](TimelineClip& clip) {
        clip.gradingKeyframes.erase(
            std::remove_if(clip.gradingKeyframes.begin(),
                           clip.gradingKeyframes.end(),
                           [&selectedFrames](const TimelineClip::GradingKeyframe& keyframe) {
                               return selectedFrames.contains(keyframe.frame);
                           }),
            clip.gradingKeyframes.end());
        normalizeClipGradingKeyframes(clip);
    });

    if (!updated) return;

    m_selectedKeyframeFrame = 0;
    m_selectedKeyframeFrames = {0};
    m_deps.setPreviewTimelineClips();
    refresh();
    m_deps.scheduleSaveState();
    m_deps.pushHistorySnapshot();
    emit keyframesRemoved();
}

void GradingTab::onBrightnessEditingFinished()
{
    if (m_updating) return;
    applyGradeFromInspector(true);
}

void GradingTab::onContrastEditingFinished()
{
    if (m_updating) return;
    applyGradeFromInspector(true);
}

void GradingTab::onSaturationEditingFinished()
{
    if (m_updating) return;
    applyGradeFromInspector(true);
}

void GradingTab::onOpacityEditingFinished()
{
    if (m_updating) return;
    applyGradeFromInspector(true);
}

void GradingTab::onTableCustomContextMenu(const QPoint& pos)
{
    if (!m_widgets.gradingKeyframeTable) return;

    QTableWidgetItem* item = m_widgets.gradingKeyframeTable->itemAt(pos);
    if (!item) return;

    const int row = item->row();
    if (!m_widgets.gradingKeyframeTable->selectionModel()->isRowSelected(row, QModelIndex())) {
        m_widgets.gradingKeyframeTable->clearSelection();
        m_widgets.gradingKeyframeTable->selectRow(row);
    }

    const int64_t anchorFrame = item->data(Qt::UserRole).toLongLong();
    int64_t previousFrame = -1;
    int64_t nextFrame = -1;

    if (row > 0) {
        if (QTableWidgetItem* previousItem = m_widgets.gradingKeyframeTable->item(row - 1, 0)) {
            previousFrame = previousItem->data(Qt::UserRole).toLongLong();
        }
    }
    if (row + 1 < m_widgets.gradingKeyframeTable->rowCount()) {
        if (QTableWidgetItem* nextItem = m_widgets.gradingKeyframeTable->item(row + 1, 0)) {
            nextFrame = nextItem->data(Qt::UserRole).toLongLong();
        }
    }

    int deletableRowCount = 0;
    const QList<QTableWidgetSelectionRange> ranges = m_widgets.gradingKeyframeTable->selectedRanges();
    for (const QTableWidgetSelectionRange& range : ranges) {
        for (int selectedRow = range.topRow(); selectedRow <= range.bottomRow(); ++selectedRow) {
            QTableWidgetItem* selectedItem = m_widgets.gradingKeyframeTable->item(selectedRow, 0);
            if (selectedItem && selectedItem->data(Qt::UserRole).toLongLong() > 0) {
                ++deletableRowCount;
            }
        }
    }

    QMenu menu;
    QAction* addAboveAction = menu.addAction(QStringLiteral("Add Keyframe Above"));
    addAboveAction->setEnabled(previousFrame >= 0);
    QAction* addBelowAction = menu.addAction(QStringLiteral("Add Keyframe Below"));
    addBelowAction->setEnabled(nextFrame >= 0);
    menu.addSeparator();
    QAction* deleteAction = menu.addAction(deletableRowCount == 1
                                               ? QStringLiteral("Delete Row")
                                               : QStringLiteral("Delete Rows"));
    deleteAction->setEnabled(deletableRowCount > 0);

    QAction* chosen = menu.exec(m_widgets.gradingKeyframeTable->viewport()->mapToGlobal(pos));
    if (chosen == addAboveAction && addAboveAction->isEnabled()) {
        const int64_t midpointFrame = previousFrame + ((anchorFrame - previousFrame) / 2);
        if (midpointFrame > previousFrame && midpointFrame < anchorFrame) {
            const auto findKeyframeAt = [](const TimelineClip& clip, int64_t frame) -> const TimelineClip::GradingKeyframe* {
                for (const TimelineClip::GradingKeyframe& keyframe : clip.gradingKeyframes) {
                    if (keyframe.frame == frame) return &keyframe;
                }
                return nullptr;
            };
            const TimelineClip* selectedClip = m_deps.getSelectedClip();
            if (selectedClip) {
                const TimelineClip::GradingKeyframe* earlier = findKeyframeAt(*selectedClip, previousFrame);
                const TimelineClip::GradingKeyframe* later = findKeyframeAt(*selectedClip, anchorFrame);
                if (earlier && later) {
                    const double t = static_cast<double>(midpointFrame - previousFrame) / static_cast<double>(anchorFrame - previousFrame);
                    TimelineClip::GradingKeyframe midpoint;
                    midpoint.frame = midpointFrame;
                    midpoint.brightness = earlier->brightness + ((later->brightness - earlier->brightness) * t);
                    midpoint.contrast = earlier->contrast + ((later->contrast - earlier->contrast) * t);
                    midpoint.saturation = earlier->saturation + ((later->saturation - earlier->saturation) * t);
                    midpoint.opacity = earlier->opacity + ((later->opacity - earlier->opacity) * t);
                    midpoint.linearInterpolation = later->linearInterpolation;
                    const bool updated = m_deps.updateClipById(selectedClip->id, [midpoint](TimelineClip& clip) {
                        for (TimelineClip::GradingKeyframe& existing : clip.gradingKeyframes) {
                            if (existing.frame == midpoint.frame) {
                                existing = midpoint;
                                normalizeClipGradingKeyframes(clip);
                                return;
                            }
                        }
                        clip.gradingKeyframes.push_back(midpoint);
                        normalizeClipGradingKeyframes(clip);
                    });
                    if (updated) {
                        m_selectedKeyframeFrame = midpoint.frame;
                        m_selectedKeyframeFrames = {midpoint.frame};
                        m_deps.setPreviewTimelineClips();
                        refresh();
                        m_deps.scheduleSaveState();
                        m_deps.pushHistorySnapshot();
                        m_deps.seekToTimelineFrame(selectedClip->startFrame + midpoint.frame);
                    }
                }
            }
        }
    } else if (chosen == addBelowAction && addBelowAction->isEnabled()) {
        const int64_t midpointFrame = anchorFrame + ((nextFrame - anchorFrame) / 2);
        if (midpointFrame > anchorFrame && midpointFrame < nextFrame) {
            const auto findKeyframeAt = [](const TimelineClip& clip, int64_t frame) -> const TimelineClip::GradingKeyframe* {
                for (const TimelineClip::GradingKeyframe& keyframe : clip.gradingKeyframes) {
                    if (keyframe.frame == frame) return &keyframe;
                }
                return nullptr;
            };
            const TimelineClip* selectedClip = m_deps.getSelectedClip();
            if (selectedClip) {
                const TimelineClip::GradingKeyframe* earlier = findKeyframeAt(*selectedClip, anchorFrame);
                const TimelineClip::GradingKeyframe* later = findKeyframeAt(*selectedClip, nextFrame);
                if (earlier && later) {
                    const double t = static_cast<double>(midpointFrame - anchorFrame) / static_cast<double>(nextFrame - anchorFrame);
                    TimelineClip::GradingKeyframe midpoint;
                    midpoint.frame = midpointFrame;
                    midpoint.brightness = earlier->brightness + ((later->brightness - earlier->brightness) * t);
                    midpoint.contrast = earlier->contrast + ((later->contrast - earlier->contrast) * t);
                    midpoint.saturation = earlier->saturation + ((later->saturation - earlier->saturation) * t);
                    midpoint.opacity = earlier->opacity + ((later->opacity - earlier->opacity) * t);
                    midpoint.linearInterpolation = later->linearInterpolation;
                    const bool updated = m_deps.updateClipById(selectedClip->id, [midpoint](TimelineClip& clip) {
                        for (TimelineClip::GradingKeyframe& existing : clip.gradingKeyframes) {
                            if (existing.frame == midpoint.frame) {
                                existing = midpoint;
                                normalizeClipGradingKeyframes(clip);
                                return;
                            }
                        }
                        clip.gradingKeyframes.push_back(midpoint);
                        normalizeClipGradingKeyframes(clip);
                    });
                    if (updated) {
                        m_selectedKeyframeFrame = midpoint.frame;
                        m_selectedKeyframeFrames = {midpoint.frame};
                        m_deps.setPreviewTimelineClips();
                        refresh();
                        m_deps.scheduleSaveState();
                        m_deps.pushHistorySnapshot();
                        m_deps.seekToTimelineFrame(selectedClip->startFrame + midpoint.frame);
                    }
                }
            }
        }
    } else if (chosen == deleteAction && deleteAction->isEnabled()) {
        removeSelectedKeyframes();
    }
}
