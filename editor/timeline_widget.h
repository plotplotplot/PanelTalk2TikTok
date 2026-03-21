#pragma once

#include "editor_shared.h"

#include <QDir>
#include <QFileInfo>
#include <QContextMenuEvent>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QInputDialog>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QLineEdit>
#include <QUrl>
#include <QWidget>
#include <QWheelEvent>

#include <QUuid>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

// ============================================================================
// TimelineWidget - Timeline editing widget
// ============================================================================
class TimelineWidget final : public QWidget {
    Q_OBJECT
public:
    explicit TimelineWidget(QWidget* parent = nullptr) : QWidget(parent) {
        setAcceptDrops(true);
        setMinimumHeight(150);
        setMouseTracking(true);
        setAutoFillBackground(true);
        
        QPalette pal = palette();
        pal.setColor(QPalette::Window, QColor(QStringLiteral("#0f1216")));
        setPalette(pal);
        ensureTrackCount(1);
        updateMinimumTimelineHeight();
    }
    
    void setCurrentFrame(int64_t frame) {
        m_currentFrame = qMax<int64_t>(0, frame);
        update();
    }
    
    int64_t currentFrame() const { return m_currentFrame; }
    
    int64_t totalFrames() const {
        int64_t lastFrame = 300;
        for (const TimelineClip& clip : m_clips) {
            lastFrame = qMax(lastFrame, clip.startFrame + clip.durationFrames + 30);
        }
        return lastFrame;
    }
    
    void addClipFromFile(const QString& filePath, int64_t startFrame = -1);
    
    const QVector<TimelineClip>& clips() const { return m_clips; }
    const QVector<TimelineTrack>& tracks() const { return m_tracks; }

    void setClips(const QVector<TimelineClip>& clips) {
        m_clips = clips;
        for (TimelineClip& clip : m_clips) {
            normalizeClipTiming(clip);
        }
        normalizeTrackIndices();
        sortClips();
        ensureTrackCount(trackCount());
        if (!selectedClip()) {
            m_selectedClipId.clear();
        }
        updateMinimumTimelineHeight();
        update();
    }

    void setTracks(const QVector<TimelineTrack>& tracks) {
        m_tracks = tracks;
        for (int i = 0; i < m_tracks.size(); ++i) {
            if (m_tracks[i].name.trimmed().isEmpty()) {
                m_tracks[i].name = defaultTrackName(i);
            }
            m_tracks[i].height = qMax(kMinTrackHeight, m_tracks[i].height);
        }
        ensureTrackCount(trackCount());
        updateMinimumTimelineHeight();
        update();
    }

    QString selectedClipId() const { return m_selectedClipId; }

    const TimelineClip* selectedClip() const {
        for (const TimelineClip& clip : m_clips) {
            if (clip.id == m_selectedClipId) {
                return &clip;
            }
        }
        return nullptr;
    }

    void setSelectedClipId(const QString& clipId) {
        if (m_selectedClipId == clipId) {
            return;
        }
        m_selectedClipId = clipId;
        if (selectionChanged) {
            selectionChanged();
        }
        update();
    }

    bool updateClipById(const QString& clipId, const std::function<void(TimelineClip&)>& updater) {
        for (TimelineClip& clip : m_clips) {
            if (clip.id == clipId) {
                updater(clip);
                normalizeClipTiming(clip);
                update();
                return true;
            }
        }
        return false;
    }

    bool deleteSelectedClip() {
        if (m_selectedClipId.isEmpty()) {
            return false;
        }

        for (int i = 0; i < m_clips.size(); ++i) {
            if (m_clips[i].id != m_selectedClipId) {
                continue;
            }

            m_clips.removeAt(i);
            m_selectedClipId.clear();
            normalizeTrackIndices();
            sortClips();
            if (selectionChanged) {
                selectionChanged();
            }
            if (clipsChanged) {
                clipsChanged();
            }
            update();
            return true;
        }

        m_selectedClipId.clear();
        if (selectionChanged) {
            selectionChanged();
        }
        update();
        return false;
    }

    bool splitSelectedClipAtFrame(int64_t frame) {
        if (m_selectedClipId.isEmpty()) {
            return false;
        }

        for (int i = 0; i < m_clips.size(); ++i) {
            TimelineClip& clip = m_clips[i];
            if (clip.id != m_selectedClipId) {
                continue;
            }
            if (frame <= clip.startFrame || frame >= clip.startFrame + clip.durationFrames) {
                return false;
            }

            const int64_t leftDuration = frame - clip.startFrame;
            const int64_t rightDuration = clip.durationFrames - leftDuration;
            if (leftDuration <= 0 || rightDuration <= 0) {
                return false;
            }

            const bool isImage = clip.mediaType == ClipMediaType::Image;
            TimelineClip rightClip = clip;
            rightClip.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            rightClip.startFrame = frame;
            rightClip.sourceInFrame = isImage ? 0 : (clip.sourceInFrame + leftDuration);
            rightClip.startSubframeSamples = 0;
            rightClip.sourceInSubframeSamples = 0;
            rightClip.durationFrames = rightDuration;
            if (isImage) {
                rightClip.sourceDurationFrames = rightDuration;
            }
            rightClip.transformKeyframes.clear();
            for (const TimelineClip::TransformKeyframe& keyframe : clip.transformKeyframes) {
                if (keyframe.frame >= leftDuration) {
                    TimelineClip::TransformKeyframe shifted = keyframe;
                    shifted.frame -= leftDuration;
                    rightClip.transformKeyframes.push_back(shifted);
                }
            }

            QVector<TimelineClip::TransformKeyframe> leftKeyframes;
            for (const TimelineClip::TransformKeyframe& keyframe : clip.transformKeyframes) {
                if (keyframe.frame < leftDuration) {
                    leftKeyframes.push_back(keyframe);
                }
            }
            clip.transformKeyframes = leftKeyframes;

            clip.durationFrames = leftDuration;
            if (isImage) {
                clip.sourceInFrame = 0;
                clip.sourceDurationFrames = leftDuration;
            }
            clip.startSubframeSamples = 0;
            clip.sourceInSubframeSamples = 0;
            normalizeClipTransformKeyframes(clip);
            normalizeClipTransformKeyframes(rightClip);
            normalizeClipTiming(clip);
            normalizeClipTiming(rightClip);
            m_clips.insert(i + 1, rightClip);
            m_selectedClipId = rightClip.id;
            sortClips();
            if (selectionChanged) {
                selectionChanged();
            }
            if (clipsChanged) {
                clipsChanged();
            }
            update();
            return true;
        }

        return false;
    }
    
    std::function<void(int64_t)> seekRequested;
    std::function<void()> clipsChanged;
    std::function<void()> selectionChanged;
    std::function<void()> gradingRequested;
    std::function<void()> renderSyncMarkersChanged;
    bool nudgeSelectedClip(int direction);
    QVector<RenderSyncMarker> renderSyncMarkers() const { return m_renderSyncMarkers; }
    void setRenderSyncMarkers(const QVector<RenderSyncMarker>& markers);
    const RenderSyncMarker* renderSyncMarkerAtFrame(int64_t frame) const;
    bool setRenderSyncMarkerAtCurrentFrame(RenderSyncAction action);
    bool clearRenderSyncMarkerAtCurrentFrame();

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
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

    static constexpr int kDefaultTrackHeight = 44;
    static constexpr int kMinTrackHeight = 28;
    static constexpr int kTrackResizeHandleHalfHeight = 4;

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
    void updateMinimumTimelineHeight();
    QRect drawRect() const;
    QRect rulerRect() const;
    QRect trackRect() const;
    QRect timelineContentRect() const;
    QRect trackLabelRect(int trackIndex) const;
    QRect clipRectFor(const TimelineClip& clip) const;
    
    QVector<TimelineClip> m_clips;
    QVector<TimelineTrack> m_tracks;
    int64_t m_currentFrame = 0;
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
    int64_t m_snapIndicatorFrame = -1;
    QString m_selectedClipId;
    QVector<RenderSyncMarker> m_renderSyncMarkers;
};

namespace {
constexpr int kTimelineOuterMargin = 16;
constexpr int kTimelineRulerHeight = 28;
constexpr int kTimelineTrackGap = 12;
constexpr int kTimelineClipHeight = 32;
constexpr int kTimelineLabelWidth = 52;
constexpr int kTimelineLabelGap = 12;
constexpr int kTimelineTrackInnerPadding = 12;
constexpr int kTimelineClipVerticalPadding = 6;
constexpr int kTimelineTrackSpacing = 10;
}

inline void TimelineWidget::sortClips() {
    std::sort(m_clips.begin(), m_clips.end(), 
        [](const TimelineClip& a, const TimelineClip& b) {
            if (a.trackIndex == b.trackIndex) {
                const int64_t aStartSamples = clipTimelineStartSamples(a);
                const int64_t bStartSamples = clipTimelineStartSamples(b);
                if (aStartSamples == bStartSamples) {
                    return a.label < b.label;
                }
                return aStartSamples < bStartSamples;
            }
            return a.trackIndex < b.trackIndex;
        });
}

inline void TimelineWidget::sortRenderSyncMarkers() {
    std::sort(m_renderSyncMarkers.begin(), m_renderSyncMarkers.end(),
              [](const RenderSyncMarker& a, const RenderSyncMarker& b) {
                  if (a.frame == b.frame) {
                      return static_cast<int>(a.action) < static_cast<int>(b.action);
                  }
                  return a.frame < b.frame;
              });
}

inline void TimelineWidget::setRenderSyncMarkers(const QVector<RenderSyncMarker>& markers) {
    m_renderSyncMarkers = markers;
    sortRenderSyncMarkers();
    update();
}

inline const RenderSyncMarker* TimelineWidget::renderSyncMarkerAtFrame(int64_t frame) const {
    for (const RenderSyncMarker& marker : m_renderSyncMarkers) {
        if (marker.frame == frame) {
            return &marker;
        }
    }
    return nullptr;
}

inline bool TimelineWidget::setRenderSyncMarkerAtCurrentFrame(RenderSyncAction action) {
    for (RenderSyncMarker& marker : m_renderSyncMarkers) {
        if (marker.frame == m_currentFrame) {
            if (marker.action == action) {
                return false;
            }
            marker.action = action;
            sortRenderSyncMarkers();
            if (renderSyncMarkersChanged) {
                renderSyncMarkersChanged();
            }
            update();
            return true;
        }
    }

    RenderSyncMarker marker;
    marker.frame = m_currentFrame;
    marker.action = action;
    m_renderSyncMarkers.push_back(marker);
    sortRenderSyncMarkers();
    if (renderSyncMarkersChanged) {
        renderSyncMarkersChanged();
    }
    update();
    return true;
}

inline bool TimelineWidget::clearRenderSyncMarkerAtCurrentFrame() {
    for (int i = 0; i < m_renderSyncMarkers.size(); ++i) {
        if (m_renderSyncMarkers[i].frame == m_currentFrame) {
            m_renderSyncMarkers.removeAt(i);
            if (renderSyncMarkersChanged) {
                renderSyncMarkersChanged();
            }
            update();
            return true;
        }
    }
    return false;
}

inline int64_t TimelineWidget::snapThresholdFrames() const {
    static constexpr qreal kSnapThresholdPixels = 10.0;
    return qMax<int64_t>(1, qRound64(kSnapThresholdPixels / qMax<qreal>(0.25, m_pixelsPerFrame)));
}

inline int64_t TimelineWidget::nearestClipBoundaryFrame(const QString& excludedClipId, int64_t frame, bool* snapped) const {
    const int64_t threshold = snapThresholdFrames();
    int64_t bestFrame = frame;
    int64_t bestDistance = threshold + 1;

    auto consider = [&](int64_t candidate) {
        const int64_t distance = qAbs(candidate - frame);
        if (distance <= threshold && distance < bestDistance) {
            bestDistance = distance;
            bestFrame = candidate;
        }
    };

    consider(0);
    for (const TimelineClip& clip : m_clips) {
        if (clip.id == excludedClipId) {
            continue;
        }
        consider(clip.startFrame);
        consider(clip.startFrame + clip.durationFrames);
    }

    if (snapped) {
        *snapped = bestDistance <= threshold;
    }
    return bestFrame;
}

inline int64_t TimelineWidget::snapMoveStartFrame(const TimelineClip& clip,
                                           int64_t proposedStartFrame,
                                           bool* snapped,
                                           int64_t* snappedBoundaryFrame) const {
    const int64_t proposedEndFrame = proposedStartFrame + clip.durationFrames;
    const int64_t threshold = snapThresholdFrames();
    int64_t bestStartFrame = proposedStartFrame;
    int64_t bestBoundary = -1;
    int64_t bestDistance = threshold + 1;

    auto consider = [&](int64_t boundaryFrame) {
        const int64_t startDistance = qAbs(boundaryFrame - proposedStartFrame);
        if (startDistance <= threshold && startDistance < bestDistance) {
            bestDistance = startDistance;
            bestStartFrame = boundaryFrame;
            bestBoundary = boundaryFrame;
        }

        const int64_t endDistance = qAbs(boundaryFrame - proposedEndFrame);
        if (endDistance <= threshold && endDistance < bestDistance) {
            bestDistance = endDistance;
            bestStartFrame = qMax<int64_t>(0, boundaryFrame - clip.durationFrames);
            bestBoundary = boundaryFrame;
        }
    };

    consider(0);
    for (const TimelineClip& other : m_clips) {
        if (other.id == clip.id) {
            continue;
        }
        consider(other.startFrame);
        consider(other.startFrame + other.durationFrames);
    }

    if (snapped) {
        *snapped = bestDistance <= threshold;
    }
    if (snappedBoundaryFrame) {
        *snappedBoundaryFrame = bestBoundary;
    }
    return bestStartFrame;
}

inline int64_t TimelineWidget::snapTrimLeftFrame(const TimelineClip& clip,
                                          int64_t proposedStartFrame,
                                          bool* snapped,
                                          int64_t* snappedBoundaryFrame) const {
    Q_UNUSED(clip)
    const int64_t snappedFrame = nearestClipBoundaryFrame(clip.id, proposedStartFrame, snapped);
    if (snappedBoundaryFrame) {
        *snappedBoundaryFrame = (snapped && *snapped) ? snappedFrame : -1;
    }
    return snappedFrame;
}

inline int64_t TimelineWidget::snapTrimRightFrame(const TimelineClip& clip,
                                           int64_t proposedEndFrame,
                                           bool* snapped,
                                           int64_t* snappedBoundaryFrame) const {
    Q_UNUSED(clip)
    const int64_t snappedFrame = nearestClipBoundaryFrame(clip.id, proposedEndFrame, snapped);
    if (snappedBoundaryFrame) {
        *snappedBoundaryFrame = (snapped && *snapped) ? snappedFrame : -1;
    }
    return snappedFrame;
}

inline bool TimelineWidget::nudgeSelectedClip(int direction) {
    if (direction == 0 || m_selectedClipId.isEmpty()) {
        return false;
    }

    for (TimelineClip& clip : m_clips) {
        if (clip.id != m_selectedClipId) {
            continue;
        }

        if (clipIsAudioOnly(clip)) {
            const int64_t nextStartSamples =
                qMax<int64_t>(0, clipTimelineStartSamples(clip) + (direction * kAudioNudgeSamples));
            clip.startFrame = nextStartSamples / kSamplesPerFrame;
            clip.startSubframeSamples = nextStartSamples % kSamplesPerFrame;
        } else {
            clip.startFrame = qMax<int64_t>(0, clip.startFrame + direction);
        }

        normalizeClipTiming(clip);
        m_currentFrame = clip.startFrame;
        sortClips();
        if (clipsChanged) {
            clipsChanged();
        }
        update();
        return true;
    }

    return false;
}

inline int TimelineWidget::trackCount() const {
    int maxTrack = -1;
    for (const TimelineClip& clip : m_clips) {
        maxTrack = qMax(maxTrack, clip.trackIndex);
    }
    return qMax(1, qMax(m_tracks.size(), maxTrack + 1));
}

inline int TimelineWidget::nextTrackIndex() const {
    return trackCount();
}

inline void TimelineWidget::normalizeTrackIndices() {
    int maxTrackIndex = -1;
    for (const TimelineClip& clip : m_clips) {
        maxTrackIndex = qMax(maxTrackIndex, clip.trackIndex);
    }

    for (TimelineClip& clip : m_clips) {
        clip.trackIndex = qMax(0, clip.trackIndex);
    }

    ensureTrackCount(qMax(1, qMax(m_tracks.size(), maxTrackIndex + 1)));
    updateMinimumTimelineHeight();
}

inline void TimelineWidget::ensureTrackCount(int count) {
    const int desired = qMax(1, count);
    while (m_tracks.size() < desired) {
        TimelineTrack track;
        track.name = defaultTrackName(m_tracks.size());
        track.height = kDefaultTrackHeight;
        m_tracks.push_back(track);
    }
    for (int i = 0; i < m_tracks.size(); ++i) {
        if (m_tracks[i].name.trimmed().isEmpty()) {
            m_tracks[i].name = defaultTrackName(i);
        }
        m_tracks[i].height = qMax(kMinTrackHeight, m_tracks[i].height);
    }
}

inline QString TimelineWidget::defaultTrackName(int trackIndex) const {
    return QStringLiteral("Track %1").arg(trackIndex + 1);
}

inline int TimelineWidget::trackTop(int trackIndex) const {
    const QRect tracks = trackRect();
    int y = tracks.top() + kTimelineTrackInnerPadding;
    for (int i = 0; i < trackIndex && i < m_tracks.size(); ++i) {
        y += trackHeight(i) + kTimelineTrackSpacing;
    }
    return y;
}

inline int TimelineWidget::trackHeight(int trackIndex) const {
    if (trackIndex >= 0 && trackIndex < m_tracks.size()) {
        return qMax(kMinTrackHeight, m_tracks[trackIndex].height);
    }
    return kDefaultTrackHeight;
}

inline int TimelineWidget::trackIndexAtY(int y, bool allowAppendTrack) const {
    if (m_tracks.isEmpty()) {
        return 0;
    }

    for (int i = 0; i < trackCount(); ++i) {
        const int top = trackTop(i);
        const int bottom = top + trackHeight(i);
        if (y >= top && y < bottom) {
            return i;
        }
    }

    const int lastTrackBottom = trackTop(trackCount() - 1) + trackHeight(trackCount() - 1);
    if (allowAppendTrack && y >= lastTrackBottom) {
        return trackCount();
    }
    if (y < trackTop(0)) {
        return 0;
    }
    return trackCount() - 1;
}

inline int TimelineWidget::trackDropTargetAtY(int y, bool* insertsTrack) const {
    if (insertsTrack) {
        *insertsTrack = false;
    }
    if (m_tracks.isEmpty()) {
        if (insertsTrack) {
            *insertsTrack = true;
        }
        return 0;
    }

    const int firstTop = trackTop(0);
    if (y < firstTop) {
        if (insertsTrack) {
            *insertsTrack = true;
        }
        return 0;
    }

    for (int i = 0; i < trackCount(); ++i) {
        const int top = trackTop(i);
        const int bottom = top + trackHeight(i);
        if (y >= top && y < bottom) {
            return i;
        }
        const int nextTop = (i + 1 < trackCount()) ? trackTop(i + 1) : bottom + kTimelineTrackSpacing;
        if (y >= bottom && y < nextTop) {
            if (insertsTrack) {
                *insertsTrack = true;
            }
            return i + 1;
        }
    }

    if (insertsTrack) {
        *insertsTrack = true;
    }
    return trackCount();
}

inline int TimelineWidget::trackDividerAt(const QPoint& pos) const {
    const QRect tracks = trackRect();
    if (!tracks.contains(pos)) {
        return -1;
    }

    for (int i = 0; i < trackCount(); ++i) {
        const int dividerY = trackTop(i) + trackHeight(i);
        if (std::abs(pos.y() - dividerY) <= kTrackResizeHandleHalfHeight) {
            return i;
        }
    }
    return -1;
}

inline void TimelineWidget::updateMinimumTimelineHeight() {
    int trackAreaHeight = kTimelineTrackInnerPadding * 2;
    for (int i = 0; i < trackCount(); ++i) {
        trackAreaHeight += trackHeight(i);
    }
    trackAreaHeight += qMax(0, trackCount() - 1) * kTimelineTrackSpacing;
    const int minimum = (kTimelineOuterMargin * 2) + kTimelineRulerHeight + kTimelineTrackGap + trackAreaHeight;
    setMinimumHeight(qMax(150, minimum));
}

inline void TimelineWidget::insertTrackAt(int trackIndex) {
    const int insertAt = qBound(0, trackIndex, trackCount());
    ensureTrackCount(trackCount());
    for (TimelineClip& clip : m_clips) {
        if (clip.trackIndex >= insertAt) {
            clip.trackIndex += 1;
        }
    }
    TimelineTrack track;
    track.name = defaultTrackName(insertAt);
    track.height = kDefaultTrackHeight;
    m_tracks.insert(insertAt, track);
    for (int i = insertAt + 1; i < m_tracks.size(); ++i) {
        if (m_tracks[i].name.startsWith(QStringLiteral("Track "))) {
            m_tracks[i].name = defaultTrackName(i);
        }
    }
    updateMinimumTimelineHeight();
}

inline QRect TimelineWidget::drawRect() const {
    return rect().adjusted(kTimelineOuterMargin, kTimelineOuterMargin,
                           -kTimelineOuterMargin, -kTimelineOuterMargin);
}

inline QRect TimelineWidget::rulerRect() const {
    const QRect draw = drawRect();
    return QRect(draw.left(), draw.top(), draw.width(), kTimelineRulerHeight);
}

inline QRect TimelineWidget::trackRect() const {
    const QRect draw = drawRect();
    const QRect ruler = rulerRect();
    return QRect(draw.left(), ruler.bottom() + kTimelineTrackGap, draw.width(),
                 draw.height() - (kTimelineRulerHeight + kTimelineTrackGap));
}

inline QRect TimelineWidget::timelineContentRect() const {
    const QRect tracks = trackRect();
    return QRect(tracks.left() + kTimelineLabelWidth + kTimelineLabelGap,
                 tracks.top(),
                 qMax(0, tracks.width() - kTimelineLabelWidth - kTimelineLabelGap),
                 tracks.height());
}

inline QRect TimelineWidget::trackLabelRect(int trackIndex) const {
    return QRect(trackRect().left() + 6,
                 trackTop(trackIndex) + qMax(0, (trackHeight(trackIndex) - kTimelineClipHeight) / 2),
                 kTimelineLabelWidth - 12,
                 kTimelineClipHeight);
}

inline QRect TimelineWidget::clipRectFor(const TimelineClip& clip) const {
    const qreal clipFrame = samplesToFramePosition(clipTimelineStartSamples(clip));
    const qreal visibleFrame = clipFrame - static_cast<qreal>(m_frameOffset);
    const int clipX = timelineContentRect().left() + qRound(visibleFrame * m_pixelsPerFrame);
    const int clipW = qMax(40, widthForFrames(clip.durationFrames));
    const int visualHeight =
        qMax(kTimelineClipHeight, trackHeight(clip.trackIndex) - (kTimelineClipVerticalPadding * 2));
    const int clipY = trackTop(clip.trackIndex) + qMax(0, (trackHeight(clip.trackIndex) - visualHeight) / 2);
    return QRect(clipX, clipY, clipW, visualHeight);
}

inline int TimelineWidget::trackIndexAt(const QPoint& pos) const {
    for (int i = 0; i < trackCount(); ++i) {
        if (trackLabelRect(i).contains(pos)) {
            return i;
        }
    }
    return -1;
}

inline int TimelineWidget::clipIndexAt(const QPoint& pos) const {
    for (int i = 0; i < m_clips.size(); ++i) {
        if (clipRectFor(m_clips[i]).contains(pos)) {
            return i;
        }
    }
    return -1;
}

inline TimelineWidget::ClipDragMode TimelineWidget::clipDragModeAt(const TimelineClip& clip, const QPoint& pos) const {
    const QRect rect = clipRectFor(clip);
    if (!rect.contains(pos)) {
        return ClipDragMode::None;
    }
    const int edgeThreshold = qMin(10, qMax(4, rect.width() / 5));
    if (pos.x() <= rect.left() + edgeThreshold) {
        return ClipDragMode::TrimLeft;
    }
    if (pos.x() >= rect.right() - edgeThreshold) {
        return ClipDragMode::TrimRight;
    }
    return ClipDragMode::Move;
}

inline void TimelineWidget::updateHoverCursor(const QPoint& pos) {
    if (trackDividerAt(pos) >= 0) {
        setCursor(Qt::SizeVerCursor);
        return;
    }
    const int clipIndex = clipIndexAt(pos);
    if (clipIndex < 0) {
        unsetCursor();
        return;
    }
    const ClipDragMode mode = clipDragModeAt(m_clips[clipIndex], pos);
    if (mode == ClipDragMode::TrimLeft || mode == ClipDragMode::TrimRight) {
        setCursor(Qt::SizeHorCursor);
        return;
    }
    setCursor(Qt::ArrowCursor);
}

inline int64_t TimelineWidget::frameFromX(qreal x) const {
    const int left = timelineContentRect().left();
    const qreal normalized = qMax<qreal>(0.0, x - left);
    return m_frameOffset + static_cast<int64_t>(normalized / m_pixelsPerFrame);
}

inline int TimelineWidget::xFromFrame(int64_t frame) const {
    return timelineContentRect().left() + widthForFrames(frame - m_frameOffset);
}

inline int TimelineWidget::widthForFrames(int64_t frames) const {
    return static_cast<int>(frames * m_pixelsPerFrame);
}

inline QString TimelineWidget::timecodeForFrame(int64_t frame) const {
    const int64_t seconds = frame / kTimelineFps;
    const int64_t minutes = seconds / 60;
    const int64_t secs = seconds % 60;
    return QStringLiteral("%1:%2")
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(secs, 2, 10, QLatin1Char('0'));
}

inline int64_t TimelineWidget::mediaDurationFrames(const QFileInfo& info) const {
    const QString suffix = info.suffix().toLower();
    return probeMediaFile(info.absoluteFilePath(), guessDurationFrames(suffix)).durationFrames;
}

inline bool TimelineWidget::hasFileUrls(const QMimeData* mimeData) const {
    if (!mimeData || !mimeData->hasUrls()) return false;
    for (const QUrl& url : mimeData->urls()) {
        if (url.isLocalFile()) return true;
    }
    return false;
}

inline int64_t TimelineWidget::guessDurationFrames(const QString& suffix) const {
    static const QHash<QString, int64_t> kDurations = {
        {QStringLiteral("mp4"), 180},
        {QStringLiteral("mov"), 180},
        {QStringLiteral("mkv"), 180},
        {QStringLiteral("webm"), 180},
        {QStringLiteral("png"), 90},
        {QStringLiteral("jpg"), 90},
        {QStringLiteral("jpeg"), 90},
        {QStringLiteral("webp"), 90},
    };
    return kDurations.value(suffix, 120);
}

inline QColor TimelineWidget::colorForPath(const QString& path) const {
    const quint32 hash = qHash(path);
    return QColor::fromHsv(static_cast<int>(hash % 360), 160, 220, 220);
}

inline TimelineClip TimelineWidget::buildClipFromFile(const QString& filePath,
                                               int64_t startFrame,
                                               int trackIndex) const {
    const QFileInfo info(filePath);
    const MediaProbeResult probe = probeMediaFile(filePath, guessDurationFrames(info.suffix().toLower()));

    TimelineClip clip;
    clip.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    clip.filePath = filePath;
    clip.label = info.fileName();
    clip.mediaType = probe.mediaType;
    clip.hasAudio = probe.hasAudio;
    clip.sourceDurationFrames = probe.durationFrames;
    clip.startFrame = startFrame;
    clip.durationFrames = probe.durationFrames;
    clip.trackIndex = trackIndex;
    clip.color = colorForPath(filePath);
    if (clip.mediaType == ClipMediaType::Audio) {
        clip.color = QColor(QStringLiteral("#2f7f93"));
    }
    return clip;
}

inline void TimelineWidget::addClipFromFile(const QString& filePath, int64_t startFrame) {
    const QFileInfo info(filePath);
    if (!info.exists() || !info.isFile()) return;

    TimelineClip clip = buildClipFromFile(filePath,
                                          startFrame >= 0 ? startFrame : totalFrames(),
                                          nextTrackIndex());
    m_clips.push_back(clip);
    normalizeTrackIndices();
    sortClips();
    
    if (clipsChanged) clipsChanged();
    update();
}

inline void TimelineWidget::dragEnterEvent(QDragEnterEvent* event) {
    if (hasFileUrls(event->mimeData())) {
        event->acceptProposedAction();
        return;
    }
    QWidget::dragEnterEvent(event);
}

inline void TimelineWidget::dragMoveEvent(QDragMoveEvent* event) {
    if (hasFileUrls(event->mimeData())) {
        m_dropFrame = frameFromX(event->position().x());
        m_trackDropIndex = trackDropTargetAtY(event->position().toPoint().y(), &m_trackDropInGap);
        event->acceptProposedAction();
        update();
        return;
    }
    QWidget::dragMoveEvent(event);
}

inline void TimelineWidget::dragLeaveEvent(QDragLeaveEvent* event) {
    m_dropFrame = -1;
    m_trackDropIndex = -1;
    m_trackDropInGap = false;
    m_snapIndicatorFrame = -1;
    QWidget::dragLeaveEvent(event);
    update();
}

inline void TimelineWidget::dropEvent(QDropEvent* event) {
    if (!hasFileUrls(event->mimeData())) {
        QWidget::dropEvent(event);
        return;
    }
    
    int64_t insertFrame = frameFromX(event->position().x());
    bool insertsTrack = false;
    int targetTrack = trackDropTargetAtY(event->position().toPoint().y(), &insertsTrack);
    if (targetTrack < 0) {
        targetTrack = nextTrackIndex();
        insertsTrack = true;
    }
    if (insertsTrack) {
        insertTrackAt(targetTrack);
    }
    
    for (const QUrl& url : event->mimeData()->urls()) {
        if (!url.isLocalFile()) continue;
        
        const QString filePath = url.toLocalFile();
        const QFileInfo info(filePath);
        if (!info.exists() || info.isDir()) continue;
        
        ensureTrackCount(targetTrack + 1);
        TimelineClip clip = buildClipFromFile(filePath, insertFrame, targetTrack);
        m_clips.push_back(clip);
        insertFrame += clip.durationFrames + 6;
    }
    
    normalizeTrackIndices();
    sortClips();
    m_dropFrame = -1;
    m_trackDropIndex = -1;
    m_trackDropInGap = false;
    event->acceptProposedAction();
    
    if (clipsChanged) clipsChanged();
    update();
}

inline void TimelineWidget::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        const int trackHit = trackIndexAt(event->position().toPoint());
        if (trackHit >= 0) {
            const QString currentName =
                (trackHit >= 0 && trackHit < m_tracks.size()) ? m_tracks[trackHit].name : defaultTrackName(trackHit);
            bool accepted = false;
            const QString nextName = QInputDialog::getText(
                this,
                QStringLiteral("Rename Track"),
                QStringLiteral("Track name"),
                QLineEdit::Normal,
                currentName,
                &accepted);
            if (accepted) {
                ensureTrackCount(trackHit + 1);
                m_tracks[trackHit].name = nextName.trimmed().isEmpty() ? defaultTrackName(trackHit) : nextName.trimmed();
                if (clipsChanged) {
                    clipsChanged();
                }
                update();
            }
            event->accept();
            return;
        }
    }
    QWidget::mouseDoubleClickEvent(event);
}

inline void TimelineWidget::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        const int dividerHit = trackDividerAt(event->position().toPoint());
        if (dividerHit >= 0) {
            m_resizingTrackIndex = dividerHit;
            m_resizeOriginY = event->position().toPoint().y();
            m_resizeOriginHeight = trackHeight(dividerHit);
            setCursor(Qt::SizeVerCursor);
            event->accept();
            return;
        }
        const int trackHit = trackIndexAt(event->position().toPoint());
        if (trackHit >= 0) {
            m_draggedTrackIndex = trackHit;
            m_trackDropIndex = trackHit;
            update();
            return;
        }
        const int hitIndex = clipIndexAt(event->position().toPoint());
        if (hitIndex >= 0) {
            setSelectedClipId(m_clips[hitIndex].id);
            m_dragMode = clipDragModeAt(m_clips[hitIndex], event->position().toPoint());
            m_draggedClipIndex = hitIndex;
            m_dragOriginalStartFrame = m_clips[hitIndex].startFrame;
            m_dragOriginalDurationFrames = m_clips[hitIndex].durationFrames;
            m_dragOriginalSourceInFrame = m_clips[hitIndex].sourceInFrame;
            m_dragOriginalTransformKeyframes = m_clips[hitIndex].transformKeyframes;
            m_dragOffsetFrames = frameFromX(event->position().x()) - m_clips[hitIndex].startFrame;
            m_currentFrame = m_clips[hitIndex].startFrame;
            update();
            return;
        }
        
        const int64_t frame = frameFromX(event->position().x());
        m_currentFrame = frame;
        if (seekRequested) seekRequested(frame);
        update();
        return;
    }
    QWidget::mousePressEvent(event);
}

inline void TimelineWidget::mouseMoveEvent(QMouseEvent* event) {
    if (m_draggedTrackIndex >= 0 && (event->buttons() & Qt::LeftButton)) {
        const int proposed = qBound(0, trackIndexAtY(event->position().toPoint().y(), false), trackCount() - 1);
        if (proposed != m_trackDropIndex) {
            m_trackDropIndex = proposed;
            update();
        }
        return;
    }
    if (m_resizingTrackIndex >= 0 && (event->buttons() & Qt::LeftButton)) {
        ensureTrackCount(m_resizingTrackIndex + 1);
        const int delta = event->position().toPoint().y() - m_resizeOriginY;
        m_tracks[m_resizingTrackIndex].height = qMax(kMinTrackHeight, m_resizeOriginHeight + delta);
        updateMinimumTimelineHeight();
        update();
        return;
    }
    if (m_draggedClipIndex >= 0 && (event->buttons() & Qt::LeftButton)) {
        TimelineClip& clip = m_clips[m_draggedClipIndex];
        const int64_t pointerFrame = frameFromX(event->position().x());
        static constexpr int64_t kMinClipFrames = 1;
        const bool isImage = clip.mediaType == ClipMediaType::Image;
        m_snapIndicatorFrame = -1;
        if (m_dragMode == ClipDragMode::Move) {
            bool snapped = false;
            int64_t snappedBoundaryFrame = -1;
            const int64_t unsnappedStartFrame = qMax<int64_t>(0, pointerFrame - m_dragOffsetFrames);
            const int64_t newStartFrame =
                snapMoveStartFrame(clip, unsnappedStartFrame, &snapped, &snappedBoundaryFrame);
            clip.startFrame = newStartFrame;
            m_snapIndicatorFrame = snapped ? snappedBoundaryFrame : -1;
            bool insertsTrack = false;
            const int proposedTrack = trackDropTargetAtY(event->position().toPoint().y(), &insertsTrack);
            if (proposedTrack >= 0) {
                clip.trackIndex = proposedTrack;
                m_trackDropIndex = proposedTrack;
                m_trackDropInGap = insertsTrack;
            }
            m_currentFrame = newStartFrame;
        } else if (m_dragMode == ClipDragMode::TrimLeft) {
            const int64_t maxStartFrame = m_dragOriginalStartFrame + m_dragOriginalDurationFrames - kMinClipFrames;
            bool snapped = false;
            int64_t snappedBoundaryFrame = -1;
            const int64_t boundedPointerFrame = qBound<int64_t>(0, pointerFrame, maxStartFrame);
            const int64_t newStartFrame =
                qBound<int64_t>(0,
                                snapTrimLeftFrame(clip, boundedPointerFrame, &snapped, &snappedBoundaryFrame),
                                maxStartFrame);
            const int64_t trimDelta = newStartFrame - m_dragOriginalStartFrame;
            clip.startFrame = newStartFrame;
            m_snapIndicatorFrame = snapped ? snappedBoundaryFrame : -1;
            if (isImage) {
                clip.sourceInFrame = 0;
                clip.durationFrames = m_dragOriginalDurationFrames + (m_dragOriginalStartFrame - newStartFrame);
                clip.sourceDurationFrames = clip.durationFrames;
                clip.transformKeyframes = m_dragOriginalTransformKeyframes;
            } else {
                clip.sourceInFrame = m_dragOriginalSourceInFrame + trimDelta;
                clip.durationFrames = m_dragOriginalDurationFrames - trimDelta;
                clip.transformKeyframes.clear();
                for (const TimelineClip::TransformKeyframe& keyframe : m_dragOriginalTransformKeyframes) {
                    if (keyframe.frame < trimDelta) {
                        continue;
                    }
                    TimelineClip::TransformKeyframe shifted = keyframe;
                    shifted.frame -= trimDelta;
                    clip.transformKeyframes.push_back(shifted);
                }
                normalizeClipTransformKeyframes(clip);
            }
            m_currentFrame = newStartFrame;
        } else if (m_dragMode == ClipDragMode::TrimRight) {
            const int64_t minEndFrame = m_dragOriginalStartFrame + kMinClipFrames;
            const int64_t maxEndFrame = isImage
                ? std::numeric_limits<int64_t>::max()
                : m_dragOriginalStartFrame +
                      qMax<int64_t>(kMinClipFrames,
                                    mediaDurationFrames(QFileInfo(clip.filePath)) - m_dragOriginalSourceInFrame);
            bool snapped = false;
            int64_t snappedBoundaryFrame = -1;
            const int64_t boundedPointerFrame = isImage
                ? qMax<int64_t>(minEndFrame, pointerFrame)
                : qBound<int64_t>(minEndFrame, pointerFrame, maxEndFrame);
            const int64_t newEndFrame = isImage
                ? qMax<int64_t>(minEndFrame,
                                snapTrimRightFrame(clip, boundedPointerFrame, &snapped, &snappedBoundaryFrame))
                : qBound<int64_t>(minEndFrame,
                                  snapTrimRightFrame(clip, boundedPointerFrame, &snapped, &snappedBoundaryFrame),
                                  maxEndFrame);
            clip.durationFrames = newEndFrame - m_dragOriginalStartFrame;
            m_snapIndicatorFrame = snapped ? snappedBoundaryFrame : -1;
            if (isImage) {
                clip.sourceDurationFrames = clip.durationFrames;
                clip.transformKeyframes = m_dragOriginalTransformKeyframes;
            } else {
                clip.transformKeyframes.clear();
                for (const TimelineClip::TransformKeyframe& keyframe : m_dragOriginalTransformKeyframes) {
                    if (keyframe.frame < clip.durationFrames) {
                        clip.transformKeyframes.push_back(keyframe);
                    }
                }
                normalizeClipTransformKeyframes(clip);
            }
            m_currentFrame = newEndFrame;
        }
        update();
        return;
    }
    updateHoverCursor(event->position().toPoint());
    QWidget::mouseMoveEvent(event);
}

inline void TimelineWidget::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && m_resizingTrackIndex >= 0) {
        m_resizingTrackIndex = -1;
        m_resizeOriginY = 0;
        m_resizeOriginHeight = 0;
        if (clipsChanged) {
            clipsChanged();
        }
        updateHoverCursor(event->position().toPoint());
        update();
        return;
    }
    if (event->button() == Qt::LeftButton && m_draggedTrackIndex >= 0) {
        const int fromTrack = m_draggedTrackIndex;
        const int toTrack = qBound(0, m_trackDropIndex, trackCount() - 1);
        if (fromTrack != toTrack) {
            ensureTrackCount(trackCount());
            for (TimelineClip& clip : m_clips) {
                if (clip.trackIndex == fromTrack) {
                    clip.trackIndex = toTrack;
                } else if (fromTrack < toTrack && clip.trackIndex > fromTrack && clip.trackIndex <= toTrack) {
                    clip.trackIndex -= 1;
                } else if (fromTrack > toTrack && clip.trackIndex >= toTrack && clip.trackIndex < fromTrack) {
                    clip.trackIndex += 1;
                }
            }
            if (fromTrack >= 0 && fromTrack < m_tracks.size() && toTrack >= 0 && toTrack < m_tracks.size()) {
                TimelineTrack movedTrack = m_tracks.takeAt(fromTrack);
                m_tracks.insert(toTrack, movedTrack);
            }
            normalizeTrackIndices();
            sortClips();
            if (clipsChanged) clipsChanged();
        }
        m_draggedTrackIndex = -1;
        m_trackDropIndex = -1;
        m_trackDropInGap = false;
        m_snapIndicatorFrame = -1;
        update();
        return;
    }
    if (event->button() == Qt::LeftButton && m_draggedClipIndex >= 0) {
        if (m_dragMode == ClipDragMode::Move && m_trackDropInGap && m_trackDropIndex >= 0) {
            const QString movingClipId = m_clips[m_draggedClipIndex].id;
            insertTrackAt(m_trackDropIndex);
            for (TimelineClip& clip : m_clips) {
                if (clip.id == movingClipId) {
                    clip.trackIndex = m_trackDropIndex;
                    break;
                }
            }
        }
        normalizeTrackIndices();
        sortClips();
        m_draggedClipIndex = -1;
        m_dragMode = ClipDragMode::None;
        m_trackDropIndex = -1;
        m_trackDropInGap = false;
        m_snapIndicatorFrame = -1;
        m_dragOffsetFrames = 0;
        m_dragOriginalStartFrame = 0;
        m_dragOriginalDurationFrames = 0;
        m_dragOriginalSourceInFrame = 0;
        m_dragOriginalTransformKeyframes.clear();
        if (clipsChanged) clipsChanged();
        updateHoverCursor(event->position().toPoint());
        update();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

inline void TimelineWidget::contextMenuEvent(QContextMenuEvent* event) {
    const int clipIndex = clipIndexAt(event->pos());
    QMenu menu(this);
    QAction* duplicateRenderFrameAction =
        menu.addAction(QStringLiteral("Render Sync: Duplicate Frame At Playhead"));
    QAction* skipRenderFrameAction =
        menu.addAction(QStringLiteral("Render Sync: Skip Frame At Playhead"));
    QAction* clearRenderSyncAction = menu.addAction(QStringLiteral("Clear Render Sync At Playhead"));
    const RenderSyncMarker* currentSyncMarker = renderSyncMarkerAtFrame(m_currentFrame);
    clearRenderSyncAction->setEnabled(currentSyncMarker != nullptr);

    QAction* nudgeLeftAction = nullptr;
    QAction* nudgeRightAction = nullptr;
    QAction* deleteAction = nullptr;
    QAction* gradingAction = nullptr;
    QAction* resetGradingAction = nullptr;
    QAction* propertiesAction = nullptr;

    if (clipIndex >= 0) {
        menu.addSeparator();
        setSelectedClipId(m_clips[clipIndex].id);
        const bool audioOnly = clipIsAudioOnly(m_clips[clipIndex]);
        nudgeLeftAction = menu.addAction(
            audioOnly ? QStringLiteral("Nudge -25ms\tAlt+Left") : QStringLiteral("Nudge -1 Frame\tAlt+Left"));
        nudgeRightAction = menu.addAction(
            audioOnly ? QStringLiteral("Nudge +25ms\tAlt+Right") : QStringLiteral("Nudge +1 Frame\tAlt+Right"));
        menu.addSeparator();
        deleteAction = menu.addAction(QStringLiteral("Delete"));
        gradingAction = menu.addAction(QStringLiteral("Grading..."));
        resetGradingAction = menu.addAction(QStringLiteral("Reset Grading"));
        propertiesAction = menu.addAction(QStringLiteral("Properties"));
    }
    
    QAction* selected = menu.exec(event->globalPos());
    if (!selected) return;

    if (selected == duplicateRenderFrameAction) {
        setRenderSyncMarkerAtCurrentFrame(RenderSyncAction::DuplicateFrame);
        return;
    }

    if (selected == skipRenderFrameAction) {
        setRenderSyncMarkerAtCurrentFrame(RenderSyncAction::SkipFrame);
        return;
    }

    if (selected == clearRenderSyncAction) {
        clearRenderSyncMarkerAtCurrentFrame();
        return;
    }
    
    if (selected == nudgeLeftAction) {
        nudgeSelectedClip(-1);
        return;
    }

    if (selected == nudgeRightAction) {
        nudgeSelectedClip(1);
        return;
    }

    if (selected == deleteAction) {
        const QString deletedId = m_clips[clipIndex].id;
        m_clips.removeAt(clipIndex);
        if (m_selectedClipId == deletedId) {
            m_selectedClipId.clear();
            if (selectionChanged) {
                selectionChanged();
            }
        }
        if (clipsChanged) clipsChanged();
        update();
        return;
    }

    if (selected == gradingAction) {
        if (gradingRequested) {
            gradingRequested();
        }
        return;
    }

    if (selected == resetGradingAction) {
        TimelineClip& clip = m_clips[clipIndex];
        clip.brightness = 0.0;
        clip.contrast = 1.0;
        clip.saturation = 1.0;
        clip.opacity = 1.0;
        if (clipsChanged) clipsChanged();
        update();
        return;
    }
    
    if (selected == propertiesAction) {
        const TimelineClip& clip = m_clips[clipIndex];
        QMessageBox::information(this, QStringLiteral("Clip Properties"),
            QStringLiteral("Name: %1\nPath: %2\nType: %3\nStart: %4\nSource In: %5\nDuration: %6 frames\nAudio start offset: %7 ms\nBrightness: %8\nContrast: %9\nSaturation: %10\nOpacity: %11")
                .arg(clip.label)
                .arg(QDir::toNativeSeparators(clip.filePath))
                .arg(clipMediaTypeLabel(clip.mediaType))
                .arg(timecodeForFrame(clip.startFrame))
                .arg(timecodeForFrame(clip.sourceInFrame))
                .arg(clip.durationFrames)
                .arg(qRound64((clip.startSubframeSamples * 1000.0) / kAudioSampleRate))
                .arg(clip.brightness, 0, 'f', 2)
                .arg(clip.contrast, 0, 'f', 2)
                .arg(clip.saturation, 0, 'f', 2)
                .arg(clip.opacity, 0, 'f', 2));
        return;
    }
}

inline void TimelineWidget::wheelEvent(QWheelEvent* event) {
    const QPoint numDegrees = event->angleDelta() / 8;
    if (numDegrees.isNull()) {
        QWidget::wheelEvent(event);
        return;
    }
    
    const int steps = numDegrees.y() / 15;
    if (steps == 0) {
        QWidget::wheelEvent(event);
        return;
    }
    
    if (event->modifiers() & Qt::ControlModifier) {
        const int visibleFrames = qMax(1, static_cast<int>(width() / m_pixelsPerFrame));
        const int panFrames = qMax(1, visibleFrames / 12);
        m_frameOffset = qMax<int64_t>(0, m_frameOffset - steps * panFrames);
        update();
        event->accept();
        return;
    }
    
    const qreal oldPixelsPerFrame = m_pixelsPerFrame;
    const qreal cursorFrame = frameFromX(event->position().x());
    const qreal zoomFactor = steps > 0 ? 1.15 : (1.0 / 1.15);
    const QRect contentRect = timelineContentRect();
    const int64_t fullTimelineFrames = qMax<int64_t>(1, totalFrames());
    const qreal fitAllPixelsPerFrame =
        contentRect.width() > 0
            ? static_cast<qreal>(contentRect.width()) / static_cast<qreal>(fullTimelineFrames)
            : 0.01;
    const qreal minPixelsPerFrame = qMin<qreal>(0.25, fitAllPixelsPerFrame);
    m_pixelsPerFrame = qBound(minPixelsPerFrame, m_pixelsPerFrame * std::pow(zoomFactor, std::abs(steps)), 24.0);
    
    const qreal localX = event->position().x() - 16.0;
    if (m_pixelsPerFrame > 0.0) {
        const qreal newOffset = cursorFrame - qMax<qreal>(0.0, localX) / m_pixelsPerFrame;
        const int64_t visibleFrames = qMax<int64_t>(1, qRound(static_cast<qreal>(contentRect.width()) / m_pixelsPerFrame));
        const int64_t maxOffset = qMax<int64_t>(0, totalFrames() - visibleFrames);
        m_frameOffset = qBound<int64_t>(0, static_cast<int64_t>(qRound(newOffset)), maxOffset);
    }

    if (m_pixelsPerFrame <= fitAllPixelsPerFrame + 0.0001) {
        m_frameOffset = 0;
    }
    
    if (!qFuzzyCompare(oldPixelsPerFrame, m_pixelsPerFrame)) {
        update();
    }
    event->accept();
}

inline void TimelineWidget::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.fillRect(rect(), QColor(QStringLiteral("#0f1216")));
    
    const QRect draw = drawRect();
    const QRect ruler = rulerRect();
    const QRect tracks = trackRect();
    const QRect content = timelineContentRect();
    
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(QStringLiteral("#171c22")));
    painter.drawRoundedRect(tracks, 10, 10);
    
    painter.setPen(QColor(QStringLiteral("#6d7887")));
    for (int64_t frame = 0; frame <= totalFrames(); frame += 30) {
        const int x = xFromFrame(frame);
        const bool major = (frame % 150) == 0;
        painter.setPen(major ? QColor(QStringLiteral("#8fa0b5")) : QColor(QStringLiteral("#53606e")));
        painter.drawLine(x, ruler.bottom() - (major ? 18 : 10), x, tracks.bottom() - 8);
        
        if (major) {
            painter.drawText(QRect(x + 4, ruler.top(), 56, ruler.height()),
                            Qt::AlignLeft | Qt::AlignVCenter,
                            timecodeForFrame(frame));
        }
    }
    
    painter.setPen(QColor(QStringLiteral("#24303c")));
    for (int track = 0; track < trackCount(); ++track) {
        const QRect labelRect = trackLabelRect(track);
        const bool dragged = track == m_draggedTrackIndex;
        const bool target = track == m_trackDropIndex && m_draggedTrackIndex >= 0 && !m_trackDropInGap;
        painter.setBrush(dragged ? QColor(QStringLiteral("#ff6f61"))
                                 : (target ? QColor(QStringLiteral("#32465f"))
                                           : QColor(QStringLiteral("#202a34"))));
        painter.drawRoundedRect(labelRect, 8, 8);
        painter.setPen(QColor(QStringLiteral("#eef4fa")));
        painter.drawText(labelRect.adjusted(4, 0, -4, 0),
                         Qt::AlignCenter,
                         painter.fontMetrics().elidedText(m_tracks.value(track).name, Qt::ElideRight, labelRect.width() - 8));
        painter.setPen(QColor(QStringLiteral("#24303c")));
        const int dividerY = trackTop(track) + trackHeight(track);
        painter.drawLine(content.left() - 8, dividerY, tracks.right() - 10, dividerY);
    }
    
    for (const TimelineClip& clip : m_clips) {
        const QRect clipRect = clipRectFor(clip);
        const bool audioOnly = clipIsAudioOnly(clip);
        const bool showSourceGhost = clip.mediaType != ClipMediaType::Image;
        if (showSourceGhost) {
            const int64_t ghostStartFrame = qMax<int64_t>(0, clip.startFrame - clip.sourceInFrame);
            const int64_t ghostDurationFrames = qMax<int64_t>(clip.durationFrames, clip.sourceDurationFrames);
            const QRect ghostRect(xFromFrame(ghostStartFrame),
                                  clipRect.y(),
                                  qMax(40, widthForFrames(ghostDurationFrames)),
                                  clipRect.height());

            if (ghostRect != clipRect) {
                painter.setPen(QColor(255, 255, 255, 20));
                painter.setBrush(clip.color.lighter(130));
                painter.setOpacity(0.18);
                painter.drawRoundedRect(ghostRect, 7, 7);
                painter.setOpacity(1.0);
            }
        }
        
        painter.setPen(QColor(255, 255, 255, 32));
        painter.setBrush(clip.color);
        painter.drawRoundedRect(clipRect, 7, 7);

        if (audioOnly) {
            painter.save();
            const int verticalInset = qMax(5, clipRect.height() / 10);
            const QRect envelopeRect = clipRect.adjusted(8, verticalInset, -8, -verticalInset);
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(QStringLiteral("#163946")));
            painter.drawRoundedRect(envelopeRect, 5, 5);
            painter.setClipRect(envelopeRect);
            painter.setPen(QPen(QColor(QStringLiteral("#9fe7f4")), 2));
            painter.drawLine(envelopeRect.left(), envelopeRect.center().y(),
                             envelopeRect.right(), envelopeRect.center().y());
            painter.setPen(Qt::NoPen);
            for (int x = envelopeRect.left(); x < envelopeRect.right(); x += 6) {
                const int idx = (x - envelopeRect.left()) / 6;
                const qreal phase = static_cast<qreal>((idx * 17 + clip.startFrame + clip.sourceInFrame) % 100) / 99.0;
                const qreal shaped = 0.2 + std::abs(std::sin(phase * 6.28318)) * 0.8;
                const int barHeight = qMax(8, qRound(shaped * envelopeRect.height()));
                const QRect barRect(x, envelopeRect.center().y() - barHeight / 2, 4, barHeight);
                painter.setBrush(QColor(QStringLiteral("#f2feff")));
                painter.drawRoundedRect(barRect, 2, 2);
            }
            painter.restore();
        }
        
        painter.setPen(QColor(QStringLiteral("#f4f7fb")));
        if (clip.id == m_selectedClipId) {
            painter.setPen(QPen(QColor(QStringLiteral("#fff4c2")), 2));
            if (audioOnly) {
                painter.setBrush(Qt::NoBrush);
                painter.drawRoundedRect(clipRect.adjusted(1, 1, -1, -1), 7, 7);
            } else {
                painter.setBrush(clip.color.lighter(108));
                painter.drawRoundedRect(clipRect, 7, 7);
            }
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(QStringLiteral("#fff4c2")));
            const int handleInset = qMax(5, clipRect.height() / 10);
            const QRect leftHandle(clipRect.left() + 2, clipRect.top() + handleInset, 4, clipRect.height() - (handleInset * 2));
            const QRect rightHandle(clipRect.right() - 5, clipRect.top() + handleInset, 4, clipRect.height() - (handleInset * 2));
            painter.drawRoundedRect(leftHandle, 2, 2);
            painter.drawRoundedRect(rightHandle, 2, 2);
            painter.setPen(QColor(QStringLiteral("#f4f7fb")));
        }
        const QString clipTitle = audioOnly
            ? QStringLiteral("AUDIO  %1").arg(clip.label)
            : clip.label;
        painter.drawText(clipRect.adjusted(10, 0, -10, 0),
                        Qt::AlignLeft | Qt::AlignVCenter,
                        painter.fontMetrics().elidedText(clipTitle, Qt::ElideRight, 
                                                         clipRect.width() - 20));
    }

    for (const RenderSyncMarker& marker : m_renderSyncMarkers) {
        const int x = xFromFrame(marker.frame);
        const QColor markerColor = marker.action == RenderSyncAction::DuplicateFrame
            ? QColor(QStringLiteral("#ff5b5b"))
            : QColor(QStringLiteral("#ff9e3d"));
        painter.setPen(QPen(markerColor, 2));
        painter.drawLine(x, ruler.top() + 2, x, ruler.bottom() - 2);
        painter.setPen(Qt::NoPen);
        painter.setBrush(markerColor);
        painter.drawRoundedRect(QRect(x - 9, ruler.bottom() - 8, 18, 6), 3, 3);
    }
    
    if (m_dropFrame >= 0) {
        const int x = xFromFrame(m_dropFrame);
        painter.setPen(QPen(QColor(QStringLiteral("#f7b955")), 2, Qt::DashLine));
        painter.drawLine(x, tracks.top() + 2, x, tracks.bottom() - 2);
    }

    if (m_snapIndicatorFrame >= 0) {
        const int x = xFromFrame(m_snapIndicatorFrame);
        painter.setPen(QPen(QColor(QStringLiteral("#ffe082")), 2, Qt::DashLine));
        painter.drawLine(x, ruler.top() + 4, x, tracks.bottom() - 2);
    }

    if (m_trackDropInGap && m_trackDropIndex >= 0 && (m_draggedClipIndex >= 0 || m_dropFrame >= 0)) {
        int insertionY = trackTop(0) - (kTimelineTrackSpacing / 2);
        if (m_trackDropIndex >= trackCount()) {
            insertionY = trackTop(trackCount() - 1) + trackHeight(trackCount() - 1) + (kTimelineTrackSpacing / 2);
        } else if (m_trackDropIndex > 0) {
            insertionY = trackTop(m_trackDropIndex) - (kTimelineTrackSpacing / 2);
        }
        painter.setPen(QPen(QColor(QStringLiteral("#f7b955")), 2, Qt::DashLine));
        painter.drawLine(content.left() - 8, insertionY, tracks.right() - 10, insertionY);
    }
    
    const int playheadX = xFromFrame(m_currentFrame);
    painter.setPen(QPen(QColor(QStringLiteral("#ff6f61")), 3));
    painter.drawLine(playheadX, ruler.top(), playheadX, tracks.bottom());
    
    painter.setBrush(QColor(QStringLiteral("#ff6f61")));
    painter.setPen(Qt::NoPen);
    painter.drawRoundedRect(QRect(playheadX - 8, ruler.top(), 16, 12), 4, 4);
    if (const RenderSyncMarker* marker = renderSyncMarkerAtFrame(m_currentFrame)) {
        const QString label = marker->action == RenderSyncAction::DuplicateFrame
            ? QStringLiteral("DUP")
            : QStringLiteral("SKIP");
        const QRect badgeRect(playheadX + 10, ruler.top() - 2, 42, 16);
        painter.setBrush(marker->action == RenderSyncAction::DuplicateFrame
                             ? QColor(QStringLiteral("#ff5b5b"))
                             : QColor(QStringLiteral("#ff9e3d")));
        painter.drawRoundedRect(badgeRect, 6, 6);
        painter.setPen(QColor(QStringLiteral("#ffffff")));
        painter.drawText(badgeRect, Qt::AlignCenter, label);
    }
}
