# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.22)
project(wpebrowser LANGUAGES C CXX)

# The app id: override for a development copy (e.g. -DWPEB_APPID=org.sostk.wpebrowser.dev) so it can be
# installed and launched beside the real app. Everything that names the id at run time uses this same value.
set(WPEB_APPID "org.sostk.wpebrowser" CACHE STRING "webOS application id")
# The title in the launcher: a development copy gets its own, so the two can be told apart on the TV.
# appinfo requiredMemory (MB): with 1700 webOS closes a suspended background app (YouTube) when Wren starts, which frees ~200 MB; 1200 did
# not, 2200 did but slowed the launch by ~8 s (TV, 2026-11-20).
set(WPEB_REQUIRED_MEMORY 1700 CACHE STRING "appinfo requiredMemory")
set(WPEB_TITLE "Wren" CACHE STRING "launcher title")

# The kit: -DWNK_KIT_DIR=..., else $WNK_KIT_DIR, else where it was when tools/new-app.py made this.
# A relative path is taken from this folder (a git submodule at kit/ would be "kit").
if (DEFINED ENV{WNK_KIT_DIR})
    set(_kit_default "$ENV{WNK_KIT_DIR}")
else ()
    set(_kit_default "../third_party/webos-native-app-kit")
endif ()
set(WNK_KIT_DIR "${_kit_default}" CACHE PATH "The webOS native app kit (absolute, or relative to this folder)")
get_filename_component(_kit "${WNK_KIT_DIR}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
if (NOT EXISTS "${_kit}/cmake/WnkApp.cmake")
    message(FATAL_ERROR "The webOS native app kit is not at ${_kit}: set WNK_KIT_DIR")
endif ()
# set(WNK_FFMPEG ON)  # media playback: see the kit's docs/AGENT-APP-GUIDE.md, "Media playback"
include("${_kit}/cmake/WnkApp.cmake")

# The WebKit engine (engine/ built with the glibc 2.28 toolchain, plus its bundled libraries) is staged by
# tools/stage-engine.sh into a folder named "engine"; when it exists the ipk carries it next to the app.
set(WPEB_ENGINE_STAGE "${CMAKE_CURRENT_SOURCE_DIR}/../build/engine-stage/engine" CACHE PATH "Staged engine folder")
if (EXISTS "${WPEB_ENGINE_STAGE}/wpe-engine")
    set(WPEB_ENGINE_EXTRA_DIR "${WPEB_ENGINE_STAGE}")
    message(STATUS "wpebrowser: packaging the engine from ${WPEB_ENGINE_STAGE}")
else ()
    set(WPEB_ENGINE_EXTRA_DIR "")
    message(STATUS "wpebrowser: no staged engine; the package will show 'engine not found'")
endif ()

add_executable(wpebrowser main.cpp chrome.cpp screens.cpp profile.cpp storage.cpp private_loader.cpp engine_client.cpp page_view.cpp keys.cpp media_keys.cpp direct_owner.cpp media_server.cpp lg_media_player.cpp wren_ui.cpp log_export.cpp vt_texture_probe.cpp)
target_include_directories(wpebrowser PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../common")
target_compile_definitions(wpebrowser PRIVATE WPEB_APP_ID_STRING="${WPEB_APPID}")
# printf-style mismatches (a %d given a float crashed the app, 2026-11-20) are build errors for the UI target.
target_compile_options(wpebrowser PRIVATE -Wformat -Werror=format)
target_link_libraries(wpebrowser PRIVATE wnk_app wnk_media_ndl wnk_media_starfish ${CMAKE_DL_LIBS})
if (WNK_WEBOS)
    # LG's buffer-feed media pipeline (lg_media_player.cpp), from the NDK's starfish-media-pipeline header.
    target_include_directories(wpebrowser SYSTEM PRIVATE "${CMAKE_SYSROOT}/usr/include/starfish-media-pipeline")
    target_link_libraries(wpebrowser PRIVATE playerAPIs)
endif ()  # dlopen: libwpeb-owner.so (direct_owner.cpp)
target_include_directories(wpebrowser SYSTEM PRIVATE "${WNK_KIT_DIR}/third_party")  # nlohmann/json.hpp

# Fonts live in fonts/ next to the executable, on the host and in the ipk: the kit's Atkinson Hyperlegible and Wren's
# Manrope (both OFL; Manrope from Google Fonts, static weights 300/400/600/700).
add_custom_command(TARGET wpebrowser POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${WNK_FONT_DIR}" "$<TARGET_FILE_DIR:wpebrowser>/fonts"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/fonts" "$<TARGET_FILE_DIR:wpebrowser>/fonts"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/icons" "$<TARGET_FILE_DIR:wpebrowser>/icons"
        VERBATIM)

wnk_add_webos_package(
        TARGET wpebrowser
        APPID ${WPEB_APPID}
        TITLE "${WPEB_TITLE}"
        VERSION 0.3.3
        VENDOR "sostk"
        ICON "${CMAKE_CURRENT_SOURCE_DIR}/icon.png"
        LARGE_ICON "${CMAKE_CURRENT_SOURCE_DIR}/largeIcon.png"
        ICON_COLOR "#0b1020"
        REQUIRED_MEMORY ${WPEB_REQUIRED_MEMORY}  # MB; measured ~265 web + 75 UI + 56 engine + 35 network (PSS ~200); cobalt 512, LG browser 160
        SUPPORT_GIP  # webOS 3.x launcher: the icon instead of a dark tile (PR #4)
        EXPECTED_NEEDED "${CMAKE_CURRENT_SOURCE_DIR}/expected-needed.txt"
        EXTRA_DIRS "${WNK_FONT_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/fonts" "${CMAKE_CURRENT_SOURCE_DIR}/icons"
        GLIBC_RUNTIMES ${WPEB_ENGINE_EXTRA_DIR}
        # Direct GPU presentation (engine/lib/libWPEBackend-webos.so, libwpeb-owner.so) must use the TV's own
        # Wayland client and wayland-egl, the ones its Mali EGL is built against.
        # Sound goes through the TV's libpulse: its PulseAudio 9.0 server rejects our newer client (tools/stage-engine.sh).
        SYSTEM_LIBS libwayland-client.so.0 libwayland-egl.so libpulse.so.0 libplayerAPIs.so.1)

# Host smoke test: OK on the focused button, then Back twice at the root quits.
wnk_add_app_test(NAME e2e.wpebrowser.starts_and_quits TARGET wpebrowser PREFIX APP_WPEBROWSER
        KEYS "Escape,Escape" PASS "browser ui ready")

if (NOT CMAKE_CROSSCOMPILING)
    add_executable(engine-environment-test tests/engine_environment.cpp engine_client.cpp private_loader.cpp)
    target_compile_features(engine-environment-test PRIVATE cxx_std_17)
    target_include_directories(engine-environment-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/../common")
    target_link_libraries(engine-environment-test PRIVATE wnk_logging)
    add_test(NAME engine.environment COMMAND engine-environment-test)
    # An engine that stops reading: send() gives up after its deadline, the engine is reported lost, stop() does not hang.
    add_executable(engine-stall-test tests/engine_stall.cpp engine_client.cpp private_loader.cpp)
    target_compile_features(engine-stall-test PRIVATE cxx_std_17)
    target_compile_definitions(engine-stall-test PRIVATE WPEB_APP_ID_STRING="test")
    target_include_directories(engine-stall-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/../common")
    target_link_libraries(engine-stall-test PRIVATE wnk_logging)
    add_test(NAME engine.stall COMMAND engine-stall-test)
    add_executable(storage-test tests/storage.cpp storage.cpp)
    target_compile_features(storage-test PRIVATE cxx_std_17)
    target_include_directories(storage-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME browser.storage COMMAND storage-test)
    add_executable(log-export-test tests/log_export.cpp log_export.cpp)
    target_compile_features(log-export-test PRIVATE cxx_std_17)
    target_include_directories(log-export-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME browser.log_export COMMAND log-export-test)
    add_executable(start-filter-test tests/start_filter.cpp)
    target_compile_features(start-filter-test PRIVATE cxx_std_17)
    target_include_directories(start-filter-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME browser.start_filter COMMAND start-filter-test)
    # The media server's stream handling against fake decoders (tests/media_fakes shadows the kit's NDL/Starfish backends and logger).
    find_package(Threads REQUIRED)
    add_executable(media-server-test tests/media_server.cpp media_server.cpp)
    target_compile_features(media-server-test PRIVATE cxx_std_17)
    target_compile_definitions(media-server-test PRIVATE WPEB_APP_ID_STRING="test")
    target_include_directories(media-server-test BEFORE PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests/media_fakes")
    target_include_directories(media-server-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/../common"
        "${_kit}/media/include" "${_kit}/core/include")
    target_link_libraries(media-server-test PRIVATE Threads::Threads)
    add_test(NAME browser.media_server COMMAND media-server-test)
    add_executable(media-keys-test tests/media_keys.cpp media_keys.cpp)
    target_compile_features(media-keys-test PRIVATE cxx_std_17)
    target_include_directories(media-keys-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/../common")
    target_link_libraries(media-keys-test PRIVATE wnk_input)
    add_test(NAME browser.media_keys COMMAND media-keys-test)
    add_executable(profile-zoom-test tests/profile_zoom.cpp profile.cpp)
    target_compile_features(profile-zoom-test PRIVATE cxx_std_17)
    target_include_directories(profile-zoom-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_include_directories(profile-zoom-test SYSTEM PRIVATE "${WNK_KIT_DIR}/third_party")
    target_link_libraries(profile-zoom-test PRIVATE wnk_async wnk_logging)
    add_test(NAME browser.profile_zoom COMMAND profile-zoom-test)
    add_executable(profile-writer-test tests/profile_writer.cpp profile.cpp)
    target_compile_features(profile-writer-test PRIVATE cxx_std_17)
    target_include_directories(profile-writer-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_include_directories(profile-writer-test SYSTEM PRIVATE "${WNK_KIT_DIR}/third_party")
    target_link_libraries(profile-writer-test PRIVATE wnk_async wnk_logging)
    add_test(NAME browser.profile_writer COMMAND profile-writer-test)
    add_executable(engine-env-test tests/engine_env.cpp)
    target_compile_features(engine-env-test PRIVATE cxx_std_17)
    target_include_directories(engine-env-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME browser.engine_env COMMAND engine-env-test)
    add_executable(tab-ids-test tests/tab_ids.cpp)
    target_compile_features(tab-ids-test PRIVATE cxx_std_17)
    target_include_directories(tab-ids-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME browser.tab_ids COMMAND tab-ids-test)
    set(LOADER_FIXTURE_INTERP "/tmp/wpe-loader-test/ld-test.so.1")
    add_executable(loader-fixture tests/loader_fixture.c)
    target_link_options(loader-fixture PRIVATE "-Wl,--dynamic-linker=${LOADER_FIXTURE_INTERP}")
    add_executable(private-loader-test tests/private_loader.cpp private_loader.cpp)
    target_compile_features(private-loader-test PRIVATE cxx_std_17)
    target_include_directories(private-loader-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_definitions(private-loader-test PRIVATE
        LOADER_FIXTURE="$<TARGET_FILE:loader-fixture>" LOADER_FIXTURE_INTERP="${LOADER_FIXTURE_INTERP}")
    add_dependencies(private-loader-test loader-fixture)
    add_test(NAME browser.private_loader COMMAND private-loader-test)
endif ()
