#include "profile_config.h"
#include "file_util.h"
#include "logger.h"
#include "text_util.h"

#include <algorithm>
#include <cstdlib>
#include <cwctype>
#include <map>
#include <sstream>

namespace {
std::string Trim(std::string s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}
std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)tolower(c); });
    return s;
}
using Fields = std::map<std::string, std::string>;
Fields Read(const std::string& content) {
    Fields fields;
    std::istringstream in(content.rfind("\xEF\xBB\xBF", 0) == 0 ? content.substr(3) : content);
    std::string line;
    while (std::getline(in, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq != std::string::npos) fields[Lower(Trim(line.substr(0, eq)))] = Trim(line.substr(eq + 1));
    }
    return fields;
}
bool HasPrefix(const Fields& fields, const std::string& prefix) {
    auto it = fields.lower_bound(prefix);
    return it != fields.end() && it->first.rfind(prefix, 0) == 0;
}
bool Enabled(const std::string& value) { return value == "1" || Lower(value) == "true"; }
bool DefaultEnabled(const std::string& value) { return value != "0" && Lower(value) != "false"; }
bool Same(const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }
}

const wchar_t* ProfileTypeName(ProfileType type) {
    return type == ProfileType::Claude ? L"Claude" : type == ProfileType::Codex ? L"Codex" : L"None";
}

std::wstring NormalizeProfilePath(const std::wstring& path) {
    if (path.empty()) return L"";
    DWORD count = ExpandEnvironmentStringsW(path.c_str(), nullptr, 0);
    if (!count) return L"";
    std::wstring expanded(count, L'\0');
    if (!ExpandEnvironmentStringsW(path.c_str(), &expanded[0], count)) return L"";
    expanded.resize(count - 1);
    std::replace(expanded.begin(), expanded.end(), L'/', L'\\');
    bool absolute = (expanded.size() >= 3 && expanded[1] == L':' && expanded[2] == L'\\') ||
                    (expanded.size() > 2 && expanded[0] == L'\\' && expanded[1] == L'\\');
    if (!absolute || expanded.find_first_of(L"\r\n|<>\"*?") != std::wstring::npos ||
        expanded.find(L'%') != std::wstring::npos) return L"";
    DWORD needed = GetFullPathNameW(expanded.c_str(), 0, nullptr, nullptr);
    if (!needed) return L"";
    std::wstring full(needed, L'\0');
    DWORD written = GetFullPathNameW(expanded.c_str(), needed, &full[0], nullptr);
    if (!written || written >= needed) return L"";
    full.resize(written);
    while (full.size() > 3 && full.back() == L'\\') full.pop_back();
    return full;
}

std::wstring DefaultProviderPath(ProfileType type) {
    if (type == ProfileType::None) return L"";
    return NormalizeProfilePath(type == ProfileType::Claude ? L"%USERPROFILE%\\.claude" : L"%USERPROFILE%\\.codex");
}

std::wstring ProfileId(const Profile& p) {
    if (p.type == ProfileType::None || p.configDir.empty()) return L"";
    std::wstring id = (p.type == ProfileType::Claude ? L"claude:" : L"codex:") + p.configDir;
    std::transform(id.begin(), id.end(), id.begin(), ::towlower);
    return id;
}

void ReadProfileConfig(const std::string& content, Config& cfg) {
    Fields fields = Read(content);
    for (const auto& entry : fields) {
        if (entry.first.rfind("profile_", 0) != 0) continue;
        const char* start = entry.first.c_str() + 8;
        char* end = nullptr;
        long slot = strtol(start, &end, 10);
        std::string field = end && *end == '_' ? end + 1 : "";
        if (end == start || slot < 1 || slot > kMaxProfiles ||
            (field != "type" && field != "name" && field != "profile_path" && field != "icon" &&
             field != "color" && field != "ring_badge" && field != "usage_enabled" &&
             field != "vscode_profile"))
            LogWarn(L"config: unknown profile key [%hs]", entry.first.c_str());
    }
    for (int i = 1; i <= kMaxProfiles; ++i) {
        std::string prefix = "profile_" + std::to_string(i) + "_";
        std::string type = Lower(fields[prefix + "type"]);
        std::wstring path = Utf8ToWide(fields[prefix + "profile_path"]);
        if (type.empty() || type == "none") continue;
        if (type != "claude" && type != "codex") {
            LogWarn(L"config: profile_%d_type is unknown; no assistant is configured for this slot", i);
            continue;
        }
        if (path.empty()) continue;
        Profile p;
        p.type = type == "claude" ? ProfileType::Claude : ProfileType::Codex;
        p.path = path;
        p.configDir = NormalizeProfilePath(path);
        if (p.configDir.empty()) {
            LogWarn(L"config: profile_%d_profile_path is invalid; no assistant is configured for this slot", i);
            continue;
        }
        bool duplicate = false;
        for (const auto& q : cfg.profiles) duplicate = duplicate || ProfileId(q) == ProfileId(p);
        if (duplicate) { LogWarn(L"config: profile_%d duplicates an earlier account; ignoring it", i); continue; }
        p.slot = i;
        p.name = Utf8ToWide(fields[prefix + "name"]);
        p.icon = Utf8ToWide(fields[prefix + "icon"]);
        p.vscodeProfile = Utf8ToWide(fields[prefix + "vscode_profile"]);
        p.ringBadge = DefaultEnabled(fields[prefix + "ring_badge"]);
        p.usageEnabled = DefaultEnabled(fields[prefix + "usage_enabled"]);
        p.isDefault = Same(p.configDir, DefaultProviderPath(p.type));
        std::string color = fields[prefix + "color"];
        if (!color.empty()) {
            p.hasColor = color.size() == 6 && color.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos;
            if (p.hasColor) {
                unsigned long n = strtoul(color.c_str(), nullptr, 16);
                p.color = RGB((n >> 16) & 255, (n >> 8) & 255, n & 255);
            } else LogWarn(L"config: profile_%d_color isn't RRGGBB; ignoring it", i);
        }
        Log(L"config: profile %d: type=%ls folder=[%ls] usage=%d vscode_profile=[%ls]", i, ProfileTypeName(p.type),
            p.configDir.c_str(), p.usageEnabled, p.vscodeProfile.c_str());
        cfg.profiles.push_back(p);
    }
}

bool MigrateLegacyProfiles(const std::string& content, std::string& converted) {
    Fields fields = Read(content);
    converted = content;
    if (HasPrefix(fields, "profile_") || (!HasPrefix(fields, "claude_profile_") && !HasPrefix(fields, "codex_profile_") &&
                                        !fields.count("codex_usage_meter_enabled")))
        return false;
    std::string nl = content.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    bool bom = content.rfind("\xEF\xBB\xBF", 0) == 0;
    converted = bom ? "\xEF\xBB\xBF" : "";
    std::istringstream in(bom ? content.substr(3) : content);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string key = Lower(Trim(line.substr(0, line.find('='))));
        if (key.rfind("claude_profile_", 0) == 0 || key.rfind("codex_profile_", 0) == 0 || key == "codex_usage_meter_enabled") continue;
        converted += line + nl;
    }
    converted += nl + "; Unified profiles: empty type or path means None. Paths below preserve the old accounts." + nl;
    int next = 1;
    for (const char* provider : {"claude", "codex"}) {
        bool claude = std::string(provider) == "claude";
        bool usage = claude || Enabled(fields["codex_usage_meter_enabled"]);
        for (int old = 1; old <= 5; ++old) {
            std::string prefix = std::string(provider) + "_profile_" + std::to_string(old) + "_";
            // The old loader always supplied Claude slot 1, including a
            // Codex-only file. Preserve that default only during migration.
            if (!HasPrefix(fields, prefix) && !(old == 1 && (claude || usage))) continue;
            std::string path = fields[prefix + "profile_path"];
            if (path.empty()) {
                if (old != 1 || (!claude && !usage)) continue;
                // An explicit path is written ONLY during legacy migration.
                if (claude) path = "%USERPROFILE%\\.claude";
                else {
                    wchar_t inherited[32768] = {};
                    GetEnvironmentVariableW(L"CODEX_HOME", inherited, 32768);
                    path = inherited[0] ? WideToUtf8(inherited) : "%USERPROFILE%\\.codex";
                }
            }
            std::string dest = "profile_" + std::to_string(next++) + "_";
            converted += nl + dest + "type=" + (claude ? "Claude" : "Codex") + nl;
            for (const char* field : {"name", "profile_path", "icon", "color", "ring_badge"})
                converted += dest + field + "=" + (std::string(field) == "profile_path" ? path : fields[prefix + field]) + nl;
            converted += dest + "usage_enabled=" + (usage ? "true" : "false") + nl;
        }
    }
    return true;
}

std::string MigrateProfileConfigFile(const std::wstring& path, const std::string& content) {
    std::string converted;
    if (!MigrateLegacyProfiles(content, converted)) return content;
    std::wstring backup = path + L".pre-unified-profiles." + std::to_wstring(GetTickCount64()) + L".bak";
    std::wstring temporary = path + L".unified-profiles.tmp";
    if (!CopyFileW(path.c_str(), backup.c_str(), TRUE)) {
        LogWarn(L"config: couldn't back up legacy profiles; using their migration in memory only");
        return converted;
    }
    if (!WriteFileBytes(temporary, converted) || !MoveFileExW(temporary.c_str(), path.c_str(),
                                                            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        LogWarn(L"config: couldn't save migrated profiles; original file and backup retained");
    } else Log(L"config: migrated unified profiles; original saved at %ls", backup.c_str());
    return converted;
}
