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
static double find_method_geometry(BlurMethod *blur, double r2)
{
    double frac = frexp(sqrt(0.11569 * r2 + 0.20591047), &blur->level);
    double mul = pow(0.25, blur->level);
    blur->radius = 8 - (int) ((10.1525 + 0.8335 * mul) * (1 - frac));
    blur->radius = FFMAX(blur->radius, 4);
    return mul;
}

static void find_best_method(BlurMethod *blur, double r2)
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
        double mul = find_method_geometry(blur, r2);
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

    find_best_method(blur, r2);
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
void ass_gaussian_blur_bounds(double r2, int *padding, int *alignment)
{
    BlurMethod blur;
    if (r2 < 0.5) {
        blur.level = 0;
        blur.radius = 4;
    } else {
        find_method_geometry(&blur, r2);
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

#if CONFIG_ASM && ARCH_AARCH64
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
#if CONFIG_ASM && ARCH_AARCH64
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

bool ass_gaussian_blur(ASS_Renderer *renderer, Bitmap *bm, const Bitmap *src,
                       double r2x, double r2y)
{
    const BitmapEngine *engine = &renderer->engine;
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
    size_t scratch_limit = (renderer->experimental_edges ? 32 : 8) * 1024 * 1024;
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

    engine->stripe_unpack(tmp, src->buffer, src->stride, w, h);
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
        engine->shrink_horz(buf[index ^ 1], buf[index], w, h);
        w = (w + 5) >> 1;
        index ^= 1;
    }
    assert(blur_x.radius >= 4 && blur_x.radius <= 8);
    engine->blur_horz[blur_x.radius - 4](buf[index ^ 1], buf[index], w, h, blur_x.coeff);
    w += 2 * blur_x.radius;
    index ^= 1;
    assert(blur_y.radius >= 4 && blur_y.radius <= 8);
    bool large_vertical = w &&
        (w >= 128 * 1024 || h >= (128 * 1024 + w - 1) / w);
    if (!ass_run_vertical_filter(renderer, NULL, engine->blur_vert[blur_y.radius - 4],
            buf[index ^ 1], buf[index], w, h, h + 2 * blur_y.radius, blur_y.coeff) &&
        !(large_vertical && engine->align_order == 4 &&
          ass_trim_blur_vertical(engine->blur_vert[blur_y.radius - 4],
              buf[index ^ 1], buf[index], w, h, h + 2 * blur_y.radius, blur_y.coeff)))
        engine->blur_vert[blur_y.radius - 4](buf[index ^ 1], buf[index], w, h, blur_y.coeff);
    h += 2 * blur_y.radius;
    index ^= 1;
    for (int i = 0; i < blur_x.level; i++) {
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

    engine->stripe_pack(bm->buffer, bm->stride, buf[index], w, h);
    if (!borrowed) ass_aligned_free(tmp);
    return true;
}

