#pragma once

#include "editor_shared.h"

#include <QContextMenuEvent>
#include <QDir>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QInputDialog>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QPoint>
#include <QRect>
#include <QString>
#include <QUrl>
#include <QWidget>
#include <QWheelEvent>
#include <QUuid>
#include <QVector>

#include <functional>

class TimelineWidget final : public QWidget {
    Q_OBJECT
public:
    explicit TimelineWidget(QWidget* parent = nullptr);

    void setCurrentFrame(int64_t frame);
    int64_t currentFrame() const { return m_currentFrame; }
    int64_t totalFrames() const;

    void addClipFromFile(const QString& filePath, int64_t startFrame = -1);

    const QVector<TimelineClip>& clips() const { return m_clips; }
    const QVector<TimelineTrack>& tracks() const { return m_tracks; }

    void setClips(const QVector<TimelineClip>& clips);
    void setTracks(const QVector<TimelineTrack>& tracks);

    QString selectedClipId() const { return m_selectedClipId; }
    const TimelineClip* selectedClip() const;
    void setSelectedClipId(const QString& clipId);

    bool updateClipById(const QString& clipId, const std::function<void(TimelineClip&)>& updater);
    bool deleteSelectedClip();
    bool splitSelectedClipAtFrame(int64_t frame);
    bool nudgeSelectedClip(int direction);

    QVector<RenderSyncMarker> renderSyncMarkers() const { return m_renderSyncMarkers; }
    void setRenderSyncMarkers(const QVector<RenderSyncMarker>& markers);
    const RenderSyncMarker* renderSyncMarkerAtFrame(const QString& clipId, int64_t frame) const;
    bool setRenderSyncMarkerAtCurrentFrame(const QString& clipId, RenderSyncAction action, int count = 1);
    bool clearRenderSyncMarkerAtCurrentFrame(const QString& clipId);

    qreal timelineZoom() const { return m_pixelsPerFrame; }
    void setTimelineZoom(qreal pixelsPerFrame);

    int verticalScrollOffset() const { return m_verticalScrollOffset; }
    void setVerticalScrollOffset(int offset);

    int64_t exportStartFrame() const { return m_exportRanges.isEmpty() ? 0 : m_exportRanges.constFirst().startFrame; }
    int64_t exportEndFrame() const { return m_exportRanges.isEmpty() ? 0 : m_exportRanges.constLast().endFrame; }
    const QVector<ExportRangeSegment>& exportRanges() const { return m_exportRanges; }
    void setExportRange(int64_t startFrame, int64_t endFrame);
    void setExportRanges(const QVector<ExportRangeSegment>& ranges);

    std::function<void(int64_t)> seekRequested;
    std::function<void()> clipsChanged;
    std::function<void()> selectionChanged;
    std::function<void()> gradingRequested;
    std::function<void()> renderSyncMarkersChanged;
    std::function<void(const QString&, const QString&)> transcribeRequested;
    std::function<void(const QString&)> createProxyRequested;
    std::function<void(const QString&)> deleteProxyRequested;
    std::function<void()> exportRangeChanged;

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void paintEvent(QPaintEvent*) override;

private:
    enum class ClipDragMode {
        None,
        Move,
        TrimLeft,
        TrimRight,
    };

    enum class ExportRangeDragMode {
        None,
        Start,
        End,
    };

    static constexpr int kDefaultTrackHeight = 44;
    static constexpr int kMinTrackHeight = 28;
    static constexpr int kTrackResizeHandleHalfHeight = 4;
    static constexpr int kTimelineOuterMargin = 16;
    static constexpr int kTimelineTopBarHeight = 52;
    static constexpr int kTimelineRulerHeight = 28;
    static constexpr int kTimelineTrackGap = 12;
    static constexpr int kTimelineClipHeight = 32;
    static constexpr int kTimelineLabelWidth = 52;
    static constexpr int kTimelineLabelGap = 12;
    static constexpr int kTimelineTrackInnerPadding = 12;
    static constexpr int kTimelineClipVerticalPadding = 6;
    static constexpr int kTimelineTrackSpacing = 10;

    void sortClips();
    void sortRenderSyncMarkers();
    int64_t snapThresholdFrames() const;
    int64_t nearestClipBoundaryFrame(const QString& excludedClipId, int64_t frame, bool* snapped = nullptr) const;
    int64_t snapMoveStartFrame(const TimelineClip& clip, int64_t proposedStartFrame, bool* snapped = nullptr,
                               int64_t* snappedBoundaryFrame = nullptr) const;
    int64_t snapTrimLeftFrame(const TimelineClip& clip, int64_t proposedStartFrame, bool* snapped = nullptr,
                              int64_t* snappedBoundaryFrame = nullptr) const;
    int64_t snapTrimRightFrame(const TimelineClip& clip, int64_t proposedEndFrame, bool* snapped = nullptr,
                               int64_t* snappedBoundaryFrame = nullptr) const;

    int clipIndexAt(const QPoint& pos) const;
    ClipDragMode clipDragModeAt(const TimelineClip& clip, const QPoint& pos) const;
    void updateHoverCursor(const QPoint& pos);
    int trackIndexAt(const QPoint& pos) const;
    int trackIndexAtY(int y, bool allowAppendTrack = false) const;
    int trackDropTargetAtY(int y, bool* insertsTrack) const;
    int trackDividerAt(const QPoint& pos) const;

    int64_t frameFromX(qreal x) const;
    int xFromFrame(int64_t frame) const;
    int widthForFrames(int64_t frames) const;
    QString timecodeForFrame(int64_t frame) const;

    int64_t mediaDurationFrames(const QFileInfo& info) const;
    bool hasFileUrls(const QMimeData* mimeData) const;
    int64_t guessDurationFrames(const QString& suffix) const;
    QColor colorForPath(const QString& path) const;
    TimelineClip buildClipFromFile(const QString& filePath, int64_t startFrame, int trackIndex) const;

    int trackCount() const;
    int nextTrackIndex() const;
    void normalizeTrackIndices();
    void ensureTrackCount(int count);
    void insertTrackAt(int trackIndex);
    QString defaultTrackName(int trackIndex) const;
    int trackTop(int trackIndex) const;
    int trackHeight(int trackIndex) const;
    int totalTrackAreaHeight() const;
    int maxVerticalScrollOffset() const;
    void updateMinimumTimelineHeight();

    QRect drawRect() const;
    QRect topBarRect() const;
    QRect rulerRect() const;
    QRect trackRect() const;
    QRect timelineContentRect() const;
    QRect exportRangeRect() const;
    QRect exportHandleRect(int segmentIndex, bool startHandle) const;
    QRect exportSegmentRect(const ExportRangeSegment& segment) const;
    QRect trackLabelRect(int trackIndex) const;
    QRect clipRectFor(const TimelineClip& clip) const;
    QRect renderSyncMarkerRect(const TimelineClip& clip, const RenderSyncMarker& marker) const;

    void normalizeExportRange();
    void normalizeExportRanges();
    int exportSegmentIndexAtFrame(int64_t frame) const;
    int exportHandleAtPos(const QPoint& pos, bool* startHandleOut) const;
    const RenderSyncMarker* renderSyncMarkerAtPos(const QPoint& pos, int* clipIndexOut = nullptr) const;
    void openRenderSyncMarkerMenu(const QPoint& globalPos, const QString& clipId);
    bool clipHasProxyAvailable(const TimelineClip& clip) const;

    QVector<TimelineClip> m_clips;
    QVector<TimelineTrack> m_tracks;
    int64_t m_currentFrame = 0;
    QVector<ExportRangeSegment> m_exportRanges = {ExportRangeSegment{0, 300}};
    int64_t m_dropFrame = -1;
    int m_draggedClipIndex = -1;
    ClipDragMode m_dragMode = ClipDragMode::None;
    int64_t m_dragOriginalStartFrame = 0;
    int64_t m_dragOriginalDurationFrames = 0;
    int64_t m_dragOriginalSourceInFrame = 0;
    QVector<TimelineClip::TransformKeyframe> m_dragOriginalTransformKeyframes;
    int64_t m_dragOffsetFrames = 0;
    int m_draggedTrackIndex = -1;
    int m_trackDropIndex = -1;
    bool m_trackDropInGap = false;
    int m_resizingTrackIndex = -1;
    int m_resizeOriginY = 0;
    int m_resizeOriginHeight = 0;
    qreal m_pixelsPerFrame = 4.0;
    int64_t m_frameOffset = 0;
    int m_verticalScrollOffset = 0;
    int64_t m_snapIndicatorFrame = -1;
    QString m_selectedClipId;
    QString m_hoveredClipId;
    QVector<RenderSyncMarker> m_renderSyncMarkers;
    ExportRangeDragMode m_exportRangeDragMode = ExportRangeDragMode::None;
    int m_exportRangeDragSegmentIndex = -1;
};
