// SPDX-License-Identifier: GPL-3.0-or-later
// Shows the engine's frames: a GlView that keeps the page in one GLES2 texture, updated from the shared
// frame slots (damaged rows only), drawn scaled into the view. While a field in the page is being edited it
// holds the kit's text input, so the TV keyboard's text and editing keys reach the page.
#pragma once

#include "engine_client.hpp"
#include "vt_texture_probe.hpp"
#include "wnk/ui/widgets.hpp"

#include <GLES2/gl2.h>

#include <memory>

namespace browser {

class PageView : public wnk::ui::GlView {
 public:
    explicit PageView(EngineClient &engine);
    ~PageView() override;

    // Page size in CSS px the engine renders at (the texture is scaled to the view).
    // True while a hardware video plane is under the UI: the page is drawn with its own alpha and this view
    // must not clear opaquely, so the video shows through the hole WebKit punches.
    void set_clear_transparent(bool on) { clear_transparent_ = on; }

    // The video "hole" in page pixels (x, y, w, h); empty when there is no hardware video area. The page is
    // drawn transparent there (so the plane under the UI shows) and opaque-over-white everywhere else, which
    // keeps a page without a background of its own white -- correct by default, no launch parameter needed.
    void set_hole(float x, float y, float w, float h) {
        if (x == hole_x_ && y == hole_y_ && w == hole_w_ && h == hole_h_) return;
        hole_x_ = x, hole_y_ = y, hole_w_ = w, hole_h_ = h;
        invalidate_content();
    }


    void set_page_size(uint32_t w, uint32_t h) { page_w_ = w, page_h_ = h; }
    uint32_t page_width() const { return page_w_; }
    uint32_t page_height() const { return page_h_; }
    // Maps a point in logical UI units to page CSS px; false if outside the view.
    bool to_page(wnk::Point logical, int32_t &x, int32_t &y) const;

    // Opens/closes the TV keyboard for the page (false: also when focus leaves the page for the browser's own UI).
    void ime(wnk::ui::Ui &ui, bool focused);
    bool editing() const { return editing_; }
    // An editable in the page gained/lost focus (the engine says so). The TV keyboard opens only when the viewer caused
    // it: a click or OK on the page just before. A field the page focuses by itself (a search page's autofocus, a test
    // page focusing inputs) stays focused without the keyboard until the viewer clicks it or presses OK.
    void field_focus(wnk::ui::Ui &ui, bool focused, double now_ms);
    // The viewer clicked the page or pressed OK on it.
    void user_activated(double now_ms);
    // Per frame: opens the keyboard shortly after a click/OK on the page while one of its fields has focus (a click on
    // the field that already has focus brings no new focus event; one elsewhere moves focus away first).
    void tick(wnk::ui::Ui &ui, double now_ms);
    bool has_frame() const { return tex_w_ > 0; }
    // Frames from other tabs are dropped; switching shows a blank page until the new tab's first frame.
    void show_tab(TabId tab) {
        if (tab != tab_) tab_ = tab, tex_w_ = tex_h_ = 0, invalidate_content();
    }
    // Direct presentation (DirectOwner): the page is its own surface below the window. Once a tab's surface has
    // shown a frame, this view becomes a transparent hole over it; until then it stays blank. The hole is a still
    // picture, so the view is static there (redrawn only when the hole changes): an animated view had the UI
    // present every frame, ~15% of a core on the TV for nothing. SHM frames arrive all the time: animated.
    void set_direct(bool on) {
        direct_ = on, direct_tab_ = 0;
        set_animated(!on);
        invalidate_content();
    }
    bool direct() const { return direct_; }
    void direct_shown(TabId tab, bool shown) {
        const TabId before = direct_tab_;
        if (shown) direct_tab_ = tab;
        else if (direct_tab_ == tab) direct_tab_ = 0;
        if (direct_tab_ != before) invalidate_content();
    }

 protected:
    void render_gl(const wnk::render::CustomTarget &target, double now_ms) override;
    bool on_text(wnk::ui::Ui &ui, const std::string &utf8) override;
    bool on_edit_key(wnk::ui::Ui &ui, wnk::input::EditKey key) override;
    void on_text_input_closed(wnk::ui::Ui &ui) override;

 private:
    bool build();
    void render_vt(const char *url, double now_ms);
    void upload(const EngineFrame &f);

    EngineClient &engine_;
    uint32_t page_w_ = 1280, page_h_ = 720;
    bool clear_transparent_ = false;
    GLuint program_ = 0, tex_ = 0;
    // EXPERIMENT (WPE_BROWSER_DEBUG_VT_TEXTURE=<url>): libvt's texture instead of the engine's frame.
    std::unique_ptr<VtTextureProbe> vt_;
    bool vt_failed_ = false, vt_dumped_ = false;
    int vt_w_ = 0, vt_h_ = 0;
    double vt_started_ms_ = 0;
    GLint u_tex_ = -1, u_hole_ = -1, u_has_hole_ = -1;
    // The hole in page pixels (empty when no video area is known).
    float hole_x_ = 0, hole_y_ = 0, hole_w_ = 0, hole_h_ = 0;
    uint32_t tex_w_ = 0, tex_h_ = 0;
    bool editing_ = false;
    bool field_focused_ = false;    // a page editable has focus (engine), keyboard open or not
    double last_user_ms_ = -1e9;    // last click/OK on the page
    double open_at_ms_ = 0;         // open the keyboard at this time if a field still has focus (0: nothing pending)
    // Text a remote keyboard typed in replace mode (see on_text) and when.
    std::string ime_run_;
    double ime_run_ms_ = 0;
    TabId tab_ = 0;
    bool direct_ = false;
    TabId direct_tab_ = 0;
};

}  // namespace browser
