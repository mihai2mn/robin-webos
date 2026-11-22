// SPDX-License-Identifier: GPL-3.0-or-later
// The profile writer: flush() on the worker pool racing flush_now() on the main thread, a corrupt file kept aside,
// and settings.json with values of the wrong type.
#include "profile.hpp"

#include "wnk/async/async.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

#include <cstdlib>
#include <unistd.h>

namespace fs = std::filesystem;
static int failures = 0;

static void check(bool ok, const std::string &what) {
    std::cout << (ok ? "ok   " : "FAIL ") << what << '\n';
    if (!ok) ++failures;
}

static std::string slurp(const std::string &p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static void put(const std::string &p, const std::string &text) { std::ofstream(p, std::ios::binary) << text; }

int main() {
    char pattern[] = "/tmp/wpe-profile-writer-test-XXXXXX";
    const std::string dir = ::mkdtemp(pattern);

    // 1. flush() on the pool and flush_now() on this thread, many times over: every file is complete JSON, the last state wins,
    //    and no temp file is left behind.
    {
        wnk::async::WorkerPool pool(2);
        browser::Profile p;
        p.load(dir + "/race");
        double now = 0;
        const int rounds = 300;
        for (int i = 0; i < rounds; ++i) {
            p.add_visit("t" + std::to_string(i), "https://example.org/" + std::to_string(i));
            p.add_bookmark("b" + std::to_string(i), "https://example.org/b" + std::to_string(i));
            now += 5000;
            p.flush(pool, now);  // snapshot i goes to the pool
            if (i % 3 == 0) {
                p.add_visit("n" + std::to_string(i), "https://example.net/" + std::to_string(i));
                p.flush_now();  // a newer snapshot, written here while the pool may still be writing the older one
            }
            const std::string text = slurp(dir + "/race/history.json");
            if (text.empty()) continue;  // not written yet
            const auto h = nlohmann::json::parse(text, nullptr, false);
            if (!h.is_array()) { check(false, "history.json parses while writes are in flight (round " + std::to_string(i) + ")"); break; }
        }
        p.add_visit("last", "https://example.org/last");
        now += 5000;
        p.flush(pool, now);
        p.flush_now();
        for (int i = 0; i < 2000 && (pool.stats().queued != 0 || pool.stats().running != 0); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        browser::Profile q;
        q.load(dir + "/race");
        check(!q.history().empty() && q.history().front().url == "https://example.org/last", "the newest history entry is the one on disk");
        check(q.history().size() == p.history().size(), "history on disk has every entry");
        check(q.bookmarks().size() == p.bookmarks().size(), "bookmarks on disk have every entry");
        size_t strays = 0;
        for (const auto &e : fs::directory_iterator(dir + "/race"))
            if (e.path().filename().string().find(".tmp") != std::string::npos) ++strays;
        check(strays == 0, "no temp file left behind");
    }

    // 2. A corrupt file is kept aside, not overwritten by the next save.
    {
        fs::create_directories(dir + "/bad");
        put(dir + "/bad/history.json", "[{\"title\":\"x\",\"url\":\"https://a.example/\",\"time\":1},{\"tit");
        put(dir + "/bad/bookmarks.json", "{\"not\":\"an array\"}");
        browser::Profile p;
        p.load(dir + "/bad");
        check(p.history().empty() && p.bookmarks().empty(), "corrupt files load as empty");
        check(fs::exists(dir + "/bad/history.json.corrupt") && slurp(dir + "/bad/history.json.corrupt").find("a.example") != std::string::npos,
              "corrupt history kept as history.json.corrupt");
        check(fs::exists(dir + "/bad/bookmarks.json.corrupt"), "wrong-kind bookmarks kept aside");
        p.add_visit("new", "https://b.example/");
        p.flush_now();
        check(slurp(dir + "/bad/history.json.corrupt").find("a.example") != std::string::npos, "saving does not touch the kept copy");
        browser::Profile q;
        q.load(dir + "/bad");
        check(q.history().size() == 1, "the new history is saved");
    }

    // 3. Missing and empty files are not corrupt.
    {
        fs::create_directories(dir + "/empty");
        put(dir + "/empty/history.json", "");
        browser::Profile p;
        p.load(dir + "/empty");
        check(!fs::exists(dir + "/empty/history.json.corrupt"), "an empty file is not kept aside");
    }

    // 4. Wrong value types in settings.json fall back to defaults instead of throwing.
    {
        fs::create_directories(dir + "/types");
        put(dir + "/types/settings.json",
            "{\"show_bookmarks_bar\":\"yes\",\"search_engine\":\"1\",\"zoom\":null,\"restore_session\":3,\"sharp_text\":[]}");
        browser::Profile p;
        bool threw = false;
        try { p.load(dir + "/types"); } catch (...) { threw = true; }
        check(!threw, "wrong value types do not throw");
        check(p.settings.show_bookmarks_bar == true && p.settings.search_engine == 0 && p.settings.zoom == 0 &&
                  p.settings.restore_session == false && p.settings.sharp_text == true,
              "wrong value types give the defaults");
        put(dir + "/types/settings.json", "{\"search_engine\":2,\"zoom\":99,\"restore_session\":true,\"show_bookmarks_bar\":false}");
        browser::Profile q;
        q.load(dir + "/types");
        check(q.settings.search_engine == 2 && q.settings.zoom == static_cast<int>(browser::kZoomLevels.size()) - 1 &&
                  q.settings.restore_session && !q.settings.show_bookmarks_bar,
              "right types are read and clamped");
    }

    // 5. An unchanged session is not written again; a changed one is.
    {
        browser::Profile p;
        p.load(dir + "/session");
        p.set_session({{"A", "https://a.example/"}, {"B", "https://b.example/"}}, 1);
        p.flush_now();
        const std::string file = dir + "/session/session.json";
        check(fs::exists(file), "a new session is written");
        fs::remove(file);
        p.set_session({{"A", "https://a.example/"}, {"B", "https://b.example/"}}, 1);
        p.flush_now();
        check(!fs::exists(file), "the same session again is not written");
        p.set_session({{"A", "https://a.example/"}, {"B", "https://b.example/"}}, 0);
        p.flush_now();
        check(fs::exists(file), "another active tab is written");
        fs::remove(file);
        p.set_session({{"A", "https://a.example/"}, {"B2", "https://b.example/"}}, 0);
        p.flush_now();
        check(fs::exists(file), "another title is written");
    }

    std::error_code ec;
    fs::remove_all(dir, ec);
    return failures == 0 ? 0 : 1;
}
