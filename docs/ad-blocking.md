# Ad blocking

Wren has two ad blockers. The package carries one or both; the UI starts the engine with the uBO blocker when it
is there, and with the rule list otherwise (`browser/engine_client.cpp`). Both are off on YouTube and on the hosts
where the user allowed ads (**Hold OK → Allow ads on this site**).

## The rule list (tools/adblock)

EasyList and EasyPrivacy, converted at build time to WebKit's content-blocker format and compiled into a store
the engine loads. Fast (WebKit matches requests in the network process) but lossy: WebKit's format cannot say
uBO's redirects, scriptlets, `removeparam`, procedural cosmetic filters or many exceptions, so those are dropped.

## The uBO blocker (engine/ubo, tools/ubo) — prototype

uBlock Origin's default lists and uBO's own resources, matched with Brave's
[adblock-rust](https://github.com/brave/adblock-rust) (the engine Brave uses for the same lists) inside the web
process, as a WebKit web process extension (`lib/web-extensions/libwren-ubo.so`):

| | how |
|---|---|
| Network blocking, exceptions, `$important`, `$badfilter`, … | `WebKitWebPage::send-request`: cancel the request |
| `redirect=` / `redirect-rule=` | the request loads uBO's neutered resource (noop.js, the GTM/GA stubs, 1x1.gif, …) as a data: URL |
| `removeparam=` | the request loads the cleaned URL |
| Scriptlets `+js(...)`, trusted scriptlets on uBO's own lists | run in each frame's main world when its window object is created, before the page's scripts |
| Specific and generic element hiding | `cosmetic.js` in an isolated world: a style sheet, plus generic rules for the classes and ids the document uses |
| Procedural filters (`:has-text`, `:upward`, `:xpath`, `:matches-css`, `:remove()`, `:style()`, …) | `cosmetic.js`, re-run as the document changes |

Lists (uBO's defaults on a fresh install): uBlock filters, Badware risks, Privacy, Unbreak, Quick fixes, EasyList,
EasyPrivacy, Online Malicious URL Blocklist, Peter Lowe's list. `tools/ubo/update.py` refreshes them, resolving
`!#include` and evaluating `!#if` the way uBO would in a WebKit browser.

Not (yet) like uBO: no element picker, no per-site dynamic filtering, no HTML filtering (`##^`), no `$csp`
(WebKit's send-request cannot add response headers), and the request type is inferred (Sec-Fetch-Dest, Accept,
extension) because send-request does not say what a request is for.

Build:

```sh
tools/ubo/build.sh data      # lists + uBO resources -> build/ubo/data (cargo, node)
tools/ubo/build.sh lib       # the extension for the TV -> build/ubo/armv7-unknown-linux-gnueabi (zig or CC=...)
tools/ubo/test/run.sh        # desktop test with the system's WPE WebKit
```

`tools/stage-engine.sh` then puts `engine/ubo/` and `engine/lib/web-extensions/` in the package.
`WREN_UBO=0` in the engine's environment falls back to the rule list; `WREN_UBO_LOG=1` logs each blocked,
redirected or rewritten request, `WREN_UBO_LOG=2` every request with the type it was given.

Desktop test (WPE WebKit 2.54, 2026-11-19): network blocking, `redirect=` for scripts and images, `removeparam`,
scriptlets (trusted ones too, ahead of the page's own scripts), specific, generic (also for elements added
later) and procedural hiding all pass; nothing applies with the blocker off or the site allowed. WebKit sets
Sec-Fetch-Dest on subresource requests, so request types are exact. The test page is served as
`wrentest.127.0.0.1.nip.io` because EasyList turns generic hiding off on 127.0.0.1 and localhost.

Memory: engine.dat is about 24 MB and is held by the web process.
