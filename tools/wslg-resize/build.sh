#!/bin/bash
# Builds patched rdprail-shell.so + rdp-backend.so for the WSLg running right
# now, plus the version-checking shims WSLg loads them through. See
# docs/WSLG-RESIZE.md for what this is for and how to install the result.
#
# Adapted from Mihir-Null/wslg-resize-fix (MIT, see LICENSE here),
# linux/build-shell.sh at 4bb7072: same throwaway build root, same WSLg meson
# flags, same symbol checks -- limited to its patches 0001 + 0002 (the X11-only
# 0003/0004 and the xwayland.so module aren't needed for VS Code).
#
# Runs INSIDE the WSLg system distro as root, from Windows:
#   wsl.exe --cd / -d <distro> --system -u root -e bash /mnt/c/<path to this folder>/build.sh
# Needs weston-<commit>.tar.gz next to this script (see the doc for how to
# make it). Nothing is installed into the system distro: its / is an overlay
# thrown away on `wsl --shutdown`, and the build root lives in its /tmp.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
BR=/tmp/wslg-buildroot
OUT="$HERE/out"
JOBS="$(nproc)"
# The weston commit the patches were reviewed and tested against (WSLg 1.0.73.2).
REVIEWED_COMMIT=04d436c7d9a0cd55fa64b6612c7fa678d6fcd077

WESTON_COMMIT="$(awk '/^weston:/{print $2}' /mnt/wslg/versions.txt)"
WSLG_VERSION="$(awk '/^WSLg/{print $NF}' /mnt/wslg/versions.txt)"
echo "== WSLg $WSLG_VERSION, weston $WESTON_COMMIT"
[ -n "$WESTON_COMMIT" ] || { echo "!! cannot read the weston commit from /mnt/wslg/versions.txt" >&2; exit 1; }
if [ "$WESTON_COMMIT" != "$REVIEWED_COMMIT" ] && [ "${FORCE:-0}" != 1 ]; then
  echo "!! this WSLg's weston ($WESTON_COMMIT) isn't the one the patches were reviewed against" >&2
  echo "   ($REVIEWED_COMMIT). Check the patches still make sense for it, then rerun with FORCE=1." >&2
  exit 1
fi
TARBALL="$HERE/weston-$WESTON_COMMIT.tar.gz"
[ -s "$TARBALL" ] || { echo "!! missing $TARBALL (see docs/WSLG-RESIZE.md)" >&2; exit 1; }

if [ ! -x "$BR/usr/bin/gcc" ]; then
  echo "== creating build root at $BR"
  mkdir -p "$BR/etc"
  cp -a /etc/yum.repos.d /etc/pki "$BR/etc/"
  tdnf --installroot="$BR" --releasever=3.0 -y -q install \
    bash coreutils sed gawk grep findutils patch tar gzip \
    gcc binutils glibc-devel kernel-headers meson ninja-build pkgconf \
    wayland-devel wayland-protocols-devel pixman-devel libxkbcommon-devel \
    libinput-devel libevdev-devel systemd-devel libdrm-devel mesa-libgbm-devel \
    cairo-devel pango-devel libpng-devel libjpeg-turbo-devel libwebp-devel \
    glib-devel librsvg2-devel libxml2-devel \
    libxcb-devel libXcursor-devel libX11-devel \
    libglvnd-devel dbus-devel openssl-devel
fi

# Compile against WSLg's own FreeRDP / rdpapplist / WSL-stub builds, i.e.
# exactly what the modules will run against.
echo "== syncing WSLg-built headers/libs into build root"
for d in freerdp2 winpr2 rdpapplist wsl; do
  [ -e "/usr/include/$d" ] && cp -a "/usr/include/$d" "$BR/usr/include/"
done
cp -a /usr/lib/libfreerdp* /usr/lib/libwinpr* "$BR/usr/lib/"
mkdir -p "$BR/usr/lib/pkgconfig"
for pc in freerdp2 freerdp-server2 winpr2 winpr-tools2; do
  [ -e "/usr/lib/pkgconfig/$pc.pc" ] && cp -a "/usr/lib/pkgconfig/$pc.pc" "$BR/usr/lib/pkgconfig/"
done

SRC="$BR/work/weston"
STAMP="$WESTON_COMMIT $(cat "$HERE"/patches/*.patch | sha256sum | cut -c1-16)"
if [ ! -f "$SRC/.patched" ] || [ "$(cat "$SRC/.patched")" != "$STAMP" ]; then
  rm -rf "$SRC"; mkdir -p "$SRC"
  echo "== unpacking weston $WESTON_COMMIT"
  tar -xzf "$TARBALL" -C "$SRC"
  # upstream rdprail sources use CRLF; normalise so the patches apply
  sed -i 's/\r$//' "$SRC/rdprail-shell/shell.c" "$SRC/libweston/backend-rdp/rdprail.c"
  for p in "$HERE"/patches/*.patch; do
    echo "   applying $(basename "$p")"
    sed 's/\r$//' "$p" | chroot "$BR" patch -d /work/weston -p1 --forward
  done
  echo "$STAMP" > "$SRC/.patched"
fi

for m in proc dev sys; do mountpoint -q "$BR/$m" || { mkdir -p "$BR/$m"; mount --bind "/$m" "$BR/$m"; }; done
trap 'for m in proc dev sys; do umount -l "$BR/$m" 2>/dev/null || true; done' EXIT

# WSLg's own meson flags (microsoft/wslg Dockerfile), so config.h matches.
chroot "$BR" /bin/bash -euo pipefail -c "
  cd /work/weston
  export C_INCLUDE_PATH=/usr/include/freerdp2:/usr/include/winpr2:/usr/include/wsl/stubs:/usr/include
  [ -f build/build.ninja ] || meson setup build --prefix=/usr --buildtype=debugoptimized \
    -Dbackend-default=rdp -Dbackend-drm=false -Dbackend-drm-screencast-vaapi=false \
    -Dbackend-headless=false -Dbackend-wayland=false -Dbackend-x11=false -Dbackend-fbdev=false \
    -Dcolor-management-colord=false -Dscreenshare=false -Dsystemd=false -Dwslgd=true \
    -Dremoting=false -Dpipewire=false -Dshell-fullscreen=false -Dcolor-management-lcms=false \
    -Dshell-ivi=false -Dshell-kiosk=false -Ddemo-clients=false -Dsimple-clients=[] -Dtools=[] \
    -Dresize-pool=false -Dwcap-decode=false -Dtest-junit-xml=false
  ninja -C build -j$JOBS rdprail-shell/rdprail-shell.so libweston/backend-rdp/rdp-backend.so
"

MODDIR="$OUT/weston-$WESTON_COMMIT"
rm -rf "$OUT"; mkdir -p "$MODDIR" "$BR/live"
publish() {  # <build-relative path> <live path>
  local rel="$1" live="$2" name lib d; name="$(basename "$rel")"
  chroot "$BR" strip --strip-debug "/work/weston/build/$rel" -o "/work/$name"
  cp "$BR/work/$name" "$MODDIR/$name"
  cp "$MODDIR/$name" "$BR/live/new.so"; cp "$live" "$BR/live/orig.so"
  # Exports must match the stock module exactly; any new import must come
  # from a library the stock module already links, as shipped in this WSLg.
  rm -rf "$BR/live/needed"; mkdir -p "$BR/live/needed"
  for lib in $(chroot "$BR" readelf -d /live/orig.so | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p'); do
    for d in "$(dirname "$live")" /usr/lib /usr/lib64 /lib64; do
      [ -e "$d/$lib" ] && { cp -L "$d/$lib" "$BR/live/needed/"; break; }
    done
  done
  if chroot "$BR" bash -euo pipefail -s "$name" <<'EOF'
cd /live
for f in new orig; do
  nm -D --undefined-only $f.so | awk '{print $2}' | sort -u > $f.und
  nm -D --defined-only   $f.so | awk '{print $3}' | sort -u > $f.def
done
if [ "$(comm -3 orig.def new.def)" ]; then
  echo "!! $1: exported symbols differ from stock:"; comm -3 orig.def new.def; exit 1
fi
extra="$(comm -13 orig.und new.und)"
nm -D --defined-only needed/* 2>/dev/null | awk 'NF==3 {print $3}' | sed 's/@@/@/' | sort -u > provided
missing=""
for s in $extra; do grep -qxF "$s" provided || missing="$missing $s"; done
if [ -n "$missing" ]; then echo "!! $1: new imports not provided by live libs:$missing"; exit 1; fi
[ -z "$extra" ] || echo "   $1: new imports, all provided by live libs: $(tr '\n' ' ' <<<"$extra")"
EOF
  then echo "== $name: exports identical to stock, imports OK"
  else echo "!! $name: symbol check failed -- do NOT use" >&2; exit 1; fi
}
publish rdprail-shell/rdprail-shell.so /usr/lib/weston/rdprail-shell.so
publish libweston/backend-rdp/rdp-backend.so /usr/lib/libweston-9/rdp-backend.so
echo "$WESTON_COMMIT" > "$MODDIR/weston-commit"

# Shims: tiny, libweston-free; export only the module entry point and import
# only from the live libc.
mkdir -p "$BR/work/shim"; sed 's/\r$//' "$HERE/shim.c" > "$BR/work/shim/shim.c"
chroot "$BR" bash -euo pipefail <<'EOF'
cd /work/shim
nm -D --defined-only /live/needed/libc.so.6 | awk 'NF==3 {print $3}' | sed 's/@@/@/' | sort -u > libc.provided
for m in BACKEND:rdp-backend.so:weston_backend_init SHELL:rdprail-shell.so:wet_shell_init; do
  IFS=: read -r def out entry <<<"$m"
  gcc -shared -fPIC -O2 -Wall -Wextra -Werror -fvisibility=hidden -D_FORTIFY_SOURCE=2 \
      -DSHIM_$def -DSHIM_MODULE="\"$out\"" -o "$out" shim.c
  strip --strip-unneeded "$out"
  exp="$(nm -D --defined-only "$out" | awk '{print $3}' | tr '\n' ' ')"
  [ "$exp" = "$entry " ] || { echo "!! shim $out exports '$exp', expected '$entry'"; exit 1; }
  for s in $(nm -D --undefined-only "$out" | awk '$1=="U" {print $2}'); do
    grep -qxF "$s" libc.provided || { echo "!! shim $out imports $s, not in live libc"; exit 1; }
  done
  echo "== shim $out: exports $entry, imports only live libc"
done
EOF
cp "$BR/work/shim/rdp-backend.so" "$BR/work/shim/rdprail-shell.so" "$OUT/"
(cd "$OUT" && sha256sum *.so weston-*/*.so)
echo "== done: $OUT"
