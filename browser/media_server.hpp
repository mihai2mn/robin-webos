// SPDX-License-Identifier: GPL-3.0-or-later
// Hardware video for pages (common/wpe_media_ipc.h): the web process's video sink sends the encoded stream here,
// and this process feeds LG's decoder through the kit's NdlBackend. LG's media stack registers bus names under
// the app's luna role, which is bound to this executable, so the decoder cannot live in WPEWebProcess.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "lg_media_player.hpp"
#include "wnk/geometry.hpp"
#include "wpe_media_ipc.h"
#include <chrono>

namespace wnk::media { class MediaBackend; class NdlBackend; class StarfishBackend; }

namespace browser {

class MediaServer {
public:
    MediaServer();
    ~MediaServer();
    MediaServer(const MediaServer &) = delete;
    MediaServer &operator=(const MediaServer &) = delete;

    // Listens on `path` if LG's decoder library is there; false with *why otherwise.
    // video_window: the compositor offers video windows, without which LG's media pipeline cannot show video.
    // backend: "starfish" (default: LG's buffer-feed pipeline with its own exported video window, which reorders B-frames),
    // "ndl" (NDL direct media: low-delay decoder, B-frames out of order), or "" for the default. Starfish falls back to NDL
    // when it is not available. WPEB_STARFISH=0|1 (debug) overrides.
    bool start(const std::string &path, bool video_window, std::string *why, const std::string &backend = "");
    void stop();
    bool running() const { return listen_fd_ >= 0; }
    // The decoder takes the page's AAC audio too (Starfish backend): the engine is told so (WPEB_MEDIA_HW_AUDIO).
    bool takes_audio() const { return starfish_ != nullptr; }
    const std::string &path() const { return path_; }
    // A page's video is on LG's decoder right now (any thread).
    bool video_active() const { return owner_.load() != 0; }
    // App lifecycle: LG's decoder must know when the app leaves the foreground.
    static void set_foreground(bool foreground);
    // Page -> screen mapping for the video area: WebKit reports the video rectangle in page-surface pixels
    // (see engine/gst/wpebmediasink.c, the sink's "rectangle" property), while the decoder wants drawable
    // pixels and our window scrolls the page inside the view. Called on the main thread on every resize.
    void set_page_mapping(float x, float y, float scale_x, float scale_y, float view_w = 0, float view_h = 0);
    // The page that owned the stream navigated away or its tab closed, and the engine has not said so (it keeps the old
    // page's media element alive for a while and pauses it later): unload the decoder and clear the rectangle now.
    // Any thread. Returns true if a stream was released.
    bool release_owner(const char *why);
    // UI frame loop: apply the newest video area (plane + hole) together with the page frame about to be drawn.
    void commit_area(bool frame_ready);
    wnk::Rect plane_rect(float x, float y, float w, float h) const;
    // The connection whose stream holds the decoder (0: none); changes with every new Load.
    uint64_t owner_id() const { return owner_.load(); }
    // Clears the video rectangle (owner released, or another stream took over).
    void clear_hole();
    // The video rectangle in PAGE pixels (x, y, width, height), all zero when no video area is known. This is
    // the "hole" WebKit punched in the page: the UI composites opacity outside it and transparency inside it.
    struct Hole {
        float x = 0, y = 0, width = 0, height = 0;
        bool empty() const { return width <= 0 || height <= 0; }
    };
    // Bumped on every change of the rectangle (store or clear): lets the UI notice that a new rectangle arrived.
    uint64_t hole_serial() const { return hole_serial_.load(); }
    Hole hole() const { return Hole{hole_x_.load(), hole_y_.load(), hole_w_.load(), hole_h_.load()}; }


private:
    void accept_loop();
    void serve(int fd, uint64_t id);

    std::string path_;
    int listen_fd_ = -1;
    int wake_[2] = {-1, -1};
    std::thread acceptor_;
    std::mutex mutex_;  // the backends
    // As Chromium on webOS: a stream with a video window on the page surface plays through LG's media pipeline
    // bound to it (LgMediaPlayer); without one, through NDL (whose video sits below the UI window).
    std::unique_ptr<wnk::media::MediaBackend> backend_;
    wnk::media::StarfishBackend *starfish_ = nullptr;  // backend_ when it is the Starfish one
    LgMediaPlayer lg_;
    bool use_lg_ = false;  // the owner's stream is on lg_ (else on NDL)
    bool ndl_loaded_ = false;
    // Page -> screen transform for WPEB_MEDIA_AREA (guarded by mutex_).
    // Read by the media thread from the WPEB_MEDIA_AREA handler, written by the render thread each
    // layout: four floats, so plain atomics -- a mutex here was taken every frame from the render thread.
    using Clock = std::chrono::steady_clock;
    struct PendingArea {
        WpebMediaArea area{};
        uint64_t id = 0;
        Clock::time_point at{};
        uint64_t seq = 0;
        bool valid = false;
    };
    std::mutex area_mu_;  // pending_ only
    PendingArea pending_;
    uint64_t pending_seq_ = 0;
    std::atomic<float> view_w_{0}, view_h_{0};
    std::atomic<float> page_x_{0}, page_y_{0}, page_scale_x_{1}, page_scale_y_{1};
    std::atomic<uint64_t> hole_serial_{0}, hole_id_{0};  // hole_id_: the stream that sent the stored rectangle
    std::atomic<float> hole_x_{0}, hole_y_{0}, hole_w_{0}, hole_h_{0};  // page px; read by the render thread

    std::atomic<uint64_t> owner_{0};  // the connection whose stream is loaded (latest LOAD wins)
    std::atomic<uint64_t> next_id_{1};
    std::atomic<int> connections_{0};
};

}  // namespace browser
