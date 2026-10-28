#include "config.h"
#include "ass_outline.h"
#include "ass_rasterizer.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static uint32_t seed = 0x741932abu;

static unsigned random_number(void)
{
    seed ^= seed << 13;
    seed ^= seed >> 17;
    return seed ^= seed << 5;
}

static int coordinate(void)
{
    return ((int) (random_number() % 601) - 300) * 64;
}

static void validate(const ASS_Outline *outline)
{
    assert(outline->n_points <= outline->max_points);
    assert(outline->n_segments <= outline->max_segments);
    size_t count = 0;
    for (size_t i = 0; i < outline->n_segments; i++) {
        unsigned order = outline->segments[i] & OUTLINE_COUNT_MASK;
        assert(order >= 1 && order <= 3);
        count += order;
    }
    assert(count == outline->n_points);
    assert(!outline->n_segments ||
           (outline->segments[outline->n_segments - 1] & OUTLINE_CONTOUR_END));
}

static int winding(const ASS_Outline *outline, double x, double y, bool *boundary)
{
    size_t first = 0;
    int result = 0;
    for (size_t i = 0; i < outline->n_points; i++) {
        ASS_Vector a = outline->points[i];
        ASS_Vector b = outline->points[outline->segments[i] & OUTLINE_CONTOUR_END ? first : i + 1];
        double cross = ((double) b.x - a.x) * (y - a.y) -
                       ((double) b.y - a.y) * (x - a.x);
        if (fabs(cross) < 1e-6 &&
            x >= FFMIN(a.x, b.x) && x <= FFMAX(a.x, b.x) &&
            y >= FFMIN(a.y, b.y) && y <= FFMAX(a.y, b.y))
            *boundary = true;
        if (a.y <= y && b.y > y && cross > 0) result++;
        if (a.y > y && b.y <= y && cross < 0) result--;
        if (outline->segments[i] & OUTLINE_CONTOUR_END) first = i + 1;
    }
    return result;
}

int main(void)
{
    ASS_Outline source = {0}, output = {0};
    assert(ass_outline_alloc(&source, 128, 128));
    BitmapEngine engine = {.align_order = 4, .tile_order = 4};
    RasterizerData raster = {0};
    assert(ass_rasterizer_init(&engine, &raster, 16));
    unsigned long queries = 0;
    for (unsigned trial = 0; trial < 20000; trial++) {
        source.n_points = source.n_segments = 0;
        unsigned contours = 1 + random_number() % 4;
        bool curves = trial % 3 == 0;
        for (unsigned c = 0; c < contours; c++) {
            unsigned count = 3 + random_number() % 32;
            for (unsigned i = 0; i < count; i++) {
                unsigned order = curves ? 1 + random_number() % 3 : 1;
                for (unsigned j = 0; j < order; j++) {
                    ASS_Vector p = {coordinate(), coordinate()};
                    assert(ass_outline_add_point(&source, p, 0));
                }
                assert(ass_outline_add_segment(&source, order |
                    (i + 1 == count ? OUTLINE_CONTOUR_END : 0)));
            }
        }
        double m[2][3] = {
            {(trial & 1 ? -1 : 1) * (double) (1 + random_number() % 3), 0, coordinate()},
            {0, (trial & 2 ? -1 : 1) * (double) (1 + random_number() % 3), coordinate()}
        };
        if (trial < 3000) {
            assert(ass_rasterizer_set_outline(&raster, &source, false, NULL, 0));
            size_t original = raster.size[0];
            assert(ass_rasterizer_set_outline(&raster, &source, false, NULL, 16));
            assert(raster.size[0] <= original);
        }
        if (trial % 5 == 0) {
            m[0][1] = 1;
            m[1][0] = -1;
        }
        ASS_Rect clip = {-50, -40, 60, 70};
        assert(ass_outline_transform_2d_clipped(&output, &source, m, &clip));
        validate(&output);
        assert(output.n_points <= source.n_points);
        assert(output.n_segments <= source.n_segments);
        for (unsigned perspective = 0; perspective < 2; perspective++) {
            ASS_Outline reference = {0};
            double projection[3][3] = {
                {m[0][0], m[0][1], m[0][2]},
                {m[1][0], m[1][1], m[1][2]},
                {0.0001, -0.00005, trial % 7 == 0 ? -1 : 4}
            };
            bool expected = perspective ?
                ass_outline_transform_3d(&reference, &source, projection) :
                ass_outline_transform_2d(&reference, &source, m);
            bool actual = perspective ?
                ass_outline_transform_3d_reuse(&output, &source, projection) :
                ass_outline_transform_2d_reuse(&output, &source, m);
            assert(actual == expected);
            if (actual) {
                validate(&output);
                assert(reference.n_points == output.n_points);
                assert(reference.n_segments == output.n_segments);
                assert(!memcmp(reference.points, output.points,
                               reference.n_points * sizeof(ASS_Vector)));
                assert(!memcmp(reference.segments, output.segments, reference.n_segments));
            }
            ass_outline_free(&reference);
        }
        // Restore clipped output for the interior-winding comparison.
        assert(ass_outline_transform_2d_clipped(&output, &source, m, &clip));
        if (!curves) {
            ASS_Outline reference = {0};
            assert(ass_outline_transform_2d(&reference, &source, m));
            for (unsigned q = 0; q < 32; q++) {
                double x = ((int) (random_number() % 109) - 49) * 64 + 0.25;
                double y = ((int) (random_number() % 109) - 39) * 64 + 0.25;
                bool boundary = false;
                int a = winding(&reference, x, y, &boundary);
                int b = winding(&output, x, y, &boundary);
                assert(boundary || a == b);
                queries++;
            }
            ass_outline_free(&reference);
        }
        ASS_Outline empty = {0};
        assert(ass_outline_transform_2d_clipped(&output, &empty, m, &clip));
        assert(!output.n_points && !output.n_segments);
    }
    // Reject malformed contours without leaving stale reusable pointers.
    ASS_Vector point = {0};
    char flags = 0;
    ASS_Outline invalid = {.n_points = 1, .n_segments = 1, .points = &point, .segments = &flags};
    double identity[2][3] = {{1, 0, 0}, {0, 1, 0}};
    ASS_Rect clip = {-10, -10, 10, 10};
    assert(!ass_outline_transform_2d_clipped(&output, &invalid, identity, &clip));
    flags = OUTLINE_LINE_SEGMENT;
    assert(!ass_outline_transform_2d_clipped(&output, &invalid, identity, &clip));
    flags |= OUTLINE_CONTOUR_END;
    assert(ass_outline_transform_2d_clipped(&output, &invalid, identity, &clip));
    validate(&output);
    source.n_points = source.n_segments = 0;
    for (unsigned i = 0; i < 1024; i++) {
        double angle = i * 2 * M_PI / 1024;
        ASS_Vector p = {ass_lrint(8192 * cos(angle)), ass_lrint(8192 * sin(angle))};
        assert(ass_outline_add_point(&source, p, OUTLINE_LINE_SEGMENT |
            (i == 1023 ? OUTLINE_CONTOUR_END : 0)));
    }
    assert(ass_rasterizer_set_outline(&raster, &source, false, NULL, 16));
    assert(raster.size[0] >= 3 && raster.size[0] < source.n_points / 2);
    for (size_t i = 0; i < source.n_points; i++) {
        double nearest = INFINITY;
        for (size_t j = 0; j < raster.size[0]; j++) {
            struct segment *s = raster.linebuf[0] + j;
            double ax = s->b > 0 ? s->x_max : s->x_min;
            double ay = s->a < 0 ? s->y_max : s->y_min;
            double bx = s->b > 0 ? s->x_min : s->x_max;
            double by = s->a < 0 ? s->y_min : s->y_max;
            double dx = bx - ax, dy = by - ay;
            double t = ((source.points[i].x - ax) * dx +
                        (source.points[i].y - ay) * dy) / (dx * dx + dy * dy);
            t = FFMINMAX(t, 0, 1);
            nearest = FFMIN(nearest, hypot(source.points[i].x - ax - t * dx,
                                          source.points[i].y - ay - t * dy));
        }
        assert(nearest <= 23.0);
    }
    ass_outline_free(&source);
    ass_outline_free(&output);
    ass_rasterizer_done(&raster);
    printf("Validated 20000 outlines, %lu winding queries, 40000 exact reusable transforms, 3000 raster simplifications\n", queries);
    return 0;
}
