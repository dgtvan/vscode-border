#pragma once

#include <windows.h>

#include <string>
#include <vector>

// Locates VS Code's `code.cmd`/`code-insiders.cmd` CLI shim. Passing -n to
// Code.exe directly does NOT work: the raw exe rejects it ("bad option: -n")
// and exits immediately -- ShellExecuteW still reports success (the process
// did launch), which is what made that silently no-op rather than fail
// loudly. The CLI shim is what actually supports -n: it sets
// ELECTRON_RUN_AS_NODE=1 and re-invokes Code.exe against its internal
// cli.js (itself under a version-hash-named subfolder the shim resolves
// relatively) to forward the request to the already-running instance over
// IPC, or to start a fresh one. Reusing the shim sidesteps reimplementing
// (and keeping in sync) that internal path resolution here.
//
// Tried in order (see vscode_installs.h):
//   1. `runningWindow`'s own install (bin\*.cmd next to its exe), so the
//      build the user is actually running (Stable vs. Insiders) wins.
//   2. The remembered install -- the one VS Code windows last ran, kept
//      across restarts -- for once every VS Code window is closed, which is
//      exactly when a "new window" action is the only way back in.
//   3. FindInstalledVSCodeCli.
// `runningWindow` may be null. Logs a warning when nothing is found.
bool ResolveVSCodeCliShim(HWND runningWindow, std::wstring& outCmdPath);

// The CLI shim on PATH, else in the well-known per-user and machine-wide
// install locations (Stable before Insiders at each step). Doesn't log.
bool FindInstalledVSCodeCli(std::wstring& outCmdPath);

// Runs the shim with `args` (e.g. `-n "D:\path"`), hidden -- the shim's own
// cmd.exe console would otherwise flash on screen, though it only forwards
// the request and exits almost immediately -- as Claude profile `profile`
// (see claude_profile.h): VS Code gives the window it opens the CLI's
// environment, CLAUDE_CONFIG_DIR included. Logs on failure.
bool RunVSCodeCli(const std::wstring& cmdPath, const std::wstring& args, const std::wstring& profile);

// Opens a brand-new VS Code window -- empty if `folder` is empty, else with
// `folder` open (note `code -n` on a folder VS Code already has open just
// re-activates that window). The window runs Claude Code as profile
// `profile` (see claude_profile.h; empty: the default). `reason` tags the
// focus-trace launch record. Returns whether the launch was handed off
// (false, logged, if no CLI shim was found or it couldn't be run).
bool OpenNewVSCodeWindow(HWND runningWindow, const std::wstring& folder, const wchar_t* reason,
                         const std::wstring& profile = L"");

// Asks one VS Code window to close -- the same request as its own X button,
// so VS Code still gets to prompt about unsaved changes.
void CloseVSCodeWindow(HWND target);

// Restores `target` if it's minimized or maximized, then sizes it to 75% of
// the width and height of the work area of the monitor it's mostly on and
// centers it there -- for a window that has ended up bigger than, or partly
// off, its screen. Logs where it lands.
void FitVSCodeWindowToScreen(HWND target);

// A VS Code window to close and open again (see CloseAndReopenVSCodeWindows).
struct ReopenWindow {
    HWND target = nullptr;
    std::wstring folder;  // what to open again: its folder, or empty for an empty window
    bool reopen = true;   // false: closed but not opened again (its folder isn't known)
    std::wstring profile; // the Claude profile to open it as (see claude_profile.h)
    std::wstring label;   // for log lines
};

// Asks each window in `windows` to close (see CloseVSCodeWindow) and, once
// it's gone, opens its folder again in a new window as its `profile`. Only
// once it's gone: VS Code keeps one window per folder, so opening it while
// the old window is still there would just bring that one forward. With
// `waitForVSCodeExit` (restarting every window) nothing is reopened until
// all of them are gone and VS Code itself has exited, so it starts afresh.
// Closing can be held up, or cancelled, at VS Code's prompt about unsaved
// changes: a window still open after a minute is left alone and not
// reopened. Returns at once; the waiting runs on a timer on this thread.
// `reason` tags the log lines and the focus-trace launch records.
void CloseAndReopenVSCodeWindows(const std::vector<ReopenWindow>& windows, bool waitForVSCodeExit,
                                 const wchar_t* reason);

// Asks every window in `targets` to close, after an OK/Cancel confirmation
// owned by `owner`. WM_CLOSE is the same request as the window's own X
// button, so VS Code still gets to prompt about unsaved changes per window.
// `source` tags the log lines (e.g. L"hud", L"tray"). No-op if `targets`
// has no live window.
void ConfirmAndCloseVSCodeWindows(HWND owner, const std::vector<HWND>& targets, const wchar_t* source);
