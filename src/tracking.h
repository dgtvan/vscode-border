#pragma once

#include <windows.h>

#include <cstddef>

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

void PollForegroundChange();

// Forces every tracked overlay to repaint on its next sync (e.g. after a
// config reload changes colors/thickness/label settings).
void ForceRepaintAllTracked();

// Re-derives every tracked window's label from its current title (e.g.
// after RefreshWorktreeCache() picks up newly-created worktrees).
void RefreshAllLabels();

// Destroys all overlays and releases all WinEvent hooks. Call on quit.
void CleanupAllTracked();

size_t TrackedWindowCount();

// Registered both globally (to discover new windows) and per-process (to
// track location/destroy events for already-tracked windows).
void CALLBACK WinEventProc(HWINEVENTHOOK hook, DWORD event, HWND hwnd, LONG idObject, LONG idChild,
                           DWORD idEventThread, DWORD dwmsEventTime);
