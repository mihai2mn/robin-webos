# Robin 1.1.1 release notes

Robin is a web browser for LG webOS TVs, based on Wren 0.3.3 by sostk
([upstream](https://github.com/sostk/webos-wpe-browser)). Tested on one LG CX with webOS 5.3 (rooted).

Versions: Robin 1.1.1, based on Wren 0.3.3, WPE WebKit 2.54.3 with patches 0001 to 0032 ([patches.md](patches.md)).

1.1.1 fixes two ways a page with several videos froze. Speed is the same as 1.1.0.

## Fixes

- **A feed with several videos no longer freezes after the second one starts.** Robin's audio output for videos
  played inside the page answered every volume or mute notification by setting the same values again, which sent the
  notification round again. On a Reddit feed one thread ended up doing only that, the page stopped responding and
  reloading did nothing. Settings that change nothing are now ignored.
- **Scrolling a feed of muted videos no longer freezes the page** (WebKit patch 0032). When a muted video scrolled out
  of view, WebKit paused it from the page's main thread while that video's loader thread waited for the main thread,
  and each waited for the other. The page froze after about 30 s of scrolling, with nothing using the processor.
- **After a skip, LG's player gets the new position before Play.** A page that resumed playback before the data from
  the new position arrived sent Play first.

## How it was checked

On the CX:

- The same subreddit feed, scrolled on the TV: with 1.1.0's audio output it froze on the second scroll; with 1.1.1 it
  ran ten scrolls with every video starting. A second run of 30 rounds of scrolling down and up (about 4 minutes)
  did not freeze.
- A local page playing two muted videos one after another with volume changes: the page's timers never stopped for
  more than 0.2 s, and each volume change was handled once.
- Skips on a 2160p test clip, with the data after the skip held back by 2.5 s to imitate a slow site: playback carried on
  at the new position in every run, including the runs where Play arrived before the new data.

## Known limits

- A user still reports a black picture after skipping on one video site, until the quality is changed. 1.1.0's notes
  said this was fixed; it is not. The site's playback events and LG's frame counts look normal after a skip, and it has
  not been reproduced on a test clip yet.
- Small videos inside a feed are decoded by the TV's processor and stutter while the page scrolls. Fullscreen videos
  and large players go to LG's hardware player and play at full rate.

The limits of 1.1.0 still apply ([README.md](../README.md#limits)).

## Upgrading

Install over 1.1.0 or 1.0.0; bookmarks, history, settings and the ad blocker's lists stay.
