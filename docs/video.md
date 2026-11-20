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
- **Crop**: a video partly off screen or under the toolbar is cut with the exported window's crop region, not squashed.
- **Scrolling**: the plane is placed from the page's measured scroll motion and settles onto the hole when it stops.
  The plane-to-hole gap on a scroll test page fell from 10.6 px to 2.0 px on average.
- Navigating away, closing the tab or going home releases the decoder.

## Hybrid policy

The page script `engine/ubo/compat.js` decides per video element, before its player starts. Turn it off with
`WPE_BROWSER_DEBUG_HYBRID=0`. The decisions appear in `engine.log` as `hybrid video:` lines.

- **Software, in the page:** a muted video smaller than 960 by 540 CSS pixels, not fullscreen, with no controls, not
  protected. Previews and animated clips stay in the page and scroll with it. Patch 0010 supports this.
- **Hardware plane:** everything else: large boxes, controls, fullscreen, protected (DRM) content, and any video that
  is **not muted**, because software video has no audio output on webOS 5.
- A muted preview that gets unmuted is reloaded on the hardware plane at its media time.

## Known issues

- **Audio has not been heard.** It was checked in logs only: the load, PCM and volume calls and their return values.
- An unmuted short clip inside an article moves to the hardware plane, which does not scroll exactly with the page.
- One hardware video at a time. A second one loses its picture.
- Autoplay is allowed only for muted videos (WebKit policy). An unmuted `autoplay` video waits for a user action.
  `WPE_BROWSER_DEBUG_AUTOPLAY=allow` is for tests.
- Test pages need a server with HTTP range support.
- The 64-bit engine has no hardware video ([engine64.md](engine64.md)).
- Memory: the page cache is off by default and the web and network processes have pressure limits (420 MB by
  default for the network process). The `appinfo.json` declares `requiredMemory` 450.
