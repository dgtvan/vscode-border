#pragma once

#include <windows.h>

#include <string>
#include <vector>

struct ProjectListHudEntry {
    HWND target = nullptr;
    RECT windowRect = {}; // target's on-screen bounds -- used to order entries to match window layout
    std::wstring label;    // display text (an alias, if the user set one for rawLabel -- see label_alias.h)
    std::wstring rawLabel; // the un-aliased label, i.e. the alias map's key
    COLORREF color = RGB(0, 0, 0);
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
// appended at the end in their window-layout order -- measures/sizes the
// HUD to fit them -- as a single horizontal strip or a vertical list, per
// `style.horizontal` -- with every item sharing one width, and shows it
// anchored to the bottom-right of the work area, unless the user has
// manually moved/resized it (Ctrl+left-click-drag either edge to resize,
// the middle to move; plain right-click opens a context menu instead, e.g.
// to set an alias for an item's label). Once manually placed/sized, that
// placement is kept on later calls instead of being recomputed. Does
// nothing if `entries` is empty; call HideProjectListHud in that case
// instead.
//
// When style.manualOrder is set, plain left-click-and-drag on an item (as
// opposed to a plain click, which activates it) reorders it: other items
// shift live to open a gap at the candidate drop position as the dragged
// item, shown floating, follows the cursor. The result is persisted
// immediately on drop.
void UpdateProjectListHud(HWND hud, const std::vector<ProjectListHudEntry>& entries,
                          const ProjectListHudStyle& style);