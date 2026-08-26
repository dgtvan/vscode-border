#pragma once

#include <windows.h>

#include <string>
#include <vector>

// Whether Claude Code is currently running in this window's integrated
// terminal(s) -- see claude_status.h. When a window has more than one
// Claude Code session, the aggregate is whichever of these is highest in
// this list: Attention (blocked on a permission/MCP prompt -- needs the
// user right now) outranks Working (actively generating/running a tool),
// which outranks Waiting (a finished turn, idle, ready for the next
// prompt).
enum class ClaudeStatus { None, Attention, Working, Waiting };

struct ProjectListHudEntry {
    HWND target = nullptr;
    RECT windowRect = {}; // target's on-screen bounds -- used to order entries to match window layout
    long long trackSeq = 0; // this window's track order, oldest first -- the append order for a manual-mode
                             // entry that has no saved position yet (see ApplyManualOrder in
                             // project_list_hud.cpp)
    std::wstring label;    // display text (an alias, if the user set one for rawLabel -- see label_alias.h)
    std::wstring rawLabel; // the un-aliased label, i.e. the alias map's key
    std::wstring path;     // absolute folder path this window has open, resolved via worktree_resolver's
                            // ResolveFolderPath -- empty if VS Code hasn't recorded it (e.g. a plain
                            // folder it's never logged to workspaceStorage, or a multi-root workspace).
                            // Used only for the "Add to Favourites" context-menu action -- see favourites.h.
    COLORREF color = RGB(0, 0, 0);
    ClaudeStatus claudeStatus = ClaudeStatus::None;
};

struct ProjectListHudStyle {
    bool horizontal = true; // true = single horizontal strip, false = vertical list
    bool manualOrder = false; // true = user can drag items to reorder (see project_list_order.h);
                              // false = always sorted by window left edge
    int rowHeight = 18;
    int fontSize = 13;
    bool labelTextColorAuto = false;
    COLORREF labelTextColor = RGB(0, 0, 0);
    int normalOpacity = 255;
    int hoverOpacity = 255;
    bool activateOnHover = false;
    // AI status indicator colors (see claude_status.h / ClaudeStatus above)
    // -- Claude-named internally since Claude is still the only implemented
    // data source, even though the user-facing config keys are the generic
    // `ai_indicator_*` (see config.h).
    COLORREF claudeColorWorking = RGB(255, 159, 10);
    COLORREF claudeColorAttention = RGB(220, 38, 38);
    COLORREF claudeColorWaiting = RGB(52, 199, 89);
    bool claudeBorderColorAuto = true;
    COLORREF claudeBorderColor = RGB(255, 255, 255);
    // A fixed square "+" button appended after the last entry -- opens a new
    // VS Code window when clicked. Unlike the entries above, it can't be
    // drag-reordered and never triggers activateOnHover. `newWindowButtonColor`
    // is picked by the caller (e.g. the next color in tracking.cpp's
    // round-robin palette allocation) since this module has no notion of a
    // color palette of its own.
    bool showNewWindowButton = false;
    COLORREF newWindowButtonColor = RGB(0, 0, 0);
};

// `horizontal` is the style to start with (see ProjectListHudStyle) --
// needed up front because it's also used to interpret any placement
// remembered for the current monitor scenario before the first real
// UpdateProjectListHud call establishes it via `style`.
HWND CreateProjectListHud(HINSTANCE hInstance, bool horizontal);

// Hides the HUD (config disabled, or nothing currently worth showing).
void HideProjectListHud(HWND hud);

// Sorts `entries` to match window layout (left-to-right, top-to-bottom) --
// or, if `style.manualOrder` is set, to the last order the user dragged
// them into (see project_list_order.h), with any entries never seen before
// appended at the end in their window-layout order -- then moves every
// favourite entry (see favourites.h) ahead of the non-favourites,
// preserving each group's relative order, so favourites always sit at the
// front of the list regardless of window layout or manual order --
// measures/sizes the HUD to fit them -- as a single horizontal strip or a
// vertical list, per `style.horizontal` -- with every item sharing one
// width, and shows it anchored to the bottom-right of the work area, unless
// the user has manually moved/resized it (Ctrl+left-click-drag either edge
// to resize, the middle to move; plain right-click opens a context menu
// instead, e.g. to set an alias for an item's label). Once manually
// placed/sized, that placement is kept on later calls instead of being
// recomputed. Does nothing if `entries` is empty; call HideProjectListHud
// in that case instead.
//
// When style.manualOrder is set, plain left-click-and-drag on an item (as
// opposed to a plain click, which activates it) reorders it: other items
// shift live to open a gap at the candidate drop position as the dragged
// item, shown floating, follows the cursor. The result is persisted
// immediately on drop. A dragged item is confined to its own favourite/
// non-favourite group -- it can be reordered within that group, but can't
// be dropped across the boundary into the other one, so the favourites-
// always-in-front rule above can't be dragged away.
void UpdateProjectListHud(HWND hud, const std::vector<ProjectListHudEntry>& entries,
                          const ProjectListHudStyle& style);