#pragma once

#include <windows.h>

#include <cstddef>

// Always-on tracing for everything in this app that can move the input
// focus, plus detection of the specific misbehaviour it exists to catch:
// *several VS Code windows lighting up at once* (their taskbar buttons /
// thumbnails all showing the highlighted "wants attention" state).
//
// Why that symptom needs tracing at all, rather than just a fix: there is
// no Win32 API to ask "is this window currently flashing?", so the flashing
// state itself can't be observed after the fact. What *can* be recorded is
// every condition that produces it, of which this app has exactly three:
//
//  1. A denied SetForegroundWindow. Documented behaviour: when the call
//     can't actually bring the window forward (the caller doesn't hold the
//     foreground-activation right -- another process is foreground, a menu
//     is up, the foreground lock timeout hasn't expired), the system
//     "flashes the window's taskbar button" instead of failing silently.
//     One denied call highlights one window; a burst of them across
//     several windows highlights all of them, which is exactly the reported
//     symptom.
//  2. A burst of activation requests in general -- e.g. sweeping the mouse
//     across the project-list HUD with project_list_activate_on_hover=true
//     asks to activate every item the cursor crosses, one after another.
//  3. Launching several VS Code windows at once (favourites auto-open at
//     startup). Those windows request the foreground *themselves*; the last
//     one wins and the rest flash. Nothing in this app calls
//     SetForegroundWindow on that path, so without the launch being logged
//     it looks like an unexplained storm.
//
// Every entry point below writes unconditionally (not gated on
// verbose_logging) -- the whole point is that a user who has just seen the
// symptom can hand over the log without having had to predict it. Volume
// is low: these are user-initiated events, not per-frame ones.

// What kind of window an activation request is aimed at. Only the outcome
// for ExternalWindow is warning-worthy: this app's own windows are all
// WS_EX_TOOLWINDOW (no taskbar button to flash) and the project-list HUD is
// additionally WS_EX_NOACTIVATE, so a denial there is both routine and
// invisible -- warning on it would light the tray badge constantly and bury
// the denials that actually matter.
enum class FocusTargetKind {
    ExternalWindow, // a VS Code window, or whatever previously held the foreground
    OwnUi,          // this app's own HUD / alias edit box / tray owner window
};

// Single choke point for SetForegroundWindow. Logs the target, the reason,
// the foreground before and after, and whether the call was granted or
// denied; feeds the burst detector; returns SetForegroundWindow's own
// result. `reason` is a short stable tag (e.g. L"hud-hover") -- it's what
// makes a log line attributable to one call site.
bool RequestForeground(HWND target, FocusTargetKind kind, const wchar_t* reason);

// Records an observed foreground change (from the EVENT_SYSTEM_FOREGROUND
// hook -- desktop-wide, so this sees switches this app didn't cause too).
// Attributes each one to this app or to something else by proximity to the
// last RequestForeground, and flags rapid churn between VS Code windows.
void NoteForegroundChange(HWND hwnd, bool isTrackedVSCodeWindow);

// Records that this app asked the OS to open `count` new VS Code window(s)
// -- see reason 3 in the header comment above.
void NoteWindowLaunchRequest(const wchar_t* reason, size_t count);

// Installed once by tracking.cpp. Called when a storm is detected, so the
// warning can be followed by the full tracked-window inventory (which
// windows exist, which is foreground) without focus_trace.cpp having to
// depend on tracking.cpp.
typedef void (*FocusStormSnapshotFn)(const wchar_t* reason);
void SetFocusStormSnapshotHook(FocusStormSnapshotFn fn);

// Formats "hwnd=... class=[...] title=[...] pid=... iconic=... visible=..."
// into a caller-supplied buffer. Exposed so tracking.cpp's snapshot dump
// describes windows identically to the lines above it.
void DescribeWindowForLog(HWND hwnd, wchar_t* buf, size_t bufChars);
