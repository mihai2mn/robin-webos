// SPDX-License-Identifier: GPL-3.0-or-later
// Which environment variables a launch parameter {"wpebEnv": {...}} may put in the engine's environment.
//
// Any app that can launch the browser with parameters can set them, so most are for developers only: the DEV app id (ends in
// ".dev") accepts every listed name, a release id only the harmless ones, unless the file <data folder>/dev-switches exists
// (a launch parameter cannot create it; someone with a shell on the TV can).
#pragma once

#include <string>

namespace browser {

inline bool env_starts_with(const std::string &s, const char *prefix) { return s.rfind(prefix, 0) == 0; }

// The names the engine launcher passes on at all (docs/usage.md): the prefix allowlist.
inline bool engine_env_listed(const std::string &name) {
    if (name == "WEBKIT_EXEC_PATH" || name == "WEBKIT_INJECTED_BUNDLE_PATH") return false;
    return env_starts_with(name, "WEBKIT_") || env_starts_with(name, "JSC_") || env_starts_with(name, "WPE_BROWSER_DEBUG") ||
           env_starts_with(name, "GST_DEBUG") || env_starts_with(name, "G_TLS_") || env_starts_with(name, "MSE_");
}

// Listed names that only tune or log: JSC_* / MSE_* (engine tuning) and a few debug switches that write to the engine log.
// Not these: WPE_BROWSER_DEBUG_SCRIPT (runs a script in every page), _SETTINGS, _USER_AGENT, _FORGET_COOKIES, _HYBRID,
// _PAGE_APPID, _VT_TEXTURE, WEBKIT_* (inspector servers, ...), GST_DEBUG* (GST_DEBUG_FILE writes a file), G_TLS_* (TLS priorities).
inline bool engine_env_harmless(const std::string &name) {
    if (env_starts_with(name, "JSC_") || env_starts_with(name, "MSE_")) return true;
    static const char *const kNames[] = {"WPE_BROWSER_DEBUG_CONSOLE", "WPE_BROWSER_DEBUG_AUTOPLAY", "WPE_BROWSER_DEBUG_ENGINE",
                                         "WPE_BROWSER_DEBUG_MESSAGES", "WPE_BROWSER_DEBUG_REQUESTS", "WPE_BROWSER_DEBUG_FRAMES",
                                         "WPE_BROWSER_DEBUG_UBO_LOG", "WPE_BROWSER_DEBUG_MEDIA_AREA"};
    for (const char *n : kNames)
        if (name == n) return true;
    return false;
}

inline bool app_id_is_dev(const std::string &app_id) {
    return app_id.size() > 4 && app_id.compare(app_id.size() - 4, 4, ".dev") == 0;
}

// `unlocked`: the app id is a dev id, or the dev-switches file exists.
inline bool engine_env_allowed(const std::string &name, bool unlocked) {
    return engine_env_listed(name) && (unlocked || engine_env_harmless(name));
}

}  // namespace browser
