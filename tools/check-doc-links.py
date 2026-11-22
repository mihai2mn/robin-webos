#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Checks that every relative link and image in the repository's Markdown files points at a file or folder that
exists, and that a #fragment names a heading of the target Markdown file.

    python3 tools/check-doc-links.py        (run from anywhere; exits 1 if a link is broken)
"""
import re
import subprocess
import sys
import urllib.parse
from pathlib import Path

root = Path(subprocess.check_output(["git", "rev-parse", "--show-toplevel"], text=True, cwd=Path(__file__).resolve().parent).strip())
files = [root / f for f in subprocess.check_output(["git", "ls-files", "*.md"], text=True, cwd=root).split()
         if not f.startswith("third_party/")]
LINK = re.compile(r"!?\[[^\]]*\]\(([^)\s]+)(?:\s+\"[^\"]*\")?\)|<img[^>]*\ssrc=\"([^\"]+)\"")
FENCE = re.compile(r"^```")


def anchors(path):
    out = set()
    fence = False
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if FENCE.match(line):
            fence = not fence
        m = None if fence else re.match(r"#+\s+(.*?)\s*#*$", line)
        if m:
            t = re.sub(r"[`*_]", "", m.group(1)).lower()
            t = re.sub(r"[^\w\- ]", "", t).replace(" ", "-")
            out.add(t)
    return out


bad = 0
total = 0
for f in files:
    fence = False
    for n, line in enumerate(f.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
        if FENCE.match(line):
            fence = not fence
        if fence:
            continue
        for m in LINK.finditer(line):
            target = m.group(1) or m.group(2)
            if re.match(r"^(https?:|mailto:)", target):
                continue
            total += 1
            path, _, frag = target.partition("#")
            path = urllib.parse.unquote(path)
            dest = (f.parent / path).resolve() if path else f
            if not dest.exists():
                print(f"{f.relative_to(root)}:{n}: missing {target}")
                bad += 1
            elif frag and dest.suffix == ".md" and frag.lower() not in anchors(dest):
                print(f"{f.relative_to(root)}:{n}: no heading #{frag} in {dest.relative_to(root)}")
                bad += 1
print(f"{total} relative links in {len(files)} Markdown files, {bad} broken")
sys.exit(1 if bad else 0)
