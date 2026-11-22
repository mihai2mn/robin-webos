// SPDX-License-Identifier: GPL-3.0-or-later
#include "log_export.hpp"

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>

namespace browser {

namespace {

bool url_end(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '"' || c == '\'' || c == '<' || c == '>' ||
           c == ')' || c == ']' || c == '}' || c == ',' || c == '`';
}

// The last `max` bytes of a file, starting at a line break; "" with `missing` set if it can't be read.
std::string read_tail(const std::string &path, std::size_t max, bool *missing, bool *cut) {
    *missing = false;
    *cut = false;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        *missing = true;
        return {};
    }
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    std::streamoff from = 0;
    if (size > 0 && static_cast<std::size_t>(size) > max) {
        from = size - static_cast<std::streamoff>(max);
        *cut = true;
    }
    in.seekg(from);
    std::string s(static_cast<std::size_t>(size - from), '\0');
    in.read(&s[0], static_cast<std::streamsize>(s.size()));
    s.resize(static_cast<std::size_t>(in.gcount()));
    if (*cut) {
        const auto nl = s.find('\n');
        if (nl != std::string::npos) s.erase(0, nl + 1);
    }
    return s;
}

}  // namespace

namespace {

// The length of the URL scheme ("https://", ...) that starts at text[i], in any letter case; 0 if none does.
std::size_t scheme_at(const std::string &text, std::size_t i) {
    static const char *const kSchemes[] = {"https://", "http://", "wss://", "ws://"};
    for (const char *s : kSchemes) {
        const std::size_t n = std::strlen(s);
        if (i + n > text.size()) continue;
        std::size_t k = 0;
        while (k < n && std::tolower(static_cast<unsigned char>(text[i + k])) == s[k]) ++k;
        if (k == n) return n;
    }
    return 0;
}

}  // namespace

// One pass over the text, every search bounded by the end of the address it is looking at: a log of tens of megabytes
// with no address (or none with a query) used to take quadratic time.
std::string redact_urls(const std::string &text) {
    std::string out;
    out.reserve(text.size());
    std::size_t copied = 0;  // text[0, copied) is in `out`
    std::size_t i = 0;
    while (i < text.size()) {
        const std::size_t len = scheme_at(text, i);
        if (len == 0) {
            ++i;
            continue;
        }
        const std::size_t at = i;
        std::size_t end = at + len;
        while (end < text.size() && !url_end(text[end])) ++end;
        const std::size_t auth_start = at + len;
        std::size_t auth_end = auth_start;
        while (auth_end < end && text[auth_end] != '/' && text[auth_end] != '?' && text[auth_end] != '#') ++auth_end;
        out.append(text, copied, auth_start - copied);
        std::size_t host_start = auth_start;
        for (std::size_t k = auth_end; k > auth_start; --k) {
            if (text[k - 1] == '@') {
                out += "…@";
                host_start = k;
                break;
            }
        }
        std::size_t cut = host_start;
        while (cut < end && text[cut] != '?' && text[cut] != '#') ++cut;
        if (cut < end) {
            out.append(text, host_start, cut - host_start);
            out += "?…";
        } else {
            out.append(text, host_start, end - host_start);
        }
        copied = end;
        i = end;
    }
    out.append(text, copied, std::string::npos);
    return out;
}

std::string export_logs(const LogExportRequest &req, const std::string &dest_dir, std::string *error) {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_r(&now, &tm);
    char stamp[32];
    std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tm);
    char when[64];
    std::strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S %z", &tm);

    std::ostringstream o;
    o << "Wren logs, saved " << when << "\n";
    for (const std::string &h : req.header) o << h << "\n";
    o << "Web addresses below keep their host and path only (query strings and fragments removed).\n"
         "Please read this file before posting it publicly.\n";

    static const char *const kLogs[][2] = {
        {"engine.log", "the web engine, this run"},
        {"engine.prev.log", "the web engine, the run before (the crash, if the app was reopened)"},
        {"events.log", "the browser's interface"},
    };
    for (const auto &l : kLogs) {
        bool missing = false, cut = false;
        const std::string body = read_tail(req.runtime_dir + "/" + l[0], req.max_bytes_per_log, &missing, &cut);
        o << "\n===== " << l[0] << " (" << l[1] << ")" << (missing ? ": not found" : "")
          << (cut ? ": only the end, it is long" : "") << " =====\n";
        o << redact_urls(body);
        if (!body.empty() && body.back() != '\n') o << "\n";
    }

    const std::string path = dest_dir + "/wren-logs-" + stamp + ".txt";
    std::FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) {
        if (error) *error = std::strerror(errno);
        return {};
    }
    const std::string text = o.str();
    const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    const bool closed = std::fclose(f) == 0;
    if (!ok || !closed) {
        if (error) *error = "the drive is full or was removed";
        std::remove(path.c_str());
        return {};
    }
    return path;
}

}  // namespace browser
