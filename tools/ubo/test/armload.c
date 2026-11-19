/* SPDX-License-Identifier: GPL-3.0-or-later
 * ARM load test for libwren-ubo.so under QEMU (tools/ubo/test/qemu.sh): loads the package's WebKit the way the
 * web process has it, then the extension, then calls its entry point with real initialization data so it
 * deserializes engine.dat on ARM. There is no web process, so the extension's signal hookups fail harmlessly
 * after the data has loaded; the point is the log line "wren-ubo: loaded in N ms" or its warnings.
 *   armload ENGINE_DIR */
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

typedef void *(*variant_new_fn)(const char *fmt, ...);
typedef void (*init_fn)(void *extension, void *user_data);

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: armload ENGINE_DIR\n");
        return 2;
    }
    char path[1024];
    snprintf(path, sizeof path, "%s/lib/libWPEWebKit-2.0.so.1", argv[1]);
    if (!dlopen(path, RTLD_NOW | RTLD_GLOBAL)) { fprintf(stderr, "webkit: %s\n", dlerror()); return 1; }
    snprintf(path, sizeof path, "%s/lib/libsoup-3.0.so.0", argv[1]);
    if (!dlopen(path, RTLD_NOW | RTLD_GLOBAL)) { fprintf(stderr, "soup: %s\n", dlerror()); return 1; }
    snprintf(path, sizeof path, "%s/lib/web-extensions/libwren-ubo.so", argv[1]);
    void *ext = dlopen(path, RTLD_NOW);
    if (!ext) { fprintf(stderr, "extension: %s\n", dlerror()); return 1; }
    printf("extension loaded with every symbol resolved\n");
    init_fn init = (init_fn)dlsym(ext, "webkit_web_process_extension_initialize_with_user_data");
    variant_new_fn variant_new = (variant_new_fn)dlsym(RTLD_DEFAULT, "g_variant_new");
    if (!init || !variant_new) { fprintf(stderr, "entry point missing\n"); return 1; }
    snprintf(path, sizeof path, "%s/ubo", argv[1]);
    const char *none[] = { NULL };
    (void)none;
    void *data = variant_new("(sb^as)", path, 1, none);
    fflush(stdout);
    init(NULL, data);
    return 0;
}
