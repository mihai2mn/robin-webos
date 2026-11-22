// SPDX-License-Identifier: GPL-3.0-or-later
// Cost and equivalence test of the pop-up rule matcher (engine/src/popup_filter.h) against the one it replaced
// (popup_filter_ref.h):  g++ -O2 -std=c++17 -I engine/src tools/ubo/test/popup_filter_cost_test.cpp -o /tmp/pfc && /tmp/pfc engine/ubo/popups.txt
// 1. a shipped rule with four wildcards against an address of 'n' dots: the old matcher's steps grow with n^3, the new one's time stays flat;
// 2. the shipped rules give the same decision as before on a sample of addresses made from the rules themselves.
#include "popup_filter.h"
#include "popup_filter_ref.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>

static int fails = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL line %d: %s\n", __LINE__, #cond); ++fails; } } while (0)

static double ms_since(std::chrono::steady_clock::time_point t0)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

int main(int argc, char **argv)
{
    const char *rule = "://link-file-*.*.*/*/$popup\n";
    popup::Rules nw;
    popup_ref::Rules ol;
    CHECK(nw.add_text(rule) == 1 && ol.add_text(rule) == 1);
    std::printf("%-8s %14s %12s %12s\n", "dots", "old steps", "old ms", "new ms");
    for (size_t n : {250, 500, 1000, 2000}) {
        const std::string url = "https://link-file-" + std::string(n, '.');
        auto t0 = std::chrono::steady_clock::now();
        popup_ref::steps = 0;
        const bool o = ol.blocks(url, "https://site.test/");
        const double om = ms_since(t0);
        t0 = std::chrono::steady_clock::now();
        const bool b = nw.blocks(url, "https://site.test/");
        const double nm = ms_since(t0);
        std::printf("%-8zu %14llu %12.2f %12.3f\n", n, popup_ref::steps, om, nm);
        CHECK(o == b);
    }
    {   // what a page can send: 5000+ dots (the old matcher would need ~2e10 steps, so only the new one is run)
        const std::string url = "https://link-file-" + std::string(20000, '.');
        const auto t0 = std::chrono::steady_clock::now();
        const bool b = nw.blocks(url, "https://site.test/");
        const double nm = ms_since(t0);
        std::printf("20000 dots, new only: %.3f ms (blocked=%d)\n", nm, (int)b);
        CHECK(nm < 50);
    }

    if (argc > 1) {
        std::ifstream f(argv[1]);
        std::stringstream ss;
        ss << f.rdbuf();
        popup::Rules a;
        popup_ref::Rules b;
        const size_t na = a.add_text(ss.str()), nb = b.add_text(ss.str());
        std::printf("shipped rules: new %zu, old %zu taken\n", na, nb);
        CHECK(na == nb && na > 100);
        // addresses made from each rule's pattern (a hit), the same cut short (near miss), and under a plain host
        std::vector<std::string> urls = {"https://site.test/", "https://x.test/a/b?c=d&e=f", "blob:https://v.test/1", "about:blank"};
        std::istringstream lines(ss.str());
        for (std::string l; std::getline(lines, l);) {
            if (l.compare(0, 2, "@@") == 0)
                l.erase(0, 2);
            l = l.substr(0, l.rfind('$'));
            std::string p;
            for (char c : l)
                if (c == '*')
                    p += "ab.c/d";
                else if (c == '^')
                    p += '/';
                else if (c != '|')
                    p += c;
            if (p.empty())
                continue;
            const bool full = p.find("://") != std::string::npos;
            urls.push_back(full ? p : "https://" + p);
            urls.push_back("https://h.test/p?" + p);
            urls.push_back("https://www." + p + "/end");
            urls.push_back("https://" + p.substr(0, p.size() - 1));
            urls.push_back("https://" + p.substr(1) + "?q");
        }
        size_t diff = 0, blocked = 0, total = 0;
        for (const char *opener : {"https://site.test/", "https://news.example/s", "https://www.video.example/w"})
            for (const std::string &u : urls) {
                const bool x = a.blocks(u, opener), y = b.blocks(u, opener);
                ++total;
                blocked += x;
                if (x != y && ++diff <= 10)
                    std::printf("DIFF new=%d old=%d  %s\n", (int)x, (int)y, u.c_str());
            }
        std::printf("%zu decisions compared, %zu blocked, %zu differ\n", total, blocked, diff);
        CHECK(diff == 0 && blocked > 100);
    }
    std::printf(fails ? "%d FAILED\n" : "all ok\n", fails);
    return fails ? 1 : 0;
}
