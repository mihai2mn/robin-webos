// SPDX-License-Identifier: GPL-3.0-or-later
// What the host test's fake decoders (LG pipeline and NDL) were asked to do, and how they answer.
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace browser { class LgMediaPlayer; }

namespace fake {
struct Event {
    Event(std::string w, std::string d = {}, int64_t s = -1, bool a = false, bool i = true)
        : what(std::move(w)), detail(std::move(d)), seq(s), accepted(a), intact(i) {}
    std::string what;     // "lg.load", "lg.feed", "lg.unload", "lg.pause", "lg.flush", "ndl.load", "ndl.feed", ...
    std::string detail;   // lg.load: the window id
    int64_t seq = -1;     // feeds: the unit's number (from its payload); pause: 1/0
    bool accepted = false;  // feeds: not refused as full
    bool intact = true;     // feeds: the payload read back as it was sent
};
struct State {
    std::mutex mu;
    std::vector<Event> events;
    std::atomic<bool> lg_available{true};
    std::atomic<bool> lg_load_ok{true};
    std::atomic<bool> ndl_load_ok{true};
    std::atomic<int> feed_sleep_us{0};
    std::atomic<int> full_every{0};  // every n-th feed is refused as full (0: never)
    std::atomic<int> feeds{0};
    std::atomic<int> bad_reads{0};
    std::atomic<browser::LgMediaPlayer *> lg{nullptr};
    void reset() {
        std::lock_guard<std::mutex> l(mu);
        events.clear();
        lg_available = true; lg_load_ok = true; ndl_load_ok = true;
        feed_sleep_us = 0; full_every = 0; feeds = 0; bad_reads = 0;
    }
    void add(Event e) { std::lock_guard<std::mutex> l(mu); events.push_back(std::move(e)); }
    std::vector<Event> snapshot() { std::lock_guard<std::mutex> l(mu); return events; }
};
State &state();

// A test unit: 'U', its number as 16 hex digits, then filler derived from it (no zero bytes: never an Annex B start code).
std::vector<uint8_t> unit_payload(uint64_t seq, size_t size = 256);
// Reads the whole unit (as a decoder does) and checks it; -1 when it is not a test unit as sent.
int64_t read_unit(const uint8_t *d, size_t n, bool *intact);
// One fake feed: sleeps, reads, records, answers full or not.
bool feed_unit(const char *what, const uint8_t *d, size_t n);
}  // namespace fake
