# Architecture notes

This document records the non-obvious design decisions in
`src/vscode_border.cpp` -- the "why", not the "what" (the code comments
cover the what). Written up so a future change doesn't accidentally
re-introduce a bug that was already found and fixed once.

## Why a custom-drawn overlay instead of Windows' native border-color API

Windows 11 exposes `DWMWA_BORDER_COLOR` via `DwmSetWindowAttribute`, which
recolors a window's native frame directly -- no extra window, no drawing
code. The first version of this app used exactly that. It was abandoned for
two reasons, found the hard way:

1. **It silently depends on a system personalization setting.** With
   "Show accent color on title bars and window borders"
   (`HKCU\Software\Microsoft\Windows\DWM\ColorPrevalence`) turned off,
   Windows renders a default gray border and ignores the app-requested
   color -- `DwmSetWindowAttribute` still returns `S_OK`. This was only
   diagnosed by sampling actual screen pixels and finding gray instead of
   the requested RGB.
2. **It cannot control thickness.** Only color is exposed; thickness is a
   fixed OS metric. The only way to change it is a global registry border
   width that affects every window on the system, not just VS Code.

The overlay approach draws its own thin layered popup window per VS Code
window instead, so it works regardless of that system setting and thickness
is just a config value.

## Z-order: `SetWindowPos(w, insertAfter, ...)` puts `w` *behind* `insertAfter`

This is the single easiest thing to get backwards in this codebase.
`hWndInsertAfter` names the window that ends up **in front of** the window
being positioned -- not behind it. To put the overlay in front of its
target, you must insert the overlay behind whatever currently sits in front
of the target:

```cpp
HWND aboveTarget = GetWindow(target, GW_HWNDPREV); // window currently in front of target
SetWindowPos(overlay, aboveTarget, ...);           // overlay now sits between them
```

**The trap:** once the overlay is correctly stacked, `GetWindow(target,
GW_HWNDPREV)` returns the overlay itself. A single combined
`SetWindowPos(overlay, aboveTarget, x, y, ...)` call reused for every sync
becomes self-referential on the second call onward and silently stops
applying updates -- the overlay freezes in place forever, looking exactly
like a "stuck" bug rather than a logic bug. `SyncOverlay` splits this into
two calls: an unconditional `SWP_NOZORDER` move, and a separate Z-order fix
that's skipped entirely once the overlay is already correctly stacked.

## Tight border: `DWMWA_EXTENDED_FRAME_BOUNDS`, not `GetWindowRect`

`GetWindowRect` on a modern Win32 window includes an invisible resize
hit-test margin -- about 7px larger than what's actually visible on every
side except the top. Inflating that rect outward by the border thickness
leaves a visible gap between the window and the border. `DwmGetWindowAttribute(hwnd,
DWMWA_EXTENDED_FRAME_BOUNDS, ...)` gives the true visible bounds instead,
confirmed by measuring both and comparing to the overlay's actual rect.

## Window detection: which events, and why more than one

A VS Code window is tracked when it passes `IsCandidateTopLevelWindow`
(visible, unowned, not a tool window, `Chrome_WidgetWin_1` class, non-empty
title) *and* its process is `Code.exe`/`Code - Insiders.exe`. Several
events can trigger a (re-)check, each catching a case the others miss:

| Event | Why it's needed |
|---|---|
| `EVENT_OBJECT_CREATE` / `EVENT_OBJECT_SHOW` | The obvious "a window appeared" signal. |
| `EVENT_OBJECT_NAMECHANGE` | A new window is often shown *before* its title is set. Without this, a window whose title was empty at SHOW-time would sit untracked until the next safety-net rescan. |
| `EVENT_OBJECT_DESTROY` | Removes the overlay and frees the window's color slot. |
| `EVENT_OBJECT_LOCATIONCHANGE` | Move/resize/Z-order/visibility changes -- keeps the overlay glued to its target. |

## Why there's still a periodic rescan, and what else was considered

`SetWinEventHook` with `WINEVENT_OUTOFCONTEXT` (what this app uses) is
documented by Microsoft as able to silently skip a notification if the
receiving app doesn't process it fast enough. There is no purely
event-driven guarantee with this API. The rescan timer
(`rescan_interval_ms` in config, default 1500ms) exists purely as a cheap
backstop for that documented gap -- not as the primary detection mechanism
(it rarely if ever fires the actual tracking decision; the event hooks do).

Two alternatives were considered and rejected:

- **`WINEVENT_INCONTEXT` hooks** -- same API, but Windows injects a DLL
  into every process that fires the event (including every VS Code
  process) and runs the callback synchronously in-process. More reliable
  delivery, but requires shipping and injecting a DLL system-wide, which is
  exactly the kind of behavior antivirus/EDR software flags. Not a good
  trade for a small utility.
- **`RegisterShellHookWindow`** -- an older, message-based mechanism (what
  classic taskbar-replacement tools use) delivering `HSHELL_WINDOWCREATED`
  / `HSHELL_WINDOWDESTROYED` as window messages instead of hook callbacks.
  Same reliability class as `SetWinEventHook`, just a different pipe -- not
  meaningfully more guaranteed, so not worth the added code path.

## Performance-sensitive design choices

See [PERFORMANCE.md](PERFORMANCE.md) for the full review and measured
numbers. In summary, three choices matter most:

1. **Per-process-scoped `LOCATIONCHANGE`/`DESTROY` hooks.** These two
   events are hooked with `idProcess` set to each tracked window's owning
   PID (ref-counted, since one VS Code process can own several windows),
   not globally. A global registration would mean every window move/resize
   from *every app on the desktop* invokes this app's callback.
2. **Cheap checks before expensive ones.** `TrackWindow` runs the
   in-process checks (class name, visibility, owner, title) before the one
   call that's an actual kernel round-trip (`OpenProcess` +
   `QueryFullProcessImageNameW`). The periodic rescan calls `EnumWindows`
   over every top-level window on the desktop, so this ordering matters.
3. **No full-buffer clear in `PaintOverlay`.** `CreateDIBSection` always
   returns a freshly committed buffer, which Windows guarantees is already
   zero-filled. An explicit `ZeroMemory` over the whole buffer would be a
   second full-buffer pass for no benefit.
