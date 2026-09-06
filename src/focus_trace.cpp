#include "focus_trace.h"

#include "logger.h"

#include <deque>
#include <unordered_map>

// How close together this app's own activation requests have to be to
// count as one burst. Generous enough to span a mouse sweep across the
// whole project-list HUD (which fires one request per item crossed), short
// enough that two deliberate, separate clicks never merge into one.
static const DWORD kRequestBurstWindowMs = 1500;

// The same idea for *observed* foreground changes, which play out far more
// slowly: a batch of VS Code windows launching at once takes seconds to
// appear and fight over the foreground (measured on this machine: three
// windows across ~2.3s on a two-favourite startup), so a 1.5s window would
// see each one in isolation and never spot the pattern.
static const DWORD kForegroundChurnWindowMs = 5000;

// Distinct target windows within the relevant window before it is called a
// storm. Two is a normal pattern (activate an item, then restore the
// previous foreground on mouse-leave, see EndProjectListHoverFocus) --
// three different windows coming forward is not.
static const size_t kStormDistinctTargets = 3;

// A foreground change landing this soon after one of our own
// RequestForeground calls is almost certainly that call taking effect,
// rather than the user alt-tabbing at exactly that moment.
static const DWORD kAttributionWindowMs = 400;

// How long after something this app did that a burst of foreground changes
// is still plausibly its fault. Rapid switching between VS Code windows is
// only *reported* when it falls inside one of these periods -- outside one
// it is just someone alt-tabbing quickly, which must not raise a warning or
// badge the tray icon. Launching windows gets the longer period because the
// windows it creates take seconds to appear and self-activate.
static const DWORD kSuspectAfterRequestMs = 2000;
static const DWORD kSuspectAfterLaunchMs = 15000;

struct FocusAttempt {
    DWORD tick = 0;
    HWND target = nullptr;
    bool granted = false;
    const wchar_t* reason = L"";
};

static std::deque<FocusAttempt> g_attempts;
static std::deque<FocusAttempt> g_foregroundChanges; // reuses the struct; `granted` unused here
static DWORD g_lastRequestTick = 0;
static const wchar_t* g_lastRequestReason = L"";
static bool g_lastRequestGranted = false;
static DWORD g_lastStormWarnTick = 0;
static DWORD g_suspectUntilTick = 0; // see kSuspectAfter*Ms
static const wchar_t* g_suspectReason = L"";
static FocusStormSnapshotFn g_snapshotHook = nullptr;

// GetTickCount wraps every ~49 days. Measuring elapsed time by subtracting
// unsigned DWORDs stays correct across that wrap; comparing raw tick values
// with `<` does not, so every deadline check here goes through these two.
static bool WithinMs(DWORD now, DWORD since, DWORD windowMs) { return (now - since) <= windowMs; }

static void OpenSuspectPeriod(DWORD now, DWORD durationMs, const wchar_t* reason) {
    g_suspectUntilTick = now + durationMs;
    g_suspectReason = reason;
}

// Signed difference, so this reads correctly on either side of a tick wrap:
// negative means the deadline is already behind us.
static bool InSuspectPeriod(DWORD now) { return (LONG)(g_suspectUntilTick - now) > 0; }

void SetFocusStormSnapshotHook(FocusStormSnapshotFn fn) { g_snapshotHook = fn; }

void DescribeWindowForLog(HWND hwnd, wchar_t* buf, size_t bufChars) {
    if (!buf || bufChars == 0) return;
    if (!hwnd) {
        _snwprintf_s(buf, bufChars, _TRUNCATE, L"hwnd=(null)");
        return;
    }
    if (!IsWindow(hwnd)) {
        _snwprintf_s(buf, bufChars, _TRUNCATE, L"hwnd=%p (dead)", hwnd);
        return;
    }
    wchar_t cls[80] = {};
    GetClassNameW(hwnd, cls, 80);
    wchar_t title[160] = {};
    GetWindowTextW(hwnd, title, 160);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    _snwprintf_s(buf, bufChars, _TRUNCATE, L"hwnd=%p class=[%ls] title=[%ls] pid=%lu iconic=%d visible=%d", hwnd,
                 cls, title, pid, IsIconic(hwnd) ? 1 : 0, IsWindowVisible(hwnd) ? 1 : 0);
}

static void PruneOlderThanWindow(std::deque<FocusAttempt>& q, DWORD now, DWORD windowMs) {
    while (!q.empty() && !WithinMs(now, q.front().tick, windowMs)) q.pop_front();
}

static size_t DistinctTargets(const std::deque<FocusAttempt>& q) {
    size_t distinct = 0;
    for (size_t i = 0; i < q.size(); i++) {
        bool seenEarlier = false;
        for (size_t j = 0; j < i; j++) {
            if (q[j].target == q[i].target) {
                seenEarlier = true;
                break;
            }
        }
        if (!seenEarlier) distinct++;
    }
    return distinct;
}

// Emits the storm warning plus the full contents of the burst, so the log
// says which windows were involved and what asked for each -- that is what
// distinguishes "hover activation swept the HUD" from "the favourites
// auto-open launched five windows" after the fact. Rate-limited to one
// warning per burst: a single sweep would otherwise warn again on every
// item after the third.
// `showGranted` exists because `granted` only means something for the
// request queue. The foreground-change queue reuses FocusAttempt without
// filling that field in, and printing it there produced a dump where every
// line read `granted=0` -- i.e. it described a run of perfectly successful
// activations as three denials, which is the exact signature of the bug
// this file is meant to catch. Actively misleading, so it is now omitted
// rather than printed as a hardcoded zero.
static void ReportStorm(const wchar_t* what, const std::deque<FocusAttempt>& q, DWORD now, DWORD windowMs,
                        bool showGranted) {
    if (g_lastStormWarnTick != 0 && WithinMs(now, g_lastStormWarnTick, windowMs)) return;
    g_lastStormWarnTick = now;

    LogWarn(L"focus storm: %ls -- %zu event(s) across %zu distinct window(s) within %lums. Several VS Code "
            L"windows may now be showing the highlighted wants-attention state in the taskbar.",
            what, q.size(), DistinctTargets(q), windowMs);
    for (const FocusAttempt& a : q) {
        wchar_t desc[400] = {};
        DescribeWindowForLog(a.target, desc, 400);
        if (showGranted)
            Log(L"focus storm:   -%lums reason=[%ls] granted=%d %ls", now - a.tick, a.reason, a.granted ? 1 : 0,
                desc);
        else
            Log(L"focus storm:   -%lums reason=[%ls] %ls", now - a.tick, a.reason, desc);
    }
    if (g_snapshotHook) g_snapshotHook(L"focus storm");
}

// Same burst, logged without raising a warning or badging the tray -- for
// the case where the burst provably did *not* produce the reported symptom
// (see the granted-only check in RequestForeground). Keeping the full dump
// means a sweep is still reconstructable from the log afterwards.
static void LogBenignBurst(const wchar_t* what, const std::deque<FocusAttempt>& q, DWORD now, DWORD windowMs) {
    Log(L"focus burst: %ls -- %zu event(s) across %zu distinct window(s) within %lums, all granted, so no window "
        L"was left highlighted; not reporting it as a storm.",
        what, q.size(), DistinctTargets(q), windowMs);
    for (const FocusAttempt& a : q) {
        wchar_t desc[400] = {};
        DescribeWindowForLog(a.target, desc, 400);
        Log(L"focus burst:   -%lums reason=[%ls] granted=%d %ls", now - a.tick, a.reason, a.granted ? 1 : 0, desc);
    }
}

static bool AnyDenied(const std::deque<FocusAttempt>& q) {
    for (const FocusAttempt& a : q)
        if (!a.granted) return true;
    return false;
}

// A refused SetForegroundWindow does not fail quietly. The system puts the
// target's taskbar button into the flashing wants-attention state instead,
// which is the entire user-visible symptom this file exists to explain: a
// VS Code taskbar thumbnail pulsing several times, on its own, moments
// after a HUD click that appeared to do nothing. One refusal is enough --
// the taskbar repeats the pulse a few times and then settles, so "it kept
// pulsing and then stopped" is a single denial, not a burst.
//
// The activation is already lost by the time we get here; leaving the pulse
// behind only makes a silent no-op look like the window is demanding
// attention. FLASHW_STOP is the documented way to clear that state and it
// works on windows owned by other processes. Cost of being wrong: if the
// target had genuinely asked for attention in the same instant, this
// cancels that too -- much the lesser annoyance of the two.
static void CancelTaskbarFlash(HWND target) {
    if (!target || !IsWindow(target)) return;
    FLASHWINFO fi = {};
    fi.cbSize = sizeof(fi);
    fi.hwnd = target;
    fi.dwFlags = FLASHW_STOP;
    FlashWindowEx(&fi);
}

// The highlight is not guaranteed to be applied before SetForegroundWindow
// returns, so cancelling purely inline can race it and lose. Cancelling a
// second time once the dust has settled covers that ordering and costs
// nothing when the first one already worked.
//
// Note what this deliberately is *not*: a retry of the focus request. A
// denial normally means the user is busy elsewhere -- a shell flyout (the
// taskbar's thumbnail preview, XamlExplorerHostIslandWindow) holds the
// foreground lock while it is up -- so re-asking would be this app fighting
// the user for the foreground. Stopping a flash takes nothing away from
// anyone, so it is safe to repeat; stealing the foreground is not.
static const UINT kFlashStopSettleDelayMs = 250;
static std::unordered_map<UINT_PTR, HWND> g_flashStopTimers;

static void CALLBACK FlashStopTimerProc(HWND, UINT, UINT_PTR idEvent, DWORD) {
    KillTimer(nullptr, idEvent);
    auto it = g_flashStopTimers.find(idEvent);
    if (it == g_flashStopTimers.end()) return;
    HWND target = it->second;
    g_flashStopTimers.erase(it);
    CancelTaskbarFlash(target);
    // Logged because the effect is not otherwise observable: no Win32 call
    // reports whether a taskbar button is flashing (the same reason this
    // file exists at all), so this line is the only evidence the settle
    // pass ran. Cheap -- it happens only after a denial.
    Log(L"focus denied: taskbar highlight cancelled again after %ums (settle pass), hwnd=%p",
        kFlashStopSettleDelayMs, target);
}

// Thread timer (no window handle), matching ScheduleLabelUpdate in
// tracking.cpp -- the callback runs on this app's own message loop, so
// g_flashStopTimers needs no locking.
static void CancelTaskbarFlashNowAndAfterSettling(HWND target) {
    CancelTaskbarFlash(target);
    UINT_PTR id = SetTimer(nullptr, 0, kFlashStopSettleDelayMs, FlashStopTimerProc);
    if (id) g_flashStopTimers[id] = target;
}

bool RequestForeground(HWND target, FocusTargetKind kind, const wchar_t* reason) {
    DWORD now = GetTickCount();
    HWND before = GetForegroundWindow();

    wchar_t targetDesc[400] = {};
    DescribeWindowForLog(target, targetDesc, 400);
    wchar_t beforeDesc[400] = {};
    DescribeWindowForLog(before, beforeDesc, 400);

    // Whether this app already holds the foreground is the single best
    // predictor of whether the call will be granted, and it is the piece
    // that cannot be reconstructed from the log afterwards -- capture it
    // before the call, not after.
    DWORD foregroundPid = 0;
    if (before) GetWindowThreadProcessId(before, &foregroundPid);
    bool weAreForeground = foregroundPid == GetCurrentProcessId();

    BOOL ok = SetForegroundWindow(target);
    DWORD lastError = ok ? 0 : GetLastError();
    HWND after = GetForegroundWindow();

    Log(L"focus request reason=[%ls] kind=%ls granted=%d lastError=%lu weHeldForeground=%d", reason,
        kind == FocusTargetKind::ExternalWindow ? L"external" : L"own-ui", ok ? 1 : 0, lastError,
        weAreForeground ? 1 : 0);
    Log(L"focus request   target: %ls", targetDesc);
    Log(L"focus request   before: %ls", beforeDesc);
    Log(L"focus request   after:  hwnd=%p%ls", after, after == target ? L" (== target)" : L" (NOT the target)");

    // The denial case is the one that produces the reported symptom, so it
    // gets a warning of its own rather than only counting toward a burst --
    // a single VS Code window left highlighted is still this app
    // misbehaving, and one occurrence never reaches kStormDistinctTargets.
    if (!ok && kind == FocusTargetKind::ExternalWindow) {
        CancelTaskbarFlashNowAndAfterSettling(target);
        LogWarn(L"focus denied: SetForegroundWindow for reason=[%ls] was refused (lastError=%lu, "
                L"weHeldForeground=%d) -- Windows put that window's taskbar button into the flashing "
                L"wants-attention state instead of activating it; cancelling that highlight. The "
                L"activation itself is lost, not retried. foreground was: %ls. target: %ls",
                reason, lastError, weAreForeground ? 1 : 0, beforeDesc, targetDesc);
    }

    g_lastRequestTick = now;
    g_lastRequestReason = reason;
    g_lastRequestGranted = ok != 0;
    if (kind == FocusTargetKind::ExternalWindow) OpenSuspectPeriod(now, kSuspectAfterRequestMs, reason);

    PruneOlderThanWindow(g_attempts, now, kRequestBurstWindowMs);
    g_attempts.push_back({now, target, ok != 0, reason});
    if (DistinctTargets(g_attempts) >= kStormDistinctTargets) {
        // A burst only produces the reported symptom via the denials in it:
        // a *granted* request activates the window instead of highlighting
        // it. Clicking briskly through three HUD items is an ordinary
        // gesture, and warning that "several windows may now be showing the
        // highlighted wants-attention state" when all three were granted is
        // simply untrue -- it badges the tray for normal use and devalues
        // the warning when it is real. The burst is still logged either way.
        const wchar_t* what = L"this app asked to activate several different windows in quick succession";
        if (AnyDenied(g_attempts)) ReportStorm(what, g_attempts, now, kRequestBurstWindowMs, true);
        else LogBenignBurst(what, g_attempts, now, kRequestBurstWindowMs);
    }
    return ok != 0;
}

void NoteForegroundChange(HWND hwnd, bool isTrackedVSCodeWindow) {
    DWORD now = GetTickCount();
    bool ours = g_lastRequestTick != 0 && WithinMs(now, g_lastRequestTick, kAttributionWindowMs);

    wchar_t desc[400] = {};
    DescribeWindowForLog(hwnd, desc, 400);
    Log(L"foreground -> %ls tracked=%d source=%ls", desc, isTrackedVSCodeWindow ? 1 : 0,
        ours ? g_lastRequestReason : L"external (not this app)");

    // Rapid churn *between VS Code windows* is its own signature, distinct
    // from the request burst above: it is what a storm looks like when the
    // activations came from VS Code itself (several windows launching at
    // once) rather than from any SetForegroundWindow call of ours.
    if (!isTrackedVSCodeWindow) return;

    // Which is why a change that is simply one of our own *granted*
    // requests taking effect does not belong in this queue at all -- that
    // is the window we just successfully activated arriving, not a window
    // fighting for the foreground. Recording it anyway is what made
    // clicking through four HUD items at roughly one per second raise a
    // storm: four granted activations, four attributed foreground changes,
    // three distinct windows inside the 5s window, and every click
    // re-opening the 2s suspect period that gates the warning. Ordinary
    // use, warned about and badged on the tray.
    //
    // Denied requests are deliberately still recorded: those do not move
    // the foreground, so a change arriving after one came from somewhere
    // else and is exactly what this detector is for. So is anything with no
    // request behind it -- the launch case (favourites auto-open), where
    // the new windows activate themselves and the losers flash, which is
    // what this queue was written for in the first place.
    if (ours && g_lastRequestGranted) return;

    PruneOlderThanWindow(g_foregroundChanges, now, kForegroundChurnWindowMs);
    g_foregroundChanges.push_back({now, hwnd, false, ours ? g_lastRequestReason : L"external"});
    if (DistinctTargets(g_foregroundChanges) < kStormDistinctTargets) return;

    // Only warn while something this app did could still explain it.
    // Outside a suspect period, three VS Code windows taking the foreground
    // in five seconds is just someone alt-tabbing quickly -- warning on
    // that would badge the tray icon during ordinary use and drown out the
    // real thing. Every change is logged above either way, so a stretch of
    // churn stays visible in the log even when it is not ours.
    if (!InSuspectPeriod(now)) {
        Log(L"foreground: %zu VS Code window(s) took the foreground within %lums, but nothing this app did can "
            L"explain it -- not reporting it as a storm",
            DistinctTargets(g_foregroundChanges), kForegroundChurnWindowMs);
        return;
    }
    wchar_t what[300] = {};
    _snwprintf_s(what, 300, _TRUNCATE, L"the foreground bounced between several VS Code windows after [%ls]",
                 g_suspectReason);
    ReportStorm(what, g_foregroundChanges, now, kForegroundChurnWindowMs, false);
}

void NoteWindowLaunchRequest(const wchar_t* reason, size_t count) {
    if (count == 0) return;
    // Not a warning on its own -- opening several windows at once is a
    // feature (favourites auto-open). It is logged so the taskbar
    // highlighting that naturally follows is attributable to it, since
    // those windows request the foreground themselves and nothing here
    // would otherwise appear in the log at all.
    Log(L"window launch reason=[%ls] count=%zu -- each new VS Code window requests the foreground itself; the "
        L"ones that lose that race highlight their own taskbar buttons",
        reason, count);
    OpenSuspectPeriod(GetTickCount(), kSuspectAfterLaunchMs, reason);
    if (count >= kStormDistinctTargets) {
        Log(L"window launch: %zu at once is enough to leave several VS Code windows highlighted in the taskbar "
            L"without this app calling SetForegroundWindow at all",
            count);
    }
}
