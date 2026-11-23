// SPDX-License-Identifier: GPL-3.0-or-later
#include "page_view.hpp"

#include "wnk/log.hpp"
#include "wnk/ui/ui.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace browser {
namespace {

constexpr const char *kCat = "page";

// The libvt experiment switch, resolved ONCE per process: this sits in the render loop, so a per-frame
// std::getenv is exactly what the reviewer flagged. Empty or "0" = the experiment is off.
const char *vt_texture_switch() {
    static const char *const value = std::getenv("WPE_BROWSER_DEBUG_VT_TEXTURE");
    return value;
}

GLuint compile(GLenum type, const char *src) {
    const GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    return s;
}

}  // namespace

PageView::PageView(EngineClient &engine) : engine_(engine) {
    set_focusable(true);
    name = "page";
}

PageView::~PageView() {
    if (const GLuint vt_tex = vt_ ? vt_->texture() : 0; vt_tex != 0) glDeleteTextures(1, &vt_tex);
    vt_.reset();
    if (tex_ != 0) glDeleteTextures(1, &tex_);
    if (program_ != 0) glDeleteProgram(program_);
}

bool PageView::build() {
    // WebKit's pixels are B,G,R,A bytes; GLES2 has no BGRA upload format everywhere, so the texture is
    // filled as RGBA and the shader swaps the channels back.
    const char *vs =
        "attribute vec2 p; attribute vec2 t; varying vec2 vt;"
        "void main() { vt = t; gl_Position = vec4(p, 0.0, 1.0); }";
    // Compositing, correct by default with no launch parameter:
    //   * inside the video "hole"  -> straight alpha, so the hardware video plane under the UI shows through;
    //   * outside the hole         -> the page premultiplied over white at alpha 1, so a page with no background
    //                                of its own stays white instead of turning black or see-through.
    // WebKit's SHM bitmap is premultiplied (AcceleratedSurface) while the kit blends straight alpha
    // (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA), hence the un-premultiply inside the hole. `hole` is in texture
    // (uv) coordinates; a zero-size hole means there is no video area and the whole page takes the outside path.
    const char *fs =
        "precision mediump float; varying vec2 vt; uniform sampler2D tex;"
        "uniform vec4 hole; uniform float has_hole;"
        "void main() {"
        "  vec4 c = texture2D(tex, vt).bgra;"          // premultiplied: c.rgb <= c.a
        "  float inside = has_hole"
        "    * step(hole.x, vt.x) * step(vt.x, hole.x + hole.z)"
        "    * step(hole.y, vt.y) * step(vt.y, hole.y + hole.w);"
        "  vec3 straight = c.a > 0.0 ? c.rgb / c.a : vec3(0.0);"
        "  vec4 hole_out = vec4(straight, c.a);"                       // let the plane show through
        "  vec4 opaque_out = vec4(c.rgb + (1.0 - c.a) * vec3(1.0), 1.0);"  // over white, opaque
        "  gl_FragColor = mix(opaque_out, hole_out, inside);"

        "}";
    program_ = glCreateProgram();
    const GLuint v = compile(GL_VERTEX_SHADER, vs), f = compile(GL_FRAGMENT_SHADER, fs);
    glAttachShader(program_, v);
    glAttachShader(program_, f);
    glBindAttribLocation(program_, 0, "p");
    glBindAttribLocation(program_, 1, "t");
    glLinkProgram(program_);
    glDeleteShader(v);
    glDeleteShader(f);
    GLint ok = 0;
    glGetProgramiv(program_, GL_LINK_STATUS, &ok);
    if (ok == 0) {
        WNK_LOGE(kCat, "page shader did not link");
        glDeleteProgram(program_);
        program_ = 0;
        return false;
    }
    u_tex_ = glGetUniformLocation(program_, "tex");
    u_hole_ = glGetUniformLocation(program_, "hole");
    u_has_hole_ = glGetUniformLocation(program_, "has_hole");
    glGenTextures(1, &tex_);
    glBindTexture(GL_TEXTURE_2D, tex_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return true;
}

void PageView::upload(const EngineFrame &f) {
    glBindTexture(GL_TEXTURE_2D, tex_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    const bool packed = f.stride == f.width * 4;
    if (f.width != tex_w_ || f.height != tex_h_) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, static_cast<GLsizei>(f.width), static_cast<GLsizei>(f.height), 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, packed ? f.pixels : nullptr);
        if (!packed) {
            for (uint32_t y = 0; y < f.height; ++y)
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, static_cast<GLint>(y), static_cast<GLsizei>(f.width), 1, GL_RGBA,
                                GL_UNSIGNED_BYTE, f.pixels + static_cast<size_t>(y) * f.stride);
        }
        tex_w_ = f.width;
        tex_h_ = f.height;
        return;
    }
    // GLES2 cannot upload a sub-rectangle of a wider row (no UNPACK_ROW_LENGTH), so upload the full-width
    // band of rows covering all damage: one call when rows are packed.
    uint32_t y0 = 0, y1 = f.height;
    if (!f.damage.empty()) {
        y0 = f.height;
        y1 = 0;
        for (const WpeRect &r : f.damage) {
            const int32_t top = std::max(0, r.y), bottom = std::min<int32_t>(static_cast<int32_t>(f.height), r.y + r.height);
            if (bottom <= top) continue;
            y0 = std::min(y0, static_cast<uint32_t>(top));
            y1 = std::max(y1, static_cast<uint32_t>(bottom));
        }
        if (y1 <= y0) return;
    }
    if (packed) {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, static_cast<GLint>(y0), static_cast<GLsizei>(f.width),
                        static_cast<GLsizei>(y1 - y0), GL_RGBA, GL_UNSIGNED_BYTE, f.pixels + static_cast<size_t>(y0) * f.stride);
    } else {
        for (uint32_t y = y0; y < y1; ++y)
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, static_cast<GLint>(y), static_cast<GLsizei>(f.width), 1, GL_RGBA,
                            GL_UNSIGNED_BYTE, f.pixels + static_cast<size_t>(y) * f.stride);
    }
}

void PageView::render_gl(const wnk::render::CustomTarget &, double now_ms) {
    if (direct_) {
        // Alpha 0 lets the page surface below the window show; the draw call's scissor limits it to this view.
        const bool hole = tab_ != 0 && direct_tab_ == tab_;
        glClearColor(hole ? 0 : 1, hole ? 0 : 1, hole ? 0 : 1, hole ? 0 : 1);
        glClear(GL_COLOR_BUFFER_BIT);
        return;
    }
    if (program_ == 0 && !build()) return;
    // EXPERIMENT: WPE_BROWSER_DEBUG_VT_TEXTURE=<media url> draws libvt's texture instead of the engine's frames,
    // to answer whether the app that owns the video gets frames as a GL texture (vt_texture_probe.cpp).
    {
        // The probe logs through the app's logger; raw stderr does not reach events.log on this platform.
        static const bool sink_installed = [] {
            set_vt_log_fn([](const char *line) { WNK_LOGI("page", "vt-probe: %s", line); });
            return true;
        }();
        (void)sink_installed;
        // The switch comes from the environment (once, see vt_texture_switch) or the launch parameter.
        // (not cached: the launch parameter may be set after the first frame; an empty std::string copy allocates nothing)
        const char *const vt_env = vt_texture_switch();
        const std::string vturl = (vt_env && *vt_env) ? std::string(vt_env) : vt_texture_url();
        if (!vturl.empty() && vturl != "0") {
            if (!vt_failed_) render_vt(vturl.c_str(), now_ms);
            return;
        }
    }
    if (const EngineFrame *f = engine_.frame()) {
        if (f->tab == tab_) upload(*f);
        engine_.frame_done();
    }
    // Clear transparent while a video plane is showing through this view: an opaque clear here would wipe
    // the coverage the kit accumulated for the hole before the page quad is drawn.
    if (clear_transparent_) glClearColor(0, 0, 0, 0);
    else glClearColor(1, 1, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    if (tex_w_ == 0) return;

    // Full-view quad; texture row 0 is the top of the page.
    static const GLfloat quad[] = {
        -1, -1, 0, 1,  //
        1,  -1, 1, 1,  //
        -1, 1,  0, 0,  //
        1,  1,  1, 0,
    };
    glUseProgram(program_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex_);
    glUniform1i(u_tex_, 0);
    // The hole arrives in the pixels of the buffer WebKit painted (the same pixels the texture holds), but `vt` is a
    // texture coordinate, so divide by the TEXTURE size. It must not be scaled by texture/page size: that factor is
    // the engine's device scale (1.5), and applying it to a rectangle already in device pixels put the hole at 2.25x the
    // CSS position instead of 1.5x (TV, 2026-11-20: video at CSS (137,211) showed through from (308,563), not (205,404)).
    const bool has_hole = hole_w_ > 0 && hole_h_ > 0 && tex_w_ > 0 && tex_h_ > 0;
    if (has_hole) {
        const auto tw = static_cast<float>(tex_w_), th = static_cast<float>(tex_h_);
        glUniform4f(u_hole_, hole_x_ / tw, hole_y_ / th, hole_w_ / tw, hole_h_ / th);
        glUniform1f(u_has_hole_, 1.0f);
    } else {
        glUniform4f(u_hole_, 0.0f, 0.0f, 0.0f, 0.0f);
        glUniform1f(u_has_hole_, 0.0f);
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), quad);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), quad + 2);
    glDisable(GL_BLEND);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
}

// EXPERIMENT (WPE_BROWSER_DEBUG_VT_TEXTURE=<url>): show libvt's texture, and after ~6 s dump one frame plus a
// report (frames, VT return codes, fps, CPU) to /media/developer/temp/video/.
void PageView::render_vt(const char *url, double now_ms) {
    if (vt_started_ms_ == 0) {
        vt_started_ms_ = now_ms;
        fprintf(stderr, "[vt-probe] render_vt entered, url=%s\n", url);
        fflush(stderr);
    }
    if (!vt_) {
        vt_ = std::make_unique<VtTextureProbe>();
        std::string why;
        const bool ok = vt_->start(url, &why);
        fprintf(stderr, "[vt-probe] start() -> %s%s%s\n", ok ? "OK" : "FAILED", ok ? "" : ": ", ok ? "" : why.c_str());
        fflush(stderr);
        if (!ok) {
            vt_.reset();
            vt_failed_ = true;
            return;
        }
    }
    int w = 0, h = 0;
    if (vt_->update(&w, &h)) {
        if (vt_w_ == 0) {
            fprintf(stderr, "[vt-probe] first frame %dx%d\n", w, h);
            fflush(stderr);
        }
        vt_w_ = w, vt_h_ = h;
    }
    if (!vt_dumped_ && now_ms - vt_started_ms_ > 6000) {
        vt_dumped_ = true;
        vt_->dump_png("/media/developer/temp/video/vt-texture.png", vt_w_, vt_h_);
        fprintf(stderr, "[vt-probe] RESULT frames=%d events=%d fps=%.2f tex_w=%d tex_h=%d\n",
                vt_->frames(), vt_->events_available(), vt_->fps(), vt_w_, vt_h_);
        fflush(stderr);
    }
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    if (vt_->texture() == 0) return;
    static const GLfloat quad[] = {-1, -1, 0, 1, 1, -1, 1, 1, -1, 1, 0, 0, 1, 1, 1, 0};
    glUseProgram(program_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, vt_->texture());
    glUniform1i(u_tex_, 0);
    glUniform1f(u_has_hole_, 0.0f);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), quad);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), quad + 2);
    glDisable(GL_BLEND);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
}

bool PageView::to_page(wnk::Point l, int32_t &x, int32_t &y) const {
    const wnk::Rect r = screen_rect();
    if (r.w <= 0 || r.h <= 0 || l.x < r.x || l.y < r.y || l.x >= r.x + r.w || l.y >= r.y + r.h) return false;
    x = static_cast<int32_t>((l.x - r.x) * static_cast<float>(page_w_) / r.w);
    y = static_cast<int32_t>((l.y - r.y) * static_cast<float>(page_h_) / r.h);
    return true;
}

void PageView::ime(wnk::ui::Ui &ui, bool focused) {
    if (focused && !editing_) {
        editing_ = true;
        ime_run_.clear();
        ui.begin_text_input(this);
    } else if (!focused && editing_) {
        editing_ = false;
        ime_run_.clear();
        ui.end_text_input(this);
    }
}

// How long after a click/OK a field's focus counts as the viewer's doing.
static constexpr double kUserFocusWindowMs = 1500;
// After a click/OK on the page with a field already focused: wait this long for the focus to move elsewhere.
static constexpr double kOpenDelayMs = 300;

void PageView::field_focus(wnk::ui::Ui &ui, bool focused, double now_ms) {
    field_focused_ = focused;
    if (!focused) {
        open_at_ms_ = 0;
        ime(ui, false);
        return;
    }
    if (now_ms - last_user_ms_ <= kUserFocusWindowMs) {
        open_at_ms_ = 0;
        ime(ui, true);
    }
}

void PageView::user_activated(double now_ms) {
    last_user_ms_ = now_ms;
    if (field_focused_ && !editing_) open_at_ms_ = now_ms + kOpenDelayMs;
}

void PageView::tick(wnk::ui::Ui &ui, double now_ms) {
    if (open_at_ms_ <= 0 || now_ms < open_at_ms_) return;
    open_at_ms_ = 0;
    if (field_focused_ && !editing_) ime(ui, true);
}

static size_t utf8_code_points(const std::string &s) {
    size_t n = 0;
    for (unsigned char c : s)
        if ((c & 0xC0) != 0x80) ++n;
    return n;
}

bool PageView::on_text(wnk::ui::Ui &ui, const std::string &utf8) {
    // LG's remote keyboards (the phone app) type through the TV's input method in replace mode: every update carries
    // the whole text typed so far ("a", "ab", "abc") and replaces the previous update. SDL hands it over as plain text,
    // so the page got "aababc" (TV, 2026-11-23: com.webos.service.ime insertText with replace:true; reported on a C9).
    // An update that extends the last one, or is a shorter start of it, is turned back into the difference. Ordinary
    // typing sends one character at a time and never matches.
    const double now = ui.now_ms();
    if (!ime_run_.empty() && now - ime_run_ms_ < 15000 && utf8 != ime_run_) {
        if (utf8.size() > ime_run_.size() && utf8.compare(0, ime_run_.size(), ime_run_) == 0) {
            engine_.commit_text(utf8.substr(ime_run_.size()));
            ime_run_ = utf8, ime_run_ms_ = now;
            return true;
        }
        if (!utf8.empty() && utf8.size() < ime_run_.size() && ime_run_.compare(0, utf8.size(), utf8) == 0 &&
            utf8_code_points(ime_run_) > 1) {
            for (size_t i = utf8_code_points(ime_run_.substr(utf8.size())); i > 0; --i) {
                engine_.key(true, 0xff08, 14 + 8, 0);  // Backspace
                engine_.key(false, 0xff08, 14 + 8, 0);
            }
            ime_run_ = utf8, ime_run_ms_ = now;
            return true;
        }
    }
    engine_.commit_text(utf8);
    ime_run_ = utf8, ime_run_ms_ = now;
    return true;
}

bool PageView::on_edit_key(wnk::ui::Ui &, wnk::input::EditKey key) {
    ime_run_.clear();  // the field was edited another way: a later remote update is new text again
    using K = wnk::input::EditKey;
    uint32_t keysym = 0, evdev = 0;
    switch (key) {
    case K::Backspace: keysym = 0xff08, evdev = 14; break;
    case K::Delete: keysym = 0xffff, evdev = 111; break;
    case K::Left: keysym = 0xff51, evdev = 105; break;
    case K::Right: keysym = 0xff53, evdev = 106; break;
    case K::Home: keysym = 0xff50, evdev = 102; break;
    case K::End: keysym = 0xff57, evdev = 107; break;
    case K::Enter: keysym = 0xff0d, evdev = 28; break;
    case K::Clear: return true;  // no page equivalent
    }
    engine_.key(true, keysym, evdev + 8, 0);
    engine_.key(false, keysym, evdev + 8, 0);
    return true;
}

void PageView::on_text_input_closed(wnk::ui::Ui &) {
    // The viewer closed the TV keyboard; the page field keeps focus until the user moves on (OK or a click opens it again).
    editing_ = false;
    ime_run_.clear();
}

}  // namespace browser
