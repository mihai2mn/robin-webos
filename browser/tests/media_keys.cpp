// SPDX-License-Identifier: GPL-3.0-or-later
#include "media_keys.hpp"
#include "wpe_ipc.h"

#include <iostream>
#include <string>

static int failures = 0;

static void check(bool ok, const std::string &what) {
    std::cout << (ok ? "ok   " : "FAIL ") << what << '\n';
    if (!ok) ++failures;
}

int main() {
    using A = wnk::input::Action;
    using M = browser::MediaKeyAction;

    // Kit actions map to the right media commands.
    check(browser::media_key_action(A::PlayPause, 261, 0) == M::PlayPause, "PlayPause -> PlayPause");
    check(browser::media_key_action(A::Play, 450, 0) == M::PlayPause, "Play -> PlayPause toggle");
    check(browser::media_key_action(A::Pause, 72, 0) == M::Pause, "Pause -> Pause");
    check(browser::media_key_action(A::Stop, 120, 0) == M::Stop, "Stop -> Stop");
    check(browser::media_key_action(A::Rewind, 285, 0) == M::Rewind, "Rewind -> Rewind");
    check(browser::media_key_action(A::FastForward, 286, 0) == M::FastForward, "FastForward -> FastForward");

    // Host Spacebar must NOT be captured as a remote media key even though host_keymap maps it to PlayPause.
    check(browser::media_key_action(A::PlayPause, 44, ' ') == M::None, "Spacebar (sc=44) is ignored");
    check(browser::media_key_action(A::PlayPause, 0, ' ') == M::None, "Spacebar (kc=' ') is ignored");
    check(browser::media_key_action(A::Up, 82, 0) == M::None, "D-pad Up is ignored");
    check(browser::media_key_action(A::Confirm, 40, '\r') == M::None, "OK is ignored");

    // Raw scancode fallback works even if Action is Unknown.
    check(browser::media_key_action(A::Unknown, 261, 0) == M::PlayPause, "raw AUDIOPLAY scancode");
    check(browser::media_key_action(A::Unknown, 72, 0) == M::Pause, "raw PAUSE scancode");
    check(browser::media_key_action(A::Unknown, 120, 0) == M::Stop, "raw STOP scancode");
    check(browser::media_key_action(A::Unknown, 260, 0) == M::Stop, "raw AUDIOSTOP scancode");
    check(browser::media_key_action(A::Unknown, 285, 0) == M::Rewind, "raw AUDIOREWIND scancode");
    check(browser::media_key_action(A::Unknown, 286, 0) == M::FastForward, "raw AUDIOFASTFORWARD scancode");

    // Every generated script fits inside WPE_IPC_MAX_TEXT and contains the expected operation.
    check(browser::media_key_script(M::None).empty(), "None script is empty");
    for (M m : {M::PlayPause, M::Pause, M::Stop, M::Rewind, M::FastForward}) {
        const std::string js = browser::media_key_script(m);
        check(!js.empty(), std::string("script non-empty for ") + browser::media_key_label(m));
        check(js.size() < WPE_IPC_MAX_TEXT, std::string("script fits WPE_IPC_MAX_TEXT (") + std::to_string(js.size()) + ")");
        check(js.rfind("(function(){", 0) == 0 && js.size() > 5 && js.substr(js.size() - 5) == "})();",
              std::string("script is an IIFE for ") + browser::media_key_label(m));
    }
    check(browser::media_key_script(M::Rewind).find("-10") != std::string::npos, "rewind seeks -10 s");
    check(browser::media_key_script(M::FastForward).find("+10") != std::string::npos, "fast-forward seeks +10 s");
    check(browser::media_key_script(M::Stop).find("currentTime=0") != std::string::npos, "stop resets currentTime to 0");
    check(browser::media_key_script(M::PlayPause).find("v.play()") != std::string::npos &&
              browser::media_key_script(M::PlayPause).find("v.pause()") != std::string::npos,
          "play/pause toggles play and pause");

    return failures ? 1 : 0;
}
