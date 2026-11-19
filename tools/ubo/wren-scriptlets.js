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

// wren-media-debug.js: logs each MSE SourceBuffer's buffered ranges (after appends, and on seeks) and the video's
// state, through console.log (which WPE_BROWSER_DEBUG_CONSOLE=1 sends to engine.log). Diagnosis only.
function wrenMediaDebug() {
    'use strict';
    const ranges = r => { const o = []; for (let i = 0; i < r.length; i++) o.push(r.start(i).toFixed(3) + '-' + r.end(i).toFixed(3)); return o.join(',') || 'none'; };
    const buffers = [];
    // MediaSource can be hidden on the site (wren-no-mse): then there are no buffers to log.
    const add = self.MediaSource && MediaSource.prototype.addSourceBuffer;
    let appends = 0;
    if (add) MediaSource.prototype.addSourceBuffer = function (type) {
        const sb = add.call(this, type);
        const label = /audio/.test(type) ? 'audio' : /video/.test(type) ? 'video' : type;
        buffers.push([label, sb]);
        sb.addEventListener('updateend', () => {
            if (++appends <= 12 || appends % 20 === 0) console.log('[wren-media] append ' + label + ' #' + appends + ' buffered ' + ranges(sb.buffered) + ' offset ' + sb.timestampOffset);
        });
        return sb;
    };
    const state = (ev, v) => console.log('[wren-media] ' + ev + ' t=' + v.currentTime.toFixed(3) + ' rs=' + v.readyState + ' paused=' + v.paused +
        ' video.buffered=' + ranges(v.buffered) + ' ' + buffers.map(([l, sb]) => { try { return l + '=' + ranges(sb.buffered); } catch (e) { return l + '=gone'; } }).join(' '));
    for (const ev of ['seeking', 'seeked', 'waiting', 'playing', 'canplay', 'loadeddata']) {
        addEventListener(ev, e => { if (e.target instanceof HTMLVideoElement) state(ev, e.target); }, true);
    }
    // What covers a playing video (hardware video sits behind the page: anything painted above it hides it).
    const name = el => el.tagName.toLowerCase() + (el.id ? '#' + el.id : '') + (typeof el.className === 'string' && el.className.trim() ? '.' + el.className.trim().split(/\s+/).slice(0, 3).join('.') : '');
    const describe = el => { const s = getComputedStyle(el); const r = el.getBoundingClientRect();
        return name(el) + '[bg ' + s.backgroundColor + (s.backgroundImage !== 'none' ? ' img' : '') + ' op ' + s.opacity + ' z ' + s.zIndex + ' pos ' + s.position + ' ' + Math.round(r.width) + 'x' + Math.round(r.height) + ']'; };
    const stack = (why, v) => {
        const r = v.getBoundingClientRect();
        const pts = [[0.5, 0.5], [0.2, 0.2], [0.8, 0.8]];
        for (const [fx, fy] of pts) {
            const x = r.left + r.width * fx, y = r.top + r.height * fy;
            if (x < 0 || y < 0 || x > innerWidth || y > innerHeight) continue;
            const els = document.elementsFromPoint(x, y);
            const i = els.indexOf(v);
            console.log('[wren-media] ' + why + ' ' + name(v) + ' ' + Math.round(r.width) + 'x' + Math.round(r.height) + ' at ' + fx + ',' + fy +
                ': above it ' + (i < 0 ? '(video not hit) ' + els.slice(0, 5).map(describe).join(' > ') : els.slice(0, i).map(describe).join(' > ') || 'nothing') +
                ' | below: ' + (i < 0 ? '' : els.slice(i + 1, i + 3).map(describe).join(' > ')));
        }
    };
    addEventListener('playing', e => { if (e.target instanceof HTMLVideoElement) { const v = e.target; setTimeout(() => stack('covering', v), 1500); } }, true);
    // Also once the page has settled, for every video, playing or not (layout is what matters).
    addEventListener('load', () => setTimeout(() => {
        const vids = [...document.querySelectorAll('video')];
        console.log('[wren-media] page has ' + vids.length + ' video element(s)');
        const mq = q => q + '=' + matchMedia(q).matches;
        console.log('[wren-media] input: ' + ['(hover: hover)', '(any-hover: hover)', '(pointer: fine)', '(pointer: coarse)', '(any-pointer: fine)'].map(mq).join(' ') +
            ' touch=' + ('ontouchstart' in window) + ' maxTouchPoints=' + navigator.maxTouchPoints + ' ua=' + navigator.userAgent.slice(0, 120));
        // How thumbnails do their previews: the first element whose attributes mention one.
        const thumb = [...document.querySelectorAll('img')].find(i => /screenshot|thumb|preview/i.test(i.currentSrc || i.src || i.getAttribute('data-src') || ''));
        let box = thumb;
        for (let n = 0; box && n < 3 && box.parentElement && box.parentElement !== document.body; n++) box = box.parentElement;
        if (box) console.log('[wren-media] preview markup: ' + box.outerHTML.replace(/\s+/g, ' ').slice(0, 2500));
        vids.slice(0, 4).forEach(v => {
            stack('layout', v);
            const chain = []; let p = v.parentElement;
            for (let n = 0; p && n < 6; n++, p = p.parentElement) chain.push(describe(p));
            console.log('[wren-media] parents of ' + name(v) + ': ' + chain.join(' < '));
        });
    }, 6000), { once: true });
}

export const wrenScriptlets = [
    { name: 'wren-gif-videos.js', fn: wrenGifVideos, aliases: ['wren-gif-videos'] },
    { name: 'wren-media-debug.js', fn: wrenMediaDebug, aliases: ['wren-media-debug'] },
];
