// SPDX-License-Identifier: GPL-3.0-or-later
#include "profile.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <cstdlib>
#include <unistd.h>

namespace fs = std::filesystem;
static int failures = 0;

static void check(bool ok, const std::string &what) {
    std::cout << (ok ? "ok   " : "FAIL ") << what << '\n';
    if (!ok) ++failures;
}

int main() {
    // Host normalisation.
    check(browser::site_host("https://www.Example.ORG:8443/path?q=1#f") == "example.org", "strips scheme, www., port, path, lowercases");
    check(browser::site_host("http://127.0.0.1:8099/test.html") == "127.0.0.1", "keeps IPv4 host without port");
    check(browser::site_host("http://localhost:8099/test.html") == "localhost", "keeps localhost without port");
    check(browser::site_host("https://user:pass@sub.example.net/page") == "sub.example.net", "strips userinfo");
    check(browser::site_host("http://[::1]:8080/a") == "[::1]", "keeps IPv6 literal");
    check(browser::site_host("about:blank").empty(), "about: has no site host");
    check(browser::site_host("data:text/html,hi").empty(), "data: has no site host");
    check(browser::site_host("").empty(), "empty URL has no site host");

    // Permille snapping.
    check(browser::zoom_index_for_permille(1000) == 0, "1000 -> index 0");
    check(browser::zoom_index_for_permille(1500) == 3, "1500 -> index 3");
    check(browser::zoom_index_for_permille(1480) == 3, "1480 snaps to 1500");
    check(browser::zoom_index_for_permille(2500) == static_cast<int>(browser::kZoomLevels.size()) - 1, "above max snaps to max");

    // Persistence per host in Profile.
    char pattern[] = "/tmp/wpe-profile-zoom-test-XXXXXX";
    const std::string dir = ::mkdtemp(pattern);
    {
        browser::Profile p;
        p.load(dir);
        check(p.zoom_permille_for("https://a.example/page") == 1000, "default zoom is 100%");
        check(!p.has_site_zoom("https://a.example/page"), "no override initially");

        p.set_site_zoom("https://www.a.example/one", 3);  // 150%
        p.set_site_zoom("http://127.0.0.1:8099/v", 2);    // 125%
        p.set_site_zoom("about:blank", 4);                // ignored

        check(p.has_site_zoom("https://a.example/two"), "www. and non-www. share host zoom");
        check(p.zoom_permille_for("https://a.example/two") == 1500, "host a remembers 150%");
        check(p.zoom_permille_for("http://127.0.0.1:8099/other") == 1250, "127.0.0.1 remembers 125%");
        check(p.zoom_permille_for("http://localhost:8099/other") == 1000, "localhost stays at default 100%");
        check(p.zoom_permille_for("about:blank") == 1000, "about:blank stays at default");

        // Changing global default does not overwrite saved per-host zoom.
        p.settings.zoom = 1;  // 110%
        p.settings_changed();
        check(p.zoom_permille_for("http://localhost:8099/other") == 1100, "unconfigured host follows default 110%");
        check(p.zoom_permille_for("https://a.example/two") == 1500, "configured host keeps 150%");

        // Setting a host back to the current default clears its override.
        p.set_site_zoom("http://127.0.0.1:8099/v", 1);
        check(!p.has_site_zoom("http://127.0.0.1:8099/v"), "setting host to default clears override");

        p.flush_now();
        check(fs::exists(dir + "/zooms.json"), "zooms.json was written");

        // Icon URLs must be http(s) only (blocks file:// local reads in the image loader).
        p.set_icon("https://a.example/page", "file:///etc/passwd");
        p.set_icon("https://a.example/page", "javascript:alert(1)");
        p.set_icon("https://a.example/page", "data:image/png;base64,AA==");
        check(p.icon_for("https://a.example/page").empty(), "non-http icon URLs rejected");
        p.set_icon("https://a.example/page", "https://a.example/icon.png");
        check(p.icon_for("https://a.example/page") == "https://a.example/icon.png", "https icon URL accepted");
        p.clear_history(0);
        check(p.icon_for("https://a.example/page").empty(), "clearing all history clears saved site icons");
        p.add_bookmark("B", "https://www.b.example/home");
        p.set_icon("https://b.example/x", "https://b.example/icon.png");
        p.set_icon("https://c.example/x", "https://c.example/icon.png");
        p.clear_history(0);
        check(p.icon_for("https://b.example/y") == "https://b.example/icon.png", "a bookmarked site keeps its icon when history is cleared");
        check(p.icon_for("https://c.example/x").empty(), "an unbookmarked site's icon is cleared with the history");
        p.remove_bookmark("https://www.b.example/home");
        p.set_site_zoom("https://www.a.example/one", 3);  // restore zoom for persistence test below
        p.flush_now();
    }

    // Reload into a fresh Profile and verify persistence.
    {
        browser::Profile p2;
        p2.load(dir);
        check(p2.settings.zoom == 1, "reloaded default zoom is 110%");
        check(p2.has_site_zoom("https://a.example/"), "reloaded host a has override");
        check(p2.zoom_permille_for("https://a.example/another") == 1500, "reloaded host a is 150%");
        check(!p2.has_site_zoom("http://127.0.0.1:8099/"), "cleared host did not come back");
        check(p2.zoom_permille_for("http://localhost:8099/") == 1100, "reloaded default host is 110%");
    }

    // Corrupt / out-of-range values in zooms.json are safely ignored.
    {
        std::ofstream(dir + "/zooms.json") << R"({"bad.example": 99999, "str.example": "big", "good.example": 1750})";
        browser::Profile p3;
        p3.load(dir);
        check(!p3.has_site_zoom("https://bad.example/"), "out-of-range zoom ignored");
        check(!p3.has_site_zoom("https://str.example/"), "non-integer zoom ignored");
        check(p3.zoom_permille_for("https://good.example/") == 1750, "valid entry still loaded");
    }

    fs::remove_all(dir);
    return failures ? 1 : 0;
}
