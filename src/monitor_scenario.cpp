#include "monitor_scenario.h"

#include "file_util.h"

#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <map>
#include <vector>

namespace {

// Every currently-active monitor's PNP hardware id, e.g.
// "MONITOR\ACI27E2\{4d36e96e-e325-11ce-bfc1-08002be10318}\0001" -- walks
// adapters (\\.\DISPLAY1, ...) then each adapter's monitors, since a
// monitor's id is only available via the adapter that's driving it.
std::vector<std::wstring> ActiveMonitorHardwareIds() {
    std::vector<std::wstring> ids;
    DISPLAY_DEVICEW adapter = {};
    adapter.cb = sizeof(adapter);
    for (DWORD adapterIndex = 0; EnumDisplayDevicesW(nullptr, adapterIndex, &adapter, 0); adapterIndex++) {
        if (adapter.StateFlags & DISPLAY_DEVICE_ACTIVE) {
            DISPLAY_DEVICEW monitor = {};
            monitor.cb = sizeof(monitor);
            for (DWORD monitorIndex = 0; EnumDisplayDevicesW(adapter.DeviceName, monitorIndex, &monitor, 0);
                 monitorIndex++) {
                if (monitor.StateFlags & DISPLAY_DEVICE_ACTIVE) ids.push_back(monitor.DeviceID);
                monitor.cb = sizeof(monitor);
            }
        }
        adapter.cb = sizeof(adapter);
    }
    return ids;
}

// PNP hardware ids are ASCII by spec (letters/digits/backslash/braces/
// hyphens), so a lossless narrow<->wide roundtrip doesn't need real UTF-8
// conversion -- just truncate/widen each code unit.
std::string NarrowAscii(const std::wstring& s) {
    std::string out;
    out.reserve(s.size());
    for (wchar_t c : s) out.push_back(c <= 0x7F ? (char)c : '_');
    return out;
}

std::wstring WidenAscii(const std::string& s) {
    std::wstring out;
    out.reserve(s.size());
    for (unsigned char c : s) out.push_back((wchar_t)c);
    return out;
}

std::wstring GetStateFilePath() {
    return GetExeDir() + L"\\project_list_hud_positions.ini";
}

struct PlacementRecord {
    int x = 0;
    int y = 0;
    int width = 0;
};

// Section-per-scenario INI, mirroring vscode_settings.cpp's state-file
// pattern: `[<scenario key>]` header, `key=value` lines under it.
std::map<std::wstring, PlacementRecord> LoadAll() {
    std::map<std::wstring, PlacementRecord> result;
    std::string content = ReadFileBytes(GetStateFilePath());
    std::string currentSection;

    size_t pos = 0;
    while (pos <= content.size()) {
        size_t eol = content.find('\n', pos);
        std::string line = content.substr(pos, (eol == std::string::npos ? content.size() : eol) - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (eol == std::string::npos) pos = content.size() + 1;
        else pos = eol + 1;

        if (line.size() >= 2 && line.front() == '[' && line.back() == ']') {
            currentSection = line.substr(1, line.size() - 2);
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos || currentSection.empty()) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        PlacementRecord& rec = result[WidenAscii(currentSection)];
        if (key == "x") rec.x = atoi(val.c_str());
        else if (key == "y") rec.y = atoi(val.c_str());
        else if (key == "width") rec.width = atoi(val.c_str());
    }
    return result;
}

void SaveAll(const std::map<std::wstring, PlacementRecord>& all) {
    std::string out;
    for (const auto& entry : all) {
        out += "[" + NarrowAscii(entry.first) + "]\n";
        out += "x=" + std::to_string(entry.second.x) + "\n";
        out += "y=" + std::to_string(entry.second.y) + "\n";
        out += "width=" + std::to_string(entry.second.width) + "\n";
    }
    WriteFileBytes(GetStateFilePath(), out);
}

} // namespace

std::wstring GetMonitorScenarioKey() {
    std::vector<std::wstring> ids = ActiveMonitorHardwareIds();
    std::sort(ids.begin(), ids.end()); // order-independent -- same monitors, any adapter enumeration order
    if (ids.empty()) return L"none";

    std::wstring key;
    for (size_t i = 0; i < ids.size(); i++) {
        if (i) key += L"+";
        key += ids[i];
    }
    return key;
}

SavedHudPlacement LoadHudPlacement(const std::wstring& scenarioKey) {
    std::map<std::wstring, PlacementRecord> all = LoadAll();
    auto it = all.find(scenarioKey);
    SavedHudPlacement result;
    if (it == all.end()) return result;
    result.found = true;
    result.x = it->second.x;
    result.y = it->second.y;
    result.width = it->second.width;
    return result;
}

void SaveHudPlacement(const std::wstring& scenarioKey, int x, int y, int width) {
    std::map<std::wstring, PlacementRecord> all = LoadAll();
    PlacementRecord& rec = all[scenarioKey];
    rec.x = x;
    rec.y = y;
    rec.width = width;
    SaveAll(all);
}
