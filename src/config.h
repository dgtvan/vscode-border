#pragma once

#include <windows.h>

#include <string>
#include <utility>
#include <vector>

enum class ProfileType { None, Claude, Codex };

// One account folder. Empty type/path is never an implicit account.
struct Profile {
    ProfileType type = ProfileType::None;
    std::wstring name, path, configDir, icon;
    bool hasColor = false;
    COLORREF color = RGB(0, 0, 0);
    bool ringBadge = true;
    bool usageEnabled = true;
    bool isDefault = false; // standard provider folder: no fallback icon
    int slot = 0;
};
const int kMaxProfiles = 10;

struct Config {
    int thickness = 4;
    int opacity = 255;
    int rescanIntervalMs = 1500;
    bool showLabel = true;
    int labelHeight = 22;
    int labelFontSize = 13;
    bool labelTextColorAuto = false; // true = auto black/white contrast against the border color
    COLORREF labelTextColor = RGB(0, 0, 0); // used when labelTextColorAuto is false
    std::wstring labelAliasFormat = L"<Alias> - <Branch>"; // border label text for an aliased window --
                                                            // <Alias>/<Branch> tokens (see tracking.cpp's BorderLabel)
    bool showProjectList = true;
    bool projectListFixedMatchTaskbar = true; // paint the hub's reserved band to look like the taskbar
    int projectListOpacityNormal = 255;
    int projectListOpacityHover = 255;
    bool projectListActivateOnHover = false;
    bool verboseLogging = false; // logs every WinEvent + overlay sync decision for tracked windows
    std::vector<COLORREF> palette;

    std::vector<Profile> profiles; // slot order; configured account folders only

    // AI status indicator (see ai_provider.h) -- "claude" is the only
    // currently-implemented provider; other tokens (e.g. a future "copilot")
    // are accepted but produce no indicator until that integration exists.
    bool aiIndicatorEnabled = true;
    std::vector<std::string> aiIndicatorProviders = {"claude"};
    COLORREF aiIndicatorColorWorking = RGB(255, 159, 10);
    COLORREF aiIndicatorColorAttention = RGB(220, 38, 38);
    COLORREF aiIndicatorColorWaiting = RGB(52, 199, 89);
    bool aiIndicatorBorderColorAuto = true; // true = auto black/white contrast against the item's own color
    COLORREF aiIndicatorBorderColor = RGB(255, 255, 255); // used when aiIndicatorBorderColorAuto is false

    // Usage rings on the hub, one pair per enabled assistant profile.
    bool usageMeterEnabled = true;
    int usageMeterRefreshMinutes = 5;
};

// Global, live config -- reloadable at runtime via the tray menu.
extern Config g_config;

std::wstring GetConfigPath();
Config LoadConfig(const std::wstring& path);
