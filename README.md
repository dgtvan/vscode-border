# vscode-border

Draws a colored border around every VS Code window so you can tell windows
apart at a glance. Each window gets its own color, assigned automatically.

![Four VS Code windows, each with a distinct colored border](assets/demo.png)

## How it works

Each VS Code top-level window gets its own lightweight, click-through,
layered popup window painted as a hollow colored rectangle just outside the
target window's bounds. The overlay tracks its target's position, size,
Z-order, and visibility via `SetWinEventHook` (mainly
`EVENT_OBJECT_LOCATIONCHANGE`) -- no polling/redraw loop while idle. A slow
timer (`rescan_interval_ms` in `config.ini`) exists only as a safety net for
any missed event.

This means it works regardless of Windows theme/personalization settings,
and even when a window is maximized (the frame just gets clipped at whatever
screen edge the window is flush against).

For the non-obvious design decisions behind this (Z-order gotchas, why a
custom overlay instead of Windows' native border-color API, why there's
still a periodic rescan and what alternatives were considered), see
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md). For measured CPU/RAM/handle
numbers and the optimizations that came out of them, see
[docs/PERFORMANCE.md](docs/PERFORMANCE.md).

## Build

Requires the WinLibs MinGW-w64 g++ toolchain (installed via
`winget install BrechtSanders.WinLibs.POSIX.UCRT`).

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1
```

Produces `bin\vscode_border.exe` (statically linked -- no MinGW runtime DLLs
needed) and copies the default `config.ini` into `bin\` if one isn't already
there.

## Run

```powershell
Start-Process bin\vscode_border.exe
```

It runs in the background with a tray icon (right-click for **Reload
Config** / **Open Config** / **Open Log Folder** / **Exit**). To launch it
automatically at login, see [docs/AUTOSTART.md](docs/AUTOSTART.md).

If something unexpected happens during a run (e.g. a VS Code window's
title doesn't match any recognized shape -- see [Getting the repo/branch
label](#getting-the-repobranch-label-instead-of-just-the-folder-name)), the
tray icon gets a small yellow "!" badge and its tooltip changes to say
so. **Open Log Folder** jumps straight to the current session's log file
under `bin\logs\` (set `verbose_logging=true` in `config.ini` for full
per-event tracing on top of that). Every launch starts a new file rather
than overwriting the last one -- so closing the app (even by accident) never
loses whatever was already logged -- named
`vscode_border_<session-start-timestamp>_<part>.log`; a long-running
session gets a new part once a day (and, as a safety net, if a single part
grows past 5 MB) without losing the rest of that session's history, and
files sharing the same `<session-start-timestamp>` prefix are all part of
the same run, in `part` order. Files older than 14 days are deleted
automatically on the next startup. The warning badge only ever reflects
the current run -- restarting the app is what clears it (past runs' logs
are still there under `bin\logs\` to look at, they just don't affect the
badge).

## Configuration

Edit `bin\config.ini` (copied there from the template at the repo root on
first build):

| Key | Meaning |
|---|---|
| `thickness` | Border width in pixels, drawn just outside each window. |
| `opacity` | 0 (invisible) - 255 (fully opaque). |
| `rescan_interval_ms` | Safety-net rescan interval; new/closed windows are normally detected instantly. |
| `colors` | Comma-separated `RRGGBB` hex colors, assigned round-robin to windows. |
| `show_label` | Show the folder/repo name as a label chip inside the border's top-left corner (`true`/`false`). |
| `show_project_list` | Show an interactive project list HUD at the bottom-right of the desktop, using the same labels/colors as the border chips; includes minimized windows so it doubles as a restore list (`true`/`false`). |
| `project_list_style` | Project list HUD layout: `horizontal` (single strip, all items share one width) or `vertical` (stacked list, shared column width). Both are resizable by Ctrl+left-click-dragging either edge and movable by Ctrl+left-click-dragging the middle; a plain right-click opens a context menu (**Set Alias**, and **Add to Favourites** -- see below). Default `horizontal`. |
| `project_list_order` | Item order: `auto` (sorted by window left edge) or `manual` (drag items with a plain left-click to arrange them yourself -- see below). Default `auto`. |
| `project_list_opacity_normal` | Project list HUD opacity when no item is hovered: 0 (invisible) - 255 (fully opaque). |
| `project_list_opacity_hover` | Project list HUD opacity for the currently hovered item: 0 (invisible) - 255 (fully opaque). |
| `project_list_activate_on_hover` | Temporarily activate the matching VS Code window when hovering a project-list item (`true`/`false`, default `false`). |
| `label_height` | Label chip height in pixels. |
| `label_font_size` | Label text font size in points. |
| `label_text_color` | Label text color: a hex `RRGGBB` color, or `auto` to pick black/white based on contrast against the border color. Default `000000` (black). |
| `ai_indicator_enabled` | Show the AI status indicator on project list HUD items (`true`/`false` -- see below). Default `true`. |
| `ai_indicator_provider` | Comma-separated AI services to source it from: `claude` (implemented) and/or `copilot` (reserved, not yet implemented). |
| `ai_indicator_color_working` / `_attention` / `_waiting` | Hex `RRGGBB` colors for the 3 states -- see below. |
| `ai_indicator_border_color` | Indicator border color: `auto` (black/white by contrast) or a fixed hex `RRGGBB`. Default `auto`. |

After editing, use the tray icon's **Reload Config** to apply changes
without restarting -- except for `colors`, which only takes effect on the
next launch (windows already tracked keep the color they were assigned).

### Project list HUD manual ordering

With `project_list_order=manual`, plain left-click-and-hold on an item (as
opposed to a plain click, which activates its window) drags it to a new
position: as you move it, the other items shift live to open a gap at the
candidate drop spot, and the item you're dragging floats above them,
following the cursor, so you can always tell exactly where it'll land
before you let go. Windows that appear for the first time are appended at
the end until you place them. The order is matched by label text (same
caveat as aliases: two windows sharing an identical label are kept
adjacent to each other, not independently ordered) and is remembered
across restarts, stored in `project_list_order.ini` next to `config.ini`.
Switching `project_list_order` back to `auto` falls back to window-left-edge
sorting; switching back to `manual` restores the last saved arrangement.

### Project list HUD aliases

Right-click any item (or its border label chip -- same underlying label)
and choose **Set Alias** to rename how it displays, without touching the
actual folder/repo/branch. The item's text turns into an editable box in
place -- type the new name and press Enter (or click elsewhere) to save it,
or Escape to cancel; clearing the text back to empty removes the alias.
Once an item has an alias, the same menu also offers **Reset Alias**, which
clears it immediately without opening the edit box. Aliases are matched by
the label's exact text, so if two windows ever happen to produce the
identical label, aliasing one aliases both. Stored in `label_aliases.ini`
next to `config.ini`, and re-read on **Reload Config** as well as at
startup.

### Project list HUD favourites

The fixed "+" square at the end of the project list HUD opens a brand-new,
empty VS Code window on a plain click. Right-clicking it instead opens a
menu of saved favourite projects/folders, one submenu per favourite --
hover a favourite's name to expand it to **Open in New Window** (bolded as
the default action) and **Remove from Favourites**.

To add a favourite, right-click any regular item (or its border label
chip) and choose **Add to Favourites** -- this saves the window's currently
open folder (resolved to a real absolute path via the same workspaceStorage
lookup the AI status indicator and worktree name substitution use, so it
only works for a folder VS Code has actually recorded there; a multi-root
workspace, for instance, isn't covered, and the menu item is left off
entirely when there's nothing to save, or when that folder is already a
favourite). Removal only happens from the "+" button's own menu -- there's
no "Remove from Favourites" on a regular item, so there's exactly one place
to do it. Favourites are matched by folder path, not label text, and stored
in `favourites.ini` next to `config.ini`.

### AI status indicator

Each HUD item can show a small status indicator in its top-right corner
for an AI coding assistant running in that VS Code window's terminal(s):

- **Working** -- amber, a small square chasing itself around an 8-position
  ring -- actively generating a response or running a tool (from
  `UserPromptSubmit` until the turn's `Stop`/`StopFailure`/`SubagentStop`).
- **Attention** -- red, a single pulsing square -- blocked mid-turn on a
  permission prompt or an MCP server asking the user something. This is
  the state that most needs you to look at it.
- **Waiting** -- green, a single static square -- the turn has finished,
  ready for your next prompt.
- No indicator at all if nothing's running there.

Claude Code's `Stop`/`StopFailure`/`SubagentStop` payloads also carry a live
`background_tasks` list (anything still running in the background -- a dev
server, a watcher, a subagent). An earlier version of this feature treated a
non-empty list as still "Working", but real usage showed that doesn't work:
a background task has no way to signal "about to finish" versus "started
once and will just sit there running for the rest of the session" -- both
report the same live status on every check, so a long-lived process (e.g. a
dev server) pinned the indicator to "Working" indefinitely, long after the
conversation itself was done. Rather than chase staleness thresholds that
can't fix that (a window short enough to avoid the false positive provides
no signal either way), this was removed -- `background_tasks` is still
logged for visibility (`bin\logs\claude_hook_events.log`) but no longer
affects the indicator.

**Attention** can also go stale in a way `background_tasks` couldn't self-heal
from at all: there's no "permission granted, resuming" hook (unlike MCP
elicitation, which has `ElicitationResult`), so once a permission prompt
fires, nothing updates the status again until the turn's eventual `Stop` --
even if you answered it seconds later and Claude went right back to work.
Rather than guess with a timeout (any guess is wrong either for a prompt
you haven't actually answered yet, or for one you have), `claude_provider.cpp`
cross-checks against real evidence: Claude Code writes a transcript file per
session under `~/.claude/projects/<encoded-cwd>/<session_id>.jsonl`, gaining
a new line for every message and tool call regardless of which hooks are
wired up. If that file has been written to more than 10 seconds after the
status file's last hook-reported snapshot, that's independent proof
something happened more recently than what the hooks told us -- so the
status is corrected to "Working" instead of staying on a stale "Attention"
(or "Waiting"). Confirmed against a real stuck case: a `PermissionRequest`
fired and froze the status on "Attention" while the transcript kept growing
for another 14 minutes until the turn's `Stop` finally arrived -- this check
catches exactly that gap. Degrades harmlessly if the transcript file can't
be found (e.g. a future Claude Code version changes this layout) -- the
status just stays as last reported by the hooks, same as before this
existed.

**Working** has the opposite gap: `Stop` does not fire when the user
manually interrupts a turn (Escape/Ctrl+C mid-response), confirmed against
a real session that stayed reported as "Working" for over 20 minutes after
being interrupted, with no hook ever firing again. Rather than another
timeout, this uses an even more direct piece of evidence: Claude Code
appends a literal `[Request interrupted by user]` marker to the transcript
the moment an interrupt happens. If a session's status is "Working" and the
transcript's very last entry is that exact marker (meaning nothing has
happened since), it's corrected to "Waiting" -- an interrupted turn is over,
same as a normal one.

The transcript directory name is derived from the project's absolute path:
every non-alphanumeric character (`:`, `\`, `.`, a space, an already-literal
`-`, etc.) becomes its own `-`, not collapsed -- confirmed against real
directories on disk, including a worktree path with a literal `.` in it
(`...\repo.worktrees\...` became `...-repo-worktrees-...`, the `.` isn't
special-cased).

If a window has more than one session running (e.g. several terminals),
the indicator reflects the aggregate, in that priority order: Attention
beats Working beats Waiting.

Controlled by `config.ini`:

```ini
ai_indicator_enabled=true      ; turn the whole feature on/off
ai_indicator_provider=claude   ; comma-separated: "claude" today, "copilot" reserved for later
ai_indicator_color_working=FF9F0A
ai_indicator_color_attention=DC2626
ai_indicator_color_waiting=34C759
ai_indicator_border_color=auto ; "auto" (black/white by contrast) or a fixed hex RRGGBB
```

Claude Code has no built-in way for another app to observe its state, so
this works via Claude Code's **hooks**: `claude_status_hook.ps1` (repo
root, copied to `bin\` on every build) runs on several of Claude Code's
lifecycle events and writes a small status file per session under
`bin\claude_status\`. This app manages the hook registrations in
`%USERPROFILE%\.claude\settings.json` for you -- automatically, on
startup and every **Reload Config** -- adding them when
`ai_indicator_enabled=true` and `claude` is in `ai_indicator_provider`,
removing them otherwise. No manual JSON editing needed. It only ever
touches its own entries (matched by their exact command string, tracked in
`bin\ai_hooks_installed.ini` so a relocated install can still find and
clean up old ones) -- everything else in that file, including hooks you've
added yourself, is left alone. If it can't safely parse that file (e.g. a
hand-edit broke its JSON), it logs a warning, sets the tray icon's warning
badge, and leaves the file untouched rather than risk overwriting it -- see
`src/claude_provider.*` and `src/json_scan.*`.

This is Claude Code's implementation of a generic `AiProvider` interface
(`src/ai_provider.h`) -- installing/removing whatever config it needs and
reading back whatever it reported are the two things any AI assistant
integration has to do, so a future Copilot integration would plug in the
same way, behind the same interface, without changing anything else that
uses it (`tracking.cpp`'s window-matching).

A hook here applies to every Claude Code session on the machine, in any
project -- not just this repo. Changes to `~/.claude/settings.json` are
picked up by already-running sessions automatically (typically within a
few seconds), so nothing needs restarting for it to take effect. Each
status file is matched to a tracked window by folder (a real absolute-path
match when VS Code has recorded one, e.g. via `workspaceStorage`, falling
back to a plain name comparison otherwise -- see `src/claude_provider.*`
and `src/worktree_resolver.*`). Stale/abandoned status files are cleaned
up automatically -- see "Known limitations" below for exactly how.

### Project list HUD position/size memory

Ctrl+left-click-dragging the project list HUD to move or resize it is
remembered separately per **monitor scenario** -- the current set of active
monitors, identified by hardware id (so unplugging/replugging or switching
a projector between "extend" and "second screen only" doesn't lose the
placement you set for each one). Moving it while on a laptop's built-in
screen, then again after connecting an external monitor, remembers both
independently and switches between them automatically as monitors are
connected/disconnected -- no manual re-positioning needed each time. A
scenario that's never been manually placed falls back to the default
bottom-right auto-placement. Stored in
`project_list_hud_positions.ini` next to `config.ini`.

### Getting the repo/branch label instead of just the folder name

![Label chip showing repo/branch on one window and the folder name on another](assets/label.png)

The label is scraped from the VS Code window's title -- there's no other
way for an external app to ask VS Code what folder a window has open. By
default VS Code's title only contains the folder name, so to show the git
repo + branch instead, VS Code's `window.title` setting needs to be:

```json
"window.title": "${activeRepositoryName} - ${activeRepositoryBranchName} - ${folderName}"
```

**This is applied automatically -- no manual settings.json editing
needed.** Whenever `show_label=true` (the default), this app writes that
`window.title` value into VS Code's global `settings.json` for you (both
stable and Insiders, whichever are installed), the moment it starts or
Reload Config runs. If `window.title` already had some other value, the
original is remembered (in `window_title_state.ini` next to `config.ini`)
and restored automatically the next time you set `show_label=false` and
reload. If `window.title` wasn't set at all, it's removed again on
disable, leaving the file exactly as it was found.

If `window.title` already happened to have exactly that same value before
this app ever touched it (e.g. you'd set it manually per an older version
of this doc), it's left alone and not tracked for restoration -- turning
the label off later won't remove it. If you edit `window.title` yourself
while the label is on, that edit is respected: this app notices the value
no longer matches what it wrote and stops managing it rather than
overwriting your change.

With this set, the label shows `repo - branch` when the window is inside a
git repo, and falls back to just the folder name otherwise (e.g. a plain
folder with no `.git`, or an empty window). If you use git worktrees, VS
Code's `${activeRepositoryName}` shows the *worktree's* folder name rather
than the main repo's -- this app corrects that automatically as long as
your worktrees follow the `<mainRepo>.worktrees\<worktreeName>` layout, by
reading VS Code's own recently-opened-folder records. That mapping is
built once and cached in memory; use **Reload Config** from the tray icon
to pick up worktrees created after the app started.

## Known limitations

- Only tracks VS Code's own top-level windows (`Code.exe` / `Code -
  Insiders.exe`, main editor windows -- not floating panels/dialogs).
- If you have more open windows than colors in the palette, colors repeat.
- Border thickness/color is drawn by this app, not by Windows -- closing the
  app removes all borders immediately (nothing persists in the registry or
  window state).
- Deleting/uninstalling this app while `show_label=true` leaves the
  `window.title` edit it made in VS Code's `settings.json` in place --
  there's no uninstall hook to restore it. Set `show_label=false` and let
  it reload (or start it once more) before removing the app if you want
  that reverted first.
- If a terminal running Claude Code is killed abruptly (not a clean `/exit`
  or closing the terminal normally), its `SessionEnd` hook never fires --
  Claude Code's hook payload has no process id or other liveness signal in
  it at all, so there's no *documented* way to detect that. This app works
  around it anyway: `claude_status_hook.ps1` walks up its own process
  ancestry looking for the real `claude.exe` process and records that pid,
  and `claude_provider.cpp` does a live lookup of that pid (via
  `OpenProcess`/`QueryFullProcessImageNameW`, confirming it's *still*
  claude.exe and not some unrelated process Windows has since reused that
  pid for) every time it reads statuses -- a confirmed-dead session is
  deleted automatically, no manual cleanup needed in the normal case. This
  relies on undocumented internals (specifically, that Claude Code's hook
  processes are descended from the main `claude.exe` process a small,
  bounded number of hops up) that could change in a future Claude Code
  update; if the ancestry walk ever fails to find `claude.exe`, that
  session's status is logged (`claude_provider: ... has no pid recorded`)
  and falls back to a much coarser, generous 24-hour staleness check
  instead (a status with no pid that hasn't been updated in over a day is
  treated as abandoned) -- deliberately not applied to the normal, precise
  pid-based path, since a genuinely long-running task can legitimately stay
  "working" for 30+ minutes and a short timeout would misfire on it there.
  Between the two, every case resolves on its own -- there's no manual
  clear/recovery action, by design, since one shouldn't be needed.
- Claude Code status matching falls back to a plain folder-name comparison
  when VS Code hasn't recorded that folder's real path (e.g. multi-root
  workspaces aren't covered) -- two windows opened on different folders
  that happen to share the same name are indistinguishable in that case.
- Deleting/uninstalling this app while `ai_indicator_enabled=true` leaves
  the hook entries it added in `~/.claude/settings.json` in place -- same
  caveat as `window.title` above, no uninstall hook runs. Claude Code will
  keep harmlessly invoking a now-missing script path. Set
  `ai_indicator_enabled=false` and let it reload (or start it once more)
  before removing the app if you want those removed first.

## Project layout

- `src/vscode_border.cpp` -- tray icon and app lifecycle (`wWinMain`).
- `src/file_util.*` -- shared exe-relative path / file-read / file-write helpers.
- `src/text_util.*` -- shared UTF-8-safe line-escaping helpers for the small `*.ini` state files below.
- `src/monitor_scenario.*` -- identifies the current active-monitor set and persists/recalls the
  project list HUD's placement per scenario (see "Project list HUD position/size memory" above).
- `src/label_alias.*` -- persists/recalls user-set label aliases (see "Project list HUD aliases" above).
- `src/project_list_order.*` -- persists/recalls the manually-dragged item order (see "Project list HUD
  manual ordering" above).
- `src/favourites.*` -- persists/recalls the "+" button's favourite projects list (see "Project list HUD
  favourites" above).
- `src/ai_provider.h` -- abstract interface an AI coding assistant integration implements (install/remove its
  config, read back its reported status, manual clear) -- see "AI status indicator" above.
- `src/claude_provider.*` -- Claude Code's `AiProvider` implementation: installs/removes its hook
  registrations in `%USERPROFILE%\.claude\settings.json`, reads the per-session status files
  `claude_status_hook.ps1` writes, and auto-expires ones whose recorded pid is no longer a live
  `claude.exe` process (see "Known limitations" above).
- `src/json_scan.*` -- minimal JSONC span-scanner (find/insert/remove one key or array element without
  parsing/reserializing the rest of the file), generalized from `vscode_settings.cpp`'s scanner.
- `src/logger.*` -- file logging.
- `src/config.*` -- `config.ini` loading.
- `src/window_title.*` -- VS Code window title parsing (repo/branch/folder).
- `src/worktree_resolver.*` -- maps a git worktree's folder name back to its main repo name, and resolves a
  folder's real absolute path (used for the AI status indicator's window matching).
- `src/vscode_settings.*` -- auto-manages VS Code's `window.title` setting to match `show_label` (see [Getting the repo/branch label](#getting-the-repobranch-label-instead-of-just-the-folder-name)).
- `src/window_discovery.*` -- finding/filtering VS Code top-level windows.
- `src/overlay.*` -- passive per-window border overlay creation/painting.
- `src/project_list_hud.*` -- interactive desktop project-list HUD: sorts/sizes/positions itself from the entries it's given, plus hover focus, click, move, and resize behavior.
- `src/layered_rendering.*` -- shared alpha-blended text helpers for layered windows.
- `src/tray_icon.*` -- tray icon warning-badge compositing.
- `src/tracking.*` -- tracked-window bookkeeping, WinEvent hooks, border sync, and feeding the HUD its entries.
- `src/resource.rc`, `assets/app.ico`, `assets/square-dashed.png` -- tray icon resource (`app.ico` is generated from the PNG; see [Credits](#credits)).
- `config.ini` -- default config template (copied to `bin\` on first build).
- `claude_status_hook.ps1` -- Claude Code hook script (copied to `bin\` on every build; see "Claude Code
  status dots" above).
- `build.ps1` -- build script.
- `install-autostart.ps1` / `uninstall-autostart.ps1` -- autostart management, see [docs/AUTOSTART.md](docs/AUTOSTART.md).
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) -- design decisions and why.
- [docs/PERFORMANCE.md](docs/PERFORMANCE.md) -- measured resource usage and optimizations.

## Credits

Tray icon: ["Square dashed" icon](https://www.flaticon.com/free-icon/square-dashed_17295663) from Flaticon.
