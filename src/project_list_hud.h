#pragma once

#include <windows.h>

#include <string>
#include <vector>

struct ProjectListHudEntry {
    HWND target = nullptr;
    std::wstring label;
    COLORREF color = RGB(0, 0, 0);
};

HWND CreateProjectListHud(HINSTANCE hInstance);

void UpdateProjectListHud(HWND hud, int x, int y, int width, int height,
                          const std::vector<ProjectListHudEntry>& entries,
                          int rowHeight, int fontSize,
                          bool labelTextColorAuto, COLORREF labelTextColor,
                          int normalOpacity, int hoverOpacity, bool activateOnHover);