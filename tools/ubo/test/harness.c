/* SPDX-License-Identifier: GPL-3.0-or-later
 * Desktop test for the uBO blocker (tools/ubo/test/run.sh): a headless WPE WebKit view with libwren-ubo.so loaded
 * the way the engine loads it, which opens each page and prints the result of a JavaScript check on it.
 *
 *   harness UBO_DATA_DIR EXTENSION_DIR WAIT_MS URL CHECK_JS [URL CHECK_JS ...]
 *
 * CHECK_JS is an expression evaluated in the page once it has loaded and WAIT_MS more have passed; its value is
 * printed as JSON. TEST_ADS_OFF=1 starts with the blocker off; TEST_ALLOWED="host,host" allows ads on those hosts. */
#include <wpe/webkit.h>
#include <wpe/headless/wpe-headless.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static GMainLoop *loop;
static const char *ubo_dir;
static int wait_ms, failures;

static void on_init(WebKitWebContext *context, gpointer data)
{
    GVariantBuilder hosts;
    g_variant_builder_init(&hosts, G_VARIANT_TYPE("as"));
    char **allowed = g_strsplit(g_getenv("TEST_ALLOWED") ? g_getenv("TEST_ALLOWED") : "", ",", -1);
    for (char **a = allowed; *a; ++a)
        if (**a)
            g_variant_builder_add(&hosts, "s", *a);
    g_strfreev(allowed);
    const gboolean on = g_strcmp0(g_getenv("TEST_ADS_OFF"), "1") != 0;
    webkit_web_context_set_web_process_extensions_initialization_user_data(context,
        g_variant_new("(sbas)", ubo_dir, on, &hosts));
}

static gboolean run_check(gpointer data);

static void on_checked(GObject *view, GAsyncResult *result, gpointer data)
{
    static int retries;
    GError *error = NULL;
    JSCValue *v = webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW(view), result, &error);
    /* A page that reloads itself (bot challenges) can be between documents: try again a few times. */
    if (!v && retries < 5) {
        retries++;
        g_clear_error(&error);
        g_timeout_add(3000, run_check, data);
        return;
    }
    retries = 0;
    if (v) {
        char *json = jsc_value_to_json(v, 0);
        printf("RESULT %s\n", json ? json : "undefined");
        g_free(json);
        g_object_unref(v);
    } else {
        printf("RESULT error: %s\n", error->message);
        failures++;
        g_clear_error(&error);
    }
    fflush(stdout);
    g_main_loop_quit(loop);
}

typedef struct { WebKitWebView *view; const char *check; } Pending;

static gboolean run_check(gpointer data)
{
    Pending *p = data;
    /* TEST_WORLD=name: run the check in an isolated world (pages that replace built-ins can't break it). */
    webkit_web_view_evaluate_javascript(p->view, p->check, -1, g_getenv("TEST_WORLD"), NULL, NULL, on_checked, p);
    return G_SOURCE_REMOVE;
}

static void on_load_changed(WebKitWebView *view, WebKitLoadEvent event, gpointer data)
{
    if (event == WEBKIT_LOAD_FINISHED)
        g_timeout_add(wait_ms, run_check, data);
}

static gboolean on_load_failed(WebKitWebView *view, WebKitLoadEvent event, const char *uri, GError *error, gpointer data)
{
    printf("LOAD FAILED %s: %s\n", uri, error->message);
    return FALSE;
}

int main(int argc, char **argv)
{
    if (argc < 6 || (argc - 4) % 2) {
        fprintf(stderr, "usage: harness UBO_DATA_DIR EXTENSION_DIR WAIT_MS URL CHECK_JS [URL CHECK_JS ...]\n");
        return 2;
    }
    ubo_dir = argv[1];
    wait_ms = atoi(argv[3]);
    loop = g_main_loop_new(NULL, FALSE);
    WebKitWebContext *context = webkit_web_context_get_default();
    webkit_web_context_set_web_process_extensions_directory(context, argv[2]);
    /* Desktop WPE sandboxes the web process (bubblewrap): it sees only what is added. */
    webkit_web_context_add_path_to_sandbox(context, argv[1], TRUE);
    g_signal_connect(context, "initialize-web-process-extensions", G_CALLBACK(on_init), NULL);

    WPEDisplay *display = wpe_display_headless_new();
    WebKitNetworkSession *session = webkit_network_session_new_ephemeral();
    WebKitWebView *view = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "display", display,
        "network-session", session, NULL));
    Pending pending = { view, NULL };
    g_signal_connect(view, "load-changed", G_CALLBACK(on_load_changed), &pending);
    g_signal_connect(view, "load-failed", G_CALLBACK(on_load_failed), NULL);
    for (int i = 4; i < argc; i += 2) {
        pending.check = argv[i + 1];
        printf("PAGE %s\n", argv[i]);
        fflush(stdout);
        webkit_web_view_load_uri(view, argv[i]);
        g_main_loop_run(loop);
    }
    g_object_unref(view);
    g_object_unref(session);
    g_object_unref(display);
    return failures ? 1 : 0;
}
