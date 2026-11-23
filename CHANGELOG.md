# Changelog

## 1.1.1 (not released yet)

- **A feed with several videos no longer freezes its page.** Each video's audio output answered volume and mute
  notifications by setting the same values again, which sent the notifications round again. On a Reddit feed the
  second video to start left one thread doing only that, the page stopped responding, and reloading did nothing.
  Settings that do not change anything are now ignored (same feed on the TV: frozen on the second scroll before, ten
  scrolls and every video playing after).
- **After a skip, Play waits for the new position.** When a page resumed playback before the first data from the new
  position arrived, LG's player got Play before Seek. It now gets Seek first.

## 1.1.0

Faster start, JavaScript about twice as fast, smoother scrolling, and fixes from use on the TV. Measured on an LG CX
(webOS 5.3) against 1.0.0, same pages, same TV:

| | 1.0.0 | 1.1.0 |
|---|---|---|
| Launch to window | 8.4 s | 1.6 s |
| Speedometer 3.1 | 0.505 | 0.61 |
| JavaScript micro benchmark (lower is better) | 858 ms | 415 ms |
| Scrolling a long Wikipedia article | 6.1 fps | 14 fps |
| Loading that article | 10.7 s | 9.7 s |

- **Robin opens in about 1.5 s instead of 8.5 s.** It asked webOS for 1700 MB of free memory, which webOS never had,
  so it held every launch behind the splash screen for its full 7 s timeout. It now asks for 1200 MB.
- **JavaScript calls are 3 to 20 times faster.** WebKit 2.54 switches every inline cache off on 32-bit builds, so no
  call was ever linked and the optimizing JIT could not inline a function or turn Math.sqrt and friends into single
  instructions. Robin turns the caches back on (`JSC_forceICFailure=false`; `{"wpebEnv":{"JSC_forceICFailure":"true"}}`
  restores WebKit's choice).
- **The RegExp JIT is on.** Two bugs in its 32-bit ARM entry code are fixed (found by webos-browser-lab); a framework
  documentation site that used to crash its page with it now loads every time, and regular expressions run about twice
  as fast.
- **CSS selectors are compiled to machine code** on the 32-bit engine, as on 64-bit WebKit (ported from
  webos-browser-lab). `WEBKIT_DISABLE_CSS_JIT=1` turns it off.
- **Scrolling past sticky headers is more than twice as fast.** A scroll no longer lays the whole page out again for
  each sticky element (from webos-browser-lab).
- **No more cache purge every 2 seconds.** Any page above about 180 MB had its caches dropped every 2 s, and above about
  290 MB also its compiled JavaScript, style data and decoded images. That now happens when memory pressure rises, and
  at most every 30 s while it stays high. A heavy page peaks about 30 MB higher; the limits that protect the TV are
  unchanged.
- **The TV keyboard no longer pops up on its own.** It opens when you click a text field or press OK on it, not when a
  page focuses a field by itself.
- **Typing from the phone app no longer doubles letters.** LG's remote keyboards send the whole text each time and mean
  "replace"; Robin appended it ("aababc").
- **Seeking no longer leaves the picture black.** After a seek or a quality switch, frames are held back until the first
  keyframe reaches the TV's decoder. On 4K streams the picture stayed black until the resolution was changed.
- **Busy pages no longer freeze every tab.** Endless animations, such as loading spinners in a feed, pause while they are
  off screen. Seven spinners below the fold kept the engine's main thread at full load, and all tabs stopped answering.
- **Blur and shadow effects are drawn by the graphics chip** instead of the engine's main thread.
- **Less ad-blocker work on large pages**: no video bookkeeping on pages without video, and generic hiding rules in
  fewer batches while a page loads.
- **Hover previews on thumbnail pages play again.** The limit on software-decoded videos counted paused previews and
  stopped all of them once two had loaded.

Still slow: very heavy pages, such as large infinite feeds, can keep one CPU core busy on the TV. Page loads are about
twice as slow as in LG's own browser; most of that is WebKit's layout and style work on the TV's CPU.

## 1.0.0

First release. See [docs/release-notes-1.0.0.md](docs/release-notes-1.0.0.md).
