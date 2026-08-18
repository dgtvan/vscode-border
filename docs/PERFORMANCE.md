# Performance review

This app is meant to be a tiny, always-running background utility, so it
should be near-invisible in Task Manager. This document records an actual
measurement pass (not just theoretical analysis) done against the build in
this repo, plus the optimizations that came out of it and one trade-off
that was deliberately *not* taken.

All numbers below were measured on the developer's machine (3 tracked VS
Code windows, one a near-fullscreen ~2200x1000px window) via
`Get-Process` (CPU/RAM/handles) and `GetGuiResources` (GDI/User object
counts), immediately after each code change, so they reflect the current
code, not a stale snapshot.

## Idle

| Metric | Before optimization | After |
|---|---|---|
| CPU (15s idle window) | 0.624% avg | **0%** (below measurement resolution) |
| Working set | 46.83 MB | 46.82 MB (stable, 0 growth over 15s) |
| Handles | 194 (stable) | 194 (stable) |
| GDI objects | 6 (stable) | 6 (stable) |
| User objects | 13 (stable) | 13 (stable) |

The "before" 0.624% came from a real inefficiency, not just measurement
noise: the periodic safety-net rescan (`EnumWindows` over every top-level
window on the whole desktop, every `rescan_interval_ms`) was calling
`TrackWindow`, which ran the **expensive** check (`OpenProcess` +
`QueryFullProcessImageNameW` -- an actual kernel round-trip) before the
**cheap** one (class name / visibility / owner -- all in-process). That
meant every unrelated top-level window on the desktop (Chrome, Excel,
Explorer, Notepad++, everything) paid the expensive check on every rescan,
forever. Swapping the check order (cheap first, so almost everything is
rejected before the expensive call ever runs) took idle CPU to
unmeasurable.

The 46 MB working set is mostly the statically-linked C++ runtime
(`-static -static-libgcc -static-libstdc++`, chosen so the exe has zero
MinGW DLL dependencies) rather than anything this app allocates itself --
see "Memory" below.

## Event hook scoping

`EVENT_OBJECT_LOCATIONCHANGE` and `EVENT_OBJECT_DESTROY` were originally
hooked globally (`idProcess=0`), meaning the callback fired for every
window move/resize/Z-order/visibility change from *every application on the
desktop*, not just VS Code. They're now hooked per-process, scoped to each
tracked window's owning PID, ref-counted since multiple windows can share a
process (verified: on this machine, 3 VS Code windows all share one PID,
and the hook was registered exactly once, not three times). `EVENT_OBJECT_CREATE`
/ `SHOW` / `NAMECHANGE` stay global -- they're how brand-new,
not-yet-tracked VS Code processes get discovered in the first place, and
they're comparatively rare events (windows aren't created/renamed
continuously the way they're moved).

This wasn't independently measured in isolation (it's entangled with the
check-ordering fix above in the idle numbers), but it removes the single
highest-frequency event class in the Windows accessibility event system
from this app's hot path entirely, at the OS level, rather than filtering
it after delivery.

## No leaks under repeated resize

200 forced-repaint resize events fired at a target window in ~2 seconds
(synthetic worst case -- faster than a human resize-drag):

| Metric | Before | After |
|---|---|---|
| Handles | 194 -> 194 | 194 -> 194 |
| GDI objects | 6 -> 6 | 6 -> 6 |
| User objects | 13 -> 13 | 13 -> 13 |

Zero delta in both runs. `PaintOverlay`'s GDI resource handling
(`CreateDIBSection` / `SelectObject` / `DeleteObject` / `DeleteDC` /
`ReleaseDC`) was audited by hand for every early-return path and confirmed
leak-free by this stress test.

## Active-resize CPU cost (and a trade-off deliberately not taken)

The same 200-event burst measured **~45-55% of one CPU core** for its
~2 second duration. This is a real, non-zero cost, worth being honest
about:

- **It's synthetic worst-case.** 200 distinct-size repaints in ~2 seconds
  (one every ~10ms) is faster than most real mouse-driven resize dragging
  produces.
- **It's bounded to the resize's duration.** CPU returns to 0% the instant
  the window stops resizing -- confirmed in the idle measurements above,
  taken on the same build.
- **It doesn't accumulate.** Confirmed leak-free above; repeated resizing
  doesn't make this worse over a session.
- **It's proportional to pixel area.** A near-fullscreen window (this
  test's target) is the worst case; smaller windows cost proportionally
  less per repaint.

The dominant cost per repaint is `CreateDIBSection` allocating a fresh
buffer (up to several MB for a large window) and `UpdateLayeredWindow`
compositing it via DWM, both paid on every single resize event. A cheap,
safe win (dropping a redundant full-buffer `ZeroMemory` -- see
[ARCHITECTURE.md](ARCHITECTURE.md)) was applied. **A larger optimization
was considered and deliberately not implemented:** reusing a persistent,
monotonically-grown backing buffer per overlay (only reallocating when the
window's size exceeds previously-seen capacity) instead of allocating fresh
on every repaint. This would cut the alloc/free churn but requires manual
stride-vs-logical-size pixel addressing and careful GDI object lifecycle
management across resizes -- real complexity, with visual-glitch risk if
gotten wrong, to reduce a cost that's already transient, bounded, and only
material during active interactive resizing. Given this is meant to stay a
small, easy-to-reason-about codebase, that trade wasn't taken. If real
usage ever shows this mattering in practice, this is the optimization to
revisit.

## Summary

| State | CPU | Notes |
|---|---|---|
| Idle | ~0% | Purely event-driven; rescan timer rarely does real work |
| Actively dragging/resizing a window | up to ~50% of one core, synthetic worst case | Bounded to drag duration, returns to 0% immediately after, does not leak |

Memory and handle usage are flat over time in both states (measured over
idle windows and after repeated resize stress) -- the ~47MB working set is
almost entirely the statically-linked runtime, not per-window state.
