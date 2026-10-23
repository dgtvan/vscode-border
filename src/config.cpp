#include "config.h"
#include "file_util.h"
#include "logger.h"
#include "text_util.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>

Config g_config;

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

// A folder path from config.ini with its environment variables (%USERPROFILE%)
// expanded and no trailing separator.
static std::wstring ExpandConfigPath(const std::wstring& path) {
    wchar_t buf[MAX_PATH] = {};
    DWORD n = ExpandEnvironmentStringsW(path.c_str(), buf, MAX_PATH);
    std::wstring out = n > 0 && n <= MAX_PATH ? buf : path;
    while (out.size() > 3 && (out.back() == L'\\' || out.back() == L'/')) out.pop_back();
    return out;
}

Config LoadConfig(const std::wstring& path) {
    Config cfg;
    cfg.palette = DefaultPalette();
    ClaudeProfile slots[kMaxClaudeProfiles];

    std::string content = ReadFileBytes(path);
    // Not returned early: the default Claude profile is added below either way.
    if (content.empty()) Log(L"config: not found at %ls, using defaults", path.c_str());

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
        } else if (key.rfind("claude_profile_", 0) == 0) {
            // claude_profile_<N>_name / _profile_path / _icon / _color / _ring_badge
            const char* rest = key.c_str() + strlen("claude_profile_");
            char* end = nullptr;
            long slot = strtol(rest, &end, 10);
            std::string field = (end && *end == '_') ? std::string(end + 1) : std::string();
            if (end == rest || slot < 1 || slot > kMaxClaudeProfiles ||
                (field != "name" && field != "profile_path" && field != "icon" && field != "color" &&
                 field != "ring_badge")) {
                LogWarn(L"config: unknown key [%hs] -- Claude profiles are "
                        L"claude_profile_<1-%d>_name/_profile_path/_icon/_color/_ring_badge",
                        key.c_str(), kMaxClaudeProfiles);
            } else if (field == "color") {
                ClaudeProfile& p = slots[slot - 1];
                p.hasColor = ParseOptionalHexColor(key, val, p.color);
            } else if (field == "ring_badge") {
                // Empty keeps the default (shown).
                slots[slot - 1].ringBadge = !(val == "0" || _stricmp(val.c_str(), "false") == 0);
            } else {
                ClaudeProfile& p = slots[slot - 1];
                (field == "name" ? p.name : field == "icon" ? p.icon : p.path) = Utf8ToWide(val);
            }
        } else if (key.rfind("user_profile_", 0) == 0) {
            // Gone: profiles are Claude Code config folders now, not Windows
            // accounts. Only a set value is worth a word.
            if (!val.empty() && val[0] != '<') {
                LogWarn(L"config: %hs is no longer used -- profiles are claude_profile_<N>_* now; remove the line",
                        key.c_str());
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

    // Slot 1 is the default profile, with or without a _profile_path. Any
    // other slot without one is unused, whatever else it has.
    for (int i = 0; i < kMaxClaudeProfiles; i++) {
        ClaudeProfile p = slots[i];
        p.slot = i + 1;
        p.isDefault = i == 0;
        if (p.path.empty() && !p.isDefault) continue;
        if (!p.path.empty()) {
            p.configDir = ExpandConfigPath(p.path);
            if (p.configDir.size() < 3 || p.configDir[1] != L':' || (p.configDir[2] != L'\\' && p.configDir[2] != L'/')) {
                LogWarn(L"config: claude_profile_%d_profile_path [%ls] isn't an absolute path (like "
                        L"C:\\Users\\me\\.claude-work) -- ignoring the profile",
                        p.slot, p.path.c_str());
                if (!p.isDefault) continue;
                p.path.clear();
            }
        }
        if (p.path.empty()) p.configDir = ExpandConfigPath(L"%USERPROFILE%\\.claude");
        bool duplicate = false;
        for (const ClaudeProfile& q : cfg.claudeProfiles) {
            duplicate = duplicate || CompareStringOrdinal(q.configDir.c_str(), -1, p.configDir.c_str(), -1, TRUE) ==
                                         CSTR_EQUAL;
        }
        if (duplicate) {
            LogWarn(L"config: claude_profile_%d_profile_path [%ls] is an earlier profile's folder -- ignoring it",
                    p.slot, p.configDir.c_str());
            continue;
        }
        // The icon can be a whole base64 image -- logged by size only.
        wchar_t color[8] = L"";
        if (p.hasColor) swprintf(color, 8, L"%02X%02X%02X", GetRValue(p.color), GetGValue(p.color), GetBValue(p.color));
        Log(L"config: Claude profile %d: name=[%ls] folder=[%ls]%ls icon=[%ls] color=[%ls] ring_badge=%d", p.slot,
            p.name.c_str(), p.configDir.c_str(),
            GetFileAttributesW(p.configDir.c_str()) == INVALID_FILE_ATTRIBUTES ? L" (doesn't exist yet)" : L"",
            p.icon.size() > 260 ? (std::to_wstring(p.icon.size()) + L" chars").c_str() : p.icon.c_str(), color,
            p.ringBadge);
        cfg.claudeProfiles.push_back(p);
    }

    std::string providersJoined;
    for (const std::string& p : cfg.aiIndicatorProviders) {
        if (!providersJoined.empty()) providersJoined += ",";
        providersJoined += p;
    }

    Log(L"config: loaded thickness=%d opacity=%d rescan_ms=%d show_label=%d show_project_list=%d project_list_fixed_match_taskbar=%d project_list_opacity_normal=%d project_list_opacity_hover=%d project_list_activate_on_hover=%d label_height=%d label_font_size=%d label_text_color_auto=%d label_text_color=%06X label_alias_format=[%ls] verbose_logging=%d colors=%zu ai_indicator_enabled=%d ai_indicator_provider=%hs usage_meter_enabled=%d usage_meter_refresh_minutes=%d",
        cfg.thickness, cfg.opacity, cfg.rescanIntervalMs, cfg.showLabel, cfg.showProjectList,
        cfg.projectListFixedMatchTaskbar,
        cfg.projectListOpacityNormal, cfg.projectListOpacityHover, cfg.projectListActivateOnHover,
        cfg.labelHeight, cfg.labelFontSize,
        cfg.labelTextColorAuto, (unsigned)((GetRValue(cfg.labelTextColor) << 16) | (GetGValue(cfg.labelTextColor) << 8) | GetBValue(cfg.labelTextColor)),
        cfg.labelAliasFormat.c_str(), cfg.verboseLogging, cfg.palette.size(), cfg.aiIndicatorEnabled, providersJoined.c_str(),
        cfg.usageMeterEnabled, cfg.usageMeterRefreshMinutes);
    return cfg;
}
