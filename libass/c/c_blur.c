/*
 * Copyright (C) 2015-2022 libass contributors
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
#include "ass_utils.h"

#include <stddef.h>
#include <stdint.h>
#include <memory.h>


static int16_t zero_line[16];
static int16_t dither_line[32] = {
     8, 40,  8, 40,  8, 40,  8, 40,  8, 40,  8, 40,  8, 40,  8, 40,
    56, 24, 56, 24, 56, 24, 56, 24, 56, 24, 56, 24, 56, 24, 56, 24,
};

inline static const int16_t *get_line(const int16_t *ptr, size_t offs, size_t size)
{
    return offs < size ? ptr + offs : zero_line;
}

static inline int16_t shrink_func(int16_t p1p, int16_t p1n,
                                  int16_t z0p, int16_t z0n,
                                  int16_t n1p, int16_t n1n)
{
    /*
    return (1 * p1p + 5 * p1n + 10 * z0p + 10 * z0n + 5 * n1p + 1 * n1n + 16) >> 5;
    */
    int32_t r = (p1p + p1n + n1p + n1n) >> 1;
    r = (r + z0p + z0n) >> 1;
    r = (r + p1n + n1p) >> 1;
    return (r + z0p + z0n + 2) >> 2;
}

static inline void expand_func(int16_t *rp, int16_t *rn,
                               int16_t p1, int16_t z0, int16_t n1)
{
    /*
    *rp = (5 * p1 + 10 * z0 + 1 * n1 + 8) >> 4;
    *rn = (1 * p1 + 10 * z0 + 5 * n1 + 8) >> 4;
    */
    uint16_t r = (uint16_t) (((uint16_t) (p1 + n1) >> 1) + z0) >> 1;
    *rp = (uint16_t) (((uint16_t) (r + p1) >> 1) + z0 + 1) >> 1;
    *rn = (uint16_t) (((uint16_t) (r + n1) >> 1) + z0 + 1) >> 1;
}


#define ALIGNMENT     16
#define SUFFIX(name)  name ## 16_c
#include "blur_template.h"
#undef ALIGNMENT
#undef SUFFIX

#define ALIGNMENT     32
#define SUFFIX(name)  name ## 32_c
#include "blur_template.h"
#undef ALIGNMENT
#undef SUFFIX

#if !ARCH_AARCH64 && defined(__arm__) && defined(__ARM_NEON)
#include <arm_neon.h>

static inline uint16x8_t neon_shrink_vec(uint16x8_t p1p, uint16x8_t p1n,
                                         uint16x8_t z0p, uint16x8_t z0n,
                                         uint16x8_t n1p, uint16x8_t n1n)
{
    uint16x8_t sum_outer = vaddq_u16(p1p, n1n);
    uint16x8_t sum_mid   = vaddq_u16(p1n, n1p);
    uint16x8_t sum_ctr   = vaddq_u16(z0p, z0n);
    uint16x8_t r = vhaddq_u16(sum_outer, sum_mid);
    r = vhaddq_u16(r, sum_ctr);
    r = vhaddq_u16(r, sum_mid);
    return vrshrq_n_u16(vhaddq_u16(r, sum_ctr), 1);
}

static inline void neon_expand_vec(uint16x8_t *rp, uint16x8_t *rn,
                                   uint16x8_t p1, uint16x8_t z0, uint16x8_t n1)
{
    uint16x8_t r = vhaddq_u16(vhaddq_u16(p1, n1), z0);
    *rp = vrhaddq_u16(vhaddq_u16(r, p1), z0);
    *rn = vrhaddq_u16(vhaddq_u16(r, n1), z0);
}

void ass_stripe_unpack16_neon(int16_t *restrict dst, const uint8_t *restrict src,
                              ptrdiff_t src_stride, size_t width, size_t height)
{
    ASSUME(!((uintptr_t) dst % 16));
    ASSUME(!((uintptr_t) src % 16) && !(src_stride % 16));
    ASSUME(width > 0 && height > 0);

    size_t step = 8 * height;
    size_t step2 = 16 * height;
    size_t w16 = width & ~(size_t) 15;
    bool tail8 = (width - w16) > 0;
    bool tail16 = (width - w16) > 8;
    for (size_t y = 0; y < height; y++) {
        int16_t *ptr = dst;
        for (size_t x = 0; x < w16; x += 16) {
            uint8x16_t s = vld1q_u8(src + x);
            uint8x8x2_t z0 = vzip_u8(vget_low_u8(s), vget_low_u8(s));
            uint8x8x2_t z1 = vzip_u8(vget_high_u8(s), vget_high_u8(s));
            uint16x8_t u0 = vreinterpretq_u16_u8(vcombine_u8(z0.val[0], z0.val[1]));
            uint16x8_t u1 = vreinterpretq_u16_u8(vcombine_u8(z1.val[0], z1.val[1]));
            vst1q_s16(ptr, vreinterpretq_s16_u16(vrshrq_n_u16(u0, 2)));
            vst1q_s16(ptr + step, vreinterpretq_s16_u16(vrshrq_n_u16(u1, 2)));
            ptr += step2;
        }
        if (tail8) {
            uint8x8_t s0 = vld1_u8(src + w16);
            uint8x8x2_t z0 = vzip_u8(s0, s0);
            uint16x8_t u0 = vreinterpretq_u16_u8(vcombine_u8(z0.val[0], z0.val[1]));
            vst1q_s16(ptr, vreinterpretq_s16_u16(vrshrq_n_u16(u0, 2)));
            if (tail16) {
                uint8x8_t s1 = vld1_u8(src + w16 + 8);
                uint8x8x2_t z1 = vzip_u8(s1, s1);
                uint16x8_t u1 = vreinterpretq_u16_u8(vcombine_u8(z1.val[0], z1.val[1]));
                vst1q_s16(ptr + step, vreinterpretq_s16_u16(vrshrq_n_u16(u1, 2)));
            }
        }
        dst += 8;
        src += src_stride;
    }
}

void ass_stripe_pack16_neon(uint8_t *restrict dst, ptrdiff_t dst_stride,
                            const int16_t *restrict src, size_t width, size_t height)
{
    ASSUME(!((uintptr_t) dst % 16) && !(dst_stride % 16));
    ASSUME(!((uintptr_t) src % 16));
    ASSUME(width > 0 && height > 0);

    int16x8_t vd0 = vld1q_s16(dither_line);
    int16x8_t vd1 = vld1q_s16(dither_line + 16);
    size_t step = 8 * height;
    size_t x = 0;
    for (; x + 8 < width; x += 16) {
        uint8_t *ptr = dst;
        const int16_t *src2 = src + step;
        size_t y = 0;
        for (; y + 1 < height; y += 2) {
            int16x8_t a0 = vld1q_s16(src);
            int16x8_t b0 = vld1q_s16(src2);
            int16x8_t ta0 = vaddq_s16(vsubq_s16(a0, vshrq_n_s16(a0, 8)), vd0);
            int16x8_t tb0 = vaddq_s16(vsubq_s16(b0, vshrq_n_s16(b0, 8)), vd0);
            vst1q_u8(ptr, vcombine_u8(vshrn_n_u16(vreinterpretq_u16_s16(ta0), 6),
                                      vshrn_n_u16(vreinterpretq_u16_s16(tb0), 6)));

            int16x8_t a1 = vld1q_s16(src + 8);
            int16x8_t b1 = vld1q_s16(src2 + 8);
            int16x8_t ta1 = vaddq_s16(vsubq_s16(a1, vshrq_n_s16(a1, 8)), vd1);
            int16x8_t tb1 = vaddq_s16(vsubq_s16(b1, vshrq_n_s16(b1, 8)), vd1);
            vst1q_u8(ptr + dst_stride, vcombine_u8(vshrn_n_u16(vreinterpretq_u16_s16(ta1), 6),
                                                   vshrn_n_u16(vreinterpretq_u16_s16(tb1), 6)));
            ptr += 2 * dst_stride;
            src += 16;
            src2 += 16;
        }
        if (y < height) {
            int16x8_t a0 = vld1q_s16(src);
            int16x8_t b0 = vld1q_s16(src2);
            int16x8_t ta0 = vaddq_s16(vsubq_s16(a0, vshrq_n_s16(a0, 8)), vd0);
            int16x8_t tb0 = vaddq_s16(vsubq_s16(b0, vshrq_n_s16(b0, 8)), vd0);
            vst1q_u8(ptr, vcombine_u8(vshrn_n_u16(vreinterpretq_u16_s16(ta0), 6),
                                      vshrn_n_u16(vreinterpretq_u16_s16(tb0), 6)));
            src += 8;
        }
        src += step;
        dst += 16;
    }
    if (x < width) {
        uint8_t *ptr = dst;
        size_t y = 0;
        for (; y + 1 < height; y += 2) {
            int16x8_t s0 = vld1q_s16(src);
            int16x8_t t0 = vaddq_s16(vsubq_s16(s0, vshrq_n_s16(s0, 8)), vd0);
            vst1_u8(ptr, vshrn_n_u16(vreinterpretq_u16_s16(t0), 6));

            int16x8_t s1 = vld1q_s16(src + 8);
            int16x8_t t1 = vaddq_s16(vsubq_s16(s1, vshrq_n_s16(s1, 8)), vd1);
            vst1_u8(ptr + dst_stride, vshrn_n_u16(vreinterpretq_u16_s16(t1), 6));

            ptr += 2 * dst_stride;
            src += 16;
        }
        if (y < height) {
            int16x8_t s0 = vld1q_s16(src);
            int16x8_t t0 = vaddq_s16(vsubq_s16(s0, vshrq_n_s16(s0, 8)), vd0);
            vst1_u8(ptr, vshrn_n_u16(vreinterpretq_u16_s16(t0), 6));
            src += 8;
        }
        dst += 8;
    }
    size_t left = dst_stride - ((width + 7) & ~(size_t) 7);
    if (left) {
        for (size_t y = 0; y < height; y++) {
            memset(dst, 0, left);
            dst += dst_stride;
        }
    }
}

void ass_shrink_horz16_neon(int16_t *restrict dst, const int16_t *restrict src,
                            size_t src_width, size_t src_height)
{
    ASSUME(!((uintptr_t) dst % 16));
    ASSUME(!((uintptr_t) src % 16));
    ASSUME(src_width > 0 && src_height > 0);

    size_t dst_width = (src_width + 5) >> 1;
    size_t n_src_stripes = (src_width + 7) >> 3;
    size_t size = n_src_stripes * 8 * src_height;
    size_t step = 8 * src_height;
    size_t dst_stripes = (dst_width + 7) >> 3;

    size_t offs = 0;
    for (size_t sx = 0; sx < dst_stripes; sx++) {
        if (sx >= 1 && 2 * sx + 1 < n_src_stripes) {
            const int16_t *p0 = src + offs - step;
            const int16_t *p1 = src + offs;
            const int16_t *p2 = src + offs + step;
            for (size_t y = 0; y < src_height; y++) {
                uint16x8_t v0 = vreinterpretq_u16_s16(vld1q_s16(p0));
                uint16x8_t v1 = vreinterpretq_u16_s16(vld1q_s16(p1));
                uint16x8_t v2 = vreinterpretq_u16_s16(vld1q_s16(p2));

                uint16x8x2_t uz01 = vuzpq_u16(v0, v1);
                uint16x4x2_t uz2  = vuzp_u16(vget_low_u16(v2), vget_high_u16(v2));
                uint16x8_t e2 = vcombine_u16(uz2.val[0], uz2.val[0]);
                uint16x8_t o2 = vcombine_u16(uz2.val[1], uz2.val[1]);

                uint16x8_t p1p = vextq_u16(uz01.val[0], e2, 2);
                uint16x8_t z0p = vextq_u16(uz01.val[0], e2, 3);
                uint16x8_t n1p = vextq_u16(uz01.val[0], e2, 4);
                uint16x8_t p1n = vextq_u16(uz01.val[1], o2, 2);
                uint16x8_t z0n = vextq_u16(uz01.val[1], o2, 3);
                uint16x8_t n1n = vextq_u16(uz01.val[1], o2, 4);

                vst1q_s16(dst, vreinterpretq_s16_u16(neon_shrink_vec(p1p, p1n, z0p, z0n, n1p, n1n)));
                dst += 8; p0 += 8; p1 += 8; p2 += 8;
            }
            offs += 2 * step;
        } else {
            for (size_t y = 0; y < src_height; y++) {
                uint16x8_t v0 = vreinterpretq_u16_s16(vld1q_s16(get_line(src, offs - step, size)));
                uint16x8_t v1 = vreinterpretq_u16_s16(vld1q_s16(get_line(src, offs, size)));
                uint16x8_t v2 = vreinterpretq_u16_s16(vld1q_s16(get_line(src, offs + step, size)));

                uint16x8x2_t uz01 = vuzpq_u16(v0, v1);
                uint16x4x2_t uz2  = vuzp_u16(vget_low_u16(v2), vget_high_u16(v2));
                uint16x8_t e2 = vcombine_u16(uz2.val[0], uz2.val[0]);
                uint16x8_t o2 = vcombine_u16(uz2.val[1], uz2.val[1]);

                uint16x8_t p1p = vextq_u16(uz01.val[0], e2, 2);
                uint16x8_t z0p = vextq_u16(uz01.val[0], e2, 3);
                uint16x8_t n1p = vextq_u16(uz01.val[0], e2, 4);
                uint16x8_t p1n = vextq_u16(uz01.val[1], o2, 2);
                uint16x8_t z0n = vextq_u16(uz01.val[1], o2, 3);
                uint16x8_t n1n = vextq_u16(uz01.val[1], o2, 4);

                vst1q_s16(dst, vreinterpretq_s16_u16(neon_shrink_vec(p1p, p1n, z0p, z0n, n1p, n1n)));
                dst  += 8;
                offs += 8;
            }
            offs += step;
        }
    }
}

void ass_shrink_vert16_neon(int16_t *restrict dst, const int16_t *restrict src,
                            size_t src_width, size_t src_height)
{
    ASSUME(!((uintptr_t) dst % 16));
    ASSUME(!((uintptr_t) src % 16));
    ASSUME(src_width > 0 && src_height > 0);

    size_t dst_height = (src_height + 5) >> 1;
    size_t full_pairs = src_height >> 1;
    size_t step = 8 * src_height;
    uint16x8_t vzero = vdupq_n_u16(0);

    for (size_t x = 0; x < src_width; x += 8) {
        const int16_t *s = src;
        uint16x8_t p1p = vzero, p1n = vzero, z0p = vzero, z0n = vzero;
        for (size_t y = 0; y < full_pairs; y++) {
            uint16x8_t n1p = vreinterpretq_u16_s16(vld1q_s16(s));
            uint16x8_t n1n = vreinterpretq_u16_s16(vld1q_s16(s + 8));
            vst1q_s16(dst, vreinterpretq_s16_u16(neon_shrink_vec(p1p, p1n, z0p, z0n, n1p, n1n)));
            p1p = z0p; p1n = z0n;
            z0p = n1p; z0n = n1n;
            s += 16;
            dst += 8;
        }
        size_t y = full_pairs;
        if (src_height & 1) {
            uint16x8_t n1p = vreinterpretq_u16_s16(vld1q_s16(s));
            vst1q_s16(dst, vreinterpretq_s16_u16(neon_shrink_vec(p1p, p1n, z0p, z0n, n1p, vzero)));
            p1p = z0p; p1n = z0n;
            z0p = n1p; z0n = vzero;
            dst += 8;
            y++;
        }
        for (; y < dst_height; y++) {
            vst1q_s16(dst, vreinterpretq_s16_u16(neon_shrink_vec(p1p, p1n, z0p, z0n, vzero, vzero)));
            p1p = z0p; p1n = z0n;
            z0p = vzero; z0n = vzero;
            dst += 8;
        }
        src += step;
    }
}

void ass_expand_horz16_neon(int16_t *restrict dst, const int16_t *restrict src,
                            size_t src_width, size_t src_height)
{
    ASSUME(!((uintptr_t) dst % 16));
    ASSUME(!((uintptr_t) src % 16));
    ASSUME(src_width > 0 && src_height > 0);

    size_t dst_width = 2 * src_width + 4;
    size_t n_src_stripes = (src_width + 7) >> 3;
    size_t size = n_src_stripes * 8 * src_height;
    size_t step = 8 * src_height;

    size_t offs = 0;
    size_t sx = 0;
    for (size_t x = 8; x < dst_width; x += 16, sx++) {
        if (sx >= 1 && sx < n_src_stripes) {
            const int16_t *p0 = src + offs - step;
            const int16_t *p1 = src + offs;
            for (size_t y = 0; y < src_height; y++) {
                uint16x8_t v0 = vreinterpretq_u16_s16(vld1q_s16(p0));
                uint16x8_t v1 = vreinterpretq_u16_s16(vld1q_s16(p1));
                uint16x8_t t0 = vextq_u16(v0, v1, 6);
                uint16x8_t z0 = vextq_u16(v0, v1, 7);
                uint16x8_t rp, rn;
                neon_expand_vec(&rp, &rn, t0, z0, v1);
                uint16x8x2_t z = vzipq_u16(rp, rn);
                vst1q_s16(dst, vreinterpretq_s16_u16(z.val[0]));
                vst1q_s16(dst + step, vreinterpretq_s16_u16(z.val[1]));
                dst += 8; p0 += 8; p1 += 8;
            }
            offs += step;
        } else {
            for (size_t y = 0; y < src_height; y++) {
                uint16x8_t v0 = vreinterpretq_u16_s16(vld1q_s16(get_line(src, offs - step, size)));
                uint16x8_t v1 = vreinterpretq_u16_s16(vld1q_s16(get_line(src, offs, size)));
                uint16x8_t p1 = vextq_u16(v0, v1, 6);
                uint16x8_t z0 = vextq_u16(v0, v1, 7);
                uint16x8_t rp, rn;
                neon_expand_vec(&rp, &rn, p1, z0, v1);
                uint16x8x2_t z = vzipq_u16(rp, rn);
                vst1q_s16(dst, vreinterpretq_s16_u16(z.val[0]));
                vst1q_s16(dst + step, vreinterpretq_s16_u16(z.val[1]));
                dst  += 8;
                offs += 8;
            }
        }
        dst += step;
    }
    if ((dst_width - 1) & 8)
        return;

    for (size_t y = 0; y < src_height; y++) {
        uint16x8_t v0 = vreinterpretq_u16_s16(vld1q_s16(get_line(src, offs - step, size)));
        uint16x8_t v1 = vreinterpretq_u16_s16(vld1q_s16(get_line(src, offs, size)));
        uint16x8_t p1 = vextq_u16(v0, v1, 6);
        uint16x8_t z0 = vextq_u16(v0, v1, 7);
        uint16x8_t rp, rn;
        neon_expand_vec(&rp, &rn, p1, z0, v1);
        uint16x8x2_t z = vzipq_u16(rp, rn);
        vst1q_s16(dst, vreinterpretq_s16_u16(z.val[0]));
        dst  += 8;
        offs += 8;
    }
}

void ass_expand_vert16_neon(int16_t *restrict dst, const int16_t *restrict src,
                            size_t src_width, size_t src_height)
{
    ASSUME(!((uintptr_t) dst % 16));
    ASSUME(!((uintptr_t) src % 16));
    ASSUME(src_width > 0 && src_height > 0);

    size_t step = 8 * src_height;
    uint16x8_t vzero = vdupq_n_u16(0);

    for (size_t x = 0; x < src_width; x += 8) {
        const int16_t *s = src;
        uint16x8_t p1 = vzero, z0 = vzero;
        for (size_t k = 0; k < src_height; k++) {
            uint16x8_t n1 = vreinterpretq_u16_s16(vld1q_s16(s));
            uint16x8_t rp, rn;
            neon_expand_vec(&rp, &rn, p1, z0, n1);
            vst1q_s16(dst,     vreinterpretq_s16_u16(rp));
            vst1q_s16(dst + 8, vreinterpretq_s16_u16(rn));
            p1 = z0;
            z0 = n1;
            s += 8;
            dst += 16;
        }
        uint16x8_t rp, rn;
        neon_expand_vec(&rp, &rn, p1, z0, vzero);
        vst1q_s16(dst,     vreinterpretq_s16_u16(rp));
        vst1q_s16(dst + 8, vreinterpretq_s16_u16(rn));
        neon_expand_vec(&rp, &rn, z0, vzero, vzero);
        vst1q_s16(dst + 16, vreinterpretq_s16_u16(rp));
        vst1q_s16(dst + 24, vreinterpretq_s16_u16(rn));
        dst += 32;
        src += step;
    }
}

#define NEON_BLUR_TAP_LANE(acc_lo, acc_hi, vc, v1, v2, pvec, lane) \
    do { \
        int16x8_t d1_ = vsubq_s16((v1), (vc)); \
        int16x8_t d2_ = vsubq_s16((v2), (vc)); \
        (acc_lo) = vmlal_lane_s16((acc_lo), vget_low_s16(d1_),  (pvec), (lane)); \
        (acc_hi) = vmlal_lane_s16((acc_hi), vget_high_s16(d1_), (pvec), (lane)); \
        (acc_lo) = vmlal_lane_s16((acc_lo), vget_low_s16(d2_),  (pvec), (lane)); \
        (acc_hi) = vmlal_lane_s16((acc_hi), vget_high_s16(d2_), (pvec), (lane)); \
    } while (0)

static inline bool neon_vec_is_zero(uint8x16_t v)
{
    uint32x4_t u = vreinterpretq_u32_u8(v);
    uint32x2_t o = vorr_u32(vget_low_u32(u), vget_high_u32(u));
    return (vget_lane_u32(o, 0) | vget_lane_u32(o, 1)) == 0;
}

static inline bool blur_horz_probe_uniform(const int16_t *src, size_t src_width,
                                           size_t src_height, bool need_prev2)
{
    if (src_width < 128 || src_height < 32 || src_width < (16384 + src_height - 1) / src_height)
        return false;
    size_t stripes = (src_width + 7) >> 3;
    size_t step = 8 * src_height;
    size_t sx_step = stripes >> 2;
    size_t sy_step = src_height >> 2;
    size_t sx0 = stripes >> 3;
    size_t sy0 = src_height >> 3;
    if (sx0 < 2) sx0 = 2;
    int uniform = 0;
    for (int i = 0; i < 4; i++) {
        size_t sx = sx0 + (size_t) i * sx_step;
        if (sx >= stripes) break;
        const int16_t *col = src + sx * step;
        for (int j = 0; j < 4; j++) {
            size_t sy = sy0 + (size_t) j * sy_step;
            const int16_t *p = col + sy * 8;
            int16x8_t s0 = vld1q_s16(p);
            int16x8_t s1 = vld1q_s16(p - step);
            int16x8_t vd = vdupq_lane_s16(vget_low_s16(s1), 0);
            uint8x16_t diff = vorrq_u8(vreinterpretq_u8_s16(veorq_s16(s1, vd)),
                                       vreinterpretq_u8_s16(veorq_s16(s0, vd)));
            if (need_prev2) {
                int16x8_t s2 = vld1q_s16(p - 2 * step);
                diff = vorrq_u8(diff, vreinterpretq_u8_s16(veorq_s16(s2, vd)));
            }
            if (neon_vec_is_zero(diff))
                uniform++;
        }
    }
    return uniform >= 4;
}

#define NEON_BLUR_HORZ_STEP(N, NEED_PREV2, CHECK_UNIFORM, TAPS_EXPR) \
    do { \
        if (CHECK_UNIFORM) { \
            int16x8_t vd_ = vdupq_lane_s16(vget_low_s16(s1), 0); \
            uint8x16_t diff_ = vorrq_u8(vreinterpretq_u8_s16(veorq_s16(s1, vd_)), \
                                        vreinterpretq_u8_s16(veorq_s16(s0, vd_))); \
            if (NEED_PREV2) \
                diff_ = vorrq_u8(diff_, vreinterpretq_u8_s16(veorq_s16(s2, vd_))); \
            if (neon_vec_is_zero(diff_)) { \
                vst1q_s16(dst, vd_); \
                break; \
            } \
        } \
        int32x4_t acc_lo = vhalf, acc_hi = vhalf; \
        int16x8_t vc = (N == 8) ? s1 : vextq_s16(s1, s0, 8 - N); \
        TAPS_EXPR \
        int16x8_t delta = vcombine_s16(vshrn_n_s32(acc_lo, 16), vshrn_n_s32(acc_hi, 16)); \
        vst1q_s16(dst, vaddq_s16(vc, delta)); \
    } while (0)

#define NEON_BLUR_HORZ_BODY(N, NEED_PREV2, TAPS_EXPR) \
void ass_blur ## N ## _horz16_neon(int16_t *restrict dst, const int16_t *restrict src, \
                                   size_t src_width, size_t src_height, \
                                   const int16_t *restrict param) \
{ \
    ASSUME(!((uintptr_t) dst % 16)); \
    ASSUME(!((uintptr_t) src % 16)); \
    ASSUME(src_width > 0 && src_height > 0); \
    size_t dst_width = src_width + 2 * N; \
    size_t size = ((src_width + 7) & ~(size_t) 7) * src_height; \
    size_t step = 8 * src_height; \
    int16x4_t p0 = vld1_s16(param); \
    int16x4_t p1 = NEED_PREV2 ? vld1_s16(param + 4) : p0; \
    (void) p0; (void) p1; \
    int32x4_t vhalf = vdupq_n_s32(0x8000); \
    bool use_uniform = blur_horz_probe_uniform(src, src_width, src_height, NEED_PREV2); \
    size_t offs = 0; \
    size_t interior_start = NEED_PREV2 ? 16 : 8; \
    size_t src_aligned = (src_width + 7) & ~(size_t) 7; \
    for (size_t x = 0; x < dst_width; x += 8) { \
        bool interior = (x >= interior_start && x < src_aligned); \
        if (interior && use_uniform) { \
            const int16_t *p_s0 = src + offs; \
            const int16_t *p_s1 = p_s0 - step; \
            const int16_t *p_s2 = NEED_PREV2 ? (p_s1 - step) : p_s1; \
            for (size_t y = 0; y < src_height; y++) { \
                int16x8_t s0 = vld1q_s16(p_s0); \
                int16x8_t s1 = vld1q_s16(p_s1); \
                int16x8_t s2 = NEED_PREV2 ? vld1q_s16(p_s2) : s1; \
                (void) s2; \
                NEON_BLUR_HORZ_STEP(N, NEED_PREV2, 1, TAPS_EXPR); \
                dst += 8; p_s0 += 8; p_s1 += 8; p_s2 += 8; \
            } \
            offs += step; \
        } else if (interior) { \
            const int16_t *p_s0 = src + offs; \
            const int16_t *p_s1 = p_s0 - step; \
            const int16_t *p_s2 = NEED_PREV2 ? (p_s1 - step) : p_s1; \
            for (size_t y = 0; y < src_height; y++) { \
                int16x8_t s0 = vld1q_s16(p_s0); \
                int16x8_t s1 = vld1q_s16(p_s1); \
                int16x8_t s2 = NEED_PREV2 ? vld1q_s16(p_s2) : s1; \
                (void) s2; \
                NEON_BLUR_HORZ_STEP(N, NEED_PREV2, 0, TAPS_EXPR); \
                dst += 8; p_s0 += 8; p_s1 += 8; p_s2 += 8; \
            } \
            offs += step; \
        } else { \
            for (size_t y = 0; y < src_height; y++) { \
                int16x8_t s0 = vld1q_s16(get_line(src, offs, size)); \
                int16x8_t s1 = vld1q_s16(get_line(src, offs - step, size)); \
                int16x8_t s2 = NEED_PREV2 ? vld1q_s16(get_line(src, offs - 2 * step, size)) : s1; \
                (void) s2; \
                NEON_BLUR_HORZ_STEP(N, NEED_PREV2, use_uniform, TAPS_EXPR); \
                dst += 8; offs += 8; \
            } \
        } \
    } \
}

NEON_BLUR_HORZ_BODY(4, 0,
    NEON_BLUR_TAP_LANE(acc_lo, acc_hi, vc, s1,                 s0,                 p0, 3);
    NEON_BLUR_TAP_LANE(acc_lo, acc_hi, vc, vextq_s16(s1,s0,1), vextq_s16(s1,s0,7), p0, 2);
    NEON_BLUR_TAP_LANE(acc_lo, acc_hi, vc, vextq_s16(s1,s0,2), vextq_s16(s1,s0,6), p0, 1);
    NEON_BLUR_TAP_LANE(acc_lo, acc_hi, vc, vextq_s16(s1,s0,3), vextq_s16(s1,s0,5), p0, 0);
)

NEON_BLUR_HORZ_BODY(5, 1,
    NEON_BLUR_TAP_LANE(acc_lo, acc_hi, vc, vextq_s16(s2,s1,6), s0,                 p1, 0);
    NEON_BLUR_TAP_LANE(acc_lo, acc_hi, vc, vextq_s16(s2,s1,7), vextq_s16(s1,s0,7), p0, 3);
    NEON_BLUR_TAP_LANE(acc_lo, acc_hi, vc, s1,                 vextq_s16(s1,s0,6), p0, 2);
    NEON_BLUR_TAP_LANE(acc_lo, acc_hi, vc, vextq_s16(s1,s0,1), vextq_s16(s1,s0,5), p0, 1);
    NEON_BLUR_TAP_LANE(acc_lo, acc_hi, vc, vextq_s16(s1,s0,2), vextq_s16(s1,s0,4), p0, 0);
)

NEON_BLUR_HORZ_BODY(6, 1,
    NEON_BLUR_TAP_LANE(acc_lo, acc_hi, vc, vextq_s16(s2,s1,4), s0,                 p1, 1);
    NEON_BLUR_TAP_LANE(acc_lo, acc_hi, vc, vextq_s16(s2,s1,5), vextq_s16(s1,s0,7), p1, 0);
    NEON_BLUR_TAP_LANE(acc_lo, acc_hi, vc, vextq_s16(s2,s1,6), vextq_s16(s1,s0,6), p0, 3);
    NEON_BLUR_TAP_LANE(acc_lo, acc_hi, vc, vextq_s16(s2,s1,7), vextq_s16(s1,s0,5), p0, 2);
    NEON_BLUR_TAP_LANE(acc_lo, acc_hi, vc, s1,                 vextq_s16(s1,s0,4), p0, 1);
    NEON_BLUR_TAP_LANE(acc_lo, acc_hi, vc, vextq_s16(s1,s0,1), vextq_s16(s1,s0,3), p0, 0);
)

NEON_BLUR_HORZ_BODY(7, 1,
    NEON_BLUR_TAP_LANE(acc_lo, acc_hi, vc, vextq_s16(s2,s1,2), s0,                 p1, 2);
    NEON_BLUR_TAP_LANE(acc_lo, acc_hi, vc, vextq_s16(s2,s1,3), vextq_s16(s1,s0,7), p1, 1);
    NEON_BLUR_TAP_LANE(acc_lo, acc_hi, vc, vextq_s16(s2,s1,4), vextq_s16(s1,s0,6), p1, 0);
    NEON_BLUR_TAP_LANE(acc_lo, acc_hi, vc, vextq_s16(s2,s1,5), vextq_s16(s1,s0,5), p0, 3);
    NEON_BLUR_TAP_LANE(acc_lo, acc_hi, vc, vextq_s16(s2,s1,6), vextq_s16(s1,s0,4), p0, 2);
    NEON_BLUR_TAP_LANE(acc_lo, acc_hi, vc, vextq_s16(s2,s1,7), vextq_s16(s1,s0,3), p0, 1);
    NEON_BLUR_TAP_LANE(acc_lo, acc_hi, vc, s1,                 vextq_s16(s1,s0,2), p0, 0);
)

NEON_BLUR_HORZ_BODY(8, 1,
    NEON_BLUR_TAP_LANE(acc_lo, acc_hi, vc, s2,                 s0,                 p1, 3);
    NEON_BLUR_TAP_LANE(acc_lo, acc_hi, vc, vextq_s16(s2,s1,1), vextq_s16(s1,s0,7), p1, 2);
    NEON_BLUR_TAP_LANE(acc_lo, acc_hi, vc, vextq_s16(s2,s1,2), vextq_s16(s1,s0,6), p1, 1);
    NEON_BLUR_TAP_LANE(acc_lo, acc_hi, vc, vextq_s16(s2,s1,3), vextq_s16(s1,s0,5), p1, 0);
    NEON_BLUR_TAP_LANE(acc_lo, acc_hi, vc, vextq_s16(s2,s1,4), vextq_s16(s1,s0,4), p0, 3);
    NEON_BLUR_TAP_LANE(acc_lo, acc_hi, vc, vextq_s16(s2,s1,5), vextq_s16(s1,s0,3), p0, 2);
    NEON_BLUR_TAP_LANE(acc_lo, acc_hi, vc, vextq_s16(s2,s1,6), vextq_s16(s1,s0,2), p0, 1);
    NEON_BLUR_TAP_LANE(acc_lo, acc_hi, vc, vextq_s16(s2,s1,7), vextq_s16(s1,s0,1), p0, 0);
)

#define NEON_BLUR_VERT_BODY(N, NEED_P1, INNER_TAPS) \
void ass_blur ## N ## _vert16_neon(int16_t *restrict dst, const int16_t *restrict src, \
                                   size_t src_width, size_t src_height, \
                                   const int16_t *restrict param) \
{ \
    ASSUME(!((uintptr_t) dst % 16)); \
    ASSUME(!((uintptr_t) src % 16)); \
    ASSUME(src_width > 0 && src_height > 0); \
    size_t dst_height = src_height + 2 * N; \
    size_t step = 8 * src_height; \
    int16x4_t p0 = vld1_s16(param); \
    int16x4_t p1 = NEED_P1 ? vld1_s16(param + 4) : p0; \
    (void) p0; (void) p1; \
    int32x4_t vhalf = vdupq_n_s32(0x8000); \
    int16x8_t vzero = vdupq_n_s16(0); \
    size_t y_mid_start = 2 * N; \
    size_t y_mid_end = src_height > y_mid_start ? src_height : y_mid_start; \
    for (size_t x = 0; x < src_width; x += 8) { \
        int zero_run = 2 * N; \
        size_t y = 0; \
        size_t top_end = dst_height < y_mid_start ? dst_height : y_mid_start; \
        for (; y < top_end; y++) { \
            if (y < src_height && !neon_vec_is_zero(vreinterpretq_u8_s16(vld1q_s16(src + y * 8)))) \
                zero_run = 0; \
            else \
                zero_run++; \
            if (zero_run >= 2 * N + 1) { \
                vst1q_s16(dst, vzero); \
                dst += 8; \
                continue; \
            } \
            size_t offs = y * 8; \
            int32x4_t acc_lo = vhalf, acc_hi = vhalf; \
            int16x8_t vc = vld1q_s16(get_line(src, offs - (size_t) N * 8, step)); \
            for (int i = N; i > 0; i--) { \
                int16x8_t v1 = vld1q_s16(get_line(src, offs - (size_t) (N + i) * 8, step)); \
                int16x8_t v2 = vld1q_s16(get_line(src, offs - (size_t) (N - i) * 8, step)); \
                int16x4_t vp = vdup_n_s16(param[i - 1]); \
                int16x8_t d1 = vsubq_s16(v1, vc); \
                int16x8_t d2 = vsubq_s16(v2, vc); \
                acc_lo = vmlal_s16(acc_lo, vget_low_s16(d1), vp); \
                acc_hi = vmlal_s16(acc_hi, vget_high_s16(d1), vp); \
                acc_lo = vmlal_s16(acc_lo, vget_low_s16(d2), vp); \
                acc_hi = vmlal_s16(acc_hi, vget_high_s16(d2), vp); \
            } \
            int16x8_t delta = vcombine_s16(vshrn_n_s32(acc_lo, 16), vshrn_n_s32(acc_hi, 16)); \
            vst1q_s16(dst, vaddq_s16(vc, delta)); \
            dst += 8; \
        } \
        for (; y < y_mid_end; y++) { \
            if (!neon_vec_is_zero(vreinterpretq_u8_s16(vld1q_s16(src + y * 8)))) \
                zero_run = 0; \
            else \
                zero_run++; \
            if (zero_run >= 2 * N + 1) { \
                vst1q_s16(dst, vzero); \
                dst += 8; \
                continue; \
            } \
            const int16_t *cp = src + (y - N) * 8; \
            int32x4_t acc_lo = vhalf, acc_hi = vhalf; \
            int16x8_t vc = vld1q_s16(cp); \
            INNER_TAPS \
            int16x8_t delta = vcombine_s16(vshrn_n_s32(acc_lo, 16), vshrn_n_s32(acc_hi, 16)); \
            vst1q_s16(dst, vaddq_s16(vc, delta)); \
            dst += 8; \
        } \
        for (; y < dst_height; y++) { \
            zero_run++; \
            if (zero_run >= 2 * N + 1) { \
                vst1q_s16(dst, vzero); \
                dst += 8; \
                continue; \
            } \
            size_t offs = y * 8; \
            int32x4_t acc_lo = vhalf, acc_hi = vhalf; \
            int16x8_t vc = vld1q_s16(get_line(src, offs - (size_t) N * 8, step)); \
            for (int i = N; i > 0; i--) { \
                int16x8_t v1 = vld1q_s16(get_line(src, offs - (size_t) (N + i) * 8, step)); \
                int16x8_t v2 = vld1q_s16(get_line(src, offs - (size_t) (N - i) * 8, step)); \
                int16x4_t vp = vdup_n_s16(param[i - 1]); \
                int16x8_t d1 = vsubq_s16(v1, vc); \
                int16x8_t d2 = vsubq_s16(v2, vc); \
                acc_lo = vmlal_s16(acc_lo, vget_low_s16(d1), vp); \
                acc_hi = vmlal_s16(acc_hi, vget_high_s16(d1), vp); \
                acc_lo = vmlal_s16(acc_lo, vget_low_s16(d2), vp); \
                acc_hi = vmlal_s16(acc_hi, vget_high_s16(d2), vp); \
            } \
            int16x8_t delta = vcombine_s16(vshrn_n_s32(acc_lo, 16), vshrn_n_s32(acc_hi, 16)); \
            vst1q_s16(dst, vaddq_s16(vc, delta)); \
            dst += 8; \
        } \
        src += step; \
    } \
}

NEON_BLUR_VERT_BODY(4, 0,
    NEON_BLUR_TAP_LANE(acc_lo, acc_hi, vc, vld1q_s16(cp - 32), vld1q_s16(cp + 32), p0, 3);
    NEON_BLUR_TAP_LANE(acc_lo, acc_hi, vc, vld1q_s16(cp - 24), vld1q_s16(cp + 24), p0, 2);
    NEON_BLUR_TAP_LANE(acc_lo, acc_hi, vc, vld1q_s16(cp - 16), vld1q_s16(cp + 16), p0, 1);
    NEON_BLUR_TAP_LANE(acc_lo, acc_hi, vc, vld1q_s16(cp -  8), vld1q_s16(cp +  8), p0, 0);
)
NEON_BLUR_VERT_BODY(5, 1,
    NEON_BLUR_TAP_LANE(acc_lo, acc_hi, vc, vld1q_s16(cp - 40), vld1q_s16(cp + 40), p1, 0);
    NEON_BLUR_TAP_LANE(acc_lo, acc_hi, vc, vld1q_s16(cp - 32), vld1q_s16(cp + 32), p0, 3);
    NEON_BLUR_TAP_LANE(acc_lo, acc_hi, vc, vld1q_s16(cp - 24), vld1q_s16(cp + 24), p0, 2);
    NEON_BLUR_TAP_LANE(acc_lo, acc_hi, vc, vld1q_s16(cp - 16), vld1q_s16(cp + 16), p0, 1);
    NEON_BLUR_TAP_LANE(acc_lo, acc_hi, vc, vld1q_s16(cp -  8), vld1q_s16(cp +  8), p0, 0);
)
NEON_BLUR_VERT_BODY(6, 1,
    NEON_BLUR_TAP_LANE(acc_lo, acc_hi, vc, vld1q_s16(cp - 48), vld1q_s16(cp + 48), p1, 1);
    NEON_BLUR_TAP_LANE(acc_lo, acc_hi, vc, vld1q_s16(cp - 40), vld1q_s16(cp + 40), p1, 0);
    NEON_BLUR_TAP_LANE(acc_lo, acc_hi, vc, vld1q_s16(cp - 32), vld1q_s16(cp + 32), p0, 3);
    NEON_BLUR_TAP_LANE(acc_lo, acc_hi, vc, vld1q_s16(cp - 24), vld1q_s16(cp + 24), p0, 2);
    NEON_BLUR_TAP_LANE(acc_lo, acc_hi, vc, vld1q_s16(cp - 16), vld1q_s16(cp + 16), p0, 1);
    NEON_BLUR_TAP_LANE(acc_lo, acc_hi, vc, vld1q_s16(cp -  8), vld1q_s16(cp +  8), p0, 0);
)
NEON_BLUR_VERT_BODY(7, 1,
    NEON_BLUR_TAP_LANE(acc_lo, acc_hi, vc, vld1q_s16(cp - 56), vld1q_s16(cp + 56), p1, 2);
    NEON_BLUR_TAP_LANE(acc_lo, acc_hi, vc, vld1q_s16(cp - 48), vld1q_s16(cp + 48), p1, 1);
    NEON_BLUR_TAP_LANE(acc_lo, acc_hi, vc, vld1q_s16(cp - 40), vld1q_s16(cp + 40), p1, 0);
    NEON_BLUR_TAP_LANE(acc_lo, acc_hi, vc, vld1q_s16(cp - 32), vld1q_s16(cp + 32), p0, 3);
    NEON_BLUR_TAP_LANE(acc_lo, acc_hi, vc, vld1q_s16(cp - 24), vld1q_s16(cp + 24), p0, 2);
    NEON_BLUR_TAP_LANE(acc_lo, acc_hi, vc, vld1q_s16(cp - 16), vld1q_s16(cp + 16), p0, 1);
    NEON_BLUR_TAP_LANE(acc_lo, acc_hi, vc, vld1q_s16(cp -  8), vld1q_s16(cp +  8), p0, 0);
)
NEON_BLUR_VERT_BODY(8, 1,
    NEON_BLUR_TAP_LANE(acc_lo, acc_hi, vc, vld1q_s16(cp - 64), vld1q_s16(cp + 64), p1, 3);
    NEON_BLUR_TAP_LANE(acc_lo, acc_hi, vc, vld1q_s16(cp - 56), vld1q_s16(cp + 56), p1, 2);
    NEON_BLUR_TAP_LANE(acc_lo, acc_hi, vc, vld1q_s16(cp - 48), vld1q_s16(cp + 48), p1, 1);
    NEON_BLUR_TAP_LANE(acc_lo, acc_hi, vc, vld1q_s16(cp - 40), vld1q_s16(cp + 40), p1, 0);
    NEON_BLUR_TAP_LANE(acc_lo, acc_hi, vc, vld1q_s16(cp - 32), vld1q_s16(cp + 32), p0, 3);
    NEON_BLUR_TAP_LANE(acc_lo, acc_hi, vc, vld1q_s16(cp - 24), vld1q_s16(cp + 24), p0, 2);
    NEON_BLUR_TAP_LANE(acc_lo, acc_hi, vc, vld1q_s16(cp - 16), vld1q_s16(cp + 16), p0, 1);
    NEON_BLUR_TAP_LANE(acc_lo, acc_hi, vc, vld1q_s16(cp -  8), vld1q_s16(cp +  8), p0, 0);
)

#undef NEON_BLUR_TAP_LANE
#undef NEON_BLUR_HORZ_STEP
#undef NEON_BLUR_HORZ_BODY
#undef NEON_BLUR_VERT_BODY
#endif
