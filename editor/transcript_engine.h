#pragma once

#include <QHash>
#include <QJsonDocument>
#include <QString>
#include <QVector>

#include "editor_shared.h"

namespace editor
{

class TranscriptEngine
{
public:
    QString transcriptPathForClip(const TimelineClip &clip) const;
    QString secondsToTranscriptTime(double seconds) const;
    bool parseTranscriptTime(const QString &text, double *secondsOut) const;
    bool saveTranscriptJson(const QString &path, const QJsonDocument &doc) const;

    int64_t adjustedLocalFrameForClip(const TimelineClip &clip,
                                      int64_t localTimelineFrame,
                                      const QVector<RenderSyncMarker> &markers) const;

    void appendMergedExportFrame(QVector<ExportRangeSegment> &ranges, int64_t frame) const;

    QVector<ExportRangeSegment> transcriptWordExportRanges(const QVector<ExportRangeSegment> &baseRanges,
                                                           const QVector<TimelineClip> &clips,
                                                           const QVector<RenderSyncMarker> &markers,
                                                           int transcriptPrependMs,
                                                           int transcriptPostpendMs) const;

    void invalidateCache();

private:
    mutable QHash<QString, QVector<ExportRangeSegment>> m_transcriptWordRangesCache;
    mutable QString m_transcriptWordRangesCacheSignature;
};

} // namespace editor
