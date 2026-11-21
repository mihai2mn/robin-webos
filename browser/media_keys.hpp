// SPDX-License-Identifier: GPL-3.0-or-later
// Remote media keys (play, pause, stop, rewind, fast-forward) -> main-frame JavaScript that controls the
// page's main video (or audio fallback) through WPE_APP_RUN_JS.
#pragma once

#include "wnk/input/input.hpp"

#include <cstdint>
#include <string>

namespace browser {

enum class MediaKeyAction : uint8_t {
    None = 0,
    PlayPause,    // toggle play / pause
    Pause,        // pause
    Stop,         // pause and seek to 0
    Rewind,       // seek -10 s
    FastForward,  // seek +10 s
};

// Maps a remote key (kit Action + raw SDL scancode/keycode) to a media action for the page's main video.
// Returns None for non-media keys (and never intercepts Space, which belongs to the page).
MediaKeyAction media_key_action(wnk::input::Action action, int scancode, int keycode);

// Self-contained JavaScript (well under WPE_IPC_MAX_TEXT) that locates the page's main video element
// (checking fullscreen, active/visible videos, same-origin iframes, and open shadow roots, with an audio
// fallback) and performs `cmd`. Empty for MediaKeyAction::None.
std::string media_key_script(MediaKeyAction cmd);

// Short user-facing / log label for `cmd` (empty for None).
const char *media_key_label(MediaKeyAction cmd);

}  // namespace browser
