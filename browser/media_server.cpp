// SPDX-License-Identifier: GPL-3.0-or-later
#include "media_server.hpp"

#include "start_filter.hpp"

#include "wpe_media_ipc.h"
#include "wnk/log.hpp"
#include "wnk/media/ndl_backend.hpp"
#include "wnk/media/starfish_backend.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
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
    if (fd < 0 || ::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 ||
        ::chmod(path.c_str(), 0600) != 0 || ::listen(fd, 4) != 0 || ::pipe2(wake_, O_CLOEXEC) != 0) {
        *why = std::string("media socket: ") + std::strerror(errno);
        if (fd >= 0) ::close(fd);
        ::unlink(path.c_str());
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

// The plane's lead (ms): see the header. Re-read from /tmp/wpeb-plane-lead twice a second so a test can change it on the fly.
// 100 ms: measured on the TV with an auto-scrolling page (screenshots, plane vs the hole): the plane trailed the hole by one
// scroll step (10-12 px, one area interval of ~116 ms) in nearly every shot; with a lead of 0/60/100/130 ms the black strip
// at the top of the hole averaged 10.6/5.7/2.2/0.8 px and the strips at both edges together 18.7/13.0/5.7/6.5 px.
float MediaServer::default_plane_lead_ms() { return 100.0f; }

float MediaServer::plane_lead_ms() {
    const auto now = Clock::now();
    if (lead_read_.time_since_epoch().count() == 0 || now - lead_read_ > std::chrono::milliseconds(500)) {
        lead_read_ = now;
        lead_ms_ = default_plane_lead_ms();
        if (FILE *f = std::fopen("/tmp/wpeb-plane-lead", "r")) {
            float v;
            if (std::fscanf(f, "%f", &v) == 1) lead_ms_ = std::max(0.0f, std::min(v, 200.0f));
            std::fclose(f);
        }
    }
    return lead_ms_;
}

// The plane for the page rectangle (sx, sy, sw, sh) in screen pixels, unclipped. Starfish's exported window takes a source
// rectangle: a video partly off the screen is cut by the compositor, not squashed into the visible part (NDL has no such
// call). A parked (not visible) video keeps the whole picture.
void MediaServer::place_plane(float sx, float sy, float sw, float sh) {
    const wnk::Rect vis = plane_rect(sx, sy, sw, sh);
    const bool parked = vis.w <= 16 && vis.h <= 9 && vis.x == 0 && vis.y == 0 && (sx > 16 || sy > 9 || sw > 16 || sh > 9);
    if (starfish_ && !parked && sw > 0 && sh > 0)
        starfish_->set_area_cropped(vis, (vis.x - sx) / sw, (vis.y - sy) / sh, vis.w / sw, vis.h / sh);
    else
        backend_->set_area(vis);
}

// No new area this frame: a plane that was placed ahead of the hole goes back onto it once the motion has stopped (no area
// for 220 ms: areas of a scrolling page come every 100-120 ms), or it would stay displaced by the predicted distance.
void MediaServer::settle_plane() {
    std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock() || !track_.shifted || !backend_) return;
    if (Clock::now() - track_.at < std::chrono::milliseconds(220)) return;
    track_.shifted = false;
    track_.count = 0;
    if (!use_lg_) place_plane(track_.sx, track_.sy, track_.sw, track_.sh);
}

// Called by the UI on its frame loop: frame_ready = an engine frame of the shown tab is about to be drawn. Applies
// the newest AREA then (or after kMaxAge without a frame: a static page whose video moved). Never waits for the feeder.
void MediaServer::commit_area(bool frame_ready) {
    PendingArea p;
    {
        std::lock_guard<std::mutex> al(area_mu_);
        if (pending_.valid) {
            if (!frame_ready && Clock::now() - pending_.at < std::chrono::milliseconds(40)) return;
            p = pending_;
        }
    }
    if (!p.valid) {
        settle_plane();
        return;
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
    // With no owner, only a stream that has not loaded yet (its AREA may precede its Load): after a release (UNLOAD, or release_owner when
    // the page went away) the stream's areas no longer make a hole.
    const uint64_t owner = owner_.load();
    const bool from_owner = owner == p.id || (owner == 0 && p.before_load);
    if (backend_ && a.width > 0 && a.height > 0 && from_owner && !use_lg_) {
        // Where the video is going: the compositor shows the plane some milliseconds after the UI's own frame (screenshots of a
        // scrolling page: the plane trailed the hole by one to two scroll steps), so a plane put on the hole trails it by that
        // much motion. The plane is placed plane_lead_ms() of its measured motion ahead; the hole stays exact, and once the
        // motion stops (settle_plane) the plane returns to it.
        const auto now = Clock::now();
        const float lead = plane_lead_ms();
        PlaneTrack &t = track_;
        if (t.id != p.id || t.sw != sw || t.sh != sh) t.count = t.head = 0;
        t.id = p.id;
        t.sw = sw;
        t.sh = sh;
        t.ring[t.head] = PlaneSample{now, sx, sy};
        t.head = (t.head + 1) % 8;
        if (t.count < 8) ++t.count;
        // Velocity (px/ms): the newest sample against the newest one that is at least 40 ms older (and not over 160 ms).
        float vx = 0, vy = 0;
        for (int i = 2; i <= t.count; ++i) {
            const PlaneSample &o = t.ring[(t.head - i + 16) % 8];
            const float dt = std::chrono::duration<float, std::milli>(now - o.at).count();
            if (dt > 160) break;
            if (dt >= 40) {
                vx = (sx - o.sx) / dt;
                vy = (sy - o.sy) / dt;
                break;
            }
        }
        t.sx = sx;
        t.sy = sy;
        t.at = now;
        float px = sx, py = sy;
        t.shifted = false;
        if (lead > 0 && (vx != 0 || vy != 0)) {
            constexpr float kCap = 60.0f;  // never further than this from the hole
            px = sx + std::max(-kCap, std::min(kCap, vx * lead));
            py = sy + std::max(-kCap, std::min(kCap, vy * lead));
            t.shifted = true;
        }
        if (area_debug) fprintf(stderr, "[plane] v=(%.3f,%.3f px/ms) lead=%.0f ms -> %+.1f,%+.1f px\n", vx, vy, lead, px - sx, py - sy);
        place_plane(px, py, sw, sh);
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
        struct ucred cred{};
        socklen_t cred_len = sizeof(cred);
        if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &cred_len) != 0 || cred.uid != ::getuid()) {
            WNK_LOGW(kCat, "media socket: rejected peer (uid %u)", static_cast<unsigned>(cred.uid));
            ::close(fd);
            continue;
        }
        if (connections_.load() >= 16) {
            WNK_LOGW(kCat, "media socket: connection limit reached");
            ::close(fd);
            continue;
        }
        const uint64_t id = next_id_++;
        ++connections_;
        std::thread([this, fd, id] {
            serve(fd, id);
            ::close(fd);
            --connections_;
        }).detach();
    }
}

// Can the decoder be (re)started at this access unit? (Annex-B; see StartFilter::restart_point: H.264 IDR or a non-IDR picture behind a
// recovery-point SEI, HEVC IRAP; a unit the scan cannot read counts, so a feed is never held back for ever.)
static bool is_keyframe(uint32_t codec, const std::vector<uint8_t> &d) {
    return StartFilter::restart_point(codec == WPEB_MEDIA_H264 ? StartFilter::Codec::H264 : codec == WPEB_MEDIA_HEVC ? StartFilter::Codec::Hevc : StartFilter::Codec::Other,
                                      d.data(), d.size());
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
        uint64_t start_seq = 0;  // feeder: the start of feed (arm_seq) this unit was last judged for (a unit the decoder refused is not judged twice)
        uint64_t seq = 0;        // arrival order (next_seq): a unit the feeder gives back goes in again where it was
    };
    std::mutex qm;
    std::condition_variable qcv;
    std::deque<Unit> queues[2];  // [0] video, [1] audio
    uint64_t generation = 0;     // qm; a flush or load drops what was queued before it
    uint64_t next_seq = 0;       // qm; Unit::seq of the next unit queued
    uint64_t reload_epoch = 0;   // mutex_ and qm (both held to change it): counts the retries begun; the feeder reads it under either
    uint64_t arm_seq = 0;        // qm; counts the starts of a feed (load, reload, flush): the feeder arms its start filter for each
    uint64_t arm_gen = 0;        // qm; units of an older generation do not belong to that start
    bool stop = false;
    bool ready = false;          // qm; the decoder is loaded (units wait until then)
    std::atomic<bool> backend_audio{false};  // the backend's load took audio (feeder reads it)
    std::deque<Unit> replay;  // qm; units fed since the Load while no picture has been presented yet (a Load that fails early is repeated with them)
    uint64_t replay_bytes = 0;
    bool replay_ok = false;   // qm; the replay is complete (nothing was dropped from it)
    std::atomic<bool> recover_wanted{false};  // the LG pipeline failed after showing pictures: the reader thread loads it again at the current position
    std::atomic<uint32_t> key_codec{0};       // the stream's codec (for finding keyframes)
    std::atomic<bool> need_key{false};        // after such a reload the decoder is fed from the next keyframe on (a start in the middle of a GOP smears)
    std::atomic<int64_t> need_key_at{0};      // when need_key was set (steady clock, ns): a stream with no keyframe in kKeyWait is fed as it comes
    constexpr auto kKeyWait = std::chrono::seconds(15);
    std::atomic<bool> retry_wanted{false};  // the pipeline failed before its first picture: the reader thread loads it again
    std::atomic<bool> eos_pushed{false};  // the decoder was told the stream ended (cleared by a flush or load)
    constexpr uint64_t kReplayMax = 48u << 20;  // 4K at 40 Mbit/s: about 9 s
    std::thread feeder([&] {
        Clock::time_point blocked_until[2] = {};
        bool ended[2] = {};          // feeder thread; the streams whose end was reached, in eos_generation
        bool pushed_eos = false;
        uint64_t eos_generation = 0;
        StartFilter start_filter;  // leading pictures at the start of a feed (start_filter.hpp)
        uint64_t seen_arm = 0, armed_gen = 0;
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
            if (need_key.load() && !u.eos) {
                // A stream whose keyframes are not IDR/IRAP (gradual refresh) would be dropped for ever, picture and sound: after kKeyWait
                // it is fed as it comes (a smeared start is better than a dead stream).
                if (now - Clock::time_point(std::chrono::nanoseconds(need_key_at.load())) > kKeyWait) {
                    WNK_LOGW(kCat, "stream %llu: no keyframe within %d s after the reload: feeding from here", static_cast<unsigned long long>(id),
                             static_cast<int>(kKeyWait.count()));
                    need_key = false;
                } else {
                    // Only video can say where a keyframe is; the audio before it goes too (it would play ahead of a picture that is not there).
                    if (pick == 1 || !is_keyframe(key_codec.load(), u.packet.data)) {
                        queues[pick].pop_front();
                        continue;
                    }
                    need_key = false;
                }
            }
            if (pick == 0 && !u.eos) {
                // A load, a reload or a seek starts a feed: if it starts on a keyframe with leading pictures, they are left out.
                if (arm_seq != seen_arm) { seen_arm = arm_seq; armed_gen = arm_gen; start_filter.arm(); }
                if (u.start_seq != seen_arm && u.generation >= armed_gen) {
                    u.start_seq = seen_arm;
                    const uint32_t kc = key_codec.load();
                    if (start_filter.drop(kc == WPEB_MEDIA_HEVC ? StartFilter::Codec::Hevc : kc == WPEB_MEDIA_H264 ? StartFilter::Codec::H264 : StartFilter::Codec::Other,
                                          u.packet.data.data(), u.packet.data.size(), u.packet.pts_us)) {
                        WNK_LOGI(kCat, "stream %llu: leading picture left out (pts %.3f s, %d so far since the start of the feed)",
                                 static_cast<unsigned long long>(id), u.packet.pts_us / 1e6, start_filter.dropped());
                        queues[pick].pop_front();
                        continue;
                    }
                }
            }
            if (u.eos) {
                queues[pick].pop_front();  // u is gone from here on
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
                    else if (owner_.load() == id && !use_lg_ && ndl_loaded_ && backend_ && !pushed_eos && ended[0] &&
                             (ended[1] || !backend_audio.load())) {
                        backend_->end_of_stream();
                        pushed_eos = true;
                        eos_pushed = true;
                    }
                }
                ql.lock();
                continue;
            }
            // The unit leaves the queue before qm is let go: FLUSH, LOAD and UNLOAD clear the queues under qm alone, which freed the
            // buffer the decoder was reading (LG's Feed gets its address). A unit the decoder refuses goes back where it was.
            const uint64_t epoch = reload_epoch;
            Unit unit = std::move(u);
            queues[pick].pop_front();
            ql.unlock();
            bool full = false, requeue = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (reload_epoch != epoch) {
                    requeue = true;  // a retry began after this unit was taken out: it goes to the reloaded pipeline, behind the replay
                } else if (owner_.load() == id && backend_ && (use_lg_ || ndl_loaded_)) {
                    if (use_lg_) {
                        if (pick == 0 || lg_.has_audio())
                            full = lg_.feed(pick == 1, unit.packet.data.data(), unit.packet.data.size(), unit.packet.pts_us) ==
                                   LgMediaPlayer::Feed::Full;
                    } else if (pick == 0 || backend_audio.load()) {
                        full = backend_->feed(unit.packet) == wnk::media::MediaBackend::Feed::Full;
                    }
                }
                // qm while mutex_ is still held (the lock order): a retry (mutex_, then qm) cannot come between this feed and the replay
                // bookkeeping below, so what the pipeline was given is in the replay it takes.
                ql.lock();
            }
            if ((full || requeue) && generation == g && (full || owner_.load() == id)) {
                // Back in front of what came after it (by arrival: a retry may have put its replay in front meanwhile).
                auto &q = queues[pick];
                auto at = q.begin();
                while (at != q.end() && at->seq < unit.seq) ++at;
                q.insert(at, std::move(unit));
                if (requeue) continue;
                ++full_waits;
                blocked_until[pick] = Clock::now() + std::chrono::milliseconds(10);
                if (owner_.load() == id && replay_ok && use_lg_ && !retry_wanted.load() && lg_.error() != 0 && lg_.frames_presented() == 0) retry_wanted = true;
                if (owner_.load() == id && use_lg_ && !recover_wanted.load() && lg_.error() != 0 && lg_.frames_presented() > 0) recover_wanted = true;
                continue;
            }
            if (generation == g) {
                // Keep what the decoder was given until its first picture shows (a failed start is repeated with it).
                if (replay_ok && use_lg_) {
                    if (lg_.frames_presented() > 0 || replay_bytes > kReplayMax) {
                        replay.clear();
                        replay_bytes = 0;
                        replay_ok = false;
                    } else {
                        replay_bytes += unit.packet.data.size();
                        replay.push_back(std::move(unit));
                    }
                }
            }
            if (pick == 0) ++units;
            if (owner_.load() == id && replay_ok && use_lg_ && !retry_wanted.load() && lg_.error() != 0 && lg_.frames_presented() == 0) retry_wanted = true;
            if (owner_.load() == id && use_lg_ && !recover_wanted.load() && lg_.error() != 0 && lg_.frames_presented() > 0) recover_wanted = true;
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
        // mutex_ first, then the owner (as release_owner): between a compare-and-swap outside the lock and the unload, a newer
        // stream's Load could take the decoder, and this stream then unloaded that stream's fresh pipeline.
        std::lock_guard<std::mutex> lock(mutex_);
        uint64_t mine = id;
        if (owner_.compare_exchange_strong(mine, 0)) {
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
    bool pending_valid = false;
    Clock::time_point load_at{};

    // The page element's volume and mute (WPEB_MEDIA_VOLUME): kept for the stream, applied after its Load. A muted element that
    // is unmuted later is the same stream: its audio goes through LG's pipeline from the start (the sink no longer builds the
    // software chain for a muted element when this backend takes the audio), so only these two values change.
    uint32_t vol_permille = 1000;
    bool vol_muted = false;
    // The element's pause state (WPEB_MEDIA_PAUSE, as applied): kept for the stream and applied after every load, as LG's player starts
    // playing on its first feed by itself. A pause that came while the Load was held (the stream did not own the decoder yet), or
    // before a retry or recovery reloaded the pipeline, was lost: the video played under a paused element.
    bool paused_state = false;
    bool loaded_once = false;  // reader thread: do_load ran for this stream (its areas without an owner no longer make a hole)

    bool force_ndl = false;  // the last attempt after LG's pipeline failed to start: the Starfish backend's decoder
    int retries = 0;
    bool close_stream = false;  // reader thread: a (re)load failed; the connection is closed (below) so the page gets a media error
    // No decoder any more (qm held): nothing will consume the queues, which are not capped. Closing the connection makes the web
    // process's sink fail its next write ("Hardware video service lost"): the page gets a media error instead of a stall.
    const auto close_without_decoder = [&](const char *what) {
        queues[0].clear();
        queues[1].clear();
        ++generation;
        close_stream = true;
        WNK_LOGW(kCat, "stream %llu: %s failed: closing the stream", static_cast<unsigned long long>(id), what);
    };
    // What do_load did. Lost: a reload (retry or recovery) found the decoder no longer this stream's: a newer stream's Load or the UI
    // took it while the reload waited without mutex_, and taking it back would leave that stream without a picture.
    enum class Loaded { Yes, No, Lost };
    const auto do_load = [&](const WpebMediaLoad &l, bool reload) -> Loaded {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!backend_) return Loaded::No;
        if (reload && owner_.load() != id) return Loaded::Lost;
        auto &b = *backend_;
        key_codec = l.codec;
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
        // Starfish takes the page's AAC next to the video (A/V sync from LG's pipeline); NDL only video.
        if (starfish_ && audio_state == AudioState::Hardware &&
            (audio_format.codec == WPEB_MEDIA_AAC || audio_format.codec == WPEB_MEDIA_PCM)) {
            wnk::media::AudioFormat af;
            // PCM: what the web process decoded (Opus, Vorbis, MP3, ...), as S16LE, which LG's pipeline plays like the AAC.
            af.codec = audio_format.codec == WPEB_MEDIA_PCM ? wnk::media::CodecId::PcmS16 : wnk::media::CodecId::Aac;
            af.sample_rate = static_cast<int>(audio_format.rate);
            af.channels = static_cast<int>(audio_format.channels);
            f.audio = af;
        }
        backend_audio = f.audio.has_value();
        std::string error;
        // One decoder per app: the newest stream takes it (another tab or element loses its picture).
        lg_.unload();
        if (ndl_loaded_) b.unload();
        use_lg_ = ndl_loaded_ = false;
        owner_ = id;
        loaded_once = true;
        if (hole_id_.load() != id) clear_hole();  // another stream's rectangle no longer applies (this one's AREA may precede its Load)
        units = 0;
        full_waits = 0;
        const bool with_audio = audio_state == AudioState::Hardware;
        if (l.window_id[0] && !force_ndl && LgMediaPlayer::available()) {
            const LgMediaPlayer::Codec c = l.codec == WPEB_MEDIA_H264 ? LgMediaPlayer::Codec::H264
                                           : l.codec == WPEB_MEDIA_HEVC ? LgMediaPlayer::Codec::Hevc
                                           : l.codec == WPEB_MEDIA_VP9  ? LgMediaPlayer::Codec::Vp9
                                                                        : LgMediaPlayer::Codec::Av1;
            LgMediaPlayer::Audio a;
            a.codec = audio_format.codec == WPEB_MEDIA_OPUS  ? LgMediaPlayer::Audio::Codec::Opus
                      : audio_format.codec == WPEB_MEDIA_PCM ? LgMediaPlayer::Audio::Codec::Pcm
                                                             : LgMediaPlayer::Audio::Codec::Aac;
            if (a.codec == LgMediaPlayer::Audio::Codec::Pcm) {
                // As the Starfish backend does for PCM (kit: pcm_lead_ms_for): LG's PCM mixer channel plays ahead of its video.
                a.pcm_lead_ms = wnk::media::StarfishBackend::pcm_lead_ms_for(wnk::media::StarfishBackend::detect_audio_stack(), "");
                WNK_LOGI(kCat, "pcm: audio stamped +%d ms", a.pcm_lead_ms);
            }
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
        if (ndl_loaded_ && starfish_) starfish_->set_volume(static_cast<int>(vol_permille / 10), vol_muted);
        if (use_lg_) lg_.set_volume(vol_permille, vol_muted);
        if (paused_state) {
            if (use_lg_) lg_.pause(true);
            else if (ndl_loaded_) b.pause(true);
        }
        if (use_lg_ || ndl_loaded_)
            WNK_LOGI(kCat, "stream %llu: %s for %s %ux%u @ %.2f fps%s%s%s", static_cast<unsigned long long>(id),
                     use_lg_ ? "LG media pipeline" : "NDL", codec_label(l.codec), l.width, l.height, l.fps_milli / 1000.0,
                     !(use_lg_ && with_audio) ? "" : audio_format.codec == WPEB_MEDIA_OPUS ? " with its Opus audio" : audio_format.codec == WPEB_MEDIA_PCM ? " with its PCM audio" : " with its AAC audio", l.window_id[0] ? ", window " : "", l.window_id);
        else
            WNK_LOGW(kCat, "stream %llu: decoder load failed: %s", static_cast<unsigned long long>(id), error.c_str());
        std::lock_guard<std::mutex> ql(qm);
        replay.clear();
        replay_bytes = 0;
        replay_ok = use_lg_;
        arm_gen = generation;  // the units from here on start a feed
        ++arm_seq;
        return use_lg_ || ndl_loaded_ ? Loaded::Yes : Loaded::No;
    };
    // A reload found the decoder taken (Loaded::Lost): the stream goes on as one whose decoder a newer Load took (its units are
    // consumed and dropped by the feeder, as before the reload).
    const auto lost_decoder = [&](const char *what) {
        std::lock_guard<std::mutex> ql(qm);
        replay.clear();
        replay_bytes = 0;
        replay_ok = false;
        ready = true;
        qcv.notify_all();
        WNK_LOGI(kCat, "stream %llu: %s dropped: another stream holds the decoder now", static_cast<unsigned long long>(id), what);
    };
    // The LG pipeline failed before showing a picture (TV, 2026-11-20: "fail to create AVProxy" for a page's main video right after its
    // ads, while another app still held a decoder or the ad's pipeline was still being released): load it again after a pause, with
    // what it was fed, up to twice; the last attempt uses the other decoder (the Starfish backend). Generic: any early pipeline error.
    const auto do_retry = [&] {
        if (!retry_wanted.exchange(false)) return;
        if (!pending_valid || owner_.load() != id) return;  // a stream the newest Load took the decoder from must not take it back
        const int code = lg_.error();
        ++retries;
        bool lost = false;
        std::deque<Unit> again;
        const int delay_ms = retries == 1 ? 500 : 1500;
        WNK_LOGW(kCat, "stream %llu: LG pipeline error %d before its first picture: attempt %d to load it again in %d ms%s", static_cast<unsigned long long>(id),
                 code, retries + 1, delay_ms, retries >= 2 ? " (Starfish backend)" : "");
        {
            // use_lg_ is the owner's: a stream that lost the decoder meanwhile leaves it (and the pipeline) to the new owner.
            // mutex_ before the replay is taken: the feeder is not in the middle of a feed, so the unit it was feeding is in the replay
            // (it used to be fed to the failed pipeline after the swap and then missed from both); a unit it took out of the queue and
            // has not fed yet goes back into it (reload_epoch).
            std::lock_guard<std::mutex> lock(mutex_);
            if (owner_.load() != id) {
                lost = true;
            } else {
                {
                    std::lock_guard<std::mutex> ql(qm);
                    ready = false;
                    again.swap(replay);
                    replay_bytes = 0;
                    replay_ok = false;
                    ++reload_epoch;
                    // The feeder may have asked again since the exchange above (a feed to the failing pipeline before this lock): that
                    // is this same retry, not another one (it used up the next attempt and went to the fallback decoder).
                    retry_wanted = false;
                }
                lg_.unload();
                use_lg_ = false;
            }
        }
        if (!lost) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            if (retries >= 2) force_ndl = true;
        }
        const Loaded r = lost ? Loaded::Lost : do_load(pending, true);
        if (r == Loaded::Lost) {
            lost_decoder("the retry");
            return;
        }
        {
            std::lock_guard<std::mutex> ql(qm);
            // What was fed goes first again (per stream in order), ahead of what came meanwhile.
            for (auto it = again.rbegin(); it != again.rend(); ++it) queues[it->packet.type == wnk::media::TrackType::Audio ? 1 : 0].push_front(std::move(*it));
            ready = r == Loaded::Yes;
            if (!ready) close_without_decoder("the retry");  // as do_recover: a failed retry left the stream black and its queues growing
            qcv.notify_all();
        }
    };
    // The LG pipeline failed after it had shown pictures (TV, 2026-11-21, a seek in a video app: smeared picture, "pipeline error 100",
    // frames presented stop while units are still fed, and the decoder stayed held): unload it, load it again at the current position
    // and feed it from the next keyframe. Not more than three times in a minute, then it is left to the page (its own recovery).
    std::deque<Clock::time_point> recoveries;
    bool cap_logged = false;     // reader thread: the "not reloaded" line is printed once per error episode (a Load or a reload starts a new one)
    const auto do_recover = [&] {
        if (!recover_wanted.exchange(false)) return;
        if (!pending_valid || !use_lg_ || owner_.load() != id) return;  // as do_retry: only the stream that owns the pipeline reloads it
        const auto now = Clock::now();
        while (!recoveries.empty() && now - recoveries.front() > std::chrono::seconds(60)) recoveries.pop_front();
        const int code = lg_.error();
        if (recoveries.size() >= 3) {
            if (!cap_logged) WNK_LOGW(kCat, "stream %llu: LG pipeline error %d again: not reloaded (3 in a minute; not logged again for this error)", static_cast<unsigned long long>(id), code);
            cap_logged = true;
            return;
        }
        cap_logged = false;
        recoveries.push_back(now);
        WNK_LOGW(kCat, "stream %llu: LG pipeline error %d mid-stream: reloading it at the current position", static_cast<unsigned long long>(id), code);
        {
            std::lock_guard<std::mutex> ql(qm);
            ready = false;
        }
        bool lost = false;
        {
            // As in do_retry: only the owner unloads the pipeline and clears use_lg_.
            std::lock_guard<std::mutex> lock(mutex_);
            if (owner_.load() != id) {
                lost = true;
            } else {
                lg_.unload();
                use_lg_ = false;
            }
        }
        if (!lost) {
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            need_key_at = Clock::now().time_since_epoch().count();
            need_key = true;
        }
        const Loaded r = lost ? Loaded::Lost : do_load(pending, true);
        if (r == Loaded::Lost) {
            lost_decoder("the reload");
            return;
        }
        {
            std::lock_guard<std::mutex> ql(qm);
            replay_ok = false;  // no replay for a stream that has shown pictures
            replay.clear();
            ready = r == Loaded::Yes;
            if (!ready) close_without_decoder("the reload");
            qcv.notify_all();
        }
    };
    const auto maybe_load = [&] {
        if (!load_pending) return;
        const bool waited = Clock::now() - load_at >= kAudioWait;
        if (audio_state == AudioState::Unknown && !no_audio_track && !waited) return;
        if (audio_state == AudioState::Unknown && waited)
            WNK_LOGW(kCat, "stream %llu: no word from the audio side: video only", static_cast<unsigned long long>(id));
        load_pending = false;
        if (do_load(pending, false) == Loaded::No) {
            // Neither decoder took the stream: as a failed reload, the page gets a media error instead of a black video.
            std::lock_guard<std::mutex> ql(qm);
            ready = false;
            close_without_decoder("the load");
            qcv.notify_all();
            return;
        }
        set_ready(true);
    };

    for (;;) {
        if (close_stream) break;
        pollfd pfd{fd, POLLIN, 0};
        // While a stream plays through the LG pipeline the wait is bounded: the feeder thread sets retry_wanted/recover_wanted while this
        // thread may already be blocked here, and nothing may arrive on the socket to wake it.
        const int pr = ::poll(&pfd, 1, load_pending || retry_wanted.load() || recover_wanted.load() || (use_lg_ && owner_.load() == id) ? 100 : -1);
        if (pr < 0 && errno == EINTR) continue;
        if (pr < 0) break;
        if (pr == 0) {
            maybe_load();
            do_retry();
            do_recover();
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
            if (retry_wanted.load()) do_retry();
            if (recover_wanted.load()) do_recover();
            const bool audio = h.type == WPEB_MEDIA_AUDIO;
            if (audio && audio_state != AudioState::Hardware) continue;
            Unit u;
            u.packet.type = audio ? wnk::media::TrackType::Audio : wnk::media::TrackType::Video;
            u.packet.pts_us = h.pts_us;
            u.packet.data.assign(payload.begin(), payload.end());
            std::lock_guard<std::mutex> ql(qm);
            u.generation = generation;
            u.seq = next_seq++;
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
            pending_valid = true;
            retries = 0;
            force_ndl = false;
            retry_wanted = false;
            recover_wanted = false;
            need_key = false;
            cap_logged = false;
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
                         audio_format.codec == WPEB_MEDIA_OPUS ? "Opus" : audio_format.codec == WPEB_MEDIA_PCM ? "PCM" : "AAC", audio_format.rate, audio_format.channels);
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
            u.seq = next_seq++;
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
            {
                std::lock_guard<std::mutex> ql(qm);
                arm_gen = generation;  // a seek: what follows starts a feed
                ++arm_seq;
                // What a retry repeats is what the decoder was given since the seek: units from before it would be fed first again
                // (from the old position, and older than the start filter's arm_gen, so not judged by it). The replay stays complete
                // (replay_ok as it was): the feed after a seek starts over at the new position.
                replay.clear();
                replay_bytes = 0;
            }
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
            pending_ = PendingArea{a, id, Clock::now(), ++pending_seq_, true, !loaded_once};

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
            paused_state = paused != 0;
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
            vol_permille = v.permille;
            vol_muted = v.muted != 0;
            std::lock_guard<std::mutex> lock(mutex_);
            if (owner_.load() == id) {
                if (use_lg_) lg_.set_volume(v.permille, v.muted != 0);
                else if (ndl_loaded_ && starfish_) starfish_->set_volume(static_cast<int>(v.permille / 10), v.muted != 0);
            }
            WNK_LOGI(kCat, "stream %llu: volume %u.%u%%%s", static_cast<unsigned long long>(id), v.permille / 10, v.permille % 10,
                     v.muted ? " (muted)" : "");
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
