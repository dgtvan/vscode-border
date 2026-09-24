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
// Tried in order:
//   1. `runningWindow`'s own install (bin\*.cmd next to its exe), so the
//      build the user is actually running (Stable vs. Insiders) wins.
//   2. The last shim step 1 found -- keeps that same build once every VS
//      Code window is closed, which is exactly when a "new window" action
//      is the only way back in.
//   3. PATH, then the well-known per-user and machine-wide install
//      locations (Stable before Insiders at each step) -- e.g. at startup,
//      before any VS Code window is running.
// `runningWindow` may be null. Logs a warning when nothing is found.
bool ResolveVSCodeCliShim(HWND runningWindow, std::wstring& outCmdPath);

// Runs the shim with `args` (e.g. `-n "D:\path"`), hidden -- the shim's own
// cmd.exe console would otherwise flash on screen, though it only forwards
// the request and exits almost immediately. Logs on failure.
bool RunVSCodeCli(const std::wstring& cmdPath, const std::wstring& args);

// Opens a brand-new VS Code window -- empty if `folder` is empty, else with
// `folder` open (note `code -n` on a folder VS Code already has open just
// re-activates that window). `reason` tags the focus-trace launch record.
void OpenNewVSCodeWindow(HWND runningWindow, const std::wstring& folder, const wchar_t* reason);

// Asks every window in `targets` to close, after an OK/Cancel confirmation
// owned by `owner`. WM_CLOSE is the same request as the window's own X
// button, so VS Code still gets to prompt about unsaved changes per window.
// `source` tags the log lines (e.g. L"hud", L"tray"). No-op if `targets`
// has no live window.
void ConfirmAndCloseVSCodeWindows(HWND owner, const std::vector<HWND>& targets, const wchar_t* source);
