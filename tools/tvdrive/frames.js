#!/usr/bin/env node
// SPDX-License-Identifier: GPL-3.0-or-later
// Frame fingerprints: what the SCREEN did over a few seconds, as text. Input: a raw file of 32x18 8-bit gray frames
// (tvdrive frames makes it from a burst of small screen captures, about 25 per second) and times.txt ("index seconds"
// per frame). Each frame is compared cell by cell (a cell is 60x60 screen pixels) with the one before and with the
// distinct pictures seen in the last second, which separates: nothing changes / something moves or plays (new pictures
// keep coming) / FLICKER (the screen keeps going back to a picture it showed a moment ago).
//   node frames.js <frames.raw> <times.txt> [--thr N]
'use strict';
const fs = require('fs');
const W = 32, H = 18, N = W * H, CELL = 60;
const args = process.argv.slice(2), ti = args.indexOf('--thr');
const THR = ti >= 0 ? +args.splice(ti, 2)[1] : 14;   // gray levels a cell must change by (JPEG noise is below this)
const raw = fs.readFileSync(args[0]);
const times = fs.readFileSync(args[1], 'utf8').trim().split('\n').map(l => +l.split(/\s+/)[1]);
const count = Math.min(Math.floor(raw.length / N), times.length);
if (count < 3) { console.log('too few frames'); process.exit(1); }
const frame = i => raw.subarray(i * N, (i + 1) * N), t = i => times[i] - times[0];
const diff = (a, b) => { let n = 0, x0 = W, y0 = H, x1 = -1, y1 = -1; for (let k = 0; k < N; k++) if (Math.abs(a[k] - b[k]) > THR) { n++; const x = k % W, y = (k / W) | 0; if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y; } return { n, x0, y0, x1, y1 }; };
const same = (a, b) => diff(a, b).n <= 2;
const look = f => { let s = 0, dark = 0, bright = 0; for (const v of f) { s += v; if (v < 20) dark++; if (v > 232) bright++; } return { mean: Math.round(s / N), dark: Math.round(100 * dark / N), bright: Math.round(100 * bright / N) }; };
const sec = v => v.toFixed(2), px = d => `x ${d.x0 * CELL}-${(d.x1 + 1) * CELL}, y ${d.y0 * CELL}-${(d.y1 + 1) * CELL}`;
const merge = (a, d) => { if (!a) return { ...d }; return { n: Math.max(a.n, d.n), x0: Math.min(a.x0, d.x0), y0: Math.min(a.y0, d.y0), x1: Math.max(a.x1, d.x1), y1: Math.max(a.y1, d.y1) }; };

// Per frame: did it change, and is it a picture that was on screen within the last second (a "return")?
const info = [{ changed: false, ret: false }];
const recent = [{ i: 0 }];   // one entry per distinct recent picture: its latest frame index
for (let i = 1; i < count; i++) {
  const f = frame(i), d = diff(frame(i - 1), f), changed = d.n > 2;
  let ret = false;
  if (changed) {
    while (recent.length && t(i) - t(recent[0].i) > 1.0) recent.shift();
    const hit = recent.find(r => r.i !== i - 1 && same(frame(r.i), f));
    if (hit) { ret = true; hit.i = i; } else recent.push({ i });
  } else if (recent.length) recent[recent.length - 1].i = i;
  info.push({ changed, ret, d });
}

// Runs: static stretches and changing stretches (a pause shorter than 0.6 s does not end a changing stretch).
const runs = [];
for (let i = 1; i < count; i++) {
  const c = info[i].changed, last = runs[runs.length - 1];
  if (c) {
    if (last && last.kind === 'change' && t(i) - t(last.end) < 0.6) { last.end = i; last.frames++; last.returns += info[i].ret ? 1 : 0; last.box = merge(last.box, info[i].d); if (!last.pics.some(k => same(frame(k), frame(i)))) last.pics.push(i); }
    else runs.push({ kind: 'change', start: i - 1, end: i, frames: 1, returns: info[i].ret ? 1 : 0, box: { ...info[i].d }, pics: same(frame(i - 1), frame(i)) ? [i] : [i - 1, i] });
  }
}
const out = [];
const dur = t(count - 1);
out.push(`SCREEN ${sec(dur)} s, ${count} captures (${(count / dur).toFixed(1)} per second), cells of ${CELL} px, change threshold ${THR}`);
const extremes = []; for (let i = 0; i < count; i++) { const l = look(frame(i)); if (l.dark >= 85) extremes.push([i, 'black']); else if (l.bright >= 85) extremes.push([i, 'white']); }
if (!runs.length) out.push('static: nothing on screen changed');
let cursor = 0;
for (const r of runs) {
  if (t(r.start) - cursor > 0.3) out.push(`${sec(cursor)}-${sec(t(r.start))}s static`);
  const len = t(r.end) - t(r.start), rate = r.returns / Math.max(len, 0.2);
  // Flicker: the screen keeps coming back to pictures it just showed. Motion and video keep producing new ones.
  // (An animation that swings back shows a few repeats at its turning point, among many different pictures.)
  const flicker = r.frames >= 5 && r.returns >= 3 && r.pics.length <= Math.max(3, r.frames / 4);
  const where = `${px(r.box)} (${Math.round(100 * ((r.box.x1 - r.box.x0 + 1) * (r.box.y1 - r.box.y0 + 1)) / N)}% of the screen)`;
  if (flicker) {
    const a = look(frame(r.start)), b = look(frame(r.start + 1));
    out.push(`${sec(t(r.start))}-${sec(t(r.end))}s ** FLICKER **: ${r.frames} changes among only ${r.pics.length} different pictures, ${r.returns} back to one shown within the last second (${rate.toFixed(1)} per s), region ${where}; pictures alternate around brightness ${a.mean} (${a.dark}% black, ${a.bright}% white) and ${b.mean} (${b.dark}% black, ${b.bright}% white)`);
  } else if (r.frames === 1) out.push(`${sec(t(r.end))}s one change, region ${where}`);
  else out.push(`${sec(t(r.start))}-${sec(t(r.end))}s changing: ${r.frames} changes in ${sec(len)} s, ${r.pics.length} different pictures (motion, scrolling or video), region ${where}`);
  cursor = t(r.end);
}
if (runs.length && dur - cursor > 0.3) out.push(`${sec(cursor)}-${sec(dur)}s static`);
if (extremes.length) {
  const kinds = {}; for (const [i, k] of extremes) (kinds[k] || (kinds[k] = [])).push(i);
  for (const k of Object.keys(kinds)) out.push(`${kinds[k].length} capture(s) almost entirely ${k}: first at ${sec(t(kinds[k][0]))}s, last at ${sec(t(kinds[k][kinds[k].length - 1]))}s`);
}
console.log(out.join('\n'));
