#include "assistant_profile.h"

#include "config.h"
#include "file_util.h"
#include "json_scan.h"
#include "layered_rendering.h"
#include "logger.h"
#include "text_util.h"
#include "profile_icon.h"
#include "profile_config.h"
#include "codex_usage.h"

#include <algorithm>
#include <cwctype>
#include <map>
#include <vector>

namespace {

std::wstring TrimTrailingSeparators(std::wstring path) {
    while (path.size() > 3 && (path.back() == L'\\' || path.back() == L'/')) path.pop_back();
    return path;
}

bool SamePathText(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), (int)a.size(), b.c_str(), (int)b.size(), TRUE) == CSTR_EQUAL;
}

std::wstring EnvVar(const wchar_t* name) {
    wchar_t buf[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(name, buf, MAX_PATH);
    return n > 0 && n < MAX_PATH ? buf : L"";
}

std::wstring LeafName(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

ULONGLONG FileTimeValue(const FILETIME& ft) {
    return ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

// A JSON string token (quotes included) as text: undoes \", \\, \/, \n,
// \t, \r, \b, \f and \uXXXX (surrogate pairs too).
std::wstring DecodeJsonString(const std::string& raw) {
    if (raw.size() < 2 || raw.front() != '"' || raw.back() != '"') return L"";
    std::wstring out;
    std::string run;
    auto flush = [&]() {
        out += Utf8ToWide(run);
        run.clear();
    };
    for (size_t i = 1; i + 1 < raw.size(); i++) {
        if (raw[i] != '\\' || i + 2 >= raw.size()) {
            run += raw[i];
            continue;
        }
        char c = raw[++i];
        if (c == 'u' && i + 4 < raw.size()) {
            flush();
            out += (wchar_t)strtoul(raw.substr(i + 1, 4).c_str(), nullptr, 16);
            i += 4;
            continue;
        }
        run += c == 'n' ? '\n' : c == 't' ? '\t' : c == 'r' ? '\r' : c == 'b' ? '\b' : c == 'f' ? '\f' : c;
    }
    flush();
    return out;
}

// ---- Who's signed in ----

// Where Claude Code keeps profile `id`'s global state (.claude.json, with
// the signed-in account) when CLAUDE_CONFIG_DIR names its folder -- as it
// always does for a window this app opens: inside that folder.
std::wstring GlobalStatePath(const std::wstring& id) {
    return ProfileConfigDir(id) + L"\\.claude.json";
}

// ---- Icons ----

std::wstring ConfiguredIconSource(const std::wstring& id) {
    const Profile* p = FindProfile(id);
    return p ? ResolveProfileIconSource(p->icon) : L"";
}

// ---- Which profile a window runs ----

// One ide\*.lock file Claude Code's VS Code extension writes when it starts
// in a window: {"pid":<VS Code's main process>,"workspaceFolders":[...],...}.
// Not always removed when the window closes, and every window of a VS Code
// shares the pid, so a lock can be left from a window closed since.
struct IdeLock {
    ULONGLONG written = 0; // the file's last write time, as parsed
    DWORD pid = 0;
    std::vector<std::wstring> folders;
};

// Every lock file seen, by full path, re-parsed only when rewritten.
std::map<std::wstring, IdeLock> g_lockFiles;

// A live window's lock: its profile, and its folders.
struct LiveLock {
    std::wstring profile;
    ULONGLONG written = 0;
    std::vector<std::wstring> folders;
};
std::vector<LiveLock> g_liveLocks;
ULONGLONG g_lockScanTick = 0;
std::wstring g_lockScanDirs; // the config folders the last scan covered, to rescan right after a reload

const ULONGLONG kLockRescanMs = 2000;

IdeLock ParseLockFile(const std::wstring& path, ULONGLONG written) {
    IdeLock lock;
    lock.written = written;
    std::string content = ReadFileBytes(path);
    if (JsonScan::FindRootObjectOpenBrace(content) == std::string::npos) return lock;
    JsonScan::Span root = JsonScan::EnsureRootObject(content);
    JsonScan::KeyEntry pid = JsonScan::FindKeyInObject(content, root.start, root.end, "pid");
    if (pid.found) lock.pid = (DWORD)strtoul(content.c_str() + pid.valueStart, nullptr, 10);
    JsonScan::KeyEntry folders = JsonScan::FindKeyInObject(content, root.start, root.end, "workspaceFolders");
    if (folders.found && folders.valueStart < content.size() && content[folders.valueStart] == '[') {
        for (const JsonScan::Span& e : JsonScan::FindArrayElements(content, folders.valueStart + 1, folders.valueEnd - 1)) {
            std::wstring folder = DecodeJsonString(content.substr(e.start, e.end - e.start));
            if (!folder.empty()) lock.folders.push_back(TrimTrailingSeparators(folder));
        }
    }
    return lock;
}

// Whether `pid` is still the VS Code that a lock written at `written`
// names: a running Code.exe started before then -- not one that has reused
// the pid of a VS Code that's gone.
bool IsLockOwnerAlive(DWORD pid, ULONGLONG written) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;
    wchar_t path[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    FILETIME created, exited, kernel, user;
    DWORD exitCode = 0;
    bool alive = QueryFullProcessImageNameW(process, 0, path, &size) &&
                 GetProcessTimes(process, &created, &exited, &kernel, &user) &&
                 GetExitCodeProcess(process, &exitCode) && exitCode == STILL_ACTIVE && FileTimeValue(created) <= written;
    CloseHandle(process);
    std::wstring name = LeafName(path);
    return alive && (_wcsicmp(name.c_str(), L"Code.exe") == 0 || _wcsicmp(name.c_str(), L"Code - Insiders.exe") == 0);
}

void RescanLocksIfDue() {
    std::wstring dirs;
    for (const Profile& p : g_config.profiles) if (p.type == ProfileType::Claude) dirs += p.configDir + L"|";
    ULONGLONG now = GetTickCount64();
    if (dirs == g_lockScanDirs && now - g_lockScanTick < kLockRescanMs) return;
    g_lockScanDirs = dirs;
    g_lockScanTick = now;

    g_liveLocks.clear();
    std::map<std::wstring, IdeLock> seen;
    for (const Profile& p : g_config.profiles) {
        if (p.type != ProfileType::Claude) continue;
        std::wstring ideDir = p.configDir + L"\\ide\\";
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((ideDir + L"*.lock").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            std::wstring path = ideDir + fd.cFileName;
            ULONGLONG written = FileTimeValue(fd.ftLastWriteTime);
            auto it = g_lockFiles.find(path);
            IdeLock lock = it != g_lockFiles.end() && it->second.written == written ? it->second
                                                                                    : ParseLockFile(path, written);
            seen[path] = lock;
            if (lock.pid == 0 || !IsLockOwnerAlive(lock.pid, written)) continue;
            g_liveLocks.push_back({ProfileId(p), written, lock.folders});
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    g_lockFiles.swap(seen);
}

} // namespace

std::vector<std::wstring> ConfiguredProfileIds() {
    std::vector<std::wstring> out;
    for (const Profile& p : g_config.profiles) {
        out.push_back(ProfileId(p));
    }
    return out;
}

const Profile* FindProfile(const std::wstring& id) {
    if (id.empty()) return nullptr;
    for (const Profile& p : g_config.profiles) if (SameProfile(ProfileId(p), id)) return &p;
    return nullptr;
}

bool SameProfile(const std::wstring& a, const std::wstring& b) { return SamePathText(a, b); }

std::wstring ProfileConfigDir(const std::wstring& id) {
    const Profile* p = FindProfile(id);
    return p ? p->configDir : L"";
}

std::wstring ProfileEmail(const std::wstring& id) {
    const Profile* profile = FindProfile(id);
    if (!profile) return L"";
    if (profile->type == ProfileType::Codex) {
        for (const auto& s : GetCodexUsageSnapshots()) if (SameProfile(s.profile, id)) return s.email;
        return L"";
    }
    // Per file, with the write time it was read at: .claude.json is large
    // and rewritten often, but its email only changes on a new sign-in.
    struct Cached {
        ULONGLONG written = 0;
        std::wstring email;
    };
    static std::map<std::wstring, Cached> cache;
    std::wstring path = GlobalStatePath(id);
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return L"";
    ULONGLONG written = FileTimeValue(data.ftLastWriteTime);
    Cached& c = cache[path];
    if (c.written == written) return c.email;
    c.written = written;
    c.email.clear();
    std::string content = ReadFileBytes(path);
    if (JsonScan::FindRootObjectOpenBrace(content) == std::string::npos) return L"";
    JsonScan::Span root = JsonScan::EnsureRootObject(content);
    JsonScan::KeyEntry account = JsonScan::FindKeyInObject(content, root.start, root.end, "oauthAccount");
    if (!account.found || account.valueStart >= content.size() || content[account.valueStart] != '{') return L"";
    JsonScan::KeyEntry email =
        JsonScan::FindKeyInObject(content, account.valueStart + 1, account.valueEnd - 1, "emailAddress");
    if (email.found) c.email = DecodeJsonString(content.substr(email.valueStart, email.valueEnd - email.valueStart));
    return c.email;
}

std::wstring ProfileName(const std::wstring& id) {
    const Profile* p = FindProfile(id);
    if (!p) return id.empty() ? L"None" : L"Unavailable profile";
    if (!p->name.empty()) return p->name;
    std::wstring email = ProfileEmail(id);
    if (!email.empty()) return email;
    return LeafName(p->configDir);
}

std::wstring ProfileMenuText(const std::wstring& id) {
    std::wstring out;
    for (wchar_t c : ProfileName(id)) {
        if (c == L'&') out += L'&';
        out += c;
    }
    return out;
}

bool ShowsProfileIcon(const std::wstring& id) {
    const Profile* p = FindProfile(id);
    return p && (!p->isDefault || !p->icon.empty());
}

void BlendProfileIcon(const std::wstring& id, UINT32* pixels, int surfaceWidth, int surfaceHeight, int left,
                            int top, const RECT& clip) {
    // Per profile, with the icon source and name it was rendered from, so a
    // changed _icon or _name takes effect on Reload Config.
    struct CachedIcon {
        std::wstring source, name;
        std::vector<UINT32> pixels;
    };
    static std::map<std::wstring, CachedIcon> cache;
    std::wstring source = ConfiguredIconSource(id), name = ProfileName(id);
    auto it = cache.find(id);
    if (it == cache.end() || it->second.source != source || it->second.name != name) {
        Log(L"claude_profile: icon for [%ls] from %ls", name.c_str(), DescribeProfileIconSource(source).c_str());
        it = cache.insert_or_assign(id, CachedIcon{source, name, RenderProfileIcon(name, source, kProfileIconSize)})
                 .first;
    }
    if (it->second.pixels.empty()) return;
    BlendPremultipliedIcon(it->second.pixels.data(), kProfileIconSize, pixels, surfaceWidth, surfaceHeight, left, top,
                           clip);
}

HBITMAP GetProfileMenuBitmap(const std::wstring& id) {
    if (!ShowsProfileIcon(id)) return nullptr;
    // Keyed like BlendProfileIcon's cache; a bitmap replaced by a
    // changed icon is kept rather than freed, since a menu may still be
    // showing it.
    static std::map<std::wstring, HBITMAP> cache;
    std::wstring source = ConfiguredIconSource(id), name = ProfileName(id);
    std::wstring key = id + L"|" + name + L"|" + source;
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;

    int size = GetSystemMetrics(SM_CXSMICON);
    std::vector<UINT32> icon = RenderProfileIcon(name, source, size);
    HBITMAP bmp = nullptr;
    if (!icon.empty()) {
        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = size;
        bmi.bmiHeader.biHeight = -size; // top-down
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        bmp = CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (bmp && bits) memcpy(bits, icon.data(), icon.size() * sizeof(UINT32));
    }
    cache[key] = bmp;
    return bmp;
}

bool ProfileOfWindow(HWND target, const std::wstring& path, const std::wstring& folderName, std::wstring& profile) {
    if (ResolveLaunchedWindowProfile(target, path, folderName, profile)) return true;
    RescanLocksIfDue();
    profile.clear();
    std::wstring folder = TrimTrailingSeparators(path);
    // By path; without one, by folder name. Either way the newest lock wins.
    const LiveLock* best = nullptr;
    for (const LiveLock& lock : g_liveLocks) {
        for (const std::wstring& f : lock.folders) {
            bool match = !folder.empty() ? SamePathText(f, folder)
                                         : !folderName.empty() && _wcsicmp(LeafName(f).c_str(), folderName.c_str()) == 0;
            if (match && (!best || lock.written > best->written)) best = &lock;
        }
    }
    if (best) {
        profile = best->profile;
        return true;
    }
    if (folder.empty() && folderName.empty()) {
        // A window with no folder open: its lock lists none, so nothing
        // says which of them it is -- unless every such window runs the
        // same profile.
        const std::wstring* bare = nullptr;
        for (const LiveLock& lock : g_liveLocks) {
            if (!lock.folders.empty()) continue;
            if (bare && !SamePathText(*bare, lock.profile)) return false;
            bare = &lock.profile;
        }
        if (bare) profile = *bare;
        return bare != nullptr;
    }
    return false;
}

std::wstring ProfileEnvironment(const std::wstring& id) {
    const Profile* profile = FindProfile(id);
    std::vector<std::wstring> vars;
    if (wchar_t* block = GetEnvironmentStringsW()) {
        for (const wchar_t* p = block; *p; p += wcslen(p) + 1) {
            if (_wcsnicmp(p, L"CLAUDE_CONFIG_DIR=", 18) != 0 && _wcsnicmp(p, L"CODEX_HOME=", 11) != 0) vars.push_back(p);
        }
        FreeEnvironmentStringsW(block);
    }
    // Explicit empty values also override VS Code's initial process env.
    vars.push_back(L"CLAUDE_CONFIG_DIR=" + (profile && profile->type == ProfileType::Claude ? profile->configDir : L""));
    vars.push_back(L"CODEX_HOME=" + (profile && profile->type == ProfileType::Codex ? profile->configDir : L""));
    std::sort(vars.begin(), vars.end(), [](const std::wstring& a, const std::wstring& b) {
        return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_LESS_THAN;
    });
    std::wstring out;
    for (const auto& v : vars) out += v + L'\0';
    out += L'\0';
    return out;
}

std::wstring ProfileExtensionArguments(const std::wstring& id) {
    const Profile* p = FindProfile(id);
    std::wstring args;
    if (!p || p->type != ProfileType::Claude) args += L" --disable-extension anthropic.claude-code";
    if (!p || p->type != ProfileType::Codex) args += L" --disable-extension openai.chatgpt";
    return args;
}

void CheckClaudeProfileFolders() {
    const Profile* p = nullptr;
    for (const auto& candidate : g_config.profiles)
        if (candidate.type == ProfileType::Claude && candidate.isDefault) p = &candidate;
    std::wstring home = EnvVar(L"USERPROFILE");
    if (!p || !SamePathText(p->configDir, home + L"\\.claude")) return;
    std::wstring inside = p->configDir + L"\\.claude.json", outside = home + L"\\.claude.json";
    if (GetFileAttributesW(inside.c_str()) != INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesW(outside.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return;
    }
    LogWarn(L"claude_profile: the default Claude profile's state is in %ls, but windows are opened with "
            L"CLAUDE_CONFIG_DIR=%ls, where Claude Code looks for %ls -- copy the file there (and set "
            L"CLAUDE_CONFIG_DIR for your user account too, so Claude Code run from a terminal agrees), or Claude "
            L"Code in those windows starts the profile over",
            outside.c_str(), p->configDir.c_str(), inside.c_str());
}
