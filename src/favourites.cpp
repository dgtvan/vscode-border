#include "favourites.h"

#include "file_util.h"
#include "text_util.h"

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
