#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# The release package: ONE ipk with the UI, the 32-bit WebKit 2.54.3 engine (JIT+DFG), the aarch64 engine behind the
# engine launcher, and the uBO data. Two variants from the same tree: the dev id (org.sostk.wpebrowser.dev, for
# testing beside the real app) and the real id (org.sostk.wpebrowser). Builds from a CLEAN CLONE of a commit, so
# uncommitted work in the working tree never ends up in the package. Host only (WSL/Linux + Docker); no TV commands.
#
#   bash tools/package-release.sh all                 every phase, both ids -> build/release/*.ipk, MD5SUMS, BUILD-INFO.txt
#   bash tools/package-release.sh clone|launcher|engine32|stage|ui|add64|sums|check|publish   one phase (each needs the ones before)
#   IDS="dev"  COMMIT=<sha>  BUILD_ROOT=~/webos-browser-build (alias WPE_BUILD_ROOT)  OUT=...  bash tools/package-release.sh all
# The packages are assembled in $OUT.tmp (add64 empties it first) and moved into $OUT by `publish` only when every id was built and
# `check` passed; `publish` first removes the old *_arm.ipk, MD5SUMS and BUILD-INFO.txt from $OUT (other files there are left), so
# $OUT never holds a mixture of two runs. MD5SUMS and BUILD-INFO.txt describe exactly the ipks of $IDS (default: both ids).
# `check` unpacks every ipk of $IDS and fails on: a missing ipk or an extra one, a wrong package id/version, an app directory
# that is not the id, missing engine/ or engine64/ entries, a 32-bit file that is not ARM or a 64-bit one that is not AArch64, a wrong
# PT_INTERP, a DT_NEEDED library that is neither in the package nor in TV_LIBS, a dangling link, a BUILD-INFO commit that is not COMMIT.
#
# Inputs that are NOT built here (each has its own script; the package records which it used, build/release/BUILD-INFO.txt):
#   STAGE_BASE  tools/stage-engine.sh output (libs, glibc, GStreamer, fonts, certs, licences): build/engine-stage-jit/engine
#   WK254       the in-tree WebKit 2.54.3 build with patches/wpewebkit/2.54.3/0001..0026, every file of the directory (tools/build-engine.sh with
#               WEBKIT_VERSION=2.54.3, then ninja; wk254.sh): libWPEWebKit, WPE{Web,Network,GPU}Process, injected bundle.
#               The stripped libWPEWebKit has md5 a281cdbc88005b5338e401dc226f7613 (= the TV's engine254jit); NOT rebuilt here
#               (a WebKit build takes hours; `tools/build-engine.sh start` with WEBKIT_VERSION=2.54.3 rebuilds it from the patch series).
#   STAGE64     tools/engine-aarch64.sh stage output: build/engine-stage-aarch64/engine (aarch64 engine, glibc, Mesa)
#   uBO         built here from the clone (phase `ubo`): engine/ubo data (tools/ubo/update.py downloads the filter lists and uBO resources,
#               needs network; tools/ubo/build.sh data compiles them with cargo + node) and libwren-ubo.so for armv7 (x-tools gcc) and aarch64 (Buildroot gcc).
# Rebuilt here from the clean clone: wpe-engine + libgstwpebmedia.so (against the 2.54.3 headers), the launcher, the UI.
set -euo pipefail
repo=$(cd "$(dirname "$0")/.." && pwd)
# One build root for every tool this script runs: BUILD_ROOT (this script) and WPE_BUILD_ROOT (build-launcher.sh, build-engine.sh, ...) name the
# same directory; setting both to different values is refused, and the child scripts get the resolved root explicitly (BR_OUT is passed
# as $BR/br-out to build-webos-backend.sh whatever the environment holds: engine-aarch64.sh documents BR_OUT as the aarch64 tree).
if [ -n "${BUILD_ROOT:-}" ] && [ -n "${WPE_BUILD_ROOT:-}" ] && [ "$BUILD_ROOT" != "$WPE_BUILD_ROOT" ]; then
    echo "BUILD_ROOT=$BUILD_ROOT and WPE_BUILD_ROOT=$WPE_BUILD_ROOT differ: set one of them" >&2; exit 2
fi
BR=${BUILD_ROOT:-${WPE_BUILD_ROOT:-$HOME/webos-browser-build}}
COMMIT=$(git -C "$repo" rev-parse --verify "${COMMIT:-HEAD}^{commit}")   # always the full id: BUILD-INFO and `check` compare it
OUT=${OUT:-$repo/build/release}
STAGE_OUT=$OUT.tmp   # the packages are assembled here and move into $OUT only when the whole set was built and checked
WORK=${WORK:-$BR/release-work}
IDS=${IDS:-"dev release"}
VERSION=${VERSION:-0.3.3}
STAGE_BASE=${STAGE_BASE:-$HOME/wren/build/engine-stage-jit/engine}
WK_SHARED=${WK_SHARED:-$BR/br-out/build/wpewebkit-2.54.3}
WK_TREE=${WK_TREE:-$WORK/wk-tree}
WK254=$WK_TREE   # what stage takes the engine libraries from
STAGE64=${STAGE64:-$repo/build/engine-stage-aarch64/engine}
NDK=${WEBOS_NDK:-$HOME/webos-ndk/arm-webos-linux-gnueabi_sdk-buildroot}
STRIP32=$BR/x-tools/arm-webos-linux-gnueabi/bin/arm-webos-linux-gnueabi-strip
STRIP64=$BR/br-out-aarch64/host/bin/aarch64-buildroot-linux-gnu-strip
PATCHELF=${PATCHELF:-$BR/br-out-aarch64/host/bin/patchelf}
src=$WORK/src
export TMPDIR=${PKG_TMP:-$WORK/tmp}; mkdir -p "$TMPDIR"
appid() { case "$1" in dev) echo org.sostk.wpebrowser.dev ;; release) echo org.sostk.wpebrowser ;; *) echo "bad id $1" >&2; exit 2 ;; esac; }
title_of() { case "$1" in dev) echo "Wren Test" ;; *) echo "Wren" ;; esac; }
mb() { awk -v x="$1" 'BEGIN{printf "%.1f", x/1048576}'; }
say() { echo "== $(date +%T) $*"; }
phase=${1:-all}

# ---- clone: a clean checkout of COMMIT, with the kit submodule and its local patches --------------------------
do_clone() {
    say "clean clone of $COMMIT -> $src"
    rm -rf "$src"; mkdir -p "$WORK"
    git clone -q --no-checkout "$repo" "$src"
    git -C "$src" checkout -q "$COMMIT"
    local kitsha; kitsha=$(git -C "$src" ls-tree HEAD third_party/webos-native-app-kit | awk '{print $3}')
    git -C "$src" config submodule.third_party/webos-native-app-kit.url "${KIT_REPO:-$repo/third_party/webos-native-app-kit}"   # KIT_REPO: a git clone of the kit when the working tree has only a copy
    git -C "$src" -c protocol.file.allow=always submodule update --init -q
    git -C "$src/third_party/webos-native-app-kit" checkout -q "$kitsha"
    ( cd "$src" && sh third_party/patches/apply-kit-patches.sh ) > "$WORK/kit-patches.log" 2>&1 || { tail "$WORK/kit-patches.log"; exit 1; }
    find "$src/tools" -name "*.sh" -exec sed -i "s/$//" {} +   # a few scripts are stored with CRLF
    git -C "$src" log -1 --format='%H %s'
    # the kit with its patches must equal the working tree's (which the earlier builds used), file by file
    diff -rq --strip-trailing-cr --exclude=.git "$src/third_party/webos-native-app-kit" "$repo/third_party/webos-native-app-kit" \
        | head -5 | sed 's/^/   kit differs: /' || true
}

# ---- overlay: uncommitted packaging/launcher files of the working tree (empty once they are committed) -------------
# Opt-in (testing only): copy working-tree paths over the clean clone. A release builds the commit as is.
OVERLAY=${OVERLAY-}
do_overlay() {
    for p in $OVERLAY; do say "overlay from the working tree: $p"; mkdir -p "$src/$(dirname "$p")"; cp -a "$repo/$p" "$src/$(dirname "$p")/"; done
    # some scripts are stored with CRLF in git (tools/build-webos-backend.sh): bash chokes on pipefail followed by CR
    find "$src/tools" -name '*.sh' -exec sed -i 's/$//' {} +
}

# ---- launcher: dynamic, PT_INTERP = the app's private loader link (the UI links it for the launcher) ---------
do_launcher() {
    for id in $IDS; do
        say "launcher for $id"
        mkdir -p "$WORK/launcher-$id"
        WPE_BUILD_ROOT="$BR" LAUNCHER_SRC="$src/engine/launcher/wpe-launcher.c" LAUNCHER_INTERP="/tmp/$(appid $id)/ld-linux.so.3" OUT_DIR="$WORK/launcher-$id" bash "$src/tools/build-launcher.sh" | sed "s/^/   /"
    done
}

# ---- engine32: wpe-engine + libgstwpebmedia.so from the clone, against the 2.54.3 headers ------------------------
do_engine32() {
    say "32-bit wpe-engine against WebKit 2.54.3 headers"
    local inc=$BR/inc254
    if [ ! -d "$inc/wpe-webkit-2.0" ]; then
        mkdir -p "$inc"; cp -a "$BR/br-out-aarch64/target/usr/include/wpe-webkit-2.0" "$inc/"
    fi
    # Giving -DCMAKE_C(XX)_FLAGS REPLACES Buildroot's toolchain default ("-O2 -g0 -D_FORTIFY_SOURCE=1 ..."), and the Release
    # type adds only -DNDEBUG: without these the wrapper and the video sink were built at -O0. ENGINE_OPT is that default.
    local opt=${ENGINE_OPT:--O2 -g0 -D_LARGEFILE_SOURCE -D_LARGEFILE64_SOURCE -D_FILE_OFFSET_BITS=64 -D_FORTIFY_SOURCE=1 -ffunction-sections -fdata-sections}
    docker run --rm --user "$(id -u):$(id -g)" -e HOME=/build -e ENGINE_OPT="$opt" -v "$src:/src" -v "$BR:/build" -w /build webos-browser-build:1 sh -c '
        set -e
        cmake -S /src/engine -B /build/'"${WORK#$BR/}"'/engine-build -G Ninja \
            -DCMAKE_TOOLCHAIN_FILE=/build/br-out/host/share/buildroot/toolchainfile.cmake -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_CXX_FLAGS="$ENGINE_OPT -I/build/inc254/wpe-webkit-2.0 -I/build/inc254/wpe-webkit-2.0/wpe-platform" \
            -DCMAKE_C_FLAGS="$ENGINE_OPT -I/build/inc254/wpe-webkit-2.0 -I/build/inc254/wpe-webkit-2.0/wpe-platform" \
            -DCMAKE_EXE_LINKER_FLAGS="-Wl,--gc-sections" -DCMAKE_SHARED_LINKER_FLAGS="-Wl,--gc-sections" > /build/'"${WORK#$BR/}"'/engine-cfg.log 2>&1
        cmake --build /build/'"${WORK#$BR/}"'/engine-build -v > /build/'"${WORK#$BR/}"'/engine-build.log 2>&1; tail -3 /build/'"${WORK#$BR/}"'/engine-build.log
        grep -m1 -o " -O[0-3s]\b.*-c " /build/'"${WORK#$BR/}"'/engine-build.log | cut -c1-200 | sed "s/^/compile flags: /"' | sed 's/^/   /'
    ls -l "$WORK/engine-build/wpe-engine" "$WORK/engine-build/libgstwpebmedia.so" | sed 's/^/   /'
    # GStreamer's deinterleave element (WebKit's decodeAudioData needs it; the Buildroot tree does not build it)
    bash "$src/tools/build-gst-interleave.sh" "$BR" "$WORK/engine-build" | sed 's/^/   /'
}

# ---- backend: direct GPU presentation libraries (NDK), built from the clone --------------------------------------
do_backend() {
    say "libWPEBackend-webos.so + libwpeb-owner.so"
    ( cd "$src" && rm -rf build/webos-backend && WEBOS_NDK="$NDK" BR_OUT="$BR/br-out" bash tools/build-webos-backend.sh ) > "$WORK/backend.log" 2>&1 || { tail "$WORK/backend.log"; exit 1; }
    ls -l "$src/build/webos-backend/libWPEBackend-webos.so" "$src/build/webos-backend/libwpeb-owner.so" | sed "s/^/   /"
}


# ---- webkit32: libWPEWebKit = pristine 2.54.3 + patches/wpewebkit/2.54.3/0001..N, built in a PRIVATE COPY of the build tree -------
# The shared in-tree build (WK_SHARED, shared between several people and scripts) is not trusted to match the series: files get
# patched temporarily there. So: (1) a pristine source tree from the Buildroot download plus the repo's series in order; (2) a copy
# of the shared tree (cp -a keeps mtimes, so ninja stays incremental; bind-mounted at the SAME path in the container, so the absolute
# paths in build.ninja stay valid and the shared tree is never touched); (3) every source file that differs from the pristine tree is
# overwritten in the copy; (4) ninja in the copy (ccache is in the container's environment as in wk254.sh); (5) the copy must then differ
# from the pristine tree in 0 files (and miss none), or this phase fails (wk_diff). Stage takes libWPEWebKit, the helpers and the injected bundle from the copy.
# Disk: the copy is ~6 GB (removed with `webkit32 clean`).
# wk_diff <pristine> <copy>: the source files of <pristine> that differ in <copy> or are missing there, one relative path per line.
# (A file only in the copy is build output or a file a patch deleted: not listed.) diff's own failure (exit 2, e.g. an unreadable
# file) is an error here: it is never read as "0 differing".
wk_diff() {
    local pr=$1 c=$2 out rc=0 line d n
    out=$(LC_ALL=C diff -rq --no-dereference "$pr" "$c" 2>"$WORK/wk-diff.err") || rc=$?
    [ "$rc" -le 1 ] || { echo "diff exit $rc: $(head -3 "$WORK/wk-diff.err")" >&2; return 2; }
    out=$(printf '%s\n' "$out" | grep -a -e '^Files ' -e "^Only in $pr" || true)
    while IFS= read -r line; do
        [ -n "$line" ] || continue
        case "$line" in
            "Files $pr/"*" and $c/"*" differ") line=${line#"Files $pr/"}; echo "${line%% and $c/*}" ;;
            "Only in $pr"*": "*) line=${line#"Only in "}; d=${line%%: *}; n=${line#*: }; d=${d#"$pr"}; d=${d#/}; echo "${d:+$d/}$n" ;;
        esac
    done <<<"$out"
}
do_webkit32() {
    local tb=$BR/dl/wpewebkit/wpewebkit-2.54.3.tar.xz pr=$WORK/wk-pristine
    [ "${1:-}" = clean ] && { rm -rf "$WK_TREE" "$pr"; return; }
    [ -f "$tb" ] || { echo "no $tb" >&2; exit 1; }
    say "pristine 2.54.3 + patches -> $pr"
    rm -rf "$pr"; mkdir -p "$pr"; tar xf "$tb" -C "$pr" --strip-components=1
    local n=0 p
    for p in "$src"/buildroot-external/patches/wpewebkit/2.54.3/*.patch; do
        patch -p1 -s --no-backup-if-mismatch -d "$pr" < "$p" || { echo "patch $p failed" >&2; exit 1; }
        n=$((n+1))
    done
    echo "   applied $n patches (last: $(basename "$p"))"
    if [ ! -d "$WK_TREE" ] || [ "${REFRESH_WK:-0}" = 1 ]; then
        say "copying the build tree $WK_SHARED -> $WK_TREE ($(du -sm "$WK_SHARED" | cut -f1) MB)"
        rm -rf "$WK_TREE"; mkdir -p "$(dirname "$WK_TREE")"
        ( exec 9>"$BR/.wk254.lock"; flock 9; cp -a "$WK_SHARED" "$WK_TREE" )
    fi
    say "comparing the copy with the pristine sources"
    wk_diff "$pr" "$WK_TREE" > "$WORK/wk-differing.txt" || { echo "the comparison of the copy with the pristine sources failed" >&2; exit 1; }
    echo "   $(wc -l < "$WORK/wk-differing.txt") differing source files"; sed 's/^/     /' "$WORK/wk-differing.txt" | head -20
    while read -r f; do mkdir -p "$(dirname "$WK_TREE/$f")"; cp -a "$pr/$f" "$WK_TREE/$f"; done < "$WORK/wk-differing.txt"
    # Cortex-A55 code generation is the default (measured: JS -12%, nothing slower; tools/a55-probe ran every flag set on the TV).
    # WK_CPU="" builds for the toolchain's Cortex-A9 default again. Re-configuring the cache changes every compile line: a full rebuild.
    local cpu=${WK_CPU-"-march=armv8-a+crc -mfpu=neon-fp-armv8 -mtune=cortex-a55"} fl
    fl="-D_LARGEFILE_SOURCE -D_LARGEFILE64_SOURCE -D_FILE_OFFSET_BITS=64 -g0 -D_FORTIFY_SOURCE=1 -mthumb -O2 $cpu"
    say "configure the copy with: $fl"
    docker run --rm --user "$(id -u):$(id -g)" -e HOME=/build -v "$BR:/build" -v "$WK_TREE:/build/br-out/build/wpewebkit-2.54.3" -w /build webos-browser-build:1 \
        bash -c "cd /build/br-out/build/wpewebkit-2.54.3 && export PATH=/build/br-out/host/bin:\$PATH && cmake . -DCMAKE_C_FLAGS='$fl' -DCMAKE_CXX_FLAGS='$fl'" > "$WORK/webkit32-cfg.log" 2>&1 \
        || { tail -20 "$WORK/webkit32-cfg.log"; exit 1; }
    say "ninja in the private copy${FULL:+ (FULL=1: clean first, hours)}"
    docker run --rm --user "$(id -u):$(id -g)" -e HOME=/build -v "$BR:/build" -v "$WK_TREE:/build/br-out/build/wpewebkit-2.54.3" -w /build \
        --cpu-shares 512 webos-browser-build:1 nice -n 5 \
        bash -c 'cd /build/br-out/build/wpewebkit-2.54.3 && export PATH=/build/br-out/host/bin:$PATH && if [ -n "$FULL" ]; then ninja -t clean >/dev/null; fi && ninja -j${JOBS_WK:-24}' > "$WORK/webkit32-ninja.log" 2>&1 \
        || { tail -25 "$WORK/webkit32-ninja.log"; exit 1; }
    tail -1 "$WORK/webkit32-ninja.log" | cut -c1-120
    # The promised check: after the sync the copy must have no source file that differs from, or is missing against, the pristine tree.
    wk_diff "$pr" "$WK_TREE" > "$WORK/wk-differing-after.txt" || { echo "the final comparison of the copy with the pristine sources failed" >&2; exit 1; }
    if [ -s "$WORK/wk-differing-after.txt" ]; then
        echo "FAIL: $(wc -l < "$WORK/wk-differing-after.txt") source files of the build copy still differ from pristine 2.54.3 + the series:" >&2
        head -20 "$WORK/wk-differing-after.txt" | sed 's/^/     /' >&2; exit 1
    fi
    echo "   differing files after the sync: 0"
    rm -rf "$pr"
}

# ---- ubo: the ad blocker's data and web-process extension, built from the clone --------------------------------------
# tools/ubo/update.py downloads uBO's default lists and resources (network; the lists are whatever they are today, uBO's
# commit is recorded in COMMIT), tools/ubo/build.sh compiles them (cargo, node) and links libwren-ubo.so for the TV (armv7,
# x-tools gcc = glibc 2.28) and for the 64-bit engine (aarch64, Buildroot gcc). The extension must accept the user data the
# engine sends, "(sbasass)": checked with strings.
do_ubo() {
    export PATH=$HOME/.cargo/bin:$PATH RUSTC_WRAPPER=
    local U=$WORK/ubo X32=$BR/x-tools/arm-webos-linux-gnueabi A64=$BR/br-out-aarch64
    # UBO_LIBS_ONLY=1: keep the lists and data of the last run (no download), rebuild only the two extension libraries
    # (a change to engine/ubo/src or extension.c).
    if [ -z "${UBO_LIBS_ONLY:-}" ]; then
    rm -rf "$U"; mkdir -p "$U"
    rustup target add armv7-unknown-linux-gnueabi aarch64-unknown-linux-gnu 2>&1 | tail -2 | sed 's/^/   /'
    ( cd "$src" && python3 tools/ubo/update.py ) > "$WORK/ubo-update.log" 2>&1 || { tail "$WORK/ubo-update.log"; exit 1; }
    cp "$src/tools/ubo/ubo/COMMIT" "$U/COMMIT"
    say "ubo data (cargo build of the list compiler)"
    ( cd "$src" && OUT="$U" sh tools/ubo/build.sh data ) > "$WORK/ubo-data.log" 2>&1 || { tail -20 "$WORK/ubo-data.log"; exit 1; }
    ls -l "$U/data/engine.dat" | sed 's/^/   /'
    fi
    local sr32=$BR/br-out/host/arm-buildroot-linux-gnueabi/sysroot
    say "libwren-ubo.so armv7"
    ( cd "$src" && OUT="$U" CC="$X32/bin/arm-webos-linux-gnueabi-gcc -mcpu=cortex-a9 -mfloat-abi=softfp" \
        EXT_CFLAGS="$(PKG_CONFIG_SYSROOT_DIR=$sr32 PKG_CONFIG_LIBDIR=$sr32/usr/lib/pkgconfig:$sr32/usr/share/pkgconfig pkg-config --cflags wpe-web-process-extension-2.0 libsoup-3.0)" \
        sh tools/ubo/build.sh lib armv7-unknown-linux-gnueabi ) > "$WORK/ubo-lib32.log" 2>&1 || { tail -20 "$WORK/ubo-lib32.log"; exit 1; }
    say "libwren-ubo.so aarch64"
    local sr64=$A64/host/aarch64-buildroot-linux-gnu/sysroot
    ( cd "$src" && OUT="$U" CC="$A64/host/bin/aarch64-buildroot-linux-gnu-gcc" \
        EXT_CFLAGS="$(PKG_CONFIG_SYSROOT_DIR=$sr64 PKG_CONFIG_LIBDIR=$sr64/usr/lib/pkgconfig:$sr64/usr/share/pkgconfig pkg-config --cflags wpe-web-process-extension-2.0 libsoup-3.0)" \
        CARGO_TARGET_AARCH64_UNKNOWN_LINUX_GNU_LINKER="$U/cc-aarch64-unknown-linux-gnu" \
        sh tools/ubo/build.sh lib aarch64-unknown-linux-gnu ) > "$WORK/ubo-lib64.log" 2>&1 || { tail -20 "$WORK/ubo-lib64.log"; exit 1; }
    local f
    for f in "$U"/armv7-unknown-linux-gnueabi/libwren-ubo.so "$U"/aarch64-unknown-linux-gnu/libwren-ubo.so; do
        [ "$(strings -a "$f" | grep -c '(sbasass)')" -gt 0 ] || { echo "$f does not accept (sbasass)" >&2; exit 1; }
        file "$f" | cut -c1-120 | sed 's/^/   /'
    done
}

# ---- stage: <WORK>/stage-<id>/engine = base + WebKit 2.54.3 + launcher in front + uBO ---------------------------
interp_all() {  # interp_all <dir> <link>: every 32-bit ARM executable gets PT_INTERP <link>
    local d=$1 link=$2 f
    find "$d" -type f \( -perm -u+x -o -name '*.so*' \) | while read -r f; do
        if head -c 4 "$f" | grep -q ELF && "$PATCHELF" --print-interpreter "$f" >/dev/null 2>&1; then
            "$PATCHELF" --set-interpreter "$link" "$f"
        fi
    done
}
do_stage() {
    for id in $IDS; do
        local app; app=$(appid "$id"); local s=$WORK/stage-$id/engine
        say "stage $id ($app) -> $s"
        [ -d "$STAGE_BASE" ] || { echo "no base stage $STAGE_BASE (tools/stage-engine.sh)" >&2; exit 1; }
        chmod -R u+w "$WORK/stage-$id" 2>/dev/null || true
        rm -rf "$WORK/stage-$id"; mkdir -p "$WORK/stage-$id"
        cp -a "$STAGE_BASE" "$s"
        rm -f "$s/wpe-engine"                        # the 2.50.5 engine
        # WebKit 2.54.3 (the JIT series), stripped
        install -m755 "$WK254/lib/libWPEWebKit-2.0.so.1.11.6" "$s/lib/libWPEWebKit-2.0.so.1"
        for h in WPEWebProcess WPENetworkProcess WPEGPUProcess; do install -m755 "$WK254/bin/$h" "$s/libexec/$h"; done
        install -m755 "$WK254/lib/libWPEInjectedBundle.so" "$s/lib/injected-bundle/libWPEInjectedBundle.so"
        install -m755 "$WORK/engine-build/libgstwpebmedia.so" "$s/lib/gstreamer-1.0/libgstwpebmedia.so"
        install -m755 "$WORK/engine-build/libgstinterleave.so" "$s/lib/gstreamer-1.0/libgstinterleave.so"
        install -m755 "$src/build/webos-backend/libWPEBackend-webos.so" "$src/build/webos-backend/libwpeb-owner.so" "$s/lib/"
        install -m755 "$WORK/engine-build/wpe-engine" "$s/wpe-engine32"
        install -m755 "$WORK/launcher-$id/wpe-engine" "$s/wpe-engine"
        for f in "$s/lib/libWPEWebKit-2.0.so.1" "$s"/libexec/WPE*Process "$s/lib/injected-bundle/libWPEInjectedBundle.so" \
                 "$s/lib/gstreamer-1.0/libgstwpebmedia.so" "$s/lib/gstreamer-1.0/libgstinterleave.so" "$s/wpe-engine32" "$s/lib/libWPEBackend-webos.so" "$s/lib/libwpeb-owner.so"; do "$STRIP32" "$f"; done
        # PT_INTERP of everything that is exec'd: the app's own loader link
        for f in "$s/wpe-engine32" "$s"/libexec/*; do
            "$PATCHELF" --print-interpreter "$f" >/dev/null 2>&1 && "$PATCHELF" --set-interpreter "/tmp/$app/ld-linux.so.3" "$f"
        done
        # uBO: data + the web-process extension, built by the `ubo` phase from the clone
        [ -f "$WORK/ubo/data/engine.dat" ] && [ -f "$WORK/ubo/armv7-unknown-linux-gnueabi/libwren-ubo.so" ] || { echo "run the ubo phase first" >&2; exit 1; }
        mkdir -p "$s/ubo" "$s/lib/web-extensions"; cp -a "$WORK/ubo/data/." "$s/ubo/"
        # The page scripts are plain files: always the clone's, never the copy the ubo phase made (a ubo phase from before a change to
        # engine/ubo/compat.js left the old policy in the package).
        cp "$src/engine/ubo/compat.js" "$src/engine/ubo/cosmetic.js" "$s/ubo/"
        install -m755 "$WORK/ubo/armv7-unknown-linux-gnueabi/libwren-ubo.so" "$s/lib/web-extensions/libwren-ubo.so"
        du -sh "$s" | sed 's/^/   /'
    done
}

# ---- ui: the kit package (UI + engine/) per id ------------------------------------------------------------------
do_ui() {
    for id in $IDS; do
        local app; app=$(appid "$id")
        say "UI + kit package for $app"
        docker run --rm -u "$(id -u):$(id -g)" -e HOME=/build -v "$src:/src" -v "$BR:/build" -v "$HOME/webos-ndk:$HOME/webos-ndk:ro" \
            -e WEBOS_NDK="$NDK" -w /src/browser webos-browser-build:1 sh -c "
            set -e
            cmake -S . -B build/webos-$id -G 'Unix Makefiles' -DCMAKE_TOOLCHAIN_FILE=\"\$WEBOS_NDK/share/buildroot/toolchainfile.cmake\" \
                -DCMAKE_BUILD_TYPE=Release ${SOURCE_URL:+-DWNK_SOURCE_URL=$SOURCE_URL} -DWPEB_APPID=$app -DWPEB_TITLE=\"$(title_of $id)\" -DWPEB_ENGINE_STAGE=/build/${WORK#$BR/}/stage-$id/engine > /tmp/cfg.log 2>&1 || { tail -25 /tmp/cfg.log; exit 1; }
            cmake --build build/webos-$id -j\$(nproc) > /tmp/build.log 2>&1 || { grep -E 'error|Error' /tmp/build.log | head -20; exit 1; }
            cmake --build build/webos-$id --target wnk-ipks > /tmp/ipk.log 2>&1 || { tail -30 /tmp/ipk.log; exit 1; }
            ls -l build/webos-$id/dist/" 2>&1 | tail -6 | sed 's/^/   /'
    done
}

# ---- add64: engine64/ and the engine notes go into the same ipk -------------------------------------------------
do_add64() {
    [ -d "$STAGE64" ] || { echo "no aarch64 stage $STAGE64 (tools/engine-aarch64.sh stage)" >&2; exit 1; }
    rm -rf "$STAGE_OUT"; mkdir -p "$STAGE_OUT"   # this run's set only; $OUT is not touched until `publish`
    for id in $IDS; do
        local app; app=$(appid "$id")
        local in; in=$(ls -t "$src"/browser/build/webos-"$id"/dist/${app}_*_arm.ipk | head -1)
        say "add engine64 to $in"
        local w; w=$(mktemp -d)
        ( cd "$w" && ar x "$in" && mkdir data && tar xzf data.tar.gz -C data )
        local appdir="$w/data/usr/palm/applications/$app"
        cp -a "$STAGE64" "$appdir/engine64"
        rm -rf "$appdir/engine64/licenses" "$appdir/engine64/lib/glibc/gconv"   # UTF-8/ASCII only; licences ship in engine/licenses
        find "$appdir/engine64" -type f \( -name '*.so' -o -name '*.so.*' -o -perm -u+x \) | while read -r f; do
            case "$f" in *.dat|*.conf|*.json|*.txt|*.ini|*.js) continue ;; esac
            "$STRIP64" --strip-unneeded "$f" 2>/dev/null || true
        done

        # PT_INTERP of what WebKit exec()s itself: the link the launcher creates in /tmp/<app id>/ (wpe-launcher.c)
        for f in "$appdir/engine64/wpe-engine" "$appdir"/engine64/libexec/*; do
            "$PATCHELF" --print-interpreter "$f" >/dev/null 2>&1 && "$PATCHELF" --set-interpreter "/tmp/$app/ld-linux-aarch64.so.1" "$f"
        done
        # uBO for the 64-bit engine: it looks for ubo/ and lib/web-extensions/ next to /proc/self/exe, which is the LOADER
        # (engine64/lib/glibc/) because the launcher starts it through ld-linux-aarch64.so.1. So the aarch64 extension goes
        # there and the data is a link to the 32-bit engine's copy (same format).
        mkdir -p "$appdir/engine64/lib/glibc/lib/web-extensions"
        install -m755 "$WORK/ubo/aarch64-unknown-linux-gnu/libwren-ubo.so" "$appdir/engine64/lib/glibc/lib/web-extensions/libwren-ubo.so"
        ln -s ../../../engine/ubo "$appdir/engine64/lib/glibc/ubo"
        cat > "$appdir/ENGINES.txt" <<EOF
Wren $VERSION ($app): two web engines in one package
  engine/    32-bit WPE WebKit 2.54.3 (ARMv7 JIT + DFG), the default. engine/wpe-engine is a small launcher; the real
             engine is engine/wpe-engine32.
  engine64/  the same WebKit 2.54.3 as an aarch64 process (about twice as fast on JavaScript, software rendering,
             shared-memory frames only: no hardware video, no direct presentation). Its ad blocker is the same uBO data with an aarch64 extension.
Switching (the launcher, engine/wpe-engine, reads these in this order):
  1. launch parameter  {"wpebEnv":{"WPE_BROWSER_DEBUG_ENGINE":"64"}}   (or "32")   - one start, e.g.
       ares-launch -d <tv> $app -p '{"wpebEnv":{"WPE_BROWSER_DEBUG_ENGINE":"64"}}'
  2. a file named engine64 containing the word "on" in the app's data folder (WPE_BROWSER_DATA_DIR,
     /media/developer/$app.data*/ on the TV)   - persistent; delete it (or write "off") to go back
  3. otherwise the 32-bit engine.
If engine64/ or its loader is missing the launcher logs "wpe-launcher: ..." in engine.log and starts the 32-bit engine.
Hardware video in shared-memory mode is opt-in: launch parameter {"wpebShmVideo":"1"} (32-bit engine).
Licences of the bundled libraries: licenses/ and engine/licenses/.
EOF
        # control: new Installed-Size; same package name and version
        local kb; kb=$(du -sk "$w/data" | cut -f1)
        mkdir -p "$w/ctl"; tar xzf "$w/control.tar.gz" -C "$w/ctl"
        sed -i "s/^Installed-Size:.*/Installed-Size: $kb/" "$w/ctl/control" 2>/dev/null || true
        ( cd "$w/ctl" && tar czf "$w/control.tar.gz" --owner=0 --group=0 control )   # no "./": the TV installer rejects ./ entries
        ( cd "$w/data" && tar czf "$w/data.tar.gz" --owner=0 --group=0 usr )
        local outf=$STAGE_OUT/${app}_${VERSION}_arm.ipk
        rm -f "$outf"; ( cd "$w" && python3 "$src/tools/mkar.py" "$outf" debian-binary control.tar.gz data.tar.gz )   # not `ar rc`: BusyBox ar on the TV rejects "name/"
        echo "   $outf ($(mb "$(stat -c %s "$outf")") MB, installed $((kb/1024)) MB)"
        rm -rf "$w"
    done
}

do_sums() {
    [ -d "$STAGE_OUT" ] || { echo "no $STAGE_OUT: run add64 first" >&2; exit 1; }
    local id
    ( cd "$STAGE_OUT" && for id in $IDS; do echo "./$(appid "$id")_${VERSION}_arm.ipk"; done | xargs md5sum | tee MD5SUMS | sed 's/^/   /' )
    { echo "commit $COMMIT ($(git -C "$repo" log -1 --format=%s "$COMMIT"))"
      echo "ids $IDS  version $VERSION"
      echo "date $(date -u +%FT%TZ)"
      echo "WK254 libWPEWebKit (stripped) md5 $(md5sum "$WORK/stage-${IDS%% *}/engine/lib/libWPEWebKit-2.0.so.1" | cut -d' ' -f1)"
      echo "inputs: 32-bit engine stage $(basename "$(dirname "$STAGE_BASE")"), 64-bit engine stage $(basename "$(dirname "$STAGE64")"); uBO: built from the clone ($(cat "$WORK/ubo/COMMIT" 2>/dev/null))"
      echo "kit $(git -C "$src/third_party/webos-native-app-kit" log -1 --format=%h)"; } > "$STAGE_OUT/BUILD-INFO.txt"
}

# ---- check: validate the assembled ipks (default: $STAGE_OUT, else $OUT; CHECK_DIR=... for another) --------------------------
# Libraries the TV itself provides (DT_NEEDED entries that are allowed to be missing from the package): the Wayland client and EGL window
# libraries of the direct-presentation backend, and PulseAudio's client (only the optional gst pulse plugin wants it; GStreamer skips a plugin
# it cannot load).
TV_LIBS=${TV_LIBS:-"libwayland-client.so.0 libwayland-egl.so libpulse.so.0"}
do_check() {
    local dir=${CHECK_DIR:-} id app ipk w f n d tree arch interp bad=0 count=0
    if [ -z "$dir" ]; then if [ -d "$STAGE_OUT" ]; then dir=$STAGE_OUT; else dir=$OUT; fi; fi
    say "check $dir (ids: $IDS)"
    fail() { echo "   FAIL: $*" >&2; bad=$((bad+1)); }
    local bi; bi=$(sed -n 's/^commit \([0-9a-f]\{40\}\).*/\1/p' "$dir/BUILD-INFO.txt" 2>/dev/null | head -1)
    [ "$bi" = "$COMMIT" ] || fail "BUILD-INFO.txt commit '${bi:-none}' is not $COMMIT"
    if [ -e "$src/.git" ]; then [ "$(git -C "$src" rev-parse HEAD)" = "$COMMIT" ] || fail "the clean clone $src is not at $COMMIT"; fi
    for ipk in "$dir"/*.ipk; do
        [ -e "$ipk" ] || break
        n=$(basename "$ipk"); d=0
        for id in $IDS; do [ "$n" = "$(appid "$id")_${VERSION}_arm.ipk" ] && d=1; done
        [ "$d" = 1 ] || fail "$n is not one of the packages of IDS=$IDS (a leftover of another run?)"
    done
    for id in $IDS; do
        app=$(appid "$id"); ipk=$dir/${app}_${VERSION}_arm.ipk
        if [ ! -f "$ipk" ]; then fail "$ipk missing (app id $app)"; continue; fi
        count=$((count+1))
        grep -qF " ./$(basename "$ipk")" "$dir/MD5SUMS" 2>/dev/null && ( cd "$dir" && grep -F " ./$(basename "$ipk")" MD5SUMS | md5sum -c --status ) || fail "MD5SUMS has no matching line for $(basename "$ipk")"
        w=$(mktemp -d "$TMPDIR/check.XXXXXX")
        if ! ( cd "$w" && ar x "$ipk" && mkdir data ctl && tar xzf data.tar.gz -C data && tar xzf control.tar.gz -C ctl ); then fail "$ipk does not unpack"; rm -rf "$w"; continue; fi
        grep -qx "Package: $app" "$w/ctl/control" || fail "$app: control has not 'Package: $app'"
        grep -qx "Version: $VERSION" "$w/ctl/control" || fail "$app: control has not 'Version: $VERSION'"
        local ad=$w/data/usr/palm/applications
        [ "$(ls "$ad" 2>/dev/null)" = "$app" ] || fail "$app: the applications folder holds '$(ls "$ad" 2>/dev/null | tr '\n' ' ')', not just $app"
        local A=$ad/$app
        grep -q "\"id\": *\"$app\"" "$A/appinfo.json" 2>/dev/null || fail "$app: appinfo.json id is not $app"
        for f in ENGINES.txt engine/wpe-engine engine/wpe-engine32 engine/lib/libWPEWebKit-2.0.so.1 engine/libexec/WPEWebProcess engine/libexec/WPENetworkProcess \
                 engine/libexec/WPEGPUProcess engine/lib/injected-bundle/libWPEInjectedBundle.so engine/lib/gstreamer-1.0/libgstwpebmedia.so \
                 engine/lib/libWPEBackend-webos.so engine/lib/glibc/ld-linux.so.3 engine/ubo/engine.dat engine/lib/web-extensions/libwren-ubo.so \
                 engine64/wpe-engine engine64/lib/libWPEWebKit-2.0.so.1 engine64/libexec/WPEWebProcess engine64/libexec/WPENetworkProcess \
                 engine64/lib/glibc/ld-linux-aarch64.so.1 engine64/lib/glibc/ubo/engine.dat engine64/lib/glibc/lib/web-extensions/libwren-ubo.so; do
            [ -e "$A/$f" ] || fail "$app: $f missing"
        done
        while read -r f; do fail "$app: dangling link $f"; done < <(cd "$A" && find . -xtype l)
        for tree in engine engine64; do
            [ -d "$A/$tree" ] || continue
            if [ "$tree" = engine ]; then arch=ARM; else arch=AArch64; fi
            # every ELF file: the right machine; every DT_NEEDED name: a file of the same engine tree, or a TV library
            find "$A/$tree" -type f > "$w/files.txt"; sed 's|.*/||' "$w/files.txt" | sort -u > "$w/names.txt"; : > "$w/needed.txt"
            while read -r f; do
                head -c4 "$f" | grep -q ELF || continue
                readelf -h "$f" 2>/dev/null | grep -q "Machine: *$arch\$" || fail "$app: ${f#$A/} is not $arch"
                readelf -d "$f" 2>/dev/null | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p' | sed "s|$| ${f#$A/}|" >> "$w/needed.txt"
            done < "$w/files.txt"
            while read -r n f; do
                grep -qxF "$n" "$w/names.txt" && continue
                case " $TV_LIBS " in *" $n "*) continue ;; esac
                fail "$app: $f needs $n: not in $tree/ and not a TV library (TV_LIBS)"
            done < <(sort -u "$w/needed.txt")
        done
        # PT_INTERP of what is exec'd: the app's own loader links (the launcher makes them in /tmp/<app id>/)
        for f in engine/wpe-engine engine/wpe-engine32 engine/libexec/WPEWebProcess engine/libexec/WPENetworkProcess engine/libexec/WPEGPUProcess; do
            interp=$(readelf -l "$A/$f" 2>/dev/null | sed -n 's/.*interpreter: \(.*\)\]/\1/p')
            [ "$interp" = "/tmp/$app/ld-linux.so.3" ] || fail "$app: $f PT_INTERP is '$interp'"
        done
        for f in engine64/wpe-engine engine64/libexec/WPEWebProcess engine64/libexec/WPENetworkProcess engine64/libexec/WPEGPUProcess; do
            interp=$(readelf -l "$A/$f" 2>/dev/null | sed -n 's/.*interpreter: \(.*\)\]/\1/p')
            [ "$interp" = "/tmp/$app/ld-linux-aarch64.so.1" ] || fail "$app: $f PT_INTERP is '$interp'"
        done
        rm -rf "$w"
    done
    [ "$count" -ge 1 ] || fail "no package of IDS=$IDS found in $dir"
    [ "$bad" -eq 0 ] || { echo "check: $bad problem(s): nothing is published" >&2; exit 1; }
    say "check passed: $count package(s) in $dir"
}

# ---- publish: the checked set replaces the old one in $OUT --------------------------------------------------------
do_publish() {
    [ -d "$STAGE_OUT" ] || { echo "nothing to publish: no $STAGE_OUT" >&2; exit 1; }
    CHECK_DIR=$STAGE_OUT do_check
    mkdir -p "$OUT"
    rm -f "$OUT"/*_arm.ipk "$OUT/MD5SUMS" "$OUT/BUILD-INFO.txt"
    mv "$STAGE_OUT"/*_arm.ipk "$STAGE_OUT/MD5SUMS" "$STAGE_OUT/BUILD-INFO.txt" "$OUT/"
    rmdir "$STAGE_OUT"
    say "published to $OUT"; ls -l "$OUT"/*_arm.ipk | sed 's/^/   /'
}

case "$phase" in
    clone) do_clone ;;
    overlay) do_overlay ;;
    launcher) do_launcher ;;
    engine32) do_engine32 ;;
    backend) do_backend ;;
    webkit32) do_webkit32 "${2:-}" ;;
    ubo) do_ubo ;;
    stage) do_stage ;;
    ui) do_ui ;;
    add64) do_add64 ;;
    sums) do_sums ;;
    check) do_check ;;
    publish) do_publish ;;
    all) do_clone; do_overlay; do_launcher; do_webkit32; do_engine32; do_backend; do_ubo; do_stage; do_ui; do_add64; do_sums; do_publish ;;
    *) echo "usage: $0 all|clone|overlay|launcher|webkit32|engine32|backend|ubo|stage|ui|add64|sums|check|publish" >&2; exit 2 ;;
esac
