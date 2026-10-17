# Resizing WSL VS Code windows (WSLg)

Linux VS Code running inside a WSL distro shows up on the Windows desktop
through WSLg. Out of the box such a window can be resized from its top and
bottom edges, but not from its left and right ones. This page explains why,
and how it's fixed: patched WSLg modules (see
[WSLG-PATCHES.md](WSLG-PATCHES.md) for building and installing them) plus
this app's border overlay. It's optional: without the patched modules
everything else this app does for WSL windows still works, and the left/right
edges just stay unresizable.

## Why it doesn't work

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
margin, and WSLg's `WESTON_RDPRAIL_SHELL_LOCAL_MOVE` setting (it only hands
*moves* to Windows; resizes still hit problem 1). Snapping (Win+arrows) and
maximize do work, because msrdc and Weston handle those through a separate
path.

## How it's fixed

Two of the patched modules' patches, from
[Mihir-Null/wslg-resize-fix](https://github.com/Mihir-Null/wslg-resize-fix)
(MIT), reviewed:

| patch | module | what it does |
|---|---|---|
| `0001` | `rdprail-shell.so` | implements that TODO: resizes the surface (same conversion and min/max clamp as the snap path, maximized/fullscreen untouched) and applies the new position together with the resized buffer, so the window doesn't flash back to its old size. Same change as PR #176. |
| `0002` | `rdp-backend.so` | a control FIFO, `/mnt/wslg/runtime-dir/wslg-window-ctl` (mode `0600`, owner uid 1000), where each `move <window id> <left> <top> <right> <bottom>` line is handed to Weston's own, unmodified Client Window Move handler -- the same code a real move from msrdc runs. Malformed or out-of-range lines are rejected. |

The upstream project's own Windows helper (a Rust program watching for
resizes by tiling window managers) isn't used: **this app is the writer**. For
WSL windows, the border overlay gets an invisible 5px strip outside its border
-- an 8px grab zone with the 3px border, about the size of a Windows window's
own resize band -- that shows the matching resize cursor and resizes the
window from that edge or corner. Each step goes *only* down the FIFO: Linux VS
Code decides the size it actually takes (it may round, or hit a limit), and
WSLg's reply then sets the Windows frame to match. Setting the frame from here
as well leaves the two out of step whenever the app picks a slightly different
size -- the app then hangs outside its frame, and the border is drawn off it.
Requests are paced so only one is in flight at a time (the next goes once
WSLg has replied, or after 120 ms). A label drag sends the final position the
same way, so Linux keeps its menus and popups where the window actually is.

The strip only appears when that distro's WSLg has the control FIFO, i.e. the
patched modules are loaded; on stock WSLg it stays off (re-checked once a
minute), since a resize there would only produce the frame/app mismatch.

### The shadow margin

Linux VS Code is an X11 app under WSLg (it runs with `--ozone-platform=x11`),
and Weston's X11 window manager puts each such window inside a 32px
transparent margin for its drop shadow, which WSLg includes in the Windows
window rect. The border leaves it out by measuring it from the window's own
pixels (`PrintWindow`). The measurement is redone after a size change once the
size has held for 300 ms, and only kept if the margin is the same on all four
sides (it always is) -- one taken mid-change, or of a window whose frame and
app disagree, is thrown away. A brand-new window can measure as no margin at
all (its first frames come before the shadow), so a 0 margin is re-measured
every second for the window's first 30 seconds.

(The upstream project's patches 0003/0004 remove that margin, or the X11 frame
altogether; they aren't used here.)

## Check it's working

On top of [the modules being loaded](WSLG-PATCHES.md#check-theyre-loaded):

- `weston.log` has
  `RDP backend: wslg window control FIFO at /mnt/wslg/runtime-dir/wslg-window-ctl`,
  and `ls -la /mnt/wslg/runtime-dir/` shows `wslg-window-ctl` as a FIFO (`p`).
- This app's log has `wsl: distro [<distro>] has the WSLg window control FIFO`
  within a minute.
- Hovering just outside a WSL window's left or right edge shows ↔; dragging
  resizes the window *and* the app inside it.
