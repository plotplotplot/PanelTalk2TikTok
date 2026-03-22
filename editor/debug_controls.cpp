#include "debug_controls.h"

#include <QByteArray>

#include <atomic>

namespace editor {
namespace {

bool envFlagEnabled(const char* name) {
    return qEnvironmentVariableIntValue(name) == 1;
}

std::atomic<bool> g_debugPlayback{envFlagEnabled("EDITOR_DEBUG_PLAYBACK")};
std::atomic<bool> g_debugCache{envFlagEnabled("EDITOR_DEBUG_CACHE")};
std::atomic<bool> g_debugDecode{envFlagEnabled("EDITOR_DEBUG_DECODE")};

}

bool debugPlaybackEnabled() {
    return g_debugPlayback.load();
}

bool debugCacheEnabled() {
    return g_debugCache.load();
}

bool debugDecodeEnabled() {
    return g_debugDecode.load();
}

void setDebugPlaybackEnabled(bool enabled) {
    g_debugPlayback.store(enabled);
}

void setDebugCacheEnabled(bool enabled) {
    g_debugCache.store(enabled);
}

void setDebugDecodeEnabled(bool enabled) {
    g_debugDecode.store(enabled);
}

QJsonObject debugControlsSnapshot() {
    return QJsonObject{
        {QStringLiteral("playback"), debugPlaybackEnabled()},
        {QStringLiteral("cache"), debugCacheEnabled()},
        {QStringLiteral("decode"), debugDecodeEnabled()}
    };
}

bool setDebugControl(const QString& name, bool enabled) {
    if (name == QStringLiteral("playback")) {
        setDebugPlaybackEnabled(enabled);
        return true;
    }
    if (name == QStringLiteral("cache")) {
        setDebugCacheEnabled(enabled);
        return true;
    }
    if (name == QStringLiteral("decode")) {
        setDebugDecodeEnabled(enabled);
        return true;
    }
    if (name == QStringLiteral("all")) {
        setDebugPlaybackEnabled(enabled);
        setDebugCacheEnabled(enabled);
        setDebugDecodeEnabled(enabled);
        return true;
    }
    return false;
}

} // namespace editor
