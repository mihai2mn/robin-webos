# Using Robin

## The remote

Robin works with the arrows, OK and Back of any LG remote; colour keys are optional shortcuts.

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

**TV mode:** on TV web apps (a `tv.` host or a `/tv` path, such as YouTube's TV app) Robin hides its panel and
progress line so the page fills the screen. Hold OK → **Toolbar** brings the tabs back. youtube.com links open
YouTube's TV app; the engine variable `WPE_BROWSER_DESKTOP_YOUTUBE=1` keeps the desktop site.

## Media keys

The remote's media keys act on the page's **main video**, so they work with players that have no remote support of
their own. Robin picks the video like this: the one inside the fullscreen element if there is one; otherwise, among the
videos with media (also in same-origin iframes and open shadow roots), a playing video that is not a small muted
preview first, then a visible, unmuted or large one; an `<audio>` element only if the page has no video. A seek stops at the
start and at the end (or the seekable range of a live stream). The space bar of a
keyboard is never a media key: it stays with the page.

## Zoom per site

Hold OK → **Zoom** (or Menu → zoom) changes the zoom of the **site you are on**, and Robin remembers it for that host.
The panel says "Remembered for <host>". Pages without a host (the home page) use the default zoom, which is shown as
"Default for sites without their own zoom". The zoom is applied again when a page of that site loads, in any tab.

The zooms are stored in `zooms.json` in the profile folder (up to 500 sites). Because they name the sites you visited,
**clearing all history also forgets them**.

## Launch parameters

`ares-launch -d <device> io.github.mihai2mn.robin -p '<json>'`:

| Parameter | Effect |
|---|---|
| `{"target": "https://…"}` | open this URL |
| `{"wpebRenderer": "gpu" \| "software"}` | GPU (default) or software rendering (needs an engine built with `STAGE_MESA=1`) |
| `{"wpebPresent": "direct" \| "shm"}` | draw pages straight to the screen (default) or via shared memory |
| `{"wpebGlibc": "bundled" \| "system"}` | force the engine's bundled glibc or the TV's (default: bundled only where the TV's glibc is older than 2.28) |
| `{"wpebShow": "home" \| "panel" \| "quick" \| "side" \| "zoom" \| "tabs"}` | open that screen a few seconds after start (screenshots, tests) |
| `{"wpebShmVideo": "1"}` | opt into hardware video when running with `{"wpebPresent": "shm"}` |
| `{"wpebVideoBackend": "starfish" \| "ndl"}` | select the hardware video backend (default: `starfish`, with `ndl` fallback) |
| `{"wpebEnv": {"NAME": "value"}}` | allowlisted engine environment prefixes: `WEBKIT_*` (excluding `WEBKIT_EXEC_PATH` and `WEBKIT_INJECTED_BUNDLE_PATH`), `JSC_*`, `WPE_BROWSER_DEBUG*` (e.g. `WPE_BROWSER_DEBUG_CONSOLE=1`, `WPE_BROWSER_DEBUG_ENGINE=64`), `GST_DEBUG*`, `G_TLS_*`, `MSE_*`. An app id ending in `.dev` passes all of them. A release app id passes only the harmless ones (`JSC_*`, `MSE_*`, `WPE_BROWSER_DEBUG_CONSOLE`, `_AUTOPLAY`, `_ENGINE`, `_MESSAGES`, `_REQUESTS`, `_FRAMES`, `_UBO_LOG`, `_MEDIA_AREA`; see `browser/engine_env.hpp`) unless the file `<data folder>/dev-switches` exists (create it with a shell on the TV; a launch parameter cannot). Names it refuses are logged. |

## Logs

On the TV, in `/tmp/io.github.mihai2mn.robin/` (cleared when the TV restarts):

- `engine.log`: the web engine, this run; `engine.prev.log`: the run before (the crash, if the app was reopened)
- `events.log`: the interface

**Settings → Help and feedback → Save logs to USB** writes all three to a USB drive as `robin-logs-<date>.txt`, with
query strings and fragments cut from web addresses. On a rooted TV they can also be fetched over SSH:

```sh
ssh root@<tv-ip> 'cd /tmp/io.github.mihai2mn.robin && tail -c 1500000 engine.log engine.prev.log events.log' > robin-logs.txt
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
