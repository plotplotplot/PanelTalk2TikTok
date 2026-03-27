#include "inspector_controller.h"

// NOTE:
// This file is an extracted destination for the inspector-refresh logic that
// currently lives inside EditorWindow. The original methods still reach straight
// into EditorWindow members, so the final step is to either:
//   1) add a narrow InspectorContext interface, or
//   2) let InspectorController be a friend/helper of EditorWindow.
//
// For now, the original method bodies are preserved below as comments so you
// can move them mechanically without losing anything.

InspectorController::InspectorController(EditorWindow *owner)
    : m_owner(owner)
{
}

/*
QString clipLabelForId(const QString &clipId) const
    {
        if (!m_timeline)
        {
            return clipId;
        }
        for (const TimelineClip &clip : m_timeline->clips())
        {
            if (clip.id == clipId)
            {
                return clip.label;
            }
        }
        return clipId;
    }

QColor clipColorForId(const QString &clipId) const
    {
        if (!m_timeline)
        {
            return QColor(QStringLiteral("#24303c"));
        }
        for (const TimelineClip &clip : m_timeline->clips())
        {
            if (clip.id == clipId)
            {
                return clip.color;
            }
        }
        return QColor(QStringLiteral("#24303c"));
    }

bool parseSyncActionText(const QString &text, RenderSyncAction *actionOut) const
    {
        const QString normalized = text.trimmed().toLower();
        if (normalized == QStringLiteral("duplicate") || normalized == QStringLiteral("dup"))
        {
            *actionOut = RenderSyncAction::DuplicateFrame;
            return true;
        }
        if (normalized == QStringLiteral("skip"))
        {
            *actionOut = RenderSyncAction::SkipFrame;
            return true;
        }
        return false;
    }

void refreshSyncInspector()
    {
        m_syncInspectorClipLabel->setText(QStringLiteral("Sync"));
        m_updatingSyncInspector = true;
        m_syncTable->clearContents();
        m_syncTable->setRowCount(0);
        if (!m_timeline || m_timeline->renderSyncMarkers().isEmpty())
        {
            m_syncInspectorDetailsLabel->setText(QStringLiteral("No render sync markers in the timeline."));
            m_updatingSyncInspector = false;
            return;
        }

        const QVector<RenderSyncMarker> markers = m_timeline->renderSyncMarkers();
        m_syncInspectorDetailsLabel->setText(
            QStringLiteral("%1 sync markers across the timeline. Edit Frame, Count, or Action directly.")
                .arg(markers.size()));
        m_syncTable->setRowCount(markers.size());
        for (int i = 0; i < markers.size(); ++i)
        {
            const RenderSyncMarker &marker = markers[i];
            const QColor clipColor = clipColorForId(marker.clipId);
            const QColor rowBackground = QColor(clipColor.red(), clipColor.green(), clipColor.blue(), 72);
            const QColor rowForeground = QColor(QStringLiteral("#f4f7fb"));
            const QString clipLabel = clipLabelForId(marker.clipId);
            auto *clipItem = new QTableWidgetItem(QString());
            clipItem->setData(Qt::UserRole, marker.clipId);
            clipItem->setData(Qt::UserRole + 1, QVariant::fromValue(static_cast<qint64>(marker.frame)));
            clipItem->setFlags(clipItem->flags() & ~Qt::ItemIsEditable);
            clipItem->setToolTip(clipLabel);
            auto *frameItem = new QTableWidgetItem(QString::number(marker.frame));
            auto *countItem = new QTableWidgetItem(QString::number(marker.count));
            auto *actionItem = new QTableWidgetItem(
                marker.action == RenderSyncAction::DuplicateFrame ? QStringLiteral("Duplicate")
                                                                  : QStringLiteral("Skip"));
            for (QTableWidgetItem *item : {clipItem, frameItem, countItem, actionItem})
            {
                item->setBackground(rowBackground);
                item->setForeground(rowForeground);
            }
            m_syncTable->setItem(i, 0, clipItem);
            m_syncTable->setItem(i, 1, frameItem);
            m_syncTable->setItem(i, 2, countItem);
            m_syncTable->setItem(i, 3, actionItem);
        }
        m_updatingSyncInspector = false;
    }

void refreshTranscriptInspector()
    {
        const TimelineClip *clip = m_timeline ? m_timeline->selectedClip() : nullptr;
        m_updatingTranscriptInspector = true;
        m_transcriptTable->clearContents();
        m_transcriptTable->setRowCount(0);
        m_loadedTranscriptPath.clear();
        m_loadedTranscriptDoc = QJsonDocument();

        if (!clip || clip->mediaType != ClipMediaType::Audio)
        {
            m_transcriptInspectorClipLabel->setText(QStringLiteral("No transcript selected"));
            m_transcriptInspectorDetailsLabel->setText(QStringLiteral("Select an audio clip with a WhisperX JSON transcript."));
            m_updatingTranscriptInspector = false;
            return;
        }

        const QString transcriptPath = transcriptPathForClip(*clip);
        QFile transcriptFile(transcriptPath);
        if (!transcriptFile.open(QIODevice::ReadOnly))
        {
            m_transcriptInspectorClipLabel->setText(clip->label);
            m_transcriptInspectorDetailsLabel->setText(QStringLiteral("No transcript file found."));
            m_updatingTranscriptInspector = false;
            return;
        }

        QJsonParseError parseError;
        const QJsonDocument transcriptDoc = QJsonDocument::fromJson(transcriptFile.readAll(), &parseError);
        if (parseError.error != QJsonParseError::NoError || !transcriptDoc.isObject())
        {
            m_transcriptInspectorClipLabel->setText(clip->label);
            m_transcriptInspectorDetailsLabel->setText(QStringLiteral("Invalid transcript JSON file."));
            m_updatingTranscriptInspector = false;
            return;
        }

        m_loadedTranscriptPath = transcriptPath;
        m_loadedTranscriptDoc = transcriptDoc;

        const QJsonObject root = transcriptDoc.object();
        const QJsonArray segments = root.value(QStringLiteral("segments")).toArray();
        
        // Count total words across all segments
        int totalWords = 0;
        for (const QJsonValue &segValue : segments)
        {
            const QJsonObject segment = segValue.toObject();
            const QJsonArray words = segment.value(QStringLiteral("words")).toArray();
            totalWords += words.size();
        }

        m_transcriptInspectorClipLabel->setText(clip->label);
        m_transcriptInspectorDetailsLabel->setText(
            QStringLiteral("%1 words across %2 segments").arg(totalWords).arg(segments.size()));

        // Populate table with words
        int row = 0;
        for (const QJsonValue &segValue : segments)
        {
            const QJsonObject segment = segValue.toObject();
            const QJsonArray words = segment.value(QStringLiteral("words")).toArray();
            
            for (const QJsonValue &wordValue : words)
            {
                const QJsonObject word = wordValue.toObject();
                const QString text = word.value(QStringLiteral("word")).toString();
                const double startTime = word.value(QStringLiteral("start")).toDouble();
                const double endTime = word.value(QStringLiteral("end")).toDouble();
                const int64_t startFrame = static_cast<int64_t>(startTime * kTimelineFps);
                const int64_t endFrame = static_cast<int64_t>(endTime * kTimelineFps);

                m_transcriptTable->setRowCount(row + 1);
                
                auto *startItem = new QTableWidgetItem(frameToTimecode(startFrame));
                startItem->setData(Qt::UserRole, startTime);
                startItem->setData(Qt::UserRole + 1, endTime);
                startItem->setData(Qt::UserRole + 2, QVariant::fromValue(startFrame));
                startItem->setData(Qt::UserRole + 3, QVariant::fromValue(endFrame));
                startItem->setFlags(startItem->flags() & ~Qt::ItemIsEditable);
                
                auto *endItem = new QTableWidgetItem(frameToTimecode(endFrame));
                endItem->setFlags(endItem->flags() & ~Qt::ItemIsEditable);
                
                auto *textItem = new QTableWidgetItem(text);
                textItem->setFlags(textItem->flags() & ~Qt::ItemIsEditable);
                
                m_transcriptTable->setItem(row, 0, startItem);
                m_transcriptTable->setItem(row, 1, endItem);
                m_transcriptTable->setItem(row, 2, textItem);
                
                row++;
            }
        }
        m_updatingTranscriptInspector = false;
    }
*/
