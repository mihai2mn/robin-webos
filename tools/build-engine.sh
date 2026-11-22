#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# The whole softfp engine from scratch on one machine, unattended: crosstool-ng toolchain, Buildroot (at the commit
# the development notes (not in the repository) records), every package with WebKit's JIT (WPEB_SOFTFP_JIT, patches/wpewebkit/*), then the staged engine.
#   bash tools/build-engine.sh start      detached container; JOBS=24 by default (WebKit wants ~2 GB of RAM per job)
#   bash tools/build-engine.sh status     stage and the last lines of the log
#   bash tools/build-engine.sh stop
# Needs the image (docker build -t webos-browser-build:1 docker/). Output: build/engine-stage-jit/engine; the build tree
# is in $WPE_BUILD_ROOT (default ~/webos-browser-build): x-tools/, buildroot/, br-out/, dl/, ccache/.
set -euo pipefail
project=$(cd "$(dirname "$0")/.." && pwd)
build_root=${WPE_BUILD_ROOT:-$HOME/webos-browser-build}
container=webos-wpe-engine-build
tuple=arm-webos-linux-gnueabi
buildroot_commit=${BUILDROOT_COMMIT:-cdd3776b}
jobs=${JOBS:-24}
# WebKit release: 2.50.5 is Buildroot's own; 2.54.3 swaps the recipe's version and hash (patches: patches/wpewebkit/2.54.3/).
webkit_version=${WEBKIT_VERSION:-2.50.5}
case "${1:-status}" in
  start)
    if docker ps --format '{{.Names}}' | grep -qx "$container"; then echo "already running" >&2; exit 1; fi
    mkdir -p "$build_root"
    printf 'starting\n' > "$build_root/engine.status"
    docker run --detach --rm --name "$container" --user "$(id -u):$(id -g)" -e HOME=/build -e JOBS="$jobs" \
      -e BUILDROOT_COMMIT="$buildroot_commit" -e WEBKIT_VERSION="$webkit_version" --cpu-shares 256 \
      -v "$project:/src" -v "$build_root:/build" \
      -v /usr/bin/qemu-arm-static:/usr/local/bin/qemu-arm-static:ro \
      -w /build webos-browser-build:1 nice -n 10 bash /src/tools/build-engine.sh inside ;;
  inside)
    exec >> /build/engine.log 2>&1
    trap 'result=$?; printf "exit=%d (%s)\n" "$result" "$(cat /build/engine.stage 2>/dev/null)" > /build/engine.status' EXIT
    stage() { printf '%s\n' "$1" > /build/engine.stage; printf '%s\n' "$1" > /build/engine.status; echo "== $(date '+%F %T') $1"; }
    stage "start, $JOBS jobs"
    if [ ! -x "/build/x-tools/$tuple/bin/$tuple-gcc" ]; then
      stage toolchain
      mkdir -p /build/ctng /build/tarballs
      cp /src/toolchain/ct-ng.defconfig /build/ctng/defconfig
      (cd /build/ctng && ct-ng defconfig && ct-ng "build.$JOBS")
    fi
    # linux/dma-buf.h appeared in Linux 4.6; WebKit's GBM code includes it (the development notes (not in the repository) 2026-11-10). The sysroot is read-only.
    inc="/build/x-tools/$tuple/$tuple/sysroot/usr/include/linux"
    if [ ! -f "$inc/dma-buf.h" ]; then
      chmod u+w "$inc"; cp /src/toolchain/extra-headers/linux/dma-buf.h "$inc/"; chmod u-w "$inc"
    fi
    if [ ! -d /build/buildroot/.git ]; then
      stage "buildroot source"
      git clone -q https://gitlab.com/buildroot.org/buildroot.git /build/buildroot
    fi
    git -C /build/buildroot checkout -q -- package/wpewebkit
    git -C /build/buildroot checkout -q "$BUILDROOT_COMMIT"
    if [ "$WEBKIT_VERSION" = 2.54.3 ]; then
      # 2.54.3 already has Buildroot's CMake 4.4 fix (its 0001 patch); our 2.54.3 patches are version-specific.
      p=/build/buildroot/package/wpewebkit
      sed -i 's/^WPEWEBKIT_VERSION = .*/WPEWEBKIT_VERSION = 2.54.3/' $p/wpewebkit.mk
      sed -i -e '/wpewebkit-2.50.5/d' $p/wpewebkit.hash
      echo 'sha256  787afd32ae23eac061621465390f44a462e874a9ebaba0b65521d7237fa8a5e4  wpewebkit-2.54.3.tar.xz' >> $p/wpewebkit.hash
      rm -f $p/0001-CMake-4.4-*.patch
    fi
    echo "WebKit $(sed -n 's/^WPEWEBKIT_VERSION = //p' /build/buildroot/package/wpewebkit/wpewebkit.mk)"
    echo "buildroot at $(git -C /build/buildroot log -1 --format='%h %cd')"
    mk() { make -C /build/buildroot O=/build/br-out BR2_EXTERNAL=/src/buildroot-external WPEB_SOFTFP_JIT=y BR2_JLEVEL="$JOBS" "$@"; }
    stage "packages (toolchain wrapper, host tools, libraries, LLVM, Mesa, GStreamer, WebKit)"
    mk webos_browser_defconfig
    mk
    # Buildroot's own sysroot is a copy of the toolchain's: make sure the header is there too.
    br_inc=$(ls -d /build/br-out/host/*-buildroot-linux-gnueabi/sysroot/usr/include/linux 2>/dev/null | head -n 1)
    [ -n "$br_inc" ] && [ ! -f "$br_inc/dma-buf.h" ] && cp /src/toolchain/extra-headers/linux/dma-buf.h "$br_inc/"
    wk=/build/br-out/build/wpewebkit-$WEBKIT_VERSION/
    grep -E '^ENABLE_JIT:|^ENABLE_DFG_JIT:|^ENABLE_C_LOOP:|^ENABLE_WEBASSEMBLY:' "$wk/CMakeCache.txt" || true
    stage "licences"
    mk legal-info >/dev/null
    stage "engine"
    BR=/build/br-out OUT=/src/build/engine-stage-jit/engine ENGINE_BUILD=/build/engine-build sh /src/tools/stage-engine.sh
    stage "PASS"
    ;;
  status)
    cat "$build_root/engine.status" 2>/dev/null || echo "not started"
    tr '\r' '\n' < "$build_root/engine.log" 2>/dev/null | grep -a -E '^>>>|^\[[0-9]+/[0-9]+\]|^== |PASS|rror' | tail -n 4 | cut -c1-200 || true ;;
  stop)
    docker stop "$container" ;;
  *) echo 'usage: bash tools/build-engine.sh start|status|stop' >&2; exit 2 ;;
esac
