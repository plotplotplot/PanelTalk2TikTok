#include "titles_tab.h"
#include "keyframe_table_shared.h"

#include <QHeaderView>
#include <QMenu>
#include <QSignalBlocker>

TitlesTab::TitlesTab(const Widgets &widgets, const Dependencies &deps, QObject *parent)
    : QObject(parent)
    , m_widgets(widgets)
    , m_deps(deps)
{
}

void TitlesTab::wire()
{
    auto *table = m_widgets.titleKeyframeTable;
    if (table) {
        table->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(table, &QTableWidget::itemChanged, this, &TitlesTab::onTableItemChanged);
        connect(table, &QTableWidget::itemSelectionChanged, this, &TitlesTab::onTableSelectionChanged);
        connect(table, &QTableWidget::itemClicked, this, &TitlesTab::onTableItemClicked);
    }

    auto connectSpin = [this](QDoubleSpinBox *spin) {
        if (!spin) return;
        connect(spin, &QDoubleSpinBox::editingFinished, this, &TitlesTab::applyKeyframeFromInspector);
    };
    connectSpin(m_widgets.titleXSpin);
    connectSpin(m_widgets.titleYSpin);
    connectSpin(m_widgets.titleFontSizeSpin);
    connectSpin(m_widgets.titleOpacitySpin);

    if (m_widgets.titleTextEdit) {
        connect(m_widgets.titleTextEdit, &QLineEdit::editingFinished, this, &TitlesTab::applyKeyframeFromInspector);
    }
    if (m_widgets.titleFontCombo) {
        connect(m_widgets.titleFontCombo, &QFontComboBox::currentFontChanged, this, [this]() {
            if (!m_updating) applyKeyframeFromInspector();
        });
    }
    if (m_widgets.titleBoldCheck) {
        connect(m_widgets.titleBoldCheck, &QCheckBox::toggled, this, [this]() {
            if (!m_updating) applyKeyframeFromInspector();
        });
    }
    if (m_widgets.titleItalicCheck) {
        connect(m_widgets.titleItalicCheck, &QCheckBox::toggled, this, [this]() {
            if (!m_updating) applyKeyframeFromInspector();
        });
    }

    if (m_widgets.addTitleKeyframeButton) {
        connect(m_widgets.addTitleKeyframeButton, &QPushButton::clicked, this, &TitlesTab::upsertKeyframeAtPlayhead);
    }
    if (m_widgets.removeTitleKeyframeButton) {
        connect(m_widgets.removeTitleKeyframeButton, &QPushButton::clicked, this, &TitlesTab::removeSelectedKeyframes);
    }
    if (m_widgets.centerHorizontalButton) {
        connect(m_widgets.centerHorizontalButton, &QPushButton::clicked, this, &TitlesTab::centerHorizontal);
    }
    if (m_widgets.centerVerticalButton) {
        connect(m_widgets.centerVerticalButton, &QPushButton::clicked, this, &TitlesTab::centerVertical);
    }
}

void TitlesTab::refresh()
{
    m_updating = true;
    auto *table = m_widgets.titleKeyframeTable;

    const TimelineClip *clip = m_deps.getSelectedClipConst ? m_deps.getSelectedClipConst() : nullptr;

    if (m_widgets.titlesInspectorClipLabel) {
        m_widgets.titlesInspectorClipLabel->setText(
            clip ? clip->label : QStringLiteral("No clip selected"));
    }

    if (!clip || !m_deps.clipHasVisuals || !m_deps.clipHasVisuals(*clip)) {
        if (table) {
            const QSignalBlocker blocker(table);
            table->clearContents();
            table->setRowCount(0);
        }
        TitleKeyframeDisplay defaults;
        updateWidgetsFromKeyframe(defaults);
        m_selectedKeyframeFrame = -1;
        m_selectedKeyframeFrames.clear();
        m_updating = false;
        return;
    }

    if (table) {
        const QSignalBlocker blocker(table);
        populateTable(*clip);
    }

    // Determine active keyframe
    const int64_t currentTimelineFrame = m_deps.getCurrentTimelineFrame ? m_deps.getCurrentTimelineFrame() : 0;
    const int64_t localFrame = qMax<int64_t>(0, currentTimelineFrame - clip->startFrame);

    if (m_selectedKeyframeFrame < 0 || m_selectedKeyframeFrames.isEmpty()) {
        const int nearest = nearestKeyframeIndex(*clip, localFrame);
        if (nearest >= 0 && nearest < clip->titleKeyframes.size()) {
            m_selectedKeyframeFrame = clip->titleKeyframes[nearest].frame;
            m_selectedKeyframeFrames = {m_selectedKeyframeFrame};
        }
    }

    const TitleKeyframeDisplay displayed = evaluateDisplayedTitle(*clip, localFrame);
    updateWidgetsFromKeyframe(displayed);

    if (table && !m_selectedKeyframeFrames.isEmpty()) {
        const QSignalBlocker blocker(table);
        editor::restoreSelectionByFrameRole(table, m_selectedKeyframeFrames);
    }

    if (m_widgets.titlesInspectorDetailsLabel) {
        m_widgets.titlesInspectorDetailsLabel->setText(
            QStringLiteral("%1 title keyframes").arg(clip->titleKeyframes.size()));
    }

    m_updating = false;
    syncTableToPlayhead();
}

void TitlesTab::populateTable(const TimelineClip &clip)
{
    auto *table = m_widgets.titleKeyframeTable;
    if (!table) return;

    table->clearContents();
    table->setRowCount(clip.titleKeyframes.size());

    for (int row = 0; row < clip.titleKeyframes.size(); ++row) {
        const auto &kf = clip.titleKeyframes[row];
        auto setCell = [&](int col, const QString &text) {
            auto *item = new QTableWidgetItem(text);
            item->setData(Qt::UserRole, QVariant::fromValue(static_cast<qint64>(kf.frame)));
            table->setItem(row, col, item);
        };
        setCell(0, QString::number(kf.frame));
        setCell(1, kf.text);
        setCell(2, QString::number(kf.translationX, 'f', 1));
        setCell(3, QString::number(kf.translationY, 'f', 1));
        setCell(4, QString::number(kf.fontSize, 'f', 1));
        setCell(5, QString::number(kf.opacity, 'f', 2));

        auto *interpItem = new QTableWidgetItem(kf.linearInterpolation
            ? QStringLiteral("Linear") : QStringLiteral("Step"));
        interpItem->setData(Qt::UserRole, QVariant::fromValue(static_cast<qint64>(kf.frame)));
        interpItem->setFlags(interpItem->flags() & ~Qt::ItemIsEditable);
        table->setItem(row, 6, interpItem);
    }
}

TitlesTab::TitleKeyframeDisplay TitlesTab::evaluateDisplayedTitle(
    const TimelineClip &clip, int64_t localFrame) const
{
    TitleKeyframeDisplay display;
    if (clip.titleKeyframes.isEmpty()) return display;

    // Find the keyframe at or before localFrame
    int bestIdx = 0;
    for (int i = 0; i < clip.titleKeyframes.size(); ++i) {
        if (clip.titleKeyframes[i].frame <= localFrame) {
            bestIdx = i;
        }
    }

    const auto &kf = clip.titleKeyframes[bestIdx];
    display.frame = kf.frame;
    display.text = kf.text;
    display.translationX = kf.translationX;
    display.translationY = kf.translationY;
    display.fontSize = kf.fontSize;
    display.opacity = kf.opacity;
    display.fontFamily = kf.fontFamily;
    display.bold = kf.bold;
    display.italic = kf.italic;
    display.linearInterpolation = kf.linearInterpolation;
    return display;
}

void TitlesTab::updateWidgetsFromKeyframe(const TitleKeyframeDisplay &display)
{
    auto blockAndSet = [](QDoubleSpinBox *spin, double val) {
        if (!spin) return;
        const QSignalBlocker b(spin);
        spin->setValue(val);
    };
    blockAndSet(m_widgets.titleXSpin, display.translationX);
    blockAndSet(m_widgets.titleYSpin, display.translationY);
    blockAndSet(m_widgets.titleFontSizeSpin, display.fontSize);
    blockAndSet(m_widgets.titleOpacitySpin, display.opacity);

    if (m_widgets.titleTextEdit) {
        const QSignalBlocker b(m_widgets.titleTextEdit);
        m_widgets.titleTextEdit->setText(display.text);
    }
    if (m_widgets.titleFontCombo) {
        const QSignalBlocker b(m_widgets.titleFontCombo);
        m_widgets.titleFontCombo->setCurrentFont(QFont(display.fontFamily));
    }
    if (m_widgets.titleBoldCheck) {
        const QSignalBlocker b(m_widgets.titleBoldCheck);
        m_widgets.titleBoldCheck->setChecked(display.bold);
    }
    if (m_widgets.titleItalicCheck) {
        const QSignalBlocker b(m_widgets.titleItalicCheck);
        m_widgets.titleItalicCheck->setChecked(display.italic);
    }
}

void TitlesTab::applyKeyframeFromInspector()
{
    if (m_updating) return;
    const QString clipId = m_deps.getSelectedClipId ? m_deps.getSelectedClipId() : QString();
    if (clipId.isEmpty() || m_selectedKeyframeFrame < 0) return;

    const int64_t targetFrame = m_selectedKeyframeFrame;
    const QString text = m_widgets.titleTextEdit ? m_widgets.titleTextEdit->text() : QString();
    const double x = m_widgets.titleXSpin ? m_widgets.titleXSpin->value() : 0.0;
    const double y = m_widgets.titleYSpin ? m_widgets.titleYSpin->value() : 0.0;
    const double fontSize = m_widgets.titleFontSizeSpin ? m_widgets.titleFontSizeSpin->value() : 48.0;
    const double opacity = m_widgets.titleOpacitySpin ? m_widgets.titleOpacitySpin->value() : 1.0;
    const QString fontFamily = m_widgets.titleFontCombo
        ? m_widgets.titleFontCombo->currentFont().family() : kDefaultFontFamily;
    const bool bold = m_widgets.titleBoldCheck ? m_widgets.titleBoldCheck->isChecked() : true;
    const bool italic = m_widgets.titleItalicCheck ? m_widgets.titleItalicCheck->isChecked() : false;

    if (m_deps.updateClipById) {
        m_deps.updateClipById(clipId, [&](TimelineClip &clip) {
            for (auto &kf : clip.titleKeyframes) {
                if (kf.frame == targetFrame) {
                    kf.text = text;
                    kf.translationX = x;
                    kf.translationY = y;
                    kf.fontSize = fontSize;
                    kf.opacity = opacity;
                    kf.fontFamily = fontFamily;
                    kf.bold = bold;
                    kf.italic = italic;
                    break;
                }
            }
            normalizeClipTitleKeyframes(clip);
        });
    }
    if (m_deps.setPreviewTimelineClips) m_deps.setPreviewTimelineClips();
    if (m_deps.refreshInspector) m_deps.refreshInspector();
    if (m_deps.scheduleSaveState) m_deps.scheduleSaveState();
    if (m_deps.pushHistorySnapshot) m_deps.pushHistorySnapshot();
}

void TitlesTab::upsertKeyframeAtPlayhead()
{
    const QString clipId = m_deps.getSelectedClipId ? m_deps.getSelectedClipId() : QString();
    if (clipId.isEmpty()) return;

    const int64_t currentFrame = m_deps.getCurrentTimelineFrame ? m_deps.getCurrentTimelineFrame() : 0;
    const int64_t clipStart = m_deps.getSelectedClipStartFrame ? m_deps.getSelectedClipStartFrame() : 0;
    const int64_t localFrame = qMax<int64_t>(0, currentFrame - clipStart);

    if (m_deps.updateClipById) {
        m_deps.updateClipById(clipId, [&](TimelineClip &clip) {
            const int64_t clampedFrame = qBound<int64_t>(0, localFrame, qMax<int64_t>(0, clip.durationFrames - 1));
            for (auto &kf : clip.titleKeyframes) {
                if (kf.frame == clampedFrame) {
                    m_selectedKeyframeFrame = clampedFrame;
                    m_selectedKeyframeFrames = {clampedFrame};
                    return;
                }
            }
            TimelineClip::TitleKeyframe newKf;
            newKf.frame = clampedFrame;
            if (m_widgets.titleTextEdit)
                newKf.text = m_widgets.titleTextEdit->text();
            if (m_widgets.titleXSpin)
                newKf.translationX = m_widgets.titleXSpin->value();
            if (m_widgets.titleYSpin)
                newKf.translationY = m_widgets.titleYSpin->value();
            if (m_widgets.titleFontSizeSpin)
                newKf.fontSize = m_widgets.titleFontSizeSpin->value();
            if (m_widgets.titleOpacitySpin)
                newKf.opacity = m_widgets.titleOpacitySpin->value();
            if (m_widgets.titleFontCombo)
                newKf.fontFamily = m_widgets.titleFontCombo->currentFont().family();
            if (m_widgets.titleBoldCheck)
                newKf.bold = m_widgets.titleBoldCheck->isChecked();
            if (m_widgets.titleItalicCheck)
                newKf.italic = m_widgets.titleItalicCheck->isChecked();
            clip.titleKeyframes.push_back(newKf);
            normalizeClipTitleKeyframes(clip);
            m_selectedKeyframeFrame = clampedFrame;
            m_selectedKeyframeFrames = {clampedFrame};
        });
    }
    if (m_deps.setPreviewTimelineClips) m_deps.setPreviewTimelineClips();
    if (m_deps.refreshInspector) m_deps.refreshInspector();
    if (m_deps.scheduleSaveState) m_deps.scheduleSaveState();
    if (m_deps.pushHistorySnapshot) m_deps.pushHistorySnapshot();
}

void TitlesTab::removeSelectedKeyframes()
{
    if (!hasRemovableKeyframeSelection()) return;
    const QString clipId = m_deps.getSelectedClipId ? m_deps.getSelectedClipId() : QString();
    if (clipId.isEmpty()) return;

    const QSet<int64_t> framesToRemove = m_selectedKeyframeFrames;

    if (m_deps.updateClipById) {
        m_deps.updateClipById(clipId, [&](TimelineClip &clip) {
            clip.titleKeyframes.erase(
                std::remove_if(clip.titleKeyframes.begin(), clip.titleKeyframes.end(),
                    [&](const TimelineClip::TitleKeyframe &kf) {
                        return framesToRemove.contains(kf.frame);
                    }),
                clip.titleKeyframes.end());
            normalizeClipTitleKeyframes(clip);
        });
    }
    m_selectedKeyframeFrame = -1;
    m_selectedKeyframeFrames.clear();
    if (m_deps.setPreviewTimelineClips) m_deps.setPreviewTimelineClips();
    if (m_deps.refreshInspector) m_deps.refreshInspector();
    if (m_deps.scheduleSaveState) m_deps.scheduleSaveState();
    if (m_deps.pushHistorySnapshot) m_deps.pushHistorySnapshot();
}

void TitlesTab::centerHorizontal()
{
    if (m_updating || m_selectedKeyframeFrame < 0) return;
    const QString clipId = m_deps.getSelectedClipId ? m_deps.getSelectedClipId() : QString();
    if (clipId.isEmpty()) return;

    const int64_t targetFrame = m_selectedKeyframeFrame;
    if (m_deps.updateClipById) {
        m_deps.updateClipById(clipId, [targetFrame](TimelineClip &clip) {
            for (auto &kf : clip.titleKeyframes) {
                if (kf.frame == targetFrame) {
                    kf.translationX = 0.0;
                    break;
                }
            }
        });
    }
    if (m_widgets.titleXSpin) {
        const QSignalBlocker b(m_widgets.titleXSpin);
        m_widgets.titleXSpin->setValue(0.0);
    }
    if (m_deps.setPreviewTimelineClips) m_deps.setPreviewTimelineClips();
    if (m_deps.refreshInspector) m_deps.refreshInspector();
    if (m_deps.scheduleSaveState) m_deps.scheduleSaveState();
    if (m_deps.pushHistorySnapshot) m_deps.pushHistorySnapshot();
}

void TitlesTab::centerVertical()
{
    if (m_updating || m_selectedKeyframeFrame < 0) return;
    const QString clipId = m_deps.getSelectedClipId ? m_deps.getSelectedClipId() : QString();
    if (clipId.isEmpty()) return;

    const int64_t targetFrame = m_selectedKeyframeFrame;
    if (m_deps.updateClipById) {
        m_deps.updateClipById(clipId, [targetFrame](TimelineClip &clip) {
            for (auto &kf : clip.titleKeyframes) {
                if (kf.frame == targetFrame) {
                    kf.translationY = 0.0;
                    break;
                }
            }
        });
    }
    if (m_widgets.titleYSpin) {
        const QSignalBlocker b(m_widgets.titleYSpin);
        m_widgets.titleYSpin->setValue(0.0);
    }
    if (m_deps.setPreviewTimelineClips) m_deps.setPreviewTimelineClips();
    if (m_deps.refreshInspector) m_deps.refreshInspector();
    if (m_deps.scheduleSaveState) m_deps.scheduleSaveState();
    if (m_deps.pushHistorySnapshot) m_deps.pushHistorySnapshot();
}

void TitlesTab::syncTableToPlayhead()
{
    auto *table = m_widgets.titleKeyframeTable;
    if (!table || m_updating) return;
    if (m_widgets.titleAutoScrollCheck && !m_widgets.titleAutoScrollCheck->isChecked()) return;

    const TimelineClip *clip = m_deps.getSelectedClipConst ? m_deps.getSelectedClipConst() : nullptr;
    if (!clip || clip->titleKeyframes.isEmpty()) return;

    const int64_t currentFrame = m_deps.getCurrentTimelineFrame ? m_deps.getCurrentTimelineFrame() : 0;
    const int64_t localFrame = qMax<int64_t>(0, currentFrame - clip->startFrame);

    int bestRow = 0;
    for (int row = 0; row < table->rowCount(); ++row) {
        const int64_t rowFrame = editor::rowFrameRole(table, row);
        if (rowFrame <= localFrame) {
            bestRow = row;
        }
    }
    table->scrollToItem(table->item(bestRow, 0), QAbstractItemView::PositionAtCenter);
}

int TitlesTab::selectedKeyframeIndex(const TimelineClip &clip) const
{
    if (m_selectedKeyframeFrame < 0) return -1;
    for (int i = 0; i < clip.titleKeyframes.size(); ++i) {
        if (clip.titleKeyframes[i].frame == m_selectedKeyframeFrame) return i;
    }
    return -1;
}

int TitlesTab::nearestKeyframeIndex(const TimelineClip &clip, int64_t localFrame) const
{
    if (clip.titleKeyframes.isEmpty()) return -1;
    int best = 0;
    int64_t bestDist = std::abs(clip.titleKeyframes[0].frame - localFrame);
    for (int i = 1; i < clip.titleKeyframes.size(); ++i) {
        const int64_t dist = std::abs(clip.titleKeyframes[i].frame - localFrame);
        if (dist < bestDist) {
            bestDist = dist;
            best = i;
        }
    }
    return best;
}

bool TitlesTab::hasRemovableKeyframeSelection() const
{
    return !m_selectedKeyframeFrames.isEmpty();
}

void TitlesTab::onTableItemChanged(QTableWidgetItem *item)
{
    if (m_updating || !item) return;
    auto *table = m_widgets.titleKeyframeTable;
    if (!table) return;

    const int row = item->row();
    const int64_t originalFrame = editor::rowFrameRole(table, row);
    const QString clipId = m_deps.getSelectedClipId ? m_deps.getSelectedClipId() : QString();
    if (clipId.isEmpty()) return;

    bool ok = false;
    const int64_t newFrame = table->item(row, 0) ? table->item(row, 0)->text().toLongLong(&ok) : originalFrame;
    if (!ok) { refresh(); return; }

    const QString text = table->item(row, 1) ? table->item(row, 1)->text() : QString();
    const double x = table->item(row, 2) ? table->item(row, 2)->text().toDouble() : 0.0;
    const double y = table->item(row, 3) ? table->item(row, 3)->text().toDouble() : 0.0;
    const double fontSize = table->item(row, 4) ? table->item(row, 4)->text().toDouble() : 48.0;
    const double opacity = table->item(row, 5) ? table->item(row, 5)->text().toDouble() : 1.0;

    if (m_deps.updateClipById) {
        m_deps.updateClipById(clipId, [&](TimelineClip &clip) {
            for (auto &kf : clip.titleKeyframes) {
                if (kf.frame == originalFrame) {
                    kf.frame = newFrame;
                    kf.text = text;
                    kf.translationX = x;
                    kf.translationY = y;
                    kf.fontSize = fontSize;
                    kf.opacity = opacity;
                    break;
                }
            }
            normalizeClipTitleKeyframes(clip);
        });
    }
    m_selectedKeyframeFrame = newFrame;
    m_selectedKeyframeFrames = {newFrame};
    if (m_deps.setPreviewTimelineClips) m_deps.setPreviewTimelineClips();
    if (m_deps.refreshInspector) m_deps.refreshInspector();
    if (m_deps.scheduleSaveState) m_deps.scheduleSaveState();
    if (m_deps.pushHistorySnapshot) m_deps.pushHistorySnapshot();
}

void TitlesTab::onTableSelectionChanged()
{
    if (m_updating || m_syncingTableSelection) return;
    auto *table = m_widgets.titleKeyframeTable;
    if (!table) return;

    m_selectedKeyframeFrames = editor::collectSelectedFrameRoles(table);
    m_selectedKeyframeFrame = editor::primarySelectedFrameRole(table);

    const TimelineClip *clip = m_deps.getSelectedClipConst ? m_deps.getSelectedClipConst() : nullptr;
    if (clip && m_selectedKeyframeFrame >= 0) {
        const int idx = selectedKeyframeIndex(*clip);
        if (idx >= 0) {
            m_updating = true;
            TitleKeyframeDisplay display;
            const auto &kf = clip->titleKeyframes[idx];
            display.frame = kf.frame;
            display.text = kf.text;
            display.translationX = kf.translationX;
            display.translationY = kf.translationY;
            display.fontSize = kf.fontSize;
            display.opacity = kf.opacity;
            display.fontFamily = kf.fontFamily;
            display.bold = kf.bold;
            display.italic = kf.italic;
            display.linearInterpolation = kf.linearInterpolation;
            updateWidgetsFromKeyframe(display);
            m_updating = false;
        }
    }

    if (m_deps.onKeyframeSelectionChanged) m_deps.onKeyframeSelectionChanged();
}

void TitlesTab::onTableItemClicked(QTableWidgetItem *item)
{
    if (!item || m_updating) return;
    // Column 6 = Interp toggle
    if (item->column() == 6) {
        const bool isLinear = item->text() == QStringLiteral("Linear");
        item->setText(isLinear ? QStringLiteral("Step") : QStringLiteral("Linear"));

        const int64_t frame = editor::rowFrameRole(m_widgets.titleKeyframeTable, item->row());
        const QString clipId = m_deps.getSelectedClipId ? m_deps.getSelectedClipId() : QString();
        if (!clipId.isEmpty() && m_deps.updateClipById) {
            m_deps.updateClipById(clipId, [&](TimelineClip &clip) {
                for (auto &kf : clip.titleKeyframes) {
                    if (kf.frame == frame) {
                        kf.linearInterpolation = !isLinear;
                        break;
                    }
                }
            });
        }
        if (m_deps.setPreviewTimelineClips) m_deps.setPreviewTimelineClips();
        if (m_deps.scheduleSaveState) m_deps.scheduleSaveState();
        if (m_deps.pushHistorySnapshot) m_deps.pushHistorySnapshot();
    }
}
