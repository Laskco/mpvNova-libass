/*
 * Copyright (C) 2014 Vabishchevich Nikolay <vabnick@gmail.com>
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
#include <stdint.h>
#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif
#ifdef _MSC_VER
#include <intrin.h>
#pragma intrinsic(_BitScanReverse)
#endif

#include "ass_utils.h"
#include "ass_outline.h"
#include "ass_rasterizer.h"



static inline int ilog2(uint32_t n)
{
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_clz(n) ^ 31;
#elif defined(_MSC_VER)
    unsigned long res;
    _BitScanReverse(&res, n);
    return res;
#else
    int res = 0;
    for (int ord = 16; ord; ord /= 2)
        if (n >= ((uint32_t) 1 << ord)) {
            res += ord;
            n >>= ord;
        }
    return res;
#endif
}


bool ass_rasterizer_init(const BitmapEngine *engine, RasterizerData *rst, int outline_error)
{
    rst->outline_error = outline_error;
    rst->linebuf[0] = rst->linebuf[1] = NULL;
    rst->size[0] = rst->capacity[0] = 0;
    rst->size[1] = rst->capacity[1] = 0;
    rst->n_first = 0;

    unsigned align = 1 << engine->align_order;
    unsigned size = 1 << (2 * engine->tile_order);
    rst->tile = ass_aligned_alloc(align, size, false);
    return rst->tile;
}

/**
 * \brief Ensure sufficient buffer size (allocate if necessary)
 * \param index index (0 or 1) of the input segment buffer (rst->linebuf)
 * \param delta requested size increase
 * \return false on error
 */
#if defined(__GNUC__) || defined(__clang__)
#define ASS_NOINLINE __attribute__((noinline))
#define ASS_ALWAYS_INLINE static inline __attribute__((always_inline))
#else
#define ASS_NOINLINE
#define ASS_ALWAYS_INLINE static inline
#endif

static ASS_NOINLINE bool check_capacity_grow(RasterizerData *rst, int index, size_t delta)
{
    size_t capacity = FFMAX(2 * rst->capacity[index], 64);
    while (capacity < delta)
        capacity *= 2;
    void *ptr = realloc(rst->linebuf[index], sizeof(struct segment) * capacity);
    if (!ptr)
        return false;

    rst->linebuf[index] = (struct segment *) ptr;
    rst->capacity[index] = capacity;
    return true;
}

static inline bool check_capacity(RasterizerData *rst, int index, size_t delta)
{
    delta += rst->size[index];
    if (rst->capacity[index] >= delta)
        return true;
    return check_capacity_grow(rst, index, delta);
}

void ass_rasterizer_done(RasterizerData *rst)
{
    free(rst->linebuf[0]);
    free(rst->linebuf[1]);

    ass_aligned_free(rst->tile);
}


/*
 * Tiled Rasterization Algorithm
 *
 * 1) Convert splines into polylines using recursive subdivision.
 *
 * 2) Determine which segments of resulting polylines fall into each tile.
 * That's done through recursive splitting of segment array with horizontal or vertical lines.
 * Each individual segment can lie fully left(top) or right(bottom) from the splitting line or cross it.
 * In the latter case copies of such segment go to both output arrays. Also winding count
 * of the top-left corner of the second result rectangle gets calculated simultaneously with splitting.
 * Winding count of the first result rectangle is the same as of the source rectangle.
 *
 * 3) When the splitting is done to the tile level, there are 3 possible outcome:
 * - Tile doesn't have segments at all--fill it with solid color in accordance with winding count.
 * - Tile have only 1 segment--use simple half-plane filling algorithm.
 * - Generic case with 2 or more segments.
 * In the latter case each segment gets rasterized as right trapezoid into buffer
 * with additive or subtractive blending.
 */


// Helper struct for spline split decision
typedef struct {
    ASS_Vector r;
    int64_t r2, er;
} OutlineSegment;

static inline void segment_init(OutlineSegment *seg,
                                ASS_Vector beg, ASS_Vector end,
                                int32_t outline_error)
{
    int32_t x = end.x - beg.x;
    int32_t y = end.y - beg.y;
    int32_t abs_x = x < 0 ? -x : x;
    int32_t abs_y = y < 0 ? -y : y;

    seg->r.x = x;
    seg->r.y = y;
    seg->r2 = x * (int64_t) x + y * (int64_t) y;
    seg->er = outline_error * (int64_t) FFMAX(abs_x, abs_y);
}

static inline bool segment_subdivide(const OutlineSegment *seg,
                                     ASS_Vector beg, ASS_Vector pt)
{
    int32_t x = pt.x - beg.x;
    int32_t y = pt.y - beg.y;
#if UINTPTR_MAX == UINT32_MAX
    if (((uint32_t) (seg->r.x + 16384) | (uint32_t) (seg->r.y + 16384) |
         (uint32_t) (x + 16384) | (uint32_t) (y + 16384)) < 32768U &&
        seg->er <= 0x3FFFFFFF) {
        int32_t pdr = seg->r.x * x + seg->r.y * y;
        int32_t pcr = seg->r.x * y - seg->r.y * x;
        int32_t er = (int32_t) seg->er;
        int32_t r2 = (int32_t) seg->r2;
        return pdr < -er || pdr > r2 + er || (pcr < 0 ? -pcr : pcr) > er;
    }
#endif
    int64_t pdr = seg->r.x * (int64_t) x + seg->r.y * (int64_t) y;
    int64_t pcr = seg->r.x * (int64_t) y - seg->r.y * (int64_t) x;
    return pdr < -seg->er || pdr > seg->r2 + seg->er ||
        (pcr < 0 ? -pcr : pcr) > seg->er;
}

// Normalize raw halfplane coefficients without changing their direction.
static inline void normalize_line(struct segment *line)
{
    int32_t a = line->a;
    int32_t b = line->b;
    int32_t abs_a = a < 0 ? -a : a;
    int32_t abs_b = b < 0 ? -b : b;
    uint32_t max_ab = (uint32_t) (abs_a > abs_b ? abs_a : abs_b);
    int shift = 30 - ilog2(max_ab);
    max_ab <<= shift + 1;
    line->a = (int32_t) ((uint32_t) a << shift);
    line->b = (int32_t) ((uint32_t) b << shift);
    line->c = (int64_t) ((uint64_t) line->c << shift);
    uint32_t sq = (uint32_t) ((uint64_t) max_ab * max_ab >> 32);
    line->scale = (int32_t) (((uint64_t) 0x53333333 * sq >> 32) +
                             0x8810624DU - ((uint64_t) 0xBBC6A7EF * max_ab >> 32));
}

static void normalize_lines(RasterizerData *rst, size_t first)
{
    for (size_t i = first; i < rst->size[0]; i++)
        normalize_line(rst->linebuf[0] + i);
}

static inline bool quad_needs_split(ASS_Vector p0, ASS_Vector p1, ASS_Vector p2,
                                    int32_t outline_error)
{
#if UINTPTR_MAX == UINT32_MAX
    int32_t dx = p2.x - p0.x;
    int32_t dy = p2.y - p0.y;
    int32_t x1 = p1.x - p0.x;
    int32_t y1 = p1.y - p0.y;
    if (((uint32_t) (dx + 16384) | (uint32_t) (dy + 16384) |
         (uint32_t) (x1 + 16384) | (uint32_t) (y1 + 16384)) < 32768U &&
        outline_error <= 32768) {
        int32_t abs_x = dx < 0 ? -dx : dx;
        int32_t abs_y = dy < 0 ? -dy : dy;
        int32_t r2 = dx * dx + dy * dy;
        int32_t er = outline_error * (abs_x > abs_y ? abs_x : abs_y);
        int32_t pdr = dx * x1 + dy * y1;
        int32_t pcr = dx * y1 - dy * x1;
        return pdr < -er || pdr > r2 + er || (pcr < 0 ? -pcr : pcr) > er;
    }
#endif
    OutlineSegment seg;
    segment_init(&seg, p0, p2, outline_error);
    return segment_subdivide(&seg, p0, p1);
}

static inline bool cubic_needs_split(ASS_Vector p0, ASS_Vector p1, ASS_Vector p2, ASS_Vector p3,
                                     int32_t outline_error)
{
#if UINTPTR_MAX == UINT32_MAX
    int32_t dx = p3.x - p0.x;
    int32_t dy = p3.y - p0.y;
    int32_t x1 = p1.x - p0.x;
    int32_t y1 = p1.y - p0.y;
    int32_t x2 = p2.x - p0.x;
    int32_t y2 = p2.y - p0.y;
    if (((uint32_t) (dx + 16384) | (uint32_t) (dy + 16384) |
         (uint32_t) (x1 + 16384) | (uint32_t) (y1 + 16384) |
         (uint32_t) (x2 + 16384) | (uint32_t) (y2 + 16384)) < 32768U &&
        outline_error <= 32768) {
        int32_t abs_x = dx < 0 ? -dx : dx;
        int32_t abs_y = dy < 0 ? -dy : dy;
        int32_t r2 = dx * dx + dy * dy;
        int32_t er = outline_error * (abs_x > abs_y ? abs_x : abs_y);
        int32_t pdr1 = dx * x1 + dy * y1;
        int32_t pcr1 = dx * y1 - dy * x1;
        if (pdr1 < -er || pdr1 > r2 + er || (pcr1 < 0 ? -pcr1 : pcr1) > er)
            return true;
        int32_t pdr2 = dx * x2 + dy * y2;
        int32_t pcr2 = dx * y2 - dy * x2;
        return pdr2 < -er || pdr2 > r2 + er || (pcr2 < 0 ? -pcr2 : pcr2) > er;
    }
#endif
    OutlineSegment seg;
    segment_init(&seg, p0, p3, outline_error);
    return segment_subdivide(&seg, p0, p1) || segment_subdivide(&seg, p0, p2);
}

/**
 * \brief Add new segment to polyline
 */
ASS_ALWAYS_INLINE bool add_line_bbox(RasterizerData *rst, ASS_Vector pt0, ASS_Vector pt1,
                                     bool normalize, ASS_Rect *bbox)
{
    int32_t x = pt1.x - pt0.x;
    int32_t y = pt1.y - pt0.y;
    if (!(x | y))
        return true;

    size_t sz = rst->size[0];
    if (sz >= rst->capacity[0] && !check_capacity_grow(rst, 0, sz + 1))
        return false;
    struct segment *line = rst->linebuf[0] + sz;
    rst->size[0] = sz + 1;

    int32_t flags = SEGFLAG_EXACT_LEFT | SEGFLAG_EXACT_RIGHT |
                    SEGFLAG_EXACT_TOP | SEGFLAG_EXACT_BOTTOM;
    if (x < 0)
        flags ^= SEGFLAG_UL_DR;
    if (y >= 0)
        flags ^= SEGFLAG_DN | SEGFLAG_UL_DR;
    line->flags = flags;

    int32_t xmin = FFMIN(pt0.x, pt1.x);
    int32_t xmax = FFMAX(pt0.x, pt1.x);
    int32_t ymin = FFMIN(pt0.y, pt1.y);
    int32_t ymax = FFMAX(pt0.y, pt1.y);
    line->x_min = xmin;
    line->x_max = xmax;
    line->y_min = ymin;
    line->y_max = ymax;
    if (bbox)
        rectangle_update(bbox, xmin, ymin, xmax, ymax);

    int64_t c = y * (int64_t) pt0.x - x * (int64_t) pt0.y;
    if (normalize) {
        int32_t abs_a = y < 0 ? -y : y;
        int32_t abs_b = x < 0 ? -x : x;
        uint32_t max_ab = (uint32_t) (abs_a > abs_b ? abs_a : abs_b);
        int shift = 30 - ilog2(max_ab);
        max_ab <<= shift + 1;
        line->a = (int32_t) ((uint32_t) y << shift);
        line->b = (int32_t) ((uint32_t) (-x) << shift);
        line->c = (int64_t) ((uint64_t) c << shift);
        uint32_t sq = (uint32_t) ((uint64_t) max_ab * max_ab >> 32);
        line->scale = (int32_t) (((uint64_t) 0x53333333 * sq >> 32) +
                                 0x8810624DU - ((uint64_t) 0xBBC6A7EF * max_ab >> 32));
    } else {
        line->a = y;
        line->b = -x;
        line->c = c;
        line->scale = 0;
    }
    return true;
}

static bool add_line(RasterizerData *rst, ASS_Vector pt0, ASS_Vector pt1,
                     bool normalize)
{
    return add_line_bbox(rst, pt0, pt1, normalize, NULL);
}

/**
 * \brief Add quadratic spline to polyline using iterative subdivision.
 */
static bool add_quadratic_split(RasterizerData *rst, const ASS_Vector *pt_in,
                                bool normalize, ASS_Rect *bbox)
{
    // Depth is bounded by log2(OUTLINE_MAX) <= 28, so 64 entries cannot overflow.
    ASS_Vector stack[64][3];
    int sp = 0;
    int32_t err = rst->outline_error;
    ASS_Vector p0 = pt_in[0], p1 = pt_in[1], p2 = pt_in[2];

    for (;;) {
        ASS_Vector m01 = { p0.x + p1.x, p0.y + p1.y };
        ASS_Vector m12 = { p1.x + p2.x, p1.y + p2.y };
        ASS_Vector mid = { (m01.x + m12.x + 2) >> 2, (m01.y + m12.y + 2) >> 2 };
        m01.x >>= 1; m01.y >>= 1;
        m12.x >>= 1; m12.y >>= 1;

        // Push right half first so left half is emitted in exact original order.
        if (sp < 64) {
            stack[sp][0] = mid;
            stack[sp][1] = m12;
            stack[sp][2] = p2;
            sp++;
            p1 = m01;
            p2 = mid;
        } else {
            ASS_Vector left[3]  = { p0, m01, mid };
            ASS_Vector right[3] = { mid, m12, p2 };
            if (!add_quadratic_split(rst, left, normalize, bbox) ||
                !add_quadratic_split(rst, right, normalize, bbox))
                return false;
            if (!sp) return true;
            sp--;
            p0 = stack[sp][0]; p1 = stack[sp][1]; p2 = stack[sp][2];
        }

        while (!quad_needs_split(p0, p1, p2, err)) {
            if (!add_line_bbox(rst, p0, p2, normalize, bbox))
                return false;
            if (!sp)
                return true;
            sp--;
            p0 = stack[sp][0]; p1 = stack[sp][1]; p2 = stack[sp][2];
        }
    }
}

/**
 * \brief Add cubic spline to polyline using iterative subdivision.
 */
static bool add_cubic_split(RasterizerData *rst, const ASS_Vector *pt_in,
                            bool normalize, ASS_Rect *bbox)
{
    ASS_Vector stack[64][4];
    int sp = 0;
    int32_t err = rst->outline_error;
    ASS_Vector p0 = pt_in[0], p1 = pt_in[1], p2 = pt_in[2], p3 = pt_in[3];

    for (;;) {
        ASS_Vector n1 = { p0.x + p1.x, p0.y + p1.y };
        ASS_Vector center = { p1.x + p2.x + 2, p1.y + p2.y + 2 };
        ASS_Vector n5 = { p2.x + p3.x, p2.y + p3.y };
        ASS_Vector n2 = { n1.x + center.x, n1.y + center.y };
        ASS_Vector n4 = { center.x + n5.x, center.y + n5.y };
        ASS_Vector n3 = { (n2.x + n4.x - 1) >> 3, (n2.y + n4.y - 1) >> 3 };
        n2.x >>= 2; n2.y >>= 2;
        n4.x >>= 2; n4.y >>= 2;
        n1.x >>= 1; n1.y >>= 1;
        n5.x >>= 1; n5.y >>= 1;

        if (sp < 64) {
            stack[sp][0] = n3;
            stack[sp][1] = n4;
            stack[sp][2] = n5;
            stack[sp][3] = p3;
            sp++;
            p1 = n1;
            p2 = n2;
            p3 = n3;
        } else {
            ASS_Vector left[4]  = { p0, n1, n2, n3 };
            ASS_Vector right[4] = { n3, n4, n5, p3 };
            if (!add_cubic_split(rst, left, normalize, bbox) ||
                !add_cubic_split(rst, right, normalize, bbox))
                return false;
            if (!sp) return true;
            sp--;
            p0 = stack[sp][0]; p1 = stack[sp][1]; p2 = stack[sp][2]; p3 = stack[sp][3];
        }

        while (!cubic_needs_split(p0, p1, p2, p3, err)) {
            if (!add_line_bbox(rst, p0, p3, normalize, bbox))
                return false;
            if (!sp)
                return true;
            sp--;
            p0 = stack[sp][0]; p1 = stack[sp][1]; p2 = stack[sp][2]; p3 = stack[sp][3];
        }
    }
}


#if UINTPTR_MAX == UINT32_MAX || defined(__aarch64__)
static size_t contour_split32(const ASS_Vector *points, size_t begin,
                              size_t end, int tolerance)
{
    int32_t dx = points[end].x - points[begin].x;
    int32_t dy = points[end].y - points[begin].y;
    int32_t length2 = dx * dx + dy * dy;
    int32_t error = tolerance * FFMAX(abs(dx), abs(dy));
    int32_t largest = 0;
    size_t split = begin;
    size_t i = begin + 1;
#if defined(__ARM_NEON)
    if (length2) {
        int32x4_t best = vdupq_n_s32(0);
        uint32x4_t indices = vdupq_n_u32(begin);
        const uint32_t offsets[4] = {0, 1, 2, 3};
        uint32x4_t lanes = vld1q_u32(offsets);
        for (; i + 4 <= end; i += 4) {
            int32x4x2_t p = vld2q_s32((const int32_t *) (points + i));
            int32x4_t x = vsubq_s32(p.val[0], vdupq_n_s32(points[begin].x));
            int32x4_t y = vsubq_s32(p.val[1], vdupq_n_s32(points[begin].y));
            int32x4_t dot = vmlaq_n_s32(vmulq_n_s32(x, dx), y, dy);
            int32x4_t cross = vmlsq_n_s32(vmulq_n_s32(y, dx), x, dy);
            // Saturation only clamps negative candidates; those cannot beat zero.
            int32x4_t score = vmaxq_s32(
                vsubq_s32(vabsq_s32(cross), vdupq_n_s32(error)),
                vmaxq_s32(vsubq_s32(vnegq_s32(dot), vdupq_n_s32(error)),
                          vqsubq_s32(dot, vdupq_n_s32(length2 + error))));
            uint32x4_t larger = vcgtq_s32(score, best);
            best = vmaxq_s32(score, best);
            indices = vbslq_u32(larger, vaddq_u32(lanes, vdupq_n_u32(i)), indices);
        }
        int32_t scores[4];
        uint32_t positions[4];
        vst1q_s32(scores, best);
        vst1q_u32(positions, indices);
        // Equal scores must retain the first contour point, not the first lane.
        for (int lane = 0; lane < 4; lane++) {
            if (scores[lane] > largest ||
                (scores[lane] == largest && positions[lane] < split)) {
                largest = scores[lane];
                split = positions[lane];
            }
        }
    }
#endif
    for (; i < end; i++) {
        int32_t x = points[i].x - points[begin].x;
        int32_t y = points[i].y - points[begin].y;
        int32_t score;
        if (!length2) {
            score = abs(x) + abs(y);
        } else {
            int32_t dot = dx * x + dy * y;
            int32_t cross = dx * y - dy * x;
            score = abs(cross) - error;
            // Avoid subtracting length2 from a negative dot: that unused
            // negative score could overflow even with the bounded products.
            if (dot < -error) score = FFMAX(score, -dot - error);
            if (dot > length2 + error) score = FFMAX(score, dot - length2 - error);
        }
        if (score > largest) {
            largest = score;
            split = i;
        }
    }
    return split;
}

#if defined(__ARM_NEON)
static bool contour_fits32(const ASS_Vector *points, size_t begin, size_t end)
{
    if (abs(points[end].x - points[begin].x) > 32500 ||
        abs(points[end].y - points[begin].y) > 32500)
        return false;
    int32x4_t extent = vdupq_n_s32(0);
    size_t i = begin + 1;
    for (; i + 4 <= end + 1; i += 4) {
        int32x4x2_t p = vld2q_s32((const int32_t *) (points + i));
        int32x4_t x = vsubq_s32(p.val[0], vdupq_n_s32(points[begin].x));
        int32x4_t y = vsubq_s32(p.val[1], vdupq_n_s32(points[begin].y));
        extent = vmaxq_s32(extent, vmaxq_s32(vabsq_s32(x), vabsq_s32(y)));
    }
    int32_t lanes[4];
    vst1q_s32(lanes, extent);
    int32_t bound = FFMAX(FFMAX(lanes[0], lanes[1]), FFMAX(lanes[2], lanes[3]));
    for (; i <= end; i++) {
        bound = FFMAX(bound, abs(points[i].x - points[begin].x));
        bound = FFMAX(bound, abs(points[i].y - points[begin].y));
    }
    return bound <= 32500;
}
#endif
#endif

// Input is raw; every return leaves all surviving contour lines normalized.
static bool simplify_contour(RasterizerData *rst, size_t first, int tolerance)
{
    size_t count = rst->size[0] - first;
    if (count < 64 || count > 65536) {
        normalize_lines(rst, first);
        return true;
    }
    if (!check_capacity(rst, 1, count + 1)) {
        normalize_lines(rst, first);
        return false;
    }

    // The second segment buffer is scratch until raster subdivision starts.
    // Its storage is large and aligned enough for points, an interval stack,
    // and a keep mask, even when size_t is 64 bits.
    ASS_Vector *points = (void *) rst->linebuf[1];
    size_t *stack = (void *) (points + count + 1);
    uint8_t *keep = (void *) (stack + 2 * (count + 1));
    struct segment *line = rst->linebuf[0] + first;
    points[0] = (ASS_Vector) {
        line->b > 0 ? line->x_max : line->x_min,
        line->a < 0 ? line->y_max : line->y_min
    };
#if UINTPTR_MAX == UINT32_MAX || defined(__aarch64__)
    ASS_Rect bounds = {points[0].x, points[0].y, points[0].x, points[0].y};
#endif
    size_t corner = 0;
    int64_t furthest = 0;
    for (size_t i = 0; i < count; i++) {
        line = rst->linebuf[0] + first + i;
        points[i + 1] = (ASS_Vector) {
            line->b > 0 ? line->x_min : line->x_max,
            line->a < 0 ? line->y_min : line->y_max
        };
#if UINTPTR_MAX == UINT32_MAX || defined(__aarch64__)
        rectangle_update(&bounds, points[i + 1].x, points[i + 1].y,
                          points[i + 1].x, points[i + 1].y);
#endif
        int64_t dx = (int64_t) points[i + 1].x - points[0].x;
        int64_t dy = (int64_t) points[i + 1].y - points[0].y;
        int64_t distance = llabs(dx) + llabs(dy);
        if (distance > furthest) {
            furthest = distance;
            corner = i + 1;
        }
    }
    if (points[count].x != points[0].x || points[count].y != points[0].y ||
        !corner || corner == count) {
        normalize_lines(rst, first);
        return true;
    }

#if UINTPTR_MAX == UINT32_MAX || defined(__aarch64__)
    // 2 * 32500^2 + 128 * 32500 fits INT32_MAX. Keep the original
    // wide arithmetic for larger contours or tolerances.
    bool narrow = tolerance >= 0 && tolerance <= 128 &&
        (int64_t) bounds.x_max - bounds.x_min <= 32500 &&
        (int64_t) bounds.y_max - bounds.y_min <= 32500;
#if defined(__ARM_NEON)
    bool bounded = tolerance >= 0 && tolerance <= 128 &&
        (int64_t) bounds.x_max - bounds.x_min <= (1 << 29) &&
        (int64_t) bounds.y_max - bounds.y_min <= (1 << 29);
#endif
#endif

    memset(keep, 0, count + 1);
    keep[0] = keep[corner] = keep[count] = 1;
    size_t used = 0;
    stack[used++] = 0;
    stack[used++] = corner;
    stack[used++] = corner;
    stack[used++] = count;
    while (used) {
        size_t end = stack[--used], begin = stack[--used], split = begin;
#if UINTPTR_MAX == UINT32_MAX || defined(__aarch64__)
        bool use_narrow = narrow;
#if defined(__ARM_NEON)
        use_narrow = use_narrow ||
            (bounded && end - begin >= 8 && contour_fits32(points, begin, end));
#endif
        if (use_narrow) {
            split = contour_split32(points, begin, end, tolerance);
        } else
#endif
        {
            int64_t dx = (int64_t) points[end].x - points[begin].x;
            int64_t dy = (int64_t) points[end].y - points[begin].y;
            int64_t length2 = dx * dx + dy * dy;
            int64_t error = tolerance * FFMAX(llabs(dx), llabs(dy));
            int64_t largest = 0;
            for (size_t i = begin + 1; i < end; i++) {
                int64_t x = (int64_t) points[i].x - points[begin].x;
                int64_t y = (int64_t) points[i].y - points[begin].y;
                int64_t dot = dx * x + dy * y;
                int64_t cross = dx * y - dy * x;
                int64_t score = FFMAX(llabs(cross) - error,
                                      FFMAX(-dot - error, dot - length2 - error));
                if (!length2) score = llabs(x) + llabs(y);
                if (score > largest) {
                    largest = score;
                    split = i;
                }
            }
        }
        if (split != begin) {
            keep[split] = 1;
            stack[used++] = begin;
            stack[used++] = split;
            stack[used++] = split;
            stack[used++] = end;
        }
    }
    rst->size[0] = first;
    ASS_Vector previous = points[0];
    for (size_t i = 1; i <= count; i++) {
        if (!keep[i]) continue;
        if (!add_line(rst, previous, points[i], true)) return false;
        previous = points[i];
    }
    return true;
}

bool ass_rasterizer_set_outline(RasterizerData *rst,
                                const ASS_Outline *path, bool extra,
                                const ASS_Rect *clip, int simplify)
{
    if (!extra) {
        rectangle_reset(&rst->bbox);
        rst->n_first = 0;
    }
    rst->size[0] = rst->n_first;

#ifndef NDEBUG
    for (size_t i = 0; i < path->n_points; i++)
        assert(vec_in_range(path->points[i], OUTLINE_MAX));
#endif

    if (path->n_segments && !check_capacity(rst, 0, path->n_segments))
        return false;

    ASS_Vector *start = path->points, *cur = start;
    size_t contour_start = rst->size[0];
    // RDP needs only coefficient signs and endpoint bounds, not normalization.
    bool normalize = !simplify;
    bool track_bbox = !clip && !simplify;
    ASS_Rect local_bbox = rst->bbox;
    ASS_Rect *bbox = track_bbox ? &local_bbox : NULL;
    int32_t outline_error = rst->outline_error;
    for (size_t i = 0; i < path->n_segments; i++) {
        char seg_tag = path->segments[i];
        int n = seg_tag & OUTLINE_COUNT_MASK;
        cur += n;

        ASS_Vector *end = cur;
        if (seg_tag & OUTLINE_CONTOUR_END) {
            end = start;
            start = cur;
        }

        switch (n) {
        case OUTLINE_LINE_SEGMENT:
            if (!add_line_bbox(rst, cur[-1], *end, normalize, bbox))
                goto fail;
            break;

        case OUTLINE_QUADRATIC_SPLINE: {
            if (!quad_needs_split(cur[-2], cur[-1], *end, outline_error)) {
                if (!add_line_bbox(rst, cur[-2], *end, normalize, bbox))
                    goto fail;
            } else {
                ASS_Vector p[3] = { cur[-2], cur[-1], *end };
                if (!add_quadratic_split(rst, p, normalize, bbox))
                    goto fail;
            }
            break;
        }

        case OUTLINE_CUBIC_SPLINE: {
            if (!cubic_needs_split(cur[-3], cur[-2], cur[-1], *end, outline_error)) {
                if (!add_line_bbox(rst, cur[-3], *end, normalize, bbox))
                    goto fail;
            } else {
                ASS_Vector p[4] = { cur[-3], cur[-2], cur[-1], *end };
                if (!add_cubic_split(rst, p, normalize, bbox))
                    goto fail;
            }
            break;
        }

        default:
            goto fail;
        }
        if (seg_tag & OUTLINE_CONTOUR_END) {
            if (simplify && !simplify_contour(rst, contour_start, simplify))
                return false;
            if (clip) {
                ASS_Rect bounds;
                rectangle_reset(&bounds);
                for (size_t k = contour_start; k < rst->size[0]; k++) {
                    struct segment *line = &rst->linebuf[0][k];
                    rectangle_update(&bounds, line->x_min, line->y_min,
                                      line->x_max, line->y_max);
                }
                rectangle_update(&rst->bbox, bounds.x_min, bounds.y_min,
                                  bounds.x_max, bounds.y_max);
                if ((int64_t) bounds.x_max < (int64_t) clip->x_min * 64 ||
                    (int64_t) bounds.y_max < (int64_t) clip->y_min * 64 ||
                    (int64_t) bounds.x_min > (int64_t) clip->x_max * 64 ||
                    (int64_t) bounds.y_min > (int64_t) clip->y_max * 64)
                    rst->size[0] = contour_start;
            }
            contour_start = rst->size[0];
        }
    }
    if (simplify)
        normalize_lines(rst, contour_start);
    assert(start == cur && (!cur || cur == path->points + path->n_points));

    if (track_bbox) {
        rst->bbox = local_bbox;
    } else if (!clip) {
        for (size_t k = rst->n_first; k < rst->size[0]; k++) {
            struct segment *line = &rst->linebuf[0][k];
            rectangle_update(&rst->bbox,
                             line->x_min, line->y_min,
                             line->x_max, line->y_max);
        }
    }
    if (!extra)
        rst->n_first = rst->size[0];
    return true;

fail:
    // Earlier contours are finalized; never expose a raw trailing contour.
    if (simplify)
        normalize_lines(rst, contour_start);
    return false;
}

typedef struct {
    ASS_Vector *pts;
    size_t n, cap;
} PointBuf;

static bool point_buf_push(PointBuf *b, ASS_Vector p)
{
    if (b->n >= b->cap) {
        size_t ncap = b->cap ? b->cap * 2 : 256;
        if (!ASS_REALLOC_ARRAY(b->pts, ncap))
            return false;
        b->cap = ncap;
    }
    b->pts[b->n++] = p;
    return true;
}

static bool flatten_quad_to_buf(PointBuf *b, const ASS_Vector *pt_in, int32_t err)
{
    ASS_Vector stack[32][3];
    uint8_t depth[32];
    int sp = 0;
    uint8_t d = 0;
    ASS_Vector p0 = pt_in[0], p1 = pt_in[1], p2 = pt_in[2];
    for (;;) {
        ASS_Vector m01 = { p0.x + p1.x, p0.y + p1.y };
        ASS_Vector m12 = { p1.x + p2.x, p1.y + p2.y };
        ASS_Vector mid = { (m01.x + m12.x + 2) >> 2, (m01.y + m12.y + 2) >> 2 };
        m01.x >>= 1; m01.y >>= 1;
        m12.x >>= 1; m12.y >>= 1;
        if (d < 28 && sp < 32) {
            stack[sp][0] = mid;
            stack[sp][1] = m12;
            stack[sp][2] = p2;
            depth[sp] = (uint8_t) (d + 1);
            sp++;
            p1 = m01;
            p2 = mid;
            d++;
        } else {
            if (!point_buf_push(b, p2))
                return false;
            if (!sp)
                return true;
            sp--;
            p0 = stack[sp][0]; p1 = stack[sp][1]; p2 = stack[sp][2]; d = depth[sp];
        }
        while (!quad_needs_split(p0, p1, p2, err)) {
            if (!point_buf_push(b, p2))
                return false;
            if (!sp)
                return true;
            sp--;
            p0 = stack[sp][0]; p1 = stack[sp][1]; p2 = stack[sp][2]; d = depth[sp];
        }
    }
}

static bool flatten_cubic_to_buf(PointBuf *b, const ASS_Vector *pt_in, int32_t err)
{
    ASS_Vector stack[32][4];
    uint8_t depth[32];
    int sp = 0;
    uint8_t d = 0;
    ASS_Vector p0 = pt_in[0], p1 = pt_in[1], p2 = pt_in[2], p3 = pt_in[3];
    for (;;) {
        ASS_Vector n1 = { p0.x + p1.x, p0.y + p1.y };
        ASS_Vector center = { p1.x + p2.x + 2, p1.y + p2.y + 2 };
        ASS_Vector n5 = { p2.x + p3.x, p2.y + p3.y };
        ASS_Vector n2 = { n1.x + center.x, n1.y + center.y };
        ASS_Vector n4 = { center.x + n5.x, center.y + n5.y };
        ASS_Vector n3 = { (n2.x + n4.x - 1) >> 3, (n2.y + n4.y - 1) >> 3 };
        n2.x >>= 2; n2.y >>= 2;
        n4.x >>= 2; n4.y >>= 2;
        n1.x >>= 1; n1.y >>= 1;
        n5.x >>= 1; n5.y >>= 1;
        if (d < 28 && sp < 32) {
            stack[sp][0] = n3;
            stack[sp][1] = n4;
            stack[sp][2] = n5;
            stack[sp][3] = p3;
            depth[sp] = (uint8_t) (d + 1);
            sp++;
            p1 = n1;
            p2 = n2;
            p3 = n3;
            d++;
        } else {
            if (!point_buf_push(b, p3))
                return false;
            if (!sp)
                return true;
            sp--;
            p0 = stack[sp][0]; p1 = stack[sp][1]; p2 = stack[sp][2]; p3 = stack[sp][3]; d = depth[sp];
        }
        while (!cubic_needs_split(p0, p1, p2, p3, err)) {
            if (!point_buf_push(b, p3))
                return false;
            if (!sp)
                return true;
            sp--;
            p0 = stack[sp][0]; p1 = stack[sp][1]; p2 = stack[sp][2]; p3 = stack[sp][3]; d = depth[sp];
        }
    }
}

bool ass_outline_simplify(ASS_Outline *dst, const ASS_Outline *src,
                          int32_t tolerance)
{
    ass_outline_clear(dst);
    if (!src || !src->n_points || !src->n_segments)
        return true;

    PointBuf poly = {0};
    size_t *stack = NULL;
    size_t stack_cap = 0;
    uint8_t *keep = NULL;
    size_t keep_cap = 0;

    if (!ass_outline_alloc(dst, FFMIN(src->n_points, (size_t) 256),
                           FFMIN(src->n_segments, (size_t) 256)))
        return false;

    int32_t err = FFMAX(tolerance / 2, 16);
    const ASS_Vector *start = src->points, *cur = start;
    bool contour_begin = true;

    for (size_t i = 0; i < src->n_segments; i++) {
        char seg_tag = src->segments[i];
        int n = seg_tag & OUTLINE_COUNT_MASK;
        if (contour_begin) {
            poly.n = 0;
            if (!point_buf_push(&poly, *cur))
                goto fail;
            contour_begin = false;
        }
        cur += n;
        const ASS_Vector *end = (seg_tag & OUTLINE_CONTOUR_END) ? start : cur;
        if (seg_tag & OUTLINE_CONTOUR_END)
            start = cur;

        switch (n) {
        case OUTLINE_LINE_SEGMENT:
            if (!point_buf_push(&poly, *end))
                goto fail;
            break;
        case OUTLINE_QUADRATIC_SPLINE:
            if (!quad_needs_split(cur[-2], cur[-1], *end, err)) {
                if (!point_buf_push(&poly, *end))
                    goto fail;
            } else {
                ASS_Vector p[3] = { cur[-2], cur[-1], *end };
                if (!flatten_quad_to_buf(&poly, p, err))
                    goto fail;
            }
            break;
        case OUTLINE_CUBIC_SPLINE:
            if (!cubic_needs_split(cur[-3], cur[-2], cur[-1], *end, err)) {
                if (!point_buf_push(&poly, *end))
                    goto fail;
            } else {
                ASS_Vector p[4] = { cur[-3], cur[-2], cur[-1], *end };
                if (!flatten_cubic_to_buf(&poly, p, err))
                    goto fail;
            }
            break;
        default:
            goto fail;
        }

        if (!(seg_tag & OUTLINE_CONTOUR_END))
            continue;
        contour_begin = true;

        size_t count = poly.n > 0 ? poly.n - 1 : 0;
        if (count < 3)
            continue;

        const ASS_Vector *pts = poly.pts;
        ASS_Rect bounds = {pts[0].x, pts[0].y, pts[0].x, pts[0].y};
        size_t corner = 0;
        int64_t furthest = 0;
        for (size_t k = 1; k <= count; k++) {
            rectangle_update(&bounds, pts[k].x, pts[k].y, pts[k].x, pts[k].y);
            int64_t dx = (int64_t) pts[k].x - pts[0].x;
            int64_t dy = (int64_t) pts[k].y - pts[0].y;
            int64_t dist = llabs(dx) + llabs(dy);
            if (dist > furthest) {
                furthest = dist;
                corner = k;
            }
        }
        int64_t bw = (int64_t) bounds.x_max - bounds.x_min;
        int64_t bh = (int64_t) bounds.y_max - bounds.y_min;
        if (bw < tolerance / 4 && bh < tolerance / 4)
            continue;

        if (count < 4 || count > 65536 || !corner || corner == count) {
            for (size_t k = 0; k < count; k++) {
                char tag = OUTLINE_LINE_SEGMENT | (k + 1 == count ? OUTLINE_CONTOUR_END : 0);
                if (!ass_outline_add_point(dst, pts[k], tag))
                    goto fail;
            }
            continue;
        }

        if (2 * (count + 1) > stack_cap) {
            size_t ncap = FFMAX(stack_cap * 2, 2 * (count + 1));
            if (!ASS_REALLOC_ARRAY(stack, ncap))
                goto fail;
            stack_cap = ncap;
        }
        if (count + 1 > keep_cap) {
            size_t ncap = FFMAX(keep_cap * 2, count + 1);
            if (!ASS_REALLOC_ARRAY(keep, ncap))
                goto fail;
            keep_cap = ncap;
        }
        memset(keep, 0, count + 1);
        keep[0] = keep[corner] = keep[count] = 1;

#if UINTPTR_MAX == UINT32_MAX || defined(__aarch64__)
        bool narrow = tolerance >= 0 && tolerance <= 128 &&
            bw <= 32500 && bh <= 32500;
#endif
        size_t used = 0;
        stack[used++] = 0;
        stack[used++] = corner;
        stack[used++] = corner;
        stack[used++] = count;
        while (used) {
            size_t end_i = stack[--used], beg_i = stack[--used], split = beg_i;
#if UINTPTR_MAX == UINT32_MAX || defined(__aarch64__)
            if (narrow) {
                split = contour_split32(pts, beg_i, end_i, tolerance);
            } else
#endif
            {
                int64_t dx = (int64_t) pts[end_i].x - pts[beg_i].x;
                int64_t dy = (int64_t) pts[end_i].y - pts[beg_i].y;
                int64_t length2 = dx * dx + dy * dy;
                int64_t error = (int64_t) tolerance * FFMAX(llabs(dx), llabs(dy));
                int64_t largest = 0;
                for (size_t k = beg_i + 1; k < end_i; k++) {
                    int64_t x = (int64_t) pts[k].x - pts[beg_i].x;
                    int64_t y = (int64_t) pts[k].y - pts[beg_i].y;
                    int64_t score;
                    if (!length2) {
                        score = llabs(x) + llabs(y);
                    } else {
                        int64_t dot = dx * x + dy * y;
                        int64_t cross = dx * y - dy * x;
                        score = FFMAX(llabs(cross) - error,
                                      FFMAX(-dot - error, dot - length2 - error));
                    }
                    if (score > largest) {
                        largest = score;
                        split = k;
                    }
                }
            }
            if (split != beg_i) {
                keep[split] = 1;
                stack[used++] = beg_i;
                stack[used++] = split;
                stack[used++] = split;
                stack[used++] = end_i;
            }
        }

        size_t kept = 0;
        for (size_t k = 0; k < count; k++)
            kept += keep[k];
        if (kept < 3)
            continue;

        size_t emitted = 0;
        for (size_t k = 0; k < count; k++) {
            if (!keep[k])
                continue;
            emitted++;
            char tag = OUTLINE_LINE_SEGMENT | (emitted == kept ? OUTLINE_CONTOUR_END : 0);
            if (!ass_outline_add_point(dst, pts[k], tag))
                goto fail;
        }
    }

    free(poly.pts);
    free(stack);
    free(keep);
    return true;

fail:
    free(poly.pts);
    free(stack);
    free(keep);
    ass_outline_free(dst);
    return false;
}


static void segment_move_x(struct segment *line, int32_t x)
{
    line->x_min -= x;
    line->x_max -= x;
    line->x_min = FFMAX(line->x_min, 0);
    line->c -= line->a * (int64_t) x;

    static const int test = SEGFLAG_EXACT_LEFT | SEGFLAG_UL_DR;
    if (!line->x_min && (line->flags & test) == test)
        line->flags &= ~SEGFLAG_EXACT_TOP;
}

static void segment_move_y(struct segment *line, int32_t y)
{
    line->y_min -= y;
    line->y_max -= y;
    line->y_min = FFMAX(line->y_min, 0);
    line->c -= line->b * (int64_t) y;

    static const int test = SEGFLAG_EXACT_TOP | SEGFLAG_UL_DR;
    if (!line->y_min && (line->flags & test) == test)
        line->flags &= ~SEGFLAG_EXACT_LEFT;
}

static inline int segment_check_left(const struct segment *line, int32_t x)
{
    if (line->flags & SEGFLAG_EXACT_LEFT)
        return line->x_min >= x;
    int64_t cc = line->c - line->a * (int64_t) x -
        line->b * (int64_t) (line->flags & SEGFLAG_UL_DR ? line->y_min : line->y_max);
    if (line->a < 0)
        cc = -cc;
    return cc >= 0;
}

static inline int segment_check_right(const struct segment *line, int32_t x)
{
    if (line->flags & SEGFLAG_EXACT_RIGHT)
        return line->x_max <= x;
    int64_t cc = line->c - line->a * (int64_t) x -
        line->b * (int64_t) (line->flags & SEGFLAG_UL_DR ? line->y_max : line->y_min);
    if (line->a > 0)
        cc = -cc;
    return cc >= 0;
}

static inline int segment_check_top(const struct segment *line, int32_t y)
{
    if (line->flags & SEGFLAG_EXACT_TOP)
        return line->y_min >= y;
    int64_t cc = line->c - line->b * (int64_t) y -
        line->a * (int64_t) (line->flags & SEGFLAG_UL_DR ? line->x_min : line->x_max);
    if (line->b < 0)
        cc = -cc;
    return cc >= 0;
}

static inline int segment_check_bottom(const struct segment *line, int32_t y)
{
    if (line->flags & SEGFLAG_EXACT_BOTTOM)
        return line->y_max <= y;
    int64_t cc = line->c - line->b * (int64_t) y -
        line->a * (int64_t) (line->flags & SEGFLAG_UL_DR ? line->x_max : line->x_min);
    if (line->b > 0)
        cc = -cc;
    return cc >= 0;
}

/**
 * \brief Split list of segments horizontally
 * \param src in: input array, can coincide with *dst0 or *dst1
 * \param n_src in: numbers of input segments for both groups
 * \param dst0, dst1 out: output arrays of at least n_src[0] + n_src[1] size;
 *                      dst1 may be NULL to discard the second output
 * \param n_dst0, n_dst1 out: numbers of output segments for both groups
 * \param winding out: resulting winding of bottom-split point
 * \param x in: split coordinate
 */
static inline void polyline_split_horz_group(const struct segment *src, size_t n,
                                             struct segment **dst0_p, size_t *n0_p,
                                             struct segment **dst1_p, size_t *n1_p,
                                             int *winding_p, int32_t x)
{
    const struct segment *end = src + n;
    struct segment *dst0 = *dst0_p;
    struct segment *dst1 = *dst1_p;
    size_t n0 = 0, n1 = 0;
    int winding = *winding_p;

    if (dst0 && dst1) {
        for (; src != end; src++) {
            int32_t xmax = src->x_max;
            if (xmax <= x) {
                if (!src->y_min && (src->flags & SEGFLAG_EXACT_TOP))
                    winding += src->a < 0 ? 1 : -1;
                if (src->x_min >= x)
                    continue;
                if (dst0 != src)
                    *dst0 = *src;
                dst0++;
                n0++;
                continue;
            }
            int32_t xmin = src->x_min;
            if (xmin > x) {
                int32_t a = src->a;
                dst1->c = src->c - a * (int64_t) x;
                dst1->a = a;
                dst1->b = src->b;
                dst1->scale = src->scale;
                dst1->flags = src->flags;
                dst1->x_min = xmin - x;
                dst1->x_max = xmax - x;
                dst1->y_min = src->y_min;
                dst1->y_max = src->y_max;
                dst1++;
                n1++;
                continue;
            }

            int delta = 0;
            if (!src->y_min && (src->flags & SEGFLAG_EXACT_TOP))
                delta = src->a < 0 ? 1 : -1;
            if (segment_check_right(src, x)) {
                winding += delta;
                if (xmin >= x)
                    continue;
                if (dst0 != src)
                    *dst0 = *src;
                dst0->x_max = x;
                dst0++;
                n0++;
                continue;
            }
            if (segment_check_left(src, x)) {
                int32_t a = src->a;
                int32_t flags = src->flags;
                int32_t nxmin = xmin > x ? xmin - x : 0;
                static const int test = SEGFLAG_EXACT_LEFT | SEGFLAG_UL_DR;
                if (!nxmin && (flags & test) == test)
                    flags &= ~SEGFLAG_EXACT_TOP;
                dst1->c = src->c - a * (int64_t) x;
                dst1->a = a;
                dst1->b = src->b;
                dst1->scale = src->scale;
                dst1->flags = flags;
                dst1->x_min = nxmin;
                dst1->x_max = xmax - x;
                dst1->y_min = src->y_min;
                dst1->y_max = src->y_max;
                dst1++;
                n1++;
                continue;
            }
            int32_t flags = src->flags;
            if (flags & SEGFLAG_UL_DR)
                winding += delta;
            int32_t a = src->a;
            int32_t f0 = flags & ~SEGFLAG_EXACT_TOP;
            int32_t f1 = flags & ~SEGFLAG_EXACT_BOTTOM;
            if (flags & SEGFLAG_UL_DR) {
                int32_t tmp = f0;
                f0 = f1;
                f1 = tmp;
            }
            f0 |= SEGFLAG_EXACT_RIGHT;
            f1 |= SEGFLAG_EXACT_LEFT;
            dst1->c = src->c - a * (int64_t) x;
            dst1->a = a;
            dst1->b = src->b;
            dst1->scale = src->scale;
            dst1->flags = f1;
            dst1->x_min = 0;
            dst1->x_max = xmax - x;
            dst1->y_min = src->y_min;
            dst1->y_max = src->y_max;
            if (dst0 != src)
                *dst0 = *src;
            dst0->x_max = x;
            dst0->flags = f0;
            dst0++;
            dst1++;
            n0++;
            n1++;
        }
        *dst0_p = dst0;
        *dst1_p = dst1;
        *n0_p = n0;
        *n1_p = n1;
        *winding_p = winding;
        return;
    }

    for (; src != end; src++) {
        int32_t xmax = src->x_max;
        if (xmax <= x) {
            if (!src->y_min && (src->flags & SEGFLAG_EXACT_TOP))
                winding += src->a < 0 ? 1 : -1;
            if (src->x_min >= x)
                continue;
            if (dst0) {
                if (dst0 != src)
                    *dst0 = *src;
                dst0++;
            }
            n0++;
            continue;
        }
        int32_t xmin = src->x_min;
        if (xmin > x) {
            if (dst1) {
                int32_t a = src->a;
                int64_t nc = src->c - a * (int64_t) x;
                if (dst1 != src)
                    *dst1 = *src;
                dst1->c = nc;
                dst1->x_min = xmin - x;
                dst1->x_max = xmax - x;
                dst1++;
            }
            n1++;
            continue;
        }

        int delta = 0;
        if (!src->y_min && (src->flags & SEGFLAG_EXACT_TOP))
            delta = src->a < 0 ? 1 : -1;
        if (segment_check_right(src, x)) {
            winding += delta;
            if (xmin >= x)
                continue;
            if (dst0) {
                if (dst0 != src)
                    *dst0 = *src;
                dst0->x_max = x;
                dst0++;
            }
            n0++;
            continue;
        }
        if (segment_check_left(src, x)) {
            if (dst1) {
                if (dst1 != src)
                    *dst1 = *src;
                segment_move_x(dst1, x);
                dst1++;
            }
            n1++;
            continue;
        }
        if (src->flags & SEGFLAG_UL_DR)
            winding += delta;
        if (dst0) {
            if (dst0 != src)
                *dst0 = *src;
            dst0->x_max = x;
            dst0->flags &= ~(dst0->flags & SEGFLAG_UL_DR ?
                             SEGFLAG_EXACT_BOTTOM : SEGFLAG_EXACT_TOP);
            dst0->flags |= SEGFLAG_EXACT_RIGHT;
            dst0++;
        } else if (dst1) {
            int32_t a = src->a;
            int64_t nc = src->c - a * (int64_t) x;
            int32_t flags = src->flags;
            if (dst1 != src)
                *dst1 = *src;
            dst1->c = nc;
            dst1->x_min = 0;
            dst1->x_max = xmax - x;
            dst1->flags = (flags & ~(flags & SEGFLAG_UL_DR ?
                                     SEGFLAG_EXACT_TOP : SEGFLAG_EXACT_BOTTOM)) |
                          SEGFLAG_EXACT_LEFT;
            dst1++;
        }
        n0++;
        n1++;
    }

    *dst0_p = dst0;
    *dst1_p = dst1;
    *n0_p = n0;
    *n1_p = n1;
    *winding_p = winding;
}

static void polyline_split_horz(const struct segment *src, const size_t n_src[2],
                                struct segment *dst0, size_t n_dst0[2],
                                struct segment *dst1, size_t n_dst1[2],
                                int winding[2], int32_t x)
{
    n_dst0[0] = n_dst0[1] = 0;
    n_dst1[0] = n_dst1[1] = 0;
    if (n_src[0])
        polyline_split_horz_group(src, n_src[0], &dst0, &n_dst0[0],
                                  &dst1, &n_dst1[0], &winding[0], x);
    if (n_src[1])
        polyline_split_horz_group(src + n_src[0], n_src[1], &dst0, &n_dst0[1],
                                  &dst1, &n_dst1[1], &winding[1], x);
}

/**
 * \brief Split list of segments vertically
 */
static inline void polyline_split_vert_group(const struct segment *src, size_t n,
                                             struct segment **dst0_p, size_t *n0_p,
                                             struct segment **dst1_p, size_t *n1_p,
                                             int *winding_p, int32_t y)
{
    const struct segment *end = src + n;
    struct segment *dst0 = *dst0_p;
    struct segment *dst1 = *dst1_p;
    size_t n0 = 0, n1 = 0;
    int winding = *winding_p;

    if (dst0 && dst1) {
        for (; src != end; src++) {
            int32_t ymax = src->y_max;
            if (ymax <= y) {
                if (!src->x_min && (src->flags & SEGFLAG_EXACT_LEFT))
                    winding += src->b < 0 ? 1 : -1;
                if (src->y_min >= y)
                    continue;
                if (dst0 != src)
                    *dst0 = *src;
                dst0++;
                n0++;
                continue;
            }
            int32_t ymin = src->y_min;
            if (ymin > y) {
                int32_t b = src->b;
                dst1->c = src->c - b * (int64_t) y;
                dst1->a = src->a;
                dst1->b = b;
                dst1->scale = src->scale;
                dst1->flags = src->flags;
                dst1->x_min = src->x_min;
                dst1->x_max = src->x_max;
                dst1->y_min = ymin - y;
                dst1->y_max = ymax - y;
                dst1++;
                n1++;
                continue;
            }

            int delta = 0;
            if (!src->x_min && (src->flags & SEGFLAG_EXACT_LEFT))
                delta = src->b < 0 ? 1 : -1;
            if (segment_check_bottom(src, y)) {
                winding += delta;
                if (ymin >= y)
                    continue;
                if (dst0 != src)
                    *dst0 = *src;
                dst0->y_max = y;
                dst0++;
                n0++;
                continue;
            }
            if (segment_check_top(src, y)) {
                int32_t b = src->b;
                int32_t flags = src->flags;
                int32_t nymin = ymin > y ? ymin - y : 0;
                static const int test = SEGFLAG_EXACT_TOP | SEGFLAG_UL_DR;
                if (!nymin && (flags & test) == test)
                    flags &= ~SEGFLAG_EXACT_LEFT;
                dst1->c = src->c - b * (int64_t) y;
                dst1->a = src->a;
                dst1->b = b;
                dst1->scale = src->scale;
                dst1->flags = flags;
                dst1->x_min = src->x_min;
                dst1->x_max = src->x_max;
                dst1->y_min = nymin;
                dst1->y_max = ymax - y;
                dst1++;
                n1++;
                continue;
            }
            int32_t flags = src->flags;
            if (flags & SEGFLAG_UL_DR)
                winding += delta;
            int32_t b = src->b;
            int32_t f0 = flags & ~SEGFLAG_EXACT_LEFT;
            int32_t f1 = flags & ~SEGFLAG_EXACT_RIGHT;
            if (flags & SEGFLAG_UL_DR) {
                int32_t tmp = f0;
                f0 = f1;
                f1 = tmp;
            }
            f0 |= SEGFLAG_EXACT_BOTTOM;
            f1 |= SEGFLAG_EXACT_TOP;
            dst1->c = src->c - b * (int64_t) y;
            dst1->a = src->a;
            dst1->b = b;
            dst1->scale = src->scale;
            dst1->flags = f1;
            dst1->x_min = src->x_min;
            dst1->x_max = src->x_max;
            dst1->y_min = 0;
            dst1->y_max = ymax - y;
            if (dst0 != src)
                *dst0 = *src;
            dst0->y_max = y;
            dst0->flags = f0;
            dst0++;
            dst1++;
            n0++;
            n1++;
        }
        *dst0_p = dst0;
        *dst1_p = dst1;
        *n0_p = n0;
        *n1_p = n1;
        *winding_p = winding;
        return;
    }

    for (; src != end; src++) {
        int32_t ymax = src->y_max;
        if (ymax <= y) {
            if (!src->x_min && (src->flags & SEGFLAG_EXACT_LEFT))
                winding += src->b < 0 ? 1 : -1;
            if (src->y_min >= y)
                continue;
            if (dst0) {
                if (dst0 != src)
                    *dst0 = *src;
                dst0++;
            }
            n0++;
            continue;
        }
        int32_t ymin = src->y_min;
        if (ymin > y) {
            if (dst1) {
                int32_t b = src->b;
                int64_t nc = src->c - b * (int64_t) y;
                if (dst1 != src)
                    *dst1 = *src;
                dst1->c = nc;
                dst1->y_min = ymin - y;
                dst1->y_max = ymax - y;
                dst1++;
            }
            n1++;
            continue;
        }

        int delta = 0;
        if (!src->x_min && (src->flags & SEGFLAG_EXACT_LEFT))
            delta = src->b < 0 ? 1 : -1;
        if (segment_check_bottom(src, y)) {
            winding += delta;
            if (ymin >= y)
                continue;
            if (dst0) {
                if (dst0 != src)
                    *dst0 = *src;
                dst0->y_max = y;
                dst0++;
            }
            n0++;
            continue;
        }
        if (segment_check_top(src, y)) {
            if (dst1) {
                if (dst1 != src)
                    *dst1 = *src;
                segment_move_y(dst1, y);
                dst1++;
            }
            n1++;
            continue;
        }
        if (src->flags & SEGFLAG_UL_DR)
            winding += delta;
        if (dst0) {
            if (dst0 != src)
                *dst0 = *src;
            dst0->y_max = y;
            dst0->flags &= ~(dst0->flags & SEGFLAG_UL_DR ?
                             SEGFLAG_EXACT_RIGHT : SEGFLAG_EXACT_LEFT);
            dst0->flags |= SEGFLAG_EXACT_BOTTOM;
            dst0++;
        } else if (dst1) {
            int32_t b = src->b;
            int64_t nc = src->c - b * (int64_t) y;
            int32_t flags = src->flags;
            if (dst1 != src)
                *dst1 = *src;
            dst1->c = nc;
            dst1->y_min = 0;
            dst1->y_max = ymax - y;
            dst1->flags = (flags & ~(flags & SEGFLAG_UL_DR ?
                                     SEGFLAG_EXACT_LEFT : SEGFLAG_EXACT_RIGHT)) |
                          SEGFLAG_EXACT_TOP;
            dst1++;
        }
        n0++;
        n1++;
    }

    *dst0_p = dst0;
    *dst1_p = dst1;
    *n0_p = n0;
    *n1_p = n1;
    *winding_p = winding;
}

static void polyline_split_vert(const struct segment *src, const size_t n_src[2],
                                struct segment *dst0, size_t n_dst0[2],
                                struct segment *dst1, size_t n_dst1[2],
                                int winding[2], int32_t y)
{
    n_dst0[0] = n_dst0[1] = 0;
    n_dst1[0] = n_dst1[1] = 0;
    if (n_src[0])
        polyline_split_vert_group(src, n_src[0], &dst0, &n_dst0[0],
                                  &dst1, &n_dst1[0], &winding[0], y);
    if (n_src[1])
        polyline_split_vert_group(src + n_src[0], n_src[1], &dst0, &n_dst0[1],
                                  &dst1, &n_dst1[1], &winding[1], y);
}


static inline void rasterizer_fill_solid(const BitmapEngine *engine,
                                         uint8_t *buf, int width, int height, ptrdiff_t stride,
                                         int set)
{
    assert(!(width  & ((1 << engine->tile_order) - 1)));
    assert(!(height & ((1 << engine->tile_order) - 1)));

    ptrdiff_t step = 1 << engine->tile_order;
    ptrdiff_t tile_stride = stride * (1 << engine->tile_order);
    width  >>= engine->tile_order;
    height >>= engine->tile_order;
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++)
            engine->fill_solid(buf + x * step, stride, set);
        buf += tile_stride;
    }
}

static inline void rasterizer_fill_halfplane(const BitmapEngine *engine,
                                             uint8_t *buf, int width, int height, ptrdiff_t stride,
                                             int32_t a, int32_t b, int64_t c, int32_t scale,
                                             bool single_tile_guaranteed)
{
    assert(!(width  & ((1 << engine->tile_order) - 1)));
    assert(!(height & ((1 << engine->tile_order) - 1)));
    if (single_tile_guaranteed &&
        width == 1 << engine->tile_order && height == 1 << engine->tile_order) {
        engine->fill_halfplane(buf, stride, a, b, c, scale);
        return;
    }

    uint32_t abs_a = a < 0 ? -a : a;
    uint32_t abs_b = b < 0 ? -b : b;
    int64_t size = (int64_t) (abs_a + abs_b) << (engine->tile_order + 5);
    int64_t offs = ((int64_t) a + b) * (1 << (engine->tile_order + 5));

    ptrdiff_t step = 1 << engine->tile_order;
    ptrdiff_t tile_stride = stride * (1 << engine->tile_order);
    width  >>= engine->tile_order;
    height >>= engine->tile_order;
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int64_t cc = c - (a * (int64_t) x + b * (int64_t) y) * (1 << (engine->tile_order + 6));
            int64_t offs_c = offs - cc;
            int64_t abs_c = offs_c < 0 ? -offs_c : offs_c;
            if (abs_c < size)
                engine->fill_halfplane(buf + x * step, stride, a, b, cc, scale);
            else
                engine->fill_solid(buf + x * step, stride,
                                   ((uint32_t) (offs_c >> 32) ^ scale) & 0x80000000);
        }
        buf += tile_stride;
    }
}

enum {
    FLAG_SOLID     = 1,
    FLAG_COMPLEX   = 2,
    FLAG_REVERSE   = 4,
    FLAG_GENERIC   = 8,
};

static inline int get_fill_flags(struct segment *line, size_t n_lines, int winding)
{
    if (!n_lines)
        return winding ? FLAG_SOLID : 0;
    if (n_lines > 1)
        return FLAG_COMPLEX | FLAG_GENERIC;

    static const int test = SEGFLAG_UL_DR | SEGFLAG_EXACT_LEFT;
    if (((line->flags & test) != test) == !(line->flags & SEGFLAG_DN))
        winding++;

    switch (winding) {
    case 0:
        return FLAG_COMPLEX | FLAG_REVERSE;
    case 1:
        return FLAG_COMPLEX;
    default:
        return FLAG_SOLID;
    }
}

/**
 * \brief Main quad-tree filling function
 * \param index index (0 or 1) of the input segment buffer (rst->linebuf)
 * \param offs current offset from the beginning of the buffer
 * \param winding bottom-left winding value
 * \return false on error
 * Rasterizes (possibly recursive) one quad-tree level.
 * Truncates used input buffer.
 */
static bool rasterizer_fill_level(const BitmapEngine *engine, RasterizerData *rst,
                                  uint8_t *buf, int width, int height, ptrdiff_t stride,
                                  int index, const size_t n_lines[2], const int winding[2],
                                  const ASS_Rect *clip, int x0, int y0)
{
    assert(width > 0 && height > 0);
    assert((unsigned) index < 2u && n_lines[0] + n_lines[1] <= rst->size[index]);
    assert(!(width  & ((1 << engine->tile_order) - 1)));
    assert(!(height & ((1 << engine->tile_order) - 1)));

    size_t offs = rst->size[index] - n_lines[0] - n_lines[1];
    if (clip && (x0 >= clip->x_max || y0 >= clip->y_max ||
                 x0 + width <= clip->x_min || y0 + height <= clip->y_min)) {
        rst->size[index] = offs;
        return true;
    }
    struct segment *line = rst->linebuf[index] + offs, *line1 = line + n_lines[0];
    int flags0 = get_fill_flags(line,  n_lines[0], winding[0]);
    int flags1 = get_fill_flags(line1, n_lines[1], winding[1]);
    int flags = (flags0 | flags1) ^ FLAG_COMPLEX;
    if (flags & (FLAG_SOLID | FLAG_COMPLEX)) {
        if (clip) {
            int cx0 = FFMAX(x0, clip->x_min);
            int cy0 = FFMAX(y0, clip->y_min);
            int cx1 = FFMIN(x0 + width, clip->x_max);
            int cy1 = FFMIN(y0 + height, clip->y_max);
            uint8_t *dst = buf + (ptrdiff_t) (cy0 - clip->y_min) * stride + (cx0 - clip->x_min);
            rasterizer_fill_solid(engine, dst, cx1 - cx0, cy1 - cy0, stride, flags & FLAG_SOLID);
        } else {
            rasterizer_fill_solid(engine, buf, width, height, stride, flags & FLAG_SOLID);
        }
        rst->size[index] = offs;
        return true;
    }
    if (!(flags & FLAG_GENERIC) && ((flags0 ^ flags1) & FLAG_COMPLEX)) {
        if (flags1 & FLAG_COMPLEX)
            line = line1;
        if (clip) {
            int cx0 = FFMAX(x0, clip->x_min);
            int cy0 = FFMAX(y0, clip->y_min);
            int cx1 = FFMIN(x0 + width, clip->x_max);
            int cy1 = FFMIN(y0 + height, clip->y_max);
            int64_t cc = line->c - ((int64_t) line->a * (cx0 - x0) +
                                    (int64_t) line->b * (cy0 - y0)) * 64;
            uint8_t *dst = buf + (ptrdiff_t) (cy0 - clip->y_min) * stride + (cx0 - clip->x_min);
            rasterizer_fill_halfplane(engine, dst, cx1 - cx0, cy1 - cy0, stride,
                                      line->a, line->b, cc,
                                      flags & FLAG_REVERSE ? -line->scale : line->scale,
                                      width == 1 << engine->tile_order &&
                                      height == 1 << engine->tile_order);
        } else {
            rasterizer_fill_halfplane(engine, buf, width, height, stride,
                                      line->a, line->b, line->c,
                                      flags & FLAG_REVERSE ? -line->scale : line->scale,
                                      true);
        }
        rst->size[index] = offs;
        return true;
    }
    if (width == 1 << engine->tile_order && height == 1 << engine->tile_order) {
        uint8_t *dst = clip ?
            buf + (ptrdiff_t) (y0 - clip->y_min) * stride + (x0 - clip->x_min) : buf;
        if (!(flags1 & FLAG_COMPLEX)) {
            engine->fill_generic(dst, stride, line, n_lines[0], winding[0]);
            rst->size[index] = offs;
            return true;
        }
        if (!(flags0 & FLAG_COMPLEX)) {
            engine->fill_generic(dst, stride, line1, n_lines[1], winding[1]);
            rst->size[index] = offs;
            return true;
        }
        if (flags0 & FLAG_GENERIC)
            engine->fill_generic(dst, stride, line, n_lines[0], winding[0]);
        else
            engine->fill_halfplane(dst, stride, line->a, line->b, line->c,
                                   flags0 & FLAG_REVERSE ? -line->scale : line->scale);
        if (flags1 & FLAG_GENERIC)
            engine->fill_generic(rst->tile, width, line1, n_lines[1], winding[1]);
        else
            engine->fill_halfplane(rst->tile, width, line1->a, line1->b, line1->c,
                                   flags1 & FLAG_REVERSE ? -line1->scale : line1->scale);
        engine->merge(dst, stride, rst->tile);
        rst->size[index] = offs;
        return true;
    }

    size_t offs1 = rst->size[index ^ 1];
    if (!check_capacity(rst, index ^ 1, n_lines[0] + n_lines[1]))
        return false;
    struct segment *dst0 = line;
    struct segment *dst1 = rst->linebuf[index ^ 1] + offs1;

    uint8_t *buf1 = buf;
    int width1  = width;
    int height1 = height;
    int x1 = x0, y1 = y0;
    size_t n_next0[2], n_next1[2];
    int winding1[2] = { winding[0], winding[1] };
    if (width > height) {
        width = 1 << ilog2(width - 1);
        width1 -= width;
        x1 += width;
        if (!clip)
            buf1 += width;
        if (clip) {
            if (x1 >= clip->x_max)
                dst1 = NULL;
            if (x0 + width <= clip->x_min)
                dst0 = NULL;
        }
        polyline_split_horz(line, n_lines,
                            dst0, n_next0, dst1, n_next1,
                            winding1, (int32_t) width << 6);
    } else {
        height = 1 << ilog2(height - 1);
        height1 -= height;
        y1 += height;
        if (!clip)
            buf1 += height * stride;
        if (clip) {
            if (y1 >= clip->y_max)
                dst1 = NULL;
            if (y0 + height <= clip->y_min)
                dst0 = NULL;
        }
        polyline_split_vert(line, n_lines,
                            dst0, n_next0, dst1, n_next1,
                            winding1, (int32_t) height << 6);
    }
    bool keep0 = dst0 != NULL;
    bool keep1 = dst1 != NULL;
    rst->size[index ^ 0] = offs  + (keep0 ? n_next0[0] + n_next0[1] : 0);
    rst->size[index ^ 1] = offs1 + (keep1 ? n_next1[0] + n_next1[1] : 0);

    if (keep0 && !rasterizer_fill_level(engine, rst, buf, width, height, stride,
                               index ^ 0, n_next0, winding, clip, x0, y0))
        return false;
    assert(rst->size[index ^ 0] == offs);
    if (keep1 && !rasterizer_fill_level(engine, rst, buf1, width1, height1, stride,
                               index ^ 1, n_next1, winding1, clip, x1, y1))
        return false;
    assert(rst->size[index ^ 1] == offs1);
    return true;
}

bool ass_rasterizer_fill(const BitmapEngine *engine, RasterizerData *rst,
                         uint8_t *buf, int x0, int y0,
                         int width, int height, ptrdiff_t stride,
                         const ASS_Rect *clip)
{
    assert(width > 0 && height > 0);
    assert(!(width  & ((1 << engine->tile_order) - 1)));
    assert(!(height & ((1 << engine->tile_order) - 1)));
    x0 *= 1 << 6;  y0 *= 1 << 6;

    struct segment *line = rst->linebuf[0];
    struct segment *end = line + rst->size[0];
#if defined(__ARM_NEON)
    int32_t shift_arr[4] = { x0, x0, y0, y0 };
    int32x4_t vshift = vld1q_s32(shift_arr);
    for (; line != end; line++) {
        int32x4_t bounds = vld1q_s32(&line->x_min);
        vst1q_s32(&line->x_min, vsubq_s32(bounds, vshift));
        line->c -= line->a * (int64_t) x0 + line->b * (int64_t) y0;
    }
#else
    for (; line != end; line++) {
        line->x_min -= x0;
        line->x_max -= x0;
        line->y_min -= y0;
        line->y_max -= y0;
        line->c -= line->a * (int64_t) x0 + line->b * (int64_t) y0;
    }
#endif
    rst->bbox.x_min -= x0;
    rst->bbox.x_max -= x0;
    rst->bbox.y_min -= y0;
    rst->bbox.y_max -= y0;

    if (!check_capacity(rst, 1, rst->size[0]))
        return false;

    size_t n_unused[2];
    size_t n_lines[2] = { rst->n_first, rst->size[0] - rst->n_first };
    int winding[2] = { 0, 0 };

    int32_t size_x = (int32_t) width << 6;
    int32_t size_y = (int32_t) height << 6;
    if (rst->bbox.x_max >= size_x) {
        polyline_split_horz(rst->linebuf[0], n_lines,
                            rst->linebuf[0], n_lines,
                            NULL, n_unused,
                            winding, size_x);
        winding[0] = winding[1] = 0;
    }
    if (rst->bbox.y_max >= size_y) {
        polyline_split_vert(rst->linebuf[0], n_lines,
                            rst->linebuf[0], n_lines,
                            NULL, n_unused,
                            winding, size_y);
        winding[0] = winding[1] = 0;
    }
    if (rst->bbox.x_min <= 0) {
        polyline_split_horz(rst->linebuf[0], n_lines,
                            NULL, n_unused,
                            rst->linebuf[0], n_lines,
                            winding, 0);
    }
    if (rst->bbox.y_min <= 0) {
        polyline_split_vert(rst->linebuf[0], n_lines,
                            NULL, n_unused,
                            rst->linebuf[0], n_lines,
                            winding, 0);
    }
    rst->size[0] = n_lines[0] + n_lines[1];
    rst->size[1] = 0;
    return rasterizer_fill_level(engine, rst,
                                 buf, width, height, stride,
                                 0, n_lines, winding, clip, 0, 0);
}
