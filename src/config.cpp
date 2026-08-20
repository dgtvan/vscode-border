#include "config.h"
#include "file_util.h"
#include "logger.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

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

Config LoadConfig(const std::wstring& path) {
    Config cfg;
    cfg.palette = DefaultPalette();

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
        } else if (key == "colors") {
            std::vector<COLORREF> palette;
            for (const std::string& item : SplitChar(val, ',')) {
                std::string c = Trim(item);
                if (!c.empty()) palette.push_back(ParseHexColor(c));
            }
            if (!palette.empty()) cfg.palette = palette;
        }
    }

    Log(L"config: loaded thickness=%d opacity=%d rescan_ms=%d show_label=%d show_project_list=%d project_list_opacity_normal=%d project_list_opacity_hover=%d project_list_activate_on_hover=%d label_height=%d label_font_size=%d label_text_color_auto=%d label_text_color=%06X verbose_logging=%d colors=%zu",
        cfg.thickness, cfg.opacity, cfg.rescanIntervalMs, cfg.showLabel, cfg.showProjectList,
        cfg.projectListOpacityNormal, cfg.projectListOpacityHover, cfg.projectListActivateOnHover,
        cfg.labelHeight, cfg.labelFontSize,
        cfg.labelTextColorAuto, (unsigned)((GetRValue(cfg.labelTextColor) << 16) | (GetGValue(cfg.labelTextColor) << 8) | GetBValue(cfg.labelTextColor)),
        cfg.verboseLogging, cfg.palette.size());
    return cfg;
}
