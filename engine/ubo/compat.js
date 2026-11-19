// SPDX-License-Identifier: GPL-3.0-or-later
// Wren's page compatibility fixes (engine/ubo/extension.c runs this in every http(s) frame's main world, before the
// page's scripts, whether or not ads are blocked there).
//
// Pointer: WebKit tells pages this TV has a touchscreen and no hover ((hover: none), (pointer: coarse), touch
// events), but the LG Magic Remote is a mouse-like pointer. Sites then turn off hover previews (thumbnails that
// play on hover) and switch to touch layouts. So pages are told what a desktop with a mouse would tell them: matchMedia answers, CSS
// @media rules the page can read are rewritten, and the touch event API is hidden.
(function wrenCompat() {
    'use strict';
    // Each media feature -> what a mouse-driven desktop answers. (min-width: 0px) is always true, (max-width: 0px)
    // never: they stand in for true and false inside any media query.
    const T = '(min-width: 0px)', F = '(max-width: 0px)';
    const fix = q => q
        .replace(/\(\s*(any-)?hover\s*:\s*hover\s*\)/gi, T)
        .replace(/\(\s*(any-)?hover\s*:\s*none\s*\)/gi, F)
        .replace(/\(\s*(any-)?hover\s*\)/gi, T)
        .replace(/\(\s*(any-)?pointer\s*:\s*fine\s*\)/gi, T)
        .replace(/\(\s*(any-)?pointer\s*:\s*(coarse|none)\s*\)/gi, F)
        .replace(/\(\s*(any-)?pointer\s*\)/gi, T);
    const relevant = q => /hover|pointer/i.test(q);

    const matchMedia = window.matchMedia;
    if (matchMedia) {
        window.matchMedia = function (query) {
            return matchMedia.call(this, typeof query === 'string' && relevant(query) ? fix(query) : query);
        };
    }

    // Touch: gone from feature tests ('ontouchstart' in window, window.TouchEvent, document.createTouch).
    for (const proto of [Window.prototype, Document.prototype, Element.prototype, HTMLElement.prototype,
                         typeof SVGElement !== 'undefined' && SVGElement.prototype]) {
        if (!proto) continue;
        for (const ev of ['ontouchstart', 'ontouchend', 'ontouchmove', 'ontouchcancel']) {
            try { delete proto[ev]; } catch (e) {}
        }
    }
    for (const ev of ['ontouchstart', 'ontouchend', 'ontouchmove', 'ontouchcancel']) { try { delete window[ev]; } catch (e) {} }
    for (const name of ['TouchEvent', 'Touch', 'TouchList']) { try { delete window[name]; } catch (e) {} }
    try { delete Document.prototype.createTouch; delete Document.prototype.createTouchList; } catch (e) {}

    // CSS: @media rules with hover/pointer conditions, in the style sheets the page can read (its own and CORS ones).
    const done = new WeakSet();
    const rewrite = list => {
        for (const rule of list) {
            if (rule.media && rule.cssRules) {
                const text = rule.media.mediaText;
                if (relevant(text)) { try { rule.media.mediaText = fix(text); } catch (e) {} }
                rewrite(rule.cssRules);
            } else if (rule.cssRules) {
                rewrite(rule.cssRules); // @supports, @layer, ...
            }
        }
    };
    const sweep = () => {
        for (const sheet of document.styleSheets) {
            if (done.has(sheet)) continue;
            let rules;
            try { rules = sheet.cssRules; } catch (e) { continue; } // cross-origin without CORS: not readable
            if (!rules) continue;
            done.add(sheet);
            rewrite(rules);
        }
    };
    let queued = false;
    const later = () => { if (!queued) { queued = true; setTimeout(() => { queued = false; sweep(); }, 50); } };
    document.addEventListener('DOMContentLoaded', sweep);
    addEventListener('load', sweep);
    addEventListener('load', e => { if (e.target && (e.target.tagName === 'LINK' || e.target.tagName === 'STYLE')) later(); }, true);
    new MutationObserver(later).observe(document, { childList: true, subtree: true });
})();
