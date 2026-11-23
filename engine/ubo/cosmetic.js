// SPDX-License-Identifier: GPL-3.0-or-later
// Wren's cosmetic filtering (engine/ubo/extension.c runs this in an isolated world of every http(s) frame, before
// the document exists). Given by the extension:
//   __wrenUbo.css          the site's own hiding rules, as a style sheet
//   __wrenUbo.procedural   JSON: procedural filters ({selector: [{type, arg}...], action: {type, arg} | null})
//   __wrenUbo.exceptions   JSON: class/id selectors excepted on this site (handed back to __wrenUboHidden)
//   __wrenUbo.generichide  true: no generic hiding on this site
//   __wrenUboHidden(classes, ids, exceptions)  generic hiding rules for these classes and ids (newline-separated)
(function wrenUboCosmetic() {
    'use strict';
    const data = self.__wrenUbo;
    const hidden = self.__wrenUboHidden;
    if (!data || typeof hidden !== 'function') return;
    delete self.__wrenUbo;
    delete self.__wrenUboHidden;

    let procedural = [];
    try { procedural = JSON.parse(data.procedural).map(s => JSON.parse(s)); } catch (e) { procedural = []; }
    const exceptions = data.exceptions;

    // One style element for everything we hide, kept in the document if a page script removes it.
    const style = document.createElement('style');
    style.setAttribute('data-wren', '');
    const sheet = data.css || '';
    // New rules go into a style element of their own: reassigning the text of one ever-growing sheet re-parses all of it and restyles the
    // whole page for every batch.
    const styles = [style];
    const placeStyle = () => {
        const parent = document.head || document.documentElement;
        if (!parent) return false;
        for (const s of styles) if (s.parentNode !== parent) parent.appendChild(s);
        if (style.textContent !== sheet) style.textContent = sheet;
        return true;
    };
    const addRules = css => {
        if (!css) return;
        const s = document.createElement('style');
        s.setAttribute('data-wren', '');
        s.textContent = css;
        styles.push(s);
        placeStyle();
    };

    // Generic hiding: ask for the rules of classes and ids not seen yet in this document.
    const seenClasses = new Set(), seenIds = new Set();
    let pendingClasses = [], pendingIds = [];
    // deep: also the subtree (added nodes). An attribute change concerns the element itself only: a class toggled on <body> must not walk the document.
    const collect = (root, deep = true) => {
        if (data.generichide || !root || root.nodeType !== 1) return;
        const visit = el => {
            if (el.id && !seenIds.has(el.id)) { seenIds.add(el.id); pendingIds.push(el.id); }
            const cl = el.classList;
            for (let i = 0; cl && i < cl.length; i++) {
                const c = cl[i];
                if (!seenClasses.has(c)) { seenClasses.add(c); pendingClasses.push(c); }
            }
        };
        visit(root);
        if (!deep) return;
        const all = root.querySelectorAll('[id],[class]');
        for (let i = 0; i < all.length; i++) visit(all[i]);
    };
    const flushGeneric = () => {
        if (pendingClasses.length === 0 && pendingIds.length === 0) return;
        const classes = pendingClasses.join('\n'), ids = pendingIds.join('\n');
        pendingClasses = []; pendingIds = [];
        const css = hidden(classes, ids, exceptions);
        addRules(css);
    };

    // Procedural filters (uBO's :has-text, :upward, :matches-css, :xpath, ... and :remove/:style actions).
    // exact: a plain value must equal the whole string (:matches-css, :matches-attr, as uBO); :has-text and the like look for it inside.
    const textMatcher = (arg, exact) => {
        const m = /^\/(.+)\/([imsu]*)$/.exec(arg);
        if (m) { try { const re = new RegExp(m[1], m[2]); return s => re.test(s); } catch (e) { return null; } }
        return exact ? s => s === arg : s => s.includes(arg);
    };
    const pairMatcher = arg => {
        // name: value  (matches-css)  or  "name"="value"  (matches-attr); either side may be /regex/.
        const m = /^\s*"?(.+?)"?\s*[:=]\s*"?(.*?)"?\s*$/.exec(arg);
        if (!m) return null;
        return [textMatcher(m[1], true) || (() => false), m[2] === '' ? () => true : (textMatcher(m[2], true) || (() => false))];
    };
    const ops = {
        'css-selector': (nodes, arg, first) => {
            if (first) return Array.from(document.querySelectorAll(arg));
            const out = [];
            const sibling = /^\s*[+~]/.test(arg);
            const relative = /^\s*[>]/.test(arg) || /^\s/.test(arg);
            for (const n of nodes) {
                if (sibling) {
                    // A sibling combinator after an operator: querySelectorAll only finds descendants, so ask the parent about the marked node.
                    if (!n.parentElement) continue;
                    n.setAttribute('data-wren-ctx', '');
                    try { out.push(...n.parentElement.querySelectorAll('[data-wren-ctx]' + arg.trim())); } finally { n.removeAttribute('data-wren-ctx'); }
                } else if (relative) out.push(...n.querySelectorAll(':scope' + arg));
                else if (n.matches(arg)) out.push(n);
            }
            return out;
        },
        'has-text': (nodes, arg) => { const t = textMatcher(arg); return t ? nodes.filter(n => t(n.textContent || '')) : []; },
        'min-text-length': (nodes, arg) => nodes.filter(n => (n.textContent || '').length >= (parseInt(arg, 10) || 0)),
        'matches-path': (nodes, arg) => { const t = textMatcher(arg); return t && t(location.pathname + location.search) ? nodes : []; },
        'upward': (nodes, arg) => {
            const n = parseInt(arg, 10);
            return nodes.map(el => {
                if (String(n) === arg.trim()) { for (let i = 0; i < n && el; i++) el = el.parentElement; return el; }
                return el.parentElement ? el.parentElement.closest(arg) : null;
            }).filter(Boolean);
        },
        'xpath': (nodes, arg, first) => {
            const out = [];
            for (const ctx of first ? [document] : nodes) {
                const r = document.evaluate(arg, ctx, null, XPathResult.ORDERED_NODE_SNAPSHOT_TYPE, null);
                for (let i = 0; i < r.snapshotLength; i++) if (r.snapshotItem(i).nodeType === 1) out.push(r.snapshotItem(i));
            }
            return out;
        },
        'matches-attr': (nodes, arg) => {
            const p = pairMatcher(arg);
            if (!p) return [];
            return nodes.filter(n => Array.from(n.attributes).some(a => p[0](a.name) && p[1](a.value)));
        },
        'matches-css': (nodes, arg) => cssMatch(nodes, arg, null),
        'matches-css-before': (nodes, arg) => cssMatch(nodes, arg, '::before'),
        'matches-css-after': (nodes, arg) => cssMatch(nodes, arg, '::after'),
    };
    const cssMatch = (nodes, arg, pseudo) => {
        const p = pairMatcher(arg);
        if (!p) return [];
        const m = /^\s*(.+?)\s*:/.exec(arg);
        const prop = m ? m[1].replace(/^"|"$/g, '') : '';
        return nodes.filter(n => p[1](getComputedStyle(n, pseudo).getPropertyValue(prop)));
    };
    const done = new WeakMap(); // element -> filters already applied to it
    const runProcedural = () => {
        for (const f of procedural) {
            let nodes = [];
            let ok = true;
            let first = true;      // no node set yet: the next selector starts from the document
            f.selector.forEach(op => {
                if (!ok) return;
                const fn = ops[op.type];
                if (!fn) { ok = false; return; }
                // :matches-path(...) first is a condition on the page, not on nodes: it gates the rest of the filter.
                if (op.type === 'matches-path' && first) {
                    const t = textMatcher(op.arg);
                    if (!t || !t(location.pathname + location.search)) ok = false;
                    return;
                }
                try { nodes = fn(nodes, op.arg, first); } catch (e) { ok = false; }
                first = false;
            });
            if (!ok) continue;
            const action = f.action;
            for (const el of nodes) {
                let applied = done.get(el);
                if (applied && applied.has(f)) continue;
                if (!applied) done.set(el, applied = new Set());
                applied.add(f);
                if (!action) el.style.setProperty('display', 'none', 'important');
                else if (action.type === 'remove') el.remove();
                else if (action.type === 'style') el.setAttribute('style', (el.getAttribute('style') || '') + ';' + action.arg);
                else if (action.type === 'remove-attr') el.removeAttribute(action.arg);
                else if (action.type === 'remove-class') el.classList.remove(action.arg);
            }
        }
    };

    // Work in batches: at most every 100 ms while the document changes, every 400 ms while it is still being parsed. Each batch of generic
    // rules is a new style sheet, and each new sheet restyles the whole page: on a large article that was one full restyle every
    // 100 ms of the load (TV: about a second of a Wikipedia load). The site's own rules (data.css) are in place from the start.
    let scheduled = false;
    // The procedural filters run again only when nodes were added or an element's class/id changed (not for every mutation of the page).
    let needProcedural = true;
    const run = () => {
        scheduled = false;
        placeStyle();
        flushGeneric();
        if (procedural.length && needProcedural) { needProcedural = false; runProcedural(); }
    };
    const schedule = () => {
        if (scheduled) return;
        scheduled = true;
        setTimeout(run, document.readyState === 'loading' ? 400 : 100);
    };
    const observer = new MutationObserver(records => {
        for (const r of records) {
            if (r.type === 'attributes') { collect(r.target, false); if (r.target !== document.documentElement && r.target !== document.body) needProcedural = true; }
            else if (r.addedNodes.length) { needProcedural = true; for (const n of r.addedNodes) collect(n); }
            if (r.type === 'childList' && r.removedNodes.length) {
                for (const n of r.removedNodes) if (styles.includes(n)) { schedule(); break; }
            }
        }
        schedule();
    });
    // This runs when the window object is created, before the document it will show exists: watch whichever
    // document is current once it has an element, and again if it is replaced.
    let watched = null;
    const attach = () => {
        if (watched === document || !document.documentElement) return watched === document;
        if (watched) observer.disconnect();
        watched = document;
        observer.observe(document, { childList: true, subtree: true, attributes: true, attributeFilter: ['class', 'id'] });
        collect(document.documentElement);
        placeStyle();
        schedule();
        return true;
    };
    const boot = () => { if (!attach()) setTimeout(boot, 10); };
    boot();
    addEventListener('DOMContentLoaded', () => { attach(); collect(document.documentElement); needProcedural = true; run(); }, true);
    addEventListener('load', () => { attach(); needProcedural = true; schedule(); }, true);
})();
