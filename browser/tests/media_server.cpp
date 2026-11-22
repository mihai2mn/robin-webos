// SPDX-License-Identifier: GPL-3.0-or-later
// The media server's stream handling (media_server.cpp) on the host, against fake decoders (tests/media_fakes): the web
// process's side is a socket client sending what the video sink sends. Covers the feeder's queue (a unit the decoder is
// reading is never freed by a seek; a unit refused as full is fed again, in order), the reloads after an early LG pipeline
// error (only the owner reloads, a failed reload closes the stream, the replay is what the failed pipeline was given since
// the last seek, nothing is lost or reordered), a first Load that loads nothing, the pause state across reloads and the
// ownership of the video area. Build it with -fsanitize=address or -fsanitize=thread to see the queue's lifetimes.
#include "media_server.hpp"

#include "fake_media.hpp"
#include "lg_event_tokens.hpp"
#include "wnk/log.hpp"
#include "wpe_media_ipc.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

// ---- the fake LG pipeline (the real lg_media_player.cpp is webOS only) --------------------------------------------------
namespace fake {
State &state() { static State s; return s; }

std::vector<uint8_t> unit_payload(uint64_t seq, size_t size) {
    std::vector<uint8_t> d(size);
    char hex[17];
    std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(seq));
    d[0] = 'U';
    std::memcpy(d.data() + 1, hex, 16);
    for (size_t i = 17; i < size; ++i) d[i] = static_cast<uint8_t>(0x80 | ((seq + i) & 0x7f));
    return d;
}

int64_t read_unit(const uint8_t *d, size_t n, bool *intact) {
    *intact = false;
    if (n < 17 || d[0] != 'U') return -1;
    char hex[17];
    std::memcpy(hex, d + 1, 16);
    hex[16] = 0;
    char *end = nullptr;
    const unsigned long long seq = std::strtoull(hex, &end, 16);
    if (end != hex + 16) return -1;
    bool ok = true;
    for (size_t i = 17; i < n; ++i) ok &= d[i] == static_cast<uint8_t>(0x80 | ((seq + i) & 0x7f));
    *intact = ok;
    return static_cast<int64_t>(seq);
}

bool feed_unit(const char *what, const uint8_t *d, size_t n) {
    State &s = state();
    // The decoder holds the buffer for a while (LG's Feed copies it on its own time): sleep first, read after.
    if (const int us = s.feed_sleep_us.load()) std::this_thread::sleep_for(std::chrono::microseconds(us));
    bool intact = false;
    const int64_t seq = read_unit(d, n, &intact);
    if (!intact) ++s.bad_reads;
    const int k = ++s.feeds;
    const int every = s.full_every.load();
    const bool accepted = every <= 0 || k % every != 0;
    s.add({what, "", seq, accepted, intact});
    return accepted;
}
}  // namespace fake

namespace browser {
LgMediaPlayer::~LgMediaPlayer() { unload(); }
bool LgMediaPlayer::available() { return fake::state().lg_available.load(); }
std::string LgMediaPlayer::unavailable_reason() { return "fake"; }
bool LgMediaPlayer::load(const std::string &, const std::string &window_id, Codec, int, int, double, const Audio *audio,
                         std::string *error) {
    if (!fake::state().lg_load_ok.load()) {
        fake::state().add({"lg.load", "failed:" + window_id});
        *error = "fake LG load refused";
        return false;
    }
    api_ = reinterpret_cast<StarfishMediaAPIs *>(this);  // never dereferenced here
    event_token_ = lg_event_tokens().issue(this);       // as the real load: each Load's events carry its own token
    {
        std::lock_guard<std::mutex> l(fake::state().mu);
        fake::state().lg_tokens.push_back(event_token_);
    }
    playing_ = paused_ = false;
    seek_pending_ = true;
    has_audio_ = audio != nullptr;
    error_ = 0;
    frames_ = 0;
    fed_ = 0;
    fake::state().lg = this;
    fake::state().add({"lg.load", window_id});
    return true;
}
LgMediaPlayer::Feed LgMediaPlayer::feed(bool, const uint8_t *data, size_t size, int64_t) {
    if (api_ == nullptr) return Feed::Error;
    if (!playing_ && !paused_) {  // as the real one: the first feed after a load starts playback
        playing_ = true;
        fake::state().add({"lg.autoplay"});
    }
    if (!fake::feed_unit("lg.feed", data, size)) return Feed::Full;
    ++fed_;
    return Feed::Ok;
}
void LgMediaPlayer::set_hdr(const WpebMediaHdr &) {}
void LgMediaPlayer::end_of_stream() { fake::state().add({"lg.eos"}); }
void LgMediaPlayer::set_volume(unsigned, bool) {}
void LgMediaPlayer::flush() { if (api_) fake::state().add({"lg.flush"}); }
void LgMediaPlayer::pause(bool paused) {
    if (api_ == nullptr || paused == paused_) return;
    paused_ = paused;
    if (!paused) playing_ = true;
    fake::state().add({"lg.pause", "", paused ? 1 : 0});
}
void LgMediaPlayer::unload() {
    if (api_ == nullptr) return;
    lg_event_tokens().retire(event_token_);
    event_token_ = nullptr;
    api_ = nullptr;
    fake::state().add({"lg.unload"});
}
// Test hooks: type 1 = a pipeline error (num: its code), type 2 = num frames presented.
void LgMediaPlayer::on_event(int type, int64_t num, const char *) {
    if (type == 1) error_ = static_cast<int>(num);
    if (type == 2) frames_ += num;
}
}  // namespace browser

// ---- the test --------------------------------------------------------------------------------------------------------
using browser::MediaServer;
using namespace std::chrono_literals;

static int failures = 0;
static void check(bool ok, const std::string &what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    std::fflush(stdout);
    if (!ok) ++failures;
}

static bool wait_until(const std::function<bool()> &pred, std::chrono::milliseconds limit) {
    const auto end = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < end) {
        if (pred()) return true;
        std::this_thread::sleep_for(2ms);
    }
    return pred();
}

static std::string sock_dir() {
    const char *d = std::getenv("MEDIA_TEST_DIR");
    return d ? d : "/tmp";
}

struct Client {
    int fd = -1;
    explicit Client(const std::string &path) {
        fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        sockaddr_un a{};
        a.sun_family = AF_UNIX;
        std::snprintf(a.sun_path, sizeof a.sun_path, "%s", path.c_str());
        if (::connect(fd, reinterpret_cast<sockaddr *>(&a), sizeof a) != 0) { std::perror("connect"); std::exit(2); }
    }
    ~Client() { close(); }
    void close() { if (fd >= 0) ::close(fd); fd = -1; }
    bool send(uint32_t type, int64_t pts, const void *p = nullptr, uint32_t n = 0) {
        WpebMediaHeader h{type, n, pts};
        if (::send(fd, &h, sizeof h, MSG_NOSIGNAL) != static_cast<ssize_t>(sizeof h)) return false;
        return n == 0 || ::send(fd, p, n, MSG_NOSIGNAL) == static_cast<ssize_t>(n);
    }
    void load(const char *window) {
        WpebMediaLoad l{};
        l.codec = WPEB_MEDIA_H264;
        l.width = 1920;
        l.height = 1080;
        l.fps_milli = 30000;
        std::snprintf(l.window_id, sizeof l.window_id, "%s", window);
        send(WPEB_MEDIA_LOAD, 0, &l, sizeof l);
    }
    void audio_none() { send(WPEB_MEDIA_AUDIO_NONE, 0); }
    void unit(uint64_t seq) { const auto d = fake::unit_payload(seq); send(WPEB_MEDIA_VIDEO, static_cast<int64_t>(seq) * 33333, d.data(), static_cast<uint32_t>(d.size())); }
    void flush() { send(WPEB_MEDIA_FLUSH, 0); }
    void pause(bool p) { const uint32_t v = p; send(WPEB_MEDIA_PAUSE, 0, &v, sizeof v); }
    void area(int x, int y, int w, int h) { const WpebMediaArea a{x, y, w, h}; send(WPEB_MEDIA_AREA, 0, &a, sizeof a); }
    // The server closed this connection (recv sees the end) within the limit.
    bool closed_within(std::chrono::milliseconds limit) {
        pollfd p{fd, POLLIN, 0};
        if (::poll(&p, 1, static_cast<int>(limit.count())) <= 0) return false;
        char c;
        const ssize_t n = ::recv(fd, &c, 1, MSG_DONTWAIT);
        return n == 0 || (n < 0 && errno == ECONNRESET);  // a reset: it closed with what this side sent still unread
    }
};

// Feeds (accepted ones) after the n-th successful load (0-based) of either decoder, up to the next one.
static std::vector<int64_t> accepted_after_load(int n, const char *feed = "lg.feed") {
    int loads = -1;
    std::vector<int64_t> out;
    for (const auto &e : fake::state().snapshot()) {
        if ((e.what == "lg.load" || e.what == "ndl.load") && e.detail.rfind("failed", 0) != 0 && e.detail != "failed") ++loads;
        else if (loads == n && e.what == feed && e.accepted) out.push_back(e.seq);
    }
    return out;
}
static int count(const std::string &what, const std::string &detail = "") {
    int c = 0;
    for (const auto &e : fake::state().snapshot()) c += e.what == what && (detail.empty() || e.detail == detail);
    return c;
}
static int lg_loads_ok() {
    int c = 0;
    for (const auto &e : fake::state().snapshot()) c += e.what == "lg.load" && e.detail.rfind("failed:", 0) != 0;
    return c;
}
static std::string list(const std::vector<int64_t> &v) {
    std::string s;
    for (size_t i = 0; i < v.size() && i < 40; ++i) s += (i ? " " : "") + std::to_string(v[i]);
    if (v.size() > 40) s += " ...";
    return s;
}
static bool contiguous(const std::vector<int64_t> &v, int64_t from, int64_t to) {
    if (v.size() != static_cast<size_t>(to - from + 1)) return false;
    for (size_t i = 0; i < v.size(); ++i) if (v[i] != from + static_cast<int64_t>(i)) return false;
    return true;
}

struct Server {
    MediaServer ms;
    std::string path;
    explicit Server(const char *name) {
        fake::state().reset();
        fake_log::clear();
        path = sock_dir() + "/media-test-" + name + ".sock";
        std::string why;
        if (!ms.start(path, true, &why, "ndl")) { std::fprintf(stderr, "start: %s\n", why.c_str()); std::exit(2); }
    }
};

// An LG stream that failed before its first picture: the pipeline reports an error and the next feed notices it.
static void fail_before_first_picture(Client &c, uint64_t next_seq) {
    fake::state().lg.load()->on_event(1, 100, "");
    c.unit(next_seq);
}

static void test_flush_while_feeding() {  // finding 1
    Server s("flush");
    fake::state().lg_available = false;
    fake::state().feed_sleep_us = 300;
    fake::state().full_every = 3;
    Client c(s.path);
    c.load("");
    c.audio_none();
    uint64_t seq = 0;
    for (int round = 0; round < 120; ++round) {
        for (int i = 0; i < 25; ++i) c.unit(seq++);
        std::this_thread::sleep_for(1ms);
        c.flush();
    }
    wait_until([] { return false; }, 300ms);
    std::vector<int64_t> fed = accepted_after_load(0, "ndl.feed");
    bool increasing = true;
    for (size_t i = 1; i < fed.size(); ++i) increasing &= fed[i] > fed[i - 1];
    check(fake::state().bad_reads.load() == 0, "f1: every unit the decoder read was intact while FLUSH cleared the queue (" +
          std::to_string(fake::state().bad_reads.load()) + " bad of " + std::to_string(fake::state().feeds.load()) + " feeds)");
    check(increasing && !fed.empty(), "f1: units accepted across seeks go in order (" + std::to_string(fed.size()) + " accepted)");
}

static void test_full_refeeds_in_order() {  // finding 1 (the refused unit goes back)
    Server s("full");
    fake::state().lg_available = false;
    fake::state().full_every = 2;
    Client c(s.path);
    c.load("");
    c.audio_none();
    for (uint64_t i = 0; i < 200; ++i) c.unit(i);  // each refusal holds the stream back 10 ms
    wait_until([] { return accepted_after_load(0, "ndl.feed").size() >= 200; }, 8000ms);
    const auto fed = accepted_after_load(0, "ndl.feed");
    check(contiguous(fed, 0, 199), "f1: units refused as full are fed again, all of them, in order (" + std::to_string(fed.size()) + " accepted)");
}

static void test_retry_keeps_newer_owner() {  // finding 2
    Server s("owner");
    Client a(s.path);
    a.load("wA");
    a.audio_none();
    for (uint64_t i = 0; i < 5; ++i) a.unit(i);
    wait_until([] { return accepted_after_load(0).size() >= 5; }, 2000ms);
    fail_before_first_picture(a, 5);
    const bool retrying = wait_until([] { return fake_log::contains("attempt 2 to load it again"); }, 2000ms);
    Client b(s.path);
    b.load("wB");
    b.audio_none();
    wait_until([] { return count("lg.load", "wB") == 1; }, 2000ms);
    std::this_thread::sleep_for(1200ms);  // past A's retry delay (500 ms)
    const auto ev = fake::state().snapshot();
    bool a_after_b = false, seen_b = false;
    for (const auto &e : ev) {
        if (e.what == "lg.load" && e.detail == "wB") seen_b = true;
        else if (seen_b && e.what == "lg.load" && e.detail == "wA") a_after_b = true;
    }
    check(retrying && seen_b, "f2: setup: A retried and B loaded");
    check(!a_after_b && s.ms.owner_id() == 2, "f2: A's retry does not take the decoder back from B (owner " + std::to_string(s.ms.owner_id()) + ")");
    int unload_after_b = 0;
    seen_b = false;
    for (const auto &e : ev) {
        if (e.what == "lg.load" && e.detail == "wB") seen_b = true;
        else if (seen_b && e.what == "lg.unload") ++unload_after_b;
    }
    check(unload_after_b == 0, "f2: B's pipeline is not unloaded by A's retry");
}

static void test_failed_retry_closes() {  // finding 5
    Server s("retryfail");
    Client a(s.path);
    a.load("wA");
    a.audio_none();
    for (uint64_t i = 0; i < 5; ++i) a.unit(i);
    wait_until([] { return accepted_after_load(0).size() >= 5; }, 2000ms);
    fake::state().lg_load_ok = false;
    fake::state().ndl_load_ok = false;
    fail_before_first_picture(a, 5);
    check(a.closed_within(3000ms), "f5: a retry that loads no decoder closes the stream (the page gets a media error)");
}

static void test_first_load_fails_closes() {  // finding 12
    Server s("loadfail");
    fake::state().lg_load_ok = false;
    fake::state().ndl_load_ok = false;
    Client a(s.path);
    a.load("wA");
    a.audio_none();
    a.unit(0);
    check(a.closed_within(2000ms), "f12: a first Load that loads no decoder closes the stream");
}

static void test_retry_after_seek_replays_since_seek() {  // finding 6
    Server s("seekreplay");
    Client a(s.path);
    a.load("wA");
    a.audio_none();
    for (uint64_t i = 0; i < 10; ++i) a.unit(i);
    wait_until([] { return accepted_after_load(0).size() >= 10; }, 2000ms);
    a.flush();
    for (uint64_t i = 100; i < 110; ++i) a.unit(i);
    wait_until([] { return accepted_after_load(0).size() >= 20; }, 2000ms);
    fail_before_first_picture(a, 110);
    wait_until([] { return lg_loads_ok() >= 2 && accepted_after_load(1).size() >= 11; }, 3000ms);
    const auto again = accepted_after_load(1);
    check(contiguous(again, 100, 110), "f6: a retry after a seek replays what was fed since the seek, from the seek on (" + list(again) + ")");
}

static void test_retry_replays_everything_in_order() {  // finding 14
    Server s("replayall");
    fake::state().feed_sleep_us = 20000;
    Client a(s.path);
    a.load("wA");
    a.audio_none();
    for (uint64_t i = 0; i < 40; ++i) a.unit(i);
    wait_until([] { return accepted_after_load(0).size() >= 5; }, 2000ms);
    fake::state().lg.load()->on_event(1, 100, "");  // the next feed notices it
    wait_until([] { return lg_loads_ok() >= 2 && accepted_after_load(1).size() >= 40; }, 5000ms);
    const auto again = accepted_after_load(1);
    check(contiguous(again, 0, 39), "f14: the retry feeds every unit the failed pipeline was given, then the rest, none lost (" + list(again) + ")");
}

static bool paused_after_load(int n) {
    int loads = -1;
    bool paused = false, autoplay = false;
    for (const auto &e : fake::state().snapshot()) {
        if (e.what == "lg.load" && e.detail.rfind("failed:", 0) != 0) ++loads;
        else if (loads == n && e.what == "lg.pause") paused = e.seq == 1;
        else if (loads == n && e.what == "lg.autoplay" && !paused) autoplay = true;
    }
    return paused && !autoplay;
}

static void test_pause_across_retry() {  // finding 7
    Server s("pauseretry");
    Client a(s.path);
    a.load("wA");
    a.audio_none();
    for (uint64_t i = 0; i < 5; ++i) a.unit(i);
    wait_until([] { return accepted_after_load(0).size() >= 5; }, 2000ms);
    a.pause(true);
    wait_until([] { return count("lg.pause") == 1; }, 1000ms);
    fail_before_first_picture(a, 5);
    wait_until([] { return lg_loads_ok() >= 2 && !accepted_after_load(1).empty(); }, 3000ms);
    check(paused_after_load(1), "f7: a stream paused before a retry stays paused on the reloaded pipeline");
}

static void test_pause_during_held_load() {  // finding 7
    Server s("pauseheld");
    Client a(s.path);
    a.load("wA");  // no word about audio: the Load waits up to a second
    a.pause(true);
    a.unit(0);
    wait_until([] { return lg_loads_ok() >= 1 && !accepted_after_load(0).empty(); }, 3000ms);
    check(paused_after_load(0), "f7: a pause sent while the Load was held applies once it loads");
}

// An event from the pipeline of the n-th LG load (0-based), as LG delivers it: through the callback with that Load's data.
// MEDIA_TEST_OLD_EVENT_ROUTING=1 delivers it the way it was before the tokens (the data was the player itself), for comparison.
static void lg_event_of_load(size_t n, int type, int64_t num) {
    void *token = nullptr;
    {
        std::lock_guard<std::mutex> l(fake::state().mu);
        if (n < fake::state().lg_tokens.size()) token = fake::state().lg_tokens[n];
    }
    if (std::getenv("MEDIA_TEST_OLD_EVENT_ROUTING")) {
        fake::state().lg.load()->on_event(type, num, "");
        return;
    }
    browser::lg_event_tokens().dispatch(token, [&](browser::LgMediaPlayer *p) { p->on_event(type, num, ""); });
}

// Stream A plays on LG's pipeline, stream B's Load takes the decoder (A's pipeline is unloaded, B's loaded on the same player).
static void two_loads(Server &s, Client &a, Client &b) {
    (void)s;
    a.load("wA");
    a.audio_none();
    for (uint64_t i = 0; i < 5; ++i) a.unit(i);
    wait_until([] { return accepted_after_load(0).size() >= 5; }, 2000ms);
    b.load("wB");
    b.audio_none();
    wait_until([] { return count("lg.load", "wB") == 1; }, 2000ms);
}

static void test_late_error_of_unloaded_pipeline() {  // finding 4
    Server s("lateerror");
    Client a(s.path), b(s.path);
    two_loads(s, a, b);
    lg_event_of_load(0, 1, 100);  // A's pipeline reports an error while it is taken down
    for (uint64_t i = 0; i < 5; ++i) b.unit(i);
    std::this_thread::sleep_for(1200ms);  // a retry would have run by now (poll 100 ms + 500 ms delay)
    check(!fake_log::contains("stream 2: LG pipeline error") && lg_loads_ok() == 2,
          "f4: an error of the unloaded pipeline does not make the next load retry (" + std::to_string(lg_loads_ok()) + " LG loads)");
}

static void test_late_frames_of_unloaded_pipeline() {  // finding 4
    Server s("lateframes");
    Client a(s.path), b(s.path);
    two_loads(s, a, b);
    lg_event_of_load(0, 2, 50);   // A's pipeline's last frame counts
    lg_event_of_load(1, 1, 100);  // B's own pipeline fails before its first picture
    for (uint64_t i = 0; i < 5; ++i) b.unit(i);
    const bool retried = wait_until([] { return fake_log::contains("stream 2: LG pipeline error 100 before its first picture"); }, 2000ms);
    check(retried, "f4: frames the unloaded pipeline presented do not count for the next load (its early error is retried)");
}

static void test_area_after_release() {  // finding 10
    Server s("arearelease");
    Client a(s.path);
    a.load("wA");
    a.audio_none();
    a.area(10, 10, 100, 100);
    wait_until([] { return lg_loads_ok() >= 1; }, 2000ms);
    std::this_thread::sleep_for(50ms);
    s.ms.commit_area(true);
    check(s.ms.hole().width == 100, "f10: setup: the owner's area makes the hole");
    s.ms.release_owner("test");
    check(s.ms.hole().empty(), "f10: setup: release_owner clears the hole");
    a.area(20, 20, 200, 200);
    std::this_thread::sleep_for(100ms);
    s.ms.commit_area(true);
    check(s.ms.hole().empty(), "f10: a released stream's area does not bring the hole back (hole width " + std::to_string(static_cast<int>(s.ms.hole().width)) + ")");
}

static void test_area_of_other_stream() {  // finding 11
    Server s("areaother");
    Client a(s.path);
    a.load("wA");
    a.audio_none();
    wait_until([] { return lg_loads_ok() >= 1; }, 2000ms);
    a.area(10, 10, 100, 100);
    std::this_thread::sleep_for(50ms);
    s.ms.commit_area(true);
    a.area(30, 30, 100, 100);
    std::this_thread::sleep_for(50ms);
    Client b(s.path);
    b.area(500, 500, 50, 50);
    std::this_thread::sleep_for(100ms);
    s.ms.commit_area(true);
    check(s.ms.hole().x == 30, "f11: another stream's area does not displace the owner's newest one (hole x " + std::to_string(static_cast<int>(s.ms.hole().x)) + ")");
    b.load("wB");
    b.audio_none();
    wait_until([] { return count("lg.load", "wB") == 1; }, 2000ms);
    std::this_thread::sleep_for(50ms);
    s.ms.commit_area(true);
    check(s.ms.hole().x == 500, "f11: a stream that takes the decoder gets the hole of its last area (hole x " + std::to_string(static_cast<int>(s.ms.hole().x)) + ", width " + std::to_string(static_cast<int>(s.ms.hole().width)) + ")");
}

int main(int argc, char **argv) {
    const std::string only = argc > 1 ? argv[1] : "";
    const std::pair<const char *, void (*)()> tests[] = {
        {"flush", test_flush_while_feeding},       {"full", test_full_refeeds_in_order},
        {"owner", test_retry_keeps_newer_owner},   {"retryfail", test_failed_retry_closes},
        {"loadfail", test_first_load_fails_closes}, {"seekreplay", test_retry_after_seek_replays_since_seek},
        {"replayall", test_retry_replays_everything_in_order}, {"pauseretry", test_pause_across_retry},
        {"pauseheld", test_pause_during_held_load}, {"arearelease", test_area_after_release},
        {"areaother", test_area_of_other_stream},   {"lateerror", test_late_error_of_unloaded_pipeline},
        {"lateframes", test_late_frames_of_unloaded_pipeline},
    };
    for (const auto &[name, fn] : tests)
        if (only.empty() || only == name) fn();
    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
