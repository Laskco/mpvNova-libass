/*
 * Copyright (C) 2009-2022 libass contributors
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


#define ALIGNMENT  16

static inline uint16_t sliding_sum(uint16_t *prev, uint16_t next)
{
    uint16_t sum = *prev + next;
    *prev = next;
    return sum;
}

/**
 * \brief Blur with [[1,2,1], [2,4,2], [1,2,1]] kernel
 * This blur is the same as the one employed by vsfilter.
 * Pure C implementation.
 */
void ass_be_blur_c(uint8_t *restrict buf, ptrdiff_t stride,
                   size_t width, size_t height, uint16_t *restrict tmp)
{
    ASSUME(!((uintptr_t) buf % ALIGNMENT) && !(stride % ALIGNMENT));
    ASSUME(!((uintptr_t) tmp % ALIGNMENT));
    ASSUME(width > 1 && height > 1);

    uint16_t *col_pix_buf = tmp;
    uint16_t *col_sum_buf = tmp + stride;

    {
        size_t x = 1;
        uint16_t sum = buf[x - 1];
        for ( ; x < width; x++) {
            uint16_t col_pix = sliding_sum(&sum, buf[x - 1] + buf[x]);
            col_pix_buf[x - 1] = col_sum_buf[x - 1] = col_pix;
        }
        uint16_t col_pix = sum + buf[x - 1];
        col_pix_buf[x - 1] = col_sum_buf[x - 1] = col_pix;
    }

    for (size_t y = 1; y < height; y++) {
        uint8_t *dst = buf;
        buf += stride;

        size_t x = 1;
        uint16_t sum = buf[x - 1];
        for ( ; x < width; x++) {
            uint16_t col_pix = sliding_sum(&sum, buf[x - 1] + buf[x]);
            uint16_t col_sum = sliding_sum(&col_pix_buf[x - 1], col_pix);
            dst[x - 1] = sliding_sum(&col_sum_buf[x - 1], col_sum) >> 4;
        }
        uint16_t col_pix = sum + buf[x - 1];
        uint16_t col_sum = sliding_sum(&col_pix_buf[x - 1], col_pix);
        dst[x - 1] = sliding_sum(&col_sum_buf[x - 1], col_sum) >> 4;
    }

    for (size_t x = 0; x < width; x++)
        buf[x] = (col_sum_buf[x] + col_pix_buf[x]) >> 4;
}

#if !ARCH_AARCH64 && defined(__arm__) && defined(__ARM_NEON)
#include <arm_neon.h>

void ass_be_blur_neon(uint8_t *restrict buf, ptrdiff_t stride,
                      size_t width, size_t height, uint16_t *restrict tmp)
{
    ASSUME(!((uintptr_t) buf % ALIGNMENT) && !(stride % ALIGNMENT));
    ASSUME(!((uintptr_t) tmp % ALIGNMENT));
    ASSUME(width > 1 && height > 1);

    uint16_t *col_pix_buf = tmp;
    uint16_t *col_sum_buf = tmp + stride;
    const uint8x16_t vzero = vdupq_n_u8(0);
    size_t vec_w = width > 16 ? ((width - 1) & ~(size_t) 15) : 0;

    {
        size_t x = 0;
        for (; x < vec_w; x += 16) {
            uint8x16_t vcur = vld1q_u8(buf + x);
            uint8x16_t vprev = x ? vld1q_u8(buf + x - 1) : vextq_u8(vzero, vcur, 15);
            uint8x16_t vnext = vld1q_u8(buf + x + 1);
            uint16x8_t h_lo = vaddq_u16(
                vaddl_u8(vget_low_u8(vcur), vget_low_u8(vcur)),
                vaddl_u8(vget_low_u8(vprev), vget_low_u8(vnext)));
            uint16x8_t h_hi = vaddq_u16(
                vaddl_u8(vget_high_u8(vcur), vget_high_u8(vcur)),
                vaddl_u8(vget_high_u8(vprev), vget_high_u8(vnext)));
            vst1q_u16(col_pix_buf + x, h_lo);
            vst1q_u16(col_sum_buf + x, h_lo);
            vst1q_u16(col_pix_buf + x + 8, h_hi);
            vst1q_u16(col_sum_buf + x + 8, h_hi);
        }
        uint16_t left = x ? buf[x - 1] : 0;
        for (; x + 1 < width; x++) {
            uint16_t cur = buf[x];
            uint16_t h = left + (cur << 1) + buf[x + 1];
            col_pix_buf[x] = col_sum_buf[x] = h;
            left = cur;
        }
        uint16_t h = left + ((uint16_t) buf[width - 1] << 1);
        col_pix_buf[width - 1] = col_sum_buf[width - 1] = h;
    }

    for (size_t y = 1; y < height; y++) {
        uint8_t *dst = buf;
        buf += stride;

        size_t x = 0;
        for (; x < vec_w; x += 16) {
            uint8x16_t vcur = vld1q_u8(buf + x);
            uint8x16_t vprev = x ? vld1q_u8(buf + x - 1) : vextq_u8(vzero, vcur, 15);
            uint8x16_t vnext = vld1q_u8(buf + x + 1);
            uint16x8_t h_lo = vaddq_u16(
                vaddl_u8(vget_low_u8(vcur), vget_low_u8(vcur)),
                vaddl_u8(vget_low_u8(vprev), vget_low_u8(vnext)));
            uint16x8_t h_hi = vaddq_u16(
                vaddl_u8(vget_high_u8(vcur), vget_high_u8(vcur)),
                vaddl_u8(vget_high_u8(vprev), vget_high_u8(vnext)));

            uint16x8_t pp_lo = vld1q_u16(col_pix_buf + x);
            uint16x8_t pp_hi = vld1q_u16(col_pix_buf + x + 8);
            vst1q_u16(col_pix_buf + x, h_lo);
            vst1q_u16(col_pix_buf + x + 8, h_hi);

            uint16x8_t cs_lo = vaddq_u16(pp_lo, h_lo);
            uint16x8_t cs_hi = vaddq_u16(pp_hi, h_hi);
            uint16x8_t ps_lo = vld1q_u16(col_sum_buf + x);
            uint16x8_t ps_hi = vld1q_u16(col_sum_buf + x + 8);
            vst1q_u16(col_sum_buf + x, cs_lo);
            vst1q_u16(col_sum_buf + x + 8, cs_hi);

            uint8x8_t out_lo = vshrn_n_u16(vaddq_u16(ps_lo, cs_lo), 4);
            uint8x8_t out_hi = vshrn_n_u16(vaddq_u16(ps_hi, cs_hi), 4);
            vst1q_u8(dst + x, vcombine_u8(out_lo, out_hi));
        }
        uint16_t left = x ? buf[x - 1] : 0;
        for (; x + 1 < width; x++) {
            uint16_t cur = buf[x];
            uint16_t col_pix = left + (cur << 1) + buf[x + 1];
            uint16_t col_sum = sliding_sum(&col_pix_buf[x], col_pix);
            dst[x] = sliding_sum(&col_sum_buf[x], col_sum) >> 4;
            left = cur;
        }
        uint16_t col_pix = left + ((uint16_t) buf[width - 1] << 1);
        uint16_t col_sum = sliding_sum(&col_pix_buf[width - 1], col_pix);
        dst[width - 1] = sliding_sum(&col_sum_buf[width - 1], col_sum) >> 4;
    }

    size_t x = 0;
    for (; x + 16 <= width; x += 16) {
        uint8x8_t out_lo = vshrn_n_u16(
            vaddq_u16(vld1q_u16(col_sum_buf + x), vld1q_u16(col_pix_buf + x)), 4);
        uint8x8_t out_hi = vshrn_n_u16(
            vaddq_u16(vld1q_u16(col_sum_buf + x + 8), vld1q_u16(col_pix_buf + x + 8)), 4);
        vst1q_u8(buf + x, vcombine_u8(out_lo, out_hi));
    }
    for (; x < width; x++)
        buf[x] = (col_sum_buf[x] + col_pix_buf[x]) >> 4;
}
#endif
