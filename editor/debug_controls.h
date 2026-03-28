#pragma once

#include <QJsonObject>
#include <QJsonValue>
#include <QString>

namespace editor {

enum class DebugLogLevel : int {
    Off = 0,
    Warn = 1,
    Info = 2,
    Debug = 3,
    Verbose = 4,
};

enum class DecodePreference : int {
    Auto = 0,
    Hardware = 1,
    Software = 2,
};

DebugLogLevel debugPlaybackLevel();
DebugLogLevel debugCacheLevel();
DebugLogLevel debugDecodeLevel();

bool debugPlaybackEnabled();
bool debugCacheEnabled();
bool debugDecodeEnabled();
bool debugPlaybackWarnEnabled();
bool debugCacheWarnEnabled();
bool debugDecodeWarnEnabled();
bool debugPlaybackWarnOnlyEnabled();
bool debugCacheWarnOnlyEnabled();
bool debugDecodeWarnOnlyEnabled();
bool debugPlaybackVerboseEnabled();
bool debugCacheVerboseEnabled();
bool debugDecodeVerboseEnabled();

bool debugLeadPrefetchEnabled();
int debugLeadPrefetchCount();
int debugPrefetchMaxQueueDepth();
int debugPrefetchMaxInflight();
int debugPrefetchMaxPerTick();
int debugPrefetchSkipVisiblePendingThreshold();
int debugVisibleQueueReserve();
int debugPlaybackWindowAhead();
DecodePreference debugDecodePreference();

void setDebugPlaybackEnabled(bool enabled);
void setDebugCacheEnabled(bool enabled);
void setDebugDecodeEnabled(bool enabled);
void setDebugPlaybackLevel(DebugLogLevel level);
void setDebugCacheLevel(DebugLogLevel level);
void setDebugDecodeLevel(DebugLogLevel level);
void setDebugLeadPrefetchEnabled(bool enabled);
void setDebugLeadPrefetchCount(int count);
void setDebugPrefetchMaxQueueDepth(int depth);
void setDebugPrefetchMaxInflight(int inflight);
void setDebugPrefetchMaxPerTick(int perTick);
void setDebugPrefetchSkipVisiblePendingThreshold(int threshold);
void setDebugVisibleQueueReserve(int reserve);
void setDebugPlaybackWindowAhead(int ahead);
void setDebugDecodePreference(DecodePreference preference);

QJsonObject debugControlsSnapshot();
bool setDebugControl(const QString& name, bool enabled);
bool setDebugControlLevel(const QString& name, DebugLogLevel level);
bool setDebugOption(const QString& name, const QJsonValue& value);
QString debugLogLevelToString(DebugLogLevel level);
bool parseDebugLogLevel(const QString& text, DebugLogLevel* levelOut);
QString decodePreferenceToString(DecodePreference preference);
bool parseDecodePreference(const QString& text, DecodePreference* preferenceOut);

} // namespace editor
