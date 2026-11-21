# tvdrive

Drive the dev copy of the browser on an LG webOS TV the way a person with the remote does, and take screenshots.
One bash script (`tvdrive`) plus a small Python helper that runs on the TV (`tvinput.py`, piped over ssh).
Works from Git Bash and WSL. Needs `ssh`/`scp`/`base64`/`timeout`; `ffmpeg` is optional (small screenshot copies).

Setup: an ssh host called `lgtv` (root on a rooted TV with `/opt/bin/tv` and python3 on the TV). Override with
`TVDRIVE_HOST`, `TVDRIVE_APP` (default `org.sostk.wpebrowser.dev`; the script refuses the release app id),
`TVDRIVE_OUT` (screenshot folder, default `./tvdrive-out`).

| command | what it does |
|---|---|
| `shot [file]` | screenshot (1920x1080 PNG) plus `<name>-small.jpg` (640 px) for cheap reading |
| `open <url> [--fresh] [--wait S]` | open a URL in the app (`--fresh` closes it first) |
| `click x y` / `move x y` | pointer to screen pixel x,y (absolute), then click |
| `nudge dx dy` | relative pointer move (about 3.1 px per unit) |
| `scroll dy [x y]` | wheel: positive = down, one unit is about 60 px; default pointer spot 960 600 |
| `key name...` | remote keys: up down left right ok back home red green yellow blue ch+ ch- play pause stop rew ff info 0-9 |
| `type "text"` | text into the focused field, by clicking LG's on-screen keyboard |
| `log [n]` / `log --follow` | tail the app's events.log and engine.log |
| `state` | foreground app, memory/swap, web process sizes, lock owner |
| `lock <owner>` / `unlock <owner>` | the shared dev-app lock; `close` closes the app |
| `frames [seconds] [--fps N]` | what the screen did over a short capture burst: static / changing / FLICKER stretches (see Screen traces) |

## How it works (and what to know)
- Pointer, wheel and remote keys go through LG's network-input service: the unix socket
  `/tmp/netinput.pointer.sock` (created on demand by a luna call that the helper makes). A packet is a 4-byte big-endian
  length plus text, e.g. `type:move\ndx:5\ndy:5\ndown:0\n\n`, `type:click\n\n`, `type:scroll\ndx:0\ndy:1\n\n`
  (dy +1 is UP), `type:button\nname:ENTER\n\n`.
- Moves are relative, and the TV hides an idle pointer and re-shows it at the screen centre. So an absolute move
  slams into the bottom-right corner (it clamps there) and walks back with small steps in ONE connection:
  3.11 screen px per unit, accuracy about +-15 px. Aim at the middle of a target, not its edge.
- `type` clicks the keys of the on-screen keyboard (it opens when a page field is focused: click the field first, and
  `key back` closes it). Supported: letters (shift is clicked for capitals), digits, space . , @ ? and `\n` (Enter), `\b` (backspace).
- Digits and arrow keys reach the page as key events; red/green/yellow/blue/back/home are the app's or the system's own.
  Red opens the tab list, blue a new tab, back leaves overlays and goes back in history, home leaves the app.
- Not working: `drag` (the socket sends no button press, so no text selection or slider drags). Screenshots do not
  show the pointer or protected video (Netflix etc. appear black).
- The pointer's recent positions and clicks are in the app's events.log (`[pointer]` lines), handy to check where a click landed.

Proof run (Wikipedia article: open, scroll, click a link, back, type into the search box): `proof/`.

## Motion traces (animation as text)
Screenshots at about 1 fps cannot show an animation. `motion-recorder.js` runs in the page (launch parameter
`wpebEnv: {"WPE_BROWSER_DEBUG_SCRIPT": "<path on the TV>/motion-recorder.js", "WPE_BROWSER_DEBUG_CONSOLE": "1"}`) and
samples, every rendered frame, the boxes and opacity of what can move by itself (fixed/sticky elements, running
animations, videos, `window.__motionSelectors`) plus the scroll offset; `window.__motionRecorder.mark("label")` puts a
label on the timeline. `node motion.js engine.log [--svg out.svg] [--from ms] [--to ms]` turns the log into text:
per element and property the holds, moves (duration, easing, speed) and one-frame jumps, direction reversals
(three within a second is reported as FLICKER), and frame gaps with the long frames. It sees layout motion only:
a picture that flickers without any element moving (a compositing fault) is not in it.

## Screen traces (`tvdrive frames`)
`tvdrive frames [seconds] [--fps N]` captures the real screen in a short burst (default 4 s at 10 per second, at most
25; video plane and toolbar included) and prints what it did: static stretches, changing stretches (with how many
different pictures), and FLICKER (the screen alternating among a few pictures), each with the screen region. It
complements the motion trace: that one knows which element moved and how, this one sees faults no element explains.
Cost on the TV while it runs (measured with the motion recorder on a 60 fps page): median and p95 frame gap unchanged
(17 / 18 ms), worst gap 31-35 ms instead of 24 ms. A busy TV gives fewer captures than asked for.

## Selftest
`node selftest.js` checks `motion.js` and `frames.js` on synthetic inputs in a temp folder (no TV, no network): one
`ok <case>` or `FAIL <case>` line per case, exit code 1 if any case fails; `node selftest.js frames` runs only the
matching cases.
