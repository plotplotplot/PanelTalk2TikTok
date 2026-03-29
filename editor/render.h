#pragma once

#include "editor_shared.h"

#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
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
    bool usingHardwareEncode = false;
    QString encoderLabel;
    qint64 elapsedMs = 0;
    qint64 estimatedRemainingMs = -1;
    qint64 renderStageMs = 0;
    qint64 renderDecodeStageMs = 0;
    qint64 renderTextureStageMs = 0;
    qint64 renderCompositeStageMs = 0;
    qint64 renderNv12StageMs = 0;
    qint64 gpuReadbackMs = 0;
    qint64 overlayStageMs = 0;
    qint64 convertStageMs = 0;
    qint64 encodeStageMs = 0;
    qint64 audioStageMs = 0;
    qint64 maxFrameRenderStageMs = 0;
    qint64 maxFrameDecodeStageMs = 0;
    qint64 maxFrameTextureStageMs = 0;
    qint64 maxFrameReadbackStageMs = 0;
    qint64 maxFrameConvertStageMs = 0;
    QImage previewFrame;
    QJsonArray skippedClips;
    QJsonObject skippedClipReasonCounts;
    QJsonObject renderStageTable;
    QJsonObject worstFrameTable;
};

struct RenderRequest {
    QString outputPath;
    QString outputFormat;
    QSize outputSize;
    bool useProxyMedia = false;
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
    bool usedHardwareEncode = false;
    QString encoderLabel;
    int64_t framesRendered = 0;
    qint64 elapsedMs = 0;
    qint64 renderStageMs = 0;
    qint64 renderDecodeStageMs = 0;
    qint64 renderTextureStageMs = 0;
    qint64 renderCompositeStageMs = 0;
    qint64 renderNv12StageMs = 0;
    qint64 gpuReadbackMs = 0;
    qint64 overlayStageMs = 0;
    qint64 convertStageMs = 0;
    qint64 encodeStageMs = 0;
    qint64 audioStageMs = 0;
    qint64 maxFrameRenderStageMs = 0;
    qint64 maxFrameDecodeStageMs = 0;
    qint64 maxFrameTextureStageMs = 0;
    qint64 maxFrameReadbackStageMs = 0;
    qint64 maxFrameConvertStageMs = 0;
    QString message;
    QJsonArray skippedClips;
    QJsonObject skippedClipReasonCounts;
    QJsonObject renderStageTable;
    QJsonObject worstFrameTable;
};

RenderResult renderTimelineToFile(const RenderRequest& request,
                                  const std::function<bool(const RenderProgress&)>& progressCallback = {});
