// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage.hpp"
#include <cerrno>
#include <dirent.h>
#include <ftw.h>
#include <sys/stat.h>
#include <unistd.h>

namespace browser {

bool make_dir(const std::string &path) {
    for (size_t i = 1; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == '/') {
            const std::string part = path.substr(0, i);
            if (::mkdir(part.c_str(), 0700) != 0 && errno != EEXIST) return false;
        }
    }
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

namespace {
thread_local std::string *g_bad = nullptr;

int check_entry(const char *path, const struct stat *st, int type, struct FTW *) {
    bool ok = true;
    if (type == FTW_D)
        ok = ::access(path, R_OK | W_OK | X_OK) == 0;
    else if (type == FTW_DNR || type == FTW_NS)
        ok = false;
    else if (type == FTW_F && S_ISREG(st->st_mode))
        ok = ::access(path, R_OK | W_OK) == 0;
    if (!ok && g_bad) *g_bad = path;
    return ok ? 0 : 1;
}
} // namespace

bool usable_tree(const std::string &path, std::string *bad) {
    struct stat st {};
    if (::lstat(path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
        if (bad) *bad = path;
        return false;
    }
    g_bad = bad;
    const int rc = ::nftw(path.c_str(), check_entry, 16, FTW_PHYS);
    g_bad = nullptr;
    return rc == 0;
}

namespace {
// One level: the folder itself must be usable, every file in it readable and writable, every folder in it usable
// (not entered). `bad` names the first that is not.
bool usable_shallow(const std::string &path, std::string *bad) {
    struct stat st {};
    if (::lstat(path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) || ::access(path.c_str(), R_OK | W_OK | X_OK) != 0) {
        if (bad) *bad = path;
        return false;
    }
    DIR *d = ::opendir(path.c_str());
    if (d == nullptr) {
        if (bad) *bad = path;
        return false;
    }
    bool ok = true;
    while (const dirent *e = ::readdir(d)) {
        const std::string name = e->d_name;
        if (name == "." || name == "..") continue;
        const std::string p = path + "/" + name;
        if (::lstat(p.c_str(), &st) != 0) continue;  // gone meanwhile
        const bool entry_ok = S_ISDIR(st.st_mode)   ? ::access(p.c_str(), R_OK | W_OK | X_OK) == 0
                              : S_ISREG(st.st_mode) ? ::access(p.c_str(), R_OK | W_OK) == 0
                                                    : true;
        if (!entry_ok) {
            if (bad) *bad = p;
            ok = false;
            break;
        }
    }
    ::closedir(d);
    return ok;
}
}  // namespace

bool usable_profile_tree(const std::string &path, std::string *bad) {
    if (!usable_shallow(path, bad)) return false;
    struct stat st {};
    const std::string webkit = path + "/webkit";  // cookies.sqlite and the like; its cache below is the engine's to repair
    if (::lstat(webkit.c_str(), &st) == 0 && S_ISDIR(st.st_mode) && !usable_shallow(webkit, bad)) return false;
    return true;
}

std::string own_usable_profile_dir(const std::string &preferred, std::string *why) {
    std::string bad;
    if (make_dir(preferred) && usable_profile_tree(preferred, &bad)) return preferred;
    if (why) *why = "not usable: " + (bad.empty() ? preferred : bad);
    const std::string alt = preferred + "-" + std::to_string(::getuid());
    if (make_dir(alt) && usable_profile_tree(alt, &bad)) return alt;
    if (why) *why += "; not usable: " + (bad.empty() ? alt : bad);
    return {};
}

std::string own_usable_dir(const std::string &preferred, std::string *why) {
    std::string bad;
    if (make_dir(preferred) && usable_tree(preferred, &bad)) return preferred;
    if (why) *why = "not usable: " + (bad.empty() ? preferred : bad);
    const std::string alt = preferred + "-" + std::to_string(::getuid());
    if (make_dir(alt) && usable_tree(alt, &bad)) return alt;
    if (why) *why += "; not usable: " + (bad.empty() ? alt : bad);
    return {};
}

} // namespace browser
