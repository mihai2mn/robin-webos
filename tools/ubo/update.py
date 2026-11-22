#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Refreshes the uBO-grade ad blocker's inputs (before a release); tools/ubo/build.sh compiles them.

    update.py [UBO_COMMIT]

- lists/: uBlock Origin's default filter lists (the ones uBO enables on a fresh install), with `!#include`
  resolved and `!#if` evaluated for Robin (WebKit, no HTML filtering), so the compiler sees plain lists.
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

# Extra lists, each compiled into its own engine (lists/<name>.dat) that wren://adblock turns on and off. uBO's
# Unbreak and Quick fixes go into each one too, so their exceptions still apply. (name, url, flags, on by default,
# title, what it does). On by default: the extra lists commonly recommended on top of uBO's defaults.
OPTIONAL = [
    ("adguard-url-tracking", "https://ublockorigin.github.io/uAssets/filters/privacy-removeparam.txt", "trusted", True,
     "AdGuard URL Tracking Protection", "Removes tracking parameters (utm_, fbclid, ...) from links"),
    ("ublock-annoyances", "https://ublockorigin.github.io/uAssets/filters/annoyances.txt", "trusted", True,
     "uBlock filters – Annoyances", "Pop-ups, overlays, 'turn off your ad blocker' nags and other clutter"),
    ("cookie-notices", "https://ublockorigin.github.io/uAssets/filters/annoyances-cookies.txt", "trusted", True,
     "Cookie notices (EasyList/uBO)", "Hides cookie banners and consent pop-ups"),
    ("easylist-social", "https://ublockorigin.github.io/uAssets/thirdparties/easylist-social.txt", "", True,
     "EasyList – Social widgets", "Share buttons and social media embeds"),
    ("easylist-annoyances", "https://ublockorigin.github.io/uAssets/thirdparties/easylist-annoyances.txt", "", True,
     "EasyList – Other annoyances", "Sticky bars, 'download our app' banners and similar"),
    ("easylist-newsletters", "https://ublockorigin.github.io/uAssets/thirdparties/easylist-newsletters.txt", "", True,
     "EasyList – Newsletter notices", "Newsletter sign-up pop-ups"),
    ("easylist-notifications", "https://ublockorigin.github.io/uAssets/thirdparties/easylist-notifications.txt", "", True,
     "EasyList – Notification prompts", "'Allow notifications' prompts"),
    ("easylist-chat", "https://ublockorigin.github.io/uAssets/thirdparties/easylist-chat.txt", "", True,
     "EasyList – Chat widgets", "Support chat bubbles"),
    ("phishing", "https://malware-filter.gitlab.io/phishing-filter/phishing-filter.txt", "", True,
     "Phishing URL Blocklist", "Known phishing sites"),
    ("adguard-base", "https://filters.adtidy.org/extension/ublock/filters/2_without_easylist.txt", "", False,
     "AdGuard Base", "More ad blocking; overlaps with EasyList and uses more memory"),
    ("easylist-ai", "https://ublockorigin.github.io/uAssets/thirdparties/easylist-ai.txt", "", False,
     "EasyList – AI widgets", "AI assistant widgets and suggestions"),
    ("block-lan", "https://ublockorigin.github.io/uAssets/filters/lan-block.txt", "trusted", False,
     "Block outsider intrusion into LAN", "Stops websites from reaching devices on your home network; can break local services"),
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
    tokens = re.findall(r"&&|\|\||[()!]|[A-Za-z_]\w*|\S", expr)
    pos = [0]

    def peek():
        return tokens[pos[0]] if pos[0] < len(tokens) else None

    def parse_or():
        val = parse_and()
        while peek() == "||":
            pos[0] += 1
            rhs = parse_and()
            val = val or rhs
        return val

    def parse_and():
        val = parse_not()
        while peek() == "&&":
            pos[0] += 1
            rhs = parse_not()
            val = val and rhs
        return val

    def parse_not():
        if peek() == "!":
            pos[0] += 1
            return not parse_not()
        return parse_atom()

    def parse_atom():
        tok = peek()
        if tok == "(":
            pos[0] += 1
            val = parse_or()
            if peek() != ")":
                raise ValueError("missing )")
            pos[0] += 1
            return val
        if tok is not None and re.fullmatch(r"[A-Za-z_]\w*", tok):
            pos[0] += 1
            return bool(TOKENS.get(tok, False))
        raise ValueError(f"unexpected token: {tok!r}")

    try:
        result = parse_or()
        return result if pos[0] == len(tokens) else False
    except ValueError:
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


HEADERS = []


def update_lists():
    d = HERE / "lists"
    d.mkdir(exist_ok=True)
    HEADERS.clear()
    def fetch_list(name, url):
        lines = resolve(url)
        (d / f"{name}.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
        version = next((l.split(":", 1)[1].strip() for l in lines[:80]
                        if l.lower().startswith(("! version:", "# version:", "! last modified:", "# last modified:", "! updated:"))), "?")
        print(f"{name}: {len(lines)} lines, version {version}")
        # The lists' own words about who made them and under what terms travel with the compiled data (lists/LICENCES.txt).
        head = [l.strip() for l in lines[:80] if l.strip().lower().lstrip("!# ").startswith(
            ("title", "homepage", "licen", "copyright", "credits", "policy", "last modified", "last updated", "version", "expires"))]
        HEADERS.append((name, url, head))
        return version

    manifest = [{"name": n, "url": u, "flags": f, "version": fetch_list(n, u)} for n, u, f in LISTS]
    (d / "lists.json").write_text(json.dumps(manifest, indent=1) + "\n")
    optional = [{"name": n, "url": u, "flags": f, "default": on, "title": t, "about": a, "version": fetch_list(n, u)}
                for n, u, f, on, t, a in OPTIONAL]
    (d / "optional.json").write_text(json.dumps(optional, indent=1, ensure_ascii=False) + "\n")
    with open(d / "LICENCES.txt", "w", encoding="utf-8") as f:
        f.write("Filter lists compiled into this package, as downloaded on the build day: name, address, and the list's own\n"
                "header lines (title, homepage, licence, copyright). See licenses/ad-blocker/NOTICES.txt for the licence of each.\n")
        for n, u, head in HEADERS:
            f.write(f"\n== {n}\n{u}\n" + "\n".join(head) + "\n")


def update_ubo(commit):
    if commit is None:
        commit = json.loads(fetch("https://api.github.com/repos/gorhill/uBlock/commits/master"))["sha"]
    tgz = fetch(f"https://codeload.github.com/gorhill/uBlock/tar.gz/{commit}")
    d = HERE / "ubo"
    shutil.rmtree(d, ignore_errors=True)
    d.mkdir(parents=True, exist_ok=True)
    d_root = d.resolve()
    # All of src/js: the scriptlet modules import helpers from it (urlskip.js, ...).
    wanted = ("src/web_accessible_resources/", "src/js/", "LICENSE.txt")
    with tarfile.open(fileobj=io.BytesIO(tgz)) as tar:
        for m in tar.getmembers():
            rel = m.name.split("/", 1)[1] if "/" in m.name else ""
            if not m.isfile() or not rel.startswith(wanted):
                continue
            target = (d / rel.removeprefix("src/")).resolve()
            if not target.is_relative_to(d_root):
                continue
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(tar.extractfile(m).read())
    (d / "COMMIT").write_text(commit + "\n")
    print(f"uBO resources: {commit}")


if __name__ == "__main__":
    update_lists()
    update_ubo(sys.argv[1] if len(sys.argv) > 1 else None)
