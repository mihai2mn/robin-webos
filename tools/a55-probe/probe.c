// SPDX-License-Identifier: GPL-3.0-or-later
// A55 instruction probe: does code built with the candidate CPU flags run on the TV? Each test computes the same
// result by a path the flags change (hardware divide, fused multiply-add, ARMv8 rounding, CRC32, NEON) and by a
// portable reference the compiler cannot fuse (volatile/noinline), and compares. Prints "ok name" / "FAIL name".
// PROBE_EXTRA=1 (-DPROBE_EXTRA) adds the optional ARMv8.2 extensions (dot product, half-precision) that
// -mcpu=cortex-a55 would use; they are only compiled when asked.
#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <string.h>
#include <arm_neon.h>
#if defined(__ARM_FEATURE_CRC32)
#include <arm_acle.h>
#endif
static int bad;
#define CHECK(name, cond) do { if (cond) printf("ok %s\n", name); else { printf("FAIL %s\n", name); bad++; } } while (0)
__attribute__((noinline)) static int32_t sdiv_(int32_t a, int32_t b) { return a / b; }
__attribute__((noinline)) static uint32_t udiv_(uint32_t a, uint32_t b) { return a / b; }
__attribute__((noinline)) static float fma_(float a, float b, float c) { return a * b + c; }
__attribute__((noinline)) static double dfma_(double a, double b, double c) { return a * b + c; }
__attribute__((noinline)) static float rint_(float x) { return rintf(x); }
__attribute__((noinline)) static float floor_(float x) { return floorf(x); }
__attribute__((noinline)) static double round_(double x) { return round(x); }
__attribute__((noinline)) static float sqrt_(float x) { return sqrtf(x); }
int main(void) {
    volatile int32_t a = -1000003, b = 977; volatile uint32_t ua = 4000000007u, ub = 65537u;
    CHECK("sdiv", sdiv_(a, b) == -1023 && a % b == -1000003 + 1023 * 977);
    CHECK("udiv", udiv_(ua, ub) == 61034 && ua - 61034u * ub == 4000000007u - 61034u * 65537u);
    int ok = 1;
    for (int32_t i = -50; i <= 50; i++) for (int32_t j = 1; j < 40; j++) if (sdiv_(i * 7919, j) != (int32_t)((i * 7919) / j)) ok = 0;
    CHECK("sdiv-sweep", ok);
    volatile float fa = 1.0f + 1.0f / (1 << 20), fb = 1.0f - 1.0f / (1 << 20), fc = -1.0f;
    float f = fma_(fa, fb, fc);               // fused: -2^-40 exactly; unfused float: 0
    CHECK("fma-float", f == -9.094947e-13f || f == 0.0f);
    volatile double da = 1.0 + 1e-8, db = 1.0 - 1e-8, dc = -1.0;
    CHECK("fma-double", fabs(dfma_(da, db, dc) + 1e-16) < 1e-15);
    CHECK("rint", rint_(2.5f) == 2.0f && rint_(3.5f) == 4.0f && rint_(-2.5f) == -2.0f);
    CHECK("floor", floor_(-2.5f) == -3.0f && floor_(2.5f) == 2.0f);
    CHECK("round", round_(2.5) == 3.0 && round_(-2.5) == -3.0 && round_(0.49999999999999994) == 0.0);
    CHECK("sqrt", sqrt_(2.0f) > 1.41421f && sqrt_(2.0f) < 1.41422f);
#if defined(__ARM_FEATURE_CRC32)
    uint32_t crc = 0xffffffffu; const char *s = "123456789";
    for (int i = 0; i < 9; i++) crc = __crc32b(crc, (uint8_t)s[i]);
    CHECK("crc32b", ~crc == 0xCBF43926u);
#else
    printf("skip crc32 (not enabled)\n");
#endif
    int32_t xs[8] = {1, 2, 3, 4, 5, 6, 7, 8}, ys[8] = {8, 7, 6, 5, 4, 3, 2, 1};
    int32x4_t acc = vdupq_n_s32(0);
    acc = vmlaq_s32(acc, vld1q_s32(xs), vld1q_s32(ys)); acc = vmlaq_s32(acc, vld1q_s32(xs + 4), vld1q_s32(ys + 4));
    int32_t lanes[4]; vst1q_s32(lanes, acc);
    CHECK("neon-mla", lanes[0] + lanes[1] + lanes[2] + lanes[3] == 120);
#ifdef __ARM_FEATURE_FMA
    float32x4_t v = vfmaq_f32(vdupq_n_f32(1.0f), vdupq_n_f32(2.0f), vdupq_n_f32(3.0f));
    CHECK("neon-fma", vgetq_lane_f32(v, 0) == 7.0f);
#endif
#ifdef PROBE_EXTRA
    uint8_t da8[16], db8[16]; for (int i = 0; i < 16; i++) { da8[i] = i + 1; db8[i] = 2; }
    uint32x4_t dp = vdotq_u32(vdupq_n_u32(0), vld1q_u8(da8), vld1q_u8(db8));
    CHECK("dotprod", vgetq_lane_u32(dp, 0) == 20 && vgetq_lane_u32(dp, 3) == 116);
    float16x8_t h = vdupq_n_f16(1.5f); float16x8_t h2 = vaddq_f16(h, h);
    CHECK("fp16-add", (float)vgetq_lane_f16(h2, 0) == 3.0f);
#endif
    puts(bad ? "RESULT FAIL" : "RESULT PASS");
    return bad != 0;
}
