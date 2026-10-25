#include "worktree_resolver.h"

#include "config.h"
#include "file_util.h"
#include "logger.h"
#include "text_util.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

static std::unordered_map<std::wstring, std::wstring> g_cache; // lowercased worktree leaf name -> main repo name
// One workspaceStorage entry: a folder VS Code has opened, plus the storage
// directory VS Code keeps for it -- written to while that folder is open in
// a window, which is what ResolveFolderPath uses to tell apart several
// recorded folders sharing one leaf name.
struct FolderCandidate {
    std::wstring path;
    std::wstring storageDir; // see LastUsedTime
};
static std::unordered_map<std::wstring, std::vector<FolderCandidate>>
    g_pathCache; // lowercased leaf name -> every recorded folder with that leaf (every entry, not just worktrees)
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

// The folder workspaceStorage entry `storageDir` records, any trailing
// separator trimmed, into `path`. False if it records none (see
// ReadFolderUri); true with `path` empty if its `uri` doesn't parse.
static bool ReadRecordedFolder(const std::wstring& storageDir, std::wstring& path, std::string& uri) {
    if (!ReadFolderUri(storageDir + L"\\workspace.json", uri)) return false;
    path = FileUriToPath(uri);
    while (!path.empty() && (path.back() == L'\\' || path.back() == L'/')) path.pop_back();
    return true;
}

// Calls `f` with each workspaceStorage entry's directory under VS Code user
// data folder `userDir`. False (with GetLastError set) if there's no
// workspaceStorage there, or it can't be read.
template <typename F>
static bool ForEachStorageDir(const std::wstring& userDir, F f) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((userDir + L"\\workspaceStorage\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        f(userDir + L"\\workspaceStorage\\" + fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return true;
}

static void ProcessWorkspaceJson(const std::wstring& storageDir) {
    std::wstring path;
    std::string uri;
    if (!ReadRecordedFolder(storageDir, path, uri)) return;
    if (path.empty()) {
        Log(L"worktree resolver: %ls\\workspace.json -> unparseable uri [%hs]", storageDir.c_str(), uri.c_str());
        return;
    }
    FolderCandidate c;
    c.path = path;
    c.storageDir = storageDir;
    AddRecordedFolder(c);
}

static void ScanUserDataDir(const std::wstring& userDir) {
    int entryCount = 0;
    bool found = ForEachStorageDir(userDir, [&](const std::wstring& storageDir) {
        entryCount++;
        ProcessWorkspaceJson(storageDir);
    });
    if (!found) {
        LogDiag(L"worktree resolver: no workspaceStorage under [%ls] (lastError=%lu)", userDir.c_str(), GetLastError());
        return;
    }
    LogDiag(L"worktree resolver: scanned %d workspaceStorage entries under [%ls]", entryCount, userDir.c_str());
}

// The stable and Insiders VS Code user data folders, whether or not they
// exist; empty if %APPDATA% isn't available.
static std::vector<std::wstring> VSCodeUserDirs() {
    wchar_t appData[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        Log(L"worktree resolver: %%APPDATA%% not available");
        return {};
    }
    std::wstring base(appData);
    return {base + L"\\Code\\User", base + L"\\Code - Insiders\\User"};
}

static void BuildCache() {
    g_cache.clear();
    g_pathCache.clear();

    for (const std::wstring& userDir : VSCodeUserDirs()) ScanUserDataDir(userDir);

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

std::wstring ResolveFolderPath(const std::wstring& name, const std::wstring& repoName) {
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

    // On a miss, the same self-correcting rebuild as ResolveMainRepoName,
    // sharing its cooldown timer -- a miss from either function within the
    // same window shouldn't trigger two rescans back to back.
    if (it == g_pathCache.end() || it->second.empty()) {
        ULONGLONG now = GetTickCount64();
        if (now - g_lastRebuildTick >= kRebuildCooldownMs) {
            BuildCache();
            g_lastRebuildTick = now;
            it = g_pathCache.find(key);
        }
    }
    if (it == g_pathCache.end() || it->second.empty()) return L"";
    const std::vector<FolderCandidate>& candidates = it->second;
    if (candidates.size() == 1) return candidates[0].path;

    // Several recorded folders share this leaf name. Rank them: one that
    // still exists, then one inside the window's own repo (the title's
    // ${activeRepositoryName}, matched both raw and worktree-substituted),
    // then whichever VS Code wrote to most recently -- for a folder open
    // right now, that's its own window.
    const FolderCandidate* best = nullptr;
    bool bestExists = false, bestInRepo = false;
    ULONGLONG bestTime = 0;
    for (const FolderCandidate& c : candidates) {
        bool exists = IsExistingDirectory(c.path);
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

std::vector<std::wstring> RecentFolders(size_t max) {
    std::vector<std::pair<ULONGLONG, FolderCandidate>> found;
    for (const std::wstring& userDir : VSCodeUserDirs()) {
        ForEachStorageDir(userDir, [&](const std::wstring& storageDir) {
            FolderCandidate c;
            std::string uri;
            if (!ReadRecordedFolder(storageDir, c.path, uri) || c.path.empty()) return;
            c.storageDir = storageDir;
            found.push_back({LastUsedTime(c), c});
        });
    }
    std::stable_sort(found.begin(), found.end(),
                     [](const auto& a, const auto& b) { return a.first > b.first; });

    std::vector<std::wstring> result;
    std::vector<std::wstring> seen; // lowercased, alongside `result`
    for (const auto& f : found) {
        if (result.size() >= max) break;
        std::wstring key = ToLowerCopy(f.second.path);
        if (std::find(seen.begin(), seen.end(), key) != seen.end()) continue;
        if (!IsExistingDirectory(f.second.path)) continue;
        seen.push_back(key);
        result.push_back(f.second.path);
    }
    return result;
}

static std::wstring LeafName(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

static std::string TrimLine(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::wstring FolderAliasKey(const std::wstring& folder) {
    // The checkout's root: the nearest folder (from `folder` up) with a
    // ".git" -- a directory in a normal clone, a "gitdir: <path>" file in a
    // worktree or submodule.
    std::wstring root = folder, gitDir;
    while (!root.empty()) {
        std::wstring dotGit = root + L"\\.git";
        DWORD attrs = GetFileAttributesW(dotGit.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES) {
            if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
                gitDir = dotGit;
            } else {
                std::string line = TrimLine(ReadFileBytes(dotGit));
                const std::string prefix = "gitdir:";
                if (line.compare(0, prefix.size(), prefix) == 0) {
                    gitDir = Utf8ToWide(TrimLine(line.substr(prefix.size())));
                    std::replace(gitDir.begin(), gitDir.end(), L'/', L'\\');
                    bool absolute = (gitDir.size() > 1 && gitDir[1] == L':') || gitDir.compare(0, 2, L"\\\\") == 0;
                    if (!gitDir.empty() && !absolute) gitDir = root + L"\\" + gitDir;
                }
            }
            break;
        }
        size_t slash = root.find_last_of(L"\\/");
        if (slash == std::wstring::npos) break;
        root.erase(slash);
    }
    if (gitDir.empty()) return LeafName(folder);

    std::string head = TrimLine(ReadFileBytes(gitDir + L"\\HEAD"));
    std::wstring branch;
    const std::string refPrefix = "ref: refs/heads/";
    if (head.compare(0, refPrefix.size(), refPrefix) == 0) {
        branch = Utf8ToWide(head.substr(refPrefix.size()));
    } else if (head.size() >= 8 && head.compare(0, 4, "ref:") != 0) {
        branch = Utf8ToWide(head.substr(0, 8));
    }
    if (branch.empty()) return LeafName(folder);

    std::wstring repo = LeafName(root);
    std::wstring mainRepo = ResolveMainRepoName(repo);
    return (mainRepo.empty() ? repo : mainRepo) + L" - " + branch;
}

void RefreshWorktreeCache() {
    BuildCache();
    g_cacheBuilt = true;
    g_lastRebuildTick = GetTickCount64();
}

void InvalidateWorktreeCache() {
    g_cacheBuilt = false;
}
