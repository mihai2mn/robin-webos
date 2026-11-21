// SPDX-License-Identifier: GPL-3.0-or-later
// Host-side unit tests for engine/webos-backend/video_crop.h.
#include "video_crop.h"

#include <assert.h>
#include <stdio.h>

static void test_os_info_parsing(void)
{
    assert(wpeb_webos_major_from_os_info(NULL) == 0);
    assert(wpeb_webos_major_from_os_info("") == 0);
    assert(wpeb_webos_major_from_os_info("{}") == 0);
    assert(wpeb_webos_major_from_os_info("{\"webos_release\":\"5.3.0\"}") == 5);
    assert(wpeb_webos_major_from_os_info("{\"webos_release\" : \"4.4.2\", \"core_os_release\":\"4.4.2-22\"}") == 4);
    assert(wpeb_webos_major_from_os_info("{\"core_os_release\":\"6.5.3\",\"webos_release\":\"6.5.3\"}") == 6);
    assert(wpeb_webos_major_from_os_info("{\"webos_release\":\"10.2.0\"}") == 10);
    assert(wpeb_webos_major_from_os_info("{\"webos_release\":\"x\"}") == 0);
}

static void test_mode_resolution(void)
{
    // Auto default: crop on webOS 5+ and unknown (0), squash on webOS 1..4.
    assert(wpeb_crop_mode_resolve(NULL, NULL, NULL, 5) == WPEB_CROP_MODE_CROP);
    assert(wpeb_crop_mode_resolve(NULL, NULL, NULL, 6) == WPEB_CROP_MODE_CROP);
    assert(wpeb_crop_mode_resolve(NULL, NULL, NULL, 0) == WPEB_CROP_MODE_CROP);
    assert(wpeb_crop_mode_resolve(NULL, NULL, NULL, 4) == WPEB_CROP_MODE_SQUASH);
    assert(wpeb_crop_mode_resolve("auto", NULL, NULL, 4) == WPEB_CROP_MODE_SQUASH);
    assert(wpeb_crop_mode_resolve("auto", NULL, NULL, 5) == WPEB_CROP_MODE_CROP);

    // Explicit WPE_BROWSER_DEBUG_VIDEO_CROP overrides webOS version and legacy env vars.
    assert(wpeb_crop_mode_resolve("squash", NULL, NULL, 5) == WPEB_CROP_MODE_SQUASH);
    assert(wpeb_crop_mode_resolve("0", NULL, NULL, 5) == WPEB_CROP_MODE_SQUASH);
    assert(wpeb_crop_mode_resolve("off", NULL, NULL, 5) == WPEB_CROP_MODE_SQUASH);
    assert(wpeb_crop_mode_resolve("park", NULL, NULL, 5) == WPEB_CROP_MODE_PARK);
    assert(wpeb_crop_mode_resolve("crop", "1", "squash", 4) == WPEB_CROP_MODE_CROP);

    // Legacy WPEB_NO_CROP and WPEB_CROP_FALLBACK still work when primary switch is unset.
    assert(wpeb_crop_mode_resolve(NULL, "1", NULL, 5) == WPEB_CROP_MODE_SQUASH);
    assert(wpeb_crop_mode_resolve(NULL, "0", NULL, 5) == WPEB_CROP_MODE_CROP);
    assert(wpeb_crop_mode_resolve(NULL, NULL, "squash", 5) == WPEB_CROP_MODE_SQUASH);
    assert(wpeb_crop_mode_resolve(NULL, NULL, "park", 5) == WPEB_CROP_MODE_PARK);
}

static void test_fully_visible_and_under_toolbar(void)
{
    // 1920x1080 screen, 88 px docked toolbar -> page_h = 992, surface_h = 1080.
    // Fully visible below toolbar.
    struct wpeb_video_plan p = wpeb_video_plan_compute(1920, 1080, 200, 100, 960, 540, 1920, 992, 1080, WPEB_CROP_MODE_CROP);
    assert(p.action == WPEB_VIDEO_ACTION_WINDOW);
    assert(!p.clipped);
    assert(p.dx == 200 && p.dy == 188 && p.dw == 960 && p.dh == 540);
    assert(p.sx == 0 && p.sy == 0 && p.sw == 1920 && p.sh == 1080);

    // Scrolled 50 px under the 88 px docked toolbar (y = -50 -> dy = +38 >= 0):
    // still inside [0, 1080], so extends cleanly behind the opaque UI toolbar without clipping.
    p = wpeb_video_plan_compute(1920, 1080, 200, -50, 960, 540, 1920, 992, 1080, WPEB_CROP_MODE_CROP);
    assert(p.action == WPEB_VIDEO_ACTION_WINDOW);
    assert(!p.clipped);
    assert(p.dx == 200 && p.dy == 38 && p.dw == 960 && p.dh == 540);
}

static void test_top_crop_squash_park(void)
{
    // Scrolled past the top of the screen: y = -223 on page_h=992, surface_h=1080 -> dy = -135, dh = 540.
    // Exact 1/4 of the 540 px height is above y=0 -> cy = 270, ch = 810, dst = (200, 0, 960, 405).
    struct wpeb_video_plan p = wpeb_video_plan_compute(1920, 1080, 200, -223, 960, 540, 1920, 992, 1080, WPEB_CROP_MODE_CROP);
    assert(p.action == WPEB_VIDEO_ACTION_CROP);
    assert(p.clipped);
    assert(p.ow == 1920 && p.oh == 1080);
    assert(p.sx == 0 && p.sy == 270 && p.sw == 1920 && p.sh == 810);
    assert(p.dx == 200 && p.dy == 0 && p.dw == 960 && p.dh == 405);

    // Same input in squash fallback mode: destination clipped, source stays full picture.
    p = wpeb_video_plan_compute(1920, 1080, 200, -223, 960, 540, 1920, 992, 1080, WPEB_CROP_MODE_SQUASH);
    assert(p.action == WPEB_VIDEO_ACTION_WINDOW);
    assert(p.clipped);
    assert(p.sx == 0 && p.sy == 0 && p.sw == 1920 && p.sh == 1080);
    assert(p.dx == 200 && p.dy == 0 && p.dw == 960 && p.dh == 405);

    // Same input in park fallback mode.
    p = wpeb_video_plan_compute(1920, 1080, 200, -223, 960, 540, 1920, 992, 1080, WPEB_CROP_MODE_PARK);
    assert(p.action == WPEB_VIDEO_ACTION_PARK);
    assert(p.clipped);
    assert(p.dx == 0 && p.dy == 0 && p.dw == 16 && p.dh == 9);
}

static void test_bottom_left_right_and_corners(void)
{
    // Bottom-left corner clipped (fullscreen: page_h == surface_h == 1080).
    // Video 960x540 at (-240, 810): left 1/4 offscreen, bottom 1/2 offscreen.
    struct wpeb_video_plan p = wpeb_video_plan_compute(1920, 1080, -240, 810, 960, 540, 1920, 1080, 1080, WPEB_CROP_MODE_CROP);
    assert(p.action == WPEB_VIDEO_ACTION_CROP);
    assert(p.sx == 480 && p.sy == 0 && p.sw == 1440 && p.sh == 540);
    assert(p.dx == 0 && p.dy == 810 && p.dw == 720 && p.dh == 270);

    // Right edge clipped: video 960x540 at (1440, 100) -> right 1/2 offscreen.
    p = wpeb_video_plan_compute(1920, 1080, 1440, 100, 960, 540, 1920, 1080, 1080, WPEB_CROP_MODE_CROP);
    assert(p.action == WPEB_VIDEO_ACTION_CROP);
    assert(p.sx == 0 && p.sy == 0 && p.sw == 960 && p.sh == 1080);
    assert(p.dx == 1440 && p.dy == 100 && p.dw == 480 && p.dh == 540);
}

static void test_sliver_clamping_and_offscreen(void)
{
    // 1 px sliver at the right and bottom edge: must NEVER exceed picture or surface bounds.
    struct wpeb_video_plan p = wpeb_video_plan_compute(1920, 1080, 1919, 1079, 960, 540, 1920, 1080, 1080, WPEB_CROP_MODE_CROP);
    assert(p.action == WPEB_VIDEO_ACTION_CROP);
    assert(p.sx >= 0 && p.sy >= 0 && p.sw >= 2 && p.sh >= 2);
    assert(p.sx + p.sw <= 1920 && p.sy + p.sh <= 1080);
    assert(p.dx >= 0 && p.dy >= 0 && p.dw >= 2 && p.dh >= 2);
    assert(p.dx + p.dw <= 1920 && p.dy + p.dh <= 1080);

    // 1 px sliver at the top edge (dy = -539, dh = 540 -> y0 = 0, y1 = 1).
    p = wpeb_video_plan_compute(1920, 1080, 100, -539, 960, 540, 1920, 1080, 1080, WPEB_CROP_MODE_CROP);
    assert(p.action == WPEB_VIDEO_ACTION_CROP);
    assert(p.sy >= 0 && p.sh >= 2 && p.sy + p.sh <= 1080);
    assert(p.dy == 0 && p.dh == 2 && p.dy + p.dh <= 1080);

    // Completely scrolled off the top (dy = -600, dh = 540 -> y1 = -60 <= 0): MUST park, never send negative dy.
    p = wpeb_video_plan_compute(1920, 1080, 200, -688, 960, 540, 1920, 992, 1080, WPEB_CROP_MODE_CROP);
    assert(p.action == WPEB_VIDEO_ACTION_PARK);
    assert(p.dx == 0 && p.dy == 0 && p.dw == 16 && p.dh == 9);

    // Completely scrolled off the bottom (dy = 1200 >= 1080): MUST park.
    p = wpeb_video_plan_compute(1920, 1080, 200, 1200, 960, 540, 1920, 992, 1080, WPEB_CROP_MODE_CROP);
    assert(p.action == WPEB_VIDEO_ACTION_PARK);
    assert(p.dx == 0 && p.dy == 0 && p.dw == 16 && p.dh == 9);

    // Unknown picture dimensions (sw=0, sh=0) fall back to element dimensions w, h.
    p = wpeb_video_plan_compute(0, 0, 200, -270, 960, 540, 1920, 1080, 1080, WPEB_CROP_MODE_CROP);
    assert(p.action == WPEB_VIDEO_ACTION_CROP);
    assert(p.ow == 960 && p.oh == 540);
    assert(p.sx == 0 && p.sy == 270 && p.sw == 960 && p.sh == 270);
    assert(p.dx == 200 && p.dy == 0 && p.dw == 960 && p.dh == 270);
}

int main(void)
{
    test_os_info_parsing();
    test_mode_resolution();
    test_fully_visible_and_under_toolbar();
    test_top_crop_squash_park();
    test_bottom_left_right_and_corners();
    test_sliver_clamping_and_offscreen();
    printf("test_video_crop: ALL PASSED\n");
    return 0;
}
