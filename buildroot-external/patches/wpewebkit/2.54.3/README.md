# WebKit patches for WPE WebKit 2.54.3

Buildroot applies this folder instead of `../` when building 2.54.3 (`WEBKIT_VERSION=2.54.3 tools/build-engine.sh`).
Rebased from the 2.50.5 set and regenerated exactly against 2.54.3 (no fuzz). Dropped:

- 0003 (grayscale font antialiasing): 2.54 has WPE_SETTINGS_SUBPIXEL_LAYOUT_NONE as the default and maps unknown
  values to no subpixel order (grayscale).
- 0007 (GStreamer ring buffer size): fixed upstream.

0004 also re-enables the baseline JIT and the DFG on ARMv7 Linux (hardfp and softfp): 2.54 turns both off on every
32-bit build, which would cost the TV the JIT tiers 2.50.5 has. Upstream no longer tests 32-bit JIT.

0015-0021 are the 32-bit ARMv7 JIT/DFG bring-up on 2.54 (see docs/jit-armv7.md):

- 0015 makes the 32-bit baseline JIT and DFG compile again; 0016 turns them on at runtime (Options.h/OptionsList.h).
- 0017 stops the baseline DataIC path downcasting a handler-IC cache to the repatching form (memory corruption in
  release builds); 0018 is the "Bug A" fix (put_by_val profile register aliased the handler register, r9).
- 0019 takes r11 (jitDataRegister) out of the allocatable register file; 0020 ports the 32-bit DFG to the 2.54
  DataIC property-cache design ("Bug C"). Without 0019/0020 a DFG-enabled 32-bit build crashes on the first property
  access; the safe fallback is `useDFGJIT = false` on ARMv7 (OptionsList.h) with 0015-0018 only.
- 0021 initialises the metadata-table value profiles to the empty JSValue on 32-bit (zeroed buckets decoded as the
  double 0.0, so the DFG mispredicted every unsampled site; this made Map.prototype.forEach loop forever once
  DFG-compiled at low thresholds).
- 0023 fixes the RegExp (YARR) JIT's ARMv7 entry code (webos-browser-lab 0010 + 0012, MIT): it overwrote the caller's
  r4 before saving it, and read the 5th (stacked) argument at fp + 0 instead of fp + 8. With these the RegExp JIT is on
  (0016's default); before, it was kept off because a framework docs site's web process exited ~2 s after load (6/6).
  Lab 0011 (cellLock re-entry on the compiler thread's bytecode fallback) is already in 2.54.3.
- 0025 carries the `webkitglib/2.54` queue after 2.54.3 (`c01fbd906..9715f4680`, 21 commits, 58 tarball `Source/` files)
  until upstream cuts `wpewebkit-2.54.4.tar.xz`. (The five `main` fixes noted in `agy-2026-11-21/lane4/REPORT.md` —
  bugs 328009, 315153, 319380+327551, 329007, 328163 — were already cherry-picked upstream before 2.54.3 was tagged.)
- 0026 keeps at most `MSE_MAX_BACK_BUFFER_SECONDS` seconds (default 30, minimum 3, 0 = off) of played media behind the playhead for
  plain `MediaSource` on GStreamer: `evictCodedFrames()` (before every append) queues the first phase of the usual eviction
  (start of the buffer up to min(currentTime - keep, last sync sample before currentTime)). No updateend, byte limits unchanged.
  After a seek back out of the buffered media (playhead > 5 s behind the previous check and not inside the buffer) it also drops
  data more than the same number of seconds beyond the playhead, so the re-fetched segments do not come on top of it (4K: about
  +90 MB at once before). A seek inside the buffer removes nothing.
- 0027 kills the web process only when the footprint is above the kill threshold (after the release) in `MEMORY_KILL_CONSECUTIVE_POLLS` measurements in a row
  (default 3, 1 = old behaviour), or at once when it is 12.5% above (runaway). Spikes of 17-30 MB media appends fall back within ~10 s.
- 0028 repeats WebKit's memory relief only when the pressure policy gets stricter or after `MEMORY_RELIEF_REPEAT_S` seconds
  (default 30, 0 = every poll). Robin polls every 2 s with thresholds most pages exceed (182 / 286 MB), so the relief (JIT
  code, style data, decoded images, a GC) ran every 2 s on ordinary pages. The kill check still runs on every poll.
  The engine sets it from `WPE_BROWSER_DEBUG_MEM_RELIEF_S`.
