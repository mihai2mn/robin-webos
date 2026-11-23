# Robin 1.1.0 release notes

Robin is a web browser for LG webOS TVs, based on Wren 0.3.3 by sostk
([upstream](https://github.com/sostk/webos-wpe-browser)). It was tested on one LG CX with webOS 5.3 (rooted); a user
reports it running on a non-rooted CX in Developer Mode.

Versions: Robin 1.1.0, based on Wren 0.3.3, WPE WebKit 2.54.3 with patches 0001 to 0031 ([patches.md](patches.md)).

## Faster

All numbers from the same LG CX, same test pages, 1.0.0 against 1.1.0. The test pages are served from the TV itself
(a local Wikipedia copy, Speedometer 3.1), so the network does not enter into them.

| | 1.0.0 | 1.1.0 | LG's own browser |
|---|---|---|---|
| Launch to window | 8.4 s | 1.6 s | about 2 s |
| Speedometer 3.1 | 0.505 | 0.61 | cannot run it |
| JavaScript micro benchmark (lower is better) | 858 ms | 415 ms | 358 ms |
| Scrolling a long Wikipedia article | 6.1 fps | 14 fps | 58 fps |
| Loading that article | 10.7 s | 9.7 s | 4.0 s |

Where it came from:

- **Launch.** The app asked webOS for 1700 MB of free memory before starting. The TV's memory manager could never find
  that much and held every launch behind the splash screen for its 7-second timeout. Robin now asks for 1200 MB.
- **JavaScript inline caches.** WebKit 2.54 sets `forceICFailure` on every 32-bit build, so every call took the slow
  path through the linker, and the optimizing JIT (DFG) never learned which function a call site calls. It inlined
  nothing and called Math.sqrt, Math.floor and the like as full native calls (about 400 ns each). With the caches on,
  those calls are 8 to 20 times faster. Robin sets `JSC_forceICFailure=false` for the engine; the launch parameter
  `{"wpebEnv":{"JSC_forceICFailure":"true"}}` restores WebKit's default.
- **RegExp JIT** (patch 0023): two bugs in the 32-bit entry code, found and fixed by
  [webos-browser-lab](https://github.com/ViieeS/webos-browser-lab) (MIT): the caller's r4 register was overwritten, and
  the fifth argument was read from the wrong stack slot. 1.0.0 kept the RegExp JIT off because of them.
- **CSS selector JIT** (patch 0031, from webos-browser-lab, ported to 2.54.3): selectors are compiled to ARM code
  instead of being matched in C++. `WEBKIT_DISABLE_CSS_JIT=1` turns it off.
- **Sticky elements** (patch 0030, from webos-browser-lab): scrolling moves sticky boxes on the compositor instead of
  laying out the page again.
- **Memory relief** (patch 0028): WebKit repeated its memory relief on every 2-second poll while a page stayed above
  its threshold, throwing away compiled JavaScript, style data and decoded images each time. It now runs when the
  pressure rises and at most every 30 s while it stays high (`WPE_BROWSER_DEBUG_MEM_RELIEF_S`).
- **Blur and drop shadows** (patch 0029) get their own compositing layer, so the graphics chip applies them.

## Fixes

- The TV keyboard opens when you click a text field or press OK on it, not when a page focuses a field by itself.
- Typing from LG's phone app no longer doubles letters (its keyboard sends the whole text each time, meant as a
  replacement).
- Seeking on 4K streams no longer leaves the picture black until the quality changes.
- Endless animations off screen are paused; a feed with loading spinners below the fold no longer freezes every tab.
- Hover previews on thumbnail pages play again.

## How it was checked

On the CX, with the release build:

- JIT correctness pages: arithmetic and math checksums, 71 math folding cases, 64 regular expression cases each run
  300 times, 41 CSS selectors over 366 elements with the selector JIT on and off (identical results).
- A framework documentation site that crashed with the RegExp JIT before: 8 of 8 loads fine.
- Video: muted test clips on a short and a long page, fullscreen with the side panel, a seek, leaving fullscreen.
- A heavy page's memory peak: 351 MB (321 MB in 1.0.0), against a kill limit of 832 MB.
- 17 real sites (news, documentation, shopping, code hosting, forums), 40 s each with scripted scrolling and the inline
  caches on: no page process died.

## Upgrading

Install over 1.0.0; bookmarks, history, settings and the ad blocker's lists stay. See [README.md](../README.md#install).

## Known limits

The limits of 1.0.0 still apply ([README.md](../README.md#limits)), except that the RegExp JIT is now on.
