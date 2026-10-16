# Resizing WSL VS Code windows (WSLg)

Linux VS Code running inside a WSL distro shows up on the Windows desktop
through WSLg. Out of the box such a window can be resized from its top and
bottom edges, but not from its left and right ones. This page explains why,
and how to fix it with a small patch to WSLg's compositor plus this app's
border overlay. The patch is optional: without it everything else this app
does for WSL windows still works, and the left/right edges just stay
unresizable.

Tested on WSL with WSLg 1.0.73.2 (Weston `04d436c7`), Windows 11.

## Why it doesn't work

A WSLg window is a `RAIL_WINDOW` owned by `msrdc.exe`; the Linux app's real
surface lives in Weston (WSLg's compositor) inside the WSLg *system distro*.
Two separate problems stop a Windows-side resize from reaching the app:

1. **Weston ignores the size.** When Windows moves a WSLg window, Weston's
   `shell_backend_request_window_move()` (`rdprail-shell/shell.c`) moves it and
   stops at `//TODO: support window resize`. The Windows frame changes size
   while the app keeps drawing at its old size -- a big frame with a small app
   in its corner. Tracked upstream as
   [microsoft/wslg#22](https://github.com/microsoft/wslg/issues/22); a fix is
   proposed in [weston-mirror PR #176](https://github.com/microsoft/weston-mirror/pull/176)
   (not merged at the time of writing).
2. **msrdc doesn't report most resizes.** It only tells Weston about a move or
   resize when its own Win32 move/size loop ends. A plain `SetWindowPos` --
   which is all another app can do -- never reaches Weston at all.

The left/right edges fail because of how Linux VS Code's own resize works under
WSLg: WSLg runs moves/resizes on the Linux side, and for the side edges that
never results in a resize (the cursor does turn into ↔, but dragging does
nothing). Top and bottom work.

So no Windows-side trick works on stock WSLg. Things tried and ruled out, for
the record: `SetWindowPos` alone, wrapping it in
`WM_ENTERSIZEMOVE`/`WM_EXITSIZEMOVE`, posting `WM_SYSCOMMAND SC_SIZE`, pacing
the requests during a drag, posting mouse messages to the window's transparent
margin, Linux VS Code in X11 mode, and WSLg's `WESTON_RDPRAIL_SHELL_LOCAL_MOVE`
setting (it only hands *moves* to Windows; resizes still hit problem 1).
Snapping (Win+arrows) and maximize do work, because msrdc and Weston handle
those through a separate path.

## How it's fixed

Two Weston patches from
[Mihir-Null/wslg-resize-fix](https://github.com/Mihir-Null/wslg-resize-fix)
(MIT), reviewed and kept in [`tools/wslg-resize/patches`](../tools/wslg-resize/patches):

| patch | module | what it does |
|---|---|---|
| `0001` | `rdprail-shell.so` | implements that TODO: resizes the surface (same conversion and min/max clamp as the snap path, maximized/fullscreen untouched) and applies the new position together with the resized buffer, so the window doesn't flash back to its old size. Same change as PR #176. |
| `0002` | `rdp-backend.so` | a control FIFO, `/mnt/wslg/runtime-dir/wslg-window-ctl` (mode `0600`, owner uid 1000), where each `move <window id> <left> <top> <right> <bottom>` line is handed to Weston's own, unmodified Client Window Move handler -- the same code a real move from msrdc runs. Malformed or out-of-range lines are rejected. |

The upstream project's other two patches (0003/0004) only change X11 windows
and aren't needed for VS Code, which is a Wayland app. Its Windows helper (a
Rust program watching for resizes by tiling window managers) isn't needed
either: **this app is the writer**. For WSL windows, the border overlay gets
an invisible 5px strip outside its border -- an 8px grab zone with the 3px
border, about the size of a Windows window's own resize band -- that shows the
matching resize cursor and resizes the window from that edge or corner. Each
step goes to the window as a `SetWindowPos` (so the frame follows at once)
*and* down the FIFO (so Weston resizes the Linux app to match), paced so only
one size is in flight at a time. A label drag sends the final position the same
way, so Linux keeps its menus and popups where the window actually is.

The strip only appears when that distro's WSLg has the control FIFO, i.e. the
patched modules are loaded; on stock WSLg it stays off (re-checked once a
minute), since a resize there would only produce the frame/app mismatch.

The patched modules are loaded through **shims**: tiny modules that read the
running WSLg's Weston commit from `/mnt/wslg/versions.txt` and load
`weston-<commit>/<module>` next to them if it exists, else the stock module.
A module built for one WSLg loaded into another would crash Weston, so after
a WSL update the shims silently fall back to stock WSLg until you rebuild.

## Install

Everything below is per Windows user; no admin rights. `<distro>` is the WSL
distro running Linux VS Code.

1. **Check the WSLg version** -- the patches were reviewed against Weston
   `04d436c7`:

   ```powershell
   wsl.exe -d <distro> --system -e cat /mnt/wslg/versions.txt
   ```

2. **Get Weston's source at that commit**, as `weston-<commit>.tar.gz` in
   `tools/wslg-resize`. From a git checkout of
   [microsoft/weston-mirror](https://github.com/microsoft/weston-mirror) (or
   an empty repo):

   ```sh
   git fetch --depth 1 <weston-mirror remote> <commit>
   git archive --format=tar.gz -o <repo>/tools/wslg-resize/weston-<commit>.tar.gz FETCH_HEAD
   ```

   (Inside the WSLg system distro the CA bundle can't verify github.com, so the
   source is fetched on the Windows side.)

3. **Build** inside the WSLg system distro, as root. It works in a throwaway
   build root in the system distro's `/tmp` (that distro's `/` is discarded on
   `wsl --shutdown`) and installs nothing:

   ```powershell
   wsl.exe --cd / -d <distro> --system -u root -e bash /mnt/c/<path to repo>/tools/wslg-resize/build.sh
   ```

   It refuses an unreviewed Weston commit (rerun with `FORCE=1` after checking
   the patches still fit), and refuses to publish a module whose exports
   differ from stock or that imports a symbol the live system libraries don't
   provide. The result is in `tools/wslg-resize/out`.

4. **Copy the result** somewhere permanent, e.g.
   `%LOCALAPPDATA%\wslg-resize-modules` -- the two shims plus the
   `weston-<commit>` folder.

5. **Point WSLg at the shims** in `%USERPROFILE%\.wslgconfig` (WSLg reads it
   at start and passes `[system-distro-env]` entries to Weston;
   `WESTON_MODULE_MAP` is libweston's own module-path override):

   ```ini
   [system-distro-env]
   WESTON_MODULE_MAP=rdp-backend.so=/mnt/c/Users/<you>/AppData/Local/wslg-resize-modules/rdp-backend.so;rdprail-shell.so=/mnt/c/Users/<you>/AppData/Local/wslg-resize-modules/rdprail-shell.so
   ```

6. **Restart WSL**: `wsl --shutdown` (closes every running distro), then start
   Linux VS Code again.

## Check it's working

- `/mnt/wslg/weston.log` (readable from inside the distro) has
  `wslg-resize-fix: rdp-backend.so: loaded patched …` and
  `… rdprail-shell.so: loaded patched …`, and
  `RDP backend: wslg window control FIFO at /mnt/wslg/runtime-dir/wslg-window-ctl`.
- `ls -la /mnt/wslg/runtime-dir/` shows `wslg-window-ctl` as a FIFO (`p`).
- This app's log has `wsl: distro [<distro>] has the WSLg window control FIFO`
  within a minute.
- Hovering just outside a WSL window's left or right edge shows ↔; dragging
  resizes the window *and* the app inside it.

## After a WSL update

The shims find no `weston-<new commit>` build and load the stock modules:
WSLg keeps working, `weston.log` says
`no patched build for weston <commit> (rebuild wslg-resize-fix)`, and this app
turns the resize strip off. To get it back, repeat steps 1-4 for the new
commit (check the patches still apply and make sense for it) and
`wsl --shutdown`. Old `weston-*` folders can stay.

## Undo

Delete the `WESTON_MODULE_MAP` line from `%USERPROFILE%\.wslgconfig` (or the
file, if that was all it held) and run `wsl --shutdown`. Optionally delete
`%LOCALAPPDATA%\wslg-resize-modules`. If WSLg ever fails to start with the
modules in place, this is also the way out.
