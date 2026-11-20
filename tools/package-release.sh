#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# The release package: ONE ipk with the UI, the 32-bit WebKit 2.54.3 engine (JIT+DFG), the aarch64 engine behind the
# engine launcher, and the uBO data. Two variants from the same tree: the dev id (org.sostk.wpebrowser.dev, for
# testing beside the real app) and the real id (org.sostk.wpebrowser). Builds from a CLEAN CLONE of a commit, so
# uncommitted work in the working tree never ends up in the package. Host only (WSL/Linux + Docker); no TV commands.
#
#   bash tools/package-release.sh all                 every phase, both ids -> build/release/*.ipk, NOTES-data
#   bash tools/package-release.sh clone|launcher|engine32|stage|ui|add64|check|sums   one phase (each needs the ones before)
#   IDS="dev"  COMMIT=<sha>  BUILD_ROOT=~/webos-browser-build  OUT=...  bash tools/package-release.sh all
#
# Inputs that are NOT built here (each has its own script; the package records which it used, build/release/BUILD-INFO.txt):
#   STAGE_BASE  tools/stage-engine.sh output (libs, glibc, GStreamer, fonts, certs, licences): build/engine-stage-jit/engine
#   WK254       the in-tree WebKit 2.54.3 build with patches/wpewebkit/2.54.3/0001..0021 (tools/build-engine.sh with
#               WEBKIT_VERSION=2.54.3, then ninja; wk254.sh): libWPEWebKit, WPE{Web,Network,GPU}Process, injected bundle.
#               The stripped libWPEWebKit has md5 a281cdbc88005b5338e401dc226f7613 (= the TV's engine254jit); NOT rebuilt here
#               (a WebKit build takes hours; `tools/build-engine.sh start` with WEBKIT_VERSION=2.54.3 rebuilds it from the patch series).
#   STAGE64     tools/engine-aarch64.sh stage output: build/engine-stage-aarch64/engine (aarch64 engine, glibc, Mesa)
#   UBO_IPK     an earlier package that carries engine/ubo/ and engine/lib/web-extensions/libwren-ubo.so (dist/...ubo3...ipk);
#               regenerate with tools/ubo/build.sh data + lib (needs cargo, node, network).
# Rebuilt here from the clean clone: wpe-engine + libgstwpebmedia.so (against the 2.54.3 headers), the launcher, the UI.
set -euo pipefail
repo=$(cd "$(dirname "$0")/.." && pwd)
BR=${BUILD_ROOT:-$HOME/webos-browser-build}
COMMIT=${COMMIT:-$(git -C "$repo" rev-parse HEAD)}
OUT=${OUT:-$repo/build/release}
WORK=${WORK:-$BR/release-work}
IDS=${IDS:-"dev release"}
VERSION=${VERSION:-0.3.3}
STAGE_BASE=${STAGE_BASE:-$HOME/wren/build/engine-stage-jit/engine}
WK254=${WK254:-$BR/br-out/build/wpewebkit-2.54.3}
STAGE64=${STAGE64:-$repo/build/engine-stage-aarch64/engine}
UBO_IPK=${UBO_IPK:-$repo/dist/org.sostk.wpebrowser_0.3.3-ubo3_arm.ipk}
NDK=${WEBOS_NDK:-$HOME/webos-ndk/arm-webos-linux-gnueabi_sdk-buildroot}
STRIP32=$BR/x-tools/arm-webos-linux-gnueabi/bin/arm-webos-linux-gnueabi-strip
STRIP64=$BR/br-out-aarch64/host/bin/aarch64-buildroot-linux-gnu-strip
PATCHELF=${PATCHELF:-$BR/br-out-aarch64/host/bin/patchelf}
src=$WORK/src
export TMPDIR=${PKG_TMP:-$WORK/tmp}; mkdir -p "$TMPDIR"
appid() { case "$1" in dev) echo org.sostk.wpebrowser.dev ;; release) echo org.sostk.wpebrowser ;; *) echo "bad id $1" >&2; exit 2 ;; esac; }
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
    git -C "$src" config submodule.third_party/webos-native-app-kit.url "$repo/third_party/webos-native-app-kit"
    git -C "$src" -c protocol.file.allow=always submodule update --init -q
    git -C "$src/third_party/webos-native-app-kit" checkout -q "$kitsha"
    ( cd "$src" && sh third_party/patches/apply-kit-patches.sh ) > "$WORK/kit-patches.log" 2>&1 || { tail "$WORK/kit-patches.log"; exit 1; }
    git -C "$src" log -1 --format='%H %s'
    # the kit with its patches must equal the working tree's (which the earlier builds used), file by file
    diff -rq --strip-trailing-cr --exclude=.git "$src/third_party/webos-native-app-kit" "$repo/third_party/webos-native-app-kit" \
        | head -5 | sed 's/^/   kit differs: /' || true
}

# ---- overlay: uncommitted packaging/launcher files of the working tree (empty once they are committed) -------------
OVERLAY=${OVERLAY-"engine/launcher tools"}
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
        LAUNCHER_SRC="$src/engine/launcher/wpe-launcher.c" LAUNCHER_INTERP="/tmp/$(appid $id)/ld-linux.so.3" OUT_DIR="$WORK/launcher-$id" bash "$repo/tools/build-launcher.sh" | sed "s/^/   /"
    done
}

# ---- engine32: wpe-engine + libgstwpebmedia.so from the clone, against the 2.54.3 headers ------------------------
do_engine32() {
    say "32-bit wpe-engine against WebKit 2.54.3 headers"
    local inc=$BR/inc254
    if [ ! -d "$inc/wpe-webkit-2.0" ]; then
        mkdir -p "$inc"; cp -a "$BR/br-out-aarch64/target/usr/include/wpe-webkit-2.0" "$inc/"
    fi
    docker run --rm --user "$(id -u):$(id -g)" -e HOME=/build -v "$src:/src" -v "$BR:/build" -w /build webos-browser-build:1 sh -c '
        set -e
        cmake -S /src/engine -B /build/release-work/engine-build -G Ninja \
            -DCMAKE_TOOLCHAIN_FILE=/build/br-out/host/share/buildroot/toolchainfile.cmake -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_CXX_FLAGS="-I/build/inc254/wpe-webkit-2.0 -I/build/inc254/wpe-webkit-2.0/wpe-platform" \
            -DCMAKE_C_FLAGS="-I/build/inc254/wpe-webkit-2.0 -I/build/inc254/wpe-webkit-2.0/wpe-platform" > /build/release-work/engine-cfg.log 2>&1
        cmake --build /build/release-work/engine-build 2>&1 | tail -3' | sed 's/^/   /'
    ls -l "$WORK/engine-build/wpe-engine" "$WORK/engine-build/libgstwpebmedia.so" | sed 's/^/   /'
}

# ---- backend: direct GPU presentation libraries (NDK), built from the clone --------------------------------------
do_backend() {
    say "libWPEBackend-webos.so + libwpeb-owner.so"
    ( cd "$src" && rm -rf build/webos-backend && WEBOS_NDK="$NDK" bash tools/build-webos-backend.sh ) > "$WORK/backend.log" 2>&1 || { tail "$WORK/backend.log"; exit 1; }
    ls -l "$src/build/webos-backend/libWPEBackend-webos.so" "$src/build/webos-backend/libwpeb-owner.so" | sed "s/^/   /"
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
        install -m755 "$src/build/webos-backend/libWPEBackend-webos.so" "$src/build/webos-backend/libwpeb-owner.so" "$s/lib/"
        install -m755 "$WORK/engine-build/wpe-engine" "$s/wpe-engine32"
        install -m755 "$WORK/launcher-$id/wpe-engine" "$s/wpe-engine"
        for f in "$s/lib/libWPEWebKit-2.0.so.1" "$s"/libexec/WPE*Process "$s/lib/injected-bundle/libWPEInjectedBundle.so" \
                 "$s/lib/gstreamer-1.0/libgstwpebmedia.so" "$s/wpe-engine32" "$s/lib/libWPEBackend-webos.so" "$s/lib/libwpeb-owner.so"; do "$STRIP32" "$f"; done
        # PT_INTERP of everything that is exec'd: the app's own loader link
        for f in "$s/wpe-engine32" "$s"/libexec/*; do
            "$PATCHELF" --print-interpreter "$f" >/dev/null 2>&1 && "$PATCHELF" --set-interpreter "/tmp/$app/ld-linux.so.3" "$f"
        done
        # uBO: data + the web-process extension (prebuilt, from UBO_IPK)
        local t; t=$(mktemp -d)
        ( cd "$t" && ar x "$UBO_IPK" && tar xzf data.tar.gz --wildcards '*/engine/ubo/*' '*/engine/lib/web-extensions/*' )
        cp -a "$t"/usr/palm/applications/*/engine/ubo "$s/"
        mkdir -p "$s/lib/web-extensions"; cp -a "$t"/usr/palm/applications/*/engine/lib/web-extensions/. "$s/lib/web-extensions/"
        rm -rf "$t"
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
                -DCMAKE_BUILD_TYPE=Release -DWPEB_APPID=$app -DWPEB_ENGINE_STAGE=/build/release-work/stage-$id/engine > /tmp/cfg.log 2>&1 || { tail -25 /tmp/cfg.log; exit 1; }
            cmake --build build/webos-$id -j\$(nproc) > /tmp/build.log 2>&1 || { grep -E 'error|Error' /tmp/build.log | head -20; exit 1; }
            cmake --build build/webos-$id --target wnk-ipks > /tmp/ipk.log 2>&1 || { tail -30 /tmp/ipk.log; exit 1; }
            ls -l build/webos-$id/dist/" 2>&1 | tail -6 | sed 's/^/   /'
    done
}

# ---- add64: engine64/ and the engine notes go into the same ipk -------------------------------------------------
do_add64() {
    [ -d "$STAGE64" ] || { echo "no aarch64 stage $STAGE64 (tools/engine-aarch64.sh stage)" >&2; exit 1; }
    mkdir -p "$OUT"
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
        cat > "$appdir/ENGINES.txt" <<EOF
Wren $VERSION ($app): two web engines in one package
  engine/    32-bit WPE WebKit 2.54.3 (ARMv7 JIT + DFG), the default. engine/wpe-engine is a small launcher; the real
             engine is engine/wpe-engine32.
  engine64/  the same WebKit 2.54.3 as an aarch64 process (about twice as fast on JavaScript, software rendering,
             shared-memory frames only: no hardware video, no direct presentation). No ad blocker in it yet.
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
        local outf=$OUT/${app}_${VERSION}_arm.ipk
        rm -f "$outf"; ( cd "$w" && python3 "$repo/tools/mkar.py" "$outf" debian-binary control.tar.gz data.tar.gz )   # not `ar rc`: BusyBox ar on the TV rejects "name/"
        echo "   $outf ($(mb "$(stat -c %s "$outf")") MB, installed $((kb/1024)) MB)"
        rm -rf "$w"
    done
}

do_sums() {
    ( cd "$OUT" && md5sum ./*.ipk | tee MD5SUMS | sed 's/^/   /' )
    { echo "commit $COMMIT ($(git -C "$repo" log -1 --format=%s "$COMMIT"))"
      echo "date $(date -u +%FT%TZ)"
      echo "WK254 libWPEWebKit (stripped) md5 $(md5sum "$WORK/stage-${IDS%% *}/engine/lib/libWPEWebKit-2.0.so.1" | cut -d' ' -f1)"
      echo "STAGE_BASE $STAGE_BASE  STAGE64 $STAGE64  UBO_IPK $UBO_IPK"
      echo "kit $(git -C "$src/third_party/webos-native-app-kit" log -1 --format=%h)"; } > "$OUT/BUILD-INFO.txt"
}

case "$phase" in
    clone) do_clone ;;
    overlay) do_overlay ;;
    launcher) do_launcher ;;
    engine32) do_engine32 ;;
    backend) do_backend ;;
    stage) do_stage ;;
    ui) do_ui ;;
    add64) do_add64 ;;
    sums) do_sums ;;
    all) do_clone; do_overlay; do_launcher; do_engine32; do_backend; do_stage; do_ui; do_add64; do_sums ;;
    *) echo "usage: $0 all|clone|launcher|engine32|backend|stage|ui|add64|sums" >&2; exit 2 ;;
esac
