#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Refreshes the uBO-grade ad blocker's inputs (before a release); tools/ubo/build.sh compiles them.

    update.py [UBO_COMMIT]

- lists/: uBlock Origin's default filter lists (the ones uBO enables on a fresh install), with `!#include`
  resolved and `!#if` evaluated for Wren (WebKit, no HTML filtering), so the compiler sees plain lists.
- ubo/: from uBlock Origin's repository at UBO_COMMIT (default: master's head, recorded in ubo/COMMIT): the redirect
  resources (web_accessible_resources/, js/redirect-resources.js) and the scriptlets (js/resources/*.js).
Licences: uBO and its filters GPL-3.0-or-later; EasyList/EasyPrivacy GPL-3.0-or-later or CC BY-SA 3.0; Peter
Lowe's list (pgl.yoyo.org) and URLhaus (malware-filter, CC0) as their sites state.
"""
import io
import json
import re
import shutil
import sys
import tarfile
import urllib.parse
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
UA = {"User-Agent": "wren-ubo-update"}

# uBO's defaults (assets/assets.json, entries without "off"). `trusted`: uBO's own lists, which uBO lets use
# trusted scriptlets. `hosts`: hosts-file format.
LISTS = [
    ("ublock-filters", "https://ublockorigin.github.io/uAssets/filters/filters.txt", "trusted"),
    ("ublock-badware", "https://ublockorigin.github.io/uAssets/filters/badware.txt", "trusted"),
    ("ublock-privacy", "https://ublockorigin.github.io/uAssets/filters/privacy.txt", "trusted"),
    ("ublock-unbreak", "https://ublockorigin.github.io/uAssets/filters/unbreak.txt", "trusted"),
    ("ublock-quick-fixes", "https://ublockorigin.github.io/uAssets/filters/quick-fixes.txt", "trusted"),
    ("easylist", "https://ublockorigin.github.io/uAssets/thirdparties/easylist.txt", ""),
    ("easyprivacy", "https://ublockorigin.github.io/uAssets/thirdparties/easyprivacy.txt", ""),
    ("urlhaus", "https://malware-filter.gitlab.io/urlhaus-filter/urlhaus-filter-vivaldi-online.txt", ""),
    ("plowe", "https://pgl.yoyo.org/adservers/serverlist.php?hostformat=hosts&showintro=1&mimetype=plaintext", "hosts"),
]

# uBO's `!#if` tokens, as uBO itself would set them in a WebKit browser with no HTML filtering.
TOKENS = {
    "ext_ublock": True, "ext_ubol": False, "ext_abp": False, "adguard": False, "ext_devbuild": False,
    "env_chromium": False, "env_edge": False, "env_firefox": False, "env_safari": True, "env_mobile": False,
    "env_mv3": False, "cap_html_filtering": False, "cap_user_stylesheet": True, "cap_ipaddress": False,
    "false": False, "true": True,
}


def fetch(url):
    with urllib.request.urlopen(urllib.request.Request(url, headers=UA), timeout=60) as r:
        return r.read()


def condition(expr):
    """Evaluates a `!#if` expression: tokens, !, &&, ||, parentheses. Unknown tokens are false, as in uBO."""
    py = re.sub(r"[A-Za-z_][\w]*", lambda m: str(TOKENS.get(m.group(0), False)), expr)
    py = py.replace("&&", " and ").replace("||", " or ").replace("!", " not ")
    try:
        return bool(eval(py, {"__builtins__": {}}, {}))
    except Exception:
        return False


def resolve(url, depth=0):
    """A list's lines with `!#include` inlined (same site only, as uBO does) and `!#if`/`!#else` applied."""
    text = fetch(url).decode("utf-8", "replace")
    out, stack = [], []  # stack of "is this branch live"
    for line in text.splitlines():
        s = line.strip()
        if s.startswith("!#if "):
            stack.append(condition(s[5:]))
            continue
        if s == "!#else":
            if stack:
                stack[-1] = not stack[-1]
            continue
        if s == "!#endif":
            if stack:
                stack.pop()
            continue
        if not all(stack):
            continue
        if s.startswith("!#include "):
            inc = urllib.parse.urljoin(url, s[len("!#include "):].strip())
            if depth < 3 and urllib.parse.urlparse(inc).netloc == urllib.parse.urlparse(url).netloc:
                out.append(f"! included: {inc}")
                out.extend(resolve(inc, depth + 1))
            continue
        out.append(line)
    return out


def update_lists():
    d = HERE / "lists"
    d.mkdir(exist_ok=True)
    manifest = []
    for name, url, flags in LISTS:
        lines = resolve(url)
        (d / f"{name}.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
        version = next((l.split(":", 1)[1].strip() for l in lines[:80]
                        if l.lower().startswith(("! version:", "# version:", "! last modified:", "# last modified:", "! updated:"))), "?")
        print(f"{name}: {len(lines)} lines, version {version}")
        manifest.append({"name": name, "url": url, "flags": flags, "version": version})
    (d / "lists.json").write_text(json.dumps(manifest, indent=1) + "\n")


def update_ubo(commit):
    if commit is None:
        commit = json.loads(fetch("https://api.github.com/repos/gorhill/uBlock/commits/master"))["sha"]
    tgz = fetch(f"https://codeload.github.com/gorhill/uBlock/tar.gz/{commit}")
    d = HERE / "ubo"
    shutil.rmtree(d, ignore_errors=True)
    # All of src/js: the scriptlet modules import helpers from it (urlskip.js, ...).
    wanted = ("src/web_accessible_resources/", "src/js/", "LICENSE.txt")
    with tarfile.open(fileobj=io.BytesIO(tgz)) as tar:
        for m in tar.getmembers():
            rel = m.name.split("/", 1)[1] if "/" in m.name else ""
            if not m.isfile() or not rel.startswith(wanted):
                continue
            target = d / rel.removeprefix("src/")
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(tar.extractfile(m).read())
    (d / "COMMIT").write_text(commit + "\n")
    print(f"uBO resources: {commit}")


if __name__ == "__main__":
    update_lists()
    update_ubo(sys.argv[1] if len(sys.argv) > 1 else None)
