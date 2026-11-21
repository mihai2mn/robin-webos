# Building Wren

This is the build the fork's release package comes from. It is long and needs a lot of disk. Upstream
planned about 30 GB for the 32-bit engine alone. This fork builds two engines and a private WebKit copy, so plan
for several times that (not measured), a Linux or WSL host with Docker, and many hours for the two WebKit builds. The UI app builds with the
webOS NDK on the host.

Build outputs go to `~/webos-browser-build/` (`WPE_BUILD_ROOT`), outside the repository. Output you stage for
packaging goes to `build/`, which git ignores.

## The parts

| Part | Source | Built by | Output |
|---|---|---|---|
| Cross toolchain (GCC 14.2, glibc 2.28, Linux 4.4 headers, `arm-webos-linux-gnueabi`) | `toolchain/ct-ng.defconfig` | `tools/build-engine.sh` (crosstool-ng in Docker) | `x-tools/` |
| 32-bit engine libraries (WebKit 2.54.3, GStreamer, ICU, ...) | `buildroot-external/` (a Buildroot external tree) | `tools/build-engine.sh`, Buildroot commit `cdd3776b` | `br-out/`, then `build/engine-stage-jit/engine` |
| 64-bit engine libraries | same tree, `webos_browser_aarch64_defconfig` | `tools/build-engine-aarch64.sh` | `br-out-aarch64/` |
| `wpe-engine`, media sink, launcher, direct-presentation backend | `engine/`, `common/` | `tools/package-release.sh` phases | in the package |
| UI app | `browser/` and the app kit | `browser/build.sh`, `tools/package-release.sh ui` | in the package |
| uBO data and extension | `engine/ubo/`, `tools/ubo/` | `tools/ubo/build.sh`, `package-release.sh ubo` | in the package |

## 1. Get the sources

```sh
git clone <this repository> && cd <it>
git submodule update --init
sh third_party/patches/apply-kit-patches.sh      # the app kit patches 0001-0009, idempotent
docker build -t webos-browser-build:1 docker/
```

`docker/run.sh` mounts a fixed source path (`-v /home/sostk/Desktop/waydroid-test:/src`). Edit it before using it.
The `build-*.sh` scripts below mount the checkout themselves.

## 2. The 32-bit engine

```sh
WEBKIT_VERSION=2.54.3 bash tools/build-engine.sh start     # detached container; status: ... status, stop: ... stop
```

This builds the toolchain, then Buildroot (at commit `cdd3776b`) with the fork's WebKit series
(`buildroot-external/patches/wpewebkit/2.54.3/`), then stages the engine with its libraries, glibc, fonts, CA
bundle and licences (`tools/stage-engine.sh`) into `build/engine-stage-jit/engine`. Set `JOBS` to your memory:
WebKit needs about 2 GB of RAM per job. Without `WEBKIT_VERSION` the script builds upstream's 2.50.5.

`package-release.sh` also takes WebKit's own build tree (`br-out/build/wpewebkit-2.54.3`) as `WK_SHARED`, see the
`webkit32` phase below.

## 3. The 64-bit engine

```sh
WEBKIT_VERSION=2.54.3 bash tools/build-engine-aarch64.sh start
docker/run.sh 'BR_OUT=/build/br-out-aarch64 bash /src/tools/engine-aarch64.sh stage'    # inside the container
```

The stage step needs `cmake`, which the container has and a plain WSL host may not. It writes
`build/engine-stage-aarch64/engine`. Do not run the two Buildroot builds at the same time: they share the Buildroot
checkout, the download cache and ccache. See [engine64.md](engine64.md).

## 4. The release package

`tools/package-release.sh` builds the ipk from a **clean clone of a commit**, so uncommitted work never ships.

```sh
bash tools/package-release.sh all                 # both ids; about 20 minutes when the WebKit tree is already built
IDS="dev" COMMIT=<sha> PKG_TMP=/big/disk bash tools/package-release.sh all
OVERLAY="engine/ubo tools" bash tools/package-release.sh all    # testing only: copy these working-tree paths over the clone
```

Output: `build/release/*.ipk`, `MD5SUMS` and `BUILD-INFO.txt` (commit, WebKit library md5, inputs used).

**Inputs it does not build itself** (each has its own script above):

| Variable | What | Default |
|---|---|---|
| `STAGE_BASE` | staged 32-bit engine: libraries, glibc, GStreamer, fonts, certificates, licences | `~/wren/build/engine-stage-jit/engine` (set it to `build/engine-stage-jit/engine`) |
| `STAGE64` | staged aarch64 engine | `build/engine-stage-aarch64/engine` |
| `WK_SHARED` | the in-tree WebKit 2.54.3 build | `$BUILD_ROOT/br-out/build/wpewebkit-2.54.3` |
| `KIT_REPO` | a git clone of the app kit, if the working tree holds only a copy | the submodule |
| `WEBOS_NDK` | the webOS NDK (UI and backend) | `~/webos-ndk/arm-webos-linux-gnueabi_sdk-buildroot` |
| `PKG_TMP`, `WORK`, `OUT` | scratch folder (put it on a big disk), work tree, output folder | under `BUILD_ROOT`, `build/release` |

**Phases** (`all` runs them in this order; a single phase needs the ones before it):

1. `clone`: clean checkout of the commit, the kit submodule, kit patches, CRLF stripped from `tools/*.sh`.
2. `overlay`: only with `OVERLAY`.
3. `launcher`: the dynamic engine launcher, one per app id (its interpreter path holds the id).
4. `webkit32`: builds a pristine 2.54.3 tree plus the fork's patches in order, compares it with a private copy of
   `WK_SHARED`, overwrites every differing source file from the pristine tree, rebuilds with ninja, and fails unless
   the copy then has no differing file. Needs the WebKit tarball in `$BUILD_ROOT/dl/wpewebkit/`. About 6 GB of disk.
   `FULL=1` cleans first (hours). `webkit32 clean` removes the copy. Caveat: ninja trusts object mtimes.
5. `engine32`: `wpe-engine`, `libgstwpebmedia.so`, and `libgstinterleave.so` (`tools/build-gst-interleave.sh`) against the 2.54.3 headers, in the container.
6. `backend`: `libWPEBackend-webos.so` and `libwpeb-owner.so` with the NDK.
7. `ubo`: **needs network**. `tools/ubo/update.py` downloads uBO's filter lists and resources (the uBO commit is
   recorded in `ubo/COMMIT`). `tools/ubo/build.sh data` compiles them (needs `cargo` and `node`).
   `tools/ubo/build.sh lib` links `libwren-ubo.so` for armv7 (with the cross toolchain's gcc) and for aarch64
   (with Buildroot's gcc). It needs `rustup target add armv7-unknown-linux-gnueabi aarch64-unknown-linux-gnu`.
8. `stage`: base stage + WebKit 2.54.3 + launcher + uBO into one engine tree per id.
9. `ui`: the UI app per id (`WPEB_APPID`), packed with the kit.
10. `add64`: adds `engine64/` and writes the ipks with `tools/mkar.py`. The TV's installer wants ar member names
    without a trailing `/` and tar entries without `./`. GNU `ar rc` output is refused.
11. `sums`: `MD5SUMS` and `BUILD-INFO.txt`.

The `dev` id (`org.sostk.wpebrowser.dev`) installs next to the real app for testing.

## The UI alone

`./build.sh` in `browser/` builds and tests the UI on the desktop (`cmake --preset host-debug`,
`ctest --preset host-debug`). `./build.sh webos` builds the ipk of the UI only.

Check a package against LG firmware images offline:
`third_party/webos-native-app-kit/tools/fw-check.py --min-release 1.0.0 <ipk>` (needs the webosbrew firmware
symbol inventory).

## Size

Each ipk is about 230 MB and installs to about 546 MB: the UI (a few MB), `engine/` (221 MB) and `engine64/`
(319 MB). Most of `engine64/` is Mesa's software renderer (llvmpipe, with libLLVM at 67 MB), the 122 MB WebKit
library and 33 MB of ICU data. The 32-bit engine carries its own ICU data too. A shared ICU data file would save
about 30 MB. Replacing llvmpipe by softpipe would save about 65 MB and is untested.

## Licences in the package

The ipk bundles open-source libraries under their own licences:
- the UI's (SDL, libwebp, libass, ...): in the package's `licenses/` folder;
- the engine's (WebKit: LGPL-2.1+/BSD; GStreamer, GLib, libsoup, FFmpeg: LGPL; ICU, SQLite, and the rest of its
  Buildroot packages): in `engine/licenses/`, one folder per package with `manifest.csv` (package, version,
  licence, source). These come from Buildroot's `make ... legal-info`, which `tools/stage-engine.sh` expects to
  have been run;
- the ad blocker: adblock-rust (MPL-2.0), rustc-hash (`engine/ubo/vendor`, MIT or Apache-2.0), and the filter
  lists (named in `tools/ubo/update.py`).

See [NOTES.md](../NOTES.md) for toolchain details.
