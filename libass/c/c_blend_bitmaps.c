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

#include <stddef.h>
#include <stdint.h>

#include "ass_utils.h"


#define ALIGNMENT  16

/**
 * \brief Add two bitmaps together at a given position
 * Uses additive blending, clipped to [0,255]. Pure C implementation.
 */
void ass_add_bitmaps_c(uint8_t *restrict dst, ptrdiff_t dst_stride,
                       const uint8_t *restrict src, ptrdiff_t src_stride,
                       size_t width, size_t height)
{
    ASSUME(!(dst_stride % ALIGNMENT));
    ASSUME(!(src_stride % ALIGNMENT));
    ASSUME(width > 0 && height > 0);

    uint8_t *end = dst + dst_stride * height;
    while (dst < end) {
        for (size_t x = 0; x < width; x++) {
            unsigned out = dst[x] + src[x];
            dst[x] = FFMIN(out, 255);
        }
        dst += dst_stride;
        src += src_stride;
    }
}

void ass_imul_bitmaps_c(uint8_t *restrict dst, ptrdiff_t dst_stride,
                        const uint8_t *restrict src, ptrdiff_t src_stride,
                        size_t width, size_t height)
{
    ASSUME(!(dst_stride % ALIGNMENT));
    ASSUME(!(src_stride % ALIGNMENT));
    ASSUME(width > 0 && height > 0);

    uint8_t *end = dst + dst_stride * height;
    while (dst < end) {
        for (size_t x = 0; x < width; x++) {
            dst[x] = (dst[x] * (255 - src[x]) + 255) >> 8;
        }
        dst += dst_stride;
        src += src_stride;
    }
}

void ass_mul_bitmaps_c(uint8_t *restrict dst, ptrdiff_t dst_stride,
                       const uint8_t *restrict src1, ptrdiff_t src1_stride,
                       const uint8_t *restrict src2, ptrdiff_t src2_stride,
                       size_t width, size_t height)
{
    ASSUME(!((uintptr_t) dst % ALIGNMENT) && !(dst_stride % ALIGNMENT));
    ASSUME(!(src1_stride % ALIGNMENT));
    ASSUME(!(src2_stride % ALIGNMENT));
    ASSUME(width > 0 && height > 0);

    uint8_t *end = dst + dst_stride * height;
    while (dst < end) {
        for (size_t x = 0; x < width; x++) {
            dst[x] = (src1[x] * src2[x] + 255) >> 8;
        }
        dst  += dst_stride;
        src1 += src1_stride;
        src2 += src2_stride;
    }
}

#if !ARCH_AARCH64 && defined(__arm__) && defined(__ARM_NEON)
#include <arm_neon.h>

void ass_add_bitmaps_neon(uint8_t *restrict dst, ptrdiff_t dst_stride,
                          const uint8_t *restrict src, ptrdiff_t src_stride,
                          size_t width, size_t height)
{
    ASSUME(!(dst_stride % ALIGNMENT));
    ASSUME(!(src_stride % ALIGNMENT));
    ASSUME(width > 0 && height > 0);

    uint8_t *end = dst + dst_stride * height;
    while (dst < end) {
        size_t x = 0;
        for (; x + 16 <= width; x += 16)
            vst1q_u8(dst + x, vqaddq_u8(vld1q_u8(dst + x), vld1q_u8(src + x)));
        if (x + 8 <= width) {
            vst1_u8(dst + x, vqadd_u8(vld1_u8(dst + x), vld1_u8(src + x)));
            x += 8;
        }
        for (; x < width; x++) {
            unsigned out = dst[x] + src[x];
            dst[x] = FFMIN(out, 255);
        }
        dst += dst_stride;
        src += src_stride;
    }
}

void ass_imul_bitmaps_neon(uint8_t *restrict dst, ptrdiff_t dst_stride,
                           const uint8_t *restrict src, ptrdiff_t src_stride,
                           size_t width, size_t height)
{
    ASSUME(!(dst_stride % ALIGNMENT));
    ASSUME(!(src_stride % ALIGNMENT));
    ASSUME(width > 0 && height > 0);

    const uint16x8_t v255 = vdupq_n_u16(255);
    uint8_t *end = dst + dst_stride * height;
    while (dst < end) {
        size_t x = 0;
        for (; x + 16 <= width; x += 16) {
            uint8x16_t vd = vld1q_u8(dst + x);
            uint8x16_t vs = vmvnq_u8(vld1q_u8(src + x));
            uint16x8_t lo = vmlal_u8(v255, vget_low_u8(vd), vget_low_u8(vs));
            uint16x8_t hi = vmlal_u8(v255, vget_high_u8(vd), vget_high_u8(vs));
            vst1q_u8(dst + x, vcombine_u8(vshrn_n_u16(lo, 8), vshrn_n_u16(hi, 8)));
        }
        if (x + 8 <= width) {
            uint8x8_t vd = vld1_u8(dst + x);
            uint8x8_t vs = vmvn_u8(vld1_u8(src + x));
            uint16x8_t lo = vmlal_u8(v255, vd, vs);
            vst1_u8(dst + x, vshrn_n_u16(lo, 8));
            x += 8;
        }
        for (; x < width; x++)
            dst[x] = (dst[x] * (255 - src[x]) + 255) >> 8;
        dst += dst_stride;
        src += src_stride;
    }
}

void ass_mul_bitmaps_neon(uint8_t *restrict dst, ptrdiff_t dst_stride,
                          const uint8_t *restrict src1, ptrdiff_t src1_stride,
                          const uint8_t *restrict src2, ptrdiff_t src2_stride,
                          size_t width, size_t height)
{
    ASSUME(!((uintptr_t) dst % ALIGNMENT) && !(dst_stride % ALIGNMENT));
    ASSUME(!(src1_stride % ALIGNMENT));
    ASSUME(!(src2_stride % ALIGNMENT));
    ASSUME(width > 0 && height > 0);

    const uint16x8_t v255 = vdupq_n_u16(255);
    uint8_t *end = dst + dst_stride * height;
    while (dst < end) {
        size_t x = 0;
        for (; x + 16 <= width; x += 16) {
            uint8x16_t v1 = vld1q_u8(src1 + x);
            uint8x16_t v2 = vld1q_u8(src2 + x);
            uint16x8_t lo = vmlal_u8(v255, vget_low_u8(v1), vget_low_u8(v2));
            uint16x8_t hi = vmlal_u8(v255, vget_high_u8(v1), vget_high_u8(v2));
            vst1q_u8(dst + x, vcombine_u8(vshrn_n_u16(lo, 8), vshrn_n_u16(hi, 8)));
        }
        if (x + 8 <= width) {
            uint8x8_t v1 = vld1_u8(src1 + x);
            uint8x8_t v2 = vld1_u8(src2 + x);
            uint16x8_t lo = vmlal_u8(v255, v1, v2);
            vst1_u8(dst + x, vshrn_n_u16(lo, 8));
            x += 8;
        }
        for (; x < width; x++)
            dst[x] = (src1[x] * src2[x] + 255) >> 8;
        dst  += dst_stride;
        src1 += src1_stride;
        src2 += src2_stride;
    }
}
#endif
