// SPDX-License-Identifier: GPL-3.0-or-later
// Host test of engine/src/popup_filter.h:  g++ -std=c++17 -I engine/src tools/ubo/test/popup_filter_test.cpp -o /tmp/pft && /tmp/pft [popups.txt]
#include "popup_filter.h"

#include <cstdio>
#include <fstream>
#include <sstream>

static int fails = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL line %d: %s\n", __LINE__, #cond); ++fails; } } while (0)

int main(int argc, char **argv)
{
    popup::Rules r;
    r.add_text(
        "! comment\n"
        "||ads.example^$popup\n"
        "||bit.ly^$popup,domain=news.example|~sub.news.example\n"
        "/?placement=*&redirect$popup\n"
        "|blob:$popup,domain=video.example\n"
        "||tracker.example^$popup,3p\n"
        "@@||ads.example/ok^$popup\n"
        "@@||accounts.example^$popup\n"
        "/regex.*/$popup\n"
        "||script.example^$script\n"
        "||nonpopup.example^\n"
        "example.org##.banner\n"
        "||wild.*^$popup\n");
    CHECK(r.size() == 8);  // the regex, the script one, the plain one and the cosmetic one are not taken
    CHECK(r.blocks("https://ads.example/x", "https://site.test/"));
    CHECK(r.blocks("https://a.b.ads.example/x?y=1", "https://site.test/"));
    CHECK(!r.blocks("https://notads.example/x", "https://site.test/"));
    CHECK(!r.blocks("https://ads.example/ok/page", "https://site.test/"));          // exception
    CHECK(r.blocks("https://bit.ly/abc", "https://news.example/story"));
    CHECK(r.blocks("https://bit.ly/abc", "https://www.news.example/story"));
    CHECK(!r.blocks("https://bit.ly/abc", "https://sub.news.example/story"));       // ~domain
    CHECK(!r.blocks("https://bit.ly/abc", "https://other.test/"));                  // domain= not met
    CHECK(r.blocks("https://x.test/?placement=7&redirect", "https://site.test/"));
    CHECK(r.blocks("blob:https://v.example/1234", "https://video.example/watch"));
    CHECK(!r.blocks("blob:https://v.example/1234", "https://other.test/watch"));
    CHECK(r.blocks("https://tracker.example/", "https://site.test/"));              // third party
    CHECK(!r.blocks("https://tracker.example/", "https://www.tracker.example/"));   // first party
    CHECK(!r.blocks("https://accounts.example/o/oauth2", "https://site.test/"));
    CHECK(r.blocks("https://wild.net/x", "https://site.test/"));
    CHECK(r.blocks("https://a.wild.co.uk/x", "https://site.test/"));
    CHECK(!r.blocks("", "https://site.test/"));
    CHECK(popup::Rules::site_of("a.b.example.co.uk") == "example.co.uk");
    CHECK(popup::Rules::site_of("www.example.com") == "example.com");

    if (argc > 1) {  // a real list: how many it takes, and a few spot checks
        std::ifstream f(argv[1]);
        std::stringstream ss;
        ss << f.rdbuf();
        popup::Rules big;
        const size_t n = big.add_text(ss.str());
        std::printf("%s: %zu rules taken\n", argv[1], n);
        CHECK(n > 100);
        CHECK(!big.blocks("https://accounts.google.com/o/oauth2/auth", "https://site.test/"));
        CHECK(!big.blocks("https://www.wikipedia.org/", "https://site.test/"));
    }
    std::printf(fails ? "%d FAILED\n" : "all ok\n", fails);
    return fails ? 1 : 0;
}
