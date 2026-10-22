#include "favourites.h"

#include "account.h"
#include "file_util.h"
#include "focus_trace.h"
#include "logger.h"
#include "text_util.h"
#include "vscode_cli.h"
#include "wsl.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>

namespace {

std::wstring GetFavouritesFilePath() {
    return GetExeDir() + L"\\favourites.ini";
}

std::wstring ToLowerCopy(const std::wstring& s) {
    std::wstring out = s;
    std::transform(out.begin(), out.end(), out.begin(), ::towlower);
    return out;
}

std::vector<FavouriteProject> LoadAll() {
    std::vector<FavouriteProject> result;
    std::string content = ReadFileBytes(GetFavouritesFilePath());

    size_t pos = 0;
    while (pos <= content.size()) {
        size_t eol = content.find('\n', pos);
        std::string line = content.substr(pos, (eol == std::string::npos ? content.size() : eol) - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (eol == std::string::npos) pos = content.size() + 1;
        else pos = eol + 1;
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;

        // "=" is the field delimiter (path=label per line), split on the
        // *first* occurrence only -- see text_util.h. Another account's
        // favourite is prefixed "@account|": no absolute path starts with
        // "@", and an account name can't contain "|".
        std::wstring account;
        if (line[0] == '@') {
            size_t bar = line.find('|');
            if (bar == std::string::npos) continue;
            account = UnescapeTextLine(line.substr(1, bar - 1));
            line = line.substr(bar + 1);
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::wstring path = UnescapeTextLine(line.substr(0, eq));
        std::wstring label = UnescapeTextLine(line.substr(eq + 1));
        if (!path.empty() && !label.empty()) result.push_back({label, path, account});
    }
    return result;
}

void SaveAll(const std::vector<FavouriteProject>& all) {
    std::string out;
    for (const FavouriteProject& f : all) {
        if (!f.account.empty()) out += "@" + EscapeTextLine(f.account) + "|";
        out += EscapeTextLine(f.path) + "=" + EscapeTextLine(f.label) + "\n";
    }
    WriteFileBytes(GetFavouritesFilePath(), out);
}

// Case-insensitive path comparison, tolerant of a trailing separator on
// either side: favourites.ini holds whatever path the window reported when
// the user saved it, while the tracked-window paths come back from VS
// Code's own workspaceStorage records, and the two do not necessarily
// agree about a trailing backslash.
bool SamePath(const std::wstring& a, const std::wstring& b) {
    auto trimmed = [](const std::wstring& p) {
        size_t end = p.size();
        while (end > 0 && (p[end - 1] == L'\\' || p[end - 1] == L'/')) end--;
        return p.substr(0, end);
    };
    std::wstring ta = trimmed(a), tb = trimmed(b);
    return !ta.empty() && ta.size() == tb.size() && _wcsicmp(ta.c_str(), tb.c_str()) == 0;
}

// Windows account names are case-insensitive, non-ASCII letters included.
bool SameAccountName(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), (int)a.size(), b.c_str(), (int)b.size(), TRUE) == CSTR_EQUAL;
}

bool IsAlreadyOpen(const FavouriteProject& f, const std::vector<OpenFolder>& openFolders) {
    for (const OpenFolder& open : openFolders) {
        if (SamePath(f.path, open.path) && SameAccountName(f.account, open.account)) return true;
    }
    return false;
}

bool Matches(const FavouriteProject& f, const std::wstring& pathKey, const std::wstring& account) {
    return ToLowerCopy(f.path) == pathKey && SameAccountName(f.account, account);
}

const wchar_t* AccountForLog(const std::wstring& account) {
    return account.empty() ? L"this account" : account.c_str();
}

std::vector<FavouriteProject> g_favourites;
bool g_loaded = false;

void EnsureLoaded() {
    if (!g_loaded) {
        g_favourites = LoadAll();
        g_loaded = true;
    }
}

} // namespace

std::vector<FavouriteProject> LoadFavourites() {
    EnsureLoaded();
    return g_favourites;
}

bool IsFavourite(const std::wstring& path, const std::wstring& account) {
    EnsureLoaded();
    std::wstring key = ToLowerCopy(path);
    return std::any_of(g_favourites.begin(), g_favourites.end(),
                       [&](const FavouriteProject& f) { return Matches(f, key, account); });
}

void AddFavourite(const std::wstring& label, const std::wstring& path, const std::wstring& account) {
    EnsureLoaded();
    if (path.empty() || IsFavourite(path, account)) return;
    g_favourites.push_back({label, path, account});
    SaveAll(g_favourites);
    Log(L"favourites: added [%ls] (%ls) as [%ls]", label.c_str(), path.c_str(), AccountForLog(account));
}

void RefreshFavouriteLabel(const std::wstring& path, const std::wstring& account, const std::wstring& label) {
    EnsureLoaded();
    std::wstring key = ToLowerCopy(path);
    for (FavouriteProject& f : g_favourites) {
        if (!Matches(f, key, account)) continue;
        if (f.label != label) {
            f.label = label;
            SaveAll(g_favourites);
        }
        return;
    }
}

void RemoveFavourite(const std::wstring& path, const std::wstring& account) {
    EnsureLoaded();
    std::wstring key = ToLowerCopy(path);
    auto it = std::remove_if(g_favourites.begin(), g_favourites.end(),
                             [&](const FavouriteProject& f) { return Matches(f, key, account); });
    if (it == g_favourites.end()) return;
    g_favourites.erase(it, g_favourites.end());
    SaveAll(g_favourites);
    Log(L"favourites: removed %ls as [%ls]", path.c_str(), AccountForLog(account));
}

bool IsFavouriteOpen(const FavouriteProject& favourite, const std::vector<OpenFolder>& openFolders) {
    return IsAlreadyOpen(favourite, openFolders);
}

void OpenAllFavourites(HWND runningWindow, HWND owner, const std::vector<OpenFolder>& alreadyOpenFolders,
                       const wchar_t* reason) {
    EnsureLoaded();
    if (g_favourites.empty()) return;

    // Decide what actually needs launching before resolving the shim or
    // announcing anything, so a run where every favourite is already open
    // does nothing at all and says so in one line.
    std::vector<const FavouriteProject*> toOpen;
    for (const FavouriteProject& f : g_favourites) {
        if (IsAlreadyOpen(f, alreadyOpenFolders)) {
            Log(L"favourites: [%ls] is already open as [%ls] -- not relaunching it (a redundant `code -n` would "
                L"only re-activate that window, leaving the others highlighted in the taskbar)",
                f.path.c_str(), AccountForLog(f.account));
            continue;
        }
        toOpen.push_back(&f);
    }
    if (toOpen.empty()) {
        Log(L"favourites: all %zu favourite(s) already open, nothing to launch", g_favourites.size());
        return;
    }

    // This account's shim is only needed for its own Windows folders -- a
    // WSL one (see wsl.h) opens in its distro's Linux VS Code, and another
    // account's finds that account's VS Code by itself.
    auto usesOwnShim = [](const FavouriteProject* f) {
        return f->account.empty() && !ParseWslUncPath(f->path, nullptr, nullptr);
    };
    std::wstring cmdPath;
    if (std::any_of(toOpen.begin(), toOpen.end(), usesOwnShim) && !ResolveVSCodeCliShim(runningWindow, cmdPath)) {
        size_t before = toOpen.size();
        toOpen.erase(std::remove_if(toOpen.begin(), toOpen.end(), usesOwnShim), toOpen.end());
        LogWarn(L"favourites: no VS Code CLI shim -- skipping %zu of this account's favourite(s) in open-all (%ls)",
                before - toOpen.size(), reason);
        if (toOpen.empty()) return;
    }

    // Logged as one batch before the loop, not per favourite: the whole
    // point of the record is that N windows are about to race each other
    // for the foreground, which is a property of the batch (see
    // focus_trace.h's reason 3), not of any single ShellExecuteW. Counts
    // only what is actually being launched, so a run with nothing to do
    // never opens a suspect period and so can never attribute someone
    // alt-tabbing to this app.
    NoteWindowLaunchRequest(reason, toOpen.size());
    Log(L"favourites: open-all (%ls) launching %zu favourite(s)", reason, toOpen.size());

    for (const FavouriteProject* f : toOpen) {
        std::wstring distro, linuxPath;
        if (ParseWslUncPath(f->path, &distro, &linuxPath)) LaunchInWsl(distro, LinuxVSCodeOpenCommand(distro, linuxPath));
        else if (!f->account.empty()) OpenNewVSCodeWindowAs(owner, f->account, f->path, reason);
        else RunVSCodeCli(cmdPath, L"-n \"" + f->path + L"\"");
    }
}
