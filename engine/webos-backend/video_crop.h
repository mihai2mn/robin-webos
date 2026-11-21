// SPDX-License-Identifier: GPL-3.0-or-later
// Pure geometry and policy helpers for hardware video plane placement on webOS (wl_webos_foreign / wl_webos_exported).
// Kept header-only and free of Wayland/GLib types so host-side unit tests exercise the exact code built into
// libWPEBackend-webos.so.
#pragma once

#include <ctype.h>
#include <stdint.h>
#include <string.h>

enum wpeb_crop_mode {
    WPEB_CROP_MODE_CROP = 0,   // wl_webos_exported.set_crop_region (webOS 5+ default)
    WPEB_CROP_MODE_SQUASH = 1, // wl_webos_exported.set_exported_window clipped to surface (webOS 4 / fallback)
    WPEB_CROP_MODE_PARK = 2,   // park clipped video at 0,0 16x9 (diagnostic / A/B fallback)
};

enum wpeb_video_action {
    WPEB_VIDEO_ACTION_WINDOW = 0, // set_exported_window(src, dst)
    WPEB_VIDEO_ACTION_CROP = 1,   // set_crop_region(org, src, dst)
    WPEB_VIDEO_ACTION_PARK = 2,   // set_exported_window(full, 0,0 16x9) when offscreen or in park mode
};

struct wpeb_video_plan {
    enum wpeb_video_action action;
    int clipped; // 1 if the requested rectangle extended past the surface bounds
    int ow, oh;  // original_input size (whole coded picture)
    int sx, sy, sw, sh; // source_region in picture pixels
    int dx, dy, dw, dh; // destination_region in surface pixels
};

static inline const char *wpeb_crop_mode_name(enum wpeb_crop_mode mode)
{
    switch (mode) {
    case WPEB_CROP_MODE_CROP:
        return "crop";
    case WPEB_CROP_MODE_SQUASH:
        return "squash";
    case WPEB_CROP_MODE_PARK:
        return "park";
    }
    return "crop";
}

static inline const char *wpeb_video_plan_label(const struct wpeb_video_plan *plan)
{
    if (!plan)
        return "none";
    if (plan->action == WPEB_VIDEO_ACTION_CROP)
        return "crop";
    if (plan->action == WPEB_VIDEO_ACTION_PARK)
        return "park";
    return plan->clipped ? "squash" : "window";
}

// Extracts the major version number from /var/run/nyx/os_info.json's "webos_release" field (e.g. "5.3.0" -> 5).
// Returns 0 when absent, unreadable, or malformed.
static inline int wpeb_webos_major_from_os_info(const char *json)
{
    if (!json)
        return 0;
    const char *p = strstr(json, "\"webos_release\"");
    if (!p)
        return 0;
    p += strlen("\"webos_release\"");
    while (*p && isspace((unsigned char)*p))
        ++p;
    if (*p != ':')
        return 0;
    ++p;
    while (*p && isspace((unsigned char)*p))
        ++p;
    if (*p != '"')
        return 0;
    ++p;
    if (!isdigit((unsigned char)*p))
        return 0;
    int major = 0;
    while (isdigit((unsigned char)*p) && major < 1000)
        major = major * 10 + (*p++ - '0');
    return major;
}

// Resolves the clipping policy from environment variables and detected webOS major version:
//   1. WPE_BROWSER_DEBUG_VIDEO_CROP (forwarded by browser/engine_client.cpp via {"wpebEnv": ...}):
//      "crop" -> force set_crop_region
//      "squash" | "0" | "off" -> shrink-to-fit via set_exported_window
//      "park" -> park clipped video at (0, 0, 16, 9)
//      "auto" | unset | empty -> auto
//   2. Legacy env fallbacks (when WPE_BROWSER_DEBUG_VIDEO_CROP is unset/empty):
//      WPEB_NO_CROP (non-empty and != "0") -> squash
//      WPEB_CROP_FALLBACK="squash" | "park"
//   3. Auto default: set_crop_region (CROP), except on webOS 1..4 where wl_webos_exported predates
//      set_crop_region (added May 2019 for webOS 5.0; sending opcode 2 on webOS 4 would be a fatal protocol error).
static inline enum wpeb_crop_mode wpeb_crop_mode_resolve(const char *debug_env, const char *legacy_no_crop,
                                                         const char *legacy_fallback, int webos_major)
{
    if (debug_env && *debug_env && strcmp(debug_env, "auto") != 0) {
        if (!strcmp(debug_env, "crop") || !strcmp(debug_env, "1") || !strcmp(debug_env, "on"))
            return WPEB_CROP_MODE_CROP;
        if (!strcmp(debug_env, "squash") || !strcmp(debug_env, "0") || !strcmp(debug_env, "off"))
            return WPEB_CROP_MODE_SQUASH;
        if (!strcmp(debug_env, "park"))
            return WPEB_CROP_MODE_PARK;
    } else {
        if (legacy_no_crop && *legacy_no_crop && strcmp(legacy_no_crop, "0") != 0)
            return WPEB_CROP_MODE_SQUASH;
        if (legacy_fallback && *legacy_fallback) {
            if (!strcmp(legacy_fallback, "squash"))
                return WPEB_CROP_MODE_SQUASH;
            if (!strcmp(legacy_fallback, "park"))
                return WPEB_CROP_MODE_PARK;
            if (!strcmp(legacy_fallback, "crop"))
                return WPEB_CROP_MODE_CROP;
        }
    }
    if (webos_major >= 1 && webos_major <= 4)
        return WPEB_CROP_MODE_SQUASH;
    return WPEB_CROP_MODE_CROP;
}

// Computes the exported-window placement for a video rectangle (x, y, w, h) in page-buffer pixels on a surface of size
// (surface_w, surface_h) whose bottom page_h pixels hold the page (the top surface_h - page_h strip sits behind the UI
// window's opaque docked toolbar).
//
// Key properties:
// - When the video scrolls partly under the docked toolbar but still stays inside [0, surface_h] (0 <= dy < top_inset),
//   it is NOT clipped at all: the plane extends cleanly behind the opaque UI toolbar without scaling or cropping.
// - When the video extends past any edge of [0, surface_w] x [0, surface_h] and still intersects it:
//     * CROP mode computes exact proportional source coordinates clamped inside [0, picture_w] x [0, picture_h] and
//       issues set_crop_region(org, src, dst).
//     * SQUASH mode clips dst to the surface and issues set_exported_window(full, dst).
//     * PARK mode parks dst at (0, 0, 16, 9) via set_exported_window.
// - When the video is completely outside [0, surface_w] x [0, surface_h] (zero intersection), it is parked at
//   (0, 0, 16, 9) instead of sending invalid out-of-bounds/negative destination coordinates to the compositor.
static inline struct wpeb_video_plan wpeb_video_plan_compute(int sw, int sh, int x, int y, int w, int h, int surface_w,
                                                             int page_h, int surface_h, enum wpeb_crop_mode mode)
{
    struct wpeb_video_plan out;
    memset(&out, 0, sizeof(out));

    const int picture_w = sw > 0 ? sw : w;
    const int picture_h = sh > 0 ? sh : h;
    out.ow = picture_w;
    out.oh = picture_h;
    out.sx = 0;
    out.sy = 0;
    out.sw = picture_w;
    out.sh = picture_h;

    const int top_inset = surface_h > page_h ? surface_h - page_h : 0;
    const int dx = x;
    const int dy = y + top_inset;
    const int dw = w;
    const int dh = h;

    const int x0 = dx < 0 ? 0 : dx;
    const int y0 = dy < 0 ? 0 : dy;
    const int x1 = dx + dw > surface_w ? surface_w : dx + dw;
    const int y1 = dy + dh > surface_h ? surface_h : dy + dh;

    // Completely outside the surface: never send negative or out-of-bounds coordinates to the compositor.
    if (x1 <= x0 || y1 <= y0 || surface_w <= 0 || surface_h <= 0 || dw <= 0 || dh <= 0) {
        out.action = WPEB_VIDEO_ACTION_PARK;
        out.clipped = 1;
        out.dx = 0;
        out.dy = 0;
        out.dw = surface_w >= 16 ? 16 : (surface_w > 0 ? surface_w : 16);
        out.dh = surface_h >= 9 ? 9 : (surface_h > 0 ? surface_h : 9);
        return out;
    }

    const int clipped = (x0 != dx || y0 != dy || x1 != dx + dw || y1 != dy + dh);
    out.clipped = clipped;
    if (!clipped) {
        out.action = WPEB_VIDEO_ACTION_WINDOW;
        out.dx = dx;
        out.dy = dy;
        out.dw = dw;
        out.dh = dh;
        return out;
    }

    if (mode == WPEB_CROP_MODE_PARK) {
        out.action = WPEB_VIDEO_ACTION_PARK;
        out.dx = 0;
        out.dy = 0;
        out.dw = surface_w >= 16 ? 16 : surface_w;
        out.dh = surface_h >= 9 ? 9 : surface_h;
        return out;
    }

    int vis_x = x0;
    int vis_y = y0;
    int vis_w = x1 - x0;
    int vis_h = y1 - y0;
    const int min_dw = surface_w < 2 ? surface_w : 2;
    const int min_dh = surface_h < 2 ? surface_h : 2;
    if (vis_w < min_dw) {
        vis_w = min_dw;
        if (vis_x + vis_w > surface_w)
            vis_x = surface_w - vis_w;
    }
    if (vis_h < min_dh) {
        vis_h = min_dh;
        if (vis_y + vis_h > surface_h)
            vis_y = surface_h - vis_h;
    }

    out.dx = vis_x;
    out.dy = vis_y;
    out.dw = vis_w;
    out.dh = vis_h;

    if (mode == WPEB_CROP_MODE_SQUASH) {
        out.action = WPEB_VIDEO_ACTION_WINDOW;
        return out;
    }

    // Proportional source crop in coded-picture coordinates, clamped strictly inside [0, picture_w] x [0, picture_h]
    // (mirrors LG Chromium's ComputeVideoHoleDisplayRect and avoids invalid-param rejections on 1 px slivers).
    int cx = (int)(((int64_t)(x0 - dx) * picture_w + dw / 2) / dw);
    int cy = (int)(((int64_t)(y0 - dy) * picture_h + dh / 2) / dh);
    int cw = (int)(((int64_t)(x1 - x0) * picture_w + dw / 2) / dw);
    int ch = (int)(((int64_t)(y1 - y0) * picture_h + dh / 2) / dh);

    const int min_sw = picture_w < 2 ? picture_w : 2;
    const int min_sh = picture_h < 2 ? picture_h : 2;
    if (cw < min_sw)
        cw = min_sw;
    if (ch < min_sh)
        ch = min_sh;
    if (cw > picture_w)
        cw = picture_w;
    if (ch > picture_h)
        ch = picture_h;
    if (cx < 0)
        cx = 0;
    if (cy < 0)
        cy = 0;
    if (cx + cw > picture_w)
        cx = picture_w - cw;
    if (cy + ch > picture_h)
        cy = picture_h - ch;

    out.action = WPEB_VIDEO_ACTION_CROP;
    out.sx = cx;
    out.sy = cy;
    out.sw = cw;
    out.sh = ch;
    return out;
}
