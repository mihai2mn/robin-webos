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
src=$project/engine/launcher/wpe-launcher.c
out=$project/build/launcher

[ -x "$cc" ] || { echo "missing the 32-bit cross compiler: $cc" >&2; echo "set WPE_BUILD_ROOT or build the toolchain (toolchain/)" >&2; exit 1; }
mkdir -p "$out"

# -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp: identical to the UI and the 32-bit engine (the TV's ABI).
# -Os: it is called once per engine start; size and clarity matter, speed does not.
"$cc" -static -Os -Wall -Wextra -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp -marm \
    -o "$out/wpe-engine" "$src"

"$readelf" -h "$out/wpe-engine" | grep -E 'Class|Machine|Type' | sed 's/^/  /'
echo "  $(du -h "$out/wpe-engine" | cut -f1)  $out/wpe-engine"
# It must be static: the UI execs it before the app's private loader link exists (browser/private_loader.cpp).
if "$readelf" -lW "$out/wpe-engine" | grep -q 'Requesting program interpreter'; then
    echo "error: the launcher is dynamically linked; it must be static" >&2
    exit 1
fi
echo "  static: no PT_INTERP (runs on the TV's own glibc, or none at all)"
