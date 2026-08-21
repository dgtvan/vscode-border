#include "label_alias.h"

#include "file_util.h"

#include <windows.h>

#include <map>

namespace {

std::wstring GetAliasFilePath() {
    return GetExeDir() + L"\\label_aliases.ini";
}

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string out((size_t)len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &out[0], len, nullptr, nullptr);
    return out;
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring out((size_t)len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], len);
    return out;
}

// "=" is the field delimiter (label=alias per line, one entry per line --
// same convention as config.ini and the other *_state.ini files), so only
// a literal newline/backslash within a field needs escaping to keep the
// format strictly line-based; "=" itself is never escaped -- a label that
// happens to contain one is split on the *first* "=" only, same as
// config.cpp's own key=value parsing. Labels can be non-ASCII (folder/repo
// names), so this operates on the UTF-8 bytes, not a per-wchar_t basis.
std::string EscapeField(const std::wstring& w) {
    std::string in = WideToUtf8(w);
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else out += c;
    }
    return out;
}

std::wstring UnescapeField(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char n = s[i + 1];
            if (n == 'n') { out += '\n'; i++; continue; }
            if (n == 'r') { out += '\r'; i++; continue; }
            if (n == '\\') { out += '\\'; i++; continue; }
        }
        out += s[i];
    }
    return Utf8ToWide(out);
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

        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::wstring label = UnescapeField(line.substr(0, eq));
        std::wstring alias = UnescapeField(line.substr(eq + 1));
        if (!label.empty() && !alias.empty()) result[label] = alias;
    }
    return result;
}

void SaveAll(const std::map<std::wstring, std::wstring>& all) {
    std::string out;
    for (const auto& entry : all) {
        out += EscapeField(entry.first) + "=" + EscapeField(entry.second) + "\n";
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
