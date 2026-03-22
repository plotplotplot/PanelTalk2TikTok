#pragma once

#include "editor_shared.h"

#include <QImage>
#include <QSize>
#include <QString>
#include <QVector>

#include <functional>

struct RenderProgress {
    int64_t framesCompleted = 0;
    int64_t totalFrames = 0;
    int segmentIndex = 0;
    int segmentCount = 0;
    int64_t timelineFrame = 0;
    int64_t segmentStartFrame = 0;
    int64_t segmentEndFrame = 0;
    bool usingGpu = false;
    QImage previewFrame;
};

struct RenderRequest {
    QString outputPath;
    QString outputFormat;
    QSize outputSize;
    QVector<TimelineClip> clips;
    QVector<RenderSyncMarker> renderSyncMarkers;
    QVector<ExportRangeSegment> exportRanges;
    int64_t exportStartFrame = 0;
    int64_t exportEndFrame = 0;
};

struct RenderResult {
    bool success = false;
    bool cancelled = false;
    bool usedGpu = false;
    QString message;
};

RenderResult renderTimelineToFile(const RenderRequest& request,
                                  const std::function<bool(const RenderProgress&)>& progressCallback = {});
