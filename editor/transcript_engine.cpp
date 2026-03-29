#include "transcript_engine.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QSaveFile>

#include <algorithm>
#include <cmath>

namespace editor
{

QString TranscriptEngine::transcriptPathForClip(const TimelineClip &clip) const
    {
        return transcriptWorkingPathForClipFile(clip.filePath);
    }

QString TranscriptEngine::secondsToTranscriptTime(double seconds) const
    {
        const qint64 millis = qMax<qint64>(0, qRound64(seconds * 1000.0));
        const qint64 totalSeconds = millis / 1000;
        const qint64 minutes = totalSeconds / 60;
        const qint64 secs = totalSeconds % 60;
        const qint64 ms = millis % 1000;
        return QStringLiteral("%1:%2.%3")
            .arg(minutes, 2, 10, QLatin1Char('0'))
            .arg(secs, 2, 10, QLatin1Char('0'))
            .arg(ms, 3, 10, QLatin1Char('0'));
    }

bool TranscriptEngine::parseTranscriptTime(const QString &text, double *secondsOut) const
    {
        bool ok = false;
        const double numericValue = text.toDouble(&ok);
        if (ok)
        {
            *secondsOut = qMax(0.0, numericValue);
            return true;
        }

        const QString trimmed = text.trimmed();
        const QStringList minuteParts = trimmed.split(QLatin1Char(':'));
        if (minuteParts.size() != 2)
        {
            return false;
        }
        const int minutes = minuteParts[0].toInt(&ok);
        if (!ok)
        {
            return false;
        }
        const double secValue = minuteParts[1].toDouble(&ok);
        if (!ok)
        {
            return false;
        }
        *secondsOut = qMax(0.0, minutes * 60.0 + secValue);
        return true;
    }

bool TranscriptEngine::saveTranscriptJson(const QString &path, const QJsonDocument &doc) const
    {
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        {
            return false;
        }
        const QByteArray payload = doc.toJson(QJsonDocument::Indented);
        if (file.write(payload) != payload.size())
        {
            file.cancelWriting();
            return false;
        }
        return file.commit();
    }

int64_t TranscriptEngine::adjustedLocalFrameForClip(const TimelineClip &clip,
                                      int64_t localTimelineFrame,
                                      const QVector<RenderSyncMarker> &markers) const
    {
        int64_t adjustedLocalFrame = qMax<int64_t>(0, localTimelineFrame);
        int duplicateCarry = 0;
        for (const RenderSyncMarker &marker : markers)
        {
            if (marker.clipId != clip.id)
            {
                continue;
            }
            const int64_t markerLocalFrame = marker.frame - clip.startFrame;
            if (markerLocalFrame < 0 || markerLocalFrame >= localTimelineFrame)
            {
                continue;
            }
            if (duplicateCarry > 0)
            {
                adjustedLocalFrame -= 1;
                duplicateCarry -= 1;
                continue;
            }
            if (marker.action == RenderSyncAction::DuplicateFrame)
            {
                adjustedLocalFrame -= 1;
                duplicateCarry = qMax(0, marker.count - 1);
            }
            else if (marker.action == RenderSyncAction::SkipFrame)
            {
                adjustedLocalFrame += marker.count;
            }
        }
        return adjustedLocalFrame;
    }

void TranscriptEngine::appendMergedExportFrame(QVector<ExportRangeSegment> &ranges, int64_t frame) const
    {
        if (ranges.isEmpty() || frame > ranges.constLast().endFrame + 1)
        {
            ranges.push_back(ExportRangeSegment{frame, frame});
            return;
        }
        ranges.last().endFrame = qMax(ranges.last().endFrame, frame);
    }

QVector<ExportRangeSegment> TranscriptEngine::transcriptWordExportRanges(const QVector<ExportRangeSegment> &baseRanges,
                                                           const QVector<TimelineClip> &clips,
                                                           const QVector<RenderSyncMarker> &markers,
                                                           int transcriptPrependMs,
                                                           int transcriptPostpendMs) const
    {
        QString cacheSignature;
        cacheSignature.reserve(256);
        cacheSignature += QStringLiteral("pre=%1|post=%2|").arg(transcriptPrependMs).arg(transcriptPostpendMs);
        for (const ExportRangeSegment &range : baseRanges)
        {
            cacheSignature += QStringLiteral("base:%1-%2|").arg(range.startFrame).arg(range.endFrame);
        }
        for (const RenderSyncMarker &marker : markers)
        {
            cacheSignature += QStringLiteral("marker:%1:%2:%3:%4|")
                                  .arg(marker.clipId)
                                  .arg(marker.frame)
                                  .arg(static_cast<int>(marker.action))
                                  .arg(marker.count);
        }
        for (const TimelineClip &clip : clips)
        {
            const QFileInfo transcriptInfo(transcriptPathForClip(clip));
            cacheSignature += QStringLiteral("clip:%1:%2:%3:%4:%5:%6:%7|")
                                  .arg(clip.id)
                                  .arg(clip.startFrame)
                                  .arg(clip.durationFrames)
                                  .arg(clip.sourceInFrame)
                                  .arg(clip.sourceDurationFrames)
                                  .arg(clip.filePath)
                                  .arg(transcriptInfo.exists() ? transcriptInfo.lastModified().toMSecsSinceEpoch() : -1);
        }

        if (m_transcriptWordRangesCacheSignature == cacheSignature && !m_transcriptWordRangesCache.isEmpty())
        {
            QVector<ExportRangeSegment> result;
            for (auto it = m_transcriptWordRangesCache.constBegin(); it != m_transcriptWordRangesCache.constEnd(); ++it)
            {
                result.append(it.value());
            }
            std::sort(result.begin(), result.end(),
                      [](const ExportRangeSegment &a, const ExportRangeSegment &b)
                      {
                          return a.startFrame < b.startFrame;
                      });
            QVector<ExportRangeSegment> merged;
            for (const auto &range : result)
            {
                if (merged.isEmpty() || range.startFrame > merged.last().endFrame + 1)
                {
                    merged.append(range);
                }
                else
                {
                    merged.last().endFrame = qMax(merged.last().endFrame, range.endFrame);
                }
            }
            return merged;
        }

        m_transcriptWordRangesCache.clear();
        m_transcriptWordRangesCacheSignature = cacheSignature;

        QVector<ExportRangeSegment> resolvedBaseRanges = baseRanges;
        if (resolvedBaseRanges.isEmpty())
        {
            int64_t endFrame = 0;
            for (const TimelineClip &clip : clips)
            {
                endFrame = qMax(endFrame, clip.startFrame + clip.durationFrames);
            }
            resolvedBaseRanges.push_back(ExportRangeSegment{0, endFrame});
        }

        QVector<ExportRangeSegment> allTranscriptRanges;

        for (const TimelineClip &clip : clips)
        {
            if (clip.durationFrames <= 0)
            {
                continue;
            }

            QFile transcriptFile(transcriptPathForClip(clip));
            if (!transcriptFile.open(QIODevice::ReadOnly))
            {
                continue;
            }

            QJsonParseError parseError;
            const QJsonDocument transcriptDoc =
                QJsonDocument::fromJson(transcriptFile.readAll(), &parseError);
            if (parseError.error != QJsonParseError::NoError || !transcriptDoc.isObject())
            {
                continue;
            }

            // Build source word ranges from transcript
            QVector<ExportRangeSegment> sourceWordRanges;
            const QJsonArray segments = transcriptDoc.object().value(QStringLiteral("segments")).toArray();
            for (const QJsonValue &segmentValue : segments)
            {
                const QJsonArray words = segmentValue.toObject().value(QStringLiteral("words")).toArray();
                for (const QJsonValue &wordValue : words)
                {
                    const QJsonObject wordObj = wordValue.toObject();
                    if (wordObj.value(QStringLiteral("word")).toString().trimmed().isEmpty())
                    {
                        continue;
                    }

                    double startSeconds = wordObj.value(QStringLiteral("start")).toDouble(-1.0);
                    const double endSeconds = wordObj.value(QStringLiteral("end")).toDouble(-1.0);
                    if (startSeconds < 0.0 || endSeconds < startSeconds)
                    {
                        continue;
                    }

                    const double prependSeconds = transcriptPrependMs / 1000.0;
                    const double postpendSeconds = transcriptPostpendMs / 1000.0;
                    startSeconds = qMax(0.0, startSeconds - prependSeconds);
                    const double adjustedEndSeconds = qMax(startSeconds, endSeconds + postpendSeconds);

                    const int64_t startFrame =
                        qMax<int64_t>(0, static_cast<int64_t>(std::floor(startSeconds * kTimelineFps)));
                    const int64_t endFrame =
                        qMax<int64_t>(startFrame, static_cast<int64_t>(std::ceil(adjustedEndSeconds * kTimelineFps)) - 1);
                    sourceWordRanges.push_back(ExportRangeSegment{startFrame, endFrame});
                }
            }

            if (sourceWordRanges.isEmpty())
            {
                continue;
            }

            // Sort and merge overlapping word ranges at source level
            std::sort(sourceWordRanges.begin(), sourceWordRanges.end(),
                      [](const ExportRangeSegment &a, const ExportRangeSegment &b)
                      {
                          if (a.startFrame == b.startFrame)
                          {
                              return a.endFrame < b.endFrame;
                          }
                          return a.startFrame < b.startFrame;
                      });
            QVector<ExportRangeSegment> mergedSourceWordRanges;
            for (const ExportRangeSegment &wordRange : std::as_const(sourceWordRanges))
            {
                if (mergedSourceWordRanges.isEmpty() ||
                    wordRange.startFrame > mergedSourceWordRanges.constLast().endFrame + 1)
                {
                    mergedSourceWordRanges.push_back(wordRange);
                }
                else
                {
                    mergedSourceWordRanges.last().endFrame =
                        qMax(mergedSourceWordRanges.last().endFrame, wordRange.endFrame);
                }
            }

            QVector<ExportRangeSegment> clipTimelineRanges;

            for (const ExportRangeSegment &baseRange : resolvedBaseRanges)
            {
                const int64_t clipStart = qMax<int64_t>(clip.startFrame, baseRange.startFrame);
                const int64_t clipEnd =
                    qMin<int64_t>(clip.startFrame + clip.durationFrames - 1, baseRange.endFrame);
                if (clipEnd < clipStart)
                {
                    continue;
                }

                for (int64_t timelineFrame = clipStart; timelineFrame <= clipEnd; ++timelineFrame)
                {
                    const int64_t localTimelineFrame = timelineFrame - clip.startFrame;
                    if (localTimelineFrame < 0 || localTimelineFrame >= clip.durationFrames)
                    {
                        continue;
                    }
                    const int64_t adjustedLocalFrame = adjustedLocalFrameForClip(clip, localTimelineFrame, markers);
                    const int64_t sourceFrame = clip.sourceInFrame + adjustedLocalFrame;
                    bool inWord = false;
                    for (const ExportRangeSegment &wordRange : std::as_const(mergedSourceWordRanges))
                    {
                        if (sourceFrame >= wordRange.startFrame && sourceFrame <= wordRange.endFrame)
                        {
                            inWord = true;
                            break;
                        }
                    }
                    if (inWord)
                    {
                        appendMergedExportFrame(clipTimelineRanges, timelineFrame);
                    }
                }
            }

            // Cache ranges for this clip
            if (!clipTimelineRanges.isEmpty())
            {
                m_transcriptWordRangesCache[clip.id] = clipTimelineRanges;
                allTranscriptRanges.append(clipTimelineRanges);
            }
        }

        // Sort and merge all ranges from all clips
        std::sort(allTranscriptRanges.begin(), allTranscriptRanges.end(),
                  [](const ExportRangeSegment &a, const ExportRangeSegment &b)
                  {
                      return a.startFrame < b.startFrame;
                  });
        QVector<ExportRangeSegment> merged;
        for (const auto &range : allTranscriptRanges)
        {
            if (merged.isEmpty() || range.startFrame > merged.last().endFrame + 1)
            {
                merged.append(range);
            }
            else
            {
                merged.last().endFrame = qMax(merged.last().endFrame, range.endFrame);
            }
        }
        return merged;
    }

void TranscriptEngine::invalidateCache()
{
    m_transcriptWordRangesCache.clear();
    m_transcriptWordRangesCacheSignature.clear();
}

} // namespace editor
