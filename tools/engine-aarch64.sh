#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# wpe-engine for aarch64 (the TV's org.webosbrew.bridge-64to32), built from the aarch64 Buildroot output
# (buildroot-external/configs/webos_browser_aarch64_defconfig; tools/build-engine-aarch64.sh builds that output in a
# container) instead of the 32-bit engine's softfp tree. Nothing here touches the 32-bit build: same sources
# (engine/), Buildroot's own toolchain file for the aarch64 output, its own build directory.
#
#   bash tools/engine-aarch64.sh              build + stage engine64/ (the aarch64 counterpart of stage-engine.sh)
#   bash tools/engine-aarch64.sh build        configure + build only, into build/engine-aarch64/engine-build
#   bash tools/engine-aarch64.sh check        per-source -fsyntax-only sweep, continuing past errors
#   bash tools/engine-aarch64.sh preflight    compile what does not need WebKit (media sink, protocol header, wire
#                                             layout) and list what blocks the rest
#   bash tools/engine-aarch64.sh plugins      the GStreamer plugin set assertion on its own
#   BR_OUT=~/webos-browser-build/br-out-aarch64 JOBS=2 bash tools/engine-aarch64.sh
#
# Requires a finished Buildroot output. The engine cannot be built or staged before BOTH of these hold, and the
# script says so instead of producing a half-working package:
#   1. WebKit 2.54.3 for aarch64 in the output: usr/include/wpe-webkit-<ver>/, usr/lib/libWPEWebKit-2.0.so*,
#      usr/lib/pkgconfig/{wpe-webkit-2.0,wpe-platform-2.0}.pc  (build/wpewebkit-* exists)
#   2. the full GStreamer plugin set the 32-bit engine ships (see "required" in plugins below): an incomplete tree
#      has only 16 plugins (no qtdemux/matroska/souphttp/pulse/vpx/audioparsers/adaptivedemux2/mpegtsdemux) and the
#      browser would play nothing.
# In either case: bash tools/build-engine-aarch64.sh start [TARGETS=wpewebkit-depends] finishes the tree.
#
# What the stage step produces (build/engine-stage-aarch64/engine/, the aarch64 counterpart of stage-engine.sh):
#   wpe-engine, libexec/{WPEWebProcess,WPENetworkProcess,gst-plugin-scanner}, lib/*.so (DT_NEEDED closure),
#   lib/glibc/ (the tree's glibc 2.44 + loader: mandatory, see REPORT.md §5),
#   lib/mesa/  (libEGL/libGLESv2 + libgallium + libLLVM: mandatory, SHM presentation is the only mode), gstreamer
#   plugins, fonts, certificates. Every executable gets PT_INTERP =
#   /tmp/io.github.mihai2mn.robin/ld-linux-aarch64.so.1 (browser/private_loader.cpp creates that link) and
#   RPATH $ORIGIN/lib:$ORIGIN/lib/glibc:$ORIGIN/lib/mesa.
set -euo pipefail
project=$(cd "$(dirname "$0")/.." && pwd)
BR_OUT=${BR_OUT:-$HOME/webos-browser-build/br-out-aarch64}
JOBS=${JOBS:-2}
out=$project/build/engine-aarch64
work=$out/work
build=$out/engine-build
stage=$project/build/engine-stage-aarch64/engine
mode=${1:-stage}

if [ ! -f "$BR_OUT/.config" ]; then
    echo "no Buildroot output at $BR_OUT (BR_OUT=... overrides)" >&2
    exit 1
fi
if ! grep -qx 'BR2_aarch64=y' "$BR_OUT/.config"; then
    echo "$BR_OUT is not an aarch64 Buildroot output" >&2
    exit 1
fi
TARGET=$BR_OUT/target
TC=$(ls -d "$BR_OUT"/host/aarch64-buildroot-linux-gnu/sysroot 2>/dev/null | head -n 1)
CROSS=$BR_OUT/host/bin/aarch64-buildroot-linux-gnu
PKGCONFIG=$BR_OUT/host/bin/pkg-config
READELF=$CROSS-readelf
# Buildroot >= 2026.08 has no staging/: packages install into target/, while the toolchain sysroot keeps the
# generated headers (glibconfig.h) and a copy of everything installed so far. pkg-config's sysroot must be the
# TOOLCHAIN sysroot (absolute -I/usr/... cflags are refused by the compiler wrapper), with target/ searched first so
# packages built after the toolchain (i.e. everything) come from the freshest tree.
pc_libdir="$TARGET/usr/lib/pkgconfig:$TARGET/usr/share/pkgconfig:$TC/usr/lib/pkgconfig:$TC/usr/share/pkgconfig"
[ -d "$BR_OUT/staging/usr/include" ] && pc_libdir="$BR_OUT/staging/usr/lib/pkgconfig:$BR_OUT/staging/usr/share/pkgconfig:$pc_libdir"
export PKG_CONFIG_SYSROOT_DIR=$TC PKG_CONFIG_LIBDIR=$pc_libdir
# The tree's host tools are linked against its privileged host libraries.
export LD_LIBRARY_PATH="$BR_OUT/host/lib:$BR_OUT/host/lib64:${LD_LIBRARY_PATH:-}"
# Buildroot's config.site names the build tools "aarch64-linux-" (not "<tuple>-"), and pkg-config substitutes that
# tuple into every package's tool variables: CMake's FindPkgConfig fails with "generate the toolchain by running
# config.site" before it looks at anything (glib-2.0 names gdbus-codegen). Wrap the real host tools.
mkdir -p "$work/tools"
for t in gdbus-codegen glib-mkenums glib-genmarshal gio-querymodules gdbus wayland-scanner; do
    real=$(command -v "$t" 2>/dev/null || true)
    [ -n "$real" ] || continue
    printf '#!/bin/sh\nexec "%s" "$@"\n' "$real" > "$work/tools/aarch64-linux-$t"
    chmod +x "$work/tools/aarch64-linux-$t"
done
export PATH=$work/tools:$PATH

pc_ver() { "$PKGCONFIG" --modversion "$1" 2>/dev/null || echo "MISSING"; }

echo "== aarch64 Buildroot output: $BR_OUT"
for p in wpe-webkit-2.0 wpe-platform-2.0 wpe-1.0 glib-2.0 gio-unix-2.0 gstreamer-1.0 gstreamer-base-1.0; do
    printf '   %-20s %s\n' "$p" "$(pc_ver "$p")"
done

# The GStreamer plugins the engine's package needs (tools/stage-engine.sh's required list, plus the aarch64
# specifics). An incomplete Buildroot run leaves the tree with 16 plugins and no qtdemux: a browser that plays
# nothing. Fail loudly here rather than stage that.
required_plugins="coreelements typefindfunctions playback app isomp4 matroska libav videoparsersbad audioparsers \
vpx opus pulseaudio soup adaptivedemux2 mpegtsdemux autodetect"
check_plugins() {
    local missing=() dir=$TARGET/usr/lib/gstreamer-1.0
    for p in $required_plugins; do
        [ -f "$dir/libgst$p.so" ] || missing+=("$p")
    done
    if [ ${#missing[@]} -gt 0 ]; then
        echo "missing GStreamer plugins in $dir: ${missing[*]}" >&2
        echo "   ($(ls "$dir" 2>/dev/null | wc -l) plugins present: $(ls "$dir" 2>/dev/null | sed 's/^libgst//;s/\.so$//' | tr '\n' ' '))" >&2
        echo "   the aarch64 Buildroot run is incomplete (no build/gst1-plugins-good-* or -bad-*):" >&2
        echo "   bash tools/build-engine-aarch64.sh start TARGETS=wpewebkit-depends   # finishes the tree" >&2
        return 1
    fi
    echo "   GStreamer plugins: $(ls "$dir" | wc -l) present, all $(echo $required_plugins | wc -w) required are there"
}
# Mesa is what WPEWebProcess draws with (no EGL in the engine: platform.cpp reports no EGL display); the 32-bit
# engine ships without it by default, the aarch64 one cannot. libEGL/libGLESv2 alone are useless: they dispatch to
# libgallium. libgallium embeds whichever Gallium drivers the defconfig enabled; with BR2_PACKAGE_MESA3D_LLVM off
# there is no libLLVM and softpipe is the rasteriser. engine64 ships llvmpipe; softpipe-only stays stageable.
check_mesa() {
    # Globs must be expanded by the shell, so they are written literally with the directory prefix.
    local found=0
    for p in "$TARGET/usr/lib"/libEGL.so.1 "$TARGET/usr/lib"/libGLESv2.so.2 "$TARGET/usr/lib"/libgallium-*.so \
             "$TARGET/usr/lib"/libgbm.so.1 "$TARGET/usr/lib"/libdrm.so.2; do
        [ -e "$p" ] || { echo "missing $(basename "$p") in target/usr/lib (BR2_PACKAGE_MESA3D*)" >&2; return 1; }
        found=1
    done
    # libLLVM is optional: present only when llvmpipe is enabled. libgallium must be able to load either way.
    [ "$found" = 1 ] || return 1
    if ls "$TARGET"/usr/lib/libLLVM.so.*[0-9] >/dev/null 2>&1; then
        echo "   Mesa: $(du -ch "$TARGET"/usr/lib/libgallium-*.so "$TARGET"/usr/lib/libLLVM.so.*[0-9] 2>/dev/null | tail -n 1 | cut -f1) (libgallium + libLLVM, llvmpipe)"
    else
        echo "   Mesa: $(du -sh "$TARGET"/usr/lib/libgallium-*.so 2>/dev/null | cut -f1) (libgallium, softpipe - no libLLVM)"
    fi
}
# libpulse comes from the TV (its PulseAudio 9 drops newer clients, the development notes (not in the repository)); bundling ours breaks sound silence-
# side, so make sure the tree's copy is NOT a dependency candidate for the engine's package.
check_pulse_not_bundled() {
    if [ -e "$TARGET/usr/lib/libpulse.so.0" ]; then
        echo "   note: target ships libpulse.so.0; the stage step must not copy it (the TV's own is used)"
    fi
}

if [ "$mode" = plugins ]; then
    check_plugins
    check_mesa
    check_pulse_not_bundled
    exit 0
fi

# preflight: everything that compiles without a WPE WebKit header. Real compiles, no stub headers.
if [ "$mode" = preflight ]; then
    rc=0
    echo "== preflight: common/ wire layout (static asserts, C and C++)"
    printf '#include "wpe_ipc.h"\n#include "wpe_media_ipc.h"\nint wpeb_ok;\n' > "$work/wire.c"
    cp "$work/wire.c" "$work/wire.cpp"
    "$CROSS"-gcc -std=c11 -Wall -Wextra -Werror -I"$project/common" -c "$work/wire.c" -o "$work/wire.o" \
        && echo "   OK   aarch64 C11: every IPC struct size/offset as on the 32-bit ABI"
    "$CROSS"-g++ -std=c++17 -Wall -Wextra -Werror -I"$project/common" -c "$work/wire.cpp" -o "$work/wirepp.o" \
        && echo "   OK   aarch64 C++17"
    echo "== preflight: engine/gst/wpebmediasink.c against aarch64 GStreamer (compile + link as a real plugin)"
    if "$CROSS"-gcc -std=gnu11 -O2 -fPIC -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
        -I"$project/common" $("$PKGCONFIG" --cflags gstreamer-1.0 gstreamer-base-1.0 gstreamer-audio-1.0 \
        gstreamer-video-1.0 gstreamer-pbutils-1.0) -c "$project/engine/gst/wpebmediasink.c" -o "$work/wpebmediasink.o"; then
        echo "   OK   compiled ($("$CROSS"-size "$work/wpebmediasink.o" | tail -n 1))"
        "$CROSS"-gcc -shared -Wl,-soname,libgstwpebmedia.so -o "$work/libgstwpebmedia.so" "$work/wpebmediasink.o" \
            $("$PKGCONFIG" --libs gstreamer-1.0 gstreamer-base-1.0 gstreamer-audio-1.0 gstreamer-video-1.0 \
            gstreamer-pbutils-1.0) && echo "   OK   linked $work/libgstwpebmedia.so"
        "$READELF" -h "$work/libgstwpebmedia.so" | grep -E 'Machine' | sed 's/^ */   /'
    else
        rc=1
    fi
    echo "== preflight: what still blocks the engine"
    for need in "usr/include/wpe-webkit-*:wpe/webkit.h (main.cpp, direct.cpp)" \
                "usr/lib/pkgconfig/wpe-platform-2.0.pc:wpe/wpe-platform.h (platform.cpp)"; do
        path=${need%%:*}; why=${need#*:}
        if ls -d "$TARGET"/$path >/dev/null 2>&1; then echo "   have  $path"; else echo "   NEED  $path -- $why"; fi
    done
    exit $rc
fi

# check: compile every engine source on its own and keep going, for a "how far does this get" report.
if [ "$mode" = check ]; then
    rc=0
    for f in "$project"/engine/src/*.cpp; do
        b=$(basename "$f" .cpp)
        if "$CROSS"-g++ -std=c++17 -fsyntax-only -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
            $("$PKGCONFIG" --cflags glib-2.0 gio-unix-2.0) -I"$project/common" -I"$project/engine/src" \
            -I"$project/engine/webos-backend" "$f" 2>"$work/$b.log"; then
            printf '   OK   src/%-12s %s warning(s)\n' "$b.cpp" "$(grep -c 'warning:' "$work/$b.log" || true)"
        else
            printf '   FAIL src/%-12s %s\n' "$b.cpp" "$(grep -m 1 'error:' "$work/$b.log" | cut -c1-140)"
            rc=1
        fi
    done
    exit $rc
fi

# ---- build ----
if ! "$PKGCONFIG" --exists wpe-webkit-2.0 wpe-platform-2.0; then
    cat >&2 <<'EOF'
blocked: WebKit 2.54.3 for aarch64 is not in this Buildroot output yet
   (needs usr/include/wpe-webkit-<ver>/, usr/lib/libWPEWebKit-2.0.so*, usr/lib/pkgconfig/wpe-webkit-2.0.pc,
    usr/lib/pkgconfig/wpe-platform-2.0.pc; build/wpewebkit-* is the build directory that will appear)
   finish it with: bash tools/build-engine-aarch64.sh start
   meanwhile: bash tools/engine-aarch64.sh preflight   (everything that does not need WebKit, really compiled)
EOF
    exit 1
fi

cmake=cmake
[ -x "$BR_OUT/host/bin/cmake" ] && cmake=$BR_OUT/host/bin/cmake
echo "== configure: $build"
# CMAKE_SYSROOT is deliberately NOT passed: Buildroot's toolchainfile.cmake sets it (toolchainfile.cmake:76-77) to
# the TOOLCHAIN sysroot, which holds the CRT objects (Scrt1.o, crti.o, ...) and the wpe-*.pc files. Passing $TARGET
# (the target rootfs) instead has the libraries but no CRT objects, so even CMake's compiler sanity check dies with
# "ld: cannot find Scrt1.o". CMAKE_FIND_ROOT_PATH still searches the target tree first so newly-built packages win.
"$cmake" -S "$project/engine" -B "$build" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$BR_OUT/host/share/buildroot/toolchainfile.cmake" \
    -DCMAKE_FIND_ROOT_PATH="$TARGET;$TC" \
    -DCMAKE_BUILD_TYPE=Release
echo "== build (jobs=$JOBS)"
CMAKE_BUILD_PARALLEL_LEVEL=$JOBS "$cmake" --build "$build"

[ "$mode" = build ] && exit 0

# ---- stage: the aarch64 counterpart of tools/stage-engine.sh (same layout, see that file) ----
echo "== stage: $stage"
check_plugins
check_mesa
check_pulse_not_bundled
libexec=$(ls -d "$TARGET"/usr/libexec/wpe-webkit-* 2>/dev/null | head -n 1)
[ -n "$libexec" ] || { echo "missing usr/libexec/wpe-webkit-* (WebKit not installed in the target tree)" >&2; exit 1; }
chmod -R u+w "$stage" 2>/dev/null || true
rm -rf "$stage"
mkdir -p "$stage/libexec" "$stage/lib/gio/modules" "$stage/lib/glibc" "$stage/lib/mesa" "$stage/lib/injected-bundle" \
         "$stage/lib/gstreamer-1.0" "$stage/etc/fonts" "$stage/etc/ssl/certs" "$stage/share"
cp "$build/wpe-engine" "$build/libgstwpebmedia.so" "$stage/" || { echo "the engine build left no wpe-engine or libgstwpebmedia.so in $build: nothing to stage" >&2; exit 1; }
cp "$libexec"/WPEWebProcess "$libexec"/WPENetworkProcess "$stage/libexec/"
[ -x "$libexec/WPEGPUProcess" ] && cp "$libexec/WPEGPUProcess" "$stage/libexec/"
cp "$TARGET"/usr/lib/gio/modules/libgiognutls.so "$stage/lib/gio/modules/"
cp "$(ls "$TARGET"/usr/lib/wpe-webkit-*/injected-bundle/libWPEInjectedBundle.so)" "$stage/lib/injected-bundle/"
mkdir -p "$stage/lib/gstreamer-1.0"
for plugin in "$TARGET"/usr/lib/gstreamer-1.0/*.so; do
    cp -L "$plugin" "$stage/lib/gstreamer-1.0/"
done
cp -f "$build/libgstwpebmedia.so" "$stage/lib/gstreamer-1.0/"   # the video sink the engine plays through: without it nothing plays
cp "$TARGET/usr/libexec/gstreamer-1.0/gst-plugin-scanner" "$stage/libexec/"
echo "== bundle the tree's glibc (the bridge's 2.39 cannot run these libraries: see REPORT.md §5)"
# libc & friends sit in <sysroot>/lib; libstdc++ is installed by gcc into <sysroot>/usr/lib, so both are searched.
find_glibc() {
    for d in "$TC/lib" "$TC/usr/lib" "$TARGET/lib" "$TARGET/usr/lib"; do
        [ -e "$d/$1" ] && { echo "$d/$1"; return 0; }
    done
    return 1
}
for so in ld-linux-aarch64.so.1 libc.so.6 libm.so.6 libmvec.so.1 libpthread.so.0 libdl.so.2 librt.so.1 \
          libresolv.so.2 libutil.so.1 libanl.so.1 libnss_files.so.2 libnss_dns.so.2 libgcc_s.so.1 libstdc++.so.6; do
    src=$(find_glibc "$so") || { echo "   ! $so not found in the tree" >&2; glibc_missing=1; continue; }
    cp -L "$src" "$stage/lib/glibc/"
done
[ "${glibc_missing:-0}" = 0 ] || { echo "the bundled glibc is incomplete: not staging" >&2; exit 1; }
[ -d "$TARGET/usr/lib/gconv" ] && cp -r "$TARGET/usr/lib/gconv" "$stage/lib/glibc/" && chmod -R u+w "$stage/lib/glibc/gconv"

# ---- Mesa: mandatory for aarch64 (unlike the 32-bit default, where the TV's Mali GPU is used) ----
# The engine presents through shared memory only (platform.cpp reports no EGL display), so WPEWebProcess composes
# with Mesa's surfaceless platform (llvmpipe/softpipe). libEGL.so.1 and libGLESv2.so.2 are thin dispatch libraries:
# doing anything needs libgallium-*.so, which needs libLLVM (84 MB on aarch64) -> ~100 MB. Staging only the two
# entry points leaves the web process unable to create an EGL display: no glReadPixels, no SHM frames, a blank page
# with no error. (The 32-bit stage-engine.sh has the same block as `lib/mesa/`, :170-176.)
STAGE_MESA=${STAGE_MESA:-1}
if [ "$STAGE_MESA" != 1 ]; then
    echo "   STAGE_MESA=$STAGE_MESA: NOT staging Mesa. The aarch64 engine presents through SHM only, so WPEWebProcess" >&2
    echo "   will have no EGL and the page will never be drawn. Only sensible with a GOPA/GPU path that does not exist." >&2
    exit 1
fi
echo "== Mesa (llvmpipe, surfaceless: the only renderer the aarch64 engine has)"
mesa_seeds=""
# The globs have to be expanded against the tree's directory, not the current one: expand "$TARGET/usr/lib"/<glob>,
# never a bare <glob> inside `[ -e "$TARGET/usr/lib/$f" ]` (that leaves $f literal and stages nothing).
for p in "$TARGET/usr/lib"/libEGL.so.1 "$TARGET/usr/lib"/libGLESv2.so.2 "$TARGET/usr/lib"/libgallium-*.so \
         "$TARGET/usr/lib"/libLLVM.so.* "$TARGET/usr/lib"/libglapi.so.* "$TARGET/usr/lib"/libgbm.so.1 \
         "$TARGET/usr/lib"/libdrm.so.2 "$TARGET/lib"/libgallium-*.so "$TARGET/lib"/libLLVM.so.* \
         "$TARGET/lib"/libEGL.so.1 "$TARGET/lib"/libGLESv2.so.2; do
    [ -e "$p" ] || continue
    # libLLVM's unversioned .so links and static archives are of no use at run time.
    case "$(basename "$p")" in libLLVM.so | libLLVM-*.so) continue ;; esac
    cp -L "$p" "$stage/lib/mesa/"
    mesa_seeds="$mesa_seeds $stage/lib/mesa/$(basename "$p")"
done
[ -n "$mesa_seeds" ] || { echo "no Mesa in target/usr/lib (BR2_PACKAGE_MESA3D*): cannot stage" >&2; exit 1; }
# The driver is dlopen()ed by name (libgallium-<ver>.so), so its SONAME resolution has to work. Its non-glibc
# dependencies (libexpat, libz, ...) are collected below with the rest of the closure; this only tots up what Mesa
# itself brings, so the ~100 MB is not a surprise.
echo "   Mesa: $(ls "$stage/lib/mesa" | wc -l) files, $(du -sh "$stage/lib/mesa" | cut -f1) (libLLVM is most of it)"

# ---- the rest of the DT_NEEDED closure, as stage-engine.sh's collect() does ----
find_lib() {
    for d in "$TARGET/usr/lib" "$TARGET/lib" "$TC/usr/lib" "$TC/lib"; do
        [ -e "$d/$1" ] && { echo "$d/$1"; return 0; }
    done
    return 1
}
# The TV provides its own PulseAudio (its 9.0 drops newer clients); bundling the tree's libpulse breaks sound.
SYSTEM_LIBS=' ld-linux-aarch64.so.1 libc.so.6 libm.so.6 libmvec.so.1 libpthread.so.0 libdl.so.2 librt.so.1
              libresolv.so.2 libutil.so.1 libanl.so.1 libnss_files.so.2 libnss_dns.so.2 libgcc_s.so.1
              libstdc++.so.6 libpulse.so.0 '
seen=' '
collect() {
    todo="$1"
    while [ -n "$todo" ]; do
        next=''
        for f in $todo; do
            for need in $("$READELF" -d "$f" 2>/dev/null | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p'); do
                case "$SYSTEM_LIBS" in *" $need "*) continue ;; esac
                case "$seen" in *" $need "*) continue ;; esac
                seen="$seen$need "
                # Mesa's libraries are already staged in lib/mesa/ (and lib/mesa precedes lib/ in RUNPATH). Copying
                # them into lib/ as well duplicated libLLVM (64 MB) + libgallium (17 MB) byte for byte: 81 MB wasted.
                if [ -f "$stage/lib/mesa/$need" ]; then
                    next="$next $stage/lib/mesa/$need"
                    continue
                fi
                path=$(find_lib "$need") || { echo "   ! $need (needed by $(basename "$f")) not in the tree" >&2; lib_missing=1; continue; }
                cp -L "$path" "$stage/lib/$need"
                next="$next $stage/lib/$need"
            done
        done
        todo=$next
    done
}
echo "== collect the libraries the engine and its helpers need (Mesa's own dependencies included: libexpat, libz, ...)"
collect "$stage/wpe-engine $stage/libexec/* $stage/lib/gio/modules/libgiognutls.so $stage/lib/injected-bundle/libWPEInjectedBundle.so $stage/lib/gstreamer-1.0/*.so $stage/libexec/gst-plugin-scanner $mesa_seeds"
[ "${lib_missing:-0}" = 0 ] || { echo "libraries the engine needs are missing from the tree (listed above): not staging a package that cannot start" >&2; exit 1; }
echo "   $(ls "$stage/lib"/*.so* 2>/dev/null | wc -l) libraries in lib/"

cp -r "$TARGET/usr/share/fonts" "$stage/share/"
cp "$TARGET/etc/ssl/certs/ca-certificates.crt" "$stage/etc/ssl/certs/" || { echo "no $TARGET/etc/ssl/certs/ca-certificates.crt: every https page would fail" >&2; exit 1; }

# ---- PT_INTERP: the TV has no /lib/ld-linux-aarch64.so.1 ----
# Everything the engine execs is linked against Buildroot's loader path, which does not exist on the TV, so each
# exec would fail with ENOENT. Point them at the same link in the app's runtime folder that the 32-bit engine uses
# (browser/private_loader.cpp): the UI points /tmp/io.github.mihai2mn.robin/ld-linux-aarch64.so.1 at our bundled
# lib/glibc/ld-linux-aarch64.so.1 (or at another loader, e.g. the bridge's, with our libc supplied through
# --library-path / LD_LIBRARY_PATH). patchelf is a Buildroot host package (BR2_PACKAGE_HOST_PATCHELF=y); it exists
# in host/bin once the tree has been built that far.
LOADER_LINK=${LOADER_LINK:-/tmp/io.github.mihai2mn.robin/ld-linux-aarch64.so.1}
if ! grep -qx 'BR2_PACKAGE_HOST_PATCHELF=y' "$BR_OUT/.config"; then
    echo "BR2_PACKAGE_HOST_PATCHELF is not enabled in $BR_OUT/.config; PT_INTERP cannot be set" >&2
    exit 1
fi
PATCHELF=$BR_OUT/host/bin/patchelf
[ -x "$PATCHELF" ] || {
    echo "missing $PATCHELF (a Buildroot host package: it appears once 'make host-patchelf' has run)" >&2
    echo "   build it with: bash tools/build-engine-aarch64.sh start TARGETS=host-patchelf" >&2
    exit 1
}
echo "== PT_INTERP -> $LOADER_LINK"
for exe in "$stage/wpe-engine" "$stage"/libexec/* "$stage/lib/gstreamer-1.0/libgstwpebmedia.so"; do
    [ -f "$exe" ] || continue
    # Shared objects have no PT_INTERP; only executables do.
    "$READELF" -l "$exe" 2>/dev/null | grep -q 'Requesting program interpreter' || continue
    "$PATCHELF" --set-interpreter "$LOADER_LINK" "$exe"
    # $ORIGIN is the directory the executable lives in, so a helper in libexec/ needs ../lib, not lib.
    # Using $ORIGIN/lib for everything made libexec/WPENetworkProcess look in libexec/lib (which does not exist)
    # and the network/web processes then failed with "libWPEWebKit-2.0.so.1: cannot open shared object file".
    case "$exe" in
        "$stage"/libexec/*) origin='$ORIGIN/../lib:$ORIGIN/../lib/glibc:$ORIGIN/../lib/mesa' ;;
        *)                  origin='$ORIGIN/lib:$ORIGIN/lib/glibc:$ORIGIN/lib/mesa' ;;
    esac
    # --force-rpath is required, not cosmetic. DT_RUNPATH is NOT transitive: when libWPEWebKit-2.0.so.1 needs
    # libatomic.so.1, the loader searches the RUNPATH of libWPEWebKit (which has none), NOT the executable's.
    # DT_RPATH *is* inherited by the whole closure, so the bundled lib/ is found for every level. Symptom without
    # it: helpers fail with "libatomic.so.1: cannot open shared object file" even though lib/libatomic.so.1 exists.
    "$PATCHELF" --force-rpath --set-rpath "$origin" "$exe"
done

# ---- RPATH on every bundled SHARED OBJECT, not just the executables ----
# A library that is loaded *dynamically* (libsoup/gnutls pulling in libbrotlidec, for example) is resolved against
# its OWN RPATH: the executing binary's RPATH is not consulted for a dlopen()-style load, and DT_RUNPATH is not
# transitive at all. Without this, WPENetworkProcess gets past libWPEWebKit and libatomic and then dies with
#   find library=libbrotlidec.so.1; search path=/lib64:/usr/lib64   (no RPATH line)
# even though lib/libbrotlidec.so.1 exists. So each .so gets a search path relative to its own directory.
#
# lib/glibc/ is deliberately EXCLUDED: those objects are the bundled loader, libc and its companions, and pointing
# them at the bundle's own lib/ risks resolving a second libc (or the loader's own deps) against the wrong copy.
# They are found through the loader's --library-path / the executable RPATH, which is how the loader itself works.
echo "== RPATH on bundled shared objects"
rpath_count=0
while IFS= read -r so; do
    # Only ELF shared objects; skip the glibc directory (see above) and the gstreamer scanner-style binaries.
    case "$so" in
        "$stage"/lib/glibc/*) continue ;;
    esac
    case "$(head -c4 "$so" 2>/dev/null | od -An -tx1 | tr -d ' \n')" in
        7f454c46) ;;
        *) continue ;;
    esac
    case "$so" in
        "$stage"/lib/mesa/*) so_origin='$ORIGIN:$ORIGIN/..:$ORIGIN/../lib:$ORIGIN/../lib/glibc' ;;
        "$stage"/lib/gstreamer-1.0/*) so_origin='$ORIGIN:$ORIGIN/..:$ORIGIN/../mesa:$ORIGIN/../glibc' ;;
        *) so_origin='$ORIGIN:$ORIGIN/..:$ORIGIN/mesa:$ORIGIN/glibc' ;;
    esac
    if "$PATCHELF" --force-rpath --set-rpath "$so_origin" "$so" 2>"$work/patchelf.err"; then rpath_count=$((rpath_count+1))
    else echo "   ! patchelf failed on ${so#$stage/}: $(head -1 "$work/patchelf.err")" >&2; rpath_failed=1; fi
done < <(find "$stage" -type f -name '*.so*' 2>/dev/null)
[ "${rpath_failed:-0}" = 0 ] || { echo "RPATH could not be set on every shared object: not staging" >&2; exit 1; }
echo "   RPATH set on $rpath_count shared objects (lib/glibc excluded)"

echo "== check: every executable loads through $LOADER_LINK and finds its libraries"
bad=0
for exe in "$stage/wpe-engine" "$stage"/libexec/*; do
    [ -f "$exe" ] || continue
    "$READELF" -l "$exe" 2>/dev/null | grep -q 'Requesting program interpreter' || continue
    i=$("$READELF" -l "$exe" | sed -n 's/.*Requesting program interpreter: \(.*\)\]/\1/p')
    [ "$i" = "$LOADER_LINK" ] || { echo "   ! $(basename "$exe"): interpreter is '$i'" >&2; bad=1; }
    for need in $("$READELF" -d "$exe" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p'); do
        [ -e "$stage/lib/$need" ] || [ -e "$stage/lib/glibc/$need" ] || { echo "   ! $(basename "$exe") needs $need, not staged" >&2; bad=1; }
    done
done
[ -e "$stage/lib/glibc/ld-linux-aarch64.so.1" ] || { echo "   ! lib/glibc/ld-linux-aarch64.so.1 missing" >&2; bad=1; }
for so in libc.so.6 libstdc++.so.6 libgcc_s.so.1; do
    [ -e "$stage/lib/glibc/$so" ] || { echo "   ! lib/glibc/$so missing" >&2; bad=1; }
done
[ -e "$stage/lib/mesa/libgallium-"[0-9]*.so ] || { echo "   ! lib/mesa/libgallium-*.so missing (no renderer)" >&2; bad=1; }
# libLLVM is only required when libgallium was built with llvmpipe; with softpipe-only Mesa it is absent by design.
if "$READELF" -d "$stage/lib/mesa/libgallium-"[0-9]*.so 2>/dev/null | grep -q 'libLLVM'; then
    [ -e "$stage/lib/mesa/libLLVM.so."* ] || { echo "   ! lib/mesa/libLLVM.so.* missing but libgallium needs it" >&2; bad=1; }
fi
[ "$bad" = 0 ] || exit 1

echo "== dependencies of the engine"
"$READELF" -d "$stage/wpe-engine" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/   \1/p'
echo "== staged: $(du -sh "$stage" | cut -f1) in $stage"
echo "   lib/glibc: $(du -sh "$stage/lib/glibc" | cut -f1)   lib/mesa: $(du -sh "$stage/lib/mesa" | cut -f1)"
