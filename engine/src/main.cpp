// SPDX-License-Identifier: GPL-3.0-or-later
// wpe-engine: runs WPE WebKit for the webOS browser UI app, which spawns it with a SOCK_SEQPACKET socket on
// fd 3 (common/wpe_ipc.h). Each browser tab is one WebKitWebView; all views share one web process (they are
// created as related views) to keep memory down on a TV. Only the active tab is visible and renders. Two ways
// to the screen, chosen at start-up:
//   SHM (default): pages render into shared-memory buffers (platform.cpp), and each frame is copied into a
//                  memfd slot the UI maps and draws with the TV's GPU.
//   direct:        the web process draws with the TV's EGL into its own surface in the UI's webOS surface
//                  group (direct.cpp, engine/webos-backend); no frames pass through here.
//
// Environment (set by the UI app):
//   WPE_BROWSER_DATA_DIR   cookies, local storage (persistent)
//   WPE_BROWSER_CACHE_DIR  HTTP cache
//   WEBKIT_EXEC_PATH       where WPEWebProcess / WPENetworkProcess are (patched to work in release builds)
//   LD_LIBRARY_PATH, GIO_MODULE_DIR, FONTCONFIG_FILE, ... for the bundled libraries
//   WPE_BROWSER_PRESENT    "direct" for direct presentation, with WPEB_SURFACE_GROUP (the UI's surface group),
//                          WPEB_SURFACE_LAYER (default "page") and WPEB_WAYLAND_SOCKET (the compositor's socket)
#include "../../common/wpe_ipc.h"
#include "direct.h"
#include "platform.h"
#include "popup_filter.h"

#include <wpe/webkit.h>

#include <glib-unix.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <map>
#include <memory>
#include <cctype>
#include <cstring>
#include <string>
#include <vector>
#include <utility>

// Also used by direct.cpp.
void log_msg(const char *fmt, ...) G_GNUC_PRINTF(1, 2);
void log_msg(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "wpe-engine: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

namespace {

struct Tab {
    uint32_t id = 0;
    WebKitWebView *web_view = nullptr;
    WPEView *view = nullptr;                  // SHM mode
    direct::View *direct = nullptr;           // direct mode (owned by the web view's backend)
    WebKitInputMethodContext *ime = nullptr;  // direct mode (owned by the web view)
    bool resize_pending = false;              // direct mode: resized while hidden, applied when shown
    // A window a page opened (window.open, target=_blank): who opened it, when, and whether it is the kind that can be a pop-under
    // (opened by a script, for another site): closed again if the opener moves on within a few seconds (popup_opener_moves).
    uint32_t opener = 0;
    int64_t born_us = 0;
    bool pop_under_candidate = false;
    bool memory_kill = false;                 // the memory guard is ending this tab's web process (reported as "used too much memory")
};

struct Engine {
    int sock = WPE_IPC_ENGINE_FD;
    GMainLoop *loop = nullptr;
    WPEDisplay *display = nullptr;  // SHM mode only
    bool direct = false;            // WPE_BROWSER_PRESENT=direct and the backend loaded (direct.cpp)
    WebKitNetworkSession *session = nullptr;
    WebKitWebContext *context = nullptr;  // our own (memory-pressure-settings is construct-only); never g.context
    WebKitSettings *settings = nullptr;
    std::map<uint32_t, Tab> tabs;
    uint32_t active = 0;
    // Direct mode tab switch: the new tab is shown once the old one's surface has left the layer (or after
    // kShowAfterDetachMs), so two pages never hold it at once and the old one is detached by its own thread.
    uint32_t show_pending = 0, detach_awaited = 0;
    guint show_timer = 0;
    uint32_t width = 1280, height = 720;
    double zoom = 1.0;
    double scale = 1.0;      // device scale: buffers are CSS size x scale (sharp text on a 1080p screen)

    // Frame slots in a memfd shared with the UI.
    int frames_fd = -1;
    uint8_t *frames = nullptr;
    uint32_t slot_bytes = 0;
    uint32_t next_slot = 0;
    // last frame handed to the UI (for the identical-empty-frame skip in on_frame)
    bool last_valid = false;
    uint32_t last_slot = 0, last_tab = 0, last_width = 0, last_height = 0, last_stride = 0;
    uint32_t serial = 0;
    WPEBuffer *in_flight[WPE_IPC_FRAME_SLOTS] = {};
    WPEView *in_flight_view[WPE_IPC_FRAME_SLOTS] = {};

    // Pointer state for modifiers and press counts.
    uint32_t buttons = 0;
    double pointer_x = 0, pointer_y = 0;
};

Engine g;


guint32 now_ms()
{
    return static_cast<guint32>(g_get_monotonic_time() / 1000);
}

Tab *tab_by_id(uint32_t id)
{
    auto it = g.tabs.find(id);
    return it == g.tabs.end() ? nullptr : &it->second;
}

uint32_t tab_of_web_view(WebKitWebView *web_view)
{
    for (auto &[id, tab] : g.tabs) {
        if (tab.web_view == web_view)
            return id;
    }
    return 0;
}

uint32_t tab_of_view(WPEView *view)
{
    for (auto &[id, tab] : g.tabs) {
        if (tab.view == view)
            return id;
    }
    return 0;
}

Tab *active_tab()
{
    return tab_by_id(g.active);
}

// ---- sending ---------------------------------------------------------------------------------------------

bool send_msg(uint32_t type, uint32_t tab, const void *payload, uint32_t size, int fd = -1)
{
    uint8_t packet[WPE_IPC_MAX_PACKET];
    if (sizeof(WpeMsgHeader) + size > sizeof(packet))
        return false;
    WpeMsgHeader header { type, size, tab };
    memcpy(packet, &header, sizeof(header));
    if (size)
        memcpy(packet + sizeof(header), payload, size);

    iovec iov { packet, sizeof(header) + size };
    msghdr msg {};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))];
    if (fd >= 0) {
        msg.msg_control = control;
        msg.msg_controllen = sizeof(control);
        cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(cmsg), &fd, sizeof(int));
    }
    for (;;) {
        ssize_t n = sendmsg(g.sock, &msg, MSG_NOSIGNAL);
        if (n >= 0)
            return true;
        if (errno == EINTR)
            continue;
        log_msg("send failed: %s", strerror(errno));
        return false;
    }
}

void send_text(uint32_t type, uint32_t tab, const char *text)
{
    WpeText t {};
    const size_t len = text ? std::min<size_t>(strlen(text), WPE_IPC_MAX_TEXT - 1) : 0;
    t.length = static_cast<uint32_t>(len);
    if (len)
        memcpy(t.text, text, len);
    t.text[len] = '\0';
    send_msg(type, tab, &t, static_cast<uint32_t>(offsetof(WpeText, text) + len + 1));
}

void send_error(uint32_t tab, uint32_t kind, uint32_t code, const char *message)
{
    WpeError e {};
    e.kind = kind;
    e.code = code;
    const size_t len = message ? std::min<size_t>(strlen(message), WPE_IPC_MAX_TEXT - 1) : 0;
    e.message.length = static_cast<uint32_t>(len);
    if (len)
        memcpy(e.message.text, message, len);
    send_msg(WPE_ENGINE_ERROR, tab, &e, static_cast<uint32_t>(offsetof(WpeError, message) + offsetof(WpeText, text) + len + 1));
}

void send_history(uint32_t tab, WebKitWebView *web_view)
{
    WpeHistory h { webkit_web_view_can_go_back(web_view) ? 1u : 0u, webkit_web_view_can_go_forward(web_view) ? 1u : 0u };
    send_msg(WPE_ENGINE_HISTORY, tab, &h, sizeof(h));
}

// ---- frames ----------------------------------------------------------------------------------------------

bool ensure_frames(uint32_t needed)
{
    if (g.frames && needed <= g.slot_bytes)
        return true;
    for (auto *b : g.in_flight) {
        if (b)
            return false; // cannot remap under a frame the UI is still reading; the caller drops this one
    }
    const uint32_t slot = (needed + 0xFFFFFu) & ~0xFFFFFu;
    if (g.frames) {
        munmap(g.frames, static_cast<size_t>(g.slot_bytes) * WPE_IPC_FRAME_SLOTS);
        g.frames = nullptr;
    }
    if (g.frames_fd >= 0)
        close(g.frames_fd);
    g.frames_fd = memfd_create("wpe-browser-frames", MFD_CLOEXEC);
    if (g.frames_fd < 0) {
        log_msg("memfd_create: %s", strerror(errno));
        return false;
    }
    const size_t total = static_cast<size_t>(slot) * WPE_IPC_FRAME_SLOTS;
    if (ftruncate(g.frames_fd, static_cast<off_t>(total)) < 0) {
        log_msg("ftruncate: %s", strerror(errno));
        return false;
    }
    void *map = mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_SHARED, g.frames_fd, 0);
    if (map == MAP_FAILED) {
        log_msg("mmap: %s", strerror(errno));
        return false;
    }
    g.frames = static_cast<uint8_t *>(map);
    g.slot_bytes = slot;
    g.next_slot = 0;
    g.last_valid = false;   // the new memfd holds no previous frame
    WpeFrames f { slot, WPE_IPC_FRAME_SLOTS };
    return send_msg(WPE_ENGINE_FRAMES, 0, &f, sizeof(f), g.frames_fd);
}

void on_frame(void *, WPEView *view, WPEBuffer *buffer, const WPERectangle *damage, guint n_damage)
{
    const uint32_t tab = tab_of_view(view);
    WPEBufferSHM *shm = WPE_BUFFER_SHM(buffer);
    GBytes *bytes = wpe_buffer_shm_get_data(shm);
    gsize size = 0;
    const auto *pixels = static_cast<const uint8_t *>(g_bytes_get_data(bytes, &size));
    const uint32_t width = static_cast<uint32_t>(wpe_buffer_get_width(buffer));
    const uint32_t height = static_cast<uint32_t>(wpe_buffer_get_height(buffer));
    const uint32_t stride = wpe_buffer_shm_get_stride(shm);
    const uint32_t needed = stride * height;
    // A hidden tab's frame (e.g. rendered just before a switch) is not shown: hand it straight back.
    if (tab != g.active || !pixels || size < needed || !ensure_frames(needed)) {
        webos_view_frame_shown(WEBOS_VIEW(view), buffer);
        return;
    }

    // WebKit 2.54 paints a frame per animation tick even when nothing visible changed, and an empty damage list is
    // NOT proof of that (AcceleratedSurface::takeFrameDamageRects returns {} also when no damage was recorded, e.g.
    // after a swap-chain reset on resize). So an empty list is treated as "unknown", and the frame is skipped only
    // when its pixels are byte-identical to the last frame handed to the UI for this tab: then nothing can be lost.
    if (n_damage == 0 && g.last_valid && g.last_tab == tab && g.last_width == width && g.last_height == height
        && g.last_stride == stride) {
        const bool same = !memcmp(g.frames + static_cast<size_t>(g.last_slot) * g.slot_bytes, pixels, needed);
        if (same) {
            webos_view_frame_shown(WEBOS_VIEW(view), buffer);
            return;
        }
    }

    uint32_t slot = g.next_slot;
    for (uint32_t i = 0; i < WPE_IPC_FRAME_SLOTS && g.in_flight[slot]; ++i)
        slot = (slot + 1) % WPE_IPC_FRAME_SLOTS;
    if (g.in_flight[slot]) {
        webos_view_frame_shown(WEBOS_VIEW(view), buffer);
        return;
    }
    g.next_slot = (slot + 1) % WPE_IPC_FRAME_SLOTS;
    memcpy(g.frames + static_cast<size_t>(slot) * g.slot_bytes, pixels, needed);
    g.last_valid = true; g.last_slot = slot; g.last_tab = tab; g.last_width = width; g.last_height = height; g.last_stride = stride;

    WpeFrame f {};
    f.slot = slot;
    f.serial = ++g.serial;
    f.width = width;
    f.height = height;
    f.stride = stride;
    if (n_damage > 0 && n_damage <= WPE_IPC_MAX_DAMAGE) {
        f.n_damage = n_damage;
        for (guint i = 0; i < n_damage; ++i)
            f.damage[i] = WpeRect { damage[i].x, damage[i].y, damage[i].width, damage[i].height };
    } else if (n_damage > WPE_IPC_MAX_DAMAGE) {
        int x0 = damage[0].x, y0 = damage[0].y, x1 = x0 + damage[0].width, y1 = y0 + damage[0].height;
        for (guint i = 1; i < n_damage; ++i) {
            x0 = std::min(x0, damage[i].x);
            y0 = std::min(y0, damage[i].y);
            x1 = std::max(x1, damage[i].x + damage[i].width);
            y1 = std::max(y1, damage[i].y + damage[i].height);
        }
        f.n_damage = 1;
        f.damage[0] = WpeRect { x0, y0, x1 - x0, y1 - y0 };
    }
    g.in_flight[slot] = WPE_BUFFER(g_object_ref(buffer));
    g.in_flight_view[slot] = WPE_VIEW(g_object_ref(view));
    if (!send_msg(WPE_ENGINE_FRAME, tab, &f, WPE_IPC_PAYLOAD_BYTES(WpeFrame, damage))) {
        g_clear_object(&g.in_flight[slot]);
        g_clear_object(&g.in_flight_view[slot]);
        webos_view_frame_shown(WEBOS_VIEW(view), buffer);
    }
}

void frame_done(const WpeFrameDone &d)
{
    if (d.slot >= WPE_IPC_FRAME_SLOTS || !g.in_flight[d.slot])
        return;
    WPEBuffer *buffer = g.in_flight[d.slot];
    WPEView *view = g.in_flight_view[d.slot];
    g.in_flight[d.slot] = nullptr;
    g.in_flight_view[d.slot] = nullptr;
    webos_view_frame_shown(WEBOS_VIEW(view), buffer);
    g_object_unref(buffer);
    g_object_unref(view);
}

void on_ime_focus(void *, WPEView *view, gboolean focused, WPEInputPurpose purpose, int x, int y, int width, int height)
{
    WpeIme m { focused ? 1u : 0u, static_cast<uint32_t>(purpose), x, y, width, height };
    send_msg(WPE_ENGINE_IME, tab_of_view(view), &m, sizeof(m));
}

// ---- input (always to the active tab) --------------------------------------------------------------------

void dispatch(WPEEvent *event)
{
    if (event)
        wpe_view_event(wpe_event_get_view(event), event);
    if (event)
        wpe_event_unref(event);
}

WPEModifiers button_modifier(uint32_t button)
{
    switch (button) {
    case 1: return WPE_MODIFIER_POINTER_BUTTON1;
    case 2: return WPE_MODIFIER_POINTER_BUTTON2;
    case 3: return WPE_MODIFIER_POINTER_BUTTON3;
    default: return static_cast<WPEModifiers>(0);
    }
}

void handle_key(WPEView *view, const WpeKey &k)
{
    dispatch(wpe_event_keyboard_new(k.down ? WPE_EVENT_KEYBOARD_KEY_DOWN : WPE_EVENT_KEYBOARD_KEY_UP, view,
        WPE_INPUT_SOURCE_KEYBOARD, now_ms(), static_cast<WPEModifiers>(k.modifiers), k.keycode, k.keyval));
}

void handle_pointer(WPEView *view, const WpePointer &p)
{
    const double x = p.x, y = p.y;
    const guint32 t = p.time_ms ? p.time_ms : now_ms();
    switch (p.kind) {
    case WPE_POINTER_MOVE: {
        const double dx = x - g.pointer_x, dy = y - g.pointer_y;
        g.pointer_x = x;
        g.pointer_y = y;
        dispatch(wpe_event_pointer_move_new(WPE_EVENT_POINTER_MOVE, view, WPE_INPUT_SOURCE_MOUSE, t,
            static_cast<WPEModifiers>(g.buttons), x, y, dx, dy));
        break;
    }
    case WPE_POINTER_DOWN:
    case WPE_POINTER_UP: {
        const bool down = p.kind == WPE_POINTER_DOWN;
        g.pointer_x = x;
        g.pointer_y = y;
        const WPEModifiers bit = button_modifier(p.button);
        if (down)
            g.buttons |= bit;
        else
            g.buttons &= ~static_cast<uint32_t>(bit);
        const guint count = down ? wpe_view_compute_press_count(view, x, y, p.button, t) : 0;
        dispatch(wpe_event_pointer_button_new(down ? WPE_EVENT_POINTER_DOWN : WPE_EVENT_POINTER_UP, view,
            WPE_INPUT_SOURCE_MOUSE, t, static_cast<WPEModifiers>(g.buttons), p.button, x, y, count));
        break;
    }
    case WPE_POINTER_LEAVE:
        dispatch(wpe_event_pointer_move_new(WPE_EVENT_POINTER_LEAVE, view, WPE_INPUT_SOURCE_MOUSE, t,
            static_cast<WPEModifiers>(g.buttons), g.pointer_x, g.pointer_y, 0, 0));
        break;
    }
}

void handle_scroll(WPEView *view, const WpeScroll &s)
{
    // Wheel notches, not pixels (precise_deltas = FALSE). WPE's sign is the opposite of ours: like its
    // Wayland platform, negative means "content moves up" (scroll down).
    dispatch(wpe_event_scroll_new(view, WPE_INPUT_SOURCE_MOUSE, now_ms(), static_cast<WPEModifiers>(g.buttons),
        -s.delta_x / 120.0, -s.delta_y / 120.0, FALSE, FALSE, s.x, s.y));
}

void resize_view(Tab &tab)
{
    if (tab.direct) {
        direct::set_scale(tab.direct, g.scale);
        direct::set_size(tab.direct, g.width, g.height);
        return;
    }
    WPEView *view = tab.view;
    if (WPEToplevel *toplevel = wpe_view_get_toplevel(view)) {
        wpe_toplevel_resize(toplevel, static_cast<int>(g.width), static_cast<int>(g.height));
        if (wpe_toplevel_get_scale(toplevel) != g.scale)
            wpe_toplevel_scale_changed(toplevel, g.scale);
    } else
        wpe_view_resized(view, static_cast<int>(g.width), static_cast<int>(g.height));
}

// In direct mode only the shown tab is resized now; hidden tabs get the size when shown (tab_activate). A hidden
// page resized draws a frame into its detached, unmapped surface, which maps it again as a window of its own above
// the browser, and webOS then minimises the browser (TV, 2026-11-15: new tab, Home page, then a page in it; the
// docked toolbar changes the page size between the two).
void resize_shown();

void resize(uint32_t width, uint32_t height)
{
    g.width = std::clamp<uint32_t>(width, 64, 3840);
    g.height = std::clamp<uint32_t>(height, 64, 2160);
    resize_shown();
}

void resize_shown()
{
    for (auto &[id, tab] : g.tabs) {
        if (tab.direct && id != g.active) {
            tab.resize_pending = true;
            continue;
        }
        tab.resize_pending = false;
        resize_view(tab);
    }
}

// ---- WebKit signals (user_data = nothing; the tab is looked up from the view) ----------------------------

void on_uri(WebKitWebView *view, GParamSpec *, gpointer) { send_text(WPE_ENGINE_URI, tab_of_web_view(view), webkit_web_view_get_uri(view)); }
void on_title(WebKitWebView *view, GParamSpec *, gpointer) { send_text(WPE_ENGINE_TITLE, tab_of_web_view(view), webkit_web_view_get_title(view)); }

void on_progress(WebKitWebView *view, GParamSpec *, gpointer)
{
    WpeProgress p { static_cast<uint32_t>(std::clamp(webkit_web_view_get_estimated_load_progress(view), 0.0, 1.0) * 1000) };
    send_msg(WPE_ENGINE_PROGRESS, tab_of_web_view(view), &p, sizeof(p));
}

void on_load_changed(WebKitWebView *view, WebKitLoadEvent event, gpointer)
{
    WpeLoad l {};
    switch (event) {
    case WEBKIT_LOAD_STARTED: l.event = WPE_LOAD_STARTED; break;
    case WEBKIT_LOAD_COMMITTED: l.event = WPE_LOAD_COMMITTED; break;
    case WEBKIT_LOAD_FINISHED: l.event = WPE_LOAD_FINISHED; break;
    default: return;
    }
    const uint32_t tab = tab_of_web_view(view);
    send_msg(WPE_ENGINE_LOAD, tab, &l, sizeof(l));
    send_history(tab, view);

    // A Cloudflare bot check ("cf-mitigated: challenge" on the page's own response) runs about a minute of
    // JavaScript on a TV's CPU (TV, 2026-11-16: 68–84 s; JIT thresholds did not shorten it). The UI says so.
    if (event == WEBKIT_LOAD_COMMITTED) {
        WebKitWebResource *main = webkit_web_view_get_main_resource(view);
        WebKitURIResponse *response = main ? webkit_web_resource_get_response(main) : nullptr;
        SoupMessageHeaders *headers = response ? webkit_uri_response_get_http_headers(response) : nullptr;
        const char *mitigated = headers ? soup_message_headers_get_one(headers, "cf-mitigated") : nullptr;
        if (mitigated && g_ascii_strcasecmp(mitigated, "challenge") == 0) {
            log_msg("tab %u: the page is a bot check (cf-mitigated: challenge)", tab);
            const WpeFlag notice { WPE_NOTICE_BOT_CHECK };
            send_msg(WPE_ENGINE_NOTICE, tab, &notice, sizeof(notice));
        }
    }

    // Page timings for the log (TV diagnostics: which site, how long to first commit / full load).
    static guint32 started = 0;
    const char *uri = webkit_web_view_get_uri(view);
    if (event == WEBKIT_LOAD_STARTED) {
        started = now_ms();
        log_msg("tab %u load started: %.200s", tab, uri ? uri : "");
    } else {
        log_msg("tab %u load %s after %u ms: %.200s", tab, event == WEBKIT_LOAD_COMMITTED ? "committed" : "finished",
            now_ms() - started, uri ? uri : "");
    }
}

gboolean on_load_failed(WebKitWebView *view, WebKitLoadEvent, gchar *uri, GError *error, gpointer)
{
    const uint32_t tab = tab_of_web_view(view);
    WpeLoad l { WPE_LOAD_FAILED };
    send_msg(WPE_ENGINE_LOAD, tab, &l, sizeof(l));
    if (!g_error_matches(error, WEBKIT_NETWORK_ERROR, WEBKIT_NETWORK_ERROR_CANCELLED)) {
        log_msg("load failed for %s: %s", uri, error->message);
        send_error(tab, WPE_ERROR_LOAD, static_cast<uint32_t>(error->code), error->message);
    }
    return FALSE; // let WebKit show its error page
}

gboolean on_tls_error(WebKitWebView *view, gchar *uri, GTlsCertificate *, GTlsCertificateFlags flags, gpointer)
{
    log_msg("TLS error for %s (flags 0x%x)", uri, flags);
    send_error(tab_of_web_view(view), WPE_ERROR_TLS, flags, uri);
    return FALSE;
}

void on_web_process_terminated(WebKitWebView *view, WebKitWebProcessTerminationReason reason, gpointer)
{
    if (Tab *killed = tab_by_id(tab_of_web_view(view)); killed && killed->memory_kill) {
        killed->memory_kill = false;
        reason = WEBKIT_WEB_PROCESS_EXCEEDED_MEMORY_LIMIT;
    }
    const char *why = reason == WEBKIT_WEB_PROCESS_CRASHED ? "crashed"
        : reason == WEBKIT_WEB_PROCESS_EXCEEDED_MEMORY_LIMIT ? "used too much memory" : "was terminated";
    // With the time and page, so a crash can be matched to what was on screen (no core dumps: see main()).
    const char *uri = webkit_web_view_get_uri(view);
    GDateTime *now = g_date_time_new_now_utc();
    gchar *when = g_date_time_format(now, "%F %T UTC");
    log_msg("web process %s at %s on %s", why, when, uri ? uri : "(no page)");
    g_free(when);
    g_date_time_unref(now);
    send_error(tab_of_web_view(view), WPE_ERROR_WEB_PROCESS_CRASHED, static_cast<uint32_t>(reason), why);
}

// Whether the page is scrolled to its top, for the UI's toolbar (shown at the top, hidden once the page scrolls).
// The script runs in its own world, so pages can neither see nor fake it. Pages that scroll an element instead of
// the document (web apps) count too: scroll events are caught on the way down (capture) from any large scroller.
constexpr const char *kScrollWorld = "wpeb";
constexpr const char *kScrollScript = R"JS((function () {
    var last = null;
    function report(top) {
        if (top === last) return;
        last = top;
        window.webkit.messageHandlers.wpebScroll.postMessage(top);
    }
    function pageTop() {
        var e = document.scrollingElement || document.documentElement;
        return (window.scrollY || (e ? e.scrollTop : 0)) <= 2;
    }
    addEventListener('scroll', function (ev) {
        var t = ev.target;
        if (t === document || t === window || t === document.scrollingElement) { report(pageTop()); return; }
        if (t && t.clientHeight > innerHeight * 0.5) report(t.scrollTop <= 2 && pageTop());
    }, {capture: true, passive: true});
    // Back/forward puts a page back at its old position, after this script ran and without a scroll event. Look
    // again shortly after the page shows (always sent: the UI may have seen other pages meanwhile).
    function recheck() {
        last = null;
        report(pageTop());
        [150, 600, 1500].forEach(function (ms) { setTimeout(function () { last = null; report(pageTop()); }, ms); });
    }
    addEventListener('pageshow', recheck);
    recheck();
    // The element that scrolls the content: the document, or (web apps, sites with a fixed layout) the largest
    // element that scrolls vertically and covers most of the screen. Spatial navigation only scrolls the box that holds
    // the focused link, so with nothing focusable on screen such a box never moves under the arrow keys.
    function scroller() {
        var doc = document.scrollingElement || document.documentElement;
        if (doc && doc.scrollHeight > doc.clientHeight + 4) return doc;
        var best = null, bestArea = innerWidth * innerHeight * 0.3;
        var all = document.body ? document.body.getElementsByTagName('*') : [];
        for (var i = 0; i < all.length && i < 5000; i++) {
            var el = all[i];
            if (el.scrollHeight <= el.clientHeight + 4 || el.clientHeight < innerHeight * 0.4) continue;
            var oy = getComputedStyle(el).overflowY;
            if (oy !== 'auto' && oy !== 'scroll' && oy !== 'overlay') continue;
            var area = el.clientWidth * el.clientHeight;
            if (area > bestArea) { best = el; bestArea = area; }
        }
        return best || doc;
    }
    // Up / Down that changed nothing: spatial navigation moved no focus, nothing scrolled and the page did not take
    // the key (defaultPrevented: web apps with their own remote handling). Down then scrolls the content box itself;
    // Up does too unless the content is at its top, where it brings the toolbar (wpebEdge). Typing never counts.
    addEventListener('keydown', function (ev) {
        if (ev.key !== 'ArrowUp' && ev.key !== 'ArrowDown') return;
        var a = document.activeElement;
        if (a && (a.isContentEditable || /^(INPUT|TEXTAREA|SELECT)$/.test(a.tagName))) return;
        var y = window.scrollY, s = scroller(), st = s ? s.scrollTop : 0;
        setTimeout(function () {
            if (ev.defaultPrevented || document.activeElement !== a || window.scrollY !== y) return;
            if (s && s.scrollTop !== st) return;
            var up = ev.key === 'ArrowUp';
            if (s && (up ? s.scrollTop > 2 : s.scrollTop + s.clientHeight < s.scrollHeight - 2)) {
                var step = Math.round(Math.min(s.clientHeight, innerHeight) * (ev.repeat ? 0.15 : 0.4));
                s.scrollBy({top: up ? -step : step, behavior: ev.repeat ? 'auto' : 'smooth'});
            } else if (up && !ev.repeat && pageTop()) {
                window.webkit.messageHandlers.wpebEdge.postMessage(1);
            }
        }, 120);
    }, {capture: true});
    // The page's icon for the UI's tabs, tiles and bookmarks: apple-touch-icon (large, made for home screens), else
    // the largest icon link in a format the UI decodes (PNG, SVG, WebP, JPEG, GIF; not ICO). Sent after load and
    // again when the head changes (single-page apps).
    var lastIcon = null;
    function sendIcon() {
        var best = null, bestScore = -1;
        var links = document.querySelectorAll('link[rel]');
        for (var i = 0; i < links.length; i++) {
            var l = links[i], rel = (l.getAttribute('rel') || '').toLowerCase(), href = l.href || '';
            if (!href || href.indexOf('data:') === 0) continue;
            var touch = rel.indexOf('apple-touch-icon') >= 0, icon = /(^|\s)icon(\s|$)/.test(rel);
            if (!touch && !icon) continue;
            var type = (l.getAttribute('type') || '').toLowerCase(), path = href.split(/[?#]/)[0].toLowerCase();
            if (/\.ico$/.test(path) || type.indexOf('icon') >= 0) continue;
            if (!touch && !/\.(png|svg|webp|jpe?g|gif)$/.test(path) && !/png|svg|webp|jpeg|gif/.test(type)) continue;
            var size = 0, m = /(\d+)x\d+/.exec(l.getAttribute('sizes') || '');
            if (m) size = parseInt(m[1], 10);
            if (/\.svg$/.test(path) || type.indexOf('svg') >= 0) size = 256;
            var score = (touch ? 1000 : 0) + Math.min(size || 32, 512);
            if (score > bestScore) { best = href; bestScore = score; }
        }
        best = best || '';
        if (best === lastIcon) return;
        lastIcon = best;
        window.webkit.messageHandlers.wpebIcon.postMessage(best);
    }
    addEventListener('load', function () { sendIcon(); setTimeout(sendIcon, 2000); });
    addEventListener('pageshow', function () { lastIcon = null; sendIcon(); });
    if (document.head) new MutationObserver(function () { setTimeout(sendIcon, 300); }).observe(document.head, {childList: true});
})();)JS";

// Page-load timing (Navigation Timing, after the load event), one log line per page: where the time goes (network:
// DNS, connect/TLS, server, transfer; then parsing, scripts and layout up to DOMContentLoaded / load). Milliseconds
// from the start of the navigation.
constexpr const char *kPerfScript = R"JS((function () {
    addEventListener('load', function () {
        setTimeout(function () {
            var n = performance.getEntriesByType('navigation')[0];
            if (!n) return;
            var res = performance.getEntriesByType('resource'), kb = 0;
            for (var i = 0; i < res.length; i++) kb += (res[i].transferSize || 0) / 1024;
            var fcp = 0, paints = performance.getEntriesByType('paint');
            for (var j = 0; j < paints.length; j++) if (paints[j].name === 'first-contentful-paint') fcp = paints[j].startTime;
            var r = function (x) { return Math.round(x); };
            window.webkit.messageHandlers.wpebPerf.postMessage(
                'dns ' + r(n.domainLookupEnd - n.domainLookupStart) +
                ' connect ' + r(n.connectEnd - n.connectStart) +
                ' (tls ' + r(n.secureConnectionStart > 0 ? n.connectEnd - n.secureConnectionStart : 0) + ')' +
                ' ttfb ' + r(n.responseStart - n.requestStart) +
                ' html ' + r(n.responseEnd - n.responseStart) +
                ' | interactive ' + r(n.domInteractive) + ' dcl ' + r(n.domContentLoadedEventEnd) +
                ' fcp ' + r(fcp) + ' load ' + r(n.loadEventStart) +
                ' | ' + res.length + ' resources ' + r(kb) + ' KB | ' + location.host);
            // Resources that took over 2 s or ended after DOMContentLoaded+load waited on them: what slows a page.
            var slow = [];
            for (var k = 0; k < res.length; k++) {
                var e = res[k], d = e.responseEnd - e.startTime;
                if (d < 2000) continue;
                var u = e.name.replace(/[?#].*$/, '');
                if (u.length > 90) u = u.slice(0, 60) + '...' + u.slice(-27);
                slow.push(r(e.startTime) + '+' + r(d) + 'ms ' + (e.initiatorType || '?') + ' ' + u);
            }
            slow.sort(function (x, y) { return parseInt(y.split('+')[1], 10) - parseInt(x.split('+')[1], 10); });
            for (var m = 0; m < slow.length && m < 8; m++)
                window.webkit.messageHandlers.wpebPerf.postMessage('slow resource ' + slow[m]);
        }, 0);
    });
})();)JS";

void on_perf_message(WebKitUserContentManager *, JSCValue *value, gpointer data)
{
    gchar *text = jsc_value_to_string(value);
    log_msg("tab %u timing: %s", GPOINTER_TO_UINT(data), text);
    g_free(text);
}

gboolean on_fullscreen_changed(WebKitWebView *view, gpointer entered)
{
    const uint32_t id = tab_of_web_view(view);
    WpeFlag flag = {GPOINTER_TO_UINT(entered)};
    log_msg("tab %u %s fullscreen", id, flag.value ? "entered" : "left");
    send_msg(WPE_ENGINE_FULLSCREEN, id, &flag, sizeof(flag));
    return FALSE; // let it happen
}

void on_scroll_message(WebKitUserContentManager *, JSCValue *value, gpointer data)
{
    const uint32_t id = GPOINTER_TO_UINT(data);
    WpeFlag flag = {jsc_value_to_boolean(value) ? 1u : 0u};
    if (getenv("WPE_BROWSER_DEBUG_SCROLL"))
        log_msg("tab %u scroll: %s", id, flag.value ? "top" : "scrolled");
    send_msg(WPE_ENGINE_SCROLL, id, &flag, sizeof(flag));
}

// YouTube opens as its TV app (youtube.com/tv, "leanback"): on the TV it is interactive in ~1.6 s and ~180 MB
// against 10-15 s and ~600 MB for the desktop site, and is made for the remote (2026-11-14). Page navigations to
// youtube.com / m.youtube.com / youtu.be go there instead, videos deep-linked (#/watch?v=ID). Embedded players
// (/embed/), APIs and the TV app itself are left alone. WPE_BROWSER_DESKTOP_YOUTUBE=1 turns it off.
std::string youtube_tv_url(const char *uri)
{
    if (!uri || getenv("WPE_BROWSER_DESKTOP_YOUTUBE"))
        return {};
    std::string u = uri;
    const size_t scheme = u.find("://");
    if (scheme == std::string::npos || u.compare(0, 4, "http") != 0)
        return {};
    const size_t host_start = scheme + 3;
    const size_t host_end = u.find_first_of("/?#", host_start);
    std::string host = u.substr(host_start, host_end == std::string::npos ? std::string::npos : host_end - host_start);
    std::string rest = host_end == std::string::npos ? "/" : u.substr(host_end);
    if (rest[0] != '/')
        rest = "/" + rest;
    auto video_id_ok = [](const std::string &id) {
        if (id.empty() || id.size() > 20)
            return false;
        for (char c : id)
            if (!isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_')
                return false;
        return true;
    };
    auto watch = [&](const std::string &id) { return video_id_ok(id) ? "https://www.youtube.com/tv#/watch?v=" + id : std::string("https://www.youtube.com/tv"); };
    if (host == "youtu.be") {
        const std::string id = rest.substr(1, rest.find_first_of("/?#", 1) == std::string::npos ? std::string::npos : rest.find_first_of("/?#", 1) - 1);
        return watch(id);
    }
    if (host != "youtube.com" && host != "www.youtube.com" && host != "m.youtube.com")
        return {};
    const std::string path = rest.substr(0, rest.find_first_of("?#"));
    if (path.rfind("/watch", 0) == 0) {
        const size_t v = rest.find("v=");
        if (v == std::string::npos)
            return watch({});
        const size_t end = rest.find_first_of("&#", v + 2);
        return watch(rest.substr(v + 2, end == std::string::npos ? std::string::npos : end - v - 2));
    }
    if (path.rfind("/shorts/", 0) == 0) {
        const size_t end = path.find('/', 8);
        return watch(path.substr(8, end == std::string::npos ? std::string::npos : end - 8));
    }
    // The site's pages (not /tv, /embed/, /api/, /s/, /youtubei/, sign-in, ...).
    static const char *const kPages[] = {"/", "/results", "/feed/", "/@", "/channel/", "/c/", "/user/", "/playlist", "/gaming", "/premium"};
    for (const char *p : kPages) {
        if (path == p || (strlen(p) > 1 && path.rfind(p, 0) == 0))
            return "https://www.youtube.com/tv";
    }
    return {};
}

gboolean load_later(gpointer data)
{
    auto *job = static_cast<std::pair<WebKitWebView *, std::string> *>(data);
    webkit_web_view_load_uri(job->first, job->second.c_str());
    g_object_unref(job->first);
    delete job;
    return G_SOURCE_REMOVE;
}

bool popup_navigation_blocked(WebKitWebView *view, const char *uri);
void popup_opener_moves(WebKitWebView *view, const char *uri);

gboolean on_decide_policy(WebKitWebView *view, WebKitPolicyDecision *decision, WebKitPolicyDecisionType type, gpointer)
{
    if (type != WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION)
        return FALSE;
    WebKitNavigationAction *action = webkit_navigation_policy_decision_get_navigation_action(WEBKIT_NAVIGATION_POLICY_DECISION(decision));
    const char *uri = webkit_uri_request_get_uri(webkit_navigation_action_get_request(action));
    if (popup_navigation_blocked(view, uri)) {
        webkit_policy_decision_ignore(decision);
        return TRUE;
    }
    popup_opener_moves(view, uri);
    const std::string tv = youtube_tv_url(uri);
    if (tv.empty())
        return FALSE;
    log_msg("tab %u: %s -> %s (YouTube's TV app)", tab_of_web_view(view), uri, tv.c_str());
    webkit_policy_decision_ignore(decision);
    g_idle_add(load_later, new std::pair<WebKitWebView *, std::string>(WEBKIT_WEB_VIEW(g_object_ref(view)), tv));
    return TRUE;
}

void on_icon_message(WebKitUserContentManager *, JSCValue *value, gpointer data)
{
    gchar *url = jsc_value_to_string(value);
    send_text(WPE_ENGINE_ICON, GPOINTER_TO_UINT(data), url ? url : "");
    g_free(url);
}

void on_edge_message(WebKitUserContentManager *, JSCValue *value, gpointer data)
{
    WpeFlag flag = {static_cast<uint32_t>(jsc_value_to_int32(value))};
    send_msg(WPE_ENGINE_EDGE, GPOINTER_TO_UINT(data), &flag, sizeof(flag));
}

// Media limits. Video is decoded on the CPU (GStreamer libav/libvpx); the TV's 4 A55 cores keep up with H.264 at 720p30.
// Offered more, sites stream what they think the screen deserves: YouTube sent VP9 1080p60 for the 1920x1080 page,
// the decoder dropped most frames and the web process grew to ~800 MB until webOS (sam) killed it (TV, 2026-11-11).
// Sites ask MediaSource.isTypeSupported('...; width=; height=; framerate='), canPlayType and mediaCapabilities, so
// answering "no" beyond the limit makes them pick a stream we can play. Page world, every frame (embedded players).
// WPE_BROWSER_DEBUG_MEDIA="off" or "PIXELS@FPS[+vp9]" (e.g. "2073600@60+vp9") changes it.
constexpr const char *kMediaLimitScript = R"JS((function () {
  const cfg = __WPEB_MEDIA__;
  const num = (t, k) => { const m = t.match(new RegExp('[;\\s]' + k + '\\s*=\\s*"?([0-9.]+)')); return m ? parseFloat(m[1]) : 0; };
  const tooBig = (w, h, fps) => (w || 0) * (h || 0) > cfg.pixels || (fps || 0) > cfg.fps + 0.5;
  // Dolby (AC-3/E-AC-3): YouTube then sends 5.1 E-AC-3, decoded in software to 6 channels for a stereo
  // PulseAudio stream; the audio branch stalled and held the video 3 s ahead of it (TV, 2026-11-11). AAC/Opus play.
  const codecOk = (t) => !/av01|av1/.test(t) && !/ec-3|ac-3/.test(t) && (cfg.vp9 || !/^video\/.*(vp9|vp09)/.test(t));
  // The TV's H.264 decoder presents only half of the frames of H.264 above 1080p at more than 30 fps (TV, 2026-11-20: 2560x1440@60 gave
  // 30 frames/s through the LG player and 30 displayed + 130 dropped per 5 s through Starfish, whatever frame rate or envelope was declared;
  // 3840x2160@50 gave 25; 1920x1080@60, 3840x2160@30 and HEVC 2560x1440@60 are full rate). Players are told such H.264 does not play, so they
  // pick another rendition (1080p60 or 2160p30) or codec. WPE_BROWSER_DEBUG_MEDIA="...+nocap264" lifts it.
  const slowAvc = (t, w, h, fps) => !cfg.noAvcCap && /avc|h264|h\.264/.test(t) && (w || 0) * (h || 0) > 1920 * 1088 && (fps || 0) > 30.5;
  const ok = (type) => {
    if (typeof type !== 'string') return true;
    const t = type.toLowerCase();
    return codecOk(t) && !tooBig(num(t, 'width'), num(t, 'height'), num(t, 'framerate')) && !slowAvc(t, num(t, 'width'), num(t, 'height'), num(t, 'framerate'));
  };
  for (const name of ['MediaSource', 'ManagedMediaSource', 'WebKitMediaSource']) {
    const MS = window[name];
    if (!MS || typeof MS.isTypeSupported !== 'function') continue;
    const original = MS.isTypeSupported.bind(MS);
    Object.defineProperty(MS, 'isTypeSupported', { value: function isTypeSupported(t) { return ok(t) && original(t); }, configurable: true, writable: true });
  }
  if (window.HTMLMediaElement) {
    const original = HTMLMediaElement.prototype.canPlayType;
    Object.defineProperty(HTMLMediaElement.prototype, 'canPlayType', { value: function canPlayType(t) { return ok(t) ? original.call(this, t) : ''; }, configurable: true, writable: true });
  }
  const mc = navigator.mediaCapabilities;
  if (mc && typeof mc.decodingInfo === 'function') {
    const original = mc.decodingInfo.bind(mc);
    mc.decodingInfo = function decodingInfo(config) {
      const v = config && config.video;
      if (v && (!ok(String(v.contentType || '')) || tooBig(v.width, v.height, v.framerate) || slowAvc(String(v.contentType || '').toLowerCase(), v.width, v.height, v.framerate)))
        return Promise.resolve({ supported: false, smooth: false, powerEfficient: false, configuration: config });
      return original(config);
    };
  }
})();)JS";

std::string media_limit_script()
{
    // uint64_t, not unsigned long: 32-bit on the softfp engine, 64-bit on the aarch64 one, and the same "PIXELS@FPS"
    // values must parse to the same numbers in both.
    uint64_t pixels = 1280 * 720, fps = 30;  // software decoding
    bool vp9 = false;
    // LG's decoder (hardware video, WPEB_MEDIA_SOCKET) plays 4K60 H.264/HEVC/VP9.
    if (getenv("WPEB_MEDIA_SOCKET")) {
        // The TV's decoder plays 4096x2304@60; the limit is the decoder's, not the page's. (A lower limit is only an emergency
        // measure: WPE_BROWSER_DEBUG_MEDIA="PIXELS@FPS+vp9".)
        pixels = 3840 * 2160;
        fps = 60;
        vp9 = true;
    }
    if (const char *v = getenv("WPE_BROWSER_DEBUG_MEDIA")) {
        if (!strcmp(v, "off"))
            return {};
        char *end = nullptr;
        if (uint64_t p = strtoull(v, &end, 10)) pixels = p;
        if (end && *end == '@') fps = strtoull(end + 1, &end, 10);
        vp9 = strstr(v, "+vp9") != nullptr;
        log_msg("media limit: %" PRIu64 " pixels, %" PRIu64 " fps%s", pixels, fps, vp9 ? ", VP9 allowed" : "");
    }
    const char *dm = getenv("WPE_BROWSER_DEBUG_MEDIA");
    const bool no_avc_cap = dm && strstr(dm, "+nocap264") != nullptr;
    char cfg[160];
    snprintf(cfg, sizeof(cfg), "{pixels: %" PRIu64 ", fps: %" PRIu64 ", vp9: %s, noAvcCap: %s}", pixels, fps, vp9 ? "true" : "false", no_avc_cap ? "true" : "false");
    std::string script = kMediaLimitScript;
    const std::string marker = "__WPEB_MEDIA__";
    script.replace(script.find(marker), marker.size(), cfg);
    return script;
}

void add_media_limits(WebKitWebView *web_view)
{
    static const std::string source = media_limit_script();
    WebKitUserContentManager *ucm = webkit_web_view_get_user_content_manager(web_view);
    if (!ucm || source.empty())
        return;
    WebKitUserScript *script = webkit_user_script_new(source.c_str(), WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
        WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, nullptr, nullptr);
    webkit_user_content_manager_add_script(ucm, script);
    webkit_user_script_unref(script);
}

// WPE_BROWSER_DEBUG_CONSOLE=1: the page's console, uncaught errors, and its media/DRM calls go to the engine log, so
// a site that doesn't play can be diagnosed over ares-shell (the remote inspector would need a port open to the
// LAN). Page world, top frame and iframes; debug only, since the page can see window.webkit.messageHandlers.
constexpr const char *kConsoleScript = R"JS((function () {
  const h = window.webkit && window.webkit.messageHandlers && window.webkit.messageHandlers.wpebConsole;
  if (!h) return;
  const where = window === top ? '' : '[frame ' + location.host + '] ';
  const str = (v) => { try { return typeof v === 'string' ? v : v instanceof Error ? v.name + ': ' + v.message : JSON.stringify(v); } catch (e) { return String(v); } };
  const send = (kind, args) => { try { h.postMessage(where + kind + ' ' + Array.from(args, str).join(' ').slice(0, 800)); } catch (e) {} };
  for (const level of ['log', 'info', 'warn', 'error', 'debug']) {
    const original = console[level];
    console[level] = function () { send(level, arguments); return original.apply(this, arguments); };
  }
  addEventListener('error', (e) => {
    if (e.target && e.target !== window && e.target.tagName) {
      const t = e.target; send('resource-error', [t.tagName, t.src || t.href || '', t.error ? 'code=' + t.error.code + ' ' + t.error.message : '']);
    } else send('uncaught', [e.message, (e.filename || '') + ':' + e.lineno]);
  }, true);
  addEventListener('unhandledrejection', (e) => send('unhandled-rejection', [e.reason]));
  const n = navigator;
  if (n.requestMediaKeySystemAccess) {
    const original = n.requestMediaKeySystemAccess.bind(n);
    n.requestMediaKeySystemAccess = function (ks, cfg) {
      send('eme', ['requestMediaKeySystemAccess', ks, cfg]);
      return original(ks, cfg).then((a) => { send('eme', [ks, 'granted']); return a; }, (e) => { send('eme', [ks, 'rejected', e]); throw e; });
    };
  } else send('eme', ['navigator.requestMediaKeySystemAccess missing']);
  if (window.MediaKeySession) {
    const P = MediaKeySession.prototype;
    for (const m of ['generateRequest', 'update', 'load']) {
      const original = P[m];
      if (!original) continue;
      P[m] = function () {
        if (!this.__wpeb) { this.__wpeb = 1; this.addEventListener('keystatuseschange', () => { const s = []; this.keyStatuses.forEach((v) => s.push(v)); send('eme', ['key statuses', s.join(',')]); }); }
        send('eme', [m, arguments[0], arguments[1] && arguments[1].byteLength !== undefined ? arguments[1].byteLength + ' bytes' : '']);
        return original.apply(this, arguments).catch((e) => { send('eme', [m, 'failed', e]); throw e; });
      };
    }
  }
  if (window.MediaSource) {
    const original = MediaSource.prototype.addSourceBuffer;
    MediaSource.prototype.addSourceBuffer = function (type) {
      try { const sb = original.call(this, type); send('mse', ['addSourceBuffer', type]); return sb; } catch (e) { send('mse', ['addSourceBuffer', type, 'failed', e]); throw e; }
    };
  }
  const asked = new Set();
  const ask = (what, t, r) => { const k = what + t + r; if (asked.has(k)) return; asked.add(k); send('type', [what, t, '->', r]); };
  if (window.HTMLMediaElement) {
    const cpt = HTMLMediaElement.prototype.canPlayType;
    HTMLMediaElement.prototype.canPlayType = function (t) { const r = cpt.call(this, t); ask('canPlayType', t, JSON.stringify(r)); return r; };
  }
  for (const name of ['MediaSource', 'ManagedMediaSource', 'WebKitMediaSource']) {
    const MS = window[name];
    if (!MS || typeof MS.isTypeSupported !== 'function') continue;
    const its = MS.isTypeSupported.bind(MS);
    MS.isTypeSupported = function (t) { const r = its(t); ask(name + '.isTypeSupported', t, r); return r; };
  }
  if (window.HTMLMediaElement) {
    for (const ev of ['loadedmetadata', 'canplay', 'play', 'playing', 'pause', 'seeking', 'seeked', 'waiting', 'stalled', 'encrypted', 'waitingforkey', 'error', 'abort', 'emptied']) {
      addEventListener(ev, (e) => {
        const v = e.target; if (!(v instanceof HTMLMediaElement)) return;
        send('media', [ev, (v.currentSrc || v.src || '').slice(0, 120), 't=' + v.currentTime.toFixed(1), 'rs=' + v.readyState,
          v.error ? 'code=' + v.error.code + ' ' + v.error.message : '', e.initDataType || '']);
      }, true);
    }
  }
  const interesting = (u) => /\.(mpd|m3u8)|licen|widevine|drm|pallycon|token|stream|play/i.test(u);
  if (window.fetch) {
    const original = window.fetch;
    window.fetch = function (input, init) {
      const u = String(input && input.url || input);
      const p = original.apply(this, arguments);
      if (interesting(u)) p.then((r) => {
        send('fetch', [(init && init.method) || 'GET', u.slice(0, 200), r.status]);
        if (/json|mpegurl|dash/.test(r.headers.get('content-type') || '')) r.clone().text().then((b) => send('fetch-body', [u.slice(0, 80), b.slice(0, 1500)]), () => {});
      }, (e) => send('fetch', [u.slice(0, 200), 'failed', e]));
      return p;
    };
  }
  const open = XMLHttpRequest.prototype.open;
  XMLHttpRequest.prototype.open = function (method, url) {
    const u = String(url);
    if (interesting(u)) this.addEventListener('loadend', () => send('xhr', [method, u.slice(0, 200), this.status]));
    return open.apply(this, arguments);
  };
})();)JS";

void on_console_message(WebKitUserContentManager *, JSCValue *value, gpointer data)
{
    gchar *text = jsc_value_to_string(value);
    log_msg("tab %u page: %s", GPOINTER_TO_UINT(data), text);
    g_free(text);
}

// Hybrid video policy (build/video/hybrid/DESIGN.md): compat.js decides, per video element and before its player
// starts, whether it draws in the page (<video wren-software>, WebKit patch 0010: software decode, scrolls with the
// page) or on the TV's hardware video plane (the hole punch: no CPU, and the only way to show HD). This script
// evaluates compat.js with the policy switched on (it has normally already run in the web process extension; then this
// is the same file, run again, cheap) and reports its decisions to the engine log ("hybrid video:" lines). The policy
// itself stays in compat.js, so it can be changed without rebuilding the engine, and an engine that predates it keeps
// its old preview-only behaviour. WPE_BROWSER_DEBUG_HYBRID=0 turns this off.
constexpr const char *kHybridVideoLoader = R"JS((function () {
  // compat.js has already run in this page: it is the web process extension's script (engine/ubo/compat.js, the same
  // file wren:// serves). Its policy object is the switch; everything else is unchanged from the engine's point of view.
  if (window.__wrenVideoPolicy && window.__wrenHybridReady) { window.__wrenVideoPolicy.on = true; return; }
  // An engine without the extension (or an older build): read the file synchronously, at document start, so the policy
  // exists before the page's own scripts can start a video. compat.js is an IIFE that runs itself and ignores its
  // arguments (proven in build/video/lead-check/loader-probe.js: new Function(text)(env) hands it nothing), so the
  // policy is switched on through the object it publishes, not by passing it in.
  const xhr = new XMLHttpRequest();
  try { xhr.open('GET', __WPEB_COMPAT__, false); xhr.send(); } catch (e) { return; }
  if (xhr.status !== 200 && xhr.status !== 0) return;
  try { new Function(xhr.responseText)(); } catch (e) { return; }
  if (window.__wrenVideoPolicy) window.__wrenVideoPolicy.on = true;
})();)JS";

// The page's answers (compat.js -> wpebHybrid), one log line each: "hybrid video: <what>". WPE_BROWSER_DEBUG_CONSOLE
// also prints the element's own console, which is where a debugging session on a site starts.
void on_hybrid_message(WebKitUserContentManager *, JSCValue *value, gpointer data)
{
    gchar *text = jsc_value_to_string(value);
    if (text && *text)
        log_msg("tab %u hybrid video: %s", GPOINTER_TO_UINT(data), text);
    g_free(text);
}

// compat.js's emulated fullscreen (a hardware-plane video stretched over the page) tells the UI the same thing WebKit's own
// fullscreen does, so the toolbar goes away and the page gets the whole screen. "1" entered, "0" left.
void on_emulated_fullscreen(WebKitUserContentManager *, JSCValue *value, gpointer data)
{
    gchar *text = jsc_value_to_string(value);
    const uint32_t id = GPOINTER_TO_UINT(data);
    if (text && (text[0] == '1' || text[0] == '0') && !text[1]) {
        WpeFlag flag = {static_cast<uint32_t>(text[0] == '1')};
        log_msg("tab %u %s fullscreen (emulated)", id, flag.value ? "entered" : "left");
        send_msg(WPE_ENGINE_FULLSCREEN, id, &flag, sizeof(flag));
    }
    g_free(text);
}

std::string hybrid_video_script()
{
    if (getenv("WPE_BROWSER_DEBUG_HYBRID") && !strcmp(getenv("WPE_BROWSER_DEBUG_HYBRID"), "0"))
        return {};
    // compat.js is engine/ubo/compat.js, served by this engine as a wren:// resource (on_wren_scheme): the TV keeps the
    // installed package's compat.js and a new one is a rebuild, but the policy's numbers do not need the file to come
    // from the package.
    std::string script = kHybridVideoLoader;
    // The replacement keeps the quotes of the C literal: the page needs a JS string, and the C++ literal is the only
    // place the URI appears (build/video/lead-check/loader-probe.js checks the result parses).
    const std::string marker = "__WPEB_COMPAT__";
    script.replace(script.find(marker), marker.size(), "\"wren://adblock/compat.js\"");
    return script;
}

// WPE_BROWSER_DEBUG_SCRIPT=<file>: a script of the tester's (a path the engine can read) run in every top frame at document end, e.g. to
// click a page's play button or read a player's state while the page's console goes to the log (WPE_BROWSER_DEBUG_CONSOLE=1). Not a feature
// for users: there is no way to set it but the launch parameter wpebEnv of a developer's launch.
void add_debug_script(WebKitWebView *web_view)
{
    static const std::string source = [] {
        std::string text;
        if (const char *path = getenv("WPE_BROWSER_DEBUG_SCRIPT")) {
            gchar *data = nullptr;
            if (g_file_get_contents(path, &data, nullptr, nullptr)) { text = data; g_free(data); log_msg("debug script %s: %zu bytes", path, text.size()); }
            else log_msg("debug script %s: unreadable", path);
        }
        return text;
    }();
    WebKitUserContentManager *ucm = webkit_web_view_get_user_content_manager(web_view);
    if (!ucm || source.empty())
        return;
    WebKitUserScript *script = webkit_user_script_new(source.c_str(), WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
        WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_END, nullptr, nullptr);
    webkit_user_content_manager_add_script(ucm, script);
    webkit_user_script_unref(script);
}

void add_hybrid_video(WebKitWebView *web_view, uint32_t id)
{
    static const std::string source = hybrid_video_script();
    WebKitUserContentManager *ucm = webkit_web_view_get_user_content_manager(web_view);
    if (!ucm || source.empty())
        return;
    if (webkit_user_content_manager_register_script_message_handler(ucm, "wpebFullscreen", nullptr))
        g_signal_connect(ucm, "script-message-received::wpebFullscreen", G_CALLBACK(on_emulated_fullscreen), GUINT_TO_POINTER(id));
    // The decision log is a message handler every page could post to, and engine.log lives in the TV's RAM (/tmp):
    // only while debugging (WPE_BROWSER_DEBUG_HYBRID=log, or the console reporter is on anyway).
    if ((getenv("WPE_BROWSER_DEBUG_HYBRID") && !strcmp(getenv("WPE_BROWSER_DEBUG_HYBRID"), "log")) || getenv("WPE_BROWSER_DEBUG_CONSOLE")) {
        if (webkit_user_content_manager_register_script_message_handler(ucm, "wpebHybrid", nullptr))
            g_signal_connect(ucm, "script-message-received::wpebHybrid", G_CALLBACK(on_hybrid_message), GUINT_TO_POINTER(id));
    }
    WebKitUserScript *script = webkit_user_script_new(source.c_str(), WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
        WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, nullptr, nullptr);
    webkit_user_content_manager_add_script(ucm, script);
    webkit_user_script_unref(script);
}

void add_console_reporter(WebKitWebView *web_view, uint32_t id)
{
    static const bool enabled = getenv("WPE_BROWSER_DEBUG_CONSOLE") != nullptr;
    WebKitUserContentManager *ucm = webkit_web_view_get_user_content_manager(web_view);
    if (!enabled || !ucm || !webkit_user_content_manager_register_script_message_handler(ucm, "wpebConsole", nullptr))
        return;
    g_signal_connect(ucm, "script-message-received::wpebConsole", G_CALLBACK(on_console_message), GUINT_TO_POINTER(id));
    WebKitUserScript *script = webkit_user_script_new(kConsoleScript, WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
        WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, nullptr, nullptr);
    webkit_user_content_manager_add_script(ucm, script);
    webkit_user_script_unref(script);
}

// Site quirks: sites that take any AppleWebKit agent without "Chrome" for Safari and then serve FairPlay, which only
// Apple's Safari has. NetTV (webtv.nettv.com.np) tests AppleWebKit && !/Chrome|CriOS|Edg|OPR|Firefox|FxiOS/, sends
// X-Client-Browser: Safari and gets an fps.ezdrm.com license for HLS; Video.js then finds "no compatible source"
// (TV, 2026-11-13). Other browsers get Widevine, which we have (libocdm). A Chrome token in navigator.userAgent, on
// these sites only, takes that path. The HTTP User-Agent header is unchanged.
constexpr const char *kChromeAgentScript = R"JS((function () {
  const ua = navigator.userAgent;
  if (/Chrome\//.test(ua)) return;
  const spoofed = ua.replace(/ Version\/[\d.]+/, '').replace(/ Safari\//, ' Chrome/120.0.0.0 Safari/');
  Object.defineProperty(Navigator.prototype, 'userAgent', { get: () => spoofed, configurable: true });
})();)JS";
const char *const kChromeAgentSites[] = {"https://nettv.com.np/*", "https://*.nettv.com.np/*", nullptr};

void add_site_quirks(WebKitWebView *web_view)
{
    WebKitUserContentManager *ucm = webkit_web_view_get_user_content_manager(web_view);
    if (!ucm)
        return;
    WebKitUserScript *script = webkit_user_script_new(kChromeAgentScript, WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
        WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, kChromeAgentSites, nullptr);
    webkit_user_content_manager_add_script(ucm, script);
    webkit_user_script_unref(script);
}

void add_scroll_reporter(WebKitWebView *web_view, uint32_t id)
{
    add_site_quirks(web_view);
    add_media_limits(web_view);
    add_hybrid_video(web_view, id);
    add_debug_script(web_view);
    add_console_reporter(web_view, id); // after the limits, so it wraps them and logs what pages are told
    WebKitUserContentManager *ucm = webkit_web_view_get_user_content_manager(web_view);
    if (!ucm || !webkit_user_content_manager_register_script_message_handler(ucm, "wpebScroll", kScrollWorld))
        return;
    g_signal_connect(ucm, "script-message-received::wpebScroll", G_CALLBACK(on_scroll_message), GUINT_TO_POINTER(id));
    if (webkit_user_content_manager_register_script_message_handler(ucm, "wpebIcon", kScrollWorld))
        g_signal_connect(ucm, "script-message-received::wpebIcon", G_CALLBACK(on_icon_message), GUINT_TO_POINTER(id));
    if (webkit_user_content_manager_register_script_message_handler(ucm, "wpebEdge", kScrollWorld))
        g_signal_connect(ucm, "script-message-received::wpebEdge", G_CALLBACK(on_edge_message), GUINT_TO_POINTER(id));
    WebKitUserScript *script = webkit_user_script_new_for_world(kScrollScript, WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
        WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_END, kScrollWorld, nullptr, nullptr);
    webkit_user_content_manager_add_script(ucm, script);
    webkit_user_script_unref(script);
    if (webkit_user_content_manager_register_script_message_handler(ucm, "wpebPerf", kScrollWorld)) {
        g_signal_connect(ucm, "script-message-received::wpebPerf", G_CALLBACK(on_perf_message), GUINT_TO_POINTER(id));
        script = webkit_user_script_new_for_world(kPerfScript, WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
            WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, kScrollWorld, nullptr, nullptr);
        webkit_user_content_manager_add_script(ucm, script);
        webkit_user_script_unref(script);
    }
    // WPE_BROWSER_DEBUG_MESSAGES=1: postMessage traffic to the top frame (e.g. a Cloudflare challenge frame's status
    // and error codes) goes to the engine log. Listened to from Wren's isolated world, so the page's own functions
    // stay untouched; WPE_BROWSER_DEBUG_CONSOLE wraps them and bot checks notice that.
    static const bool messages = getenv("WPE_BROWSER_DEBUG_MESSAGES") != nullptr;
    if (messages && webkit_user_content_manager_register_script_message_handler(ucm, "wpebMessage", kScrollWorld)) {
        g_signal_connect(ucm, "script-message-received::wpebMessage", G_CALLBACK(on_console_message), GUINT_TO_POINTER(id));
        script = webkit_user_script_new_for_world(R"JS(addEventListener('message', function (e) {
  var d; try { d = typeof e.data === 'string' ? e.data : JSON.stringify(e.data); } catch (x) { d = String(e.data); }
  window.webkit.messageHandlers.wpebMessage.postMessage('message from ' + e.origin + ': ' + String(d).slice(0, 700));
}, true);)JS", WEBKIT_USER_CONTENT_INJECT_TOP_FRAME, WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, kScrollWorld, nullptr, nullptr);
        webkit_user_content_manager_add_script(ucm, script);
        webkit_user_script_unref(script);
    }
}

void on_history_changed(WebKitBackForwardList *, WebKitBackForwardListItem *, gpointer, gpointer web_view)
{
    auto *view = WEBKIT_WEB_VIEW(web_view);
    send_history(tab_of_web_view(view), view);
}

void tab_open(uint32_t id, const std::string &uri, WebKitWebView *opener = nullptr);
void ubo_attach(WebKitUserContentManager *ucm, WebKitWebView *view);

// window.close() from a page: the UI closes the tab (and shows its opener).
// WPE_BROWSER_DEBUG_FORGET_COOKIES="example.com,other.org": removes those sites' cookies (only theirs) before the
// first page loads, e.g. to see a Cloudflare challenge again without its clearance cookie. Waits until done.
void forget_cookies(const char *sites)
{
    struct Job {
        GMainLoop *loop;
        gchar **sites;
    } job { g_main_loop_new(nullptr, FALSE), g_strsplit(sites, ",", -1) };
    WebKitWebsiteDataManager *manager = webkit_network_session_get_website_data_manager(g.session);
    webkit_website_data_manager_fetch(manager, WEBKIT_WEBSITE_DATA_COOKIES, nullptr, [](GObject *source, GAsyncResult *result, gpointer data) {
        auto *job = static_cast<Job *>(data);
        auto *manager = WEBKIT_WEBSITE_DATA_MANAGER(source);
        GList *records = webkit_website_data_manager_fetch_finish(manager, result, nullptr), *matched = nullptr;
        for (GList *r = records; r; r = r->next) {
            const char *name = webkit_website_data_get_name(static_cast<WebKitWebsiteData *>(r->data));
            for (gchar **s = job->sites; name && *s; ++s)
                if (**s && g_str_has_suffix(name, *s)) {
                    log_msg("forgetting the cookies of %s", name);
                    matched = g_list_prepend(matched, r->data);
                    break;
                }
        }
        if (!matched) {
            log_msg("no cookies to forget");
            g_list_free_full(records, reinterpret_cast<GDestroyNotify>(webkit_website_data_unref));
            g_main_loop_quit(job->loop);
            return;
        }
        webkit_website_data_manager_remove(manager, WEBKIT_WEBSITE_DATA_COOKIES, matched, nullptr, [](GObject *source, GAsyncResult *result, gpointer data) {
            webkit_website_data_manager_remove_finish(WEBKIT_WEBSITE_DATA_MANAGER(source), result, nullptr);
            g_main_loop_quit(static_cast<Job *>(data)->loop);
        }, job);
        g_list_free(matched);
        g_list_free_full(records, reinterpret_cast<GDestroyNotify>(webkit_website_data_unref));
    }, &job);
    g_main_loop_run(job.loop);
    g_main_loop_unref(job.loop);
    g_strfreev(job.sites);
}

// WPE_BROWSER_DEBUG_REQUESTS=1: every request of a page and its frames, with the response status or the error, goes
// to the engine log (e.g. how far a Cloudflare challenge frame gets). Nothing in the page changes.
std::string short_uri(const char *uri)
{
    std::string u = uri ? uri : "";
    return u.size() > 160 ? u.substr(0, 160) + "..." : u;
}

void on_resource_finished(WebKitWebResource *resource, gpointer)
{
    WebKitURIResponse *response = webkit_web_resource_get_response(resource);
    log_msg("request done %u %s", response ? webkit_uri_response_get_status_code(response) : 0,
        short_uri(webkit_web_resource_get_uri(resource)).c_str());
}

void on_resource_failed(WebKitWebResource *resource, GError *error, gpointer)
{
    log_msg("request failed %s: %s", short_uri(webkit_web_resource_get_uri(resource)).c_str(), error ? error->message : "?");
}

void on_resource_load_started(WebKitWebView *, WebKitWebResource *resource, WebKitURIRequest *request, gpointer)
{
    const char *method = webkit_uri_request_get_http_method(request);
    log_msg("request %s %s", method ? method : "GET", short_uri(webkit_uri_request_get_uri(request)).c_str());
    g_signal_connect(resource, "finished", G_CALLBACK(on_resource_finished), nullptr);
    g_signal_connect(resource, "failed", G_CALLBACK(on_resource_failed), nullptr);
}

void on_close(WebKitWebView *view, gpointer)
{
    if (const uint32_t id = tab_of_web_view(view))
        send_msg(WPE_ENGINE_TAB_CLOSE, id, nullptr, 0);
}

// EME: navigator.requestMediaKeySystemAccess asks the UI process first, and WebKit's default handler denies it, so
// every key system (ClearKey too) failed with NotSupportedError (TV, 2026-11-13). Allowed for every page, as desktop
// browsers do for Widevine. Other requests go to the default handler (denied).
gboolean on_permission_request(WebKitWebView *view, WebKitPermissionRequest *request, gpointer)
{
    if (!WEBKIT_IS_MEDIA_KEY_SYSTEM_PERMISSION_REQUEST(request))
        return FALSE;
    log_msg("tab %u: media key system %s allowed", tab_of_web_view(view),
        webkit_media_key_system_permission_get_name(WEBKIT_MEDIA_KEY_SYSTEM_PERMISSION_REQUEST(request)));
    webkit_permission_request_allow(request);
    return TRUE;
}

// window.open and target=_blank: a real new window, as a new tab. Sign-in popups (Google, Apple, ...) need it: they
// talk to window.opener and close themselves. WebKit loads the request into the view returned here.
bool popup_create_blocked(WebKitWebView *view, WebKitNavigationAction *action, std::string *why);
bool ubo_popup_third_party(WebKitWebView *opener, const char *target);

WebKitWebView *on_create(WebKitWebView *view, WebKitNavigationAction *action, gpointer)
{
    static uint32_t next = WPE_ENGINE_TAB_ID_BASE;
    const uint32_t opener = tab_of_web_view(view);
    std::string why;
    if (popup_create_blocked(view, action, &why)) {
        const char *target = webkit_uri_request_get_uri(webkit_navigation_action_get_request(action));
        log_msg("tab %u: window blocked (%s): %.200s", opener, why.c_str(), target && *target ? target : "about:blank");
        return nullptr;  // window.open gives the page null, as for any blocked pop-up
    }
    const uint32_t id = next++;
    tab_open(id, {}, view);
    Tab *tab = tab_by_id(id);
    if (!tab)
        return nullptr;
    tab->opener = opener;
    tab->born_us = g_get_monotonic_time();
    tab->pop_under_candidate = webkit_navigation_action_get_navigation_type(action) != WEBKIT_NAVIGATION_TYPE_LINK_CLICKED
        && ubo_popup_third_party(view, webkit_uri_request_get_uri(webkit_navigation_action_get_request(action)));
    WpeTabCreated c { opener };
    send_msg(WPE_ENGINE_TAB_CREATED, id, &c, sizeof(c));
    log_msg("tab %u opened by tab %u (window.open)", id, opener);
    return tab->web_view;  // still ours (WebKit only takes its page)
}

// ---- direct presentation (direct.cpp) -------------------------------------------------------------------

void send_present(uint32_t tab, int32_t status, uint32_t width, uint32_t height)
{
    WpePresent p { g.direct ? WPE_PRESENT_DIRECT : WPE_PRESENT_SHM, status, width, height };
    send_msg(WPE_ENGINE_PRESENT, tab, &p, sizeof(p));
}

// ---- memory guard ----------------------------------------------------------------------------------------------
// WebKit's pressure handling works per process on its own limit; what freezes this TV is the sum: all web processes, LG's apps and
// the swap together (2026-11-20: swap 599/599 MB, 56 MB free, load 15, several tabs open). So the engine watches the system: when
// available memory AND free swap are both nearly gone, the web processes of tabs that are not shown are ended first (their page is
// reloaded when the tab is shown again: the UI treats it like a crash), then, if it goes on, the shown tab's, which the UI reloads
// once (a second time soon after it tells the user). WPE_BROWSER_DEBUG_MEM_GUARD=0 turns it off; _AVAIL_MB / _SWAP_PCT set the lines.
std::string env_or(const char *name, const char *fallback);

struct MemInfo { long avail_kb = -1, swap_total_kb = 0, swap_free_kb = 0; };

bool read_meminfo(MemInfo &m)
{
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f)
        return false;
    char line[128];
    while (fgets(line, sizeof line, f)) {
        long v;
        if (sscanf(line, "MemAvailable: %ld", &v) == 1) m.avail_kb = v;
        else if (sscanf(line, "SwapTotal: %ld", &v) == 1) m.swap_total_kb = v;
        else if (sscanf(line, "SwapFree: %ld", &v) == 1) m.swap_free_kb = v;
    }
    fclose(f);
    return m.avail_kb >= 0;
}

// Resident + swapped memory (kB) of the web process (a child of this process), 0 if none is found. All tabs live in it.
long web_process_footprint_kb()
{
    long best = 0;
    const pid_t self = getpid();
    GDir *dir = g_dir_open("/proc", 0, nullptr);
    if (!dir)
        return 0;
    while (const gchar *name = g_dir_read_name(dir)) {
        if (!g_ascii_isdigit(name[0]))
            continue;
        gchar *stat_path = g_strdup_printf("/proc/%s/stat", name);
        gchar *stat = nullptr;
        if (g_file_get_contents(stat_path, &stat, nullptr, nullptr)) {
            // pid (comm) state ppid ...: comm may hold spaces, so find the last ')'
            const char *rp = strrchr(stat, ')');
            int ppid = 0;
            char state;
            if (rp && sscanf(rp + 1, " %c %d", &state, &ppid) == 2 && ppid == self) {
                gchar *exe = g_strdup_printf("/proc/%s/exe", name);
                gchar *target = g_file_read_link(exe, nullptr);
                if (target && strstr(target, "WPEWebProcess")) {
                    gchar *status_path = g_strdup_printf("/proc/%s/status", name);
                    gchar *status = nullptr;
                    if (g_file_get_contents(status_path, &status, nullptr, nullptr)) {
                        long rss = 0, swap = 0;
                        for (gchar **l = g_strsplit(status, "\n", 0), **it = l; *it; ++it) {
                            long v;
                            if (sscanf(*it, "VmRSS: %ld", &v) == 1) rss = v;
                            else if (sscanf(*it, "VmSwap: %ld", &v) == 1) swap = v;
                            if (!*(it + 1)) { g_strfreev(l); break; }
                        }
                        best = std::max(best, rss + swap);
                        g_free(status);
                    }
                    g_free(status_path);
                }
                g_free(target);
                g_free(exe);
            }
        }
        g_free(stat);
        g_free(stat_path);
    }
    g_dir_close(dir);
    return best;
}

gboolean memory_guard_tick(gpointer)
{
    static const long avail_floor_kb = atol(env_or("WPE_BROWSER_DEBUG_MEM_GUARD_AVAIL_MB", "110").c_str()) * 1024;
    static const long swap_pct = atol(env_or("WPE_BROWSER_DEBUG_MEM_GUARD_SWAP_PCT", "12").c_str());
    static gint64 last_kill_us = 0;
    MemInfo m;
    if (!read_meminfo(m))
        return G_SOURCE_CONTINUE;
    const bool swap_gone = m.swap_total_kb <= 0 || m.swap_free_kb * 100 < m.swap_total_kb * swap_pct;
    // The web process (all tabs) is also watched on its own: at 70% of WebKit's own limit, background tabs go before WebKit's kill threshold
    // ends every tab at once (2026-11-20: all five tabs were reset together at the threshold while the TV was not yet out of memory).
    static const long limit_kb = atol(env_or("WPE_BROWSER_DEBUG_MEM_LIMIT_MB", "420").c_str()) * 1024;
    const bool process_big = limit_kb > 0 && g.tabs.size() > 1 && web_process_footprint_kb() * 10 > limit_kb * 7;
    if (!process_big && (m.avail_kb >= avail_floor_kb || !swap_gone))
        return G_SOURCE_CONTINUE;
    const gint64 now = g_get_monotonic_time();
    if (now - last_kill_us < 6 * G_USEC_PER_SEC)  // give the last kill time to free its memory
        return G_SOURCE_CONTINUE;
    // The pages of all tabs live in ONE web process (no process swap, no COOP): ending it ends every tab, so while there are background
    // tabs the UI is asked to close the least recently shown one (WPE_ENGINE_DISCARD_REQUEST); only with none left is the shown
    // tab's process ended (the UI reloads it).
    last_kill_us = now;
    if (g.tabs.size() > 1) {
        log_msg("memory guard: %ld MB available, swap %ld of %ld MB free: asking the UI to discard a background tab", m.avail_kb / 1024,
            m.swap_free_kb / 1024, m.swap_total_kb / 1024);
        send_msg(WPE_ENGINE_DISCARD_REQUEST, 0, nullptr, 0);
        return G_SOURCE_CONTINUE;
    }
    Tab *victim = active_tab();
    if (!victim || !victim->web_view)
        return G_SOURCE_CONTINUE;
    victim->memory_kill = true;
    log_msg("memory guard: %ld MB available, swap %ld of %ld MB free: ending tab %u's web process (the only tab)", m.avail_kb / 1024,
        m.swap_free_kb / 1024, m.swap_total_kb / 1024, victim->id);
    webkit_web_view_terminate_web_process(victim->web_view);
    return G_SOURCE_CONTINUE;
}

void start_memory_guard()
{
    if (!strcmp(env_or("WPE_BROWSER_DEBUG_MEM_GUARD", "1").c_str(), "0"))
        return;
    g_timeout_add_seconds(3, memory_guard_tick, nullptr);
}

void show_pending_tab();

void on_direct_status(uint32_t tab, int32_t status, uint32_t width, uint32_t height)
{
    send_present(tab, status, width, height);
    if (g.show_pending && tab == g.detach_awaited && direct::is_detached_status(status))
        show_pending_tab();
}

void on_direct_ime(uint32_t tab, bool focused, uint32_t purpose, int x, int y, int width, int height)
{
    WpeIme m { focused ? 1u : 0u, purpose, x, y, width, height };
    send_msg(WPE_ENGINE_IME, tab, &m, sizeof(m));
}

void set_visible(Tab &tab, bool visible)
{
    if (tab.direct)
        direct::set_shown(tab.direct, visible);
    else
        wpe_view_set_visible(tab.view, visible);
}

void set_focused(Tab &tab, bool focused)
{
    if (tab.direct)
        direct::set_focused(tab.direct, focused);
    else if (focused)
        wpe_view_focus_in(tab.view);
    else
        wpe_view_focus_out(tab.view);
}

// ---- tabs ------------------------------------------------------------------------------------------------

// `opener`: a popup's opener (window.open): WebKit requires the new view to be related to it, and the page gets its
// window.opener. Otherwise related to the first tab.
void tab_open(uint32_t id, const std::string &uri, WebKitWebView *opener)
{
    if (!id || tab_by_id(id))
        return;
    // Related views share the first view's web process: one process for every tab saves a lot of memory
    // on a TV (a page crash then takes all tabs down, and the UI reloads them).
    WebKitWebView *related = opener;
    for (auto &[other, tab] : g.tabs) {
        if (related)
            break;
        related = tab.web_view;
    }
    Tab tab { id };
    WebKitWebView *web_view = nullptr;
    // Its own content manager: a related view would otherwise share the first tab's (scroll reports per tab).
    WebKitUserContentManager *ucm = webkit_user_content_manager_new();
    // Autoplay with sound is allowed in every tab: this is a TV, the page was opened on purpose, and a player whose play() is
    // rejected for lack of a gesture tears itself down, so its Play button never works afterwards (TV, 2026-11-20).
    // WPE_BROWSER_DEBUG_AUTOPLAY=policy: muted videos only, except in tabs opened on YouTube. Set once per tab (construct-only).
    const bool restrict_autoplay = g_strcmp0(getenv("WPE_BROWSER_DEBUG_AUTOPLAY"), "policy") == 0
        && uri.find("youtube.com") == std::string::npos && uri.find("youtu.be") == std::string::npos;
    WebKitWebsitePolicies *policies = webkit_website_policies_new_with_policies("autoplay",
        restrict_autoplay ? WEBKIT_AUTOPLAY_ALLOW_WITHOUT_SOUND : WEBKIT_AUTOPLAY_ALLOW, nullptr);
    if (g.direct) {
        WebKitWebViewBackend *backend = direct::create_backend(id, g.width, g.height, &tab.direct);
        if (backend) {
            web_view = related
                ? WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "backend", backend, "related-view", related,
                    "settings", g.settings, "user-content-manager", ucm, "website-policies", policies, nullptr))
                : WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "backend", backend, "web-context", g.context, "network-session", g.session,
                    "settings", g.settings, "user-content-manager", ucm, "website-policies", policies, nullptr));
            tab.ime = direct::create_ime(id, on_direct_ime);
            webkit_web_view_set_input_method_context(web_view, tab.ime);
            g_object_unref(tab.ime); // the web view keeps it
        }
    } else {
        web_view = related
            // The display must be passed to related views too: without it WebKit would use
            // wpe_display_get_default(), which creates a display of another platform instead of ours.
            ? WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "related-view", related, "display", g.display,
                "settings", g.settings, "user-content-manager", ucm, "website-policies", policies, nullptr))
            : WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "display", g.display, "web-context", g.context, "network-session", g.session,
                "settings", g.settings, "user-content-manager", ucm, "website-policies", policies, nullptr));
        tab.view = webkit_web_view_get_wpe_view(web_view);
    }
    g_object_unref(ucm); // the view keeps its own reference
    g_object_unref(policies);
    if (!web_view || (!tab.view && !tab.direct)) {
        log_msg("WebKit did not create a view for tab %u", id);
        if (web_view)
            g_object_unref(web_view);
        send_error(id, WPE_ERROR_FATAL, 0, "could not create a view");
        return;
    }
    tab.web_view = web_view;
    g.tabs[id] = tab;
    if (getenv("WPEB_TRANSPARENT_PAGE")) {
        // Only for SHM hardware video (the UI sets WPEB_TRANSPARENT_PAGE): a transparent default background also
        // makes every page without a background of its own transparent, which would show black or see-through
        // until the UI composites the page over white outside the video rectangle.
        // Transparent page background: WebKit punches a transparent hole where a hardware video plays
        // (engine/gst/wpebmediasink.c), and the UI lets the TV's video plane show through it. With the default
        // opaque background the hole would be white. A non-opaque colour also clears the opaque rectangles
        // (wpe_view_set_opaque_rectangles), which is what makes WebKit clear the frame to transparent.
        WebKitColor transparent = { 0, 0, 0, 0 };
        webkit_web_view_set_background_color(web_view, &transparent);
    }
    ubo_attach(webkit_web_view_get_user_content_manager(web_view), web_view);

    g_signal_connect(web_view, "notify::uri", G_CALLBACK(on_uri), nullptr);
    g_signal_connect(web_view, "notify::title", G_CALLBACK(on_title), nullptr);
    g_signal_connect(web_view, "notify::estimated-load-progress", G_CALLBACK(on_progress), nullptr);
    g_signal_connect(web_view, "load-changed", G_CALLBACK(on_load_changed), nullptr);
    g_signal_connect(web_view, "load-failed", G_CALLBACK(on_load_failed), nullptr);
    g_signal_connect(web_view, "load-failed-with-tls-errors", G_CALLBACK(on_tls_error), nullptr);
    g_signal_connect(web_view, "web-process-terminated", G_CALLBACK(on_web_process_terminated), nullptr);
    g_signal_connect(web_view, "create", G_CALLBACK(on_create), nullptr);
    g_signal_connect(web_view, "close", G_CALLBACK(on_close), nullptr);
    g_signal_connect(web_view, "permission-request", G_CALLBACK(on_permission_request), nullptr);
    if (getenv("WPE_BROWSER_DEBUG_REQUESTS"))
        g_signal_connect(web_view, "resource-load-started", G_CALLBACK(on_resource_load_started), nullptr);
    g_signal_connect(web_view, "decide-policy", G_CALLBACK(on_decide_policy), nullptr);
    g_signal_connect(web_view, "enter-fullscreen", G_CALLBACK(on_fullscreen_changed), GUINT_TO_POINTER(1));
    g_signal_connect(web_view, "leave-fullscreen", G_CALLBACK(on_fullscreen_changed), GUINT_TO_POINTER(0));
    g_signal_connect(webkit_web_view_get_back_forward_list(web_view), "changed", G_CALLBACK(on_history_changed), web_view);
    add_scroll_reporter(web_view, id);

    Tab &t = g.tabs[id];
    resize_view(t);
    webkit_web_view_set_zoom_level(web_view, g.zoom);
    const bool active = id == g.active;
    set_visible(t, active);
    if (active)
        set_focused(t, true);
    if (!uri.empty())
        webkit_web_view_load_uri(web_view, uri.c_str());
    log_msg("tab %u opened (%zu open)", id, g.tabs.size());
}

void tab_close(uint32_t id)
{
    auto it = g.tabs.find(id);
    if (it == g.tabs.end())
        return;
    // Frames of this view still in flight keep their own references (released on FRAME_DONE).
    g_signal_handlers_disconnect_by_data(webkit_web_view_get_back_forward_list(it->second.web_view), it->second.web_view);
    webkit_web_view_try_close(it->second.web_view);
    g_object_unref(it->second.web_view);
    g.tabs.erase(it);
    if (g.active == id)
        g.active = 0;
    log_msg("tab %u closed (%zu open)", id, g.tabs.size());
}

constexpr guint kShowAfterDetachMs = 300;

void show_tab_now(Tab &tab)
{
    if (tab.resize_pending) {
        tab.resize_pending = false;
        resize_view(tab);
    }
    set_visible(tab, true);
    set_focused(tab, true);
}

void show_pending_tab()
{
    if (g.show_timer) {
        g_source_remove(g.show_timer);
        g.show_timer = 0;
    }
    const uint32_t id = std::exchange(g.show_pending, 0u);
    g.detach_awaited = 0;
    if (id != 0 && id == g.active)
        if (Tab *tab = active_tab())
            show_tab_now(*tab);
}

void tab_activate(uint32_t id)
{
    if (g.active == id)
        return;
    uint32_t awaited = 0;
    if (Tab *old = active_tab()) {
        if (old->direct && direct::shown(old->direct))
            awaited = old->id;
        set_focused(*old, false);
        set_visible(*old, false);
        // A hidden page's video must not go on: the hardware plane and decoder are the shown tab's (the UI releases them), and a
        // page that plays on behind another tab only costs memory and bandwidth. The page's own pause handlers see a normal pause.
        static const char kPause[] = "document.querySelectorAll('video,audio').forEach(function(m){try{m.pause()}catch(e){}})";
        webkit_web_view_evaluate_javascript(old->web_view, kPause, sizeof(kPause) - 1, nullptr, nullptr, nullptr, nullptr, nullptr);
    }
    g.active = id;
    Tab *tab = active_tab();
    if (tab == nullptr)
        return;
    send_history(id, tab->web_view);
    if (awaited != 0 && tab->direct) {
        // The old page detaches on its own thread when it isn't drawing (engine/webos-backend: a frame drawn after
        // its detach maps it as a window of its own and webOS minimises the browser, TV 2026-11-15).
        if (g.show_timer)
            g_source_remove(g.show_timer);
        g.show_pending = id;
        g.detach_awaited = awaited;
        g.show_timer = g_timeout_add(kShowAfterDetachMs, [](gpointer) -> gboolean {
            g.show_timer = 0;
            show_pending_tab();
            return G_SOURCE_REMOVE;
        }, nullptr);
        return;
    }
    show_tab_now(*tab);
}

// ---- website data and zoom ---------------------------------------------------------------------------------

void clear_data(const WpeClearData &c)
{
    unsigned types = 0;
    if (c.what & WPE_CLEAR_COOKIES_AND_SITE_DATA)
        types |= WEBKIT_WEBSITE_DATA_COOKIES | WEBKIT_WEBSITE_DATA_SESSION_STORAGE | WEBKIT_WEBSITE_DATA_LOCAL_STORAGE
            | WEBKIT_WEBSITE_DATA_INDEXEDDB_DATABASES | WEBKIT_WEBSITE_DATA_SERVICE_WORKER_REGISTRATIONS
            | WEBKIT_WEBSITE_DATA_DOM_CACHE | WEBKIT_WEBSITE_DATA_HSTS_CACHE | WEBKIT_WEBSITE_DATA_OFFLINE_APPLICATION_CACHE;
    if (c.what & WPE_CLEAR_CACHE)
        types |= WEBKIT_WEBSITE_DATA_MEMORY_CACHE | WEBKIT_WEBSITE_DATA_DISK_CACHE;
    if (!types) {
        WpeDataCleared done { c.what, 1 };
        send_msg(WPE_ENGINE_DATA_CLEARED, 0, &done, sizeof(done));
        return;
    }
    auto *what = new uint32_t(c.what);
    // timespan: GTimeSpan in microseconds; 0 = everything.
    webkit_website_data_manager_clear(webkit_network_session_get_website_data_manager(g.session),
        static_cast<WebKitWebsiteDataTypes>(types), static_cast<GTimeSpan>(c.seconds) * G_TIME_SPAN_SECOND, nullptr,
        [](GObject *source, GAsyncResult *result, gpointer data) {
            std::unique_ptr<uint32_t> what(static_cast<uint32_t *>(data));
            GError *error = nullptr;
            const gboolean ok = webkit_website_data_manager_clear_finish(WEBKIT_WEBSITE_DATA_MANAGER(source), result, &error);
            if (!ok) {
                log_msg("clearing website data failed: %s", error ? error->message : "?");
                g_clear_error(&error);
            }
            WpeDataCleared done { *what, ok ? 1u : 0u };
            send_msg(WPE_ENGINE_DATA_CLEARED, 0, &done, sizeof(done));
        },
        what);
}

// The uBO-grade ad blocker (engine/ubo, docs/ad-blocking.md): a web process extension with uBlock Origin's lists.
// This 0.3.3-based build has no ad-blocker setting in the UI, so the engine finds the blocker next to itself
// (engine/ubo/engine.dat, engine/lib/web-extensions/libwren-ubo.so) and offers its settings at wren://adblock:
// on/off, the extra filter lists (engine/ubo/lists/catalog.ini) and the sites where ads are allowed. They are kept in
// $WPE_BROWSER_DATA_DIR/ubo-settings.ini and sent to the web process when they change ("wren-ubo:state" (basass)).
// "My filters", the user's own rules (uBO syntax, e.g. site-specific fixes with Wren's generic scriptlets), are
// kept as text in $WPE_BROWSER_DATA_DIR/ubo-my-filters.txt and parsed in the web process: never part of the package.
// WREN_UBO=0 in the environment turns the blocker off altogether. It never runs on YouTube (engine/ubo/extension.c).
struct UboList {
    std::string name, title, about;
    bool on_by_default = false;
    guint64 size = 0;
};

struct Ubo {
    std::string dir;              // engine/ubo (empty: no blocker in this package)
    std::string settings_path;    // $WPE_BROWSER_DATA_DIR/ubo-settings.ini
    std::string my_filters_path;  // $WPE_BROWSER_DATA_DIR/ubo-my-filters.txt
    std::vector<UboList> catalog; // extra lists on offer
    bool on = true;
    std::vector<std::string> allowed, lists;
    std::string my_filters;       // the user's own rules, filter text
    popup::Rules popups;          // $popup / $popunder rules: ubo/popups.txt and the user's own (popup_filter.h)
};

constexpr size_t UBO_MY_FILTERS_MAX = 256 * 1024;
Ubo ubo;

// The pop-up rules: the lists' (ubo/popups.txt) and the user's own lines of the same kind.
void ubo_reload_popups()
{
    ubo.popups = popup::Rules();
    gchar *text = nullptr;
    gsize len = 0;
    if (!ubo.dir.empty() && g_file_get_contents((ubo.dir + "/popups.txt").c_str(), &text, &len, nullptr))
        ubo.popups.add_text(std::string(text, len));
    g_free(text);
    ubo.popups.add_text(ubo.my_filters);
    log_msg("ad blocker: %zu pop-up rule(s)", ubo.popups.size());
}

std::vector<std::string> ubo_split(const gchar *const *v)
{
    std::vector<std::string> out;
    for (; v && *v; ++v) {
        std::string s = *v;
        for (char &c : s)
            c = g_ascii_tolower(c);
        while (!s.empty() && (s.back() == ' ' || s.back() == '/'))
            s.pop_back();
        while (!s.empty() && s.front() == ' ')
            s.erase(0, 1);
        if (s.rfind("https://", 0) == 0) s.erase(0, 8);
        if (s.rfind("http://", 0) == 0) s.erase(0, 7);
        if (s.rfind("www.", 0) == 0) s.erase(0, 4);
        if (!s.empty() && std::find(out.begin(), out.end(), s) == out.end())
            out.push_back(s);
    }
    return out;
}

void ubo_load_settings()
{
    GKeyFile *catalog = g_key_file_new();
    if (g_key_file_load_from_file(catalog, (ubo.dir + "/lists/catalog.ini").c_str(), G_KEY_FILE_NONE, nullptr)) {
        gchar **groups = g_key_file_get_groups(catalog, nullptr);
        for (gchar **gr = groups; gr && *gr; ++gr) {
            UboList l;
            l.name = *gr;
            gchar *t = g_key_file_get_string(catalog, *gr, "title", nullptr), *a = g_key_file_get_string(catalog, *gr, "about", nullptr);
            l.title = t ? t : l.name;
            l.about = a ? a : "";
            g_free(t);
            g_free(a);
            l.on_by_default = g_key_file_get_boolean(catalog, *gr, "default", nullptr);
            l.size = g_key_file_get_uint64(catalog, *gr, "size", nullptr);
            ubo.catalog.push_back(l);
        }
        g_strfreev(groups);
    }
    g_key_file_unref(catalog);

    for (const UboList &l : ubo.catalog)
        if (l.on_by_default)
            ubo.lists.push_back(l.name);
    GKeyFile *kf = g_key_file_new();
    if (g_key_file_load_from_file(kf, ubo.settings_path.c_str(), G_KEY_FILE_NONE, nullptr)) {
        GError *e = nullptr;
        const gboolean on = g_key_file_get_boolean(kf, "ubo", "enabled", &e);
        if (!e)
            ubo.on = on;
        g_clear_error(&e);
        gchar **v = g_key_file_get_string_list(kf, "ubo", "allowed", nullptr, nullptr);
        ubo.allowed = ubo_split(v);
        g_strfreev(v);
        if (g_key_file_has_key(kf, "ubo", "lists", nullptr)) {
            v = g_key_file_get_string_list(kf, "ubo", "lists", nullptr, nullptr);
            ubo.lists.clear();
            for (const std::string &n : ubo_split(v))
                for (const UboList &l : ubo.catalog)
                    if (l.name == n)
                        ubo.lists.push_back(n);
            g_strfreev(v);
        }
    }
    g_key_file_unref(kf);

    gchar *mine = nullptr;
    gsize mine_len = 0;
    if (g_file_get_contents(ubo.my_filters_path.c_str(), &mine, &mine_len, nullptr))
        ubo.my_filters.assign(mine, std::min(mine_len, UBO_MY_FILTERS_MAX));
    g_free(mine);
}

void ubo_save_my_filters()
{
    GError *e = nullptr;
    if (!g_file_set_contents(ubo.my_filters_path.c_str(), ubo.my_filters.data(), ubo.my_filters.size(), &e)) {
        log_msg("ad blocker: my filters not saved: %s", e->message);
        g_clear_error(&e);
    }
}

void ubo_save_settings()
{
    GKeyFile *kf = g_key_file_new();
    g_key_file_set_comment(kf, nullptr, nullptr, " Wren ad blocker settings (wren://adblock)", nullptr);
    g_key_file_set_boolean(kf, "ubo", "enabled", ubo.on);
    std::vector<const gchar *> a, l;
    for (const std::string &s : ubo.allowed) a.push_back(s.c_str());
    for (const std::string &s : ubo.lists) l.push_back(s.c_str());
    g_key_file_set_string_list(kf, "ubo", "allowed", a.data(), a.size());
    g_key_file_set_string_list(kf, "ubo", "lists", l.data(), l.size());
    GError *e = nullptr;
    if (!g_key_file_save_to_file(kf, ubo.settings_path.c_str(), &e)) {
        log_msg("ad blocker: settings not saved: %s", e->message);
        g_clear_error(&e);
    }
    g_key_file_unref(kf);
}

GVariant *ubo_strv(const std::vector<std::string> &v)
{
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE("as"));
    for (const std::string &s : v)
        g_variant_builder_add(&b, "s", s.c_str());
    return g_variant_builder_end(&b);
}

void on_initialize_web_process_extensions(WebKitWebContext *context, gpointer)
{
    webkit_web_context_set_web_process_extensions_initialization_user_data(context,
        g_variant_new("(sb@as@ass)", ubo.dir.c_str(), ubo.on, ubo_strv(ubo.allowed), ubo_strv(ubo.lists), ubo.my_filters.c_str()));
}

void ubo_broadcast()
{
    webkit_web_context_send_message_to_all_extensions(g.context,
        webkit_user_message_new("wren-ubo:state", g_variant_new("(b@as@ass)", ubo.on, ubo_strv(ubo.allowed), ubo_strv(ubo.lists),
            ubo.my_filters.c_str())));
    log_msg("ad blocker: %s, %zu extra list(s), %zu site(s) allowed", ubo.on ? "on" : "off", ubo.lists.size(), ubo.allowed.size());
}

std::string html_escape(const std::string &s)
{
    std::string out;
    for (char c : s) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        case '\'': out += "&#39;"; break;
        default: out += c;
        }
    }
    return out;
}

// wren://adblock: the settings page. Big, remote-friendly controls (the engine turns spatial navigation on).
std::string ubo_page()
{
    std::string h = R"HTML(<!doctype html><html><head><meta charset="utf-8"><title>Ad blocker settings</title>
<meta name="viewport" content="width=device-width">
<style>
:root{color-scheme:dark;--bg:#0b1020;--card:#151c33;--ink:#e8ecf8;--dim:#9aa6c4;--on:#3f7cff;--line:#28314f}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--ink);font:22px/1.4 system-ui,sans-serif;padding:40px 64px}
h1{font-size:40px;margin:0 0 6px}h2{font-size:26px;margin:36px 0 12px;color:var(--dim);font-weight:600}
p.note{color:var(--dim);margin:0 0 20px}
button{font:inherit;color:inherit;background:var(--card);border:3px solid var(--line);border-radius:14px;padding:16px 22px;cursor:pointer}
button:focus{outline:none;border-color:#fff;box-shadow:0 0 0 4px var(--on)}
.row{display:flex;align-items:center;gap:20px;width:100%;text-align:left;margin:0 0 12px}
.row .t{flex:1}.row small{display:block;color:var(--dim);font-size:18px}
.sw{width:84px;height:44px;border-radius:22px;background:#3a4363;position:relative;flex:none}
.sw::after{content:"";position:absolute;top:5px;left:6px;width:34px;height:34px;border-radius:50%;background:#cfd6ea;transition:left .15s}
[aria-pressed=true] .sw{background:var(--on)}[aria-pressed=true] .sw::after{left:44px;background:#fff}
.size{color:var(--dim);font-size:18px;flex:none}
.add{display:flex;gap:12px;margin-bottom:12px}input{font:inherit;flex:1;padding:16px;border-radius:14px;border:3px solid var(--line);background:#0f1530;color:inherit}
input:focus,textarea:focus{outline:none;border-color:#fff}
textarea{display:block;width:100%;font:20px/1.4 monospace;padding:16px;border-radius:14px;border:3px solid var(--line);background:#0f1530;color:inherit;margin-bottom:12px}
code{font-size:18px;color:var(--ink)}
.host{display:flex;align-items:center;gap:16px;margin-bottom:8px}.host span{flex:1}
#saved{position:fixed;right:40px;top:30px;background:var(--on);padding:10px 18px;border-radius:12px;opacity:0;transition:opacity .2s}
</style></head><body>
<div id="saved">Saved</div>
<h1>Ad blocker</h1><p class="note">uBlock Origin's lists, matched inside Wren. Changes apply to pages loaded from now on. Never used on YouTube.</p>
)HTML";
    h += "<button class=\"row\" data-cmd=\"on\" aria-pressed=\"" + std::string(ubo.on ? "true" : "false") +
         "\"><span class=\"t\">Block ads and trackers<small>uBlock filters, EasyList, EasyPrivacy, malware and Peter Lowe's lists (always used when on)</small></span><span class=\"sw\"></span></button>\n";
    h += "<h2>More filter lists</h2><p class=\"note\">Each list uses some of the TV's memory while it is on.</p>\n";
    for (const UboList &l : ubo.catalog) {
        const bool on = std::find(ubo.lists.begin(), ubo.lists.end(), l.name) != ubo.lists.end();
        char size[32];
        snprintf(size, sizeof size, "%.1f MB", l.size / 1e6);
        h += "<button class=\"row\" data-cmd=\"list\" data-name=\"" + html_escape(l.name) + "\" aria-pressed=\"" +
             (on ? "true" : "false") + "\"><span class=\"t\">" + html_escape(l.title) + "<small>" + html_escape(l.about) +
             "</small></span><span class=\"size\">" + size + "</span><span class=\"sw\"></span></button>\n";
    }
    h += "<h2>Sites where ads are allowed</h2><div class=\"add\"><input id=\"host\" placeholder=\"example.com\" "
         "autocapitalize=\"off\" autocomplete=\"off\" spellcheck=\"false\"><button id=\"addhost\">Allow ads</button></div><div id=\"hosts\">\n";
    for (const std::string &a : ubo.allowed)
        h += "<div class=\"host\"><span>" + html_escape(a) + "</span><button data-cmd=\"disallow\" data-name=\"" +
             html_escape(a) + "\">Remove</button></div>\n";
    h += "</div><h2>My filters</h2><p class=\"note\">Your own rules, in uBlock Origin's syntax, one per line. They stay on "
         "this TV (in the browser's data folder), never in the package. Wren's fixes for any site: "
         "<code>example.com##+js(wren-no-mse)</code> plays plain video files instead of MSE streams, "
         "<code>example.com##+js(wren-media-debug)</code> logs media details.</p>"
         "<textarea id=\"mine\" rows=\"8\" spellcheck=\"false\" autocapitalize=\"off\">" + html_escape(ubo.my_filters) +
         "</textarea><button id=\"savemine\">Save my filters</button>\n";
    h += R"HTML(
<script>
document.getElementById('savemine').addEventListener('click', () => send('mine\n' + document.getElementById('mine').value));
const send = m => { try { window.webkit.messageHandlers.wrenAdblock.postMessage(m); flash(); } catch (e) {} };
const flash = () => { const s = document.getElementById('saved'); s.style.opacity = 1; clearTimeout(flash.t); flash.t = setTimeout(() => s.style.opacity = 0, 900); };
document.addEventListener('click', e => {
  const b = e.target.closest('button[data-cmd]');
  if (!b) return;
  if (b.dataset.cmd === 'disallow') { send('disallow\n' + b.dataset.name); b.parentNode.remove(); return; }
  const on = b.getAttribute('aria-pressed') !== 'true';
  b.setAttribute('aria-pressed', on);
  send(b.dataset.cmd === 'on' ? 'on\n' + (on ? 1 : 0) : 'list\n' + b.dataset.name + '\n' + (on ? 1 : 0));
});
const add = () => {
  const i = document.getElementById('host'), v = i.value.trim().toLowerCase().replace(/^https?:\/\//, '').replace(/^www\./, '').replace(/\/.*$/, '');
  if (!v) return;
  send('allow\n' + v);
  const d = document.createElement('div'); d.className = 'host';
  d.innerHTML = '<span></span><button data-cmd="disallow">Remove</button>';
  d.firstChild.textContent = v; d.lastChild.dataset.name = v;
  document.getElementById('hosts').appendChild(d); i.value = '';
};
document.getElementById('addhost').addEventListener('click', add);
document.getElementById('host').addEventListener('keydown', e => { if (e.key === 'Enter') add(); });
</script></body></html>
)HTML";
    return h;
}

void on_wren_scheme(WebKitURISchemeRequest *request, gpointer)
{
    const char *uri = webkit_uri_scheme_request_get_uri(request);
    std::string body, type = "text/html";
    if (g_str_has_prefix(uri, "wren://adblock/compat.js") && !ubo.dir.empty()) {
        // The video policy (add_hybrid_video): the same file the web process extension runs, for the one case where
        // the extension is missing (reactor or the engine's own older builds).
        gchar *code = nullptr;
        if (!g_file_get_contents((ubo.dir + "/compat.js").c_str(), &code, nullptr, nullptr)) {
            GError *e = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "compat.js missing");
            webkit_uri_scheme_request_finish_error(request, e);
            g_error_free(e);
            return;
        }
        body = code;
        type = "application/javascript";
        g_free(code);
    } else if (g_str_has_prefix(uri, "wren://adblock") && !ubo.dir.empty()) {
        body = ubo_page();
    } else if (g_str_has_prefix(uri, "wren://adblock")) {
        body = "<!doctype html><meta charset=utf-8><title>Ad blocker</title><body style=\"font:24px system-ui;padding:48px\">"
               "This Wren package has no ad blocker.";
    } else {
        GError *e = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "No such page: try wren://adblock");
        webkit_uri_scheme_request_finish_error(request, e);
        g_error_free(e);
        return;
    }
    gsize len = body.size();
    GInputStream *in = g_memory_input_stream_new_from_data(g_memdup2(body.data(), len), len, g_free);
    webkit_uri_scheme_request_finish(request, in, len, type.c_str());
    g_object_unref(in);
}

// Messages from wren://adblock ("on\n1", "list\nNAME\n0", "allow\nHOST", "disallow\nHOST", "mine\nTEXT"). Only a tab showing
// wren://adblock is listened to: the handler exists in every page's main world, so a web page could post too.
void on_adblock_message(WebKitUserContentManager *, JSCValue *value, gpointer view)
{
    const char *uri = webkit_web_view_get_uri(WEBKIT_WEB_VIEW(view));
    if (!uri || !g_str_has_prefix(uri, "wren://adblock") || ubo.dir.empty() || !jsc_value_is_string(value))
        return;
    gchar *text = jsc_value_to_string(value);
    if (g_str_has_prefix(text, "mine\n")) {
        ubo.my_filters.assign(text + 5, std::min(strlen(text + 5), UBO_MY_FILTERS_MAX));
        g_free(text);
        ubo_save_my_filters();
        ubo_reload_popups();
        ubo_broadcast();
        return;
    }
    gchar **p = g_strsplit(text, "\n", 3);
    g_free(text);
    const std::string cmd = p[0] ? p[0] : "", arg = p[0] && p[1] ? p[1] : "", arg2 = p[0] && p[1] && p[2] ? p[2] : "";
    g_strfreev(p);
    if (cmd == "on") {
        ubo.on = arg == "1";
    } else if (cmd == "list") {
        const bool known = std::any_of(ubo.catalog.begin(), ubo.catalog.end(), [&](const UboList &l) { return l.name == arg; });
        ubo.lists.erase(std::remove(ubo.lists.begin(), ubo.lists.end(), arg), ubo.lists.end());
        if (known && arg2 == "1")
            ubo.lists.push_back(arg);
    } else if (cmd == "allow" || cmd == "disallow") {
        const gchar *one[] = { arg.c_str(), nullptr };
        const std::vector<std::string> host = ubo_split(one);
        if (host.empty())
            return;
        ubo.allowed.erase(std::remove(ubo.allowed.begin(), ubo.allowed.end(), host[0]), ubo.allowed.end());
        if (cmd == "allow")
            ubo.allowed.push_back(host[0]);
    } else {
        return;
    }
    ubo_save_settings();
    ubo_broadcast();
}

// Called for every new tab (tab_open): the settings page's message handler on the tab's content manager.
void ubo_attach(WebKitUserContentManager *ucm, WebKitWebView *view)
{
    if (webkit_user_content_manager_register_script_message_handler(ucm, "wrenAdblock", nullptr))
        g_signal_connect(ucm, "script-message-received::wrenAdblock", G_CALLBACK(on_adblock_message), view);
}

// ---- pop-ups and pop-unders -------------------------------------------------------------------------------
// uBO blocks the windows its filters name ($popup) and the ones a page opens without being asked. Here a new window
// (window.open, target=_blank) is refused when
//  - no user gesture started it,
//  - a $popup rule matches its address (the opener page is the context of $domain= and $3p), also later, when the
//    window moves on to another address,
//  - it is a second window within two seconds of the opener's last one (one click, one window), or
//  - it is a pop-under: a script opened it for another site and the opener page then navigates within three seconds:
//    the window is closed again (popup_opener_moves).
// A window the user asked for (a link with target=_blank, a sign-in window opened by a click) passes all four. Not for
// sites on the ad blocker's allowed list, nor when the blocker is off.
bool popups_guarded(const std::string &opener_uri)
{
    if (ubo.dir.empty() || !ubo.on)
        return false;
    const std::string host = popup::Rules::host_of(popup::Rules::lower(opener_uri));
    for (const std::string &a : ubo.allowed)
        if (!host.empty() && popup::Rules::host_in(host, popup::Rules::lower(a)))
            return false;
    return true;
}

bool ubo_popup_third_party(WebKitWebView *opener, const char *target)
{
    const char *o = webkit_web_view_get_uri(opener);
    const std::string oh = popup::Rules::host_of(popup::Rules::lower(o ? o : "")), th = popup::Rules::host_of(popup::Rules::lower(target ? target : ""));
    return !oh.empty() && !th.empty() && popup::Rules::site_of(oh) != popup::Rules::site_of(th);
}

bool popup_create_blocked(WebKitWebView *view, WebKitNavigationAction *action, std::string *why)
{
    const char *o = webkit_web_view_get_uri(view);
    const std::string opener_uri = o ? o : "";
    if (!popups_guarded(opener_uri))
        return false;
    if (!webkit_navigation_action_is_user_gesture(action)) {
        *why = "no user gesture";
        return true;
    }
    const char *t = webkit_uri_request_get_uri(webkit_navigation_action_get_request(action));
    const char *which = nullptr;
    if (ubo.popups.blocks(t ? t : "", opener_uri, &which)) {
        *why = std::string("pop-up rule ") + (which ? which : "");
        return true;
    }
    static std::map<uint32_t, int64_t> last;  // opener tab -> when it last got a window
    const uint32_t opener = tab_of_web_view(view);
    const int64_t now = g_get_monotonic_time();
    auto it = last.find(opener);
    if (it != last.end() && now - it->second < 2000000) {
        *why = "a second window within 2 s";
        return true;
    }
    last[opener] = now;
    return false;
}

// A window that is going somewhere (an address change or a redirect): the rules apply to where it ends up.
bool popup_navigation_blocked(WebKitWebView *view, const char *uri)
{
    Tab *tab = tab_by_id(tab_of_web_view(view));
    if (!tab || !tab->opener || !uri || g_get_monotonic_time() - tab->born_us > 20000000)
        return false;
    Tab *from = tab_by_id(tab->opener);
    const char *o = from ? webkit_web_view_get_uri(from->web_view) : nullptr;
    const std::string opener_uri = o ? o : "";
    if (!popups_guarded(opener_uri))
        return false;
    const char *which = nullptr;
    if (!ubo.popups.blocks(uri, opener_uri, &which))
        return false;
    log_msg("tab %u: window closed (pop-up rule %s): %.200s", tab->id, which ? which : "", uri);
    send_msg(WPE_ENGINE_TAB_CLOSE, tab->id, nullptr, 0);
    return true;
}

// The opener navigates (to `uri`) soon after it opened a window by script for another site: that window was a pop-under.
void popup_opener_moves(WebKitWebView *view, const char *uri)
{
    const uint32_t id = tab_of_web_view(view);
    if (!id || !uri || !*uri || ubo.dir.empty() || !ubo.on)
        return;
    const char *cur = webkit_web_view_get_uri(view);
    if (cur && g_strcmp0(cur, uri) == 0)
        return;
    const int64_t now = g_get_monotonic_time();
    std::vector<uint32_t> under;
    for (auto &[tid, tab] : g.tabs)
        if (tab.opener == id && tab.pop_under_candidate && now - tab.born_us < 3000000 && popups_guarded(cur ? cur : ""))
            under.push_back(tid);
    for (uint32_t tid : under) {
        log_msg("tab %u: window closed (a pop-under: tab %u moved on to %.120s right after opening it)", tid, id, uri);
        send_msg(WPE_ENGINE_TAB_CLOSE, tid, nullptr, 0);
    }
}

void setup_ubo(const std::string &data_dir)
{
    WebKitWebContext *context = g.context;
    webkit_web_context_register_uri_scheme(context, "wren", on_wren_scheme, nullptr, nullptr);
    // Pages from the web can't load or frame it; it counts as a secure context of its own.
    WebKitSecurityManager *security = webkit_web_context_get_security_manager(context);
    webkit_security_manager_register_uri_scheme_as_local(security, "wren");
    webkit_security_manager_register_uri_scheme_as_secure(security, "wren");
    if (g_strcmp0(getenv("WREN_UBO"), "0") == 0) {
        log_msg("ad blocker: off (WREN_UBO=0)");
        return;
    }
    gchar *exe = g_file_read_link("/proc/self/exe", nullptr);
    gchar *base = exe ? g_path_get_dirname(exe) : nullptr;
    g_free(exe);
    if (!base)
        return;
    const std::string dir = std::string(base) + "/ubo", ext = std::string(base) + "/lib/web-extensions";
    g_free(base);
    if (::access((dir + "/engine.dat").c_str(), R_OK) != 0 || ::access((ext + "/libwren-ubo.so").c_str(), R_OK) != 0) {
        log_msg("ad blocker: not in this package");
        return;
    }
    ubo.dir = dir;
    ubo.settings_path = data_dir + "/ubo-settings.ini";
    ubo.my_filters_path = data_dir + "/ubo-my-filters.txt";
    ubo_load_settings();
    ubo_reload_popups();
    webkit_web_context_set_web_process_extensions_directory(context, ext.c_str());
    webkit_web_context_add_path_to_sandbox(context, dir.c_str(), TRUE);
    g_signal_connect(context, "initialize-web-process-extensions", G_CALLBACK(on_initialize_web_process_extensions), nullptr);
    log_msg("ad blocker: uBO lists, %s, %zu of %zu extra list(s), %zu site(s) allowed; settings at wren://adblock",
        ubo.on ? "on" : "off", ubo.lists.size(), ubo.catalog.size(), ubo.allowed.size());
}

void set_zoom(uint32_t permille)
{
    g.zoom = std::clamp(permille, 250u, 5000u) / 1000.0;
    for (auto &[id, tab] : g.tabs)
        webkit_web_view_set_zoom_level(tab.web_view, g.zoom);
}

void set_scale(uint32_t permille)
{
    g.scale = std::clamp(permille, 1000u, 3000u) / 1000.0;
    resize_shown();
    log_msg("device scale %.2f", g.scale);
}

// ---- receiving -------------------------------------------------------------------------------------------

template<typename T> const T *payload(const uint8_t *buf, ssize_t n, const WpeMsgHeader &h, size_t min = sizeof(T))
{
    if (h.size < min || static_cast<size_t>(n) < sizeof(WpeMsgHeader) + min)
        return nullptr;
    return reinterpret_cast<const T *>(buf + sizeof(WpeMsgHeader));
}

std::string text_of(const uint8_t *buf, ssize_t n, const WpeMsgHeader &h)
{
    if (h.size < offsetof(WpeText, text) || static_cast<size_t>(n) < sizeof(WpeMsgHeader) + offsetof(WpeText, text))
        return {};
    const auto *t = reinterpret_cast<const WpeText *>(buf + sizeof(WpeMsgHeader));
    const size_t avail = static_cast<size_t>(n) - sizeof(WpeMsgHeader) - offsetof(WpeText, text);
    return std::string(t->text, std::min<size_t>({ t->length, avail, WPE_IPC_MAX_TEXT - 1 }));
}

void handle_message(const uint8_t *buf, ssize_t n)
{
    WpeMsgHeader h;
    memcpy(&h, buf, sizeof(h));
    Tab *tab = tab_by_id(h.tab);      // navigation target
    Tab *active = active_tab();       // input target
    switch (h.type) {
    case WPE_APP_TAB_OPEN: tab_open(h.tab, text_of(buf, n, h)); break;
    case WPE_APP_TAB_CLOSE: tab_close(h.tab); break;
    case WPE_APP_TAB_ACTIVATE: tab_activate(h.tab); break;
    case WPE_APP_LOAD_URI: {
        const std::string uri = text_of(buf, n, h);
        if (tab && !uri.empty())
            webkit_web_view_load_uri(tab->web_view, uri.c_str());
        break;
    }
    case WPE_APP_GO_BACK: if (tab) webkit_web_view_go_back(tab->web_view); break;
    case WPE_APP_GO_FORWARD: if (tab) webkit_web_view_go_forward(tab->web_view); break;
    case WPE_APP_RELOAD: if (tab) webkit_web_view_reload(tab->web_view); break;
    case WPE_APP_STOP: if (tab) webkit_web_view_stop_loading(tab->web_view); break;
    case WPE_APP_RUN_JS: {
        const std::string script = text_of(buf, n, h);
        if (tab)
            webkit_web_view_evaluate_javascript(tab->web_view, script.c_str(), static_cast<gssize>(script.size()),
                nullptr, nullptr, nullptr, nullptr, nullptr);
        break;
    }
    case WPE_APP_RESIZE:
        if (auto *s = payload<WpeSize>(buf, n, h))
            resize(s->width, s->height);
        break;
    case WPE_APP_VISIBLE:
        if (auto *f = payload<WpeFlag>(buf, n, h); f && active)
            set_visible(*active, f->value != 0);
        break;
    case WPE_APP_FOCUS:
        if (auto *f = payload<WpeFlag>(buf, n, h); f && active)
            set_focused(*active, f->value != 0);
        break;
    case WPE_APP_KEY:
        if (auto *k = payload<WpeKey>(buf, n, h); k && active) {
            if (active->direct)
                direct::key(active->direct, *k);
            else
                handle_key(active->view, *k);
        }
        break;
    case WPE_APP_POINTER:
        if (auto *p = payload<WpePointer>(buf, n, h); p && active) {
            if (active->direct)
                direct::pointer(active->direct, *p, g.buttons);
            else
                handle_pointer(active->view, *p);
        }
        break;
    case WPE_APP_SCROLL:
        if (auto *s = payload<WpeScroll>(buf, n, h); s && active) {
            if (active->direct)
                direct::scroll(active->direct, *s, g.buttons);
            else
                handle_scroll(active->view, *s);
        }
        break;
    case WPE_APP_TEXT_COMMIT: {
        const std::string text = text_of(buf, n, h);
        if (g.direct) {
            if (active)
                direct::commit_text(active->ime, text.c_str());
        } else
            webos_display_commit_text(WEBOS_DISPLAY(g.display), text.c_str());
        break;
    }
    case WPE_APP_FRAME_DONE:
        if (auto *d = payload<WpeFrameDone>(buf, n, h))
            frame_done(*d);
        break;
    case WPE_APP_CLEAR_DATA:
        if (auto *c = payload<WpeClearData>(buf, n, h))
            clear_data(*c);
        break;
    case WPE_APP_SET_ZOOM:
        if (auto *z = payload<WpeZoom>(buf, n, h))
            set_zoom(z->permille);
        break;
    case WPE_APP_SET_SCALE:
        if (auto *z = payload<WpeZoom>(buf, n, h))
            set_scale(z->permille);
        break;
    case WPE_APP_SET_LAYER: {
        const std::string layer = text_of(buf, n, h);
        if (g.direct && !layer.empty()) {
            direct::set_layer(layer.c_str());
            for (auto &[id, t] : g.tabs)
                direct::reattach(t.direct);
        }
        break;
    }
    case WPE_APP_QUIT:
        g_main_loop_quit(g.loop);
        break;
    default:
        log_msg("ignoring message type %u", h.type);
    }
}

gboolean on_socket(gint fd, GIOCondition condition, gpointer)
{
    if (condition & (G_IO_HUP | G_IO_ERR)) {
        log_msg("UI closed the connection");
        g_main_loop_quit(g.loop);
        return G_SOURCE_REMOVE;
    }
    alignas(WpeMsgHeader) uint8_t buf[WPE_IPC_MAX_PACKET];
    for (;;) {
        ssize_t n = recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return G_SOURCE_CONTINUE;
            log_msg("recv: %s", strerror(errno));
            g_main_loop_quit(g.loop);
            return G_SOURCE_REMOVE;
        }
        if (n == 0) {
            g_main_loop_quit(g.loop);
            return G_SOURCE_REMOVE;
        }
        if (static_cast<size_t>(n) >= sizeof(WpeMsgHeader))
            handle_message(buf, n);
    }
}

std::string env_or(const char *name, const char *fallback)
{
    const char *v = g_getenv(name);
    return v && *v ? v : fallback;
}

} // namespace

int main(int, char **)
{
    signal(SIGPIPE, SIG_IGN);
    // No on-disk media buffering on 32-bit systems: GStreamer's downloadbuffer keeps file offsets in gsize, so
    // files over 4 GiB (long 4K movies) wrap and never play; it also wrote whole videos to the TV's storage.
    // WebKit patch 0012 makes urisourcebin honour this too. Playback buffers in memory instead.
    if (sizeof(gsize) < 8)
        g_setenv("WPE_SHELL_DISABLE_MEDIA_DISK_CACHE", "1", FALSE);
    // No core dumps from the engine or its WebKit processes (inherited): the kernel writes them into the app's own
    // folder on the TV's small app partition (8 dumps, 1.4 GB, filled it on 2026-11-10). WPE_BROWSER_CORE_DUMPS=1
    // keeps them for debugging.
    if (!getenv("WPE_BROWSER_CORE_DUMPS")) {
        const struct rlimit none = {0, 0};
        setrlimit(RLIMIT_CORE, &none);
    }
    if (fcntl(g.sock, F_GETFD) < 0) {
        fprintf(stderr, "wpe-engine: fd %d is not open; this program is started by the browser app\n", g.sock);
        return 2;
    }

    // The UI speaks first: protocol version and the page size.
    alignas(WpeMsgHeader) uint8_t buf[WPE_IPC_MAX_PACKET];
    ssize_t n;
    do {
        n = recv(g.sock, buf, sizeof(buf), 0);
    } while (n < 0 && errno == EINTR);
    WpeMsgHeader h {};
    if (n >= static_cast<ssize_t>(sizeof(h)))
        memcpy(&h, buf, sizeof(h));
    const auto *hello = h.type == WPE_APP_HELLO ? payload<WpeAppHello>(buf, n, h) : nullptr;
    if (!hello || hello->version != WPE_IPC_VERSION) {
        log_msg("bad or missing hello (type %u)", h.type);
        return 3;
    }
    g.width = hello->width;
    g.height = hello->height;

    g.loop = g_main_loop_new(nullptr, FALSE);

    // Direct presentation (direct.cpp) or SHM frames (platform.cpp). WebKit allows one per process: once a
    // WPEDisplay exists, views with a libwpe backend are refused, so this is decided before anything else.
    if (env_or("WPE_BROWSER_PRESENT", "shm") == "direct") {
        g.direct = !env_or("WPEB_SURFACE_GROUP", "").empty() && direct::init(env_or("WPEB_SURFACE_GROUP", "").c_str(), env_or("WPEB_SURFACE_LAYER", "page").c_str(),
            on_direct_status);
        if (!g.direct)
            log_msg("direct presentation unavailable; using SHM frames");
        // A view keeps its web process across sites (WebKit patch 0006): a new process would need the view's layer,
        // and the webOS compositor hides the app when it moves from one process's surface to another's (TV,
        // 2026-11-11, YouTube's COOP swap). All tabs already share one web process here.
        if (g.direct)
            g_setenv("WPE_BROWSER_NO_PROCESS_SWAP", "1", TRUE);
    }

    GError *error = nullptr;
    if (!g.direct) {
        WebOSSink sink { on_frame, on_ime_focus, nullptr };
        g.display = webos_display_new(&sink);
        if (!wpe_display_connect(g.display, &error)) {
            log_msg("display: %s", error->message);
            return 4;
        }
        wpe_display_set_primary(g.display);
    }

    // Grayscale text antialiasing: LCD subpixel AA (WPE's default, RGB) shows as coloured, soft glyph edges
    // on a TV panel, more so once the page is scaled to the screen. 0xFF is not a WPE layout; WebKit
    // patch 0003 reads it as "none". (Direct mode: fontconfig's rgba setting in the bundled fonts.conf.)
    if (WPESettings *settings = g.display ? wpe_display_get_settings(g.display) : nullptr) {
        if (!wpe_settings_set_byte(settings, WPE_SETTING_FONT_SUBPIXEL_LAYOUT, 0xFF, WPE_SETTINGS_SOURCE_PLATFORM, &error)) {
            log_msg("font settings: %s", error->message);
            g_clear_error(&error);
        }
    }

#if defined(__arm__) && !defined(__aarch64__)
    // TLS speed of the 32-bit network process: GnuTLS/nettle here have no AES-GCM acceleration for 32-bit ARM (the 64-bit code uses the CPU's
    // crypto extensions, 32-bit code cannot), so with the default order (AES-GCM first) one core decrypts only ~50 Mbit/s: a 4K stream from
    // a CDN (40-60 Mbit/s) rebuffered every ~2 s while plain HTTP on the same Wi-Fi ran at 215 Mbit/s (TV, 2026-11-20). ChaCha20-Poly1305
    // runs at ~220 Mbit/s on the same core, so it goes first; the others stay as fallbacks for servers without it. Read by glib-networking
    // when a connection is made; a value set by the user (the launch parameter wpebEnv G_TLS_GNUTLS_PRIORITY) is kept.
    g_setenv("G_TLS_GNUTLS_PRIORITY", "NORMAL:-CIPHER-ALL:+CHACHA20-POLY1305:+AES-128-GCM:+AES-256-GCM:+AES-128-CBC:+AES-256-CBC", FALSE);
#endif
    const std::string data_dir = env_or("WPE_BROWSER_DATA_DIR", "/tmp/wpe-browser/data");
    const std::string cache_dir = env_or("WPE_BROWSER_CACHE_DIR", "/tmp/wpe-browser/cache");
    g_mkdir_with_parents(data_dir.c_str(), 0700);
    g_mkdir_with_parents(cache_dir.c_str(), 0700);
    // Memory-pressure limits for the network process (webkit_network_session_set_memory_pressure_settings covers only
    // that process; the web processes would need a WebKitWebContext built with "memory-pressure-settings"). The TV has
    // 2.6 GB of which ~700 MB are available while LG's own apps run, and a 32-bit web process starts to hurt at ~480 MB
    // on heavy pages. Without settings WebKit sizes its limit from the physical RAM (it sees all 2.6 GB), so pressure
    // handling only starts when memchute is already killing apps. Must be set before the session exists.
    // WPE_BROWSER_DEBUG_MEM_LIMIT_MB=0: WebKit's own.
    // WPE_BROWSER_DEBUG_MEM_LIMIT_MB=<n>: the process limit; the thresholds below are fractions of it (conservative / strict) and
    // the kill threshold stays off (a killed tab is worse than a slow one).
    {
        const long limit_mb = atol(env_or("WPE_BROWSER_DEBUG_MEM_LIMIT_MB", "420").c_str());
        if (limit_mb > 0) {
            WebKitMemoryPressureSettings *mp = webkit_memory_pressure_settings_new();
            webkit_memory_pressure_settings_set_memory_limit(mp, static_cast<guint>(limit_mb));
            // strict first: the setter asserts conservative < strict, and the default strict is below 0.5
            webkit_memory_pressure_settings_set_strict_threshold(mp, atof(env_or("WPE_BROWSER_DEBUG_MEM_STRICT", "0.75").c_str()));
            webkit_memory_pressure_settings_set_conservative_threshold(mp, atof(env_or("WPE_BROWSER_DEBUG_MEM_CONSERVATIVE", "0.5").c_str()));
            // The kill threshold (a multiple of the limit, WebKit measures resident + swapped): a web process that goes on growing past
            // its limit while the TV is already swapping (2026-11-20: 585 MB at a 420 MB limit, swap full, the whole TV frozen) is
            // ended, and the UI reloads the tab (browser/main.cpp: crashed tabs). 0 = never.
            webkit_memory_pressure_settings_set_kill_threshold(mp, atof(env_or("WPE_BROWSER_DEBUG_MEM_KILL", "1.3").c_str()));
            webkit_memory_pressure_settings_set_poll_interval(mp, atof(env_or("WPE_BROWSER_DEBUG_MEM_POLL_S", "5").c_str()));
            webkit_network_session_set_memory_pressure_settings(mp);  // the network process
            g.context = WEBKIT_WEB_CONTEXT(g_object_new(WEBKIT_TYPE_WEB_CONTEXT, "memory-pressure-settings", mp, nullptr));  // the web processes
            webkit_memory_pressure_settings_free(mp);
            log_msg("memory pressure: limit %ld MB, conservative %s, strict %s", limit_mb,
                env_or("WPE_BROWSER_DEBUG_MEM_CONSERVATIVE", "0.5").c_str(), env_or("WPE_BROWSER_DEBUG_MEM_STRICT", "0.75").c_str());
        }
    }
    if (!g.context) g.context = WEBKIT_WEB_CONTEXT(g_object_new(WEBKIT_TYPE_WEB_CONTEXT, nullptr));
    g.session = webkit_network_session_new(data_dir.c_str(), cache_dir.c_str());
    webkit_cookie_manager_set_persistent_storage(webkit_network_session_get_cookie_manager(g.session),
        (data_dir + "/cookies.sqlite").c_str(), WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE);

    // WPE_BROWSER_DEBUG_CACHE_MODEL: web (WebKit's default for browsers), doc (document viewer: no memory cache between pages),
    // primary (a few resources). Measured on the TV: see build/jit254/REPORT.md (round 20).
    {
        const std::string cm = env_or("WPE_BROWSER_DEBUG_CACHE_MODEL", "web");
        if (cm == "doc") webkit_web_context_set_cache_model(g.context, WEBKIT_CACHE_MODEL_DOCUMENT_VIEWER);
        else if (cm == "primary") webkit_web_context_set_cache_model(g.context, WEBKIT_CACHE_MODEL_DOCUMENT_BROWSER);
        else webkit_web_context_set_cache_model(g.context, WEBKIT_CACHE_MODEL_WEB_BROWSER);
        log_msg("cache model %s, page cache %s", cm.c_str(), env_or("WPE_BROWSER_DEBUG_PAGE_CACHE", "0") == "1" ? "on" : "off");
    }
    g.settings = webkit_settings_new();
    webkit_settings_set_enable_spatial_navigation(g.settings, TRUE);
    // The back/forward page cache keeps whole documents (DOM, layers, JS heaps) of the pages left behind: tens of MB
    // each on a 2.6 GB TV that LG's apps share. Off by default; WPE_BROWSER_DEBUG_PAGE_CACHE=1 restores it.
    webkit_settings_set_enable_page_cache(g.settings, env_or("WPE_BROWSER_DEBUG_PAGE_CACHE", "0") == "1");
    // No "webOS" in the user agent: sites' mobile checks list it (Palm's phones), e.g. webtv.nettv.com.np's
    // /iPhone|...|webOS|.../i showed its mobile page (2026-11-11). The rest is WPE's desktop Linux agent.
    webkit_settings_set_user_agent_with_application_details(g.settings, "WPEBrowser", "0.2");
    webkit_settings_set_enable_developer_extras(g.settings, FALSE);
    // EME (navigator.requestMediaKeySystemAccess) is off by default on WPE. Widevine comes from libocdm, which
    // drives the TV's own CDM (drm/ocdm-widevine); ClearKey is WebKit's own.
    webkit_settings_set_enable_encrypted_media(g.settings, TRUE);
    // Experiments: WPE_BROWSER_DEBUG_SETTINGS="enable-javascript=false;enable-page-cache=0" sets WebKitSettings properties
    // (booleans, integers, strings), e.g. from the launch parameter {"wpebEnv": {"WPE_BROWSER_DEBUG_SETTINGS": "..."}}.
    if (const char *debug = getenv("WPE_BROWSER_DEBUG_SETTINGS")) {
        gchar **pairs = g_strsplit(debug, ";", -1);
        for (gchar **p = pairs; *p; ++p) {
            gchar **kv = g_strsplit(*p, "=", 2);
            GParamSpec *spec = kv[0] && kv[1] ? g_object_class_find_property(G_OBJECT_GET_CLASS(g.settings), kv[0]) : nullptr;
            if (!spec) {
                if (**p) log_msg("debug setting %s: unknown", *p);
            } else if (spec->value_type == G_TYPE_BOOLEAN) {
                g_object_set(g.settings, kv[0], (gboolean)(g_strcmp0(kv[1], "true") == 0 || g_strcmp0(kv[1], "1") == 0), nullptr);
                log_msg("debug setting %s=%s", kv[0], kv[1]);
            } else if (spec->value_type == G_TYPE_UINT || spec->value_type == G_TYPE_INT) {
                GValue v = G_VALUE_INIT;
                g_value_init(&v, spec->value_type);
                if (spec->value_type == G_TYPE_UINT) g_value_set_uint(&v, (guint)atoi(kv[1])); else g_value_set_int(&v, atoi(kv[1]));
                g_object_set_property(G_OBJECT(g.settings), kv[0], &v);
                g_value_unset(&v);
                log_msg("debug setting %s=%s", kv[0], kv[1]);
            } else if (spec->value_type == G_TYPE_STRING) {
                g_object_set(g.settings, kv[0], kv[1], nullptr);
                log_msg("debug setting %s=%s", kv[0], kv[1]);
            }
            g_strfreev(kv);
        }
        g_strfreev(pairs);
    }
    // WPE_BROWSER_DEBUG_USER_AGENT: the whole user agent, for compatibility tests (a user agent has ";", which
    // WPE_BROWSER_DEBUG_SETTINGS uses between settings).
    if (const char *ua = getenv("WPE_BROWSER_DEBUG_USER_AGENT"); ua && *ua) {
        webkit_settings_set_user_agent(g.settings, ua);
        log_msg("debug user agent: %s", ua);
    }
    if (g.direct) {
        // COOP (YouTube sends it) moves a page into a new web process by its own path, which ignores
        // WPE_BROWSER_NO_PROCESS_SWAP; see there for why direct presentation can't follow a process switch.
        WebKitFeatureList *features = webkit_settings_get_all_features();
        for (gsize i = 0; i < webkit_feature_list_get_length(features); ++i) {
            WebKitFeature *f = webkit_feature_list_get(features, i);
            const char *id = webkit_feature_get_identifier(f);
            if (g_strcmp0(id, "CrossOriginOpenerPolicy") == 0) {
                webkit_settings_set_feature_enabled(g.settings, f, FALSE);
                log_msg("COOP off (direct presentation keeps pages in their web process)");
            }
            // WebKit 2.54 composites only the damaged rectangles and expects the previous frame's pixels to stay in the
            // target. A libwpe backend window (libWPEBackend-webos, Mali EGL) does not keep them: the page showed black
            // except where it animated (TV, 2026-11-20). Full repaints in direct mode; SHM mode keeps damage.
            // WPE_BROWSER_DEBUG_DAMAGE=1 leaves damage tracking on (tests).
            if (!getenv("WPE_BROWSER_DEBUG_DAMAGE") && (g_strcmp0(id, "PropagateDamagingInformation") == 0
                    || g_strcmp0(id, "UseDamagingInformationForCompositing") == 0 || g_strcmp0(id, "UnifyDamagedRegions") == 0)) {
                webkit_settings_set_feature_enabled(g.settings, f, FALSE);
                log_msg("direct presentation: %s off", id);
            }
        }
        webkit_feature_list_unref(features);
    }

    if (const char *sites = getenv("WPE_BROWSER_DEBUG_FORGET_COOKIES"); sites && *sites)
        forget_cookies(sites);

    setup_ubo(data_dir);
    g_unix_fd_add(g.sock, static_cast<GIOCondition>(G_IO_IN | G_IO_HUP | G_IO_ERR), on_socket, nullptr);

    WpeEngineReady ready { WPE_IPC_VERSION, webkit_get_major_version(), webkit_get_minor_version(), webkit_get_micro_version() };
    send_msg(WPE_ENGINE_READY, 0, &ready, sizeof(ready));
    send_present(0, 0, 0, 0);
    log_msg("ready: WPE WebKit %u.%u.%u, page %ux%u", ready.webkit_major, ready.webkit_minor, ready.webkit_micro,
        g.width, g.height);

    start_memory_guard();
    g_main_loop_run(g.loop);

    for (uint32_t i = 0; i < WPE_IPC_FRAME_SLOTS; ++i) {
        g_clear_object(&g.in_flight[i]);
        g_clear_object(&g.in_flight_view[i]);
    }
    while (!g.tabs.empty())
        tab_close(g.tabs.begin()->first);
    g_object_unref(g.settings);
    g_object_unref(g.session);
    g_clear_object(&g.display);
    g_main_loop_unref(g.loop);
    return 0;
}
