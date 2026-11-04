#include "vscode_profiles.h"

#include "file_util.h"
#include "json_scan.h"
#include "logger.h"
#include "text_util.h"

#include <windows.h>

#include <algorithm>
#include <map>

namespace {

std::wstring TrimTrailingSeparators(std::wstring path) {
    while (path.size() > 3 && (path.back() == L'\\' || path.back() == L'/')) path.pop_back();
    return path;
}

bool SamePathText(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), (int)a.size(), b.c_str(), (int)b.size(), TRUE) == CSTR_EQUAL;
}

// A JSON string value's text (quotes included in `span`), \" and \\ undone.
std::wstring StringValue(const std::string& content, const JsonScan::KeyEntry& entry) {
    if (!entry.found || entry.valueStart >= content.size() || content[entry.valueStart] != '"') return L"";
    std::string raw = content.substr(entry.valueStart, entry.valueEnd - entry.valueStart);
    std::string out;
    for (size_t i = 1; i + 1 < raw.size(); i++) {
        if (raw[i] == '\\' && i + 2 < raw.size()) i++;
        out.push_back(raw[i]);
    }
    return Utf8ToWide(out);
}

// `userDataProfiles`: location id -> name. The default profile isn't listed
// (associations name it `__default__profile__`), which is what makes it
// indistinguishable from "no profile" here -- as it should be, since a
// folder on the default profile is not pinned to any assistant.
std::map<std::wstring, std::wstring> ProfileNamesById(const std::string& content) {
    std::map<std::wstring, std::wstring> names;
    if (JsonScan::FindRootObjectOpenBrace(content) == std::string::npos) return names;
    std::string copy = content;
    JsonScan::Span root = JsonScan::EnsureRootObject(copy);
    JsonScan::KeyEntry profiles = JsonScan::FindKeyInObject(copy, root.start, root.end, "userDataProfiles");
    if (!profiles.found || profiles.valueStart >= copy.size() || copy[profiles.valueStart] != '[') return names;
    for (const JsonScan::Span& e :
         JsonScan::FindArrayElements(copy, profiles.valueStart + 1, profiles.valueEnd - 1)) {
        if (e.start >= copy.size() || copy[e.start] != '{') continue;
        std::wstring location = StringValue(copy, JsonScan::FindKeyInObject(copy, e.start + 1, e.end - 1, "location"));
        std::wstring name = StringValue(copy, JsonScan::FindKeyInObject(copy, e.start + 1, e.end - 1, "name"));
        if (!location.empty() && !name.empty()) names[location] = name;
    }
    return names;
}

// The folder URIs in `profileAssociations.workspaces`, as native paths, each
// with the location id it is associated with.
std::vector<std::pair<std::wstring, std::wstring>> Associations(const std::string& content) {
    std::vector<std::pair<std::wstring, std::wstring>> out;
    if (JsonScan::FindRootObjectOpenBrace(content) == std::string::npos) return out;
    std::string copy = content;
    JsonScan::Span root = JsonScan::EnsureRootObject(copy);
    JsonScan::KeyEntry assoc = JsonScan::FindKeyInObject(copy, root.start, root.end, "profileAssociations");
    if (!assoc.found || assoc.valueStart >= copy.size() || copy[assoc.valueStart] != '{') return out;
    JsonScan::KeyEntry workspaces =
        JsonScan::FindKeyInObject(copy, assoc.valueStart + 1, assoc.valueEnd - 1, "workspaces");
    if (!workspaces.found || workspaces.valueStart >= copy.size() || copy[workspaces.valueStart] != '{') return out;
    for (const JsonScan::KeyEntry& member :
         JsonScan::FindObjectMembers(copy, workspaces.valueStart + 1, workspaces.valueEnd - 1)) {
        std::wstring folder = TrimTrailingSeparators(FileUriToPath(JsonScan::KeyName(copy, member)));
        std::wstring id = StringValue(copy, member);
        if (!folder.empty() && !id.empty()) out.push_back({folder, id});
    }
    return out;
}

// One storage.json, parsed only when it has been rewritten since last time.
struct Cached {
    ULONGLONG written = 0;
    std::map<std::wstring, std::wstring> folders; // folder path -> profile name
    std::vector<std::wstring> names;
};

ULONGLONG FileTimeValue(const FILETIME& ft) {
    return ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

std::vector<std::wstring> StoragePaths() {
    wchar_t appData[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    std::wstring base(appData);
    return {base + L"\\Code\\User\\globalStorage\\storage.json",
            base + L"\\Code - Insiders\\User\\globalStorage\\storage.json"};
}

// Every readable storage.json's parse, most recently written first -- so an
// Insiders window's folder resolves from Insiders' own state, and whichever
// install the user actually works in wins when both know the folder.
const std::vector<const Cached*> Storages() {
    static std::map<std::wstring, Cached> cache;
    std::vector<std::pair<ULONGLONG, const Cached*>> found;
    for (const std::wstring& path : StoragePaths()) {
        WIN32_FILE_ATTRIBUTE_DATA data;
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) continue;
        ULONGLONG written = FileTimeValue(data.ftLastWriteTime);
        Cached& entry = cache[path];
        if (entry.written != written) {
            entry.written = written;
            entry.folders.clear();
            entry.names.clear();
            std::string content = ReadFileBytes(path);
            std::map<std::wstring, std::wstring> names = ProfileNamesById(content);
            for (const auto& name : names) entry.names.push_back(name.second);
            for (const auto& association : Associations(content)) {
                auto name = names.find(association.second);
                if (name != names.end()) entry.folders[association.first] = name->second;
            }
            LogDiag(L"vscode_profiles: %ls -> %zu profile(s), %zu associated folder(s)", path.c_str(),
                    entry.names.size(), entry.folders.size());
        }
        found.push_back({written, &entry});
    }
    std::sort(found.begin(), found.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<const Cached*> out;
    for (const auto& entry : found) out.push_back(entry.second);
    return out;
}

} // namespace

std::wstring ParseVSCodeProfileForFolder(const std::string& storageJson, const std::wstring& folder) {
    std::map<std::wstring, std::wstring> names = ProfileNamesById(storageJson);
    std::wstring wanted = TrimTrailingSeparators(folder);
    for (const auto& association : Associations(storageJson)) {
        if (!SamePathText(association.first, wanted)) continue;
        auto name = names.find(association.second);
        return name == names.end() ? L"" : name->second;
    }
    return L"";
}

std::vector<std::wstring> ParseVSCodeProfileNames(const std::string& storageJson) {
    std::vector<std::wstring> names;
    for (const auto& entry : ProfileNamesById(storageJson)) names.push_back(entry.second);
    return names;
}

std::wstring VSCodeProfileForFolder(const std::wstring& folder) {
    if (folder.empty()) return L"";
    std::wstring wanted = TrimTrailingSeparators(folder);
    for (const Cached* storage : Storages()) {
        for (const auto& entry : storage->folders)
            if (SamePathText(entry.first, wanted)) return entry.second;
    }
    return L"";
}

bool VSCodeProfileExists(const std::wstring& name) {
    if (name.empty()) return false;
    for (const Cached* storage : Storages()) {
        for (const std::wstring& known : storage->names)
            if (_wcsicmp(known.c_str(), name.c_str()) == 0) return true;
    }
    return false;
}
