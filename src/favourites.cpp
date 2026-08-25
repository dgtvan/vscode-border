#include "favourites.h"

#include "file_util.h"
#include "logger.h"
#include "text_util.h"

#include <shellapi.h>
#include <windows.h>

#include <algorithm>
#include <cwctype>

namespace {

std::wstring GetFavouritesFilePath() {
    return GetExeDir() + L"\\favourites.ini";
}

std::wstring GetEnvVar(const wchar_t* name) {
    wchar_t buf[MAX_PATH] = {};
    DWORD len = GetEnvironmentVariableW(name, buf, MAX_PATH);
    return (len > 0 && len < MAX_PATH) ? std::wstring(buf) : std::wstring();
}

// Same shim project_list_hud.cpp's ResolveVSCodeCliShim resolves (see its
// comment for why the CLI shim, not Code.exe directly, is required for -n
// to work) -- but found without a running window's process path to start
// from, since this runs at app startup before any VS Code window is
// necessarily tracked yet. Tries the shim's directory on PATH first (the
// common case: VS Code's installer offers "Add to PATH", on by default),
// then the well-known per-user and machine-wide install locations. Stable
// build is tried before Insiders at each step, since there's no
// running-window signal here to know which the user actually favours.
bool ResolveVSCodeCliShimStandalone(std::wstring& outCmdPath) {
    const wchar_t* kShimNames[] = {L"code.cmd", L"code-insiders.cmd"};
    for (const wchar_t* name : kShimNames) {
        wchar_t found[MAX_PATH] = {};
        if (SearchPathW(nullptr, name, nullptr, MAX_PATH, found, nullptr) > 0) {
            outCmdPath = found;
            return true;
        }
    }

    std::wstring localAppData = GetEnvVar(L"LOCALAPPDATA");
    std::wstring programFiles = GetEnvVar(L"ProgramFiles");
    std::vector<std::wstring> candidates;
    if (!localAppData.empty()) {
        candidates.push_back(localAppData + L"\\Programs\\Microsoft VS Code\\bin\\code.cmd");
        candidates.push_back(localAppData + L"\\Programs\\Microsoft VS Code Insiders\\bin\\code-insiders.cmd");
    }
    if (!programFiles.empty()) {
        candidates.push_back(programFiles + L"\\Microsoft VS Code\\bin\\code.cmd");
        candidates.push_back(programFiles + L"\\Microsoft VS Code Insiders\\bin\\code-insiders.cmd");
    }
    for (const std::wstring& candidate : candidates) {
        DWORD attrs = GetFileAttributesW(candidate.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
            outCmdPath = candidate;
            return true;
        }
    }
    return false;
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
        // *first* occurrence only -- see text_util.h.
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::wstring path = UnescapeTextLine(line.substr(0, eq));
        std::wstring label = UnescapeTextLine(line.substr(eq + 1));
        if (!path.empty() && !label.empty()) result.push_back({label, path});
    }
    return result;
}

void SaveAll(const std::vector<FavouriteProject>& all) {
    std::string out;
    for (const FavouriteProject& f : all) {
        out += EscapeTextLine(f.path) + "=" + EscapeTextLine(f.label) + "\n";
    }
    WriteFileBytes(GetFavouritesFilePath(), out);
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

bool IsFavourite(const std::wstring& path) {
    EnsureLoaded();
    std::wstring key = ToLowerCopy(path);
    return std::any_of(g_favourites.begin(), g_favourites.end(),
                       [&key](const FavouriteProject& f) { return ToLowerCopy(f.path) == key; });
}

void AddFavourite(const std::wstring& label, const std::wstring& path) {
    EnsureLoaded();
    if (path.empty() || IsFavourite(path)) return;
    g_favourites.push_back({label, path});
    SaveAll(g_favourites);
}

void RemoveFavourite(const std::wstring& path) {
    EnsureLoaded();
    std::wstring key = ToLowerCopy(path);
    auto it = std::remove_if(g_favourites.begin(), g_favourites.end(),
                             [&key](const FavouriteProject& f) { return ToLowerCopy(f.path) == key; });
    if (it == g_favourites.end()) return;
    g_favourites.erase(it, g_favourites.end());
    SaveAll(g_favourites);
}

void OpenAllFavouritesAtStartup() {
    EnsureLoaded();
    if (g_favourites.empty()) return;

    std::wstring cmdPath;
    if (!ResolveVSCodeCliShimStandalone(cmdPath)) {
        LogWarn(L"favourites: could not locate the VS Code CLI shim (code.cmd/code-insiders.cmd) on PATH or in "
                L"the usual install locations -- skipping startup auto-open of %zu favourite(s)",
                g_favourites.size());
        return;
    }

    for (const FavouriteProject& f : g_favourites) {
        std::wstring args = L"-n \"" + f.path + L"\"";
        HINSTANCE result = ShellExecuteW(nullptr, L"open", cmdPath.c_str(), args.c_str(), nullptr, SW_HIDE);
        if ((INT_PTR)result <= 32) {
            LogWarn(L"favourites: startup auto-open failed for [%ls] via %ls, code=%Id", f.path.c_str(),
                    cmdPath.c_str(), (INT_PTR)result);
        }
    }
}
