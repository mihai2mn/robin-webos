# Changelog

## 1.1.0

Faster start, faster scrolling and JavaScript, and fixes from use on the TV. Measured on an LG CX (webOS 5.3) against
1.0.0, same pages, same TV:

| | 1.0.0 | 1.1.0 |
|---|---|---|
| Launch to window | 8.4 s | 1.3-1.7 s |
| Speedometer 3.1 | 0.505 | 0.593 |
| Scrolling a long Wikipedia article | 6.1 fps | 14.1 fps |
| Loading that article | 10.7 s | 9.9 s |

- **Robin opens in about 1.5 s instead of 8.5 s.** It asked webOS for 1700 MB of free memory, which webOS never had,
  so it held every launch behind the splash screen for its full 7 s timeout. It now asks for 1200 MB.
- **Scrolling past sticky headers is more than twice as fast.** A scroll no longer lays the whole page out again for
  each sticky element (from webos-browser-lab).
- **CSS selectors are compiled to machine code** on the TV's 32-bit engine, as on 64-bit WebKit (ported from
  webos-browser-lab). `WEBKIT_DISABLE_CSS_JIT=1` turns it off for testing.
- **The RegExp JIT is on.** Two bugs in its 32-bit ARM entry code are fixed (found by webos-browser-lab); a framework
  documentation site that used to crash its page with it now loads every time, and regular expressions run about twice
  as fast.
- **No more cache purge every 2 seconds.** Any page above about 180 MB had its caches dropped every 2 s, and above about
  290 MB also its compiled JavaScript, style data and decoded images. That now happens when memory pressure rises, and
  at most every 30 s while it stays high. A heavy page peaks about 30 MB higher; the limits that protect the TV are
  unchanged.
- **Seeking no longer leaves the picture black.** After a seek or a quality switch, frames are held back until the first
  keyframe reaches the TV's decoder. On 4K streams the picture stayed black until the resolution was changed.
- **Busy pages no longer freeze every tab.** Endless animations, such as loading spinners in a feed, pause while they are
  off screen. Seven spinners below the fold kept the engine's main thread at full load, and all tabs stopped answering.
- **Blur and shadow effects are drawn by the graphics chip** instead of the engine's main thread.
- **Less ad-blocker work on large pages**: no video bookkeeping on pages without video, and generic hiding rules in
  fewer batches while a page loads.
- **Hover previews on thumbnail pages play again.** The limit on software-decoded videos counted paused previews and
  stopped all of them once two had loaded.

Still slow: very heavy pages, such as large infinite feeds, can keep one CPU core busy on the TV. JavaScript that calls
built-in functions in tight loops (Math.sin and the like) is several times slower than in LG's own browser.

## 1.0.0

First release. See [docs/release-notes-1.0.0.md](docs/release-notes-1.0.0.md).
