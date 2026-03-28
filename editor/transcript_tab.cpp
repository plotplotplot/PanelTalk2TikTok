#include "transcript_tab.h"

#include "clip_serialization.h"
#include "editor_shared.h"

#include <QAbstractItemView>
#include <QColor>
#include <QFile>
#include <QFont>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSignalBlocker>
#include <cmath>

TranscriptTab::TranscriptTab(const Widgets& widgets, const Dependencies& deps, QObject* parent)
    : QObject(parent)
    , m_widgets(widgets)
    , m_deps(deps)
{
}

void TranscriptTab::wire()
{
    if (m_widgets.transcriptTable) {
        connect(m_widgets.transcriptTable, &QTableWidget::itemClicked,
                this, &TranscriptTab::onTranscriptItemClicked);
        connect(m_widgets.transcriptTable, &QTableWidget::itemChanged,
                this, &TranscriptTab::applyTableEdit);
    }
    if (m_widgets.transcriptFollowCurrentWordCheckBox) {
        connect(m_widgets.transcriptFollowCurrentWordCheckBox, &QCheckBox::toggled,
                this, &TranscriptTab::onFollowCurrentWordToggled);
    }
    if (m_widgets.transcriptOverlayEnabledCheckBox) {
        connect(m_widgets.transcriptOverlayEnabledCheckBox, &QCheckBox::toggled,
                this, &TranscriptTab::onOverlaySettingChanged);
    }
    if (m_widgets.transcriptMaxLinesSpin) {
        connect(m_widgets.transcriptMaxLinesSpin, qOverload<int>(&QSpinBox::valueChanged),
                this, &TranscriptTab::onOverlaySettingChanged);
    }
    if (m_widgets.transcriptMaxCharsSpin) {
        connect(m_widgets.transcriptMaxCharsSpin, qOverload<int>(&QSpinBox::valueChanged),
                this, &TranscriptTab::onOverlaySettingChanged);
    }
    if (m_widgets.transcriptAutoScrollCheckBox) {
        connect(m_widgets.transcriptAutoScrollCheckBox, &QCheckBox::toggled,
                this, &TranscriptTab::onOverlaySettingChanged);
    }
    if (m_widgets.transcriptOverlayXSpin) {
        connect(m_widgets.transcriptOverlayXSpin, qOverload<double>(&QDoubleSpinBox::valueChanged),
                this, &TranscriptTab::onOverlaySettingChanged);
    }
    if (m_widgets.transcriptOverlayYSpin) {
        connect(m_widgets.transcriptOverlayYSpin, qOverload<double>(&QDoubleSpinBox::valueChanged),
                this, &TranscriptTab::onOverlaySettingChanged);
    }
    if (m_widgets.transcriptOverlayWidthSpin) {
        connect(m_widgets.transcriptOverlayWidthSpin, qOverload<int>(&QSpinBox::valueChanged),
                this, &TranscriptTab::onOverlaySettingChanged);
    }
    if (m_widgets.transcriptOverlayHeightSpin) {
        connect(m_widgets.transcriptOverlayHeightSpin, qOverload<int>(&QSpinBox::valueChanged),
                this, &TranscriptTab::onOverlaySettingChanged);
    }
    if (m_widgets.transcriptFontFamilyCombo) {
        connect(m_widgets.transcriptFontFamilyCombo, &QFontComboBox::currentFontChanged,
                this, &TranscriptTab::onOverlaySettingChanged);
    }
    if (m_widgets.transcriptFontSizeSpin) {
        connect(m_widgets.transcriptFontSizeSpin, qOverload<int>(&QSpinBox::valueChanged),
                this, &TranscriptTab::onOverlaySettingChanged);
    }
    if (m_widgets.transcriptBoldCheckBox) {
        connect(m_widgets.transcriptBoldCheckBox, &QCheckBox::toggled,
                this, &TranscriptTab::onOverlaySettingChanged);
    }
    if (m_widgets.transcriptItalicCheckBox) {
        connect(m_widgets.transcriptItalicCheckBox, &QCheckBox::toggled,
                this, &TranscriptTab::onOverlaySettingChanged);
    }
    if (m_widgets.transcriptPrependMsSpin) {
        connect(m_widgets.transcriptPrependMsSpin, qOverload<int>(&QSpinBox::valueChanged),
                this, &TranscriptTab::onPrependMsChanged);
    }
    if (m_widgets.transcriptPostpendMsSpin) {
        connect(m_widgets.transcriptPostpendMsSpin, qOverload<int>(&QSpinBox::valueChanged),
                this, &TranscriptTab::onPostpendMsChanged);
    }
    if (m_widgets.speechFilterEnabledCheckBox) {
        connect(m_widgets.speechFilterEnabledCheckBox, &QCheckBox::toggled,
                this, &TranscriptTab::onSpeechFilterEnabledToggled);
    }
    if (m_widgets.speechFilterFadeSamplesSpin) {
        connect(m_widgets.speechFilterFadeSamplesSpin, qOverload<int>(&QSpinBox::valueChanged),
                this, &TranscriptTab::onSpeechFilterFadeSamplesChanged);
    }
}

void TranscriptTab::refresh()
{
    const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    m_updating = true;

    if (m_widgets.transcriptTable) {
        m_widgets.transcriptTable->clearContents();
        m_widgets.transcriptTable->setRowCount(0);
    }
    m_loadedTranscriptPath.clear();
    m_loadedTranscriptDoc = QJsonDocument();

    if (!clip || clip->mediaType != ClipMediaType::Audio) {
        if (m_widgets.transcriptInspectorClipLabel) {
            m_widgets.transcriptInspectorClipLabel->setText(QStringLiteral("No transcript selected"));
        }
        if (m_widgets.transcriptInspectorDetailsLabel) {
            m_widgets.transcriptInspectorDetailsLabel->setText(
                QStringLiteral("Select an audio clip with a WhisperX JSON transcript."));
        }
        m_updating = false;
        return;
    }

    if (m_widgets.transcriptInspectorClipLabel) {
        m_widgets.transcriptInspectorClipLabel->setText(clip->label);
    }

    updateOverlayWidgetsFromClip(*clip);
    loadTranscriptFile(*clip);
    m_updating = false;
}

void TranscriptTab::applyOverlayFromInspector(bool pushHistory)
{
    if (m_updating || !m_deps.getSelectedClip || !m_deps.updateClipById) return;

    const TimelineClip* selectedClip = m_deps.getSelectedClip();
    if (!selectedClip) return;

    const bool updated = m_deps.updateClipById(selectedClip->id, [this](TimelineClip& clip) {
        clip.transcriptOverlay.enabled = m_widgets.transcriptOverlayEnabledCheckBox &&
                                         m_widgets.transcriptOverlayEnabledCheckBox->isChecked();
        clip.transcriptOverlay.maxLines = m_widgets.transcriptMaxLinesSpin
            ? m_widgets.transcriptMaxLinesSpin->value()
            : 2;
        clip.transcriptOverlay.maxCharsPerLine = m_widgets.transcriptMaxCharsSpin
            ? m_widgets.transcriptMaxCharsSpin->value()
            : 28;
        clip.transcriptOverlay.autoScroll = m_widgets.transcriptAutoScrollCheckBox &&
                                            m_widgets.transcriptAutoScrollCheckBox->isChecked();
        clip.transcriptOverlay.translationX = m_widgets.transcriptOverlayXSpin
            ? m_widgets.transcriptOverlayXSpin->value()
            : 0.0;
        clip.transcriptOverlay.translationY = m_widgets.transcriptOverlayYSpin
            ? m_widgets.transcriptOverlayYSpin->value()
            : 640.0;
        clip.transcriptOverlay.boxWidth = m_widgets.transcriptOverlayWidthSpin
            ? m_widgets.transcriptOverlayWidthSpin->value()
            : 900.0;
        clip.transcriptOverlay.boxHeight = m_widgets.transcriptOverlayHeightSpin
            ? m_widgets.transcriptOverlayHeightSpin->value()
            : 220.0;
        clip.transcriptOverlay.fontFamily = m_widgets.transcriptFontFamilyCombo
            ? m_widgets.transcriptFontFamilyCombo->currentFont().family()
            : QStringLiteral("DejaVu Sans");
        clip.transcriptOverlay.fontPointSize = m_widgets.transcriptFontSizeSpin
            ? m_widgets.transcriptFontSizeSpin->value()
            : 42;
        clip.transcriptOverlay.bold = m_widgets.transcriptBoldCheckBox &&
                                      m_widgets.transcriptBoldCheckBox->isChecked();
        clip.transcriptOverlay.italic = m_widgets.transcriptItalicCheckBox &&
                                        m_widgets.transcriptItalicCheckBox->isChecked();
    });

    if (!updated) return;

    if (m_deps.setPreviewTimelineClips) m_deps.setPreviewTimelineClips();
    if (m_deps.refreshInspector) m_deps.refreshInspector();
    if (m_deps.scheduleSaveState) m_deps.scheduleSaveState();
    if (pushHistory && m_deps.pushHistorySnapshot) m_deps.pushHistorySnapshot();
}

void TranscriptTab::syncTableToPlayhead(int64_t absolutePlaybackSample, int64_t sourceFrame)
{
    Q_UNUSED(absolutePlaybackSample);
    if (!m_widgets.transcriptTable || m_updating) return;
    if (!m_widgets.transcriptFollowCurrentWordCheckBox ||
        !m_widgets.transcriptFollowCurrentWordCheckBox->isChecked()) {
        return;
    }

    const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    if (!clip || clip->mediaType != ClipMediaType::Audio || m_loadedTranscriptPath.isEmpty()) {
        m_widgets.transcriptTable->clearSelection();
        return;
    }

    int matchingRow = -1;
    for (int row = 0; row < m_widgets.transcriptTable->rowCount(); ++row) {
        QTableWidgetItem* startItem = m_widgets.transcriptTable->item(row, 0);
        if (!startItem) continue;
        const int64_t startFrame = startItem->data(Qt::UserRole + 2).toLongLong();
        const int64_t endFrame = startItem->data(Qt::UserRole + 3).toLongLong();
        if (sourceFrame >= startFrame && sourceFrame <= endFrame) {
            matchingRow = row;
            break;
        }
    }

    if (matchingRow < 0) {
        m_widgets.transcriptTable->clearSelection();
        return;
    }

    if (!m_widgets.transcriptTable->selectionModel()->isRowSelected(matchingRow, QModelIndex())) {
        m_widgets.transcriptTable->setCurrentCell(matchingRow, 0);
        m_widgets.transcriptTable->selectRow(matchingRow);
    }

    if ((!m_widgets.transcriptAutoScrollCheckBox || m_widgets.transcriptAutoScrollCheckBox->isChecked()) &&
        m_widgets.transcriptTable->item(matchingRow, 0)) {
        QTableWidgetItem* item = m_widgets.transcriptTable->item(matchingRow, 0);
        m_widgets.transcriptTable->scrollToItem(item, QAbstractItemView::PositionAtCenter);
    }
}

void TranscriptTab::applyTableEdit(QTableWidgetItem* item)
{
    if (m_updating || !item || m_loadedTranscriptPath.isEmpty() || !m_loadedTranscriptDoc.isObject()) {
        return;
    }

    const int segmentIndex = item->data(Qt::UserRole + 5).toInt();
    const int wordIndex = item->data(Qt::UserRole + 6).toInt();
    const bool isGap = item->data(Qt::UserRole + 4).toBool();
    if (isGap || segmentIndex < 0 || wordIndex < 0) return;

    QJsonObject root = m_loadedTranscriptDoc.object();
    QJsonArray segments = root.value(QStringLiteral("segments")).toArray();
    if (segmentIndex >= segments.size()) return;

    QJsonObject segmentObj = segments.at(segmentIndex).toObject();
    QJsonArray words = segmentObj.value(QStringLiteral("words")).toArray();
    if (wordIndex >= words.size()) return;

    QJsonObject wordObj = words.at(wordIndex).toObject();
    if (item->column() == 0 || item->column() == 1) {
        double seconds = 0.0;
        if (!m_transcriptEngine.parseTranscriptTime(item->text(), &seconds)) {
            refresh();
            return;
        }
        if (item->column() == 0) {
            const double currentEnd = wordObj.value(QStringLiteral("end")).toDouble(seconds);
            wordObj[QStringLiteral("start")] = qMin(seconds, currentEnd);
        } else {
            const double currentStart = wordObj.value(QStringLiteral("start")).toDouble(0.0);
            wordObj[QStringLiteral("end")] = qMax(seconds, currentStart);
        }
    } else if (item->column() == 2) {
        wordObj[QStringLiteral("word")] = item->text();
    }

    words.replace(wordIndex, wordObj);
    segmentObj[QStringLiteral("words")] = words;
    segments.replace(segmentIndex, segmentObj);
    root[QStringLiteral("segments")] = segments;
    m_loadedTranscriptDoc.setObject(root);

    if (!m_transcriptEngine.saveTranscriptJson(m_loadedTranscriptPath, m_loadedTranscriptDoc)) {
        refresh();
        return;
    }

    refresh();
    if (m_deps.scheduleSaveState) m_deps.scheduleSaveState();
}

void TranscriptTab::deleteSelectedRows()
{
    if (m_updating || !m_widgets.transcriptTable || m_loadedTranscriptPath.isEmpty() ||
        !m_loadedTranscriptDoc.isObject()) {
        return;
    }

    const QList<QTableWidgetSelectionRange> ranges = m_widgets.transcriptTable->selectedRanges();
    if (ranges.isEmpty()) return;

    QSet<quint64> deleteKeys;
    for (const QTableWidgetSelectionRange& range : ranges) {
        for (int row = range.topRow(); row <= range.bottomRow(); ++row) {
            QTableWidgetItem* item = m_widgets.transcriptTable->item(row, 0);
            if (!item || item->data(Qt::UserRole + 4).toBool()) continue;
            const int segmentIndex = item->data(Qt::UserRole + 5).toInt();
            const int wordIndex = item->data(Qt::UserRole + 6).toInt();
            if (segmentIndex < 0 || wordIndex < 0) continue;
            deleteKeys.insert((static_cast<quint64>(segmentIndex) << 32) |
                              static_cast<quint32>(wordIndex));
        }
    }
    if (deleteKeys.isEmpty()) return;

    QJsonObject root = m_loadedTranscriptDoc.object();
    QJsonArray segments = root.value(QStringLiteral("segments")).toArray();
    for (int segmentIndex = 0; segmentIndex < segments.size(); ++segmentIndex) {
        QJsonObject segmentObj = segments.at(segmentIndex).toObject();
        const QJsonArray words = segmentObj.value(QStringLiteral("words")).toArray();
        QJsonArray filteredWords;
        for (int wordIndex = 0; wordIndex < words.size(); ++wordIndex) {
            const quint64 key = (static_cast<quint64>(segmentIndex) << 32) |
                                static_cast<quint32>(wordIndex);
            if (!deleteKeys.contains(key)) {
                filteredWords.push_back(words.at(wordIndex));
            }
        }
        segmentObj[QStringLiteral("words")] = filteredWords;
        segments.replace(segmentIndex, segmentObj);
    }

    root[QStringLiteral("segments")] = segments;
    m_loadedTranscriptDoc.setObject(root);
    if (!m_transcriptEngine.saveTranscriptJson(m_loadedTranscriptPath, m_loadedTranscriptDoc)) {
        refresh();
        return;
    }

    refresh();
    if (m_deps.scheduleSaveState) m_deps.scheduleSaveState();
}

void TranscriptTab::onTranscriptItemClicked(QTableWidgetItem* item)
{
    if (m_updating || !item || !m_deps.getSelectedClip || !m_deps.seekToTimelineFrame) {
        return;
    }

    const TimelineClip* selectedClip = m_deps.getSelectedClip();
    if (!selectedClip || selectedClip->mediaType != ClipMediaType::Audio) {
        return;
    }

    const int64_t startFrame = item->data(Qt::UserRole + 2).toLongLong();
    const int64_t timelineFrame = qMax<int64_t>(
        selectedClip->startFrame,
        selectedClip->startFrame + (startFrame - selectedClip->sourceInFrame));
    m_deps.seekToTimelineFrame(timelineFrame);
}

void TranscriptTab::onFollowCurrentWordToggled(bool checked)
{
    Q_UNUSED(checked);
    if (m_updating) return;
    if (m_deps.scheduleSaveState) m_deps.scheduleSaveState();
    if (m_deps.refreshInspector) m_deps.refreshInspector();
}

void TranscriptTab::onOverlaySettingChanged()
{
    applyOverlayFromInspector(true);
}

void TranscriptTab::onPrependMsChanged(int value)
{
    m_transcriptPrependMs = qMax(0, value);
    refresh();
    emit speechFilterSettingsChanged();
    if (m_deps.scheduleSaveState) m_deps.scheduleSaveState();
    if (m_deps.pushHistorySnapshot) m_deps.pushHistorySnapshot();
}

void TranscriptTab::onPostpendMsChanged(int value)
{
    m_transcriptPostpendMs = qMax(0, value);
    refresh();
    emit speechFilterSettingsChanged();
    if (m_deps.scheduleSaveState) m_deps.scheduleSaveState();
    if (m_deps.pushHistorySnapshot) m_deps.pushHistorySnapshot();
}

void TranscriptTab::onSpeechFilterEnabledToggled(bool enabled)
{
    m_speechFilterEnabled = enabled;
    emit speechFilterSettingsChanged();
    if (m_deps.scheduleSaveState) m_deps.scheduleSaveState();
    if (m_deps.pushHistorySnapshot) m_deps.pushHistorySnapshot();
}

void TranscriptTab::onSpeechFilterFadeSamplesChanged(int value)
{
    m_speechFilterFadeSamples = qMax(0, value);
    emit speechFilterSettingsChanged();
    if (m_deps.scheduleSaveState) m_deps.scheduleSaveState();
    if (m_deps.pushHistorySnapshot) m_deps.pushHistorySnapshot();
}

void TranscriptTab::updateOverlayWidgetsFromClip(const TimelineClip& clip)
{
    if (!m_widgets.transcriptOverlayEnabledCheckBox) return;

    QSignalBlocker enabledBlock(m_widgets.transcriptOverlayEnabledCheckBox);
    QSignalBlocker maxLinesBlock(m_widgets.transcriptMaxLinesSpin);
    QSignalBlocker maxCharsBlock(m_widgets.transcriptMaxCharsSpin);
    QSignalBlocker autoScrollBlock(m_widgets.transcriptAutoScrollCheckBox);
    QSignalBlocker xBlock(m_widgets.transcriptOverlayXSpin);
    QSignalBlocker yBlock(m_widgets.transcriptOverlayYSpin);
    QSignalBlocker widthBlock(m_widgets.transcriptOverlayWidthSpin);
    QSignalBlocker heightBlock(m_widgets.transcriptOverlayHeightSpin);
    QSignalBlocker fontBlock(m_widgets.transcriptFontFamilyCombo);
    QSignalBlocker fontSizeBlock(m_widgets.transcriptFontSizeSpin);
    QSignalBlocker boldBlock(m_widgets.transcriptBoldCheckBox);
    QSignalBlocker italicBlock(m_widgets.transcriptItalicCheckBox);

    m_widgets.transcriptOverlayEnabledCheckBox->setChecked(clip.transcriptOverlay.enabled);
    if (m_widgets.transcriptMaxLinesSpin) {
        m_widgets.transcriptMaxLinesSpin->setValue(clip.transcriptOverlay.maxLines);
    }
    if (m_widgets.transcriptMaxCharsSpin) {
        m_widgets.transcriptMaxCharsSpin->setValue(clip.transcriptOverlay.maxCharsPerLine);
    }
    if (m_widgets.transcriptAutoScrollCheckBox) {
        m_widgets.transcriptAutoScrollCheckBox->setChecked(clip.transcriptOverlay.autoScroll);
    }
    if (m_widgets.transcriptOverlayXSpin) {
        m_widgets.transcriptOverlayXSpin->setValue(clip.transcriptOverlay.translationX);
    }
    if (m_widgets.transcriptOverlayYSpin) {
        m_widgets.transcriptOverlayYSpin->setValue(clip.transcriptOverlay.translationY);
    }
    if (m_widgets.transcriptOverlayWidthSpin) {
        m_widgets.transcriptOverlayWidthSpin->setValue(static_cast<int>(clip.transcriptOverlay.boxWidth));
    }
    if (m_widgets.transcriptOverlayHeightSpin) {
        m_widgets.transcriptOverlayHeightSpin->setValue(static_cast<int>(clip.transcriptOverlay.boxHeight));
    }
    if (m_widgets.transcriptFontFamilyCombo) {
        m_widgets.transcriptFontFamilyCombo->setCurrentFont(QFont(clip.transcriptOverlay.fontFamily));
    }
    if (m_widgets.transcriptFontSizeSpin) {
        m_widgets.transcriptFontSizeSpin->setValue(clip.transcriptOverlay.fontPointSize);
    }
    if (m_widgets.transcriptBoldCheckBox) {
        m_widgets.transcriptBoldCheckBox->setChecked(clip.transcriptOverlay.bold);
    }
    if (m_widgets.transcriptItalicCheckBox) {
        m_widgets.transcriptItalicCheckBox->setChecked(clip.transcriptOverlay.italic);
    }
}

void TranscriptTab::loadTranscriptFile(const TimelineClip& clip)
{
    QString transcriptPath;
    if (!ensureEditableTranscriptForClipFile(clip.filePath, &transcriptPath)) {
        transcriptPath = transcriptWorkingPathForClipFile(clip.filePath);
    }

    QFile transcriptFile(transcriptPath);
    if (!transcriptFile.open(QIODevice::ReadOnly)) {
        if (m_widgets.transcriptInspectorDetailsLabel) {
            m_widgets.transcriptInspectorDetailsLabel->setText(QStringLiteral("No transcript file found."));
        }
        return;
    }

    QJsonParseError parseError;
    const QJsonDocument transcriptDoc = QJsonDocument::fromJson(transcriptFile.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !transcriptDoc.isObject()) {
        if (m_widgets.transcriptInspectorDetailsLabel) {
            m_widgets.transcriptInspectorDetailsLabel->setText(QStringLiteral("Invalid transcript JSON file."));
        }
        return;
    }

    m_loadedTranscriptPath = transcriptPath;
    m_loadedTranscriptDoc = transcriptDoc;

    const QJsonArray segments = transcriptDoc.object().value(QStringLiteral("segments")).toArray();
    int totalWords = 0;
    for (const QJsonValue& segValue : segments) {
        totalWords += segValue.toObject().value(QStringLiteral("words")).toArray().size();
    }

    if (m_widgets.transcriptInspectorDetailsLabel) {
        m_widgets.transcriptInspectorDetailsLabel->setText(
            QStringLiteral("%1 words across %2 segments, showing normalized speech windows and gaps")
                .arg(totalWords)
                .arg(segments.size()));
    }

    QVector<TranscriptRow> rows = parseTranscriptRows(segments, m_transcriptPrependMs, m_transcriptPostpendMs);
    adjustOverlappingRows(rows);
    populateTable(rows);
}

QVector<TranscriptTab::TranscriptRow> TranscriptTab::parseTranscriptRows(const QJsonArray& segments,
                                                                         int prependMs,
                                                                         int postpendMs)
{
    QVector<TranscriptRow> rows;
    const double prependSeconds = prependMs / 1000.0;
    const double postpendSeconds = postpendMs / 1000.0;

    for (int segmentIndex = 0; segmentIndex < segments.size(); ++segmentIndex) {
        const QJsonArray words = segments.at(segmentIndex).toObject().value(QStringLiteral("words")).toArray();
        for (int wordIndex = 0; wordIndex < words.size(); ++wordIndex) {
            const QJsonObject word = words.at(wordIndex).toObject();
            const QString text = word.value(QStringLiteral("word")).toString();
            const double rawStartTime = word.value(QStringLiteral("start")).toDouble(-1.0);
            const double rawEndTime = word.value(QStringLiteral("end")).toDouble(-1.0);
            if (text.trimmed().isEmpty() || rawStartTime < 0.0 || rawEndTime < rawStartTime) continue;

            const double adjustedStartTime = qMax(0.0, rawStartTime - prependSeconds);
            const double adjustedEndTime = qMax(adjustedStartTime, rawEndTime + postpendSeconds);

            TranscriptRow row;
            row.startFrame = qMax<int64_t>(0, static_cast<int64_t>(std::floor(adjustedStartTime * kTimelineFps)));
            row.endFrame = qMax<int64_t>(row.startFrame,
                                         static_cast<int64_t>(std::ceil(adjustedEndTime * kTimelineFps)) - 1);
            row.text = text;
            row.segmentIndex = segmentIndex;
            row.wordIndex = wordIndex;
            rows.push_back(row);
        }
    }

    return rows;
}

void TranscriptTab::adjustOverlappingRows(QVector<TranscriptRow>& rows)
{
    for (int i = 1; i < rows.size(); ++i) {
        TranscriptRow& previous = rows[i - 1];
        TranscriptRow& current = rows[i];
        if (current.startFrame <= previous.endFrame) {
            const int64_t overlap = previous.endFrame - current.startFrame + 1;
            const int64_t trimPrevious = overlap / 2;
            const int64_t trimCurrent = overlap - trimPrevious;
            previous.endFrame = qMax(previous.startFrame, previous.endFrame - trimPrevious);
            current.startFrame = qMin(current.endFrame, current.startFrame + trimCurrent);
            if (current.startFrame <= previous.endFrame) {
                current.startFrame = qMin(current.endFrame, previous.endFrame + 1);
            }
        }
    }
}

void TranscriptTab::populateTable(const QVector<TranscriptRow>& rows)
{
    if (!m_widgets.transcriptTable) return;

    QVector<TranscriptRow> displayRows;
    displayRows.reserve(rows.size() * 2);
    for (const TranscriptRow& wordRow : rows) {
        if (!displayRows.isEmpty()) {
            const TranscriptRow& previous = displayRows.constLast();
            const int64_t gapLength = wordRow.startFrame - previous.endFrame - 1;
            if (gapLength > 1) {
                TranscriptRow gapRow;
                gapRow.startFrame = previous.endFrame + 1;
                gapRow.endFrame = wordRow.startFrame - 1;
                gapRow.text = QStringLiteral("[Gap]");
                gapRow.isGap = true;
                displayRows.push_back(gapRow);
            }
        }
        displayRows.push_back(wordRow);
    }

    m_widgets.transcriptTable->setRowCount(displayRows.size());
    const QColor gapColor(255, 248, 196);
    for (int row = 0; row < displayRows.size(); ++row) {
        const TranscriptRow& entry = displayRows.at(row);
        const double startTime = static_cast<double>(entry.startFrame) / kTimelineFps;
        const double endTime = static_cast<double>(entry.endFrame + 1) / kTimelineFps;

        auto* startItem = new QTableWidgetItem(m_transcriptEngine.secondsToTranscriptTime(startTime));
        auto* endItem = new QTableWidgetItem(m_transcriptEngine.secondsToTranscriptTime(endTime));
        auto* textItem = new QTableWidgetItem(entry.text);

        QTableWidgetItem* rowItems[] = {startItem, endItem, textItem};
        for (QTableWidgetItem* tableItem : rowItems) {
            tableItem->setData(Qt::UserRole, startTime);
            tableItem->setData(Qt::UserRole + 1, endTime);
            tableItem->setData(Qt::UserRole + 2, QVariant::fromValue(entry.startFrame));
            tableItem->setData(Qt::UserRole + 3, QVariant::fromValue(entry.endFrame));
            tableItem->setData(Qt::UserRole + 4, entry.isGap);
            tableItem->setData(Qt::UserRole + 5, entry.segmentIndex);
            tableItem->setData(Qt::UserRole + 6, entry.wordIndex);
        }

        if (entry.isGap) {
            for (QTableWidgetItem* tableItem : rowItems) {
                tableItem->setBackground(gapColor);
                tableItem->setFlags(tableItem->flags() & ~Qt::ItemIsEditable);
            }
        }

        m_widgets.transcriptTable->setItem(row, 0, startItem);
        m_widgets.transcriptTable->setItem(row, 1, endItem);
        m_widgets.transcriptTable->setItem(row, 2, textItem);
    }
}
