# Pasting Windows screenshots into Linux VS Code (WSLg)

A screenshot copied on Windows (Win+Shift+S, Print Screen, a "Copy image"
anywhere) pastes fine into Windows VS Code -- e.g. into the Claude Code
extension -- but not into Linux VS Code running in a WSL distro: nothing
happens, or the extension says there's no image. This has nothing to do with
this app; it's a gap in WSLg's clipboard bridges, fixed with two more patches
in the same patched WSLg modules (see [WSLG-PATCHES.md](WSLG-PATCHES.md) for
building and installing them).

## Why it doesn't work

Linux VS Code is an X11 app under WSLg (it runs with `--ozone-platform=x11`),
so an image copied on Windows has to cross two bridges inside WSLg's Weston to
reach it -- and neither passes it on in a form VS Code reads:

1. **Windows → Wayland** (`libweston/backend-rdp/rdpclip.c`). WSLg's
   clipboard table maps a Windows image only from `CF_DIB`, to `image/bmp`,
   and has no entry for the `PNG` format screenshot tools and most apps put on
   the clipboard too. Chromium/Electron -- VS Code, and so its webviews, such
   as the Claude Code extension's panel -- only reads `image/png`.
2. **Wayland → X11** (`xwayland/selection.c`). Weston's X11 clipboard bridge
   only ever offers X11 apps `UTF8_STRING`/`TEXT` and refuses any other
   target. Even with `image/png` on the Wayland side, an X11 app sees text
   only.

You can see both from inside the distro with a screenshot on the Windows
clipboard: `wl-paste --list-types` (Wayland side) shows `image/bmp` only, and
`xclip -selection clipboard -t TARGETS -o` (X11 side, what VS Code reads)
shows only text targets.

## How it's fixed

| patch | module | what it does |
|---|---|---|
| `0003` | `rdp-backend.so` | maps Windows' registered `"PNG"` format to `image/png`, matched by format name like the existing `"HTML Format"` and `"Rich Text Format"` entries, with a pass-through handler: the existing raw-text one trims trailing NUL/newline bytes on the way in and appends a NUL on the way out, which a binary format mustn't get. Linux then sees both `image/png` and `image/bmp`. |
| `0004` | `xwayland.so` | also advertises an `image/png` target to X11 apps when the Wayland selection offers it, and serves it through the bridge's existing transfer code (including its INCR path for large data), bytes untouched. Text handling is unchanged. |

Copying an image from a Linux app to Windows isn't covered (the X11 → Wayland
direction still passes text only).

## The Claude Code CLI

The Claude Code *CLI* (in a WSL terminal) doesn't read the clipboard through
X11 like VS Code does: it runs `wl-paste` (or `xclip`), and reports no image
if neither is installed. So for pasting into the CLI, also install
`wl-clipboard` in the distro:

```sh
sudo apt install wl-clipboard
```

With patch 0003 it then gets a real `image/png`; without it, it gets only the
BMP, which some Claude Code versions mishandle on WSLg.

## Check it's working

On top of [the modules being loaded](WSLG-PATCHES.md#check-theyre-loaded),
with a screenshot on the Windows clipboard:

- `wl-paste --list-types` lists `image/png` (as well as `image/bmp`);
- `xclip -selection clipboard -t TARGETS -o` lists `image/png`;
- pasting it into the Claude Code extension in Linux VS Code works.
