// SPDX-License-Identifier: GPL-3.0-or-later
// Wren's own scriptlets, injected like uBO's (tools/ubo/scriptlets-to-json.mjs adds them to resources.json;
// tools/ubo/wren-filters.txt says where they run).

// wren-gif-videos.js: sites serve GIFs as short muted looping videos. On a TV every video goes to the hardware
// decoder, which plays one at a time, so a page full of them shows one (and each new one stops the last). Where
// the GIF behind such a video has a public image URL, show that image instead: the page decodes and animates
// images itself, any number at once. Runs in the page's main world before its scripts; follows shadow roots.
function wrenGifVideos() {
    'use strict';
    const imageFor = url => {
        if (!url) return null;
        let u;
        try { u = new URL(url, location.href); } catch (e) { return null; }
        // Reddit uploads: preview.redd.it/<name>.gif?...&format=mp4&s=... -> i.redd.it/<name>.gif (public, unsigned)
        if (u.hostname === 'preview.redd.it') {
            const m = /^\/([^/]+\.gif)$/i.exec(u.pathname);
            if (m) return 'https://i.redd.it/' + m[1];
        }
        // Giphy: .../media/<id>/<file>.mp4, i.giphy.com/<id>.mp4, giphy.com/gifs/...-<id> -> i.giphy.com/<id>.webp
        if (/(^|\.)giphy\.com$/.test(u.hostname)) {
            const m = /\/media\/(?:v1\.[^/]+\/)?([A-Za-z0-9]+)\//.exec(u.pathname) || /^\/([A-Za-z0-9]+)\.(?:mp4|webp|gif)$/.exec(u.pathname);
            if (m) return 'https://i.giphy.com/' + m[1] + '.webp';
        }
        // Reddit's giphy proxy: external-preview.redd.it/<...>.gif?...format=mp4 carries the giphy id in "giphy|<id>"
        // only in its data, not its URL; leave those to the page.
        return null;
    };
    const sourceOf = v => v.getAttribute('src') || (v.querySelector('source[src]') || {}).src || v.currentSrc || '';
    const swapped = new WeakSet();
    const swap = v => {
        if (swapped.has(v) || !v.isConnected) return;
        const img = imageFor(sourceOf(v));
        if (!img) return;
        swapped.add(v);
        const el = document.createElement('img');
        el.src = img;
        el.alt = v.getAttribute('aria-label') || 'GIF';
        el.decoding = 'async';
        el.className = v.className;
        el.setAttribute('style', (v.getAttribute('style') || '') + ';object-fit:contain;max-width:100%;');
        const w = v.getAttribute('width'), h = v.getAttribute('height');
        if (w) el.setAttribute('width', w);
        if (h) el.setAttribute('height', h);
        try { v.pause(); } catch (e) {}
        v.removeAttribute('src');
        v.querySelectorAll('source').forEach(s => s.remove());
        try { v.load(); } catch (e) {}
        v.replaceWith(el);
    };
    const scan = root => {
        if (root.nodeName === 'VIDEO') swap(root);
        if (root.querySelectorAll) root.querySelectorAll('video').forEach(swap);
    };
    const observe = root => new MutationObserver(records => {
        for (const r of records) {
            if (r.type === 'attributes') { if (r.target.nodeName === 'VIDEO' || r.target.nodeName === 'SOURCE') scan(r.target.closest ? (r.target.closest('video') || r.target) : r.target); continue; }
            for (const n of r.addedNodes) if (n.nodeType === 1) scan(n);
        }
    }).observe(root, { childList: true, subtree: true, attributes: true, attributeFilter: ['src'] });
    // Shadow roots (Reddit draws comments and players in them): watch each as it is made.
    const attach = Element.prototype.attachShadow;
    Element.prototype.attachShadow = function (init) {
        const root = attach.call(this, init);
        try { observe(root); } catch (e) {}
        return root;
    };
    // A video that starts before the observer saw it: swap it rather than play it on the hardware decoder.
    const play = HTMLMediaElement.prototype.play;
    HTMLMediaElement.prototype.play = function () {
        if (this.nodeName === 'VIDEO' && imageFor(sourceOf(this))) { swap(this); return Promise.resolve(); }
        return play.apply(this, arguments);
    };
    observe(document);
    document.addEventListener('DOMContentLoaded', () => scan(document), { once: true });
}

export const wrenScriptlets = [
    { name: 'wren-gif-videos.js', fn: wrenGifVideos, aliases: ['wren-gif-videos'] },
];
