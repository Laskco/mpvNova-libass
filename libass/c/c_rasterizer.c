/*
 * Copyright (C) 2014-2022 libass contributors
 *
 * This file is part of libass.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "config.h"
#include "ass_compat.h"

#include <assert.h>

#include "ass_rasterizer.h"


#define ALIGNMENT  16

#define TILE_SIZE  16
#include "rasterizer_template.h"
#undef TILE_SIZE

#define TILE_SIZE  32
#include "rasterizer_template.h"
#undef TILE_SIZE

#if !ARCH_AARCH64 && defined(__arm__) && defined(__ARM_NEON)
#include <arm_neon.h>

#define NEON_RESCALE_AB(ab, scale, tile_order) \
    (((ab) * (int64_t) (scale) + ((int64_t) 1 << (45 + (tile_order)))) >> (46 + (tile_order)))
#define NEON_RESCALE_C(c, scale, tile_order) \
    (((int32_t) ((c) >> (7 + (tile_order))) * (int64_t) (scale) + ((int64_t) 1 << 44)) >> 45)

static inline void neon_update_border16(int16_t *res,
                                        int32_t abs_a, int32_t b, int32_t abs_b,
                                        int16x8_t vc0, int16x8_t vc1,
                                        int32_t up, int32_t dn)
{
    int32_t size = dn - up;
    int32_t w = 1024 + (size << 4) - abs_a;
    if (w > 1024) w = 1024;
    w <<= 3;

    int32_t dc_b = (abs_b * size) >> 6;
    int32_t dc = (FFMIN(abs_a, dc_b) + 2) >> 2;

    int32_t base = (b * (up + dn)) >> 7;
    int32_t offs1 = size - (((base + dc) * w) >> 16);
    int32_t offs2 = size - (((base - dc) * w) >> 16);

    int16x4_t vw = vdup_n_s16((int16_t) w);
    int16x8_t voffs1 = vdupq_n_s16((int16_t) offs1);
    int16x8_t voffs2 = vdupq_n_s16((int16_t) offs2);
    int16x8_t vzero = vdupq_n_s16(0);
    int16x8_t vsize = vdupq_n_s16((int16_t) (size << 1));

    int32x4_t p0_lo = vmull_s16(vget_low_s16(vc0), vw);
    int32x4_t p0_hi = vmull_s16(vget_high_s16(vc0), vw);
    int16x8_t cw0 = vcombine_s16(vshrn_n_s32(p0_lo, 16), vshrn_n_s32(p0_hi, 16));
    int16x8_t c1_0 = vminq_s16(vmaxq_s16(vaddq_s16(cw0, voffs1), vzero), vsize);
    int16x8_t c2_0 = vminq_s16(vmaxq_s16(vaddq_s16(cw0, voffs2), vzero), vsize);
    vst1q_s16(res, vaddq_s16(vld1q_s16(res), vaddq_s16(c1_0, c2_0)));

    int32x4_t p1_lo = vmull_s16(vget_low_s16(vc1), vw);
    int32x4_t p1_hi = vmull_s16(vget_high_s16(vc1), vw);
    int16x8_t cw1 = vcombine_s16(vshrn_n_s32(p1_lo, 16), vshrn_n_s32(p1_hi, 16));
    int16x8_t c1_1 = vminq_s16(vmaxq_s16(vaddq_s16(cw1, voffs1), vzero), vsize);
    int16x8_t c2_1 = vminq_s16(vmaxq_s16(vaddq_s16(cw1, voffs2), vzero), vsize);
    vst1q_s16(res + 8, vaddq_s16(vld1q_s16(res + 8), vaddq_s16(c1_1, c2_1)));
}

static inline void neon_update_border32(int16_t *res,
                                        int32_t abs_a, int32_t b, int32_t abs_b,
                                        int16x8_t vc, int16x8_t vstep,
                                        int32_t up, int32_t dn)
{
    int32_t size = dn - up;
    int32_t w = 512 + (size << 3) - abs_a;
    if (w > 512) w = 512;
    w <<= 5;

    int32_t dc_b = (abs_b * size) >> 6;
    int32_t dc = (FFMIN(abs_a, dc_b) + 2) >> 2;

    int32_t base = (b * (up + dn)) >> 7;
    int32_t offs1 = size - (((base + dc) * w) >> 16);
    int32_t offs2 = size - (((base - dc) * w) >> 16);

    int16x4_t vw = vdup_n_s16((int16_t) w);
    int16x8_t voffs1 = vdupq_n_s16((int16_t) offs1);
    int16x8_t voffs2 = vdupq_n_s16((int16_t) offs2);
    int16x8_t vzero = vdupq_n_s16(0);
    int16x8_t vsize = vdupq_n_s16((int16_t) (size << 1));

    for (int v = 0; v < 4; v++) {
        int32x4_t p_lo = vmull_s16(vget_low_s16(vc), vw);
        int32x4_t p_hi = vmull_s16(vget_high_s16(vc), vw);
        int16x8_t cw = vcombine_s16(vshrn_n_s32(p_lo, 16), vshrn_n_s32(p_hi, 16));
        int16x8_t c1 = vminq_s16(vmaxq_s16(vaddq_s16(cw, voffs1), vzero), vsize);
        int16x8_t c2 = vminq_s16(vmaxq_s16(vaddq_s16(cw, voffs2), vzero), vsize);
        vst1q_s16(res + v * 8, vaddq_s16(vld1q_s16(res + v * 8), vaddq_s16(c1, c2)));
        vc = vsubq_s16(vc, vstep);
    }
}

#define DEFINE_NEON_SOLID_AND_HALFPLANE(T_SIZE, T_ORDER, N_VEC, R_SHIFT) \
void ass_fill_solid_tile ## T_SIZE ## _neon(uint8_t *buf, ptrdiff_t stride, int set) \
{ \
    ASSUME(!((uintptr_t) buf % ALIGNMENT) && !(stride % ALIGNMENT)); \
    uint8x16_t v = vdupq_n_u8(set ? 255 : 0); \
    for (int y = 0; y < T_SIZE; y++) { \
        for (int x = 0; x < T_SIZE; x += 16) \
            vst1q_u8(buf + x, v); \
        buf += stride; \
    } \
} \
\
void ass_fill_halfplane_tile ## T_SIZE ## _neon(uint8_t *buf, ptrdiff_t stride, \
                                                int32_t a, int32_t b, int64_t c, int32_t scale) \
{ \
    ASSUME(!((uintptr_t) buf % ALIGNMENT) && !(stride % ALIGNMENT)); \
    const int32_t full_value = 1 << (14 - T_ORDER); \
    int32_t aa = (int32_t) NEON_RESCALE_AB(a, scale, T_ORDER); \
    int32_t bb = (int32_t) NEON_RESCALE_AB(b, scale, T_ORDER); \
    int32_t cc = (int32_t) NEON_RESCALE_C(c, scale, T_ORDER) + (full_value >> 1) - ((aa + bb) >> 1); \
    int32_t abs_a = aa < 0 ? -aa : aa; \
    int32_t abs_b = bb < 0 ? -bb : bb; \
    int32_t delta = (FFMIN(abs_a, abs_b) + 2) >> 2; \
    cc -= delta; \
\
    static const int16_t ramp[8] = { 0, 1, 2, 3, 4, 5, 6, 7 }; \
    int16x8_t vc = vmlsq_n_s16(vdupq_n_s16((int16_t) cc), vld1q_s16(ramp), (int16_t) aa); \
    int16x8_t vstep = vdupq_n_s16((int16_t) (8 * aa)); \
    int16x8_t vbadj = vdupq_n_s16((int16_t) (bb - (T_SIZE - 8) * aa)); \
    int16x8_t vdelta2 = vdupq_n_s16((int16_t) (2 * delta)); \
    int16x8_t vzero = vdupq_n_s16(0); \
    int16x8_t vfull = vdupq_n_s16((int16_t) full_value); \
    for (int y = 0; y < T_SIZE; y++) { \
        for (int v = 0; v < N_VEC; v += 2) { \
            int16x8_t t0 = vaddq_s16(vc, vdelta2); \
            int16x8_t c1a = vminq_s16(vmaxq_s16(vc, vzero), vfull); \
            int16x8_t c2a = vminq_s16(vmaxq_s16(t0, vzero), vfull); \
            uint16x8_t ra = vreinterpretq_u16_s16(vaddq_s16(c1a, c2a)); \
            vc = vsubq_s16(vc, vstep); \
            int16x8_t t1 = vaddq_s16(vc, vdelta2); \
            int16x8_t c1b = vminq_s16(vmaxq_s16(vc, vzero), vfull); \
            int16x8_t c2b = vminq_s16(vmaxq_s16(t1, vzero), vfull); \
            uint16x8_t rb = vreinterpretq_u16_s16(vaddq_s16(c1b, c2b)); \
            vst1q_u8(buf + v * 8, vcombine_u8(vqshrn_n_u16(ra, R_SHIFT), \
                                              vqshrn_n_u16(rb, R_SHIFT))); \
            vc = vsubq_s16(vc, (v + 2 == N_VEC) ? vbadj : vstep); \
        } \
        buf += stride; \
    } \
} \
\
void ass_merge_tile ## T_SIZE ## _neon(uint8_t *restrict buf, ptrdiff_t stride, \
                                       const uint8_t *restrict tile) \
{ \
    ASSUME(!((uintptr_t) buf % ALIGNMENT) && !(stride % ALIGNMENT)); \
    for (int y = 0; y < T_SIZE; y++) { \
        for (int x = 0; x < T_SIZE; x += 16) \
            vst1q_u8(buf + x, vmaxq_u8(vld1q_u8(buf + x), vld1q_u8(tile + x))); \
        buf += stride; \
        tile += T_SIZE; \
    } \
}

DEFINE_NEON_SOLID_AND_HALFPLANE(16, 4, 2, 3)
DEFINE_NEON_SOLID_AND_HALFPLANE(32, 5, 4, 2)

void ass_fill_generic_tile16_neon(uint8_t *restrict buf, ptrdiff_t stride,
                                  const struct segment *restrict line, size_t n_lines,
                                  int winding)
{
    ASSUME(!((uintptr_t) buf % ALIGNMENT) && !(stride % ALIGNMENT));
    int16x8_t vzero = vdupq_n_s16(0);
    int16x8_t vfull = vdupq_n_s16(1024);
    int16_t res[16 * 16] __attribute__((aligned(16)));
    int16_t delta[18] __attribute__((aligned(16))) = {0};
    memset(res, 0, sizeof(res));

    static const int16_t ramp[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
    int16x8_t vramp = vld1q_s16(ramp);
    const struct segment *end = line + n_lines;
    for (; line != end; ++line) {
        assert(line->y_min >= 0 && line->y_min < 1024);
        assert(line->y_max > 0 && line->y_max <= 1024);
        assert(line->y_min <= line->y_max);

        int32_t flags = line->flags;
        int32_t up_delta = flags & SEGFLAG_DN ? 4 : 0;
        int32_t dn_delta = up_delta;
        if (!line->x_min && (flags & SEGFLAG_EXACT_LEFT)) dn_delta ^= 4;
        if (flags & SEGFLAG_UL_DR) {
            int32_t tmp = up_delta;
            up_delta = dn_delta;
            dn_delta = tmp;
        }

        int32_t y_min = line->y_min, y_max = line->y_max;
        int32_t up = y_min >> 6, dn = y_max >> 6;
        int32_t up_pos = y_min & 63, dn_pos = y_max & 63;
        if (up_delta) {
            int32_t d1 = up_pos << 2;
            delta[up + 1] -= (int16_t) d1;
            delta[up] -= (int16_t) (256 - d1);
        }
        if (dn_delta) {
            int32_t d1 = dn_pos << 2;
            delta[dn + 1] += (int16_t) d1;
            delta[dn] += (int16_t) (256 - d1);
        }
        if (y_min == y_max)
            continue;

        int32_t scale = line->scale;
        int32_t a = (int32_t) NEON_RESCALE_AB(line->a, scale, 4);
        int32_t b = (int32_t) NEON_RESCALE_AB(line->b, scale, 4);
        int32_t c = (int32_t) NEON_RESCALE_C(line->c, scale, 4) - (a >> 1) - b * up;

        int32_t abs_a = a < 0 ? -a : a;
        int32_t abs_b = b < 0 ? -b : b;
        int16x8_t vc0 = vmlsq_n_s16(vdupq_n_s16((int16_t) c), vramp, (int16_t) a);
        int16x8_t vc1 = vsubq_s16(vc0, vdupq_n_s16((int16_t) (8 * a)));
        int16x8_t vb = vdupq_n_s16((int16_t) b);
        int16_t *row = res + up * 16;

        if (up_pos) {
            if (dn == up) {
                neon_update_border16(row, abs_a, b, abs_b, vc0, vc1, up_pos, dn_pos);
                continue;
            }
            neon_update_border16(row, abs_a, b, abs_b, vc0, vc1, up_pos, 64);
            up++;
            row += 16;
            vc0 = vsubq_s16(vc0, vb);
            vc1 = vsubq_s16(vc1, vb);
        }
        if (up < dn) {
            int32_t dc = (FFMIN(abs_a, abs_b) + 2) >> 2;
            int32_t base = 512 - (b >> 1) - dc;
            int16x8_t vbase = vdupq_n_s16((int16_t) base);
            int16x8_t vdc2 = vdupq_n_s16((int16_t) (2 * dc));
            int16x8_t cur0 = vaddq_s16(vc0, vbase);
            int16x8_t cur1 = vaddq_s16(vc1, vbase);
            for (int32_t y = up; y < dn; y++) {
                int16x8_t t0 = vaddq_s16(cur0, vdc2);
                int16x8_t c1_0 = vminq_s16(vmaxq_s16(cur0, vzero), vfull);
                int16x8_t c2_0 = vminq_s16(vmaxq_s16(t0, vzero), vfull);
                vst1q_s16(row, vsraq_n_s16(vld1q_s16(row), vaddq_s16(c1_0, c2_0), 3));

                int16x8_t t1 = vaddq_s16(cur1, vdc2);
                int16x8_t c1_1 = vminq_s16(vmaxq_s16(cur1, vzero), vfull);
                int16x8_t c2_1 = vminq_s16(vmaxq_s16(t1, vzero), vfull);
                vst1q_s16(row + 8, vsraq_n_s16(vld1q_s16(row + 8), vaddq_s16(c1_1, c2_1), 3));

                cur0 = vsubq_s16(cur0, vb);
                cur1 = vsubq_s16(cur1, vb);
                row += 16;
            }
            if (dn_pos) {
                vc0 = vsubq_s16(cur0, vbase);
                vc1 = vsubq_s16(cur1, vbase);
                neon_update_border16(row, abs_a, b, abs_b, vc0, vc1, 0, dn_pos);
            }
        } else if (dn_pos) {
            neon_update_border16(row, abs_a, b, abs_b, vc0, vc1, 0, dn_pos);
        }
    }

    int32_t cur = 256 * (int8_t) winding;
    const int16_t *row = res;
    for (int y = 0; y < 16; y++) {
        cur += delta[y];
        int16x8_t vcur = vdupq_n_s16((int16_t) cur);
        int16x8_t a0 = vabsq_s16(vaddq_s16(vld1q_s16(row), vcur));
        int16x8_t a1 = vabsq_s16(vaddq_s16(vld1q_s16(row + 8), vcur));
        vst1q_u8(buf, vcombine_u8(vqmovun_s16(a0), vqmovun_s16(a1)));
        row += 16;
        buf += stride;
    }
}

void ass_fill_generic_tile32_neon(uint8_t *restrict buf, ptrdiff_t stride,
                                  const struct segment *restrict line, size_t n_lines,
                                  int winding)
{
    ASSUME(!((uintptr_t) buf % ALIGNMENT) && !(stride % ALIGNMENT));
    int16x8_t vzero = vdupq_n_s16(0);
    int16x8_t vfull = vdupq_n_s16(512);
    int16_t res[32 * 32] __attribute__((aligned(16)));
    int16_t delta[34] __attribute__((aligned(16))) = {0};
    memset(res, 0, sizeof(res));

    static const int16_t ramp[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
    int16x8_t vramp = vld1q_s16(ramp);
    const struct segment *end = line + n_lines;
    for (; line != end; ++line) {
        assert(line->y_min >= 0 && line->y_min < 2048);
        assert(line->y_max > 0 && line->y_max <= 2048);
        assert(line->y_min <= line->y_max);

        int32_t flags = line->flags;
        int32_t up_delta = flags & SEGFLAG_DN ? 4 : 0;
        int32_t dn_delta = up_delta;
        if (!line->x_min && (flags & SEGFLAG_EXACT_LEFT)) dn_delta ^= 4;
        if (flags & SEGFLAG_UL_DR) {
            int32_t tmp = up_delta;
            up_delta = dn_delta;
            dn_delta = tmp;
        }

        int32_t y_min = line->y_min, y_max = line->y_max;
        int32_t up = y_min >> 6, dn = y_max >> 6;
        int32_t up_pos = y_min & 63, dn_pos = y_max & 63;
        if (up_delta) {
            int32_t d1 = up_pos << 2;
            delta[up + 1] -= (int16_t) d1;
            delta[up] -= (int16_t) (256 - d1);
        }
        if (dn_delta) {
            int32_t d1 = dn_pos << 2;
            delta[dn + 1] += (int16_t) d1;
            delta[dn] += (int16_t) (256 - d1);
        }
        if (y_min == y_max)
            continue;

        int32_t scale = line->scale;
        int32_t a = (int32_t) NEON_RESCALE_AB(line->a, scale, 5);
        int32_t b = (int32_t) NEON_RESCALE_AB(line->b, scale, 5);
        int32_t c = (int32_t) NEON_RESCALE_C(line->c, scale, 5) - (a >> 1) - b * up;

        int32_t abs_a = a < 0 ? -a : a;
        int32_t abs_b = b < 0 ? -b : b;
        int16x8_t vc = vmlsq_n_s16(vdupq_n_s16((int16_t) c), vramp, (int16_t) a);
        int16x8_t vstep = vdupq_n_s16((int16_t) (8 * a));
        int16x8_t vb = vdupq_n_s16((int16_t) b);
        int16_t *row = res + up * 32;

        if (up_pos) {
            if (dn == up) {
                neon_update_border32(row, abs_a, b, abs_b, vc, vstep, up_pos, dn_pos);
                continue;
            }
            neon_update_border32(row, abs_a, b, abs_b, vc, vstep, up_pos, 64);
            up++;
            row += 32;
            vc = vsubq_s16(vc, vb);
        }
        if (up < dn) {
            int32_t dc = (FFMIN(abs_a, abs_b) + 2) >> 2;
            int32_t base = 256 - (b >> 1) - dc;
            int16x8_t vbase = vdupq_n_s16((int16_t) base);
            int16x8_t vdc2 = vdupq_n_s16((int16_t) (2 * dc));
            int16x8_t vbadj = vdupq_n_s16((int16_t) (b - 24 * a));
            int16x8_t cur = vaddq_s16(vc, vbase);
            for (int32_t y = up; y < dn; y++) {
                for (int v = 0; v < 4; v++) {
                    int16x8_t t = vaddq_s16(cur, vdc2);
                    int16x8_t c1 = vminq_s16(vmaxq_s16(cur, vzero), vfull);
                    int16x8_t c2 = vminq_s16(vmaxq_s16(t, vzero), vfull);
                    vst1q_s16(row + v * 8, vsraq_n_s16(vld1q_s16(row + v * 8), vaddq_s16(c1, c2), 2));
                    cur = vsubq_s16(cur, v == 3 ? vbadj : vstep);
                }
                row += 32;
            }
            if (dn_pos) {
                vc = vsubq_s16(cur, vbase);
                neon_update_border32(row, abs_a, b, abs_b, vc, vstep, 0, dn_pos);
            }
        } else if (dn_pos) {
            neon_update_border32(row, abs_a, b, abs_b, vc, vstep, 0, dn_pos);
        }
    }

    int32_t cur = 256 * (int8_t) winding;
    const int16_t *row = res;
    for (int y = 0; y < 32; y++) {
        cur += delta[y];
        int16x8_t vcur = vdupq_n_s16((int16_t) cur);
        for (int v = 0; v < 4; v += 2) {
            int16x8_t a0 = vabsq_s16(vaddq_s16(vld1q_s16(row + v * 8), vcur));
            int16x8_t a1 = vabsq_s16(vaddq_s16(vld1q_s16(row + v * 8 + 8), vcur));
            vst1q_u8(buf + v * 8, vcombine_u8(vqmovun_s16(a0), vqmovun_s16(a1)));
        }
        row += 32;
        buf += stride;
    }
}

#undef DEFINE_NEON_SOLID_AND_HALFPLANE
#undef NEON_RESCALE_AB
#undef NEON_RESCALE_C
#endif
