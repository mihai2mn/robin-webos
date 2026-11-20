# WebKit patches for WPE WebKit 2.54.3

Buildroot applies this folder instead of `../` when building 2.54.3 (`WEBKIT_VERSION=2.54.3 tools/build-engine.sh`).
Rebased from the 2.50.5 set and regenerated exactly against 2.54.3 (no fuzz). Dropped:

- 0003 (grayscale font antialiasing): 2.54 has WPE_SETTINGS_SUBPIXEL_LAYOUT_NONE as the default and maps unknown
  values to no subpixel order (grayscale).
- 0007 (GStreamer ring buffer size): fixed upstream.

0004 also re-enables the baseline JIT and the DFG on ARMv7 Linux (hardfp and softfp): 2.54 turns both off on every
32-bit build, which would cost the TV the JIT tiers 2.50.5 has. Upstream no longer tests 32-bit JIT.

0015-0021 are the 32-bit ARMv7 JIT/DFG bring-up on 2.54 (build/jit254/REPORT.md sec. 20-21):

- 0015 makes the 32-bit baseline JIT and DFG compile again; 0016 turns them on at runtime (Options.h/OptionsList.h).
- 0017 stops the baseline DataIC path downcasting a handler-IC cache to the repatching form (memory corruption in
  release builds); 0018 is the "Bug A" fix (put_by_val profile register aliased the handler register, r9).
- 0019 takes r11 (jitDataRegister) out of the allocatable register file; 0020 ports the 32-bit DFG to the 2.54
  DataIC property-cache design ("Bug C"). Without 0019/0020 a DFG-enabled 32-bit build crashes on the first property
  access; the safe fallback is `useDFGJIT = false` on ARMv7 (OptionsList.h) with 0015-0018 only.
- 0021 initialises the metadata-table value profiles to the empty JSValue on 32-bit (zeroed buckets decoded as the
  double 0.0, so the DFG mispredicted every unsampled site; this made Map.prototype.forEach loop forever once
  DFG-compiled at low thresholds).
