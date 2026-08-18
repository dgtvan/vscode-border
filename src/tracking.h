#pragma once

#include <windows.h>

#include <cstddef>

// Must be called once, before any other function here, with the app's
// HINSTANCE (needed to create overlay windows).
void TrackingInit(HINSTANCE hInstance);

// Finds new VS Code windows to track and drops stale ones, then re-syncs
// every tracked overlay's position/size/label.
void RescanAllWindows();

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
