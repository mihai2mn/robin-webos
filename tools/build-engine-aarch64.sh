#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# The 64-bit (aarch64) engine for the TV's org.webosbrew.bridge-64to32, built in the same container image as the
# 32-bit one (tools/build-engine.sh) but in its own output tree, container, log and status:
#   bash tools/build-engine-aarch64.sh start|status|stop       (JOBS=8 TARGETS="..." WEBKIT_VERSION=2.54.3)
# TARGETS defaults to everything; TARGETS=wpewebkit-depends builds the toolchain and every library WebKit needs.
# Shares $WPE_BUILD_ROOT/buildroot (checked out at the same commit; WebKit recipe swapped to WEBKIT_VERSION), dl/
# and ccache/ with the 32-bit build: do not run both containers' Buildroot steps at the same moment.
set -euo pipefail
project=$(cd "$(dirname "$0")/.." && pwd)
build_root=${WPE_BUILD_ROOT:-$HOME/webos-browser-build}
container=webos-wpe-engine-build-aarch64
buildroot_commit=${BUILDROOT_COMMIT:-cdd3776b}
jobs=${JOBS:-8}
webkit_version=${WEBKIT_VERSION:-2.54.3}
case "${1:-status}" in
  start)
    if docker ps --format '{{.Names}}' | grep -qx "$container"; then echo "already running" >&2; exit 1; fi
    printf 'starting\n' > "$build_root/engine-aarch64.status"
    docker run --detach --rm --name "$container" --user "$(id -u):$(id -g)" -e HOME=/build -e JOBS="$jobs" \
      -e BUILDROOT_COMMIT="$buildroot_commit" -e WEBKIT_VERSION="$webkit_version" -e TARGETS="${TARGETS:-}" \
      --cpu-shares 256 -v "$project:/src" -v "$build_root:/build" -w /build webos-browser-build:1 \
      nice -n 10 bash /src/tools/build-engine-aarch64.sh inside ;;
  inside)
    exec >> /build/engine-aarch64.log 2>&1
    trap 'r=$?; printf "exit=%d (%s)\n" "$r" "$(cat /build/engine-aarch64.stage 2>/dev/null)" > /build/engine-aarch64.status' EXIT
    stage() { printf '%s\n' "$1" | tee /build/engine-aarch64.stage > /build/engine-aarch64.status; echo "== $(date '+%F %T') $1"; }
    stage "start, $JOBS jobs, WebKit $WEBKIT_VERSION, targets: ${TARGETS:-all}"
    git -C /build/buildroot checkout -q -- package/wpewebkit
    git -C /build/buildroot checkout -q "$BUILDROOT_COMMIT"
    if [ "$WEBKIT_VERSION" = 2.54.3 ]; then
      p=/build/buildroot/package/wpewebkit
      sed -i 's/^WPEWEBKIT_VERSION = .*/WPEWEBKIT_VERSION = 2.54.3/' $p/wpewebkit.mk
      sed -i -e '/wpewebkit-2\.50\.5/d' $p/wpewebkit.hash
      echo 'sha256  787afd32ae23eac061621465390f44a462e874a9ebaba0b65521d7237fa8a5e4  wpewebkit-2.54.3.tar.xz' >> $p/wpewebkit.hash
      rm -f $p/0001-CMake-4.4-*.patch
    fi
    mk() { make -C /build/buildroot O=/build/br-out-aarch64 BR2_EXTERNAL=/src/buildroot-external BR2_JLEVEL="$JOBS" "$@"; }
    stage "configure"
    mk webos_browser_aarch64_defconfig
    stage "build ${TARGETS:-all}"
    # linux/dma-buf.h appeared in Linux 4.6; WebKit (MemoryMappedGPUBuffer.cpp) includes it and the sysroot has 4.4 headers.
    if [ ! -f /build/br-out-aarch64/staging/usr/include/linux/dma-buf.h ]; then
      mk toolchain >/dev/null && inc=$(ls -d /build/br-out-aarch64/host/aarch64-buildroot-linux-gnu/sysroot/usr/include/linux) && cp /src/toolchain/extra-headers/linux/dma-buf.h "$inc/"
    fi
    # shellcheck disable=SC2086
    mk $TARGETS
    stage "PASS" ;;
  status)
    cat "$build_root/engine-aarch64.status" 2>/dev/null || echo "not started"
    tr '\r' '\n' < "$build_root/engine-aarch64.log" 2>/dev/null | grep -a -E '^>>>|^== |PASS|rror' | tail -n 4 | cut -c1-200 || true ;;
  stop) docker stop "$container" ;;
  *) echo 'usage: bash tools/build-engine-aarch64.sh start|status|stop' >&2; exit 2 ;;
esac
