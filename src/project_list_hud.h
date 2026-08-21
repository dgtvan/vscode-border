#pragma once

#include <windows.h>

#include <string>
#include <vector>

struct ProjectListHudEntry {
    HWND target = nullptr;
    RECT windowRect = {}; // target's on-screen bounds -- used to order entries to match window layout
    std::wstring label;
    COLORREF color = RGB(0, 0, 0);
};

struct ProjectListHudStyle {
    bool horizontal = true; // true = single horizontal strip, false = vertical list
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

// Sorts `entries` to match window layout (left-to-right, top-to-bottom),
// measures/sizes the HUD to fit them -- as a single horizontal strip of
// individually-sized items or a vertical list with one shared column width,
// per `style.horizontal` -- and shows it anchored to the bottom-right of
// the work area, unless the user has manually moved/resized it
// (right-click-drag either edge to resize, the middle to move). In the
// horizontal style, resizing scales every item's width proportionally
// rather than setting one shared width. Once manually placed/sized, that
// placement is kept on later calls instead of being recomputed. Does
// nothing if `entries` is empty; call HideProjectListHud in that case
// instead.
void UpdateProjectListHud(HWND hud, const std::vector<ProjectListHudEntry>& entries,
                          const ProjectListHudStyle& style);