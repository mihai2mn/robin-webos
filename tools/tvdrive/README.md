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
