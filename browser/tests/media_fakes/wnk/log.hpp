// SPDX-License-Identifier: GPL-3.0-or-later
// Host test stand-in for the kit's logger (tests/media_server.cpp): lines go to a list the test can search.
#pragma once

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

namespace fake_log {
inline std::mutex &mu() { static std::mutex m; return m; }
inline std::vector<std::string> &lines() { static std::vector<std::string> l; return l; }
inline void logf(const char *level, const char *cat, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
inline void logf(const char *level, const char *cat, const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> l(mu());
    lines().push_back(buf);
    if (std::getenv("MEDIA_TEST_VERBOSE")) std::fprintf(stderr, "  [%s %s] %s\n", level, cat, buf);
}
inline bool contains(const std::string &needle) {
    std::lock_guard<std::mutex> l(mu());
    for (const auto &s : lines()) if (s.find(needle) != std::string::npos) return true;
    return false;
}
inline void clear() { std::lock_guard<std::mutex> l(mu()); lines().clear(); }
}  // namespace fake_log

#define WNK_LOGT(category, ...) ::fake_log::logf("T", category, __VA_ARGS__)
#define WNK_LOGD(category, ...) ::fake_log::logf("D", category, __VA_ARGS__)
#define WNK_LOGI(category, ...) ::fake_log::logf("I", category, __VA_ARGS__)
#define WNK_LOGW(category, ...) ::fake_log::logf("W", category, __VA_ARGS__)
#define WNK_LOGE(category, ...) ::fake_log::logf("E", category, __VA_ARGS__)
