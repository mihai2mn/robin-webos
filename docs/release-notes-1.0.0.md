# Robin 1.0.0 release notes

Robin is a web browser for rooted LG webOS TVs, based on Wren 0.3.3 by sostk
([upstream](https://github.com/sostk/webos-wpe-browser)). This is its first release. It was tested on one LG CX with
webOS 5.3. Read [README.md](../README.md) for what Robin is and how it relates to Wren.

Versions: Robin 1.0.0, based on Wren 0.3.3, WPE WebKit 2.54.3.

## What is new over Wren 0.3.3

### Engine

- WPE WebKit **2.54.3** instead of 2.50.5, with a series of 25 patches, numbered 0001 to 0027 ([patches.md](patches.md)).
- WebKit 2.54 turns the JIT off on every 32-bit build. The series brings back the ARMv7 Baseline JIT and DFG on the
  TV's 32-bit softfp userland ([jit-armv7.md](jit-armv7.md)). The RegExp JIT stays off.
- An optional **64-bit (aarch64) engine** in the same package. It has the 64-bit JIT, draws in software and has no
  hardware video ([engine64.md](engine64.md)). The 32-bit engine is the default.
- A small launcher that picks the engine at start.
- Fixes for black pages in direct mode with WebKit 2.54. Black-picture starts on static pages: 9 of 62 launches before
  the fix, 0 of 30 with it.
- The shared-memory path skips frames identical to the last one sent.

### Video

- Hardware video through LG's Starfish pipeline: H.264 and VP9, AAC audio, other decoded audio (Opus, Vorbis) as PCM,
  page volume and mute, seeking, end of stream ([video.md](video.md)).
- A hybrid policy: small muted previews stay in the page, everything else goes to the hardware plane.
- The video plane follows the page's scroll, and a video partly off screen is cropped (direct mode on the tested CX).
- Emulated fullscreen for hardware video, including iframe players.
- Several fixes for which stream owns the decoder when two tabs play video.

### Ad blocking

- A uBlock Origin style blocker on Brave's adblock-rust, loaded in the web process: network rules, redirects,
  `removeparam`, scriptlets, generic and site cosmetic filters, procedural filters and pop-up rules
  ([ad-blocking.md](ad-blocking.md)).
- Settings, extra filter lists, allowed hosts and **My filters** on `wren://adblock`.
- Never applied on YouTube, where the lists cannot stop its video ads and a blocked request has stopped its player.
- Cost, measured on four news and reference sites: page load within about +1.6 s to -0.4 s of the same load without
  the blocker, and about +40 MB of memory typically.

### Memory

- Memory limits for the web and network processes. A full-length streaming video on default settings played to its
  end with the web process peaking at 683 MB against an 832 MB limit.
- No back/forward page cache by default.
- The kill check compares resident memory, and a short spike no longer kills the web process (it needs several polls
  in a row).
- A declared memory need in `appinfo.json` (1700 MB) so webOS frees memory when the browser starts.

### Input

- Remote media keys (play, pause, stop, rewind and fast-forward by 10 s) act on the page's main video.
- Page zoom is remembered per site. Clearing all history forgets it.

### Branding and packaging

- New name, icon and accent colour. New app ids: `io.github.mihai2mn.robin` and `io.github.mihai2mn.robin.dev`
  (shown as "Robin" and "Robin Test"). They install **beside** Wren and do not replace it.
- `tools/package-release.sh` builds both ipks from a clean clone of a commit and checks them (ELF machine and
  interpreter, needed libraries, ids, versions, licence files). It writes `MD5SUMS` and `BUILD-INFO.txt`.
  The build is not bit-for-bit reproducible: the toolchain image, the filter lists and the staged base are not pinned.
- The package carries its licence material: `licenses/SOURCE.txt` names the source and commit, `engine/licenses/`
  and `engine64/licenses/` (Mesa and LLVM too) cover the engine libraries, `licenses/ad-blocker/` covers the ad blocker
  and the filter lists.
- Tools for working on a rooted TV: `tools/tvdrive` drives a test copy from a PC.

## Known limits

- One TV model was tested. Other models and webOS versions are untested.
- Audio is not confirmed by ear. The logs show the calls reaching LG's pipeline.
- The RegExp JIT is off on the 32-bit engine.
- The 64-bit engine is software rendered, shared-memory only and slower to load (Wikipedia 8.2 s against 4.7 s).
- One hardware video at a time.
- The package is large (below).
- Widevine is at software security level and was not tested with Robin.
- An overlay with a high z-index can cover an emulated-fullscreen video.

The full list is in the [README](../README.md#limits).

## Tested hardware

LG CX, webOS 5.3, kernel 4.4.84, about 2.7 GB of RAM. Measurements in these notes are from this TV with the
development package of 2026-11-22.

## Install size

About 230 MB to download, about 546 MB installed, and roughly 380 MB free on the TV's app storage while the installer
runs. The size is two engines in one package ([building.md](building.md#size)).

## Files in this release

- `io.github.mihai2mn.robin_1.0.0_arm.ipk` and `io.github.mihai2mn.robin.dev_1.0.0_arm.ipk`
- `MD5SUMS`
- `BUILD-INFO.txt`: the commit, the WebKit library checksum, the app kit commit
- Source: see [What a release must attach or link](building.md#what-a-release-must-attach-or-link)

## Keeping your Wren data

Robin starts with an empty profile, because a profile lives in a folder named after the app id. If you used Wren on the
same TV and want its bookmarks, history, zoom levels, cookies and ad blocker settings, copy its folder once.

Start Robin once and close it, so that its own folder exists. Close Wren too. Then run this on a PC (`<tv>` is your
TV's ssh host):

```sh
ssh root@<tv> 'cd /media/developer &&
  owner=$(stat -c %u:%g io.github.mihai2mn.robin.data) &&
  mv io.github.mihai2mn.robin.data io.github.mihai2mn.robin.data.empty &&
  cp -a org.sostk.wpebrowser.data io.github.mihai2mn.robin.data &&
  chown -R "$owner" io.github.mihai2mn.robin.data && chmod 700 io.github.mihai2mn.robin.data'
```

The `chown` matters. webOS runs each app under its own user, and Robin ignores a profile folder it does not own and
starts empty instead. The copy leaves Wren's folder untouched. When Robin shows your bookmarks, you can delete
`io.github.mihai2mn.robin.data.empty`. This was run on the tested CX: 5 bookmarks, 93 history entries and 5 open tabs
came across.

## Going back to Wren

Robin and Wren are separate apps. To use Wren again, open it. To remove Robin, use the Homebrew Channel or
`ares-install -d <device> -r io.github.mihai2mn.robin` (and `io.github.mihai2mn.robin.dev` if you installed it). Wren's
data was never touched. Get Wren from [upstream](https://github.com/sostk/webos-wpe-browser).
