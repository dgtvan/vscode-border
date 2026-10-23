#include "project_list_hud.h"

#include "claude_profile.h"
#include "favourites.h"
#include "focus_trace.h"
#include "label_alias.h"
#include "layered_rendering.h"
#include "logger.h"
#include "monitor_scenario.h"
#include "taskbar_backdrop.h"
#include "vscode_cli.h"
#include "window_discovery.h"
#include "wsl.h"

#include <dwmapi.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cwchar>

static const wchar_t* kProjectListHudClassName = L"VSCodeBorderProjectListHudWndClass";
static const int kProjectListGap = 6;
static const int kProjectListEdgeGrip = 14; // wide enough to reliably land a Ctrl+drag resize -- too
                                             // thin here and a slightly-off drag silently becomes a
                                             // DragMove instead (no visible feedback that resizing
                                             // didn't "take"), leaving manualWidth false so the width
                                             // keeps auto-fitting to content indefinitely
static const int kProjectListMinWidth = 130; // manual (drag-resize) lower bound -- no upper bound: the
                                              // user can size the HUD as wide as the band
static const int kProjectListAutoMaxWidth = 360; // auto-measured upper bound for the shared item/column
                                                  // width -- narrower so a single long label can't blow up
                                                  // the HUD when the user hasn't sized it by hand
static const int kProjectListMeasurePaddingX = 32;
static const int kReorderDragThreshold = 4; // pixels of movement before a plain left-click-on-an-item
                                             // commits to a reorder-drag instead of a click-to-activate
static const int kProjectListMinRowHeight = 18;
static const UINT kAppBarCallbackMsg = WM_APP + 1; // shell -> HUD AppBar notifications (ABN_*)
static const UINT_PTR kClaudePulseTimerId = 1; // repaints while >=1 item is ClaudeStatus::Working or
                                                // loading, to animate its indicator -- started/stopped
                                                // on demand, see UpdateProjectListHud
static const UINT kClaudePulseIntervalMs = 150;
// Ring-spinner geometry shared by the Claude "Working" indicator and the
// loading-state indicator (see DrawHudItem): 8 cells around a 3x3 grid's
// perimeter (empty center), in clockwise order, one lit at a time.
static const int kRingCellSize = 4, kRingGap = 1, kRingStride = kRingCellSize + kRingGap;
static const int kRingSpan = 3 * kRingCellSize + 2 * kRingGap;
static const int kRingCells[8][2] = {
    {0, 0}, {1, 0}, {2, 0}, {2, 1}, {2, 2}, {1, 2}, {0, 2}, {0, 1},
};

// Usage rings (see ProjectListHudUsageRing): each profile's group is a chip
// in that profile's own background color (or none), holding its icon (if shown) and a rowHeight-square pair of
// rings, outer weekly and inner session.
static const int kUsageRingPad = 3, kUsageRingBadgeGap = 3, kUsageRingGap = 4;
static const double kUsageWarnPercent = 70, kUsageCriticalPercent = 90;
static const UINT kUsageUnavailablePulseMs = 1600; // one fade out and back in of the "!" (see DrawUsageRings)
static const COLORREF kUsageTrackColor = RGB(84, 89, 96),
                      kUsageOkColor = RGB(63, 174, 106), kUsageWarnColor = RGB(224, 165, 38),
                      kUsageCriticalColor = RGB(224, 81, 59), kUsageStaleColor = RGB(138, 148, 156);
static const wchar_t* kUsageTooltipClassName = L"VSCodeBorderUsageTooltipWndClass";
static const wchar_t* kUsageStripClassName = L"VSCodeBorderUsageStripWndClass";
static const int kUsageTooltipPadX = 8, kUsageTooltipPadY = 5;
static const COLORREF kUsageTooltipBackColor = RGB(43, 43, 43), kUsageTooltipBorderColor = RGB(96, 96, 96),
                      kUsageTooltipTextColor = RGB(235, 235, 235);

static HFONT CreateHudFont(int fontSize, int weight) {
    return CreateFontW(-fontSize, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                       CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
}

struct ProjectListHudState {
    std::vector<ProjectListHudEntry> entries;
    std::vector<RECT> itemRects; // per-entry rect within the HUD's own client area (local coords)
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    int rowHeight = 0;
    int fontSize = 0;
    bool labelTextColorAuto = false;
    COLORREF labelTextColor = RGB(0, 0, 0);
    int normalOpacity = 255;
    int hoverOpacity = 255;
    bool activateOnHover = false;
    COLORREF claudeColorWorking = RGB(255, 159, 10);
    COLORREF claudeColorAttention = RGB(220, 38, 38);
    COLORREF claudeColorWaiting = RGB(52, 199, 89);
    bool claudeBorderColorAuto = true;
    COLORREF claudeBorderColor = RGB(255, 255, 255);
    bool showNewWindowButton = false;
    COLORREF newWindowButtonColor = RGB(0, 0, 0);
    std::vector<ProjectListHudUsageRing> usageRings;
    std::vector<RECT> usageRingRects; // within usageStrip's own client area
    HWND usageStrip = nullptr;  // the rings' own window, at the right end of the band (see SyncUsageStrip)
    int usageHoverRing = -1;    // ring group under the cursor, -1 for none
    bool usageTrackingMouseLeave = false;
    HWND usageTooltip = nullptr; // popup shown above a hovered ring group (see SyncUsageTooltip)
    int usageTooltipRing = -1;   // the ring group it's showing, -1 while hidden
    std::wstring usageTooltipText;
    HFONT usageTooltipFont = nullptr;
    int usageTooltipFontSize = 0;
    RECT actionButtonRects[3] = {}; // indexed by HudActionButton; only the first ActionButtonCount are
                                     // valid -- see Rebuild*ItemRects. Not part of itemRects/entries: fixed
                                     // at the end, no drag-reorder. Button k's "index" for hover/click
                                     // purposes is the sentinel entries.size() + k.
    int hoverIndex = -1;
    bool trackingMouseLeave = false;
    bool hoverFocusActive = false;
    HWND previousForeground = nullptr;
    bool clickActivates = false; // WS_EX_NOACTIVATE temporarily lifted -- see SetHudClickActivates
    bool manualPosition = false;
    bool manualWidth = false;
    int manualItemWidth = 0; // the user's chosen per-item width, independent of
                              // how many items are currently shown -- state->width (the whole strip) is
                              // re-derived from this every time entries.size() changes, so item count
                              // never changes how wide each item looks (see RebuildItemRects)
    enum DragMode { DragNone, DragMove, DragResizeLeft, DragResizeRight, DragReorder } dragMode = DragNone;
    POINT dragStart = {0, 0};
    RECT dragStartRect = {0, 0, 0, 0};
    std::vector<std::wstring> manualOrder; // aliasKeys in last-dragged display order, this run only
    int pendingDragIndex = -1;   // item index under the cursor at WM_LBUTTONDOWN, before a plain-left-drag
                                  // has moved far enough to commit to DragReorder (vs. a plain click)
    int reorderIndex = -1;       // during DragReorder: dragged entry's current position in state->entries
    POINT reorderGrabOffset = {0, 0}; // cursor position relative to the dragged item's top-left at grab
                                       // time, so the floating item doesn't jump to be cursor-centered
    POINT reorderCursorClient = {0, 0}; // live cursor position (client coords) during DragReorder, used
                                        // to place the floating item
    HWND dragGhost = nullptr; // small always-on-top window, shown only during DragReorder, that renders
                              // the floating dragged item -- kept separate from the HUD's own window so
                              // it can move freely outside the HUD's bounds (see PaintDragGhost)
    std::wstring scenarioKey; // current monitor scenario (see monitor_scenario.h) -- tracked so a
                              // WM_DISPLAYCHANGE can tell whether the active monitor set actually
                              // changed, and so a completed drag knows which scenario to save under
    HWND editControl = nullptr; // non-null while inline-editing an item's alias (see BeginAliasEdit)
    HFONT editFont = nullptr;
    int editIndex = -1;
    bool contextMenuOpen = false; // true for the duration of TrackPopupMenu's modal loop -- see
                                   // UpdateProjectListHud's guard for why this (and editIndex >= 0)
                                   // needs to suppress resyncs while set
    bool pulseTimerActive = false; // mirrors whether kClaudePulseTimerId is currently running
    bool docked = false; // registered as a bottom-edge AppBar, i.e. the hub is on -- see SetProjectListHudDocked
    int dockBandHeight = 0;
    RECT dockRect = {}; // the reserved band (screen coords) the shell granted, valid while docked. Spans
                        // the whole monitor width; the HUD itself sits somewhere inside it.
    bool fullscreenAppOpen = false; // the shell reported a fullscreen app (ABN_FULLSCREENAPP),
                                     // so the HUD stays hidden instead of painting over it
    bool matchTaskbar = false; // paint the rest of the band like the taskbar (see backdrop)
    HWND backdrop = nullptr;   // fills the band behind the HUD -- see taskbar_backdrop.h
    bool visibleBeforeFullscreen = false; // whether to bring the HUD back once that fullscreen app closes --
                                           // it may have been hidden for its own reasons (no windows, etc.)
    bool appBarFailureLogged = false; // see SetProjectListHudDocked -- retried every sync, warned about once
};

static int ClampInt(int value, int minValue, int maxValue) {
    return std::max(minValue, std::min(value, maxValue));
}

// The fixed square buttons after the last entry, in display order. Only
// shown while style.showNewWindowButton is set.
enum HudActionButton { HudActionNewWindow = 0, HudActionMinimizeAll = 1, HudActionCloseAll = 2 };

// Minimize-all / close-all are temporarily hidden -- flip this back on to
// show them again; everything else about them is still wired up.
static const bool kShowBulkWindowButtons = false;

// How many of the HudActionButton squares are shown: none with the buttons
// off, just the new-window one with no windows open (nothing to minimize or
// close -- the hub is then only what opens a window again), else all three.
static int ActionButtonCount(const ProjectListHudState* state) {
    if (!state->showNewWindowButton) return 0;
    return (state->entries.empty() || !kShowBulkWindowButtons) ? 1 : 3;
}

// Width of the action buttons laid out side by side, gaps between them.
static int ActionButtonsRowWidth(const ProjectListHudState* state) {
    int count = ActionButtonCount(state);
    return count > 0 ? count * state->rowHeight + (count - 1) * kProjectListGap : 0;
}

static int UsageRingGroupWidth(const ProjectListHudState* state, const ProjectListHudUsageRing& ring) {
    return 2 * kUsageRingPad + (ring.showProfileIcon ? kProfileIconSize + kUsageRingBadgeGap : 0) + state->rowHeight;
}

static int UsageRingsRowWidth(const ProjectListHudState* state) {
    int width = 0;
    for (const ProjectListHudUsageRing& ring : state->usageRings) width += UsageRingGroupWidth(state, ring);
    return width + (state->usageRings.empty() ? 0 : ((int)state->usageRings.size() - 1) * kUsageRingGap);
}

// Space the action buttons take at the end of the strip,
// including the gap separating them from the last entry.
static int ActionButtonsReserve(const ProjectListHudState* state) {
    return ActionButtonCount(state) > 0 ? ActionButtonsRowWidth(state) + kProjectListGap : 0;
}

// The right edge the HUD itself can reach: the band's, less the usage rings'
// strip at its right end and a gap before it.
static int HudRightLimit(const ProjectListHudState* state) {
    int rings = UsageRingsRowWidth(state);
    return (int)state->dockRect.right - (rings > 0 ? rings + kProjectListGap : 0);
}

// The HudActionButton a hover/hit index refers to (see ProjectListHitTest),
// or -1 if it's an entry or no hit at all.
static int ActionButtonForIndex(const ProjectListHudState* state, int index) {
    int k = index - (int)state->entries.size();
    return (k >= 0 && k < ActionButtonCount(state)) ? k : -1;
}

// Returns an entry index for a hit within state->itemRects, or the sentinel
// state->entries.size() + k for a hit on action button k (see
// actionButtonRects' comment), or -1 for no hit.
static int ProjectListHitTest(const ProjectListHudState* state, int x, int y) {
    if (!state || x < 0 || x >= state->width || y < 0 || y >= state->height) return -1;
    for (size_t i = 0; i < state->itemRects.size(); i++) {
        const RECT& r = state->itemRects[i];
        if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) return (int)i;
    }
    for (int k = 0; k < ActionButtonCount(state); k++) {
        const RECT& r = state->actionButtonRects[k];
        if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) return (int)state->entries.size() + k;
    }
    return -1;
}

// Like ProjectListHitTest, but for choosing a reorder-drop target during an
// active drag: ignores the vertical coordinate entirely (how far above or
// below the strip the ghost has been dragged) and maps purely by position
// along the strip, clamped to the first/last
// slot past either end. This lets a middle slot still be targeted while
// dragging far outside the strip's bounds, not just the two end slots.
static int ProjectListDragCandidate(const ProjectListHudState* state, int x) {
    if (!state || state->itemRects.empty()) return -1;
    if (x < state->itemRects.front().left) return 0;
    for (size_t i = 0; i < state->itemRects.size(); i++) {
        if (x < state->itemRects[i].right) return (int)i;
    }
    return (int)state->itemRects.size() - 1;
}

// True if `entry`'s folder is a saved favourite, whatever profile its window
// runs (see favourites.h) -- shown as the star marker only; it has no
// effect on the item's position.
static bool EntryIsFavourite(const ProjectListHudEntry& entry) {
    return !entry.path.empty() && IsFavourite(entry.path);
}

static bool IsProjectListResizeEdge(const ProjectListHudState* state, int x) {
    return state && state->width > 0 && (x < kProjectListEdgeGrip || x >= state->width - kProjectListEdgeGrip);
}

static ProjectListHudState::DragMode ProjectListDragModeForPoint(const ProjectListHudState* state, int x) {
    if (!state) return ProjectListHudState::DragNone;
    if (x < kProjectListEdgeGrip) return ProjectListHudState::DragResizeLeft;
    if (x >= state->width - kProjectListEdgeGrip) return ProjectListHudState::DragResizeRight;
    return ProjectListHudState::DragMove;
}

// Divides state->width evenly across items, same width for every one
// regardless of label length (long labels just ellipsize). Cheap (no
// GDI/text-measurement calls), so it's safe to call on every mouse-move
// frame of an interactive resize-drag.
static void RebuildItemRects(ProjectListHudState* state) {
    size_t n = state->entries.size();
    state->itemRects.assign(n, RECT{});

    int itemsWidth = state->width - ActionButtonsReserve(state);

    if (n > 0) {
        int itemWidth = std::max(1, (itemsWidth - (int)(n - 1) * kProjectListGap) / (int)n);
        int x = 0;
        for (size_t i = 0; i < n; i++) {
            state->itemRects[i] = {x, 0, x + itemWidth, state->rowHeight};
            x += itemWidth + kProjectListGap;
        }
    }

    int size = state->rowHeight;
    int x = state->width - ActionButtonsRowWidth(state);
    for (int k = 0; k < ActionButtonCount(state); k++) {
        state->actionButtonRects[k] = {x, 0, x + size, size};
        x += size + kProjectListGap;
    }
    state->usageRingRects.clear();
    x = 0;
    for (const ProjectListHudUsageRing& ring : state->usageRings) {
        int w = UsageRingGroupWidth(state, ring);
        state->usageRingRects.push_back({x, 0, x + w, size});
        x += w + kUsageRingGap;
    }
}

// Move/resize is Ctrl+left-click-drag (plain right-click is the context
// menu instead -- see WM_RBUTTONUP), so the move/resize cursor hints only
// show while Ctrl is actually held; otherwise items just look clickable.
static LPCWSTR ProjectListCursorForPoint(const ProjectListHudState* state, int x, int y) {
    if (state && (state->dragMode == ProjectListHudState::DragResizeLeft ||
                  state->dragMode == ProjectListHudState::DragResizeRight)) {
        return IDC_SIZEWE;
    }
    if (state && state->dragMode == ProjectListHudState::DragMove) return IDC_SIZEALL;
    bool ctrlDown = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    if (ctrlDown) return IsProjectListResizeEdge(state, x) ? IDC_SIZEWE : IDC_SIZEALL;
    return ProjectListHitTest(state, x, y) >= 0 ? IDC_HAND : IDC_ARROW;
}

static void RenderProjectListHud(HWND hud, ProjectListHudState* state);
static void SyncUsageStrip(HWND hud, ProjectListHudState* state);
static void PaintDragGhost(ProjectListHudState* state);
static void MoveDragGhost(ProjectListHudState* state);

// True if a visible window sits in front of the HUD in z-order and
// overlaps it. The HUD is topmost, so only another topmost window can --
// ordinary windows, maximized or not, are always behind it.
static bool IsHudCovered(HWND hud, const ProjectListHudState* state) {
    RECT hudRect = {state->x, state->y, state->x + state->width, state->y + state->height};
    for (HWND w = GetWindow(hud, GW_HWNDPREV); w; w = GetWindow(w, GW_HWNDPREV)) {
        if (w == state->dragGhost || w == state->editControl || !IsWindowVisible(w)) continue;
        // Visible-but-cloaked: suspended UWP apps, windows on other virtual
        // desktops -- present in the z-order, not on screen.
        BOOL cloaked = FALSE;
        DwmGetWindowAttribute(w, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
        if (cloaked) continue;
        RECT r, overlap;
        if (GetWindowRect(w, &r) && IntersectRect(&overlap, &hudRect, &r)) return true;
    }
    return false;
}

// Brings the backdrop (when `withBackdrop`) and then the HUD to the front
// of the topmost band -- the backdrop directly behind the HUD, both in
// front of everything else, the way the taskbar sits in front of every
// ordinary window. One DeferWindowPos batch, which Windows applies as a
// unit, so there's never a moment with the backdrop in front of the HUD
// (a visible blink, when these were separate calls).
static void RaiseBandWindows(HWND hud, ProjectListHudState* state, bool withBackdrop) {
    const UINT flags = SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE;
    // The rings' strip goes along, in front of the backdrop too.
    bool strip = state->usageStrip && IsWindowVisible(state->usageStrip);
    HDWP batch = BeginDeferWindowPos((withBackdrop ? 2 : 1) + (strip ? 1 : 0));
    if (batch && withBackdrop) batch = DeferWindowPos(batch, state->backdrop, HWND_TOPMOST, 0, 0, 0, 0, flags);
    if (batch && strip) batch = DeferWindowPos(batch, state->usageStrip, HWND_TOPMOST, 0, 0, 0, 0, flags);
    if (batch) batch = DeferWindowPos(batch, hud, HWND_TOPMOST, 0, 0, 0, 0, flags | SWP_SHOWWINDOW);
    if (!batch || !EndDeferWindowPos(batch)) {
        if (withBackdrop) SetWindowPos(state->backdrop, HWND_TOPMOST, 0, 0, 0, 0, flags);
        if (strip) SetWindowPos(state->usageStrip, HWND_TOPMOST, 0, 0, 0, 0, flags);
        SetWindowPos(hud, HWND_TOPMOST, 0, 0, 0, 0, flags | SWP_SHOWWINDOW);
    }
    // Re-asserting HWND_TOPMOST has been seen not to move the HUD ahead of
    // some other window that went topmost more recently (e.g. briefly, when
    // another app's own window is maximized) -- toggling through
    // HWND_NOTOPMOST forces a real re-insertion at the front. Only as a
    // fallback: the toggle briefly drops the HUD behind the backdrop, which
    // is harmless here only because the HUD is covered anyway.
    if (IsHudCovered(hud, state)) {
        LogDiag(L"hud: still covered after raise -- toggling through HWND_NOTOPMOST");
        SetWindowPos(hud, HWND_NOTOPMOST, 0, 0, 0, 0, flags);
        SetWindowPos(hud, HWND_TOPMOST, 0, 0, 0, 0, flags);
    }
}

static void PositionProjectListHud(HWND hud, ProjectListHudState* state) {
    if (!state) return;
    if (state->docked && state->fullscreenAppOpen) {
        ShowWindow(hud, SW_HIDE);
        SyncUsageStrip(hud, state);
        return;
    }
    bool wasVisible = IsWindowVisible(hud) != FALSE;
    SetWindowPos(hud, nullptr, state->x, state->y, state->width, state->height, SWP_NOACTIVATE | SWP_NOZORDER);
    // Only touch the z-order when it's actually wrong: this runs on every
    // sync and every frame of a drag.
    bool backdrop = state->docked && IsWindowVisible(state->backdrop);
    bool backdropDemoted = backdrop && !(GetWindowLongPtrW(state->backdrop, GWL_EXSTYLE) & WS_EX_TOPMOST);
    if (!wasVisible || backdropDemoted || IsHudCovered(hud, state)) RaiseBandWindows(hud, state, backdrop);
    SyncUsageStrip(hud, state);
}

// Re-checks the current monitor scenario against state->scenarioKey and, if
// it changed, applies whatever placement was last saved for the new one --
// or drops back to auto-placement if this scenario has never been seen
// before, letting the next regular UpdateProjectListHud recompute the
// default position/width. Called once at HUD creation (so the very first
// sync already uses a remembered placement, if any) and on every
// WM_DISPLAYCHANGE. Returns true if the scenario actually changed.
//
// The saved "width" means *per-item* width, not the whole strip's, so
// restoring it doesn't depend on how many windows happen to be tracked at
// that moment (see manualItemWidth's comment). Saved under the scenario key
// plus "|docked": the suffix dates from when the hub also had a free-floating
// mode with placements of its own, and is kept so existing ones still load.
static std::wstring PlacementKey(const ProjectListHudState* state) {
    return state->scenarioKey + L"|docked";
}

static void LoadPlacementForCurrentKey(ProjectListHudState* state) {
    SavedHudPlacement saved = LoadHudPlacement(PlacementKey(state));
    state->manualPosition = saved.found;
    state->manualWidth = saved.found;
    if (saved.found) {
        state->x = saved.x;
        state->manualItemWidth = std::max(1, saved.width);
        size_t n = state->entries.size();
        state->width = (n > 0 ? (int)n * state->manualItemWidth + (int)(n - 1) * kProjectListGap
                               : state->manualItemWidth) +
                       ActionButtonsReserve(state);
    }
}

static bool ApplyScenarioForCurrentMonitors(ProjectListHudState* state) {
    std::wstring key = GetMonitorScenarioKey();
    if (!state || key == state->scenarioKey) return false;
    state->scenarioKey = key;
    LoadPlacementForCurrentKey(state);
    return true;
}

// Fits state->x/y/width into the reserved band, left of the usage rings'
// strip (see HudRightLimit): y is pinned to the band's bottom (the HUD can
// only slide sideways), the width is capped at what's left of the band, and
// x is either the remembered one clamped into it or, if never placed by
// hand, right-aligned against the rings.
static void FitIntoDock(ProjectListHudState* state) {
    const RECT& d = state->dockRect;
    int right = HudRightLimit(state);
    state->width = std::max(1, std::min(state->width, right - (int)d.left));
    state->x = state->manualPosition ? ClampInt(state->x, d.left, right - state->width) : right - state->width;
    state->y = d.bottom - state->height; // on the taskbar side, clear of the backdrop's border row
}

// Shown exactly while the band is reserved, enabled, and not yielding to a
// fullscreen app. Showing it resamples the taskbar, so every caller that
// may have changed the band (or the taskbar) gets a fresh sample.
static void SyncBackdrop(HWND hud, ProjectListHudState* state) {
    if (state->docked && state->matchTaskbar && !state->fullscreenAppOpen) {
        ShowTaskbarBackdrop(state->backdrop, state->dockRect);
        if (IsWindowVisible(hud)) RaiseBandWindows(hud, state, true);
        else SetWindowPos(state->backdrop, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE);
    } else {
        HideTaskbarBackdrop(state->backdrop);
    }
}

// Asks the shell for a band of dockBandHeight at the bottom of the primary
// monitor and reserves it -- the shell stacks it against the taskbar when
// the taskbar is on that same edge, so the band lands directly above it,
// and shrinks the work area so maximized windows stop above the band. The
// QUERYPOS/re-apply-height/SETPOS sequence is the documented one: QUERYPOS
// may move either edge of the proposed rect out of the taskbar's way, so
// the height is reapplied from the (adjusted) bottom before committing.
static void RefreshDockRect(HWND hud, ProjectListHudState* state) {
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    APPBARDATA abd = {};
    abd.cbSize = sizeof(abd);
    abd.hWnd = hud;
    abd.uEdge = ABE_BOTTOM;
    abd.rc = mi.rcMonitor;
    abd.rc.top = abd.rc.bottom - state->dockBandHeight;
    SHAppBarMessage(ABM_QUERYPOS, &abd);
    abd.rc.top = abd.rc.bottom - state->dockBandHeight;
    SHAppBarMessage(ABM_SETPOS, &abd);
    state->dockRect = abd.rc;
    Log(L"hud: docked band=(%ld,%ld)-(%ld,%ld)", abd.rc.left, abd.rc.top, abd.rc.right, abd.rc.bottom);
    SyncBackdrop(hud, state);
}

// Re-lays-out an already-populated HUD after the band itself moved (taskbar
// moved/resized, display change) -- without waiting for the next regular
// UpdateProjectListHud, which only runs when something about the tracked
// windows changes.
static void RelayoutDocked(HWND hud, ProjectListHudState* state) {
    if (!state->docked || state->entries.empty()) return;
    FitIntoDock(state);
    RebuildItemRects(state);
    if (IsWindowVisible(hud)) {
        RenderProjectListHud(hud, state);
        PositionProjectListHud(hud, state);
    }
}

static bool RegisterAppBar(HWND hud) {
    APPBARDATA abd = {};
    abd.cbSize = sizeof(abd);
    abd.hWnd = hud;
    abd.uCallbackMessage = kAppBarCallbackMsg;
    return SHAppBarMessage(ABM_NEW, &abd) != 0;
}

static void UnregisterAppBar(HWND hud) {
    APPBARDATA abd = {};
    abd.cbSize = sizeof(abd);
    abd.hWnd = hud;
    SHAppBarMessage(ABM_REMOVE, &abd);
}

// True while one of the shell's own flyouts holds the foreground -- the
// taskbar thumbnail list that clicking a grouped taskbar button opens
// (several VS Code windows -> one grouped button -> this flyout), or the
// Quick Settings panel (network/volume/battery icons), both of which are
// exactly the moment someone reaches for this HUD instead of picking a
// thumbnail or dismissing the panel.
//
// While either is up, *no* SetForegroundWindow aimed at another window is
// granted, from anyone. Measured with a stand-in for this HUD against a
// throwaway target window: refused with a plain request, after a dummy
// SendInput, via SwitchToThisWindow, after an injected Alt tap, after an
// injected Escape (which does not even close it), and after minimize +
// restore -- and still refused when this process had been made the
// foreground one by AttachThreadInput. The same stand-in with the flyout
// closed was granted every time. The flyout gives the foreground back only
// when it is itself deactivated, and a WS_EX_NOACTIVATE click never
// deactivates anything -- see SetHudClickActivates for the way out.
// ControlCenterWindow (Quick Settings) was added after logs showed the same
// refused-request/flashing-taskbar-button symptom with it in the foreground.
//
// The shell's CoreWindow panels -- Search and Start (SearchHost.exe, and
// StartMenuExperienceHost.exe on builds where Start lives there) and the
// Notification Center (ShellExperienceHost.exe) -- were added the same way:
// four refused HUD clicks in a row with Search in the foreground. Measured
// with the stand-in: Search and Notification Center each refused every plain
// request and granted every one with SetHudClickActivates. The class alone
// is not enough -- packaged apps use it too -- so the owning process decides.
static bool ForegroundIsTaskbarFlyout() {
    HWND fg = GetForegroundWindow();
    if (!fg || !IsWindowVisible(fg)) return false; // Alt+Tab's switcher shares the class but is invisible
    wchar_t cls[64] = {};
    GetClassNameW(fg, cls, 64);
    if (wcscmp(cls, L"XamlExplorerHostIslandWindow") == 0 || wcscmp(cls, L"ControlCenterWindow") == 0) return true;
    if (wcscmp(cls, L"Windows.UI.Core.CoreWindow") != 0) return false;
    std::wstring exe;
    if (!GetProcessImageName(fg, exe)) return false;
    return _wcsicmp(exe.c_str(), L"SearchHost.exe") == 0 || _wcsicmp(exe.c_str(), L"StartMenuExperienceHost.exe") == 0 ||
           _wcsicmp(exe.c_str(), L"ShellExperienceHost.exe") == 0;
}

// The HUD is WS_EX_NOACTIVATE so that using it never takes focus away from
// the editor. That is also why a HUD click cannot get past a taskbar flyout
// (see ForegroundIsTaskbarFlyout): only a *system* activation deactivates
// the flyout, and a no-activate window never causes one.
//
// So while a flyout holds the foreground and the cursor is over the HUD, the
// style is lifted: the click then activates the HUD the ordinary way, which
// dismisses the flyout and makes this process the foreground one, and the
// item's activation that follows on button-up is granted. Measured with the
// same stand-in: 5 of 5 granted, the target foreground immediately, where
// every attempt without this was refused. Put back as soon as the cursor
// leaves or the flyout is gone, so everyday clicks stay exactly as before.
static void SetHudClickActivates(HWND hud, ProjectListHudState* state, bool activates) {
    if (!state || state->clickActivates == activates) return;
    LONG_PTR ex = GetWindowLongPtrW(hud, GWL_EXSTYLE);
    SetWindowLongPtrW(hud, GWL_EXSTYLE, activates ? (ex & ~WS_EX_NOACTIVATE) : (ex | WS_EX_NOACTIVATE));
    state->clickActivates = activates;
    // Logged because the HUD then shows up in the trace as a foreground
    // change with no request of ours behind it (source=external), which is
    // otherwise unexplained.
    Log(L"hud: %ls", activates ? L"taskbar flyout holds the foreground -- a click on the HUD will activate the "
                                 L"HUD itself to dismiss it, so the item's activation can be granted"
                               : L"back to no-activate clicks");
}

// `reason` is a short stable tag naming the gesture that got here (see
// focus_trace.h) -- hover and click both land in this one function, and
// telling them apart in the log is the difference between "the user clicked
// three items" and "one mouse sweep asked to activate every item it
// crossed", which look identical otherwise.
static void ActivateProjectListItem(ProjectListHudState* state, int index, const wchar_t* reason) {
    if (!state || index < 0 || index >= (int)state->entries.size()) return;
    HWND target = state->entries[index].target;
    if (!target || !IsWindow(target)) return;

    if (!state->hoverFocusActive) {
        state->previousForeground = GetForegroundWindow();
        state->hoverFocusActive = true;
    }

    // Logged for every activation, not just the minimized ones: when the
    // focus request that follows is refused, the trace otherwise identifies
    // the target by hwnd alone, and which *item* was clicked then can't be
    // recovered from the log at all.
    bool minimized = IsIconic(target) != 0;
    Log(L"hud activate reason=[%ls] index=%d label=[%ls]%ls", reason, index,
        state->entries[index].label.c_str(), minimized ? L" -- restoring minimized target first" : L"");
    if (minimized) ShowWindow(target, SW_RESTORE);
    RequestForeground(target, FocusTargetKind::ExternalWindow, reason);
}

// The running VS Code window the CLI shim is derived from (see
// vscode_cli.h's ResolveVSCodeCliShim) -- null once every window is closed,
// which that resolver falls back from. Never a WSLg window: its process is
// WSL's msrdc.exe, not a VS Code install with a shim next to it.
static HWND AnyTrackedWindow(const ProjectListHudState* state) {
    if (!state) return nullptr;
    for (const ProjectListHudEntry& e : state->entries) {
        if (e.wslDistro.empty()) return e.target;
    }
    return nullptr;
}

// Minimizes every tracked VS Code window -- the hub's minimize-all button.
// ShowWindowAsync so one hung window can't stall the HUD's own thread.
static void MinimizeAllVSCodeWindows(const ProjectListHudState* state) {
    int count = 0;
    for (const ProjectListHudEntry& e : state->entries) {
        if (!e.target || !IsWindow(e.target) || IsIconic(e.target)) continue;
        ShowWindowAsync(e.target, SW_MINIMIZE);
        count++;
    }
    Log(L"hud minimize-all: minimized %d of %d window(s)", count, (int)state->entries.size());
}

// Asks every tracked VS Code window to close -- the hub's close-all button.
// Confirms first (see ConfirmAndCloseVSCodeWindows), since it sits right
// next to the other buttons and a stray click would otherwise tear down the
// whole session.
static void CloseAllVSCodeWindows(HWND hud, ProjectListHudState* state) {
    // Snapshot first: a resync while the confirmation is up would replace
    // state->entries -- suppressed below, but the targets are all we need.
    std::vector<HWND> targets;
    for (const ProjectListHudEntry& e : state->entries) targets.push_back(e.target);
    // Same resync guard as the context menus (see UpdateProjectListHud):
    // the message box runs its own modal loop.
    state->contextMenuOpen = true;
    ConfirmAndCloseVSCodeWindows(hud, targets, L"hud");
    state->contextMenuOpen = false;
}

static void EndProjectListHoverFocus(ProjectListHudState* state, bool restorePrevious) {
    if (!state || !state->hoverFocusActive) return;
    HWND previous = state->previousForeground;
    state->hoverFocusActive = false;
    state->previousForeground = nullptr;

    if (restorePrevious && previous && IsWindow(previous)) {
        if (IsIconic(previous)) ShowWindow(previous, SW_RESTORE);
        RequestForeground(previous, FocusTargetKind::ExternalWindow, L"hud-hover-restore-previous");
    }
}

static std::wstring TrimWhitespace(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return L"";
    size_t b = s.find_last_not_of(L" \t\r\n");
    return s.substr(a, b - a + 1);
}

// Ends the in-progress alias edit (if any): on commit, saves the trimmed
// text as the new alias for that item's aliasKey (an empty result removes
// the alias, reverting to the raw label) and updates the entry's displayed
// text immediately. Guarding on editIndex/editControl up front, and
// clearing them *before* DestroyWindow, makes this safe to call
// re-entrantly -- DestroyWindow on a focused window synchronously fires
// WM_KILLFOCUS, which would otherwise call back into this function while
// it's still unwinding.
static void EndAliasEdit(HWND hud, ProjectListHudState* state, bool commit) {
    if (!state || state->editIndex < 0 || !state->editControl) return;
    int index = state->editIndex;
    HWND edit = state->editControl;
    HFONT font = state->editFont;
    state->editIndex = -1;
    state->editControl = nullptr;
    state->editFont = nullptr;

    if (commit && index < (int)state->entries.size()) {
        wchar_t buf[256] = {};
        GetWindowTextW(edit, buf, 255);
        std::wstring newAlias = TrimWhitespace(buf);
        const ProjectListHudEntry& entry = state->entries[index];
        SetAlias(entry.aliasKey, newAlias);
        state->entries[index].label = newAlias.empty() ? entry.rawLabel : newAlias;
    }

    DestroyWindow(edit);
    if (font) DeleteObject(font);
    RenderProjectListHud(hud, state);
}

// Classic (pre-comctl32-subclassing) WNDPROC swap for the alias EDIT
// control -- the original proc is stashed in the edit control's own
// GWLP_USERDATA (distinct from the HUD window's GWLP_USERDATA, which holds
// ProjectListHudState). Catches Enter/Escape to commit/cancel, and
// focus-loss (e.g. clicking elsewhere) to commit.
static LRESULT CALLBACK AliasEditSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    WNDPROC origProc = (WNDPROC)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if ((msg == WM_KEYDOWN && (wParam == VK_RETURN || wParam == VK_ESCAPE)) || msg == WM_KILLFOCUS) {
        HWND hud = GetParent(hwnd);
        ProjectListHudState* state = (ProjectListHudState*)GetWindowLongPtrW(hud, GWLP_USERDATA);
        EndAliasEdit(hud, state, msg != WM_KEYDOWN || wParam == VK_RETURN);
        return 0; // control is destroyed by EndAliasEdit -- do not forward to origProc
    }
    return CallWindowProcW(origProc, hwnd, msg, wParam, lParam);
}

// Turns item `index` into an inline text box (prefilled with its current
// display text) so the user can type a new alias -- Enter/focus-loss
// commits, Escape cancels. See EndAliasEdit for how the result is applied.
//
// The edit box is a genuine top-level window OWNED by (not a WS_CHILD of)
// the HUD, positioned in screen coordinates over the item -- not a child
// window in HUD client coordinates. A WS_CHILD control does not reliably
// repaint on top of a parent that paints itself via UpdateLayeredWindow
// (as the HUD does, for its per-pixel-alpha chips): it still receives
// keyboard input, but typed characters don't visibly appear until
// something else forces a full repaint of that screen region. An owned
// top-level window composites normally regardless, sidestepping that
// entirely. GetParent() on an owned popup still returns the owner, so
// AliasEditSubclassProc's lookup of the HUD's state is unaffected.
static void BeginAliasEdit(HWND hud, ProjectListHudState* state, int index) {
    if (!state || index < 0 || index >= (int)state->itemRects.size()) return;
    EndAliasEdit(hud, state, false); // defensive: cancel any stray edit already in progress

    const RECT& item = state->itemRects[index];
    int screenX = state->x + item.left;
    int screenY = state->y + item.top;
    int width = item.right - item.left;
    int height = item.bottom - item.top;

    HWND edit = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"EDIT", state->entries[index].label.c_str(),
                                 WS_POPUP | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                                 screenX, screenY, width, height,
                                 hud, nullptr, (HINSTANCE)GetWindowLongPtrW(hud, GWLP_HINSTANCE), nullptr);
    if (!edit) return;

    HFONT font = CreateHudFont(state->fontSize, FW_SEMIBOLD);
    SendMessageW(edit, WM_SETFONT, (WPARAM)font, TRUE);
    SendMessageW(edit, EM_SETSEL, 0, -1);

    WNDPROC origProc = (WNDPROC)SetWindowLongPtrW(edit, GWLP_WNDPROC, (LONG_PTR)AliasEditSubclassProc);
    SetWindowLongPtrW(edit, GWLP_USERDATA, (LONG_PTR)origProc);
    RequestForeground(edit, FocusTargetKind::OwnUi, L"alias-edit-box");
    SetFocus(edit);

    state->editControl = edit;
    state->editFont = font;
    state->editIndex = index;
}

// Clears whatever alias is set for item `index`, reverting its displayed
// text back to the raw label -- the same end result as BeginAliasEdit
// followed by committing an empty string, just without opening the edit
// box first.
static void ClearAlias(HWND hud, ProjectListHudState* state, int index) {
    if (!state || index < 0 || index >= (int)state->entries.size()) return;
    SetAlias(state->entries[index].aliasKey, L"");
    state->entries[index].label = state->entries[index].rawLabel;
    RenderProjectListHud(hud, state);
}

// Puts `path` on the clipboard as plain Unicode text. The clipboard takes
// ownership of the HGLOBAL only when SetClipboardData succeeds, so it's
// freed here on every other exit.
static void CopyPathToClipboard(HWND hud, const std::wstring& path) {
    size_t bytes = (path.size() + 1) * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!mem) {
        Log(L"copy directory path: GlobalAlloc(%zu) failed, lastError=%lu", bytes, GetLastError());
        return;
    }
    void* dst = GlobalLock(mem);
    if (!dst) {
        Log(L"copy directory path: GlobalLock failed, lastError=%lu", GetLastError());
        GlobalFree(mem);
        return;
    }
    memcpy(dst, path.c_str(), bytes);
    GlobalUnlock(mem);

    if (!OpenClipboard(hud)) {
        Log(L"copy directory path: OpenClipboard failed (held by another app?), lastError=%lu", GetLastError());
        GlobalFree(mem);
        return;
    }
    EmptyClipboard();
    if (!SetClipboardData(CF_UNICODETEXT, mem)) {
        Log(L"copy directory path: SetClipboardData failed, lastError=%lu", GetLastError());
        GlobalFree(mem);
    }
    CloseClipboard();
}

// Opens a File Explorer window showing the contents of `path`. Checked for
// existence first because explorer.exe, handed a folder that no longer
// exists (e.g. a worktree deleted while its window stayed open), silently
// opens its default Home view instead of reporting anything.
static void OpenPathInExplorer(const std::wstring& path) {
    DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        Log(L"open directory in explorer: [%ls] is not an existing directory", path.c_str());
        return;
    }
    std::wstring params = L"\"" + path + L"\"";
    HINSTANCE result = ShellExecuteW(nullptr, L"open", L"explorer.exe", params.c_str(), nullptr, SW_SHOWNORMAL);
    if ((INT_PTR)result <= 32) {
        Log(L"ShellExecuteW(open directory in explorer) failed for %ls, code=%Id", path.c_str(), (INT_PTR)result);
    }
}

// Appends Claude profile `profile` (empty for the default) to `m` as item
// `id`, with its icon (see ShowsClaudeProfileIcon).
static void AppendProfileMenuItem(HMENU m, UINT id, const std::wstring& profile) {
    AppendMenuW(m, MF_STRING, id, ClaudeProfileMenuText(profile).c_str());
    MENUITEMINFOW mii = {sizeof(mii)};
    mii.fMask = MIIM_BITMAP;
    mii.hbmpItem = GetClaudeProfileMenuBitmap(profile);
    if (mii.hbmpItem) SetMenuItemInfoW(m, GetMenuItemCount(m) - 1, TRUE, &mii);
}

static void ShowItemContextMenu(HWND hud, ProjectListHudState* state, int index, POINT screenPt) {
    // Groups top to bottom: directory, reopen, alias, favourites, close. No path means VS
    // Code hasn't recorded this window's folder in its own workspaceStorage
    // (e.g. a multi-root workspace) -- nothing to copy/open in Explorer or
    // for a favourite to reopen later, so the directory and favourites
    // groups are left off entirely rather than shown disabled, leaving just
    // the alias group. Once a folder IS already a favourite, the favourites
    // item toggles to "Remove from Favourites" instead -- same removal is
    // also still offered from the "+" button's own menu (see
    // ShowNewWindowButtonMenu), for a favourite that isn't currently
    // open as a hub item.
    // While loading (see ProjectListHudEntry::loading), this window's
    // path/alias may not be known yet, or may still change -- an alias set
    // now would key it to a rawLabel that's about to be replaced, silently
    // orphaning it once VS Code's title/worktree mapping settles. Rather
    // than a menu whose shape/enabled-state shifts as that data trickles
    // in, show every possible item, all disabled, so it's unambiguous:
    // nothing here is usable yet.
    // "Reopen As" closes this window and opens its folder again as another
    // Claude profile (any configured one but the window's own) -- see
    // vscode_cli.h's CloseAndReopenVSCodeWindows. Left off with no path, for
    // a WSL window, or when there's no other profile.
    bool loading =index >= 0 && index < (int)state->entries.size() && state->entries[index].loading;
    bool hasPath = !loading && index >= 0 && index < (int)state->entries.size() && !state->entries[index].path.empty();
    bool alreadyFav = hasPath && EntryIsFavourite(state->entries[index]);
    bool hasAlias = !loading && index >= 0 && index < (int)state->entries.size() &&
                    state->entries[index].label != state->entries[index].rawLabel;
    // A WSL window's folder (its \\wsl.localhost path, see wsl.h) can only be
    // shown in Explorer while the distro shares its files with Windows,
    // which it can switch off -- greyed out then, rather than an item that
    // silently does nothing. (The distro is running: its window is open.)
    bool explorerReachable = true;
    if (hasPath && ParseWslUncPath(state->entries[index].path, nullptr, nullptr)) {
        DWORD attrs = GetFileAttributesW(state->entries[index].path.c_str());
        explorerReachable = attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
    }

    HMENU menu = CreatePopupMenu();
    UINT itemFlags = MF_STRING | (loading ? MF_GRAYED : 0);
    if (hasPath || loading) {
        AppendMenuW(menu, itemFlags, 5, L"Copy Directory Path");
        AppendMenuW(menu, itemFlags | (explorerReachable ? 0 : MF_GRAYED), 6, L"Open Directory in File Explorer");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    }
    // Reopen As ids: kReopenAsBase + profile slot (0 for the default, 1 + i
    // for profiles[i]).
    const UINT kReopenAsBase = 0x100;
    const std::vector<std::wstring> profiles = OtherClaudeProfiles();
    bool wslEntry = index >= 0 && index < (int)state->entries.size() &&
                    (!state->entries[index].wslDistro.empty() || ParseWslUncPath(state->entries[index].path, nullptr, nullptr));
    if (loading && !profiles.empty()) {
        AppendMenuW(menu, itemFlags, 0, L"Reopen As");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    } else if (hasPath && !wslEntry) {
        HMENU reopenMenu = CreatePopupMenu();
        for (size_t slot = 0; slot <= profiles.size(); slot++) {
            std::wstring profile = slot == 0 ? std::wstring() : profiles[slot - 1];
            if (SameClaudeProfile(profile, state->entries[index].profile)) continue;
            AppendProfileMenuItem(reopenMenu, kReopenAsBase + (UINT)slot, profile);
        }
        if (GetMenuItemCount(reopenMenu) > 0) {
            AppendMenuW(menu, MF_POPUP, (UINT_PTR)reopenMenu, L"Reopen As");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        } else {
            DestroyMenu(reopenMenu);
        }
    }
    AppendMenuW(menu, itemFlags, 1, L"Set Alias...");
    if (hasAlias || loading) AppendMenuW(menu, itemFlags, 2, L"Clear Alias");
    if (hasPath || loading) {
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, itemFlags, alreadyFav ? 4 : 3, alreadyFav ? L"Remove from Favourites" : L"Add to Favourites");
    }
    // Always last, in its own group. Stays enabled while loading: closing
    // doesn't depend on the path/alias that's still settling, and a window
    // stuck loading is exactly one you might want gone.
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 7, L"Close Window");

    RequestForeground(hud, FocusTargetKind::OwnUi,
                      L"item-context-menu"); // required so the menu dismisses correctly on an outside click
    // Deliberately no TPM_RIGHTBUTTON: that flag restricts item *selection*
    // to the right mouse button, but the universal convention (and the only
    // thing a user would naturally try) is right-click to open, then
    // left-click to pick an item -- TPM_RIGHTBUTTON would silently eat that.
    state->contextMenuOpen = true; // see UpdateProjectListHud's guard
    int cmd = TrackPopupMenu(menu, TPM_RETURNCMD, screenPt.x, screenPt.y, 0, hud, nullptr);
    state->contextMenuOpen = false;
    DestroyMenu(menu); // recursively destroys the Reopen As submenu too
    if (cmd >= (int)kReopenAsBase) {
        size_t slot = (size_t)(cmd - (int)kReopenAsBase);
        if (index < 0 || index >= (int)state->entries.size() || slot > profiles.size()) return;
        const ProjectListHudEntry& entry = state->entries[index];
        std::wstring profile = slot == 0 ? std::wstring() : profiles[slot - 1];
        Log(L"hud item menu: [%ls] -> Reopen As [%ls]", entry.label.c_str(), ClaudeProfileName(profile).c_str());
        ReopenWindow w;
        w.target = entry.target;
        w.folder = entry.path;
        w.profile = profile;
        w.label = entry.label;
        CloseAndReopenVSCodeWindows({w}, false, L"hud-item-reopen-as");
        return;
    }
    if (cmd > 0 && index >= 0 && index < (int)state->entries.size()) {
        static const wchar_t* const kActions[] = {L"", L"Set Alias", L"Clear Alias", L"Add to Favourites",
                                                  L"Remove from Favourites", L"Copy Directory Path",
                                                  L"Open Directory in File Explorer", L"Close Window"};
        Log(L"hud item menu: [%ls] -> %ls", state->entries[index].label.c_str(),
            cmd < (int)(sizeof(kActions) / sizeof(kActions[0])) ? kActions[cmd] : L"?");
    }
    if (cmd == 1) BeginAliasEdit(hud, state, index);
    else if (cmd == 2) ClearAlias(hud, state, index);
    else if (cmd == 3) {
        const ProjectListHudEntry& entry = state->entries[index];
        AddFavourite(entry.label, entry.path);
    } else if (cmd == 4) {
        RemoveFavourite(state->entries[index].path);
    } else if (cmd == 5) {
        // A WSL folder as the Linux path it is inside the distro.
        std::wstring linuxPath;
        bool wsl = ParseWslUncPath(state->entries[index].path, nullptr, &linuxPath);
        CopyPathToClipboard(hud, wsl ? linuxPath : state->entries[index].path);
    } else if (cmd == 6) {
        OpenPathInExplorer(state->entries[index].path);
    } else if (cmd == 7 && index >= 0 && index < (int)state->entries.size()) {
        // Same request as the window's own X button -- VS Code still gets
        // to prompt about unsaved changes (see CloseVSCodeWindow).
        HWND target = state->entries[index].target;
        CloseVSCodeWindow(target, L"hud-item-close");
        Log(L"hud item close: sent WM_CLOSE to [%ls]", state->entries[index].label.c_str());
    }
}

// Click on the fixed "+" new-window button. First "Start VS Code": with
// Claude profiles configured besides the default (see claude_profile.h) a
// submenu of which profile to open the new window as -- the default (bolded,
// with its icon if claude_profile_1_icon is set), then the others in slot
// order, each with its icon; without any it's a plain item opening the
// window as the default. Then, after a separator, a "Favourites" submenu
// holding the saved favourite projects (see favourites.h) sorted by label,
// each as its own submenu -- hovering a
// favourite's name expands it to "Open in New Window As" (a submenu of every
// profile it could open as -- see vscode_cli.h's OpenNewVSCodeWindow; on an
// open folder it just re-activates its window, whatever profile that runs;
// a WSL folder lists only the default) and "Remove from Favourites". A plain
// Win32 popup menu item can't both carry its own click action and expand a
// submenu on hover, so nesting "open" one level down (rather than a bare
// top-level click) is what buys room for "remove" to live right next to it,
// without a separate top-level "Remove Favourite" list to keep in sync
// with this one. This is the only way to open (or remove) a favourite that
// isn't currently open as a hub item -- ShowItemContextMenu's own "Remove
// from Favourites" only reaches a favourite while it's a live entry.
static void ShowNewWindowButtonMenu(HWND hud, ProjectListHudState* state, POINT screenPt) {
    std::vector<FavouriteProject> favourites = LoadFavourites();
    const std::vector<std::wstring> profiles = OtherClaudeProfiles();

    std::stable_sort(favourites.begin(), favourites.end(),
                     [](const FavouriteProject& a, const FavouriteProject& b) {
        return CompareStringEx(LOCALE_NAME_USER_DEFAULT, NORM_IGNORECASE | SORT_DIGITSASNUMBERS, a.label.c_str(), -1,
                               b.label.c_str(), -1, nullptr, nullptr, 0) == CSTR_LESS_THAN;
    });

    // Command ids: per favourite i, remove = 1 + i, open as profile slot j =
    // kOpenAsBase + i*kOpenAsStride + j; start as profile slot j =
    // kStartAsBase + j, well above any favourite's -- decoded back on return
    // below. A profile slot is 0 for the default, 1 + i for profiles[i].
    const UINT kOpenAsBase = 0x1000, kOpenAsStride = 8; // stride > the 5 profile slots
    const UINT kStartAsBase = 0x8000;
    auto slotProfile = [&](size_t slot) { return slot == 0 ? std::wstring() : profiles[slot - 1]; };
    HMENU menu = CreatePopupMenu();
    if (profiles.empty()) {
        AppendMenuW(menu, MF_STRING, kStartAsBase, L"Start VS Code");
    } else {
        HMENU startMenu = CreatePopupMenu();
        for (size_t slot = 0; slot <= profiles.size(); slot++) {
            AppendProfileMenuItem(startMenu, kStartAsBase + (UINT)slot, slotProfile(slot));
        }
        SetMenuDefaultItem(startMenu, kStartAsBase, FALSE);
        AppendMenuW(menu, MF_POPUP, (UINT_PTR)startMenu, L"Start VS Code");
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    HMENU favMenu = CreatePopupMenu();
    if (favourites.empty()) {
        AppendMenuW(favMenu, MF_STRING | MF_GRAYED, 0, L"(No Favourites)");
    } else {
        for (size_t i = 0; i < favourites.size(); i++) {
            UINT removeId = (UINT)(1 + i);
            HMENU sub = CreatePopupMenu();
            // A WSL folder only opens as the default.
            HMENU asMenu = CreatePopupMenu();
            size_t lastSlot = ParseWslUncPath(favourites[i].path, nullptr, nullptr) ? 0 : profiles.size();
            for (size_t slot = 0; slot <= lastSlot; slot++) {
                AppendProfileMenuItem(asMenu, kOpenAsBase + (UINT)i * kOpenAsStride + (UINT)slot, slotProfile(slot));
            }
            AppendMenuW(sub, MF_POPUP, (UINT_PTR)asMenu, L"Open in New Window As");
            AppendMenuW(sub, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(sub, MF_STRING, removeId, L"Remove from Favourites");
            AppendMenuW(favMenu, MF_POPUP, (UINT_PTR)sub, favourites[i].label.c_str());
        }
    }
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)favMenu, L"Favourites");

    RequestForeground(hud, FocusTargetKind::OwnUi, L"new-window-button-menu");
    state->contextMenuOpen = true; // see UpdateProjectListHud's guard
    int cmd = TrackPopupMenu(menu, TPM_RETURNCMD, screenPt.x, screenPt.y, 0, hud, nullptr);
    state->contextMenuOpen = false;
    DestroyMenu(menu); // recursively destroys the Start VS Code, Favourites, per-favourite and open-as submenus too

    if (cmd <= 0) return;
    if (cmd >= (int)kStartAsBase) {
        size_t slot = (size_t)(cmd - (int)kStartAsBase);
        if (slot <= profiles.size()) OpenNewVSCodeWindow(AnyTrackedWindow(state), L"", L"hud-new-window-button", slotProfile(slot));
        return;
    }
    if (cmd >= (int)kOpenAsBase) {
        size_t i = (size_t)(cmd - (int)kOpenAsBase) / kOpenAsStride;
        size_t slot = (size_t)(cmd - (int)kOpenAsBase) % kOpenAsStride;
        if (i >= favourites.size() || slot > profiles.size()) return;
        OpenNewVSCodeWindow(AnyTrackedWindow(state), favourites[i].path, L"hud-favourite-open-as", slotProfile(slot));
        return;
    }
    size_t i = (size_t)(cmd - 1);
    if (i < favourites.size()) RemoveFavourite(favourites[i].path);
}

// Shared cleanup for ending a DragReorder, called both from a real
// WM_LBUTTONUP and from WM_MOUSEMOVE's defensive check for a left button
// that's no longer physically down (see that check's comment).
static void EndReorderDrag(HWND hwnd, ProjectListHudState* state, int mouseX, int mouseY) {
    state->dragMode = ProjectListHudState::DragNone;
    state->pendingDragIndex = -1;
    state->reorderIndex = -1;
    if (GetCapture() == hwnd) ReleaseCapture();
    std::vector<std::wstring> order;
    order.reserve(state->entries.size());
    for (const ProjectListHudEntry& e : state->entries) order.push_back(e.aliasKey);
    state->manualOrder = order;
    if (state->dragGhost) ShowWindow(state->dragGhost, SW_HIDE);
    RenderProjectListHud(hwnd, state);
    SetCursor(LoadCursorW(nullptr, ProjectListCursorForPoint(state, mouseX, mouseY)));
}

// Shared cleanup for ending a DragMove/DragResizeLeft/DragResizeRight, same
// two call sites as EndReorderDrag above.
static void EndMoveResizeDrag(HWND hwnd, ProjectListHudState* state, int mouseX, int mouseY) {
    state->dragMode = ProjectListHudState::DragNone;
    if (GetCapture() == hwnd) ReleaseCapture();
    if (state->manualPosition) SaveHudPlacement(PlacementKey(state), state->x, state->manualItemWidth);
    SetCursor(LoadCursorW(nullptr, ProjectListCursorForPoint(state, mouseX, mouseY)));
}

// Shows the hovered usage ring group's tooltip just above it and keeps its
// text current; hides it once no ring group is hovered.
static void SyncUsageTooltip(HWND hud, ProjectListHudState* state) {
    if (!state || !state->usageTooltip) return;
    int ring = state->usageStrip && IsWindowVisible(state->usageStrip) ? state->usageHoverRing : -1;
    if (ring < 0 || ring >= (int)state->usageRings.size()) {
        if (state->usageTooltipRing >= 0) ShowWindow(state->usageTooltip, SW_HIDE);
        state->usageTooltipRing = -1;
        state->usageTooltipText.clear();
        return;
    }
    const std::wstring& text = state->usageRings[ring].tooltip;
    if (ring == state->usageTooltipRing && text == state->usageTooltipText) return;
    state->usageTooltipText = text;

    if (!state->usageTooltipFont || state->usageTooltipFontSize != state->fontSize) {
        if (state->usageTooltipFont) DeleteObject(state->usageTooltipFont);
        state->usageTooltipFontSize = state->fontSize;
        state->usageTooltipFont = CreateHudFont(state->fontSize, FW_NORMAL);
    }
    HDC dc = GetDC(nullptr);
    HFONT oldFont = (HFONT)SelectObject(dc, state->usageTooltipFont);
    RECT textRect = {0, 0, 0, 0};
    DrawTextW(dc, text.c_str(), (int)text.size(), &textRect, DT_CALCRECT | DT_NOPREFIX);
    SelectObject(dc, oldFont);
    ReleaseDC(nullptr, dc);
    int w = textRect.right + 2 * kUsageTooltipPadX, h = textRect.bottom + 2 * kUsageTooltipPadY;
    RECT strip;
    GetWindowRect(state->usageStrip, &strip);
    const RECT& r = state->usageRingRects[ring];
    int x = std::max((int)state->dockRect.left, std::min((int)strip.left + (int)r.left, (int)state->dockRect.right - w));
    int y = strip.top - h - 4;
    SetWindowPos(state->usageTooltip, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(state->usageTooltip, nullptr, TRUE);
    state->usageTooltipRing = ring;
}

// The usage tooltip popup: GWLP_USERDATA is the HUD's state, whose
// usageTooltipText/usageTooltipFont it paints. Never takes the mouse.
static LRESULT CALLBACK UsageTooltipWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_NCHITTEST:
            return HTTRANSPARENT;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            const ProjectListHudState* state = (const ProjectListHudState*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
            RECT rc;
            GetClientRect(hwnd, &rc);
            HBRUSH bg = CreateSolidBrush(kUsageTooltipBackColor), border = CreateSolidBrush(kUsageTooltipBorderColor);
            FillRect(dc, &rc, bg);
            FrameRect(dc, &rc, border);
            DeleteObject(bg);
            DeleteObject(border);
            if (state && state->usageTooltipFont) {
                HFONT oldFont = (HFONT)SelectObject(dc, state->usageTooltipFont);
                SetBkMode(dc, TRANSPARENT);
                SetTextColor(dc, kUsageTooltipTextColor);
                RECT textRect = {kUsageTooltipPadX, kUsageTooltipPadY, rc.right - kUsageTooltipPadX,
                                 rc.bottom - kUsageTooltipPadY};
                DrawTextW(dc, state->usageTooltipText.c_str(), (int)state->usageTooltipText.size(), &textRect,
                          DT_NOPREFIX);
                SelectObject(dc, oldFont);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// The usage rings' strip: GWLP_USERDATA is the HUD's state. Hovering a ring
// group shows its tooltip; clicks do nothing.
static LRESULT CALLBACK UsageStripWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    ProjectListHudState* state = (ProjectListHudState*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
        case WM_MOUSEMOVE: {
            if (!state) break;
            int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam), hit = -1;
            for (size_t j = 0; j < state->usageRingRects.size(); j++) {
                const RECT& r = state->usageRingRects[j];
                if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) hit = (int)j;
            }
            if (hit != state->usageHoverRing) {
                state->usageHoverRing = hit;
                SyncUsageTooltip(hwnd, state);
            }
            if (!state->usageTrackingMouseLeave) {
                TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, hwnd, 0};
                state->usageTrackingMouseLeave = TrackMouseEvent(&tme) != 0;
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            if (!state) break;
            state->usageTrackingMouseLeave = false;
            state->usageHoverRing = -1;
            SyncUsageTooltip(hwnd, state);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static LRESULT CALLBACK ProjectListHudWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    ProjectListHudState* state = (ProjectListHudState*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    // Explorer restarted (crash, or killed by hand): every AppBar
    // registration died with it, so register again and reclaim the band.
    static const UINT taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");
    if (msg == taskbarCreatedMsg && state && state->docked) {
        RegisterAppBar(hwnd);
        RefreshDockRect(hwnd, state);
        RelayoutDocked(hwnd, state);
        return 0;
    }

    switch (msg) {
        case WM_MOUSEMOVE: {
            if (!state) break;
            int mouseX = GET_X_LPARAM(lParam);
            int mouseY = GET_Y_LPARAM(lParam);

            // Some touchpads' "tap twice and drag" gesture leaves the
            // logical left button down (continuing to feed mouse-move
            // messages to whoever holds capture) past the finger's actual
            // lift, only truly releasing on a later confirming tap -- so a
            // drag started that way never got a WM_LBUTTONUP and looked
            // stuck until the user clicked again. Checking the *real*
            // hardware button state here ends the drag as soon as it's
            // actually released, regardless of whether a matching
            // WM_LBUTTONUP message ever shows up.
            if (state->dragMode != ProjectListHudState::DragNone && !(GetAsyncKeyState(VK_LBUTTON) & 0x8000)) {
                if (state->dragMode == ProjectListHudState::DragReorder) {
                    EndReorderDrag(hwnd, state, mouseX, mouseY);
                } else {
                    EndMoveResizeDrag(hwnd, state, mouseX, mouseY);
                }
                return 0;
            }

            // Promote a pending plain-left-click on an item into a
            // reorder-drag once the cursor has moved far enough to
            // disambiguate it from a plain click (which activates the
            // window on release instead -- see WM_LBUTTONUP).
            if (state->dragMode == ProjectListHudState::DragNone &&
                state->pendingDragIndex >= 0) {
                POINT pt;
                GetCursorPos(&pt);
                if (abs(pt.x - state->dragStart.x) >= kReorderDragThreshold ||
                    abs(pt.y - state->dragStart.y) >= kReorderDragThreshold) {
                    state->dragMode = ProjectListHudState::DragReorder;
                    state->reorderIndex = state->pendingDragIndex;
                    const RECT& item = state->itemRects[state->reorderIndex];
                    state->reorderGrabOffset = {mouseX - item.left, mouseY - item.top};
                    state->reorderCursorClient = {mouseX, mouseY};
                    SetCapture(hwnd);
                    RenderProjectListHud(hwnd, state); // show the now-empty original slot immediately
                    PaintDragGhost(state); // paints + positions the floating item for the first time
                }
            }

            if (state->dragMode == ProjectListHudState::DragReorder) {
                state->reorderCursorClient = {mouseX, mouseY};
                int candidate = ProjectListDragCandidate(state, mouseX);
                if (candidate >= 0 && candidate < (int)state->entries.size() && candidate != state->reorderIndex) {
                    ProjectListHudEntry dragged = state->entries[state->reorderIndex];
                    state->entries.erase(state->entries.begin() + state->reorderIndex);
                    state->entries.insert(state->entries.begin() + candidate, dragged);
                    state->reorderIndex = candidate;
                    RebuildItemRects(state);
                    RenderProjectListHud(hwnd, state); // only repaint the (expensive, full-bitmap) main
                                                        // grid when the arrangement actually changed
                }
                // Cheap every frame: just repositions the already-painted
                // ghost (see MoveDragGhost/PaintDragGhost's comments for
                // why repainting on every mouse-move visibly lagged behind
                // fast cursor movement).
                MoveDragGhost(state);
                SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
                return 0;
            }

            if (state->dragMode != ProjectListHudState::DragNone) {
                // Pinned to the band: sideways only, never out of it.
                POINT pt;
                GetCursorPos(&pt);
                int dx = pt.x - state->dragStart.x;
                int left = state->dragStartRect.left;
                int right = state->dragStartRect.right;
                const RECT& d = state->dockRect;
                state->manualPosition = true;
                if (state->dragMode == ProjectListHudState::DragMove) {
                    int w = right - left;
                    left = ClampInt(left + dx, d.left, HudRightLimit(state) - w);
                    right = left + w;
                } else {
                    if (state->dragMode == ProjectListHudState::DragResizeLeft) {
                        left = std::max(std::min(left + dx, right - kProjectListMinWidth), (int)d.left);
                    } else {
                        right = std::min(std::max(right + dx, left + kProjectListMinWidth), HudRightLimit(state));
                    }
                    state->manualWidth = true;
                }

                int newWidth = right - left;
                bool sizeChanged = newWidth != state->width;
                state->x = left;
                state->width = newWidth;
                // Cache what per-item width this drag corresponds to *right
                // now* (entries.size() is fixed for the duration of a single
                // drag) -- this, not state->width, is what gets remembered on
                // WM_LBUTTONUP.
                size_t n = state->entries.size();
                int buttonReserve = ActionButtonsReserve(state);
                state->manualItemWidth =
                    n > 0 ? std::max(1, (newWidth - buttonReserve - (int)(n - 1) * kProjectListGap) / (int)n)
                          : newWidth;
                if (sizeChanged) {
                    RebuildItemRects(state);
                    RenderProjectListHud(hwnd, state);
                }
                PositionProjectListHud(hwnd, state);
                SetCursor(LoadCursorW(nullptr, state->dragMode == ProjectListHudState::DragMove ? IDC_SIZEALL : IDC_SIZEWE));
                return 0;
            }

            // Re-evaluated on every move rather than once on entry: the
            // flyout can open or close while the cursor sits on the HUD.
            // Cheap -- one GetForegroundWindow and one GetClassNameW.
            bool flyoutUp = ForegroundIsTaskbarFlyout();
            SetHudClickActivates(hwnd, state, flyoutUp);

            int hoverIndex = ProjectListHitTest(state, mouseX, mouseY);
            if (hoverIndex != state->hoverIndex) {
                state->hoverIndex = hoverIndex;
                RenderProjectListHud(hwnd, state);
                // The highest-risk activation path in the app: one mouse
                // sweep across the strip crosses every item and asks to
                // activate each in turn, which is exactly the shape that
                // leaves a row of VS Code windows highlighted in the
                // taskbar (see focus_trace.h). Tagged distinctly so the log
                // shows that immediately.
                //
                // Not attempted at all while a taskbar flyout is up: hover
                // has no click to dismiss it with, so every one of these
                // would be refused (see ForegroundIsTaskbarFlyout) -- a
                // sweep would just be a burst of denials. Clicking still
                // works, via SetHudClickActivates.
                if (state->activateOnHover && hoverIndex >= 0) {
                    if (flyoutUp) Log(L"hud-hover: skipped index=%d -- a taskbar flyout holds the foreground", hoverIndex);
                    else ActivateProjectListItem(state, hoverIndex, L"hud-hover");
                }
            }
            SetCursor(LoadCursorW(nullptr, ProjectListCursorForPoint(state, mouseX, mouseY)));
            if (!state->trackingMouseLeave) {
                TRACKMOUSEEVENT tme = {};
                tme.cbSize = sizeof(tme);
                tme.dwFlags = TME_LEAVE;
                tme.hwndTrack = hwnd;
                state->trackingMouseLeave = TrackMouseEvent(&tme) != 0;
            }
            return 0;
        }
        case WM_LBUTTONDOWN:
            // Plain left-click activates on release (see WM_LBUTTONUP)
            // without needing anything at button-down time, unless it turns
            // into a reorder-drag once the cursor moves far enough (see
            // WM_MOUSEMOVE's promotion check) -- so for now just remember
            // which item (if any) was under the cursor, and where, without
            // committing to anything yet. Ctrl+left-click instead starts a
            // slide/resize drag immediately (plain right-click being the
            // context menu, see WM_RBUTTONUP).
            if (state && (GetKeyState(VK_CONTROL) & 0x8000)) {
                state->dragMode = ProjectListDragModeForPoint(state, GET_X_LPARAM(lParam));
                GetCursorPos(&state->dragStart);
                GetWindowRect(hwnd, &state->dragStartRect);
                SetCapture(hwnd);
                SetCursor(LoadCursorW(nullptr, state->dragMode == ProjectListHudState::DragMove ? IDC_SIZEALL : IDC_SIZEWE));
            } else if (state) {
                // The action buttons (hit-tested as sentinel indices from
                // entries.size() up, see ProjectListHitTest) are deliberately
                // excluded here: they're fixed at the end and never
                // reorder-draggable, so they need no pending-drag tracking --
                // their clicks are handled entirely on WM_LBUTTONUP via
                // hoverIndex, same as a plain item click.
                int index = ProjectListHitTest(state, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
                // A loading entry's rawLabel may still change (see
                // ProjectListHudEntry::loading) -- not draggable into a
                // reorder that a later relabel would silently orphan.
                // Plain click-to-activate on release still works either way,
                // since that only needs hoverIndex, not pendingDragIndex.
                if (index >= 0 && index < (int)state->entries.size() && !state->entries[index].loading) {
                    state->pendingDragIndex = index;
                    GetCursorPos(&state->dragStart);
                }
            }
            return 0;
        case WM_LBUTTONUP:
            // The activation SetHudClickActivates allowed has already
            // happened, at button-down -- nothing left for the style to do.
            SetHudClickActivates(hwnd, state, false);
            if (state && state->dragMode == ProjectListHudState::DragReorder) {
                EndReorderDrag(hwnd, state, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
                return 0;
            }
            if (state && state->dragMode != ProjectListHudState::DragNone) {
                EndMoveResizeDrag(hwnd, state, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
                return 0;
            }
            if (state) state->pendingDragIndex = -1;
            if (state && state->hoverIndex >= 0 && state->hoverIndex < (int)state->entries.size()) {
                ActivateProjectListItem(state, state->hoverIndex, L"hud-click");
                EndProjectListHoverFocus(state, false);
            } else if (state) {
                switch (ActionButtonForIndex(state, state->hoverIndex)) {
                    case HudActionNewWindow: {
                        POINT screenPt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                        ClientToScreen(hwnd, &screenPt);
                        ShowNewWindowButtonMenu(hwnd, state, screenPt);
                        break;
                    }
                    case HudActionMinimizeAll:
                        EndProjectListHoverFocus(state, false);
                        MinimizeAllVSCodeWindows(state);
                        break;
                    case HudActionCloseAll:
                        EndProjectListHoverFocus(state, false);
                        CloseAllVSCodeWindows(hwnd, state);
                        break;
                }
            }
            return 0;
        case WM_RBUTTONUP:
            if (state) {
                int index = ProjectListHitTest(state, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
                if (index >= 0 && index < (int)state->entries.size()) {
                    POINT screenPt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                    ClientToScreen(hwnd, &screenPt);
                    ShowItemContextMenu(hwnd, state, index, screenPt);
                }
            }
            return 0;
        case WM_DISPLAYCHANGE:
            if (!state) return 0;
            ApplyScenarioForCurrentMonitors(state);
            if (state->docked) {
                // The shell also sends ABN_POSCHANGED for this, but the
                // band's monitor may have changed size under us either way.
                RefreshDockRect(hwnd, state);
                RelayoutDocked(hwnd, state);
            }
            return 0;
        case WM_TIMER:
            if (wParam == kClaudePulseTimerId && state) {
                RenderProjectListHud(hwnd, state);
                bool ringPulsing = std::any_of(state->usageRings.begin(), state->usageRings.end(),
                                               [](const ProjectListHudUsageRing& r) { return r.unavailable; });
                if (ringPulsing) SyncUsageStrip(hwnd, state);
            }
            return 0;
        case WM_MOUSELEAVE:
            SetHudClickActivates(hwnd, state, false);
            if (state) {
                if (state->dragMode != ProjectListHudState::DragNone) return 0;
                state->trackingMouseLeave = false;
                if (state->hoverIndex != -1) {
                    state->hoverIndex = -1;
                    RenderProjectListHud(hwnd, state);
                }
                EndProjectListHoverFocus(state, true);
            }
            return 0;
        case WM_SETCURSOR:
            if (state && LOWORD(lParam) == HTCLIENT) {
                POINT pt;
                GetCursorPos(&pt);
                ScreenToClient(hwnd, &pt);
                SetCursor(LoadCursorW(nullptr, ProjectListCursorForPoint(state, pt.x, pt.y)));
                return TRUE;
            }
            break;
        case WM_CAPTURECHANGED:
            // If this interrupts an in-progress reorder-drag (e.g. another
            // app steals capture), state->entries is left in whatever
            // mid-drag arrangement it had reached -- not yet in manualOrder,
            // so the next regular sync reapplies the last dropped order and
            // self-corrects the visual back to it.
            if (state) {
                state->dragMode = ProjectListHudState::DragNone;
                state->pendingDragIndex = -1;
                state->reorderIndex = -1;
                if (state->dragGhost) ShowWindow(state->dragGhost, SW_HIDE);
            }
            return 0;
        case kAppBarCallbackMsg:
            if (!state || !state->docked) return 0;
            if (wParam == ABN_POSCHANGED) {
                // Taskbar moved/resized/auto-hide toggled, or another
                // AppBar came or went -- renegotiate the band.
                RefreshDockRect(hwnd, state);
                RelayoutDocked(hwnd, state);
            } else if (wParam == ABN_FULLSCREENAPP) {
                bool open = lParam != 0;
                // The shell also reports clicking the bare desktop as a
                // fullscreen app opening (the desktop window covers the
                // whole monitor) -- hiding the HUD for that would make it
                // vanish every time the user clicks their wallpaper.
                if (open) {
                    wchar_t cls[64] = {};
                    GetClassNameW(GetForegroundWindow(), cls, 64);
                    if (wcscmp(cls, L"Progman") == 0 || wcscmp(cls, L"WorkerW") == 0) return 0;
                }
                if (open == state->fullscreenAppOpen) return 0;
                state->fullscreenAppOpen = open;
                Log(L"hud: fullscreen app %ls -- docked HUD %ls", open ? L"opened" : L"closed",
                    open ? L"hidden" : L"restored");
                SyncBackdrop(hwnd, state);
                if (open) {
                    state->visibleBeforeFullscreen = IsWindowVisible(hwnd) != FALSE;
                    ShowWindow(hwnd, SW_HIDE);
                    SyncUsageStrip(hwnd, state);
                } else if (state->visibleBeforeFullscreen && !state->entries.empty()) {
                    FitIntoDock(state);
                    RenderProjectListHud(hwnd, state);
                    PositionProjectListHud(hwnd, state);
                }
            }
            return 0;
        case WM_ACTIVATE:
            // Part of the AppBar protocol. (ABM_WINDOWPOSCHANGED, its
            // WM_WINDOWPOSCHANGED counterpart, is deliberately not sent:
            // it only matters for auto-hide AppBars, and this window
            // re-asserts its z-order via SetWindowPos on every sync and
            // every frame of a drag -- a cross-process call to Explorer
            // each time, for nothing.)
            if (state && state->docked) {
                APPBARDATA abd = {};
                abd.cbSize = sizeof(abd);
                abd.hWnd = hwnd;
                SHAppBarMessage(ABM_ACTIVATE, &abd);
            }
            break;
        case WM_DESTROY:
            // A still-registered AppBar would leave its band reserved --
            // an empty strip that maximized windows keep stopping above.
            if (state && state->docked) UnregisterAppBar(hwnd);
            break;
        case WM_NCDESTROY:
            if (state && state->dragGhost) DestroyWindow(state->dragGhost);
            if (state && state->usageTooltip) DestroyWindow(state->usageTooltip);
            if (state && state->usageStrip) DestroyWindow(state->usageStrip);
            if (state && state->usageTooltipFont) DeleteObject(state->usageTooltipFont);
            if (state && state->backdrop) DestroyWindow(state->backdrop);
            delete state;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            break;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void RegisterProjectListHudClass(HINSTANCE hInstance) {
    static bool registered = false;
    if (registered) return;

    WNDCLASSW wc = {};
    wc.lpfnWndProc = ProjectListHudWndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = kProjectListHudClassName;
    RegisterClassW(&wc);
    registered = true;
}

// The drag-ghost window is purely decorative (PaintDragGhost/MoveDragGhost
// paint and reposition it, nothing ever needs to handle input on it --
// mouse capture stays on the main HUD window for the whole drag, see
// WM_MOUSEMOVE), so DefWindowProcW
// is all it needs.
static const wchar_t* kProjectListGhostClassName = L"VSCodeBorderProjectListGhostWndClass";

static void RegisterProjectListGhostClass(HINSTANCE hInstance) {
    static bool registered = false;
    if (registered) return;

    WNDCLASSW wc = {};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = hInstance;
    wc.lpszClassName = kProjectListGhostClassName;
    RegisterClassW(&wc);
    registered = true;
}

HWND CreateProjectListHud(HINSTANCE hInstance) {
    RegisterProjectListHudClass(hInstance);
    HWND hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kProjectListHudClassName, L"", WS_POPUP,
        0, 0, 1, 1, nullptr, nullptr, hInstance, nullptr);
    if (!hwnd) return nullptr;
    ProjectListHudState* state = new ProjectListHudState();
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)state);
    // Establishes the initial scenarioKey and, if this monitor scenario has
    // a remembered placement, pre-sets manualPosition/manualWidth from it
    // so the very first UpdateProjectListHud call (once real entries show
    // up) uses it instead of auto-anchoring.
    ApplyScenarioForCurrentMonitors(state);

    // Created hidden up front (rather than lazily per-drag) to keep drag
    // start/end cheap -- just shown/positioned/painted for the duration of
    // a DragReorder. WS_EX_TRANSPARENT since it's purely visual: mouse
    // capture stays on `hwnd` for the whole drag regardless of what's on
    // top of it on screen. Deliberately NOT owned by `hwnd` (nullptr parent,
    // not hwnd): Windows couples an owned window's z-order to its owner in
    // a way that silently overrode this window's own HWND_TOPMOST requests
    // -- SetWindowPos kept reporting success and landing at the requested
    // rect, but WindowFromPoint at that exact rect still resolved to the
    // (owner) HUD window, i.e. the HUD was painting over this one despite
    // "winning" every individual z-order call. A fully independent
    // top-level window has no such coupling.
    RegisterProjectListGhostClass(hInstance);
    state->backdrop = CreateTaskbarBackdrop(hInstance);
    state->dragGhost = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kProjectListGhostClassName, L"", WS_POPUP,
        0, 0, 1, 1, nullptr, nullptr, hInstance, nullptr);

    // Drawn by hand rather than with a common-controls tooltip: without a
    // manifest this exe gets comctl32 5.82, whose tooltip crashed the app
    // here. Not owned by `hwnd`, for the same reason as the drag ghost.
    WNDCLASSW tipClass = {};
    tipClass.lpfnWndProc = UsageTooltipWndProc;
    tipClass.hInstance = hInstance;
    tipClass.lpszClassName = kUsageTooltipClassName;
    RegisterClassW(&tipClass);
    state->usageTooltip = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
                                          kUsageTooltipClassName, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr,
                                          hInstance, nullptr);
    if (state->usageTooltip) {
        SetWindowLongPtrW(state->usageTooltip, GWLP_USERDATA, (LONG_PTR)state);
    } else {
        LogWarn(L"hud: couldn't create the usage rings' tooltip, lastError=%lu", GetLastError());
    }

    // Its own window rather than part of the HUD, so it stays at the band's
    // right end wherever the HUD is slid to.
    WNDCLASSW stripClass = {};
    stripClass.lpfnWndProc = UsageStripWndProc;
    stripClass.hInstance = hInstance;
    stripClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    stripClass.lpszClassName = kUsageStripClassName;
    RegisterClassW(&stripClass);
    state->usageStrip = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                        kUsageStripClassName, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, hInstance,
                                        nullptr);
    if (state->usageStrip) {
        SetWindowLongPtrW(state->usageStrip, GWLP_USERDATA, (LONG_PTR)state);
    } else {
        LogWarn(L"hud: couldn't create the usage rings' window, lastError=%lu", GetLastError());
    }

    return hwnd;
}

// Fills a 5-point star, centered at (cx, cy) with the given outer radius,
// into `pixels` -- rasterized via ray-casting point-in-polygon against the
// star's 10 vertices (5 outer, 5 inner, alternating), rather than drawn as
// a text glyph. Raw GDI's Segoe UI -- the exact font/params DrawHudItem
// uses, via BlendTextIntoPixels's DrawTextW -- has no star glyph at all
// (verified with GetGlyphIndicesW against U+2605/2606/2B50, all reported
// missing); DrawTextW doesn't do the Uniscribe-style font-fallback that'd
// otherwise paper over that, so it would render as a tofu box. Hand
// rasterizing sidesteps needing any particular font to have the glyph.
static void FillStar(UINT32* pixels, int width, int height, int cx, int cy, int outerRadius, UINT32 colorPx) {
    const double kPi = 3.14159265358979323846;
    double vx[10], vy[10];
    for (int i = 0; i < 10; i++) {
        double angle = -kPi / 2 + i * kPi / 5; // vertex 0 points straight up
        double r = (i % 2 == 0) ? outerRadius : outerRadius * 0.42; // inner points ~42% of outer
        vx[i] = cx + r * cos(angle);
        vy[i] = cy + r * sin(angle);
    }
    int top = std::max(0, cy - outerRadius), bottom = std::min(height, cy + outerRadius + 1);
    int left = std::max(0, cx - outerRadius), right = std::min(width, cx + outerRadius + 1);
    for (int py = top; py < bottom; py++) {
        double sy = py + 0.5;
        for (int px = left; px < right; px++) {
            double sx = px + 0.5;
            bool inside = false;
            for (int i = 0, j = 9; i < 10; j = i++) {
                if (((vy[i] > sy) != (vy[j] > sy)) &&
                    (sx < (vx[j] - vx[i]) * (sy - vy[i]) / (vy[j] - vy[i]) + vx[i])) {
                    inside = !inside;
                }
            }
            if (inside) pixels[py * width + px] = colorPx;
        }
    }
}

// Left margin (from item.left) reserved for the favourites star marker,
// including the gap before the label text -- see DrawFavouriteMarker.
static const int kFavouriteMarkerLeftPad = 19;

// Draws the favourites star marker at an item's left edge, vertically
// centered -- a filled gold star with a 1px auto-contrast border (same
// ContrastTextColor logic as the Claude status indicator's border, since
// the chip's background color is user-configurable and a fixed fill can
// end up low-contrast against it otherwise).
static void DrawFavouriteMarker(UINT32* pixels, int width, int height, const RECT& item, COLORREF chipBaseColor) {
    const int kMargin = 3, kBorderRadius = 6, kFillRadius = 5;
    int cx = (int)item.left + kMargin + kBorderRadius;
    int cy = (int)(item.top + item.bottom) / 2;
    COLORREF borderColor = ContrastTextColor(chipBaseColor);
    UINT32 borderPx = (UINT32(255) << 24) | (UINT32(GetRValue(borderColor)) << 16) |
                      (UINT32(GetGValue(borderColor)) << 8) | UINT32(GetBValue(borderColor));
    const UINT32 kFillColorPx = (UINT32(255) << 24) | (UINT32(255) << 16) | (UINT32(199) << 8) | UINT32(44); // gold
    FillStar(pixels, width, height, cx, cy, kBorderRadius, borderPx);
    FillStar(pixels, width, height, cx, cy, kFillRadius, kFillColorPx);
}

// How much further right an entry's label starts, to make room for the
// icons in front of it: WSL (see ProjectListHudEntry::wslDistro), then the
// profile's (ProjectListHudEntry::profile).
static int WslIconTextShift(const ProjectListHudEntry& entry) {
    return entry.wslDistro.empty() ? 0 : kWslIconSize + kWslIconGap;
}
static int LabelIconsTextShift(const ProjectListHudEntry& entry) {
    return WslIconTextShift(entry) + (entry.showProfileIcon ? kProfileIconSize + kProfileIconGap : 0);
}

// Draws one entry's chip (background + label text, alpha-blended by
// `opacity`) into `pixels` at `item`. `item` may fall partly or entirely
// outside [0,width)x[0,height) -- used for the floating dragged item
// during DragReorder, which follows the cursor freely -- so bounds are
// clamped explicitly (BlendTextIntoPixels already does its own clamping
// internally).
static void DrawHudItem(HDC screenDC, UINT32* pixels, int width, int height, int rowHeight,
                        const ProjectListHudEntry& entry, const RECT& item, bool highlighted, int opacity,
                        bool isFavourite, bool labelTextColorAuto, COLORREF labelTextColor, HFONT font,
                        COLORREF claudeColorWorking, COLORREF claudeColorAttention,
                        COLORREF claudeColorWaiting, bool claudeBorderColorAuto, COLORREF claudeBorderColor) {
    const int paddingX = 12;
    // A loading item's star (if it's a favourite) is suppressed below --
    // its folder path may still change, so whether it's a favourite isn't
    // settled yet.
    bool showFavouriteMarker = isFavourite && !entry.loading;
    int leftTextPad = showFavouriteMarker ? kFavouriteMarkerLeftPad : paddingX;
    int itemWidth = item.right - item.left;
    COLORREF color = entry.color;
    BYTE r = GetRValue(color), g = GetGValue(color), b = GetBValue(color);
    if (highlighted) {
        r = (BYTE)std::min(255, (int)r + 32);
        g = (BYTE)std::min(255, (int)g + 32);
        b = (BYTE)std::min(255, (int)b + 32);
    }
    COLORREF chipColor = RGB(r, g, b);
    UINT32 chipPx = (UINT32(255) << 24) | (UINT32(r) << 16) | (UINT32(g) << 8) | UINT32(b);

    // Loading fill: the item's own color on the left fading, on a 115deg
    // diagonal, into grey on the right (see the reddit-ads preview's
    // "Variant B") -- computed once per item as a projection onto the
    // gradient direction, normalized so 0.0/1.0 land exactly on the box's
    // two extreme corners along that direction (the same convention CSS's
    // linear-gradient uses), then reused below both to fill the chip and
    // to color the spinner dots' local background so they blend correctly
    // wherever they land on the gradient.
    const double kPi = 3.14159265358979323846;
    const double kLoadingAngleRad = 115.0 * kPi / 180.0;
    const double kLoadingGradDx = sin(kLoadingAngleRad), kLoadingGradDy = -cos(kLoadingAngleRad);
    const double kLoadingBandStart = 0.30, kLoadingBandEnd = 0.70;
    BYTE loadingGrey = (BYTE)(highlighted ? 160 : 128);
    double loadingCorners[4][2] = {
        {0.0, 0.0}, {(double)itemWidth, 0.0}, {0.0, (double)(item.bottom - item.top)},
        {(double)itemWidth, (double)(item.bottom - item.top)},
    };
    double loadingMinProj = 1e18, loadingMaxProj = -1e18;
    for (auto& c : loadingCorners) {
        double p = c[0] * kLoadingGradDx + c[1] * kLoadingGradDy;
        loadingMinProj = std::min(loadingMinProj, p);
        loadingMaxProj = std::max(loadingMaxProj, p);
    }
    double loadingSpan = loadingMaxProj - loadingMinProj;
    auto LoadingColorAt = [&](int absCol, int absRow) -> UINT32 {
        double px = absCol - item.left, py = absRow - item.top;
        double proj = px * kLoadingGradDx + py * kLoadingGradDy;
        double t = loadingSpan > 1e-6 ? (proj - loadingMinProj) / loadingSpan : 0.0;
        t = std::max(0.0, std::min(1.0, t));
        BYTE fr, fg, fb;
        if (t <= kLoadingBandStart) {
            fr = r; fg = g; fb = b;
        } else if (t >= kLoadingBandEnd) {
            fr = fg = fb = loadingGrey;
        } else {
            double lt = (t - kLoadingBandStart) / (kLoadingBandEnd - kLoadingBandStart);
            fr = (BYTE)(r + (loadingGrey - (int)r) * lt);
            fg = (BYTE)(g + (loadingGrey - (int)g) * lt);
            fb = (BYTE)(b + (loadingGrey - (int)b) * lt);
        }
        return (UINT32(fr) << 16) | (UINT32(fg) << 8) | UINT32(fb);
    };

    int rowStart = std::max((int)item.top, 0), rowEnd = std::min((int)item.bottom, height);
    int colStart = std::max((int)item.left, 0), colEnd = std::min((int)item.right, width);
    for (int row = rowStart; row < rowEnd; row++) {
        for (int col = colStart; col < colEnd; col++) {
            pixels[row * width + col] = entry.loading ? (0xFF000000u | LoadingColorAt(col, row)) : chipPx;
        }
    }

    // Favourites star marker, left edge -- drawn before the opacity pass
    // below so it fades with the rest of the chip on hover/normal opacity,
    // same reasoning as the Claude status indicator's placement.
    if (showFavouriteMarker) DrawFavouriteMarker(pixels, width, height, item, color);
    // A WSLg entry's WSL icon: where the label would otherwise start,
    // after the star on a favourite, with the label moved over to make room.
    if (!entry.wslDistro.empty()) {
        BlendWslIcon(pixels, width, height, (int)item.left + leftTextPad, (int)item.top + (rowHeight - kWslIconSize) / 2,
                     item);
        leftTextPad += WslIconTextShift(entry);
    }
    if (entry.showProfileIcon) {
        BlendClaudeProfileIcon(entry.profile, pixels, width, height, (int)item.left + leftTextPad,
                               (int)item.top + (rowHeight - kProfileIconSize) / 2, item);
        leftTextPad += kProfileIconSize + kProfileIconGap;
    }

    COLORREF tx = labelTextColorAuto ? ContrastTextColor(color) : labelTextColor;
    BlendTextIntoPixels(screenDC, pixels, width, height, item.left + leftTextPad, item.top,
                        itemWidth - leftTextPad - paddingX, rowHeight, entry.label, font,
                        chipColor, tx, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // Claude Code status indicator, top-right corner -- drawn before the
    // opacity pass below so it fades with the rest of the chip on
    // hover/normal opacity instead of always looking fully solid.
    const int dotMargin = 4;
    // The chip's background color is user-configurable (see config.ini's
    // `colors`), so a fixed red/green/amber fill can end up low-contrast
    // against it -- a 1px border, auto-picked black or white by the same
    // luminance check already used for the label text (ContrastTextColor),
    // keeps every indicator legible regardless of that item's own color.
    // The border is solid even where the fill itself pulses (Attention),
    // so it stays a stable anchor rather than fading with it.
    UINT32 borderPx = [&] {
        COLORREF bc = claudeBorderColorAuto ? ContrastTextColor(color) : claudeBorderColor;
        return (UINT32(255) << 24) | (UINT32(GetRValue(bc)) << 16) | (UINT32(GetGValue(bc)) << 8) | UINT32(GetBValue(bc));
    }();
    auto drawBorderedSquare = [&](int left, int top, int size, UINT32 fillPx) {
        int right = left + size, bottom = top + size;
        int bRowStart = std::max(top, rowStart), bRowEnd = std::min(bottom, rowEnd);
        int bColStart = std::max(left, colStart), bColEnd = std::min(right, colEnd);
        for (int row = bRowStart; row < bRowEnd; row++) {
            for (int col = bColStart; col < bColEnd; col++) pixels[row * width + col] = borderPx;
        }
        int iRowStart = std::max(top + 1, rowStart), iRowEnd = std::min(bottom - 1, rowEnd);
        int iColStart = std::max(left + 1, colStart), iColEnd = std::min(right - 1, colEnd);
        for (int row = iRowStart; row < iRowEnd; row++) {
            for (int col = iColStart; col < iColEnd; col++) pixels[row * width + col] = fillPx;
        }
    };
    if (entry.loading) {
        // Radial spinner: kDots evenly spaced around a small circle, faded
        // from solid (auto-contrast accent color) down to the chip's own
        // background color going backwards from the current position --
        // reads as "still resolving this window's repo/branch/alias" (see
        // IsWindowLoading) without touching the chip's real color at all,
        // and takes priority over the AI status indicator since
        // ComputeAiStatus's folder match isn't reliable yet during this
        // window.
        const int kDots = 8, kSpinnerRadius = 6, kDotRadius = 2;
        int cx = (int)item.right - dotMargin - kSpinnerRadius - kDotRadius;
        int cy = (int)(item.top + item.bottom) / 2; // vertically centered on the item, unlike the
                                                      // top-anchored Claude status dot/ring below
        COLORREF accent = claudeBorderColorAuto ? ContrastTextColor(color) : claudeBorderColor;
        int activeIndex = (int)((GetTickCount64() / kClaudePulseIntervalMs) % kDots);
        for (int i = 0; i < kDots; i++) {
            double angle = -kPi / 2 + i * 2.0 * kPi / kDots;
            int dx = cx + (int)std::lround(kSpinnerRadius * cos(angle));
            int dy = cy + (int)std::lround(kSpinnerRadius * sin(angle));
            int distBack = (activeIndex - i + kDots) % kDots; // 0 at the lit dot, fading with distance
            double t = 1.0 - (double)distBack / kDots;
            UINT32 localBg = LoadingColorAt(dx, dy);
            BYTE bgR = (BYTE)((localBg >> 16) & 0xFF), bgG = (BYTE)((localBg >> 8) & 0xFF), bgB = (BYTE)(localBg & 0xFF);
            BYTE dr = (BYTE)(bgR + (GetRValue(accent) - (int)bgR) * t);
            BYTE dg = (BYTE)(bgG + (GetGValue(accent) - (int)bgG) * t);
            BYTE db = (BYTE)(bgB + (GetBValue(accent) - (int)bgB) * t);
            UINT32 dotPx = (UINT32(255) << 24) | (UINT32(dr) << 16) | (UINT32(dg) << 8) | UINT32(db);
            int dRowStart = std::max(dy - kDotRadius, rowStart), dRowEnd = std::min(dy + kDotRadius + 1, rowEnd);
            int dColStart = std::max(dx - kDotRadius, colStart), dColEnd = std::min(dx + kDotRadius + 1, colEnd);
            for (int row = dRowStart; row < dRowEnd; row++) {
                for (int col = dColStart; col < dColEnd; col++) {
                    if ((row - dy) * (row - dy) + (col - dx) * (col - dx) <= kDotRadius * kDotRadius) {
                        pixels[row * width + col] = dotPx;
                    }
                }
            }
        }
    } else if (entry.claudeStatus == ClaudeStatus::Working) {
        // Ring spinner: 8 cells in a 3x3 grid with an empty center, only
        // one lit at a time, advancing one step per pulse timer tick --
        // reads as a single square chasing itself around the ring. (A
        // brightness pulse on a single dot was tried first and judged too
        // subtle to notice at a glance.)
        COLORREF dotColor = claudeColorWorking;
        UINT32 dotPx = (UINT32(255) << 24) | (UINT32(GetRValue(dotColor)) << 16) |
                      (UINT32(GetGValue(dotColor)) << 8) | UINT32(GetBValue(dotColor));
        int activeIndex = (int)((GetTickCount64() / kClaudePulseIntervalMs) % 8);
        int baseX = (int)item.right - dotMargin - kRingSpan;
        int baseY = (int)item.top + dotMargin;
        int sqLeft = baseX + kRingCells[activeIndex][0] * kRingStride;
        int sqTop = baseY + kRingCells[activeIndex][1] * kRingStride;
        drawBorderedSquare(sqLeft, sqTop, kRingCellSize, dotPx);
    } else if (entry.claudeStatus == ClaudeStatus::Attention || entry.claudeStatus == ClaudeStatus::Waiting) {
        // A single big square, vertically centered on the item (unlike the
        // ring spinner above, which stays corner-anchored) -- red and
        // pulsing for Attention (blocked on a permission/MCP prompt, needs
        // the user right now), green and static for Waiting (a finished
        // turn, idle, nothing needed). Same footprint as the ring
        // spinner's 14x14 bounding box, so all three states occupy the
        // same visual "slot" regardless of which is showing.
        const int bigSize = 14;
        COLORREF dotColor = entry.claudeStatus == ClaudeStatus::Attention ? claudeColorAttention : claudeColorWaiting;
        int dr = GetRValue(dotColor), dg = GetGValue(dotColor), db = GetBValue(dotColor);
        if (entry.claudeStatus == ClaudeStatus::Attention) {
            // Pulse by blending toward the chip's own background color and
            // back -- the opacity pass below discards and recomputes this
            // pixel's alpha uniformly for the whole item, so only the RGB
            // value itself can carry a per-pixel animation through that
            // pass (see the ring spinner's construction for the same
            // reasoning). A faster cycle and a lower dim floor than a
            // typical pulse, since this state is meant to read as more
            // urgent than a calm animation.
            double phase = (GetTickCount64() % 900) / 900.0 * 2.0 * 3.14159265358979323846;
            double t = 0.25 + 0.75 * (0.5 + 0.5 * sin(phase));
            dr = (int)(r + (dr - (int)r) * t);
            dg = (int)(g + (dg - (int)g) * t);
            db = (int)(b + (db - (int)b) * t);
        }
        UINT32 dotPx = (UINT32(255) << 24) | (UINT32(dr) << 16) | (UINT32(dg) << 8) | UINT32(db);
        int sqTop = (int)item.top + (rowHeight - bigSize) / 2;
        int sqLeft = (int)item.right - dotMargin - bigSize;
        drawBorderedSquare(sqLeft, sqTop, bigSize, dotPx);
    }

    BYTE alpha = (BYTE)opacity;
    for (int row = rowStart; row < rowEnd; row++) {
        for (int col = colStart; col < colEnd; col++) {
            UINT32 px = pixels[row * width + col];
            BYTE pxR = (BYTE)((px >> 16) & 0xFF);
            BYTE pxG = (BYTE)((px >> 8) & 0xFF);
            BYTE pxB = (BYTE)(px & 0xFF);
            pixels[row * width + col] = (UINT32(alpha) << 24) | (UINT32((pxR * alpha) / 255) << 16) |
                                        (UINT32((pxG * alpha) / 255) << 8) | UINT32((pxB * alpha) / 255);
        }
    }
}

// Draws one fixed action button: same chip fill / highlight-brighten /
// opacity treatment as DrawHudItem, but a centered `glyph` instead of a
// left-aligned label, and no AI status indicator.
static void DrawActionButton(HDC screenDC, UINT32* pixels, int width, int height, const RECT& item,
                             const wchar_t* glyph, COLORREF color, bool highlighted, int opacity, HFONT font,
                             bool labelTextColorAuto, COLORREF labelTextColor) {
    BYTE r = GetRValue(color), g = GetGValue(color), b = GetBValue(color);
    if (highlighted) {
        r = (BYTE)std::min(255, (int)r + 32);
        g = (BYTE)std::min(255, (int)g + 32);
        b = (BYTE)std::min(255, (int)b + 32);
    }
    COLORREF chipColor = RGB(r, g, b);
    UINT32 chipPx = (UINT32(255) << 24) | (UINT32(r) << 16) | (UINT32(g) << 8) | UINT32(b);

    int rowStart = std::max((int)item.top, 0), rowEnd = std::min((int)item.bottom, height);
    int colStart = std::max((int)item.left, 0), colEnd = std::min((int)item.right, width);
    for (int row = rowStart; row < rowEnd; row++) {
        for (int col = colStart; col < colEnd; col++) pixels[row * width + col] = chipPx;
    }

    COLORREF tx = labelTextColorAuto ? ContrastTextColor(color) : labelTextColor;
    BlendTextIntoPixels(screenDC, pixels, width, height, item.left, item.top, item.right - item.left,
                        item.bottom - item.top, glyph, font, chipColor, tx,
                        DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    BYTE alpha = (BYTE)opacity;
    for (int row = rowStart; row < rowEnd; row++) {
        for (int col = colStart; col < colEnd; col++) {
            UINT32 px = pixels[row * width + col];
            BYTE pxR = (BYTE)((px >> 16) & 0xFF);
            BYTE pxG = (BYTE)((px >> 8) & 0xFF);
            BYTE pxB = (BYTE)(px & 0xFF);
            pixels[row * width + col] = (UINT32(alpha) << 24) | (UINT32((pxR * alpha) / 255) << 16) |
                                        (UINT32((pxG * alpha) / 255) << 8) | UINT32((pxB * alpha) / 255);
        }
    }
}

static UINT32 OpaquePixel(COLORREF c) {
    return 0xFF000000u | (UINT32(GetRValue(c)) << 16) | (UINT32(GetGValue(c)) << 8) | UINT32(GetBValue(c));
}

static COLORREF UsageRingColor(double percent, bool stale) {
    if (stale) return kUsageStaleColor;
    if (percent >= kUsageCriticalPercent) return kUsageCriticalColor;
    if (percent >= kUsageWarnPercent) return kUsageWarnColor;
    return kUsageOkColor;
}

// Draws one profile's usage ring group (see ProjectListHudUsageRing) at
// `item`: the chip in the profile's own color, its icon, the twin
// rings -- antialiased by supersampling -- then the same opacity pass as
// DrawHudItem.
static void DrawUsageRings(UINT32* pixels, int width, int height, int rowHeight, const RECT& item,
                           const ProjectListHudUsageRing& ring, int opacity) {
    int rowStart = std::max((int)item.top, 0), rowEnd = std::min((int)item.bottom, height);
    int colStart = std::max((int)item.left, 0), colEnd = std::min((int)item.right, width);
    // Without a chip color the group is left as the surface's own near-
    // transparent background, and only what's drawn on it shows.
    if (ring.hasChipColor) {
        UINT32 chipPx = OpaquePixel(ring.chipColor);
        for (int row = rowStart; row < rowEnd; row++) {
            for (int col = colStart; col < colEnd; col++) pixels[row * width + col] = chipPx;
        }
    }

    int left = (int)item.left + kUsageRingPad;
    if (ring.showProfileIcon) {
        BlendClaudeProfileIcon(ring.profile, pixels, width, height, left, (int)item.top + (rowHeight - kProfileIconSize) / 2,
                         item);
        left += kProfileIconSize + kUsageRingBadgeGap;
    }

    const double kPi = 3.14159265358979323846;
    double size = rowHeight, cx = left + size / 2, cy = item.top + size / 2;
    double stroke = size * 0.13;
    double outerMid = size / 2 - 1 - stroke / 2, innerMid = outerMid - stroke - 1.2;
    struct Band {
        double inner, outer, fraction;
        COLORREF fill;
    } bands[2] = {
        {outerMid - stroke / 2, outerMid + stroke / 2, ring.weeklyPercent / 100,
         UsageRingColor(ring.weeklyPercent, ring.stale)},
        {innerMid - stroke / 2, innerMid + stroke / 2, ring.sessionPercent / 100,
         UsageRingColor(ring.sessionPercent, ring.stale)},
    };
    // Unavailable: no numbers that could pass for a reading -- just the outer
    // ring's track, and an amber "!" inside it fading out and back in.
    int bandCount = 2;
    double glyphStrength = 0;
    if (ring.unavailable) {
        bands[0].fraction = 0;
        bandCount = 1;
        double phase = (GetTickCount64() % kUsageUnavailablePulseMs) / (double)kUsageUnavailablePulseMs * 2 * kPi;
        glyphStrength = 0.3 + 0.7 * (0.5 + 0.5 * cos(phase));
    }
    // The "!": a bar above a dot, sized to the ring.
    double barHalfWidth = size * 0.06, barTop = cy - size * 0.24, barBottom = cy + size * 0.05;
    double dotY = cy + size * 0.17, dotRadius = size * 0.065;
    const int kSamples = 4;
    int ringRight = std::min(colEnd, left + (int)size);
    for (int row = rowStart; row < rowEnd; row++) {
        for (int col = std::max(colStart, left); col < ringRight; col++) {
            // Premultiplied sums: a sample on the bare background adds the
            // chip's color when there is one, and nothing (no coverage)
            // when it's transparent.
            double sumR = 0, sumG = 0, sumB = 0, covered = 0;
            for (int sy = 0; sy < kSamples; sy++) {
                for (int sx = 0; sx < kSamples; sx++) {
                    double dx = col + (sx + 0.5) / kSamples - cx, dy = row + (sy + 0.5) / kSamples - cy;
                    double dist = sqrt(dx * dx + dy * dy);
                    double turn = atan2(dx, -dy) / (2 * kPi); // clockwise from 12 o'clock
                    if (turn < 0) turn += 1;
                    bool onBand = false;
                    COLORREF c = ring.chipColor;
                    for (int k = 0; k < bandCount; k++) {
                        const Band& b = bands[k];
                        if (dist >= b.inner && dist < b.outer) {
                            c = turn < b.fraction ? b.fill : kUsageTrackColor;
                            onBand = true;
                        }
                    }
                    double ddy = dy + cy - dotY;
                    bool onGlyph = ring.unavailable &&
                                   ((fabs(dx) <= barHalfWidth && dy + cy >= barTop && dy + cy <= barBottom) ||
                                    dx * dx + ddy * ddy <= dotRadius * dotRadius);
                    if (onGlyph) {
                        // Fading: amber over whatever is behind it -- the chip,
                        // or nothing on a transparent one.
                        double t = glyphStrength;
                        sumR += GetRValue(kUsageWarnColor) * t;
                        sumG += GetGValue(kUsageWarnColor) * t;
                        sumB += GetBValue(kUsageWarnColor) * t;
                        if (ring.hasChipColor) {
                            sumR += GetRValue(ring.chipColor) * (1 - t);
                            sumG += GetGValue(ring.chipColor) * (1 - t);
                            sumB += GetBValue(ring.chipColor) * (1 - t);
                        }
                        covered += ring.hasChipColor ? 1 : t;
                        continue;
                    }
                    if (!onBand && !ring.hasChipColor) continue;
                    sumR += GetRValue(c);
                    sumG += GetGValue(c);
                    sumB += GetBValue(c);
                    covered++;
                }
            }
            if (covered <= 0) continue;
            const int n = kSamples * kSamples;
            pixels[row * width + col] = (UINT32(std::min(255.0, covered * 255 / n)) << 24) | (UINT32(sumR / n) << 16) |
                                        (UINT32(sumG / n) << 8) | UINT32(sumB / n);
        }
    }

    // Scales the premultiplied pixels, keeping at least the background's
    // alpha of 1 so the whole group still takes the mouse.
    for (int row = rowStart; row < rowEnd; row++) {
        for (int col = colStart; col < colEnd; col++) {
            UINT32 px = pixels[row * width + col];
            UINT32 a = std::max<UINT32>(1, ((px >> 24) * opacity) / 255);
            pixels[row * width + col] = (a << 24) | (((((px >> 16) & 0xFF) * opacity) / 255) << 16) |
                                        (((((px >> 8) & 0xFF) * opacity) / 255) << 8) | (((px & 0xFF) * opacity) / 255);
        }
    }
}

static void RenderProjectListHud(HWND hud, ProjectListHudState* state) {
    if (!state || state->width <= 0 || state->height <= 0) return;
    if (state->entries.empty() && !state->showNewWindowButton) return; // nothing to draw at all

    int width = state->width;
    int height = state->height;
    int rowHeight = state->rowHeight;

    HDC screenDC = GetDC(nullptr);
    HDC memDC = CreateCompatibleDC(screenDC);

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(memDC, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp) {
        DeleteDC(memDC);
        ReleaseDC(nullptr, screenDC);
        return;
    }
    HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, bmp);

    UINT32* pixels = (UINT32*)bits;
    for (int i = 0; i < width * height; i++) pixels[i] = 0x01000000u;

    HFONT font = CreateHudFont(state->fontSize, FW_SEMIBOLD);

    // While reorder-dragging, the dragged item's own slot is left empty --
    // PaintDragGhost/MoveDragGhost draw it separately, in its own
    // always-on-top window, so it can float freely outside the HUD's own
    // bounds (see there).
    bool dragging = state->dragMode == ProjectListHudState::DragReorder;
    for (size_t i = 0; i < state->entries.size() && i < state->itemRects.size(); i++) {
        if (dragging && (int)i == state->reorderIndex) continue;
        bool highlighted = (int)i == state->hoverIndex;
        int opacity = highlighted ? state->hoverOpacity : state->normalOpacity;
        DrawHudItem(screenDC, pixels, width, height, rowHeight, state->entries[i], state->itemRects[i],
                    highlighted, opacity, EntryIsFavourite(state->entries[i]), state->labelTextColorAuto,
                    state->labelTextColor, font, state->claudeColorWorking, state->claudeColorAttention,
                    state->claudeColorWaiting, state->claudeBorderColorAuto, state->claudeBorderColor);
    }

    // "+" takes the color the next new window will get (see
    // ProjectListHudStyle::newWindowButtonColor); the other two are a
    // neutral grey, with close-all turning the usual caption-button red on
    // hover.
    static const wchar_t* const kGlyphs[3] = {L"+", L"–", L"×"};
    const COLORREF kNeutral = RGB(96, 96, 96), kCloseHover = RGB(196, 43, 28);
    for (int k = 0; k < ActionButtonCount(state); k++) {
        bool highlighted = state->hoverIndex == (int)state->entries.size() + k;
        int opacity = highlighted ? state->hoverOpacity : state->normalOpacity;
        COLORREF color = k == HudActionNewWindow ? state->newWindowButtonColor : kNeutral;
        if (k == HudActionCloseAll && highlighted) {
            color = kCloseHover;
            highlighted = false; // already the hover color -- don't brighten it further
        }
        DrawActionButton(screenDC, pixels, width, height, state->actionButtonRects[k], kGlyphs[k], color,
                         highlighted, opacity, font, state->labelTextColorAuto, state->labelTextColor);
    }

    DeleteObject(font);

    POINT ptSrc = {0, 0};
    SIZE sz = {width, height};
    BLENDFUNCTION blend = {};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;

    UpdateLayeredWindow(hud, screenDC, nullptr, &sz, memDC, &ptSrc, 0, &blend, ULW_ALPHA);

    SelectObject(memDC, oldBmp);
    DeleteObject(bmp);
    DeleteDC(memDC);
    ReleaseDC(nullptr, screenDC);
}

// Repositions state->dragGhost to track the cursor -- cheap (a single
// SetWindowPos, no repainting), since the dragged item's *appearance*
// never changes for the duration of a drag, only its screen position. Call
// on every mouse-move frame of a DragReorder; see PaintDragGhost for the
// one-time paint that must happen first.
static void MoveDragGhost(ProjectListHudState* state) {
    if (!state || !state->dragGhost) return;
    int screenX = state->x + state->reorderCursorClient.x - state->reorderGrabOffset.x;
    int screenY = state->y + state->reorderCursorClient.y - state->reorderGrabOffset.y;
    // See PaintDragGhost's comment: toggling through HWND_NOTOPMOST forces a
    // real re-insertion at the front of the topmost band, which a plain
    // repeated HWND_TOPMOST call does not reliably do once some other
    // window (e.g. the shell's taskbar-hover helper) has gone topmost more
    // recently than we have.
    SetWindowPos(state->dragGhost, HWND_NOTOPMOST, screenX, screenY, 0, 0, SWP_NOACTIVATE | SWP_NOSIZE);
    SetWindowPos(state->dragGhost, HWND_TOPMOST, screenX, screenY, 0, 0,
                SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOSIZE);
}

// Paints state->dragGhost's appearance (the dragged item's chip) and
// positions it for the first time, at the start of a DragReorder -- a
// screen-coordinate window, entirely independent of the HUD's own bounds,
// so the item being dragged can move freely outside the HUD (unlike the
// grid of other items, which is confined to it). See MoveDragGhost for the
// cheap per-frame repositioning that follows: repainting on every
// mouse-move -- a full 32bpp bitmap create+draw+composite pass, on top of
// the main grid's own such pass -- measurably lagged behind fast cursor
// movement, which looked like the dragged item getting stuck/not keeping
// up. Since the content never changes mid-drag, painting it once is enough.
static void PaintDragGhost(ProjectListHudState* state) {
    if (!state || !state->dragGhost) return;
    if (state->reorderIndex < 0 || state->reorderIndex >= (int)state->entries.size() ||
        state->reorderIndex >= (int)state->itemRects.size()) {
        return;
    }
    const RECT& slot = state->itemRects[state->reorderIndex];
    int itemW = slot.right - slot.left, itemH = slot.bottom - slot.top;
    if (itemW <= 0 || itemH <= 0) return;

    HDC screenDC = GetDC(nullptr);
    HDC memDC = CreateCompatibleDC(screenDC);

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = itemW;
    bmi.bmiHeader.biHeight = -itemH;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(memDC, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp) {
        DeleteDC(memDC);
        ReleaseDC(nullptr, screenDC);
        return;
    }
    HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, bmp);

    UINT32* pixels = (UINT32*)bits;
    for (int i = 0; i < itemW * itemH; i++) pixels[i] = 0x01000000u;

    HFONT font = CreateHudFont(state->fontSize, FW_SEMIBOLD);
    RECT localRect = {0, 0, itemW, itemH}; // ghost's own bitmap -- always local-origin, unlike the
                                            // screen-position SetWindowPos call below
    // Drawn "highlighted" (brightened, like a hovered item) so the item
    // you're actively moving reads as visually active.
    DrawHudItem(screenDC, pixels, itemW, itemH, state->rowHeight, state->entries[state->reorderIndex],
                localRect, true, state->hoverOpacity, EntryIsFavourite(state->entries[state->reorderIndex]),
                state->labelTextColorAuto, state->labelTextColor, font, state->claudeColorWorking,
                state->claudeColorAttention, state->claudeColorWaiting, state->claudeBorderColorAuto,
                state->claudeBorderColor);
    DeleteObject(font);

    int screenX = state->x + state->reorderCursorClient.x - state->reorderGrabOffset.x;
    int screenY = state->y + state->reorderCursorClient.y - state->reorderGrabOffset.y;
    // Re-asserting HWND_TOPMOST on a window that's already topmost doesn't
    // reliably move it ahead of other topmost windows that appeared more
    // recently (e.g. the shell's taskbar-hover helper window) -- toggling
    // through HWND_NOTOPMOST first forces a real re-insertion at the front.
    // See MoveDragGhost for the same pattern, applied every frame.
    SetWindowPos(state->dragGhost, HWND_NOTOPMOST, screenX, screenY, itemW, itemH, SWP_NOACTIVATE);
    SetWindowPos(state->dragGhost, HWND_TOPMOST, screenX, screenY, itemW, itemH, SWP_NOACTIVATE | SWP_SHOWWINDOW);

    POINT ptSrc = {0, 0};
    SIZE sz = {itemW, itemH};
    BLENDFUNCTION blend = {};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;

    UpdateLayeredWindow(state->dragGhost, screenDC, nullptr, &sz, memDC, &ptSrc, 0, &blend, ULW_ALPHA);

    SelectObject(memDC, oldBmp);
    DeleteObject(bmp);
    DeleteDC(memDC);
    ReleaseDC(nullptr, screenDC);
}

// Shows the usage rings' strip exactly while the HUD is, at the right end of
// the band level with it, freshly painted; hides it otherwise.
static void SyncUsageStrip(HWND hud, ProjectListHudState* state) {
    if (!state || !state->usageStrip) return;
    int width = UsageRingsRowWidth(state), height = state->rowHeight;
    if (!IsWindowVisible(hud) || width <= 0 || height <= 0) {
        if (IsWindowVisible(state->usageStrip)) ShowWindow(state->usageStrip, SW_HIDE);
        if (state->usageHoverRing >= 0) {
            state->usageHoverRing = -1;
            SyncUsageTooltip(hud, state);
        }
        return;
    }

    HDC screenDC = GetDC(nullptr);
    HDC memDC = CreateCompatibleDC(screenDC);
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(memDC, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bmp) {
        HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, bmp);
        UINT32* pixels = (UINT32*)bits;
        // Nearly transparent rather than fully, so the gaps between groups
        // still take the mouse, same as the HUD's own background.
        for (int i = 0; i < width * height; i++) pixels[i] = 0x01000000u;
        for (size_t j = 0; j < state->usageRings.size() && j < state->usageRingRects.size(); j++) {
            DrawUsageRings(pixels, width, height, height, state->usageRingRects[j], state->usageRings[j],
                           state->normalOpacity);
        }
        POINT ptDst = {(int)state->dockRect.right - width, state->y}, ptSrc = {0, 0};
        SIZE sz = {width, height};
        BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        UpdateLayeredWindow(state->usageStrip, screenDC, &ptDst, &sz, memDC, &ptSrc, 0, &blend, ULW_ALPHA);
        SelectObject(memDC, oldBmp);
        DeleteObject(bmp);
    }
    DeleteDC(memDC);
    ReleaseDC(nullptr, screenDC);

    if (!IsWindowVisible(state->usageStrip)) {
        SetWindowPos(state->usageStrip, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    }
    SyncUsageTooltip(hud, state);
}

void HideProjectListHud(HWND hud) {
    if (!hud) return;
    ShowWindow(hud, SW_HIDE);
    SyncUsageStrip(hud, (ProjectListHudState*)GetWindowLongPtrW(hud, GWLP_USERDATA));
}

void SetProjectListHudDocked(HWND hud, bool docked, int rowHeight, bool matchTaskbar) {
    ProjectListHudState* state = hud ? (ProjectListHudState*)GetWindowLongPtrW(hud, GWLP_USERDATA) : nullptr;
    if (!state) return;
    // With the taskbar-matching backdrop, one extra row on top for its
    // border line (see taskbar_backdrop.h) -- FitIntoDock keeps the HUD
    // below it, the way the taskbar's own icons sit below its border.
    int bandHeight = std::max(kProjectListMinRowHeight, rowHeight) + (matchTaskbar ? 1 : 0);
    if (docked == state->docked && (!docked || bandHeight == state->dockBandHeight)) {
        if (matchTaskbar != state->matchTaskbar) { // only reachable while the hub is off
            state->matchTaskbar = matchTaskbar;
            SyncBackdrop(hud, state);
        }
        return;
    }
    state->matchTaskbar = matchTaskbar;

    bool modeChanged = docked != state->docked;
    if (docked && modeChanged && !RegisterAppBar(hud)) {
        // Without the band there's nowhere to put the hub -- it stays
        // hidden (see UpdateProjectListHud), and the next sync tries again.
        if (!state->appBarFailureLogged) LogWarn(L"hud: couldn't register as an AppBar -- hub hidden until it can");
        state->appBarFailureLogged = true;
        return;
    }
    state->appBarFailureLogged = false;
    if (!docked) {
        UnregisterAppBar(hud);
        state->fullscreenAppOpen = false;
        state->docked = false;
        SyncBackdrop(hud, state);
    }
    state->docked = docked;
    state->dockBandHeight = bandHeight;
    if (docked) RefreshDockRect(hud, state);
    if (modeChanged) {
        Log(L"hud: band %ls", docked ? L"reserved" : L"released");
        LoadPlacementForCurrentKey(state);
    }
    RelayoutDocked(hud, state);
}

// Widest label (padded, clamped) across `entries`, using the same font
// RenderProjectListHud paints normal (non-hover) rows with -- so the HUD is
// sized to fit what it's about to draw.
static int MeasureRequiredWidth(const std::vector<ProjectListHudEntry>& entries, int fontSize) {
    HDC screenDC = GetDC(nullptr);
    HFONT font = CreateHudFont(fontSize, FW_SEMIBOLD);
    HFONT oldFont = (HFONT)SelectObject(screenDC, font);
    int width = 0;
    for (const ProjectListHudEntry& entry : entries) {
        SIZE textSz = {0, 0};
        // A favourite reserves extra room on the left for its star marker
        // (see kFavouriteMarkerLeftPad vs. the plain paddingX every other
        // item uses) -- added on top of the usual measure padding so a
        // favourite's label isn't truncated any more than a non-favourite's
        // would be at the same text width.
        int reserve = kProjectListMeasurePaddingX + (EntryIsFavourite(entry) ? (kFavouriteMarkerLeftPad - 12) : 0) +
                      LabelIconsTextShift(entry);
        GetTextExtentPoint32W(screenDC, entry.label.c_str(), (int)entry.label.size(), &textSz);
        width = std::max(width, (int)textSz.cx + reserve);
    }
    SelectObject(screenDC, oldFont);
    DeleteObject(font);
    ReleaseDC(nullptr, screenDC);
    return ClampInt(width, kProjectListMinWidth, kProjectListAutoMaxWidth);
}

// Rearranges `entries` to match `order` (a list of aliasKeys in the user's
// last-dragged order): entries whose aliasKey appears in `order` come
// first, in that relative order; any not found (e.g. a project opened since
// the last drag, so it has no position yet) are appended afterward,
// oldest-tracked first (trackSeq) -- i.e. plain append order, not wherever
// Windows happened to put that window on screen.
static void ApplyManualOrder(std::vector<ProjectListHudEntry>& entries, const std::vector<std::wstring>& order) {
    std::stable_sort(entries.begin(), entries.end(),
                     [&order](const ProjectListHudEntry& a, const ProjectListHudEntry& b) {
        auto ia = std::find(order.begin(), order.end(), a.aliasKey);
        auto ib = std::find(order.begin(), order.end(), b.aliasKey);
        bool aFound = ia != order.end(), bFound = ib != order.end();
        if (aFound != bFound) return aFound;
        if (aFound) return (ia - ib) < 0;
        return a.trackSeq < b.trackSeq;
    });
}

void UpdateProjectListHud(HWND hud, const std::vector<ProjectListHudEntry>& entries,
                          const ProjectListHudStyle& style) {
    // An empty list is a valid hub as long as the new-window button is on:
    // that button alone is what's left when every VS Code window is closed,
    // and it's how the user opens one again. Only a hub with nothing at all
    // in it is skipped.
    if (entries.empty() && !style.showNewWindowButton) return;

    ProjectListHudState* state = (ProjectListHudState*)GetWindowLongPtrW(hud, GWLP_USERDATA);
    if (!state) return;
    // Windows still pumps this app's timers/WinEvents while TrackPopupMenu's
    // modal loop is running (and similarly, nothing stops a regular sync
    // from firing while the alias edit box is up, or mid-drag) -- resyncing
    // right then would re-assert HWND_TOPMOST on the HUD via
    // PositionProjectListHud (which can visually push it back above the
    // still-open menu/edit box), or clobber state->entries out from under
    // an in-progress drag's own live reordering. Skip the sync entirely
    // until whichever is active finishes; nothing here needs to be
    // live-updated while the user's attention is on any of them anyway.
    if (state->contextMenuOpen || state->editIndex >= 0 || state->dragMode != ProjectListHudState::DragNone) {
        return;
    }
    // No reserved band (the AppBar couldn't be registered -- see
    // SetProjectListHudDocked): nowhere to put the hub.
    if (!state->docked) {
        ShowWindow(hud, SW_HIDE);
        SyncUsageStrip(hud, state);
        return;
    }

    // Pull loading entries (see ProjectListHudEntry::loading) out before
    // drag-order matching below: it keys off rawLabel, which can still be
    // changing moment to moment while loading, and would otherwise make the
    // whole list visibly reshuffle for no action the user actually took. Keep them in their own fixed,
    // trackSeq-ordered block at the very end instead -- each one moves to
    // its proper place exactly once, when it finishes loading, not before.
    std::vector<ProjectListHudEntry> loadingEntries;
    std::vector<ProjectListHudEntry> settled;
    for (const ProjectListHudEntry& e : entries) {
        if (e.loading) loadingEntries.push_back(e);
        else settled.push_back(e);
    }
    std::sort(loadingEntries.begin(), loadingEntries.end(),
             [](const ProjectListHudEntry& a, const ProjectListHudEntry& b) { return a.trackSeq < b.trackSeq; });

    ApplyManualOrder(settled, state->manualOrder);

    // Keep a favourite's stored label in sync with its live hub item
    // whenever that project is actually open -- otherwise the favourites
    // menu (ShowNewWindowButtonMenu) would keep showing whatever
    // label happened to be captured back when "Add to Favourites" was
    // clicked, even after the hub item's own label later changes (e.g. the
    // window title reparses differently once VS Code picks up this app's
    // window.title setting, or the user sets/changes an alias).
    for (const ProjectListHudEntry& e : settled) {
        if (!e.path.empty() && !e.loading) RefreshFavouriteLabel(e.path, e.label);
    }

    std::vector<ProjectListHudEntry> sorted = settled;
    sorted.insert(sorted.end(), loadingEntries.begin(), loadingEntries.end());

    int rowHeight = std::max(kProjectListMinRowHeight, style.rowHeight);
    state->rowHeight = rowHeight;
    state->entries = sorted;
    state->showNewWindowButton = style.showNewWindowButton;
    state->newWindowButtonColor = style.newWindowButtonColor;
    state->usageRings = style.usageRings;
    int buttonReserve = ActionButtonsReserve(state); // after entries/rowHeight/showNewWindowButton are set
    size_t n = sorted.size();

    // Every item shares one width, sized to fit the widest label when not
    // manually resized. When manually resized, reuse the remembered per-item
    // width (manualItemWidth) rather than whatever state->width happened to
    // be -- state->width is a function of both that and the current entry
    // count, so re-deriving it here is what keeps every item the same width
    // no matter how many windows are currently tracked. The action buttons,
    // when shown, get fixed square slots on top of that (see
    // RebuildItemRects).
    int itemWidth = !state->manualWidth ? MeasureRequiredWidth(sorted, style.fontSize)
                                         : std::max(1, state->manualItemWidth);
    // With no entries the new-window button (the only one shown then, see
    // ActionButtonCount) is the whole hub: just its square, without the gap
    // buttonReserve carries to separate it from the last item there is no
    // longer any of.
    state->width = n > 0 ? (int)n * itemWidth + (int)(n - 1) * kProjectListGap + buttonReserve
                         : std::max(0, buttonReserve - kProjectListGap);
    state->height = rowHeight;
    FitIntoDock(state);
    // After FitIntoDock, since the band can cap the width.
    RebuildItemRects(state);

    state->fontSize = style.fontSize;
    state->labelTextColorAuto = style.labelTextColorAuto;
    state->labelTextColor = style.labelTextColor;
    state->claudeColorWorking = style.claudeColorWorking;
    state->claudeColorAttention = style.claudeColorAttention;
    state->claudeColorWaiting = style.claudeColorWaiting;
    state->claudeBorderColorAuto = style.claudeBorderColorAuto;
    state->claudeBorderColor = style.claudeBorderColor;
    state->normalOpacity = ClampInt(style.normalOpacity, 0, 255);
    state->hoverOpacity = ClampInt(style.hoverOpacity, 0, 255);
    state->activateOnHover = style.activateOnHover;
    if (!state->activateOnHover) EndProjectListHoverFocus(state, true);
    // The action buttons' hover "indices" are the sentinels from
    // sorted.size() up (see ProjectListHitTest) -- past the last valid entry
    // index, not out of range, while they're shown.
    int maxHoverIndex = (int)sorted.size() - 1 + ActionButtonCount(state);
    if (state->hoverIndex > maxHoverIndex) state->hoverIndex = -1;

    // Both the Working ring spinner and the Attention pulse need a repaint
    // every kClaudePulseIntervalMs to actually animate, but only while
    // there's something to animate -- started/stopped here so an idle
    // desktop (or one where nothing's currently animating) never pays for
    // it, same on-demand pattern as tracking.cpp's foreground-poll timer.
    bool anyAnimating = std::any_of(sorted.begin(), sorted.end(), [](const ProjectListHudEntry& e) {
        return e.loading || e.claudeStatus == ClaudeStatus::Working || e.claudeStatus == ClaudeStatus::Attention;
    }) || std::any_of(state->usageRings.begin(), state->usageRings.end(),
                      [](const ProjectListHudUsageRing& r) { return r.unavailable; });
    if (anyAnimating && !state->pulseTimerActive) {
        SetTimer(hud, kClaudePulseTimerId, kClaudePulseIntervalMs, nullptr);
        state->pulseTimerActive = true;
    } else if (!anyAnimating && state->pulseTimerActive) {
        KillTimer(hud, kClaudePulseTimerId);
        state->pulseTimerActive = false;
    }

    if (state->usageHoverRing >= (int)state->usageRings.size()) state->usageHoverRing = -1;

    RenderProjectListHud(hud, state);
    PositionProjectListHud(hud, state); // also syncs the rings' strip
}