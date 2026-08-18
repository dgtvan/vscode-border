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
    bool verboseLogging = false; // logs every WinEvent + overlay sync decision for tracked windows
    std::vector<COLORREF> palette;
};

// Global, live config -- reloadable at runtime via the tray menu.
extern Config g_config;

std::wstring GetConfigPath();
Config LoadConfig(const std::wstring& path);
