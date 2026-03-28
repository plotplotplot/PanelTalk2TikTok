#include "media_pipeline_shared.h"

#include <algorithm>
#include <QSet>

namespace {

int64_t nextValidFrameShared(int64_t currentFrame,
                             int step,
                             const QVector<ExportRangeSegment>& exportRanges) {
    int64_t nextFrame = currentFrame + step;
    if (exportRanges.isEmpty()) {
        return nextFrame;
    }

    if (step > 0) {
        for (const ExportRangeSegment& range : exportRanges) {
            if (nextFrame < range.startFrame) {
                return range.startFrame;
            }
            if (nextFrame >= range.startFrame && nextFrame <= range.endFrame) {
                return nextFrame;
            }
        }
    } else {
        for (int i = exportRanges.size() - 1; i >= 0; --i) {
            const ExportRangeSegment& range = exportRanges.at(i);
            if (nextFrame > range.endFrame) {
                return range.endFrame;
            }
            if (nextFrame >= range.startFrame && nextFrame <= range.endFrame) {
                return nextFrame;
            }
        }
    }

    return -1;
}

} // namespace

namespace editor {

bool clipIsActiveAtTimelineFrame(const TimelineClip& clip,
                                 qreal timelineFrame,
                                 bool bypassGrading) {
    if (timelineFrame < static_cast<qreal>(clip.startFrame) ||
        timelineFrame >= static_cast<qreal>(clip.startFrame + clip.durationFrames)) {
        return false;
    }
    if (bypassGrading) {
        return true;
    }
    return evaluateClipGradingAtPosition(clip, timelineFrame).opacity > 0.0001;
}

QVector<int64_t> collectLookaheadTimelineFrames(int64_t startTimelineFrame,
                                                int lookaheadFrames,
                                                int step,
                                                const QVector<ExportRangeSegment>& exportRanges) {
    QVector<int64_t> frames;
    if (lookaheadFrames <= 0 || step == 0) {
        return frames;
    }

    frames.reserve(lookaheadFrames);
    int64_t currentTimelineFrame = startTimelineFrame;
    for (int i = 0; i < lookaheadFrames; ++i) {
        currentTimelineFrame = nextValidFrameShared(currentTimelineFrame, step, exportRanges);
        if (currentTimelineFrame < 0) {
            break;
        }
        frames.push_back(currentTimelineFrame);
    }
    return frames;
}

QVector<int64_t> collectSequenceLookaheadSourceFrames(const TimelineClip& clip,
                                                      qreal startTimelineFrame,
                                                      int lookaheadFrames,
                                                      const QVector<RenderSyncMarker>& renderSyncMarkers,
                                                      bool bypassGrading) {
    QVector<int64_t> sourceFrames;
    if (!isImageSequencePath(clip.filePath) || lookaheadFrames <= 0) {
        return sourceFrames;
    }

    sourceFrames.reserve(lookaheadFrames);
    int64_t lastFrame = std::numeric_limits<int64_t>::min();
    for (int offset = 0; offset < lookaheadFrames; ++offset) {
        const qreal futureTimelineFrame = startTimelineFrame + static_cast<qreal>(offset);
        if (!clipIsActiveAtTimelineFrame(clip, futureTimelineFrame, bypassGrading)) {
            continue;
        }
        const int64_t sourceFrame =
            sourceFrameForClipAtTimelinePosition(clip, futureTimelineFrame, renderSyncMarkers);
        if (sourceFrame == lastFrame) {
            continue;
        }
        sourceFrames.push_back(sourceFrame);
        lastFrame = sourceFrame;
    }
    return sourceFrames;
}

QVector<SequencePrefetchRequest> collectSequencePrefetchRequestsAtTimelineFrame(
    const QVector<SequencePrefetchClip>& clips,
    qreal timelineFrame,
    const QVector<RenderSyncMarker>& renderSyncMarkers,
    bool bypassGrading,
    int priority) {
    QVector<SequencePrefetchClip> activeClips;
    activeClips.reserve(clips.size());
    for (const SequencePrefetchClip& candidate : clips) {
        if (candidate.decodePath.isEmpty() || !isImageSequencePath(candidate.decodePath)) {
            continue;
        }
        if (!clipIsActiveAtTimelineFrame(candidate.clip, timelineFrame, bypassGrading)) {
            continue;
        }
        activeClips.push_back(candidate);
    }

    std::sort(activeClips.begin(),
              activeClips.end(),
              [timelineFrame](const SequencePrefetchClip& a, const SequencePrefetchClip& b) {
                  const int64_t aDistance =
                      qAbs(qRound64(timelineFrame) - a.clip.startFrame);
                  const int64_t bDistance =
                      qAbs(qRound64(timelineFrame) - b.clip.startFrame);
                  if (aDistance == bDistance) {
                      return a.clip.id < b.clip.id;
                  }
                  return aDistance < bDistance;
              });

    QVector<SequencePrefetchRequest> requests;
    requests.reserve(activeClips.size());
    QSet<QString> seenKeys;
    for (const SequencePrefetchClip& candidate : activeClips) {
        const int64_t sourceFrame =
            sourceFrameForClipAtTimelinePosition(candidate.clip, timelineFrame, renderSyncMarkers);
        const QString dedupeKey =
            candidate.decodePath + QLatin1Char('|') + QString::number(sourceFrame);
        if (seenKeys.contains(dedupeKey)) {
            continue;
        }
        seenKeys.insert(dedupeKey);
        requests.push_back(SequencePrefetchRequest{
            candidate.clip.id,
            candidate.decodePath,
            qRound64(timelineFrame),
            sourceFrame,
            priority
        });
    }
    return requests;
}

} // namespace editor
