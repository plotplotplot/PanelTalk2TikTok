#pragma once

#include "editor_shared.h"

#include <QVector>

namespace editor {

struct SequencePrefetchClip {
    TimelineClip clip;
    QString decodePath;
};

struct SequencePrefetchRequest {
    QString clipId;
    QString decodePath;
    int64_t timelineFrame = -1;
    int64_t sourceFrame = -1;
    int priority = 0;
};

bool clipIsActiveAtTimelineFrame(const TimelineClip& clip,
                                 qreal timelineFrame,
                                 bool bypassGrading);

QVector<int64_t> collectLookaheadTimelineFrames(int64_t startTimelineFrame,
                                                int lookaheadFrames,
                                                int step,
                                                const QVector<ExportRangeSegment>& exportRanges);

QVector<int64_t> collectSequenceLookaheadSourceFrames(const TimelineClip& clip,
                                                      qreal startTimelineFrame,
                                                      int lookaheadFrames,
                                                      const QVector<RenderSyncMarker>& renderSyncMarkers,
                                                      bool bypassGrading);

QVector<SequencePrefetchRequest> collectSequencePrefetchRequestsAtTimelineFrame(
    const QVector<SequencePrefetchClip>& clips,
    qreal timelineFrame,
    const QVector<RenderSyncMarker>& renderSyncMarkers,
    bool bypassGrading,
    int priority);

} // namespace editor
