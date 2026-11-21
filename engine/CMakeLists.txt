# SPDX-License-Identifier: GPL-3.0-or-later
# wpe-engine: the WPE WebKit host process of the webOS browser.
# Cross-build with Buildroot's toolchain file (glibc 2.28 toolchain + staging sysroot):
#   cmake -S engine -B build -DCMAKE_TOOLCHAIN_FILE=<br-out>/host/share/buildroot/toolchainfile.cmake
#
# 32-bit softfp engine: tools/stage-engine.sh against the softfp br-out (SHM presentation only: platform.cpp, via
# WPEPlatform's WPE_TYPE_DISPLAY; direct presentation adds the WebKit dlopen()ed libWPEBackend-webos.so).
# aarch64 engine (the TV's bridge-64to32): tools/engine-aarch64.sh against the aarch64 br-out. There is no 32-bit
# library a 64-bit process could load, so direct presentation (WPE_BROWSER_PRESENT=direct) is left out there unless
# WPEB_AARCH64_DIRECT=ON is given with an aarch64 libWPEBackend-webos.so in place; the engine then runs the SHM path.
cmake_minimum_required(VERSION 3.20)
project(wpe_engine LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(WPEB_AARCH64_DIRECT OFF CACHE BOOL "aarch64: build direct presentation (needs an aarch64 libWPEBackend-webos.so)")

find_package(PkgConfig REQUIRED)
pkg_check_modules(WPE REQUIRED IMPORTED_TARGET wpe-webkit-2.0 wpe-platform-2.0 wpe-1.0 glib-2.0 gio-unix-2.0)

add_executable(wpe-engine src/main.cpp src/platform.cpp src/direct.cpp)
target_include_directories(wpe-engine PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../common)
# libWPEBackend-webos.so (engine/webos-backend, built with the webOS NDK) is dlopen()ed in direct mode.
target_link_libraries(wpe-engine PRIVATE PkgConfig::WPE ${CMAKE_DL_LIBS})
target_compile_options(wpe-engine PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
# A 64-bit engine (aarch64, the TV's bridge-64to32) leaves direct presentation out by default: WPEB_NO_DIRECT makes
# direct.cpp empty and main.cpp always use the SHM path. Gated on the target processor, not on CMAKE_SIZEOF_VOID_P:
# 64-bit host builds (the host test harness) must keep direct presentation. The softfp build is a 32-bit ARM target.
if (CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)" AND NOT WPEB_AARCH64_DIRECT)
    message(STATUS "aarch64: SHM presentation only (set WPEB_AARCH64_DIRECT=ON for direct, with an aarch64 backend)")
    target_compile_definitions(wpe-engine PRIVATE WPEB_NO_DIRECT=1)
endif ()
# Libraries are bundled next to the engine inside the app package (engine/lib).
set_target_properties(wpe-engine PROPERTIES
    BUILD_RPATH_USE_ORIGIN ON
    INSTALL_RPATH "$ORIGIN/lib")
install(TARGETS wpe-engine RUNTIME DESTINATION .)

# Hardware video (engine/gst/wpebmediasink.c): GStreamer sinks that hand the encoded stream to the UI process.
pkg_check_modules(GSTBASE IMPORTED_TARGET gstreamer-1.0 gstreamer-base-1.0 gstreamer-audio-1.0 gstreamer-video-1.0
                  gstreamer-pbutils-1.0 gio-2.0)
if (GSTBASE_FOUND)
    add_library(gstwpebmedia MODULE gst/wpebmediasink.c)
    target_include_directories(gstwpebmedia PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../common)
    target_link_libraries(gstwpebmedia PRIVATE PkgConfig::GSTBASE ${CMAKE_DL_LIBS})
    target_compile_options(gstwpebmedia PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
endif ()
