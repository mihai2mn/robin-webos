// SPDX-License-Identifier: GPL-3.0-or-later
// EXPERIMENT: does the app that OWNS the video get frames from libvt as a GL texture?
// Enabled only by WPE_BROWSER_DEBUG_VT_TEXTURE=<media url> / {"wpebVtTexture":"<url>"}. See the header.
//
// The arrangement (docs/lg-chromium-media.md §6, the maintainer's video notes):
//   VT_CreateVideoWindow -> its id becomes StarfishMediaAPIs Load option.windowId -> the pipeline decodes INTO
//   that VT window -> VT_UpdateOutputTexture(our texture) fills a GL texture in OUR context.
//
// Review fixes folded in (2026-11-20):
//   1. dump_png reads through an FBO bound to tex_ (GLES2 has no glGetTexImage); the FBO status is logged,
//      because a libvt-filled texture may not be colour-renderable -- that is itself an answer.
//   2. Load uses the URI transport (the pipeline fetches the URL itself); BUFFERSTREAM without Feed would
//      never decode and "no frames" would have been a false negative. Load's rc and every event are logged.
//   3. New frames are counted from VT_AVAILABLE events, not from render calls; both numbers are reported.
//   4. The enable URL is resolved once (by the caller), not per frame.

#include "vt_texture_probe.hpp"

#if !defined(WNK_SDL_WEBOS)
// Not a webOS build (no libvt, no Starfish header): the experiment is compiled out and can never start.
namespace browser {
void set_vt_log_fn(VtLogFn) {}
void set_vt_texture_url_from_launch(const std::string &) {}
std::string vt_texture_url() { return {}; }
VtTextureProbe::VtTextureProbe() = default;
VtTextureProbe::~VtTextureProbe() = default;
bool VtTextureProbe::start(const char *, std::string *why) {
    if (why) *why = "not a webOS build";
    return false;
}
void VtTextureProbe::stop() {}
bool VtTextureProbe::update(int *, int *) { return false; }
bool VtTextureProbe::dump_png(const char *, int, int) { return false; }
}  // namespace browser
#else

#include <dlfcn.h>

#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include <cstdarg>
#include <exception>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include <starfish-media-pipeline/StarfishMediaAPIs.h>

namespace browser {
namespace {

// libvt: declared here because the NDK ships no vt_openapi.h.
typedef int32_t VT_STATUS_T;
typedef int32_t VT_VIDEO_WINDOW_ID;
typedef int32_t VT_RESOURCE_ID;
typedef int32_t VT_CONTEXT_ID;
typedef struct {
    uint16_t w, h;
} VT_RESOLUTION_T;
typedef struct {
    uint16_t x, y, w, h;
} VT_REGION_T;
typedef struct {
    uint8_t a, b, c, d;
    VT_REGION_T activeRegion;
    VT_REGION_T maxRegion;
} VT_OUTPUT_INFO_T;
typedef void (*VT_EVENT_HANDLER)(int type, void *data, void *user_data);

typedef VT_VIDEO_WINDOW_ID (*fn_create_window)(int32_t);
typedef VT_STATUS_T (*fn_acquire)(VT_VIDEO_WINDOW_ID, VT_RESOURCE_ID *);
typedef VT_CONTEXT_ID (*fn_create_ctx)(VT_RESOURCE_ID, int);
typedef VT_STATUS_T (*fn_set_res)(VT_CONTEXT_ID, VT_RESOLUTION_T *);
typedef VT_STATUS_T (*fn_set_region)(VT_CONTEXT_ID, int);
typedef VT_STATUS_T (*fn_set_location)(VT_CONTEXT_ID, int);
typedef VT_STATUS_T (*fn_register)(VT_CONTEXT_ID, VT_EVENT_HANDLER, void *);
typedef VT_STATUS_T (*fn_generate)(VT_RESOURCE_ID, VT_CONTEXT_ID, uint32_t *, VT_OUTPUT_INFO_T *);
typedef VT_STATUS_T (*fn_update)(VT_CONTEXT_ID, uint32_t, VT_OUTPUT_INFO_T *);
typedef VT_STATUS_T (*fn_delete_tex)(VT_CONTEXT_ID, uint32_t);
typedef VT_STATUS_T (*fn_destroy_ctx)(VT_CONTEXT_ID);
typedef VT_STATUS_T (*fn_release)(VT_VIDEO_WINDOW_ID, VT_RESOURCE_ID);
typedef VT_STATUS_T (*fn_destroy_window)(VT_VIDEO_WINDOW_ID);

VtLogFn g_log_fn = nullptr;

void vlog(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (g_log_fn) g_log_fn(buf);  // the app's logger: the only sink that reaches events.log
    fprintf(stderr, "[vt-probe] %s\n", buf);
    fflush(stderr);
}
}  // namespace

void set_vt_log_fn(VtLogFn fn) { g_log_fn = fn; }

namespace {

double monotonic_s() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1e9;
}
double cpu_s() {
    timespec ts{};
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) != 0) return 0;
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1e9;
}

// Events are reported from the pipeline's callback: count VT_AVAILABLE (new frame available) and note
// whether the pipeline reached PLAYING, so "no frame" can be told apart from "never decoded".
volatile int g_events = 0;
volatile int g_playing = 0;
volatile int g_load_completed = 0;
volatile int g_last_event_type = -1;

}  // namespace

struct VtImpl {
    // libvt entry points, named after the exported symbols (the SYM() macro relies on it)
    void *lib = nullptr;
    fn_create_window VT_CreateVideoWindow = nullptr;
    fn_acquire VT_AcquireVideoWindowResource = nullptr;
    fn_create_ctx VT_CreateContext = nullptr;
    fn_set_res VT_SetTextureResolution = nullptr;
    fn_set_region VT_SetTextureSourceRegion = nullptr;
    fn_set_location VT_SetTextureSourceLocation = nullptr;
    fn_register VT_RegisterEventHandler = nullptr;
    fn_generate VT_GenerateTexture = nullptr;
    fn_update VT_UpdateOutputTexture = nullptr;
    fn_delete_tex VT_DeleteTexture = nullptr;
    fn_destroy_ctx VT_DestroyContext = nullptr;
    fn_release VT_ReleaseVideoWindowResource = nullptr;
    fn_destroy_window VT_DestroyVideoWindow = nullptr;

    VT_VIDEO_WINDOW_ID window = -1;
    VT_RESOURCE_ID resource = -1;
    VT_CONTEXT_ID ctx = -1;
    VT_RESOLUTION_T res{1280, 720};

    StarfishMediaAPIs *player = nullptr;
    std::string url;
    double started_s = 0;
    double cpu_at_start = 0;
    int load_rc = -999;
    int last_update_rc = -999;
    int render_calls = 0;      // how often update() ran
    int frames_seen = 0;       // VT_AVAILABLE events seen (frames the plane produced)
    int frames_read = 0;       // update() reported a non-empty region
    int prev_events = 0;
};

namespace {
void vt_event(int type, void *, void *user) {
    if (type == 0) {  // VT_AVAILABLE
        ++g_events;
        g_last_event_type = type;
    }
    (void)user;
}
void starfish_event(int type, int64_t num, const char *str) {
    g_last_event_type = type;
    switch (type) {
    case PF_EVENT_TYPE_STR_STATE_UPDATE__LOADCOMPLETED: g_load_completed = 1; break;
    case PF_EVENT_TYPE_STR_STATE_UPDATE__PLAYING: g_playing = 1; break;
    default: break;
    }
    // Log every event: the report must show whether the pipeline reached PLAYING.
    vlog("starfish event 0x%x num=%lld %s", type, static_cast<long long>(num), str && *str ? str : "");
}
}  // namespace

namespace {
// The launch-parameter value ({"wpebVtTexture":"<url>"}): a luna-launched app cannot be given env vars.
std::string g_launch_url;
}  // namespace

void set_vt_texture_url_from_launch(const std::string &url) { g_launch_url = url; }
std::string vt_texture_url() { return g_launch_url; }

// The player object must live in an over-sized aligned block: the firmware's object is larger than the header
// declares (lg_media_player.cpp:137-144 uses 64 KB for the same reason).
static StarfishMediaAPIs *make_player() {
    constexpr size_t kSize = 64 * 1024;
    void *mem = nullptr;
    if (posix_memalign(&mem, 64, kSize) != 0 || !mem) return nullptr;
    return new (mem) StarfishMediaAPIs(nullptr);
}

VtTextureProbe::VtTextureProbe() = default;
VtTextureProbe::~VtTextureProbe() { stop(); }

bool VtTextureProbe::start(const char *url, std::string *why) {
    auto *im = new VtImpl;
    impl_ = im;
    im->url = url ? url : "";
    im->cpu_at_start = cpu_s();
    g_events = g_playing = g_load_completed = 0;

    im->lib = dlopen("libvt.so.1", RTLD_NOW);
    if (!im->lib) {
        const char *derr = dlerror();
        *why = std::string("no libvt.so.1: ") + (derr ? derr : "(no reason)");
        vlog("start: %s", why->c_str());
        return false;
    }
#define SYM(name) im->name = reinterpret_cast<decltype(im->name)>(dlsym(im->lib, #name))
    SYM(VT_CreateVideoWindow);
    SYM(VT_AcquireVideoWindowResource);
    SYM(VT_CreateContext);
    SYM(VT_SetTextureResolution);
    SYM(VT_SetTextureSourceRegion);
    SYM(VT_SetTextureSourceLocation);
    SYM(VT_RegisterEventHandler);
    SYM(VT_GenerateTexture);
    SYM(VT_UpdateOutputTexture);
    SYM(VT_DeleteTexture);
    SYM(VT_DestroyContext);
    SYM(VT_ReleaseVideoWindowResource);
    SYM(VT_DestroyVideoWindow);
#undef SYM
    if (!im->VT_CreateVideoWindow || !im->VT_AcquireVideoWindowResource || !im->VT_CreateContext || !im->VT_UpdateOutputTexture) {
        *why = "libvt is missing entry points";
        vlog("start: %s", why->c_str());
        return false;
    }

    im->window = im->VT_CreateVideoWindow(0);
    if (im->window < 0) {
        *why = "VT_CreateVideoWindow failed: " + std::to_string(im->window);
        vlog("start: %s", why->c_str());
        return false;
    }
    if (im->VT_AcquireVideoWindowResource(im->window, &im->resource) != 0) {
        *why = "VT_AcquireVideoWindowResource failed";
        vlog("start: %s", why->c_str());
        return false;
    }
    im->ctx = im->VT_CreateContext(im->resource, 0);
    if (im->ctx < 0) {
        *why = "VT_CreateContext failed";
        vlog("start: %s", why->c_str());
        return false;
    }
    if (im->VT_SetTextureResolution) im->VT_SetTextureResolution(im->ctx, &im->res);
    if (im->VT_SetTextureSourceRegion) im->VT_SetTextureSourceRegion(im->ctx, 1 /* MAX */);
    if (im->VT_SetTextureSourceLocation) im->VT_SetTextureSourceLocation(im->ctx, 0 /* DISPLAY */);
    if (im->VT_RegisterEventHandler) im->VT_RegisterEventHandler(im->ctx, vt_event, im);
    vlog("libvt ready: window=%d resource=%d ctx=%d", im->window, im->resource, im->ctx);

    if (im->url.rfind("vt-only:", 0) == 0) {
        im->started_s = monotonic_s();
        vlog("vt-only mode: skipping StarfishMediaAPIs");
        return true;
    }

    // The pipeline decodes INTO the VT window: that id is the binding the whole experiment depends on.
    // Construction and Load are wrapped: a library that throws must not take the UI down silently, and the
    // report needs the exception text. (The build is -fno-exceptions, so try/catch is compiled out there;
    // the step logging before and after each call is what identifies the thrower in that case.)
    vlog("constructing StarfishMediaAPIs...");
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS)
    try {
        im->player = make_player();
    } catch (const std::exception &e) {
        vlog("StarfishMediaAPIs constructor threw std::exception: %s", e.what());
        *why = std::string("StarfishMediaAPIs threw: ") + e.what();
        return false;
    } catch (...) {
        vlog("StarfishMediaAPIs constructor threw a non-std exception");
        *why = "StarfishMediaAPIs threw a non-std exception";
        return false;
    }
#else
    im->player = make_player();
#endif
    vlog("StarfishMediaAPIs constructed (%p)", static_cast<void *>(im->player));
    if (!im->player) {
        // NOTE: libplayerAPIs is documented to call exit() itself rather than return when it cannot make
        // its connection, so reaching here is already a result (see the harness report).
        *why = "could not construct StarfishMediaAPIs";
        vlog("start: %s", why->c_str());
        return false;
    }

    // URI transport: the pipeline fetches the URL itself. BUFFERSTREAM would need Feed() calls from us,
    // and with no data fed a "no frame" answer would have been a false negative.
    const std::string window_id = std::to_string(im->window);
    char payload[2048];
    std::snprintf(payload, sizeof payload,
                  "{\"args\":[{\"mediaTransportType\":\"URI\",\"option\":{\"appId\":\"%s\",\"windowId\":\"%s\","
                  "\"payload\":\"%s\",\"transmission\":{\"trickPlay\":0}}}]}",
                  WPEB_APP_ID_STRING, window_id.c_str(), im->url.c_str());
    vlog("Load payload: %s", payload);
    vlog("calling Load...");
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS)
    bool ok = false;
    try {
        ok = im->player->Load(payload, starfish_event);
    } catch (const std::exception &e) {
        vlog("Load threw std::exception: %s", e.what());
        *why = std::string("Load threw: ") + e.what();
        return false;
    } catch (...) {
        vlog("Load threw a non-std exception");
        *why = "Load threw a non-std exception";
        return false;
    }
#else
    const bool ok = im->player->Load(payload, starfish_event);
#endif
    im->load_rc = ok ? 0 : -1;
    vlog("Load -> %s (rc=%d), windowId=%s", ok ? "true" : "false", im->load_rc, window_id.c_str());
    if (!ok) {
        *why = "StarfishMediaAPIs::Load rejected the URI payload";
        return false;
    }
    im->player->setWindowId(window_id.c_str());
    im->started_s = monotonic_s();
    return true;
}

bool VtTextureProbe::update(int *w, int *h) {
    auto *im = static_cast<VtImpl *>(impl_);
    if (!im || im->ctx < 0) return false;
    if (tex_ == 0) {
        glGenTextures(1, &tex_);
        glBindTexture(GL_TEXTURE_2D, tex_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, im->res.w, im->res.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        vlog("created GL texture %u (%dx%d)", tex_, im->res.w, im->res.h);
    }
    ++im->render_calls;
    VT_OUTPUT_INFO_T info{};
    const int rc = im->VT_UpdateOutputTexture ? im->VT_UpdateOutputTexture(im->ctx, tex_, &info) : -1;
    im->last_update_rc = rc;

    // Frame counting: a frame is NEW when libvt posted another VT_AVAILABLE since the last update, not when
    // update() was merely called (at 60 Hz UI that would report ~60 fps for a single still frame).
    if (g_events != im->prev_events) {
        im->prev_events = g_events;
        ++im->frames_seen;
    }
    events_ = g_events;
    playing_ = g_playing != 0;
    load_ok_ = g_load_completed != 0;
    frames_ = im->frames_seen;

    if (rc != 0) return false;
    if (info.activeRegion.w == 0 || info.activeRegion.h == 0) return false;
    ++im->frames_read;
    const double dt = monotonic_s() - im->started_s;
    fps_ = dt > 0 ? static_cast<double>(im->frames_seen) / dt : 0;
    if (w) *w = info.activeRegion.w;
    if (h) *h = info.activeRegion.h;
    return true;
}

bool VtTextureProbe::dump_png(const char *path, int w, int h) {
    auto *im = static_cast<VtImpl *>(impl_);
    if (!im) return false;
    const double cpu = cpu_s() - im->cpu_at_start;
    const double wall = monotonic_s() - im->started_s;
    // Report everything the verdict depends on: render calls vs real frames, Load rc, PLAYING, FBO status.
    vlog("RESULT render_calls=%d new_frames=%d frames_read=%d vt_events=%d update_rc=%d load_rc=%d "
         "load_completed=%d playing=%d fps=%.2f wall=%.1fs cpu=%.2fs (%.1f%%) tex=%u",
         im->render_calls, im->frames_seen, im->frames_read, g_events, im->last_update_rc, im->load_rc,
         g_load_completed ? 1 : 0, g_playing ? 1 : 0, fps_, wall, cpu, wall > 0 ? 100.0 * cpu / wall : 0.0, tex_);

    if (w <= 0 || h <= 0 || tex_ == 0) {
        vlog("no frame to dump (texture %u, %dx%d)", tex_, w, h);
        return false;
    }

    // glReadPixels reads the BOUND FRAMEBUFFER, not the bound texture (GLES2 has no glGetTexImage), so
    // attach tex_ to a temporary FBO, read it, and restore the previous binding.
    GLint prev_fbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex_, 0);
    const GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    vlog("FBO status = 0x%x (%s)", st, st == GL_FRAMEBUFFER_COMPLETE ? "COMPLETE" : "INCOMPLETE");
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        // This IS an answer: a libvt-filled texture that is not colour-renderable tells us the frames (if any)
        // cannot be read back this way, independently of whether they arrived.
        vlog("FBO INCOMPLETE -> readback impossible; the frame status above still stands");
        glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prev_fbo));
        glDeleteFramebuffers(1, &fbo);
        return false;
    }

    std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    const GLenum err = glGetError();
    if (err != GL_NO_ERROR) vlog("glReadPixels error 0x%x", err);
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prev_fbo));
    glDeleteFramebuffers(1, &fbo);

    // Content check: a texture libvt never filled is all zeros. Report it so an all-black PNG is not
    // mistaken for a black video frame.
    size_t nonzero = 0;
    for (size_t i = 0; i < px.size(); i += 4)
        if (px[i] || px[i + 1] || px[i + 2]) ++nonzero;
    vlog("readback: %zu of %zu pixels non-zero", nonzero, px.size() / 4);

    FILE *f = fopen(path, "wb");
    if (!f) {
        vlog("cannot write %s", path);
        return false;
    }
    auto be32 = [](uint8_t *p, uint32_t v) {
        p[0] = static_cast<uint8_t>(v >> 24), p[1] = static_cast<uint8_t>(v >> 16);
        p[2] = static_cast<uint8_t>(v >> 8), p[3] = static_cast<uint8_t>(v);
    };
    static uint32_t crc_table[256];
    static bool crc_init = false;
    if (!crc_init) {
        for (uint32_t n = 0; n < 256; ++n) {
            uint32_t c = n;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            crc_table[n] = c;
        }
        crc_init = true;
    }
    auto crc = [](const uint8_t *p, size_t n) {
        uint32_t c = 0xFFFFFFFFu;
        for (size_t i = 0; i < n; ++i) c = crc_table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
        return c ^ 0xFFFFFFFFu;
    };
    auto chunk = [&](const char *type, const std::vector<uint8_t> &data) {
        uint8_t len[4];
        be32(len, static_cast<uint32_t>(data.size()));
        fwrite(len, 1, 4, f);
        std::vector<uint8_t> body(type, type + 4);
        body.insert(body.end(), data.begin(), data.end());
        fwrite(body.data(), 1, body.size(), f);
        uint8_t c[4];
        be32(c, crc(body.data(), body.size()));
        fwrite(c, 1, 4, f);
    };
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    std::vector<uint8_t> ihdr(13);
    be32(&ihdr[0], static_cast<uint32_t>(w));
    be32(&ihdr[4], static_cast<uint32_t>(h));
    ihdr[8] = 8, ihdr[9] = 6;
    chunk("IHDR", ihdr);
    std::vector<uint8_t> raw;
    raw.reserve(static_cast<size_t>(h) * (1 + static_cast<size_t>(w) * 4));
    for (int y = h - 1; y >= 0; --y) {
        raw.push_back(0);
        raw.insert(raw.end(), &px[static_cast<size_t>(y) * w * 4],
                   &px[static_cast<size_t>(y) * w * 4 + static_cast<size_t>(w) * 4]);
    }
    std::vector<uint8_t> z{0x78, 0x01};
    size_t off = 0;
    while (off < raw.size()) {
        const size_t n = std::min<size_t>(65535, raw.size() - off);
        const bool last = off + n >= raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back(static_cast<uint8_t>(n & 0xFF));
        z.push_back(static_cast<uint8_t>(n >> 8));
        z.push_back(static_cast<uint8_t>(~n & 0xFF));
        z.push_back(static_cast<uint8_t>((~n >> 8) & 0xFF));
        z.insert(z.end(), raw.begin() + off, raw.begin() + off + n);
        off += n;
    }
    uint32_t a = 1, b = 0;
    for (uint8_t v : raw) {
        a = (a + v) % 65521;
        b = (b + a) % 65521;
    }
    const uint32_t adler = (b << 16) | a;
    z.push_back(static_cast<uint8_t>(adler >> 24));
    z.push_back(static_cast<uint8_t>(adler >> 16));
    z.push_back(static_cast<uint8_t>(adler >> 8));
    z.push_back(static_cast<uint8_t>(adler));
    chunk("IDAT", z);
    chunk("IEND", std::vector<uint8_t>{});
    fclose(f);
    vlog("wrote %s (%dx%d)", path, w, h);
    return true;
}

void VtTextureProbe::stop() {
    auto *im = static_cast<VtImpl *>(impl_);
    if (!im) return;
    if (im->player) {
        im->player->Unload();
        im->player->~StarfishMediaAPIs();
        std::free(im->player);
        im->player = nullptr;
    }
    if (im->VT_DestroyContext && im->ctx >= 0) im->VT_DestroyContext(im->ctx);
    if (im->VT_ReleaseVideoWindowResource && im->window >= 0 && im->resource >= 0)
        im->VT_ReleaseVideoWindowResource(im->window, im->resource);
    if (im->VT_DestroyVideoWindow && im->window >= 0) im->VT_DestroyVideoWindow(im->window);
    if (im->lib) dlclose(im->lib);
    delete im;
    impl_ = nullptr;
}

}  // namespace browser

#endif  // WNK_SDL_WEBOS
