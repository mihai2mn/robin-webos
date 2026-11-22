// SPDX-License-Identifier: GPL-3.0-or-later
// Logs saved for a bug report keep web addresses' host and path but never their query or fragment.
#include "log_export.hpp"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unistd.h>

static int failures = 0;

static void check(bool ok, const std::string &what) {
    std::cout << (ok ? "ok   " : "FAIL ") << what << '\n';
    if (!ok) ++failures;
}

static void same(const std::string &got, const std::string &want) {
    check(got == want, "\"" + got + "\" == \"" + want + "\"");
}

int main() {
    using browser::redact_urls;
    same(redact_urls("load https://www.youtube.com/tv#/watch?v=abc done"), "load https://www.youtube.com/tv?… done");
    same(redact_urls("GET https://lic.example/wv?token=SECRET&x=1"), "GET https://lic.example/wv?…");
    same(redact_urls("\"http://a.b/p/q\" and 'ws://c.d/s?id=9'"), "\"http://a.b/p/q\" and 'ws://c.d/s?…'");
    same(redact_urls("no url here?really#yes"), "no url here?really#yes");
    same(redact_urls("https://a.b/?q=1 then plain? text"), "https://a.b/?… then plain? text");
    same(redact_urls("(https://a.b/x?y), https://c.d"), "(https://a.b/x?…), https://c.d");
    same(redact_urls("https://user:secret@host.example/path?k=v"), "https://…@host.example/path?…");
    same(redact_urls("http://a@b:pw@host.example/p"), "http://…@host.example/p");
    same(redact_urls("https://host.example/@handle/post"), "https://host.example/@handle/post");
    same(redact_urls(""), "");
    // Schemes in any letter case.
    same(redact_urls("GET HTTPS://a.b/x?token=SECRET end"), "GET HTTPS://a.b/x?… end");
    same(redact_urls("Http://u:p@A.b/p#frag"), "Http://…@A.b/p?…");
    same(redact_urls("Wss://c.d/s?id=9"), "Wss://c.d/s?…");
    same(redact_urls("http://"), "http://");
    same(redact_urls("a https://x.y?q=1 b https://z.w#f"), "a https://x.y?… b https://z.w?…");

    // Big logs: no address at all, and many addresses, in linear time (the quadratic version took minutes at this size).
    {
        const auto t0 = std::chrono::steady_clock::now();
        std::string big;
        for (int k = 0; k < 200000; ++k) big += "plain log line number " + std::to_string(k) + " without links\n";
        check(redact_urls(big) == big, "a big log without addresses comes back unchanged");
        std::string many, want;
        for (int k = 0; k < 100000; ++k) {
            many += "see http://h" + std::to_string(k) + ".example/p?k=v ok\n";
            want += "see http://h" + std::to_string(k) + ".example/p?… ok\n";
        }
        check(redact_urls(many) == want, "a big log of addresses is redacted");
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        check(s < 5.0, "big logs take " + std::to_string(s) + " s (< 5)");
    }

    char pattern[] = "/tmp/wpe-logexport-test-XXXXXX";
    const std::string dir = ::mkdtemp(pattern);
    std::ofstream(dir + "/engine.log") << "first line\nopened https://bank.example/acct?session=XYZ\n";
    std::ofstream(dir + "/events.log") << std::string(5000, 'a') << "\nlast line\n";

    browser::LogExportRequest req;
    req.runtime_dir = dir;
    req.header = {"Wren 0.3.0", "TV: test"};
    req.max_bytes_per_log = 100;
    std::string err;
    const std::string out = browser::export_logs(req, dir, &err);
    check(!out.empty(), "file written: " + out + err);
    std::stringstream ss;
    ss << std::ifstream(out).rdbuf();
    const std::string text = ss.str();
    check(text.find("Wren 0.3.0\nTV: test\n") != std::string::npos, "header written");
    check(text.find("session=XYZ") == std::string::npos, "query removed");
    check(text.find("https://bank.example/acct?…") != std::string::npos, "host and path kept");
    check(text.find("engine.prev.log (") != std::string::npos && text.find(": not found") != std::string::npos,
          "missing log noted");
    check(text.find("only the end") != std::string::npos && text.find("last line") != std::string::npos &&
              text.find(std::string(200, 'a')) == std::string::npos,
          "long log cut to its end");
    check(browser::export_logs(req, dir + "/nope", &err).empty() && !err.empty(), "unwritable folder fails: " + err);

    std::system(("rm -rf '" + dir + "'").c_str());
    return failures == 0 ? 0 : 1;
}
