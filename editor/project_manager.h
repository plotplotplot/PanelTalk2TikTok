#pragma once

class EditorWindow;

// First-pass extraction target for project/state/history persistence.
// The original implementation is heavily coupled to EditorWindow widget members.
// This file gives you the correct seam and method inventory.
class ProjectManager
{
public:
    explicit ProjectManager(EditorWindow *owner);

private:
    EditorWindow *m_owner = nullptr;
};
