// SPDX-License-Identifier: GPL-3.0-or-later
// Which video units to leave out at the start of a feed (after a load, a reload or a seek).
//
// A decoder started at a keyframe that is not a clean start (HEVC CRA/BLA; H.264 non-IDR I picture) is followed in decode order by
// "leading" pictures that PRECEDE the keyframe in presentation order and may reference pictures from before the start. Those pictures
// cannot be shown right (a stream cut there never had their references) and the decoder smears them. They are never presentable after a
// start there, so they are left out; everything else is fed unchanged:
//  - H.264, first picture IDR: nothing is dropped (an IDR has no leading pictures that reference earlier ones).
//  - H.264, first picture non-IDR: units whose pts is below the first unit's pts are dropped until the first unit at or above it (the
//    leading B pictures follow their I picture directly in decode order).
//  - HEVC, first picture CRA or BLA (types 21, 16-18): RASL pictures (types 8, 9; they reference what came before the keyframe) are
//    dropped until the first picture of another kind than RASL/RADL. RADL pictures (6, 7) only reference the keyframe: kept.
//  - HEVC IDR (19, 20), a first unit that is no keyframe, other codecs: nothing is dropped.
// Units are in decode order, Annex B.
#pragma once

#include <cstddef>
#include <cstdint>

namespace browser {

class StartFilter {
public:
    enum class Codec { Other, H264, Hevc };

    // The next video unit starts a feed.
    void arm() { state_ = State::First; dropped_ = 0; }
    // Units dropped since arm().
    int dropped() const { return dropped_; }

    // True: leave this unit out. Call for every video unit, in decode order, after arm().
    bool drop(Codec codec, const uint8_t *d, size_t n, int64_t pts_us) {
        if (state_ == State::Idle) return false;
        const int t = first_vcl_type(codec, d, n);
        if (t < 0) return false;  // nothing readable: keep it
        if (state_ == State::First) {
            state_ = State::Idle;
            if (codec == Codec::H264 && t == 1) {
                state_ = State::H264Leading;
                key_pts_ = pts_us;
            } else if (codec == Codec::Hevc && (t == 21 || (t >= 16 && t <= 18))) {
                state_ = State::HevcLeading;
            }
            return false;  // the first unit itself is always fed
        }
        if (state_ == State::H264Leading) {
            if (pts_us >= key_pts_) { state_ = State::Idle; return false; }
            ++dropped_;
            return true;
        }
        // HevcLeading
        if (t == 8 || t == 9) { ++dropped_; return true; }
        if (t == 6 || t == 7) return false;
        state_ = State::Idle;
        return false;
    }

    // The type of the first slice NAL unit (H.264: 1..5; HEVC: 0..21), -1 if none is found.
    static int first_vcl_type(Codec codec, const uint8_t *d, size_t n) {
        for (size_t i = 0; i + 3 < n; ++i) {
            if (d[i] != 0 || d[i + 1] != 0 || d[i + 2] != 1) continue;
            if (codec == Codec::H264) {
                const int t = d[i + 3] & 0x1f;
                if (t >= 1 && t <= 5) return t;
            } else if (codec == Codec::Hevc) {
                const int t = (d[i + 3] >> 1) & 0x3f;
                if (t <= 21) return t;
            } else {
                return -1;
            }
            i += 2;
        }
        return -1;
    }

private:
    enum class State { Idle, First, H264Leading, HevcLeading };
    State state_ = State::Idle;
    int64_t key_pts_ = 0;
    int dropped_ = 0;
};

}  // namespace browser
