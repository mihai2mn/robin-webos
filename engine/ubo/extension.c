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
 * Initialization data (from the engine): "(sbasas)" = data directory, on, hosts where ads are allowed, extra
 * lists to use (lists/<name>.dat, see wren://adblock). User message "wren-ubo:state" "(basas)": the same
 * on/allowed/lists, sent when the settings change. ("(sbas)"/"(bas)" without lists are accepted too.)
 * The blocker is never applied on YouTube (see adblock_wanted in engine/src/main.cpp) or on allowed hosts.
 * WREN_UBO_LOG=1 logs every blocked, redirected or rewritten request (without query strings). */
#define _GNU_SOURCE /* strcasestr */
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
int32_t wren_ubo_add_list(WrenUbo *ubo, const char *name, const uint8_t *dat, size_t dat_len);
int32_t wren_ubo_remove_list(WrenUbo *ubo, const char *name);
int32_t wren_ubo_has_list(const WrenUbo *ubo, const char *name);

static struct {
    WrenUbo *ubo;
    char *dir;                  /* the data directory (engine.dat, lists/) */
    char **lists;               /* extra lists in use */
    char *cosmetic_js;          /* cosmetic.js, run in the isolated world */
    char *compat_js;            /* compat.js, run in every page's main world */
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

/* This WebKit has no AVIF decoder. Sites that ask their image server for AVIF (...jpg?format=avif,
 * imgix's fm=avif) then show nothing, so image requests lose those parameters and "image/avif" leaves the Accept
 * header: the server sends its default format. A compatibility fix, so it runs with the blocker off too. */
static void avoid_avif(WebKitURIRequest *request, const char *uri)
{
    SoupMessageHeaders *headers = webkit_uri_request_get_http_headers(request);
    const char *accept = headers ? soup_message_headers_get_one(headers, "Accept") : NULL;
    if (accept && strstr(accept, "image/avif")) {
        GString *a = g_string_new(accept);
        g_string_replace(a, "image/avif,", "", 0);
        g_string_replace(a, ",image/avif", "", 0);
        soup_message_headers_replace(headers, "Accept", a->str);
        g_string_free(a, TRUE);
    }
    const char *q = strchr(uri, '?');
    if (!q || !(strcasestr(q, "format=avif") || strcasestr(q, "fm=avif")))
        return;
    const char *hash = strchr(q, '#');
    char *query = g_strndup(q + 1, hash ? (gsize)(hash - q - 1) : strlen(q + 1));
    char **parts = g_strsplit(query, "&", -1);
    GString *out = g_string_new_len(uri, q - uri);
    gboolean first = TRUE;
    for (char **p = parts; *p; ++p) {
        if (!**p || !g_ascii_strcasecmp(*p, "format=avif") || !g_ascii_strcasecmp(*p, "fm=avif"))
            continue;
        g_string_append_c(out, first ? '?' : '&');
        g_string_append(out, *p);
        first = FALSE;
    }
    if (hash)
        g_string_append(out, hash);
    if (g.log)
        g_message("wren-ubo: avif -> default format: %.*s", (int)(q - uri), uri);
    webkit_uri_request_set_uri(request, out->str);
    g_string_free(out, TRUE);
    g_strfreev(parts);
    g_free(query);
}

static gboolean on_send_request(WebKitWebPage *page, WebKitURIRequest *request, WebKitURIResponse *redirected, gpointer data unused)
{
    const char *page_uri = webkit_web_page_get_uri(page);
    const char *uri = webkit_uri_request_get_uri(request);
    if (!uri || !(g_str_has_prefix(uri, "http:") || g_str_has_prefix(uri, "https:")))
        return FALSE;
    {
        SoupMessageHeaders *h = webkit_uri_request_get_http_headers(request);
        const char *dest = h ? soup_message_headers_get_one(h, "Sec-Fetch-Dest") : NULL;
        const char *accept = h ? soup_message_headers_get_one(h, "Accept") : NULL;
        if (g_strcmp0(dest, "image") == 0 || (accept && g_str_has_prefix(accept, "image/"))) {
            avoid_avif(request, uri);
            uri = webkit_uri_request_get_uri(request);
        }
    }
    if (!wanted(page_uri))
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
    if (g.log_all)
        g_message("wren-ubo: window object cleared, page %" G_GUINT64_FORMAT ", frame %s", webkit_web_page_get_id(page),
            uri ? uri : "(none)");
    if (!uri || !(g_str_has_prefix(uri, "http:") || g_str_has_prefix(uri, "https:")))
        return;
    /* Compatibility fixes (compat.js: a mouse-like pointer instead of a touchscreen): every page, ads blocked or not. */
    if (g.compat_js) {
        JSCContext *main = webkit_frame_get_js_context_for_script_world(frame, world);
        evaluate(main, g.compat_js, "wren-compat.js", uri);
        g_object_unref(main);
    }
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

/* An "as" variant -> a NULL-terminated array of lower-case strings (empty ones dropped). */
static char **strv_of(GVariant *as)
{
    GPtrArray *out = g_ptr_array_new();
    GVariantIter iter;
    const char *s;
    if (as) {
        g_variant_iter_init(&iter, as);
        while (g_variant_iter_next(&iter, "&s", &s))
            if (*s)
                g_ptr_array_add(out, g_ascii_strdown(s, -1));
    }
    g_ptr_array_add(out, NULL);
    return (char **)g_ptr_array_free(out, FALSE);
}

/* Loads the extra lists now wanted and frees the ones no longer wanted. A list name is a file name in lists/. */
static void apply_lists(void)
{
    static char **loaded;
    for (char **l = loaded; l && *l; ++l)
        if (!g.lists || !g_strv_contains((const char *const *)g.lists, *l))
            wren_ubo_remove_list(g.ubo, *l);
    GPtrArray *now = g_ptr_array_new();
    for (char **l = g.lists; l && *l; ++l) {
        if (strchr(*l, '/') || strchr(*l, '.'))
            continue;
        if (!wren_ubo_has_list(g.ubo, *l)) {
            char *file = g_strconcat(*l, ".dat", NULL);
            char *path = g_build_filename(g.dir, "lists", file, NULL);
            char *dat = NULL;
            gsize len = 0;
            if (g_file_get_contents(path, &dat, &len, NULL) && wren_ubo_add_list(g.ubo, *l, (const uint8_t *)dat, len))
                g_ptr_array_add(now, g_strdup(*l));
            else
                g_warning("wren-ubo: list %s not loaded (%s)", *l, path);
            g_free(dat);
            g_free(path);
            g_free(file);
        } else {
            g_ptr_array_add(now, g_strdup(*l));
        }
    }
    g_ptr_array_add(now, NULL);
    g_strfreev(loaded);
    loaded = (char **)g_ptr_array_free(now, FALSE);
}

/* on, allowed hosts and (when given) the extra lists. */
static void set_state(gboolean on, GVariant *allowed, GVariant *lists)
{
    g_strfreev(g.allowed);
    g.allowed = strv_of(allowed);
    g.on = on;
    if (lists) {
        g_strfreev(g.lists);
        g.lists = strv_of(lists);
        if (g.ubo)
            apply_lists();
    }
}

static gboolean on_user_message(WebKitWebProcessExtension *extension, WebKitUserMessage *message, gpointer data unused)
{
    if (g_strcmp0(webkit_user_message_get_name(message), "wren-ubo:state") != 0)
        return FALSE;
    GVariant *p = webkit_user_message_get_parameters(message);
    if (p && (g_variant_is_of_type(p, G_VARIANT_TYPE("(basas)")) || g_variant_is_of_type(p, G_VARIANT_TYPE("(bas)")))) {
        gboolean on = TRUE;
        g_variant_get_child(p, 0, "b", &on);
        GVariant *allowed = g_variant_get_child_value(p, 1);
        GVariant *lists = g_variant_n_children(p) > 2 ? g_variant_get_child_value(p, 2) : NULL;
        set_state(on, allowed, lists);
        g_variant_unref(allowed);
        if (lists)
            g_variant_unref(lists);
    }
    g_message("wren-ubo: %s, %u site(s) allowed, %u extra list(s) (so far: %" G_GUINT64_FORMAT " blocked, %"
        G_GUINT64_FORMAT " redirected, %" G_GUINT64_FORMAT " rewritten)", g.on ? "on" : "off",
        g.allowed ? g_strv_length(g.allowed) : 0, g.lists ? g_strv_length(g.lists) : 0, g.blocked, g.redirected,
        g.rewritten);
    return TRUE;
}

static void on_page_created(WebKitWebProcessExtension *extension, WebKitWebPage *page, gpointer data unused)
{
    if (g.log_all)
        g_message("wren-ubo: page %" G_GUINT64_FORMAT " created", webkit_web_page_get_id(page));
    g_signal_connect(page, "send-request", G_CALLBACK(on_send_request), NULL);
}

G_MODULE_EXPORT void webkit_web_process_extension_initialize_with_user_data(WebKitWebProcessExtension *extension,
    GVariant *user_data)
{
    const char *dir = NULL;
    gboolean on = TRUE;
    if (!user_data || !(g_variant_is_of_type(user_data, G_VARIANT_TYPE("(sbasas)"))
                        || g_variant_is_of_type(user_data, G_VARIANT_TYPE("(sbas)")))) {
        g_warning("wren-ubo: no initialization data, not loaded");
        return;
    }
    g_variant_get_child(user_data, 0, "&s", &dir);
    g_variant_get_child(user_data, 1, "b", &on);
    g.dir = g_strdup(dir);
    GVariant *allowed = g_variant_get_child_value(user_data, 2);
    GVariant *lists = g_variant_n_children(user_data) > 3 ? g_variant_get_child_value(user_data, 3) : NULL;
    set_state(on, allowed, lists); /* lists load below, once the core has */
    g_variant_unref(allowed);
    if (lists)
        g_variant_unref(lists);
    /* WPE_BROWSER_DEBUG_UBO_LOG: the same, under a name Wren's launch parameter {"wpebEnv": ...} passes on. */
    const char *log = g_getenv("WREN_UBO_LOG") ? g_getenv("WREN_UBO_LOG") : g_getenv("WPE_BROWSER_DEBUG_UBO_LOG");
    g.log_all = g_strcmp0(log, "2") == 0;
    g.log = g.log_all || g_strcmp0(log, "1") == 0;

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
    char *compat_path = g_build_filename(dir, "compat.js", NULL);
    if (!g_file_get_contents(compat_path, &g.compat_js, NULL, NULL))
        g_warning("wren-ubo: %s missing (no compatibility fixes)", compat_path);
    g_free(compat_path);
    g_free(dat);
    g_free(res);
    g_free(dat_path);
    g_free(res_path);
    g_free(js_path);
    if (!g.ubo)
        return;
    apply_lists();

    g.world = webkit_script_world_new_with_name("wren-ubo");
    g_signal_connect(webkit_script_world_get_default(), "window-object-cleared", G_CALLBACK(on_window_object_cleared), NULL);
    g_signal_connect(extension, "page-created", G_CALLBACK(on_page_created), NULL);
    g_signal_connect(extension, "user-message-received", G_CALLBACK(on_user_message), NULL);
    g_message("wren-ubo: loaded in %.0f ms (%s, %u site(s) allowed, %u extra list(s))",
        (g_get_monotonic_time() - started) / 1e3, g.on ? "on" : "off", g_strv_length(g.allowed),
        g.lists ? g_strv_length(g.lists) : 0);
}
