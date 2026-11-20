# The 64-bit engine

The package carries a second engine, WPE WebKit 2.54.3 built for aarch64 (`engine64/`, 319 MB installed). It is
optional and off by default.

## Why

The 32-bit engine runs JavaScript with the 32-bit JIT ([jit-armv7.md](jit-armv7.md)). The TV's kernel is 64-bit, so
a 64-bit engine can run the 64-bit JIT, which is a more mature code path. In a small benchmark set on the TV, the
aarch64 JIT finished 42.3 times faster than the 32-bit interpreter, with identical checksums. Read that number as
JIT against interpreter, not as 64-bit against 32-bit (see jit-armv7.md).

## How it runs

- It needs no 64-bit bridge from the Homebrew Channel. The kernel runs any aarch64 ELF with its own loader and
  libraries. The package carries a glibc 2.44 loader, Mesa (llvmpipe) and the libraries under `engine64/lib/`.
- The launcher `engine/wpe-engine` chooses at each start:
  1. launch parameter `{"wpebEnv":{"WPE_BROWSER_DEBUG_ENGINE":"64"}}` (or `"32"`);
  2. a file named `engine64` containing `on` in the app's data folder (persistent, delete it to go back);
  3. otherwise the 32-bit engine.
- If `engine64/` or its loader is missing, the launcher logs `wpe-launcher: ...` in `engine.log` and starts the
  32-bit engine.
- The launcher creates `/tmp/<app id>/ld-linux-aarch64.so.1` itself. It is the interpreter of the 64-bit helpers and
  does not survive a reboot.
- It has its own cache folders (`cache/aarch64`).
- The ad blocker works in it: an aarch64 build of the extension sits in `engine64/lib/glibc/lib/web-extensions/`,
  and `engine64/lib/glibc/ubo` links to the shared data.

## Limits

- **Software rendering** (Mesa llvmpipe) and shared-memory frames only. No direct GPU presentation.
- **No hardware video.**
- Cold loads are slower than on the 32-bit engine in the same mode (Wikipedia 8.2 s against 4.7 s in shared-memory mode).
- Size: 319 MB, of which the WebKit library is 122 MB, Mesa 86 MB (libLLVM 67 MB) and ICU data 33 MB.
- Tested on one TV only, with page-load and ad-blocker checks.

## Building it

[building.md](building.md), step 3. The media protocol header has `static_assert`s in `common/wpe_media_ipc.h` that
pin the wire layout so that the 32-bit UI and the 64-bit engine agree (the same on armv7 softfp, aarch64 and x86_64).
