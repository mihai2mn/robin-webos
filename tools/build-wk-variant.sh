#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# A WebKit 2.54.3 build VARIANT (other compiler flags) beside the existing build, which stays untouched.
# The existing in-tree build (WK_SHARED, what tools/build-engine.sh made) is copied to its own directory, every source file
# is brought to "pristine 2.54.3 + patches/series" (the same method as package-release.sh's webkit32 phase), the CMake cache
# is re-configured with the variant's flags (everything else, the feature switches, stays as Buildroot configured it),
# and ninja rebuilds everything that the flags touch. Host only (WSL/Linux + Docker); no TV commands.
#
#   NAME=a55 CPU="-march=armv8-a+crc -mfpu=neon-fp-armv8 -mtune=cortex-a55" bash tools/build-wk-variant.sh all
#   phases: tree | configure | build | collect | all        (each needs the ones before)
#
#   NAME      variant name (directory $VARIANTS/NAME, output $VARIANTS/NAME-out)       required
#   CPU       extra CPU flags (default: none = the toolchain wrapper's Cortex-A9 defaults)
#   OPT       optimisation (default -O2)
#   EXTRA_CF  further compile flags (e.g. -ffunction-sections -fdata-sections)          EXTRA_LD  further link flags
#   JOBS      ninja jobs (default 22; WebKit wants about 2 GB RAM per job)
#   WK_SHARED the existing build tree, VARIANTS where variants live (default ~/webos-browser-build/variants)
set -euo pipefail
repo=$(cd "$(dirname "$0")/.." && pwd)
BR=${BUILD_ROOT:-$HOME/webos-browser-build}
NAME=${NAME:?NAME=variant name}
CPU=${CPU:-}; OPT=${OPT:--O2}; EXTRA_CF=${EXTRA_CF:-}; EXTRA_LD=${EXTRA_LD:-}; JOBS=${JOBS:-22}
VARIANTS=${VARIANTS:-$BR/variants}
WK_SHARED=${WK_SHARED:-$BR/br-out/build/wpewebkit-2.54.3}
T=$VARIANTS/$NAME; OUT=$VARIANTS/$NAME-out; PR=$VARIANTS/pristine-2.54.3
TARBALL=$BR/dl/wpewebkit/wpewebkit-2.54.3.tar.xz
PATCHES=$repo/buildroot-external/patches/wpewebkit/2.54.3
IMG=${IMAGE:-webos-browser-build:1}
INCONT=/build/br-out/build/wpewebkit-2.54.3     # the tree is mounted here, so the absolute paths inside it stay valid
BASE="-D_LARGEFILE_SOURCE -D_LARGEFILE64_SOURCE -D_FILE_OFFSET_BITS=64 -g0 -D_FORTIFY_SOURCE=1 -mthumb"   # Buildroot's, -mthumb is the JIT's
FLAGS="$BASE $OPT $CPU $EXTRA_CF"
LDF="$EXTRA_LD"
say() { echo "== $(date +%T) $*"; }
# dk [docker options] IMAGE command...: in the container; or, with ENTER=<path of enter.sh> (the laptop: bubblewrap on the exported
# image root, no root, no docker; see tools/laptop-builder.sh), the same command in that root with the same /build and tree mounts.
dk() {
    if [ -n "${ENTER:-}" ]; then
        while [ "$1" != "$IMG" ]; do shift; done; shift
        "$ENTER" --bind "$T:$INCONT" --bind "$OUT:/out" env CCACHE_DISABLE=1 "$@"
    else
        docker run --rm --user "$(id -u):$(id -g)" -e HOME=/build -e CCACHE_DISABLE=1 -v "$BR:/build" -v "$T:$INCONT" -v "$OUT:/out" "$@"
    fi
}

do_tree() {
    mkdir -p "$VARIANTS"
    say "pristine 2.54.3 + $(ls "$PATCHES"/*.patch | wc -l) patches -> $PR"
    rm -rf "$PR"; mkdir -p "$PR"; tar xf "$TARBALL" -C "$PR" --strip-components=1
    for p in "$PATCHES"/*.patch; do patch -p1 -s --no-backup-if-mismatch -d "$PR" < "$p" || { echo "patch $p failed" >&2; exit 1; }; done
    [ -d "$T" ] && { echo "$T exists (remove it to start over)" >&2; exit 1; }
    say "copying $WK_SHARED ($(du -sm "$WK_SHARED" | cut -f1) MB) -> $T"
    ( exec 9>"$BR/.wk254.lock"; flock 9; cp -a "$WK_SHARED" "$T" )
    ( cd "$PR" && diff -rq . "$T" 2>/dev/null | grep -E '^Files .* differ$' | sed -E 's/^Files \.\/(.*) and .* differ$/\1/' ) > "$VARIANTS/$NAME-differing.txt" || true
    echo "   $(wc -l < "$VARIANTS/$NAME-differing.txt") source files differ from pristine+series, brought to it"
    while read -r f; do cp "$PR/$f" "$T/$f"; done < "$VARIANTS/$NAME-differing.txt"
    rm -rf "$PR"
}
do_configure() {
    mkdir -p "$OUT"
    say "configure $NAME: $FLAGS | link: $LDF"
    dk "$IMG" bash -c "cd $INCONT && export PATH=/build/br-out/host/bin:\$PATH && cmake . -DCMAKE_C_FLAGS='$FLAGS' -DCMAKE_CXX_FLAGS='$FLAGS' \
        -DCMAKE_EXE_LINKER_FLAGS='$LDF' -DCMAKE_SHARED_LINKER_FLAGS='$LDF' -DCMAKE_MODULE_LINKER_FLAGS='$LDF'" > "$OUT/configure.log" 2>&1 || { tail -20 "$OUT/configure.log"; exit 1; }
    grep -m1 -o ' -O[0-3s] .*-mthumb[^ ]* [^ ]* [^ ]* [^ ]*' "$T/build.ninja" | sed 's/^/   first compile line: /' || true
}
do_build() {
    mkdir -p "$OUT"; say "ninja -j$JOBS in $T (log $OUT/ninja.log)"
    dk --cpu-shares 512 "$IMG" nice -n 5 bash -c "cd $INCONT && export PATH=/build/br-out/host/bin:\$PATH && ninja -j$JOBS" > "$OUT/ninja.log" 2>&1 \
        || { tail -30 "$OUT/ninja.log"; exit 1; }
    tail -1 "$OUT/ninja.log" | cut -c1-120
}
do_collect() {
    local X=$BR/x-tools/arm-webos-linux-gnueabi/bin/arm-webos-linux-gnueabi
    rm -rf "$OUT/engine"; mkdir -p "$OUT/engine/libexec" "$OUT/engine/lib/injected-bundle"
    install -m755 "$T/lib/libWPEWebKit-2.0.so.1.11.6" "$OUT/engine/lib/libWPEWebKit-2.0.so.1"
    for h in WPEWebProcess WPENetworkProcess WPEGPUProcess; do install -m755 "$T/bin/$h" "$OUT/engine/libexec/$h"; done
    install -m755 "$T/lib/libWPEInjectedBundle.so" "$OUT/engine/lib/injected-bundle/libWPEInjectedBundle.so"
    ( cd "$OUT/engine" && for f in lib/libWPEWebKit-2.0.so.1 libexec/* lib/injected-bundle/*; do "$X-strip" "$f"; done )
    say "output $OUT/engine (stripped, PT_INTERP not yet patched: tools/install-variant-dev.sh does that on the way to the TV)"
    ( cd "$OUT/engine"; for f in lib/libWPEWebKit-2.0.so.1 libexec/WPEWebProcess; do
        printf '   %-34s %10d B  %s\n' "$f" "$(stat -c %s "$f")" "$("$X-readelf" -A "$f" | grep -E 'Tag_CPU_arch:|Tag_ABI_VFP_args' | tr -s ' ' | tr '\n' ' ')"
      done
      "$X-readelf" -V lib/libWPEWebKit-2.0.so.1 | grep -oE 'GLIBC_2\.[0-9]+' | sort -t. -k2 -n -u | tail -1 | sed 's/^/   highest glibc symbol version: /' ) | tee "$OUT/collect.txt"
    echo "variant $NAME: CPU='$CPU' OPT='$OPT' EXTRA_CF='$EXTRA_CF' EXTRA_LD='$EXTRA_LD'" >> "$OUT/collect.txt"
}
case "${1:-all}" in
    tree) do_tree ;; configure) do_configure ;; build) do_build ;; collect) do_collect ;;
    all) do_tree; do_configure; do_build; do_collect ;;
    *) echo "usage: NAME=x [CPU=..] $0 tree|configure|build|collect|all" >&2; exit 2 ;;
esac
