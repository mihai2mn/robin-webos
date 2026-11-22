# Patches

## WPE WebKit 2.54.3 (`buildroot-external/patches/wpewebkit/2.54.3/`)

Buildroot applies this folder in place of the 2.50.5 one when the build runs with `WEBKIT_VERSION=2.54.3`. The series
is regenerated against 2.54.3 with no fuzz. Upstream's 0003 (grayscale text: 2.54 defaults to no subpixel order) and
0007 (the ring buffer size: fixed upstream) are dropped, so the numbers 0003 and 0007 are unused. The folder has its
own README with more detail.

| Patch | Why |
|---|---|
| 0001 honour-WEBKIT_EXEC_PATH-in-release-builds | the helper processes ship inside the app, not at the compile-time path |
| 0002 use-the-default-EGL-display-without-Mesa-surfaceless | the TV's Mali EGL has no `EGL_MESA_platform_surfaceless` |
| 0004 jsc-arm-softfp-jit | JavaScriptCore's ARMv7 JIT on webOS's softfp ABI. In 2.54 it also turns the Baseline JIT and DFG back on for ARMv7 Linux |
| 0005 skia-arm32-gcc-raster-pipeline-abi | Skia's stage ABI mismatched under GCC softfp: gradients, masks and colour filters were drawn as stripes |
| 0006 wpe-optional-process-swap-on-navigation | keep a view in its web process: the webOS compositor cannot move a layer between processes |
| 0008 jsc-32bit-gcc-no-speculative-indexing-header-loads | GCC hoisted an `IndexingHeader` load above its type check ([webkit.org/b/295780](https://bugs.webkit.org/show_bug.cgi?id=295780)) |
| 0009 shm-readback-without-bgra-read-support | Mali-400 reads RGBA, not BGRA: convert the shared-memory frame |
| 0010 gstreamer-per-player-software-video | a `<video>` can be marked to decode in software inside the page (the hybrid policy), others go to the platform sink |
| 0011 gstreamer-websrc-416-past-end-is-eos | a range request that starts at the file size gets 416; treat it as end of stream |
| 0012 gstreamer-disable-media-disk-cache-for-urisourcebin | no on-disk media buffer on 32-bit, so files over 4 GiB play |
| 0013 cmake-gstreamer-gl-only-when-used | 2.54 asked for libgstgl whenever video is on, even with GStreamer GL off |
| 0014 gstreamer-dmabuf-video-frames-need-gstreamer-gl | the DMABuf frame path did not compile with GBM on and GStreamer GL off |
| 0015 jsc-32bit-jit-and-dfg-build-on-2.54 | the 32-bit Baseline JIT and DFG compile again (missing includes, 32-bit typos, stubs, two ARMv7 assembler overloads) |
| 0016 jsc-armv7-jit-enabled-by-default | 2.54 turned the JIT off at run time on 32-bit; ARMv7 Linux gets its default back |
| 0017 jsc-32bit-handler-ic-no-repatching-downcast | a handler inline cache was downcast to the repatching type: memory corruption in release builds |
| 0018 jsc-armv7-baseline-putbyval-profile-register | the `put_by_val` profile register aliased the handler register (r9) |
| 0019 jsc-armv7-jitdata-register-not-allocatable | r11 holds the JIT data pointer and must not be handed out by the DFG register allocator |
| 0020 jsc-32bit-dfg-data-ic-property-caches | the 32-bit DFG ported to 2.54's data-IC property caches |
| 0021 jsc-32bit-empty-value-profiles | zeroed value profiles decoded as the double 0.0 on 32-bit, so the DFG mispredicted |
| 0022 gstreamer-no-segment-loop-seek-with-hardware-video | a looping video with the hardware sink stalled a tab switch for 6 to 10 s and rewound early |
| 0023 jsc-armv7-regexp-jit-off | the RegExp JIT crashed one site's web process on the TV: it stays off (64-bit only) |
| 0024 webcore-gstreamer-webcodecs-int-varargs-32bit | a 64-bit value was passed as an int vararg: crash in WebCodecs decoder set-up on 32-bit |
| 0025 upstream-2.54.3-to-2.54-head | `webkitglib/2.54` queue after 2.54.3 (`c01fbd906..9715f4680`, 21 commits, 58 tarball `Source/` files) carried as one patch until upstream cuts `wpewebkit-2.54.4.tar.xz` |
| 0026 webcore-mse-back-buffer-limit | plain MediaSource kept every played second: keep at most `MSE_MAX_BACK_BUFFER_SECONDS` (default 30, 0 = off) behind the playhead; after a seek back also drops data more than that far ahead of the new position |
| 0027 wtf-memory-kill-after-consecutive-polls | a brief footprint spike above the kill threshold (a large media append) killed the web process: now only after `MEMORY_KILL_CONSECUTIVE_POLLS` (default 3) measurements in a row, or at once 12.5% above |

Details of 0015 to 0021 and how they were checked: [jit-armv7.md](jit-armv7.md).

## WPE WebKit 2.50.5 (`buildroot-external/patches/wpewebkit/`, upstream's base)

The 2.50.5 series 0001 to 0012 is still in the tree and is what `tools/build-engine.sh` uses without `WEBKIT_VERSION`.
It is upstream's 0001 to 0009 (the same ideas as above, plus 0003 grayscale antialiasing and 0007 the ring buffer
size) and this fork's 0010 to 0012.

## GnuTLS (`buildroot-external/patches/gnutls/`)

| Patch | Why |
|---|---|
| 0001 use-the-app-CA-bundle-at-its-runtime-path | read the app's packaged CA bundle (`WPE_BROWSER_CA_FILE`, via `secure_getenv`); verification is unchanged |

## webOS native app kit (`third_party/patches/webos-native-app-kit/`)

Applied on top of the submodule by `third_party/patches/apply-kit-patches.sh`. Candidates for upstreaming.

| Patch | Why |
|---|---|
| 0001 Delegate-on_pointer-hook | the app sees every pointer event first (the page under the Magic Remote pointer) |
| 0002 GLIBC_RUNTIMES-private-runtime-tree | package a private runtime (the engine) that uses the TV's glibc, with its own package check |
| 0003 TextField-set_chromeless | a text field without its own box, for the address-bar pill |
| 0004 Dialog-drop-down-menu-support | dialog width, padding and top-right anchoring, for the browser menu |
| 0005 check-package-system-lib | name further TV system libraries the private runtime may link |
| 0006 starfish-no-low-delay | do not ask LG's pipeline for low-delay mode on video-only streams (low delay shows B-frames out of order) |
| 0007 starfish-source-crop | `set_area_cropped()` uses `SDL_webOSExportedSetCropRegion`: a video partly under the toolbar is cut, not squashed |
| 0008 WebOSPackage-required-memory | a `REQUIRED_MEMORY` package option, written to `appinfo.json` as `requiredMemory` |
| 0009 starfish-volume-and-mute | `StarfishBackend::set_volume` (`setVolume`, `setAudioMute`) |

Kit patches 0001 to 0005 are already contained in the kit commit the submodule points at. The script detects that
and skips them.
