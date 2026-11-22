# Third-party notices

This repository is licensed GPL-3.0-or-later ([LICENSE](LICENSE)). It is a fork of Wren by sostk (GPL-3.0-or-later).
Everything below is either source in this repository, a dependency fetched at build time, or a file inside the
release ipk. For each item: name, where it comes from, licence, and whether it needs attention when it is shipped
in a GPL-3.0 ipk. Licence texts for the ipk's engine libraries are inside the ipk (`engine/licenses/`) and are
produced by Buildroot.

Nothing found conflicts with GPL-3.0. Five items need action before a release: see
[Things to fix before a release](#things-to-fix-before-a-release).

## In this repository

| Item | Source | Licence | Note |
|---|---|---|---|
| Wren UI, engine, media sink, backend, tools | [sostk/webos-wpe-browser](https://github.com/sostk/webos-wpe-browser) 0.3.3 and this fork | GPL-3.0-or-later | 140 files carry an SPDX line |
| `drm/ocdm-widevine` | upstream | GPL-3.0-or-later (own `LICENSE`) | Own description of the TV's Widevine interface. No Google source and no CDM in the repository |
| `buildroot-external/patches` (WebKit, GnuTLS) | upstream and this fork | the licence of the file each patch changes (WebKit: LGPL-2.1-or-later and BSD-2-Clause) | Three patches name upstream's author in their header |
| `third_party/patches/webos-native-app-kit` | this fork | GPL-3.0-or-later | Patches to the app kit |
| `third_party/webos-native-app-kit` (submodule, commit 8d53a5b) | [Native-Webos/webos-native-app-kit](https://github.com/Native-Webos/webos-native-app-kit) | GPL-3.0-or-later | Its own components are in the table below |
| `engine/ubo/vendor/rustc-hash` | [rust-lang/rustc-hash](https://github.com/rust-lang/rustc-hash) 2.1.3 | MIT OR Apache-2.0 | One change, described in `vendor/README.md`. Both licence files are present |
| `toolchain/extra-headers/linux/dma-buf.h` | Linux kernel UAPI | GPL-2.0 WITH Linux-syscall-note | A header, copied so WebKit builds against the 4.4 headers |
| `browser/fonts`: Manrope | [googlefonts/manrope](https://github.com/googlefonts/manrope) | SIL OFL-1.1 (`browser/fonts/OFL-Manrope.txt`) | Bundled unmodified |
| `browser/icons/*.svg`, `browser/branding`, `browser/icon.png`, `largeIcon.png`, `docs/screenshots` | upstream | not recorded | Upstream's artwork. The repository does not say where the icon set came from. Ask upstream, or redraw, before relying on a licence other than the repository's |
| `tools/ubo/wren-filters.txt`, `wren-scriptlets.js`, `engine/ubo/compat.js`, `cosmetic.js` | this fork | GPL-3.0-or-later | The scriptlets follow uBlock Origin's scriptlet conventions and may contain adapted code. uBlock Origin is GPL-3.0 |

## Components the app kit brings (shipped in the ipk under `licenses/`)

From the kit's [THIRD-PARTY-NOTICES.md](https://github.com/Native-Webos/webos-native-app-kit/blob/main/THIRD-PARTY-NOTICES.md).
The ipk carries the licence text of each one that is linked in.

| Component | Licence |
|---|---|
| SDL-webOS 2.30.12 (modified by the kit) | zlib |
| stb, nlohmann/json, qrcodegen | MIT (stb: MIT or public domain) |
| nanosvg, libunibreak | zlib |
| libwebp 1.4.0 | BSD-3-Clause with patent grant |
| libass 0.17.5 | ISC |
| FreeType | FTL (or GPL-2.0-or-later) |
| HarfBuzz | MIT |
| FriBidi | LGPL-2.1-or-later |
| FFmpeg (the kit's own build) | LGPL-2.1-or-later, shared libraries |
| libdovi (dolby_vision crate and 23 crates) | MIT, Apache-2.0, Zlib or Unlicense |
| Atkinson Hyperlegible Regular and Bold | SIL OFL-1.1 (`fonts/OFL.txt` in the ipk) |
| PlxNative parts, Kodi (no code copied) | GPL-3.0-or-later, GPL-2.0-or-later |

LG's firmware libraries (Starfish, NDL, Luna, EGL/GLES, libcurl on the TV) are loaded from the TV at run time and are
not redistributed. The Widevine CDM is on the TV and is not redistributed.

## Engine libraries in the ipk (Buildroot)

The 32-bit engine is built by Buildroot. The ipk's `engine/licenses/manifest.csv` lists 56 packages with licence and
source URL. The ones that matter most:

| Package | Licence (from the manifest) |
|---|---|
| WPE WebKit (2.54.3 plus this fork's patches) | LGPL-2.1-or-later, BSD-2-Clause |
| libwpe, wpebackend-fdo | BSD-2-Clause |
| GStreamer 1.24.13 (base, good, bad, libav) | LGPL-2.1-or-later (bad: LGPL-2.0-or-later and one entry the manifest marks UNKNOWN) |
| FFmpeg 9.0.2 | LGPL-2.1-or-later, libjpeg licence (no GPL or nonfree options) |
| GLib, glib-networking, libsoup 3, GnuTLS | LGPL-2.0/2.1-or-later |
| nettle, gmp, libunistring, libtasn1 | LGPL-3.0-or-later or GPL-2.0-or-later (libtasn1 tools GPL-3.0-or-later) |
| ICU 78.2 | ICU licence |
| SQLite | public domain (blessing) |
| ca-certificates data | MPL-2.0 |
| DejaVu fonts | Bitstream Vera |
| Others (expat, brotli, libvpx, opus, libvorbis, libogg, libpng, libjpeg-turbo, libxml2, libxslt, libepoxy, wayland, ...) | MIT, BSD or similar |

GPL-3.0 is compatible with all of these. LGPL libraries are shared libraries in the ipk, so they can be replaced.
The source for these packages is what Buildroot downloads. See
[what a release must attach or link](docs/building.md#what-a-release-must-attach-or-link).

## The 64-bit engine (`engine64/` in the ipk)

| Component | Licence | Note |
|---|---|---|
| Mesa (libgallium 26.2.5, EGL, GLES, gbm) | MIT and others | Software renderer (llvmpipe) |
| LLVM 23.1 (libLLVM, 67 MB) | Apache-2.0 WITH LLVM-exception | Compatible with GPL-3.0 |
| The rest | as in the table above | Same packages built for aarch64 |

## Ad blocker

| Item | Source | Licence | Note |
|---|---|---|---|
| adblock-rust 0.13.6 | [brave/adblock-rust](https://github.com/brave/adblock-rust) | MPL-2.0 | Unmodified, fetched from crates.io at build time. MPL-2.0 file-level copyleft: the source is available at that address. MPL-2.0 may be combined with GPL-3.0 |
| Rust crates (59 in `Cargo.lock`) | crates.io | MIT, Apache-2.0, MIT OR Apache-2.0, Unlicense OR MIT, Unicode-3.0 | flatbuffers is Apache-2.0. The ICU4X crates are Unicode-3.0. Proc-macro crates (syn, quote, serde_derive and similar) are build-time only |
| uBlock Origin scriptlets and redirect resources | [gorhill/uBlock](https://github.com/gorhill/uBlock), commit recorded in `ubo/COMMIT` at build time | GPL-3.0 | Fetched by `tools/ubo/update.py` |
| uBlock filters, badware, privacy, unbreak, quick fixes, annoyances, cookie notices, URL tracking (uAssets) | [uBlockOrigin/uAssets](https://github.com/uBlockOrigin/uAssets) | GPL-3.0 | |
| EasyList, EasyPrivacy and EasyList's social, annoyances, newsletters, notifications, chat and AI lists | [easylist.to](https://easylist.to) | GPL-3.0-or-later or CC BY-SA 3.0 (dual) | Use them under the GPL branch. CC BY-SA 3.0 is not compatible with GPL-3.0 |
| URLhaus filter, Phishing URL Blocklist | [malware-filter](https://gitlab.com/malware-filter) | CC0-1.0 (stated by the project; confirm on the day) | |
| AdGuard Base (optional, off by default) | [AdGuard](https://github.com/AdguardTeam/AdguardFilters) | GPL-3.0 | |
| Peter Lowe's ad server list | [pgl.yoyo.org](https://pgl.yoyo.org/adservers/) | **not confirmed** | Its terms are on the site and are not a standard licence. Read them before redistributing the compiled list, or drop it from `tools/ubo/update.py` |

The list licences above come from the projects' own pages as the author remembered them when this file was written. The
lists are downloaded when the package is built, whatever they say that day. Confirm the licence line in each list's
header on the day of the build.

## Widevine

The repository describes the TV's Widevine interface (`drm/ocdm-widevine/include/widevine/cdm.h`). It contains no
Widevine code and no CDM. The provisioning URL in `drm/ocdm-widevine/src/ocdm.cpp` carries Google's public Widevine
provisioning API key, the same one the TV's own CDM and Chromium use. See the warning in the [README](README.md#limitations-and-known-issues).

## Things to fix before a release

1. **`licenses/SOURCE.txt` in the ipk is unset.** The kit leaves "(not set: whoever distributes this package must
   publish the source and name it here)". `tools/package-release.sh` now passes `SOURCE_URL` to the kit. Set it to the
   release tag's URL.
2. **`engine/licenses/manifest.csv` is stale.** It lists wpewebkit 2.50.5 while the library is 2.54.3. Regenerate
   Buildroot's `legal-info` for the 2.54.3 build before the build.
3. **`engine64/` has no licence files.** `package-release.sh` removes `engine64/licenses` on the assumption that
   `engine/licenses` covers it, but Mesa and LLVM are in no manifest. Add Mesa's and LLVM's licence texts.
4. **The ad blocker's licences are not in the ipk.** There is no notice for adblock-rust, the Rust crates, uBlock
   Origin or the filter lists. Add a `licenses/adblock.txt` (or similar) to the package.
5. Peter Lowe's list: confirm its terms or drop it (see above).
