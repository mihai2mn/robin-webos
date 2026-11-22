// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine_env.hpp"

#include <iostream>
#include <string>

static int failures = 0;
static void check(bool ok, const std::string &what) {
    std::cout << (ok ? "ok   " : "FAIL ") << what << '\n';
    if (!ok) ++failures;
}

int main() {
    using namespace browser;
    check(app_id_is_dev("com.example.wren.dev") && !app_id_is_dev("com.example.wren") && !app_id_is_dev(".dev") &&
              !app_id_is_dev("com.example.wren.devx") && !app_id_is_dev(""),
          "only an id ending in .dev is a dev id");
    for (const char *n : {"JSC_useJIT", "MSE_MAX_BUFFER_SIZE", "WPE_BROWSER_DEBUG_CONSOLE", "WPE_BROWSER_DEBUG_AUTOPLAY",
                          "WPE_BROWSER_DEBUG_ENGINE", "WPE_BROWSER_DEBUG_REQUESTS"}) {
        check(engine_env_allowed(n, false), std::string(n) + " passes on a release id");
        check(engine_env_allowed(n, true), std::string(n) + " passes unlocked");
    }
    for (const char *n : {"WPE_BROWSER_DEBUG_SCRIPT", "WPE_BROWSER_DEBUG_SETTINGS", "WPE_BROWSER_DEBUG_USER_AGENT",
                          "WPE_BROWSER_DEBUG_FORGET_COOKIES", "WPE_BROWSER_DEBUG_FUTURE_SWITCH", "WEBKIT_INSPECTOR_SERVER",
                          "WEBKIT_DISABLE_COMPOSITING_MODE", "G_TLS_GNUTLS_PRIORITY", "GST_DEBUG_FILE", "GST_DEBUG"}) {
        check(!engine_env_allowed(n, false), std::string(n) + " is refused on a release id");
        check(engine_env_allowed(n, true), std::string(n) + " passes unlocked");
    }
    for (const char *n : {"WEBKIT_EXEC_PATH", "WEBKIT_INJECTED_BUNDLE_PATH", "LD_PRELOAD", "PATH", "", "wpe_browser_debug_console"}) {
        check(!engine_env_allowed(n, true), std::string("'") + n + "' is never passed on");
    }
    return failures == 0 ? 0 : 1;
}
