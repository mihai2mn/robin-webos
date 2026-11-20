#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds the 32-bit engine launcher (engine/launcher/wpe-launcher.c) with the project's own softfp cross toolchain,
# the same tuple as the 32-bit engine (glibc 2.28, ARMv7-A, softfp). Static on purpose: it is installed as
# <app>/engine/wpe-engine, i.e. it is the FIRST thing the UI execs, before any private loader link exists, so it must
# run on the TV's own glibc (or on webOS 4's older one) with nothing alongside it.
#
# Output: build/launcher/wpe-engine  (install it as <app>/engine/wpe-engine next to the renamed engine/wpe-engine32).
# Host only: no device commands.
set -euo pipefail
project=$(cd "$(dirname "$0")/.." && pwd)
build=${WPE_BUILD_ROOT:-$HOME/webos-browser-build}
cc=$build/x-tools/arm-webos-linux-gnueabi/bin/arm-webos-linux-gnueabi-gcc
readelf=$build/x-tools/arm-webos-linux-gnueabi/bin/arm-webos-linux-gnueabi-readelf
src=${LAUNCHER_SRC:-$project/engine/launcher/wpe-launcher.c}
out=${OUT_DIR:-$project/build/launcher}

[ -x "$cc" ] || { echo "missing the 32-bit cross compiler: $cc" >&2; echo "set WPE_BUILD_ROOT or build the toolchain (toolchain/)" >&2; exit 1; }
mkdir -p "$out"

# -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp: identical to the UI and the 32-bit engine (the TV's ABI).
# -Os: it is called once per engine start; size and clarity matter, speed does not.
static_flag=(); [ "${LAUNCHER_STATIC:-0}" = 1 ] && static_flag=(-static)
# LAUNCHER_INTERP: PT_INTERP of the dynamic launcher. The UI links its loader for the engine it spawns at that path
# (browser/private_loader.cpp: /tmp/<app id>/ld-linux.so.3), and the launcher execs the 32-bit engine behind it, which
# needs the same link; default /lib/ld-linux.so.3 (the TV loader) only works when something else made the link.
[ -n "${LAUNCHER_INTERP:-}" ] && static_flag+=("-Wl,--dynamic-linker=$LAUNCHER_INTERP")
"$cc" "${static_flag[@]}" -Os -Wall -Wextra -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp -marm \
    -o "$out/wpe-engine" "$src"

"$readelf" -h "$out/wpe-engine" | grep -E 'Class|Machine|Type' | sed 's/^/  /'
echo "  $(du -h "$out/wpe-engine" | cut -f1)  $out/wpe-engine"
# Dynamic by default (PT_INTERP /lib/ld-linux.so.3, the TV's own loader, needs only its libc): the UI reads the engine's
# PT_INTERP (browser/private_loader.cpp) and refuses to start an engine that has none ("cannot read the dynamic
# loader of .../engine/wpe-engine"), so a static launcher cannot sit in front of the engine. LAUNCHER_STATIC=1 builds the
# old static one (only useful with fake-ui, which does not look).
if "$readelf" -lW "$out/wpe-engine" | grep -q 'Requesting program interpreter'; then
    echo "  dynamic: $("$readelf" -lW "$out/wpe-engine" | sed -n 's/.*interpreter: \(.*\)\]/\1/p')"
else
    echo "  static: no PT_INTERP (the UI will refuse it)"
fi
