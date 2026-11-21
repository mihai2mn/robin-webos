// SPDX-License-Identifier: GPL-3.0-or-later
// Motion recorder: run in a page (the browser's debug-script launch parameter, or pasted into any browser's console).
// Every rendered frame it samples the boxes of the things that can move by themselves (fixed/sticky elements, elements
// with running animations or transitions, videos, anything matching window.__motionSelectors) plus the scroll offset,
// and prints only what changed, in batches, as console lines starting with "MOTION ". tools/tvdrive/motion.js turns
// those lines into a text description of the motion (and an SVG timeline).
(() => {
  if (window.__motionRecorder) return;
  const ids = new WeakMap(), names = {}, last = {};
  let nextId = 1, t0 = performance.now(), frames = [], newNames = {}, lastScan = 0, tracked = [];
  const name = el => el.tagName + (el.id ? '#' + el.id : '') +
    (typeof el.className === 'string' && el.className.trim() ? '.' + el.className.trim().split(/\s+/).slice(0, 2).join('.') : '');
  const idOf = el => { let i = ids.get(el); if (!i) { i = nextId++; ids.set(el, i); names[i] = newNames[i] = name(el); } return i; };
  // Which elements to follow: cheap to find, re-scanned twice a second (new overlays, players that appear later).
  const scan = () => {
    const set = new Set();
    for (const sel of ['video', ...(window.__motionSelectors || [])]) { try { document.querySelectorAll(sel).forEach(e => set.add(e)); } catch (e) {} }
    try { for (const a of document.getAnimations()) { const e = a.effect && a.effect.target; if (e && e.nodeType === 1) set.add(e); } } catch (e) {}
    const all = document.body ? document.body.getElementsByTagName('*') : [];
    for (let i = 0, n = Math.min(all.length, 4000); i < n; i++) {
      const p = getComputedStyle(all[i]).position;
      if (p === 'fixed' || p === 'sticky') set.add(all[i]);
    }
    tracked = [...set].slice(0, 200);
  };
  const r1 = v => Math.round(v * 10) / 10;
  const tick = now => {
    if (now - lastScan > 500) { scan(); lastScan = now; }
    const row = [r1(now - t0), Math.round(scrollX), Math.round(scrollY)];
    const scrollKey = row[1] + ',' + row[2];
    let changed = last.scroll !== scrollKey;
    last.scroll = scrollKey;
    for (const el of tracked) {
      if (!el.isConnected) continue;
      const b = el.getBoundingClientRect();
      if (b.width === 0 && b.height === 0) continue;
      const o = Math.round(parseFloat(getComputedStyle(el).opacity) * 100) / 100;
      const id = idOf(el), rec = [id, r1(b.left), r1(b.top), r1(b.width), r1(b.height), o], key = rec.join(',');
      if (last[id] !== key) { last[id] = key; row.push(rec); changed = true; }
    }
    // A frame with no change is still a frame: its time is kept (for frame gaps), its content is not.
    frames.push(changed ? row : [row[0]]);
    if (frames.length >= 30) flush();
    requestAnimationFrame(tick);
  };
  const flush = () => {
    if (!frames.length) return;
    console.log('MOTION ' + JSON.stringify({ vw: innerWidth, vh: innerHeight, els: newNames, f: frames }));
    frames = []; newNames = {};
  };
  setInterval(flush, 1000);
  addEventListener('pagehide', flush);
  window.__motionRecorder = { flush, mark: label => { flush(); console.log('MOTION ' + JSON.stringify({ mark: String(label), t: r1(performance.now() - t0) })); } };
  requestAnimationFrame(tick);
})();
