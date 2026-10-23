#include "vscode_installs.h"

#include "file_util.h"
#include "logger.h"
#include "text_util.h"

namespace {

// Loaded on first use.
std::wstring g_install;
bool g_loaded = false;

std::wstring StorePath() {
    return GetExeDir() + L"\\vscode_installs.ini";
}

// The file's lines are `account=path`, escaped (text_util.h): the Windows
// account the install was seen running as, as the system spells it.
std::wstring AccountName() {
    wchar_t name[256] = {};
    DWORD len = 256;
    return GetUserNameW(name, &len) ? name : L"";
}

void Load() {
    if (g_loaded) return;
    g_loaded = true;
    std::wstring own = AccountName();
    std::string content = ReadFileBytes(StorePath());
    size_t pos = 0;
    while (pos < content.size()) {
        size_t eol = content.find('\n', pos);
        std::string line = content.substr(pos, (eol == std::string::npos ? content.size() : eol) - pos);
        pos = eol == std::string::npos ? content.size() : eol + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t eq = line.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        std::wstring account = Utf8ToWide(line.substr(0, eq));
        if (CompareStringOrdinal(account.c_str(), -1, own.c_str(), -1, TRUE) != CSTR_EQUAL) continue;
        g_install = UnescapeTextLine(line.substr(eq + 1));
        return;
    }
}

bool IsVSCodeExe(const std::wstring& exe) {
    size_t slash = exe.find_last_of(L"\\/");
    std::wstring name = slash == std::wstring::npos ? exe : exe.substr(slash + 1);
    return _wcsicmp(name.c_str(), L"Code.exe") == 0 || _wcsicmp(name.c_str(), L"Code - Insiders.exe") == 0;
}

} // namespace

void NoteVSCodeInstall(DWORD pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return;
    wchar_t path[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    bool ok = QueryFullProcessImageNameW(process, 0, path, &size) != 0;
    CloseHandle(process);
    if (!ok || !IsVSCodeExe(path)) return;

    Load();
    if (_wcsicmp(g_install.c_str(), path) == 0) return;
    g_install = path;
    WriteFileBytes(StorePath(), WideToUtf8(AccountName()) + "=" + EscapeTextLine(g_install) + "\n");
    Log(L"vscode_installs: VS Code is %ls", path);
}

std::wstring RememberedVSCodeInstall() {
    Load();
    return g_install;
}
