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
| `EVENT_SYSTEM_FOREGROUND` | Bringing a window to the front (e.g. alt-tab) is a pure z-order change with no move/resize, which `LOCATIONCHANGE` isn't reliably fired for. Without this, the overlay only restacks itself on the next safety-net rescan -- a noticeable lag when switching back to a VS Code window. |

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

## Label text: scraped from the window title, no other source exists

`window_title.cpp` derives the border label entirely from `GetWindowTextW`
-- there is no VS Code API or IPC an external Win32 process can use to ask
"what folder/repo is this window showing". This was confirmed by probing
`SHGetPropertyStoreForWindow` on live VS Code windows: it returns zero
properties (unlike, say, Chrome, which populates
`AppUserModel_RelaunchCommand`). Title scraping plus
`EVENT_OBJECT_NAMECHANGE` (see the window-detection table above) is the
only zero-cost option, so `ParseVSCodeTitle`'s parsing is tuned to this
repo's actual `window.title` setting:

```
${activeRepositoryName} - ${activeRepositoryBranchName} - ${folderName}
```

Outside a git repo the first two placeholders collapse to a literal `"-"`
each (see the comment on `ParseVSCodeTitle` for the exact title shapes this
produces), and the default un-customized VS Code title (`"file - folder"`
or just `"folder"`) is also handled as a fallback. If this setting changes,
`ParseVSCodeTitle` needs a matching update.

Because `${activeRepositoryName}` shows a git worktree's own folder name
rather than the main repo's, `worktree_resolver.cpp` separately maps that
back to the main repo name -- see the comments there for how.

## Known issue: label can occasionally blink

The label chip sits just inside the border, overlapping the target
window's own top-left corner (where VS Code renders its own title bar/tab
area). VS Code repaints that area on every focus change; our overlay
repaints its own layered surface independently via `UpdateLayeredWindow`.
Since these are two separate top-level windows compositing over the same
pixels, there's no Windows API to guarantee DWM always composites ours
*after* VS Code's -- occasionally it doesn't, and VS Code's real title
flashes through for a single frame before our label re-covers it.

Confirmed via `verbose_logging`: during a reproduction, the log showed only
repeated `EVENT_SYSTEM_FOREGROUND` events with no `REPAINT`/`HIDE` from our
own code, ruling out a bug in our repaint logic -- this is a genuine DWM
compositing-order race between two independent windows, not something we
control.

The only real fix is to never share pixels with the target's own content
in the first place (e.g. drawing the label entirely above the border,
outside the window's bounds, in space this app exclusively owns -- this
was prototyped and confirmed to eliminate the blink). That was reverted by
request in favor of keeping the label inset into the border's corner
(cosmetic preference), accepting the occasional blink as a known,
extremely minor trade-off.

## Why `claude_status.ini` state isn't cached in memory, unlike aliases/order

`label_alias.cpp`'s alias map and `project_list_order.cpp`'s saved order are
both loaded once and cached, only re-read from disk on an explicit trigger
(`ReloadAliases`, `RefreshAllLabels`) -- safe because the only writer of
those files is this app itself, so it always knows when its own cache goes
stale.

`claude_status.cpp` can't use that pattern: its files are written by
`claude_status_hook.ps1`, an entirely separate process (or several, one per
live Claude Code session) invoked by Claude Code's own hooks, completely
outside this app's control or knowledge. There's no event this app could
hook to learn "the status changed" the way a WinEvent hook tells it a
window moved. `LoadClaudeStatuses()` re-reads the small `claude_status\`
directory from disk on every call instead, which is acceptable because
`SyncProjectListHud()` (its only caller) already runs frequently -- nearly
every relevant WinEvent for a tracked window, plus the periodic rescan --
so this doesn't introduce a new I/O cadence, just adds a bit of work to one
that already exists.
