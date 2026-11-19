/* SPDX-License-Identifier: GPL-3.0-or-later
 * libwren-ubo.so: Wren's ad blocker inside the web process (a WebKit web process extension), with uBlock Origin's
 * lists and engine semantics through adblock-rust (engine/ubo/src/lib.rs). Loaded by WebKit from the directory the
 * engine sets (engine/src/main.cpp, WPEB_UBO_DIR); replaces the compiled content-blocker list (tools/adblock).
 *
 *   network    WebKitWebPage::send-request sees every subresource request before it leaves the web process:
 *              blocked ones are cancelled, uBO `redirect=` ones load uBO's neutered resource (a data: URL) instead,
 *              `removeparam` ones load the cleaned URL.
 *   scriptlets `+js(...)` filters run in each frame's main world when its window object is created, before any
 *              of the page's scripts (WebKitScriptWorld::window-object-cleared).
 *   cosmetic   cosmetic.js runs in an isolated world: the site's hiding rules, generic hiding for the classes and
 *              ids the document uses (asked from here, __wrenUboHidden), and procedural filters.
 *
 * Initialization data (from the engine): "(sbas)" = data directory, on, hosts where ads are allowed.
 * User message "wren-ubo:state" "(bas)": the same on/allowed, sent when the UI changes them.
 * The blocker is never applied on YouTube (see adblock_wanted in engine/src/main.cpp) or on allowed hosts.
 * WREN_UBO_LOG=1 logs every blocked, redirected or rewritten request (without query strings). */
#include <wpe/webkit-web-process-extension.h>
#include <jsc/jsc.h>
#include <libsoup/soup.h>

#include <stdint.h>
#include <string.h>

/* Unused callback parameters keep a name (C17: unnamed parameters are C23). */
#define unused G_GNUC_UNUSED

/* engine/ubo/src/lib.rs */
typedef struct WrenUbo WrenUbo;
typedef struct { int32_t action; char *text; } WrenUboVerdict;
typedef struct { char *css, *script, *procedural, *exceptions; int32_t generichide; } WrenUboCosmetic;
enum { WREN_UBO_ALLOW = 0, WREN_UBO_BLOCK = 1, WREN_UBO_REDIRECT = 2, WREN_UBO_REWRITE = 3 };
WrenUbo *wren_ubo_new(const uint8_t *dat, size_t dat_len, const char *resources_json);
void wren_ubo_free(WrenUbo *ubo);
void wren_ubo_string_free(char *s);
WrenUboVerdict wren_ubo_check(const WrenUbo *ubo, const char *url, const char *source_url, const char *request_type);
WrenUboCosmetic wren_ubo_cosmetic(const WrenUbo *ubo, const char *url);
void wren_ubo_cosmetic_free(WrenUboCosmetic *c);
char *wren_ubo_hidden(const WrenUbo *ubo, const char *classes, const char *ids, const char *exceptions_json);

static struct {
    WrenUbo *ubo;
    char *cosmetic_js;          /* cosmetic.js, run in the isolated world */
    WebKitScriptWorld *world;   /* the isolated world */
    gboolean on;
    char **allowed;             /* hosts (and their subdomains) where the UI allows ads */
    gboolean log, log_all;      /* WREN_UBO_LOG=1: what was changed; =2: every request too */
    guint64 blocked, redirected, rewritten;
} g = { .on = TRUE };

static gboolean host_is(const char *host, const char *domain)
{
    const size_t h = strlen(host), d = strlen(domain);
    if (h == d)
        return g_ascii_strcasecmp(host, domain) == 0;
    return h > d && host[h - d - 1] == '.' && g_ascii_strcasecmp(host + h - d, domain) == 0;
}

/* Whether the blocker applies to a page at `uri` (its top-level document); the same rule as the engine's. */
static gboolean wanted(const char *uri)
{
    if (!g.ubo || !g.on)
        return FALSE;
    if (!uri || !*uri)
        return TRUE;
    GUri *u = g_uri_parse(uri, G_URI_FLAGS_NONE, NULL);
    const char *host = u ? g_uri_get_host(u) : NULL;
    gboolean want = TRUE;
    if (host) {
        /* YouTube: the lists cannot stop its video ads, and a blocked ad request has stopped its player before. */
        if (host_is(host, "youtube.com") || host_is(host, "youtube-nocookie.com") || host_is(host, "youtu.be"))
            want = FALSE;
        for (char **a = g.allowed; want && a && *a; ++a)
            if (host_is(host, *a))
                want = FALSE;
    }
    if (u)
        g_uri_unref(u);
    return want;
}

static gboolean has_suffix_ci(const char *path, const char *const *exts)
{
    const size_t n = strlen(path);
    for (; *exts; ++exts) {
        const size_t e = strlen(*exts);
        if (n > e && g_ascii_strcasecmp(path + n - e, *exts) == 0)
            return TRUE;
    }
    return FALSE;
}

/* uBO's request type. WebKit's send-request does not say what a request is for, so this reads the request's
 * Sec-Fetch-Dest header when WebKit has set it, then its Accept header, then the file extension.
 * NULL: a document load, which is never blocked (navigations, and frames we cannot tell from them). */
static const char *request_type(WebKitURIRequest *request, const char *uri)
{
    SoupMessageHeaders *headers = webkit_uri_request_get_http_headers(request);
    const char *dest = headers ? soup_message_headers_get_one(headers, "Sec-Fetch-Dest") : NULL;
    if (dest) {
        static const struct { const char *dest, *type; } map[] = {
            { "document", NULL }, { "iframe", "sub_frame" }, { "frame", "sub_frame" }, { "script", "script" },
            { "worker", "script" }, { "sharedworker", "script" }, { "serviceworker", "script" },
            { "audioworklet", "script" }, { "paintworklet", "script" }, { "style", "stylesheet" },
            { "image", "image" }, { "font", "font" }, { "audio", "media" }, { "video", "media" },
            { "track", "media" }, { "object", "object" }, { "embed", "object" }, { "empty", "xhr" },
            { "report", "csp_report" }, { "manifest", "other" },
        };
        for (size_t i = 0; i < G_N_ELEMENTS(map); ++i)
            if (g_ascii_strcasecmp(dest, map[i].dest) == 0)
                return map[i].type;
        return "other";
    }
    const char *accept = headers ? soup_message_headers_get_one(headers, "Accept") : NULL;
    if (accept) {
        if (g_str_has_prefix(accept, "text/html"))
            return NULL;
        if (g_str_has_prefix(accept, "text/css"))
            return "stylesheet";
        if (g_str_has_prefix(accept, "image/"))
            return "image";
    }
    char path[512];
    const char *end = strpbrk(uri, "?#");
    const size_t len = MIN(end ? (size_t)(end - uri) : strlen(uri), sizeof(path) - 1);
    memcpy(path, uri, len);
    path[len] = 0;
    static const char *const js[] = { ".js", ".mjs", NULL };
    static const char *const css[] = { ".css", NULL };
    static const char *const img[] = { ".png", ".jpg", ".jpeg", ".gif", ".webp", ".svg", ".ico", ".avif", NULL };
    static const char *const font[] = { ".woff", ".woff2", ".ttf", ".otf", NULL };
    static const char *const media[] = { ".mp4", ".webm", ".m3u8", ".mpd", ".mp3", ".m4s", ".ts", ".aac", NULL };
    if (has_suffix_ci(path, js))
        return "script";
    if (has_suffix_ci(path, css))
        return "stylesheet";
    if (has_suffix_ci(path, img))
        return "image";
    if (has_suffix_ci(path, font))
        return "font";
    if (has_suffix_ci(path, media))
        return "media";
    /* fetch() and XMLHttpRequest send Accept: * / * (or their own); treat the rest as such, as uBO's "other" would
     * miss every $xhr rule. */
    return "xhr";
}

static void log_request(const char *what, const char *uri, const char *type)
{
    if (!g.log)
        return;
    const char *cut = strpbrk(uri, "?#");
    g_message("wren-ubo: %s %s %.*s", what, type, cut ? (int)(cut - uri) : (int)strlen(uri), uri);
}

static gboolean on_send_request(WebKitWebPage *page, WebKitURIRequest *request, WebKitURIResponse *redirected, gpointer data unused)
{
    const char *page_uri = webkit_web_page_get_uri(page);
    const char *uri = webkit_uri_request_get_uri(request);
    if (!uri || !(g_str_has_prefix(uri, "http:") || g_str_has_prefix(uri, "https:")) || !wanted(page_uri))
        return FALSE;
    const char *type = request_type(request, uri);
    if (g.log_all) {
        SoupMessageHeaders *h = webkit_uri_request_get_http_headers(request);
        g_message("wren-ubo: request %s (dest %s, accept %s) page %s: %s", type ? type : "document",
            h ? soup_message_headers_get_one(h, "Sec-Fetch-Dest") : "-", h ? soup_message_headers_get_one(h, "Accept") : "-",
            page_uri ? page_uri : "-", uri);
    }
    if (!type)
        return FALSE;
    WrenUboVerdict v = wren_ubo_check(g.ubo, uri, page_uri ? page_uri : uri, type);
    gboolean stop = FALSE;
    switch (v.action) {
    case WREN_UBO_BLOCK:
        g.blocked++;
        log_request("blocked", uri, type);
        stop = TRUE; /* WebKit cancels the request */
        break;
    case WREN_UBO_REDIRECT:
        g.redirected++;
        log_request("redirected", uri, type);
        webkit_uri_request_set_uri(request, v.text);
        break;
    case WREN_UBO_REWRITE:
        g.rewritten++;
        log_request("rewritten", uri, type);
        webkit_uri_request_set_uri(request, v.text);
        break;
    default:
        break;
    }
    wren_ubo_string_free(v.text);
    return stop;
}

/* __wrenUboHidden(classes, ids, exceptions): generic hiding rules for classes and ids (newline-separated). */
static char *hidden_cb(const char *classes, const char *ids, const char *exceptions, gpointer data unused)
{
    char *css = wren_ubo_hidden(g.ubo, classes, ids, exceptions);
    char *copy = g_strdup(css ? css : "");
    wren_ubo_string_free(css);
    return copy; /* JSC frees it */
}

static void evaluate(JSCContext *ctx, const char *code, const char *what, const char *uri)
{
    JSCValue *r = jsc_context_evaluate_with_source_uri(ctx, code, -1, what, 1);
    JSCException *e = jsc_context_get_exception(ctx);
    if (e) {
        g_warning("wren-ubo: %s on %s: %s", what, uri, jsc_exception_get_message(e));
        jsc_context_clear_exception(ctx);
    }
    if (r)
        g_object_unref(r);
}

static void on_window_object_cleared(WebKitScriptWorld *world, WebKitWebPage *page, WebKitFrame *frame, gpointer data unused)
{
    const char *uri = webkit_frame_get_uri(frame);
    if (!uri || !(g_str_has_prefix(uri, "http:") || g_str_has_prefix(uri, "https:")))
        return;
    const char *top = webkit_frame_is_main_frame(frame) ? uri : webkit_web_page_get_uri(page);
    if (!wanted(top))
        return;
    WrenUboCosmetic c = wren_ubo_cosmetic(g.ubo, uri);

    /* Scriptlets: the page's own world, now, before its scripts. */
    if (c.script && *c.script) {
        JSCContext *main = webkit_frame_get_js_context_for_script_world(frame, world);
        char *code = g_strconcat("(function wrenUboScriptlets() {\nconst scriptletGlobals = {};\n", c.script,
            "\n})();", NULL);
        evaluate(main, code, "wren-ubo-scriptlets", uri);
        g_free(code);
        g_object_unref(main);
    }

    /* Cosmetic filtering: our isolated world, so the page cannot see or tamper with our functions. */
    if (g.cosmetic_js) {
        JSCContext *iso = webkit_frame_get_js_context_for_script_world(frame, g.world);
        JSCValue *fn = jsc_value_new_function(iso, "__wrenUboHidden", G_CALLBACK(hidden_cb), NULL, NULL,
            G_TYPE_STRING, 3, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
        jsc_context_set_value(iso, "__wrenUboHidden", fn);
        g_object_unref(fn);
        JSCValue *data = jsc_value_new_object(iso, NULL, NULL);
        JSCValue *v;
        jsc_value_object_set_property(data, "css", v = jsc_value_new_string(iso, c.css ? c.css : ""));
        g_object_unref(v);
        jsc_value_object_set_property(data, "procedural", v = jsc_value_new_string(iso, c.procedural ? c.procedural : "[]"));
        g_object_unref(v);
        jsc_value_object_set_property(data, "exceptions", v = jsc_value_new_string(iso, c.exceptions ? c.exceptions : "[]"));
        g_object_unref(v);
        jsc_value_object_set_property(data, "generichide", v = jsc_value_new_boolean(iso, c.generichide != 0));
        g_object_unref(v);
        jsc_context_set_value(iso, "__wrenUbo", data);
        g_object_unref(data);
        evaluate(iso, g.cosmetic_js, "wren-ubo-cosmetic.js", uri);
        g_object_unref(iso);
    }
    wren_ubo_cosmetic_free(&c);
}

static void set_state(GVariant *on_allowed)
{
    gboolean on = TRUE;
    GVariantIter *iter = NULL;
    g_variant_get(on_allowed, "(bas)", &on, &iter);
    GPtrArray *hosts = g_ptr_array_new();
    const char *h;
    while (iter && g_variant_iter_next(iter, "&s", &h))
        if (*h)
            g_ptr_array_add(hosts, g_ascii_strdown(h, -1));
    g_ptr_array_add(hosts, NULL);
    if (iter)
        g_variant_iter_free(iter);
    g_strfreev(g.allowed);
    g.allowed = (char **)g_ptr_array_free(hosts, FALSE);
    g.on = on;
}

static gboolean on_user_message(WebKitWebProcessExtension *extension, WebKitUserMessage *message, gpointer data unused)
{
    if (g_strcmp0(webkit_user_message_get_name(message), "wren-ubo:state") != 0)
        return FALSE;
    GVariant *p = webkit_user_message_get_parameters(message);
    if (p && g_variant_is_of_type(p, G_VARIANT_TYPE("(bas)")))
        set_state(p);
    g_message("wren-ubo: %s, %u site(s) allowed (so far: %" G_GUINT64_FORMAT " blocked, %" G_GUINT64_FORMAT
        " redirected, %" G_GUINT64_FORMAT " rewritten)", g.on ? "on" : "off", g.allowed ? g_strv_length(g.allowed) : 0,
        g.blocked, g.redirected, g.rewritten);
    return TRUE;
}

static void on_page_created(WebKitWebProcessExtension *extension, WebKitWebPage *page, gpointer data unused)
{
    g_signal_connect(page, "send-request", G_CALLBACK(on_send_request), NULL);
}

G_MODULE_EXPORT void webkit_web_process_extension_initialize_with_user_data(WebKitWebProcessExtension *extension,
    GVariant *user_data)
{
    const char *dir = NULL;
    GVariantIter *ignored = NULL;
    gboolean on = TRUE;
    if (!user_data || !g_variant_is_of_type(user_data, G_VARIANT_TYPE("(sbas)"))) {
        g_warning("wren-ubo: no initialization data, not loaded");
        return;
    }
    g_variant_get(user_data, "(&sbas)", &dir, &on, &ignored);
    if (ignored)
        g_variant_iter_free(ignored);
    GVariant *allowed = g_variant_get_child_value(user_data, 2);
    GVariant *state = g_variant_ref_sink(g_variant_new("(b@as)", on, allowed));
    set_state(state);
    g_variant_unref(state);
    g_variant_unref(allowed);
    g.log_all = g_strcmp0(g_getenv("WREN_UBO_LOG"), "2") == 0;
    g.log = g.log_all || g_strcmp0(g_getenv("WREN_UBO_LOG"), "1") == 0;

    const gint64 started = g_get_monotonic_time();
    char *dat_path = g_build_filename(dir, "engine.dat", NULL);
    char *res_path = g_build_filename(dir, "resources.json", NULL);
    char *js_path = g_build_filename(dir, "cosmetic.js", NULL);
    char *dat = NULL, *res = NULL;
    gsize dat_len = 0;
    GError *error = NULL;
    if (!g_file_get_contents(dat_path, &dat, &dat_len, &error) || !g_file_get_contents(res_path, &res, NULL, &error)) {
        g_warning("wren-ubo: %s", error->message);
        g_clear_error(&error);
    } else {
        g.ubo = wren_ubo_new((const uint8_t *)dat, dat_len, res);
        if (!g.ubo)
            g_warning("wren-ubo: %s or %s is not usable", dat_path, res_path);
    }
    if (!g_file_get_contents(js_path, &g.cosmetic_js, NULL, &error)) {
        g_warning("wren-ubo: %s (no cosmetic filtering)", error->message);
        g_clear_error(&error);
    }
    g_free(dat);
    g_free(res);
    g_free(dat_path);
    g_free(res_path);
    g_free(js_path);
    if (!g.ubo)
        return;

    g.world = webkit_script_world_new_with_name("wren-ubo");
    g_signal_connect(webkit_script_world_get_default(), "window-object-cleared", G_CALLBACK(on_window_object_cleared), NULL);
    g_signal_connect(extension, "page-created", G_CALLBACK(on_page_created), NULL);
    g_signal_connect(extension, "user-message-received", G_CALLBACK(on_user_message), NULL);
    g_message("wren-ubo: loaded in %.0f ms (%s, %u site(s) allowed)", (g_get_monotonic_time() - started) / 1e3,
        g.on ? "on" : "off", g_strv_length(g.allowed));
}
