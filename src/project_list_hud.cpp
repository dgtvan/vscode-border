#include "project_list_hud.h"

#include "favourites.h"
#include "label_alias.h"
#include "layered_rendering.h"
#include "logger.h"
#include "monitor_scenario.h"
#include "project_list_order.h"

#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>

static const wchar_t* kProjectListHudClassName = L"VSCodeBorderProjectListHudWndClass";
static const int kProjectListGap = 6;
static const int kProjectListEdgeGrip = 8;
static const int kProjectListMinWidth = 130; // manual (drag-resize) lower bound, both styles -- no upper
                                              // bound: the user can size the HUD as wide as they want
static const int kProjectListAutoMaxWidth = 360; // auto-measured upper bound for the shared item/column
                                                  // width -- narrower so a single long label can't blow up
                                                  // the HUD when the user hasn't sized it by hand
static const int kProjectListMeasurePaddingX = 32;
static const int kReorderDragThreshold = 4; // pixels of movement before a plain left-click-on-an-item
                                             // commits to a reorder-drag instead of a click-to-activate
static const int kProjectListBottomMargin = 24;
static const UINT_PTR kClaudePulseTimerId = 1; // repaints while >=1 item is ClaudeStatus::Working, to
                                                // animate its dot -- started/stopped on demand, see
                                                // UpdateProjectListHud
static const UINT kClaudePulseIntervalMs = 150;

static HFONT CreateHudFont(int fontSize, int weight) {
    return CreateFontW(-fontSize, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                       CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
}

struct ProjectListHudState {
    std::vector<ProjectListHudEntry> entries;
    std::vector<RECT> itemRects; // per-entry rect within the HUD's own client area (local coords)
    bool horizontal = false;
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
    RECT newWindowButtonRect = {}; // valid only while showNewWindowButton -- see Rebuild*ItemRects. Not
                                    // part of itemRects/entries: fixed at the end, no drag-reorder. Its
                                    // "index" for hover/click purposes is the sentinel entries.size().
    int hoverIndex = -1;
    bool trackingMouseLeave = false;
    bool hoverFocusActive = false;
    HWND previousForeground = nullptr;
    bool manualPosition = false;
    bool manualWidth = false;
    int manualItemWidth = 0; // horizontal style only: the user's chosen per-item width, independent of
                              // how many items are currently shown -- state->width (the whole strip) is
                              // re-derived from this every time entries.size() changes, so item count
                              // never changes how wide each item looks (see RebuildHorizontalItemRects)
    enum DragMode { DragNone, DragMove, DragResizeLeft, DragResizeRight, DragReorder } dragMode = DragNone;
    POINT dragStart = {0, 0};
    RECT dragStartRect = {0, 0, 0, 0};
    bool manualOrderMode = false; // mirrors style.manualOrder, cached here since WM_LBUTTONDOWN needs it
                                  // without going through UpdateProjectListHud
    std::vector<std::wstring> manualOrder; // rawLabels in last-dragged display order (see project_list_order.h)
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
};

static int ClampInt(int value, int minValue, int maxValue) {
    return std::max(minValue, std::min(value, maxValue));
}

// Returns an entry index for a hit within state->itemRects, or the sentinel
// state->entries.size() for a hit on the fixed new-window button (see
// newWindowButtonRect's comment), or -1 for no hit.
static int ProjectListHitTest(const ProjectListHudState* state, int x, int y) {
    if (!state || x < 0 || x >= state->width || y < 0 || y >= state->height) return -1;
    for (size_t i = 0; i < state->itemRects.size(); i++) {
        const RECT& r = state->itemRects[i];
        if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) return (int)i;
    }
    if (state->showNewWindowButton) {
        const RECT& r = state->newWindowButtonRect;
        if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) return (int)state->entries.size();
    }
    return -1;
}

// Like ProjectListHitTest, but for choosing a reorder-drop target during an
// active drag: ignores the cross-axis coordinate entirely (e.g. how far
// above/below a horizontal strip the ghost has been dragged) and maps
// purely by position along the strip's own axis, clamped to the first/last
// slot past either end. This lets a middle slot still be targeted while
// dragging far outside the strip's bounds, not just the two end slots.
static int ProjectListDragCandidate(const ProjectListHudState* state, int x, int y) {
    if (!state || state->itemRects.empty()) return -1;
    if (state->horizontal) {
        if (x < state->itemRects.front().left) return 0;
        for (size_t i = 0; i < state->itemRects.size(); i++) {
            if (x < state->itemRects[i].right) return (int)i;
        }
    } else {
        if (y < state->itemRects.front().top) return 0;
        for (size_t i = 0; i < state->itemRects.size(); i++) {
            if (y < state->itemRects[i].bottom) return (int)i;
        }
    }
    return (int)state->itemRects.size() - 1;
}

// True if `entry` is a saved favourite (see favourites.h) -- entries always
// end up favourites-first (see PinFavouritesToFront), so hover/drag logic
// elsewhere locates the boundary via FavouriteZoneCount rather than calling
// this per item.
static bool EntryIsFavourite(const ProjectListHudEntry& entry) {
    return !entry.path.empty() && IsFavourite(entry.path);
}

// Moves every favourite entry ahead of every non-favourite one, preserving
// relative order within each group (i.e. whatever ApplyManualOrder/the
// window-position sort already produced) -- applied unconditionally,
// regardless of style.manualOrder, so favourites stay pinned to the front
// of the hub item list either way. Drag-and-drop reordering (see
// FavouriteZoneCount) then confines a dragged item to its own group, so
// this front-pinning can't be undone by a drag.
static void PinFavouritesToFront(std::vector<ProjectListHudEntry>& entries) {
    std::stable_partition(entries.begin(), entries.end(), EntryIsFavourite);
}

// Count of leading favourite entries in `entries` -- the boundary between
// the favourites zone and the regular zone. Relies on entries already being
// favourites-first (see PinFavouritesToFront); state->entries always is.
static int FavouriteZoneCount(const std::vector<ProjectListHudEntry>& entries) {
    int count = 0;
    while (count < (int)entries.size() && EntryIsFavourite(entries[count])) count++;
    return count;
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

// Recomputes vertical-style item rects from state->width/rowHeight/entries
// -- cheap (no GDI/text-measurement calls), so it's safe to call on every
// mouse-move frame of an interactive resize-drag.
static void RebuildVerticalItemRects(ProjectListHudState* state) {
    state->itemRects.assign(state->entries.size(), RECT{});
    for (size_t i = 0; i < state->entries.size(); i++) {
        int y = (int)i * (state->rowHeight + kProjectListGap);
        state->itemRects[i] = {0, y, state->width, y + state->rowHeight};
    }
    if (state->showNewWindowButton) {
        int size = state->rowHeight;
        int y = (int)state->entries.size() * (state->rowHeight + kProjectListGap);
        state->newWindowButtonRect = {0, y, size, y + size};
    }
}

// Divides state->width evenly across items, same width for every one
// regardless of label length (long labels just ellipsize) -- the horizontal
// analog of RebuildVerticalItemRects's shared column width. Cheap (no
// GDI/text-measurement calls), so it's safe to call on every mouse-move
// frame of an interactive resize-drag.
static void RebuildHorizontalItemRects(ProjectListHudState* state) {
    size_t n = state->entries.size();
    state->itemRects.assign(n, RECT{});

    int buttonReserve = state->showNewWindowButton ? (state->rowHeight + kProjectListGap) : 0;
    int itemsWidth = state->width - buttonReserve;

    if (n > 0) {
        int itemWidth = std::max(1, (itemsWidth - (int)(n - 1) * kProjectListGap) / (int)n);
        int x = 0;
        for (size_t i = 0; i < n; i++) {
            state->itemRects[i] = {x, 0, x + itemWidth, state->rowHeight};
            x += itemWidth + kProjectListGap;
        }
    }

    if (state->showNewWindowButton) {
        int size = state->rowHeight;
        state->newWindowButtonRect = {state->width - size, 0, state->width, size};
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
static void PaintDragGhost(ProjectListHudState* state);
static void MoveDragGhost(ProjectListHudState* state);

static void PositionProjectListHud(HWND hud, ProjectListHudState* state) {
    if (!state) return;
    // Re-asserting HWND_TOPMOST on a window that's already topmost doesn't
    // reliably move it ahead of some other window that's gone topmost more
    // recently (e.g. briefly, when another app's own window is maximized) --
    // toggling through HWND_NOTOPMOST first forces a real re-insertion at
    // the front. Same fix, same reasoning, as MoveDragGhost/PaintDragGhost.
    SetWindowPos(hud, HWND_NOTOPMOST, state->x, state->y, state->width, state->height, SWP_NOACTIVATE);
    SetWindowPos(hud, HWND_TOPMOST, state->x, state->y, state->width, state->height,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

// Re-checks the current monitor scenario against state->scenarioKey and, if
// it changed, applies whatever placement was last saved for the new one --
// or drops back to auto-placement if this scenario has never been seen
// before, letting the next regular UpdateProjectListHud recompute the
// default position/width. Called once at HUD creation (so the very first
// sync already uses a remembered placement, if any) and on every
// WM_DISPLAYCHANGE. Returns true if the scenario actually changed.
//
// The saved "width" always means *per-item* width -- for the vertical
// style that's the same thing as the whole strip's width (one shared
// column), but for horizontal it's deliberately not the whole strip's
// width, so restoring it doesn't depend on how many windows happen to be
// tracked at that moment (see manualItemWidth's comment).
static bool ApplyScenarioForCurrentMonitors(ProjectListHudState* state) {
    std::wstring key = GetMonitorScenarioKey();
    if (!state || key == state->scenarioKey) return false;
    state->scenarioKey = key;

    SavedHudPlacement saved = LoadHudPlacement(key);
    state->manualPosition = saved.found;
    state->manualWidth = saved.found;
    if (saved.found) {
        state->x = saved.x;
        state->y = saved.y;
        if (state->horizontal) {
            state->manualItemWidth = std::max(1, saved.width);
            size_t n = state->entries.size();
            int buttonReserve = state->showNewWindowButton ? (state->rowHeight + kProjectListGap) : 0;
            state->width = (n > 0 ? (int)n * state->manualItemWidth + (int)(n - 1) * kProjectListGap
                                   : state->manualItemWidth) +
                           buttonReserve;
        } else {
            state->width = std::max(saved.width, kProjectListMinWidth);
        }
    }
    return true;
}

static void ActivateProjectListItem(ProjectListHudState* state, int index) {
    if (!state || index < 0 || index >= (int)state->entries.size()) return;
    HWND target = state->entries[index].target;
    if (!target || !IsWindow(target)) return;

    if (!state->hoverFocusActive) {
        state->previousForeground = GetForegroundWindow();
        state->hoverFocusActive = true;
    }

    if (IsIconic(target)) ShowWindow(target, SW_RESTORE);
    SetForegroundWindow(target);
}

// Resolves whichever VS Code build the tracked windows belong to (stable
// vs. Insiders) -- from an already-tracked window's own process image -- to
// that install's `code`/`code-insiders` CLI shim (bin\*.cmd next to the
// exe). Shared by OpenNewVSCodeWindow and OpenFavouriteProject below.
//
// Passing -n to Code.exe directly does NOT work: the raw exe rejects it
// ("bad option: -n") and exits immediately -- ShellExecuteW still reports
// success (the process did launch), which is what made this silently no-op
// rather than fail loudly. The CLI shim is what actually supports -n: it
// sets ELECTRON_RUN_AS_NODE=1 and re-invokes Code.exe against its internal
// cli.js (itself under a version-hash-named subfolder the shim resolves
// relatively) to forward the request to the already-running instance over
// IPC. Reusing the shim sidesteps reimplementing (and keeping in sync) that
// internal path resolution here.
static bool ResolveVSCodeCliShim(const ProjectListHudState* state, std::wstring& outCmdPath) {
    if (!state || state->entries.empty()) return false;
    HWND target = state->entries[0].target;
    if (!target || !IsWindow(target)) return false;

    DWORD pid = 0;
    GetWindowThreadProcessId(target, &pid);
    if (!pid) return false;
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProcess) return false;
    wchar_t exePath[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    bool ok = QueryFullProcessImageNameW(hProcess, 0, exePath, &size) != 0;
    CloseHandle(hProcess);
    if (!ok) return false;

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

// Launches a brand-new, empty VS Code window -- the project list HUD's "+"
// button.
static void OpenNewVSCodeWindow(const ProjectListHudState* state) {
    std::wstring cmdPath;
    if (!ResolveVSCodeCliShim(state, cmdPath)) return;

    // SW_HIDE so the shim's own cmd.exe console doesn't flash on screen --
    // it only forwards the request to the already-running instance over IPC
    // and exits almost immediately either way.
    HINSTANCE result = ShellExecuteW(nullptr, L"open", cmdPath.c_str(), L"-n", nullptr, SW_HIDE);
    if ((INT_PTR)result <= 32) {
        Log(L"ShellExecuteW(open new vscode window via %ls) failed, code=%Id", cmdPath.c_str(), (INT_PTR)result);
    }
}

// Opens `path` as a folder in a brand-new VS Code window -- same CLI shim
// as OpenNewVSCodeWindow, just with the favourite's path forwarded as the
// folder to open (quoted, since it may contain spaces).
static void OpenFavouriteProject(const ProjectListHudState* state, const std::wstring& path) {
    std::wstring cmdPath;
    if (!ResolveVSCodeCliShim(state, cmdPath)) return;

    std::wstring args = L"-n \"" + path + L"\"";
    HINSTANCE result = ShellExecuteW(nullptr, L"open", cmdPath.c_str(), args.c_str(), nullptr, SW_HIDE);
    if ((INT_PTR)result <= 32) {
        Log(L"ShellExecuteW(open favourite [%ls] via %ls) failed, code=%Id", path.c_str(), cmdPath.c_str(),
            (INT_PTR)result);
    }
}

static void EndProjectListHoverFocus(ProjectListHudState* state, bool restorePrevious) {
    if (!state || !state->hoverFocusActive) return;
    HWND previous = state->previousForeground;
    state->hoverFocusActive = false;
    state->previousForeground = nullptr;

    if (restorePrevious && previous && IsWindow(previous)) {
        if (IsIconic(previous)) ShowWindow(previous, SW_RESTORE);
        SetForegroundWindow(previous);
    }
}

static std::wstring TrimWhitespace(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return L"";
    size_t b = s.find_last_not_of(L" \t\r\n");
    return s.substr(a, b - a + 1);
}

// Ends the in-progress alias edit (if any): on commit, saves the trimmed
// text as the new alias for that item's rawLabel (an empty result removes
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
        const std::wstring& rawLabel = state->entries[index].rawLabel;
        SetAlias(rawLabel, newAlias);
        state->entries[index].label = newAlias.empty() ? rawLabel : newAlias;
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
    SetForegroundWindow(edit);
    SetFocus(edit);

    state->editControl = edit;
    state->editFont = font;
    state->editIndex = index;
}

// Clears whatever alias is set for item `index`, reverting its displayed
// text back to the raw label -- the same end result as BeginAliasEdit
// followed by committing an empty string, just without opening the edit
// box first.
static void ResetAlias(HWND hud, ProjectListHudState* state, int index) {
    if (!state || index < 0 || index >= (int)state->entries.size()) return;
    const std::wstring& rawLabel = state->entries[index].rawLabel;
    SetAlias(rawLabel, L"");
    state->entries[index].label = rawLabel;
    RenderProjectListHud(hud, state);
}

static void ShowItemContextMenu(HWND hud, ProjectListHudState* state, int index, POINT screenPt) {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, 1, L"Set Alias...");
    bool hasAlias = index >= 0 && index < (int)state->entries.size() &&
                    state->entries[index].label != state->entries[index].rawLabel;
    if (hasAlias) AppendMenuW(menu, MF_STRING, 2, L"Reset Alias");

    // No path means VS Code hasn't recorded this window's folder in its own
    // workspaceStorage (e.g. a multi-root workspace) -- nothing a favourite
    // could reopen later, so the option is left off entirely rather than
    // shown disabled. Once a folder IS already a favourite, this toggles to
    // "Remove from Favourites" instead -- same removal is also still offered
    // from the "+" button's own menu (see ShowNewWindowButtonContextMenu),
    // for a favourite that isn't currently open as a hub item.
    bool hasPath = index >= 0 && index < (int)state->entries.size() && !state->entries[index].path.empty();
    bool alreadyFav = hasPath && IsFavourite(state->entries[index].path);
    if (hasPath) {
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr); // divides the alias group above from the favourites
                                                       // group below
        AppendMenuW(menu, MF_STRING, alreadyFav ? 4 : 3, alreadyFav ? L"Remove from Favourites" : L"Add to Favourites");
    }

    SetForegroundWindow(hud); // required so the menu dismisses correctly on an outside click
    // Deliberately no TPM_RIGHTBUTTON: that flag restricts item *selection*
    // to the right mouse button, but the universal convention (and the only
    // thing a user would naturally try) is right-click to open, then
    // left-click to pick an item -- TPM_RIGHTBUTTON would silently eat that.
    state->contextMenuOpen = true; // see UpdateProjectListHud's guard
    int cmd = TrackPopupMenu(menu, TPM_RETURNCMD, screenPt.x, screenPt.y, 0, hud, nullptr);
    state->contextMenuOpen = false;
    DestroyMenu(menu);
    if (cmd == 1) BeginAliasEdit(hud, state, index);
    else if (cmd == 2) ResetAlias(hud, state, index);
    else if (cmd == 3) {
        const ProjectListHudEntry& entry = state->entries[index];
        AddFavourite(entry.label, entry.path);
    } else if (cmd == 4) {
        RemoveFavourite(state->entries[index].path);
    }
}

// Right-click on the fixed "+" new-window button: lists saved favourite
// projects (see favourites.h), each as its own submenu -- hovering a
// favourite's name expands it to "Open in New Window" (the default/bolded
// action, see OpenFavouriteProject) and "Remove from Favourites". A plain
// Win32 popup menu item can't both carry its own click action and expand a
// submenu on hover, so nesting "open" one level down (rather than a bare
// top-level click) is what buys room for "remove" to live right next to it,
// without a separate top-level "Remove Favourite" list to keep in sync
// with this one. This is the only way to open (or remove) a favourite that
// isn't currently open as a hub item -- ShowItemContextMenu's own "Remove
// from Favourites" only reaches a favourite while it's a live entry.
static void ShowNewWindowButtonContextMenu(HWND hud, ProjectListHudState* state, POINT screenPt) {
    std::vector<FavouriteProject> favourites = LoadFavourites();

    HMENU menu = CreatePopupMenu();
    if (favourites.empty()) {
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"(No Favourites)");
    } else {
        // Command ids per favourite i: open = 1 + i*2, remove = 2 + i*2 --
        // decoded back on return below.
        for (size_t i = 0; i < favourites.size(); i++) {
            UINT openId = (UINT)(1 + i * 2);
            UINT removeId = (UINT)(2 + i * 2);
            HMENU sub = CreatePopupMenu();
            AppendMenuW(sub, MF_STRING, openId, L"Open in New Window");
            AppendMenuW(sub, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(sub, MF_STRING, removeId, L"Remove from Favourites");
            SetMenuDefaultItem(sub, openId, FALSE);
            AppendMenuW(menu, MF_POPUP, (UINT_PTR)sub, favourites[i].label.c_str());
        }
    }

    SetForegroundWindow(hud);
    state->contextMenuOpen = true; // see UpdateProjectListHud's guard
    int cmd = TrackPopupMenu(menu, TPM_RETURNCMD, screenPt.x, screenPt.y, 0, hud, nullptr);
    state->contextMenuOpen = false;
    DestroyMenu(menu); // recursively destroys the per-favourite submenus too

    if (cmd <= 0) return;
    size_t i = (size_t)(cmd - 1) / 2;
    bool isOpen = (cmd - 1) % 2 == 0;
    if (i >= favourites.size()) return;
    if (isOpen) OpenFavouriteProject(state, favourites[i].path);
    else RemoveFavourite(favourites[i].path);
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
    for (const ProjectListHudEntry& e : state->entries) order.push_back(e.rawLabel);
    state->manualOrder = order;
    SaveItemOrder(order);
    if (state->dragGhost) ShowWindow(state->dragGhost, SW_HIDE);
    RenderProjectListHud(hwnd, state);
    SetCursor(LoadCursorW(nullptr, ProjectListCursorForPoint(state, mouseX, mouseY)));
}

// Shared cleanup for ending a DragMove/DragResizeLeft/DragResizeRight, same
// two call sites as EndReorderDrag above.
static void EndMoveResizeDrag(HWND hwnd, ProjectListHudState* state, int mouseX, int mouseY) {
    state->dragMode = ProjectListHudState::DragNone;
    if (GetCapture() == hwnd) ReleaseCapture();
    if (state->manualPosition) {
        int savedWidth = state->horizontal ? state->manualItemWidth : state->width;
        SaveHudPlacement(state->scenarioKey, state->x, state->y, savedWidth);
    }
    SetCursor(LoadCursorW(nullptr, ProjectListCursorForPoint(state, mouseX, mouseY)));
}

static LRESULT CALLBACK ProjectListHudWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    ProjectListHudState* state = (ProjectListHudState*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

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
            if (state->manualOrderMode && state->dragMode == ProjectListHudState::DragNone &&
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
                int candidate = ProjectListDragCandidate(state, mouseX, mouseY);
                // Confine the drop target to the dragged item's own zone
                // (favourite vs. regular) so a drag can reorder within
                // either group but can never cross the boundary -- that's
                // what keeps favourites pinned to the front (see
                // PinFavouritesToFront) even while drag-and-drop is live.
                int favZone = FavouriteZoneCount(state->entries);
                bool draggedIsFavourite = state->reorderIndex < favZone;
                candidate = draggedIsFavourite ? std::min(candidate, favZone - 1)
                                                : std::max(candidate, favZone);
                if (candidate >= 0 && candidate < (int)state->entries.size() && candidate != state->reorderIndex) {
                    ProjectListHudEntry dragged = state->entries[state->reorderIndex];
                    state->entries.erase(state->entries.begin() + state->reorderIndex);
                    state->entries.insert(state->entries.begin() + candidate, dragged);
                    state->reorderIndex = candidate;
                    if (state->horizontal) RebuildHorizontalItemRects(state);
                    else RebuildVerticalItemRects(state);
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
                POINT pt;
                GetCursorPos(&pt);
                int dx = pt.x - state->dragStart.x;
                int dy = pt.y - state->dragStart.y;
                int left = state->dragStartRect.left;
                int top = state->dragStartRect.top;
                int right = state->dragStartRect.right;
                int bottom = state->dragStartRect.bottom;

                if (state->dragMode == ProjectListHudState::DragMove) {
                    left += dx;
                    top += dy;
                    right += dx;
                    bottom += dy;
                    state->manualPosition = true;
                } else if (state->dragMode == ProjectListHudState::DragResizeLeft) {
                    left = std::min(left + dx, right - kProjectListMinWidth);
                    state->manualPosition = true;
                    state->manualWidth = true;
                } else if (state->dragMode == ProjectListHudState::DragResizeRight) {
                    right = std::max(right + dx, left + kProjectListMinWidth);
                    state->manualPosition = true;
                    state->manualWidth = true;
                }

                int newWidth = right - left;
                bool sizeChanged = newWidth != state->width;
                state->x = left;
                state->y = top;
                state->width = newWidth;
                state->height = bottom - top;
                if (state->horizontal) {
                    // Cache what per-item width this drag corresponds to
                    // *right now* (entries.size() is fixed for the duration
                    // of a single drag) -- this, not state->width, is what
                    // gets remembered on WM_LBUTTONUP.
                    size_t n = state->entries.size();
                    int buttonReserve = state->showNewWindowButton ? (state->rowHeight + kProjectListGap) : 0;
                    state->manualItemWidth =
                        n > 0 ? std::max(1, (newWidth - buttonReserve - (int)(n - 1) * kProjectListGap) / (int)n)
                              : newWidth;
                }
                if (sizeChanged) {
                    if (state->horizontal) RebuildHorizontalItemRects(state);
                    else RebuildVerticalItemRects(state);
                    RenderProjectListHud(hwnd, state);
                }
                PositionProjectListHud(hwnd, state);
                SetCursor(LoadCursorW(nullptr, state->dragMode == ProjectListHudState::DragMove ? IDC_SIZEALL : IDC_SIZEWE));
                return 0;
            }

            int hoverIndex = ProjectListHitTest(state, mouseX, mouseY);
            if (hoverIndex != state->hoverIndex) {
                state->hoverIndex = hoverIndex;
                RenderProjectListHud(hwnd, state);
                if (state->activateOnHover && hoverIndex >= 0) ActivateProjectListItem(state, hoverIndex);
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
            // move/resize drag immediately, mirroring the vertical style's
            // old right-click-drag gesture but freeing up plain right-click
            // for the context menu below.
            if (state && (GetKeyState(VK_CONTROL) & 0x8000)) {
                state->dragMode = ProjectListDragModeForPoint(state, GET_X_LPARAM(lParam));
                GetCursorPos(&state->dragStart);
                GetWindowRect(hwnd, &state->dragStartRect);
                SetCapture(hwnd);
                SetCursor(LoadCursorW(nullptr, state->dragMode == ProjectListHudState::DragMove ? IDC_SIZEALL : IDC_SIZEWE));
            } else if (state) {
                // The new-window button (hit-tested as the sentinel index
                // entries.size(), see ProjectListHitTest) is deliberately
                // excluded here: it's fixed at the end and never
                // reorder-draggable, so it needs no pending-drag tracking --
                // its click is handled entirely on WM_LBUTTONUP via
                // hoverIndex, same as a plain item click.
                int index = ProjectListHitTest(state, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
                if (index >= 0 && index < (int)state->entries.size()) {
                    state->pendingDragIndex = index;
                    GetCursorPos(&state->dragStart);
                }
            }
            return 0;
        case WM_LBUTTONUP:
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
                ActivateProjectListItem(state, state->hoverIndex);
                EndProjectListHoverFocus(state, false);
            } else if (state && state->showNewWindowButton && state->hoverIndex == (int)state->entries.size()) {
                OpenNewVSCodeWindow(state);
            }
            return 0;
        case WM_RBUTTONUP:
            if (state) {
                int index = ProjectListHitTest(state, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
                if (index >= 0 && index < (int)state->entries.size()) {
                    POINT screenPt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                    ClientToScreen(hwnd, &screenPt);
                    ShowItemContextMenu(hwnd, state, index, screenPt);
                } else if (state->showNewWindowButton && index == (int)state->entries.size()) {
                    // The new-window button (sentinel index entries.size(),
                    // see ProjectListHitTest) has no alias to set -- its own
                    // right-click menu is the favourites list instead.
                    POINT screenPt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                    ClientToScreen(hwnd, &screenPt);
                    ShowNewWindowButtonContextMenu(hwnd, state, screenPt);
                }
            }
            return 0;
        case WM_DISPLAYCHANGE:
            if (state && ApplyScenarioForCurrentMonitors(state) && state->manualPosition) {
                // Only need to force an immediate refresh when we just
                // snapped to a remembered placement -- if this scenario has
                // no saved placement, manualPosition/manualWidth are now
                // false and the next regular UpdateProjectListHud call
                // (already running periodically) will recompute the default
                // auto-placement on its own.
                if (state->horizontal) RebuildHorizontalItemRects(state);
                else RebuildVerticalItemRects(state);
                RenderProjectListHud(hwnd, state);
                PositionProjectListHud(hwnd, state);
            }
            return 0;
        case WM_TIMER:
            if (wParam == kClaudePulseTimerId && state) RenderProjectListHud(hwnd, state);
            return 0;
        case WM_MOUSELEAVE:
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
            // mid-drag arrangement it had reached -- unpersisted, so the
            // next regular sync reapplies the last *saved* manualOrder and
            // self-corrects the visual back to it.
            if (state) {
                state->dragMode = ProjectListHudState::DragNone;
                state->pendingDragIndex = -1;
                state->reorderIndex = -1;
                if (state->dragGhost) ShowWindow(state->dragGhost, SW_HIDE);
            }
            return 0;
        case WM_NCDESTROY:
            if (state && state->dragGhost) DestroyWindow(state->dragGhost);
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

HWND CreateProjectListHud(HINSTANCE hInstance, bool horizontal) {
    RegisterProjectListHudClass(hInstance);
    HWND hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kProjectListHudClassName, L"", WS_POPUP,
        0, 0, 1, 1, nullptr, nullptr, hInstance, nullptr);
    if (!hwnd) return nullptr;
    ProjectListHudState* state = new ProjectListHudState();
    state->horizontal = horizontal; // must be right before ApplyScenarioForCurrentMonitors below, since
                                     // it decides whether a saved width means "per-item" or "whole strip"
    state->manualOrder = LoadItemOrder();
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
    state->dragGhost = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kProjectListGhostClassName, L"", WS_POPUP,
        0, 0, 1, 1, nullptr, nullptr, hInstance, nullptr);

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

// Draws one entry's chip (background + label text, alpha-blended by
// `opacity`) into `pixels` at `item`. `item` may fall partly or entirely
// outside [0,width)x[0,height) -- used for the floating dragged item
// during DragReorder, which follows the cursor freely -- so bounds are
// clamped explicitly (BlendTextIntoPixels already does its own clamping
// internally).
static void DrawHudItem(HDC screenDC, UINT32* pixels, int width, int height, int rowHeight,
                        const ProjectListHudEntry& entry, const RECT& item, bool highlighted, int opacity,
                        bool isFavourite, bool labelTextColorAuto, COLORREF labelTextColor, HFONT font,
                        HFONT hoverFont, COLORREF claudeColorWorking, COLORREF claudeColorAttention,
                        COLORREF claudeColorWaiting, bool claudeBorderColorAuto, COLORREF claudeBorderColor) {
    const int paddingX = 12;
    int leftTextPad = isFavourite ? kFavouriteMarkerLeftPad : paddingX;
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

    int rowStart = std::max((int)item.top, 0), rowEnd = std::min((int)item.bottom, height);
    int colStart = std::max((int)item.left, 0), colEnd = std::min((int)item.right, width);
    for (int row = rowStart; row < rowEnd; row++) {
        for (int col = colStart; col < colEnd; col++) pixels[row * width + col] = chipPx;
    }

    // Favourites star marker, left edge -- drawn before the opacity pass
    // below so it fades with the rest of the chip on hover/normal opacity,
    // same reasoning as the Claude status indicator's placement.
    if (isFavourite) DrawFavouriteMarker(pixels, width, height, item, color);

    COLORREF tx = labelTextColorAuto ? ContrastTextColor(color) : labelTextColor;
    BlendTextIntoPixels(screenDC, pixels, width, height, item.left + leftTextPad, item.top,
                        itemWidth - leftTextPad - paddingX, rowHeight, entry.label, highlighted ? hoverFont : font,
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
    if (entry.claudeStatus == ClaudeStatus::Working) {
        // Ring spinner: 8 cells in a 3x3 grid with an empty center, only
        // one lit at a time, advancing one step per pulse timer tick --
        // reads as a single square chasing itself around the ring. (A
        // brightness pulse on a single dot was tried first and judged too
        // subtle to notice at a glance.)
        COLORREF dotColor = claudeColorWorking;
        UINT32 dotPx = (UINT32(255) << 24) | (UINT32(GetRValue(dotColor)) << 16) |
                      (UINT32(GetGValue(dotColor)) << 8) | UINT32(GetBValue(dotColor));
        const int cell = 4, gap = 1, stride = cell + gap, ringSpan = 3 * cell + 2 * gap; // 14x14 overall
        // {col, row} in the 3x3 grid, in clockwise perimeter order (each
        // step moves to an *adjacent* cell -- indexing the grid in plain
        // row-major order instead jumps straight from top-right to
        // mid-left, breaking the "chasing around a ring" illusion).
        static const int kRingCells[8][2] = {
            {0, 0}, {1, 0}, {2, 0}, {2, 1}, {2, 2}, {1, 2}, {0, 2}, {0, 1},
        };
        int activeIndex = (int)((GetTickCount64() / kClaudePulseIntervalMs) % 8);
        int baseX = (int)item.right - dotMargin - ringSpan;
        int baseY = (int)item.top + dotMargin;
        int sqLeft = baseX + kRingCells[activeIndex][0] * stride;
        int sqTop = baseY + kRingCells[activeIndex][1] * stride;
        drawBorderedSquare(sqLeft, sqTop, cell, dotPx);
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

// Draws the fixed new-window button: same chip fill / highlight-brighten /
// opacity treatment as DrawHudItem, but a centered "+" glyph instead of a
// left-aligned label, and no AI status indicator.
static void DrawNewWindowButton(HDC screenDC, UINT32* pixels, int width, int height, const RECT& item,
                                COLORREF color, bool highlighted, int opacity, HFONT font, HFONT hoverFont,
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
                        item.bottom - item.top, L"+", highlighted ? hoverFont : font, chipColor, tx,
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

static void RenderProjectListHud(HWND hud, ProjectListHudState* state) {
    if (!state || state->width <= 0 || state->height <= 0 || state->entries.empty()) return;

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
    HFONT hoverFont = CreateHudFont(state->fontSize, FW_BLACK);

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
                    state->labelTextColor, font, hoverFont, state->claudeColorWorking, state->claudeColorAttention,
                    state->claudeColorWaiting, state->claudeBorderColorAuto, state->claudeBorderColor);
    }

    if (state->showNewWindowButton) {
        bool highlighted = state->hoverIndex == (int)state->entries.size();
        int opacity = highlighted ? state->hoverOpacity : state->normalOpacity;
        DrawNewWindowButton(screenDC, pixels, width, height, state->newWindowButtonRect,
                            state->newWindowButtonColor, highlighted, opacity, font, hoverFont,
                            state->labelTextColorAuto, state->labelTextColor);
    }

    DeleteObject(font);
    DeleteObject(hoverFont);

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
    HFONT hoverFont = CreateHudFont(state->fontSize, FW_BLACK);
    RECT localRect = {0, 0, itemW, itemH}; // ghost's own bitmap -- always local-origin, unlike the
                                            // screen-position SetWindowPos call below
    // Drawn "highlighted" (brightened + bold, like a hovered item) so the
    // item you're actively moving reads as visually active.
    DrawHudItem(screenDC, pixels, itemW, itemH, state->rowHeight, state->entries[state->reorderIndex],
                localRect, true, state->hoverOpacity, EntryIsFavourite(state->entries[state->reorderIndex]),
                state->labelTextColorAuto, state->labelTextColor, font, hoverFont, state->claudeColorWorking,
                state->claudeColorAttention, state->claudeColorWaiting, state->claudeBorderColorAuto,
                state->claudeBorderColor);
    DeleteObject(font);
    DeleteObject(hoverFont);

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

void HideProjectListHud(HWND hud) {
    if (hud) ShowWindow(hud, SW_HIDE);
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
        int reserve = kProjectListMeasurePaddingX + (EntryIsFavourite(entry) ? (kFavouriteMarkerLeftPad - 12) : 0);
        GetTextExtentPoint32W(screenDC, entry.label.c_str(), (int)entry.label.size(), &textSz);
        width = std::max(width, (int)textSz.cx + reserve);
    }
    SelectObject(screenDC, oldFont);
    DeleteObject(font);
    ReleaseDC(nullptr, screenDC);
    return ClampInt(width, kProjectListMinWidth, kProjectListAutoMaxWidth);
}

// Rearranges `entries` to match `order` (a list of rawLabels in the user's
// last-dragged order): entries whose rawLabel appears in `order` come
// first, in that relative order; any not found (e.g. a project the user
// has never dragged, so it has no saved position) are appended afterward,
// oldest-tracked first (trackSeq) -- i.e. plain append order, not wherever
// Windows happened to put that window on screen.
static void ApplyManualOrder(std::vector<ProjectListHudEntry>& entries, const std::vector<std::wstring>& order) {
    std::stable_sort(entries.begin(), entries.end(),
                     [&order](const ProjectListHudEntry& a, const ProjectListHudEntry& b) {
        auto ia = std::find(order.begin(), order.end(), a.rawLabel);
        auto ib = std::find(order.begin(), order.end(), b.rawLabel);
        bool aFound = ia != order.end(), bFound = ib != order.end();
        if (aFound != bFound) return aFound;
        if (aFound) return (ia - ib) < 0;
        return a.trackSeq < b.trackSeq;
    });
}

void UpdateProjectListHud(HWND hud, const std::vector<ProjectListHudEntry>& entries,
                          const ProjectListHudStyle& style) {
    if (entries.empty()) return;

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

    std::vector<ProjectListHudEntry> sorted = entries;
    std::sort(sorted.begin(), sorted.end(), [](const ProjectListHudEntry& a, const ProjectListHudEntry& b) {
        if (a.windowRect.left != b.windowRect.left) return a.windowRect.left < b.windowRect.left;
        if (a.windowRect.top != b.windowRect.top) return a.windowRect.top < b.windowRect.top;
        return a.target < b.target;
    });
    state->manualOrderMode = style.manualOrder;
    if (style.manualOrder) ApplyManualOrder(sorted, state->manualOrder);

    // Keep a favourite's stored label in sync with its live hub item
    // whenever that project is actually open -- otherwise the favourites
    // menu (ShowNewWindowButtonContextMenu) would keep showing whatever
    // label happened to be captured back when "Add to Favourites" was
    // clicked, even after the hub item's own label later changes (e.g. the
    // window title reparses differently once VS Code picks up this app's
    // window.title setting, or the user sets/changes an alias).
    for (const ProjectListHudEntry& e : sorted) {
        if (!e.path.empty()) RefreshFavouriteLabel(e.path, e.label);
    }

    PinFavouritesToFront(sorted);

    int rowHeight = std::max(18, style.rowHeight);
    state->rowHeight = rowHeight;
    state->entries = sorted;
    if (state->horizontal != style.horizontal) {
        // A manually-set size from one style doesn't translate to the
        // other -- horizontal's per-item width and vertical's shared
        // column width aren't the same concept, so reusing either one
        // as-is after a style switch (e.g. via config reload) would size
        // things nonsensically. Position stays valid either way, just the
        // width falls back to auto-fit until the user resizes again.
        state->manualWidth = false;
        state->manualItemWidth = 0;
    }
    state->horizontal = style.horizontal;
    state->showNewWindowButton = style.showNewWindowButton;
    state->newWindowButtonColor = style.newWindowButtonColor;
    int buttonReserve = state->showNewWindowButton ? (rowHeight + kProjectListGap) : 0;
    int totalHeight;

    if (style.horizontal) {
        // Every item shares one width (like MeasureRequiredWidth's shared
        // column width for the vertical style), sized to fit the widest
        // label when not manually resized. When manually resized, reuse the
        // remembered per-item width (manualItemWidth) rather than whatever
        // state->width happened to be -- state->width is a function of both
        // that and the current entry count, so re-deriving it here is what
        // keeps every item the same width no matter how many windows are
        // currently tracked. The new-window button, when shown, gets a
        // fixed square slot on top of that (see RebuildHorizontalItemRects).
        int itemWidth = !state->manualWidth ? MeasureRequiredWidth(sorted, style.fontSize)
                                             : std::max(1, state->manualItemWidth);
        state->width = (int)sorted.size() * itemWidth + (int)(sorted.size() - 1) * kProjectListGap + buttonReserve;
        RebuildHorizontalItemRects(state);
        totalHeight = rowHeight;
    } else {
        if (!state->manualWidth) {
            state->width = MeasureRequiredWidth(sorted, style.fontSize);
        } else {
            state->width = std::max(state->width, kProjectListMinWidth);
        }
        RebuildVerticalItemRects(state);
        totalHeight = (int)sorted.size() * rowHeight + ((int)sorted.size() - 1) * kProjectListGap + buttonReserve;
    }
    state->height = totalHeight;

    if (!state->manualPosition) {
        RECT workArea = {};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
        state->x = workArea.right - state->width;
        state->y = workArea.bottom - state->height - kProjectListBottomMargin;
    }

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
    // The button's hover "index" is the sentinel sorted.size() (see
    // ProjectListHitTest) -- one past the last valid entry index, not out of
    // range, when it's shown.
    int maxHoverIndex = (int)sorted.size() - 1 + (state->showNewWindowButton ? 1 : 0);
    if (state->hoverIndex > maxHoverIndex) state->hoverIndex = -1;

    // Both the Working ring spinner and the Attention pulse need a repaint
    // every kClaudePulseIntervalMs to actually animate, but only while
    // there's something to animate -- started/stopped here so an idle
    // desktop (or one where nothing's currently animating) never pays for
    // it, same on-demand pattern as tracking.cpp's foreground-poll timer.
    bool anyAnimating = std::any_of(sorted.begin(), sorted.end(), [](const ProjectListHudEntry& e) {
        return e.claudeStatus == ClaudeStatus::Working || e.claudeStatus == ClaudeStatus::Attention;
    });
    if (anyAnimating && !state->pulseTimerActive) {
        SetTimer(hud, kClaudePulseTimerId, kClaudePulseIntervalMs, nullptr);
        state->pulseTimerActive = true;
    } else if (!anyAnimating && state->pulseTimerActive) {
        KillTimer(hud, kClaudePulseTimerId);
        state->pulseTimerActive = false;
    }

    RenderProjectListHud(hud, state);
    PositionProjectListHud(hud, state);
}