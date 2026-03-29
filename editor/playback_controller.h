#pragma once

#include <QVector>

#include "editor_shared.h"

class EditorWindow;

// Pragmatic first-pass split. This mirrors the current EditorWindow playback policy.
// It is not fully decoupled yet; the next step is to replace EditorWindow access
// with a narrow context/interface.
class PlaybackController
{
public:
    explicit PlaybackController(EditorWindow *owner);

    void advanceFrame();
    bool speechFilterPlaybackEnabled() const;
    QVector<editor::ExportRangeSegment> effectivePlaybackRanges() const;
    int64_t nextPlaybackFrame(int64_t currentFrame) const;

private:
    EditorWindow *m_owner = nullptr;
};
