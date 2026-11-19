#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Desktop test of the uBO blocker with the system's WPE WebKit (x86_64): builds the extension for the host, the
# engine data with tools/ubo/test/test-filters.txt added, serves the test page from two origins and checks it
# with the blocker on, off, and allowed on the site; then opens a few real sites and reports what was blocked.
#   tools/ubo/test/run.sh            (after tools/ubo/build.sh data; needs wpewebkit, python3, cargo, node)
set -eu
SRC=$(cd "$(dirname "$0")/../../.." && pwd)
T=$SRC/tools/ubo/test
OUT=${OUT:-$SRC/build/ubo}
B=$OUT/test
mkdir -p "$B/ext" "$B/data"

sh "$SRC/tools/ubo/build.sh" lib host
cp "$OUT/$(rustc -vV | sed -n 's/^host: //p')/libwren-ubo.so" "$B/ext/"
cc -O1 -Wall -o "$B/harness" "$T/harness.c" $(pkg-config --cflags --libs wpe-webkit-2.0 wpe-platform-headless-2.0)

UBO=$SRC/tools/ubo
LISTS=$(python3 -c "
import json
for l in json.load(open('$UBO/lists/lists.json')):
    print('$UBO/lists/' + l['name'] + '.txt' + ''.join(':' + f for f in l['flags'].split(',') if f))")
# shellcheck disable=SC2086
cargo run --quiet --release --manifest-path "$SRC/engine/ubo/Cargo.toml" --features compiler \
    --target-dir "$OUT/cargo-host" --bin wren-ubo-compile -- \
    "$B/data" "$UBO/ubo/web_accessible_resources" "$UBO/ubo/js/redirect-resources.js" "$OUT/scriptlets.json" \
    $LISTS "$UBO/wren-filters.txt:trusted" "$T/test-filters.txt:trusted" 2>&1 | grep -E 'engine:|FAIL' || true
cp "$SRC/engine/ubo/cosmetic.js" "$B/data/"
rm -rf "$B/data/lists" && cp -r "$OUT/data/lists" "$B/data/"

# 127.0.0.1:8801 serves the page; localhost:8802 is the "third party" (ads/, redir/, img/, lib/).
mkdir -p "$B/www3/wren-test/ads" "$B/www3/wren-test/redir" "$B/www3/wren-test/img" "$B/www3/lib"
echo 'window.adRan = true;' > "$B/www3/wren-test/ads/adscript.js"
echo 'window.redirRealRan = true;' > "$B/www3/wren-test/redir/stub.js"
echo 'window.okRan = true;' > "$B/www3/lib/ok.js"
echo 'echo' > "$B/www3/lib/echo.txt"
printf 'GIF89a\001\000\001\000\200\000\000\377\377\377\000\000\000!\371\004\001\000\000\000\000,\000\000\000\000\001\000\001\000\000\002\002D\001\000;' > "$B/www3/wren-test/img/pixel.gif"
python3 -m http.server 8801 --bind 127.0.0.1 --directory "$T/www" >/dev/null 2>&1 &
P1=$!
python3 -m http.server 8802 --bind 127.0.0.1 --directory "$B/www3" 2> "$B/third-party.log" &
P2=$!
trap 'kill $P1 $P2 2>/dev/null' EXIT
sleep 1

CHECK='({
  scriptlet_set_constant: seenConst === true && wrenTestConst === true,
  scriptlet_trusted: seenTrusted === 42,
  network_blocked: window.adRan !== true,
  redirect_neutered: window.redirRealRan !== true && window.redirOnload === true,
  image_redirect_loads: window.pixelOnload === true,
  allowed_script_runs: window.okRan === true,
  hide_specific: hidden("specific"),
  hide_generic_class: hidden("generic"),
  hide_generic_id: hidden("ad_banner"),
  hide_generic_late: hidden("late-generic"),
  procedural_has_text: hidden("proc"),
  procedural_keeps_other: hidden("proc-keep") === false,
  procedural_upward: hidden("upward"),
  normal_visible: hidden("normal") === false,
  fetch_ok: window.echo === "opaque",
  gif_videos_as_images: gifImages() === "https://i.redd.it/wrentest123.gif https://i.giphy.com/WrenGiphy42.webp" && gifVideosLeft() === 0,
  wren_styles: document.querySelectorAll("style[data-wren]").length,
  wren_css: ((document.querySelector("style[data-wren]") || {}).textContent || "").slice(0, 400)
})'
# nip.io resolves to 127.0.0.1; EasyList turns generic hiding off on 127.0.0.1 and localhost themselves.
PAGE=http://wrentest.127.0.0.1.nip.io:8801/page.html
export WREN_UBO_LOG=${WREN_UBO_LOG:-1}
echo "== blocker on"
timeout 60 "$B/harness" "$B/data" "$B/ext" 1500 "$PAGE" "$CHECK"
grep -o 'GET /lib/echo.txt[^ ]*' "$B/third-party.log" | tail -1 | sed 's/^/removeparam: /'
echo "== blocker on, with every extra list on by default"
DEFAULT_LISTS=$(awk -F'[][]' '/^\[/ {n = $2} /^default=true/ {print n}' "$B/data/lists/catalog.ini" | paste -sd,)
TEST_LISTS=$DEFAULT_LISTS timeout 60 "$B/harness" "$B/data" "$B/ext" 1500 "$PAGE" "$CHECK" 2>&1 | grep -E "RESULT|loaded in|not loaded"
echo "== blocker off"
TEST_ADS_OFF=1 timeout 60 "$B/harness" "$B/data" "$B/ext" 1500 "$PAGE" "$CHECK"
echo "== ads allowed on the test host"
TEST_ALLOWED=wrentest.127.0.0.1.nip.io timeout 60 "$B/harness" "$B/data" "$B/ext" 1500 "$PAGE" "$CHECK"

if [ "${REAL_SITES:-1}" = 1 ]; then
    echo "== real sites (blocked requests counted from the extension's log)"
    for url in https://d3ward.github.io/toolz/adblock https://www.speedtest.net/ https://www.theguardian.com/international https://www.accuweather.com/; do
        WREN_UBO_LOG=1 timeout 60 "$B/harness" "$B/data" "$B/ext" 8000 "$url" \
            '({title: document.title.slice(0, 60), score: (document.querySelector("#adb_test_r") || {}).textContent || null, hidden_by_wren: document.querySelectorAll("style[data-wren]").length})' \
            > "$B/real.out" 2> "$B/real.err" || true
        printf '%s  blocked=%s redirected=%s rewritten=%s  %s\n' "$url" \
            "$(grep -c 'wren-ubo: blocked' "$B/real.err")" "$(grep -c 'wren-ubo: redirected' "$B/real.err")" \
            "$(grep -c 'wren-ubo: rewritten' "$B/real.err")" "$(grep RESULT "$B/real.out" | cut -c8-)"
    done
fi
