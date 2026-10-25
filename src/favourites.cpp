#include "favourites.h"

#include "file_util.h"
#include "logger.h"
#include "text_util.h"

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
        // *first* occurrence only -- see text_util.h. Files from when a
        // favourite stored a Claude profile prefix non-default ones
        // "@<profile id>|" (no absolute path starts with "@", and none
        // contains "|"); the prefix is skipped, and the next save drops it.
        if (line[0] == '@') {
            size_t bar = line.find('|');
            if (bar == std::string::npos) continue;
            line = line.substr(bar + 1);
        }
        // The alias key, when recorded, follows the label after the *last*
        // tab: a key is a folder/repo/branch name, which can't contain one.
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::wstring path = UnescapeTextLine(line.substr(0, eq));
        std::string rest = line.substr(eq + 1);
        std::wstring aliasKey;
        size_t tab = rest.rfind('\t');
        if (tab != std::string::npos) {
            aliasKey = UnescapeTextLine(rest.substr(tab + 1));
            rest.erase(tab);
        }
        std::wstring label = UnescapeTextLine(rest);
        if (path.empty() || label.empty()) continue;
        // One favourite per folder: a file from when a folder could be a
        // favourite once per profile keeps the first.
        std::wstring key = ToLowerCopy(path);
        if (std::any_of(result.begin(), result.end(),
                        [&](const FavouriteProject& f) { return ToLowerCopy(f.path) == key; })) {
            continue;
        }
        result.push_back({label, path, aliasKey});
    }
    return result;
}

void SaveAll(const std::vector<FavouriteProject>& all) {
    std::string out;
    for (const FavouriteProject& f : all) {
        out += EscapeTextLine(f.path) + "=" + EscapeTextLine(f.label);
        if (!f.aliasKey.empty()) out += "\t" + EscapeTextLine(f.aliasKey);
        out += "\n";
    }
    WriteFileBytes(GetFavouritesFilePath(), out);
}

bool Matches(const FavouriteProject& f, const std::wstring& pathKey) {
    return ToLowerCopy(f.path) == pathKey;
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
                       [&](const FavouriteProject& f) { return Matches(f, key); });
}

void AddFavourite(const std::wstring& label, const std::wstring& path, const std::wstring& aliasKey) {
    EnsureLoaded();
    if (path.empty() || IsFavourite(path)) return;
    g_favourites.push_back({label, path, aliasKey});
    SaveAll(g_favourites);
    Log(L"favourites: added [%ls] (%ls)", label.c_str(), path.c_str());
}

void RefreshFavouriteLabel(const std::wstring& path, const std::wstring& label, const std::wstring& aliasKey) {
    EnsureLoaded();
    std::wstring key = ToLowerCopy(path);
    for (FavouriteProject& f : g_favourites) {
        if (!Matches(f, key)) continue;
        if (f.label == label && f.aliasKey == aliasKey) return;
        f.label = label;
        f.aliasKey = aliasKey;
        SaveAll(g_favourites);
        return;
    }
}

void RemoveFavourite(const std::wstring& path) {
    EnsureLoaded();
    std::wstring key = ToLowerCopy(path);
    auto it = std::remove_if(g_favourites.begin(), g_favourites.end(),
                             [&](const FavouriteProject& f) { return Matches(f, key); });
    if (it == g_favourites.end()) return;
    g_favourites.erase(it, g_favourites.end());
    SaveAll(g_favourites);
    Log(L"favourites: removed %ls", path.c_str());
}
