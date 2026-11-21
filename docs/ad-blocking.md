# Ad blocking

Wren uses a uBlock-Origin-compatible ad blocker (`engine/ubo`, `tools/ubo`) loaded as a WebKit web process extension
(`lib/web-extensions/libwren-ubo.so`), paired with a `$popup`/`$popunder` rule matcher in the engine
(`engine/src/popup_filter.h`). Settings, optional extra filter lists, allowed hosts, and custom rules are managed on
`wren://adblock`.

## How it works

uBlock Origin's default lists, optional extra lists, and uBO's own resources are matched with Brave's
[adblock-rust](https://github.com/brave/adblock-rust) (the engine Brave uses for the same lists) inside the web
process:

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

My filters: wren://adblock has a "My filters" box for the user's own rules (uBO syntax, trusted). They are kept as
text in `$WPE_BROWSER_DATA_DIR/ubo-my-filters.txt` and parsed in the web process, never built into the package.
That is where site-specific fixes belong; the package only ships generic tools for them, such as Wren's scriptlets
(`tools/ubo/wren-scriptlets.js`):

| scriptlet | what it does |
|---|---|
| `wren-no-mse` | hides Media Source Extensions, so the site's player falls back to a plain MP4 (hardware decoder); for MSE streams that stall in this WebKit |
| `wren-media-debug` | logs MSE buffered ranges, elements covering videos and pointer media queries (with `WPE_BROWSER_DEBUG_CONSOLE=1`) |
| `wren-gif-videos` | GIFs served as videos (Reddit, Giphy) shown as images |

For example `example.com##+js(wren-no-mse)`.

Not (yet) like uBO: no element picker, no per-site dynamic filtering, no HTML filtering (`##^`), no `$csp`
(WebKit's send-request cannot add response headers), and the request type is inferred (Sec-Fetch-Dest, Accept,
extension) because send-request does not say what a request is for.

Build:

```sh
tools/ubo/build.sh data      # lists + uBO resources -> build/ubo/data (cargo, node)
tools/ubo/build.sh lib       # the extension for the TV -> build/ubo/armv7-unknown-linux-gnueabi (zig or CC=...)
tools/ubo/test/run.sh        # desktop test with the system's WPE WebKit
```

`tools/package-release.sh` puts `engine/ubo/` and `engine/lib/web-extensions/` in the package.
`WREN_UBO=0` in the engine's environment disables the ad blocker; `WREN_UBO_LOG=1` logs each blocked,
redirected or rewritten request, `WREN_UBO_LOG=2` every request with the type it was given.

Desktop test (WPE WebKit 2.54, 2026-11-19): network blocking, `redirect=` for scripts and images, `removeparam`,
scriptlets (trusted ones too, ahead of the page's own scripts), specific, generic (also for elements added
later) and procedural hiding all pass; nothing applies with the blocker off or the site allowed. WebKit sets
Sec-Fetch-Dest on subresource requests, so request types are exact. The test page is served as
`wrentest.127.0.0.1.nip.io` because EasyList turns generic hiding off on 127.0.0.1 and localhost.

Memory: engine.dat is about 24 MB and is held by the web process.
