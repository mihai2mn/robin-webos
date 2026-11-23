# Changelog

## Unreleased

Fixed on `main`, not yet in a release package.

- **Seeking no longer leaves the picture black.** After a seek or a quality switch, frames are held back until the first
  keyframe reaches the TV's decoder. On 4K streams the picture stayed black until the resolution was changed.
- **Busy pages no longer freeze every tab.** Endless animations, such as loading spinners in a feed, pause while they are
  off screen. Seven spinners below the fold kept the engine's main thread at full load, and all tabs stopped answering.
- **Blur effects are drawn by the graphics chip** instead of the engine's main thread.
- **Robin opens in about 1.5 s instead of 8.5 s.** It asked webOS for 1700 MB of free memory, which webOS never had,
  so it held every launch behind the splash screen for its full 7 s timeout. It now asks for 1200 MB.
- **The RegExp JIT is on.** Two bugs in its 32-bit ARM entry code are fixed (found by webos-browser-lab); a framework
  documentation site that used to crash its page with it now loads every time, and regular expressions run about twice
  as fast.
- **No more cache purge every 2 seconds.** Any page above about 180 MB had its caches dropped every 2 s, and above about
  290 MB also its compiled JavaScript, style data and decoded images. That now happens when memory pressure rises, and
  at most every 30 s while it stays high. Speedometer 3.1: 0.505 -> 0.568.
- **Hover previews on thumbnail pages play again.** The limit on software-decoded videos counted paused previews and
  stopped all of them once two had loaded.

Still slow: very heavy pages, such as large infinite feeds, can keep one CPU core busy on the TV.

## 1.0.0

First release. See [docs/release-notes-1.0.0.md](docs/release-notes-1.0.0.md).
