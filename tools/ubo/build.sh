#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds the uBO-grade ad blocker (engine/ubo) into OUT (default build/ubo):
#   OUT/data/      engine.dat, resources.json, cosmetic.js  -> the package's engine/ubo/
#   OUT/<target>/  libwren-ubo.so                           -> the package's engine/lib/web-extensions/
#
#   tools/ubo/build.sh data            lists + uBO resources -> data/ (needs cargo and node on the build host)
#   tools/ubo/build.sh lib [TARGET]    the web process extension; TARGET is a Rust target triple:
#                                      armv7-unknown-linux-gnueabi (default: the TV's softfp userland) or "host"
# Environment for `lib`:
#   CC          C compiler (and linker) for TARGET. Default for the TV: zig cc for glibc 2.28, soft-float calls
#               (compatible with the TV's softfp libraries). With Buildroot: its arm-...-gcc.
#   EXT_CFLAGS  include flags for WPE WebKit's web process extension API, GLib, libsoup 3 and JSC for TARGET.
#               Default: pkg-config wpe-web-process-extension-2.0 libsoup-3.0 (set PKG_CONFIG_* for a sysroot).
# The extension links to nothing: WebKit, GLib and libsoup are already in the web process that loads it.
set -eu
SRC=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${OUT:-$SRC/build/ubo}
UBO=$SRC/tools/ubo
mode=${1:-}

case "$mode" in
data)
    [ -f "$UBO/lists/lists.json" ] && [ -f "$UBO/lists/optional.json" ] && [ -f "$UBO/ubo/COMMIT" ] ||
        python3 "$UBO/update.py" $(cat "$UBO/ubo/COMMIT" 2>/dev/null)
    rm -rf "$OUT/data" && mkdir -p "$OUT/data/lists"
    node "$UBO/scriptlets-to-json.mjs" "$UBO/ubo/js/resources/scriptlets.js" "$OUT/scriptlets.json"
    LISTS=$(python3 -c '
import json, os, sys
ubo = sys.argv[1]
for l in json.load(open(os.path.join(ubo, "lists", "lists.json"))):
    print(os.path.join(ubo, "lists", l["name"] + ".txt") + "".join(":" + f for f in l["flags"].split(",") if f))' "$UBO")
    # shellcheck disable=SC2086
    cargo run --quiet --release --manifest-path "$SRC/engine/ubo/Cargo.toml" --features compiler \
        --target-dir "$OUT/cargo-host" --bin wren-ubo-compile -- \
        "$OUT/data" "$UBO/ubo/web_accessible_resources" "$UBO/ubo/js/redirect-resources.js" "$OUT/scriptlets.json" $LISTS "$UBO/wren-filters.txt:trusted"
    cp "$SRC/engine/ubo/cosmetic.js" "$SRC/engine/ubo/compat.js" "$OUT/data/"
    # Notices that travel with the compiled lists and the Rust crates (the package puts them in licenses/ad-blocker/).
    mkdir -p "$OUT/data/licences"
    cp "$SRC/engine/ubo/NOTICES.txt" "$SRC/engine/ubo/THIRD-PARTY-CRATES.txt" "$UBO/lists/LICENCES.txt" "$UBO/ubo/LICENSE.txt" "$OUT/data/licences/"
    # The windows' own filters ($popup, $popunder): adblock-rust leaves them out, the engine matches them (engine/src/popup_filter.h).
    sh "$UBO/popups.sh" "$OUT/data/popups.txt" "$UBO"/lists/*.txt "$UBO/wren-filters.txt"
    # Extra lists: one engine each, with uBO's Unbreak and Quick fixes so their exceptions apply inside it, and
    # lists/catalog.ini for the engine's settings page (title, what it does, on by default, size).
    python3 -c '
import json, os, sys
ubo = sys.argv[1]
for l in json.load(open(os.path.join(ubo, "lists", "optional.json"))):
    print(l["name"], os.path.join(ubo, "lists", l["name"] + ".txt") + "".join(":" + f for f in l["flags"].split(",") if f))' "$UBO" |
    while read -r name spec; do
        cargo run --quiet --release --manifest-path "$SRC/engine/ubo/Cargo.toml" --features compiler \
            --target-dir "$OUT/cargo-host" --bin wren-ubo-compile -- --list-engine "$OUT/data/lists/$name.dat" \
            "$spec" "$UBO/lists/ublock-unbreak.txt:trusted" "$UBO/lists/ublock-quick-fixes.txt:trusted"
    done
    python3 - "$UBO/lists/optional.json" "$OUT/data/lists" <<'PY'
import json, os, sys
lists, out = json.load(open(sys.argv[1])), sys.argv[2]
with open(os.path.join(out, "catalog.ini"), "w", encoding="utf-8") as f:
    f.write("# The extra filter lists wren://adblock offers (tools/ubo/update.py, OPTIONAL).\n")
    for l in lists:
        size = os.path.getsize(os.path.join(out, l["name"] + ".dat"))
        f.write(f"\n[{l['name']}]\ntitle={l['title']}\nabout={l['about']}\ndefault={'true' if l['default'] else 'false'}\n"
                f"size={size}\nversion={l['version']}\n")
PY
    ls -l "$OUT/data" "$OUT/data/lists"
    ;;
lib)
    TARGET=${2:-armv7-unknown-linux-gnueabi}
    # Rust's standard library needs an unwinder (_Unwind_*): linked in, so the extension needs no libgcc_s.
    # zig cc: its libunwind; GCC (Buildroot): -static-libgcc.
    UNWIND=${UNWIND:-}
    if [ "$TARGET" = host ]; then
        CC=${CC:-cc}
        cargo build --quiet --release --lib --features css-validation --manifest-path "$SRC/engine/ubo/Cargo.toml" --target-dir "$OUT/cargo-host"
        LIB=$OUT/cargo-host/release/libwren_ubo.a
        TARGET=$(rustc -vV | sed -n 's/^host: //p')
    else
        if [ -z "${CC:-}" ]; then
            CC="zig cc -target arm-linux-gnueabi.2.28 -mcpu=cortex_a9"
            UNWIND=${UNWIND:--lunwind}
        fi
        UNWIND=${UNWIND:--static-libgcc}
        # Rust links through the same compiler, so its standard library is bound to glibc 2.28 too.
        printf '#!/bin/sh\nexec %s "$@"\n' "$CC" > "$OUT/cc-$TARGET"
        chmod +x "$OUT/cc-$TARGET"
        LINKER_VAR=CARGO_TARGET_$(echo "$TARGET" | tr 'a-z-' 'A-Z_')_LINKER
        env "$LINKER_VAR=$OUT/cc-$TARGET" CC="$OUT/cc-$TARGET" \
            cargo build --quiet --release --lib --features css-validation --manifest-path "$SRC/engine/ubo/Cargo.toml" \
            --target "$TARGET" --target-dir "$OUT/cargo-target"
        LIB=$OUT/cargo-target/$TARGET/release/libwren_ubo.a
    fi
    EXT_CFLAGS=${EXT_CFLAGS:-$(pkg-config --cflags wpe-web-process-extension-2.0 libsoup-3.0)}
    mkdir -p "$OUT/$TARGET"
    # Export only WebKit's entry point: Rust's symbols stay inside the extension.
    printf '{ global: webkit_web_process_extension_initialize_with_user_data; local: *; };\n' > "$OUT/exports.map"
    # shellcheck disable=SC2086
    $CC -shared -fPIC -O2 -Wall -Wextra -Wno-unused-parameter -std=gnu11 $EXT_CFLAGS \
        -o "$OUT/$TARGET/libwren-ubo.so" "$SRC/engine/ubo/extension.c" "$LIB" \
        -Wl,--gc-sections -Wl,--version-script="$OUT/exports.map" -Wl,-s $UNWIND -lm -ldl -lpthread
    ls -l "$OUT/$TARGET/libwren-ubo.so"
    ;;
*)
    echo "usage: $0 data | lib [TARGET]" >&2
    exit 2
    ;;
esac
