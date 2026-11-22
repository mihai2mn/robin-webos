// SPDX-License-Identifier: GPL-3.0-or-later
// Leading pictures at the start of a feed are left out; nothing else is.
#include "start_filter.hpp"

#include <iostream>
#include <vector>

using browser::StartFilter;
using C = StartFilter::Codec;
static int failures = 0;
static void check(bool ok, const char *what) { std::cout << (ok ? "ok   " : "FAIL ") << what << '\n'; if (!ok) ++failures; }

static std::vector<uint8_t> h264(int type) { return {0, 0, 0, 1, 0x09, 0xf0, 0, 0, 0, 1, static_cast<uint8_t>(0x60 | type), 0x88}; }
static std::vector<uint8_t> hevc(int type) { return {0, 0, 0, 1, 0x46, 0x01, 0, 0, 1, static_cast<uint8_t>(type << 1), 0x01, 0xaf}; }
static bool d(StartFilter &f, C c, const std::vector<uint8_t> &u, int64_t pts) { return f.drop(c, u.data(), u.size(), pts); }

int main() {
    {   // H.264 IDR start with B pictures: nothing dropped, even a unit with a lower pts
        StartFilter f; f.arm();
        check(!d(f, C::H264, h264(5), 100), "h264 IDR start kept");
        check(!d(f, C::H264, h264(1), 400), "h264 P kept");
        check(!d(f, C::H264, h264(1), 200), "h264 B after IDR kept");
        check(f.dropped() == 0, "h264 IDR: none dropped");
    }
    {   // H.264 non-IDR (open GOP) start: leading B (pts below) dropped, then trailing kept
        StartFilter f; f.arm();
        check(!d(f, C::H264, h264(1), 1000), "h264 non-IDR start kept");
        check(d(f, C::H264, h264(1), 900), "h264 leading B dropped");
        check(d(f, C::H264, h264(1), 950), "h264 second leading B dropped");
        check(!d(f, C::H264, h264(1), 1300), "h264 first trailing kept");
        check(!d(f, C::H264, h264(1), 1100), "h264 B after trailing kept (filter is over)");
        check(f.dropped() == 2, "h264: two dropped");
    }
    {   // HEVC CRA start: RASL dropped, RADL and trailing kept
        StartFilter f; f.arm();
        check(!d(f, C::Hevc, hevc(21), 1000), "hevc CRA start kept");
        check(d(f, C::Hevc, hevc(8), 900), "hevc RASL_N dropped");
        check(!d(f, C::Hevc, hevc(7), 920), "hevc RADL_R kept");
        check(d(f, C::Hevc, hevc(9), 950), "hevc RASL_R dropped");
        check(!d(f, C::Hevc, hevc(1), 1300), "hevc trailing kept");
        check(!d(f, C::Hevc, hevc(9), 1200), "hevc RASL after trailing kept (not at the start)");
    }
    {   // HEVC IDR start: nothing dropped
        StartFilter f; f.arm();
        check(!d(f, C::Hevc, hevc(19), 1000), "hevc IDR_W_RADL kept");
        check(!d(f, C::Hevc, hevc(6), 900), "hevc RADL after IDR kept");
    }
    {   // parameter sets in front, then the slice: the slice type is what counts
        StartFilter f; f.arm();
        std::vector<uint8_t> u = {0, 0, 0, 1, 0x67, 1, 2, 0, 0, 0, 1, 0x68, 3, 0, 0, 1, 0x61, 9};
        check(!f.drop(C::H264, u.data(), u.size(), 500), "SPS PPS non-IDR first unit kept");
        check(d(f, C::H264, h264(1), 400), "then leading dropped");
    }
    {   // not armed / unreadable / other codec
        StartFilter f;
        check(!d(f, C::H264, h264(1), 1), "not armed: kept");
        f.arm();
        std::vector<uint8_t> junk = {1, 2, 3, 4, 5};
        check(!f.drop(C::H264, junk.data(), junk.size(), 1), "unreadable first unit kept");
        check(!d(f, C::H264, h264(1), 0), "unreadable first unit leaves the filter armed: next is the start");
        check(!f.drop(C::Other, junk.data(), junk.size(), 1), "other codec kept");
    }
    {   // re-arm (seek)
        StartFilter f; f.arm();
        d(f, C::H264, h264(1), 1000); d(f, C::H264, h264(1), 1500);
        f.arm();
        check(!d(f, C::H264, h264(5), 2000), "re-armed: IDR start kept");
        check(!d(f, C::H264, h264(1), 1900), "re-armed after IDR: nothing dropped");
    }
    std::cout << (failures ? "FAILED\n" : "passed\n");
    return failures ? 1 : 0;
}
