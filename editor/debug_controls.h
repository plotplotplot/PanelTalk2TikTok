#pragma once

#include <QJsonObject>
#include <QString>

namespace editor {

bool debugPlaybackEnabled();
bool debugCacheEnabled();
bool debugDecodeEnabled();

void setDebugPlaybackEnabled(bool enabled);
void setDebugCacheEnabled(bool enabled);
void setDebugDecodeEnabled(bool enabled);

QJsonObject debugControlsSnapshot();
bool setDebugControl(const QString& name, bool enabled);

} // namespace editor
