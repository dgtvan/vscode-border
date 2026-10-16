#include "worktree_resolver.h"

#include "config.h"
#include "file_util.h"
#include "logger.h"
#include "wsl.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <map>
#include <unordered_map>
#include <vector>

static std::unordered_map<std::wstring, std::wstring> g_cache; // lowercased worktree leaf name -> main repo name
// One workspaceStorage entry: a folder VS Code has opened, plus the storage
// directory VS Code keeps for it -- written to while that folder is open in
// a window, which is what ResolveFolderPath uses to tell apart several
// recorded folders sharing one leaf name.
struct FolderCandidate {
    std::wstring path;
    std::wstring storageDir; // Windows only -- see LastUsedTime
    std::wstring distro;     // the WSL distro Linux VS Code recorded it in (path is then its
                             // \\wsl.localhost form, see wsl.h), empty for Windows VS Code
    ULONGLONG wslLastUsed = 0; // WSL only: storage's mtime as of the scan, in seconds
};
static std::unordered_map<std::wstring, std::vector<FolderCandidate>>
    g_pathCache; // lowercased leaf name -> every recorded folder with that leaf (every entry, not just worktrees)
static bool g_cacheBuilt = false;
// Linux VS Code's records per WSL distro ResolveFolderPath has been asked
// about. Fetching them costs a wsl.exe round-trip on this (the UI) thread,
// so unlike the Windows records they're not re-read on every rebuild --
// which happens every kRebuildCooldownMs while any lookup misses, the
// common case -- but kept here and just re-added to the caches each time;
// a distro is re-read only when a lookup for it misses (no more often than
// kWslRescanCooldownMs), after InvalidateWorktreeCache, or on
// RefreshWorktreeCache. Asking about a distro implies a window from it is
// open, so it's running and reading it starts nothing.
struct WslRecords {
    std::vector<FolderCandidate> folders;
    bool stale = true;
    ULONGLONG lastScanTick = 0;
};
static std::map<std::wstring, WslRecords> g_wsl;
static const ULONGLONG kWslRescanCooldownMs = 60000;

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
// native Windows path ("d:\Src\foo") -- or, with `linux`, a Linux one's
// "file:///home/me/foo" into "/home/me/foo".
static std::wstring FileUriToPath(const std::string& uri, bool linux = false) {
    const std::string prefix = "file:///";
    if (uri.compare(0, prefix.size(), prefix) != 0) return L"";

    std::string decoded = PercentDecode(uri.substr(prefix.size() - (linux ? 1 : 0)));
    if (!linux) std::replace(decoded.begin(), decoded.end(), '/', '\\');

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

// Records one folder VS Code has opened (`candidate.path`, any trailing
// separator already trimmed) in both caches.
static void AddRecordedFolder(const FolderCandidate& candidate) {
    const std::wstring& path = candidate.path;

    size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return;
    std::wstring leaf = path.substr(slash + 1);
    std::wstring parent = path.substr(0, slash);

    // Every workspace.json entry maps its own leaf folder name to its real
    // path, not just worktrees -- kept separately from the worktree-name
    // mapping below since most entries aren't worktrees at all. A list, not
    // a single path: leaf names repeat (every repo's "src", a worktree named
    // after the same branch in two repos), and whichever entry happened to
    // be scanned last would otherwise hand a window some other project's
    // folder.
    g_pathCache[ToLowerCopy(leaf)].push_back(candidate);

    size_t parentSlash = parent.find_last_of(L"\\/");
    std::wstring parentName = (parentSlash == std::wstring::npos) ? parent : parent.substr(parentSlash + 1);

    size_t suffixLen = wcslen(kWorktreesSuffix);
    if (parentName.size() <= suffixLen) {
        LogDiag(L"worktree resolver: path=[%ls] parent=[%ls] -- not a worktree layout (parent too short)", path.c_str(),
            parentName.c_str());
        return;
    }
    std::wstring parentTail = ToLowerCopy(parentName.substr(parentName.size() - suffixLen));
    if (parentTail != kWorktreesSuffix) {
        LogDiag(L"worktree resolver: path=[%ls] parent=[%ls] -- not a worktree layout (no .worktrees suffix)",
            path.c_str(), parentName.c_str());
        return;
    }

    std::wstring mainRepoName = parentName.substr(0, parentName.size() - suffixLen);
    LogDiag(L"worktree resolver: leaf=[%ls] -> mainRepo=[%ls] (from path=[%ls])", leaf.c_str(), mainRepoName.c_str(),
        path.c_str());
    g_cache[ToLowerCopy(leaf)] = mainRepoName;
}

static void ProcessWorkspaceJson(const std::wstring& storageDir) {
    std::wstring jsonPath = storageDir + L"\\workspace.json";
    std::string uri;
    if (!ReadFolderUri(jsonPath, uri)) return;

    std::wstring path = FileUriToPath(uri);
    while (!path.empty() && (path.back() == L'\\' || path.back() == L'/')) path.pop_back();
    if (path.empty()) {
        Log(L"worktree resolver: %ls -> unparseable uri [%hs]", jsonPath.c_str(), uri.c_str());
        return;
    }
    FolderCandidate c;
    c.path = path;
    c.storageDir = storageDir;
    AddRecordedFolder(c);
}

// Linux VS Code's records inside `distro` (its default user's
// workspaceStorage, both variants), fetched in one wsl.exe call as
// "<storage mtime>\t<folder uri>" lines, into g_wsl -- the caches pick them
// up on the next BuildCache.
static void ScanWslDistro(const std::wstring& distro) {
    WslRecords& rec = g_wsl[distro];
    rec.stale = false;
    rec.lastScanTick = GetTickCount64();
    std::string out;
    bool ok = RunInWsl(distro,
                       {L"sh", L"-c",
                        L"for d in \"$HOME/.config/Code/User/workspaceStorage\"/* "
                        L"\"$HOME/.config/Code - Insiders/User/workspaceStorage\"/*; do "
                        L"[ -f \"$d/workspace.json\" ] || continue; "
                        L"u=$(sed -n 's/.*\"folder\"[[:space:]]*:[[:space:]]*\"\\([^\"]*\\)\".*/\\1/p' "
                        L"\"$d/workspace.json\" | head -n 1); "
                        L"[ -n \"$u\" ] || continue; "
                        L"t=$(stat -c %Y \"$d/state.vscdb\" 2>/dev/null || stat -c %Y \"$d\"); "
                        L"printf '%s\\t%s\\n' \"$t\" \"$u\"; done; true"},
                       "", &out);
    if (!ok) {
        Log(L"worktree resolver: couldn't read VS Code's records in WSL distro [%ls]", distro.c_str());
        return;
    }
    rec.folders.clear();
    size_t pos = 0;
    while (pos < out.size()) {
        size_t eol = out.find('\n', pos);
        std::string line = out.substr(pos, (eol == std::string::npos ? out.size() : eol) - pos);
        pos = eol == std::string::npos ? out.size() : eol + 1;
        size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;
        std::wstring linuxPath = FileUriToPath(line.substr(tab + 1), true);
        while (linuxPath.size() > 1 && linuxPath.back() == L'/') linuxPath.pop_back();
        if (linuxPath.empty() || linuxPath[0] != L'/') continue;
        FolderCandidate c;
        c.path = WslUncPath(distro, linuxPath);
        c.distro = distro;
        c.wslLastUsed = strtoull(line.substr(0, tab).c_str(), nullptr, 10);
        rec.folders.push_back(c);
    }
    LogDiag(L"worktree resolver: %zu folder(s) recorded in WSL distro [%ls]", rec.folders.size(), distro.c_str());
}

static void ScanUserDataDir(const std::wstring& userDir) {
    std::wstring pattern = userDir + L"\\workspaceStorage\\*";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        LogDiag(L"worktree resolver: no workspaceStorage under [%ls] (lastError=%lu)", userDir.c_str(), GetLastError());
        return;
    }
    int entryCount = 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        entryCount++;
        ProcessWorkspaceJson(userDir + L"\\workspaceStorage\\" + fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    LogDiag(L"worktree resolver: scanned %d workspaceStorage entries under [%ls]", entryCount, userDir.c_str());
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
    for (const auto& kv : g_wsl) {
        for (const FolderCandidate& c : kv.second.folders) AddRecordedFolder(c);
    }

    Log(L"worktree resolver: cache built, %zu folder name(s), %zu worktree(s) mapped", g_pathCache.size(),
        g_cache.size());
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

static bool IsExistingDirectory(const std::wstring& path) {
    DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
}

// When VS Code last wrote this folder's storage: the newer of the storage
// directory itself (bumped as files come and go in it, e.g. SQLite's
// journal) and its state.vscdb (rewritten while the folder is open in a
// window). Read live rather than cached, since it keeps moving for as long
// as a window has the folder open.
static ULONGLONG LastUsedTime(const FolderCandidate& c) {
    if (!c.distro.empty()) return c.wslLastUsed; // only ever compared against others from the same distro
    ULONGLONG newest = 0;
    for (const std::wstring& p : {c.storageDir, c.storageDir + L"\\state.vscdb"}) {
        WIN32_FILE_ATTRIBUTE_DATA data;
        if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &data)) continue;
        ULONGLONG t = ((ULONGLONG)data.ftLastWriteTime.dwHighDateTime << 32) | data.ftLastWriteTime.dwLowDateTime;
        if (t > newest) newest = t;
    }
    return newest;
}

// True if one of `path`'s components is `repo` itself or the
// "<repo>.worktrees" folder holding its worktrees -- i.e. `path` is that
// repo's root, somewhere inside it, or one of its worktrees.
static bool PathBelongsToRepo(const std::wstring& path, const std::wstring& repo) {
    if (repo.empty()) return false;
    std::wstring worktrees = repo + kWorktreesSuffix;
    size_t start = 0;
    while (start <= path.size()) {
        size_t end = path.find_first_of(L"\\/", start);
        if (end == std::wstring::npos) end = path.size();
        std::wstring part = path.substr(start, end - start);
        if (_wcsicmp(part.c_str(), repo.c_str()) == 0 || _wcsicmp(part.c_str(), worktrees.c_str()) == 0) return true;
        start = end + 1;
    }
    return false;
}

// Re-reads `distro`'s records if they're stale (first ask, or
// InvalidateWorktreeCache) -- or, with `missed`, if the last read is older
// than kWslRescanCooldownMs -- and rebuilds the caches with them. True if it
// did.
static bool RefreshWslIfDue(const std::wstring& distro, bool missed) {
    WslRecords& rec = g_wsl[distro];
    bool due = rec.stale || (missed && GetTickCount64() - rec.lastScanTick >= kWslRescanCooldownMs);
    if (!due) return false;
    ScanWslDistro(distro);
    BuildCache();
    g_cacheBuilt = true;
    g_lastRebuildTick = GetTickCount64();
    return true;
}

std::wstring ResolveFolderPath(const std::wstring& name, const std::wstring& repoName, const std::wstring& wslDistro) {
    if (!wslDistro.empty()) RefreshWslIfDue(wslDistro, false);

    // Up front, not where the ranking below uses it: on a miss it can
    // rebuild the cache, which would free the candidate list being ranked.
    std::wstring mainRepo = repoName.empty() ? L"" : ResolveMainRepoName(repoName);

    if (!g_cacheBuilt) {
        BuildCache();
        g_cacheBuilt = true;
        g_lastRebuildTick = GetTickCount64();
    }

    std::wstring key = ToLowerCopy(name);
    auto it = g_pathCache.find(key);

    // Only folders recorded by the same VS Code count: Windows for a Windows
    // window, that distro's Linux VS Code for a WSL one. On a miss, the same
    // self-correcting rebuild as ResolveMainRepoName, sharing its cooldown
    // timer -- a miss from either function within the same window shouldn't
    // trigger two rescans back to back -- or for a WSL window, a re-read of
    // just that distro (see RefreshWslIfDue).
    auto hasCandidate = [&]() {
        return it != g_pathCache.end() && std::any_of(it->second.begin(), it->second.end(), [&](const FolderCandidate& c) {
                   return c.distro == wslDistro;
               });
    };
    if (!hasCandidate()) {
        if (!wslDistro.empty()) {
            if (RefreshWslIfDue(wslDistro, true)) it = g_pathCache.find(key);
        } else {
            ULONGLONG now = GetTickCount64();
            if (now - g_lastRebuildTick >= kRebuildCooldownMs) {
                BuildCache();
                g_lastRebuildTick = now;
                it = g_pathCache.find(key);
            }
        }
    }
    if (!hasCandidate()) return L"";
    std::vector<const FolderCandidate*> candidates;
    for (const FolderCandidate& c : it->second) {
        if (c.distro == wslDistro) candidates.push_back(&c);
    }
    if (candidates.size() == 1) return candidates[0]->path;

    // Several recorded folders share this leaf name. Rank them: one that
    // still exists, then one inside the window's own repo (the title's
    // ${activeRepositoryName}, matched both raw and worktree-substituted),
    // then whichever VS Code wrote to most recently -- for a folder open
    // right now, that's its own window.
    const FolderCandidate* best = nullptr;
    bool bestExists = false, bestInRepo = false;
    ULONGLONG bestTime = 0;
    for (const FolderCandidate* cp : candidates) {
        const FolderCandidate& c = *cp;
        // Not checked for a WSL folder: its \\wsl.localhost path can be
        // unreachable (the share switched off) or slow to time out.
        bool exists = !c.distro.empty() || IsExistingDirectory(c.path);
        bool inRepo = PathBelongsToRepo(c.path, repoName) || PathBelongsToRepo(c.path, mainRepo);
        ULONGLONG time = LastUsedTime(c);
        bool better;
        if (!best) better = true;
        else if (exists != bestExists) better = exists;
        else if (inRepo != bestInRepo) better = inRepo;
        else better = time > bestTime;
        if (!better) continue;
        best = &c;
        bestExists = exists;
        bestInRepo = inRepo;
        bestTime = time;
    }
    LogDiag(L"worktree resolver: %zu folders named [%ls] (repo=[%ls]) -> [%ls] exists=%d inRepo=%d",
            candidates.size(), name.c_str(), repoName.c_str(), best->path.c_str(), bestExists, bestInRepo);
    return best->path;
}

void RefreshWorktreeCache() {
    for (auto& kv : g_wsl) ScanWslDistro(kv.first);
    BuildCache();
    g_cacheBuilt = true;
    g_lastRebuildTick = GetTickCount64();
}

void InvalidateWorktreeCache() {
    g_cacheBuilt = false;
    for (auto& kv : g_wsl) kv.second.stale = true;
}
