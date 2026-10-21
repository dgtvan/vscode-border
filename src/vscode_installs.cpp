#include "vscode_installs.h"

#include "account.h"
#include "file_util.h"
#include "logger.h"
#include "text_util.h"

#include <algorithm>

namespace {

struct Install {
    std::wstring account; // Windows account name, as the system spells it
    std::wstring exe;
};

// Most recently seen first. Loaded on first use.
std::vector<Install> g_installs;
bool g_loaded = false;

std::wstring StorePath() {
    return GetExeDir() + L"\\vscode_installs.ini";
}

bool SameAccountName(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), (int)a.size(), b.c_str(), (int)b.size(), TRUE) == CSTR_EQUAL;
}

// `account` with empty meaning this app's own, as stored.
std::wstring StoredName(const std::wstring& account) {
    return account.empty() ? CurrentAccountName() : account;
}

// One `account=path` line each, escaped (text_util.h); an account name can't
// contain "=".
void Load() {
    if (g_loaded) return;
    g_loaded = true;
    std::string content = ReadFileBytes(StorePath());
    size_t pos = 0;
    while (pos < content.size()) {
        size_t eol = content.find('\n', pos);
        std::string line = content.substr(pos, (eol == std::string::npos ? content.size() : eol) - pos);
        pos = eol == std::string::npos ? content.size() : eol + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t eq = line.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        g_installs.push_back({Utf8ToWide(line.substr(0, eq)), UnescapeTextLine(line.substr(eq + 1))});
    }
}

void Save() {
    std::string out;
    for (const Install& i : g_installs) out += WideToUtf8(i.account) + "=" + EscapeTextLine(i.exe) + "\n";
    WriteFileBytes(StorePath(), out);
}

bool IsVSCodeExe(const std::wstring& exe) {
    size_t slash = exe.find_last_of(L"\\/");
    std::wstring name = slash == std::wstring::npos ? exe : exe.substr(slash + 1);
    return _wcsicmp(name.c_str(), L"Code.exe") == 0 || _wcsicmp(name.c_str(), L"Code - Insiders.exe") == 0;
}

// The folder every account's profile folder sits in (C:\Users), from the
// registry's profile list.
std::wstring ProfilesDirectory() {
    wchar_t raw[MAX_PATH] = {}, expanded[MAX_PATH] = {};
    DWORD size = sizeof(raw);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\ProfileList",
                     L"ProfilesDirectory", RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, nullptr, raw,
                     &size) != ERROR_SUCCESS) {
        return L"";
    }
    ExpandEnvironmentStringsW(raw, expanded, MAX_PATH);
    return expanded;
}

} // namespace

void NoteVSCodeInstall(DWORD pid, const std::wstring& account) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return;
    wchar_t path[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    bool ok = QueryFullProcessImageNameW(process, 0, path, &size) != 0;
    CloseHandle(process);
    if (!ok || !IsVSCodeExe(path)) return;

    Load();
    std::wstring name = StoredName(account);
    auto it = std::find_if(g_installs.begin(), g_installs.end(),
                           [&](const Install& i) { return SameAccountName(i.account, name); });
    bool changed = it == g_installs.end() || _wcsicmp(it->exe.c_str(), path) != 0;
    bool first = it == g_installs.begin();
    if (!changed && first) return;
    if (it != g_installs.end()) g_installs.erase(it);
    g_installs.insert(g_installs.begin(), {name, path});
    Save();
    if (changed) Log(L"vscode_installs: [%ls]'s VS Code is %ls", name.c_str(), path);
}

std::wstring RememberedVSCodeInstall(const std::wstring& account) {
    Load();
    std::wstring name = StoredName(account);
    for (const Install& i : g_installs) {
        if (SameAccountName(i.account, name)) return i.exe;
    }
    return L"";
}

void ForgetVSCodeInstall(const std::wstring& account) {
    Load();
    std::wstring name = StoredName(account);
    size_t before = g_installs.size();
    g_installs.erase(std::remove_if(g_installs.begin(), g_installs.end(),
                                    [&](const Install& i) { return SameAccountName(i.account, name); }),
                     g_installs.end());
    if (g_installs.size() == before) return;
    Save();
    Log(L"vscode_installs: forgot [%ls]'s VS Code", name.c_str());
}

std::vector<std::wstring> SharedVSCodeInstalls(const std::wstring& account) {
    Load();
    std::wstring name = StoredName(account), profiles = ProfilesDirectory();
    std::vector<std::wstring> out;
    for (const Install& i : g_installs) {
        if (SameAccountName(i.account, name)) continue;
        // Inside a profile folder only that account can reach it.
        bool inProfile = !profiles.empty() && i.exe.size() > profiles.size() &&
                         _wcsnicmp(i.exe.c_str(), profiles.c_str(), profiles.size()) == 0 &&
                         (i.exe[profiles.size()] == L'\\' || i.exe[profiles.size()] == L'/');
        if (inProfile || GetFileAttributesW(i.exe.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        if (std::find_if(out.begin(), out.end(), [&](const std::wstring& e) {
                return _wcsicmp(e.c_str(), i.exe.c_str()) == 0;
            }) == out.end()) {
            out.push_back(i.exe);
        }
    }
    return out;
}

std::wstring VSCodeShimForExe(const std::wstring& exe) {
    size_t slash = exe.find_last_of(L"\\/");
    std::wstring dir = slash == std::wstring::npos ? L"." : exe.substr(0, slash);
    std::wstring name = slash == std::wstring::npos ? exe : exe.substr(slash + 1);
    return dir + (_wcsicmp(name.c_str(), L"Code - Insiders.exe") == 0 ? L"\\bin\\code-insiders.cmd"
                                                                       : L"\\bin\\code.cmd");
}

std::wstring VSCodeExeForShim(const std::wstring& shim) {
    size_t slash = shim.find_last_of(L"\\/");
    std::wstring bin = slash == std::wstring::npos ? L"." : shim.substr(0, slash);
    std::wstring name = slash == std::wstring::npos ? shim : shim.substr(slash + 1);
    size_t binSlash = bin.find_last_of(L"\\/");
    std::wstring dir = binSlash == std::wstring::npos ? L"." : bin.substr(0, binSlash);
    return dir + (_wcsicmp(name.c_str(), L"code-insiders.cmd") == 0 ? L"\\Code - Insiders.exe" : L"\\Code.exe");
}
