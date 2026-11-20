# Runs ON the TV (python3). Usage: python3 - CMD ARGS...   (script is piped via ssh stdin)
# Sends LG network-input events (pointer socket: 4-byte big-endian length + text).
import socket, struct, sys, time, os, subprocess, base64
SOCK = '/tmp/netinput.pointer.sock'
POS = '/tmp/tvdrive.pos'
W, H = 1920, 1080

def conn():
    if not os.path.exists(SOCK):  # the service creates its sockets on first luna call
        subprocess.call(['luna-send', '-n', '1', 'luna://com.webos.service.networkinput/getPointerInputSocket', '{}'],
                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(0.5)
    c = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    c.settimeout(3)
    c.connect(SOCK)
    return c

def send(c, text):
    b = text.encode()
    c.sendall(struct.pack('>I', len(b)) + b)
    time.sleep(DELAY)

def rel(c, dx, dy, down=0):
    # split into small steps so pointer acceleration/clamping stays predictable
    while dx or dy:
        sx = max(-STEP, min(STEP, dx)); sy = max(-STEP, min(STEP, dy))
        send(c, 'type:move\ndx:%d\ndy:%d\ndown:%d\n\n' % (sx, sy, down))
        dx -= sx; dy -= sy

STEP = int(os.environ.get('TVD_STEP', '10'))
DELAY = float(os.environ.get('TVD_DELAY', '0.03'))

BRX, BRY = 1908, 1070       # where the pointer rests after slamming into the bottom-right corner (screen px)
G = float(os.environ.get('TVD_GAIN', '3.11'))   # screen px per unit of dx/dy (measured)

def absmove(c, x, y, down=0):
    # Slam into the bottom-right corner (it clamps there), then walk back with small steps.
    # Must happen in ONE connection: after the pointer idles the TV hides it and it re-appears at the screen centre.
    for _ in range(30):
        send(c, 'type:move\ndx:40\ndy:40\ndown:%d\n\n' % down)
    time.sleep(0.15)
    rel(c, -int(round((BRX - x) / G)), -int(round((BRY - y) / G)), down)

a = sys.argv[1:]
cmd = a[0]
c = conn()
if cmd == 'move':       rel(c, int(a[1]), int(a[2]))
elif cmd == 'abs':      absmove(c, int(a[1]), int(a[2]))
elif cmd == 'click':
    if len(a) > 2: absmove(c, int(a[1]), int(a[2])); time.sleep(0.2)
    send(c, 'type:click\n\n')
elif cmd == 'scroll':
    dy = int(a[1])
    if len(a) > 3: absmove(c, int(a[2]), int(a[3])); time.sleep(0.2)
    n = abs(dy); s = -1 if dy > 0 else 1   # the wheel's dy>0 is UP; tvdrive's scroll dy>0 means DOWN
    for _ in range(n): send(c, 'type:scroll\ndx:0\ndy:%d\n\n' % s); time.sleep(0.05)
elif cmd == 'drag':
    x1, y1, x2, y2 = map(int, a[1:5])
    absmove(c, x1, y1); time.sleep(0.2)
    send(c, 'type:move\ndx:0\ndy:0\ndown:1\n\n')
    rel(c, int(round((x2 - x1) / G)), int(round((y2 - y1) / G)), 1)
    send(c, 'type:move\ndx:0\ndy:0\ndown:0\n\n')
elif cmd == 'key':
    for k in a[1:]:
        send(c, 'type:button\nname:%s\n\n' % k); time.sleep(0.15)
elif cmd == 'type':
    # Clicks the keys of LG's on-screen keyboard (it opens when a page field is focused), laid out as on webOS 5:
    # columns 147 px wide from x=287 (col 0 = a/q/1...), rows at y=821 (digits), 873 (qwerty), 925 (asdf), 978 (zxcv), 1030 (space).
    text = base64.b64decode(a[1]).decode('utf-8')
    X0, DX = 287, 147
    ROW = {'1234567890': (821, 0), 'qwertyuiop': (873, 0), 'asdfghjkl': (925, 0), 'zxcvbnm': (978, 1)}
    SPECIAL = {' ': (947, 1030), '\n': (1788, 873), '\b': (1788, 821), '@': (287, 978), ',': (1462, 978), '.': (1609, 978), '?': (1609, 925)}
    SHIFT = (120, 978)
    def keypos(ch):
        if ch in SPECIAL: return SPECIAL[ch]
        for chars, (y, off) in ROW.items():
            if ch in chars: return (X0 + DX * (chars.index(ch) + off), y)
        return None
    cur = None; n = 0
    def goto(p):
        global cur, n
        if cur is None or n >= 8:
            absmove(c, p[0], p[1]); n = 0
        else:
            rel(c, int(round((p[0] - cur[0]) / G)), int(round((p[1] - cur[1]) / G)))
        cur = p; n += 1
        time.sleep(0.12)
    for ch in text:
        low = ch.lower()
        p = keypos(low)
        if p is None: sys.stderr.write('tvdrive type: no key for %r, skipped\n' % ch); continue
        if ch != low:
            goto(SHIFT); send(c, 'type:click\n\n'); time.sleep(0.25)
        goto(p); send(c, 'type:click\n\n'); time.sleep(0.2)
elif cmd == 'raw':
    send(c, a[1].replace('\\n', '\n'))
time.sleep(0.4)
c.close()
