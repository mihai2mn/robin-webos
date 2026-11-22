#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Packs the staged aarch64 engine (tools/engine-aarch64.sh stage -> build/engine-stage-aarch64/engine/) into an
# ipk that adds `engine64/` next to the existing 32-bit `engine/` in the installed Wren app, so the official 0.3.3
# UI can launch it (browser/ is NOT modified: step 4 of the plan is cancelled).
#
#   bash tools/pack-engine64.sh ipk                  add-on ipk: engine64/ only, nothing else changes
#   bash tools/pack-engine64.sh replace              replace variant: engine/wpe-engine -> the launcher, engine/ ->
#                                                    engine32/ (one-at-a-time: the 32-bit and 64-bit engines do not
#                                                    fit together, see docs/engine64.md)
#   bash tools/pack-engine64.sh study                size table + strip savings of the staged tree (no ipk)
#   bash tools/pack-engine64.sh report               what is staged and what it would add to the TV
#   STAGE=...  LAUNCHER=...  OUT=...  VERSION=0.3.3-ubo3  DEVICE_NAME=/mnt/lg/appstore ...
#
# The launcher (engine/launcher/wpe-launcher.c, tools/build-launcher.sh) is a STATIC 32-bit ARM binary installed as
# <app>/engine/wpe-engine; it execs `<app>/engine64/lib/glibc/ld-linux-aarch64.so.1 --library-path
# <app>/engine64/lib:<app>/engine64/lib/glibc:<app>/engine64/lib/mesa <app>/engine64/wpe-engine`, and falls back to
# <app>/engine/wpe-engine32. Build it with: bash tools/build-launcher.sh  (output build/launcher/wpe-engine).
#
# The launcher invokes the loader EXPLICITLY, so the 64-bit engine's PT_INTERP does not matter at run time (it still
# points at the app-private link, which is harmless: it is never used). What must exist is exactly the layout above.
#
# What an ipk is (verified against dist/org.sostk.wpebrowser_0.3.3-ubo3_arm.ipk): an `ar` archive of
#   debian-binary ("2.0\n"), control.tar.gz (control), data.tar.gz (paths relative to /)
# This script writes exactly that with ar + tar, so no webos tooling (ares-package/ipkg-build) is needed.
#
# Sizes matter: the TV's /media/developer is 2,866,224K with 2,432,800K used -- 443 MB free (measured 2026-11-19).
# The 32-bit engine tree is 220.8 MB, engine64 is expected 180-260 MB: they cannot coexist comfortably, hence the
# two modes and the size study.
set -euo pipefail
project=$(cd "$(dirname "$0")/.." && pwd)
mode=${1:-report}
stage=${STAGE:-$project/build/engine-stage-aarch64/engine}
launcher=${LAUNCHER:-$project/build/launcher/wpe-engine}
out=${OUT:-$project/dist}
version=${VERSION:-0.3.3-ubo3}
app=${APP_DIR:-/usr/palm/applications/org.sostk.wpebrowser}
tv_free_mb=${TV_FREE_MB:-443}        # free MB on /media/developer, measured on the TV 2026-11-19
tv_app_mb=${TV_APP_MB:-221}         # the installed engine/ tree (220.8 MB); the app itself is ~5 MB more
STRIP=${STRIP:-$HOME/webos-browser-build/br-out-aarch64/host/bin/aarch64-buildroot-linux-gnu-strip}

mb() { awk -v x="$1" 'BEGIN{printf "%.1f", x/1048576}'; }
du_mb() { du -sm "$1" 2>/dev/null | cut -f1; }

if [ ! -d "$stage" ]; then
    cat >&2 <<EOF
no staged engine at $stage
   build it first:  bash tools/engine-aarch64.sh          (needs WebKit 2.54.3 aarch64 in br-out-aarch64)
   meanwhile:       bash tools/pack-engine64.sh study     (works on whatever is staged, or on nothing)
EOF
    [ "$mode" = study ] || [ "$mode" = report ] || exit 1
fi

# ---- study: what dominates, what stripping saves, and the llvmpipe/LLVM lever ------------------------------
# libgallium has a hard DT_NEEDED on libLLVM, so libLLVM can never be "stripped away": it goes only by rebuilding
# Mesa without llvmpipe. softpipe (already enabled in the defconfig) stays as the software driver.
if [ "$mode" = study ]; then
    mesa_dir=$stage/lib/mesa
    # Where the Mesa numbers come from: the staged tree if it has lib/mesa, else the Buildroot target tree.
    if [ ! -d "$mesa_dir" ]; then
        mesa_dir=${TARGET_LIB:-$HOME/webos-browser-build/br-out-aarch64/target/usr/lib}
    fi
    echo "== Mesa set in $mesa_dir"
    mesa_raw=0; mesa_strip=0
    tmp=$(mktemp -d)
    for f in "$mesa_dir"/libEGL.so.1 "$mesa_dir"/libGLESv2.so.2 "$mesa_dir"/libgallium-*.so \
             "$mesa_dir"/libLLVM.so.*[0-9] "$mesa_dir"/libgbm.so.1 "$mesa_dir"/libdrm.so.2; do
        [ -e "$f" ] || continue
        cp -f "$f" "$tmp/x"
        before=$(stat -c %s "$tmp/x")
        "$STRIP" --strip-unneeded "$tmp/x" 2>/dev/null || true
        now=$(stat -c %s "$tmp/x")
        mesa_raw=$((mesa_raw+before)); mesa_strip=$((mesa_strip+now))
        printf '   %-28s %7s MB -> %7s MB\n' "$(basename "$f")" "$(mb $before)" "$(mb $now)"
    done
    rm -rf "$tmp"; tmp=$(mktemp -d)
    printf '   %-28s %7s MB -> %7s MB  (saves %s MB)\n' TOTAL "$(mb $mesa_raw)" "$(mb $mesa_strip)" \
        "$(mb $((mesa_raw-mesa_strip)))"
    echo
    # The lever: is llvmpipe in this build? libLLVM is only needed by it.
    if ls "$mesa_dir"/libLLVM.so.2* >/dev/null 2>&1; then
        llvm=$(ls "$mesa_dir"/libLLVM.so.2* 2>/dev/null | head -n 1)
        cp -f "$llvm" "$tmp/x"; lraw=$(stat -c %s "$tmp/x"); "$STRIP" --strip-unneeded "$tmp/x" 2>/dev/null || true; lstrip=$(stat -c %s "$tmp/x")
        rm -rf "$tmp"
        echo "== BIGGEST LEVER: rebuild Mesa without llvmpipe (drop libLLVM entirely)"
        echo "   libLLVM.so alone: $(mb $lraw) MB raw / $(mb $lstrip) MB stripped"
        echo "   Mesa would go from $(mb $mesa_strip) MB stripped to roughly 18 MB (softpipe only, estimate)"
        echo "   -> saves ~$(awk -v a="$mesa_strip" 'BEGIN{printf "%.0f", a/1048576-18}') MB"
        brcfg=${BR_OUT:-$HOME/webos-browser-build/br-out-aarch64}/.config
        if [ -f "$brcfg" ] && grep -qx 'BR2_PACKAGE_MESA3D_LLVM=y' "$brcfg"; then
            echo "   RECOMMENDED: set in $brcfg (then rebuild mesa3d + restage):"
            echo "     # BR2_PACKAGE_MESA3D_LLVM is not set"
            echo "     # BR2_PACKAGE_MESA3D_GALLIUM_DRIVER_LLVMPIPE is not set"
            echo "     BR2_PACKAGE_MESA3D_GALLIUM_DRIVER_SOFTPIPE=y      (already set)"
            echo "   UNVERIFIED: softpipe must still give WPEWebProcess an EGL display + glReadPixels (see PACKAGING.md)."
        else
            echo "   ($brcfg has no BR2_PACKAGE_MESA3D_LLVM=y: Mesa is already llvmpipe-free)"
        fi
    else
        echo "== no libLLVM in the Mesa set: this build is already softpipe-only"
    fi
    echo
    echo "== staged tree: $stage"
    [ -d "$stage" ] && du -sh "$stage" 2>/dev/null
    echo
    echo "== per-directory (raw):"
    if [ -d "$stage" ]; then
        du -sh "$stage"/* 2>/dev/null | sort -h | awk '{printf "   %8s  %s\n", $1, $2}'
        echo
        echo "== biggest files:"
        find "$stage" -type f -printf '%s %p\n' 2>/dev/null | sort -rn | head -n 12 |
            awk '{printf "   %8.1f MB  %s\n", $1/1048576, $2}'
        echo
        echo "== strip savings of the whole tree (--strip-unneeded):"
        tmp=$(mktemp -d)
        tot=0; after=0
        find "$stage" -type f -print0 2>/dev/null | while IFS= read -r -d '' f; do
            case "$f" in *.dat | *.ini | *.txt | *.conf | *.json | *.css | *.js) continue ;; esac
            cp "$f" "$tmp/x" 2>/dev/null || continue
            before=$(stat -c %s "$tmp/x")
            "$STRIP" --strip-unneeded "$tmp/x" 2>/dev/null || true
            now=$(stat -c %s "$tmp/x")
            echo "$before $now"
        done | awk '{b+=$1; a+=$2} END{printf "   before %s MB -> after %s MB (saves %s MB, %.0f%%)\n", \
                b/1048576, a/1048576, (b-a)/1048576, (b>0? (b-a)*100/b : 0)}'
        rm -rf "$tmp"
    fi
    echo
    echo "== space on /media/developer (443 MB free measured 2026-11-19; app+32-bit engine $tv_app_mb MB):"
    if [ -d "$stage" ]; then
        st=$(du -sm "$stage" | cut -f1)
        echo "   alongside the 32-bit engine : engine64 $st MB -> $((tv_free_mb - st)) MB free afterwards"
        [ $((tv_free_mb - st)) -lt 0 ] && echo "     -> does NOT fit (use the replace mode)"
    fi
    echo "   replace the 32-bit engine  : engine64 ~$(( ${ENGINE64_MB:-190} )) MB -> ~$((tv_free_mb - ${ENGINE64_MB:-190} )) MB free"
    exit 0
fi

# ---- report: what would be packed -------------------------------------------------------------------------
if [ "$mode" = report ]; then
    echo "== staged engine: $stage ($(du_mb "$stage") MB)"
    for d in lib lib/mesa lib/glibc libexec lib/gstreamer-1.0 share etc bin; do
        [ -d "$stage/$d" ] && printf '   %-24s %5s MB\n' "$d/" "$(du_mb "$stage/$d")"
    done
    echo "== launcher: $launcher $([ -f "$launcher" ] && echo "($(mb "$(stat -c %s "$launcher")") MB, static 32-bit ARM)" || echo "(MISSING: bash tools/build-launcher.sh)")"
    echo "== targets: $out/org.sostk.wpebrowser_$version-engine64_arm.ipk (add-on)"
    echo "            $out/org.sostk.wpebrowser_$version-engine64-replace_arm.ipk (swap)"
    exit 0
fi

if [ ! -d "$stage" ]; then echo "nothing staged at $stage" >&2; exit 1; fi
mkdir -p "$out"

# ---- common: lay out the data tree ------------------------------------------------------------------------
# engine64/lib/mesa stays a SEPARATE directory and lib/glibc stays separate from lib/: the launcher passes
#   --library-path <app>/engine64/lib:<app>/engine64/lib/glibc:<app>/engine64/lib/mesa
# (engine/launcher/wpe-launcher.c). Merging any of them into lib/ would still work at run time, but it would hide
# which library a given name resolves from and make the Mesa-on/off switch (STAGE_MESA) meaningless.
check_layout() {
    local s=$1 bad=0
    for f in wpe-engine lib/glibc/ld-linux-aarch64.so.1 lib/mesa/libEGL.so.1; do
        [ -e "$s/$f" ] || { echo "   ! missing $f (the launcher execs it: engine/launcher/wpe-launcher.c)" >&2; bad=1; }
    done
    for f in libexec/WPEWebProcess libexec/WPENetworkProcess libexec/gst-plugin-scanner; do
        [ -x "$s/$f" ] || { echo "   ! missing or not executable: $f" >&2; bad=1; }
    done
    for f in libc.so.6 libLLVM.so.23.1 libgallium-26.2.5.so; do
        # libc belongs in lib/glibc, the Mesa pieces in lib/mesa: seeing them in lib/ means the staging is mixed up.
        case "$f" in
        libc.so.6) [ -e "$s/lib/$f" ] && { echo "   ! $f in lib/: belongs in lib/glibc" >&2; bad=1; } ;;
        *) [ -e "$s/lib/$f" ] && { echo "   ! $f in lib/: belongs in lib/mesa" >&2; bad=1; } ;;
        esac
    done
    ls "$s"/lib/mesa/libgallium-*.so >/dev/null 2>&1 || { echo "   ! no lib/mesa/libgallium-*.so (no renderer)" >&2; bad=1; }
    return $bad
}
lay_out_engine64() {
    local root=$1
    rm -rf "$root"
    mkdir -p "$root$app"
    cp -a "$stage" "$root$app/engine64"
    # Run-time noise the TV does not need, and the aarch64 gconv modules nobody asks for (the 32-bit package ships
    # 6.3 MB of softfp gconv because its glibc is only used on old TVs; aarch64 has no such TVs).
    rm -rf "$root$app/engine64/licenses" 2>/dev/null || true
    if [ -d "$root$app/engine64/lib/glibc/gconv" ]; then
        echo "   dropping lib/glibc/gconv ($(du_mb "$root$app/engine64/lib/glibc/gconv") MB): UTF-8/ASCII only sites"
        rm -rf "$root$app/engine64/lib/glibc/gconv"
    fi
    check_layout "$root$app/engine64" || {
        echo "the staged tree does not match the launcher's layout; run 'bash tools/engine-aarch64.sh' again" >&2
        rm -rf "$root"
        exit 1
    }
    # Strip everything: the tree keeps .symtab/.strtab (16 MB on libLLVM alone).
    find "$root$app/engine64" -type f \( -name '*.so' -o -name '*.so.*' -o -perm -u+x \) 2>/dev/null | while read -r f; do
        case "$f" in *.dat | *.conf | *.json | *.txt) continue ;; esac
        "$STRIP" --strip-unneeded "$f" 2>/dev/null || true
    done
}

# mkipk <root-dir> <output.ipk> <Package> <Version> <Installed-Size-KB>
mkipk() {
    local root=$1 outfile=$2 pkg=$3 ver=$4 sizekb=$5 work
    work=$(mktemp -d)
    ( cd "$root" && tar czf "$work/data.tar.gz" --owner=0 --group=0 . )
    mkdir -p "$work/ctl"
    cat > "$work/ctl/control" <<EOF
Package: $pkg
Version: $ver
Section: misc
Priority: optional
Architecture: arm
Installed-Size: $sizekb
Maintainer: webos-native-app-kit <noreply@example.org>
License: GPL-3.0-or-later
Description: Wren $pkg
webOS-Package-Format-Version: 2
webOS-Packager-Version: wren-engine64/1
EOF
    ( cd "$work/ctl" && tar czf "$work/control.tar.gz" --owner=0 --group=0 control )
    printf '2.0\n' > "$work/debian-binary"
    rm -f "$outfile"
    ( cd "$work" && ar rc "$outfile" debian-binary control.tar.gz data.tar.gz && ranlib "$outfile" 2>/dev/null || true )
    rm -rf "$work"
    echo "   $outfile ($(mb "$(stat -c %s "$outfile")") MB)"
}

# ---- add-on: engine64/ beside the existing engine/ ---------------------------------------------------------
if [ "$mode" = ipk ]; then
    echo "== add-on ipk: engine64/ next to the 32-bit engine/"
    root=$(mktemp -d)
    lay_out_engine64 "$root"
    sz=$(du -sm "$root$app/engine64" | cut -f1)
    echo "   engine64: $sz MB"
    left=$((tv_free_mb - sz))
    if [ "$left" -lt 40 ]; then
        echo "   ! only $left MB would be left on /media/developer after installing (443 MB free now)." >&2
        echo "     The 32-bit engine ($tv_app_mb MB with the app) stays where it is: consider the 'replace' mode." >&2
    fi
    mkipk "$root" "$out/org.sostk.wpebrowser_${version}-engine64_arm.ipk" "org.sostk.wpebrowser.engine64" "$version" $((sz*1024))
    rm -rf "$root"
    echo "   install: ares-install -d <tv> $out/org.sostk.wpebrowser_${version}-engine64_arm.ipk"
    echo "            (or: scp to /media/developer/temp/ and ipkg install)"
    exit 0
fi

# ---- replace: engine/wpe-engine -> the launcher, old engine -> engine/wpe-engine32 ---------------------------
# The launcher's contract (engine/launcher/wpe-launcher.c): it execs the 64-bit engine if it can, and falls back to
# <app>/engine/wpe-engine32. So the 32-bit engine must be RENAMED, which an ipk cannot do across packages: this
# package ships the launcher, engine64/ AND a copy of the 32-bit files it needs (--with-engine32), or, when the 32-bit
# files are not at hand, only the launcher + engine64 and the install steps do the rename (PACKAGING.md).
if [ "$mode" = replace ]; then
    if [ ! -f "$launcher" ]; then
        echo "missing launcher $launcher" >&2
        echo "   build it: bash tools/build-launcher.sh   (static 32-bit ARM, installed as engine/wpe-engine)" >&2
        exit 1
    fi
    engine32=${ENGINE32:-}
    echo "== replace ipk: engine/wpe-engine -> launcher (+ engine32/ + engine64/)"
    root=$(mktemp -d)
    lay_out_engine64 "$root"
    mkdir -p "$root$app/engine"
    cp "$launcher" "$root$app/engine/wpe-engine"
    chmod 0755 "$root$app/engine/wpe-engine"
    if [ -n "$engine32" ] && [ -d "$engine32" ]; then
        # The installed 32-bit engine tree, copied in as engine32/: it keeps its own lib/, libexec/, ubo/ layout, so
        # its own loader link (/tmp/org.sostk.wpebrowser/ld-linux.so.3 -> engine32/lib/glibc) still works from there.
        echo "   copying the 32-bit engine from $engine32 ($(du_mb "$engine32") MB) as engine/wpe-engine32/..."
        mkdir -p "$root$app/engine/wpe-engine32"
        cp -a "$engine32"/. "$root$app/engine/wpe-engine32/"
    else
        echo "   ENGINE32=... not given: the 32-bit engine is NOT in this package."
        echo "   The install steps must rename the installed engine first (PACKAGING.md, step 3)."
    fi
    cat > "$root$app/engine/ENGINE64" <<'EOF'
engine/wpe-engine is the aarch64 launcher (tools/pack-engine64.sh replace).
It runs the 64-bit engine from ../engine64/ and falls back to wpe-engine32/ (or the legacy .packages engine).
EOF
    sz=$(du -sm "$root$app/engine" | cut -f1)
    lsz=$(mb "$(stat -c %s "$root$app/engine/wpe-engine")")
    echo "   engine/ (launcher + engine32 + engine64): $sz MB, launcher: $lsz MB"
    mkipk "$root" "$out/org.sostk.wpebrowser_${version}-engine64-replace_arm.ipk" "org.sostk.wpebrowser.engine64" "$version" $((sz*1024))
    rm -rf "$root"
    echo "   install: ares-install -d <tv> $out/org.sostk.wpebrowser_${version}-engine64-replace_arm.ipk"
    exit 0
fi

echo "usage: bash tools/pack-engine64.sh ipk|replace|study|report" >&2
exit 2
