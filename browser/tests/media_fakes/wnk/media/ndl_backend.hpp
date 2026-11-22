// SPDX-License-Identifier: GPL-3.0-or-later
// Host test stand-in for the kit's NDL backend (tests/media_server.cpp).
#pragma once

#include "fake_media.hpp"
#include "wnk/media/media.hpp"

#include <string>

namespace wnk::media {

class NdlBackend : public MediaBackend {
 public:
    static bool available(std::string * = nullptr) { return true; }
    static void set_app_foreground(bool) {}
    const char *name() const override { return "fake-ndl"; }
    BackendCaps caps() const override { return {}; }
    void set_event_callback(EventFn) override {}
    bool load(const StreamFormat &, std::string *error) override {
        const bool ok = fake::state().ndl_load_ok.load();
        fake::state().add({"ndl.load", ok ? "ok" : "failed"});
        if (!ok && error) *error = "fake NDL load refused";
        return ok;
    }
    Feed feed(const Packet &p) override {
        return fake::feed_unit("ndl.feed", p.data.data(), p.data.size()) ? Feed::Accepted : Feed::Full;
    }
    bool flush(std::string *) override { fake::state().add({"ndl.flush"}); return true; }
    void unload() override { fake::state().add({"ndl.unload"}); }
    void set_area(const Rect &) override {}
    bool pause(bool paused) override { fake::state().add({"ndl.pause", "", paused ? 1 : 0}); return true; }
    void end_of_stream() override { fake::state().add({"ndl.eos"}); }
};

}  // namespace wnk::media
