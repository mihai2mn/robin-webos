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

    // Video: the TV has one hardware video decoder per app, and every video plays on it. A thumbnail's hover preview
    // that starts while the page's main video plays takes the decoder away from it (and enough of those switches have
    // hung the web process). So: (1) a preview may not start while a main video plays; (2) a preview the page has
    // removed, or left paused and hidden, gives its stream back (decoder and buffers), and gets it back if played.
    const mains = new Set();
    const isMain = v => !v.muted || v.controls || v.getBoundingClientRect().width >= innerWidth * 0.45;
    // (0) With WebKit patch 0010, a <video wren-software> decodes in software and draws in the page, off the hardware
    // decoder: previews get it before they load. A preview: muted, no controls, in a box under 60% of the screen wide.
    const isPreview = v => (v.muted || v.hasAttribute('muted') || v.defaultMuted) && !v.controls && !v.hasAttribute('controls') &&
        ((v.parentElement || v).getBoundingClientRect().width || 1) < innerWidth * 0.6;
    const mark = v => { if (v instanceof HTMLVideoElement && !v.hasAttribute('wren-software') && isPreview(v)) v.setAttribute('wren-software', ''); };
    const srcDesc = Object.getOwnPropertyDescriptor(HTMLMediaElement.prototype, 'src');
    if (srcDesc && srcDesc.set) Object.defineProperty(HTMLMediaElement.prototype, 'src', { ...srcDesc, set(value) { mark(this); srcDesc.set.call(this, value); } });
    const load = HTMLMediaElement.prototype.load;
    HTMLMediaElement.prototype.load = function () { mark(this); return load.apply(this, arguments); };
    new MutationObserver(records => {
        for (const r of records) for (const n of r.addedNodes) {
            if (n.nodeType !== 1) continue;
            (n.tagName === 'VIDEO' ? [n] : n.querySelectorAll ? [...n.querySelectorAll('video')] : []).forEach(mark);
        }
    }).observe(document, { childList: true, subtree: true });
    const mainPlaying = () => [...mains].some(v => v.isConnected && !v.paused && !v.ended && isMain(v));
    const visible = v => { if (!v.isConnected) return false; const r = v.getBoundingClientRect(); const s = getComputedStyle(v);
        return r.width > 0 && r.height > 0 && s.visibility !== 'hidden' && s.display !== 'none' && Number(s.opacity) > 0.05; };
    const release = v => {
        if (v.__wrenReleased || isMain(v)) return;
        const src = v.getAttribute('src'), sources = [...v.querySelectorAll('source')];
        if (!src && !sources.length) return;
        v.__wrenReleased = { src, sources: sources.map(s => s.cloneNode(true)) };
        try { v.pause(); } catch (e) {}
        v.removeAttribute('src');
        sources.forEach(s => s.remove());
        try { v.load(); } catch (e) {}
    };
    const restore = v => {
        const r = v.__wrenReleased;
        if (!r) return;
        v.__wrenReleased = null;
        if (r.src) v.setAttribute('src', r.src);
        r.sources.forEach(s => v.appendChild(s));
        try { v.load(); } catch (e) {}
    };
    const play = HTMLMediaElement.prototype.play;
    HTMLMediaElement.prototype.play = function () {
        if (this instanceof HTMLVideoElement) {
            if (!isMain(this) && mainPlaying())
                return Promise.reject(new DOMException('A main video is playing', 'NotAllowedError'));
            restore(this);
        }
        return play.apply(this, arguments);
    };
    // Media events don't bubble: listen in the capture phase. (Videos inside shadow roots are not seen.)
    document.addEventListener('play', e => {
        const v = e.target;
        if (!(v instanceof HTMLVideoElement)) return;
        if (isMain(v)) { mains.add(v); return; }
        if (mainPlaying()) { try { v.pause(); } catch (err) {} }
    }, true);
    document.addEventListener('pause', e => {
        const v = e.target;
        if (!(v instanceof HTMLVideoElement) || isMain(v)) return;
        setTimeout(() => { if (v.paused && !visible(v)) release(v); }, 1500);
    }, true);
    new MutationObserver(records => {
        for (const r of records) for (const n of r.removedNodes) {
            if (n.nodeType !== 1) continue;
            const vids = n.tagName === 'VIDEO' ? [n] : [...n.querySelectorAll('video')];
            vids.forEach(v => setTimeout(() => { if (!v.isConnected) { mains.delete(v); release(v); } }, 0));
        }
    }).observe(document, { childList: true, subtree: true });
})();
