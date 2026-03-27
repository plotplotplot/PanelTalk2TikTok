#include "timeline_widget.h"

#include <QHash>

#include <algorithm>
#include <cmath>
#include <limits>

TimelineWidget::TimelineWidget(QWidget* parent) : QWidget(parent) {
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

void TimelineWidget::setCurrentFrame(int64_t frame) {
    m_currentFrame = qMax<int64_t>(0, frame);
    normalizeExportRange();
    update();
}

int64_t TimelineWidget::totalFrames() const {
    int64_t lastFrame = 300;
    for (const TimelineClip& clip : m_clips) {
        lastFrame = qMax(lastFrame, clip.startFrame + clip.durationFrames + 30);
    }
    return lastFrame;
}

void TimelineWidget::setClips(const QVector<TimelineClip>& clips) {
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
    bool hoveredClipStillExists = m_hoveredClipId.isEmpty();
    if (!hoveredClipStillExists) {
        for (const TimelineClip& clip : m_clips) {
            if (clip.id == m_hoveredClipId) {
                hoveredClipStillExists = true;
                break;
            }
        }
    }
    if (!hoveredClipStillExists) {
        m_hoveredClipId.clear();
    }
    normalizeExportRange();
    updateMinimumTimelineHeight();
    update();
}

void TimelineWidget::setTracks(const QVector<TimelineTrack>& tracks) {
    m_tracks = tracks;
    ensureTrackCount(trackCount());
    normalizeExportRange();
    updateMinimumTimelineHeight();
    update();
}

const TimelineClip* TimelineWidget::selectedClip() const {
    for (const TimelineClip& clip : m_clips) {
        if (clip.id == m_selectedClipId) {
            return &clip;
        }
    }
    return nullptr;
}

void TimelineWidget::setSelectedClipId(const QString& clipId) {
    if (m_selectedClipId == clipId) {
        return;
    }
    m_selectedClipId = clipId;
    if (selectionChanged) {
        selectionChanged();
    }
    update();
}

bool TimelineWidget::updateClipById(const QString& clipId, const std::function<void(TimelineClip&)>& updater) {
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

bool TimelineWidget::deleteSelectedClip() {
    if (m_selectedClipId.isEmpty()) {
        return false;
    }

    for (int i = 0; i < m_clips.size(); ++i) {
        if (m_clips[i].id != m_selectedClipId) {
            continue;
        }

        if (m_clips[i].locked) {
            return false;
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

bool TimelineWidget::splitSelectedClipAtFrame(int64_t frame) {
    if (m_selectedClipId.isEmpty()) {
        return false;
    }

    for (int i = 0; i < m_clips.size(); ++i) {
        TimelineClip& clip = m_clips[i];
        if (clip.id != m_selectedClipId) {
            continue;
        }
        if (clip.locked) {
            return false;
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

void TimelineWidget::sortClips() {
    std::sort(m_clips.begin(), m_clips.end(), [](const TimelineClip& a, const TimelineClip& b) {
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

void TimelineWidget::sortRenderSyncMarkers() {
    std::sort(m_renderSyncMarkers.begin(), m_renderSyncMarkers.end(),
              [](const RenderSyncMarker& a, const RenderSyncMarker& b) {
                  if (a.clipId != b.clipId) {
                      return a.clipId < b.clipId;
                  }
                  if (a.frame == b.frame) {
                      return static_cast<int>(a.action) < static_cast<int>(b.action);
                  }
                  return a.frame < b.frame;
              });
}

void TimelineWidget::setRenderSyncMarkers(const QVector<RenderSyncMarker>& markers) {
    m_renderSyncMarkers = markers;
    sortRenderSyncMarkers();
    update();
}

const RenderSyncMarker* TimelineWidget::renderSyncMarkerAtFrame(const QString& clipId, int64_t frame) const {
    for (const RenderSyncMarker& marker : m_renderSyncMarkers) {
        if (marker.clipId == clipId && marker.frame == frame) {
            return &marker;
        }
    }
    return nullptr;
}

bool TimelineWidget::setRenderSyncMarkerAtCurrentFrame(const QString& clipId, RenderSyncAction action, int count) {
    if (clipId.isEmpty()) {
        return false;
    }
    count = qMax(1, count);
    for (RenderSyncMarker& marker : m_renderSyncMarkers) {
        if (marker.clipId == clipId && marker.frame == m_currentFrame) {
            if (marker.action == action && marker.count == count) {
                return false;
            }
            marker.action = action;
            marker.count = count;
            sortRenderSyncMarkers();
            if (renderSyncMarkersChanged) {
                renderSyncMarkersChanged();
            }
            update();
            return true;
        }
    }

    RenderSyncMarker marker;
    marker.clipId = clipId;
    marker.frame = m_currentFrame;
    marker.action = action;
    marker.count = count;
    m_renderSyncMarkers.push_back(marker);
    sortRenderSyncMarkers();
    if (renderSyncMarkersChanged) {
        renderSyncMarkersChanged();
    }
    update();
    return true;
}

bool TimelineWidget::clearRenderSyncMarkerAtCurrentFrame(const QString& clipId) {
    if (clipId.isEmpty()) {
        return false;
    }
    for (int i = 0; i < m_renderSyncMarkers.size(); ++i) {
        if (m_renderSyncMarkers[i].clipId == clipId && m_renderSyncMarkers[i].frame == m_currentFrame) {
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

void TimelineWidget::normalizeExportRange() {
    normalizeExportRanges();
}

void TimelineWidget::normalizeExportRanges() {
    const int64_t total = totalFrames();
    if (m_exportRanges.isEmpty()) {
        m_exportRanges = {ExportRangeSegment{0, total}};
    }
    for (ExportRangeSegment& segment : m_exportRanges) {
        segment.startFrame = qBound<int64_t>(0, segment.startFrame, total);
        segment.endFrame = qBound<int64_t>(0, segment.endFrame, total);
        if (segment.endFrame < segment.startFrame) {
            std::swap(segment.startFrame, segment.endFrame);
        }
    }
    std::sort(m_exportRanges.begin(), m_exportRanges.end(), [](const ExportRangeSegment& a, const ExportRangeSegment& b) {
        if (a.startFrame == b.startFrame) {
            return a.endFrame < b.endFrame;
        }
        return a.startFrame < b.startFrame;
    });
    QVector<ExportRangeSegment> normalized;
    normalized.reserve(m_exportRanges.size());
    for (const ExportRangeSegment& segment : std::as_const(m_exportRanges)) {
        if (!normalized.isEmpty() &&
            normalized.constLast().startFrame == segment.startFrame &&
            normalized.constLast().endFrame == segment.endFrame) {
            continue;
        }
        normalized.push_back(segment);
    }
    m_exportRanges = normalized;
}

int TimelineWidget::exportSegmentIndexAtFrame(int64_t frame) const {
    for (int i = 0; i < m_exportRanges.size(); ++i) {
        if (frame >= m_exportRanges[i].startFrame && frame <= m_exportRanges[i].endFrame) {
            return i;
        }
    }
    return -1;
}

int TimelineWidget::exportHandleAtPos(const QPoint& pos, bool* startHandleOut) const {
    for (int i = 0; i < m_exportRanges.size(); ++i) {
        if (exportHandleRect(i, true).contains(pos)) {
            if (startHandleOut) {
                *startHandleOut = true;
            }
            return i;
        }
        if (exportHandleRect(i, false).contains(pos)) {
            if (startHandleOut) {
                *startHandleOut = false;
            }
            return i;
        }
    }
    return -1;
}

int64_t TimelineWidget::snapThresholdFrames() const {
    static constexpr qreal kSnapThresholdPixels = 10.0;
    return qMax<int64_t>(1, qRound64(kSnapThresholdPixels / qMax<qreal>(0.25, m_pixelsPerFrame)));
}

int64_t TimelineWidget::nearestClipBoundaryFrame(const QString& excludedClipId, int64_t frame, bool* snapped) const {
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

int64_t TimelineWidget::snapMoveStartFrame(const TimelineClip& clip,
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

int64_t TimelineWidget::snapTrimLeftFrame(const TimelineClip& clip,
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

int64_t TimelineWidget::snapTrimRightFrame(const TimelineClip& clip,
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

bool TimelineWidget::nudgeSelectedClip(int direction) {
    if (direction == 0 || m_selectedClipId.isEmpty()) {
        return false;
    }

    for (TimelineClip& clip : m_clips) {
        if (clip.id != m_selectedClipId) {
            continue;
        }

        if (clip.locked) {
            return false;
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

int TimelineWidget::trackCount() const {
    int maxTrack = -1;
    for (const TimelineClip& clip : m_clips) {
        maxTrack = qMax(maxTrack, clip.trackIndex);
    }
    return qMax(1, qMax(m_tracks.size(), maxTrack + 1));
}

int TimelineWidget::nextTrackIndex() const {
    return trackCount();
}

void TimelineWidget::normalizeTrackIndices() {
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

void TimelineWidget::ensureTrackCount(int count) {
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

QString TimelineWidget::defaultTrackName(int trackIndex) const {
    return QStringLiteral("Track %1").arg(trackIndex + 1);
}

int TimelineWidget::trackTop(int trackIndex) const {
    const QRect tracks = trackRect();
    int y = tracks.top() + kTimelineTrackInnerPadding - m_verticalScrollOffset;
    for (int i = 0; i < trackIndex && i < m_tracks.size(); ++i) {
        y += trackHeight(i) + kTimelineTrackSpacing;
    }
    return y;
}

int TimelineWidget::trackHeight(int trackIndex) const {
    if (trackIndex >= 0 && trackIndex < m_tracks.size()) {
        return qMax(kMinTrackHeight, m_tracks[trackIndex].height);
    }
    return kDefaultTrackHeight;
}

int TimelineWidget::totalTrackAreaHeight() const {
    int trackAreaHeight = kTimelineTrackInnerPadding * 2;
    for (int i = 0; i < trackCount(); ++i) {
        trackAreaHeight += trackHeight(i);
    }
    trackAreaHeight += qMax(0, trackCount() - 1) * kTimelineTrackSpacing;
    return trackAreaHeight;
}

int TimelineWidget::maxVerticalScrollOffset() const {
    return qMax(0, totalTrackAreaHeight() - trackRect().height());
}

void TimelineWidget::setVerticalScrollOffset(int offset) {
    const int bounded = qBound(0, offset, maxVerticalScrollOffset());
    if (m_verticalScrollOffset == bounded) {
        return;
    }
    m_verticalScrollOffset = bounded;
    update();
}

void TimelineWidget::setTimelineZoom(qreal pixelsPerFrame) {
    const QRect contentRect = timelineContentRect();
    const int64_t fullTimelineFrames = qMax<int64_t>(1, totalFrames());
    const qreal fitAllPixelsPerFrame =
        contentRect.width() > 0
            ? static_cast<qreal>(contentRect.width()) / static_cast<qreal>(fullTimelineFrames)
            : 0.01;
    const qreal minPixelsPerFrame = qMin<qreal>(0.25, fitAllPixelsPerFrame);
    const qreal bounded = qBound(minPixelsPerFrame, pixelsPerFrame, 24.0);
    if (qFuzzyCompare(m_pixelsPerFrame, bounded)) {
        return;
    }
    m_pixelsPerFrame = bounded;
    const int64_t visibleFrames = qMax<int64_t>(1, qRound(static_cast<qreal>(contentRect.width()) / m_pixelsPerFrame));
    const int64_t maxOffset = qMax<int64_t>(0, totalFrames() - visibleFrames);
    m_frameOffset = qBound<int64_t>(0, m_frameOffset, maxOffset);
    if (m_pixelsPerFrame <= fitAllPixelsPerFrame + 0.0001) {
        m_frameOffset = 0;
    }
    update();
}

int TimelineWidget::trackIndexAtY(int y, bool allowAppendTrack) const {
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

int TimelineWidget::trackDropTargetAtY(int y, bool* insertsTrack) const {
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

int TimelineWidget::trackDividerAt(const QPoint& pos) const {
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

void TimelineWidget::updateMinimumTimelineHeight() {
    setMinimumHeight(150);
    m_verticalScrollOffset = qBound(0, m_verticalScrollOffset, maxVerticalScrollOffset());
}

void TimelineWidget::insertTrackAt(int trackIndex) {
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

QRect TimelineWidget::drawRect() const {
    return rect().adjusted(kTimelineOuterMargin, kTimelineOuterMargin,
                           -kTimelineOuterMargin, -kTimelineOuterMargin);
}

QRect TimelineWidget::topBarRect() const {
    const QRect draw = drawRect();
    return QRect(draw.left(), draw.top(), draw.width(), kTimelineTopBarHeight);
}

QRect TimelineWidget::rulerRect() const {
    const QRect draw = drawRect();
    const QRect topBar = topBarRect();
    return QRect(draw.left(), topBar.bottom() + 8, draw.width(), kTimelineRulerHeight);
}

QRect TimelineWidget::trackRect() const {
    const QRect draw = drawRect();
    const QRect ruler = rulerRect();
    return QRect(draw.left(), ruler.bottom() + kTimelineTrackGap, draw.width(),
                 draw.height() - ((ruler.bottom() - draw.top() + 1) + kTimelineTrackGap));
}

QRect TimelineWidget::timelineContentRect() const {
    const QRect tracks = trackRect();
    return QRect(tracks.left() + kTimelineLabelWidth + kTimelineLabelGap,
                 tracks.top(),
                 qMax(0, tracks.width() - kTimelineLabelWidth - kTimelineLabelGap),
                 tracks.height());
}

QRect TimelineWidget::exportRangeRect() const {
    const QRect topBar = topBarRect();
    const QRect content = timelineContentRect();
    return QRect(content.left(), topBar.top() + 26, content.width(), 20);
}

QRect TimelineWidget::exportSegmentRect(const ExportRangeSegment& segment) const {
    const QRect bar = exportRangeRect();
    const int left = xFromFrame(segment.startFrame);
    const int right = xFromFrame(segment.endFrame);
    return QRect(qMin(left, right),
                 bar.top(),
                 qMax(6, qAbs(right - left)),
                 bar.height());
}

QRect TimelineWidget::exportHandleRect(int segmentIndex, bool startHandle) const {
    if (segmentIndex < 0 || segmentIndex >= m_exportRanges.size()) {
        return QRect();
    }
    const QRect bar = exportRangeRect();
    const int64_t frame = startHandle ? m_exportRanges[segmentIndex].startFrame : m_exportRanges[segmentIndex].endFrame;
    const int x = xFromFrame(frame);
    return QRect(x - 5, bar.top() - 1, 10, bar.height() + 2);
}

QRect TimelineWidget::trackLabelRect(int trackIndex) const {
    return QRect(trackRect().left() + 6,
                 trackTop(trackIndex) + qMax(0, (trackHeight(trackIndex) - kTimelineClipHeight) / 2),
                 kTimelineLabelWidth - 12,
                 kTimelineClipHeight);
}

QRect TimelineWidget::clipRectFor(const TimelineClip& clip) const {
    const qreal clipFrame = samplesToFramePosition(clipTimelineStartSamples(clip));
    const qreal visibleFrame = clipFrame - static_cast<qreal>(m_frameOffset);
    const int clipX = timelineContentRect().left() + qRound(visibleFrame * m_pixelsPerFrame);
    const int clipW = qMax(40, widthForFrames(clip.durationFrames));
    const int visualHeight =
        qMax(kTimelineClipHeight, trackHeight(clip.trackIndex) - (kTimelineClipVerticalPadding * 2));
    const int clipY = trackTop(clip.trackIndex) + qMax(0, (trackHeight(clip.trackIndex) - visualHeight) / 2);
    return QRect(clipX, clipY, clipW, visualHeight);
}

QRect TimelineWidget::renderSyncMarkerRect(const TimelineClip& clip, const RenderSyncMarker& marker) const {
    const QRect clipRect = clipRectFor(clip);
    const int left = xFromFrame(marker.frame);
    const int right = xFromFrame(marker.frame + 1);
    const int width = qMax(6, right - left);
    return QRect(left,
                 clipRect.top() + 2,
                 qMin(width, qMax(6, clipRect.right() - left)),
                 clipRect.height() - 4);
}

const RenderSyncMarker* TimelineWidget::renderSyncMarkerAtPos(const QPoint& pos, int* clipIndexOut) const {
    for (int i = m_clips.size() - 1; i >= 0; --i) {
        const TimelineClip& clip = m_clips[i];
        for (const RenderSyncMarker& marker : m_renderSyncMarkers) {
            if (marker.clipId != clip.id) {
                continue;
            }
            if (renderSyncMarkerRect(clip, marker).contains(pos)) {
                if (clipIndexOut) {
                    *clipIndexOut = i;
                }
                return &marker;
            }
        }
    }
    if (clipIndexOut) {
        *clipIndexOut = -1;
    }
    return nullptr;
}

void TimelineWidget::openRenderSyncMarkerMenu(const QPoint& globalPos, const QString& clipId) {
    const RenderSyncMarker* currentSyncMarker = renderSyncMarkerAtFrame(clipId, m_currentFrame);
    QMenu menu(this);
    QAction* duplicateRenderFrameAction =
        menu.addAction(QStringLiteral("Render Sync: Duplicate Frames For Clip..."));
    QAction* skipRenderFrameAction =
        menu.addAction(QStringLiteral("Render Sync: Skip Frames For Clip..."));
    QAction* clearRenderSyncAction = menu.addAction(QStringLiteral("Clear Clip Render Sync At Playhead"));
    clearRenderSyncAction->setEnabled(currentSyncMarker != nullptr);

    QAction* selected = menu.exec(globalPos);
    if (!selected) {
        return;
    }

    if (selected == duplicateRenderFrameAction) {
        const int defaultCount =
            currentSyncMarker && currentSyncMarker->action == RenderSyncAction::DuplicateFrame ? currentSyncMarker->count : 1;
        bool ok = false;
        const int count = QInputDialog::getInt(this,
                                               QStringLiteral("Duplicate Frames"),
                                               QStringLiteral("How many extra frames should be duplicated for this clip?"),
                                               defaultCount,
                                               1,
                                               120,
                                               1,
                                               &ok);
        if (ok) {
            setRenderSyncMarkerAtCurrentFrame(clipId, RenderSyncAction::DuplicateFrame, count);
        }
        return;
    }

    if (selected == skipRenderFrameAction) {
        const int defaultCount =
            currentSyncMarker && currentSyncMarker->action == RenderSyncAction::SkipFrame ? currentSyncMarker->count : 1;
        bool ok = false;
        const int count = QInputDialog::getInt(this,
                                               QStringLiteral("Skip Frames"),
                                               QStringLiteral("How many frames should be skipped for this clip?"),
                                               defaultCount,
                                               1,
                                               120,
                                               1,
                                               &ok);
        if (ok) {
            setRenderSyncMarkerAtCurrentFrame(clipId, RenderSyncAction::SkipFrame, count);
        }
        return;
    }

    if (selected == clearRenderSyncAction) {
        clearRenderSyncMarkerAtCurrentFrame(clipId);
    }
}

int TimelineWidget::trackIndexAt(const QPoint& pos) const {
    for (int i = 0; i < trackCount(); ++i) {
        if (trackLabelRect(i).contains(pos)) {
            return i;
        }
    }
    return -1;
}

int TimelineWidget::clipIndexAt(const QPoint& pos) const {
    for (int i = 0; i < m_clips.size(); ++i) {
        if (clipRectFor(m_clips[i]).contains(pos)) {
            return i;
        }
    }
    return -1;
}

TimelineWidget::ClipDragMode TimelineWidget::clipDragModeAt(const TimelineClip& clip, const QPoint& pos) const {
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

void TimelineWidget::updateHoverCursor(const QPoint& pos) {
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

bool TimelineWidget::clipHasProxyAvailable(const TimelineClip& clip) const {
    if (clip.mediaType != ClipMediaType::Video || isImageSequencePath(clip.filePath)) {
        return false;
    }
    return !playbackProxyPathForClip(clip).isEmpty();
}

int64_t TimelineWidget::frameFromX(qreal x) const {
    const int left = timelineContentRect().left();
    const qreal normalized = qMax<qreal>(0.0, x - left);
    return m_frameOffset + static_cast<int64_t>(normalized / m_pixelsPerFrame);
}

int TimelineWidget::xFromFrame(int64_t frame) const {
    return timelineContentRect().left() + widthForFrames(frame - m_frameOffset);
}

int TimelineWidget::widthForFrames(int64_t frames) const {
    return static_cast<int>(frames * m_pixelsPerFrame);
}

QString TimelineWidget::timecodeForFrame(int64_t frame) const {
    const int64_t seconds = frame / kTimelineFps;
    const int64_t minutes = seconds / 60;
    const int64_t secs = seconds % 60;
    return QStringLiteral("%1:%2")
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(secs, 2, 10, QLatin1Char('0'));
}

int64_t TimelineWidget::mediaDurationFrames(const QFileInfo& info) const {
    const QString suffix = info.suffix().toLower();
    return probeMediaFile(info.absoluteFilePath(), guessDurationFrames(suffix)).durationFrames;
}

bool TimelineWidget::hasFileUrls(const QMimeData* mimeData) const {
    if (!mimeData || !mimeData->hasUrls()) return false;
    for (const QUrl& url : mimeData->urls()) {
        if (url.isLocalFile()) return true;
    }
    return false;
}

int64_t TimelineWidget::guessDurationFrames(const QString& suffix) const {
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

QColor TimelineWidget::colorForPath(const QString& path) const {
    const quint32 hash = qHash(path);
    return QColor::fromHsv(static_cast<int>(hash % 360), 160, 220, 220);
}

TimelineClip TimelineWidget::buildClipFromFile(const QString& filePath,
                                               int64_t startFrame,
                                               int trackIndex) const {
    const QFileInfo info(filePath);
    const MediaProbeResult probe = probeMediaFile(filePath, guessDurationFrames(info.suffix().toLower()));

    TimelineClip clip;
    clip.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    clip.filePath = filePath;
    clip.label = isImageSequencePath(filePath) ? imageSequenceDisplayLabel(filePath) : info.fileName();
    clip.mediaType = probe.mediaType;
    clip.sourceKind = probe.sourceKind;
    clip.hasAudio = probe.hasAudio;
    clip.sourceDurationFrames = probe.durationFrames;
    clip.startFrame = startFrame;
    clip.durationFrames = probe.durationFrames;
    clip.trackIndex = trackIndex;
    clip.color = colorForPath(filePath);
    if (clip.mediaType == ClipMediaType::Audio) {
        clip.color = QColor(QStringLiteral("#2f7f93"));
    }
    normalizeClipTransformKeyframes(clip);
    return clip;
}

void TimelineWidget::addClipFromFile(const QString& filePath, int64_t startFrame) {
    const QFileInfo info(filePath);
    if (!info.exists() || (!info.isFile() && !isImageSequencePath(filePath))) return;

    TimelineClip clip = buildClipFromFile(filePath,
                                          startFrame >= 0 ? startFrame : totalFrames(),
                                          nextTrackIndex());
    m_clips.push_back(clip);
    normalizeTrackIndices();
    sortClips();

    if (clipsChanged) clipsChanged();
    update();
}

void TimelineWidget::dragEnterEvent(QDragEnterEvent* event) {
    if (hasFileUrls(event->mimeData())) {
        event->acceptProposedAction();
        return;
    }
    QWidget::dragEnterEvent(event);
}

void TimelineWidget::dragMoveEvent(QDragMoveEvent* event) {
    if (hasFileUrls(event->mimeData())) {
        m_dropFrame = frameFromX(event->position().x());
        m_trackDropIndex = trackDropTargetAtY(event->position().toPoint().y(), &m_trackDropInGap);
        event->acceptProposedAction();
        update();
        return;
    }
    QWidget::dragMoveEvent(event);
}

void TimelineWidget::dragLeaveEvent(QDragLeaveEvent* event) {
    m_dropFrame = -1;
    m_trackDropIndex = -1;
    m_trackDropInGap = false;
    m_snapIndicatorFrame = -1;
    QWidget::dragLeaveEvent(event);
    update();
}

void TimelineWidget::dropEvent(QDropEvent* event) {
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
        if (!info.exists() || (info.isDir() && !isImageSequencePath(filePath))) continue;

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

void TimelineWidget::mouseDoubleClickEvent(QMouseEvent* event) {
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

void TimelineWidget::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        int markerClipIndex = -1;
        if (const RenderSyncMarker* marker = renderSyncMarkerAtPos(event->position().toPoint(), &markerClipIndex)) {
            if (markerClipIndex >= 0) {
                setSelectedClipId(m_clips[markerClipIndex].id);
                m_currentFrame = marker->frame;
                if (seekRequested) seekRequested(marker->frame);
                openRenderSyncMarkerMenu(event->globalPosition().toPoint(), marker->clipId);
                update();
                event->accept();
                return;
            }
        }
        bool startHandle = false;
        const int exportHandleSegment = exportHandleAtPos(event->position().toPoint(), &startHandle);
        if (exportHandleSegment >= 0) {
            m_exportRangeDragSegmentIndex = exportHandleSegment;
            m_exportRangeDragMode = startHandle ? ExportRangeDragMode::Start : ExportRangeDragMode::End;
            event->accept();
            return;
        }
        if (exportRangeRect().contains(event->position().toPoint())) {
            const int64_t frame = frameFromX(event->position().x());
            if (exportSegmentIndexAtFrame(frame) >= 0) {
                m_currentFrame = frame;
                if (seekRequested) seekRequested(frame);
                update();
                event->accept();
                return;
            }
        }
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
            if (!m_clips[hitIndex].locked) {
                m_dragMode = clipDragModeAt(m_clips[hitIndex], event->position().toPoint());
                m_draggedClipIndex = hitIndex;
                m_dragOriginalStartFrame = m_clips[hitIndex].startFrame;
                m_dragOriginalDurationFrames = m_clips[hitIndex].durationFrames;
                m_dragOriginalSourceInFrame = m_clips[hitIndex].sourceInFrame;
                m_dragOriginalTransformKeyframes = m_clips[hitIndex].transformKeyframes;
                m_dragOffsetFrames = frameFromX(event->position().x()) - m_clips[hitIndex].startFrame;
            }
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

void TimelineWidget::mouseMoveEvent(QMouseEvent* event) {
    const int hoveredClipIndex = clipIndexAt(event->position().toPoint());
    const QString hoveredClipId =
        hoveredClipIndex >= 0 ? m_clips[hoveredClipIndex].id : QString();
    if (hoveredClipId != m_hoveredClipId) {
        m_hoveredClipId = hoveredClipId;
        update();
    }
    if (hoveredClipIndex >= 0) {
        const TimelineClip& hoveredClip = m_clips[hoveredClipIndex];
        const bool isSequence = hoveredClip.sourceKind == MediaSourceKind::ImageSequence;
        const QString typeLabel =
            hoveredClip.mediaType == ClipMediaType::Audio ? QStringLiteral("Audio")
            : (hoveredClip.mediaType == ClipMediaType::Image ? QStringLiteral("Image")
               : (isSequence ? QStringLiteral("Sequence") : QStringLiteral("Video")));
        const int64_t localTimelineFrame =
            qBound<int64_t>(0,
                            m_currentFrame - hoveredClip.startFrame,
                            qMax<int64_t>(0, hoveredClip.durationFrames - 1));
        const int64_t clipFrame =
            adjustedClipLocalFrameAtTimelineFrame(hoveredClip, localTimelineFrame, m_renderSyncMarkers);
        setToolTip(QStringLiteral("%1\n%2\nFrame %3")
                       .arg(hoveredClip.label, typeLabel)
                       .arg(clipFrame));
    } else {
        setToolTip(QString());
    }

    if (m_exportRangeDragMode != ExportRangeDragMode::None && (event->buttons() & Qt::LeftButton)) {
        if (m_exportRangeDragSegmentIndex < 0 || m_exportRangeDragSegmentIndex >= m_exportRanges.size()) {
            m_exportRangeDragMode = ExportRangeDragMode::None;
            m_exportRangeDragSegmentIndex = -1;
            return;
        }
        const int64_t frame = qBound<int64_t>(0, frameFromX(event->position().x()), totalFrames());
        if (m_exportRangeDragMode == ExportRangeDragMode::Start) {
            m_exportRanges[m_exportRangeDragSegmentIndex].startFrame =
                qMin(frame, m_exportRanges[m_exportRangeDragSegmentIndex].endFrame);
        } else {
            m_exportRanges[m_exportRangeDragSegmentIndex].endFrame =
                qMax(frame, m_exportRanges[m_exportRangeDragSegmentIndex].startFrame);
        }
        normalizeExportRanges();
        if (exportRangeChanged) {
            exportRangeChanged();
        }
        update();
        return;
    }
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

void TimelineWidget::leaveEvent(QEvent* event) {
    if (!m_hoveredClipId.isEmpty()) {
        m_hoveredClipId.clear();
        update();
    }
    setToolTip(QString());
    QWidget::leaveEvent(event);
}

void TimelineWidget::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && m_exportRangeDragMode != ExportRangeDragMode::None) {
        m_exportRangeDragMode = ExportRangeDragMode::None;
        m_exportRangeDragSegmentIndex = -1;
        update();
        return;
    }
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

void TimelineWidget::contextMenuEvent(QContextMenuEvent* event) {
    const int clipIndex = clipIndexAt(event->pos());
    int markerClipIndex = -1;
    const RenderSyncMarker* clickedMarker = renderSyncMarkerAtPos(event->pos(), &markerClipIndex);
    if (clickedMarker && markerClipIndex >= 0) {
        setSelectedClipId(m_clips[markerClipIndex].id);
        m_currentFrame = clickedMarker->frame;
        if (seekRequested) {
            seekRequested(clickedMarker->frame);
        }
        openRenderSyncMarkerMenu(event->globalPos(), clickedMarker->clipId);
        return;
    }
    const QString targetClipId =
        clipIndex >= 0 ? m_clips[clipIndex].id : m_selectedClipId;
    QMenu menu(this);
    QAction* setExportStartAction = menu.addAction(QStringLiteral("Set Export Start At Playhead"));
    QAction* setExportEndAction = menu.addAction(QStringLiteral("Set Export End At Playhead"));
    QAction* splitExportRangeAction = menu.addAction(QStringLiteral("Split Export Range At Playhead"));
    QAction* resetExportRangeAction = menu.addAction(QStringLiteral("Reset Export Range"));
    menu.addSeparator();
    QAction* duplicateRenderFrameAction =
        menu.addAction(QStringLiteral("Render Sync: Duplicate Frames For Clip..."));
    QAction* skipRenderFrameAction =
        menu.addAction(QStringLiteral("Render Sync: Skip Frames For Clip..."));
    QAction* clearRenderSyncAction = menu.addAction(QStringLiteral("Clear Clip Render Sync At Playhead"));
    const RenderSyncMarker* currentSyncMarker = renderSyncMarkerAtFrame(targetClipId, m_currentFrame);
    const bool hasTargetClip = !targetClipId.isEmpty();
    const int splitSegmentIndex = exportSegmentIndexAtFrame(m_currentFrame);
    splitExportRangeAction->setEnabled(
        splitSegmentIndex >= 0 &&
        m_currentFrame > m_exportRanges[splitSegmentIndex].startFrame &&
        m_currentFrame <= m_exportRanges[splitSegmentIndex].endFrame);
    duplicateRenderFrameAction->setEnabled(hasTargetClip);
    skipRenderFrameAction->setEnabled(hasTargetClip);
    clearRenderSyncAction->setEnabled(currentSyncMarker != nullptr);

    QAction* nudgeLeftAction = nullptr;
    QAction* nudgeRightAction = nullptr;
    QAction* deleteAction = nullptr;
    QAction* gradingAction = nullptr;
    QAction* resetGradingAction = nullptr;
    QAction* propertiesAction = nullptr;
    QAction* transcribeAction = nullptr;
    QAction* createProxyAction = nullptr;

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
        transcribeAction = menu.addAction(QStringLiteral("Transcribe"));
        const QString detectedProxyPath = playbackProxyPathForClip(m_clips[clipIndex]);
        createProxyAction = menu.addAction(
            detectedProxyPath.isEmpty() ? QStringLiteral("Create Proxy...")
                                        : QStringLiteral("Recreate Proxy..."));
        createProxyAction->setEnabled(
            m_clips[clipIndex].mediaType == ClipMediaType::Video &&
            !isImageSequencePath(m_clips[clipIndex].filePath));
        propertiesAction = menu.addAction(QStringLiteral("Properties"));
        menu.addSeparator();
    }

    QAction* lockAction = nullptr;
    QAction* unlockAction = nullptr;
    if (clipIndex >= 0) {
        if (m_clips[clipIndex].locked) {
            unlockAction = menu.addAction(QStringLiteral("Unlock"));
        } else {
            lockAction = menu.addAction(QStringLiteral("Lock"));
        }
    }

    QAction* selected = menu.exec(event->globalPos());
    if (!selected) return;

    if (selected == setExportStartAction) {
        if (m_exportRanges.isEmpty()) {
            m_exportRanges = {ExportRangeSegment{0, totalFrames()}};
        }
        m_exportRanges.first().startFrame = qMin(m_currentFrame, m_exportRanges.first().endFrame);
        normalizeExportRanges();
        if (exportRangeChanged) {
            exportRangeChanged();
        }
        update();
        return;
    }

    if (selected == setExportEndAction) {
        if (m_exportRanges.isEmpty()) {
            m_exportRanges = {ExportRangeSegment{0, totalFrames()}};
        }
        m_exportRanges.last().endFrame = qMax(m_currentFrame, m_exportRanges.last().startFrame);
        normalizeExportRanges();
        if (exportRangeChanged) {
            exportRangeChanged();
        }
        update();
        return;
    }

    if (selected == splitExportRangeAction) {
        if (splitSegmentIndex < 0 || splitSegmentIndex >= m_exportRanges.size()) {
            return;
        }
        const ExportRangeSegment segment = m_exportRanges[splitSegmentIndex];
        if (m_currentFrame <= segment.startFrame || m_currentFrame > segment.endFrame) {
            return;
        }
        ExportRangeSegment left = segment;
        ExportRangeSegment right = segment;
        left.endFrame = m_currentFrame - 1;
        right.startFrame = m_currentFrame;
        m_exportRanges.removeAt(splitSegmentIndex);
        m_exportRanges.insert(splitSegmentIndex, right);
        m_exportRanges.insert(splitSegmentIndex, left);
        normalizeExportRanges();
        if (exportRangeChanged) {
            exportRangeChanged();
        }
        update();
        return;
    }

    if (selected == resetExportRangeAction) {
        m_exportRanges = {ExportRangeSegment{0, totalFrames()}};
        normalizeExportRanges();
        if (exportRangeChanged) {
            exportRangeChanged();
        }
        update();
        return;
    }

    if (selected == duplicateRenderFrameAction || selected == skipRenderFrameAction) {
        if (hasTargetClip) {
            openRenderSyncMarkerMenu(event->globalPos(), targetClipId);
        }
        return;
    }

    if (selected == clearRenderSyncAction) {
        clearRenderSyncMarkerAtCurrentFrame(targetClipId);
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
        if (clipIndex >= 0) {
            setSelectedClipId(m_clips[clipIndex].id);
            deleteSelectedClip();
        }
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

    if (selected == transcribeAction) {
        if (transcribeRequested) {
            const TimelineClip& clip = m_clips[clipIndex];
            transcribeRequested(clip.filePath, clip.label);
        }
        return;
    }

    if (selected == createProxyAction) {
        if (createProxyRequested && clipIndex >= 0) {
            createProxyRequested(m_clips[clipIndex].id);
        }
        return;
    }

    if (selected == propertiesAction) {
        const TimelineClip& clip = m_clips[clipIndex];
        QMessageBox::information(this, QStringLiteral("Clip Properties"),
            QStringLiteral("Name: %1\nPath: %2\nProxy: %3\nType: %4\nSource Kind: %5\nStart: %6\nSource In: %7\nDuration: %8 frames\nAudio start offset: %9 ms\nBrightness: %10\nContrast: %11\nSaturation: %12\nOpacity: %13\nLocked: %14")
                .arg(clip.label)
                .arg(QDir::toNativeSeparators(clip.filePath))
                .arg(playbackProxyPathForClip(clip).isEmpty() ? QStringLiteral("None")
                                                               : QDir::toNativeSeparators(playbackProxyPathForClip(clip)))
                .arg(clipMediaTypeLabel(clip.mediaType))
                .arg(mediaSourceKindLabel(clip.sourceKind))
                .arg(timecodeForFrame(clip.startFrame))
                .arg(timecodeForFrame(clip.sourceInFrame))
                .arg(clip.durationFrames)
                .arg(qRound64((clip.startSubframeSamples * 1000.0) / kAudioSampleRate))
                .arg(clip.brightness, 0, 'f', 2)
                .arg(clip.contrast, 0, 'f', 2)
                .arg(clip.saturation, 0, 'f', 2)
                .arg(clip.opacity, 0, 'f', 2)
                .arg(clip.locked ? QStringLiteral("Yes") : QStringLiteral("No")));
        return;
    }

    if (selected == lockAction) {
        m_clips[clipIndex].locked = true;
        update();
        return;
    }

    if (selected == unlockAction) {
        m_clips[clipIndex].locked = false;
        update();
        return;
    }
}

void TimelineWidget::wheelEvent(QWheelEvent* event) {
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

    if (event->modifiers() & Qt::AltModifier) {
        const qreal zoomFactor = steps > 0 ? 1.12 : (1.0 / 1.12);
        bool changed = false;
        for (TimelineTrack& track : m_tracks) {
            const int oldHeight = track.height;
            track.height = qBound(kMinTrackHeight,
                                  qRound(track.height * std::pow(zoomFactor, std::abs(steps))),
                                  240);
            changed = changed || track.height != oldHeight;
        }
        if (changed) {
            updateMinimumTimelineHeight();
            if (clipsChanged) {
                clipsChanged();
            }
            update();
        }
        event->accept();
        return;
    }

    if (event->modifiers() & Qt::ShiftModifier) {
        const int visibleFrames = qMax(1, static_cast<int>(width() / m_pixelsPerFrame));
        const int panFrames = qMax(1, visibleFrames / 12);
        m_frameOffset = qMax<int64_t>(0, m_frameOffset - steps * panFrames);
        update();
        event->accept();
        return;
    }

    const bool overTrackLabels = trackRect().contains(event->position().toPoint()) &&
                                 !timelineContentRect().contains(event->position().toPoint());

    if ((event->modifiers() & Qt::ControlModifier) || !overTrackLabels) {
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

        const qreal localX = event->position().x() - static_cast<qreal>(contentRect.left());
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
        return;
    }

    setVerticalScrollOffset(m_verticalScrollOffset - (steps * 36));
    event->accept();
}

void TimelineWidget::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.fillRect(rect(), QColor(QStringLiteral("#0f1216")));

    const QRect draw = drawRect();
    const QRect topBar = topBarRect();
    const QRect ruler = rulerRect();
    const QRect tracks = trackRect();
    const QRect content = timelineContentRect();
    const QRect exportBar = exportRangeRect();

    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(QStringLiteral("#171c22")));
    painter.drawRoundedRect(topBar, 10, 10);

    const QRect frameInfoRect(draw.left() + 10, topBar.top() + 4, kTimelineLabelWidth + 44, 18);
    painter.setBrush(QColor(QStringLiteral("#202a34")));
    painter.drawRoundedRect(exportBar, 7, 7);

    painter.setBrush(QColor(QStringLiteral("#4ea1ff")));
    for (const ExportRangeSegment& segment : m_exportRanges) {
        painter.drawRoundedRect(exportSegmentRect(segment), 7, 7);
    }
    painter.setBrush(QColor(QStringLiteral("#fff4c2")));
    for (int i = 0; i < m_exportRanges.size(); ++i) {
        painter.drawRoundedRect(exportHandleRect(i, true), 3, 3);
        painter.drawRoundedRect(exportHandleRect(i, false), 3, 3);
    }
    painter.setPen(QColor(QStringLiteral("#0f1216")));
    QString exportLabel;
    for (int i = 0; i < m_exportRanges.size(); ++i) {
        if (i > 0) {
            exportLabel += QStringLiteral(" | ");
        }
        exportLabel += QStringLiteral("%1 -> %2")
                           .arg(timecodeForFrame(m_exportRanges[i].startFrame))
                           .arg(timecodeForFrame(m_exportRanges[i].endFrame));
    }
    painter.drawText(exportBar.adjusted(10, 0, -10, 0),
                     Qt::AlignCenter,
                     QStringLiteral("Export %1").arg(exportLabel));

    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(QStringLiteral("#202a34")));
    painter.drawRoundedRect(frameInfoRect, 7, 7);
    painter.setPen(QColor(QStringLiteral("#eef4fa")));
    painter.drawText(frameInfoRect.adjusted(8, 0, -8, 0),
                     Qt::AlignLeft | Qt::AlignVCenter,
                     QStringLiteral("Frame %1").arg(m_currentFrame));

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
        const bool hovered = clip.id == m_hoveredClipId;
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
        QString clipTitle = audioOnly
            ? QStringLiteral("AUDIO  %1").arg(clip.label)
            : clip.label;
        if (clip.locked) {
            clipTitle = QStringLiteral("🔒 %1").arg(clipTitle);
        }
        painter.drawText(clipRect.adjusted(10, 0, -10, 0),
                        Qt::AlignLeft | Qt::AlignVCenter,
                        painter.fontMetrics().elidedText(clipTitle, Qt::ElideRight,
                                                         clipRect.width() - 20));

        if (hovered) {
            const bool isSequence = clip.sourceKind == MediaSourceKind::ImageSequence;
            const bool hasProxy = clipHasProxyAvailable(clip);
            QString badgeText;
            if (clip.mediaType == ClipMediaType::Audio) {
                badgeText = QStringLiteral("AUDIO");
            } else if (clip.mediaType == ClipMediaType::Image) {
                badgeText = QStringLiteral("IMAGE");
            } else if (isSequence) {
                badgeText = QStringLiteral("SEQUENCE");
            } else {
                badgeText = hasProxy ? QStringLiteral("PROXY")
                                     : QStringLiteral("NEEDS PROXY");
            }
            const QFontMetrics badgeMetrics = painter.fontMetrics();
            const int badgeWidth = badgeMetrics.horizontalAdvance(badgeText) + 18;
            const int badgeHeight = 18;
            const QRect badgeRect(clipRect.right() - badgeWidth - 8,
                                  clipRect.top() + 7,
                                  badgeWidth,
                                  badgeHeight);
            painter.setPen(Qt::NoPen);
            painter.setBrush(clip.mediaType == ClipMediaType::Audio
                                 ? QColor(QStringLiteral("#1f3e4c"))
                                 : (clip.mediaType == ClipMediaType::Image
                                 ? QColor(QStringLiteral("#3a2c12"))
                                 : (isSequence ? QColor(QStringLiteral("#1f2f5a"))
                                               : (hasProxy ? QColor(QStringLiteral("#113c28"))
                                                           : QColor(QStringLiteral("#4a3113"))))));
            painter.drawRoundedRect(badgeRect, 9, 9);
            painter.setPen(clip.mediaType == ClipMediaType::Audio
                               ? QColor(QStringLiteral("#cfefff"))
                               : (clip.mediaType == ClipMediaType::Image
                               ? QColor(QStringLiteral("#ffe4a8"))
                               : (isSequence ? QColor(QStringLiteral("#c9d9ff"))
                                             : (hasProxy ? QColor(QStringLiteral("#b8f5cf"))
                                                         : QColor(QStringLiteral("#ffe1a8"))))));
            painter.drawText(badgeRect, Qt::AlignCenter, badgeText);

            const int64_t localTimelineFrame =
                qBound<int64_t>(0,
                                m_currentFrame - clip.startFrame,
                                qMax<int64_t>(0, clip.durationFrames - 1));
            const int64_t clipFrame =
                adjustedClipLocalFrameAtTimelineFrame(clip, localTimelineFrame, m_renderSyncMarkers);
            const QString frameBadgeText = QStringLiteral("FRAME %1").arg(clipFrame);
            const int frameBadgeWidth = badgeMetrics.horizontalAdvance(frameBadgeText) + 18;
            const QRect frameBadgeRect(clipRect.right() - frameBadgeWidth - 8,
                                       badgeRect.bottom() + 6,
                                       frameBadgeWidth,
                                       badgeHeight);
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(QStringLiteral("#18303e")));
            painter.drawRoundedRect(frameBadgeRect, 9, 9);
            painter.setPen(QColor(QStringLiteral("#d7f2ff")));
            painter.drawText(frameBadgeRect, Qt::AlignCenter, frameBadgeText);
        }

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
    if (const RenderSyncMarker* marker = renderSyncMarkerAtFrame(m_selectedClipId, m_currentFrame)) {
        const QString label = marker->action == RenderSyncAction::DuplicateFrame
            ? QStringLiteral("DUP %1").arg(marker->count)
            : QStringLiteral("SKIP %1").arg(marker->count);
        const QRect badgeRect(playheadX + 10, ruler.top() - 2, 58, 16);
        painter.setBrush(marker->action == RenderSyncAction::DuplicateFrame
                             ? QColor(QStringLiteral("#ff5b5b"))
                             : QColor(QStringLiteral("#ff9e3d")));
        painter.drawRoundedRect(badgeRect, 6, 6);
        painter.setPen(QColor(QStringLiteral("#ffffff")));
        painter.drawText(badgeRect, Qt::AlignCenter, label);
    }

    for (const TimelineClip& clip : m_clips) {
        for (const RenderSyncMarker& marker : m_renderSyncMarkers) {
            if (marker.clipId != clip.id ||
                marker.frame < clip.startFrame ||
                marker.frame >= clip.startFrame + clip.durationFrames) {
                continue;
            }
            const QRect markerRect = renderSyncMarkerRect(clip, marker);
            const QColor markerColor = marker.action == RenderSyncAction::DuplicateFrame
                ? QColor(QStringLiteral("#ff5b5b"))
                : QColor(QStringLiteral("#ff9e3d"));
            painter.setPen(QPen(markerColor.darker(135), 1));
            painter.setBrush(QColor(markerColor.red(), markerColor.green(), markerColor.blue(), 230));
            painter.drawRoundedRect(markerRect, 4, 4);
        }
    }
}

void TimelineWidget::setExportRange(int64_t startFrame, int64_t endFrame) {
    setExportRanges({ExportRangeSegment{startFrame, endFrame}});
}

void TimelineWidget::setExportRanges(const QVector<ExportRangeSegment>& ranges) {
    m_exportRanges = ranges;
    normalizeExportRanges();
    update();
}
