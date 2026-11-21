#!/usr/bin/env node
// SPDX-License-Identifier: GPL-3.0-or-later
// Motion analyzer: reads the "MOTION {...}" console lines of tools/tvdrive/motion-recorder.js (from a browser log, any
// prefix before "MOTION" is ignored) and describes the motion in text: per element and property a list of segments
// (hold / move with duration, easing and speed / jump within one frame), reversals (flicker), frame-gap statistics.
//   node motion.js <log file | -> [--svg out.svg] [--from ms] [--to ms] [--eps px]
'use strict';
const fs = require('fs');
const args = process.argv.slice(2);
const opt = k => { const i = args.indexOf(k); return i < 0 ? null : args.splice(i, 2)[1]; };
const svgOut = opt('--svg'), from = +(opt('--from') || 0), to = +(opt('--to') || Infinity), eps = +(opt('--eps') || 1.5);
const text = fs.readFileSync(!args[0] || args[0] === '-' ? 0 : args[0], 'utf8');

const names = {}, frames = [], marks = [];
let vw = 0, vh = 0;
for (const line of text.split('\n')) {
  const at = line.indexOf('MOTION {');
  if (at < 0) continue;
  let j; try { j = JSON.parse(line.slice(at + 7)); } catch (e) { continue; }
  if (j.mark !== undefined) { marks.push(j); continue; }
  Object.assign(names, j.els || {}); vw = j.vw || vw; vh = j.vh || vh;
  for (const f of j.f || []) if (f[0] >= from && f[0] <= to) frames.push(f);
}
if (!frames.length) { console.log('no MOTION frames found'); process.exit(1); }
frames.sort((a, b) => a[0] - b[0]);

// Frame pacing.
const gaps = []; for (let i = 1; i < frames.length; i++) gaps.push(frames[i][0] - frames[i - 1][0]);
const sorted = [...gaps].sort((a, b) => a - b), pct = p => sorted.length ? sorted[Math.min(sorted.length - 1, Math.floor(p * sorted.length))] : 0;
const median = pct(0.5) || 16.7;
const janks = []; for (let i = 1; i < frames.length; i++) if (gaps[i - 1] > Math.max(50, 2.5 * median)) janks.push([frames[i - 1][0], gaps[i - 1]]);

// Series per track and property: [t, value], only where it changed (plus first and last sample time).
const series = {};  // key "id.prop" -> [[t, v], ...]
const push = (key, t, v) => { const s = series[key] || (series[key] = []); if (!s.length || s[s.length - 1][1] !== v) s.push([t, v]); };
const PROPS = ['x', 'y', 'w', 'h', 'opacity'];
for (const f of frames) {
  if (f.length < 3) continue;
  push('scroll.x', f[0], f[1]); push('scroll.y', f[0], f[2]);
  for (let i = 3; i < f.length; i++) { const r = f[i]; for (let p = 0; p < 5; p++) push(r[0] + '.' + PROPS[p], f[0], r[p + 1]); }
}
const tEnd = frames[frames.length - 1][0], tStart = frames[0][0];

// A changed-only series is a step function sampled per frame; rebuild per-frame samples between changes so that a
// value that stayed put for a while reads as a hold, then simplify (Ramer-Douglas-Peucker) to the points that matter.
const expand = s => { const out = []; for (let i = 0; i < s.length; i++) { out.push(s[i]); const next = i + 1 < s.length ? s[i + 1][0] : tEnd; if (next - s[i][0] > 2.5 * median) out.push([next - median, s[i][1]]); } return out; };
const rdp = (pts, e) => {
  if (pts.length < 3) return pts;
  const [t1, v1] = pts[0], [t2, v2] = pts[pts.length - 1];
  let worst = 0, at = 0;
  for (let i = 1; i < pts.length - 1; i++) { const lin = v1 + (v2 - v1) * (pts[i][0] - t1) / ((t2 - t1) || 1); const d = Math.abs(pts[i][1] - lin); if (d > worst) { worst = d; at = i; } }
  if (worst <= e) return [pts[0], pts[pts.length - 1]];
  return [...rdp(pts.slice(0, at + 1), e).slice(0, -1), ...rdp(pts.slice(at), e)];
};
const fmt = v => String(Math.round(v * 100) / 100), sec = ms => (ms / 1000).toFixed(2);
const describe = (key, s) => {
  const isOp = key.endsWith('opacity'), e = isOp ? 0.03 : eps;
  const full = expand(s), pts = rdp(full, e);
  if (pts.length < 2 || pts.every(p => Math.abs(p[1] - pts[0][1]) <= e)) return null;
  const segs = [];
  for (let i = 1; i < pts.length; i++) {
    const [ta, va] = pts[i - 1], [tb, vb] = pts[i], dt = tb - ta, dv = vb - va;
    if (Math.abs(dv) <= e) { if (segs.length && segs[segs.length - 1].kind === 'hold') segs[segs.length - 1].tb = tb; else segs.push({ kind: 'hold', ta, tb, va, vb: va }); continue; }
    // Easing: where the value is at the segment's mid time, from the raw samples (0.5 linear, >0.6 fast start, <0.4 slow start).
    const mid = ta + dt / 2; let near = null; for (const p of full) if (p[0] >= ta && p[0] <= tb && (!near || Math.abs(p[0] - mid) < Math.abs(near[0] - mid))) near = p;
    const prog = near ? (near[1] - va) / dv : 0.5;
    const kind = dt <= 1.6 * median ? 'jump' : 'move';
    // Consecutive moves in the same direction are one motion (a curve cut into pieces by the simplifier).
    const prev = segs[segs.length - 1];
    if (kind === 'move' && prev && prev.kind === 'move' && Math.sign(prev.vb - prev.va) === Math.sign(dv)) { prev.tb = tb; prev.vb = vb; prev.pieces++; continue; }
    segs.push({ kind, ta, tb, va, vb, prog, pieces: 1 });
  }
  // Easing of merged moves: from the raw samples over the whole motion.
  for (const g of segs) if (g.kind === 'move') {
    const mid = (g.ta + g.tb) / 2; let near = null; for (const p of full) if (p[0] >= g.ta && p[0] <= g.tb && (!near || Math.abs(p[0] - mid) < Math.abs(near[0] - mid))) near = p;
    g.prog = near ? (near[1] - g.va) / (g.vb - g.va) : 0.5;
    const q = g.ta + (g.tb - g.ta) / 4; let nq = null; for (const p of full) if (p[0] >= g.ta && p[0] <= g.tb && (!nq || Math.abs(p[0] - q) < Math.abs(nq[0] - q))) nq = p;
    g.quarter = nq ? (nq[1] - g.va) / (g.vb - g.va) : 0.25;
  }
  const moving = segs.filter(g => g.kind !== 'hold');
  let reversals = 0, flicker = false; const revTimes = [];
  for (let i = 1; i < moving.length; i++) if (Math.sign(moving[i].vb - moving[i].va) !== Math.sign(moving[i - 1].vb - moving[i - 1].va)) { reversals++; revTimes.push(moving[i].ta); }
  for (let i = 2; i < revTimes.length; i++) if (revTimes[i] - revTimes[i - 2] <= 1000) flicker = true;
  const parts = segs.map(g => {
    if (g.kind === 'hold') return `hold ${fmt(g.va)} for ${sec(g.tb - g.ta)}s`;
    if (g.kind === 'jump') return `JUMP ${fmt(g.va)} -> ${fmt(g.vb)} at ${sec(g.ta)}s`;
    const ease = g.prog > 0.62 ? 'ease-out' : g.prog < 0.38 ? 'ease-in' : g.quarter < 0.18 ? 'ease-in-out' : 'linear';
    const speed = isOp ? '' : `, ${Math.round(Math.abs(g.vb - g.va) / ((g.tb - g.ta) / 1000))} px/s`;
    return `move ${fmt(g.va)} -> ${fmt(g.vb)} in ${Math.round(g.tb - g.ta)} ms (${sec(g.ta)}-${sec(g.tb)}s, ${ease}${speed})`;
  });
  return { key, parts, reversals, flicker, pts };
};

const out = [];
out.push(`MOTION ${sec(tEnd - tStart)} s, ${frames.length} frames, viewport ${vw}x${vh}`);
out.push(`frame gap ms: median ${fmt(median)}, p95 ${fmt(pct(0.95))}, max ${fmt(sorted[sorted.length - 1] || 0)}; ${janks.length} long frame(s)` +
  (janks.length ? ': ' + janks.slice(0, 12).map(j => `${Math.round(j[1])} ms at ${sec(j[0])}s`).join(', ') : ''));
for (const m of marks) out.push(`mark "${m.mark}" at ${sec(m.t)}s`);
const byTrack = {};
for (const key of Object.keys(series)) { const d = describe(key, series[key]); if (d) (byTrack[key.split('.')[0]] || (byTrack[key.split('.')[0]] = [])).push(d); }
const trackName = id => id === 'scroll' ? 'page scroll' : `#${id} ${names[id] || '?'}`;
if (!Object.keys(byTrack).length) out.push('nothing moved');
for (const id of Object.keys(byTrack)) {
  out.push(`track ${trackName(id)}`);
  for (const d of byTrack[id]) {
    out.push(`  ${d.key.split('.')[1]}: ${d.parts.join('; ')}`);
    if (d.reversals) out.push(`    ${d.reversals} reversal(s)${d.flicker ? '  ** FLICKER: 3 or more direction changes within 1 s **' : ''}`);
  }
}
console.log(out.join('\n'));

if (svgOut) {
  // One strip per changing property: value over time, simplified points as dots, long frames as red bars.
  const rows = Object.values(byTrack).flat(), W = 1000, H = 70, L = 220, T = 20;
  const x = t => L + (W - L - 10) * (t - tStart) / ((tEnd - tStart) || 1);
  let svg = `<svg xmlns="http://www.w3.org/2000/svg" width="${W}" height="${T + rows.length * H + 10}" font-family="sans-serif" font-size="11">` +
    `<rect width="100%" height="100%" fill="#fff"/>`;
  for (const j of janks) svg += `<rect x="${x(j[0])}" y="${T}" width="${Math.max(1, x(j[0] + j[1]) - x(j[0]))}" height="${rows.length * H}" fill="#f88" opacity="0.4"/>`;
  rows.forEach((d, i) => {
    const s = series[d.key], vals = s.map(p => p[1]), lo = Math.min(...vals), hi = Math.max(...vals), y0 = T + i * H;
    const y = v => y0 + H - 12 - (H - 24) * (v - lo) / ((hi - lo) || 1);
    const [id, prop] = d.key.split('.');
    svg += `<text x="4" y="${y0 + 16}">${trackName(id).replace(/[<&>]/g, '').slice(0, 30)}</text><text x="4" y="${y0 + 30}" fill="#555">${prop} ${fmt(lo)}..${fmt(hi)}${d.flicker ? ' FLICKER' : ''}</text>` +
      `<line x1="${L}" x2="${W - 10}" y1="${y0 + H - 6}" y2="${y0 + H - 6}" stroke="#ddd"/>`;
    let path = ''; let prev = null;
    for (const p of [...s, [tEnd, s[s.length - 1][1]]]) { if (prev) path += `L${x(p[0])},${y(prev[1])}`; path += `${path ? 'L' : 'M'}${x(p[0])},${y(p[1])}`; prev = p; }
    svg += `<path d="${path}" fill="none" stroke="${d.flicker ? '#c00' : '#06c'}" stroke-width="1.5"/>`;
    for (const p of d.pts) svg += `<circle cx="${x(p[0])}" cy="${y(p[1])}" r="2.5" fill="#000"/>`;
  });
  for (let t = Math.ceil(tStart / 500) * 500; t <= tEnd; t += 500) svg += `<text x="${x(t)}" y="12" text-anchor="middle" fill="#777">${sec(t)}s</text>`;
  fs.writeFileSync(svgOut, svg + '</svg>');
  console.log(`svg: ${svgOut}`);
}
