#include "tracking.h"

#include "ai_provider.h"
#include "config.h"
#include "focus_trace.h"
#include "label_alias.h"
#include "logger.h"
#include "overlay.h"
#include "project_list_hud.h"
#include "window_discovery.h"
#include "window_title.h"
#include "worktree_resolver.h"

#include <algorithm>
#include <cstdlib>
#include <cwchar>
#include <ctime>
#include <unordered_map>
#include <vector>

static HINSTANCE g_hInstance = nullptr;
static HWND g_ownerWnd = nullptr;
static HWND g_projectListHud = nullptr;
// Session-only "Hide Borders and Hub" tray toggle (see SetOverlaysHidden)
// -- layered on top of config.ini, not persisted, so every launch starts
// with everything shown.
static bool g_overlaysHidden = false;

const UINT_PTR kForegroundPollTimerId = 2;
static const UINT kForegroundPollIntervalMs = 50;

// A tracked window counts as "loading" (see IsWindowLoading) for up to
// this long after its current loading episode began -- first tracked, or
// the first NAMECHANGE seen after a prior episode had already ended. A
// single bounded window rather than a per-event recency check: VS Code's
// git extension can go completely silent for a while before firing the
// one NAMECHANGE that finally carries the resolved repo/branch, so
// resetting on a quiet gap flickered the item normal and back. Measured
// directly from this app's own logs on a real reload: title went from
// "no repo yet" to fully resolved "repo - branch - folder" in 3 separate
// NAMECHANGE bursts spanning ~11.5s total, with one silent gap of ~8.1s
// in the middle where nothing happened at all -- 4000ms (the previous
// value) expired mid-gap and caused exactly this flicker. Also covers a
// folder that plainly has no git repo at all (nothing will ever resolve,
// so this is simply how long that folder shows as loading before giving
// up), and a git extension that never finishes (e.g. disabled in
// settings) -- either way this is the absolute cap, not just a typical
// case estimate.
static const ULONGLONG kLoadingGraceMs = 20000;

struct TrackedWindow {
    HWND overlay = nullptr;
    int colorIndex = 0;
    int lastWidth = -1;
    int lastHeight = -1;
    DWORD pid = 0;
    std::wstring label;     // display text: rawLabel with any user-set alias applied (see label_alias.h)
    std::wstring rawLabel;  // folder name derived from the target's title -- the alias map's key
    std::wstring branch;    // ${activeRepositoryBranchName} from the title, empty outside a git repo --
                            // fills label_alias_format's <Branch> on the border's label chip (see BorderLabel)
    std::wstring folderName; // pre-worktree-substitution repo/folder name, i.e. what VS Code's own
                              // workspaceStorage records as the leaf folder name -- distinct from rawLabel,
                              // which shows the substituted *main* repo name for a worktree checkout (see
                              // ApplyLabelForTitle). Used only to resolve a real path via
                              // worktree_resolver's ResolveFolderPath (see SyncProjectListHud).
    std::wstring lastLabel; // label last painted, to detect changes
    RECT lastKnownRect = {}; // last on-screen bounds seen while not minimized -- lets a minimized
                              // window's project-list HUD entry keep sorting where it normally sits
                              // instead of jumping around (see SyncProjectListHud)
    long long trackSeq = 0; // this window's position in track order (see g_nextTrackSeq) -- lets manual
                             // mode append a project the user has never dragged (so it has no saved
                             // position) after the ones that do, instead of falling back to sorting it
                             // by on-screen position among other not-yet-ordered entries (see
                             // ApplyManualOrder). NOT a reliable "when was this actually opened" signal --
                             // e.g. multiple VS Code windows commonly share one OS process, so process
                             // creation time can't tell them apart either; see project_list_hud.cpp's
                             // ShowNewWindowButtonContextMenu for how the favourites menu instead derives
                             // open order from each entry's live position in the HUD.
    ULONGLONG loadingEpisodeStartTick = 0; // GetTickCount64() when the current loading episode began --
                                             // see kLoadingGraceMs / IsWindowLoading
    bool hasRepoInTitle = false; // whether the most recent title parse had a non-empty
                                  // ${activeRepositoryName} -- see IsWindowLoading
    bool hasFolderOrRepoInTitle = false; // whether the most recent title parse had either a repo or a
                                          // folder at all -- false only for a bare "File > New Window"
                                          // with nothing open, which will never gain one -- see
                                          // IsWindowLoading
};

static long long g_nextTrackSeq = 0;

// Single choke point for computing a tracked window's label fields, so
// every call site (initial track, name-change debounce, RefreshAllLabels)
// picks up alias resolution consistently. Also captures folderName (see its
// field comment) before any worktree substitution is applied to the
// display label.
static void ApplyLabelForTitle(TrackedWindow& tw, const std::wstring& title) {
    VSCodeTitleParts parts = ParseVSCodeTitle(title);
    tw.folderName = !parts.repo.empty() ? parts.repo : parts.folder;

    // Inside a git worktree checkout, VS Code's ${activeRepositoryName} is
    // the *worktree's* folder name rather than the main repo's -- substitute
    // the real main repo name (for display only) when the worktree_resolver
    // can map it.
    if (!parts.repo.empty()) {
        std::wstring mainRepo = ResolveMainRepoName(parts.repo);
        if (!mainRepo.empty()) parts.repo = mainRepo;
    }
    tw.hasRepoInTitle = !parts.repo.empty();
    tw.hasFolderOrRepoInTitle = !parts.repo.empty() || !parts.folder.empty();
    tw.rawLabel = BuildFolderLabel(parts);
    tw.branch = parts.branch;
    tw.label = ResolveAlias(tw.rawLabel);
}

// See kLoadingGraceMs: true while this window's rawLabel might still
// change out from under an alias or a manual reorder (both keyed by
// rawLabel) -- surfaced to the project list HUD as
// ProjectListHudEntry::loading (greyed out, alias/drag-reorder disabled).
static bool IsWindowLoading(const TrackedWindow& tw, HWND hwnd) {
    if (!tw.hasFolderOrRepoInTitle) {
        // A bare window with no folder or repo open at all -- e.g. "File >
        // New Window" -- has nothing for VS Code's git extension to ever
        // resolve, so there's no reason to wait out the grace period; it
        // would otherwise sit "loading" for the full kLoadingGraceMs for
        // no reason.
        LogFastDiag(L"loading-check hwnd=%p rawLabel=[%ls] hasFolderOrRepoInTitle=0 -> done", hwnd,
                    tw.rawLabel.c_str());
        return false;
    }
    if (tw.hasRepoInTitle) {
        LogFastDiag(L"loading-check hwnd=%p rawLabel=[%ls] hasRepoInTitle=1 -> done", hwnd, tw.rawLabel.c_str());
        return false; // title already shows repo/branch -- nothing left to wait for
    }
    ULONGLONG elapsed = GetTickCount64() - tw.loadingEpisodeStartTick;
    bool loading = elapsed < kLoadingGraceMs;
    LogFastDiag(L"loading-check hwnd=%p rawLabel=[%ls] hasRepoInTitle=0 elapsedMs=%llu -> %ls", hwnd,
                tw.rawLabel.c_str(), (unsigned long long)elapsed, loading ? L"loading" : L"done");
    return loading;
}

// Text for the border's label chip. Without an alias it's just tw.label
// (the raw "repo - branch"); with one, it's the config's label_alias_format
// with <Alias>/<Branch> (case-insensitive) substituted in a single pass, so
// an alias that itself contains "<Branch>" is left as typed -- by default
// "<alias> - <branch>", keeping the branch visible even though the alias
// replaces the raw label. A format that uses <Branch> falls back to just the
// alias outside a git repo (no branch), rather than leaving a dangling
// separator. The project list HUD shows tw.label as-is -- only the border
// uses the format.
static std::wstring BorderLabel(const TrackedWindow& tw) {
    if (tw.label == tw.rawLabel) return tw.label;

    const std::wstring& format = g_config.labelAliasFormat;
    std::wstring text;
    bool usesBranch = false;
    for (size_t i = 0; i < format.size();) {
        if (_wcsnicmp(format.c_str() + i, L"<Alias>", 7) == 0) {
            text += tw.label;
            i += 7;
        } else if (_wcsnicmp(format.c_str() + i, L"<Branch>", 8) == 0) {
            text += tw.branch;
            usesBranch = true;
            i += 8;
        } else {
            text += format[i++];
        }
    }
    if (usesBranch && tw.branch.empty()) return tw.label;
    return text;
}

static std::unordered_map<HWND, TrackedWindow> g_tracked; // target hwnd -> info
static std::vector<bool> g_colorInUse;
static int PeekNextColorIndex(); // defined below, alongside AllocateColorIndex -- forward-declared so
                                  // SyncProjectListHud (which appears first in the file) can use it

// True if `cwd` is `folderPath` itself, or somewhere underneath it (e.g. a
// Claude Code session started from a subdirectory of the opened folder) --
// checked with a trailing separator boundary so "...\myproject2" doesn't
// false-match a folderPath of "...\myproject".
static bool PathIsWithinFolder(const std::wstring& cwd, const std::wstring& folderPath) {
    if (folderPath.empty() || cwd.size() < folderPath.size()) return false;
    if (_wcsnicmp(cwd.c_str(), folderPath.c_str(), folderPath.size()) != 0) return false;
    if (cwd.size() == folderPath.size()) return true;
    wchar_t next = cwd[folderPath.size()];
    return next == L'\\' || next == L'/';
}

static std::wstring LastPathSegment(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

// Aggregates every AI session (see ai_provider.h) that belongs to this
// window's folder into one status, attention > working > waiting > none.
// Prefers a real path match via worktree_resolver's ResolveFolderPath
// (correct for worktrees and subdirectory cwds -- see folderName's field
// comment), falling back to a same-name comparison against just the last
// path segment when no path is on record, e.g. a plain folder VS Code has
// never logged to workspaceStorage.
static ClaudeStatus ComputeAiStatus(const TrackedWindow& tw, const std::vector<AiSessionStatus>& sessions) {
    if (sessions.empty() || tw.folderName.empty()) return ClaudeStatus::None;

    std::wstring folderPath = ResolveFolderPath(tw.folderName);
    LogDiag(L"ai_status: matching window folderName=[%ls] resolvedFolderPath=[%ls] against %zu session(s)",
            tw.folderName.c_str(), folderPath.c_str(), sessions.size());
    bool anyAttention = false, anyWorking = false, anyWaiting = false;
    for (const AiSessionStatus& s : sessions) {
        bool matches = !folderPath.empty() ? PathIsWithinFolder(s.cwd, folderPath)
                                            : _wcsicmp(LastPathSegment(s.cwd).c_str(), tw.folderName.c_str()) == 0;
        LogDiag(L"ai_status:   session cwd=[%ls] status=[%ls] -> %ls", s.cwd.c_str(), s.status.c_str(),
                matches ? L"MATCH" : L"no match");
        if (!matches) continue;
        if (s.status == L"attention") anyAttention = true;
        else if (s.status == L"working") anyWorking = true;
        else if (s.status == L"waiting") anyWaiting = true;
    }
    ClaudeStatus result = anyAttention  ? ClaudeStatus::Attention
                          : anyWorking  ? ClaudeStatus::Working
                          : anyWaiting  ? ClaudeStatus::Waiting
                                        : ClaudeStatus::None;
    LogDiag(L"ai_status: folderName=[%ls] resolved to %d", tw.folderName.c_str(), (int)result);
    return result;
}

static void SyncProjectListHud() {
    if (!g_projectListHud) return;
    // Before the early-outs below: the reserved band follows the config,
    // not whether any windows happen to be open right now -- otherwise
    // maximized windows would grow and shrink as VS Code windows came and
    // went.
    bool showHub = g_config.showProjectList && !g_overlaysHidden;
    SetProjectListHudDocked(g_projectListHud, showHub && g_config.projectListFixed,
                            g_config.labelHeight, g_config.projectListFixedMatchTaskbar);
    if (!showHub) {
        HideProjectListHud(g_projectListHud);
        return;
    }

    // Skip every provider's status scan entirely when the feature is off,
    // not just visually suppress the result. Each configured provider id
    // that isn't implemented yet (e.g. "copilot" today) is silently
    // skipped here -- GetAiProvider returns nullptr for it.
    std::vector<AiSessionStatus> aiSessions;
    if (g_config.aiIndicatorEnabled) {
        for (const std::string& id : g_config.aiIndicatorProviders) {
            AiProvider* provider = GetAiProvider(id);
            if (!provider) continue;
            std::vector<AiSessionStatus> s = provider->LoadStatuses();
            aiSessions.insert(aiSessions.end(), s.begin(), s.end());
        }
    }

    // No early-out on an empty g_tracked: with every VS Code window closed
    // the hub stays up as just its new-window button, which is the one
    // thing still worth clicking then (see UpdateProjectListHud).
    std::vector<ProjectListHudEntry> entries;
    for (auto& kv : g_tracked) {
        if (!IsWindow(kv.first) || !IsWindowVisible(kv.first) || kv.second.label.empty()) continue;

        // Minimized windows are still WS_VISIBLE (that's a separate style
        // bit from iconic/minimized state) -- keep showing them in the HUD
        // rather than dropping them, since restoring a minimized window is
        // exactly the kind of thing this list is for. There's no meaningful
        // *current* on-screen rect to sort by while minimized, so fall back
        // to wherever it was last seen (zero-rect if never seen, e.g.
        // tracked while already minimized).
        RECT rect;
        if (IsIconic(kv.first)) {
            rect = kv.second.lastKnownRect;
        } else if (GetVisibleWindowRect(kv.first, rect)) {
            kv.second.lastKnownRect = rect;
        } else {
            continue;
        }

        // Re-resolve on every sync (cheap: an in-memory map lookup) rather
        // than trusting the cached tw.label -- that cache is otherwise only
        // refreshed on track/name-change, so a freshly-set alias (see
        // project_list_hud.cpp's BeginAliasEdit/EndAliasEdit) wouldn't show
        // up here until the next title change, and would look like it
        // "reverted" the moment this sync next overwrote the HUD's own
        // locally-updated copy with the stale cached one.
        kv.second.label = ResolveAlias(kv.second.rawLabel);

        ProjectListHudEntry entry;
        entry.target = kv.first;
        entry.windowRect = rect;
        entry.trackSeq = kv.second.trackSeq;
        entry.label = kv.second.label;
        entry.rawLabel = kv.second.rawLabel;
        entry.path = kv.second.folderName.empty() ? L"" : ResolveFolderPath(kv.second.folderName);
        entry.color = g_config.palette[kv.second.colorIndex % g_config.palette.size()];
        entry.claudeStatus = ComputeAiStatus(kv.second, aiSessions);
        entry.loading = IsWindowLoading(kv.second, kv.first);
        // Fixed placeholder text while loading -- entry.label/rawLabel
        // otherwise still churns underneath (see IsWindowLoading), and
        // that visible text change is itself part of what looked like the
        // item randomly changing/reordering.
        if (entry.loading) entry.label = L"Loading...";
        entries.push_back(entry);
    }

    ProjectListHudStyle style;
    style.horizontal = g_config.projectListHorizontal || g_config.projectListFixed; // fixed = one-row band
    style.manualOrder = g_config.projectListManualOrder;
    style.rowHeight = g_config.labelHeight;
    style.fontSize = g_config.labelFontSize;
    style.labelTextColorAuto = g_config.labelTextColorAuto;
    style.labelTextColor = g_config.labelTextColor;
    style.normalOpacity = g_config.projectListOpacityNormal;
    style.hoverOpacity = g_config.projectListOpacityHover;
    style.activateOnHover = g_config.projectListActivateOnHover;
    style.claudeColorWorking = g_config.aiIndicatorColorWorking;
    style.claudeColorAttention = g_config.aiIndicatorColorAttention;
    style.claudeColorWaiting = g_config.aiIndicatorColorWaiting;
    style.claudeBorderColorAuto = g_config.aiIndicatorBorderColorAuto;
    style.claudeBorderColor = g_config.aiIndicatorBorderColor;
    style.showNewWindowButton = true;
    style.newWindowButtonColor = g_config.palette[PeekNextColorIndex() % g_config.palette.size()];
    UpdateProjectListHud(g_projectListHud, entries, style);
}

// EVENT_OBJECT_LOCATIONCHANGE fires for every move/resize/visibility/Z-order
// change of every top-level window on the whole desktop -- registering it
// globally (idProcess=0) means our callback gets invoked constantly on a
// busy multi-app desktop even though we only care about a handful of VS
// Code windows. Instead we register LOCATIONCHANGE/DESTROY per-process,
// scoped to the PIDs we're actually tracking, so the OS itself filters out
// everything irrelevant before it ever reaches us. Ref-counted because a
// PID can own more than one tracked window.
struct PidHooks {
    HWINEVENTHOOK location = nullptr;
    HWINEVENTHOOK destroy = nullptr;
    int refCount = 0;
};
static std::unordered_map<DWORD, PidHooks> g_pidHooks;

static void AddPidHooks(DWORD pid) {
    PidHooks& h = g_pidHooks[pid];
    h.refCount++;
    if (h.refCount == 1) {
        h.location = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE,
                                      nullptr, WinEventProc, pid, 0, WINEVENT_OUTOFCONTEXT);
        h.destroy = SetWinEventHook(EVENT_OBJECT_DESTROY, EVENT_OBJECT_DESTROY,
                                     nullptr, WinEventProc, pid, 0, WINEVENT_OUTOFCONTEXT);
        Log(L"per-process hooks added pid=%lu", pid);
    }
}

static void ReleasePidHooks(DWORD pid) {
    auto it = g_pidHooks.find(pid);
    if (it == g_pidHooks.end()) return;
    it->second.refCount--;
    if (it->second.refCount <= 0) {
        if (it->second.location) UnhookWinEvent(it->second.location);
        if (it->second.destroy) UnhookWinEvent(it->second.destroy);
        g_pidHooks.erase(it);
        Log(L"per-process hooks removed pid=%lu", pid);
    }
}

static void ReleaseAllPidHooks() {
    for (auto& kv : g_pidHooks) {
        if (kv.second.location) UnhookWinEvent(kv.second.location);
        if (kv.second.destroy) UnhookWinEvent(kv.second.destroy);
    }
    g_pidHooks.clear();
}

// Picked once per run (not once per window) so the *set* of colors handed
// out in a session still cycles through the whole palette in a fixed
// round-robin order -- only where that cycle starts changes, so two
// windows never collide and every run doesn't visually start with the same
// color for whichever window happens to be tracked first.
static int RandomColorOffset(size_t n) {
    if (n == 0) return 0;
    static bool seeded = false;
    if (!seeded) {
        srand((unsigned)time(nullptr));
        seeded = true;
    }
    return rand() % (int)n;
}

// offset/roundRobin used to be locals `static` inside AllocateColorIndex --
// hoisted to file scope (same effective lifetime/visibility) so
// PeekNextColorIndex below can read them too, without duplicating or
// disturbing AllocateColorIndex's own cursor.
static int g_colorOffset = -1;
static int g_colorRoundRobin = 0;

static int AllocateColorIndex() {
    size_t n = g_config.palette.size();
    if (g_colorInUse.size() != n) g_colorInUse.assign(n, false);
    if (g_colorOffset < 0) g_colorOffset = RandomColorOffset(n);

    for (size_t i = 0; i < n; i++) {
        size_t idx = (i + (size_t)g_colorOffset) % n;
        if (!g_colorInUse[idx]) {
            g_colorInUse[idx] = true;
            return (int)idx;
        }
    }
    int idx = (g_colorRoundRobin + g_colorOffset) % (int)n;
    g_colorRoundRobin++;
    return idx;
}

static void FreeColorIndex(int idx) {
    if (idx >= 0 && idx < (int)g_colorInUse.size()) g_colorInUse[idx] = false;
}

// Peeks whichever color AllocateColorIndex would hand out to the *next*
// tracked window, without reserving it or advancing the round-robin cursor
// -- used only to color the project list HUD's "+" new-window button so it
// previews what a window opened via that button would get.
static int PeekNextColorIndex() {
    size_t n = g_config.palette.size();
    if (n == 0) return 0;
    int offset = g_colorOffset < 0 ? 0 : g_colorOffset;
    if (g_colorInUse.size() == n) {
        for (size_t i = 0; i < n; i++) {
            size_t idx = (i + (size_t)offset) % n;
            if (!g_colorInUse[idx]) return (int)idx;
        }
    }
    return (g_colorRoundRobin + offset) % (int)n;
}

// Repositions/resizes/repaints/hides the overlay to match its target's
// current state. Cheap when only position changed (no repaint needed).
static void SyncOverlay(HWND target, TrackedWindow& tw) {
    if (!IsWindow(target) || !tw.overlay) return;

    // Re-resolve on every sync (cheap: an in-memory map lookup) so a
    // freshly-set alias (see project_list_hud.cpp's BeginAliasEdit) shows
    // up in the border's label chip promptly, not just the next time the
    // window's title itself changes.
    tw.label = ResolveAlias(tw.rawLabel);
    std::wstring borderLabel = BorderLabel(tw);

    if (g_overlaysHidden || IsIconic(target) || !IsWindowVisible(target)) {
        LogFastDiag(L"sync hwnd=%p HIDE overlaysHidden=%d iconic=%d visible=%d", target, g_overlaysHidden,
                    IsIconic(target), IsWindowVisible(target));
        ShowWindow(tw.overlay, SW_HIDE);
        SyncProjectListHud();
        return;
    }

    RECT r;
    if (!GetVisibleWindowRect(target, r)) {
        SyncProjectListHud();
        return;
    }

    int t = g_config.thickness;
    int ox = r.left - t;
    int oy = r.top - t;
    int ow = (r.right - r.left) + 2 * t;
    int oh = (r.bottom - r.top) + 2 * t;
    if (ow <= 0 || oh <= 0) {
        SyncProjectListHud();
        return;
    }

    if (ow != tw.lastWidth || oh != tw.lastHeight || borderLabel != tw.lastLabel) {
        LogFastDiag(L"sync hwnd=%p REPAINT ow=%d oh=%d (was %dx%d) label=[%ls] (was [%ls])", target, ow, oh,
                    tw.lastWidth, tw.lastHeight, borderLabel.c_str(), tw.lastLabel.c_str());
        COLORREF color = g_config.palette[tw.colorIndex % g_config.palette.size()];
        PaintOverlay(tw.overlay, ow, oh, color, t, g_config.opacity,
                     borderLabel, g_config.showLabel, g_config.labelHeight, g_config.labelFontSize,
                     g_config.labelTextColorAuto, g_config.labelTextColor);
        tw.lastWidth = ow;
        tw.lastHeight = oh;
        tw.lastLabel = borderLabel;
    }

    // Move/resize without touching Z-order. This has to be a separate call
    // from the Z-order fixup below: once the overlay is correctly stacked,
    // "the window in front of target" IS the overlay itself, and a combined
    // SetWindowPos(overlay, overlay, x, y, ...) is self-referential and
    // silently stops applying the position update -- the overlay would
    // freeze in place on every sync after the first.
    SetWindowPos(tw.overlay, nullptr, ox, oy, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);

    // SetWindowPos(w, insertAfter, ...) places w immediately BEHIND
    // insertAfter, not in front of it. To put the overlay directly in front
    // of its target, insert it behind whatever currently sits in front of
    // the target (or at HWND_TOP -- which is literally NULL, same as this
    // GetWindow call returns when target is already the frontmost window).
    // Only do this when the stacking actually needs fixing (e.g. right
    // after creation, or another window got inserted between them) --
    // otherwise this degenerates into the self-referential case above.
    HWND aboveTarget = GetWindow(target, GW_HWNDPREV);
    if (aboveTarget != tw.overlay) {
        SetWindowPos(tw.overlay, aboveTarget, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE);
    }
    SyncProjectListHud();
}

// VS Code can fire several rapid NAMECHANGE events in a row while
// re-resolving repo/branch info (e.g. right after reselecting an
// already-active window from the taskbar), some carrying a transient,
// incomplete title. Repainting on every single one flickers the label chip
// specifically: an intermediate empty label leaves that corner untouched
// (transparent), letting the real window show through for a frame, even
// though the border itself never changes (its color doesn't depend on the
// title). Debounce: only recompute + repaint once NAMECHANGE events stop
// arriving for a short interval, using whatever title is current then.
static std::unordered_map<HWND, UINT_PTR> g_labelDebounceByHwnd;
static std::unordered_map<UINT_PTR, HWND> g_labelDebounceByTimer;

static void CALLBACK LabelDebounceTimerProc(HWND, UINT, UINT_PTR idEvent, DWORD) {
    KillTimer(nullptr, idEvent);
    auto it = g_labelDebounceByTimer.find(idEvent);
    if (it == g_labelDebounceByTimer.end()) return;
    HWND target = it->second;
    g_labelDebounceByTimer.erase(it);
    g_labelDebounceByHwnd.erase(target);

    auto tracked = g_tracked.find(target);
    if (tracked == g_tracked.end()) return;
    wchar_t title[512] = {};
    GetWindowTextW(target, title, 512);
    ApplyLabelForTitle(tracked->second, title);
    LogFastDiag(L"namechange(settled) hwnd=%p title=[%ls] label=[%ls] hasRepoInTitle=%d loading=%ls", target, title,
                tracked->second.label.c_str(), tracked->second.hasRepoInTitle,
                IsWindowLoading(tracked->second, target) ? L"1" : L"0");
    SyncOverlay(target, tracked->second);
}

static void ScheduleLabelUpdate(HWND target) {
    auto existing = g_labelDebounceByHwnd.find(target);
    if (existing != g_labelDebounceByHwnd.end()) {
        KillTimer(nullptr, existing->second);
        g_labelDebounceByTimer.erase(existing->second);
    }
    UINT_PTR id = SetTimer(nullptr, 0, 120, LabelDebounceTimerProc);
    if (id) {
        g_labelDebounceByHwnd[target] = id;
        g_labelDebounceByTimer[id] = target;
    }
}

static HWND g_lastForeground = nullptr;

// Starts the poll timer the moment something is worth polling for, stops it
// the moment nothing is -- so a desktop with no VS Code window open never
// pays for it, and a laptop isn't kept out of deeper idle/sleep states by a
// 20Hz timer running for no reason.
static void UpdateForegroundPollTimer() {
    if (!g_ownerWnd) return;
    if (!g_tracked.empty()) {
        SetTimer(g_ownerWnd, kForegroundPollTimerId, kForegroundPollIntervalMs, nullptr);
    } else {
        KillTimer(g_ownerWnd, kForegroundPollTimerId);
        g_lastForeground = nullptr; // don't skip the next tracked window's first sync
    }
}

static void TrackWindow(HWND hwnd) {
    if (g_tracked.find(hwnd) != g_tracked.end()) return;

    // Cheap, in-process checks (class name / visibility / owner / title)
    // first -- this rejects nearly every window on the desktop instantly.
    // Only genuine candidates pay for OpenProcess + QueryFullProcessImageName,
    // which is the one call in this path that's an actual kernel round-trip.
    if (!IsCandidateTopLevelWindow(hwnd)) return;

    std::wstring exeName;
    if (!GetProcessImageName(hwnd, exeName) || !IsVSCodeExe(exeName)) return;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);

    wchar_t title[512] = {};
    GetWindowTextW(hwnd, title, 512);

    TrackedWindow tw;
    tw.overlay = CreateOverlay(g_hInstance);
    tw.colorIndex = AllocateColorIndex();
    tw.pid = pid;
    tw.trackSeq = g_nextTrackSeq++;
    tw.loadingEpisodeStartTick = GetTickCount64();
    ApplyLabelForTitle(tw, title);
    LogDiag(L"tracked(initial) hwnd=%p title=[%ls] rawLabel=[%ls] hasRepoInTitle=%d loadingEpisodeStartTick=%llu",
            hwnd, title, tw.rawLabel.c_str(), tw.hasRepoInTitle, (unsigned long long)tw.loadingEpisodeStartTick);
    g_tracked[hwnd] = tw;
    SyncOverlay(hwnd, g_tracked[hwnd]);
    AddPidHooks(pid);
    UpdateForegroundPollTimer();

    Log(L"tracked hwnd=%p colorIndex=%d overlay=%p pid=%lu title=[%ls] label=[%ls]",
        hwnd, tw.colorIndex, tw.overlay, pid, title, tw.label.c_str());
}

static void UntrackWindow(HWND hwnd) {
    auto it = g_tracked.find(hwnd);
    if (it == g_tracked.end()) return;
    FreeColorIndex(it->second.colorIndex);
    ReleasePidHooks(it->second.pid);
    DestroyWindow(it->second.overlay);
    g_tracked.erase(it);
    UpdateForegroundPollTimer();
    SyncProjectListHud();

    auto debounce = g_labelDebounceByHwnd.find(hwnd);
    if (debounce != g_labelDebounceByHwnd.end()) {
        KillTimer(nullptr, debounce->second);
        g_labelDebounceByTimer.erase(debounce->second);
        g_labelDebounceByHwnd.erase(debounce);
    }
}

static BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM) {
    TrackWindow(hwnd);
    return TRUE;
}

// Dumps every VS Code window this app currently knows about, one line
// each, plus which of them (if any) holds the foreground. Wired to
// focus_trace's storm detector: a warning saying "several windows were
// asked to activate" is only actionable next to the list of what existed
// at that moment, since the interesting part is usually a window that is
// in the list and shouldn't be, or one whose state (minimized, hidden)
// explains why activating it was refused.
static void LogTrackedWindowsSnapshot(const wchar_t* reason) {
    HWND fg = GetForegroundWindow();
    Log(L"tracked-window snapshot (%ls): %zu window(s)", reason, g_tracked.size());
    for (const auto& kv : g_tracked) {
        wchar_t desc[400] = {};
        DescribeWindowForLog(kv.first, desc, 400);
        Log(L"tracked-window snapshot:   %ls label=[%ls] colorIndex=%d%ls", desc, kv.second.label.c_str(),
            kv.second.colorIndex, kv.first == fg ? L" <-- FOREGROUND" : L"");
    }
}

void TrackingInit(HINSTANCE hInstance, HWND ownerWnd) {
    g_hInstance = hInstance;
    g_ownerWnd = ownerWnd;
    g_projectListHud = CreateProjectListHud(hInstance, g_config.projectListHorizontal || g_config.projectListFixed);
    SetProjectListHudDocked(g_projectListHud, g_config.showProjectList && !g_overlaysHidden && g_config.projectListFixed,
                            g_config.labelHeight, g_config.projectListFixedMatchTaskbar);
    SetFocusStormSnapshotHook(LogTrackedWindowsSnapshot);
}

void RescanAllWindows() {
    EnumWindows(EnumWindowsProc, 0);

    std::vector<HWND> stale;
    for (auto& kv : g_tracked) {
        if (!IsWindow(kv.first) || !IsCandidateTopLevelWindow(kv.first)) {
            stale.push_back(kv.first);
        }
    }
    for (HWND hwnd : stale) UntrackWindow(hwnd);

    for (auto& kv : g_tracked) SyncOverlay(kv.first, kv.second);
    // SyncOverlay already syncs the hub per tracked window, but this rescan
    // (called at startup and on the rescan timer) is also the only thing
    // that runs when *nothing* is tracked -- and the hub is shown even
    // then, as a lone new-window button.
    SyncProjectListHud();
}

void ForceRepaintAllTracked() {
    for (auto& kv : g_tracked) {
        kv.second.lastWidth = -1;
        kv.second.lastHeight = -1;
        SyncOverlay(kv.first, kv.second);
    }
}

bool AreOverlaysHidden() { return g_overlaysHidden; }

void SetOverlaysHidden(bool hidden) {
    if (hidden == g_overlaysHidden) return;
    g_overlaysHidden = hidden;
    Log(L"borders and hub %ls via tray menu", hidden ? L"hidden" : L"shown");
    for (auto& kv : g_tracked) SyncOverlay(kv.first, kv.second);
    SyncProjectListHud(); // SyncOverlay does this too, but not when nothing is tracked
}

void PollForegroundChange() {
    HWND fg = GetForegroundWindow();
    if (fg == g_lastForeground) return;
    g_lastForeground = fg;

    auto it = g_tracked.find(fg);
    if (it != g_tracked.end()) SyncOverlay(fg, it->second);
}

void RefreshAllLabels() {
    for (auto& kv : g_tracked) {
        wchar_t title[512] = {};
        GetWindowTextW(kv.first, title, 512);
        ApplyLabelForTitle(kv.second, title);
        SyncOverlay(kv.first, kv.second); // repaints automatically if the label changed
    }
    SyncProjectListHud();
}

void CleanupAllTracked() {
    for (auto& kv : g_tracked) DestroyWindow(kv.second.overlay);
    g_tracked.clear();
    if (g_projectListHud) {
        DestroyWindow(g_projectListHud);
        g_projectListHud = nullptr;
    }
    ReleaseAllPidHooks();
    UpdateForegroundPollTimer();
}

size_t TrackedWindowCount() {
    return g_tracked.size();
}

std::vector<std::wstring> GetTrackedFolderPaths() {
    std::vector<std::wstring> paths;
    for (const auto& kv : g_tracked) {
        if (kv.second.folderName.empty()) continue;
        std::wstring path = ResolveFolderPath(kv.second.folderName);
        if (!path.empty()) paths.push_back(path);
    }
    return paths;
}

void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD, DWORD) {
    if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF || !hwnd) return;

    if (g_config.verboseLogging && g_tracked.find(hwnd) != g_tracked.end()) {
        const wchar_t* name =
            event == EVENT_OBJECT_DESTROY ? L"DESTROY"
            : event == EVENT_OBJECT_SHOW ? L"SHOW"
            : event == EVENT_OBJECT_CREATE ? L"CREATE"
            : event == EVENT_OBJECT_NAMECHANGE ? L"NAMECHANGE"
            : event == EVENT_OBJECT_LOCATIONCHANGE ? L"LOCATIONCHANGE"
            : event == EVENT_SYSTEM_FOREGROUND ? L"FOREGROUND"
            : L"OTHER";
        LogFast(L"event %ls hwnd=%p", name, hwnd);
    }

    if (event == EVENT_OBJECT_DESTROY) {
        UntrackWindow(hwnd);
    } else if (event == EVENT_OBJECT_SHOW || event == EVENT_OBJECT_CREATE || event == EVENT_OBJECT_NAMECHANGE) {
        // NAMECHANGE matters because a new window is often shown before its
        // title is set -- IsCandidateTopLevelWindow requires a non-empty
        // title, so without this the window would sit untracked until the
        // next safety-net rescan (up to rescan_interval_ms later). For a
        // window we already track, NAMECHANGE also means the open
        // folder/workspace may have changed -- debounce that case (see
        // ScheduleLabelUpdate) instead of repainting on every intermediate
        // title.
        auto it = g_tracked.find(hwnd);
        if (it != g_tracked.end()) {
            if (event == EVENT_OBJECT_NAMECHANGE) {
                // A reload (Developer: Reload Window), or the window first
                // appearing, starts a fresh loading episode the moment any
                // NAMECHANGE is observed, whether or not the title/rawLabel
                // ends up any different once it settles (see
                // ApplyLabelForTitle) -- only repaints the HUD on the
                // not-loading -> loading edge; a rapid run of NAMECHANGEs
                // mid-episode doesn't restart the clock or repaint again,
                // since VS Code's git extension can go quiet for a while
                // before its one final update (see kLoadingGraceMs).
                wchar_t rawTitle[512] = {};
                GetWindowTextW(hwnd, rawTitle, 512);
                bool wasLoading = IsWindowLoading(it->second, hwnd);
                LogFastDiag(L"namechange(raw) hwnd=%p title=[%ls] wasLoading=%d", hwnd, rawTitle, wasLoading);
                if (!wasLoading) {
                    it->second.loadingEpisodeStartTick = GetTickCount64();
                    LogDiag(L"namechange(raw) hwnd=%p -- starting new loading episode", hwnd);
                    SyncProjectListHud();
                }
                ScheduleLabelUpdate(hwnd);
            } else {
                SyncOverlay(hwnd, it->second);
            }
        } else {
            TrackWindow(hwnd);
        }
    } else if (event == EVENT_OBJECT_LOCATIONCHANGE || event == EVENT_SYSTEM_FOREGROUND) {
        // Record every foreground switch on the desktop, not just switches
        // onto a VS Code window -- this hook is registered globally (see
        // vscode_border.cpp) precisely so the log can show what focus did
        // *around* an incident, including whichever other app it bounced
        // through on the way. See focus_trace.h for what that buys.
        if (event == EVENT_SYSTEM_FOREGROUND) {
            NoteForegroundChange(hwnd, g_tracked.find(hwnd) != g_tracked.end());
        }
        // FOREGROUND matters because bringing a window to the front is a
        // pure z-order change with no move/resize -- LOCATIONCHANGE isn't
        // reliably fired for that, so without this the overlay would only
        // catch up on the next safety-net rescan (up to rescan_interval_ms
        // later), showing up as a noticeable lag when alt-tabbing back to
        // a VS Code window.
        auto it = g_tracked.find(hwnd);
        if (it != g_tracked.end()) SyncOverlay(hwnd, it->second);
    }
}
