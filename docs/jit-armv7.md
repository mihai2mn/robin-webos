# The ARMv7 JIT on WebKit 2.54

## What was broken

WebKit 2.54 turned the JavaScript JIT off on every 32-bit build. Without it JavaScriptCore runs its interpreter,
which is very slow on the TV's CPU. The 32-bit code paths had also stopped compiling, because nobody builds them
upstream any more. The 2.50.5 engine upstream Wren used had a working JIT. This fork wanted the same on 2.54.

Target: ARMv7 Thumb-2, webOS's **softfp** ABI, 32-bit values (`JSVALUE32_64`), Baseline JIT and DFG on, FTL off.

## What the fixes are

All are patches in `buildroot-external/patches/wpewebkit/2.54.3/` ([patches.md](patches.md)). Each follows the
64-bit code or the 2.50.5 code. None changes 64-bit behaviour.

- **0004** compiles the JIT for ARMv7 softfp and enables it (V8's model: doubles travel in core registers only at C
  call boundaries).
- **0015** makes the 32-bit Baseline JIT and DFG compile and link again: includes that 2.54 dropped from shared
  headers, 32-bit typos, a link stub for a megamorphic getter handler that only exists on 64-bit, and two ARMv7
  `moveConditionally` overloads that shared DFG code needs.
- **0016** gives ARMv7 Linux its run-time default back (2.54 narrowed it to 64-bit).
- **0017** stops three places from treating a handler inline cache as the repatching kind. They wrote members the
  smaller object does not have, which corrupted memory in release builds.
- **0018** fixes the baseline `put_by_val` profile register. It was r9, the same register as the inline cache
  handler, so a live profile pointer was overwritten by the handler.
- **0019** takes r11 (the JIT data register) out of the DFG's allocatable registers.
- **0020** ports the 32-bit DFG to 2.54's data-IC property caches. Before it, the first property access in DFG code
  crashed.
- **0021** initialises value profiles to the empty value on 32-bit. Zeroed profiles decoded as the double 0.0, and a
  DFG-compiled `Map.prototype.forEach` looped forever.
- **0023** keeps the **RegExp JIT off**. With it on, one framework documentation site crashed its web process about
  2 s after load, 6 times out of 6. With the RegExp JIT off, 0 out of 6.

The safe fallback if the DFG is suspect is `useDFGJIT = false` with patches 0015 to 0018.

## How it was verified

All of it on the TV, with the `jsc` shell and with the full engine. The log is `build/jit254/REPORT.md` (not part
of this repository).

- A differential suite of 12 files and **185 cases**, with expected values worked out by hand, not captured from a
  run. Default, baseline-only and interpreter-only configurations all pass 185 of 185 with identical output.
- Stress runs with the DFG on at low thresholds, in three configurations, byte-identical to the interpreter's
  output: 36 of 36.
- A soak of 20 real pages and an Angular page, 60 s each with scripted scrolling, default environment: **no web
  process crash** after 0023.

## What was not shown

- A page load was **not faster** with the JIT in these tests. Loads are limited by network and layout, and the JIT
  compiles during the load. The suite measures correctness. There is no steady-state script benchmark for the 32-bit
  JIT in this repository.
- The 64-bit engine ([engine64.md](engine64.md)) ran a small benchmark set 42.3 times faster than the 32-bit
  **interpreter** (before the 32-bit JIT worked). That compares a JIT with an interpreter. It does not say that
  64-bit is that much faster than the 32-bit JIT.
- Only one TV (LG CX, webOS 5.3) was used.
