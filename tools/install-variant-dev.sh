#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Put a WebKit variant (tools/build-wk-variant.sh output) and, optionally, the -O2 engine wrapper and video sink on the DEV app
# on the TV, or put the stock files back. DEV APP ONLY (the app id is fixed here). The caller holds the TV lock
# (/tmp/wren-devapp.lock, this script checks that its owner is $OWNER). The files it replaces are saved once as <name>.stock
# beside them, so "restore" is exact and installing a second variant never loses the stock files.
#   OWNER=wren-build bash tools/install-variant-dev.sh install <variant-out-dir> [<dir with wpe-engine32 and libgstwpebmedia.so>]
#   OWNER=wren-build bash tools/install-variant-dev.sh restore
# Host: WSL/Linux with patchelf (Buildroot's host tools), ssh host "lgtv". Run on the TV: nothing but cp/mv.
set -euo pipefail
APP=org.sostk.wpebrowser.dev; E=/media/developer/apps/usr/palm/applications/$APP/engine
OWNER=${OWNER:?OWNER=<the name that holds the lock>}
PATCHELF=${PATCHELF:-$HOME/webos-browser-build/br-out-aarch64/host/bin/patchelf}
tv() { timeout "${1:-60}" ssh -o BatchMode=yes -o ConnectTimeout=25 lgtv "$2"; }
[ "$(tv 20 "head -1 /tmp/wren-devapp.lock/owner")" = "$OWNER" ] || { echo "the TV lock is not held by $OWNER" >&2; exit 2; }
FILES="lib/libWPEWebKit-2.0.so.1 libexec/WPEWebProcess libexec/WPENetworkProcess libexec/WPEGPUProcess lib/injected-bundle/libWPEInjectedBundle.so wpe-engine32 lib/gstreamer-1.0/libgstwpebmedia.so"
case "${1:-}" in
  install)
    v=${2:?variant out dir}; x=${3:-}
    stage=$(mktemp -d); mkdir -p "$stage/libexec" "$stage/lib/injected-bundle" "$stage/lib/gstreamer-1.0"
    cp "$v"/engine/lib/libWPEWebKit-2.0.so.1 "$stage/lib/"; cp "$v"/engine/libexec/* "$stage/libexec/"; cp "$v"/engine/lib/injected-bundle/* "$stage/lib/injected-bundle/"
    if [ -n "$x" ]; then cp "$x/wpe-engine32" "$stage/"; cp "$x/libgstwpebmedia.so" "$stage/lib/gstreamer-1.0/"; fi
    for f in "$stage"/libexec/* "$stage/wpe-engine32"; do [ -f "$f" ] && "$PATCHELF" --set-interpreter /tmp/$APP/ld-linux.so.3 "$f"; done
    tv 30 "tv close $APP >/dev/null 2>&1; true" || true
    tar c -C "$stage" . | tv 300 "mkdir -p /tmp/wrenvar && rm -rf /tmp/wrenvar/* && tar x -C /tmp/wrenvar"
    tv 120 "cd /tmp/wrenvar; for f in \$(find . -type f | sed 's#^\./##'); do
        [ -e $E/\$f.stock ] || cp -a $E/\$f $E/\$f.stock
        cp /tmp/wrenvar/\$f $E/\$f.new && chmod 755 $E/\$f.new && mv $E/\$f.new $E/\$f && echo installed \$f \$(stat -c %s $E/\$f)
      done; rm -rf /tmp/wrenvar"
    rm -rf "$stage" ;;
  restore)
    tv 30 "tv close $APP >/dev/null 2>&1; true" || true
    tv 120 "cd $E; for f in $FILES; do [ -e \$f.stock ] && { cp -a \$f.stock \$f.new && mv \$f.new \$f && echo restored \$f; }; done" ;;
  *) sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
esac
