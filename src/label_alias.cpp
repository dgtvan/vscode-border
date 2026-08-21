#include "label_alias.h"

#include "file_util.h"
#include "text_util.h"

#include <map>

namespace {

std::wstring GetAliasFilePath() {
    return GetExeDir() + L"\\label_aliases.ini";
}

std::map<std::wstring, std::wstring> LoadAll() {
    std::map<std::wstring, std::wstring> result;
    std::string content = ReadFileBytes(GetAliasFilePath());

    size_t pos = 0;
    while (pos <= content.size()) {
        size_t eol = content.find('\n', pos);
        std::string line = content.substr(pos, (eol == std::string::npos ? content.size() : eol) - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (eol == std::string::npos) pos = content.size() + 1;
        else pos = eol + 1;
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;

        // "=" is the field delimiter (label=alias per line), split on the
        // *first* occurrence only -- see text_util.h.
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::wstring label = UnescapeTextLine(line.substr(0, eq));
        std::wstring alias = UnescapeTextLine(line.substr(eq + 1));
        if (!label.empty() && !alias.empty()) result[label] = alias;
    }
    return result;
}

void SaveAll(const std::map<std::wstring, std::wstring>& all) {
    std::string out;
    for (const auto& entry : all) {
        out += EscapeTextLine(entry.first) + "=" + EscapeTextLine(entry.second) + "\n";
    }
    WriteFileBytes(GetAliasFilePath(), out);
}

std::map<std::wstring, std::wstring> g_aliases;
bool g_loaded = false;

void EnsureLoaded() {
    if (!g_loaded) {
        g_aliases = LoadAll();
        g_loaded = true;
    }
}

} // namespace

std::wstring ResolveAlias(const std::wstring& label) {
    EnsureLoaded();
    auto it = g_aliases.find(label);
    return it != g_aliases.end() ? it->second : label;
}

void SetAlias(const std::wstring& label, const std::wstring& alias) {
    EnsureLoaded();
    if (alias.empty()) g_aliases.erase(label);
    else g_aliases[label] = alias;
    SaveAll(g_aliases);
}

void ReloadAliases() {
    g_aliases = LoadAll();
    g_loaded = true;
}
