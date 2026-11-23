/*
 * Copyright (C) 2015 Vabishchevich Nikolay <vabnick@gmail.com>
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

#include <math.h>
#include <stdbool.h>
#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

#include "ass_utils.h"
#include "ass_bitmap.h"
#include "ass_render.h"


/*
 * Cascade Blur Algorithm
 *
 * The main idea is simple: to approximate a gaussian blur with large radius,
 * you can scale down, apply a filter with a relatively small pattern, then scale back up.
 *
 * To achieve the desired precision, scaling should be done with sufficiently smooth kernel.
 * Experiments show that downscaling of factor 2 with kernel [1, 5, 10, 10, 5, 1] and
 * corresponding upscaling are enough for 8-bit precision.
 *
 * Here we use generic filters with 5 different kernel widths (9 to 17-tap).
 * Kernel coefficients of that filter are obtained from the solution of the least-squares problem
 * for the Fourier transform of the resulting kernel.
 */

static void calc_gauss(double *res, int n, double r2)
{
    double alpha = 0.5 / r2;
    double mul = exp(-alpha), mul2 = mul * mul;
    double cur = sqrt(alpha / ASS_PI);

    res[0] = cur;
    cur *= mul;
    res[1] = cur;
    for (int i = 2; i < n; i++) {
        mul *= mul2;
        cur *= mul;
        res[i] = cur;
    }
}

static void coeff_filter(double *coeff, int n, const double kernel[4])
{
    double prev1 = coeff[1], prev2 = coeff[2], prev3 = coeff[3];
    for (int i = 0; i < n; i++) {
        double res = coeff[i + 0]  * kernel[0] +
            (prev1 + coeff[i + 1]) * kernel[1] +
            (prev2 + coeff[i + 2]) * kernel[2] +
            (prev3 + coeff[i + 3]) * kernel[3];
        prev3 = prev2;
        prev2 = prev1;
        prev1 = coeff[i];
        coeff[i] = res;
    }
}

static void calc_matrix(double mat[][8], const double *mat_freq, int n)
{
    for (int i = 0; i < n; i++) {
        mat[i][i] = mat_freq[2 * i + 2] + 3 * mat_freq[0] - 4 * mat_freq[i + 1];
        for (int j = i + 1; j < n; j++)
            mat[i][j] = mat[j][i] = mat_freq[i + j + 2] + mat_freq[j - i] +
                2 * (mat_freq[0] - mat_freq[i + 1] - mat_freq[j + 1]);
    }

    // invert transpose
    for (int k = 0; k < n; k++) {
        double z = 1 / mat[k][k];
        mat[k][k] = 1;
        for (int i = 0; i < n; i++) {
            if (i == k)
                continue;

            double mul = mat[i][k] * z;
            mat[i][k] = 0;
            for (int j = 0; j < n; j++)
                mat[i][j] -= mat[k][j] * mul;
        }
        for (int j = 0; j < n; j++)
            mat[k][j] *= z;
    }
}

/**
 * \brief Solve least squares problem for kernel of the main filter
 * \param mu out: output coefficients
 * \param n in: filter kernel radius
 * \param r2 in: desired standard deviation squared
 * \param mul in: scale multiplier
 */
static void calc_coeff(double mu[], int n, double r2, double mul)
{
    assert(n > 0 && n <= 8);

    const double w = 12096;
    double kernel[] = {
        ((( + 3280 / w) * mul + 1092 / w) * mul + 2520 / w) * mul + 5204 / w,
        ((( - 2460 / w) * mul -  273 / w) * mul -  210 / w) * mul + 2943 / w,
        ((( +  984 / w) * mul -  546 / w) * mul -  924 / w) * mul +  486 / w,
        ((( -  164 / w) * mul +  273 / w) * mul -  126 / w) * mul +   17 / w,
    };

    double mat_freq[17] = { kernel[0], kernel[1], kernel[2], kernel[3] };
    coeff_filter(mat_freq, 7, kernel);

    double vec_freq[12];
    calc_gauss(vec_freq, n + 4, r2 * mul);
    coeff_filter(vec_freq, n + 1, kernel);

    double mat[8][8];
    calc_matrix(mat, mat_freq, n);

    double vec[8];
    for (int i = 0; i < n; i++)
        vec[i] = mat_freq[0] - mat_freq[i + 1] - vec_freq[0] + vec_freq[i + 1];

    for (int i = 0; i < n; i++) {
        double res = 0;
        for (int j = 0; j < n; j++)
            res += mat[i][j] * vec[j];
        mu[i] = FFMAX(0, res);
    }
}

// Select the cascade geometry for r2 >= 0.5 without computing coefficients.
static double find_method_geometry(BlurMethod *blur, double r2, bool fast_blur)
{
    double frac = frexp(sqrt(0.11569 * r2 + 0.20591047), &blur->level);
    if (fast_blur) {
        if (blur->level < 6 && r2 * pow(0.25, blur->level + 1) >= 0.85)
            blur->level++;
        blur->radius = 4;
        return pow(0.25, blur->level);
    }
    double mul = pow(0.25, blur->level);
    blur->radius = 8 - (int) ((10.1525 + 0.8335 * mul) * (1 - frac));
    blur->radius = FFMAX(blur->radius, 4);
    return mul;
}

static void find_best_method(BlurMethod *blur, double r2, bool fast_blur)
{
    *blur = (BlurMethod) {0};
    double mu[8];
    if (r2 < 0.5) {
        blur->level = 0;
        blur->radius = 4;
        mu[1] = 0.085 * r2 * r2 * r2;
        mu[0] = 0.5 * r2 - 4 * mu[1];
        mu[2] = mu[3] = 0;
    } else {
        double mul = find_method_geometry(blur, r2, fast_blur);
        calc_coeff(mu, blur->radius, r2, mul);
    }
    for (int i = 0; i < blur->radius; i++)
        blur->coeff[i] = (int) (0x10000 * mu[i] + 0.5);
}

static void get_blur_method(ASS_Renderer *renderer, BlurMethod *blur, double r2)
{
    const unsigned count = sizeof(renderer->blur_methods) /
        sizeof(renderer->blur_methods[0]);
    for (unsigned i = 0; i < count; i++) {
        const BlurMethodCacheEntry *entry = &renderer->blur_methods[i];
        if (entry->valid && entry->r2 == r2) {
            *blur = entry->method;
            return;
        }
    }

    find_best_method(blur, r2, renderer->experimental_edges >= 2);
    // Exact keys preserve the computed coefficients; evict in round-robin order.
    renderer->blur_methods[renderer->blur_method_next] = (BlurMethodCacheEntry) {
        .r2 = r2,
        .method = *blur,
        .valid = true,
    };
    renderer->blur_method_next = (renderer->blur_method_next + 1) % count;
}

/**
 * \brief Perform approximate gaussian blur
 * \param r2x in: desired standard deviation along X axis squared
 * \param r2y in: desired standard deviation along Y axis squared
 */
void ass_gaussian_blur_bounds(double r2, bool fast_blur, int *padding, int *alignment)
{
    BlurMethod blur;
    if (r2 < 0.5) {
        blur.level = 0;
        blur.radius = 4;
    } else {
        find_method_geometry(&blur, r2, fast_blur);
    }
    // The shrink/expand cascade contributes at most 5 * (2^level - 1)
    // source pixels per side, in addition to the scaled convolution radius.
    // Keep a two-pixel guard at the integer bitmap/clip boundaries.
    *padding = ((blur.radius + 5) << blur.level) - 3;
    *alignment = 1 << blur.level;
}

static bool blur_buffer_fits(uint64_t w, uint64_t h, int stripe_width,
                             uint64_t capacity)
{
    uint64_t stride = (w + stripe_width - 1) & ~(uint64_t) (stripe_width - 1);
    return stride && h <= capacity / sizeof(int16_t) / stride;
}

static bool can_reuse_blur_output(const BlurMethod *blur_x, const BlurMethod *blur_y,
                                  uint32_t w, uint32_t h, uint32_t end_w, uint32_t end_h,
                                  int stripe_width, uint64_t capacity)
{
    const BlurMethod *blur[2] = {blur_x, blur_y};
    uint64_t dim[2] = {w, h};
    int index = 0;

    // Check every odd pass, including rounded tail lanes, before allocating output.
    for (int axis = 1; axis >= 0; axis--)
        for (int i = 0; i < blur[axis]->level; i++) {
            dim[axis] = (dim[axis] + 5) >> 1;
            index ^= 1;
            if (index && !blur_buffer_fits(dim[0], dim[1], stripe_width, capacity))
                return false;
        }
    for (int axis = 0; axis < 2; axis++) {
        dim[axis] += 2 * blur[axis]->radius;
        index ^= 1;
        if (index && !blur_buffer_fits(dim[0], dim[1], stripe_width, capacity))
            return false;
    }
    for (int axis = 0; axis < 2; axis++)
        for (int i = 0; i < blur[axis]->level; i++) {
            dim[axis] = 2 * dim[axis] + 4;
            index ^= 1;
            if (index && !blur_buffer_fits(dim[0], dim[1], stripe_width, capacity))
                return false;
        }
    return !index && dim[0] == end_w && dim[1] == end_h;
}

#if (CONFIG_ASM && ARCH_AARCH64) || (!ARCH_AARCH64 && defined(__arm__) && defined(__ARM_NEON))
ParamFilterFunc ass_blur4_vert16_neon, ass_blur5_vert16_neon,
    ass_blur6_vert16_neon, ass_blur7_vert16_neon, ass_blur8_vert16_neon;

static bool blur_row_zero(const int16_t *row)
{
    uint64_t bits[2];
    memcpy(bits, row, sizeof(bits));
    return !(bits[0] | bits[1]);
}
#endif

bool ass_trim_blur_vertical(ParamFilterFunc *filter, int16_t *dst,
                             const int16_t *src, size_t width, size_t src_h,
                             size_t dst_h, const int16_t *coeff)
{
#if (CONFIG_ASM && ARCH_AARCH64) || (!ARCH_AARCH64 && defined(__arm__) && defined(__ARM_NEON))
    ParamFilterFunc *const kernels[] = {ass_blur4_vert16_neon, ass_blur5_vert16_neon,
        ass_blur6_vert16_neon, ass_blur7_vert16_neon, ass_blur8_vert16_neon};
    int radius = 0;
    for (int i = 0; i < 5; i++)
        if (filter == kernels[i]) radius = i + 4;
    if (!radius || !dst || !src || !coeff || !width || !src_h ||
        dst_h < src_h || dst_h - src_h != 2 * (size_t) radius ||
        (((uintptr_t) dst | (uintptr_t) src) & 15) || width > SIZE_MAX - 7)
        return false;
    size_t rounded_width = (width + 7) & ~(size_t) 7;
    if (src_h > PTRDIFF_MAX / sizeof(*src) / rounded_width ||
        dst_h > PTRDIFF_MAX / sizeof(*dst) / rounded_width)
        return false;
    size_t src_bytes = rounded_width * src_h * sizeof(*src);
    size_t dst_bytes = rounded_width * dst_h * sizeof(*dst);
    uintptr_t s = (uintptr_t) src, d = (uintptr_t) dst;
    if (d <= s ? s - d < dst_bytes : d - s < src_bytes)
        return false;

    for (size_t x = 0; x < rounded_width; x += 8) {
        const int16_t *input = src + x * src_h;
        int16_t *output = dst + x * dst_h;
        size_t first = 0, end = src_h;
        while (first < end && blur_row_zero(input + 8 * first)) first++;
        if (first == end) {
            memset(output, 0, dst_h * 16);
            continue;
        }
        while (end > first + 1 && blur_row_zero(input + 8 * (end - 1))) end--;
        if (first) memset(output, 0, first * 16);
        // The kernel maps output y to input [y - 2r, y]; offset both origins.
        filter(output + 8 * first, input + 8 * first, 8, end - first, coeff);
        if (end < src_h) {
            size_t tail = src_h - end;
            memset(output + 8 * (dst_h - tail), 0, tail * 16);
        }
    }
    return true;
#else
    (void) filter; (void) dst; (void) src; (void) width;
    (void) src_h; (void) dst_h; (void) coeff;
    return false;
#endif
}

typedef struct {
    Convert8to16Func *fn;
    int16_t *dst;
    const uint8_t *src;
    ptrdiff_t src_stride;
    size_t width, height, align;
} UnpackSliceCtx;

static void unpack_slice_worker(void *arg, unsigned lane, unsigned lanes)
{
    const UnpackSliceCtx *c = arg;
    size_t units = (c->width + c->align - 1) / c->align;
    size_t begin = units / lanes * lane + units % lanes * lane / lanes;
    size_t end = units / lanes * (lane + 1) + units % lanes * (lane + 1) / lanes;
    if (begin == end)
        return;
    size_t x = begin * c->align;
    size_t w = end == units ? c->width - x : (end - begin) * c->align;
    c->fn(c->dst + x * c->height, c->src + x, c->src_stride, w, c->height);
}

#if defined(__ARM_NEON)
static const int16_t blur_slice_zero[8] = {0};
static const int16_t blur_slice_dither[16] = {
     8, 40,  8, 40,  8, 40,  8, 40,
    56, 24, 56, 24, 56, 24, 56, 24,
};

static inline const int16_t *blur_slice_line(const int16_t *ptr, size_t offs, size_t size)
{
    return offs < size ? ptr + offs : blur_slice_zero;
}

static inline bool blur_neon_vec_is_zero(uint8x16_t v)
{
    uint32x4_t u = vreinterpretq_u32_u8(v);
    uint32x2_t o = vorr_u32(vget_low_u32(u), vget_high_u32(u));
    return (vget_lane_u32(o, 0) | vget_lane_u32(o, 1)) == 0;
}

static inline uint16x8_t blur_slice_shrink_vec(uint16x8_t p1p, uint16x8_t p1n,
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

static inline void blur_slice_expand_vec(uint16x8_t *rp, uint16x8_t *rn,
                                         uint16x8_t p1, uint16x8_t z0, uint16x8_t n1)
{
    uint16x8_t r = vhaddq_u16(vhaddq_u16(p1, n1), z0);
    *rp = vrhaddq_u16(vhaddq_u16(r, p1), z0);
    *rn = vrhaddq_u16(vhaddq_u16(r, n1), z0);
}

typedef struct {
    int16_t *dst;
    const int16_t *src;
    size_t src_w, src_h;
} HorzScaleSliceCtx;

static void shrink_horz_slice_worker(void *arg, unsigned lane, unsigned lanes)
{
    const HorzScaleSliceCtx *c = arg;
    size_t dst_w = (c->src_w + 5) >> 1;
    size_t dst_stripes = (dst_w + 7) >> 3;
    size_t begin = dst_stripes / lanes * lane + dst_stripes % lanes * lane / lanes;
    size_t end = dst_stripes / lanes * (lane + 1) + dst_stripes % lanes * (lane + 1) / lanes;
    if (begin == end)
        return;
    size_t size = ((c->src_w + 7) & ~(size_t) 7) * c->src_h;
    size_t step = 8 * c->src_h;
    int16_t *dst = c->dst + begin * step;
    size_t offs = (2 * begin) * step;
    for (size_t sx = begin; sx < end; sx++) {
        for (size_t y = 0; y < c->src_h; y++) {
            uint16x8_t v0 = vreinterpretq_u16_s16(vld1q_s16(blur_slice_line(c->src, offs - step, size)));
            uint16x8_t v1 = vreinterpretq_u16_s16(vld1q_s16(blur_slice_line(c->src, offs, size)));
            uint16x8_t v2 = vreinterpretq_u16_s16(vld1q_s16(blur_slice_line(c->src, offs + step, size)));
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
            vst1q_s16(dst, vreinterpretq_s16_u16(blur_slice_shrink_vec(p1p, p1n, z0p, z0n, n1p, n1n)));
            dst  += 8;
            offs += 8;
        }
        offs += step;
    }
}

static void expand_horz_slice_worker(void *arg, unsigned lane, unsigned lanes)
{
    const HorzScaleSliceCtx *c = arg;
    size_t dst_w = 2 * c->src_w + 4;
    size_t n_pairs = dst_w > 8 ? (dst_w + 7) >> 4 : 0;
    bool has_tail = !((dst_w - 1) & 8);
    size_t total = n_pairs + (has_tail ? 1 : 0);
    size_t begin = total / lanes * lane + total % lanes * lane / lanes;
    size_t end = total / lanes * (lane + 1) + total % lanes * (lane + 1) / lanes;
    if (begin == end)
        return;
    size_t size = ((c->src_w + 7) & ~(size_t) 7) * c->src_h;
    size_t step = 8 * c->src_h;
    size_t pair_end = end < n_pairs ? end : n_pairs;
    for (size_t p = begin; p < pair_end; p++) {
        int16_t *dst = c->dst + (2 * p) * step;
        size_t offs = p * step;
        for (size_t y = 0; y < c->src_h; y++) {
            uint16x8_t v0 = vreinterpretq_u16_s16(vld1q_s16(blur_slice_line(c->src, offs - step, size)));
            uint16x8_t v1 = vreinterpretq_u16_s16(vld1q_s16(blur_slice_line(c->src, offs, size)));
            uint16x8_t p1 = vextq_u16(v0, v1, 6);
            uint16x8_t z0 = vextq_u16(v0, v1, 7);
            uint16x8_t rp, rn;
            blur_slice_expand_vec(&rp, &rn, p1, z0, v1);
            uint16x8x2_t z = vzipq_u16(rp, rn);
            vst1q_s16(dst, vreinterpretq_s16_u16(z.val[0]));
            vst1q_s16(dst + step, vreinterpretq_s16_u16(z.val[1]));
            dst  += 8;
            offs += 8;
        }
    }
    if (has_tail && end == total) {
        int16_t *dst = c->dst + (2 * n_pairs) * step;
        size_t offs = n_pairs * step;
        for (size_t y = 0; y < c->src_h; y++) {
            uint16x8_t v0 = vreinterpretq_u16_s16(vld1q_s16(blur_slice_line(c->src, offs - step, size)));
            uint16x8_t v1 = vreinterpretq_u16_s16(vld1q_s16(blur_slice_line(c->src, offs, size)));
            uint16x8_t p1 = vextq_u16(v0, v1, 6);
            uint16x8_t z0 = vextq_u16(v0, v1, 7);
            uint16x8_t rp, rn;
            blur_slice_expand_vec(&rp, &rn, p1, z0, v1);
            uint16x8x2_t z = vzipq_u16(rp, rn);
            vst1q_s16(dst, vreinterpretq_s16_u16(z.val[0]));
            dst  += 8;
            offs += 8;
        }
    }
}

#define SLICE_BLUR_TAP_LANE(acc_lo, acc_hi, vc, v1, v2, pvec, lane) \
    do { \
        int16x8_t d1_ = vsubq_s16((v1), (vc)); \
        int16x8_t d2_ = vsubq_s16((v2), (vc)); \
        (acc_lo) = vmlal_lane_s16((acc_lo), vget_low_s16(d1_),  (pvec), (lane)); \
        (acc_hi) = vmlal_lane_s16((acc_hi), vget_high_s16(d1_), (pvec), (lane)); \
        (acc_lo) = vmlal_lane_s16((acc_lo), vget_low_s16(d2_),  (pvec), (lane)); \
        (acc_hi) = vmlal_lane_s16((acc_hi), vget_high_s16(d2_), (pvec), (lane)); \
    } while (0)

#define SLICE_BLUR_HORZ_STEP(N, NEED_PREV2, CHECK_UNIFORM, TAPS_EXPR) \
    do { \
        if (CHECK_UNIFORM) { \
            int16x8_t vd_ = vdupq_lane_s16(vget_low_s16(s1), 0); \
            uint8x16_t diff_ = vorrq_u8(vreinterpretq_u8_s16(veorq_s16(s1, vd_)), \
                                        vreinterpretq_u8_s16(veorq_s16(s0, vd_))); \
            if (NEED_PREV2) \
                diff_ = vorrq_u8(diff_, vreinterpretq_u8_s16(veorq_s16(s2, vd_))); \
            if (blur_neon_vec_is_zero(diff_)) { \
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

typedef struct {
    int16_t *dst;
    const int16_t *src;
    const int16_t *coeff;
    size_t src_w, src_h;
    int radius;
    bool use_uniform;
} HorzBlurSliceCtx;

#define DEFINE_HORZ_BLUR_SLICE(N, NEED_PREV2, TAPS_EXPR) \
static void blur ## N ## _horz_slice_run(const HorzBlurSliceCtx *c, size_t begin, size_t end) \
{ \
    size_t size = ((c->src_w + 7) & ~(size_t) 7) * c->src_h; \
    size_t step = 8 * c->src_h; \
    size_t src_stripes = (c->src_w + 7) >> 3; \
    size_t interior_start = NEED_PREV2 ? 2 : 1; \
    int16x4_t p0 = vld1_s16(c->coeff); \
    int16x4_t p1 = NEED_PREV2 ? vld1_s16(c->coeff + 4) : p0; \
    (void) p0; (void) p1; \
    int32x4_t vhalf = vdupq_n_s32(0x8000); \
    int16_t *dst = c->dst + begin * step; \
    size_t offs = begin * step; \
    for (size_t sx = begin; sx < end; sx++) { \
        bool interior = (sx >= interior_start && sx < src_stripes); \
        if (interior && c->use_uniform) { \
            const int16_t *p_s0 = c->src + offs; \
            const int16_t *p_s1 = p_s0 - step; \
            const int16_t *p_s2 = NEED_PREV2 ? (p_s1 - step) : p_s1; \
            for (size_t y = 0; y < c->src_h; y++) { \
                int16x8_t s0 = vld1q_s16(p_s0); \
                int16x8_t s1 = vld1q_s16(p_s1); \
                int16x8_t s2 = NEED_PREV2 ? vld1q_s16(p_s2) : s1; \
                (void) s2; \
                SLICE_BLUR_HORZ_STEP(N, NEED_PREV2, 1, TAPS_EXPR); \
                dst += 8; p_s0 += 8; p_s1 += 8; p_s2 += 8; \
            } \
            offs += step; \
        } else if (interior) { \
            const int16_t *p_s0 = c->src + offs; \
            const int16_t *p_s1 = p_s0 - step; \
            const int16_t *p_s2 = NEED_PREV2 ? (p_s1 - step) : p_s1; \
            for (size_t y = 0; y < c->src_h; y++) { \
                int16x8_t s0 = vld1q_s16(p_s0); \
                int16x8_t s1 = vld1q_s16(p_s1); \
                int16x8_t s2 = NEED_PREV2 ? vld1q_s16(p_s2) : s1; \
                (void) s2; \
                SLICE_BLUR_HORZ_STEP(N, NEED_PREV2, 0, TAPS_EXPR); \
                dst += 8; p_s0 += 8; p_s1 += 8; p_s2 += 8; \
            } \
            offs += step; \
        } else { \
            for (size_t y = 0; y < c->src_h; y++) { \
                int16x8_t s0 = vld1q_s16(blur_slice_line(c->src, offs, size)); \
                int16x8_t s1 = vld1q_s16(blur_slice_line(c->src, offs - step, size)); \
                int16x8_t s2 = NEED_PREV2 ? vld1q_s16(blur_slice_line(c->src, offs - 2 * step, size)) : s1; \
                (void) s2; \
                SLICE_BLUR_HORZ_STEP(N, NEED_PREV2, c->use_uniform, TAPS_EXPR); \
                dst += 8; offs += 8; \
            } \
        } \
    } \
}

DEFINE_HORZ_BLUR_SLICE(4, 0,
    SLICE_BLUR_TAP_LANE(acc_lo, acc_hi, vc, s1,                 s0,                 p0, 3);
    SLICE_BLUR_TAP_LANE(acc_lo, acc_hi, vc, vextq_s16(s1,s0,1), vextq_s16(s1,s0,7), p0, 2);
    SLICE_BLUR_TAP_LANE(acc_lo, acc_hi, vc, vextq_s16(s1,s0,2), vextq_s16(s1,s0,6), p0, 1);
    SLICE_BLUR_TAP_LANE(acc_lo, acc_hi, vc, vextq_s16(s1,s0,3), vextq_s16(s1,s0,5), p0, 0);
)
DEFINE_HORZ_BLUR_SLICE(5, 1,
    SLICE_BLUR_TAP_LANE(acc_lo, acc_hi, vc, vextq_s16(s2,s1,6), s0,                 p1, 0);
    SLICE_BLUR_TAP_LANE(acc_lo, acc_hi, vc, vextq_s16(s2,s1,7), vextq_s16(s1,s0,7), p0, 3);
    SLICE_BLUR_TAP_LANE(acc_lo, acc_hi, vc, s1,                 vextq_s16(s1,s0,6), p0, 2);
    SLICE_BLUR_TAP_LANE(acc_lo, acc_hi, vc, vextq_s16(s1,s0,1), vextq_s16(s1,s0,5), p0, 1);
    SLICE_BLUR_TAP_LANE(acc_lo, acc_hi, vc, vextq_s16(s1,s0,2), vextq_s16(s1,s0,4), p0, 0);
)
DEFINE_HORZ_BLUR_SLICE(6, 1,
    SLICE_BLUR_TAP_LANE(acc_lo, acc_hi, vc, vextq_s16(s2,s1,4), s0,                 p1, 1);
    SLICE_BLUR_TAP_LANE(acc_lo, acc_hi, vc, vextq_s16(s2,s1,5), vextq_s16(s1,s0,7), p1, 0);
    SLICE_BLUR_TAP_LANE(acc_lo, acc_hi, vc, vextq_s16(s2,s1,6), vextq_s16(s1,s0,6), p0, 3);
    SLICE_BLUR_TAP_LANE(acc_lo, acc_hi, vc, vextq_s16(s2,s1,7), vextq_s16(s1,s0,5), p0, 2);
    SLICE_BLUR_TAP_LANE(acc_lo, acc_hi, vc, s1,                 vextq_s16(s1,s0,4), p0, 1);
    SLICE_BLUR_TAP_LANE(acc_lo, acc_hi, vc, vextq_s16(s1,s0,1), vextq_s16(s1,s0,3), p0, 0);
)
DEFINE_HORZ_BLUR_SLICE(7, 1,
    SLICE_BLUR_TAP_LANE(acc_lo, acc_hi, vc, vextq_s16(s2,s1,2), s0,                 p1, 2);
    SLICE_BLUR_TAP_LANE(acc_lo, acc_hi, vc, vextq_s16(s2,s1,3), vextq_s16(s1,s0,7), p1, 1);
    SLICE_BLUR_TAP_LANE(acc_lo, acc_hi, vc, vextq_s16(s2,s1,4), vextq_s16(s1,s0,6), p1, 0);
    SLICE_BLUR_TAP_LANE(acc_lo, acc_hi, vc, vextq_s16(s2,s1,5), vextq_s16(s1,s0,5), p0, 3);
    SLICE_BLUR_TAP_LANE(acc_lo, acc_hi, vc, vextq_s16(s2,s1,6), vextq_s16(s1,s0,4), p0, 2);
    SLICE_BLUR_TAP_LANE(acc_lo, acc_hi, vc, vextq_s16(s2,s1,7), vextq_s16(s1,s0,3), p0, 1);
    SLICE_BLUR_TAP_LANE(acc_lo, acc_hi, vc, s1,                 vextq_s16(s1,s0,2), p0, 0);
)
DEFINE_HORZ_BLUR_SLICE(8, 1,
    SLICE_BLUR_TAP_LANE(acc_lo, acc_hi, vc, s2,                 s0,                 p1, 3);
    SLICE_BLUR_TAP_LANE(acc_lo, acc_hi, vc, vextq_s16(s2,s1,1), vextq_s16(s1,s0,7), p1, 2);
    SLICE_BLUR_TAP_LANE(acc_lo, acc_hi, vc, vextq_s16(s2,s1,2), vextq_s16(s1,s0,6), p1, 1);
    SLICE_BLUR_TAP_LANE(acc_lo, acc_hi, vc, vextq_s16(s2,s1,3), vextq_s16(s1,s0,5), p1, 0);
    SLICE_BLUR_TAP_LANE(acc_lo, acc_hi, vc, vextq_s16(s2,s1,4), vextq_s16(s1,s0,4), p0, 3);
    SLICE_BLUR_TAP_LANE(acc_lo, acc_hi, vc, vextq_s16(s2,s1,5), vextq_s16(s1,s0,3), p0, 2);
    SLICE_BLUR_TAP_LANE(acc_lo, acc_hi, vc, vextq_s16(s2,s1,6), vextq_s16(s1,s0,2), p0, 1);
    SLICE_BLUR_TAP_LANE(acc_lo, acc_hi, vc, vextq_s16(s2,s1,7), vextq_s16(s1,s0,1), p0, 0);
)

static bool blur_slice_probe_uniform(const int16_t *src, size_t src_w, size_t src_h, bool need_prev2)
{
    if (src_w < 128 || src_h < 32)
        return false;
    size_t stripes = (src_w + 7) >> 3;
    size_t step = 8 * src_h;
    size_t sx_step = stripes >> 2;
    size_t sy_step = src_h >> 2;
    size_t sx0 = stripes >> 3;
    size_t sy0 = src_h >> 3;
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
            if (blur_neon_vec_is_zero(diff))
                uniform++;
        }
    }
    return uniform >= 4;
}

static void blur_horz_slice_worker(void *arg, unsigned lane, unsigned lanes)
{
    const HorzBlurSliceCtx *c = arg;
    size_t dst_w = c->src_w + 2 * (size_t) c->radius;
    size_t dst_stripes = (dst_w + 7) >> 3;
    size_t begin = dst_stripes / lanes * lane + dst_stripes % lanes * lane / lanes;
    size_t end = dst_stripes / lanes * (lane + 1) + dst_stripes % lanes * (lane + 1) / lanes;
    if (begin == end)
        return;
    switch (c->radius) {
    case 4: blur4_horz_slice_run(c, begin, end); break;
    case 5: blur5_horz_slice_run(c, begin, end); break;
    case 6: blur6_horz_slice_run(c, begin, end); break;
    case 7: blur7_horz_slice_run(c, begin, end); break;
    case 8: blur8_horz_slice_run(c, begin, end); break;
    }
}

typedef struct {
    uint8_t *dst;
    ptrdiff_t dst_stride;
    const int16_t *src;
    size_t width, height;
} PackSliceCtx;

static void pack_slice_worker(void *arg, unsigned lane, unsigned lanes)
{
    const PackSliceCtx *c = arg;
    size_t pairs = (c->width + 15) >> 4;
    size_t begin = pairs / lanes * lane + pairs % lanes * lane / lanes;
    size_t end = pairs / lanes * (lane + 1) + pairs % lanes * (lane + 1) / lanes;
    if (begin == end)
        return;
    size_t x0 = begin * 16;
    size_t x1 = end == pairs ? c->width : end * 16;
    size_t step = 8 * c->height;
    int16x8_t vd0 = vld1q_s16(blur_slice_dither);
    int16x8_t vd1 = vld1q_s16(blur_slice_dither + 8);
    uint8_t *dst = c->dst + x0;
    const int16_t *src = c->src + x0 * c->height;
    size_t x = x0;
    for (; x + 8 < x1; x += 16) {
        uint8_t *ptr = dst;
        const int16_t *src2 = src + step;
        size_t y = 0;
        for (; y + 1 < c->height; y += 2) {
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
            vst1q_u8(ptr + c->dst_stride, vcombine_u8(vshrn_n_u16(vreinterpretq_u16_s16(ta1), 6),
                                                      vshrn_n_u16(vreinterpretq_u16_s16(tb1), 6)));
            ptr += 2 * c->dst_stride;
            src += 16;
            src2 += 16;
        }
        if (y < c->height) {
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
    if (x < x1) {
        uint8_t *ptr = dst;
        size_t y = 0;
        for (; y + 1 < c->height; y += 2) {
            int16x8_t s0 = vld1q_s16(src);
            int16x8_t t0 = vaddq_s16(vsubq_s16(s0, vshrq_n_s16(s0, 8)), vd0);
            vst1_u8(ptr, vshrn_n_u16(vreinterpretq_u16_s16(t0), 6));
            int16x8_t s1 = vld1q_s16(src + 8);
            int16x8_t t1 = vaddq_s16(vsubq_s16(s1, vshrq_n_s16(s1, 8)), vd1);
            vst1_u8(ptr + c->dst_stride, vshrn_n_u16(vreinterpretq_u16_s16(t1), 6));
            ptr += 2 * c->dst_stride;
            src += 16;
        }
        if (y < c->height) {
            int16x8_t s0 = vld1q_s16(src);
            int16x8_t t0 = vaddq_s16(vsubq_s16(s0, vshrq_n_s16(s0, 8)), vd0);
            vst1_u8(ptr, vshrn_n_u16(vreinterpretq_u16_s16(t0), 6));
        }
        dst += 8;
    }
    if (end == pairs) {
        size_t left = (size_t) c->dst_stride - ((c->width + 7) & ~(size_t) 7);
        if (left) {
            for (size_t y = 0; y < c->height; y++) {
                memset(dst, 0, left);
                dst += c->dst_stride;
            }
        }
    }
}

#undef SLICE_BLUR_TAP_LANE
#undef SLICE_BLUR_HORZ_STEP
#undef DEFINE_HORZ_BLUR_SLICE
#endif

bool ass_gaussian_blur(ASS_Renderer *renderer, Bitmap *bm, const Bitmap *src,
                       double r2x, double r2y)
{
    const BitmapEngine *engine = &renderer->engine;
    if (renderer->experimental_edges >= 2 &&
        r2x >= 0.25 && r2x <= 1.75 && r2y >= 0.25 && r2y <= 1.75 &&
        fabs(r2x - r2y) <= 0.35 && src->w > 1 && src->h > 1) {
        int passes = (int) (r2x + r2y + 0.5);
        passes = FFMINMAX(passes, 1, 3);
        uint32_t out_w = src->w + 2 * (uint32_t) passes;
        uint32_t out_h = src->h + 2 * (uint32_t) passes;
        int src_left = src->left, src_top = src->top;
        Bitmap dst = {0};
        if (ass_alloc_bitmap(engine, &dst, out_w, out_h, true)) {
            for (uint32_t y = 0; y < src->h; y++)
                memcpy(dst.buffer + (y + passes) * dst.stride + passes,
                       src->buffer + y * src->stride, src->w);
            size_t tmp_bytes = 2 * (size_t) dst.stride * sizeof(uint16_t);
            bool use_scratch = tmp_bytes <= renderer->blur_scratch_size;
            uint16_t *be_tmp = use_scratch ? (uint16_t *) renderer->blur_scratch :
                ass_aligned_alloc(1U << engine->align_order, tmp_bytes, false);
            if (be_tmp) {
                for (int p = 0; p < passes; p++)
                    engine->be_blur(dst.buffer, dst.stride, out_w, out_h, be_tmp);
                if (!use_scratch)
                    ass_aligned_free(be_tmp);
                dst.left = src_left - passes;
                dst.top = src_top - passes;
                ass_free_bitmap(bm);
                *bm = dst;
                return true;
            }
            ass_free_bitmap(&dst);
        }
    }
    BlurMethod blur_x, blur_y;
    get_blur_method(renderer, &blur_x, r2x);
    if (r2y == r2x)
        memcpy(&blur_y, &blur_x, sizeof(blur_y));
    else get_blur_method(renderer, &blur_y, r2y);

    uint32_t w = src->w, h = src->h;
    int left = src->left, top = src->top;
    int offset_x = ((2 * blur_x.radius + 9) << blur_x.level) - 5;
    int offset_y = ((2 * blur_y.radius + 9) << blur_y.level) - 5;
    uint32_t end_w = ((w + offset_x) & ~((1 << blur_x.level) - 1)) - 4;
    uint32_t end_h = ((h + offset_y) & ~((1 << blur_y.level) - 1)) - 4;

    const int stripe_width = 1 << (engine->align_order - 1);
    uint64_t size = (((uint64_t) end_w + stripe_width - 1) & ~(stripe_width - 1)) * end_h;
    if (size > INT_MAX / 4)
        return false;

    const int alignment = 2 * stripe_width;
    uint64_t output_stride = ((uint64_t) end_w + alignment - 1) & ~(uint64_t) (alignment - 1);
    // ass_alloc_bitmap reserves an extra alignment unit after the last row.
    uint64_t output_capacity = output_stride * end_h + alignment;
    bool reuse_output = (blur_x.level || blur_y.level) &&
        can_reuse_blur_output(&blur_x, &blur_y, w, h, end_w, end_h,
                              stripe_width, output_capacity);
    size_t bytes = (reuse_output ? 2 : 4) * size;
    // Each event worker owns its renderer and scratch; never share this buffer.
    size_t scratch_limit = (renderer->experimental_edges ? 32 : 16) * 1024 * 1024;
    bool borrowed = bytes <= scratch_limit;
    if (borrowed && bytes > renderer->blur_scratch_size) {
        size_t capacity = ass_align(1024 * 1024, bytes);
        void *buffer = ass_aligned_alloc(2 * stripe_width, capacity, false);
        if (buffer) {
            ass_aligned_free(renderer->blur_scratch);
            renderer->blur_scratch = buffer;
            renderer->blur_scratch_size = capacity;
        } else {
            borrowed = false;
        }
    }
    int16_t *tmp = borrowed ? renderer->blur_scratch :
        ass_aligned_alloc(2 * stripe_width, bytes, false);
    if (!tmp)
        return false;

    {
        UnpackSliceCtx uctx = {engine->stripe_unpack, tmp, src->buffer, src->stride, w, h, (size_t) alignment};
        if (!ass_run_blur_slices(renderer, unpack_slice_worker, &uctx, (w + alignment - 1) / alignment, w, h))
            engine->stripe_unpack(tmp, src->buffer, src->stride, w, h);
    }
    // Unpack first: bm may alias src, whose allocation is freed by reallocation.
    if (reuse_output && !ass_realloc_bitmap(engine, bm, end_w, end_h)) {
        if (!borrowed) ass_aligned_free(tmp);
        return false;
    }
    int16_t *buf[2] = {tmp, reuse_output ? (int16_t *) bm->buffer : tmp + size};
    int index = 0;

    for (int i = 0; i < blur_y.level; i++) {
        if (!ass_run_vertical_filter(renderer, engine->shrink_vert, NULL,
                buf[index ^ 1], buf[index], w, h, (h + 5) >> 1, NULL))
            engine->shrink_vert(buf[index ^ 1], buf[index], w, h);
        h = (h + 5) >> 1;
        index ^= 1;
    }
    for (int i = 0; i < blur_x.level; i++) {
        bool done = false;
#if defined(__ARM_NEON)
        if (engine->align_order == 4) {
            HorzScaleSliceCtx hctx = {buf[index ^ 1], buf[index], w, h};
            size_t dst_stripes = (((w + 5) >> 1) + 7) >> 3;
            done = ass_run_blur_slices(renderer, shrink_horz_slice_worker, &hctx, dst_stripes, w, h);
        }
#endif
        if (!done)
            engine->shrink_horz(buf[index ^ 1], buf[index], w, h);
        w = (w + 5) >> 1;
        index ^= 1;
    }
    assert(blur_x.radius >= 4 && blur_x.radius <= 8);
    {
        bool done = false;
#if defined(__ARM_NEON)
        if (engine->align_order == 4) {
            HorzBlurSliceCtx bctx = {
                buf[index ^ 1], buf[index], blur_x.coeff, w, h, blur_x.radius,
                blur_slice_probe_uniform(buf[index], w, h, blur_x.radius > 4)
            };
            size_t dst_stripes = (w + 2 * (size_t) blur_x.radius + 7) >> 3;
            done = ass_run_blur_slices(renderer, blur_horz_slice_worker, &bctx, dst_stripes, w, h);
        }
#endif
        if (!done)
            engine->blur_horz[blur_x.radius - 4](buf[index ^ 1], buf[index], w, h, blur_x.coeff);
    }
    w += 2 * blur_x.radius;
    index ^= 1;
    assert(blur_y.radius >= 4 && blur_y.radius <= 8);
    bool large_vertical = w &&
        (w >= 16 * 1024 || h >= (16 * 1024 + w - 1) / w);
    if (!ass_run_vertical_filter(renderer, NULL, engine->blur_vert[blur_y.radius - 4],
            buf[index ^ 1], buf[index], w, h, h + 2 * blur_y.radius, blur_y.coeff) &&
        !(large_vertical && engine->align_order == 4 &&
          ass_trim_blur_vertical(engine->blur_vert[blur_y.radius - 4],
              buf[index ^ 1], buf[index], w, h, h + 2 * blur_y.radius, blur_y.coeff)))
        engine->blur_vert[blur_y.radius - 4](buf[index ^ 1], buf[index], w, h, blur_y.coeff);
    h += 2 * blur_y.radius;
    index ^= 1;
    for (int i = 0; i < blur_x.level; i++) {
        bool done = false;
#if defined(__ARM_NEON)
        if (engine->align_order == 4) {
            HorzScaleSliceCtx hctx = {buf[index ^ 1], buf[index], w, h};
            size_t dst_w = 2 * w + 4;
            size_t items = (dst_w > 8 ? (dst_w + 7) >> 4 : 0) + (!((dst_w - 1) & 8) ? 1 : 0);
            done = ass_run_blur_slices(renderer, expand_horz_slice_worker, &hctx, items, dst_w, h);
        }
#endif
        if (!done)
            engine->expand_horz(buf[index ^ 1], buf[index], w, h);
        w = 2 * w + 4;
        index ^= 1;
    }
    for (int i = 0; i < blur_y.level; i++) {
        if (!ass_run_vertical_filter(renderer, engine->expand_vert, NULL,
                buf[index ^ 1], buf[index], w, h, 2 * h + 4, NULL))
            engine->expand_vert(buf[index ^ 1], buf[index], w, h);
        h = 2 * h + 4;
        index ^= 1;
    }
    assert(w == end_w && h == end_h);
    // There are 2 + 2 * (level_x + level_y) passes; pack never aliases its source.
    assert(index == 0);

    if (!reuse_output && !ass_realloc_bitmap(engine, bm, w, h)) {
        if (!borrowed) ass_aligned_free(tmp);
        return false;
    }
    bm->left = left - (((blur_x.radius + 4) << blur_x.level) - 4);
    bm->top  = top - (((blur_y.radius + 4) << blur_y.level) - 4);

    {
        bool done = false;
#if defined(__ARM_NEON)
        if (engine->align_order == 4) {
            PackSliceCtx pctx = {bm->buffer, bm->stride, buf[index], w, h};
            done = ass_run_blur_slices(renderer, pack_slice_worker, &pctx, (w + 15) >> 4, w, h);
        }
#endif
        if (!done)
            engine->stripe_pack(bm->buffer, bm->stride, buf[index], w, h);
    }
    if (!borrowed) ass_aligned_free(tmp);
    return true;
}

