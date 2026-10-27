#include "vscode_cli.h"

#include "claude_profile.h"
#include "dpi_util.h"
#include "focus_trace.h"
#include "logger.h"
#include "vscode_installs.h"

#include <algorithm>
#include <cwchar>
#include <map>

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

void CloseVSCodeWindow(HWND target) {
    if (target && IsWindow(target)) PostMessageW(target, WM_CLOSE, 0, 0);
}

void FitVSCodeWindowToScreen(HWND target) {
    if (!target || !IsWindow(target)) return;
    // This app is DPI-unaware: on a scaled monitor its own view of the work
    // area and of VS Code's (per-monitor aware) window is virtualized.
    PhysicalPixelsScope physical;
    // Picked before restoring: a maximized window's restore position may be
    // on another monitor. For a minimized one this reads its pre-minimize
    // position.
    HMONITOR mon = MonitorFromWindow(target, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {sizeof(mi)};
    if (!GetMonitorInfoW(mon, &mi)) {
        Log(L"[WARN] fit to screen: GetMonitorInfoW failed, lastError=%lu", GetLastError());
        return;
    }
    if (IsIconic(target) || IsZoomed(target)) ShowWindow(target, SW_RESTORE);

    const RECT& work = mi.rcWork;
    int workW = work.right - work.left, workH = work.bottom - work.top;
    int w = workW * 3 / 4, h = workH * 3 / 4;
    RECT want = {work.left + (workW - w) / 2, work.top + (workH - h) / 2, 0, 0};
    want.right = want.left + w;
    want.bottom = want.top + h;
    // Moving a window mostly onto a monitor with a different scale than the
    // one it was on makes VS Code rescale itself to the new DPI, changing the
    // size just set -- set it once more, now that the scale stays put.
    RECT got = {};
    for (int attempt = 0; attempt < 2; attempt++) {
        SetWindowPos(target, nullptr, want.left, want.top, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
        GetWindowRect(target, &got);
        if (EqualRect(&got, &want)) break;
    }
    Log(L"fit to screen: %ls work=(%ld,%ld)-(%ld,%ld) want=(%ld,%ld)-(%ld,%ld) got=(%ld,%ld)-(%ld,%ld)",
        DescribeMonitor(mon).c_str(), work.left, work.top, work.right, work.bottom, want.left, want.top, want.right,
        want.bottom, got.left, got.top, got.right, got.bottom);
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
        CloseVSCodeWindow(target);
    }
    Log(L"%ls close-all: sent WM_CLOSE to %d window(s)", source, (int)live.size());
}

namespace {

// One CloseAndReopenVSCodeWindows call, waited on by its own timer.
struct ReopenBatch {
    std::vector<ReopenWindow> windows;
    bool waitForVSCodeExit = false;
    std::wstring reason;
    std::vector<DWORD> vscodePids; // the Windows VS Code processes the windows belonged to
    ULONGLONG closedAt = 0;
    ULONGLONG allGoneAt = 0; // when the last window went, 0 until then
};

std::map<UINT_PTR, ReopenBatch> g_reopenBatches;

const UINT kReopenPollMs = 250;
const ULONGLONG kReopenCloseTimeoutMs = 60000; // VS Code may be waiting on an unsaved-changes prompt
const ULONGLONG kReopenExitTimeoutMs = 20000;  // for VS Code itself to exit once its windows are gone

bool IsProcessRunning(DWORD pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;
    DWORD exitCode = 0;
    bool running = GetExitCodeProcess(process, &exitCode) && exitCode == STILL_ACTIVE;
    CloseHandle(process);
    return running;
}

void CALLBACK ReopenTimerProc(HWND, UINT, UINT_PTR id, DWORD) {
    auto it = g_reopenBatches.find(id);
    if (it == g_reopenBatches.end()) {
        KillTimer(nullptr, id);
        return;
    }
    ReopenBatch& batch = it->second;
    ULONGLONG now = GetTickCount64();
    bool allGone = std::all_of(batch.windows.begin(), batch.windows.end(),
                               [](const ReopenWindow& w) { return !IsWindow(w.target); });
    if (!allGone && now - batch.closedAt < kReopenCloseTimeoutMs) return;
    if (allGone && batch.waitForVSCodeExit) {
        if (!batch.allGoneAt) batch.allGoneAt = now;
        bool running = std::any_of(batch.vscodePids.begin(), batch.vscodePids.end(), IsProcessRunning);
        if (running && now - batch.allGoneAt < kReopenExitTimeoutMs) return;
        if (running) {
            Log(L"%ls: VS Code is still running %llus after its windows closed -- reopening them in it anyway",
                batch.reason.c_str(), (unsigned long long)(kReopenExitTimeoutMs / 1000));
        }
    }
    KillTimer(nullptr, id);
    ReopenBatch done = std::move(batch);
    g_reopenBatches.erase(it);

    for (const ReopenWindow& w : done.windows) {
        if (IsWindow(w.target)) {
            Log(L"%ls: [%ls] is still open %llus after it was asked to close (close cancelled?) -- not reopening it",
                done.reason.c_str(), w.label.c_str(), (unsigned long long)(kReopenCloseTimeoutMs / 1000));
        } else if (!w.reopen) {
            Log(L"%ls: [%ls] closed; not reopening it -- its folder isn't known", done.reason.c_str(), w.label.c_str());
        } else {
            OpenNewVSCodeWindow(nullptr, w.folder, done.reason.c_str(), w.profile);
        }
    }
}

} // namespace

void CloseAndReopenVSCodeWindows(const std::vector<ReopenWindow>& windows, bool waitForVSCodeExit,
                                 const wchar_t* reason) {
    ReopenBatch batch;
    batch.waitForVSCodeExit = waitForVSCodeExit;
    batch.reason = reason;
    for (const ReopenWindow& w : windows) {
        if (!w.target || !IsWindow(w.target)) continue;
        batch.windows.push_back(w);
        DWORD pid = 0;
        GetWindowThreadProcessId(w.target, &pid);
        if (pid &&
            std::find(batch.vscodePids.begin(), batch.vscodePids.end(), pid) == batch.vscodePids.end()) {
            batch.vscodePids.push_back(pid);
        }
        Log(L"%ls: closing [%ls] to reopen %ls as Claude profile [%ls]", reason, w.label.c_str(),
            !w.reopen ? L"(not: folder unknown)" : w.folder.empty() ? L"an empty window" : w.folder.c_str(),
            ClaudeProfileName(w.profile).c_str());
    }
    if (batch.windows.empty()) return;
    for (const ReopenWindow& w : batch.windows) CloseVSCodeWindow(w.target);
    batch.closedAt = GetTickCount64();
    UINT_PTR id = SetTimer(nullptr, 0, kReopenPollMs, ReopenTimerProc);
    if (!id) {
        LogWarn(L"%ls: couldn't start the timer to reopen %zu window(s), lastError=%lu", reason, batch.windows.size(),
                GetLastError());
        return;
    }
    g_reopenBatches[id] = std::move(batch);
}
