# "focus denied" warning for a hub click that did activate the window

- **Fixed:** 2026-11-02
- **Commit(s):** (this commit)
- **Area:** focus tracing / hub (project-list HUD)

## Symptom

Clicking a *minimized* project in the hub while another app held the foreground raised a
`[WARN] focus denied` and badged the tray icon, saying the click had been lost -- but the window came
up normally. Nothing was highlighted in the taskbar and nothing was dropped: the only wrong thing was
the warning itself (and the tray badge it lights, which is the user-visible part).

## Evidence

Run `20261102-081903`, the only `focus denied` in the whole of `bin\logs\`, and the only one of 41 hub
activations in that run whose target was minimized:

```
13:16:58 hud activate reason=[hud-click] index=1 label=[project-a] -- restoring minimized target first
13:16:58 focus request reason=[hud-click] kind=external granted=0 lastError=5 weHeldForeground=0
13:16:58 focus request   target: hwnd=...B0DDE class=[Chrome_WidgetWin_1] title=[project-a - Visual Studio Code]
13:16:58 focus request   before: hwnd=...1059C class=[TeamsWebView] title=[Chat | Microsoft Teams]
13:16:58 focus request   after:  hwnd=...1059c (NOT the target)
13:16:58 [WARN] focus denied: ... The activation itself is lost, not retried. ...
13:16:58 foreground -> hwnd=...B0DDE class=[Chrome_WidgetWin_1] title=[project-a - Visual Studio Code] tracked=1 source=hud-click
13:16:59 focus denied: taskbar highlight cancelled again after 250ms (settle pass), hwnd=...b0dde
```

The line *after* the warning is the target taking the foreground, in the same second, attributed to the
same click. So the activation was not lost. The tell that this was not the known flyout case: `before:`
is an ordinary app window, not a shell panel, and there is no repeat click -- the next `hud-click` on
that item is 105 s later, with the user working in other apps in between.

## Root cause

`ActivateProjectListItem` (`src/project_list_hud.cpp`) restored a minimized target and then asked for
the foreground:

```cpp
if (minimized) ShowWindow(target, SW_RESTORE);
RequestForeground(target, FocusTargetKind::ExternalWindow, reason);
```

`ShowWindow(SW_RESTORE)` on a window owned by another process activates it, but asynchronously -- it
returns long before that activation reaches the foreground. In that gap neither this app nor the target
is the foreground window, so `SetForegroundWindow` is refused with `ERROR_ACCESS_DENIED` (5), and
`RequestForeground` warns synchronously on `!ok` without ever re-checking. Milliseconds later the
restore's own activation landed and the window came forward.

Two further consequences of the bogus denial, both fixed by the same change: the `FLASHW_STOP` and its
settle pass cancelled a highlight on a window that had just *taken* the foreground (harmless, but it is
why `taskbar highlight cancelled again` appeared for a window that was never flashing), and
`g_lastRequestGranted` was left false, so `NoteForegroundChange` filed the target's arrival in the
foreground-churn queue as `deniedRequestBehind` -- i.e. as evidence toward a `focus storm`, which it
was not.

## Fix

`RequestForegroundRestoringIfMinimized` in `src/focus_trace.cpp`. For a target that is not minimized it
is plain `RequestForeground`. For one that is, it restores, claims attribution for the foreground change
the restore is about to produce (so the trace calls it `source=<reason>` and keeps it out of the churn
queue), and schedules a 250 ms settle pass -- the same delay the flash-cancel uses. At that point:

- the target holds the foreground: log `granted by the restore itself`, record a *granted* attempt, no
  warning;
- it does not: log that, then make the ordinary `RequestForeground`, which warns if that is refused.

This is not the retry the file deliberately avoids: at the settle pass no request has been made yet, so
nothing is being re-asked behind the user's back. `ActivateProjectListItem` and
`EndProjectListHoverFocus` (mouse-leave restore of the previous foreground, same race) now call it and
no longer restore the window themselves.

Turned down: simply dropping the warning when the target had been minimized. That hides a real denial on
the same path -- if the restore does not activate (a target on another desktop, a foreground lock held
by something else), the click really is lost and should still warn.

## Verification

Built clean, app restarted (run `20261102-133549`), zero `[WARN]`.

Not verified live: the fixed path itself. Reaching it needs a hub click on a minimized project while
another app holds the foreground, i.e. real desktop input, which was not performed. When it is reached,
a working instance logs `focus request reason=[hud-click] kind=external deferred: restored a minimized
target ...` followed by `granted by the restore itself within 250ms`; a genuine failure still logs the
`[WARN] focus denied`.

## Detection added

The two new log lines above make the deferred path observable either way (granted-by-restore vs. asked
anyway), which is what the original warning lacked -- it reported a verdict it had not waited for.

## History

- 2026-11-02: first fix (this commit).
