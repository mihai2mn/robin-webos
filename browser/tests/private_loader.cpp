// SPDX-License-Identifier: GPL-3.0-or-later
// The engine's executables can name a loader in the app's runtime folder; the UI links it to the engine's own glibc
// (on TVs older than glibc 2.28, or when asked) or to the TV's loader.
#include "private_loader.hpp"
#include <climits>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;
using browser::GlibcPolicy;
using browser::LoaderChoice;
static int failures = 0;
static void report(bool ok, const std::string &what) {
    std::cout << (ok ? "ok   " : "FAIL ") << what << '\n';
    if (!ok) ++failures;
}
// The message is built after the condition ran (it shows the call's err / why).
#define check(cond, what)        \
    do {                         \
        const bool ok_ = (cond); \
        report(ok_, (what));     \
    } while (0)

static void fake_loader(const std::string &path) {
    fs::create_directories(fs::path(path).parent_path());
    std::ofstream(path) << "x";
    fs::permissions(path, fs::perms::owner_all);
}

int main(int, char **argv) {
    const std::string fixture = LOADER_FIXTURE;  // linked with --dynamic-linker=LOADER_FIXTURE_INTERP
    const std::string runtime = fs::path(LOADER_FIXTURE_INTERP).parent_path();
    const std::string name = fs::path(LOADER_FIXTURE_INTERP).filename();
    fs::remove_all(runtime);
    fs::create_directories(runtime);
    char tmpl[] = "/tmp/wpe-loader-engine-XXXXXX";
    const std::string engine = ::mkdtemp(tmpl);
    const std::string lib = engine + "/lib", syslib = engine + "/system-lib";
    fs::create_directories(lib);
    fs::create_directories(syslib);

    check(browser::compare_versions("2.24", "2.28") < 0 && browser::compare_versions("2.28", "2.28") == 0 &&
              browser::compare_versions("2.31", "2.28") > 0 && browser::compare_versions("2.9", "2.28") < 0,
          "version comparison");
    check(browser::elf_interpreter(fixture) == LOADER_FIXTURE_INTERP, "reads PT_INTERP: " + browser::elf_interpreter(fixture));
    const std::string self = browser::elf_interpreter(argv[0]);
    check(!self.empty() && self.rfind("/tmp/", 0) != 0, "system loader of the test itself: " + self);
    check(browser::elf_interpreter(lib).empty(), "a folder has no loader");

    LoaderChoice c;
    std::string err;
    auto prepare = [&](GlibcPolicy p, const std::string &v) {
        err.clear();
        return browser::prepare_private_loader(fixture, lib, runtime, p, v, syslib, c, err);
    };
    check(browser::prepare_private_loader(argv[0], lib, runtime, GlibcPolicy::Auto, "2.28", syslib, c, err) && !c.linked,
          "system loader needs nothing");

    check(!prepare(GlibcPolicy::Auto, "2.28") && c.linked && err.find("missing bundled loader") != std::string::npos,
          "neither loader: reported: " + err);

    // Only the TV's loader (an engine without glibc of its own).
    const std::string system = syslib + "/" + name;
    fake_loader(system);
    check(prepare(GlibcPolicy::Bundled, "2.24") && !c.bundled_glibc && fs::read_symlink(LOADER_FIXTURE_INTERP) == system,
          "no bundled glibc: the TV's loader, whatever is asked (" + c.why + ")");

    // Both: bundled in engine/lib/glibc.
    const std::string bundled = lib + "/glibc/" + name;
    fake_loader(bundled);
    check(prepare(GlibcPolicy::Auto, "2.28") && !c.bundled_glibc && fs::read_symlink(LOADER_FIXTURE_INTERP) == system,
          "auto on glibc 2.28: the TV's (" + c.why + ")");
    check(prepare(GlibcPolicy::Auto, "2.31") && !c.bundled_glibc, "auto on glibc 2.31: the TV's");
    check(prepare(GlibcPolicy::Auto, "2.24") && c.bundled_glibc && c.glibc_dir == lib + "/glibc" &&
              fs::read_symlink(LOADER_FIXTURE_INTERP) == bundled,
          "auto on glibc 2.24: bundled (" + c.why + ")");
    check(prepare(GlibcPolicy::Bundled, "2.28") && c.bundled_glibc, "bundled forced");
    check(prepare(GlibcPolicy::System, "2.24") && !c.bundled_glibc && fs::read_symlink(LOADER_FIXTURE_INTERP) == system,
          "system forced");
    check(prepare(GlibcPolicy::System, "2.24"), "second call keeps the link");

    fs::remove(LOADER_FIXTURE_INTERP);
    fs::create_symlink("/bin/true", LOADER_FIXTURE_INTERP);
    check(prepare(GlibcPolicy::Auto, "2.24") && fs::read_symlink(LOADER_FIXTURE_INTERP) == bundled, "a wrong link is replaced");

    // No TV loader of that name (hard-float engine): bundled, from engine/lib.
    fs::remove(system);
    fs::remove_all(lib + "/glibc");
    const std::string flat = lib + "/" + name;
    fake_loader(flat);
    check(prepare(GlibcPolicy::System, "2.28") && c.bundled_glibc && c.glibc_dir == lib &&
              fs::read_symlink(LOADER_FIXTURE_INTERP) == flat,
          "no TV loader: bundled from engine/lib (" + c.why + ")");

    // A wrong link in a folder that is not the app's runtime folder cannot be fixed from here.
    fs::remove(LOADER_FIXTURE_INTERP);
    fs::create_symlink("/bin/true", LOADER_FIXTURE_INTERP);
    err.clear();
    check(!browser::prepare_private_loader(fixture, lib, "/somewhere/else/" + fs::path(runtime).filename().string(),
                                           GlibcPolicy::Auto, "2.28", syslib, c, err) &&
              err.find("runtime folder") != std::string::npos,
          "runtime folder mismatch is reported: " + err);

    // The app's "<folder>-<uid>" runtime folder (the kit refused /tmp/<folder>, another user's): the engine's loader is still in
    // /tmp/<folder>, a folder this process may not change. A link that is already right is used; a wrong one is an error, not
    // "the system's loader" with nothing checked.
    {
        const std::string uid_runtime = runtime + "-" + std::to_string(::getuid());
        fs::create_directories(uid_runtime);
        fs::remove(LOADER_FIXTURE_INTERP);
        fs::create_symlink(flat, LOADER_FIXTURE_INTERP);
        err.clear();
        check(browser::prepare_private_loader(fixture, lib, uid_runtime, GlibcPolicy::Auto, "2.28", syslib, c, err) && c.linked &&
                  fs::read_symlink(LOADER_FIXTURE_INTERP) == flat,
              "<folder>-<uid> runtime folder, link already right: used (" + err + ")");
        fs::remove(LOADER_FIXTURE_INTERP);
        fs::create_symlink("/bin/true", LOADER_FIXTURE_INTERP);
        err.clear();
        check(!browser::prepare_private_loader(fixture, lib, uid_runtime, GlibcPolicy::Auto, "2.28", syslib, c, err) &&
                  err.find("runtime folder") != std::string::npos && fs::read_symlink(LOADER_FIXTURE_INTERP) == "/bin/true",
              "<folder>-<uid> runtime folder, wrong link: reported and left alone: " + err);
        const std::string other = runtime + "-abc";
        fs::create_directories(other);
        check(browser::prepare_private_loader(fixture, lib, other, GlibcPolicy::Auto, "2.28", syslib, c, err) && !c.linked,
              "<folder>-abc is not the app's folder: system loader");
        fs::remove_all(uid_runtime);
        fs::remove_all(other);
    }

    fs::remove_all(runtime);
    fs::remove_all(engine);
    return failures ? 1 : 0;
}
