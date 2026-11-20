// SPDX-License-Identifier: GPL-3.0-or-later
#include "media_server.hpp"

#include "wpe_media_ipc.h"
#include "wnk/log.hpp"
#include "wnk/media/ndl_backend.hpp"
#include "wnk/media/starfish_backend.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace browser {
namespace {

constexpr const char *kCat = "media";

bool read_all(int fd, void *data, size_t size) {
    auto *p = static_cast<uint8_t *>(data);
    while (size) {
        const ssize_t n = ::recv(fd, p, size, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        p += n;
        size -= static_cast<size_t>(n);
    }
    return true;
}

const char *codec_label(uint32_t c) {
    switch (c) {
    case WPEB_MEDIA_H264: return "H.264";
    case WPEB_MEDIA_HEVC: return "HEVC";
    case WPEB_MEDIA_VP9: return "VP9";
    case WPEB_MEDIA_AV1: return "AV1";
    default: return "?";
    }
}

}  // namespace

MediaServer::MediaServer() = default;

MediaServer::~MediaServer() { stop(); }

void MediaServer::set_foreground(bool foreground) { wnk::media::NdlBackend::set_app_foreground(foreground); }

// Drop the video rectangle. Called whenever the stream that owned it goes away or another takes over, so a
// stale hole cannot keep the page transparent where no video is any more.
void MediaServer::clear_hole() {
    hole_x_.store(0);
    hole_y_.store(0);
    hole_w_.store(0);
    hole_h_.store(0);
    hole_id_.store(0);
    ++hole_serial_;
}


void MediaServer::set_page_mapping(float x, float y, float scale_x, float scale_y, float view_w, float view_h) {
    view_w_.store(view_w);
    view_h_.store(view_h);
    // Called from the render thread on every layout: no mutex (the media thread reads these in the AREA
    // handler, and four float stores need no lock). Same treatment as the hole fields above.
    page_x_.store(x);
    page_y_.store(y);
    page_scale_x_.store(scale_x > 0 ? scale_x : 1);
    page_scale_y_.store(scale_y > 0 ? scale_y : 1);
}

bool MediaServer::start(const std::string &path, bool video_window, std::string *why, const std::string &backend) {
    if (running()) return true;
    // LG's media pipeline (StarfishMediaAPIs) is the main path; NDL's v2 API (webOS 5+) only a fallback. webOS 3.x
    // and 4.x have the pipeline but an older NDL without NDL_DirectMediaLoad. The pipeline shows video in a video
    // window (wl_webos_foreign), which webOS 3.x's compositor lacks: there neither path works, so pages get
    // software video instead (issue #2: on an OLED B7, webOS 3.9, every video failed).
    std::string ndl_why;
    const bool ndl = wnk::media::NdlBackend::available(&ndl_why);
    const bool lg = LgMediaPlayer::available();
    if (!ndl && !lg) {
        *why = "no LG media pipeline (" + LgMediaPlayer::unavailable_reason() + ") and " + ndl_why;
        return false;
    }
    if (!ndl && !video_window) {
        *why = "no video windows (wl_webos_foreign) for LG's media pipeline and " + ndl_why;
        return false;
    }
    if (!ndl) WNK_LOGI(kCat, "NDL fallback unavailable (%s); LG media pipeline only", ndl_why.c_str());
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) {
        *why = "socket path too long";
        return false;
    }
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    ::unlink(path.c_str());
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0 || ::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 || ::listen(fd, 4) != 0 ||
        ::pipe2(wake_, O_CLOEXEC) != 0) {
        *why = std::string("media socket: ") + std::strerror(errno);
        if (fd >= 0) ::close(fd);
        return false;
    }
    path_ = path;
    listen_fd_ = fd;
    // Starfish (LG's buffer-feed pipeline, its own exported video window, a reordering decoder) unless asked for NDL: NDL's
    // decoder is low-delay and shows B-frames out of order. The exported window is created here, on the main thread.
    bool want_starfish = backend != "ndl";
    if (const char *e = std::getenv("WPEB_STARFISH")) want_starfish = std::strcmp(e, "0") != 0;
    else if (::access("/tmp/wpeb-starfish", F_OK) == 0) want_starfish = true;
    std::string sf_why;
    if (want_starfish && wnk::media::StarfishBackend::available(&sf_why)) {
        auto sf = std::make_unique<wnk::media::StarfishBackend>();
        starfish_ = sf.get();
        WNK_LOGI(kCat, "video through the Starfish pipeline (video plane: %s)",
                 sf->plane() == wnk::media::VideoPlane::ExportedWindow ? "exported window"
                 : sf->plane() == wnk::media::VideoPlane::Acb          ? "ACB"
                                                                       : "none");
        backend_ = std::move(sf);
    } else {
        if (want_starfish) WNK_LOGW(kCat, "Starfish pipeline unavailable (%s): NDL", sf_why.c_str());
        backend_ = std::make_unique<wnk::media::NdlBackend>();
    }
    acceptor_ = std::thread([this] { accept_loop(); });
    WNK_LOGI(kCat, "hardware video service on %s", path.c_str());
    return true;
}

// NDL_DirectVideoSetArea takes only a rectangle INSIDE the screen: any rectangle reaching past an edge (y < 0 as the
// page scrolls the video up, a bottom beyond 1080, x < 0 or beyond 1920) makes the TV show a wrong picture or black with
// a brightness jump (TV, 2026-11-20), and libNDL_directmedia has no crop or source-area call (its exports: Init, Load,
// Unload, VideoPlay, VideoSetArea, SetHDRInfo, ...). So the plane is clipped to the screen; a clipped video is scaled
// into the visible part (squashed, not cut). Entirely outside the page viewport: parked as a tiny rectangle at the
// top-left corner, where the toolbar or the opaque page covers it.
wnk::Rect MediaServer::plane_rect(float x, float y, float w, float h) const {
    constexpr float kScreenW = 1920, kScreenH = 1080;
    const float vx = page_x_.load(), vy = page_y_.load(), vw = view_w_.load(), vh = view_h_.load();
    const float px1 = vw > 0 ? std::min(vx + vw, kScreenW) : kScreenW, py1 = vh > 0 ? std::min(vy + vh, kScreenH) : kScreenH;
    const float px0 = std::max(vx, 0.0f), py0 = std::max(vy, 0.0f);
    if (x >= px1 || y >= py1 || x + w <= px0 || y + h <= py0) return wnk::Rect{0, 0, 16, 9};  // not visible: parked
    const float x0 = std::max(x, 0.0f), y0 = std::max(y, 0.0f);
    const float x1 = std::min(x + w, kScreenW), y1 = std::min(y + h, kScreenH);
    return wnk::Rect{x0, y0, std::max(x1 - x0, 2.0f), std::max(y1 - y0, 2.0f)};
}

// Called by the UI on its frame loop: `frame_ready` = an engine frame of the shown tab is about to be drawn. Applies
// the newest AREA then (or after kMaxAge without a frame: a static page whose video moved). Never waits for the feeder.
void MediaServer::commit_area(bool frame_ready) {
    PendingArea p;
    {
        std::lock_guard<std::mutex> al(area_mu_);
        if (!pending_.valid) return;
        if (!frame_ready && Clock::now() - pending_.at < std::chrono::milliseconds(40)) return;
        p = pending_;
    }
    std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock()) return;  // the feeder holds it: the next frame
    {
        std::lock_guard<std::mutex> al(area_mu_);
        if (pending_seq_ == p.seq) pending_.valid = false;
    }
    const WpebMediaArea &a = p.area;
    const auto scx = page_scale_x_.load(), scy = page_scale_y_.load();
    const auto sx = page_x_.load() + static_cast<float>(a.x) * scx;
    const auto sy = page_y_.load() + static_cast<float>(a.y) * scy;
    const auto sw = static_cast<float>(a.width) * scx;
    const auto sh = static_cast<float>(a.height) * scy;
    static const bool area_debug = std::getenv("WPE_BROWSER_DEBUG_MEDIA_AREA") != nullptr;
    if (area_debug)
        fprintf(stderr, "[page-area] webkit=(%d,%d %dx%d) origin=(%.1f,%.1f) scale=(%.4f,%.4f) -> area=(%.1f,%.1f %.1fx%.1f)\n",
                a.x, a.y, a.width, a.height, page_x_.load(), page_y_.load(), scx, scy, sx, sy, sw, sh);
    // Only the owner's stream drives the plane and the hole: another tab's sink must not repaint them.
    const bool from_owner = owner_.load() == p.id || owner_.load() == 0;
    if (backend_ && a.width > 0 && a.height > 0 && from_owner && !use_lg_) {
        const wnk::Rect vis = plane_rect(sx, sy, sw, sh);
        // Starfish's exported window takes a source rectangle: a video partly off the screen is cut by the compositor,
        // not squashed into the visible part (NDL has no such call). A parked (not visible) video keeps the whole picture.
        const bool parked = vis.w <= 16 && vis.h <= 9 && vis.x == 0 && vis.y == 0 && (sx > 16 || sy > 9 || sw > 16 || sh > 9);
        if (starfish_ && !parked && sw > 0 && sh > 0)
            starfish_->set_area_cropped(vis, (vis.x - sx) / sw, (vis.y - sy) / sh, vis.w / sw, vis.h / sh);
        else
            backend_->set_area(vis);
    }
    if (from_owner) {
        // The same rectangle in PAGE pixels: page_view composites opacity outside it and transparency inside it.
        hole_x_.store(static_cast<float>(a.x));
        hole_y_.store(static_cast<float>(a.y));
        hole_w_.store(static_cast<float>(a.width));
        hole_h_.store(static_cast<float>(a.height));
        hole_id_.store(p.id);
        ++hole_serial_;
    }
}

bool MediaServer::release_owner(const char *why) {
    std::lock_guard<std::mutex> lock(mutex_);
    const uint64_t id = owner_.exchange(0);
    if (id == 0) return false;
    clear_hole();
    lg_.unload();
    if (backend_ && ndl_loaded_) backend_->unload();
    use_lg_ = ndl_loaded_ = false;
    WNK_LOGI(kCat, "stream %llu released by the UI: %s", static_cast<unsigned long long>(id), why);
    return true;
}

void MediaServer::stop() {
    if (!running()) return;
    const char byte = 1;
    (void)!::write(wake_[1], &byte, 1);
    if (acceptor_.joinable()) acceptor_.join();
    ::close(listen_fd_);
    listen_fd_ = -1;
    ::unlink(path_.c_str());
    // Connections end with the web process (the engine stops first); give their threads a moment.
    for (int i = 0; i < 100 && connections_.load() > 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    std::lock_guard<std::mutex> lock(mutex_);
    lg_.unload();
    use_lg_ = ndl_loaded_ = false;
    if (connections_.load() == 0) backend_.reset();  // releases LG's decoder
    else backend_.release();                          // a thread still uses it: leaked at exit, NDL quits atexit
    ::close(wake_[0]);
    ::close(wake_[1]);
    wake_[0] = wake_[1] = -1;
}

void MediaServer::accept_loop() {
    for (;;) {
        pollfd fds[2] = {{listen_fd_, POLLIN, 0}, {wake_[0], POLLIN, 0}};
        if (::poll(fds, 2, -1) < 0) {
            if (errno == EINTR) continue;
            return;
        }
        if (fds[1].revents) return;
        if (!(fds[0].revents & POLLIN)) continue;
        const int fd = ::accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC);
        if (fd < 0) continue;
        const uint64_t id = next_id_++;
        ++connections_;
        std::thread([this, fd, id] {
            serve(fd, id);
            ::close(fd);
            --connections_;
        }).detach();
    }
}

void MediaServer::serve(int fd, uint64_t id) {
    WNK_LOGI(kCat, "stream %llu connected", static_cast<unsigned long long>(id));
    using Clock = std::chrono::steady_clock;
    std::vector<uint8_t> payload;
    std::atomic<uint64_t> units{0}, full_waits{0};
    // Access units and audio frames go to queues fed to the decoder by their own thread, as a media player's feeder
    // does: a decoder refusing data ("buffer full", e.g. while paused) never holds up the control messages behind
    // them (play, pause, seek), and a full video buffer never holds up the audio (or the other way round): LG's
    // pipeline waits for both before it plays.
    struct Unit {
        wnk::media::Packet packet;
        uint64_t generation;
        bool eos = false;  // the stream's end, behind its last unit (pts: last, so the other stream goes first)
    };
    std::mutex qm;
    std::condition_variable qcv;
    std::deque<Unit> queues[2];  // [0] video, [1] audio
    uint64_t generation = 0;     // qm; a flush or load drops what was queued before it
    bool stop = false;
    bool ready = false;          // qm; the decoder is loaded (units wait until then)
    std::atomic<bool> eos_pushed{false};  // the decoder was told the stream ended (cleared by a flush or load)
    std::thread feeder([&] {
        Clock::time_point blocked_until[2] = {};
        bool ended[2] = {};          // feeder thread; the streams whose end was reached, in eos_generation
        bool pushed_eos = false;
        uint64_t eos_generation = 0;
        std::unique_lock<std::mutex> ql(qm);
        for (;;) {
            qcv.wait(ql, [&] { return stop || (ready && (!queues[0].empty() || !queues[1].empty())); });
            if (stop) return;
            const auto now = Clock::now();
            int pick = -1;
            for (int t = 0; t < 2; ++t) {
                if (queues[t].empty() || blocked_until[t] > now) continue;
                if (pick < 0 || queues[t].front().packet.pts_us < queues[pick].front().packet.pts_us) pick = t;
            }
            if (pick < 0) {
                const uint64_t g = generation;
                qcv.wait_for(ql, std::chrono::milliseconds(10), [&] { return stop || generation != g; });
                continue;
            }
            Unit &u = queues[pick].front();
            const uint64_t g = u.generation;
            if (u.eos) {
                queues[pick].pop_front();
                if (eos_generation != g) ended[0] = ended[1] = pushed_eos = false, eos_generation = g;
                ended[pick] = true;
                ql.unlock();
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    // LG's pipeline ends once per stretch of playback (a flush or load starts a new generation):
                    // when the video has ended and so has its audio, if LG plays that too.
                    if (owner_.load() == id && use_lg_ && !pushed_eos && ended[0] && (ended[1] || !lg_.has_audio())) {
                        lg_.end_of_stream();
                        pushed_eos = true;
                        eos_pushed = true;
                    }
                    // The Starfish backend's decoder (it reorders) keeps the last pictures until it is told the stream ended.
                    else if (owner_.load() == id && !use_lg_ && ndl_loaded_ && backend_ && !pushed_eos && ended[0]) {
                        backend_->end_of_stream();
                        pushed_eos = true;
                        eos_pushed = true;
                    }
                }
                ql.lock();
                continue;
            }
            ql.unlock();
            bool full = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (owner_.load() == id && backend_ && (use_lg_ || ndl_loaded_)) {
                    if (use_lg_) {
                        if (pick == 0 || lg_.has_audio())
                            full = lg_.feed(pick == 1, u.packet.data.data(), u.packet.data.size(), u.packet.pts_us) ==
                                   LgMediaPlayer::Feed::Full;
                    } else if (pick == 0) {
                        full = backend_->feed(u.packet) == wnk::media::MediaBackend::Feed::Full;
                    }
                }
            }
            ql.lock();
            if (full && generation == g) {
                ++full_waits;
                blocked_until[pick] = Clock::now() + std::chrono::milliseconds(10);
                continue;
            }
            if (!queues[pick].empty() && queues[pick].front().generation == g) queues[pick].pop_front();
            if (pick == 0) ++units;
        }
    });
    const auto drop_queued = [&] {
        std::lock_guard<std::mutex> ql(qm);
        queues[0].clear();
        queues[1].clear();
        ++generation;
        eos_pushed = false;
        qcv.notify_all();
    };
    const auto set_ready = [&](bool r) {
        std::lock_guard<std::mutex> ql(qm);
        ready = r;
        qcv.notify_all();
    };
    const auto release = [&] {
        uint64_t mine = id;
        if (owner_.compare_exchange_strong(mine, 0)) {
            std::lock_guard<std::mutex> lock(mutex_);
            clear_hole();  // the stream that owned the plane is gone (under mutex_, like the AREA store)
            lg_.unload();
            if (backend_ && ndl_loaded_) backend_->unload();
            use_lg_ = ndl_loaded_ = false;
            WNK_LOGI(kCat, "stream %llu released the decoder after %llu access units (%llu waits for room)",
                     static_cast<unsigned long long>(id), static_cast<unsigned long long>(units.load()),
                     static_cast<unsigned long long>(full_waits.load()));
        }
    };

    // The video's Load waits for the audio side's answer (AUDIO_FORMAT or AUDIO_NONE), as LG's pipeline takes both
    // codecs at once, unless the stream has no audio or the answer does not come within kAudioWait.
    constexpr auto kAudioWait = std::chrono::milliseconds(1000);
    enum class AudioState { Unknown, None, Hardware } audio_state = AudioState::Unknown;
    WpebMediaAudio audio_format{};
    std::vector<uint8_t> audio_header;  // Opus: the OpusHead
    bool no_audio_track = false;
    bool load_pending = false;
    WpebMediaLoad pending{};
    Clock::time_point load_at{};

    const auto do_load = [&](const WpebMediaLoad &l) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!backend_) return;
        auto &b = *backend_;
        wnk::media::StreamFormat f;
        wnk::media::VideoFormat v;
        v.codec = l.codec == WPEB_MEDIA_H264 ? wnk::media::CodecId::H264
                  : l.codec == WPEB_MEDIA_HEVC ? wnk::media::CodecId::Hevc
                  : l.codec == WPEB_MEDIA_VP9  ? wnk::media::CodecId::Vp9
                                               : wnk::media::CodecId::Av1;
        v.width = static_cast<int>(l.width);
        v.height = static_cast<int>(l.height);
        v.frame_rate = l.fps_milli / 1000.0;
        f.video = v;
        std::string error;
        // One decoder per app: the newest stream takes it (another tab or element loses its picture).
        lg_.unload();
        if (ndl_loaded_) b.unload();
        use_lg_ = ndl_loaded_ = false;
        owner_ = id;
        if (hole_id_.load() != id) clear_hole();  // another stream's rectangle no longer applies (this one's AREA may precede its Load)
        units = 0;
        full_waits = 0;
        const bool with_audio = audio_state == AudioState::Hardware;
        if (l.window_id[0] && LgMediaPlayer::available()) {
            const LgMediaPlayer::Codec c = l.codec == WPEB_MEDIA_H264 ? LgMediaPlayer::Codec::H264
                                           : l.codec == WPEB_MEDIA_HEVC ? LgMediaPlayer::Codec::Hevc
                                           : l.codec == WPEB_MEDIA_VP9  ? LgMediaPlayer::Codec::Vp9
                                                                        : LgMediaPlayer::Codec::Av1;
            LgMediaPlayer::Audio a;
            a.codec = audio_format.codec == WPEB_MEDIA_OPUS ? LgMediaPlayer::Audio::Codec::Opus : LgMediaPlayer::Audio::Codec::Aac;
            a.rate = static_cast<int>(audio_format.rate);
            a.channels = static_cast<int>(audio_format.channels);
            a.header = audio_header;
            use_lg_ = lg_.load(WPEB_APP_ID_STRING, l.window_id, c, static_cast<int>(l.width),
                               static_cast<int>(l.height), l.fps_milli / 1000.0, with_audio ? &a : nullptr, &error);
            if (use_lg_) lg_.set_hdr(l.hdr);
            if (!use_lg_) WNK_LOGW(kCat, "stream %llu: LG media pipeline: %s; trying NDL", static_cast<unsigned long long>(id), error.c_str());
        }
        if (!use_lg_) {
            error.clear();
            ndl_loaded_ = b.load(f, &error);
        }
        if (use_lg_ || ndl_loaded_)
            WNK_LOGI(kCat, "stream %llu: %s for %s %ux%u @ %.2f fps%s%s%s", static_cast<unsigned long long>(id),
                     use_lg_ ? "LG media pipeline" : "NDL", codec_label(l.codec), l.width, l.height, l.fps_milli / 1000.0,
                     !(use_lg_ && with_audio) ? "" : audio_format.codec == WPEB_MEDIA_OPUS ? " with its Opus audio" : " with its AAC audio", l.window_id[0] ? ", window " : "", l.window_id);
        else
            WNK_LOGW(kCat, "stream %llu: decoder load failed: %s", static_cast<unsigned long long>(id), error.c_str());
    };
    const auto maybe_load = [&] {
        if (!load_pending) return;
        const bool waited = Clock::now() - load_at >= kAudioWait;
        if (audio_state == AudioState::Unknown && !no_audio_track && !waited) return;
        if (audio_state == AudioState::Unknown && waited)
            WNK_LOGW(kCat, "stream %llu: no word from the audio side: video only", static_cast<unsigned long long>(id));
        load_pending = false;
        do_load(pending);
        set_ready(true);
    };

    for (;;) {
        pollfd pfd{fd, POLLIN, 0};
        const int pr = ::poll(&pfd, 1, load_pending ? 100 : -1);
        if (pr < 0 && errno == EINTR) continue;
        if (pr < 0) break;
        if (pr == 0) {
            maybe_load();
            continue;
        }
        WpebMediaHeader h;
        if (!read_all(fd, &h, sizeof(h)) || h.size > WPEB_MEDIA_MAX_PAYLOAD) break;
        payload.resize(h.size);
        if (h.size && !read_all(fd, payload.data(), h.size)) break;
        if (h.type == WPEB_MEDIA_LOAD || h.type == WPEB_MEDIA_UNLOAD || h.type == WPEB_MEDIA_FLUSH)
            WNK_LOGI(kCat, "stream %llu: %s", static_cast<unsigned long long>(id),
                     h.type == WPEB_MEDIA_LOAD ? "load" : h.type == WPEB_MEDIA_UNLOAD ? "unload" : "flush");
        if (h.type == WPEB_MEDIA_UNLOAD) {
            drop_queued();
            set_ready(false);
            load_pending = false;
            release();
            continue;
        }
        if (h.type == WPEB_MEDIA_VIDEO || h.type == WPEB_MEDIA_AUDIO) {
            // A held Load also times out while units keep arriving: poll() never goes idle when a page streams
            // without pause (YouTube's TV app), and the Load waited forever (TV, 2026-11-14).
            if (load_pending) maybe_load();
            const bool audio = h.type == WPEB_MEDIA_AUDIO;
            if (audio && audio_state != AudioState::Hardware) continue;
            Unit u;
            u.packet.type = audio ? wnk::media::TrackType::Audio : wnk::media::TrackType::Video;
            u.packet.pts_us = h.pts_us;
            u.packet.data.assign(payload.begin(), payload.end());
            std::lock_guard<std::mutex> ql(qm);
            u.generation = generation;
            queues[audio ? 1 : 0].push_back(std::move(u));
            qcv.notify_all();
            continue;
        }
        switch (h.type) {
        case WPEB_MEDIA_LOAD: {
            if (h.size < sizeof(WpebMediaLoad)) break;
            drop_queued();
            set_ready(false);
            std::memcpy(&pending, payload.data(), sizeof(pending));
            pending.window_id[sizeof(pending.window_id) - 1] = '\0';
            load_pending = true;
            load_at = Clock::now();
            maybe_load();
            break;
        }
        case WPEB_MEDIA_AUDIO_FORMAT:
            if (h.size >= sizeof(WpebMediaAudio)) {
                std::memcpy(&audio_format, payload.data(), sizeof(audio_format));
                if (audio_format.header_size > h.size - sizeof(WpebMediaAudio)) break;
                audio_header.assign(payload.begin() + sizeof(WpebMediaAudio),
                                    payload.begin() + sizeof(WpebMediaAudio) + audio_format.header_size);
                audio_state = AudioState::Hardware;
                WNK_LOGI(kCat, "stream %llu: %s %u Hz, %u channels goes with the video", static_cast<unsigned long long>(id),
                         audio_format.codec == WPEB_MEDIA_OPUS ? "Opus" : "AAC", audio_format.rate, audio_format.channels);
                maybe_load();
            }
            break;
        case WPEB_MEDIA_EOS: {
            if (h.size < sizeof(uint32_t)) break;
            uint32_t audio;
            std::memcpy(&audio, payload.data(), sizeof(audio));
            if (audio && audio_state != AudioState::Hardware) break;
            WNK_LOGI(kCat, "stream %llu: end of the %s", static_cast<unsigned long long>(id), audio ? "audio" : "video");
            Unit u;
            u.packet.pts_us = INT64_MAX;
            u.eos = true;
            std::lock_guard<std::mutex> ql(qm);
            u.generation = generation;
            queues[audio ? 1 : 0].push_back(std::move(u));
            qcv.notify_all();
            break;
        }
        case WPEB_MEDIA_AUDIO_NONE:
            audio_state = AudioState::None;
            maybe_load();
            break;
        case WPEB_MEDIA_TRACKS:
            if (h.size >= sizeof(uint32_t)) {
                uint32_t has_audio;
                std::memcpy(&has_audio, payload.data(), sizeof(has_audio));
                no_audio_track = has_audio == 0;
                maybe_load();
            }
            break;
        case WPEB_MEDIA_FLUSH: {
            drop_queued();
            std::lock_guard<std::mutex> lock(mutex_);
            if (owner_.load() == id) {
                std::string error;
                if (use_lg_) lg_.flush();
                else if (ndl_loaded_ && backend_) backend_->flush(&error);
            }
            break;
        }
        case WPEB_MEDIA_AREA: {
            if (h.size < sizeof(WpebMediaArea)) break;
            WpebMediaArea a;
            std::memcpy(&a, payload.data(), sizeof(a));
            // Not applied here: the UI commits the newest rectangle together with the page frame it draws
            // (commit_area), so the plane, the hole and the picture move in the same frame instead of the plane
            // running ahead of the page by one engine frame.
            std::lock_guard<std::mutex> al(area_mu_);
            pending_ = PendingArea{a, id, Clock::now(), ++pending_seq_, true};

            break;
        }
        case WPEB_MEDIA_PAUSE: {
            if (h.size < sizeof(uint32_t)) break;
            uint32_t paused;
            std::memcpy(&paused, payload.data(), sizeof(paused));
            // The page's element "ended" as soon as the sink had handed over the last unit, which is up to seconds before the
            // decoder has shown its buffered pictures: pausing then cut the end of the video off (TV: the last 2 s of a clip
            // never appeared). A decoder that was told the stream ended is left to drain.
            if (paused != 0 && eos_pushed.load()) break;
            std::lock_guard<std::mutex> lock(mutex_);
            if (owner_.load() == id) {
                if (use_lg_) lg_.pause(paused != 0);
                else if (ndl_loaded_ && backend_) backend_->pause(paused != 0);
            }
            break;
        }
        case WPEB_MEDIA_VOLUME: {
            if (h.size < sizeof(WpebMediaVolume)) break;
            WpebMediaVolume v;
            std::memcpy(&v, payload.data(), sizeof(v));
            std::lock_guard<std::mutex> lock(mutex_);
            if (owner_.load() == id && use_lg_) lg_.set_volume(v.permille, v.muted != 0);
            break;
        }
        default:
            break;
        }
    }
    {
        std::lock_guard<std::mutex> ql(qm);
        stop = true;
        qcv.notify_all();
    }
    feeder.join();
    release();
    WNK_LOGI(kCat, "stream %llu closed", static_cast<unsigned long long>(id));
}

}  // namespace browser
