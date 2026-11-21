// SPDX-License-Identifier: GPL-3.0-or-later
// Wren's page compatibility fixes (engine/ubo/extension.c runs this in every http(s) frame's main world, before the
// page's scripts, whether or not ads are blocked there).
//
// Pointer: WebKit tells pages this TV has a touchscreen and no hover ((hover: none), (pointer: coarse), touch
// events), but the LG Magic Remote is a mouse-like pointer. Sites then turn off hover previews (thumbnails that
// play on hover) and switch to touch layouts. So pages are told what a desktop with a mouse would tell them: matchMedia answers, CSS
// @media rules the page can read are rewritten, and the touch event API is hidden.
(function wrenCompat(envelope) {
    'use strict';
    // ManagedMediaSource: desktop Chrome has none, WebKit has it next to MediaSource, and players that prefer it (hls.js since 1.5) then attach
    // through a <source> CHILD of the video (and remove the page's own <source>) instead of video.src. Players written against Chrome keep
    // a reference to their first <source>, find it detached, and every seek that reloads the source (a click on a progress bar) ends on a
    // stale MediaSource with nothing feeding it (TV, 2026-11-21, a site's own player). MediaSource works as well, so the managed one is not offered.
    try { delete window.ManagedMediaSource; delete window.ManagedSourceBuffer; delete window.BufferedChangeEvent; } catch (e) {}
    // The engine (engine/src/main.cpp, add_hybrid_video) tells us where it keeps this file, so the same code also runs
    // in a build whose engine predates the hybrid video design (build/video/hybrid/DESIGN.md).
    const base = (window.__wrenCompatBase || '/adblock/').replace(/\/?$/, '/');
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
    // What a video is before it starts: a preview is marked (this is the floor; see the policy's previewMark), and
    // everything is then given the video policy's decision (below).
    const mark = v => {
        if (!(v instanceof HTMLVideoElement)) return;
        // The floor marks previews so a video the policy never gets to decide (a shadow-root video, a decide() that
        // threw) still keeps the hardware decoder off the page's main video (DESIGN.md §2). It runs BEFORE the decision
        // and must not write once a decision owns this element (it runs again on every load(), e.g. the reload a
        // fullscreen transition does): re-adding it there would undo the decision.
        if (v.__wrenDecided !== true && !v.hasAttribute('wren-software') && !pageMarked(v) && isPreview(v)) {
            v.setAttribute('wren-software', '');
            v.__wrenPreview = true;
        }
        if (markVideo) markVideo(v);
    };
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
            // A software preview (wren-software, WebKit patch 0010) never touches the hardware decoder: let it play.
            if (!isMain(this) && !this.hasAttribute('wren-software') && mainPlaying())
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
        if (!v.hasAttribute('wren-software') && mainPlaying()) { try { v.pause(); } catch (err) {} }
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

    // Video policy (build/video/hybrid/DESIGN.md): a video that draws IN THE PAGE (<video wren-software>, WebKit patch
    // 0010) scrolls with the page; one on the hardware plane does not, but costs no CPU. So: in the page while the box
    // is small and nothing is fullscreen (the hole is invisible then anyway), on the hardware plane otherwise: a
    // controls player, protected (DRM) content, any fullscreen, or anything bigger than kSoftwareBox. Fullscreen is
    // the one case that changes a playing video: WebKit decides at start (patch 0010 reads the attribute once, and
    // that decides the decoders too), so the element is reloaded at its own media time right after the transition
    // into or out of fullscreen (the reporter says so, see the fullscreen wrapper below). Pages that mark a video
    // themselves win: wren-software (in the page) or wren-hw (on the hardware plane).
    const policy = window.__wrenVideoPolicy = {
        // CSS pixels, both dimensions: at the TV's devicePixelRatio (2) this is half the screen's width and height.
        box: 960 * 540,
        // How many videos may decode in software at once (each also composites on the CPU).
        players: 2,
        // Set by the engine when it asked for the hybrid policy (engine/src/main.cpp, add_hybrid_video): without it a
        // video is only marked the way previews always were, so an older engine keeps exactly its old behaviour.
        on: false,
        current: () => document.fullscreenElement || document.webkitFullscreenElement || null,
    };
    // Engine-provided settings (hybrid_video_script() fills this with {on: true}; an older engine leaves it undefined).
    if (envelope) Object.assign(policy, envelope);
    // Readings of the page (the engine log prints them: hybrid video: …): the policy's decisions, the transitions, and
    // the decode health that decides how big a software video may be (DESIGN.md §3.5, §6).
    const log = what => {
        try {
            const h = window.webkit && window.webkit.messageHandlers && window.webkit.messageHandlers.wpebHybrid;
            if (h) h.postMessage(JSON.stringify(what));
        } catch (e) {}
    };
    const report = (v, what) => {
        try {
            const r = v.getBoundingClientRect();
            log({ decision: what, box: Math.round(r.width) + 'x' + Math.round(r.height),
                src: String(v.currentSrc || v.src || '').split(/[?#]/)[0].slice(-80),
                protected: !!v.__wrenEmeActive, fullscreen: !!policy.current() });
        } catch (e) {}
    };
    // Software players on the page now: over budget, the newest demotes.
    const software = v => v.hasAttribute('wren-software');
    const playing = () => [...document.querySelectorAll('video')].filter(v => software(v) && !v.paused && !v.ended);
    // The policy, for one element, at the moment before its player starts. `why` is the reason it is on the hardware
    // plane ('' when it draws in the page); it is also the element's wren-policy attribute, for reading in the page.
    // Between two starts of the same player (the marking hooks run once per load) the answer stays put, so a playing
    // video is never reloaded on its own; a fullscreen transition is the one place that asks for the reload (below).
    // Called on every new start of a player (the hooks above) and after a fullscreen transition. The first decision
    // reads the element's box and the budget; a later call (a fullscreen transition) re-reads only what a transition
    // changes, fullscreen; the page's own marking is never overruled.
    const decide = v => {
        if (!(v instanceof HTMLVideoElement)) return;
        reconcile(v);
        const first = v.__wrenDecided !== true;
        let keep = v.getAttribute('wren-policy') === 'page'; // the page's own marking; decided once, then remembered
        // A wren-software that was there BEFORE this build's first decision may be the page's (an older compat.js, a page
        // that marks previews itself) or stale (a preview element reused for the main video, the page changing its mind).
        // A stale marking on a page-sized element must not win: leaving a big video in the page is exactly the CPU
        // meltdown this policy exists to prevent. So on the first decision, a pre-existing wren-software is only trusted
        // when the element is small enough for the policy to put it in the page anyway (then keep is harmless and no
        // reload is needed); otherwise the decision owns the element.
        const stale = first && !v.__wrenPreview && !keep && v.hasAttribute('wren-software') &&
            v.getBoundingClientRect().width * v.getBoundingClientRect().height > policy.box;
        if (stale) keep = false;
        // Package 2: the decision, from what can be read without writing anything.
        let why;
        if (keep) why = v.hasAttribute('wren-hw') ? 'page' : '';
        else if (!policy.on) why = v.__wrenPreview ? '' : 'off';
        else if (v.__wrenEmeActive) why = 'protected';
        else if (policy.current()) why = 'fullscreen';
        else if (v.controls && !v.muted) why = 'controls';
        // Sound only plays on the hardware plane: a software video's audio would go to PulseAudio, and webOS 5 TVs have no PulseAudio
        // server (it is silent); LG's media pipeline plays the page's audio (AAC, or decoded Opus/Vorbis as PCM) next to the hardware
        // video. Previews are muted (the isPreview rule), so they stay in the page.
        else if (!v.muted) why = 'sound';
        else if (first ? v.getBoundingClientRect().width * v.getBoundingClientRect().height > policy.box : v.__wrenBox > policy.box) why = 'box';
        else if (first && playing().length >= policy.players) why = 'budget';
        else why = '';
        if (first) {
            if (v.__wrenBox === undefined) v.__wrenBox = v.getBoundingClientRect().width * v.getBoundingClientRect().height;
            v.__wrenDecided = true;
            // A small pre-existing marking is the page's: it stays, and the element is never reloaded for the policy.
            if (!keep && !stale && v.hasAttribute('wren-software') && !v.__wrenPreview) keep = true;
        }
        v.__wrenPolicy = why ? 'hw' : 'sw';
        v.setAttribute('wren-policy', keep ? 'page' : why === '' ? 'small' : why);
        if (first && !keep) {
            v.__wrenPageMark = { hw: v.hasAttribute('wren-hw'), sw: v.hasAttribute('wren-software') && !v.__wrenPreview };
            // Older compat.js in the same window, or the page itself: its marking is kept, to be restored when the policy
            // is switched off (below).
            v.__wrenSavedSoftware = v.__wrenPageMark.sw;
        }
        // Package 3: the consequence, for WebKit patch 0010, which reads the attribute ONCE, before the player starts,
        // so the element must carry exactly what the NEXT player should do. The attribute follows the DECISION -- never
        // "was it marked before": a preview the floor marked must be un-marked when the answer is the hardware plane
        // (fullscreen, a big box), or it would stay in the page and melt the CPU.
        if (keep) {
            // The page's answer, left exactly as the page set it.
        } else if (policy.on) {
            if (why) { if (v.hasAttribute('wren-software')) v.removeAttribute('wren-software'); }
            else if (!v.hasAttribute('wren-software')) v.setAttribute('wren-software', '');
        } else {
            // Policy off (an old engine, or WPE_BROWSER_DEBUG_HYBRID=0): yesterday's behaviour, exactly -- previews are
            // marked, everything else is left as the page had it.
            if (v.__wrenPreview && !v.hasAttribute('wren-software')) v.setAttribute('wren-software', '');
        }
        v.__wrenPreview = false; // the floor's input has been used; the decision owns the attribute from here
        report(v, why ? 'hardware:' + why : 'software');
    };
    window.__wrenVideoPolicy.decide = decide;
    // Every <video> of the document, through shadow roots (the fullscreen emulation below widens it).
    let allVideos = () => [...document.querySelectorAll('video')];
    // Fullscreen for a video on the hardware plane. WebKit's real fullscreen leaves the page black and the plane where it was: once an
    // element is fullscreen, the video layer is no longer painted as a hole at all (no rectangle reaches the sink again), so the plane
    // stays at its old place under a black page (TV, 2026-11-20, a plain MP4 page and a player page alike). The same effect without the
    // engine's fullscreen: the element is stretched over the whole page with fixed positioning, the document answers as in fullscreen
    // (fullscreenElement, fullscreenchange), and the video's layer keeps painting its hole where the element now is. Only for an element
    // that is, or contains, a video on the hardware plane; everything else (and software videos) keeps WebKit's own fullscreen.
    {
        const emu = { el: null, scroll: null, href: '' };
        // Closed shadow roots hide themselves from el.shadowRoot: remember them when they are made (this file runs before the page's scripts).
        const closedRoots = new WeakMap();
        const attachShadow = Element.prototype.attachShadow;
        if (attachShadow) Element.prototype.attachShadow = function (init) {
            const root = attachShadow.apply(this, arguments);
            if (init && init.mode === 'closed') closedRoots.set(this, root);
            return root;
        };
        const shadowOf = e => e.shadowRoot || closedRoots.get(e) || null;
        // The videos of an element, through shadow roots too (a player built as a custom element keeps its <video> in its own shadow tree).
        const videosIn = el => {
            if (el instanceof HTMLVideoElement) return [el];
            const out = [];
            const walk = root => {
                if (!root || !root.querySelectorAll) return;
                out.push(...root.querySelectorAll('video'));
                for (const e of root.querySelectorAll('*')) { const s = shadowOf(e); if (s) walk(s); }
            };
            walk(el);
            if (shadowOf(el)) walk(shadowOf(el));
            return out;
        };
        allVideos = () => videosIn(document.documentElement);
        // document.fullscreenElement is retargeted against the document, as WebKit's own: an element inside a shadow tree is answered as its host.
        const retarget = n => { for (let r = n.getRootNode(); r instanceof ShadowRoot; r = n.getRootNode()) n = r.host; return n; };
        const hardwareVideoIn = el => {
            if (!policy.on || !el) return false;
            const vs = videosIn(el);
            return vs.some(v => v.isConnected && !v.hasAttribute('wren-software') && v.getAttribute('wren-policy') !== null && v.__wrenPolicy === 'hw');
        };
        const userGesture = () => !navigator.userActivation || navigator.userActivation.isActive;
        const movesToHardware = el => policy.on && !!el.querySelectorAll && videosIn(el).some(v =>
            v.isConnected && v.getAttribute('wren-policy') !== 'page' && !String(v.currentSrc || v.src || '').startsWith('blob:'));
        // Tell the browser UI (engine: wpebFullscreen), which hides its toolbar while a page is fullscreen: WebKit's own
        // fullscreen reports itself, this emulated one has to.
        const tellUi = on => {
            try { window.webkit.messageHandlers.wpebFullscreen.postMessage(on ? '1' : '0'); } catch (e) {}
        };
        // As WebKit's own: one event of each name at the element (it bubbles to the document); at the document when the element is gone.
        const fire = el => {
            for (const name of ['fullscreenchange', 'webkitfullscreenchange']) {
                try { (el.isConnected ? el : document).dispatchEvent(new Event(name, { bubbles: true, composed: true })); } catch (e) {}
            }
        };
        // Inline properties this emulation wrote, put back one by one on leave. node -> Map(property -> what it was, and what we wrote).
        // Not a snapshot of the style attribute: a page that changes its own inline style while fullscreen keeps its change, and a
        // page whose CSP forbids inline styles (setAttribute('style') is blocked, CSSOM is not) gets its CSSOM-set styles back.
        const saved = new Map();
        const put = (node, props) => {
            let m = saved.get(node);
            if (!m) saved.set(node, m = new Map());
            for (const [k, v] of props) {
                if (!m.has(k)) m.set(k, { orig: node.style.getPropertyValue(k), prio: node.style.getPropertyPriority(k) });
                node.style.setProperty(k, v, 'important');
                m.get(k).ours = node.style.getPropertyValue(k);
            }
        };
        const restoreStyles = () => {
            for (const [node, m] of saved) for (const [k, o] of m) {
                // The page rewrote this property meanwhile (or replaced the whole style): its value stays.
                if (node.style.getPropertyPriority(k) !== 'important' || node.style.getPropertyValue(k) !== o.ours) continue;
                if (o.orig) node.style.setProperty(k, o.orig, o.prio); else node.style.removeProperty(k);
            }
            for (const node of saved.keys()) if (node.getAttribute('style') === '') node.removeAttribute('style');
            saved.clear();
        };
        // The element's parent in the flattened tree: out of a shadow root into its host, out of a slot's content into the slot.
        const flatParent = n => n.assignedSlot || n.parentElement || ((r => (r instanceof ShadowRoot ? r.host : null))(n.getRootNode()));
        // Every ancestor that traps a fixed element (containing block, clip) or that holds it under something else (stacking context).
        const strip = start => {
            for (let a = flatParent(start); a; a = flatParent(a)) {
                const cs = getComputedStyle(a);
                const props = [];
                const not = (v, none) => v && v !== none;
                if (not(cs.transform, 'none')) props.push(['transform', 'none']);
                for (const p of ['translate', 'rotate', 'scale', 'filter', 'perspective', 'clip-path', 'backdrop-filter', '-webkit-backdrop-filter'])
                    if (not(cs.getPropertyValue(p), 'none')) props.push([p, 'none']);
                if (not(cs.contain, 'none')) props.push(['contain', 'none']);
                if (not(cs.containerType, 'normal')) props.push(['container-type', 'normal']);
                if (not(cs.contentVisibility, 'visible')) props.push(['content-visibility', 'visible']);
                if (not(cs.willChange, 'auto')) props.push(['will-change', 'auto']);
                if (not(cs.isolation, 'auto')) props.push(['isolation', 'auto']);
                if (not(cs.mixBlendMode, 'normal')) props.push(['mix-blend-mode', 'normal']);
                if ((cs.maskImage || cs.webkitMaskImage || 'none') !== 'none') props.push(['mask', 'none'], ['-webkit-mask', 'none']);
                if (+cs.opacity < 1) props.push(['opacity', '1']);
                if (cs.zIndex !== 'auto' || cs.position === 'fixed' || cs.position === 'sticky') props.push(['z-index', '2147483647']);
                // Without this, setting transform: none starts the page's own "transition: transform" and the element rides the animation.
                if (props.length) put(a, props.concat([['transition', 'none']]));
            }
        };
        const FILL = [['position', 'fixed'], ['top', '0'], ['left', '0'], ['right', '0'], ['bottom', '0'], ['width', '100%'], ['height', '100%'],
                      ['max-width', 'none'], ['max-height', 'none'], ['min-width', '0'], ['min-height', '0'], ['margin', '0'], ['padding', '0'],
                      ['border', '0'], ['box-sizing', 'border-box'], ['transform', 'none'], ['translate', 'none'], ['z-index', '2147483647'],
                      ['visibility', 'visible'], ['opacity', '1'], ['object-fit', 'contain']];
        const isFrame = n => n.tagName === 'IFRAME' || n.tagName === 'FRAME';
        const enter = el => {
            const swap = !!emu.el;               // another element asks while one is fullscreen: switch, one change event, no exit
            if (swap) { restoreStyles(); emu.el = null; }
            emu.el = el;
            if (!swap) emu.scroll = [scrollX, scrollY];
            emu.href = location.href;
            strip(el);
            // A container: the page's own CSS (:fullscreen video { ... }) cannot match an emulated fullscreen, so its video is made to fill
            // the screen here too, and freed from its own trapping ancestors.
            if (!(el instanceof HTMLVideoElement) && !isFrame(el))
                for (const v of videosIn(el)) if (v.isConnected && v.__wrenPolicy === 'hw') { strip(v); put(v, FILL); }
            put(el, FILL.concat([['background-color', '#000']]));
            for (const node of [document.documentElement, document.body]) if (node) put(node, [['overflow', 'hidden']]);
            document.documentElement.setAttribute('wren-emulated-fullscreen', '');
            log({ fullscreen: 'emulated enter', tag: el.tagName, swap });
            tellUi(true);
            fire(el);
            // The player inside an iframe cannot reach past its own frame: the parent makes the frame element cover the screen.
            if (window !== top) try { parent.postMessage({ wrenFullscreen: 'enter' }, '*'); } catch (e) {}
        };
        function leave() {
            const el = emu.el;
            if (!el) return;
            emu.el = null;
            restoreStyles();
            document.documentElement.removeAttribute('wren-emulated-fullscreen');
            // Back to where the page was, unless the page went somewhere else meanwhile (single-page navigation), and without animating.
            if (emu.scroll && emu.href === location.href) scrollTo({ left: emu.scroll[0], top: emu.scroll[1], behavior: 'instant' });
            log({ fullscreen: 'emulated leave', tag: el.tagName });
            tellUi(false);
            fire(el);
            if (isFrame(el)) try { el.contentWindow.postMessage({ wrenFullscreen: 'exit' }, '*'); } catch (e) {}
            if (window !== top) try { parent.postMessage({ wrenFullscreen: 'leave' }, '*'); } catch (e) {}
        }
        // The emulated fullscreen ends when its element is no longer in the document (the real one does too), or the page moves on.
        setInterval(() => { if (emu.el && !emu.el.isConnected) leave(); }, 500);
        for (const name of ['pagehide', 'popstate', 'hashchange']) addEventListener(name, () => { if (emu.el) leave(); });
        addEventListener('message', e => {
            const d = e.data;
            if (!d || typeof d !== 'object' || !d.wrenFullscreen || e.source === window) return;
            if (d.wrenFullscreen === 'exit') { if (e.source === parent && emu.el) leave(); return; }
            const frame = [...document.querySelectorAll('iframe, frame')].find(f => f.contentWindow === e.source);
            if (!frame) return;
            if (d.wrenFullscreen === 'leave') { if (emu.el === frame) leave(); return; }
            // 'enter': only with the user's activation, and only for a frame the page allowed to go fullscreen.
            const allowed = frame.allowFullscreen || /\bfullscreen\b/.test(frame.getAttribute('allow') || '') || (() => { try { return !!frame.contentDocument; } catch (x) { return false; } })();
            if (allowed && (!navigator.userActivation || navigator.userActivation.isActive)) enter(frame);
        }, true);
        for (const [name, value] of [['fullscreenElement', () => retarget(emu.el)], ['webkitFullscreenElement', () => retarget(emu.el)],
                                     ['webkitCurrentFullScreenElement', () => retarget(emu.el)], ['fullscreen', () => !!emu.el], ['webkitIsFullScreen', () => !!emu.el]]) {
            try {
                const original = Object.getOwnPropertyDescriptor(Document.prototype, name);
                Object.defineProperty(document, name, { configurable: true, get() { return emu.el ? value() : (original && original.get ? original.get.call(document) : null); } });
            } catch (e) {}
        }
        for (const name of ['requestFullscreen', 'webkitRequestFullscreen', 'webkitRequestFullScreen']) {
            const original = Element.prototype[name];
            if (!original) continue;
            Element.prototype[name] = function () {
                // Like the real one, only inside a user gesture (an ad script must not take the screen). A software video that the
                // fullscreen rule moves to the hardware plane (every one but an MSE stream, which cannot be reloaded) is emulated too:
                // WebKit's real fullscreen would leave the plane behind once the reload puts it there.
                if (userGesture() && (hardwareVideoIn(this) || movesToHardware(this))) { enter(this); return Promise.resolve(); }
                return original.apply(this, arguments);
            };
        }
        for (const name of ['exitFullscreen', 'webkitExitFullscreen', 'webkitCancelFullScreen']) {
            const original = Document.prototype[name];
            if (!original) continue;
            Document.prototype[name] = function () {
                if (emu.el) { leave(); return Promise.resolve(); }
                return original.apply(this, arguments);
            };
        }
        // The remote's Back/Escape ends it, as it ends the real one.
        addEventListener('keydown', e => {
            if (emu.el && (e.key === 'Escape' || e.keyCode === 27 || e.keyCode === 461 || e.key === 'GoBack' || e.key === 'BrowserBack')) {
                leave();
                e.preventDefault();
                e.stopPropagation();
            }
        }, true);
    }
    // A page that marks a video itself wins and is never re-decided. A wren-software this build wrote is NOT the page's:
    // the floor's preview marking (__wrenPreview) is an input to the policy, not an answer, and the floor writes it
    // immediately before the decision runs -- so this must ignore it, or every preview would read as page intent.
    function pageMarked(v) {
        if (v.hasAttribute('wren-hw')) return true;
        // A wren-software written by a previous decision of ours (the element is marked decided) is ours, not the page's.
        return v.hasAttribute('wren-software') && !v.__wrenPreview && v.__wrenDecided !== true;
    }
    // Read once per start, before anything is written: a page's wren-hw paired with its own wren-software means
    // hardware (wren-hw is the decoder choice, wren-software the renderer's default), and the default is then dropped.
    const reconcile = v => {
        if (v.__wrenPreview || !v.hasAttribute('wren-hw') || !v.hasAttribute('wren-software')) return;
        v.removeAttribute('wren-software');
        v.__wrenPreview = true; // "do not treat the removal as the page's own marking again"
    };
    // The marking hooks above (src setter, load(), inserted nodes) call this, before the player starts.
    const markVideo = v => {
        if (!(v instanceof HTMLVideoElement)) return;
        // The marking hooks run on every load; the answer is decided once per element, the first time it starts.
        if (v.__wrenDecided && v.getAttribute('wren-policy') !== 'page' && v.__wrenPolicy !== null) return;
        reconcile(v);
        if (pageMarked(v)) { if (v.getAttribute('wren-policy') !== 'page') { v.setAttribute('wren-policy', 'page'); v.__wrenPolicy = v.hasAttribute('wren-software') ? 'sw' : 'hw'; report(v, 'page'); } return; }
        decide(v);
    };
    // The reload a fullscreen transition needs, at the element's own media time: only when the answer changes.
    const transition = v => {
        const t = v.currentTime || 0, wasPlaying = !v.paused && !v.ended;
        log({ reload: true, at: t, from: v.__wrenPolicy });
        decide(v);
        v.addEventListener('loadedmetadata', () => {
            if (t > 0.01) { try { v.currentTime = t; } catch (e) {} }
            if (wasPlaying) { const p = v.play(); if (p && p.catch) p.catch(() => {}); }
        }, { once: true });
        try { v.load(); } catch (e) {}
    };
    // The reasons a fullscreen transition can create or destroy: `fullscreen` is the one it always changes (into it,
    // and out of it back to the box rule).
    const FULLSCREEN_WHY = new Set(['fullscreen']);
    const onFullscreenChange = () => {
        // Videos whose answer a transition changes: the fullscreen one (and, on the way out, those that were hardware
        // only because they were fullscreen). Each is decided afresh, then compared with what its player has, and the
        // player that no longer matches is reloaded once, at its media time.
        const work = [];
        for (const v of allVideos()) {
            if (!v.isConnected || v.getAttribute('wren-policy') === 'page' || !policy.on) continue;
            // An MSE stream (blob: source) cannot be reloaded: load() detaches its MediaSource and the site's player breaks.
            // It keeps the player it started with; the new decision applies on its next natural load.
            if (String(v.currentSrc || v.src || '').startsWith('blob:')) { log({ reload: false, reason: 'mse' }); continue; }
            const was = v.getAttribute('wren-policy'), running = v.__wrenPolicy;
            decide(v);
            // An element without a source (a page's spare players) has nothing to reload: its decision applies when it gets one.
            if (!v.currentSrc && !v.getAttribute('src') && !v.querySelector('source')) continue;
            if (v.__wrenPolicy !== running && (FULLSCREEN_WHY.has(was) || FULLSCREEN_WHY.has(v.getAttribute('wren-policy'))))
                work.push(v);
        }
        for (const v of work) transition(v);
    };
    for (const ev of ['fullscreenchange', 'webkitfullscreenchange']) document.addEventListener(ev, onFullscreenChange, true);
    // A muted video in the page that is unmuted (a preview the user turns the sound on for) is decided again: with sound it belongs on the
    // hardware plane, so it is reloaded there at its media time, as for a fullscreen transition (not an MSE stream: it cannot be reloaded).
    document.addEventListener('volumechange', e => {
        const v = e.target;
        if (!(v instanceof HTMLVideoElement) || !policy.on || v.muted || v.volume === 0) return;
        if (v.__wrenPolicy !== 'sw' || v.getAttribute('wren-policy') === 'page') return;
        if (String(v.currentSrc || v.src || '').startsWith('blob:')) { log({ reload: false, reason: 'mse' }); return; }
        const running = v.__wrenPolicy;
        decide(v);
        if (v.__wrenPolicy !== running) transition(v);
    }, true);
    // Announced synchronously inside the page's own user-gesture handler (never deferred: the original runs at once),
    // so the engine log has the edge the transition can be measured against (DESIGN.md §3.5).
    const announce = el => {
        log({ fullscreen: !policy.current() ? 'enter' : 'leave', tag: (el && el.tagName) || '' });
    };
    {
        const proto = Element.prototype;
        const rf = proto.requestFullscreen;
        if (rf) proto.requestFullscreen = function () { announce(this); return rf.apply(this, arguments); };
        const wrf = proto.webkitRequestFullscreen || proto.webkitRequestFullScreen;
        if (wrf) proto.webkitRequestFullscreen = proto.webkitRequestFullScreen = function () { announce(this); return wrf.apply(this, arguments); };
    }
    // Protected (DRM) content only plays on the hardware plane (the CDM is 32-bit and decodes in hardware), so the
    // policy has to know. Later than the decision if the page sets the keys after load(); the next load() and any
    // fullscreen transition pick it up, and a running player is not touched (it is showing something).
    {
        const mk = HTMLMediaElement.prototype.setMediaKeys;
        if (mk) HTMLMediaElement.prototype.setMediaKeys = function (keys) {
            if (keys) { this.__wrenEmeActive = true; try { this.setAttribute('wren-policy', 'protected'); } catch (e) {} }
            return mk.apply(this, arguments);
        };
    }
    // Playback health of a software player, 2 s in and 2 s later: frames decoded, frames WebKit gave up on, and the
    // source's real size. The numbers that say whether a 480p video on this TV is smooth (DESIGN.md §6).
    const quality = (v, when) => { try {
        const q = v.getVideoPlaybackQuality ? v.getVideoPlaybackQuality() : null;
        if (q) log({ quality: when, decoded: q.totalVideoFrames, dropped: q.droppedVideoFrames, inPage: software(v),
            source: (v.videoWidth || 0) + 'x' + (v.videoHeight || 0) });
    } catch (e) {} };
    document.addEventListener('play', e => {
        const v = e.target;
        if (!(v instanceof HTMLVideoElement)) return;
        setTimeout(() => quality(v, 'after 2s'), 2000);
        setTimeout(() => quality(v, 'after 4s'), 4000);
    }, true);
    window.__wrenHybridReady = true;
})();
