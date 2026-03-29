#include "playback_controller.h"

#include "playback_debug.h"

// NOTE:
// This file is an extracted destination for the playback logic currently living
// inside EditorWindow. The method bodies still need to be wired to the actual
// EditorWindow API or to a narrower playback context object.
//
// I am leaving the implementation as commented source because the original code
// refers directly to many EditorWindow members and helper methods. This keeps
// the extracted file immediately useful without pretending it is already fully
// integrated.
//
// Extracted methods from editor.cpp:
// - advanceFrame()
// - speechFilterPlaybackEnabled() const
// - effectivePlaybackRanges() const
// - nextPlaybackFrame(int64_t currentFrame) const

PlaybackController::PlaybackController(EditorWindow *owner)
    : m_owner(owner)
{
}

/*
void advanceFrame()
    {
        // Audio is always the master clock when available
        if (m_audioEngine && m_audioEngine->hasPlayableAudio())
        {
            int64_t audioSample = qMax<int64_t>(0, m_audioEngine->currentSample());
            qreal audioFramePosition = samplesToFramePosition(audioSample);
            int64_t audioFrame = qBound<int64_t>(0,
                                                 static_cast<int64_t>(std::floor(audioFramePosition)),
                                                 m_timeline->totalFrames());

            // When speech filter is active, check if audio is in a gap and seek it forward
            if (speechFilterPlaybackEnabled())
            {
                const QVector<ExportRangeSegment> ranges = effectivePlaybackRanges();
                if (!ranges.isEmpty())
                {
                    // Check if current audio position is in a gap (between word ranges)
                    bool inGap = true;
                    int64_t nextWordStart = -1;
                    for (const auto &range : ranges)
                    {
                        if (audioFrame >= range.startFrame && audioFrame <= range.endFrame)
                        {
                            // Audio is inside a word range - normal playback
                            inGap = false;
                            break;
                        }
                        if (audioFrame < range.startFrame)
                        {
                            // Audio is before this range - we're in a gap
                            nextWordStart = range.startFrame;
                            break;
                        }
                    }

                    if (inGap && nextWordStart >= 0)
                    {
                        // Audio is in a gap - seek it to the next word
                        playbackTrace(QStringLiteral("EditorWindow::advanceFrame.speechFilterSkip"),
                                      QStringLiteral("from=%1 to=%2").arg(audioFrame).arg(nextWordStart));
                        m_audioEngine->seek(nextWordStart);
                        audioSample = m_audioEngine->currentSample();
                        audioFramePosition = samplesToFramePosition(audioSample);
                        audioFrame = qBound<int64_t>(0,
                                                     static_cast<int64_t>(std::floor(audioFramePosition)),
                                                     m_timeline->totalFrames());
                    }
                    else if (inGap)
                    {
                        // Past all words - loop back to start
                        m_audioEngine->seek(ranges.constFirst().startFrame);
                        audioSample = m_audioEngine->currentSample();
                        audioFramePosition = samplesToFramePosition(audioSample);
                        audioFrame = qBound<int64_t>(0,
                                                     static_cast<int64_t>(std::floor(audioFramePosition)),
                                                     m_timeline->totalFrames());
                    }
                }
            }

            if (audioFrame == m_timeline->currentFrame())
            {
                if (m_preview)
                {
                    m_preview->setCurrentPlaybackSample(audioSample);
                }
                return;
            }
            if (m_preview)
            {
                const bool ready = m_preview->preparePlaybackAdvanceSample(audioSample);
                if (!ready)
                {
                    playbackTrace(QStringLiteral("EditorWindow::advanceFrame.catchup"),
                                  QStringLiteral("audioSample=%1 audioFrame=%2")
                                      .arg(audioSample)
                                      .arg(audioFramePosition, 0, 'f', 3));
                }
            }
            playbackTrace(QStringLiteral("EditorWindow::advanceFrame"),
                          QStringLiteral("clock=audio sample=%1 frame=%2").arg(audioSample).arg(audioFramePosition, 0, 'f', 3));
            // Audio is master - don't sync audio to itself, just update video/timeline
            setCurrentPlaybackSample(audioSample, false, /*duringPlayback=*/true);
            return;
        }

        // No audio engine - video is master (fallback)
        const int64_t nextFrame = nextPlaybackFrame(m_timeline->currentFrame());
        if (m_preview)
        {
            const bool ready = m_preview->preparePlaybackAdvance(nextFrame);
            if (!ready)
            {
                playbackTrace(QStringLiteral("EditorWindow::advanceFrame.catchup"),
                              QStringLiteral("next=%1").arg(nextFrame));
            }
        }
        playbackTrace(QStringLiteral("EditorWindow::advanceFrame"),
                      QStringLiteral("clock=video current=%1 next=%2").arg(m_timeline->currentFrame()).arg(nextFrame));
        setCurrentPlaybackSample(frameToSamples(nextFrame), false, /*duringPlayback=*/true);
    }

bool speechFilterPlaybackEnabled() const
    {
        return m_speechFilterEnabledCheckBox && m_speechFilterEnabledCheckBox->isChecked();
    }

QVector<ExportRangeSegment> effectivePlaybackRanges() const
    {
        if (!m_timeline)
        {
            return {};
        }
        QVector<ExportRangeSegment> ranges = m_timeline->exportRanges();
        if (!speechFilterPlaybackEnabled())
        {
            return ranges;
        }
        return transcriptWordExportRanges(ranges, m_timeline->clips(), m_timeline->renderSyncMarkers());
    }

int64_t nextPlaybackFrame(int64_t currentFrame) const
    {
        if (!m_timeline)
        {
            return 0;
        }

        const QVector<ExportRangeSegment> ranges = effectivePlaybackRanges();
        if (ranges.isEmpty())
        {
            const int64_t nextFrame = currentFrame + 1;
            return nextFrame > m_timeline->totalFrames() ? 0 : nextFrame;
        }

        for (const ExportRangeSegment &range : ranges)
        {
            if (currentFrame < range.startFrame)
            {
                // Jump to the start of the next word range
                return range.startFrame;
            }
            if (currentFrame >= range.startFrame && currentFrame < range.endFrame)
            {
                // Inside a word range - advance normally
                return currentFrame + 1;
            }
            // If currentFrame == range.endFrame, continue to find next range
        }
        // Finished all ranges - loop back to start
        return ranges.constFirst().startFrame;
    }
*/
