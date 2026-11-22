// SPDX-License-Identifier: GPL-3.0-or-later
// Host test stand-in for the kit's Starfish backend: never available (tests/media_server.cpp uses the LG player and NDL).
#pragma once

#include "wnk/media/media.hpp"

#include <string>

namespace wnk::media {

enum class AudioStack : uint8_t { Liblxa, LgAudioSink, AudioSinkNew, AdecSink, Other };
enum class VideoPlane : uint8_t { None, ExportedWindow, Acb };

class StarfishBackend : public MediaBackend {
 public:
    static bool available(std::string *why = nullptr) { if (why) *why = "fake"; return false; }
    static int pcm_lead_ms_for(AudioStack, const std::string &) { return 0; }
    static AudioStack detect_audio_stack() { return AudioStack::Other; }
    VideoPlane plane() const { return VideoPlane::None; }
    void set_area_cropped(const Rect &, float, float, float, float) {}
    void set_volume(int, bool) {}
    const char *name() const override { return "fake-starfish"; }
    BackendCaps caps() const override { return {}; }
    void set_event_callback(EventFn) override {}
    bool load(const StreamFormat &, std::string *) override { return false; }
    Feed feed(const Packet &) override { return Feed::Error; }
    bool flush(std::string *) override { return true; }
    void unload() override {}
    void set_area(const Rect &) override {}
};

}  // namespace wnk::media
