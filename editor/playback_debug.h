#pragma once

#include <QDateTime>
#include <QElapsedTimer>
#include <QHash>
#include <QDebug>
#include <QString>

#include <limits>

#include "debug_controls.h"

inline qint64 nowMs()
{
    return QDateTime::currentMSecsSinceEpoch();
}

inline qint64 playbackTraceMs()
{
    static QElapsedTimer timer;
    static bool started = false;
    if (!started)
    {
        timer.start();
        started = true;
    }
    return timer.elapsed();
}

inline bool playbackTraceEnabled()
{
    return debugPlaybackLevel() >= DebugLogLevel::Info;
}

inline void playbackTrace(const QString &stage, const QString &detail = QString())
{
    if (!playbackTraceEnabled())
    {
        return;
    }

    static QHash<QString, qint64> lastLogByStage;
    const qint64 now = playbackTraceMs();
    const bool throttle = !debugPlaybackVerboseEnabled();
    if (throttle &&
        (stage.startsWith(QStringLiteral("PreviewWindow::requestFramesForCurrentPosition")) ||
         stage.startsWith(QStringLiteral("EditorWindow::advanceFrame")) ||
         stage.startsWith(QStringLiteral("PreviewWindow::setCurrentFrame")) ||
         stage.startsWith(QStringLiteral("PreviewWindow::visible-request"))))
    {
        const qint64 last = lastLogByStage.value(stage, std::numeric_limits<qint64>::min());
        if (now - last < 250)
        {
            return;
        }
        lastLogByStage.insert(stage, now);
    }

    qDebug().noquote() << QStringLiteral("[PLAYBACK %1 ms] %2%3")
                              .arg(now, 6)
                              .arg(stage)
                              .arg(detail.isEmpty() ? QString() : QStringLiteral(" | ") + detail);
}
