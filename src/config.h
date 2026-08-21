#pragma once

#include <windows.h>

#include <string>
#include <vector>

struct Config {
    int thickness = 4;
    int opacity = 255;
    int rescanIntervalMs = 1500;
    bool showLabel = true;
    int labelHeight = 22;
    int labelFontSize = 13;
    bool labelTextColorAuto = false; // true = auto black/white contrast against the border color
    COLORREF labelTextColor = RGB(0, 0, 0); // used when labelTextColorAuto is false
    bool showProjectList = true;
    bool projectListHorizontal = true; // true = single horizontal strip, false = vertical list
    bool projectListManualOrder = false; // true = user can drag items to reorder, remembered across
                                          // restarts; false = always sorted by window left edge
    int projectListOpacityNormal = 255;
    int projectListOpacityHover = 255;
    bool projectListActivateOnHover = false;
    bool verboseLogging = false; // logs every WinEvent + overlay sync decision for tracked windows
    std::vector<COLORREF> palette;
};

// Global, live config -- reloadable at runtime via the tray menu.
extern Config g_config;

std::wstring GetConfigPath();
Config LoadConfig(const std::wstring& path);
