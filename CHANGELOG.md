# Changelog

## Unreleased

Fixed on `main`, not yet in a release package.

- **Seeking no longer leaves the picture black.** After a seek or a quality switch, frames are held back until the first
  keyframe reaches the TV's decoder. On 4K streams the picture stayed black until the resolution was changed.
- **Busy pages no longer freeze every tab.** Endless animations, such as loading spinners in a feed, pause while they are
  off screen. Seven spinners below the fold kept the engine's main thread at full load, and all tabs stopped answering.
- **Blur effects are drawn by the graphics chip** instead of the engine's main thread.
- **Hover previews on thumbnail pages play again.** The limit on software-decoded videos counted paused previews and
  stopped all of them once two had loaded.

Still slow: very heavy pages, such as large infinite feeds, can keep one CPU core busy on the TV.

## 1.0.0

First release. See [docs/release-notes-1.0.0.md](docs/release-notes-1.0.0.md).
