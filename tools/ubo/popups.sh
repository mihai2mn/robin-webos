#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# The "$popup" / "$popunder" lines of the filter lists, unchanged, into OUT (engine/ubo/popups.txt in the package):
# adblock-rust does not parse them (a window is judged in the engine's UI process: engine/src/popup_filter.h).
#   tools/ubo/popups.sh OUT_FILE LIST.txt...
set -eu
out=$1; shift
cat "$@" | tr -d '\r' | grep -E '^[^!#[][^#]*[$,](popup|popunder)([,=]|$)|^@@[^#]*[$,](popup|popunder)([,=]|$)' | sort -u > "$out"
echo "popups: $(wc -l < "$out") rule(s), $(wc -c < "$out") bytes -> $out"
