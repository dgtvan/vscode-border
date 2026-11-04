#include "assistant_profile.h"
#include "profile_config.h"
#include "file_util.h"
#include "text_util.h"

#include <algorithm>
#include <cstdlib>
#include <map>
#include <sstream>

namespace {
struct Assignment { DWORD pid = 0; ULONGLONG created = 0; std::wstring profile; };
struct Pending {
    ULONGLONG id = 0, tick = 0;
    std::wstring folder, profile;
    std::vector<HWND> before;
};
std::map<HWND, Assignment> g_assignments;
std::vector<Pending> g_pending;
bool g_loaded = false;
ULONGLONG g_nextRequest = 0;
std::wstring StatePath() { return GetExeDir() + L"\\window_profiles.ini"; }
Assignment Identity(HWND window) {
    Assignment a;
    if (!IsWindow(window)) return a;
    GetWindowThreadProcessId(window, &a.pid);
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, a.pid);
    FILETIME created, exited, kernel, user;
    if (process) {
        if (GetProcessTimes(process, &created, &exited, &kernel, &user))
            a.created = ((ULONGLONG)created.dwHighDateTime << 32) | created.dwLowDateTime;
        CloseHandle(process);
    }
    return a;
}
bool Valid(HWND window, const Assignment& a) {
    Assignment now = Identity(window);
    return a.created && now.pid == a.pid && now.created == a.created;
}
void Load() {
    if (g_loaded) return;
    g_loaded = true;
    std::istringstream in(ReadFileBytes(StatePath()));
    std::string line;
    while (std::getline(in, line)) {
        size_t first = line.find('|');
        if (first == std::string::npos) continue;
        size_t second = line.find('|', first + 1);
        if (second == std::string::npos) continue;
        size_t third = line.find('|', second + 1);
        if (third == std::string::npos) continue;
        HWND window = (HWND)(uintptr_t)strtoull(line.substr(0, first).c_str(), nullptr, 16);
        Assignment a;
        a.pid = (DWORD)strtoul(line.substr(first + 1, second - first - 1).c_str(), nullptr, 10);
        a.created = strtoull(line.substr(second + 1, third - second - 1).c_str(), nullptr, 10);
        a.profile = UnescapeTextLine(line.substr(third + 1));
        if (Valid(window, a)) g_assignments[window] = a;
    }
}
void Save() {
    std::ostringstream out;
    for (auto it = g_assignments.begin(); it != g_assignments.end();) {
        if (!Valid(it->first, it->second)) { it = g_assignments.erase(it); continue; }
        out << std::hex << (uintptr_t)it->first << std::dec << '|' << it->second.pid << '|' << it->second.created
            << '|' << EscapeTextLine(it->second.profile) << '\n';
        ++it;
    }
    WriteFileBytes(StatePath(), out.str());
}
BOOL CALLBACK Collect(HWND window, LPARAM data) {
    ((std::vector<HWND>*)data)->push_back(window);
    return TRUE;
}
}

ULONGLONG NoteProfileLaunch(const std::wstring& folder, const std::wstring& id) {
    Load();
    Pending p;
    p.id = ++g_nextRequest;
    p.tick = GetTickCount64();
    p.folder = NormalizeProfilePath(folder);
    p.profile = id;
    EnumWindows(Collect, (LPARAM)&p.before);
    g_pending.push_back(p);
    return p.id;
}

void CancelProfileLaunch(ULONGLONG request) {
    g_pending.erase(std::remove_if(g_pending.begin(), g_pending.end(), [&](const Pending& p) { return p.id == request; }), g_pending.end());
}

bool ResolveLaunchedWindowProfile(HWND target, const std::wstring& path, const std::wstring& folderName,
                                  std::wstring& profile) {
    Load();
    auto existing = g_assignments.find(target);
    if (existing != g_assignments.end()) {
        if (Valid(target, existing->second)) { profile = existing->second.profile; return true; }
        g_assignments.erase(existing);
    }
    ULONGLONG now = GetTickCount64();
    g_pending.erase(std::remove_if(g_pending.begin(), g_pending.end(), [&](const Pending& p) { return now - p.tick > 60000; }), g_pending.end());
    for (auto it = g_pending.begin(); it != g_pending.end(); ++it) {
        if (std::find(it->before.begin(), it->before.end(), target) != it->before.end()) continue;
        bool match;
        if (it->folder.empty()) match = path.empty() && folderName.empty();
        else if (!path.empty()) match = _wcsicmp(it->folder.c_str(), NormalizeProfilePath(path).c_str()) == 0;
        else {
            std::wstring leaf = it->folder.substr(it->folder.find_last_of(L'\\') + 1);
            match = !folderName.empty() && _wcsicmp(leaf.c_str(), folderName.c_str()) == 0;
        }
        if (!match) continue;
        Assignment a = Identity(target);
        if (!a.created) return false;
        profile = a.profile = it->profile;
        g_assignments[target] = a;
        g_pending.erase(it);
        Save();
        return true;
    }
    return false;
}

void ForgetWindowProfile(HWND target) {
    Load();
    if (g_assignments.erase(target)) Save();
}
