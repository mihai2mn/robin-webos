# Video and audio

The path from a `<video>` to the TV's screen and speakers, as this fork has it. The base design is upstream's
([architecture.md](architecture.md), [lg-chromium-media.md](lg-chromium-media.md)).

## The path

WebKit decodes nothing for hardware video. Its GStreamer pipeline ends in the fork's own sinks
(`engine/gst/wpebmediasink.c`), which send the **encoded** stream over `common/wpe_media_ipc.h` to the UI process.
The UI feeds LG's **Starfish** pipeline (`StarfishMediaAPIs`, through the app kit's `StarfishBackend`;
`browser/media_server.cpp`). LG decodes it onto the TV's video plane. WebKit leaves a hole in the page where the
video is, and the UI places the plane in it.

There are two ways to present the page:

- **Direct** (the default). The page is drawn by the GPU, the video plane is an exported window of the page.
- **Shared memory** (`{"wpebPresent":"shm"}`). Page frames travel in shared memory. Hardware video is opt-in with
  `{"wpebShmVideo":"1"}`. Starfish is the default backend there, with NDL as the automatic fallback
  (`{"wpebVideoBackend":"ndl"}`).

Starfish is used because NDL's decoder is fixed in low-delay mode, which shows streams with B-frames out of order.

## What works

All of this was checked on the LG CX (webOS 5.3) with test clips and the engine and player logs.

- H.264 and VP9 on the hardware plane, with seeking, pause, loops and end of stream.
- **AAC** audio goes encoded to Starfish with the video.
- **Decoded audio** (Opus, Vorbis, and similar) is converted to 48 kHz stereo 16-bit PCM and played by Starfish with
  the video, because webOS 5 has no PulseAudio server. The PCM leads by 400 ms, as in the kit's backend.
- **Volume and mute** from the page reach Starfish (`setVolume`, `setAudioMute`). A muted video still builds the
  hardware chain, so unmuting later works.
- **Partly off screen**: the compositor advertises `wl_webos_foreign` at version 1 only, but on the tested TV (webOS 5.3) the `wl_webos_exported` object it makes accepts `set_crop_region` anyway (checked on the TV: the picture keeps its aspect, scrolls with the page and is cut at the screen edges). Direct mode uses it when a video reaches past the screen edge (a video only partly under the docked toolbar is not cut at all: the plane extends behind the opaque toolbar until it passes `y = 0`). On webOS 4 and older the request is not assumed to exist, so the picture is shrunk to the visible part (`set_exported_window`); the same happens when `WPE_BROWSER_DEBUG_VIDEO_CROP=squash` (legacy `WPEB_NO_CROP=1`) is set; `WPE_BROWSER_DEBUG_VIDEO_CROP=park` parks clipped videos for A/B comparison. Completely off-screen videos park at `(0, 0, 16, 9)` instead of sending out-of-bounds coordinates. The kit patch 0007 uses the same `set_crop_region` request in SHM mode.
- **Scrolling**: the plane is placed from the page's measured scroll motion and settles onto the hole when it stops.
  The plane-to-hole gap on a scroll test page fell from 10.6 px to 2.0 px on average.
- Navigating away, closing the tab or going home releases the decoder.

## Hybrid policy

The page script `engine/ubo/compat.js` decides per video element, before its player starts. Turn it off with
`WPE_BROWSER_DEBUG_HYBRID=0`. The decisions appear in `engine.log` as `hybrid video:` lines when
`WPE_BROWSER_DEBUG_HYBRID=log` or `WPE_BROWSER_DEBUG_CONSOLE=1` is set.

- **Software, in the page:** a muted video smaller than 960 by 540 CSS pixels, not fullscreen, with no controls, not
  protected. Previews and animated clips stay in the page and scroll with it. Patch 0010 supports this.
- **Hardware plane:** everything else: large boxes, controls, fullscreen, protected (DRM) content, and any video that
  is **not muted**, because software video has no audio output on webOS 5.
- A muted preview that gets unmuted is reloaded on the hardware plane at its media time.

## Fullscreen

WebKit's own fullscreen leaves the page black and the video plane at its old place, because the video layer is no longer
painted as a hole. So for a video that is on the hardware plane, or that the fullscreen rule moves there, and for any
element that contains such a video, `engine/ubo/compat.js` does not use WebKit's fullscreen. It emulates it:

- **A gesture is needed**, as for the real one: compat.js rejects a script without a user action, and only then tells the
  engine. The engine itself cannot check it (the message handler is reachable by any frame of the page).
- The element is stretched over the whole screen with fixed positioning and a black background. A `<video>` is
  scaled to fit. In a container, the videos inside it are made to fill it too, because the page's `:fullscreen` CSS
  cannot match an emulated fullscreen.
- **Ancestors that trap or hold down a fixed box give up those properties** while fullscreen: transform, translate,
  rotate, scale, filter, perspective, contain, container types, content-visibility, will-change, isolation, blend mode,
  clip-path, mask, opacity and stacking z-index. This reaches through shadow roots and slots. Their own transitions are
  switched off, so nothing slides. The page's scrolling is locked (`html` and `body` get `overflow: hidden`).
- The page sees a normal fullscreen: `document.fullscreenElement`, one `fullscreenchange` on entering and one on leaving,
  and `exitFullscreen`. Another element asking while one is fullscreen switches without an exit in between. The browser
  hides its toolbar. Back or Escape leaves.
- **A player in an iframe** works: the frame tells its parent, and the parent makes the `<iframe>` element cover the
  screen. This needs a user action and a frame that allows fullscreen (or one of the same origin).
- It ends by itself when its element leaves the document or the page navigates.
- On leave, **only the properties it wrote are put back**. A style the page changed meanwhile stays, and no `style`
  attribute is rewritten as text, so a strict style CSP does not get in the way. The scroll position is restored unless
  the page moved on.
- A video that stays in the page (an MSE stream that cannot be reloaded) keeps WebKit's own fullscreen. Going fullscreen is
  also one of the reasons that moves a video to the hardware plane (see the policy above), and leaving moves it back.

Limits: an overlay with the same or a higher `z-index` as the video, and anything in the browser's top layer (an open
modal dialog), can stay on top of it. A scrolling container inside the page (not the document) still scrolls under the
video. The black background was chosen so that letterbox bars are black; it has not been compared against a transparent
one for every kind of player.

## Media sources and the LG decoder

- **ManagedMediaSource is hidden from pages** (desktop Chrome has none). Players that prefer it attach through a `<source>`
  child of the video and remove the page's own `<source>`. A seek that reloads the source then ended on a stale source with
  nothing feeding it. Plain MediaSource works as well, so players use that.
- **Parameter sets and keyframes:** the sink puts the stream's parameter sets (SPS, PPS) in front of every keyframe unit.
  LG's decoder smeared macroblocks on a 1080p H.264 stream with B-frames when a unit began with them ahead of its
  own delimiter. The LG player now starts keyframe units at their access unit delimiter, and skips the leading copies
  only when the unit has its own. An MP4 stream often carries no parameter sets in band, and then the leading copies
  are the only ones and are kept. Checked on a test clip with screenshots, no smearing in the runs after the change.

## Known issues

- **A seek in one large streaming site's TV player** gave "pipeline error 100", smeared blocks, and no presented frames
  while units kept being fed. After it the decoder stayed busy until the TV restarted. Seen once; mid-stream decoder
  error recovery was subsequently added in `browser/media_server.cpp`.
- **Audio has not been heard.** It was checked in logs only: the load, PCM and volume calls and their return values.
- An unmuted short clip inside an article moves to the hardware plane, which does not scroll exactly with the page.
- One hardware video at a time. A second one loses its picture.
- Autoplay is allowed with sound in every tab (a TV page was opened on purpose, and a player whose `play()` is rejected tears
  itself down). `WPE_BROWSER_DEBUG_AUTOPLAY=policy` restores muted-only autoplay (except in tabs opened on YouTube); for tests.
- Test pages need a server with HTTP range support.
- The 64-bit engine has no hardware video ([engine64.md](engine64.md)).
- Memory: the page cache is off by default and the web and network processes have pressure limits (520 MB by
  default for each). The `appinfo.json` declares `requiredMemory` 1700.
