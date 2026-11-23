#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# mimalloc 2.1.7 (MIT, Microsoft) as a shared library for the 32-bit engine: built with the same cross toolchain and CPU flags as
# WebKit, from a pinned release tarball. Output: $OUT/libmimalloc.so.2 (stripped) and $OUT/LICENSE. tools/package-release.sh puts
# both into the engine (engine/lib, engine/licenses/mimalloc-2.1.7) when they exist; the launch parameter
# {"wpebEnv":{"WPE_BROWSER_DEBUG_MALLOC":"mimalloc"}} preloads it (browser/engine_client.cpp).
# After webos-browser-lab's mimalloc/build_mimalloc.sh (MIT), which measured it on an LG C1.
#   BUILD_ROOT=~/webos-browser-build bash tools/build-mimalloc.sh
set -euo pipefail
VERSION=2.1.7
SHA256=0eed39319f139afde8515010ff59baf24de9e47ea316a315398e8027d198202d
BR=${BUILD_ROOT:-${WPE_BUILD_ROOT:-$HOME/webos-browser-build}}
DL=$BR/dl/mimalloc
OUT=${OUT:-$BR/mimalloc-out}
TC=$BR/x-tools/arm-webos-linux-gnueabi/bin/arm-webos-linux-gnueabi
tb=$DL/mimalloc-$VERSION.tar.gz
mkdir -p "$DL" "$OUT"
[ -f "$tb" ] || curl -fsSL -o "$tb" "https://github.com/microsoft/mimalloc/archive/refs/tags/v$VERSION.tar.gz"
echo "$SHA256  $tb" | sha256sum -c --quiet
# cmake and ninja: the build container's (as tools/package-release.sh), unless this already runs where they are.
if ! command -v cmake > /dev/null && [ -z "${IN_CONTAINER:-}" ]; then
    exec docker run --rm --user "$(id -u):$(id -g)" -e HOME=/build -e IN_CONTAINER=1 -e BUILD_ROOT=/build -e OUT=/build/mimalloc-out         -v "$BR:/build" -v "$(cd "$(dirname "$0")" && pwd):/tools:ro" -w /build webos-browser-build:1         bash -c 'export PATH=/build/br-out/host/bin:$PATH; bash /tools/build-mimalloc.sh'
fi
work=$(mktemp -d)
trap 'rm -r -- "$work"' EXIT
tar xzf "$tb" -C "$work"
# The engine's code generation (tools/package-release.sh webkit32): Cortex-A55, Thumb-2, softfp (the toolchain's default ABI).
flags="-mthumb -O2 -march=armv8-a+crc -mfpu=neon-fp-armv8 -mtune=cortex-a55"
cmake -S "$work/mimalloc-$VERSION" -B "$work/build" -G Ninja \
    -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=arm \
    -DCMAKE_C_COMPILER="$TC-gcc" -DCMAKE_CXX_COMPILER="$TC-g++" -DCMAKE_C_FLAGS="$flags" \
    -DCMAKE_BUILD_TYPE=Release -DMI_BUILD_TESTS=OFF -DMI_BUILD_STATIC=OFF -DMI_BUILD_OBJECT=OFF \
    -DMI_SECURE=OFF -DMI_OVERRIDE=ON -DMI_INSTALL_TOPLEVEL=ON > "$OUT/build.log" 2>&1 \
    || { tail -20 "$OUT/build.log"; exit 1; }
ninja -C "$work/build" >> "$OUT/build.log" 2>&1 || { tail -20 "$OUT/build.log"; exit 1; }
so=$(ls "$work"/build/libmimalloc.so.2.* | head -1)
"$TC-strip" -o "$OUT/libmimalloc.so.2" "$so"
cp "$work/mimalloc-$VERSION/LICENSE" "$OUT/LICENSE"
echo "$VERSION" > "$OUT/VERSION"
file "$OUT/libmimalloc.so.2"
"$TC-readelf" -d "$OUT/libmimalloc.so.2" | grep -E "NEEDED|SONAME"
