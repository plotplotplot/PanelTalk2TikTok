#pragma once

#include <QJsonObject>

#include "editor_shared.h"

namespace editor
{

QJsonObject clipToJson(const TimelineClip &clip);
TimelineClip clipFromJson(const QJsonObject &obj);

} // namespace editor
