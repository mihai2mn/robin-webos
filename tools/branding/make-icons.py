#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Writes every Robin icon size from the 1024 px master, browser/branding/robin-icon-1024.png (the raster of
robin-icon.svg). Needs Pillow (pip install pillow). Downscaling is LANCZOS; each icon gets a rounded-square mask
(corner radius 22% of the side, transparent outside).

    python3 tools/branding/make-icons.py        (from anywhere)

Writes: browser/branding/robin-icon-512.png, browser/icon.png (80 px, appinfo icon), browser/largeIcon.png (130 px, appinfo
largeIcon), browser/icons/robin-mark.png (96 px, the home page mark), docs/screenshots/robin-icon.png (512 px, README).
The artwork is not drawn here: see NOTICE.md for where it comes from.
"""
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[2]
MASTER = ROOT / "browser/branding/robin-icon-1024.png"
RADIUS = 0.22
OUTPUTS = [("browser/branding/robin-icon-512.png", 512), ("docs/screenshots/robin-icon.png", 512),
           ("browser/largeIcon.png", 130), ("browser/icon.png", 80), ("browser/icons/robin-mark.png", 96)]


def rounded(img):
    """img with a rounded-square alpha mask, drawn at 4x and reduced so the edge is smooth."""
    s = img.width
    mask = Image.new("L", (s * 4, s * 4), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, s * 4 - 1, s * 4 - 1), radius=int(s * 4 * RADIUS), fill=255)
    out = img.convert("RGBA")
    out.putalpha(mask.resize((s, s), Image.LANCZOS))
    return out


if __name__ == "__main__":
    master = Image.open(MASTER).convert("RGB")
    assert master.size == (1024, 1024), master.size
    for rel, size in OUTPUTS:
        icon = rounded(master.resize((size, size), Image.LANCZOS))
        icon.save(ROOT / rel, optimize=True)
        print(f"{rel}: {size} x {size}")
