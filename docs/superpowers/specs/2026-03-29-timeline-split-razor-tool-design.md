# Timeline Split & Razor Tool

## Problem

The clip split operation (`Ctrl+B`) exists but is only accessible via keyboard shortcut. There is no context menu entry, no razor/blade tool mode, and no visual indicator. This makes the feature undiscoverable and limits the editing workflow.

## Design

### 1. Right-Click Context Menu: "Split Clip At Playhead"

Add a new action to the existing clip context menu in `TimelineWidget::contextMenuEvent`. It appears after the nudge actions and before "Delete", matching the destructive-action ordering convention already in the menu.

**Enabled condition:** A clip is right-clicked (or selected), and the playhead (`m_currentFrame`) is strictly inside the clip bounds (not at the first or last frame). This matches the existing guard in `splitSelectedClipAtFrame`.

**Handler:** Calls `splitSelectedClipAtFrame(m_currentFrame)`, then fires `clipsChanged` callback.

### 2. Tool Mode State

Add a `ToolMode` enum to `TimelineWidget`:

```cpp
enum class ToolMode { Select, Razor };
```

New private member: `ToolMode m_toolMode = ToolMode::Select`.

Public API:
- `void setToolMode(ToolMode mode)` — sets mode, updates cursor, calls `update()`
- `ToolMode toolMode() const`

Signal-style callback (matching existing pattern): `std::function<void()> toolModeChanged`.

### 3. Razor Button in Transport Bar

Add a checkable `QToolButton` to the `EditorPane` transport bar `QHBoxLayout`, placed after `m_endButton` with a separator spacer. The button text is "Razor" (no icon needed — consistent with the existing text-based transport buttons). It is checkable; checked state corresponds to `ToolMode::Razor`.

`EditorPane` exposes: `QToolButton* razorButton() const`.

`EditorWindow` wires the button's `toggled(bool)` signal to `m_timeline->setToolMode(...)` and wires `toolModeChanged` back to keep the button state in sync.

### 4. Razor Mode Behavior

**Mouse interaction changes when `m_toolMode == Razor`:**

- **`mousePressEvent`** (clip hit path, ~line 1625): Instead of starting a clip drag, compute the frame from the click x-position via `frameFromX()`. Find the clip under the cursor (by hit-testing, not requiring prior selection). Call `splitSelectedClipAtFrame(clickFrame)` — temporarily setting `m_selectedClipId` to the hit clip if needed, then restoring it. After the split, remain in razor mode (don't auto-switch to Select).

- **`mouseMoveEvent`**: Track `m_razorHoverFrame` (the frame under the cursor) when in razor mode and the cursor is over the timeline content area. Call `update()` to repaint the indicator. When not over a clip, set `m_razorHoverFrame = -1`.

- **Cursor**: When in razor mode and hovering over a clip, set `Qt::CrossCursor` (the closest standard Qt cursor to a blade/scissor). Over empty space, use `Qt::ArrowCursor`. This is handled in `updateHoverCursor`.

**Paint changes:**

- In `paintEvent`, after the snap indicator drawing (~line 2547) and before the playhead drawing: if `m_toolMode == Razor && m_razorHoverFrame >= 0`, draw a vertical dashed line at `xFromFrame(m_razorHoverFrame)` from `ruler.top()` to `tracks.bottom()`. Color: `#a0e0ff` (light blue, distinct from the orange drop indicator and yellow snap indicator), 2px width, `Qt::DashLine` pen style.

### 5. Keyboard Shortcut

- **`B`** toggles between `Select` and `Razor` mode. This is the NLE industry convention (Premiere Pro, DaVinci Resolve, Final Cut Pro all use `B` for blade/razor).
- Wired as a `QShortcut` in `EditorWindow`, gated by `shouldBlockGlobalEditorShortcuts()` (same pattern as `Ctrl+B` and `Ctrl+Z`).
- `Escape` while in razor mode returns to Select mode. This reuses the existing `Escape` shortcut path if present, or adds a new one.

### 6. Clip ID Tracking

The existing `splitSelectedClipAtFrame` already generates a new UUID for the right-half clip. No changes to the ID system are needed for this feature. The split operation is fully tracked via the undo/history system (whole-state JSON snapshots).

## Files Modified

| File | Change |
|------|--------|
| `timeline_widget.h` | Add `ToolMode` enum, `m_toolMode`, `m_razorHoverFrame`, public accessors, `toolModeChanged` callback |
| `timeline_widget.cpp` | Context menu action, razor mode in mouse events, razor indicator in paint, cursor changes |
| `editor_pane.h` | Add `m_razorButton` member, `razorButton()` accessor |
| `editor_pane.cpp` | Add razor `QToolButton` to transport bar layout |
| `editor.cpp` | Wire razor button to timeline tool mode, add `B` and `Escape` shortcuts |

## Not In Scope

- Clip split lineage / parentId tracking
- Rejoin split clips
- Slip/slide tool modes
- Custom razor cursor icon (uses `Qt::CrossCursor`)
