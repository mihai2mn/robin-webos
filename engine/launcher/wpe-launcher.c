/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * wpe-launcher: the 32-bit selector that sits at <app>/engine/wpe-engine.
 *
 * The installed UI (browser/engine_client.cpp) spawns <app>/engine/wpe-engine with posix_spawn, dup2()s the engine end
 * of a SOCK_SEQPACKET socketpair onto fd 3 and the log onto 1/2, and then expects THAT PROCESS to be the engine: poll()
 * uses waitpid(pid, WNOHANG) for crash detection and stop() gives it 1 s before SIGKILL. So this program must be
 * invisible: same argv, same environment, same fds, and the same pid once it execs. It never forks and never waits;
 * every path ends in execve()/execv() or in an error exit *before* anything has been started.
 *
 * What it chooses between:
 *   32-bit  <app>/engine/wpe-engine32     (the real 32-bit engine, renamed when the ipk is repacked)
 *   64-bit  <app>/engine64/lib/glibc/ld-linux-aarch64.so.1
 *             --library-path <app>/engine64/lib:<app>/engine64/lib/glibc:<app>/engine64/lib/mesa
 *             <app>/engine64/wpe-engine
 *
 * Selection, in order:
 *   1. $WPE_BROWSER_DEBUG_ENGINE=32|64  (Wren's launch parameter {"wpebEnv":{"WPE_BROWSER_DEBUG_ENGINE":"64"}} gets
 *      through the UI's extra_env filter, which accepts names starting with WPE_BROWSER_DEBUG: engine_client.cpp:202)
 *   2. a file named "engine64" in $WPE_BROWSER_DATA_DIR whose contents start with "on" (persistent user switch;
 *      absent/unreadable = 32)
 *   3. otherwise 32.
 *
 * The 64-bit path is taken only when every piece is present and executable (checked before exec, so the UI never ends
 * up with a child that dies instantly); any 64-bit failure falls back to the 32-bit engine with a "wpe-launcher:" line
 * on stderr (which the UI redirects into engine.log), because a working 32-bit engine is always better than a dead app.
 *
 * Why 64-bit needs no bridge: the kernel is arm64, so any ELF64/AArch64 program runs once its own aarch64 loader and
 * libraries are present. org.webosbrew.bridge-64to32 is a GL/Wayland shim, not an execution environment; the engine
 * ships its own Buildroot glibc 2.44 loader under engine64/lib/glibc and never loads the bridge's libs (its glibc 2.39
 * is older than our libraries need, and <bridge>/lib on the library path would shadow our bundled Mesa).
 *
 * Static, no dependencies beyond libc: it is installed into an app whose 32-bit userland predates most things.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#define TAG "wpe-launcher: "
#define PATHBUF 4096

static void note(const char *what, const char *tail)
{
    fprintf(stderr, TAG "%s%s%s\n", what, tail && *tail ? ": " : "", tail && *tail ? tail : "");
    fflush(stderr);
}

/* A regular file the current user can execute; follows symlinks (the tree may be linked into place). */
static int is_exec_file(const char *path)
{
    struct stat st;
    if (!path || !*path || access(path, X_OK) != 0 || stat(path, &st) != 0) return 0;
    return S_ISREG(st.st_mode);
}

static int join(char *out, size_t n, const char *a, const char *b)
{
    int r = snprintf(out, n, "%s%s", a, b);
    return (r > 0 && (size_t)r < n) ? 0 : -1;
}

/* <app> from /proc/self/exe (drop "wpe-engine" and "engine"). argv[0] is not trustworthy (may be bare, relative or a
 * symlink) and the cwd is the UI's; only if /proc is not mounted do we fall back to realpath(argv[0]) (when it has a
 * slash), then $WPE_BROWSER_LAUNCHER_DIR. */
static int app_dir(char *out, size_t n, int argc, char **argv)
{
    char exe[PATHBUF];
    ssize_t len = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (len > 0 && (size_t)len < sizeof exe) {
        exe[len] = '\0';
        for (int i = 0; i < 2; ++i) {
            char *slash = strrchr(exe, '/');
            if (!slash || slash == exe) { len = 0; break; }
            *slash = '\0';
        }
        if (len > 0 && exe[0] == '/') return join(out, n, exe, "");
    }
    note("warning: /proc/self/exe unavailable, using argv[0]/the environment for the app directory", "");
    if (argc > 0 && argv[0] && strchr(argv[0], '/')) {
        char *real = realpath(argv[0], NULL);
        char tmp[PATHBUF];
        int ok = real && strlen(real) < sizeof tmp;
        if (ok) {
            strcpy(tmp, real);
            for (int i = 0; i < 2; ++i) {
                char *slash = strrchr(tmp, '/');
                if (!slash || slash == tmp) { ok = 0; break; }
                *slash = '\0';
            }
        }
        free(real);
        if (ok && tmp[0] == '/') return join(out, n, tmp, "");
    }
    const char *hint = getenv("WPE_BROWSER_LAUNCHER_DIR");
    if (hint && hint[0] == '/') return join(out, n, hint, "");
    note("cannot find the app directory", "");
    return -1;
}

/* <WPE_BROWSER_DATA_DIR>/engine64 containing "on" (ASCII, first word, case-insensitive). */
static int switch_file_says_64(void)
{
    const char *dir = getenv("WPE_BROWSER_DATA_DIR");
    if (!dir || dir[0] != '/') return 0;
    char path[PATHBUF];
    if (join(path, sizeof path, dir, "/engine64") != 0) return 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    char buf[64];
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = '\0';
    char *p = buf;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
    return (p[0] == 'o' || p[0] == 'O') && (p[1] == 'n' || p[1] == 'N') &&
           (p[2] == '\0' || p[2] == '\n' || p[2] == '\r' || p[2] == ' ' || p[2] == '\t');
}

/* Keys the 64-bit tree must own. The UI computed these for the 32-bit engine tree (<app>/engine: its libexec, its
 * GStreamer plugins, its fontconfig...), and the web process the 64-bit engine launches must come from engine64 too:
 * with the 32-bit WEBKIT_EXEC_PATH the engine started the 32-bit WPEWebProcess, which talks a different (32-bit)
 * IPC layout and was rejected with "Received invalid message: AcceleratedBackingStore_DidChangeBufferConfiguration"
 * (TV, 2026-11-20, through the real UI). Values are built from <app>/engine64. */
static const char *const kOwned[] = {
    "WEBKIT_EXEC_PATH", "WEBKIT_INJECTED_BUNDLE_PATH", "LD_LIBRARY_PATH", "LD_PRELOAD", "GCONV_PATH", "GIO_MODULE_DIR",
    "GIO_EXTRA_MODULES", "GST_PLUGIN_PATH", "GST_PLUGIN_PATH_1_0", "GST_PLUGIN_SYSTEM_PATH", "GST_PLUGIN_SYSTEM_PATH_1_0",
    "GST_PLUGIN_SCANNER", "GST_PLUGIN_SCANNER_1_0", "FONTCONFIG_FILE", "XDG_DATA_DIRS", "PULSE_CLIENTCONFIG",
    "GALLIUM_DRIVER", "WEBKIT_SKIA_ENABLE_CPU_RENDERING", "WEBKIT_DISABLE_DEFAULT_EGL_DISPLAY",
    "WEBKIT_SKIA_CPU_PAINTING_THREADS", "WPE_BROWSER_PRESENT", "WPE_BROWSER_RENDERER", NULL
};

static char *kv(const char *k, const char *a, const char *b)
{
    size_t n = strlen(k) + strlen(a) + strlen(b) + 2;
    char *s = malloc(n);
    if (s) snprintf(s, n, "%s=%s%s", k, a, b);
    return s;
}

static char **engine64_environ(const char *base, const char *libs)
{
    extern char **environ;
    size_t count = 0;
    while (environ && environ[count]) ++count;
    char **env = calloc(count + 32, sizeof *env);
    if (!env) return NULL;
    size_t keep = 0;
    for (size_t i = 0; i < count; ++i) {
        const char *e = environ[i];
        const char *eq = strchr(e, '=');
        size_t klen = eq ? (size_t)(eq - e) : strlen(e);
        int owned = 0;
        for (size_t k = 0; kOwned[k]; ++k)
            if (strlen(kOwned[k]) == klen && strncmp(e, kOwned[k], klen) == 0) owned = 1;
        if (!owned) { env[keep++] = environ[i]; continue; }
        /* Say so when we overrule a real presentation choice (e.g. wpebPresent=direct with the 64-bit engine). */
        const char *value = eq ? eq + 1 : "";
        if (klen == 19 && strncmp(e, "WPE_BROWSER_PRESENT", 19) == 0 && strcmp(value, "shm") != 0)
            note("64-bit engine is SHM-only; overriding the UI's presentation", e);
        else if (klen == 20 && strncmp(e, "WPE_BROWSER_RENDERER", 20) == 0 && strcmp(value, "software") != 0)
            note("64-bit engine renders in software; overriding the UI's renderer", e);
    }
    /* WPE_BROWSER_PRESENT=shm / WPE_BROWSER_RENDERER=software: a 64-bit engine can only present through shared memory
     * (direct mode needs the TV's 32-bit Mali EGL and libwpeb-owner.so), see ui-launch/LAUNCHER.md "Follow-up 2". */
    env[keep++] = kv("WPE_BROWSER_PRESENT", "shm", "");
    env[keep++] = kv("WPE_BROWSER_RENDERER", "software", "");
    env[keep++] = kv("LD_LIBRARY_PATH", libs, "");
    env[keep++] = kv("LD_PRELOAD", "", "");
    env[keep++] = kv("WEBKIT_EXEC_PATH", base, "/libexec");
    env[keep++] = kv("WEBKIT_INJECTED_BUNDLE_PATH", base, "/lib/injected-bundle");
    env[keep++] = kv("GCONV_PATH", base, "/lib/glibc/gconv");
    env[keep++] = kv("GIO_MODULE_DIR", base, "/lib/gio/modules");
    env[keep++] = kv("GIO_EXTRA_MODULES", "", "");
    env[keep++] = kv("GST_PLUGIN_PATH", "", "");
    env[keep++] = kv("GST_PLUGIN_PATH_1_0", "", "");
    env[keep++] = kv("GST_PLUGIN_SYSTEM_PATH", base, "/lib/gstreamer-1.0");
    env[keep++] = kv("GST_PLUGIN_SYSTEM_PATH_1_0", base, "/lib/gstreamer-1.0");
    env[keep++] = kv("GST_PLUGIN_SCANNER", base, "/libexec/gst-plugin-scanner");
    env[keep++] = kv("GST_PLUGIN_SCANNER_1_0", base, "/libexec/gst-plugin-scanner");
    env[keep++] = kv("FONTCONFIG_FILE", base, "/etc/fonts/fonts.conf");
    env[keep++] = kv("XDG_DATA_DIRS", base, "/share");
    env[keep++] = kv("PULSE_CLIENTCONFIG", base, "/etc/pulse/client.conf");
    env[keep++] = kv("GALLIUM_DRIVER", "llvmpipe", "");
    env[keep++] = kv("WEBKIT_SKIA_ENABLE_CPU_RENDERING", "1", "");
    env[keep++] = kv("WEBKIT_SKIA_CPU_PAINTING_THREADS", "4", "");
    env[keep] = NULL;
    return env;
}

/* Never returns on success. Returns only when the 64-bit engine cannot be started, after logging why. */
static void exec_engine64(const char *app, int argc, char **argv)
{
    char base[PATHBUF], loader[PATHBUF], program[PATHBUF], libs[PATHBUF], mesa[PATHBUF];

    if (join(base, sizeof base, app, "/engine64") != 0) { note("app path too long", ""); return; }
    if (join(loader, sizeof loader, base, "/lib/glibc/ld-linux-aarch64.so.1") != 0 ||
        join(program, sizeof program, base, "/wpe-engine") != 0 ||
        (size_t)snprintf(libs, sizeof libs, "%s/lib:%s/lib/glibc:%s/lib/mesa", base, base, base) >= sizeof libs) {
        note("engine64 path too long", "");
        return;
    }

    /* Pre-exec checks: a missing or non-executable piece is a fallback, not a dead child. */
    if (!is_exec_file(program)) { note("64-bit engine absent, using the 32-bit engine", program); return; }
    if (!is_exec_file(loader)) { note("64-bit loader absent, using the 32-bit engine", loader); return; }
    if (join(mesa, sizeof mesa, base, "/lib/mesa/libEGL.so.1") == 0 && access(mesa, R_OK) != 0)
        note("warning: no bundled Mesa, the 64-bit engine cannot read frames back", mesa);

    char **env = engine64_environ(base, libs);
    if (!env) { note("out of memory, using 32-bit", ""); return; }

    /* loader --library-path <libs> <program> [the UI's arguments, if it ever passes any] */
    char **av = calloc((size_t)(argc > 1 ? argc - 1 : 0) + 5, sizeof *av);
    if (!av) { note("out of memory, using 32-bit", ""); return; }
    av[0] = loader; av[1] = "--library-path"; av[2] = libs; av[3] = program;
    for (int i = 1; i < argc; ++i) av[3 + i] = argv[i];
    note("exec 64-bit engine", program);
    fflush(stdout);
    execve(loader, av, env);
    note("64-bit exec failed, using the 32-bit engine: ", strerror(errno));
}

/* Never returns on success. Try <app>/engine/wpe-engine32, then a legacy 32-bit engine if one is packaged. */
static void exec_engine32(const char *app, int argc, char **argv)
{
    char engine32[PATHBUF], legacy[PATHBUF];
    if (join(engine32, sizeof engine32, app, "/engine/wpe-engine32") != 0) { note("app path too long", ""); return; }
    if (join(legacy, sizeof legacy, app, "/engine/legacy-wpe-engine") != 0) { note("app path too long", ""); return; }

    const char *program;
    if (is_exec_file(engine32)) {
        program = engine32;
    } else if (is_exec_file(legacy)) {
        program = legacy;
        note("wpe-engine32 missing, using the legacy 32-bit engine", legacy);
    } else {
        note("no 32-bit engine found", engine32);
        note("  also tried", legacy);
        return;
    }

    /* argv[0] = the real program's path; the UI passes no other arguments today, but forward any it might. */
    char **av = calloc((size_t)(argc > 0 ? argc : 1) + 1, sizeof *av);
    if (!av) { note("out of memory, cannot exec the 32-bit engine", ""); return; }
    av[0] = (char *)program;
    for (int i = 1; i < argc; ++i) av[i] = argv[i];
    note("exec 32-bit engine", program);
    fflush(stdout);
    execv(program, av);
    note("32-bit exec failed: ", strerror(errno));
}

int main(int argc, char **argv)
{
    char app[PATHBUF];
    if (app_dir(app, sizeof app, argc, argv) != 0) return 127;

    const char *want = getenv("WPE_BROWSER_DEBUG_ENGINE");
    int use64;
    if (want && (strcmp(want, "64") == 0 || strcmp(want, "64bit") == 0)) {
        use64 = 1;
    } else if (want && (strcmp(want, "32") == 0 || strcmp(want, "32bit") == 0)) {
        use64 = 0;
    } else {
        use64 = switch_file_says_64();
    }

    if (use64) {
        exec_engine64(app, argc, argv);                       /* falls through only after logging why */
        note("falling back to the 32-bit engine", "");
    }
    exec_engine32(app, argc, argv);
    note("cannot start any engine; the UI will report that the engine is gone", "");
    return 127;
}
