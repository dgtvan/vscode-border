# Patched WSLg modules

Two things don't work for Linux VS Code under WSLg out of the box, both
because of gaps in WSLg's compositor (Weston), not in this app:

- **Resizing from the left/right edges** -- see [WSLG-RESIZE.md](WSLG-RESIZE.md).
  This one this app takes part in: it supplies the resize edges.
- **Pasting a screenshot copied on Windows** (e.g. into the Claude Code
  extension) -- see [WSLG-CLIPBOARD-IMAGES.md](WSLG-CLIPBOARD-IMAGES.md).
  Unrelated to this app; documented here because the fix is the same kind of
  patch.

Both are fixed by patching Weston's modules. This page covers what's shared:
how the patched modules are built, installed, checked, rebuilt after a WSL
update and removed. They're built and installed together -- one build
produces every patched module, and `rdp-backend.so` carries patches for both
fixes -- so installing gets you both.

Tested on WSL with WSLg 1.0.73.2 (Weston `04d436c7`), Windows 11.

## The patches

Kept in [`tools/wslg-patches/patches`](../tools/wslg-patches/patches):

| patch | module | for | from |
|---|---|---|---|
| `0001` | `rdprail-shell.so` | resize | [Mihir-Null/wslg-resize-fix](https://github.com/Mihir-Null/wslg-resize-fix) (MIT), reviewed |
| `0002` | `rdp-backend.so` | resize | same |
| `0003` | `rdp-backend.so` | clipboard images | this repo |
| `0004` | `xwayland.so` | clipboard images | this repo |

What each one does is explained on the page of the fix it belongs to.

## How they're loaded

A WSLg window is a `RAIL_WINDOW` owned by `msrdc.exe`; the Linux app's real
surface lives in Weston inside the WSLg *system distro*, a separate,
throwaway distro WSL runs next to yours. Weston loads its modules by name
(`--backend=rdp-backend.so --shell=rdprail-shell.so`, and `xwayland.so`), and
`WESTON_MODULE_MAP` -- libweston's own module-path override, which WSLg passes
through from `%USERPROFILE%\.wslgconfig` -- redirects those names.

They're redirected to **shims** (from the same upstream project): tiny
modules that read the running WSLg's Weston commit from
`/mnt/wslg/versions.txt` and load `weston-<commit>/<module>` next to them if
it exists, else the stock module. A module built for one WSLg loaded into
another would crash Weston, so after a WSL update the shims silently fall back
to stock WSLg until you rebuild. Nothing is ever written into the system
distro.

## Install

Everything below is per Windows user; no admin rights. `<distro>` is the WSL
distro running Linux VS Code.

1. **Check the WSLg version** -- the patches were reviewed against Weston
   `04d436c7`:

   ```powershell
   wsl.exe -d <distro> --system -e cat /mnt/wslg/versions.txt
   ```

2. **Get Weston's source at that commit**, as `weston-<commit>.tar.gz` in
   `tools/wslg-patches`. From a git checkout of
   [microsoft/weston-mirror](https://github.com/microsoft/weston-mirror) (or
   an empty repo):

   ```sh
   git fetch --depth 1 <weston-mirror remote> <commit>
   git archive --format=tar.gz -o <repo>/tools/wslg-patches/weston-<commit>.tar.gz FETCH_HEAD
   ```

   (Inside the WSLg system distro the CA bundle can't verify github.com, so the
   source is fetched on the Windows side. A tarball made with
   `core.autocrlf=true` has CRLF line endings; the build normalises the files
   it patches.)

3. **Build** inside the WSLg system distro, as root. It works in a throwaway
   build root in the system distro's `/tmp` (that distro's `/` is discarded on
   `wsl --shutdown`) and installs nothing:

   ```powershell
   wsl.exe --cd / -d <distro> --system -u root -e bash /mnt/<drive>/<path to repo>/tools/wslg-patches/build.sh
   ```

   It refuses an unreviewed Weston commit (rerun with `FORCE=1` after checking
   the patches still fit), and refuses to publish a module whose exports
   differ from stock or that imports a symbol the live system libraries don't
   provide. The result is in `tools/wslg-patches/out`.

4. **Shut WSL down** -- `wsl --shutdown`, which closes every running distro.
   Overwriting a module a running Weston has loaded can crash it.

5. **Copy the result** somewhere permanent, e.g.
   `%LOCALAPPDATA%\wslg-patches` -- the three shims plus the
   `weston-<commit>` folder.

6. **Point WSLg at the shims** in `%USERPROFILE%\.wslgconfig`:

   ```ini
   [system-distro-env]
   WESTON_MODULE_MAP=rdp-backend.so=/mnt/c/Users/<you>/AppData/Local/wslg-patches/rdp-backend.so;rdprail-shell.so=/mnt/c/Users/<you>/AppData/Local/wslg-patches/rdprail-shell.so;xwayland.so=/mnt/c/Users/<you>/AppData/Local/wslg-patches/xwayland.so
   ```

7. **Start Linux VS Code again.** WSLg starts with the patched modules.

## Check they're loaded

`/mnt/wslg/weston.log` (readable from inside the distro) has, for each of the
three modules, a `wslg-resize-fix: <module>: loaded patched …` line (the shims
keep the upstream project's name). Each fix's page has its own check on top of
that.

## After a WSL update

The shims find no `weston-<new commit>` build and load the stock modules:
WSLg keeps working, `weston.log` says
`no patched build for weston <commit> (rebuild wslg-resize-fix)`, and both
fixes are off (this app turns its resize strip off by itself). To get them
back, repeat steps 1-5 for the new commit -- checking the patches still apply
and still make sense for it -- and start VS Code again. Old `weston-*` folders
can stay.

## Undo

Delete the `WESTON_MODULE_MAP` line from `%USERPROFILE%\.wslgconfig` (or the
file, if that was all it held) and run `wsl --shutdown`. Optionally delete the
modules folder. If WSLg ever fails to start with the modules in place, this is
also the way out.
