#pragma once

#include <QColor>
#include <QString>

#include "editor_shared.h"

class EditorWindow;

// Pragmatic first-pass split for inspector-side logic.
class InspectorController
{
public:
    explicit InspectorController(EditorWindow *owner);

    QString clipLabelForId(const QString &clipId) const;
    QColor clipColorForId(const QString &clipId) const;
    bool parseSyncActionText(const QString &text, editor::RenderSyncAction *actionOut) const;
    void refreshSyncInspector();
    void refreshTranscriptInspector();

private:
    EditorWindow *m_owner = nullptr;
};
