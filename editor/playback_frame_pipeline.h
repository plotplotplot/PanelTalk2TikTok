#pragma once

#include "async_decoder.h"
#include "editor_shared.h"
#include "frame_handle.h"

#include <QObject>
#include <QHash>
#include <QMutex>
#include <QSet>
#include <QVector>
#include <functional>

namespace editor {

class PlaybackFramePipeline : public QObject {
    Q_OBJECT

public:
    explicit PlaybackFramePipeline(AsyncDecoder* decoder, QObject* parent = nullptr);
    ~PlaybackFramePipeline() override;

    void setTimelineClips(const QVector<TimelineClip>& clips);
    void setRenderSyncMarkers(const QVector<RenderSyncMarker>& markers);
    void setPlaybackActive(bool active);
    void setPlayheadFrame(int64_t playheadFrame);

    void requestFramesForSample(int64_t samplePosition, const std::function<void()>& onFrameReady);

    FrameHandle getFrame(const QString& clipId, int64_t frameNumber) const;
    FrameHandle getBestFrame(const QString& clipId, int64_t frameNumber) const;
    FrameHandle getPresentationFrame(const QString& clipId, int64_t frameNumber) const;
    bool isFrameBuffered(const QString& clipId, int64_t frameNumber) const;

    int pendingVisibleRequestCount() const;
    int bufferedFrameCount() const;
    int droppedPresentationFrameCount() const { return m_droppedPresentationFrames.load(); }

signals:
    void frameAvailable();

private slots:
    void onFrameReady(FrameHandle frame);

private:
    struct ClipInfo {
        TimelineClip clip;
        QString playbackPath;
        bool isSingleFrame = false;
    };

    struct PlaybackFrameInfo {
        FrameHandle frame;
        qint64 insertedAt = 0;
    };

    class PlaybackBuffer {
    public:
        void clear();
        void insert(int64_t frameNumber, const FrameHandle& frame);
        FrameHandle get(int64_t frameNumber) const;
        FrameHandle getBest(int64_t frameNumber) const;
        FrameHandle getPresentation(int64_t frameNumber) const;
        bool contains(int64_t frameNumber) const;
        int size() const;

    private:
        void trimLocked();

        mutable QMutex m_mutex;
        QHash<int64_t, PlaybackFrameInfo> m_frames;
        static constexpr int kMaxFrames = 24;
    };

    QString requestKey(const QString& clipId, int64_t frameNumber) const;
    int64_t normalizeFrameNumber(const QString& clipId, int64_t frameNumber) const;
    int64_t normalizeFrameNumber(const ClipInfo& info, int64_t frameNumber) const;
    void clearBuffers();
    void schedulePlaybackWindow(const ClipInfo& info,
                                int64_t canonicalFrame,
                                const std::function<void()>& onFrameReady);

    AsyncDecoder* m_decoder = nullptr;

    mutable QMutex m_clipsMutex;
    QHash<QString, ClipInfo> m_clips;
    QHash<QString, PlaybackBuffer*> m_buffers;

    mutable QMutex m_pendingMutex;
    QSet<QString> m_pendingVisibleRequests;
    QSet<QString> m_pendingPrefetchRequests;
    QHash<QString, int64_t> m_latestVisibleTargets;

    mutable QMutex m_markersMutex;
    QVector<RenderSyncMarker> m_renderSyncMarkers;

    std::atomic<bool> m_active{false};
    std::atomic<int64_t> m_playheadFrame{0};
    mutable std::atomic<int> m_droppedPresentationFrames{0};
};

}  // namespace editor
