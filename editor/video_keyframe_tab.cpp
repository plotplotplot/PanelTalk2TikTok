#include "video_keyframe_tab.h"
#include "clip_serialization.h"
#include "editor_shared.h"
#include "keyframe_table_shared.h"

#include <QMenu>
#include <QHeaderView>
#include <QSignalBlocker>
#include <QMessageBox>
#include <QInputDialog>
#include <QApplication>
#include <QDir>
#include <QColor>
#include <cmath>

VideoKeyframeTab::VideoKeyframeTab(const Widgets& widgets, const Dependencies& deps, QObject* parent)
    : QObject(parent)
    , m_widgets(widgets)
    , m_deps(deps)
{
    m_deferredSeekTimer.setSingleShot(true);
    connect(&m_deferredSeekTimer, &QTimer::timeout, this, [this]() {
        if (m_pendingSeekTimelineFrame < 0 || !m_deps.seekToTimelineFrame) {
            return;
        }
        m_deps.seekToTimelineFrame(m_pendingSeekTimelineFrame);
        m_pendingSeekTimelineFrame = -1;
    });
}

void VideoKeyframeTab::wire()
{
    if (m_widgets.videoTranslationXSpin) {
        connect(m_widgets.videoTranslationXSpin, qOverload<double>(&QDoubleSpinBox::valueChanged),
                this, &VideoKeyframeTab::onTranslationXChanged);
        connect(m_widgets.videoTranslationXSpin, &QDoubleSpinBox::editingFinished,
                this, &VideoKeyframeTab::onTranslationXEditingFinished);
    }
    if (m_widgets.videoTranslationYSpin) {
        connect(m_widgets.videoTranslationYSpin, qOverload<double>(&QDoubleSpinBox::valueChanged),
                this, &VideoKeyframeTab::onTranslationYChanged);
        connect(m_widgets.videoTranslationYSpin, &QDoubleSpinBox::editingFinished,
                this, &VideoKeyframeTab::onTranslationYEditingFinished);
    }
    if (m_widgets.videoRotationSpin) {
        connect(m_widgets.videoRotationSpin, qOverload<double>(&QDoubleSpinBox::valueChanged),
                this, &VideoKeyframeTab::onRotationChanged);
        connect(m_widgets.videoRotationSpin, &QDoubleSpinBox::editingFinished,
                this, &VideoKeyframeTab::onRotationEditingFinished);
    }
    if (m_widgets.videoScaleXSpin) {
        connect(m_widgets.videoScaleXSpin, qOverload<double>(&QDoubleSpinBox::valueChanged),
                this, &VideoKeyframeTab::onScaleXChanged);
        connect(m_widgets.videoScaleXSpin, &QDoubleSpinBox::editingFinished,
                this, &VideoKeyframeTab::onScaleXEditingFinished);
    }
    if (m_widgets.videoScaleYSpin) {
        connect(m_widgets.videoScaleYSpin, qOverload<double>(&QDoubleSpinBox::valueChanged),
                this, &VideoKeyframeTab::onScaleYChanged);
        connect(m_widgets.videoScaleYSpin, &QDoubleSpinBox::editingFinished,
                this, &VideoKeyframeTab::onScaleYEditingFinished);
    }
    if (m_widgets.videoInterpolationCombo) {
        connect(m_widgets.videoInterpolationCombo, &QComboBox::currentIndexChanged,
                this, &VideoKeyframeTab::onInterpolationChanged);
    }
    if (m_widgets.mirrorHorizontalCheckBox) {
        connect(m_widgets.mirrorHorizontalCheckBox, &QCheckBox::toggled,
                this, &VideoKeyframeTab::onMirrorHorizontalToggled);
    }
    if (m_widgets.mirrorVerticalCheckBox) {
        connect(m_widgets.mirrorVerticalCheckBox, &QCheckBox::toggled,
                this, &VideoKeyframeTab::onMirrorVerticalToggled);
    }
    if (m_widgets.lockVideoScaleCheckBox) {
        connect(m_widgets.lockVideoScaleCheckBox, &QCheckBox::toggled,
                this, &VideoKeyframeTab::onLockScaleToggled);
    }
    if (m_widgets.keyframeSpaceCheckBox) {
        connect(m_widgets.keyframeSpaceCheckBox, &QCheckBox::toggled,
                this, &VideoKeyframeTab::onKeyframeSpaceToggled);
    }
    if (m_widgets.addVideoKeyframeButton) {
        connect(m_widgets.addVideoKeyframeButton, &QPushButton::clicked,
                this, &VideoKeyframeTab::onAddKeyframeClicked);
    }
    if (m_widgets.removeVideoKeyframeButton) {
        connect(m_widgets.removeVideoKeyframeButton, &QPushButton::clicked,
                this, &VideoKeyframeTab::onRemoveKeyframeClicked);
    }
    if (m_widgets.videoKeyframeTable) {
        connect(m_widgets.videoKeyframeTable, &QTableWidget::itemSelectionChanged,
                this, &VideoKeyframeTab::onTableSelectionChanged);
        connect(m_widgets.videoKeyframeTable, &QTableWidget::itemChanged,
                this, &VideoKeyframeTab::onTableItemChanged);
        connect(m_widgets.videoKeyframeTable, &QTableWidget::itemClicked,
                this, &VideoKeyframeTab::onTableItemClicked);
        connect(m_widgets.videoKeyframeTable, &QTableWidget::itemDoubleClicked,
                this, &VideoKeyframeTab::onTableItemDoubleClicked);
        connect(m_widgets.videoKeyframeTable->horizontalHeader(), &QHeaderView::sectionClicked,
                this, &VideoKeyframeTab::onTableHeaderClicked);
        m_widgets.videoKeyframeTable->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(m_widgets.videoKeyframeTable, &QWidget::customContextMenuRequested,
                this, &VideoKeyframeTab::onTableCustomContextMenu);
    }
}

bool VideoKeyframeTab::showClipRelativeKeyframes() const
{
    return !m_widgets.keyframeSpaceCheckBox || m_widgets.keyframeSpaceCheckBox->isChecked();
}

int64_t VideoKeyframeTab::keyframeFrameForInspectorDisplay(const TimelineClip& clip, int64_t storedFrame) const
{
    return showClipRelativeKeyframes() ? storedFrame : (clip.startFrame + storedFrame);
}

int64_t VideoKeyframeTab::keyframeFrameFromInspectorDisplay(const TimelineClip& clip, int64_t displayedFrame) const
{
    return showClipRelativeKeyframes() ? displayedFrame : (displayedFrame - clip.startFrame);
}

qreal VideoKeyframeTab::inspectorScaleWithMirror(qreal magnitude, bool mirrored) const
{
    const qreal sanitized = sanitizeScaleValue(std::abs(magnitude));
    return mirrored ? -sanitized : sanitized;
}

void VideoKeyframeTab::syncScaleSpinPair(QDoubleSpinBox* changedSpin, QDoubleSpinBox* otherSpin)
{
    if (m_syncingScaleControls || !m_widgets.lockVideoScaleCheckBox || !m_widgets.lockVideoScaleCheckBox->isChecked() ||
        !changedSpin || !otherSpin) {
        return;
    }
    m_syncingScaleControls = true;
    otherSpin->setValue(std::abs(changedSpin->value()));
    m_syncingScaleControls = false;
}

VideoKeyframeTab::TransformKeyframeDisplay VideoKeyframeTab::keyframeForInspectorDisplay(
    const TimelineClip& clip, const TimelineClip::TransformKeyframe& keyframe) const
{
    if (showClipRelativeKeyframes()) {
        TransformKeyframeDisplay displayed;
        displayed.frame = keyframe.frame;
        displayed.translationX = keyframe.translationX;
        displayed.translationY = keyframe.translationY;
        displayed.rotation = keyframe.rotation;
        displayed.scaleX = keyframe.scaleX;
        displayed.scaleY = keyframe.scaleY;
        displayed.linearInterpolation = keyframe.linearInterpolation;
        return displayed;
    }
    
    TransformKeyframeDisplay displayed;
    displayed.frame = clip.startFrame + keyframe.frame;
    displayed.translationX = clip.baseTranslationX + keyframe.translationX;
    displayed.translationY = clip.baseTranslationY + keyframe.translationY;
    displayed.rotation = clip.baseRotation + keyframe.rotation;
    displayed.scaleX = sanitizeScaleValue(clip.baseScaleX) * sanitizeScaleValue(keyframe.scaleX);
    displayed.scaleY = sanitizeScaleValue(clip.baseScaleY) * sanitizeScaleValue(keyframe.scaleY);
    displayed.linearInterpolation = keyframe.linearInterpolation;
    return displayed;
}

TimelineClip::TransformKeyframe VideoKeyframeTab::keyframeFromInspectorDisplay(
    const TimelineClip& clip, const TransformKeyframeDisplay& displayed) const
{
    if (showClipRelativeKeyframes()) {
        TimelineClip::TransformKeyframe stored;
        stored.frame = displayed.frame;
        stored.translationX = displayed.translationX;
        stored.translationY = displayed.translationY;
        stored.rotation = displayed.rotation;
        stored.scaleX = displayed.scaleX;
        stored.scaleY = displayed.scaleY;
        stored.linearInterpolation = displayed.linearInterpolation;
        return stored;
    }
    
    TimelineClip::TransformKeyframe stored;
    stored.frame = displayed.frame - clip.startFrame;
    stored.translationX = displayed.translationX - clip.baseTranslationX;
    stored.translationY = displayed.translationY - clip.baseTranslationY;
    stored.rotation = displayed.rotation - clip.baseRotation;
    stored.scaleX = sanitizeScaleValue(displayed.scaleX / sanitizeScaleValue(clip.baseScaleX));
    stored.scaleY = sanitizeScaleValue(displayed.scaleY / sanitizeScaleValue(clip.baseScaleY));
    stored.linearInterpolation = displayed.linearInterpolation;
    return stored;
}

QString VideoKeyframeTab::videoInterpolationLabel(bool linearInterpolation) const
{
    return linearInterpolation ? QStringLiteral("Linear") : QStringLiteral("Step");
}

QString VideoKeyframeTab::nextVideoInterpolationLabel(const QString& text) const
{
    bool linearInterpolation = true;
    if (!parseVideoInterpolationText(text, &linearInterpolation)) {
        linearInterpolation = true;
    }
    return videoInterpolationLabel(!linearInterpolation);
}

bool VideoKeyframeTab::parseVideoInterpolationText(const QString& text, bool* linearInterpolationOut) const
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

int VideoKeyframeTab::selectedKeyframeIndex(const TimelineClip& clip) const
{
    for (int i = 0; i < clip.transformKeyframes.size(); ++i) {
        if (clip.transformKeyframes[i].frame == m_selectedKeyframeFrame) {
            return i;
        }
    }
    return -1;
}

QList<int64_t> VideoKeyframeTab::selectedKeyframeFramesForClip(const TimelineClip& clip) const
{
    QList<int64_t> frames;
    for (const TimelineClip::TransformKeyframe& keyframe : clip.transformKeyframes) {
        if (m_selectedKeyframeFrames.contains(keyframe.frame)) {
            frames.push_back(keyframe.frame);
        }
    }
    return frames;
}

int VideoKeyframeTab::nearestKeyframeIndex(const TimelineClip& clip) const
{
    if (!m_deps.getSelectedClip() || clip.transformKeyframes.isEmpty()) {
        return -1;
    }
    const int64_t localFrame = qBound<int64_t>(0,
                                               m_deps.getCurrentTimelineFrame() - clip.startFrame,
                                               qMax<int64_t>(0, clip.durationFrames - 1));
    int nearestIndex = 0;
    int64_t nearestDistance = std::numeric_limits<int64_t>::max();
    for (int i = 0; i < clip.transformKeyframes.size(); ++i) {
        const int64_t distance = std::abs(clip.transformKeyframes[i].frame - localFrame);
        if (distance < nearestDistance) {
            nearestDistance = distance;
            nearestIndex = i;
        }
    }
    return nearestIndex;
}

bool VideoKeyframeTab::hasRemovableKeyframeSelection(const TimelineClip& clip) const
{
    for (const int64_t frame : selectedKeyframeFramesForClip(clip)) {
        if (frame > 0) {
            return true;
        }
    }
    return false;
}

VideoKeyframeTab::TransformKeyframeDisplay VideoKeyframeTab::evaluateDisplayedTransform(const TimelineClip& clip, int64_t localFrame) const
{
    TransformKeyframeDisplay result;
    result.translationX = clip.baseTranslationX;
    result.translationY = clip.baseTranslationY;
    result.rotation = clip.baseRotation;
    result.scaleX = clip.baseScaleX;
    result.scaleY = clip.baseScaleY;
    result.linearInterpolation = true;

    if (clip.transformKeyframes.isEmpty()) {
        return result;
    }

    int beforeIndex = -1;
    for (int i = clip.transformKeyframes.size() - 1; i >= 0; --i) {
        if (clip.transformKeyframes[i].frame <= localFrame) {
            beforeIndex = i;
            break;
        }
    }

    if (beforeIndex < 0) {
        const auto& kf = clip.transformKeyframes[0];
        result.translationX = clip.baseTranslationX + kf.translationX;
        result.translationY = clip.baseTranslationY + kf.translationY;
        result.rotation = clip.baseRotation + kf.rotation;
        result.scaleX = clip.baseScaleX * kf.scaleX;
        result.scaleY = clip.baseScaleY * kf.scaleY;
        result.linearInterpolation = kf.linearInterpolation;
        return result;
    }

    const auto& before = clip.transformKeyframes[beforeIndex];
    
    if (beforeIndex == clip.transformKeyframes.size() - 1 || before.frame == localFrame) {
        result.translationX = clip.baseTranslationX + before.translationX;
        result.translationY = clip.baseTranslationY + before.translationY;
        result.rotation = clip.baseRotation + before.rotation;
        result.scaleX = clip.baseScaleX * before.scaleX;
        result.scaleY = clip.baseScaleY * before.scaleY;
        result.linearInterpolation = before.linearInterpolation;
        return result;
    }

    int afterIndex = beforeIndex + 1;
    const auto& after = clip.transformKeyframes[afterIndex];

    if (!before.linearInterpolation) {
        result.translationX = clip.baseTranslationX + before.translationX;
        result.translationY = clip.baseTranslationY + before.translationY;
        result.rotation = clip.baseRotation + before.rotation;
        result.scaleX = clip.baseScaleX * before.scaleX;
        result.scaleY = clip.baseScaleY * before.scaleY;
        return result;
    }

    const int64_t range = after.frame - before.frame;
    if (range <= 0) {
        result.translationX = clip.baseTranslationX + before.translationX;
        result.translationY = clip.baseTranslationY + before.translationY;
        result.rotation = clip.baseRotation + before.rotation;
        result.scaleX = clip.baseScaleX * before.scaleX;
        result.scaleY = clip.baseScaleY * before.scaleY;
        return result;
    }

    const double t = static_cast<double>(localFrame - before.frame) / static_cast<double>(range);
    result.translationX = clip.baseTranslationX + before.translationX + (after.translationX - before.translationX) * t;
    result.translationY = clip.baseTranslationY + before.translationY + (after.translationY - before.translationY) * t;
    result.rotation = clip.baseRotation + before.rotation + (after.rotation - before.rotation) * t;
    result.scaleX = clip.baseScaleX * (before.scaleX + (after.scaleX - before.scaleX) * t);
    result.scaleY = clip.baseScaleY * (before.scaleY + (after.scaleY - before.scaleY) * t);
    result.linearInterpolation = after.linearInterpolation;

    return result;
}

void VideoKeyframeTab::updateSpinBoxesFromKeyframe(const TransformKeyframeDisplay& keyframe)
{
    QSignalBlocker txBlock(m_widgets.videoTranslationXSpin);
    QSignalBlocker tyBlock(m_widgets.videoTranslationYSpin);
    QSignalBlocker rotBlock(m_widgets.videoRotationSpin);
    QSignalBlocker sxBlock(m_widgets.videoScaleXSpin);
    QSignalBlocker syBlock(m_widgets.videoScaleYSpin);

    m_widgets.videoTranslationXSpin->setValue(keyframe.translationX);
    m_widgets.videoTranslationYSpin->setValue(keyframe.translationY);
    m_widgets.videoRotationSpin->setValue(keyframe.rotation);
    m_widgets.videoScaleXSpin->setValue(std::abs(keyframe.scaleX));
    m_widgets.videoScaleYSpin->setValue(std::abs(keyframe.scaleY));
}

void VideoKeyframeTab::updateMirrorCheckboxesFromScale(double scaleX, double scaleY)
{
    QSignalBlocker mirrorHBlock(m_widgets.mirrorHorizontalCheckBox);
    QSignalBlocker mirrorVBlock(m_widgets.mirrorVerticalCheckBox);
    
    m_widgets.mirrorHorizontalCheckBox->setChecked(scaleX < 0.0);
    m_widgets.mirrorVerticalCheckBox->setChecked(scaleY < 0.0);
}

void VideoKeyframeTab::populateTable(const TimelineClip& clip)
{
    QList<int64_t> frames;
    if (clip.transformKeyframes.isEmpty()) {
        frames.push_back(0);
    } else {
        frames.reserve(clip.transformKeyframes.size());
        for (const TimelineClip::TransformKeyframe& keyframe : clip.transformKeyframes) {
            frames.push_back(keyframe.frame);
        }
        std::sort(frames.begin(), frames.end());
    }

    m_widgets.videoKeyframeTable->setRowCount(frames.size());
    
    for (int row = 0; row < frames.size(); ++row) {
        const int64_t frame = frames[row];
        TransformKeyframeDisplay displayedFrame;
        
        if (clip.transformKeyframes.isEmpty()) {
            displayedFrame = evaluateDisplayedTransform(clip, clip.startFrame);
            displayedFrame.frame = 0;
        } else {
            for (const TimelineClip::TransformKeyframe& keyframe : clip.transformKeyframes) {
                if (keyframe.frame == frame) {
                    displayedFrame = keyframeForInspectorDisplay(clip, keyframe);
                    break;
                }
            }
        }

        const QStringList rowValues = {
            QString::number(frame),
            QString::number(displayedFrame.translationX, 'f', 3),
            QString::number(displayedFrame.translationY, 'f', 3),
            QString::number(displayedFrame.rotation, 'f', 3),
            QString::number(displayedFrame.scaleX, 'f', 3),
            QString::number(displayedFrame.scaleY, 'f', 3),
            videoInterpolationLabel(displayedFrame.linearInterpolation)};

        for (int column = 0; column < rowValues.size(); ++column) {
            auto* item = new QTableWidgetItem(rowValues[column]);
            item->setData(Qt::UserRole, QVariant::fromValue(static_cast<qint64>(frame)));
            if (clip.transformKeyframes.isEmpty()) {
                item->setToolTip(QStringLiteral("Base transform (no stored keyframes yet)"));
            }
            m_widgets.videoKeyframeTable->setItem(row, column, item);
        }
    }
}

void VideoKeyframeTab::refresh()
{
    if (!m_widgets.videoKeyframeTable || !m_widgets.keyframesInspectorClipLabel || 
        !m_widgets.keyframesInspectorDetailsLabel) {
        return;
    }

    const TimelineClip* clip = m_deps.getSelectedClip();
    m_updating = true;

    QSignalBlocker tableBlocker(m_widgets.videoKeyframeTable);
    QSignalBlocker txBlocker(m_widgets.videoTranslationXSpin);
    QSignalBlocker tyBlocker(m_widgets.videoTranslationYSpin);
    QSignalBlocker rotBlocker(m_widgets.videoRotationSpin);
    QSignalBlocker sxBlocker(m_widgets.videoScaleXSpin);
    QSignalBlocker syBlocker(m_widgets.videoScaleYSpin);
    QSignalBlocker interpBlocker(m_widgets.videoInterpolationCombo);
    QSignalBlocker mirrorHBlocker(m_widgets.mirrorHorizontalCheckBox);
    QSignalBlocker mirrorVBlocker(m_widgets.mirrorVerticalCheckBox);

    m_widgets.videoKeyframeTable->clearContents();
    m_widgets.videoKeyframeTable->setRowCount(0);

    if (!clip || !m_deps.clipHasVisuals(*clip)) {
        m_widgets.keyframesInspectorClipLabel->setText(QStringLiteral("No visual clip selected"));
        m_widgets.keyframesInspectorDetailsLabel->setText(
            QStringLiteral("Select a visual clip to inspect its keyframes."));
        m_widgets.videoTranslationXSpin->setValue(0.0);
        m_widgets.videoTranslationYSpin->setValue(0.0);
        m_widgets.videoRotationSpin->setValue(0.0);
        m_widgets.videoScaleXSpin->setValue(1.0);
        m_widgets.videoScaleYSpin->setValue(1.0);
        m_widgets.videoInterpolationCombo->setCurrentIndex(1);
        m_widgets.mirrorHorizontalCheckBox->setChecked(false);
        m_widgets.mirrorVerticalCheckBox->setChecked(false);
        m_selectedKeyframeFrame = -1;
        m_selectedKeyframeFrames.clear();
        m_updating = false;
        return;
    }
        const QString sourcePath = QDir::toNativeSeparators(m_deps.getClipFilePath(*clip));
    const QString sourceLabel = QStringLiteral("%1 | %2")
                                    .arg(clipMediaTypeLabel(clip->mediaType),
                                         mediaSourceKindLabel(clip->sourceKind));
    m_widgets.keyframesInspectorClipLabel->setText(QStringLiteral("%1\n%2").arg(clip->label, sourceLabel));

    populateTable(*clip);

    if (m_selectedKeyframeFrame < 0) {
        const int selectedIndex = clip->transformKeyframes.isEmpty() ? 0 : nearestKeyframeIndex(*clip);
        m_selectedKeyframeFrame = selectedIndex >= 0 && selectedIndex < clip->transformKeyframes.size()
                                     ? clip->transformKeyframes[selectedIndex].frame
                                     : 0;
        m_selectedKeyframeFrames = {m_selectedKeyframeFrame};
    } else if (m_selectedKeyframeFrames.isEmpty()) {
        m_selectedKeyframeFrames = {m_selectedKeyframeFrame};
    }

    TransformKeyframeDisplay displayed;
    if (clip->transformKeyframes.isEmpty()) {
        displayed = evaluateDisplayedTransform(*clip, clip->startFrame);
        displayed.frame = 0;
    } else {
        int selectedIndex = selectedKeyframeIndex(*clip);
        if (selectedIndex < 0) {
            selectedIndex = nearestKeyframeIndex(*clip);
        }
        if (selectedIndex < 0) {
            selectedIndex = 0;
        }
        displayed = keyframeForInspectorDisplay(*clip, clip->transformKeyframes[selectedIndex]);
    }

    updateSpinBoxesFromKeyframe(displayed);
    updateMirrorCheckboxesFromScale(displayed.scaleX, displayed.scaleY);
    m_widgets.videoInterpolationCombo->setCurrentIndex(displayed.linearInterpolation ? 1 : 0);

    editor::restoreSelectionByFrameRole(m_widgets.videoKeyframeTable, m_selectedKeyframeFrames);

    const QString keyframeSummary = clip->transformKeyframes.isEmpty()
                                        ? QStringLiteral("Using base transform only")
                                        : QStringLiteral("%1 stored transform keyframe%2")
                                              .arg(clip->transformKeyframes.size())
                                              .arg(clip->transformKeyframes.size() == 1 ? QString() : QStringLiteral("s"));
    m_widgets.keyframesInspectorDetailsLabel->setText(
        QStringLiteral("%1\nSource: %2")
            .arg(keyframeSummary, sourcePath));
    m_widgets.keyframesInspectorDetailsLabel->setToolTip(sourcePath);
    m_updating = false;
    syncTableToPlayhead();
}

void VideoKeyframeTab::applyKeyframeFromInspector(bool pushHistory)
{
    if (m_updating || !m_deps.getSelectedClip()) {
        return;
    }

    const QString clipId = m_deps.getSelectedClipId();
    if (clipId.isEmpty() || m_selectedKeyframeFrame < 0) {
        return;
    }

    const bool updated = m_deps.updateClipById(clipId, [this](TimelineClip& clip) {
        const int index = selectedKeyframeIndex(clip);
        if (index < 0) {
            return;
        }
        TimelineClip::TransformKeyframe& keyframe = clip.transformKeyframes[index];
        TransformKeyframeDisplay displayed = keyframeForInspectorDisplay(clip, keyframe);
        
        displayed.translationX = m_widgets.videoTranslationXSpin->value();
        displayed.translationY = m_widgets.videoTranslationYSpin->value();
        displayed.rotation = m_widgets.videoRotationSpin->value();
        displayed.scaleX = inspectorScaleWithMirror(m_widgets.videoScaleXSpin->value(),
                                                    m_widgets.mirrorHorizontalCheckBox && m_widgets.mirrorHorizontalCheckBox->isChecked());
        displayed.scaleY = inspectorScaleWithMirror(m_widgets.videoScaleYSpin->value(),
                                                    m_widgets.mirrorVerticalCheckBox && m_widgets.mirrorVerticalCheckBox->isChecked());
        
        const TimelineClip::TransformKeyframe stored = keyframeFromInspectorDisplay(clip, displayed);
        keyframe.translationX = stored.translationX;
        keyframe.translationY = stored.translationY;
        keyframe.rotation = stored.rotation;
        keyframe.scaleX = stored.scaleX;
        keyframe.scaleY = stored.scaleY;
        keyframe.linearInterpolation = m_widgets.videoInterpolationCombo->currentIndex() == 1;
        normalizeClipTransformKeyframes(clip);
    });

    if (!updated) {
        return;
    }

    m_deps.setPreviewTimelineClips();
    m_deps.refreshInspector();
    m_deps.scheduleSaveState();
    if (pushHistory) {
        m_deps.pushHistorySnapshot();
    }
    emit keyframeApplied();
}

void VideoKeyframeTab::upsertKeyframeAtPlayhead()
{
    const TimelineClip* clip = m_deps.getSelectedClip();
    if (!clip || !m_deps.clipHasVisuals(*clip)) {
        return;
    }

    const int64_t keyframeFrame = qBound<int64_t>(0,
                                                  m_deps.getCurrentTimelineFrame() - clip->startFrame,
                                                  qMax<int64_t>(0, clip->durationFrames - 1));
    m_selectedKeyframeFrame = keyframeFrame;
    m_selectedKeyframeFrames = {keyframeFrame};

    const bool updated = m_deps.updateClipById(clip->id, [this, keyframeFrame](TimelineClip& editableClip) {
        TransformKeyframeDisplay displayed;
        displayed.frame = keyframeFrame;
        displayed.translationX = m_widgets.videoTranslationXSpin->value();
        displayed.translationY = m_widgets.videoTranslationYSpin->value();
        displayed.rotation = m_widgets.videoRotationSpin->value();
        displayed.scaleX = inspectorScaleWithMirror(
            m_widgets.videoScaleXSpin->value(),
            m_widgets.mirrorHorizontalCheckBox && m_widgets.mirrorHorizontalCheckBox->isChecked());
        displayed.scaleY = inspectorScaleWithMirror(
            m_widgets.videoScaleYSpin->value(),
            m_widgets.mirrorVerticalCheckBox && m_widgets.mirrorVerticalCheckBox->isChecked());
        TimelineClip::TransformKeyframe keyframe = keyframeFromInspectorDisplay(editableClip, displayed);
        keyframe.frame = keyframeFrame;
        keyframe.linearInterpolation = m_widgets.videoInterpolationCombo->currentIndex() == 1;

        bool replaced = false;
        for (TimelineClip::TransformKeyframe& existing : editableClip.transformKeyframes) {
            if (existing.frame == keyframeFrame) {
                existing = keyframe;
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            editableClip.transformKeyframes.push_back(keyframe);
        }
        normalizeClipTransformKeyframes(editableClip);
    });

    if (!updated) {
        return;
    }

    m_deps.setPreviewTimelineClips();
    m_deps.refreshInspector();
    m_deps.scheduleSaveState();
    m_deps.pushHistorySnapshot();
    emit keyframeAdded();
}

void VideoKeyframeTab::removeSelectedKeyframes()
{
    if (m_selectedKeyframeFrames.isEmpty()) {
        return;
    }

    const QString clipId = m_deps.getSelectedClipId();
    if (clipId.isEmpty()) {
        return;
    }

    const TimelineClip* selectedClip = m_deps.getSelectedClip();
    if (!selectedClip) {
        return;
    }

    QList<int64_t> selectedFrames = selectedKeyframeFramesForClip(*selectedClip);
    selectedFrames.erase(std::remove_if(selectedFrames.begin(),
                                        selectedFrames.end(),
                                        [](int64_t frame) { return frame <= 0; }),
                         selectedFrames.end());

    if (selectedFrames.isEmpty()) {
        m_deps.refreshInspector();
        return;
    }

    const bool updated = m_deps.updateClipById(clipId, [selectedFrames](TimelineClip& clip) {
        clip.transformKeyframes.erase(
            std::remove_if(clip.transformKeyframes.begin(),
                           clip.transformKeyframes.end(),
                           [&selectedFrames](const TimelineClip::TransformKeyframe& keyframe) {
                               return selectedFrames.contains(keyframe.frame);
                           }),
            clip.transformKeyframes.end());
        normalizeClipTransformKeyframes(clip);
    });

    if (!updated) {
        return;
    }

    m_selectedKeyframeFrame = 0;
    m_selectedKeyframeFrames = {0};
    m_deps.setPreviewTimelineClips();
    m_deps.refreshInspector();
    m_deps.scheduleSaveState();
    m_deps.pushHistorySnapshot();
    emit keyframesRemoved();
}

void VideoKeyframeTab::duplicateSelectedKeyframes(int frameDelta)
{
    if (!m_deps.getSelectedClip() || frameDelta == 0) {
        return;
    }

    const TimelineClip* selectedClip = m_deps.getSelectedClip();
    if (!selectedClip || !m_deps.clipHasVisuals(*selectedClip)) {
        return;
    }

    const QList<int64_t> selectedFrames = selectedKeyframeFramesForClip(*selectedClip);
    if (selectedFrames.isEmpty()) {
        return;
    }

    QSet<int64_t> sourceFrames;
    for (int64_t frame : selectedFrames) {
        sourceFrames.insert(frame);
    }

    const int64_t maxFrame = qMax<int64_t>(0, selectedClip->durationFrames - 1);
    QSet<int64_t> newFrames;

    const bool updated = m_deps.updateClipById(selectedClip->id, [frameDelta, maxFrame, sourceFrames, &newFrames](TimelineClip& clip) {
        bool changed = false;
        const QVector<TimelineClip::TransformKeyframe> originalKeyframes = clip.transformKeyframes;
        
        for (const TimelineClip::TransformKeyframe& keyframe : originalKeyframes) {
            if (!sourceFrames.contains(keyframe.frame)) {
                continue;
            }
            
            const int64_t newFrame = qBound<int64_t>(0, keyframe.frame + frameDelta, maxFrame);
            TimelineClip::TransformKeyframe duplicate = keyframe;
            duplicate.frame = newFrame;
            
            bool replaced = false;
            for (TimelineClip::TransformKeyframe& existing : clip.transformKeyframes) {
                if (existing.frame == newFrame) {
                    existing = duplicate;
                    replaced = true;
                    changed = true;
                    break;
                }
            }
            
            if (!replaced) {
                clip.transformKeyframes.push_back(duplicate);
                changed = true;
            }
            newFrames.insert(newFrame);
        }
        
        if (changed) {
            normalizeClipTransformKeyframes(clip);
        }
    });

    if (!updated || newFrames.isEmpty()) {
        return;
    }

    m_selectedKeyframeFrames = newFrames;
    m_selectedKeyframeFrame = *std::min_element(newFrames.begin(), newFrames.end());
    m_deps.setPreviewTimelineClips();
    m_deps.refreshInspector();
    m_deps.scheduleSaveState();
    m_deps.pushHistorySnapshot();
}

void VideoKeyframeTab::duplicateSelectedKeyframesToFrame(int64_t targetFrame)
{
    if (!m_deps.getSelectedClip()) {
        return;
    }

    const TimelineClip* selectedClip = m_deps.getSelectedClip();
    if (!selectedClip || !m_deps.clipHasVisuals(*selectedClip)) {
        return;
    }

    const QList<int64_t> selectedFrames = selectedKeyframeFramesForClip(*selectedClip);
    if (selectedFrames.isEmpty()) {
        return;
    }

    const int64_t maxFrame = qMax<int64_t>(0, selectedClip->durationFrames - 1);
    const int64_t boundedTarget = qBound<int64_t>(0, targetFrame, maxFrame);
    QSet<int64_t> sourceFrames;
    for (int64_t frame : selectedFrames) {
        sourceFrames.insert(frame);
    }

    const bool updated = m_deps.updateClipById(selectedClip->id,
                                               [boundedTarget, sourceFrames](TimelineClip& clip) {
        bool changed = false;
        const QVector<TimelineClip::TransformKeyframe> originalKeyframes = clip.transformKeyframes;

        for (const TimelineClip::TransformKeyframe& keyframe : originalKeyframes) {
            if (!sourceFrames.contains(keyframe.frame)) {
                continue;
            }

            TimelineClip::TransformKeyframe duplicate = keyframe;
            duplicate.frame = boundedTarget;

            bool replaced = false;
            for (TimelineClip::TransformKeyframe& existing : clip.transformKeyframes) {
                if (existing.frame == boundedTarget) {
                    existing = duplicate;
                    replaced = true;
                    changed = true;
                    break;
                }
            }

            if (!replaced) {
                clip.transformKeyframes.push_back(duplicate);
                changed = true;
            }
            break;
        }

        if (changed) {
            normalizeClipTransformKeyframes(clip);
        }
    });

    if (!updated) {
        return;
    }

    m_selectedKeyframeFrames = {boundedTarget};
    m_selectedKeyframeFrame = boundedTarget;
    m_deps.setPreviewTimelineClips();
    m_deps.refreshInspector();
    m_deps.scheduleSaveState();
    m_deps.pushHistorySnapshot();
}

bool VideoKeyframeTab::insertInterpolatedKeyframeBetween(int64_t earlierFrame, int64_t laterFrame)
{
    if (!m_deps.getSelectedClip() || earlierFrame < 0 || laterFrame < 0 || laterFrame <= earlierFrame) {
        return false;
    }

    const TimelineClip* selectedClip = m_deps.getSelectedClip();
    if (!selectedClip || !m_deps.clipHasVisuals(*selectedClip)) {
        return false;
    }

    const int64_t midpointFrame = earlierFrame + ((laterFrame - earlierFrame) / 2);
    if (midpointFrame <= earlierFrame || midpointFrame >= laterFrame) {
        return false;
    }

    const auto findKeyframeAt = [](const TimelineClip& clip, int64_t frame) -> const TimelineClip::TransformKeyframe* {
        for (const TimelineClip::TransformKeyframe& keyframe : clip.transformKeyframes) {
            if (keyframe.frame == frame) {
                return &keyframe;
            }
        }
        return nullptr;
    };

    const TimelineClip::TransformKeyframe* earlier = findKeyframeAt(*selectedClip, earlierFrame);
    const TimelineClip::TransformKeyframe* later = findKeyframeAt(*selectedClip, laterFrame);
    if (!earlier || !later) {
        return false;
    }

    const qreal t = static_cast<qreal>(midpointFrame - earlierFrame) /
                    static_cast<qreal>(laterFrame - earlierFrame);
    
    TimelineClip::TransformKeyframe midpoint;
    midpoint.frame = midpointFrame;
    midpoint.translationX = earlier->translationX + ((later->translationX - earlier->translationX) * t);
    midpoint.translationY = earlier->translationY + ((later->translationY - earlier->translationY) * t);
    midpoint.rotation = earlier->rotation + ((later->rotation - earlier->rotation) * t);
    midpoint.scaleX = earlier->scaleX + ((later->scaleX - earlier->scaleX) * t);
    midpoint.scaleY = earlier->scaleY + ((later->scaleY - earlier->scaleY) * t);
    midpoint.linearInterpolation = later->linearInterpolation;

    const bool updated = m_deps.updateClipById(selectedClip->id, [midpoint](TimelineClip& clip) {
        for (TimelineClip::TransformKeyframe& existing : clip.transformKeyframes) {
            if (existing.frame == midpoint.frame) {
                existing = midpoint;
                normalizeClipTransformKeyframes(clip);
                return;
            }
        }
        clip.transformKeyframes.push_back(midpoint);
        normalizeClipTransformKeyframes(clip);
    });

    if (!updated) {
        return false;
    }

    m_selectedKeyframeFrame = midpoint.frame;
    m_selectedKeyframeFrames = {midpoint.frame};
    m_deps.setPreviewTimelineClips();
    m_deps.refreshInspector();
    m_deps.seekToTimelineFrame(selectedClip->startFrame + midpoint.frame);
    m_deps.scheduleSaveState();
    m_deps.pushHistorySnapshot();
    return true;
}

void VideoKeyframeTab::syncTableToPlayhead()
{
    if (!m_widgets.videoKeyframeTable || m_updating) {
        return;
    }

    if (!m_widgets.keyframesFollowCurrentCheckBox || !m_widgets.keyframesFollowCurrentCheckBox->isChecked()) {
        return;
    }

    const TimelineClip* clip = m_deps.getSelectedClip();
    if (!clip || !m_deps.clipHasVisuals(*clip) || m_widgets.videoKeyframeTable->rowCount() <= 0) {
        m_widgets.videoKeyframeTable->clearSelection();
        return;
    }

    QWidget* focus = QApplication::focusWidget();
    const bool editingKeyframes = m_widgets.videoKeyframeTable &&
                                  focus &&
                                  m_widgets.videoKeyframeTable->isAncestorOf(focus) &&
                                  (qobject_cast<QLineEdit*>(focus) ||
                                   qobject_cast<QAbstractSpinBox*>(focus));
    if (editingKeyframes) {
        return;
    }

    const int64_t localFrame = qBound<int64_t>(0,
                                               m_deps.getCurrentTimelineFrame() - clip->startFrame,
                                               qMax<int64_t>(0, clip->durationFrames - 1));

    int matchingRow = -1;
    int64_t matchingFrame = -1;
    for (int row = 0; row < m_widgets.videoKeyframeTable->rowCount(); ++row) {
        QTableWidgetItem* item = m_widgets.videoKeyframeTable->item(row, 0);
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

    if (!m_widgets.videoKeyframeTable->selectionModel()->isRowSelected(matchingRow, QModelIndex())) {
        m_syncingTableSelection = true;
        m_widgets.videoKeyframeTable->setCurrentCell(matchingRow, 0);
        m_widgets.videoKeyframeTable->selectRow(matchingRow);
        m_syncingTableSelection = false;
    }

    if (m_widgets.keyframesAutoScrollCheckBox && m_widgets.keyframesAutoScrollCheckBox->isChecked()) {
        if (QTableWidgetItem* item = m_widgets.videoKeyframeTable->item(matchingRow, 0)) {
            m_widgets.videoKeyframeTable->scrollToItem(item, QAbstractItemView::PositionAtCenter);
        }
    }
}

void VideoKeyframeTab::setSelectedKeyframeFrame(int64_t frame)
{
    m_selectedKeyframeFrame = frame;
    m_selectedKeyframeFrames = {frame};
}

void VideoKeyframeTab::promptMultiplySelectedKeyframeScale(bool scaleX)
{
    const TimelineClip* selectedClip = m_deps.getSelectedClip();
    if (!selectedClip || !m_deps.clipHasVisuals(*selectedClip)) {
        QMessageBox::warning(nullptr,
                             QStringLiteral("Multiply Keyframe Scale"),
                             QStringLiteral("Select a visual clip first."));
        return;
    }
    
    if (selectedClip->transformKeyframes.isEmpty()) {
        QMessageBox::warning(nullptr,
                             QStringLiteral("Multiply Keyframe Scale"),
                             QStringLiteral("This clip has no stored keyframes to modify."));
        return;
    }

    bool ok = false;
    const double multiplier = QInputDialog::getDouble(
        nullptr,
        scaleX ? QStringLiteral("Multiply Scale X") : QStringLiteral("Multiply Scale Y"),
        scaleX ? QStringLiteral("Multiply all keyframe Scale X values by:")
               : QStringLiteral("Multiply all keyframe Scale Y values by:"),
        1.0,
        0.001,
        1000.0,
        3,
        &ok);
    
    if (!ok) {
        return;
    }

    const bool updated = m_deps.updateClipById(selectedClip->id, [multiplier, scaleX](TimelineClip& clip) {
        for (TimelineClip::TransformKeyframe& keyframe : clip.transformKeyframes) {
            if (scaleX) {
                keyframe.scaleX *= multiplier;
            } else {
                keyframe.scaleY *= multiplier;
            }
        }
        normalizeClipTransformKeyframes(clip);
    });

    if (!updated) {
        return;
    }

    m_deps.setPreviewTimelineClips();
    refresh();
    m_deps.scheduleSaveState();
    m_deps.pushHistorySnapshot();
}

void VideoKeyframeTab::onTranslationXChanged(double value)
{
    Q_UNUSED(value);
    if (m_updating) return;
    applyKeyframeFromInspector(false);
}

void VideoKeyframeTab::onTranslationYChanged(double value)
{
    Q_UNUSED(value);
    if (m_updating) return;
    applyKeyframeFromInspector(false);
}

void VideoKeyframeTab::onRotationChanged(double value)
{
    Q_UNUSED(value);
    if (m_updating) return;
    applyKeyframeFromInspector(false);
}

void VideoKeyframeTab::onScaleXChanged(double value)
{
    Q_UNUSED(value);
    if (m_updating) return;
    syncScaleSpinPair(m_widgets.videoScaleXSpin, m_widgets.videoScaleYSpin);
    applyKeyframeFromInspector(false);
}

void VideoKeyframeTab::onScaleYChanged(double value)
{
    Q_UNUSED(value);
    if (m_updating) return;
    syncScaleSpinPair(m_widgets.videoScaleYSpin, m_widgets.videoScaleXSpin);
    applyKeyframeFromInspector(false);
}

void VideoKeyframeTab::onTranslationXEditingFinished()
{
    if (m_updating) return;
    applyKeyframeFromInspector(true);
}

void VideoKeyframeTab::onTranslationYEditingFinished()
{
    if (m_updating) return;
    applyKeyframeFromInspector(true);
}

void VideoKeyframeTab::onRotationEditingFinished()
{
    if (m_updating) return;
    applyKeyframeFromInspector(true);
}

void VideoKeyframeTab::onScaleXEditingFinished()
{
    if (m_updating) return;
    applyKeyframeFromInspector(true);
}

void VideoKeyframeTab::onScaleYEditingFinished()
{
    if (m_updating) return;
    applyKeyframeFromInspector(true);
}

void VideoKeyframeTab::onInterpolationChanged(int index)
{
    Q_UNUSED(index);
    if (m_updating) return;
    applyKeyframeFromInspector(true);
}

void VideoKeyframeTab::onMirrorHorizontalToggled(bool checked)
{
    Q_UNUSED(checked);
    if (m_updating) return;
    applyKeyframeFromInspector(true);
}

void VideoKeyframeTab::onMirrorVerticalToggled(bool checked)
{
    Q_UNUSED(checked);
    if (m_updating) return;
    applyKeyframeFromInspector(true);
}

void VideoKeyframeTab::onLockScaleToggled(bool checked)
{
    Q_UNUSED(checked);
    if (m_updating) return;
    m_deps.scheduleSaveState();
}

void VideoKeyframeTab::onKeyframeSpaceToggled(bool checked)
{
    Q_UNUSED(checked);
    if (m_updating) return;
    refresh();
    m_deps.scheduleSaveState();
}

void VideoKeyframeTab::onAutoScrollToggled(bool checked)
{
    Q_UNUSED(checked);
    if (m_updating) return;
    m_deps.scheduleSaveState();
}

void VideoKeyframeTab::onFollowCurrentToggled(bool checked)
{
    Q_UNUSED(checked);
    if (m_updating) return;
    if (m_widgets.keyframesFollowCurrentCheckBox && m_widgets.keyframesFollowCurrentCheckBox->isChecked()) {
        syncTableToPlayhead();
    }
    m_deps.scheduleSaveState();
}

void VideoKeyframeTab::onAddKeyframeClicked()
{
    upsertKeyframeAtPlayhead();
}

void VideoKeyframeTab::onRemoveKeyframeClicked()
{
    removeSelectedKeyframes();
}

void VideoKeyframeTab::onTableSelectionChanged()
{
    if (m_updating || m_syncingTableSelection) return;

    const QSet<int64_t> selectedFrames =
        editor::collectSelectedFrameRoles(m_widgets.videoKeyframeTable);
    const int64_t primaryFrame =
        editor::primarySelectedFrameRole(m_widgets.videoKeyframeTable);

    if (primaryFrame < 0) return;

    m_selectedKeyframeFrame = primaryFrame;
    m_selectedKeyframeFrames = selectedFrames;
    refresh();

    const TimelineClip* selectedClip = m_deps.getSelectedClip();
    if (selectedClip && m_deps.seekToTimelineFrame) {
        m_pendingSeekTimelineFrame = selectedClip->startFrame + primaryFrame;
        m_deferredSeekTimer.start(QApplication::doubleClickInterval());
    }

    if (m_deps.onKeyframeSelectionChanged) {
        m_deps.onKeyframeSelectionChanged();
    }
}

void VideoKeyframeTab::onTableItemChanged(QTableWidgetItem* changedItem)
{
    if (m_updating || !changedItem) {
        if (m_deps.onKeyframeItemChanged && changedItem) {
            m_deps.onKeyframeItemChanged(changedItem);
        }
        return;
    }

    const TimelineClip* selectedClip = m_deps.getSelectedClip();
    if (!selectedClip || !m_deps.clipHasVisuals(*selectedClip)) return;

    const int row = changedItem->row();
    if (row < 0 || row >= m_widgets.videoKeyframeTable->rowCount()) return;

    auto tableText = [this, row](int column) -> QString {
        QTableWidgetItem* item = m_widgets.videoKeyframeTable->item(row, column);
        return item ? item->text().trimmed() : QString();
    };

    bool ok = false;
    TransformKeyframeDisplay displayed;
    displayed.frame = tableText(0).toLongLong(&ok);
    if (!ok) { refresh(); return; }
    displayed.translationX = tableText(1).toDouble(&ok);
    if (!ok) { refresh(); return; }
    displayed.translationY = tableText(2).toDouble(&ok);
    if (!ok) { refresh(); return; }
    displayed.rotation = tableText(3).toDouble(&ok);
    if (!ok) { refresh(); return; }
    displayed.scaleX = tableText(4).toDouble(&ok);
    if (!ok) { refresh(); return; }
    displayed.scaleY = tableText(5).toDouble(&ok);
    if (!ok) { refresh(); return; }
    if (!parseVideoInterpolationText(tableText(6), &displayed.linearInterpolation)) {
        refresh();
        return;
    }

    displayed.frame = qBound<int64_t>(0, displayed.frame, qMax<int64_t>(0, selectedClip->durationFrames - 1));
    const TimelineClip::TransformKeyframe stored = keyframeFromInspectorDisplay(*selectedClip, displayed);
    const int64_t originalFrame = changedItem->data(Qt::UserRole).toLongLong();

    const bool updated = m_deps.updateClipById(selectedClip->id, [stored, originalFrame](TimelineClip& clip) {
        bool replaced = false;
        for (TimelineClip::TransformKeyframe& existing : clip.transformKeyframes) {
            if (existing.frame == originalFrame) {
                existing = stored;
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            clip.transformKeyframes.push_back(stored);
        }
        std::sort(clip.transformKeyframes.begin(), clip.transformKeyframes.end(),
                  [](const TimelineClip::TransformKeyframe& a, const TimelineClip::TransformKeyframe& b) {
                      return a.frame < b.frame;
                  });
        normalizeClipTransformKeyframes(clip);
    });

    if (!updated) {
        refresh();
        return;
    }

    m_selectedKeyframeFrame = stored.frame;
    m_selectedKeyframeFrames = {stored.frame};
    m_deps.setPreviewTimelineClips();
    if (m_deps.onKeyframeItemChanged) {
        m_deps.onKeyframeItemChanged(changedItem);
    }
    refresh();
    m_deps.scheduleSaveState();
    m_deps.pushHistorySnapshot();
}

void VideoKeyframeTab::onTableItemClicked(QTableWidgetItem* item)
{
    if (m_updating || !item) return;
    if (item->column() != 6) return;
    item->setText(nextVideoInterpolationLabel(item->text()));
}

void VideoKeyframeTab::onTableItemDoubleClicked(QTableWidgetItem* item)
{
    Q_UNUSED(item);
    m_deferredSeekTimer.stop();
    m_pendingSeekTimelineFrame = -1;
}

void VideoKeyframeTab::onTableHeaderClicked(int section)
{
    if (section == 4 || section == 5) {
        promptMultiplySelectedKeyframeScale(section == 4);
    }
}

void VideoKeyframeTab::onTableCustomContextMenu(const QPoint& pos)
{
    if (!m_widgets.videoKeyframeTable) return;

    int row = -1;
    QTableWidgetItem* item =
        editor::ensureContextRowSelected(m_widgets.videoKeyframeTable, pos, &row);
    if (!item) return;

    const int64_t anchorFrame = item->data(Qt::UserRole).toLongLong();
    const int64_t previousFrame = editor::rowFrameRole(m_widgets.videoKeyframeTable, row - 1);
    const int64_t nextFrame = editor::rowFrameRole(m_widgets.videoKeyframeTable, row + 1);
    const int deletableRowCount =
        editor::countSelectedFrameRoles(m_widgets.videoKeyframeTable, [](int64_t frame) { return frame > 0; });
    QMenu menu;
    QAction* addAboveAction = menu.addAction(QStringLiteral("Add Keyframe Above"));
    addAboveAction->setEnabled(previousFrame >= 0);
    QAction* addBelowAction = menu.addAction(QStringLiteral("Add Keyframe Below"));
    addBelowAction->setEnabled(nextFrame >= 0);
    QAction* copyToNextFrameAction = menu.addAction(QStringLiteral("Copy to Next Frame"));
    QAction* copyToCurrentPlayheadAction = menu.addAction(QStringLiteral("Copy to Current Playhead"));
    const TimelineClip* selectedClip = m_deps.getSelectedClip();
    const int64_t currentPlayheadFrame =
        selectedClip
            ? qBound<int64_t>(0,
                              m_deps.getCurrentTimelineFrame() - selectedClip->startFrame,
                              qMax<int64_t>(0, selectedClip->durationFrames - 1))
            : -1;
    const bool canCopyToNextFrame = selectedClip &&
                                    m_deps.clipHasVisuals(*selectedClip) &&
                                    anchorFrame >= 0 &&
                                    anchorFrame < qMax<int64_t>(0, selectedClip->durationFrames - 1);
    copyToNextFrameAction->setEnabled(canCopyToNextFrame);
    copyToCurrentPlayheadAction->setEnabled(selectedClip &&
                                            m_deps.clipHasVisuals(*selectedClip) &&
                                            currentPlayheadFrame >= 0 &&
                                            currentPlayheadFrame != anchorFrame);
    menu.addSeparator();
    QAction* deleteAction = menu.addAction(deletableRowCount == 1
                                               ? QStringLiteral("Delete Row")
                                               : QStringLiteral("Delete Rows"));
    deleteAction->setEnabled(deletableRowCount > 0);

    QAction* chosen = menu.exec(m_widgets.videoKeyframeTable->viewport()->mapToGlobal(pos));
    if (chosen == addAboveAction && addAboveAction->isEnabled()) {
        insertInterpolatedKeyframeBetween(previousFrame, anchorFrame);
    } else if (chosen == addBelowAction && addBelowAction->isEnabled()) {
        insertInterpolatedKeyframeBetween(anchorFrame, nextFrame);
    } else if (chosen == copyToNextFrameAction && copyToNextFrameAction->isEnabled()) {
        m_selectedKeyframeFrame = anchorFrame;
        m_selectedKeyframeFrames = {anchorFrame};
        duplicateSelectedKeyframes(1);
    } else if (chosen == copyToCurrentPlayheadAction && copyToCurrentPlayheadAction->isEnabled()) {
        m_selectedKeyframeFrame = anchorFrame;
        m_selectedKeyframeFrames = {anchorFrame};
        duplicateSelectedKeyframesToFrame(currentPlayheadFrame);
    } else if (chosen == deleteAction && deleteAction->isEnabled()) {
        removeSelectedKeyframes();
    }
}
