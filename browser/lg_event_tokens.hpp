// SPDX-License-Identifier: GPL-3.0-or-later
// Which LG pipeline load an event belongs to (lg_media_player.cpp). StarfishMediaAPIs::Load takes a callback and a data pointer that
// every later event of that pipeline carries, from LG's own threads, for as long as the pipeline lives: also while (and after) the
// detached thread of unload() takes it down, when the same player already runs the next Load. With the player itself as the data,
// such a late error or frame count landed in the next load's counters (a spurious retry, or a retry that never came). The data is a
// token for one Load instead: an event whose token is no longer current is dropped.
#pragma once

#include <cstdint>
#include <mutex>
#include <unordered_map>

namespace browser {

class LgMediaPlayer;

class LgEventTokens {
public:
    // A new Load's token (never null): events carrying it go to p until it is retired.
    void *issue(LgMediaPlayer *p) {
        std::lock_guard<std::mutex> l(mu_);
        const uintptr_t t = ++next_;
        players_[t] = p;
        return reinterpret_cast<void *>(t);
    }
    // The Load's pipeline is going away: its events are dropped from here on. An event being delivered right now finishes first, so
    // after this returns none of the token's events reaches the player. Null: nothing.
    void retire(void *token) {
        if (token == nullptr) return;
        std::lock_guard<std::mutex> l(mu_);
        players_.erase(reinterpret_cast<uintptr_t>(token));
    }
    // Calls f(player) for a current token; false (nothing called) for a retired or unknown one.
    template <class F>
    bool dispatch(void *token, F &&f) {
        std::lock_guard<std::mutex> l(mu_);
        const auto it = players_.find(reinterpret_cast<uintptr_t>(token));
        if (it == players_.end()) return false;
        f(it->second);
        return true;
    }

private:
    std::mutex mu_;
    uintptr_t next_ = 0;
    std::unordered_map<uintptr_t, LgMediaPlayer *> players_;
};

// The process's one registry (LG's callbacks are plain functions).
inline LgEventTokens &lg_event_tokens() {
    static LgEventTokens tokens;
    return tokens;
}

}  // namespace browser
