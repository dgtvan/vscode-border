#pragma once

#include <windows.h>

#include <string>
#include <utility>
#include <vector>

// One of config.ini's user profile slots, user_profile_<N>_* (see account.h).
// Slot 1 is always the account this app runs as; the others are other
// Windows accounts.
struct UserProfile {
    std::wstring account;  // Windows account name, as written
    std::wstring icon;     // user_profile_<N>_icon: an image path or base64 image data, or empty
    bool hasColor = false; // user_profile_<N>_color set: its usage rings' background (see claude_usage.h)
    COLORREF color = RGB(0, 0, 0);
    bool ringBadge = true;    // user_profile_<N>_ring_badge: its icon in front of its usage rings
    int slot = 0;             // N
    bool thisAccount = false; // slot 1
};

// Slots user_profile_1_* .. user_profile_<kMaxUserProfiles>_*.
const int kMaxUserProfiles = 5;

// user_profile_1_account's value in config.ini: slot 1 is always the account
// this app runs as, so its account isn't something to fill in.
extern const char* const kThisAccountMarker;


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

    // Extra Linux VS Code arguments per WSL distro, from
    // `wsl_vscode_extra_args.<distro>` keys (see wsl.h's LinuxVSCodeOpenCommand).
    // The distro name is lowercased, as config.ini keys are.
    std::vector<std::pair<std::wstring, std::vector<std::wstring>>> wslVSCodeExtraArgs;

    // The user profiles in slot order (see account.h): slot 1, this app's
    // own account, always first; then the other Windows accounts VS Code can
    // be started as, the slots that have an account.
    std::vector<UserProfile> userProfiles;

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

    // Claude usage rings on the hub, one pair per account (see claude_usage.h).
    bool usageMeterEnabled = true;
    int usageMeterRefreshMinutes = 5;
};

// Global, live config -- reloadable at runtime via the tray menu.
extern Config g_config;

std::wstring GetConfigPath();
Config LoadConfig(const std::wstring& path);
