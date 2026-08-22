#include "worktree_resolver.h"

#include "config.h"
#include "file_util.h"
#include "logger.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

static std::unordered_map<std::wstring, std::wstring> g_cache; // lowercased worktree leaf name -> main repo name
static std::unordered_map<std::wstring, std::wstring> g_pathCache; // lowercased leaf name -> full path (every
                                                                     // entry, not just worktrees)
static bool g_cacheBuilt = false;

static std::wstring ToLowerCopy(const std::wstring& s) {
    std::wstring out = s;
    std::transform(out.begin(), out.end(), out.begin(), ::towlower);
    return out;
}

static std::string PercentDecode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size() && isxdigit((unsigned char)s[i + 1]) && isxdigit((unsigned char)s[i + 2])) {
            out.push_back((char)strtol(s.substr(i + 1, 2).c_str(), nullptr, 16));
            i += 2;
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

// Converts a "file:///d%3A/Src/foo" URI (as stored in workspace.json) into a
// native Windows path ("d:\Src\foo").
static std::wstring FileUriToPath(const std::string& uri) {
    const std::string prefix = "file:///";
    if (uri.compare(0, prefix.size(), prefix) != 0) return L"";

    std::string decoded = PercentDecode(uri.substr(prefix.size()));
    std::replace(decoded.begin(), decoded.end(), '/', '\\');

    int wlen = MultiByteToWideChar(CP_UTF8, 0, decoded.c_str(), -1, nullptr, 0);
    if (wlen <= 0) return L"";
    std::wstring wpath(wlen - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, decoded.c_str(), -1, &wpath[0], wlen);
    return wpath;
}

// Extracts the (single, first) string value of "folder": "..." from a
// workspace.json file -- multi-root ("workspace": "...") entries are
// skipped since they don't map to a single worktree checkout.
static bool ReadFolderUri(const std::wstring& jsonPath, std::string& outUri) {
    std::string content = ReadFileBytes(jsonPath);
    if (content.empty()) return false;

    size_t keyPos = content.find("\"folder\"");
    if (keyPos == std::string::npos) return false;
    size_t colon = content.find(':', keyPos);
    if (colon == std::string::npos) return false;
    size_t q1 = content.find('"', colon);
    if (q1 == std::string::npos) return false;
    size_t q2 = content.find('"', q1 + 1);
    if (q2 == std::string::npos) return false;

    outUri = content.substr(q1 + 1, q2 - q1 - 1);
    return true;
}

static const wchar_t* kWorktreesSuffix = L".worktrees";

static void ProcessWorkspaceJson(const std::wstring& jsonPath) {
    std::string uri;
    if (!ReadFolderUri(jsonPath, uri)) return;

    std::wstring path = FileUriToPath(uri);
    while (!path.empty() && (path.back() == L'\\' || path.back() == L'/')) path.pop_back();
    if (path.empty()) {
        Log(L"worktree resolver: %ls -> unparseable uri [%hs]", jsonPath.c_str(), uri.c_str());
        return;
    }

    size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return;
    std::wstring leaf = path.substr(slash + 1);
    std::wstring parent = path.substr(0, slash);

    // Every workspace.json entry maps its own leaf folder name to its real
    // path, not just worktrees -- kept separately from the worktree-name
    // mapping below since most entries aren't worktrees at all.
    g_pathCache[ToLowerCopy(leaf)] = path;

    size_t parentSlash = parent.find_last_of(L"\\/");
    std::wstring parentName = (parentSlash == std::wstring::npos) ? parent : parent.substr(parentSlash + 1);

    size_t suffixLen = wcslen(kWorktreesSuffix);
    if (parentName.size() <= suffixLen) {
        Log(L"worktree resolver: path=[%ls] parent=[%ls] -- not a worktree layout (parent too short)", path.c_str(),
            parentName.c_str());
        return;
    }
    std::wstring parentTail = ToLowerCopy(parentName.substr(parentName.size() - suffixLen));
    if (parentTail != kWorktreesSuffix) {
        Log(L"worktree resolver: path=[%ls] parent=[%ls] -- not a worktree layout (no .worktrees suffix)",
            path.c_str(), parentName.c_str());
        return;
    }

    std::wstring mainRepoName = parentName.substr(0, parentName.size() - suffixLen);
    Log(L"worktree resolver: leaf=[%ls] -> mainRepo=[%ls] (from path=[%ls])", leaf.c_str(), mainRepoName.c_str(),
        path.c_str());
    g_cache[ToLowerCopy(leaf)] = mainRepoName;
}

static void ScanUserDataDir(const std::wstring& userDir) {
    std::wstring pattern = userDir + L"\\workspaceStorage\\*";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        Log(L"worktree resolver: no workspaceStorage under [%ls] (lastError=%lu)", userDir.c_str(), GetLastError());
        return;
    }
    int entryCount = 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        entryCount++;
        ProcessWorkspaceJson(userDir + L"\\workspaceStorage\\" + fd.cFileName + L"\\workspace.json");
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    Log(L"worktree resolver: scanned %d workspaceStorage entries under [%ls]", entryCount, userDir.c_str());
}

static void BuildCache() {
    g_cache.clear();
    g_pathCache.clear();

    wchar_t appData[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        Log(L"worktree resolver: %%APPDATA%% not available");
        return;
    }

    std::wstring base(appData);
    ScanUserDataDir(base + L"\\Code\\User");
    ScanUserDataDir(base + L"\\Code - Insiders\\User");

    Log(L"worktree resolver: cache built, %zu worktree(s) mapped", g_cache.size());
}

static ULONGLONG g_lastRebuildTick = 0;
static const ULONGLONG kRebuildCooldownMs = 15000;

std::wstring ResolveMainRepoName(const std::wstring& repoName) {
    if (!g_cacheBuilt) {
        BuildCache();
        g_cacheBuilt = true;
        g_lastRebuildTick = GetTickCount64();
    }

    std::wstring key = ToLowerCopy(repoName);
    auto it = g_cache.find(key);

    // A miss might just mean this worktree didn't exist yet when the cache
    // was last built (e.g. it was created/opened after the app started or
    // after the last rebuild) -- rebuild once and retry before giving up,
    // so this self-corrects without needing a manual Reload Config.
    // Rate-limited so a genuinely-not-a-worktree repo (the common case)
    // doesn't trigger a rescan on every single lookup.
    if (it == g_cache.end()) {
        ULONGLONG now = GetTickCount64();
        if (now - g_lastRebuildTick >= kRebuildCooldownMs) {
            BuildCache();
            g_lastRebuildTick = now;
            it = g_cache.find(key);
        }
    }

    if (it == g_cache.end()) {
        LogDiag(L"worktree resolver: no mapping for repo=[%ls] (cache has %zu entries)", repoName.c_str(),
                g_cache.size());
        return L"";
    }
    LogDiag(L"worktree resolver: resolved repo=[%ls] -> mainRepo=[%ls]", repoName.c_str(), it->second.c_str());
    return it->second;
}

std::wstring ResolveFolderPath(const std::wstring& name) {
    if (!g_cacheBuilt) {
        BuildCache();
        g_cacheBuilt = true;
        g_lastRebuildTick = GetTickCount64();
    }

    std::wstring key = ToLowerCopy(name);
    auto it = g_pathCache.find(key);

    // Same self-correcting rebuild-on-miss as ResolveMainRepoName, and
    // sharing its cooldown timer -- a miss from either function within the
    // same window shouldn't trigger two rescans back to back.
    if (it == g_pathCache.end()) {
        ULONGLONG now = GetTickCount64();
        if (now - g_lastRebuildTick >= kRebuildCooldownMs) {
            BuildCache();
            g_lastRebuildTick = now;
            it = g_pathCache.find(key);
        }
    }

    if (it == g_pathCache.end()) return L"";
    return it->second;
}

void RefreshWorktreeCache() {
    BuildCache();
    g_cacheBuilt = true;
    g_lastRebuildTick = GetTickCount64();
}
