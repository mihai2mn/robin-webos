#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Robin's icon: one shape list, drawn twice. It writes the SVG master and rasterises the same shapes to every PNG the
app needs. No dependencies (standard library only), so the export is repeatable on any host.

    python3 tools/branding/make-icons.py            (from anywhere; run it again after editing SHAPES or ACCENT)

Writes: browser/branding/robin-icon.svg, browser/branding/robin-icon-512.png, browser/icon.png (80 px, appinfo icon),
browser/largeIcon.png (130 px, appinfo largeIcon), browser/icons/robin-mark.png (96 px, the home page header),
docs/screenshots/robin-icon.png (512 px, README).

The artwork is original: a flat, round-bodied robin with an orange-red breast on a dark rounded tile, drawn from circles,
an ellipse and a few polygons. Flat fills only (no gradients: nothing to band on an OLED). ACCENT is the breast colour and
the same value is the UI accent (browser/wren_ui.hpp kAccent, browser/chrome.hpp palette::kAccent).
"""
import math
import struct
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ACCENT = (0xF2, 0x69, 0x3A)      # robin breast orange-red, the UI accent
TILE = (0x0B, 0x10, 0x20)        # dark tile, as the app's ink
BACK = (0x8F, 0x7A, 0x66)        # back and head
WING = (0x5C, 0x49, 0x3A)
BEAK = (0xF4, 0xD0, 0x7A)
LEG = (0xE0, 0xA8, 0x45)
EYE = (0x08, 0x0A, 0x10)

# (kind, params..., colour). Coordinates on a 512 x 512 canvas, painted in order.
SHAPES = [
    ("rrect", 0, 0, 512, 512, 112, TILE),
    ("poly", [(330, 292), (456, 340), (446, 384), (322, 346)], WING),                 # tail
    ("line", 226, 400, 222, 452, 10, LEG), ("line", 270, 404, 270, 452, 10, LEG),      # legs
    ("line", 222, 452, 196, 456, 10, LEG), ("line", 270, 452, 244, 458, 10, LEG),      # feet
    ("circle", 250, 268, 152, BACK),                                                  # head and body
    ("circle", 204, 296, 100, ACCENT),                                                # breast and face
    ("ellipse", 300, 282, 74, 42, 24, WING),                                          # wing
    ("poly", [(108, 226), (50, 252), (110, 276)], BEAK),                              # beak
    ("circle", 158, 222, 17, EYE), ("circle", 152, 215, 5, (255, 255, 255)),           # eye
]


def covers(s, x, y):
    k = s[0]
    if k == "rrect":
        _, x0, y0, x1, y1, r, _c = s
        if not (x0 <= x <= x1 and y0 <= y <= y1):
            return False
        cx = min(max(x, x0 + r), x1 - r)
        cy = min(max(y, y0 + r), y1 - r)
        return (x - cx) ** 2 + (y - cy) ** 2 <= r * r
    if k == "circle":
        return (x - s[1]) ** 2 + (y - s[2]) ** 2 <= s[3] ** 2
    if k == "ellipse":
        _, cx, cy, rx, ry, deg, _c = s
        a = math.radians(deg)
        dx, dy = x - cx, y - cy
        u = dx * math.cos(a) + dy * math.sin(a)
        v = -dx * math.sin(a) + dy * math.cos(a)
        return (u / rx) ** 2 + (v / ry) ** 2 <= 1
    if k == "poly":
        pts = s[1]
        sign = 0
        for i in range(len(pts)):          # convex polygons only
            (x0, y0), (x1, y1) = pts[i], pts[(i + 1) % len(pts)]
            c = (x1 - x0) * (y - y0) - (y1 - y0) * (x - x0)
            if c != 0:
                if sign and (c > 0) != (sign > 0):
                    return False
                sign = 1 if c > 0 else -1
        return True
    if k == "line":
        _, x0, y0, x1, y1, w, _c = s
        dx, dy = x1 - x0, y1 - y0
        t = max(0.0, min(1.0, ((x - x0) * dx + (y - y0) * dy) / (dx * dx + dy * dy)))
        return (x - (x0 + t * dx)) ** 2 + (y - (y0 + t * dy)) ** 2 <= (w / 2) ** 2
    raise ValueError(k)


def bbox(s):
    k = s[0]
    if k == "rrect":
        return s[1], s[2], s[3], s[4]
    if k == "circle":
        return s[1] - s[3], s[2] - s[3], s[1] + s[3], s[2] + s[3]
    if k == "ellipse":
        r = max(s[3], s[4])
        return s[1] - r, s[2] - r, s[1] + r, s[2] + r
    if k == "poly":
        xs, ys = [p[0] for p in s[1]], [p[1] for p in s[1]]
        return min(xs), min(ys), max(xs), max(ys)
    w = s[5] / 2
    return min(s[1], s[3]) - w, min(s[2], s[4]) - w, max(s[1], s[3]) + w, max(s[2], s[4]) + w


def colour(s):
    return s[-1]


def render(size, ss):
    """RGBA bytes of the shapes at size x size, ss x ss samples per pixel."""
    scale = 512 / size
    acc = [[[0.0, 0.0, 0.0, 0.0] for _ in range(size)] for _ in range(size)]   # premultiplied
    for s in SHAPES:
        c = colour(s)
        bx0, by0, bx1, by1 = bbox(s)
        for py in range(max(0, int(by0 / scale) - 1), min(size, int(by1 / scale) + 2)):
            for px in range(max(0, int(bx0 / scale) - 1), min(size, int(bx1 / scale) + 2)):
                n = 0
                for j in range(ss):
                    y = (py + (j + 0.5) / ss) * scale
                    for i in range(ss):
                        if covers(s, (px + (i + 0.5) / ss) * scale, y):
                            n += 1
                if not n:
                    continue
                a = n / (ss * ss)
                p = acc[py][px]
                p[0] = c[0] * a + p[0] * (1 - a)
                p[1] = c[1] * a + p[1] * (1 - a)
                p[2] = c[2] * a + p[2] * (1 - a)
                p[3] = a + p[3] * (1 - a)
    out = bytearray()
    for row in acc:
        for r, g, b, a in row:
            if a <= 0:
                out += b"\0\0\0\0"
            else:
                out += bytes((round(r / a), round(g / a), round(b / a), round(a * 255)))
    return bytes(out)


def png(size, rgba):
    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    raw = b"".join(b"\0" + rgba[y * size * 4:(y + 1) * size * 4] for y in range(size))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def hexc(c):
    return "#%02X%02X%02X" % c


def svg():
    o = ['<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 512 512" width="512" height="512">',
         "<!-- Robin app icon. Generated by tools/branding/make-icons.py from its shape list: edit that, not this file. -->"]
    for s in SHAPES:
        k, c = s[0], hexc(colour(s))
        if k == "rrect":
            o.append(f'<rect x="{s[1]}" y="{s[2]}" width="{s[3] - s[1]}" height="{s[4] - s[2]}" rx="{s[5]}" fill="{c}"/>')
        elif k == "circle":
            o.append(f'<circle cx="{s[1]}" cy="{s[2]}" r="{s[3]}" fill="{c}"/>')
        elif k == "ellipse":
            o.append(f'<ellipse cx="{s[1]}" cy="{s[2]}" rx="{s[3]}" ry="{s[4]}" transform="rotate({s[5]} {s[1]} {s[2]})" fill="{c}"/>')
        elif k == "poly":
            o.append('<polygon points="%s" fill="%s"/>' % (" ".join(f"{x},{y}" for x, y in s[1]), c))
        else:
            o.append(f'<line x1="{s[1]}" y1="{s[2]}" x2="{s[3]}" y2="{s[4]}" stroke="{c}" stroke-width="{s[5]}" stroke-linecap="round"/>')
    o.append("</svg>")
    return "\n".join(o) + "\n"


OUTPUTS = [("browser/branding/robin-icon-512.png", 512), ("docs/screenshots/robin-icon.png", 512),
           ("browser/largeIcon.png", 130), ("browser/icon.png", 80), ("browser/icons/robin-mark.png", 96)]

if __name__ == "__main__":
    (ROOT / "browser/branding/robin-icon.svg").write_text(svg(), encoding="utf-8", newline="\n")
    cache = {}
    for rel, size in OUTPUTS:
        if size not in cache:
            cache[size] = png(size, render(size, 3 if size >= 400 else 5))
        (ROOT / rel).write_bytes(cache[size])
        print(f"{rel}: {size} x {size}, {len(cache[size])} bytes")
