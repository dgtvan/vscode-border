#pragma once

#include <windows.h>

#include <string>
#include <vector>

// Whether Claude Code is currently running in this window's integrated
// terminal(s) -- see claude_status.h. When a window has more than one
// Claude Code session, the aggregate is whichever of these is highest in
// this list: Attention (blocked on a permission/MCP prompt -- needs the
// user right now) outranks Working (actively generating/running a tool),
// which outranks Waiting (a finished turn, idle, ready for the next
// prompt).
enum class ClaudeStatus { None, Attention, Working, Waiting };

struct ProjectListHudEntry {
    HWND target = nullptr;
    RECT windowRect = {}; // target's on-screen bounds -- used to order entries to match window layout
    long long trackSeq = 0; // this window's track order, oldest first -- the append order for a manual-mode
                             // entry that has no saved position yet (see ApplyManualOrder in
                             // project_list_hud.cpp)
    std::wstring label;    // display text (an alias, if the user set one for aliasKey -- see label_alias.h)
    std::wstring rawLabel; // the un-aliased label
    std::wstring aliasKey; // key for this window's alias and manual-order slot -- rawLabel, or a
                            // session-only per-window key when no folder is open (see label_alias.h)
    std::wstring path;     // absolute folder path this window has open, resolved via worktree_resolver's
                            // ResolveFolderPath -- empty if VS Code hasn't recorded it (e.g. a plain
                            // folder it's never logged to workspaceStorage, or a multi-root workspace).
                            // Used only by the item context menu: "Add to Favourites" (see favourites.h),
                            // "Copy Directory Path" and "Open Directory in File Explorer".
    COLORREF color = RGB(0, 0, 0);
    ClaudeStatus claudeStatus = ClaudeStatus::None;
    std::wstring wslDistro; // distro a WSLg window's Linux VS Code runs in (see wsl.h), empty for Windows
                            // VS Code -- a WSLg entry shows the WSL icon in front of its label
    std::wstring account;   // Windows account the window's VS Code runs as, when that isn't this app's own
                            // (see account.h), else empty
    std::wstring iconAccount; // whose icon to show in front of the label (see IconAccountFor), empty for none
    // True while rawLabel might still change (VS Code's title/worktree
    // mapping still settling -- see tracking.cpp's IsWindowLoading). Drawn
    // greyed out with a spinner instead of its real color/AI status, and
    // can't be alias-edited or drag-reordered until this clears, since both
    // of those are keyed by rawLabel.
    bool loading = false;
};

// One account's Claude usage (see claude_usage.h), drawn in a strip of its own
// at the right end of the hub's band as twin rings -- outer weekly, inner session -- with the account's
// icon in front by the same rule as the items (see IconAccountFor). Hovering
// shows `tooltip`.
struct ProjectListHudUsageRing {
    std::wstring iconAccount;   // whose icon to show in front, empty for none
    double sessionPercent = -1; // -1 when unknown
    double weeklyPercent = -1;
    bool stale = false; // drawn grey: the numbers aren't current (see IsClaudeUsageStale)
    bool needsYou = false; // can't be read until someone acts (see ClaudeUsageSnapshot::needsYou): drawn as
                           // a grey ring around a pulsing amber "!" instead of any numbers
    bool hasChipColor = false; // the account's own background (its profile's _color); none: transparent
    COLORREF chipColor = RGB(0, 0, 0);
    std::wstring tooltip;
};

struct ProjectListHudStyle {
    bool manualOrder = false; // true = user can drag items to reorder (see project_list_order.h);
                              // false = always sorted by window left edge
    int rowHeight = 18;
    int fontSize = 13;
    bool labelTextColorAuto = false;
    COLORREF labelTextColor = RGB(0, 0, 0);
    int normalOpacity = 255;
    int hoverOpacity = 255;
    bool activateOnHover = false;
    // AI status indicator colors (see claude_status.h / ClaudeStatus above)
    // -- Claude-named internally since Claude is still the only implemented
    // data source, even though the user-facing config keys are the generic
    // `ai_indicator_*` (see config.h).
    COLORREF claudeColorWorking = RGB(255, 159, 10);
    COLORREF claudeColorAttention = RGB(220, 38, 38);
    COLORREF claudeColorWaiting = RGB(52, 199, 89);
    bool claudeBorderColorAuto = true;
    COLORREF claudeBorderColor = RGB(255, 255, 255);
    // Fixed square buttons appended after the last entry: "+" opens a new
    // VS Code window, then minimize-all and close-all (the latter asks for
    // confirmation first) -- those two only while there's at least one entry,
    // and currently hidden altogether (see kShowBulkWindowButtons in the .cpp).
    // Unlike the entries above, they can't be drag-reordered and never
    // trigger activateOnHover. `newWindowButtonColor` colors the "+" and
    // is picked by the caller (e.g. the next color in tracking.cpp's
    // round-robin palette allocation) since this module has no notion of a
    // color palette of its own.
    bool showNewWindowButton = false;
    COLORREF newWindowButtonColor = RGB(0, 0, 0);
    std::vector<ProjectListHudUsageRing> usageRings; // in display order; none while the usage meter is off
};

// Creates the hub hidden; it shows up once SetProjectListHudDocked has
// reserved its band and UpdateProjectListHud has given it something to show.
HWND CreateProjectListHud(HINSTANCE hInstance);

// Hides the HUD (config disabled, or nothing currently worth showing).
void HideProjectListHud(HWND hud);

// `docked` true (the hub is on) registers the HUD as a bottom-edge AppBar on
// the primary monitor, which reserves a band one item row tall directly
// above the taskbar -- maximized windows then stop above it -- and the HUD
// lives inside that band: Ctrl+drag resizes it and slides it sideways, but
// can't take it out of the band. `docked` false (the hub is off) releases
// the band; so does destroying the HUD. If the AppBar can't be registered,
// the hub stays hidden and the next call tries again. Cheap to call on
// every sync -- a no-op unless `docked` or `rowHeight` changed.
// `matchTaskbar` fills the whole band behind the HUD with a backdrop painted
// to match the taskbar (see taskbar_backdrop.h); otherwise the rest of the
// band shows the desktop.
void SetProjectListHudDocked(HWND hud, bool docked, int rowHeight, bool matchTaskbar);

// Sorts `entries` to match window layout (left-to-right, top-to-bottom) --
// or, if `style.manualOrder` is set, to the last order the user dragged
// them into (see project_list_order.h), with any entries never seen before
// appended at the end in their window-layout order -- then moves every
// favourite entry (see favourites.h) ahead of the non-favourites,
// preserving each group's relative order, so favourites always sit at the
// front of the list regardless of window layout or manual order --
// measures/sizes the HUD to fit them -- a single horizontal strip, every
// item sharing one width -- and shows it at the right end of its band (see
// SetProjectListHudDocked), unless the user has manually slid/resized it
// (Ctrl+left-click-drag either edge to resize, the middle to slide it; plain
// right-click opens a context menu instead, e.g. to set an alias for an
// item's label). Once manually placed/sized, that placement is kept on later
// calls instead of being recomputed. An empty `entries` is fine as long as
// style.showNewWindowButton is set -- the hub then shows that button on its
// own, which is what it looks like with every VS Code window closed; with
// the button off too there's nothing to show and this does nothing, so call
// HideProjectListHud in that case instead.
//
// When style.manualOrder is set, plain left-click-and-drag on an item (as
// opposed to a plain click, which activates it) reorders it: other items
// shift live to open a gap at the candidate drop position as the dragged
// item, shown floating, follows the cursor. The result is persisted
// immediately on drop. A dragged item is confined to its own favourite/
// non-favourite group -- it can be reordered within that group, but can't
// be dropped across the boundary into the other one, so the favourites-
// always-in-front rule above can't be dragged away.
void UpdateProjectListHud(HWND hud, const std::vector<ProjectListHudEntry>& entries,
                          const ProjectListHudStyle& style);