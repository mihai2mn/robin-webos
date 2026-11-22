# wpe-launcher: the 32-bit engine selector

`wpe-launcher.c`, built by [`tools/build-launcher.sh`](../../tools/build-launcher.sh), is installed as
`<app>/engine/wpe-engine` — in front of the whole engine. It exists so the *official* UI binary (which we do not build
and cannot modify) can start either the real 32-bit engine or the optional aarch64 engine, without knowing about it.

```
engine/wpe-engine        <- this launcher (32-bit ARM, static)
engine/wpe-engine32      <- the real 32-bit engine, renamed
engine64/                <- the aarch64 engine tree (optional; may be absent)
```

It must be **invisible**: the UI `posix_spawn`s it with the engine end of a `SOCK_SEQPACKET` socketpair dup2'd to
**fd 3** and its log on 1/2 (`browser/engine_client.cpp:214-231`), and then treats that pid as the engine — `poll()`
reaps it with `waitpid(WNOHANG)` for crash detection and `stop()` gives it 1 s before `SIGKILL`. So the launcher only
ever `exec`s (never forks or waits), keeps every inherited fd (above all fd 3), passes the environment through, and
forwards the UI's argv unchanged.

## Choosing, in order

1. `$WPE_BROWSER_DEBUG_ENGINE=64` / `=32` — reachable from a launch parameter, because Wren's `wpebEnv` passes
   `WPE_BROWSER_DEBUG*` names through (`browser/engine_client.cpp:202`):
   ```bash
   ares-launch -d <device> org.sostk.wpebrowser -p '{"wpebEnv":{"WPE_BROWSER_DEBUG_ENGINE":"64"}}'
   ```
2. `<WPE_BROWSER_DATA_DIR>/engine64` containing `on` — the persistent switch.
3. Otherwise the 32-bit engine (the default).

## The two execs

```c
/* 32-bit */
execv("<app>/engine/wpe-engine32", argv);                                  /* env untouched */

/* 64-bit */
execve("<app>/engine64/lib/glibc/ld-linux-aarch64.so.1",
       { loader, "--library-path",
         "<app>/engine64/lib:<app>/engine64/lib/glibc:<app>/engine64/lib/mesa",
         "<app>/engine64/wpe-engine", NULL },
       env + WPE_BROWSER_PRESENT=shm + WPE_BROWSER_RENDERER=software);
```

A 64-bit engine is SHM-only (direct presentation needs the TV's 32-bit Mali EGL and `libwpeb-owner.so`), hence the two
overrides. Everything else in the environment is the UI's, unchanged.

**Fallback:** every 64-bit problem is detected with `stat()`/`access()` *before* exec — missing or non-executable
engine, missing loader, out of memory — logged as a `wpe-launcher:` line and followed by the 32-bit engine in the same
process, so the UI never gets a child that dies instantly. A missing `engine64/lib/mesa/libEGL.so.1` is only a
warning (the web process needs it to read frames back, but the engine itself starts).

**The app dir** comes from `/proc/self/exe` (strip `wpe-engine` and `engine`), never from `argv[0]` or the cwd; with no
`/proc`, `realpath(argv[0])` and then `$WPE_BROWSER_LAUNCHER_DIR` are tried, and failing that it exits 127 with a log
line.

Static, plain C, no dependencies beyond libc: it is the first thing the UI execs, before the private-loader link
exists, so it has to work on the TV's own glibc (and on webOS 4's).

Status: **all six acceptance tests PASS on the TV**, plus 9/9 PC tests. Design and findings: see [engine64.md](../../docs/engine64.md) and [architecture.md](../../docs/architecture.md).
