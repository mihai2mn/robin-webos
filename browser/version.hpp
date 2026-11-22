// SPDX-License-Identifier: GPL-3.0-or-later
// The app version: defined once, in the repository's VERSION file (browser/CMakeLists.txt reads it and passes
// WPEB_VERSION_STRING; tools/package-release.sh reads the same file). The fallback only serves builds outside CMake.
#pragma once
#ifndef WPEB_VERSION_STRING
#define WPEB_VERSION_STRING "0.0.0-unset"
#endif
namespace browser {
inline constexpr const char *kAppVersion = WPEB_VERSION_STRING;
inline constexpr const char *kBasedOn = "Wren 0.3.3 by sostk";                 // what this app is based on
inline constexpr const char *kUpstreamUrl = "github.com/sostk/webos-wpe-browser";
inline constexpr const char *kIssuesUrl = "github.com/mihai2mn/robin-webos/issues";
}  // namespace browser
