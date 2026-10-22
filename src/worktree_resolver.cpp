#include "worktree_resolver.h"

#include "account.h"
#include "config.h"
#include "file_util.h"
#include "logger.h"
#include "text_util.h"
#include "wsl.h"

#include <windows.h>
#include <sddl.h>

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
    ULONGLONG lastUsed = 0;    // when storageDir is empty (read in WSL, or by an account helper): its
                               // LastUsedTime as of the scan -- seconds in WSL, a FILETIME from a helper
    std::wstring account;      // Windows only: the other account (see account.h) whose VS Code recorded
                               // it, empty for this app's own
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
// Other Windows accounts (see account.h) ResolveFolderPath has been asked
// about. A window running as another account is that account's VS Code,
// which records its folders in that account's own workspaceStorage. That's
// inside its profile folder, which this account normally can't read; where
// it can (an administrator granted it), every rebuild scans it like this
// account's. Otherwise a helper reads it -- this exe run with
// kListVSCodeFoldersArg as that account, with its password saved in
// Credential Manager -- on a worker thread, since starting a process as
// another account takes a while; the next lookup picks its answer up. That
// is re-read like a WSL distro's records: on a miss (no more often than
// kWslRescanCooldownMs), after InvalidateWorktreeCache, or on
// RefreshWorktreeCache.
struct AccountRecords {
    std::wstring appData; // its Roaming AppData folder, empty if not found
    bool direct = false;  // its workspaceStorage is readable from here
    std::vector<FolderCandidate> folders; // the helper's last answer
    bool stale = true;
    bool fetching = false; // a helper is running
    ULONGLONG lastScanTick = 0;
    DWORD lastError = 0; // of the last helper start, to log a failure once rather than on every retry
};
static std::map<std::wstring, AccountRecords> g_accounts;
// A helper's answer, handed from its worker thread to the next lookup.
struct AccountFetch {
    bool ok = false;
    DWORD error = 0; // when !ok: StartHiddenProcessAsAccount's, or 0 if the helper itself failed
    std::vector<FolderCandidate> folders;
};
static SRWLOCK g_fetchLock = SRWLOCK_INIT;
static std::map<std::wstring, AccountFetch> g_fetched; // guarded by g_fetchLock

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

static void ProcessWorkspaceJson(const std::wstring& storageDir, const std::wstring& account) {
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
    c.account = account;
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
        c.lastUsed = strtoull(line.substr(0, tab).c_str(), nullptr, 10);
        rec.folders.push_back(c);
    }
    LogDiag(L"worktree resolver: %zu folder(s) recorded in WSL distro [%ls]", rec.folders.size(), distro.c_str());
}

static void ScanUserDataDir(const std::wstring& userDir, const std::wstring& account = L"") {
    int entryCount = 0;
    bool found = ForEachStorageDir(userDir, [&](const std::wstring& storageDir) {
        entryCount++;
        ProcessWorkspaceJson(storageDir, account);
    });
    if (!found) {
        LogDiag(L"worktree resolver: no workspaceStorage under [%ls] (lastError=%lu)", userDir.c_str(), GetLastError());
        return;
    }
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
    for (const auto& kv : g_accounts) {
        if (kv.second.direct) {
            ScanUserDataDir(kv.second.appData + L"\\Code\\User", kv.first);
            ScanUserDataDir(kv.second.appData + L"\\Code - Insiders\\User", kv.first);
        } else {
            for (const FolderCandidate& c : kv.second.folders) AddRecordedFolder(c);
        }
    }
    for (const auto& kv : g_wsl) {
        for (const FolderCandidate& c : kv.second.folders) AddRecordedFolder(c);
    }

    Log(L"worktree resolver: cache built, %zu folder name(s), %zu worktree(s) mapped", g_pathCache.size(),
        g_cache.size());
}

// `account`'s Roaming AppData folder: its profile folder, from the
// registry's profile list, + AppData\Roaming. Empty if the account or its
// profile (made on its first sign-in) can't be found.
static std::wstring AccountRoamingAppData(const std::wstring& account) {
    BYTE sid[SECURITY_MAX_SID_SIZE];
    DWORD sidSize = sizeof(sid), domainLen = 256;
    wchar_t domain[256];
    SID_NAME_USE use;
    if (!LookupAccountNameW(nullptr, account.c_str(), sid, &sidSize, domain, &domainLen, &use)) return L"";
    LPWSTR sidText = nullptr;
    if (!ConvertSidToStringSidW(sid, &sidText)) return L"";
    std::wstring key = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\ProfileList\\" + std::wstring(sidText);
    LocalFree(sidText);
    wchar_t raw[MAX_PATH] = {}, expanded[MAX_PATH] = {};
    DWORD size = sizeof(raw);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), L"ProfileImagePath",
                     RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, nullptr, raw, &size) != ERROR_SUCCESS) {
        return L"";
    }
    DWORD n = ExpandEnvironmentStringsW(raw, expanded, MAX_PATH);
    if (n == 0 || n > MAX_PATH) return L"";
    return std::wstring(expanded) + L"\\AppData\\Roaming";
}

// The first time a window of `account` asks: whether this account can read
// that one's VS Code records itself, which a rebuild then does.
static void NoteAccount(const std::wstring& account) {
    if (g_accounts.count(account)) return;
    AccountRecords& rec = g_accounts[account];
    rec.appData = AccountRoamingAppData(account);
    auto noop = [](const std::wstring&) {};
    rec.direct = !rec.appData.empty() && (ForEachStorageDir(rec.appData + L"\\Code\\User", noop) ||
                                          ForEachStorageDir(rec.appData + L"\\Code - Insiders\\User", noop));
    if (rec.direct) {
        Log(L"worktree resolver: reading [%ls]'s VS Code records under [%ls]", account.c_str(), rec.appData.c_str());
        g_cacheBuilt = false;
    } else {
        Log(L"worktree resolver: [%ls]'s VS Code records aren't readable from this account (profile folder [%ls]) "
            L"-- reading them as that account",
            account.c_str(), rec.appData.c_str());
    }
}

// Worker thread: runs the folder-list helper as the account `param` (a
// heap std::wstring, freed here) and leaves its answer in g_fetched.
// Doesn't log -- Log isn't safe off the main thread -- so the lookup that
// picks the answer up does.
static DWORD WINAPI FetchAccountFoldersThread(LPVOID param) {
    std::wstring account = *(std::wstring*)param;
    delete (std::wstring*)param;
    AccountFetch fetch;
    SECURITY_ATTRIBUTES inherit = {sizeof(inherit), nullptr, TRUE};
    HANDLE ourOut = nullptr, childOut = nullptr;
    if (!CreatePipe(&ourOut, &childOut, &inherit, 0)) {
        fetch.error = GetLastError();
    } else {
        SetHandleInformation(ourOut, HANDLE_FLAG_INHERIT, 0);
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring commandLine = L"\"" + std::wstring(exe) + L"\" " + kListVSCodeFoldersArg;
        HANDLE child =
            StartHiddenProcessAsAccount(account, exe, commandLine, GetExeDir(), nullptr, childOut, fetch.error);
        CloseHandle(childOut);
        std::string out;
        if (child) {
            char buf[4096];
            DWORD read = 0;
            while (ReadFile(ourOut, buf, sizeof(buf), &read, nullptr) && read > 0) out.append(buf, read);
            DWORD exitCode = 1;
            if (WaitForSingleObject(child, 5000) == WAIT_OBJECT_0) GetExitCodeProcess(child, &exitCode);
            CloseHandle(child);
            fetch.ok = exitCode == 0;
        }
        CloseHandle(ourOut);
        // "<last used>\t<path>" lines -- see RunListVSCodeFolders.
        size_t pos = 0;
        while (fetch.ok && pos < out.size()) {
            size_t eol = out.find('\n', pos);
            std::string line = out.substr(pos, (eol == std::string::npos ? out.size() : eol) - pos);
            pos = eol == std::string::npos ? out.size() : eol + 1;
            size_t tab = line.find('\t');
            if (tab == std::string::npos) continue;
            FolderCandidate c;
            c.path = Utf8ToWide(line.substr(tab + 1));
            if (c.path.empty()) continue;
            c.account = account;
            c.lastUsed = strtoull(line.substr(0, tab).c_str(), nullptr, 10);
            fetch.folders.push_back(c);
        }
    }
    AcquireSRWLockExclusive(&g_fetchLock);
    g_fetched[account] = std::move(fetch);
    ReleaseSRWLockExclusive(&g_fetchLock);
    return 0;
}

// Starts a helper for `account` if its records are stale (first ask, or
// InvalidateWorktreeCache) -- or, with `missed`, if the last one started
// over kWslRescanCooldownMs ago -- and none is running already.
static void FetchAccountFoldersIfDue(const std::wstring& account, bool missed) {
    AccountRecords& rec = g_accounts[account];
    if (rec.direct || rec.fetching) return;
    bool due = rec.stale || (missed && GetTickCount64() - rec.lastScanTick >= kWslRescanCooldownMs);
    if (!due) return;
    rec.stale = false;
    rec.lastScanTick = GetTickCount64();
    std::wstring* param = new std::wstring(account);
    HANDLE thread = CreateThread(nullptr, 0, FetchAccountFoldersThread, param, 0, nullptr);
    if (!thread) {
        delete param;
        Log(L"worktree resolver: couldn't start a thread to read [%ls]'s VS Code records, lastError=%lu",
            account.c_str(), GetLastError());
        return;
    }
    CloseHandle(thread);
    rec.fetching = true;
}

static ULONGLONG g_lastRebuildTick = 0;

// Takes in the helpers' answers that came in since the last lookup, and
// rebuilds the caches with them.
static void PickUpAccountFolders() {
    std::map<std::wstring, AccountFetch> fetched;
    AcquireSRWLockExclusive(&g_fetchLock);
    fetched.swap(g_fetched);
    ReleaseSRWLockExclusive(&g_fetchLock);
    if (fetched.empty()) return;
    for (auto& kv : fetched) {
        AccountRecords& rec = g_accounts[kv.first];
        rec.fetching = false;
        AccountFetch& f = kv.second;
        if (f.ok) {
            rec.folders = std::move(f.folders);
            rec.lastError = 0;
            LogDiag(L"worktree resolver: %zu folder(s) recorded by [%ls]'s VS Code", rec.folders.size(),
                    kv.first.c_str());
            continue;
        }
        DWORD err = f.error ? f.error : ERROR_GEN_FAILURE;
        if (err == rec.lastError) continue;
        rec.lastError = err;
        if (f.error == ERROR_NOT_FOUND) {
            LogWarn(L"worktree resolver: can't read [%ls]'s VS Code records -- no password saved for it (open a VS "
                    L"Code window as it from the hub's + button and tick Remember); its windows' folders are looked "
                    L"up in this account's records meanwhile",
                    kv.first.c_str());
        } else if (f.error) {
            LogWarn(L"worktree resolver: can't read [%ls]'s VS Code records -- couldn't start the helper as it: %ls",
                    kv.first.c_str(), StartAsErrorText(f.error).c_str());
        } else {
            LogWarn(L"worktree resolver: can't read [%ls]'s VS Code records -- the helper run as it failed",
                    kv.first.c_str());
        }
    }
    BuildCache();
    g_cacheBuilt = true;
    g_lastRebuildTick = GetTickCount64();
}

// Windows account names are case-insensitive, non-ASCII letters included.
static bool SameAccountName(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), (int)a.size(), b.c_str(), (int)b.size(), TRUE) == CSTR_EQUAL;
}

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
    if (c.storageDir.empty()) return c.lastUsed; // only ever compared against others read the same way
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

std::wstring ResolveFolderPath(const std::wstring& name, const std::wstring& repoName, const std::wstring& wslDistro,
                              const std::wstring& account) {
    PickUpAccountFolders();
    if (!wslDistro.empty()) RefreshWslIfDue(wslDistro, false);
    bool otherAccount = !account.empty() && wslDistro.empty();
    if (otherAccount) {
        NoteAccount(account);
        FetchAccountFoldersIfDue(account, false);
    }

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
    // window, that distro's Linux VS Code for a WSL one -- and for another
    // account's window, that account's, else this account's (its records
    // unreadable, or not written yet: on one machine a folder of the same
    // name is most likely the same folder). On a miss, the same
    // self-correcting rebuild as ResolveMainRepoName, sharing its cooldown
    // timer -- a miss from either function within the same window shouldn't
    // trigger two rescans back to back -- or for a WSL window, a re-read of
    // just that distro (see RefreshWslIfDue), and for another account's
    // window whose records only its helper can read, another helper run
    // (see FetchAccountFoldersIfDue), even while this account's records
    // stand in for them.
    auto recordedBy = [&](const FolderCandidate& c, const std::wstring& acct) {
        return c.distro == wslDistro && SameAccountName(c.account, acct);
    };
    auto hasCandidateOf = [&](const std::wstring& acct) {
        return it != g_pathCache.end() && std::any_of(it->second.begin(), it->second.end(),
                                                      [&](const FolderCandidate& c) { return recordedBy(c, acct); });
    };
    auto hasCandidate = [&]() { return hasCandidateOf(account) || hasCandidateOf(L""); };
    if (otherAccount && !hasCandidateOf(account)) FetchAccountFoldersIfDue(account, true);
    bool missed = otherAccount && g_accounts[account].direct ? !hasCandidateOf(account) : !hasCandidate();
    if (missed) {
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
    std::wstring recorder = hasCandidateOf(account) ? account : L"";
    std::vector<const FolderCandidate*> candidates;
    for (const FolderCandidate& c : it->second) {
        if (recordedBy(c, recorder)) candidates.push_back(&c);
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
    // An account whose profile folder wasn't found is looked for again on
    // its next ask; the others' helpers rerun on theirs.
    for (auto it = g_accounts.begin(); it != g_accounts.end();) {
        it->second.stale = true;
        it = it->second.appData.empty() && !it->second.fetching ? g_accounts.erase(it) : std::next(it);
    }
    BuildCache();
    g_cacheBuilt = true;
    g_lastRebuildTick = GetTickCount64();
}

void InvalidateWorktreeCache() {
    g_cacheBuilt = false;
    for (auto& kv : g_wsl) kv.second.stale = true;
    for (auto& kv : g_accounts) kv.second.stale = true;
}

const wchar_t* const kListVSCodeFoldersArg = L"--list-vscode-folders";

int RunListVSCodeFolders() {
    wchar_t appData[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return 1;
    std::string out;
    for (const wchar_t* product : {L"\\Code\\User", L"\\Code - Insiders\\User"}) {
        ForEachStorageDir(std::wstring(appData) + product, [&](const std::wstring& storageDir) {
            FolderCandidate c;
            std::string uri;
            if (!ReadRecordedFolder(storageDir, c.path, uri) || c.path.empty()) return;
            c.storageDir = storageDir;
            out += std::to_string(LastUsedTime(c)) + "\t" + WideToUtf8(c.path) + "\n";
        });
    }
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), out.data(), (DWORD)out.size(), &written, nullptr);
    return 0;
}
