#!/usr/bin/env node
// SPDX-License-Identifier: GPL-3.0-or-later
// Selftest for the two text analyzers: makes synthetic inputs in a temp folder (no TV, no network), runs motion.js and
// frames.js on them and checks the text they print. One "ok <case>" or "FAIL <case>: expected ... / got ..." line per
// case; exit code 1 if any case fails.
//   node selftest.js [name-filter]
'use strict';
const fs = require('fs');
const os = require('os');
const path = require('path');
const { spawnSync } = require('child_process');

const HERE = __dirname, TMP = fs.mkdtempSync(path.join(os.tmpdir(), 'tvdrive-selftest-'));
const W = 32, H = 18, N = W * H;   // frames.js input: 32x18 8-bit gray cells, 576 bytes per frame
const FRAME_MS = 1000 / 60;
let tmpCount = 0;
const tmpFile = (name, data) => { const p = path.join(TMP, `${tmpCount++}-${name}`); fs.writeFileSync(p, data); return p; };
const run = (script, args) => {
  const r = spawnSync(process.execPath, [path.join(HERE, script), ...args], { cwd: TMP, encoding: 'utf8', maxBuffer: 16 << 20 });
  if (r.error) throw r.error;
  return (r.stdout || '') + (r.stderr || '');
};

// ---- motion.js input: log lines "MOTION {json}" as motion-recorder.js prints them (a console prefix may precede).
const r1 = v => Math.round(v * 10) / 10;
// samples: [{ t, els? }] where els is a {name: row} map (row = [id, x, y, w, h, opacity]); an element whose row did not
// change since the previous sample is left out of that frame, like the recorder does. Changed-only log, like the TV's.
const motionLog = samples => {
  const lines = ['[console] some unrelated page line', 'MOTION ' + JSON.stringify({ vw: 1920, vh: 1080, els: { 1: 'DIV#box' }, f: [] })];
  let prev = {};
  for (const s of samples) {
    // Every recorder row starts [t, scrollX, scrollY, ...rows] (motion.js reads element rows from index 3 onwards).
    const row = [r1(s.t), s.sx || 0, s.sy || 0];
    const els = s.els || {};
    for (const id of Object.keys(els)) {
      const key = JSON.stringify(els[id]);
      if (prev[id] !== key) { row.push(els[id]); prev[id] = key; }
    }
    lines.push('MOTION ' + JSON.stringify({ f: [row] }));   // nothing changed in a frame: the row keeps just its time
  }
  return lines.join('\n') + '\n';
};
const elementFrames = (ms, at) => { const out = []; for (let t = 0; t <= ms + 1e-6; t += FRAME_MS) out.push({ t, els: { 1: at(t) } }); return out; };
const box = (x, y) => [1, x, y, 300, 100, 1];

// (a) cubic-bezier(0, 0, 0.25, 1) (ease-out) from y=0 to y=-70 over 250 ms.
const easeOut = p => p < 1 ? 1 - Math.pow(1 - p, 3) : 1;
// (b) the box jumps between y=0 and y=-70, once every 150 ms, for 600 ms.
const jumpY = t => (Math.floor(t / 150) % 2 ? -70 : 0);
// (c) frames 16.7 ms apart with one 120 ms gap (the analyzer reports a gap longer than 50 ms as a long frame).
const gapped = [];
for (let t = 0; t <= 400; t += FRAME_MS) gapped.push({ t, els: { 1: box(0, 0) } });

const motionCases = [
  { name: 'motion ease-out move', args: ['motion.js'], expect: /move\b[^;]*ease-out/, log: motionLog(elementFrames(500, t => box(0, -70 * easeOut(Math.min(1, t / 250))))) },
  { name: 'motion flicker (4 jumps in 600 ms)', args: ['motion.js'], expect: /FLICKER/, log: motionLog(elementFrames(700, t => box(0, jumpY(t)))) },
  { name: 'motion long frame', args: ['motion.js'], expect: /1 long frame\(s\)/, log: motionLog(gapped.map(s => (s.t >= 150 && s.t < 300 ? { t: s.t + 120 } : s))) },
  { name: 'motion nothing moved', args: ['motion.js'], expect: /nothing moved/, log: motionLog(elementFrames(500, () => box(0, 0))) },
];

// ---- frames.js input: raw gray frames (576 bytes each) + times.txt ("index seconds" per line).
const frameTimes = (n, gap) => Array.from({ length: n }, (_, i) => `${i} ${(i * gap).toFixed(3)}`).join('\n') + '\n';
// 60x60 screen pixels per cell: a block a few cells wide stays in the "region" line when it moves.
const picture = paint => { const f = Buffer.alloc(N, 8); paint((x, y, v) => { if (x >= 0 && x < W && y >= 0 && y < H) f[y * W + x] = v; }); return f; };
const framesRaw = pics => Buffer.concat(pics.map(p => Buffer.from(p)));
const rawArgs = (pics, gap) => ['frames.js', tmpFile('frames.raw', framesRaw(pics)), tmpFile('times.txt', frameTimes(pics.length, gap))];
// (e) 40 identical frames of a mid-range picture (no cell below 20 or above 232, so no "almost entirely" line).
const still = picture(set => { for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) set(x, y, 96 + ((x * 7 + y * 3) % 40)); });
// (f) an 8x4-cell (480x240 px) block crossing a mid-gray picture one cell per frame (cells mostly below 20 or above 232
// would make frames.js report them as almost black / almost white, which would not test the changing stretch).
const blockAt = x0 => picture(set => { for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) set(x, y, 120); for (let y = 5; y < 9; y++) for (let x = x0; x < x0 + 8; x++) set(x, y, 200); });
const blockFrames = []; for (let x = 0; x <= 16; x++) blockFrames.push(blockAt(x));
// (g) two different mid-gray pictures, alternating on every frame for 30 frames.
const picA = picture(set => { for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) set(x, y, 120); for (let y = 0; y < 9; y++) for (let x = 0; x < 8; x++) set(x, y, 200); });
const picB = picture(set => { for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) set(x, y, 120); for (let y = 9; y < H; y++) for (let x = 8; x < 20; x++) set(x, y, 200); });
// (h) all-black frames.
const black = picture(() => {});

const frameCases = [
  // 10 captures per second is what tvdrive frames takes by default: a full second of clock time, like on the TV.
  { name: 'frames static', args: rawArgs(Array.from({ length: 40 }, () => still), 0.1), expect: /static: nothing on screen changed/ },
  { name: 'frames changing', args: rawArgs(blockFrames, 0.1), expect: /changing/, reject: /FLICKER/ },
  { name: 'frames flicker', args: rawArgs(Array.from({ length: 30 }, (_, i) => (i % 2 ? picB : picA)), 0.1), expect: /FLICKER/ },
  { name: 'frames black', args: rawArgs(Array.from({ length: 12 }, () => black)), expect: /almost entirely black/ },
];

const filter = process.argv[2] || '';
let failed = 0;
for (const c of [...motionCases, ...frameCases]) {
  if (filter && !c.name.includes(filter)) continue;
  const log = c.args[0] === 'motion.js' ? [tmpFile('motion.log', c.log)] : [];
  let printed = '';
  try { printed = run(c.args[0], [...log, ...c.args.slice(1)]).trim(); } catch (e) { printed = 'error: ' + e.message; }
  const want = 'output matching ' + c.expect + (c.reject ? ' and not ' + c.reject : '');
  const ok = c.expect.test(printed) && !(c.reject && c.reject.test(printed));
  if (ok) console.log(`ok ${c.name}`);
  else { failed++; console.log(`FAIL ${c.name}: expected ${want} / got ${JSON.stringify(printed)}`); }
}
fs.rmSync(TMP, { recursive: true, force: true });
process.exit(failed ? 1 : 0);
