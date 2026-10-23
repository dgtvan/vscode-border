#include "vscode_cli.h"
#include "wsl.h"

#include "claude_profile.h"
#include "focus_trace.h"
#include "logger.h"
#include "vscode_installs.h"

#include <cwchar>

namespace {

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

} // namespace

bool FindInstalledVSCodeCli(std::wstring& outCmdPath) {
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

bool ResolveVSCodeCliShim(HWND runningWindow, std::wstring& outCmdPath) {
    if (ResolveFromWindow(runningWindow, outCmdPath)) return true;
    std::wstring remembered = RememberedVSCodeInstall();
    if (!remembered.empty() && ResolveFromExe(remembered, outCmdPath)) return true;
    if (FindInstalledVSCodeCli(outCmdPath)) return true;
    LogWarn(L"vscode CLI shim: could not locate code.cmd/code-insiders.cmd (no running window to derive it "
            L"from, none remembered, none on PATH or in the usual install locations)");
    return false;
}

bool RunVSCodeCli(const std::wstring& cmdPath, const std::wstring& args, const std::wstring& profile) {
    wchar_t system32[MAX_PATH] = {};
    GetSystemDirectoryW(system32, MAX_PATH);
    std::wstring cmdExe = std::wstring(system32) + L"\\cmd.exe";
    std::wstring commandLine = L"cmd.exe /d /c \"\"" + cmdPath + L"\" " + args + L"\"";
    std::wstring environment = ClaudeProfileEnvironment(profile);
    STARTUPINFOW si = {sizeof(si)};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(cmdExe.c_str(), &commandLine[0], nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, &environment[0], nullptr, &si, &pi)) {
        LogWarn(L"couldn't run the VS Code CLI (%ls %ls) as Claude profile [%ls], lastError=%lu", cmdPath.c_str(),
                args.c_str(), ClaudeProfileName(profile).c_str(), GetLastError());
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

bool OpenNewVSCodeWindow(HWND runningWindow, const std::wstring& folder, const wchar_t* reason,
                         const std::wstring& profile) {
    // A WSL folder (\\wsl.localhost\<distro>\... -- see wsl.h) opens in that
    // distro's own Linux VS Code.
    std::wstring distro, linuxPath;
    if (ParseWslUncPath(folder, &distro, &linuxPath)) {
        NoteWindowLaunchRequest(reason, 1);
        LaunchInWsl(distro, LinuxVSCodeOpenCommand(distro, linuxPath));
        return true;
    }
    std::wstring cmdPath;
    if (!ResolveVSCodeCliShim(runningWindow, cmdPath)) return false;
    std::wstring args = folder.empty() ? L"-n" : L"-n \"" + folder + L"\"";
    NoteWindowLaunchRequest(reason, 1);
    if (!RunVSCodeCli(cmdPath, args, profile)) return false;
    NoteClaudeProfileLaunch(folder, profile);
    Log(L"opened a new VS Code window%ls%ls as Claude profile [%ls] (%ls)", folder.empty() ? L"" : L" for ",
        folder.c_str(), ClaudeProfileName(profile).c_str(), reason);
    return true;
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
