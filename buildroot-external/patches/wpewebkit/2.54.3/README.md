# WebKit patches for WPE WebKit 2.54.3

Buildroot applies this folder instead of `../` when building 2.54.3 (`WEBKIT_VERSION=2.54.3 tools/build-engine.sh`).
Rebased from the 2.50.5 set and regenerated exactly against 2.54.3 (no fuzz). Dropped:

- 0003 (grayscale font antialiasing): 2.54 has WPE_SETTINGS_SUBPIXEL_LAYOUT_NONE as the default and maps unknown
  values to no subpixel order (grayscale).
- 0007 (GStreamer ring buffer size): fixed upstream.

0004 also re-enables the baseline JIT and the DFG on ARMv7 Linux (hardfp and softfp): 2.54 turns both off on every
32-bit build, which would cost the TV the JIT tiers 2.50.5 has. Upstream no longer tests 32-bit JIT.
