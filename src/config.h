#pragma once

#include <windows.h>

#include <string>
#include <utility>
#include <vector>

// One of config.ini's Claude profile slots, claude_profile_<N>_* (see
// claude_profile.h). Slot 1 is always the default profile; the others are
// further Claude Code config folders.
struct ClaudeProfile {
    std::wstring name;        // claude_profile_<N>_name, empty: derived (see claude_profile.h's ClaudeProfileName)
    std::wstring path;        // claude_profile_<N>_profile_path as written, empty only in slot 1
    std::wstring configDir;   // the Claude Code config folder it names: path with environment variables
                              // expanded, no trailing separator; slot 1's without a path is %USERPROFILE%\.claude
    std::wstring icon;        // claude_profile_<N>_icon: an image path or base64 image data, or empty
    bool hasColor = false;    // claude_profile_<N>_color set: its usage rings' background (see claude_usage.h)
    COLORREF color = RGB(0, 0, 0);
    bool ringBadge = true;    // claude_profile_<N>_ring_badge: its icon in front of its usage rings
    int slot = 0;             // N
    bool isDefault = false;   // slot 1
};

// Slots claude_profile_1_* .. claude_profile_<kMaxClaudeProfiles>_*.
const int kMaxClaudeProfiles = 5;


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

    // The Claude profiles in slot order (see claude_profile.h): slot 1, the
    // default profile, always first; then the slots that have a
    // _profile_path.
    std::vector<ClaudeProfile> claudeProfiles;

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

    // Claude usage rings on the hub, one pair per Claude profile (see claude_usage.h).
    bool usageMeterEnabled = true;
    int usageMeterRefreshMinutes = 5;
};

// Global, live config -- reloadable at runtime via the tray menu.
extern Config g_config;

std::wstring GetConfigPath();
Config LoadConfig(const std::wstring& path);
