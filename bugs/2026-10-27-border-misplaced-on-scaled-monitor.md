# Border drawn in the wrong place and too big on a monitor with display scaling

- **Fixed:** 2026-10-27
- **Commit(s):** (this commit)
- **Area:** border placement

## Symptom

With the laptop (1920x1200, 100%) connected to an external 1920x1080 TV set to 150% scaling, borders
around VS Code windows on the TV were not on the window: the left edge ran down inside the window, and
the rest of the border was off to the right and below, mostly off-screen. Borders on the laptop screen
were fine.

## Evidence

From the diagnostic snapshot taken while it was happening (one of the two windows on the TV):

```
[phys] DISPLAY2 rect=(1920,0)-(3840,1080) effectiveDpi=144 (150%)
[app]  DISPLAY2 rect=(1920,0)-(3200,720)            <- the app's DPI-virtualized view
target dpi=144 awareness=per-monitor-aware | overlay dpi=96 awareness=unaware
[app]  window=(1953,-40)-(3168,767) 1215x807
[app]  visibleRect(used for border)=(1979,-58)-(3783,1142) 1804x1200   <- physical numbers, used as app numbers
[phys] window=(1970,-60)-(3792,1151) dwmFrame=(1977,-60)-(3785,1144) 1808x1204
[phys] overlay=(2004,-92)-(4719,1717) 2715x1809    <- ~1.5x too big, shifted right/down
[app]  overlay vs expected ... -> matches          <- the snapshot did not notice
```

`visibleRect` was the DWM frame unchanged, while the window rect next to it was in the app's (2/3 size)
coordinates.

## Root cause

The app declares no DPI awareness, so on a scaled monitor Windows gives it DPI-virtualized coordinates
(1 app px = 1.5 screen px at 150%) and scales its windows up to match. `GetVisibleWindowRect` used
`DWMWA_EXTENDED_FRAME_BOUNDS`, which is always in physical pixels whatever the caller's DPI awareness.
That physical rect was used as an app-space rect for the overlay, which Windows then scaled up by 1.5
again. On a 100% monitor the two coordinate spaces are the same, so this never showed before.

## Fix

`GetVisibleWindowRect` (`src/window_discovery.cpp`) maps the frame bounds into the app's coordinate space
through the window rect, which it reads twice: normally (virtualized) and inside a per-monitor-v2 thread
DPI context (physical). When the two are equal (100% monitor) nothing changes.

Making the whole process per-monitor DPI aware was the alternative. It would also make the border sharp
rather than bitmap-stretched on a scaled monitor, but every window (hub, usage strip, tooltips, label
chips) would then need to handle DPI itself, which is a much bigger change.

## Verification

- Re-ran the new mapping by hand on the snapshot's numbers: the border lands about 1-2 px outside the
  frame on each side, the intended placement.
- Rebuilt and ran on a single 100% monitor: every border unchanged (1 px outside the frame each side),
  no `[WARN]`.
- **Not** re-tested live on a scaled monitor (the TV wasn't connected while fixing).
- Known leftover: at 150% the 3 px border is stretched to about 4-5 px and slightly soft, because Windows
  scales the DPI-unaware overlay.

## Detection added

- The session log records the monitor layout (both coordinate views, with each monitor's scale) at
  startup and whenever it changes, and a `placement:` line per window when it lands on a different
  monitor or scale: physical and app rects, and the gap between border and window per side.
- The app measures each border's real on-screen position on every rescan and logs
  `[WARN] placement: border MISPLACED on screen` if it stays misplaced around a window that isn't moving.
- Diagnostic snapshots: a `PROBLEMS FOUND` summary at the top. Each window shows its app-to-physical scale
  and flags `MISPLACED on screen`. Pixel samples are also taken at the window's own edges, which still
  work when the border is somewhere else. The snapshot notes when the screen was locked, since the
  screenshot then shows the lock screen.

## History

- 2026-10-27: first fix.
