#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# S3: does code built with the Cortex-A55 flags run on the TV? Builds probe.c with the project's 32-bit toolchain in
# several flag sets (host only), then (with --tv) copies the binaries to /tmp on the TV and runs each there.
# No illegal instruction = the flags are safe for this TV. Needs the TV lock for --tv (the caller holds it).
#   bash tools/a55-probe/run.sh [--tv]
set -u
here=$(cd "$(dirname "$0")" && pwd)
BR=$HOME/webos-browser-build; CC=$BR/x-tools/arm-webos-linux-gnueabi/bin/arm-webos-linux-gnueabi-gcc
OBJDUMP=$BR/x-tools/arm-webos-linux-gnueabi/bin/arm-webos-linux-gnueabi-objdump
out=${OUT:-$here/out}; mkdir -p "$out"
rm -f "$out"/probe-*   # a probe of an earlier build must never be run and reported as this one's
failed=0
declare -A F=(
  [a9]="-march=armv7-a -mfpu=neon -mfloat-abi=softfp"
  [v8crc]="-march=armv8-a+crc -mfpu=neon-fp-armv8 -mfloat-abi=softfp -mtune=cortex-a55"
  [v8crc-thumb]="-march=armv8-a+crc -mfpu=neon-fp-armv8 -mfloat-abi=softfp -mtune=cortex-a55 -mthumb"
  [a55]="-mcpu=cortex-a55 -mfpu=neon-fp-armv8 -mfloat-abi=softfp -mthumb"
  [a55-extra]="-mcpu=cortex-a55 -mfpu=neon-fp-armv8 -mfloat-abi=softfp -mthumb -march=armv8.2-a+fp16+dotprod -DPROBE_EXTRA"
)
for k in "${!F[@]}"; do
  $CC -O2 ${F[$k]} -o "$out/probe-$k" "$here/probe.c" -lm 2>"$out/build-$k.log" || { echo "build $k FAILED: $(head -3 "$out/build-$k.log")"; rm -f "$out/probe-$k"; failed=1; continue; }
  n=$($OBJDUMP -d "$out/probe-$k" | grep -cE '\s(sdiv|udiv|vfma|vfms|vrint[a-z]*|crc32[a-z]*|vsdot|vudot)')
  echo "built $k: armv8-class instructions in the binary: $n; $(${OBJDUMP%objdump}readelf -A "$out/probe-$k" 2>/dev/null | grep -E 'Tag_CPU_arch:' | tr -s ' ')"
done
[ "$failed" = 0 ] || { echo "a build failed: nothing is run on the TV" >&2; exit 1; }
[ "${1:-}" = --tv ] || exit 0
timeout 60 scp -q -o BatchMode=yes "$out"/probe-* lgtv:/tmp/ || exit 1
for k in "${!F[@]}"; do
  [ -f "$out/probe-$k" ] || continue
  r=$(timeout 30 ssh -o BatchMode=yes lgtv "chmod +x /tmp/probe-$k; /tmp/probe-$k 2>&1; echo exit=\$?" | tr '\n' ' ')
  echo "TV $k: $r"
done
timeout 30 ssh -o BatchMode=yes lgtv "rm -f /tmp/probe-*; grep -m1 Features /proc/cpuinfo; grep -m1 'CPU part' /proc/cpuinfo"
