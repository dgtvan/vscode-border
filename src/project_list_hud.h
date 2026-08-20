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
    int rowHeight = 18;
    int fontSize = 13;
    bool labelTextColorAuto = false;
    COLORREF labelTextColor = RGB(0, 0, 0);
    int normalOpacity = 255;
    int hoverOpacity = 255;
    bool activateOnHover = false;
};

HWND CreateProjectListHud(HINSTANCE hInstance);

// Hides the HUD (config disabled, or nothing currently worth showing).
void HideProjectListHud(HWND hud);

// Sorts `entries` to match window layout (left-to-right, top-to-bottom),
// measures/sizes the HUD to fit them, and shows it anchored to the
// bottom-right of the work area -- unless the user has manually
// moved/resized it (right-click-drag), in which case that placement is kept
// instead. Does nothing if `entries` is empty; call HideProjectListHud in
// that case instead.
void UpdateProjectListHud(HWND hud, const std::vector<ProjectListHudEntry>& entries,
                          const ProjectListHudStyle& style);