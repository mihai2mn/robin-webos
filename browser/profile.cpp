// SPDX-License-Identifier: GPL-3.0-or-later
#include "profile.hpp"

#include "wnk/async/async.hpp"
#include "wnk/log.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#include <sys/stat.h>
#include <unistd.h>

namespace browser {
namespace {

constexpr const char *kCat = "profile";
constexpr size_t kMaxHistory = 5000;
constexpr size_t kMaxBookmarks = 1000;
constexpr size_t kMaxSiteZooms = 500;
constexpr double kFlushIntervalMs = 2000;

int64_t now_s() { return static_cast<int64_t>(std::time(nullptr)); }

std::string read_file(const std::string &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Writes `data` next to `path` and renames it over: a crash never leaves a half-written profile.
bool write_atomic(const std::string &path, const std::string &data) {
    const std::string tmp = path + ".tmp";
    FILE *f = std::fopen(tmp.c_str(), "wb");
    if (f == nullptr) return false;
    const bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size() && std::fflush(f) == 0 &&
                    ::fsync(fileno(f)) == 0;
    std::fclose(f);
    if (!ok || std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::remove(tmp.c_str());
        return false;
    }
    return true;
}

std::string str(const nlohmann::json &j, const char *key) {
    auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

int64_t num(const nlohmann::json &j, const char *key) {
    auto it = j.find(key);
    return it != j.end() && it->is_number_integer() ? it->get<int64_t>() : 0;
}

bool is_http_url(const std::string &url) {
    const size_t sep = url.find("://");
    if (sep == std::string::npos || (sep != 4 && sep != 5)) return false;
    std::string scheme = url.substr(0, sep);
    for (char &c : scheme) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return scheme == "http" || scheme == "https";
}

std::string icon_host(const std::string &url);  // defined with the icon lookups below

}  // namespace

const std::vector<SearchEngine> kSearchEngines = {
    {"DuckDuckGo", "https://duckduckgo.com/?q="},
    {"Google", "https://www.google.com/search?q="},
    {"Bing", "https://www.bing.com/search?q="},
    {"Startpage", "https://www.startpage.com/do/search?q="},
    {"Ecosia", "https://www.ecosia.org/search?q="},
};

const std::vector<int> kZoomLevels = {1000, 1100, 1250, 1500, 1750, 2000};

void Profile::load(const std::string &dir) {
    dir_ = dir;
    ::mkdir(dir_.c_str(), 0700);

    auto parse = [](const std::string &text) { return nlohmann::json::parse(text, nullptr, false); };
    if (auto j = parse(read_file(path(kSettings))); j.is_object()) {
        settings.show_bookmarks_bar = j.value("show_bookmarks_bar", settings.show_bookmarks_bar);
        settings.search_engine = std::clamp(j.value("search_engine", 0), 0, static_cast<int>(kSearchEngines.size()) - 1);
        settings.zoom = std::clamp(j.value("zoom", 0), 0, static_cast<int>(kZoomLevels.size()) - 1);
        settings.restore_session = j.value("restore_session", false);
        settings.sharp_text = j.value("sharp_text", settings.sharp_text);
    }
    if (auto j = parse(read_file(path(kBookmarks))); j.is_array()) {
        for (const auto &b : j) {
            if (!b.is_object() || str(b, "url").empty()) continue;
            bookmarks_.push_back({str(b, "title"), str(b, "url"), num(b, "added")});
        }
    }
    if (auto j = parse(read_file(path(kHistory))); j.is_array()) {
        for (const auto &h : j) {
            if (!h.is_object() || str(h, "url").empty()) continue;
            history_.push_back({str(h, "title"), str(h, "url"), num(h, "time")});
        }
        std::stable_sort(history_.begin(), history_.end(), [](const HistoryEntry &a, const HistoryEntry &b) { return a.time > b.time; });
    }
    if (auto j = parse(read_file(path(kSession))); j.is_object() && j["tabs"].is_array()) {
        for (const auto &t : j["tabs"]) {
            if (t.is_object()) session_.push_back({str(t, "title"), str(t, "url")});
        }
        session_active_ = static_cast<size_t>(std::max<int64_t>(0, num(j, "active")));
    }
    if (auto j = parse(read_file(path(kIcons))); j.is_object()) {
        for (const auto &[host, url] : j.items())
            if (url.is_string() && is_http_url(url.get<std::string>())) icons_[host] = url.get<std::string>();
    }
    if (auto j = parse(read_file(path(kZooms))); j.is_object()) {
        for (const auto &[raw_host, val] : j.items()) {
            if (!val.is_number_integer()) continue;
            const int permille = val.get<int>();
            if (permille < 250 || permille > 5000) continue;
            std::string host = site_host("https://" + raw_host);
            if (host.empty()) continue;
            site_zooms_[host] = kZoomLevels[static_cast<size_t>(zoom_index_for_permille(permille))];
        }
    }
    WNK_LOGI(kCat, "profile %s: %zu bookmarks, %zu history entries, %zu session tabs, %zu site zooms", dir_.c_str(),
             bookmarks_.size(), history_.size(), session_.size(), site_zooms_.size());
}

std::string Profile::path(unsigned part) const {
    switch (part) {
    case kSettings: return dir_ + "/settings.json";
    case kBookmarks: return dir_ + "/bookmarks.json";
    case kHistory: return dir_ + "/history.json";
    case kIcons: return dir_ + "/icons.json";
    case kZooms: return dir_ + "/zooms.json";
    default: return dir_ + "/session.json";
    }
}

std::string Profile::serialize(unsigned part) const {
    nlohmann::json j;
    switch (part) {
    case kSettings:
        j = {{"show_bookmarks_bar", settings.show_bookmarks_bar},
             {"search_engine", settings.search_engine},
             {"zoom", settings.zoom},
             {"restore_session", settings.restore_session},
             {"sharp_text", settings.sharp_text}};
        break;
    case kBookmarks:
        j = nlohmann::json::array();
        for (const auto &b : bookmarks_) j.push_back({{"title", b.title}, {"url", b.url}, {"added", b.added}});
        break;
    case kHistory:
        j = nlohmann::json::array();
        for (const auto &h : history_) j.push_back({{"title", h.title}, {"url", h.url}, {"time", h.time}});
        break;
    case kIcons:
        j = nlohmann::json::object();
        for (const auto &[host, url] : icons_) j[host] = url;
        break;
    case kZooms:
        j = nlohmann::json::object();
        for (const auto &[host, permille] : site_zooms_) j[host] = permille;
        break;
    default:
        j = {{"active", session_active_}, {"tabs", nlohmann::json::array()}};
        for (const auto &t : session_) j["tabs"].push_back({{"title", t.title}, {"url", t.url}});
        break;
    }
    // Invalid UTF-8 from a page title must not throw: replace it.
    return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

bool Profile::is_bookmarked(const std::string &url) const {
    return std::any_of(bookmarks_.begin(), bookmarks_.end(), [&](const Bookmark &b) { return b.url == url; });
}

void Profile::add_bookmark(const std::string &title, const std::string &url) {
    if (url.empty() || is_bookmarked(url) || bookmarks_.size() >= kMaxBookmarks) return;
    bookmarks_.push_back({title.empty() ? url : title, url, now_s()});
    dirty_ |= kBookmarks;
}

void Profile::remove_bookmark(const std::string &url) {
    const auto before = bookmarks_.size();
    bookmarks_.erase(std::remove_if(bookmarks_.begin(), bookmarks_.end(), [&](const Bookmark &b) { return b.url == url; }),
                     bookmarks_.end());
    if (bookmarks_.size() != before) dirty_ |= kBookmarks;
}

void Profile::move_bookmark(size_t from, size_t to) {
    if (from >= bookmarks_.size() || to >= bookmarks_.size() || from == to) return;
    Bookmark b = bookmarks_[from];
    bookmarks_.erase(bookmarks_.begin() + static_cast<long>(from));
    bookmarks_.insert(bookmarks_.begin() + static_cast<long>(to), b);
    dirty_ |= kBookmarks;
}

void Profile::add_visit(const std::string &title, const std::string &url) {
    if (url.empty() || url.rfind("about:", 0) == 0) return;
    if (!history_.empty() && history_.front().url == url) {
        history_.front().time = now_s();
        if (!title.empty()) history_.front().title = title;
    } else {
        history_.insert(history_.begin(), {title, url, now_s()});
        if (history_.size() > kMaxHistory) history_.resize(kMaxHistory);
    }
    dirty_ |= kHistory;
}

void Profile::set_history_title(const std::string &url, const std::string &title) {
    // Titles often arrive after the load finished: update the newest visit of that URL.
    for (auto &h : history_) {
        if (h.url == url) {
            if (h.title != title && !title.empty()) {
                h.title = title;
                dirty_ |= kHistory;
            }
            return;
        }
    }
}

void Profile::remove_history(const std::string &url, int64_t time) {
    const auto before = history_.size();
    history_.erase(std::remove_if(history_.begin(), history_.end(),
                                  [&](const HistoryEntry &h) { return h.url == url && h.time == time; }),
                   history_.end());
    if (history_.size() != before) dirty_ |= kHistory;
}

size_t Profile::clear_history(uint32_t seconds) {
    const auto before = history_.size();
    if (seconds == 0) {
        history_.clear();
        // The remembered zooms and icons name sites too: clearing all history forgets them (a time range cannot: they carry no time).
        if (!site_zooms_.empty()) { site_zooms_.clear(); dirty_ |= kZooms; }
        // Bookmarked sites keep theirs: the bookmark tiles draw them, and they are not history.
        for (auto it = icons_.begin(); it != icons_.end();) {
            const bool marked = std::any_of(bookmarks_.begin(), bookmarks_.end(), [&](const Bookmark &b) { return icon_host(b.url) == it->first; });
            if (marked) ++it; else { it = icons_.erase(it); dirty_ |= kIcons; }
        }
    } else {
        const int64_t since = now_s() - seconds;
        history_.erase(std::remove_if(history_.begin(), history_.end(), [&](const HistoryEntry &h) { return h.time >= since; }),
                       history_.end());
    }
    dirty_ |= kHistory;
    return before - history_.size();
}

void Profile::set_session(std::vector<SessionTab> tabs, size_t active) {
    session_ = std::move(tabs);
    session_active_ = active;
    dirty_ |= kSession;
}

namespace {
std::string icon_host(const std::string &url) {
    size_t start = url.find("://");
    start = start == std::string::npos ? 0 : start + 3;
    const size_t end = url.find_first_of("/?#:", start);
    std::string host = url.substr(start, end == std::string::npos ? std::string::npos : end - start);
    if (host.rfind("www.", 0) == 0) host = host.substr(4);
    return host;
}
}  // namespace

const std::string &Profile::icon_for(const std::string &url) const {
    static const std::string none;
    const auto it = icons_.find(icon_host(url));
    return it == icons_.end() ? none : it->second;
}

void Profile::set_icon(const std::string &page_url, const std::string &icon_url) {
    const std::string host = icon_host(page_url);
    if (host.empty() || !is_http_url(page_url)) return;
    auto it = icons_.find(host);
    if (icon_url.empty() || !is_http_url(icon_url)) return;  // only http(s) icons (prevents file:// reads)
    if (it != icons_.end() && it->second == icon_url) return;
    if (icons_.size() >= 500 && it == icons_.end()) icons_.erase(icons_.begin());
    icons_[host] = icon_url;
    dirty_ |= kIcons;
}

std::string site_host(const std::string &url) {
    const size_t sep = url.find("://");
    if (sep == std::string::npos) return {};
    std::string scheme = url.substr(0, sep);
    for (char &c : scheme) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (scheme != "http" && scheme != "https") return {};
    const size_t auth_start = sep + 3;
    const size_t auth_end = url.find_first_of("/?#", auth_start);
    std::string auth = url.substr(auth_start, auth_end == std::string::npos ? std::string::npos : auth_end - auth_start);
    if (const size_t at = auth.rfind('@'); at != std::string::npos) auth.erase(0, at + 1);
    if (auth.empty()) return {};
    std::string host;
    if (auth.front() == '[') {
        const size_t close = auth.find(']');
        if (close == std::string::npos || close <= 1) return {};
        host = auth.substr(0, close + 1);
    } else {
        const size_t colon = auth.find(':');
        host = colon == std::string::npos ? auth : auth.substr(0, colon);
    }
    for (char &c : host) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (host.rfind("www.", 0) == 0 && host.size() > 4) host.erase(0, 4);
    return host;
}

int zoom_index_for_permille(int permille) {
    int best = 0;
    int best_diff = std::abs(kZoomLevels[0] - permille);
    for (size_t i = 1; i < kZoomLevels.size(); ++i) {
        const int diff = std::abs(kZoomLevels[i] - permille);
        if (diff < best_diff) {
            best_diff = diff;
            best = static_cast<int>(i);
        }
    }
    return best;
}

bool Profile::has_site_zoom(const std::string &url) const {
    const std::string host = site_host(url);
    return !host.empty() && site_zooms_.find(host) != site_zooms_.end();
}

int Profile::zoom_for(const std::string &url) const {
    const std::string host = site_host(url);
    if (!host.empty()) {
        if (const auto it = site_zooms_.find(host); it != site_zooms_.end())
            return zoom_index_for_permille(it->second);
    }
    return std::clamp(settings.zoom, 0, static_cast<int>(kZoomLevels.size()) - 1);
}

int Profile::zoom_permille_for(const std::string &url) const {
    return kZoomLevels[static_cast<size_t>(zoom_for(url))];
}

void Profile::set_site_zoom(const std::string &url, int zoom_index) {
    const std::string host = site_host(url);
    if (host.empty()) return;
    const int idx = std::clamp(zoom_index, 0, static_cast<int>(kZoomLevels.size()) - 1);
    const int def_idx = std::clamp(settings.zoom, 0, static_cast<int>(kZoomLevels.size()) - 1);
    if (idx == def_idx) {
        if (site_zooms_.erase(host) > 0) dirty_ |= kZooms;
        return;
    }
    const int permille = kZoomLevels[static_cast<size_t>(idx)];
    auto it = site_zooms_.find(host);
    if (it != site_zooms_.end() && it->second == permille) return;
    if (site_zooms_.size() >= kMaxSiteZooms && it == site_zooms_.end()) site_zooms_.erase(site_zooms_.begin());
    site_zooms_[host] = permille;
    dirty_ |= kZooms;
}

void Profile::clear_site_zoom(const std::string &url) {
    const std::string host = site_host(url);
    if (!host.empty() && site_zooms_.erase(host) > 0) dirty_ |= kZooms;
}

void Profile::flush(wnk::async::WorkerPool &pool, double now_ms) {
    if (dirty_ == 0 || dir_.empty() || now_ms - last_flush_ms_ < kFlushIntervalMs) return;
    last_flush_ms_ = now_ms;
    // Serialise here (main thread owns the data), write on a worker.
    std::vector<std::pair<std::string, std::string>> files;
    for (unsigned part : {kSettings, kBookmarks, kHistory, kSession, kIcons, kZooms}) {
        if (dirty_ & part) files.emplace_back(path(part), serialize(part));
    }
    dirty_ = 0;
    pool.submit([files = std::move(files)](const wnk::async::CancellationToken &) {
        for (const auto &[p, data] : files) {
            if (!write_atomic(p, data)) WNK_LOGW(kCat, "could not save %s: %s", p.c_str(), std::strerror(errno));
        }
    });
}

void Profile::flush_now() {
    if (dir_.empty()) return;
    for (unsigned part : {kSettings, kBookmarks, kHistory, kSession, kIcons, kZooms}) {
        if ((dirty_ & part) && !write_atomic(path(part), serialize(part)))
            WNK_LOGW(kCat, "could not save %s", path(part).c_str());
    }
    dirty_ = 0;
}

std::string day_label(int64_t t) {
    const time_t tt = static_cast<time_t>(t), now = std::time(nullptr);
    tm a{}, b{};
    localtime_r(&tt, &a);
    localtime_r(&now, &b);
    if (a.tm_year == b.tm_year && a.tm_yday == b.tm_yday) return "Today";
    const time_t yesterday = now - 86400;
    tm y{};
    localtime_r(&yesterday, &y);
    if (a.tm_year == y.tm_year && a.tm_yday == y.tm_yday) return "Yesterday";
    char buf[64];
    std::strftime(buf, sizeof(buf), "%A %d %B %Y", &a);
    return buf;
}

std::string time_label(int64_t t) {
    const time_t tt = static_cast<time_t>(t);
    tm a{};
    localtime_r(&tt, &a);
    char buf[16];
    std::strftime(buf, sizeof(buf), "%H:%M", &a);
    return buf;
}

}  // namespace browser
