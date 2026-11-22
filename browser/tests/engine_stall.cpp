// SPDX-License-Identifier: GPL-3.0-or-later
// An engine that stops reading its socket: send() gives up after EngineClient::kSendDeadlineMs instead of retrying for ever on the
// UI thread, the engine is stopped and reported lost through poll() (the exited callback), and stop() does not wait on it.
// The engine is this executable under the name wpe-engine (as tests/engine_environment.cpp does), which never reads.
#include "engine_client.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include <unistd.h>

using Clock = std::chrono::steady_clock;
static int failures = 0;
static void check(bool ok, const std::string &what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    std::fflush(stdout);
    if (!ok) ++failures;
}
static long ms_since(Clock::time_point t) {
    return static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t).count());
}

int main(int, char **argv) {
    if (std::filesystem::path(argv[0]).filename() == "wpe-engine") {
        for (;;) ::pause();  // a stuck engine: alive, never reads
    }
    // Before the deadline a full socket made the first blocked send spin for ever: fail instead of hanging the test.
    std::thread([] {
        std::this_thread::sleep_for(std::chrono::seconds(10));
        std::printf("FAIL a send to the stuck engine did not return within 10 s\nFAILED\n");
        std::fflush(stdout);
        std::_Exit(1);
    }).detach();

    char pattern[] = "/tmp/wpe-stall-test-XXXXXX";
    const char *dir = mkdtemp(pattern);
    if (!dir) return 1;
    const std::filesystem::path base(dir);
    std::filesystem::create_symlink(std::filesystem::read_symlink("/proc/self/exe"), base / "wpe-engine");
    std::filesystem::create_directories(base / "etc/ssl/certs");
    std::ofstream(base / "etc/ssl/certs/ca-certificates.crt") << "test fixture\n";
    browser::EngineLaunch launch;
    launch.engine_dir = base;
    launch.runtime_dir = base;
    launch.data_dir = base / "data";
    launch.cache_dir = base / "cache";
    int fitted = 0;  // messages the stuck engine's socket took
    std::string exited_why;
    int exited = 0;
    browser::EngineCallbacks cb;
    cb.exited = [&](const std::string &why) { ++exited; exited_why = why; };
    {
        browser::EngineClient client(cb);
        std::string error;
        if (!client.start(launch, error)) {
            std::printf("FAIL start: %s\n", error.c_str());
            return 1;
        }
        // Messages until the socket is full and the client gives up on the engine.
        long slowest = 0;
        int sent = 0;
        const auto t0 = Clock::now();
        while (client.running() && ms_since(t0) < 8000) {
            const auto t = Clock::now();
            client.tab_close(1);  // no payload: the same size as stop()'s QUIT
            slowest = std::max(slowest, ms_since(t));
            ++sent;
        }
        fitted = sent - 1;  // the last one was refused
        check(!client.running(), "a stuck engine is given up (after " + std::to_string(sent) + " messages)");
        check(slowest >= 400 && slowest <= 1500, "the blocked send waited about the deadline (" + std::to_string(slowest) + " ms)");
        const auto t1 = Clock::now();
        while (exited == 0 && ms_since(t1) < 3000) {
            client.poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        check(exited == 1 && exited_why.find("stopped reading") != std::string::npos,
              "poll() reports it lost once (exited: " + std::to_string(exited) + ", \"" + exited_why + "\")");
        client.poll();
        check(exited == 1, "and only once");
    }
    {
        // stop() on a stuck engine whose socket is full: its QUIT cannot go. As many messages as fitted above (the last, blocked
        // one excluded) fill a fresh engine's socket without giving up on it; stop() then runs into the deadline and kills it.
        browser::EngineClient client({});
        std::string error;
        if (!client.start(launch, error)) return 1;
        long slowest = 0;
        for (int i = 0; i < fitted && client.running(); ++i) {
            const auto t = Clock::now();
            client.tab_close(1);  // no payload: the same size as stop()'s QUIT
            slowest = std::max(slowest, ms_since(t));
        }
        const bool full_not_given_up = client.running();
        const auto t = Clock::now();
        client.stop();
        const long took = ms_since(t);
        check(took < 2000 && full_not_given_up, "stop() does not wait on a stuck engine with a full socket (" + std::to_string(took) + " ms; socket filled without giving up: " +
                               (full_not_given_up ? "yes" : "no") + ", slowest fill " + std::to_string(slowest) + " ms)");
    }
    std::error_code ec;
    std::filesystem::remove_all(base, ec);
    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
