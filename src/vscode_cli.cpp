#include "vscode_cli.h"
#include "wsl.h"

#include "config.h"
#include "file_util.h"
#include "focus_trace.h"
#include "logger.h"

#include <shellapi.h>

#include <cwchar>

namespace {

std::wstring g_lastWindowDerivedShim; // see ResolveVSCodeCliShim's step 2

std::wstring GetEnvVar(const wchar_t* name) {
    wchar_t buf[MAX_PATH] = {};
    DWORD len = GetEnvironmentVariableW(name, buf, MAX_PATH);
    return (len > 0 && len < MAX_PATH) ? std::wstring(buf) : std::wstring();
}

bool FileExists(const std::wstring& path) {
    DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

// The CLI shim (bin\*.cmd) of the install whose Code.exe is `exePath`.
bool ResolveFromExe(const std::wstring& exePath, std::wstring& outCmdPath) {
    std::wstring installDir(exePath);
    size_t slash = installDir.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return false;
    installDir.resize(slash);

    std::wstring searchPattern = installDir + L"\\bin\\*.cmd";
    WIN32_FIND_DATAW findData = {};
    HANDLE hFind = FindFirstFileW(searchPattern.c_str(), &findData);
    if (hFind == INVALID_HANDLE_VALUE) {
        Log(L"vscode CLI shim: none found (bin\\*.cmd) under %ls", installDir.c_str());
        return false;
    }
    outCmdPath = installDir + L"\\bin\\" + findData.cFileName;
    FindClose(hFind);
    return true;
}

bool ResolveFromWindow(HWND window, std::wstring& outCmdPath) {
    if (!window || !IsWindow(window)) return false;

    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (!pid) return false;
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProcess) return false;
    wchar_t exePath[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    bool ok = QueryFullProcessImageNameW(hProcess, 0, exePath, &size) != 0;
    CloseHandle(hProcess);
    return ok && ResolveFromExe(exePath, outCmdPath);
}

// This account's user_profile_1_vscode (relative to config.ini's folder),
// or empty.
std::wstring ConfiguredOwnVSCodeExe() {
    for (const UserProfile& p : g_config.userProfiles) {
        if (!p.thisAccount || p.vscode.empty()) continue;
        bool absolute = p.vscode.size() > 1 && (p.vscode[1] == L':' || (p.vscode[0] == L'\\' && p.vscode[1] == L'\\'));
        return absolute ? p.vscode : GetExeDir() + L"\\" + p.vscode;
    }
    return L"";
}

bool ResolveStandalone(std::wstring& outCmdPath) {
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
        if (FileExists(candidate)) {
            outCmdPath = candidate;
            return true;
        }
    }
    return false;
}

} // namespace

bool ResolveVSCodeCliShim(HWND runningWindow, std::wstring& outCmdPath) {
    std::wstring configured = ConfiguredOwnVSCodeExe();
    if (!configured.empty()) {
        if (ResolveFromExe(configured, outCmdPath)) return true;
        LogWarn(L"vscode CLI shim: none next to user_profile_1_vscode [%ls] -- falling back to the usual lookup",
                configured.c_str());
    }
    if (ResolveFromWindow(runningWindow, outCmdPath)) {
        g_lastWindowDerivedShim = outCmdPath;
        return true;
    }
    if (!g_lastWindowDerivedShim.empty() && FileExists(g_lastWindowDerivedShim)) {
        outCmdPath = g_lastWindowDerivedShim;
        return true;
    }
    if (ResolveStandalone(outCmdPath)) return true;
    LogWarn(L"vscode CLI shim: could not locate code.cmd/code-insiders.cmd (no running window to derive it "
            L"from, and none on PATH or in the usual install locations)");
    return false;
}

bool RunVSCodeCli(const std::wstring& cmdPath, const std::wstring& args) {
    HINSTANCE result = ShellExecuteW(nullptr, L"open", cmdPath.c_str(), args.c_str(), nullptr, SW_HIDE);
    if ((INT_PTR)result <= 32) {
        LogWarn(L"ShellExecuteW(%ls %ls) failed, code=%Id", cmdPath.c_str(), args.c_str(), (INT_PTR)result);
        return false;
    }
    return true;
}

void OpenNewVSCodeWindow(HWND runningWindow, const std::wstring& folder, const wchar_t* reason) {
    // A WSL folder (\\wsl.localhost\<distro>\... -- see wsl.h) opens in that
    // distro's own Linux VS Code.
    std::wstring distro, linuxPath;
    if (ParseWslUncPath(folder, &distro, &linuxPath)) {
        NoteWindowLaunchRequest(reason, 1);
        LaunchInWsl(distro, LinuxVSCodeOpenCommand(distro, linuxPath));
        return;
    }
    std::wstring cmdPath;
    if (!ResolveVSCodeCliShim(runningWindow, cmdPath)) return;
    std::wstring args = folder.empty() ? L"-n" : L"-n \"" + folder + L"\"";
    NoteWindowLaunchRequest(reason, 1);
    RunVSCodeCli(cmdPath, args);
}

void CloseVSCodeWindow(HWND target, const wchar_t* reason) {
    if (!target || !IsWindow(target)) return;
    wchar_t cls[64] = {};
    GetClassNameW(target, cls, 64);
    if (wcscmp(cls, kWslgWindowClass) != 0) {
        PostMessageW(target, WM_CLOSE, 0, 0);
        return;
    }
    RequestForeground(target, FocusTargetKind::ExternalWindow, reason);
    SendMessageTimeoutW(target, WM_CLOSE, 0, 0, SMTO_ABORTIFHUNG, 2000, nullptr);
}

void ConfirmAndCloseVSCodeWindows(HWND owner, const std::vector<HWND>& targets, const wchar_t* source) {
    std::vector<HWND> live;
    for (HWND target : targets) {
        if (target && IsWindow(target)) live.push_back(target);
    }
    if (live.empty()) return;

    wchar_t prompt[128];
    swprintf(prompt, 128, L"Close all %d VS Code window(s)?", (int)live.size());
    int choice = MessageBoxW(owner, prompt, L"VS Code Border",
                             MB_OKCANCEL | MB_ICONQUESTION | MB_DEFBUTTON2 | MB_TOPMOST | MB_SETFOREGROUND);
    if (choice != IDOK) {
        Log(L"%ls close-all: cancelled", source);
        return;
    }
    for (HWND target : live) {
        CloseVSCodeWindow(target, L"close-all");
    }
    Log(L"%ls close-all: sent WM_CLOSE to %d window(s)", source, (int)live.size());
}
