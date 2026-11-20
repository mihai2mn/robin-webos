// SPDX-License-Identifier: GPL-3.0-or-later
// EXPERIMENT (WPE_BROWSER_DEBUG_VT_TEXTURE=<url> / {"wpebVtTexture":"<url>"}): can the app that OWNS the
// video get its frames as a GL texture?
//
// Why this exists: the video team measured that libvt delivers nothing to a process that does not own the
// video plane, and that StarfishMediaAPIs::exit()s unless it runs inside a platform-launched app. Our UI is
// such an app, and its media server owns NDL/Starfish playback, so it is the one process where the
// "VT window -> Starfish windowId -> VT_UpdateOutputTexture" binding can be tested
// (build/video/PLAN.md 4.6.4; REPORT.md 10.6's reopening condition).
//
// Nothing here runs unless the switch is set. The normal media path is untouched.
#pragma once

#include <cstdint>
#include <string>

namespace browser {

// Where the probe's messages go. page_view.cpp installs a sink that uses the app's logger, because raw
// stderr does NOT reach the app's events.log (measured on the TV).
using VtLogFn = void (*)(const char *line);
void set_vt_log_fn(VtLogFn fn);

// EXPERIMENT: lets a launch parameter ({"wpebVtTexture":"<media url>"}) enable the probe, since a
// luna-launched app cannot be given environment variables. Empty = disabled.
void set_vt_texture_url_from_launch(const std::string &url);
std::string vt_texture_url();

// Creates a VT video window, points LG's pipeline at it, and pulls frames with VT_UpdateOutputTexture into a
// texture the caller draws. Returns false (and fills *why) if libvt/the pipeline is unavailable.
class VtTextureProbe {
public:
    VtTextureProbe();
    ~VtTextureProbe();

    // Load `url` into a VT video window and start reading frames. `*why` receives the reason on failure.
    bool start(const char *url, std::string *why);
    void stop();

    // Pull one frame into the GL texture (call on the GL thread with the context current). Returns true when a
    // non-empty frame region was reported. `*w`/*h` receive its size.
    bool update(int *w, int *h);

    // The GL texture name (0 when nothing was read yet) -- exactly what VT_GenerateTexture returns: a texture
    // in OUR context, so drawing it is zero-copy.
    uint32_t texture() const { return tex_; }
    // Real NEW frames (VT_AVAILABLE events), not render calls -- see the .cpp for why they differ.
    int frames() const { return frames_; }
    int events_available() const { return events_; }
    double fps() const { return fps_; }
    // Whether the pipeline reported LOADCOMPLETED / reached PLAYING: distinguishes "never decoded" from "no frames".
    bool load_completed() const { return load_ok_; }
    bool playing() const { return playing_; }

    // Dump the current texture to a PNG (through an FBO bound to tex_, since GLES2 cannot read a texture
    // directly) and log the report: render calls, new frames, Load rc, PLAYING, FBO status, CPU.
    bool dump_png(const char *path, int w, int h);

private:
    void *impl_ = nullptr;  // pimpl: libvt is dlopen'd and must not leak into the normal build's headers
    uint32_t tex_ = 0;
    int frames_ = 0;
    int events_ = 0;
    double fps_ = 0;
    bool load_ok_ = false;
    bool playing_ = false;
};

}  // namespace browser
