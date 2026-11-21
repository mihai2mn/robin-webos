# Using Wren

## The remote

Wren works with the arrows, OK and Back of any LG remote; colour keys are optional shortcuts.

| Key | Does |
|---|---|
| Arrows | move between links and controls on the page (WebKit spatial navigation); with nothing to move to, ▲ ▼ scroll the page |
| ▲ at the top of a page | the glass panel: the tab row above the toolbar; ◀ ▶ and OK switch tabs |
| **Hold OK** | quick actions (new tab, bookmark, tabs, reload, close tab, home, settings, toolbar) and page zoom |
| CH + / − | previous / next tab |
| Play / Pause / Stop / Rewind / Fast-forward | control the page's main video: play/pause toggles, Pause pauses, Stop pauses and goes back to the start, Rewind and Fast-forward seek −10 s / +10 s|
| Back | back in the page's history, or to the panel; in the panel, back to the page |
| Menu | side panel: bookmarks, history, zoom, settings, exit |
| Red / Green / Yellow / Blue | all tabs / side panel / quick actions / new tab |

The Magic Remote pointer works as a mouse and its wheel scrolls.

The panel stays at the top and the page starts below it, as in a desktop browser. Full-screen video and TV apps (see
below) take the whole screen; there the panel comes back with Hold OK → **Toolbar**, or by resting the pointer at the
top edge.

**TV mode:** on TV web apps (a `tv.` host or a `/tv` path, such as YouTube's TV app) Wren hides its panel and
progress line so the page fills the screen. Hold OK → **Toolbar** brings the tabs back. youtube.com links open
YouTube's TV app; the engine variable `WPE_BROWSER_DESKTOP_YOUTUBE=1` keeps the desktop site.

## Media keys

The remote's media keys act on the page's **main video**, so they work with players that have no remote support of
their own. Wren picks the video like this: the one inside the fullscreen element if there is one; otherwise, among the
videos with media (also in same-origin iframes and open shadow roots), a playing video that is not a small muted
preview first, then a visible, unmuted or large one; an `<audio>` element only if the page has no video. A seek stops at the
start and at the end (or the seekable range of a live stream). The space bar of a
keyboard is never a media key: it stays with the page.

## Zoom per site

Hold OK → **Zoom** (or Menu → zoom) changes the zoom of the **site you are on**, and Wren remembers it for that host.
The panel says "Remembered for <host>". Pages without a host (the home page) use the default zoom, which is shown as
"Default for sites without their own zoom". The zoom is applied again when a page of that site loads, in any tab.

The zooms are stored in `zooms.json` in the profile folder (up to 500 sites). Because they name the sites you visited,
**clearing all history also forgets them**.

## Launch parameters

`ares-launch -d <device> org.sostk.wpebrowser -p '<json>'`:

| Parameter | Effect |
|---|---|
| `{"target": "https://…"}` | open this URL |
| `{"wpebRenderer": "gpu" \| "software"}` | GPU (default) or software rendering (needs an engine built with `STAGE_MESA=1`) |
| `{"wpebPresent": "direct" \| "shm"}` | draw pages straight to the screen (default) or via shared memory |
| `{"wpebGlibc": "bundled" \| "system"}` | force the engine's bundled glibc or the TV's (default: bundled only where the TV's glibc is older than 2.28) |
| `{"wpebShow": "home" \| "panel" \| "quick" \| "side" \| "zoom" \| "tabs"}` | open that screen a few seconds after start (screenshots, tests) |
| `{"wpebEnv": {"NAME": "value"}}` | environment for the engine, e.g. `WPE_BROWSER_DEBUG_CONSOLE=1` (page console, EME/MSE calls and media events to `engine.log`), `WPE_BROWSER_DESKTOP_YOUTUBE=1`, `GST_DEBUG`, `WEBKIT_*`, `WPEB_HW_OPUS=1` (experimental hardware Opus) |

## Logs

On the TV, in `/tmp/org.sostk.wpebrowser/` (cleared when the TV restarts):

- `engine.log`: the web engine, this run; `engine.prev.log`: the run before (the crash, if the app was reopened)
- `events.log`: the interface

**Settings → Help and feedback → Save logs to USB** writes all three to a USB drive as `wren-logs-<date>.txt`, with
query strings and fragments cut from web addresses. On a rooted TV they can also be fetched over SSH:

```sh
ssh root@<tv-ip> 'cd /tmp/org.sostk.wpebrowser && tail -c 1500000 engine.log engine.prev.log events.log' > wren-logs.txt
```

The logs list the addresses of pages you visited; read them before posting them publicly.

## Driving the TV from a PC (developers)

`tools/tvdrive/` drives the dev copy of the app the way a person with the remote does, and reads back what happened.
It needs a rooted TV and an ssh host. It is for testing, not for users. Details are in
[tools/tvdrive/README.md](../tools/tvdrive/README.md). In short:

| Tool | What it gives |
|---|---|
| `tvdrive shot`, `open`, `click`, `scroll`, `key`, `type`, `log`, `state` | screenshots and input: pointer, wheel, remote keys, the on-screen keyboard |
| `tvdrive frames [seconds]` | a short burst of screen captures reduced to text: static stretches, changing stretches, and flicker (the screen alternating among a few pictures). It sees faults that no page element explains |
| `motion-recorder.js` and `motion.js` | a recorder that runs in the page (`WPE_BROWSER_DEBUG_SCRIPT`) and samples element boxes, opacity and scroll on every frame; the analyzer turns the log into text: holds, moves (duration, easing, speed), jumps, reversals, long frames, and an optional SVG timeline. Layout motion only |

Running the frame burst slightly delays the screen (worst frame gap 31 to 35 ms instead of 24 ms in the one test made).
The tools were tried on one TV. The motion analyzer was first tested with a simulated log, then on the TV.
