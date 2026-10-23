#pragma once

#include "vscode_cli.h"

#include <windows.h>

#include <cstddef>
#include <string>
#include <vector>

// Must be called once, before any other function here. ownerWnd is the app's
// message-only window, used to start/stop the fast foreground-poll timer
// (see kForegroundPollTimerId below).
void TrackingInit(HINSTANCE hInstance, HWND ownerWnd);

// Finds new VS Code windows to track and drops stale ones, then re-syncs
// every tracked overlay's position/size/label.
void RescanAllWindows();

// Timer id for the fast foreground-poll fallback: cheap (single in-process
// GetForegroundWindow call) backstop for EVENT_SYSTEM_FOREGROUND, which is
// WINEVENT_OUTOFCONTEXT and documented by Microsoft as able to lag or
// silently drop under load. tracking.cpp starts/stops this timer itself
// (only while >=1 window is tracked, so a desktop with no VS Code window
// open pays nothing extra) via TrackWindow/UntrackWindow, but the resulting
// WM_TIMER message is delivered to ownerWnd, so vscode_border.cpp's WndProc
// needs this id to route it to PollForegroundChange.
extern const UINT_PTR kForegroundPollTimerId;

// Also keeps each border's label chip clickable exactly while the cursor is
// over it (see overlay.h's UpdateOverlayLabelHover).
void PollForegroundChange();

// Re-syncs the project list HUD alone (e.g. new Claude usage numbers for
// its usage rings -- see claude_usage.h).
void RefreshProjectListHud();

// Forces every tracked overlay to repaint on its next sync (e.g. after a
// config reload changes colors/thickness/label settings).
void ForceRepaintAllTracked();

// Session-only "Hide Borders and Hub" tray toggle: hides every border
// overlay and the project-list HUD at once (releasing the HUD's reserved
// band when it's docked), without touching config.ini -- not persisted, so
// every launch starts with everything shown. Turning one of them off for
// good is config.ini's job (show_project_list etc.).
bool AreOverlaysHidden();
void SetOverlaysHidden(bool hidden);

// Re-derives every tracked window's label from its current title (e.g.
// after RefreshWorktreeCache() picks up newly-created worktrees).
void RefreshAllLabels();

// Destroys all overlays and releases all WinEvent hooks. Call on quit.
void CleanupAllTracked();

size_t TrackedWindowCount();

// Every currently tracked VS Code top-level window, in no particular order
// (e.g. for the tray icon's Close All Windows).
std::vector<HWND> GetTrackedWindows();

// Every tracked window as a ReopenWindow (see vscode_cli.h) -- its folder
// and the Claude profile it runs, as its hub item shows them -- for the
// tray icon's Restart All Windows. A window whose folder path isn't known
// (see worktree_resolver.h's ResolveFolderPath) has `reopen` false; one
// with no folder open reopens empty.
std::vector<ReopenWindow> GetReopenableWindows();

// Per-window state as this app last computed it, for the tray menu's
// Capture Diagnostic Snapshot (see diagnostic_snapshot.h) -- the snapshot
// compares this against what's actually on screen, so it needs the app's
// own view (which color, which overlay, what size it last painted), not
// just the HWND list GetTrackedWindows gives.
struct TrackedWindowDiag {
    HWND target = nullptr;
    HWND overlay = nullptr;
    DWORD pid = 0;
    int colorIndex = 0;
    COLORREF color = RGB(0, 0, 0);
    int lastPaintedWidth = -1;
    int lastPaintedHeight = -1;
    RECT lastKnownRect = {};
    bool loading = false;
    std::wstring label;
    std::wstring rawLabel;
    std::wstring borderLabel;
    std::wstring branch;
    std::wstring folderName;
    std::wstring repoName;
};
std::vector<TrackedWindowDiag> GetTrackedWindowDiagnostics();

// Registered both globally (to discover new windows) and per-process (to
// track location/destroy events for already-tracked windows).
void CALLBACK WinEventProc(HWINEVENTHOOK hook, DWORD event, HWND hwnd, LONG idObject, LONG idChild,
                           DWORD idEventThread, DWORD dwmsEventTime);
