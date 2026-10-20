#include "config.h"
#include "file_util.h"
#include "logger.h"
#include "text_util.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>

Config g_config;

const char* const kThisAccountMarker = "<the account running VS Code Border - do not edit>";

static std::vector<COLORREF> DefaultPalette() {
    return {
        RGB(0xE6, 0x4A, 0x4A), RGB(0x4A, 0x9C, 0xE6), RGB(0x4A, 0xE6, 0x7A), RGB(0xE6, 0xB8, 0x4A),
        RGB(0xB0, 0x4A, 0xE6), RGB(0x4A, 0xE6, 0xDC), RGB(0xE6, 0x4A, 0xA8), RGB(0x9C, 0xE6, 0x4A),
        RGB(0xE6, 0x7A, 0x4A), RGB(0x4A, 0x5C, 0xE6),
    };
}

std::wstring GetConfigPath() {
    return GetExeDir() + L"\\config.ini";
}

static std::vector<std::string> SplitChar(const std::string& s, char delim) {
    std::vector<std::string> parts;
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); i++) {
        if (i == s.size() || s[i] == delim) {
            parts.push_back(s.substr(start, i - start));
            start = i + 1;
        }
    }
    return parts;
}

static std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

static COLORREF ParseHexColor(const std::string& hex) {
    unsigned long v = strtoul(hex.c_str(), nullptr, 16);
    BYTE r = (BYTE)((v >> 16) & 0xFF), g = (BYTE)((v >> 8) & 0xFF), b = (BYTE)(v & 0xFF);
    return RGB(r, g, b);
}

// An optional RRGGBB value: false when empty, and when it isn't one (warned
// about, naming `key`).
static bool ParseOptionalHexColor(const std::string& key, const std::string& val, COLORREF& out) {
    if (val.empty()) return false;
    if (val.size() != 6 || val.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) {
        LogWarn(L"config: %hs=[%hs] isn't a hex RRGGBB color -- ignoring it", key.c_str(), val.c_str());
        return false;
    }
    out = ParseHexColor(val);
    return true;
}

Config LoadConfig(const std::wstring& path) {
    Config cfg;
    cfg.palette = DefaultPalette();
    UserProfile slots[kMaxUserProfiles];

    std::string content = ReadFileBytes(path);
    if (content.empty()) {
        Log(L"config: not found at %ls, using defaults", path.c_str());
        return cfg;
    }

    for (const std::string& rawLine : SplitChar(content, '\n')) {
        std::string line = Trim(rawLine);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;

        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = Trim(line.substr(0, eq));
        std::string val = Trim(line.substr(eq + 1));
        std::transform(key.begin(), key.end(), key.begin(), ::tolower);

        if (key == "thickness") {
            cfg.thickness = std::max(1, atoi(val.c_str()));
        } else if (key == "opacity") {
            cfg.opacity = std::min(255, std::max(0, atoi(val.c_str())));
        } else if (key == "rescan_interval_ms") {
            cfg.rescanIntervalMs = std::max(500, atoi(val.c_str()));
        } else if (key == "show_label") {
            cfg.showLabel = (val == "1" || _stricmp(val.c_str(), "true") == 0);
        } else if (key == "show_project_list") {
            cfg.showProjectList = (val == "1" || _stricmp(val.c_str(), "true") == 0);
        } else if (key == "project_list_fixed_match_taskbar") {
            cfg.projectListFixedMatchTaskbar = (val == "1" || _stricmp(val.c_str(), "true") == 0);
        } else if (key == "project_list_order") {
            cfg.projectListManualOrder = (_stricmp(val.c_str(), "manual") == 0);
        } else if (key == "project_list_opacity_normal") {
            cfg.projectListOpacityNormal = std::min(255, std::max(0, atoi(val.c_str())));
        } else if (key == "project_list_opacity_hover") {
            cfg.projectListOpacityHover = std::min(255, std::max(0, atoi(val.c_str())));
        } else if (key == "project_list_activate_on_hover") {
            cfg.projectListActivateOnHover = (val == "1" || _stricmp(val.c_str(), "true") == 0);
        } else if (key == "label_height") {
            cfg.labelHeight = std::max(0, atoi(val.c_str()));
        } else if (key == "label_font_size") {
            cfg.labelFontSize = std::max(6, atoi(val.c_str()));
        } else if (key == "verbose_logging") {
            cfg.verboseLogging = (val == "1" || _stricmp(val.c_str(), "true") == 0);
        } else if (key == "label_text_color") {
            if (_stricmp(val.c_str(), "auto") == 0) {
                cfg.labelTextColorAuto = true;
            } else {
                cfg.labelTextColorAuto = false;
                cfg.labelTextColor = ParseHexColor(val);
            }
        } else if (key == "label_alias_format") {
            // Empty would paint an empty chip (i.e. no label at all) -- that's
            // what show_label=false is for, so keep the default instead.
            if (!val.empty()) cfg.labelAliasFormat = Utf8ToWide(val);
        } else if (key.rfind("wsl_vscode_extra_args.", 0) == 0) {
            std::wstring distro = Utf8ToWide(key.substr(strlen("wsl_vscode_extra_args.")));
            std::vector<std::wstring> args;
            for (const std::string& item : SplitChar(val, ' ')) {
                std::string a = Trim(item);
                if (!a.empty()) args.push_back(Utf8ToWide(a));
            }
            if (distro.empty()) {
                LogWarn(L"config: wsl_vscode_extra_args. has no distro name after the dot -- ignoring it");
            } else if (!args.empty()) {
                Log(L"config: wsl_vscode_extra_args for distro [%ls]: [%hs]", distro.c_str(), val.c_str());
                cfg.wslVSCodeExtraArgs.emplace_back(distro, args);
            }
        } else if (key.rfind("user_profile_", 0) == 0) {
            // user_profile_<N>_account / _icon / _vscode
            const char* rest = key.c_str() + strlen("user_profile_");
            char* end = nullptr;
            long slot = strtol(rest, &end, 10);
            std::string field = (end && *end == '_') ? std::string(end + 1) : std::string();
            if (end == rest || slot < 1 || slot > kMaxUserProfiles ||
                (field != "account" && field != "icon" && field != "vscode" && field != "color" &&
                 field != "ring_badge")) {
                LogWarn(L"config: unknown key [%hs] -- user profiles are "
                        L"user_profile_<1-%d>_account/_icon/_vscode/_color/_ring_badge",
                        key.c_str(), kMaxUserProfiles);
            } else if (field == "color") {
                UserProfile& p = slots[slot - 1];
                p.hasColor = ParseOptionalHexColor(key, val, p.color);
            } else if (field == "ring_badge") {
                // Empty keeps the default (shown).
                slots[slot - 1].ringBadge = !(val == "0" || _stricmp(val.c_str(), "false") == 0);
            } else if (slot == 1 && field == "account" && val == kThisAccountMarker) {
                // Filled in below.
            } else {
                UserProfile& p = slots[slot - 1];
                (field == "account" ? p.account : field == "icon" ? p.icon : p.vscode) = Utf8ToWide(val);
            }
        } else if (key == "colors") {
            std::vector<COLORREF> palette;
            for (const std::string& item : SplitChar(val, ',')) {
                std::string c = Trim(item);
                if (!c.empty()) palette.push_back(ParseHexColor(c));
            }
            if (!palette.empty()) cfg.palette = palette;
        } else if (key == "ai_indicator_enabled") {
            cfg.aiIndicatorEnabled = (val == "1" || _stricmp(val.c_str(), "true") == 0);
        } else if (key == "ai_indicator_provider") {
            std::vector<std::string> providers;
            for (const std::string& item : SplitChar(val, ',')) {
                std::string p = Trim(item);
                std::transform(p.begin(), p.end(), p.begin(), ::tolower);
                if (p.empty()) continue;
                // "copilot" is a documented future value -- accepted without
                // warning even though it does nothing yet (see README's
                // "Claude Code status dots" section). Anything else is
                // probably a typo, so it's dropped with a warning rather
                // than silently doing nothing.
                if (p == "claude" || p == "copilot") {
                    providers.push_back(p);
                } else {
                    LogWarn(L"config: ai_indicator_provider has unrecognized value [%hs] -- ignoring it",
                            p.c_str());
                }
            }
            cfg.aiIndicatorProviders = providers;
        } else if (key == "usage_meter_enabled") {
            cfg.usageMeterEnabled = (val == "1" || _stricmp(val.c_str(), "true") == 0);
        } else if (key == "usage_meter_refresh_minutes") {
            cfg.usageMeterRefreshMinutes = std::min(60, std::max(1, atoi(val.c_str())));
        } else if (key == "ai_indicator_color_working") {
            cfg.aiIndicatorColorWorking = ParseHexColor(val);
        } else if (key == "ai_indicator_color_attention") {
            cfg.aiIndicatorColorAttention = ParseHexColor(val);
        } else if (key == "ai_indicator_color_waiting") {
            cfg.aiIndicatorColorWaiting = ParseHexColor(val);
        } else if (key == "ai_indicator_border_color") {
            if (_stricmp(val.c_str(), "auto") == 0) {
                cfg.aiIndicatorBorderColorAuto = true;
            } else {
                cfg.aiIndicatorBorderColorAuto = false;
                cfg.aiIndicatorBorderColor = ParseHexColor(val);
            }
        }
    }

    // Slot 1 is this app's own account whatever its _account says (the
    // marker, empty, or the account's own name are all fine). Any other slot
    // without an account is unused, whatever else it has.
    wchar_t ownName[256] = {};
    DWORD ownLen = 256;
    std::wstring own = GetUserNameW(ownName, &ownLen) ? ownName : L"";
    if (!slots[0].account.empty() &&
        CompareStringOrdinal(slots[0].account.c_str(), -1, own.c_str(), -1, TRUE) != CSTR_EQUAL) {
        LogWarn(L"config: user_profile_1 is the account this app runs as, [%ls], but its _account says [%ls] -- "
                L"using it as [%ls]; another account goes in slot 2 or later, and slot 1's stays %hs",
                own.c_str(), slots[0].account.c_str(), own.c_str(), kThisAccountMarker);
    }
    slots[0].account = own;
    slots[0].thisAccount = true;
    for (int i = 0; i < kMaxUserProfiles; i++) {
        const UserProfile& p = slots[i];
        if (p.account.empty()) continue;
        bool duplicate = false;
        for (const UserProfile& q : cfg.userProfiles) {
            duplicate = duplicate || CompareStringOrdinal(q.account.c_str(), -1, p.account.c_str(), -1, TRUE) ==
                                         CSTR_EQUAL;
        }
        if (duplicate) {
            LogWarn(L"config: user_profile_%d_account [%ls] repeats an earlier profile's account -- ignoring it",
                    i + 1, p.account.c_str());
            continue;
        }
        // The icon can be a whole base64 image -- logged by size only.
        wchar_t color[8] = L"";
        if (p.hasColor) swprintf(color, 8, L"%02X%02X%02X", GetRValue(p.color), GetGValue(p.color), GetBValue(p.color));
        Log(L"config: user profile %d: account=[%ls] icon=[%ls] vscode=[%ls] color=[%ls] ring_badge=%d", i + 1,
            p.account.c_str(),
            p.icon.size() > 260 ? (std::to_wstring(p.icon.size()) + L" chars").c_str() : p.icon.c_str(),
            p.vscode.c_str(), color, p.ringBadge);
        cfg.userProfiles.push_back(p);
        cfg.userProfiles.back().slot = i + 1;
    }

    std::string providersJoined;
    for (const std::string& p : cfg.aiIndicatorProviders) {
        if (!providersJoined.empty()) providersJoined += ",";
        providersJoined += p;
    }

    Log(L"config: loaded thickness=%d opacity=%d rescan_ms=%d show_label=%d show_project_list=%d project_list_fixed_match_taskbar=%d project_list_order=%ls project_list_opacity_normal=%d project_list_opacity_hover=%d project_list_activate_on_hover=%d label_height=%d label_font_size=%d label_text_color_auto=%d label_text_color=%06X label_alias_format=[%ls] verbose_logging=%d colors=%zu ai_indicator_enabled=%d ai_indicator_provider=%hs usage_meter_enabled=%d usage_meter_refresh_minutes=%d",
        cfg.thickness, cfg.opacity, cfg.rescanIntervalMs, cfg.showLabel, cfg.showProjectList,
        cfg.projectListFixedMatchTaskbar,
        cfg.projectListManualOrder ? L"manual" : L"auto",
        cfg.projectListOpacityNormal, cfg.projectListOpacityHover, cfg.projectListActivateOnHover,
        cfg.labelHeight, cfg.labelFontSize,
        cfg.labelTextColorAuto, (unsigned)((GetRValue(cfg.labelTextColor) << 16) | (GetGValue(cfg.labelTextColor) << 8) | GetBValue(cfg.labelTextColor)),
        cfg.labelAliasFormat.c_str(), cfg.verboseLogging, cfg.palette.size(), cfg.aiIndicatorEnabled, providersJoined.c_str(),
        cfg.usageMeterEnabled, cfg.usageMeterRefreshMinutes);
    return cfg;
}
