/*
 * Copyright (C) 2006 Evgeniy Stepanov <eugeni.stepanov@gmail.com>
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
#include <math.h>
#include <string.h>
#include <stdbool.h>
#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif
#ifdef ASS_EVENT_THREADS
#include <pthread.h>
#include <unistd.h>
struct ass_event_pool;
static void event_pool_done(struct ass_event_pool *pool);
#endif
#if defined(ASS_FLE_PROFILE) || defined(ASS_EVENT_THREADS)
#include <stdint.h>
#include <time.h>
static uint64_t fle_clock(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts))
        return 0;
    return (uint64_t) ts.tv_sec * 1000000000 + ts.tv_nsec;
}
#ifdef ASS_EVENT_THREADS
static uint64_t event_elapsed_ns(uint64_t start, uint64_t end)
{
    return !start || !end || end < start ? UINT64_MAX : end - start;
}
#endif
#endif
#ifdef ASS_FLE_PROFILE
#define FLE_START uint64_t fle_last = fle_clock()
#define FLE_PHASE(n) do { \
    uint64_t fle_now = fle_clock(); \
    render_priv->fle_phase_ns[n] += fle_now - fle_last; \
    fle_last = fle_now; \
} while (0)
#else
#define FLE_START
#define FLE_PHASE(n) ((void) 0)
#endif

#ifdef CONFIG_UNIBREAK
#include <linebreak.h>
#endif

#include "ass.h"
#include "ass_outline.h"
#include "ass_render.h"
#include "ass_parse.h"
#include "ass_priv.h"
#include "ass_shaper.h"

#define MAX_GLYPHS_INITIAL 1024
#define MAX_LINES_INITIAL 64
#define MAX_BITMAPS_INITIAL 16
#define MAX_SUB_BITMAPS_INITIAL 64
#define SUBPIXEL_MASK 63
#define STROKER_PRECISION 16     // stroker error in integer units, unrelated to final accuracy
#define RASTERIZER_PRECISION 16  // rasterizer spline approximation error in 1/64 pixel units
#define POSITION_PRECISION 8.0   // rough estimate of transform error in 1/64 pixel units
#define MAX_PERSP_SCALE 16.0
#define SUBPIXEL_ORDER 3  // ~ log2(64 / POSITION_PRECISION)
#define BLUR_PRECISION (1.0 / 256)  // blur error as fraction of full input range


static bool text_info_init(TextInfo* text_info)
{
    text_info->max_bitmaps = MAX_BITMAPS_INITIAL;
    text_info->max_glyphs = MAX_GLYPHS_INITIAL;
    text_info->max_lines = MAX_LINES_INITIAL;
    text_info->n_bitmaps = 0;
    text_info->combined_bitmaps = calloc(MAX_BITMAPS_INITIAL, sizeof(CombinedBitmapInfo));
    text_info->glyphs = calloc(MAX_GLYPHS_INITIAL, sizeof(GlyphInfo));
    text_info->event_text = calloc(MAX_GLYPHS_INITIAL, sizeof(FriBidiChar));
    text_info->breaks = malloc(MAX_GLYPHS_INITIAL);
    text_info->lines = calloc(MAX_LINES_INITIAL, sizeof(LineInfo));

    if (!text_info->combined_bitmaps || !text_info->glyphs || !text_info->lines ||
        !text_info->breaks || !text_info->event_text)
        return false;

    return true;
}

static void text_info_done(TextInfo* text_info)
{
    free(text_info->glyphs);
    free(text_info->event_text);
    free(text_info->breaks);
    free(text_info->lines);
    free(text_info->combined_bitmaps);
}

static bool render_context_init(RenderContext *state, ASS_Renderer *priv)
{
    state->renderer = priv;

    if (!text_info_init(&state->text_info))
        return false;

    if (!(state->shaper = ass_shaper_new(priv->cache.metrics_cache,
                                         priv->cache.face_size_metrics_cache,
                                         priv->cache.shape_cache)))
        return false;

    int tolerance = priv->experimental_edges >= 2 ? 4 * RASTERIZER_PRECISION :
                    (priv->experimental_edges ? 2 * RASTERIZER_PRECISION : RASTERIZER_PRECISION);
    return ass_rasterizer_init(&priv->engine, &state->rasterizer, tolerance);
}

static void render_context_done(RenderContext *state)
{
    ass_rasterizer_done(&state->rasterizer);

    if (state->shaper)
        ass_shaper_free(state->shaper);

    text_info_done(&state->text_info);
}

ASS_Renderer *ass_renderer_init(ASS_Library *library)
{
    int error;
    FT_Library ft;
    ASS_Renderer *priv = 0;
    int vmajor, vminor, vpatch;

    ass_msg(library, MSGL_INFO, "libass API version: 0x%X", LIBASS_VERSION);
    ass_msg(library, MSGL_INFO, "libass source: %s", CONFIG_SOURCEVERSION);

    error = FT_Init_FreeType(&ft);
    if (error) {
        ass_msg(library, MSGL_FATAL, "%s failed", "FT_Init_FreeType");
        goto fail;
    }

    FT_Library_Version(ft, &vmajor, &vminor, &vpatch);
    ass_msg(library, MSGL_V, "Raster: FreeType %d.%d.%d",
           vmajor, vminor, vpatch);

    priv = calloc(1, sizeof(ASS_Renderer));
    if (!priv) {
        FT_Done_FreeType(ft);
        goto fail;
    }

    priv->library = library;
    priv->ftlibrary = ft;
    const char *edges = getenv("LIBASS_EXPERIMENTAL_EDGES");
    priv->experimental_edges = edges ? (!strcmp(edges, "2") ? 2 : (!strcmp(edges, "1") ? 1 : 0)) : 0;
    if (priv->experimental_edges)
        ass_msg(library, MSGL_INFO, "Experimental ASS edge processing enabled (level %d)",
                priv->experimental_edges);
    // images_root and related stuff is zero-filled in calloc

    unsigned flags = ASS_CPU_FLAG_ALL;
#if CONFIG_LARGE_TILES
    flags |= ASS_FLAG_LARGE_TILES;
#endif
    priv->engine = ass_bitmap_engine_init(flags);

    priv->cache.font_cache = ass_font_cache_create();
    priv->cache.bitmap_cache = ass_bitmap_cache_create();
    priv->cache.composite_cache = ass_composite_cache_create();
    priv->cache.outline_cache = ass_outline_cache_create();
    priv->cache.face_size_metrics_cache = ass_face_size_metrics_cache_create();
    priv->cache.metrics_cache = ass_glyph_metrics_cache_create();
    priv->cache.shape_cache = ass_shape_cache_create();
    priv->cache.event_cache = ass_event_cache_create();
    if (!priv->cache.font_cache || !priv->cache.bitmap_cache ||
        !priv->cache.composite_cache || !priv->cache.outline_cache ||
        !priv->cache.face_size_metrics_cache || !priv->cache.metrics_cache ||
        !priv->cache.shape_cache || !priv->cache.event_cache)
        goto fail;

    priv->cache.glyph_max = GLYPH_CACHE_MAX;
    priv->cache.growth = 1;
    ass_cache_watch_returns(priv->cache.bitmap_cache, 13);
    ass_cache_watch_returns(priv->cache.composite_cache, 13);

    if (!render_context_init(&priv->state, priv))
        goto fail;

    priv->user_override_style.Name = "OverrideStyle"; // name insignificant

    priv->settings.font_size_coeff = 1.;
    priv->settings.selective_style_overrides = ASS_OVERRIDE_BIT_SELECTIVE_FONT_SCALE;

    ass_shaper_info(library);
    priv->settings.shaper = ASS_SHAPING_COMPLEX;

    ass_msg(library, MSGL_V, "Initialized");

    return priv;

fail:
    ass_msg(library, MSGL_ERR, "Initialization failed");
    ass_renderer_done(priv);

    return NULL;
}

void ass_renderer_done(ASS_Renderer *render_priv)
{
    if (!render_priv)
        return;

    ass_frame_unref(render_priv->images_root);
    ass_frame_unref(render_priv->prev_images_root);
#ifdef ASS_EVENT_THREADS
    event_pool_done(render_priv->event_pool);
    free(render_priv->font_config);
#endif

    render_context_done(&render_priv->state);

    // Before composite_cache, whose values cached events hold.
    ass_cache_done(render_priv->cache.event_cache);
    ass_cache_done(render_priv->cache.composite_cache);
    ass_cache_done(render_priv->cache.bitmap_cache);
    ass_cache_done(render_priv->cache.outline_cache);
    ass_cache_done(render_priv->cache.face_size_metrics_cache);
    ass_cache_done(render_priv->cache.metrics_cache);
    ass_cache_done(render_priv->cache.shape_cache);
    ass_cache_done(render_priv->cache.font_cache);

    if (render_priv->fontselect)
        ass_fontselect_free(render_priv->fontselect);
    if (render_priv->ftlibrary)
        FT_Done_FreeType(render_priv->ftlibrary);
    free(render_priv->eimg);

    ass_aligned_free(render_priv->blur_scratch);
    ass_aligned_free(render_priv->composite_scratch);
    ass_outline_free(&render_priv->raster_outline[0]);
    ass_outline_free(&render_priv->raster_outline[1]);

    free(render_priv->look);
    free(render_priv->next_look);
    free(render_priv->settings.default_font);
    free(render_priv->settings.default_family);

    free(render_priv->user_override_style.FontName);

    free(render_priv);
}

/**
 * \brief Create a new ASS_Image
 * Parameters are the same as ASS_Image fields.
 */
static ASS_Image *my_draw_bitmap(unsigned char *bitmap, int bitmap_w,
                                 int bitmap_h, int stride, int dst_x,
                                 int dst_y, uint32_t color,
                                 CompositeHashValue *source)
{
    ASS_ImagePriv *img = malloc(sizeof(ASS_ImagePriv));
    if (!img) {
        if (!source)
            ass_aligned_free(bitmap);
        return NULL;
    }

    img->result.w = bitmap_w;
    img->result.h = bitmap_h;
    img->result.stride = stride;
    img->result.bitmap = bitmap;
    img->result.color = color;
    img->result.dst_x = dst_x;
    img->result.dst_y = dst_y;

    img->source = source;
    ass_cache_inc_ref(source);
    img->buffer = source ? NULL : bitmap;
    img->ref_count = 0;

    return &img->result;
}

// A copy of `img` whose bitmap `holder`, a cache value, keeps alive.
static ASS_Image *clone_image(const ASS_Image *img, void *holder)
{
    ASS_ImagePriv *clone = malloc(sizeof(ASS_ImagePriv));
    if (!clone)
        return NULL;
    clone->result = *img;
    clone->result.next = NULL;
    clone->source = holder;
    ass_cache_inc_ref(holder);
    clone->buffer = NULL;
    clone->ref_count = 0;
    return &clone->result;
}

/**
 * \brief Mapping between script and screen coordinates
 */
static double x2scr_pos(ASS_Renderer *render_priv, double x)
{
    return x * render_priv->frame_content_width / render_priv->par_scale_x / render_priv->track->PlayResX +
        render_priv->settings.left_margin;
}
static double x2scr_left(RenderContext *state, double x)
{
    ASS_Renderer *render_priv = state->renderer;
    if (state->explicit || !render_priv->settings.use_margins)
        return x2scr_pos(render_priv, x);
    return x * render_priv->fit_width / render_priv->par_scale_x /
        render_priv->track->PlayResX;
}
static double x2scr_right(RenderContext *state, double x)
{
    ASS_Renderer *render_priv = state->renderer;
    if (state->explicit || !render_priv->settings.use_margins)
        return x2scr_pos(render_priv, x);
    return x * render_priv->fit_width / render_priv->par_scale_x /
        render_priv->track->PlayResX +
        (render_priv->width - render_priv->fit_width);
}
static double x2scr_pos_scaled(ASS_Renderer *render_priv, double x)
{
    return x * render_priv->frame_content_width / render_priv->track->PlayResX +
        render_priv->settings.left_margin;
}
/**
 * \brief Mapping between script and screen coordinates
 */
static double y2scr_pos(ASS_Renderer *render_priv, double y)
{
    return y * render_priv->frame_content_height / render_priv->track->PlayResY +
        render_priv->settings.top_margin;
}
static double y2scr(RenderContext *state, double y)
{
    ASS_Renderer *render_priv = state->renderer;
    if (state->explicit || !render_priv->settings.use_margins)
        return y2scr_pos(render_priv, y);
    return y * render_priv->fit_height /
        render_priv->track->PlayResY +
        (render_priv->height - render_priv->fit_height) * 0.5;
}

// the same for toptitles
static double y2scr_top(RenderContext *state, double y)
{
    ASS_Renderer *render_priv = state->renderer;
    if (state->explicit || !render_priv->settings.use_margins)
        return y2scr_pos(render_priv, y);
    return y * render_priv->fit_height /
        render_priv->track->PlayResY;
}
// the same for subtitles
static double y2scr_sub(RenderContext *state, double y)
{
    ASS_Renderer *render_priv = state->renderer;
    if (state->explicit || !render_priv->settings.use_margins)
        return y2scr_pos(render_priv, y);
    return y * render_priv->fit_height /
        render_priv->track->PlayResY +
        (render_priv->height - render_priv->fit_height);
}

/*
 * \brief Convert bitmap glyphs into ASS_Image list with inverse clipping
 *
 * Inverse clipping with the following strategy:
 * - find rectangle from (x0, y0) to (cx0, y1)
 * - find rectangle from (cx0, y0) to (cx1, cy0)
 * - find rectangle from (cx0, cy1) to (cx1, y1)
 * - find rectangle from (cx1, y0) to (x1, y1)
 * These rectangles can be invalid and in this case are discarded.
 * Afterwards, they are clipped against the screen coordinates.
 * In an additional pass, the rectangles need to be split up left/right for
 * karaoke effects.  This can result in a lot of bitmaps (6 to be exact).
 */
static ASS_Image **render_glyph_i(RenderContext *state,
                                  Bitmap *bm, int dst_x, int dst_y,
                                  uint32_t color, uint32_t color2, int brk,
                                  ASS_Image **tail, unsigned type,
                                  CompositeHashValue *source)
{
    ASS_Renderer *render_priv = state->renderer;
    int i, j, x0, y0, x1, y1, cx0, cy0, cx1, cy1, sx, sy, zx, zy;
    Rect r[4];
    ASS_Image *img;

    dst_x += bm->left;
    dst_y += bm->top;
    brk -= dst_x;

    // we still need to clip against screen boundaries
    zx = x2scr_pos_scaled(render_priv, 0);
    zy = y2scr_pos(render_priv, 0);
    sx = x2scr_pos_scaled(render_priv, render_priv->track->PlayResX);
    sy = y2scr_pos(render_priv, render_priv->track->PlayResY);

    x0 = 0;
    y0 = 0;
    x1 = bm->w;
    y1 = bm->h;
    cx0 = state->clip_x0 - dst_x;
    cy0 = state->clip_y0 - dst_y;
    cx1 = state->clip_x1 - dst_x;
    cy1 = state->clip_y1 - dst_y;

    // calculate rectangles and discard invalid ones while we're at it.
    i = 0;
    r[i].x0 = x0;
    r[i].y0 = y0;
    r[i].x1 = (cx0 > x1) ? x1 : cx0;
    r[i].y1 = y1;
    if (r[i].x1 > r[i].x0 && r[i].y1 > r[i].y0) i++;
    r[i].x0 = (cx0 < 0) ? x0 : cx0;
    r[i].y0 = y0;
    r[i].x1 = (cx1 > x1) ? x1 : cx1;
    r[i].y1 = (cy0 > y1) ? y1 : cy0;
    if (r[i].x1 > r[i].x0 && r[i].y1 > r[i].y0) i++;
    r[i].x0 = (cx0 < 0) ? x0 : cx0;
    r[i].y0 = (cy1 < 0) ? y0 : cy1;
    r[i].x1 = (cx1 > x1) ? x1 : cx1;
    r[i].y1 = y1;
    if (r[i].x1 > r[i].x0 && r[i].y1 > r[i].y0) i++;
    r[i].x0 = (cx1 < 0) ? x0 : cx1;
    r[i].y0 = y0;
    r[i].x1 = x1;
    r[i].y1 = y1;
    if (r[i].x1 > r[i].x0 && r[i].y1 > r[i].y0) i++;

    // clip each rectangle to screen coordinates
    for (j = 0; j < i; j++) {
        r[j].x0 = (r[j].x0 + dst_x < zx) ? zx - dst_x : r[j].x0;
        r[j].y0 = (r[j].y0 + dst_y < zy) ? zy - dst_y : r[j].y0;
        r[j].x1 = (r[j].x1 + dst_x > sx) ? sx - dst_x : r[j].x1;
        r[j].y1 = (r[j].y1 + dst_y > sy) ? sy - dst_y : r[j].y1;
    }

    // draw the rectangles
    for (j = 0; j < i; j++) {
        int lbrk = brk;
        // kick out rectangles that are invalid now
        if (r[j].x1 <= r[j].x0 || r[j].y1 <= r[j].y0)
            continue;
        // split up into left and right for karaoke, if needed
        if (lbrk > r[j].x0) {
            if (lbrk > r[j].x1) lbrk = r[j].x1;
            img = my_draw_bitmap(bm->buffer + r[j].y0 * bm->stride + r[j].x0,
                                 lbrk - r[j].x0, r[j].y1 - r[j].y0, bm->stride,
                                 dst_x + r[j].x0, dst_y + r[j].y0, color, source);
            if (!img) break;
            img->type = type;
            *tail = img;
            tail = &img->next;
        }
        if (lbrk < r[j].x1) {
            if (lbrk < r[j].x0) lbrk = r[j].x0;
            img = my_draw_bitmap(bm->buffer + r[j].y0 * bm->stride + lbrk,
                                 r[j].x1 - lbrk, r[j].y1 - r[j].y0, bm->stride,
                                 dst_x + lbrk, dst_y + r[j].y0, color2, source);
            if (!img) break;
            img->type = type;
            *tail = img;
            tail = &img->next;
        }
    }

    return tail;
}

/**
 * \brief convert bitmap glyph into ASS_Image struct(s)
 * \param bit freetype bitmap glyph, FT_PIXEL_MODE_GRAY
 * \param dst_x bitmap x coordinate in video frame
 * \param dst_y bitmap y coordinate in video frame
 * \param color first color, RGBA
 * \param color2 second color, RGBA
 * \param brk x coordinate relative to glyph origin, color is used to the left of brk, color2 - to the right
 * \param tail pointer to the last image's next field, head of the generated list should be stored here
 * \return pointer to the new list tail
 * Performs clipping. Uses my_draw_bitmap for actual bitmap conversion.
 */
static ASS_Image **
render_glyph(RenderContext *state, Bitmap *bm, int dst_x, int dst_y,
             uint32_t color, uint32_t color2, int brk, ASS_Image **tail,
             unsigned type, CompositeHashValue *source)
{
    // Inverse clipping in use?
    if (state->clip_mode)
        return render_glyph_i(state, bm, dst_x, dst_y, color, color2,
                              brk, tail, type, source);

    // brk is absolute
    // color = color left of brk
    // color2 = color right of brk
    int b_x0, b_y0, b_x1, b_y1; // visible part of the bitmap
    int clip_x0, clip_y0, clip_x1, clip_y1;
    int tmp;
    ASS_Image *img;
    ASS_Renderer *render_priv = state->renderer;

    dst_x += bm->left;
    dst_y += bm->top;
    brk -= dst_x;

    // clipping
    clip_x0 = FFMINMAX(state->clip_x0, 0, render_priv->width);
    clip_y0 = FFMINMAX(state->clip_y0, 0, render_priv->height);
    clip_x1 = FFMINMAX(state->clip_x1, 0, render_priv->width);
    clip_y1 = FFMINMAX(state->clip_y1, 0, render_priv->height);
    b_x0 = 0;
    b_y0 = 0;
    b_x1 = bm->w;
    b_y1 = bm->h;

    tmp = dst_x - clip_x0;
    if (tmp < 0)
        b_x0 = -tmp;
    tmp = dst_y - clip_y0;
    if (tmp < 0)
        b_y0 = -tmp;
    tmp = clip_x1 - dst_x - bm->w;
    if (tmp < 0)
        b_x1 = bm->w + tmp;
    tmp = clip_y1 - dst_y - bm->h;
    if (tmp < 0)
        b_y1 = bm->h + tmp;

    if ((b_y0 >= b_y1) || (b_x0 >= b_x1))
        return tail;

    if (brk > b_x0) {           // draw left part
        if (brk > b_x1)
            brk = b_x1;
        img = my_draw_bitmap(bm->buffer + bm->stride * b_y0 + b_x0,
                             brk - b_x0, b_y1 - b_y0, bm->stride,
                             dst_x + b_x0, dst_y + b_y0, color, source);
        if (!img) return tail;
        img->type = type;
        *tail = img;
        tail = &img->next;
    }
    if (brk < b_x1) {           // draw right part
        if (brk < b_x0)
            brk = b_x0;
        img = my_draw_bitmap(bm->buffer + bm->stride * b_y0 + brk,
                             b_x1 - brk, b_y1 - b_y0, bm->stride,
                             dst_x + brk, dst_y + b_y0, color2, source);
        if (!img) return tail;
        img->type = type;
        *tail = img;
        tail = &img->next;
    }
    return tail;
}

static bool quantize_transform(double m[3][3], ASS_Vector *pos,
                               ASS_DVector *offset, bool first,
                               int pos_shift, BitmapHashKey *key)
{
    // Full transform:
    // x_out = (m_xx * x + m_xy * y + m_xz) / z,
    // y_out = (m_yx * x + m_yy * y + m_yz) / z,
    // z     =  m_zx * x + m_zy * y + m_zz.

    const double max_val = 1000000;

    const ASS_Rect *bbox = &key->outline->cbox;
    double x0 = (bbox->x_min + bbox->x_max) / 2.0;
    double y0 = (bbox->y_min + bbox->y_max) / 2.0;
    double dx = (bbox->x_max - bbox->x_min) / 2.0 + 64;
    double dy = (bbox->y_max - bbox->y_min) / 2.0 + 64;

    // Change input coordinates' origin to (x0, y0),
    // after that transformation x:[-dx, dx], y:[-dy, dy],
    // max|x| = dx and max|y| = dy.
    for (int i = 0; i < 3; i++)
        m[i][2] += m[i][0] * x0 + m[i][1] * y0;

    if (m[2][2] <= 0)
        return false;

    double w = 1 / m[2][2];
    // Transformed center of bounding box
    double center[2] = { m[0][2] * w, m[1][2] * w };
    // Change output coordinates' origin to center,
    // m_xz and m_yz is skipped as it becomes 0 and no longer needed.
    for (int i = 0; i < 2; i++)
        for (int j = 0; j < 2; j++)
            m[i][j] -= m[2][j] * center[i];

    double delta[2] = {0};
    if (!first) {
        delta[0] = offset->x;
        delta[1] = offset->y;
    }

    int32_t qr[2];  // quantized center position
    for (int i = 0; i < 2; i++) {
        center[i] /= 64 >> SUBPIXEL_ORDER;
        center[i] -= delta[i];
        if (!(fabs(center[i]) < max_val))
            return false;
        qr[i] = ass_lrint(center[i]);
    }

    // Minimal bounding box z coordinate
    double z0 = m[2][2] - fabs(m[2][0]) * dx - fabs(m[2][1]) * dy;
    // z0 clamped to z_center / MAX_PERSP_SCALE to mitigate problems with small z
    w = 1.0 / POSITION_PRECISION / FFMAX(z0, m[2][2] / MAX_PERSP_SCALE);
    double mul[2] = { dx * w, dy * w };  // 1 / q_x, 1 / q_y

    // z0 = m_zz - |m_zx| * dx - |m_zy| * dy,
    // m_zz = z0 + |m_zx| * dx + |m_zy| * dy,
    // z = m_zx * x + m_zy * y + m_zz
    //  = m_zx * (x + sign(m_zx) * dx) + m_zy * (y + sign(m_zy) * dy) + z0.

    // Let D(f) denote the absolute error of a quantity f.
    // Our goal is to determine tolerable error for matrix coefficients,
    // so that the total error of the output x_out, y_out is still acceptable.
    // As glyph dimensions are usually larger than a couple of pixels, errors
    // will be relatively small and we can use first order approximation.

    // z0 is effectively a scale factor and can thus be treated as a constant.
    // Error of constants is obviously zero, so:  D(dx) = D(dy) = D(z0) = 0.
    // For arbitrary quantities A, B, C with C not zero, the following holds true:
    //   D(A * B) <= D(A) * max|B| + max|A| * D(B),
    //   D(1 / C) <= D(C) * max|1 / C^2|.
    // Write ~ for 'same magnitude' and ~= for 'approximately'.

    // D(x_out) = D((m_xx * x + m_xy * y) / z)
    //  <= D(m_xx * x + m_xy * y) * max|1 / z| + max|m_xx * x + m_xy * y| * D(1 / z)
    //  <= (D(m_xx) * dx + D(m_xy) * dy) / z0 + (|m_xx| * dx + |m_xy| * dy) * D(z) / z0^2,
    // D(y_out) = D((m_yx * x + m_yy * y) / z)
    //  <= D(m_yx * x + m_yy * y) * max|1 / z| + max|m_yx * x + m_yy * y| * D(1 / z)
    //  <= (D(m_yx) * dx + D(m_yy) * dy) / z0 + (|m_yx| * dx + |m_yy| * dy) * D(z) / z0^2,
    // |m_xx| * dx + |m_xy| * dy = x_lim,
    // |m_yx| * dx + |m_yy| * dy = y_lim,
    // D(z) <= 2 * (D(m_zx) * dx + D(m_zy) * dy),
    // D(x_out) <= (D(m_xx) * dx + D(m_xy) * dy) / z0
    //       + 2 * (D(m_zx) * dx + D(m_zy) * dy) * x_lim / z0^2,
    // D(y_out) <= (D(m_yx) * dx + D(m_yy) * dy) / z0
    //       + 2 * (D(m_zx) * dx + D(m_zy) * dy) * y_lim / z0^2.

    // To estimate acceptable error in a matrix coefficient, pick ACCURACY for this substep,
    // set error in all other coefficients to zero and solve the system
    // D(x_out) <= ACCURACY, D(y_out) <= ACCURACY for desired D(m_ij).
    // Note that ACCURACY isn't equal to total error.
    // Total error is larger than each ACCURACY, but still of the same magnitude.
    // Via our choice of ACCURACY, we get a total error of up to several POSITION_PRECISION.

    // Quantization steps (pick: ACCURACY = POSITION_PRECISION):
    // D(m_xx), D(m_yx) ~ q_x = POSITION_PRECISION * z0 / dx,
    // D(m_xy), D(m_yy) ~ q_y = POSITION_PRECISION * z0 / dy,
    // qm_xx = round(m_xx / q_x), qm_xy = round(m_xy / q_y),
    // qm_yx = round(m_yx / q_x), qm_yy = round(m_yy / q_y).

    int32_t qm[3][2];
    for (int i = 0; i < 2; i++)
        for (int j = 0; j < 2; j++) {
            double val = m[i][j] * mul[j];
            if (!(fabs(val) < max_val))
                return false;
            qm[i][j] = ass_lrint(val);
        }

    // x_lim = |m_xx| * dx + |m_xy| * dy
    //  ~= |qm_xx| * q_x * dx + |qm_xy| * q_y * dy
    //  = (|qm_xx| + |qm_xy|) * POSITION_PRECISION * z0,
    // y_lim = |m_yx| * dx + |m_yy| * dy
    //  ~= |qm_yx| * q_x * dx + |qm_yy| * q_y * dy
    //  = (|qm_yx| + |qm_yy|) * POSITION_PRECISION * z0,
    // max(x_lim, y_lim) / z0 ~= w
    //  = max(|qm_xx| + |qm_xy|, |qm_yx| + |qm_yy|) * POSITION_PRECISION.

    // Quantization steps (pick: ACCURACY = 2 * POSITION_PRECISION):
    // D(m_zx) ~ POSITION_PRECISION * z0^2 / max(x_lim, y_lim) / dx ~= q_zx = q_x / w,
    // D(m_zy) ~ POSITION_PRECISION * z0^2 / max(x_lim, y_lim) / dy ~= q_zy = q_y / w,
    // qm_zx = round(m_zx / q_zx), qm_zy = round(m_zy / q_zy).

    int32_t qmx = abs(qm[0][0]) + abs(qm[0][1]);
    int32_t qmy = abs(qm[1][0]) + abs(qm[1][1]);
    w = POSITION_PRECISION * FFMAX(qmx, qmy);
    mul[0] *= w;
    mul[1] *= w;

    for (int j = 0; j < 2; j++) {
        double val = m[2][j] * mul[j];
        if (!(fabs(val) < max_val))
            return false;
        qm[2][j] = ass_lrint(val);
    }

    if (pos_shift) {
        int32_t step = 1 << pos_shift;
        qr[0] = (qr[0] + (step >> 1)) & ~(step - 1);
        qr[1] = (qr[1] + (step >> 1)) & ~(step - 1);
    }
    if (first && offset) {
        offset->x = center[0] - qr[0];
        offset->y = center[1] - qr[1];
    }
    *pos = (ASS_Vector) {
        .x = qr[0] >> SUBPIXEL_ORDER,
        .y = qr[1] >> SUBPIXEL_ORDER,
    };
    key->offset.x = qr[0] & ((1 << SUBPIXEL_ORDER) - 1);
    key->offset.y = qr[1] & ((1 << SUBPIXEL_ORDER) - 1);
    key->matrix_x.x = qm[0][0];  key->matrix_x.y = qm[0][1];
    key->matrix_y.x = qm[1][0];  key->matrix_y.y = qm[1][1];
    key->matrix_z.x = qm[2][0];  key->matrix_z.y = qm[2][1];
    return true;
}

static void restore_transform(double m[3][3], const BitmapHashKey *key)
{
    const ASS_Rect *bbox = &key->outline->cbox;
    double x0 = (bbox->x_min + bbox->x_max) / 2.0;
    double y0 = (bbox->y_min + bbox->y_max) / 2.0;
    double dx = (bbox->x_max - bbox->x_min) / 2.0 + 64;
    double dy = (bbox->y_max - bbox->y_min) / 2.0 + 64;

    // Arbitrary scale has chosen so that z0 = 1
    double q_x = POSITION_PRECISION / dx;
    double q_y = POSITION_PRECISION / dy;
    m[0][0] = key->matrix_x.x * q_x;
    m[0][1] = key->matrix_x.y * q_y;
    m[1][0] = key->matrix_y.x * q_x;
    m[1][1] = key->matrix_y.y * q_y;

    int32_t qmx = abs(key->matrix_x.x) + abs(key->matrix_x.y);
    int32_t qmy = abs(key->matrix_y.x) + abs(key->matrix_y.y);
    double scale_z = 1.0 / POSITION_PRECISION / FFMAX(qmx, qmy);
    m[2][0] = key->matrix_z.x * q_x * scale_z;  // qm_zx * q_zx
    m[2][1] = key->matrix_z.y * q_y * scale_z;  // qm_zy * q_zy

    m[0][2] = m[1][2] = 0;
    m[2][2] = 1 + fabs(m[2][0]) * dx + fabs(m[2][1]) * dy;
    m[2][2] = FFMIN(m[2][2], MAX_PERSP_SCALE);

    double center[2] = {
        key->offset.x * (64 >> SUBPIXEL_ORDER),
        key->offset.y * (64 >> SUBPIXEL_ORDER),
    };
    for (int i = 0; i < 2; i++)
        for (int j = 0; j < 3; j++)
            m[i][j] += m[2][j] * center[i];

    for (int i = 0; i < 3; i++)
        m[i][2] -= m[i][0] * x0 + m[i][1] * y0;
}

// Calculate bitmap memory footprint
static inline size_t bitmap_size(const Bitmap *bm)
{
    return bm->stride * bm->h;
}

/**
 * Iterate through a list of bitmaps and blend with clip vector, if
 * applicable. The blended bitmaps are added to a free list which is freed
 * at the start of a new frame.
 */
static Bitmap *get_vector_clip(RenderContext *state, ASS_Vector *pos,
                               const ASS_Rect *target_box)
{
    if (!state->clip_drawing_text.str)
        return NULL;

    ASS_Renderer *render_priv = state->renderer;

    OutlineHashKey ol_key;
    ol_key.type = OUTLINE_DRAWING;
    ol_key.u.drawing.text = state->clip_drawing_text;

    double m[3][3] = {{0}};
    int32_t scale_base = lshiftwrapi(1, state->clip_drawing_scale - 1);
    double w = scale_base > 0 ? (1.0 / scale_base) : 0;
    m[0][0] = state->screen_scale_x * w;
    m[1][1] = state->screen_scale_y * w;
    m[2][2] = 1;

    m[0][2] = int_to_d6(render_priv->settings.left_margin);
    m[1][2] = int_to_d6(render_priv->settings.top_margin);

    BitmapHashKey key = {0};
    key.outline = ass_cache_get(render_priv->cache.outline_cache, &ol_key, render_priv);
    if (!key.outline || !key.outline->valid ||
            !quantize_transform(m, pos, NULL, true,
                                render_priv->experimental_edges >= 2 ? SUBPIXEL_ORDER : 0, &key))
        return NULL;

    // Bound vector-clip rasterization to the screen (and quantized target
    // image box when provided). Pixels outside this box are never read.
    if (render_priv->width > 0 && render_priv->height > 0) {
        int32_t cx0 = 0, cy0 = 0;
        int32_t cx1 = render_priv->width, cy1 = render_priv->height;
        if (target_box) {
            int32_t qx0 = target_box->x_min & ~63;
            int32_t qy0 = target_box->y_min & ~63;
            int32_t qx1 = (target_box->x_max + 63) & ~63;
            int32_t qy1 = (target_box->y_max + 63) & ~63;
            cx0 = FFMAX(cx0, qx0);
            cy0 = FFMAX(cy0, qy0);
            cx1 = FFMIN(cx1, qx1);
            cy1 = FFMIN(cy1, qy1);
        }
        int64_t x0 = (int64_t) cx0 - pos->x;
        int64_t y0 = (int64_t) cy0 - pos->y;
        int64_t x1 = (int64_t) cx1 - pos->x;
        int64_t y1 = (int64_t) cy1 - pos->y;
        if (x1 > x0 && y1 > y0 &&
            x0 >= INT32_MIN && y0 >= INT32_MIN &&
            x1 <= INT32_MAX && y1 <= INT32_MAX) {
            key.clip_min = (ASS_Vector) {x0, y0};
            key.clip_max = (ASS_Vector) {x1, y1};
            key.clip_align = 1 << render_priv->engine.align_order;
        }
    }

    return ass_cache_get(render_priv->cache.bitmap_cache, &key, state);
}

static ASS_Image *ass_free_image(ASS_Image *img);

static bool clip_mask_preserves_src(const uint8_t *src, ptrdiff_t src_stride,
                                    const uint8_t *mask, ptrdiff_t mask_stride,
                                    int w, int h, bool inverse)
{
    for (int y = 0; y < h; y++) {
        const uint8_t *s = src + (ptrdiff_t) y * src_stride;
        const uint8_t *m = mask + (ptrdiff_t) y * mask_stride;
        int x = 0;
#if defined(__ARM_NEON)
        uint8x16_t acc = vdupq_n_u8(0);
        if (!inverse) {
            for (; x + 16 <= w; x += 16)
                acc = vorrq_u8(acc, vminq_u8(vld1q_u8(s + x), vmvnq_u8(vld1q_u8(m + x))));
        } else {
            for (; x + 16 <= w; x += 16)
                acc = vorrq_u8(acc, vminq_u8(vld1q_u8(s + x), vld1q_u8(m + x)));
        }
        uint64x2_t u = vreinterpretq_u64_u8(acc);
        if (vgetq_lane_u64(u, 0) | vgetq_lane_u64(u, 1))
            return false;
#endif
        if (!inverse) {
            for (; x < w; x++)
                if (s[x] && m[x] != 255)
                    return false;
        } else {
            for (; x < w; x++)
                if (s[x] && m[x])
                    return false;
        }
    }
    return true;
}

// Rotated lettering can occupy a thin diagonal of a large transparent bitmap.
// Keep at most eight horizontal strips, and only split when it saves enough
// masking and upload work to pay for the extra draw calls.
static ASS_Image *clip_sparse_strips(const BitmapEngine *engine, ASS_Image *cur,
                                     int x, int y, int w, int h, int left, int top,
                                     const uint8_t *mask, ptrdiff_t mask_stride)
{
    if (w < 192 || h < 96 || (int64_t) w * h < 65536)
        return NULL;
    ASS_Rect strips[8];
    int n = 0;
    int64_t area = 0;
    int band = FFMAX(64, (h - 1) / 8 + 1);
    const uint8_t *src = cur->bitmap + (ptrdiff_t) y * cur->stride + x;
    for (int row = 0; row < h;) {
        int end = row + FFMIN(band, h - row);
        int lo = w, hi = 0;
        for (int j = row; j < end && (lo || hi != w); j++) {
            const uint8_t *p = src + (ptrdiff_t) j * cur->stride;
            int a = 0, b = w;
            uint64_t word;
            while (a + 8 <= lo) {
                memcpy(&word, p + a, 8);
                if (word) break;
                a += 8;
            }
            while (a < lo && !p[a]) a++;
            if (a == w) continue;
            while (b - 8 >= FFMAX(hi, a)) {
                memcpy(&word, p + b - 8, 8);
                if (word) break;
                b -= 8;
            }
            while (b > hi && b > a && !p[b-1]) b--;
            lo = FFMIN(lo, a);
            hi = FFMAX(hi, b);
        }
        if (hi > lo) {
            strips[n++] = (ASS_Rect) {lo, row, hi, end};
            area += (int64_t) (hi - lo) * (end - row);
        }
        row = end;
    }
    if (!n) {
        cur->w = cur->h = cur->stride = 0;
        return cur;
    }
    if (area > (int64_t) w * h / 2)
        return NULL;

    ASS_ImagePriv *priv = (ASS_ImagePriv *) cur;
    ASS_Image *first = NULL, *last = NULL;
    unsigned align = 1U << engine->align_order;
    for (int i = 0; i < n; i++) {
        ASS_Rect r = strips[i];
        int sw = r.x_max - r.x_min, sh = r.y_max - r.y_min;
        const uint8_t *s_ptr = src + (ptrdiff_t) r.y_min * cur->stride + r.x_min;
        const uint8_t *m_ptr = mask + (ptrdiff_t) r.y_min * mask_stride + r.x_min;
        ASS_Image *img;
        if (priv->source &&
            clip_mask_preserves_src(s_ptr, cur->stride, m_ptr, mask_stride, sw, sh, false)) {
            img = my_draw_bitmap((unsigned char *) s_ptr, sw, sh, cur->stride,
                                 left + r.x_min, top + r.y_min, cur->color, priv->source);
        } else {
            unsigned stride = ass_align(align, sw);
            uint8_t *buf = ass_aligned_alloc(align, (size_t) stride * sh + align, false);
            img = buf ? my_draw_bitmap(buf, sw, sh, stride, left + r.x_min,
                                       top + r.y_min, cur->color, NULL) : NULL;
            if (img)
                engine->mul_bitmaps(buf, stride, s_ptr, cur->stride, m_ptr, mask_stride, sw, sh);
        }
        if (!img) {
            while (first) first = ass_free_image(first);
            return NULL;
        }
        img->type = cur->type;
        img->next = NULL;
        if (last) last->next = img;
        else first = img;
        last = img;
    }
    last->next = cur->next;
    ASS_ImagePriv *replacement = (ASS_ImagePriv *) first;
    ass_cache_dec_ref(priv->source);
    ass_aligned_free(priv->buffer);
    *priv = *replacement;
    if (last == first) last = cur;
    free(replacement);
    return last;
}

static void blend_vector_clip(RenderContext *state, ASS_Image *head)
{
    ASS_Rect head_box;
    rectangle_reset(&head_box);
    for (ASS_Image *cur = head; cur; cur = cur->next) {
        if (cur->w > 0 && cur->h > 0)
            rectangle_update(&head_box, cur->dst_x, cur->dst_y,
                             cur->dst_x + cur->w, cur->dst_y + cur->h);
    }
    if (head_box.x_min >= head_box.x_max || head_box.y_min >= head_box.y_max)
        return;

    ASS_Vector pos;
    Bitmap *clip_bm = get_vector_clip(state, &pos, &head_box);
    if (!clip_bm)
        return;
    if (!clip_bm->buffer || !clip_bm->w || !clip_bm->h) {
        if (!state->clip_drawing_mode) {
            for (ASS_Image *cur = head; cur; cur = cur->next)
                cur->w = cur->h = cur->stride = 0;
        }
        return;
    }
    ASS_Renderer *render_priv = state->renderer;

    // Iterate through bitmaps and blend/clip them
    for (ASS_Image *cur = head; cur; cur = cur->next) {
        int left, top, right, bottom, w, h;
        int ax, ay, aw, ah, as;
        int bx, by, bw, bh, bs;
        int aleft, atop, bleft, btop;
        unsigned char *abuffer, *bbuffer, *nbuffer;

        abuffer = cur->bitmap;
        bbuffer = clip_bm->buffer;
        ax = cur->dst_x;
        ay = cur->dst_y;
        aw = cur->w;
        ah = cur->h;
        as = cur->stride;
        bx = pos.x + clip_bm->left;
        by = pos.y + clip_bm->top;
        bw = clip_bm->w;
        bh = clip_bm->h;
        bs = clip_bm->stride;

        // Calculate overlap coordinates
        left = (ax > bx) ? ax : bx;
        top = (ay > by) ? ay : by;
        right = ((ax + aw) < (bx + bw)) ? (ax + aw) : (bx + bw);
        bottom = ((ay + ah) < (by + bh)) ? (ay + ah) : (by + bh);
        aleft = left - ax;
        atop = top - ay;
        w = right - left;
        h = bottom - top;
        bleft = left - bx;
        btop = top - by;

        unsigned align = 1 << render_priv->engine.align_order;
        if (state->clip_drawing_mode) {
            // Inverse clip
            if (ax + aw < bx || ay + ah < by || ax > bx + bw ||
                ay > by + bh || !h || !w) {
                continue;
            }
            if (clip_mask_preserves_src(abuffer + (ptrdiff_t) atop * as + aleft, as,
                                        bbuffer + (ptrdiff_t) btop * bs + bleft, bs,
                                        w, h, true))
                continue;

            // Allocate new buffer and add to free list
            nbuffer = ass_aligned_alloc(align, as * ah + align, false);
            if (!nbuffer)
                break;

            // Blend together
            memcpy(nbuffer, abuffer, ((ah - 1) * as) + aw);
            render_priv->engine.imul_bitmaps(nbuffer + atop * as + aleft, as,
                                             bbuffer + btop * bs + bleft, bs,
                                             w, h);
        } else {
            // Regular clip
            if (ax + aw < bx || ay + ah < by || ax > bx + bw ||
                ay > by + bh || !h || !w) {
                cur->w = cur->h = cur->stride = 0;
                continue;
            }

            // Allocate new buffer and add to free list
            unsigned ns = ass_align(align, w);
            ASS_Image *last = clip_sparse_strips(&render_priv->engine, cur, aleft, atop,
                                                 w, h, left, top,
                                                 bbuffer + (ptrdiff_t) btop * bs + bleft, bs);
            if (last) {
                cur = last;
                continue;
            }
            if (clip_mask_preserves_src(abuffer + (ptrdiff_t) atop * as + aleft, as,
                                        bbuffer + (ptrdiff_t) btop * bs + bleft, bs,
                                        w, h, false)) {
                cur->bitmap = abuffer + (ptrdiff_t) atop * as + aleft;
                cur->dst_x += aleft;
                cur->dst_y += atop;
                cur->w = w;
                cur->h = h;
                continue;
            }
            nbuffer = ass_aligned_alloc(align, ns * h + align, false);
            if (!nbuffer)
                break;

            // Blend together
            render_priv->engine.mul_bitmaps(nbuffer, ns,
                                            abuffer + atop * as + aleft, as,
                                            bbuffer + btop * bs + bleft, bs,
                                            w, h);
            cur->dst_x += aleft;
            cur->dst_y += atop;
            cur->w = w;
            cur->h = h;
            cur->stride = ns;
        }

        ASS_ImagePriv *priv = (ASS_ImagePriv *) cur;
        priv->buffer = cur->bitmap = nbuffer;
        ass_cache_dec_ref(priv->source);
        priv->source = NULL;
    }
}

/**
 * \brief Convert TextInfo struct to ASS_Image list
 * Splits glyphs in halves when needed (for \kf karaoke).
 */
static ASS_Image *render_text(RenderContext *state)
{
    ASS_Image *head;
    ASS_Image **tail = &head;
    unsigned n_bitmaps = state->text_info.n_bitmaps;
    CombinedBitmapInfo *bitmaps = state->text_info.combined_bitmaps;
    unsigned alpha_skip = state->renderer->experimental_edges ? 0xFE : 0xFF;

    for (unsigned i = 0; i < n_bitmaps; i++) {
        CombinedBitmapInfo *info = &bitmaps[i];
        if (!info->bm_s || state->border_style == 4 || _a(info->c[3]) >= alpha_skip)
            continue;

        tail =
            render_glyph(state, info->bm_s, info->x, info->y, info->c[3], 0,
                         1000000, tail, IMAGE_TYPE_SHADOW, info->image);
    }

    for (unsigned i = 0; i < n_bitmaps; i++) {
        CombinedBitmapInfo *info = &bitmaps[i];
        if (!info->bm_o || _a(info->c[2]) >= alpha_skip)
            continue;

        if ((info->effect_type == EF_KARAOKE_KO)
                && (info->effect_timing <= 0)) {
            // do nothing
        } else {
            tail =
                render_glyph(state, info->bm_o, info->x, info->y, info->c[2],
                             0, 1000000, tail, IMAGE_TYPE_OUTLINE, info->image);
        }
    }

    for (unsigned i = 0; i < n_bitmaps; i++) {
        CombinedBitmapInfo *info = &bitmaps[i];
        if (!info->bm)
            continue;

        if ((info->effect_type == EF_KARAOKE)
                || (info->effect_type == EF_KARAOKE_KO)) {
            uint32_t clr = info->effect_timing > 0 ? info->c[0] : info->c[1];
            if (_a(clr) >= alpha_skip)
                continue;
            tail =
                render_glyph(state, info->bm, info->x, info->y,
                             clr, 0, 1000000, tail,
                             IMAGE_TYPE_CHARACTER, info->image);
        } else if (info->effect_type == EF_KARAOKE_KF) {
            if (_a(info->c[0]) >= alpha_skip && _a(info->c[1]) >= alpha_skip)
                continue;
            tail =
                render_glyph(state, info->bm, info->x, info->y, info->c[0],
                             info->c[1], info->effect_timing, tail,
                             IMAGE_TYPE_CHARACTER, info->image);
        } else {
            if (_a(info->c[0]) >= alpha_skip)
                continue;
            tail =
                render_glyph(state, info->bm, info->x, info->y, info->c[0],
                             0, 1000000, tail, IMAGE_TYPE_CHARACTER, info->image);
        }
    }

    *tail = 0;
    blend_vector_clip(state, head);

    return head;
}

static void compute_string_bbox(TextInfo *text, ASS_DRect *bbox)
{
    if (text->length > 0) {
        bbox->x_min = +32000;
        bbox->x_max = -32000;
        bbox->y_min = -text->lines[0].asc;
        bbox->y_max = bbox->y_min + text->height;

        for (int i = 0; i < text->length; i++) {
            GlyphInfo *info = text->glyphs + i;
            if (info->skip) continue;
            double s = d6_to_double(info->pos.x);
            double e = s + d6_to_double(info->cluster_advance.x);
            bbox->x_min = FFMIN(bbox->x_min, s);
            bbox->x_max = FFMAX(bbox->x_max, e);
        }
    } else
        bbox->x_min = bbox->x_max = bbox->y_min = bbox->y_max = 0;
}

static ASS_Style *handle_selective_style_overrides(RenderContext *state,
                                                   ASS_Style *rstyle)
{
    // The script style is the one the event was declared with.
    ASS_Renderer *render_priv = state->renderer;
    ASS_Style *script = render_priv->track->styles +
                        state->event->Style;
    // The user style was set with ass_set_selective_style_override().
    ASS_Style *user = &render_priv->user_override_style;
    ASS_Style *new = &state->override_style_temp_storage;
    int explicit = state->explicit;
    int requested = render_priv->settings.selective_style_overrides;
    double scale;

    // Either the event's style, or the style forced with a \r tag.
    if (!rstyle)
        rstyle = script;

    // Create a new style that contains a mix of the original style and
    // user_style (the user's override style). Copy only fields from the
    // script's style that are deemed necessary.
    *new = *rstyle;

    state->apply_font_scale =
        !explicit || !(requested & ASS_OVERRIDE_BIT_SELECTIVE_FONT_SCALE);

    // On positioned events, do not apply most overrides.
    if (explicit)
        requested = 0;

    if (requested & ASS_OVERRIDE_BIT_STYLE)
        requested |= ASS_OVERRIDE_BIT_FONT_NAME |
                     ASS_OVERRIDE_BIT_FONT_SIZE_FIELDS |
                     ASS_OVERRIDE_BIT_COLORS |
                     ASS_OVERRIDE_BIT_BORDER |
                     ASS_OVERRIDE_BIT_ATTRIBUTES;

    // Copies fields even not covered by any of the other bits.
    if (requested & ASS_OVERRIDE_FULL_STYLE)
        *new = *user;

    // The user style is supposed to be independent of the script resolution.
    // Treat the user style's values as if they were specified for a script with
    // PlayResY=288, and rescale the values to the current script.
    scale = render_priv->track->PlayResY / 288.0;

    if (requested & ASS_OVERRIDE_BIT_FONT_SIZE_FIELDS) {
        new->FontSize = user->FontSize * scale;
        new->Spacing = user->Spacing * scale;
        new->ScaleX = user->ScaleX;
        new->ScaleY = user->ScaleY;
    }

    if (requested & ASS_OVERRIDE_BIT_FONT_NAME) {
        new->FontName = user->FontName;
        new->treat_fontname_as_pattern = user->treat_fontname_as_pattern;
    }

    if (requested & ASS_OVERRIDE_BIT_COLORS) {
        new->PrimaryColour = user->PrimaryColour;
        new->SecondaryColour = user->SecondaryColour;
        new->OutlineColour = user->OutlineColour;
        new->BackColour = user->BackColour;
    }

    if (requested & ASS_OVERRIDE_BIT_ATTRIBUTES) {
        new->Bold = user->Bold;
        new->Italic = user->Italic;
        new->Underline = user->Underline;
        new->StrikeOut = user->StrikeOut;
    }

    if (requested & ASS_OVERRIDE_BIT_BORDER) {
        new->BorderStyle = user->BorderStyle;
        new->Outline = user->Outline * scale;
        new->Shadow = user->Shadow * scale;
    }

    if (requested & ASS_OVERRIDE_BIT_BLUR)
        new->Blur = user->Blur * scale;

    if (requested & ASS_OVERRIDE_BIT_ALIGNMENT)
        new->Alignment = user->Alignment;

    if (requested & ASS_OVERRIDE_BIT_JUSTIFY)
        new->Justify = user->Justify;

    if (requested & ASS_OVERRIDE_BIT_MARGINS) {
        new->MarginL = user->MarginL;
        new->MarginR = user->MarginR;
        new->MarginV = user->MarginV;
    }

    if (!new->FontName)
        new->FontName = rstyle->FontName;

    state->style = new;
    state->overrides = requested;

    return new;
}

ASS_Vector ass_layout_res(ASS_Renderer *render_priv)
{
    ASS_Track *track = render_priv->track;
    if (track->LayoutResX > 0 && track->LayoutResY > 0)
        return (ASS_Vector) { track->LayoutResX, track->LayoutResY };

    ASS_Settings *settings = &render_priv->settings;
    if (settings->storage_width > 0 && settings->storage_height > 0)
        return (ASS_Vector) { settings->storage_width, settings->storage_height };

    if (settings->par <= 0 || settings->par == 1 ||
            !render_priv->frame_content_width || !render_priv->frame_content_height)
        return (ASS_Vector) { track->PlayResX, track->PlayResY };
    if (settings->par > 1)
        return (ASS_Vector) {
            FFMAX(1, lround(track->PlayResY * render_priv->frame_content_width
                    / render_priv->frame_content_height / settings->par)),
            track->PlayResY
        };
    else
        return (ASS_Vector) {
            track->PlayResX,
            FFMAX(1, lround(track->PlayResX * render_priv->frame_content_height
                    / render_priv->frame_content_width * settings->par))
        };
}

static void init_font_scale(RenderContext *state)
{
    ASS_Renderer *render_priv = state->renderer;
    ASS_Settings *settings_priv = &render_priv->settings;

    double font_scr_w = render_priv->frame_content_width;
    double font_scr_h = render_priv->frame_content_height;
    if (!state->explicit && render_priv->settings.use_margins) {
        font_scr_w = render_priv->fit_width;
        font_scr_h = render_priv->fit_height;
    }

    state->screen_scale_x = font_scr_w / render_priv->track->PlayResX;
    state->screen_scale_y = font_scr_h / render_priv->track->PlayResY;

    ASS_Vector layout_res = ass_layout_res(render_priv);
    state->blur_scale_x = font_scr_w / layout_res.x;
    state->blur_scale_y = font_scr_h / layout_res.y;
    if (render_priv->track->ScaledBorderAndShadow) {
        state->border_scale_x = state->screen_scale_x;
        state->border_scale_y = state->screen_scale_y;
    } else {
        state->border_scale_x = state->blur_scale_x;
        state->border_scale_y = state->blur_scale_y;
    }

    if (state->apply_font_scale) {
        state->screen_scale_x *= settings_priv->font_size_coeff;
        state->screen_scale_y *= settings_priv->font_size_coeff;
        state->border_scale_x *= settings_priv->font_size_coeff;
        state->border_scale_y *= settings_priv->font_size_coeff;
        state->blur_scale_x *= settings_priv->font_size_coeff;
        state->blur_scale_y *= settings_priv->font_size_coeff;
    }
}

/**
 * \brief partially reset render_context to style values
 * Works like {\r}: resets some style overrides
 */
void ass_reset_render_context(RenderContext *state, ASS_Style *style)
{
    style = handle_selective_style_overrides(state, style);

    init_font_scale(state);

    state->c[0] = style->PrimaryColour;
    state->c[1] = style->SecondaryColour;
    state->c[2] = style->OutlineColour;
    state->c[3] = style->BackColour;
    state->flags =
        (style->Underline ? DECO_UNDERLINE : 0) |
        (style->StrikeOut ? DECO_STRIKETHROUGH : 0);
    state->font_size = style->FontSize;

    state->family.str = style->FontName;
    state->family.len = strlen(style->FontName);
    state->treat_family_as_pattern = style->treat_fontname_as_pattern;
    state->bold = style->Bold;
    state->italic = style->Italic;
    ass_update_font(state);

    state->border_style = style->BorderStyle;
    state->border_x = style->Outline;
    state->border_y = style->Outline;
    state->scale_x = style->ScaleX;
    state->scale_y = style->ScaleY;
    state->hspacing = style->Spacing;
    state->be = 0;
    state->blur = style->Blur;
    state->shadow_x = style->Shadow;
    state->shadow_y = style->Shadow;
    state->frx = state->fry = 0.;
    state->frz = style->Angle;
    state->fax = state->fay = 0.;
    state->font_encoding = style->Encoding;
}

/**
 * \brief Start new event. Reset state.
 */
static void
init_render_context(RenderContext *state, ASS_Event *event)
{
    ASS_Renderer *render_priv = state->renderer;

    state->event = event;
    state->parsed_tags = 0;
    state->evt_type = EVENT_NORMAL;
    state->timed = false;
    state->plateau_valid = true;
    state->plateau_unbounded = false;
    state->alpha_plateau_valid = true;
    state->rendered_transparent = false;
    state->plateau_from = 0;
    state->plateau_until = event->Duration;
    state->plateau_active_end = 0;
    state->alpha_plateau_from = 0;
    state->alpha_plateau_until = event->Duration;
    state->alpha_active_end = 0;

    state->wrap_style = render_priv->track->WrapStyle;

    state->pos_x = 0;
    state->pos_y = 0;
    state->org_x = 0;
    state->org_y = 0;
    state->have_origin = 0;
    state->clip_x0 = 0;
    state->clip_y0 = 0;
    state->clip_x1 = render_priv->track->PlayResX;
    state->clip_y1 = render_priv->track->PlayResY;
    state->clip_mode = 0;
    state->detect_collisions = 1;
    state->fade = 0;
    state->drawing_scale = 0;
    state->pbo = 0;
    state->effect_type = EF_NONE;
    state->effect_timing = 0;
    state->effect_skip_timing = 0;
    state->reset_effect = false;

    ass_apply_transition_effects(state);
    if (state->timed) {
        state->plateau_valid = false;
        state->plateau_unbounded = true;
        state->alpha_plateau_valid = false;
    }
    state->explicit = state->evt_type != EVENT_NORMAL ||
                      ass_event_has_hard_overrides(event->Text);

    ass_reset_render_context(state, NULL);
    state->alignment = state->style->Alignment;
    state->justify = state->style->Justify;
}

static void free_render_context(RenderContext *state)
{
    state->font = NULL;
    state->family.str = NULL;
    state->family.len = 0;
    state->clip_drawing_text.str = NULL;
    state->clip_drawing_text.len = 0;
    state->text_info.length = 0;
}

/**
 * \brief Get normal and outline (border) glyphs
 * \param info out: struct filled with extracted data
 * Tries to get both glyphs from cache.
 * If they can't be found, gets a glyph from font face, generates outline,
 * and add them to cache.
 */
static void
get_outline_glyph(RenderContext *state, GlyphInfo *info)
{
    ASS_Renderer *priv = state->renderer;
    OutlineHashValue *val;
    ASS_DVector scale, offset = {0};

    int32_t asc, desc;
    OutlineHashKey key;
    if (info->drawing_text.str) {
        key.type = OUTLINE_DRAWING;
        key.u.drawing.text = info->drawing_text;
        val = ass_cache_get(priv->cache.outline_cache, &key, priv);
        if (!val || !val->valid)
            return;

        int32_t scale_base = lshiftwrapi(1, info->drawing_scale - 1);
        double w = scale_base > 0 ? (1.0 / scale_base) : 0;
        scale.x = info->scale_x * w * state->screen_scale_x / priv->par_scale_x;
        scale.y = info->scale_y * w * state->screen_scale_y;
        desc = 64 * info->drawing_pbo;
        asc = val->asc - desc;

        offset.y = -asc * scale.y;
    } else {
        key.type = OUTLINE_GLYPH;
        GlyphHashKey *k = &key.u.glyph;
        k->font = info->font;
        k->size = info->font_size;
        k->face_index = info->face_index;
        k->glyph_index = info->glyph_index;
        k->bold = info->bold;
        k->italic = info->italic;
        k->flags = info->flags;

        val = ass_cache_get(priv->cache.outline_cache, &key, priv);
        if (!val || !val->valid)
            return;

        scale.x = info->scale_x;
        scale.y = info->scale_y;
        asc  = val->asc;
        desc = val->desc;
    }

    info->outline = val;
    info->transform.scale = scale;
    info->transform.offset = offset;

    info->bbox.x_min = ass_lrint(val->cbox.x_min * scale.x + offset.x);
    info->bbox.y_min = ass_lrint(val->cbox.y_min * scale.y + offset.y);
    info->bbox.x_max = ass_lrint(val->cbox.x_max * scale.x + offset.x);
    info->bbox.y_max = ass_lrint(val->cbox.y_max * scale.y + offset.y);

    if (info->drawing_text.str || priv->settings.shaper == ASS_SHAPING_SIMPLE) {
        info->cluster_advance.x = info->advance.x = ass_lrint(val->advance * scale.x);
        info->cluster_advance.y = info->advance.y = 0;
    }
    info->asc  = ass_lrint(asc  * scale.y);
    info->desc = ass_lrint(desc * scale.y);
}

size_t ass_outline_construct(void *key, void *value, void *priv)
{
    ASS_Renderer *render_priv = priv;
    OutlineHashKey *outline_key = key;
    OutlineHashValue *v = value;
    memset(v, 0, sizeof(*v));

    switch (outline_key->type) {
    case OUTLINE_GLYPH:
        {
            GlyphHashKey *k = &outline_key->u.glyph;
            ass_face_set_size(k->font->faces[k->face_index], k->size);
            if (!ass_font_get_glyph(k->font, k->face_index, k->glyph_index,
                                    render_priv->settings.hinting))
                return 1;
            if (!ass_get_glyph_outline(&v->outline[0], &v->advance,
                                       k->font->faces[k->face_index],
                                       k->flags))
                return 1;
            ass_font_get_asc_desc(k->font, k->face_index,
                                  &v->asc, &v->desc);
            break;
        }
    case OUTLINE_DRAWING:
        {
            ASS_Rect bbox;
            const char *text = outline_key->u.drawing.text.str;  // always zero-terminated
            if (!ass_drawing_parse(&v->outline[0], &bbox, text, render_priv->library))
                return 1;

            v->advance = bbox.x_max - bbox.x_min;
            v->asc = bbox.y_max - bbox.y_min;
            v->desc = 0;
            break;
        }
    case OUTLINE_BORDER:
        {
            BorderHashKey *k = &outline_key->u.border;
            if (!k->border.x && !k->border.y)
                break;
            if (!k->outline->outline[0].n_points)
                break;

            ASS_Outline src;
            if (!ass_outline_scale_pow2(&src, &k->outline->outline[0],
                                        k->scale_ord_x, k->scale_ord_y))
                return 1;
            if (!ass_outline_stroke(&v->outline[0], &v->outline[1], &src,
                                    k->border.x * STROKER_PRECISION,
                                    k->border.y * STROKER_PRECISION,
                                    STROKER_PRECISION)) {
                ass_msg(render_priv->library, MSGL_WARN, "Cannot stroke outline");
                ass_outline_free(&v->outline[0]);
                ass_outline_free(&v->outline[1]);
                ass_outline_free(&src);
                return 1;
            }
            ass_outline_free(&src);
            break;
        }
    case OUTLINE_BOX:
        {
            ASS_Outline *ol = &v->outline[0];
            if (!ass_outline_alloc(ol, 4, 4))
                return 1;
            ol->points[0].x = ol->points[3].x = 0;
            ol->points[1].x = ol->points[2].x = 64;
            ol->points[0].y = ol->points[1].y = 0;
            ol->points[2].y = ol->points[3].y = 64;
            ol->segments[0] = OUTLINE_LINE_SEGMENT;
            ol->segments[1] = OUTLINE_LINE_SEGMENT;
            ol->segments[2] = OUTLINE_LINE_SEGMENT;
            ol->segments[3] = OUTLINE_LINE_SEGMENT | OUTLINE_CONTOUR_END;
            ol->n_points = ol->n_segments = 4;
            break;
        }
    default:
        return 1;
    }

    rectangle_reset(&v->cbox);
    ass_outline_update_cbox(&v->outline[0], &v->cbox);
    ass_outline_update_cbox(&v->outline[1], &v->cbox);
    if (v->cbox.x_min > v->cbox.x_max || v->cbox.y_min > v->cbox.y_max)
        v->cbox.x_min = v->cbox.y_min = v->cbox.x_max = v->cbox.y_max = 0;
    v->valid = true;
    return 1;
}

/**
 * \brief Calculate outline transformation matrix
 */
static void calc_transform_matrix(RenderContext *state,
                                  GlyphInfo *info, double m[3][3])
{
    ASS_Renderer *render_priv = state->renderer;

    double frx = ASS_PI / 180 * info->frx;
    double fry = ASS_PI / 180 * info->fry;
    double frz = ASS_PI / 180 * info->frz;

    double sx = -sin(frx), cx = cos(frx);
    double sy =  sin(fry), cy = cos(fry);
    double sz = -sin(frz), cz = cos(frz);

    double fax = info->fax * info->scale_x / info->scale_y;
    double fay = info->fay * info->scale_y / info->scale_x;
    double x1[3] = { 1, fax, info->shift.x + info->asc * fax };
    double y1[3] = { fay, 1, info->shift.y };

    double x2[3], y2[3];
    for (int i = 0; i < 3; i++) {
        x2[i] = x1[i] * cz - y1[i] * sz;
        y2[i] = x1[i] * sz + y1[i] * cz;
    }

    double y3[3], z3[3];
    for (int i = 0; i < 3; i++) {
        y3[i] = y2[i] * cx;
        z3[i] = y2[i] * sx;
    }

    double x4[3], z4[3];
    for (int i = 0; i < 3; i++) {
        x4[i] = x2[i] * cy - z3[i] * sy;
        z4[i] = x2[i] * sy + z3[i] * cy;
    }

    double dist = 20000 * state->blur_scale_y;
    z4[2] += dist;

    double scale_x = dist * render_priv->par_scale_x;
    double offs_x = info->pos.x - info->shift.x * render_priv->par_scale_x;
    double offs_y = info->pos.y - info->shift.y;
    for (int i = 0; i < 3; i++) {
        m[0][i] = z4[i] * offs_x + x4[i] * scale_x;
        m[1][i] = z4[i] * offs_y + y3[i] * dist;
        m[2][i] = z4[i];
    }
}

/**
 * \brief Get bitmaps for a glyph
 * \param info glyph info
 * Tries to get glyph bitmaps from bitmap cache.
 * If they can't be found, they are generated by rotating and rendering the glyph.
 * After that, bitmaps are added to the cache.
 * They are returned in info->bm (glyph), info->bm_o (outline).
 */
static double restore_blur(int qblur);

// Remember the last vector-clip crop window per (outline, pos, transform,
// blur). Once that stationary glyph's clip window moves across frames (even
// across frame-by-frame separate Dialogue events), stop cropping so the
// uncropped bitmap is cached once and masked by blend_vector_clip
// (pixel-identical to stock). Including pos, blur, and last_time != r->time
// prevents sibling events or repeated glyphs on the same frame from colliding.
static bool get_vector_clip_bounds(RenderContext *state,
                                   double *cmin_x, double *cmin_y,
                                   double *cmax_x, double *cmax_y)
{
    if (!state->clip_drawing_text.str || state->clip_drawing_mode)
        return false;
    OutlineHashKey ol_key;
    ol_key.type = OUTLINE_DRAWING;
    ol_key.u.drawing.text = state->clip_drawing_text;
    OutlineHashValue *ol = ass_cache_get(state->renderer->cache.outline_cache,
                                         &ol_key, state->renderer);
    if (!ol || !ol->valid)
        return false;
    int32_t scale_base = lshiftwrapi(1, state->clip_drawing_scale - 1);
    double w = scale_base > 0 ? (1.0 / scale_base) : 0;
    double sx = state->screen_scale_x * w / 64.0;
    double sy = state->screen_scale_y * w / 64.0;
    if (!(sx > 0 && sy > 0 && isfinite(sx) && isfinite(sy)))
        return false;
    double x0 = ol->cbox.x_min * sx + state->renderer->settings.left_margin;
    double y0 = ol->cbox.y_min * sy + state->renderer->settings.top_margin;
    double x1 = ol->cbox.x_max * sx + state->renderer->settings.left_margin;
    double y1 = ol->cbox.y_max * sy + state->renderer->settings.top_margin;
    if (!isfinite(x0) || !isfinite(y0) || !isfinite(x1) || !isfinite(y1) ||
        x0 < INT32_MIN / 2 || y0 < INT32_MIN / 2 ||
        x1 > INT32_MAX / 2 || y1 > INT32_MAX / 2)
        return false;
    *cmin_x = x0;
    *cmin_y = y0;
    *cmax_x = x1;
    *cmax_y = y1;
    return true;
}

static bool clip_window_moving(const RenderContext *state, const BitmapHashKey *k,
                               ASS_Vector pos, const FilterDesc *filter,
                               int64_t x0, int64_t y0, int64_t x1, int64_t y1)
{
    ASS_Renderer *r = state->renderer;
    uint64_t h = UINT64_C(1469598103934665603);
    const uint64_t words[7] = {
        (uint64_t) (uintptr_t) k->outline,
        ((uint64_t) (uint32_t) pos.x << 32) | (uint32_t) pos.y,
        ((uint64_t) (uint32_t) k->offset.x << 32) | (uint32_t) k->offset.y,
        ((uint64_t) (uint32_t) k->matrix_x.x << 32) | (uint32_t) k->matrix_x.y,
        ((uint64_t) (uint32_t) k->matrix_y.x << 32) | (uint32_t) k->matrix_y.y,
        ((uint64_t) (uint32_t) k->matrix_z.x << 32) | (uint32_t) k->matrix_z.y,
        (((uint64_t) (uint32_t) filter->blur_x << 32) | (uint32_t) filter->blur_y) ^
            ((uint64_t) (uint32_t) filter->be << 16),
    };
    for (int i = 0; i < 7; i++) {
        h ^= words[i];
        h *= UINT64_C(1099511628211);
        h ^= h >> 29;
    }
    if (!h)
        h = 1;
    ClipMemoEntry *m = &r->clip_memo[(h ^ (h >> 32)) & (CLIP_MEMO_SIZE - 1)];
    if (m->hash != h) {
        m->hash = h;
        m->x0 = x0;
        m->y0 = y0;
        m->x1 = x1;
        m->y1 = y1;
        m->last_time = r->time;
        m->moving = false;
        return false;
    }
    if (!m->moving && m->last_time != r->time &&
        (m->x0 != x0 || m->y0 != y0 || m->x1 != x1 || m->y1 != y1))
        m->moving = true;
    m->last_time = r->time;
    return m->moving;
}

static void set_bitmap_clip(RenderContext *state, GlyphInfo *info,
                            ASS_Vector pos, BitmapHashKey *key,
                            const FilterDesc *filter)
{
    key->clip_align = 0;
    key->clip_min = key->clip_max = (ASS_Vector) {0, 0};
#ifndef ASS_DISABLE_CLIP_CULLING
    ASS_Renderer *r = state->renderer;
    // Collision shifts, inverse clipping, backgrounds and shadows need their
    // own dependency bounds. Keep their established rendering path unchanged.
    if (!state->clip_drawing_text.str || state->clip_drawing_mode ||
        state->clip_mode || state->detect_collisions ||
        state->evt_type != EVENT_POSITIONED ||
        (filter->flags & FILTER_NONZERO_SHADOW) || info->border_style != 1 ||
        info->be > 16)
        return;
    // Cropping individual glyphs can move a multi-glyph composite's blur
    // sampling origin. Keep that path unchanged, including shaped clusters.
    if (!r->experimental_edges &&
        (state->text_info.length != 1 || state->text_info.glyphs[0].next))
        return;

    double cmin_x, cmin_y, cmax_x, cmax_y;
    if (!get_vector_clip_bounds(state, &cmin_x, &cmin_y, &cmax_x, &cmax_y))
        return;

    int pad_x = 0, pad_y = 0, align_x = 1, align_y = 1;
    double r2x = restore_blur(filter->blur_x);
    double r2y = restore_blur(filter->blur_y);
    if (r2x > 0.001 || r2y > 0.001) {
        int blur_mode = r->experimental_edges;
        ass_gaussian_blur_bounds(r2x, blur_mode, &pad_x, &align_x);
        ass_gaussian_blur_bounds(r2y, blur_mode, &pad_y, &align_y);
    }
    pad_x += info->be + 6;
    pad_y += info->be + 6;
    int64_t x0 = (int64_t) floor(cmin_x) - pos.x - pad_x;
    int64_t y0 = (int64_t) floor(cmin_y) - pos.y - pad_y;
    int64_t x1 = (int64_t) ceil(cmax_x) - pos.x + pad_x;
    int64_t y1 = (int64_t) ceil(cmax_y) - pos.y + pad_y;
    if (x0 < INT32_MIN || y0 < INT32_MIN || x1 > INT32_MAX || y1 > INT32_MAX)
        return;
    if (clip_window_moving(state, key, pos, filter, x0, y0, x1, y1))
        return;
    key->clip_min = (ASS_Vector) {x0, y0};
    key->clip_max = (ASS_Vector) {x1, y1};
    key->clip_align = FFMAX(align_x, align_y);
#endif
}

static void
get_bitmap_glyph(RenderContext *state, GlyphInfo *info,
                 int32_t *leftmost_x,
                 ASS_Vector *pos, ASS_Vector *pos_o,
                 ASS_DVector *offset, bool first, int flags,
                 const FilterDesc *filter)
{
    ASS_Renderer *render_priv = state->renderer;

    if (!info->outline || info->symbol == '\n' || info->symbol == 0 || info->skip)
        return;

    double m1[3][3], m2[3][3], m[3][3];
    const ASS_Transform *tr = &info->transform;
    calc_transform_matrix(state, info, m1);
    for (int i = 0; i < 3; i++) {
        m2[i][0] = m1[i][0] * tr->scale.x;
        m2[i][1] = m1[i][1] * tr->scale.y;
        m2[i][2] = m1[i][0] * tr->offset.x + m1[i][1] * tr->offset.y + m1[i][2];
    }
    memcpy(m, m2, sizeof(m));

    if (info->effect_type == EF_KARAOKE_KF)
        ass_outline_update_min_transformed_x(&info->outline->outline[0], m, leftmost_x);

    int pos_shift = render_priv->experimental_edges >= 2 ? SUBPIXEL_ORDER :
                    (render_priv->experimental_edges ? SUBPIXEL_ORDER - 1 : 0);
    BitmapHashKey key = {0};
    key.outline = info->outline;
    if (!quantize_transform(m, pos, offset, first, pos_shift, &key))
        return;

    set_bitmap_clip(state, info, *pos, &key, filter);
    info->bm = ass_cache_get(render_priv->cache.bitmap_cache, &key, state);
    if (!info->bm || !info->bm->buffer)
        info->bm = NULL;

    *pos_o = *pos;

    OutlineHashKey ol_key;
    if (flags & FILTER_BORDER_STYLE_3) {
        if (!(flags & (FILTER_NONZERO_BORDER | FILTER_NONZERO_SHADOW)))
            return;

        ol_key.type = OUTLINE_BOX;

        ASS_DVector bord = {
            64 * info->border_x * state->border_scale_x /
                render_priv->par_scale_x,
            64 * info->border_y * state->border_scale_y,
        };
        double width = info->hspacing_scaled + info->advance.x;
        double height = info->asc + info->desc;

        ASS_DVector orig_scale;
        orig_scale.x = info->scale_x * info->scale_fix;
        orig_scale.y = info->scale_y * info->scale_fix;

        // Emulate the WTFish behavior of VSFilter, i.e. double-scale
        // the sizes of the opaque box.
        bord.x *= orig_scale.x;
        bord.y *= orig_scale.y;
        width  *= orig_scale.x;
        height *= orig_scale.y;

        // to avoid gaps
        bord.x = FFMAX(64, bord.x);
        bord.y = FFMAX(64, bord.y);

        ASS_DVector scale = {
            (width  + 2 * bord.x) / 64,
            (height + 2 * bord.y) / 64,
        };
        ASS_DVector offset = { -bord.x, -bord.y - info->asc };
        for (int i = 0; i < 3; i++) {
            m[i][0] = m1[i][0] * scale.x;
            m[i][1] = m1[i][1] * scale.y;
            m[i][2] = m1[i][0] * offset.x + m1[i][1] * offset.y + m1[i][2];
        }
    } else {
        if (!(flags & FILTER_NONZERO_BORDER))
            return;

        ol_key.type = OUTLINE_BORDER;
        BorderHashKey *k = &ol_key.u.border;
        k->outline = info->outline;

        double bord_x =
            64 * state->border_scale_x * info->border_x / tr->scale.x /
                render_priv->par_scale_x;
        double bord_y =
            64 * state->border_scale_y * info->border_y / tr->scale.y;

        const ASS_Rect *bbox = &info->outline->cbox;
        // Estimate bounding box half size after stroking
        double dx = (bbox->x_max - bbox->x_min) / 2.0 + (bord_x + 64);
        double dy = (bbox->y_max - bbox->y_min) / 2.0 + (bord_y + 64);

        // Matrix after quantize_transform() has
        // input and output origin at bounding box center.
        double mxx = fabs(m[0][0]), mxy = fabs(m[0][1]);
        double myx = fabs(m[1][0]), myy = fabs(m[1][1]);
        double mzx = fabs(m[2][0]), mzy = fabs(m[2][1]);

        double z0 = m[2][2] - mzx * dx - mzy * dy;
        double w = 1 / FFMAX(z0, m[2][2] / MAX_PERSP_SCALE);

        // Notation from quantize_transform().
        // Note that goal here is to estimate acceptable error for stroking, i. e. D(x) and D(y).
        // Matrix coefficients are constants now, so D(m_ij) = 0 for all i, j from {x, y, z}.

        // D(z) <= |m_zx| * D(x) + |m_zy| * D(y),
        // D(x_out) = D((m_xx * x + m_xy * y) / z)
        //  <= (|m_xx| * D(x) + |m_xy| * D(y)) / z0 + x_lim * D(z) / z0^2
        //  <= (|m_xx| / z0 + |m_zx| * x_lim / z0^2) * D(x)
        //   + (|m_xy| / z0 + |m_zy| * x_lim / z0^2) * D(y),
        // D(y_out) = D((m_yx * x + m_yy * y) / z)
        //  <= (|m_yx| * D(x) + |m_yy| * D(y)) / z0 + y_lim * D(z) / z0^2
        //  <= (|m_yx| / z0 + |m_zx| * y_lim / z0^2) * D(x)
        //   + (|m_yy| / z0 + |m_zy| * y_lim / z0^2) * D(y).

        // Quantization steps (pick: ACCURACY = POSITION_PRECISION):
        // STROKER_PRECISION / 2^scale_ord_x ~ D(x) ~ POSITION_PRECISION /
        //   (max(|m_xx|, |m_yx|) / z0 + |m_zx| * max(x_lim, y_lim) / z0^2),
        // STROKER_PRECISION / 2^scale_ord_y ~ D(y) ~ POSITION_PRECISION /
        //   (max(|m_xy|, |m_yy|) / z0 + |m_zy| * max(x_lim, y_lim) / z0^2).

        double x_lim = mxx * dx + mxy * dy;
        double y_lim = myx * dx + myy * dy;
        double rz = FFMAX(x_lim, y_lim) * w;

        w *= STROKER_PRECISION / POSITION_PRECISION;
        frexp(w * (FFMAX(mxx, myx) + mzx * rz), &k->scale_ord_x);
        frexp(w * (FFMAX(mxy, myy) + mzy * rz), &k->scale_ord_y);
        bord_x = ldexp(bord_x, k->scale_ord_x);
        bord_y = ldexp(bord_y, k->scale_ord_y);
        if (!(bord_x < OUTLINE_MAX && bord_y < OUTLINE_MAX))
            return;
        k->border.x = ass_lrint(bord_x / STROKER_PRECISION);
        k->border.y = ass_lrint(bord_y / STROKER_PRECISION);
        if (!k->border.x && !k->border.y) {
            info->bm_o = info->bm;
            return;
        }

        for (int i = 0; i < 3; i++) {
            m[i][0] = ldexp(m2[i][0], -k->scale_ord_x);
            m[i][1] = ldexp(m2[i][1], -k->scale_ord_y);
            m[i][2] = m2[i][2];
        }
    }

    key.outline = ass_cache_get(render_priv->cache.outline_cache, &ol_key, render_priv);
    if (!key.outline || !key.outline->valid ||
            !quantize_transform(m, pos_o, offset, false, pos_shift, &key))
        return;

    set_bitmap_clip(state, info, *pos_o, &key, filter);
    info->bm_o = ass_cache_get(render_priv->cache.bitmap_cache, &key, state);
    if (!info->bm_o || !info->bm_o->buffer) {
        info->bm_o = NULL;
        *pos_o = *pos;
    } else if (!info->bm)
        *pos = *pos_o;
}

static inline size_t outline_size(const ASS_Outline* outline)
{
    return sizeof(ASS_Vector) * outline->n_points + outline->n_segments;
}

static bool copy_or_simplify_outline(ASS_Outline *dst, const ASS_Outline *src,
                                     int32_t tol)
{
    if (src->n_points >= 256)
        return ass_outline_simplify(dst, src, tol);
    ass_outline_clear(dst);
    if (!src->n_points || !src->n_segments)
        return true;
    if (!ass_outline_alloc(dst, src->n_points, src->n_segments))
        return false;
    memcpy(dst->points, src->points, src->n_points * sizeof(*dst->points));
    memcpy(dst->segments, src->segments, src->n_segments * sizeof(*dst->segments));
    dst->n_points = src->n_points;
    dst->n_segments = src->n_segments;
    return true;
}

static const ASS_Outline *select_outline_lod(OutlineHashValue *ov,
                                             const BitmapHashKey *k,
                                             const double m[3][3],
                                             int edges,
                                             bool *pre_simplified)
{
    *pre_simplified = false;
    if (!edges ||
        (ov->outline[0].n_points < 256 && ov->outline[1].n_points < 256))
        return ov->outline;

    double sx = hypot(m[0][0], m[1][0]);
    double sy = hypot(m[0][1], m[1][1]);
    if (k->matrix_z.x || k->matrix_z.y) {
        const ASS_Rect *cb = &ov->cbox;
        double dx = (cb->x_max - cb->x_min) * 0.5 + 64.0;
        double dy = (cb->y_max - cb->y_min) * 0.5 + 64.0;
        double x_lim = fabs(m[0][0]) * dx + fabs(m[0][1]) * dy;
        double y_lim = fabs(m[1][0]) * dx + fabs(m[1][1]) * dy;
        double p_lim = hypot(x_lim, y_lim);
        sx += fabs(m[2][0]) * p_lim;
        sy += fabs(m[2][1]) * p_lim;
    }
    double max_scale = FFMAX(sx, sy);
    if (!(max_scale > 1e-6) || !isfinite(max_scale))
        return ov->outline;

    double screen_tol = edges >= 2 ? 96.0 : 64.0;
    double raw_tol = screen_tol / max_scale;
    if (!(raw_tol >= 8.0) || !isfinite(raw_tol))
        return ov->outline;

    uint32_t clamped = raw_tol >= (double) (1U << 20) ? (1U << 20) : (uint32_t) raw_tol;
    int32_t tol = 8;
    while ((uint32_t) tol <= (clamped >> 1))
        tol <<= 1;

    for (int i = 0; i < OUTLINE_LOD_SLOTS; i++) {
        if (ov->lod[i].tol == tol) {
            *pre_simplified = true;
            return ov->lod[i].outline;
        }
    }

    int slot = ov->lod_next % OUTLINE_LOD_SLOTS;
    ov->lod_next = (uint8_t) ((slot + 1) % OUTLINE_LOD_SLOTS);
    OutlineLod *lod = &ov->lod[slot];
    ass_outline_free(&lod->outline[0]);
    ass_outline_free(&lod->outline[1]);
    lod->tol = 0;
    if (!copy_or_simplify_outline(&lod->outline[0], &ov->outline[0], tol) ||
        !copy_or_simplify_outline(&lod->outline[1], &ov->outline[1], tol)) {
        ass_outline_free(&lod->outline[0]);
        ass_outline_free(&lod->outline[1]);
        return ov->outline;
    }
    lod->tol = tol;
    *pre_simplified = true;
    return lod->outline;
}

size_t ass_bitmap_construct(void *key, void *value, void *priv)
{
    RenderContext *state = priv;
#ifdef ASS_FLE_PROFILE
    ASS_Renderer *render_priv = state->renderer;
#endif
    FLE_START;
    BitmapHashKey *k = key;
    Bitmap *bm = value;

    double m[3][3];
    restore_transform(m, k);

    bool pre_simplified = false;
    const ASS_Outline *src_ol = select_outline_lod(k->outline, k, m,
                                                   state->renderer->experimental_edges,
                                                   &pre_simplified);

    ASS_Rect clip = {k->clip_min.x, k->clip_min.y, k->clip_max.x, k->clip_max.y};
    ASS_Outline temporary[2] = {{0}};
    bool reuse = outline_size(&src_ol[0]) <= 512 * 1024 &&
        outline_size(&src_ol[1]) <= 512 * 1024;
    ASS_Outline *outline = reuse ? state->renderer->raster_outline : temporary;
    if (k->matrix_z.x || k->matrix_z.y) {
        if (reuse) {
            ass_outline_transform_3d_reuse(&outline[0], &src_ol[0], m);
            ass_outline_transform_3d_reuse(&outline[1], &src_ol[1], m);
        } else {
            ass_outline_transform_3d(&outline[0], &src_ol[0], m);
            ass_outline_transform_3d(&outline[1], &src_ol[1], m);
        }
    } else if (state->renderer->experimental_edges && k->clip_align) {
        ass_outline_transform_2d_clipped(&outline[0], &src_ol[0], m, &clip);
        ass_outline_transform_2d_clipped(&outline[1], &src_ol[1], m, &clip);
    } else if (reuse) {
        ass_outline_transform_2d_reuse(&outline[0], &src_ol[0], m);
        ass_outline_transform_2d_reuse(&outline[1], &src_ol[1], m);
    } else {
        ass_outline_transform_2d(&outline[0], &src_ol[0], m);
        ass_outline_transform_2d(&outline[1], &src_ol[1], m);
    }

    if (!ass_outline_to_bitmap(state, bm, &outline[0], &outline[1],
                               k->clip_align ? &clip : NULL, k->clip_align,
                               pre_simplified))
        memset(bm, 0, sizeof(*bm));
    if (!reuse) {
        ass_outline_free(&outline[0]);
        ass_outline_free(&outline[1]);
    }
    FLE_PHASE(6);
#ifdef ASS_FLE_PROFILE
    render_priv->fle_phase_ns[8] += (uint64_t) bm->w * bm->h;
#endif

    return sizeof(BitmapHashKey) + sizeof(Bitmap) + bitmap_size(bm) +
           sizeof(OutlineHashValue) + outline_size(&k->outline->outline[0]) + outline_size(&k->outline->outline[1]);
}

static void measure_text_on_eol(RenderContext *state, double scale, int cur_line,
                                int max_asc, int max_desc,
                                double max_border_x, double max_border_y)
{
    TextInfo *text_info = &state->text_info;
    text_info->lines[cur_line].asc  = scale * max_asc;
    text_info->lines[cur_line].desc = scale * max_desc;
    text_info->height += scale * max_asc + scale * max_desc;
    // For *VSFilter compatibility do biased rounding on max_border*
    // https://github.com/Cyberbeing/xy-VSFilter/blob/xy_sub_filter_rc4@%7B2020-05-17%7D/src/subtitles/RTS.cpp#L1465
    text_info->border_bottom = (int) (state->border_scale_y * max_border_y + 0.5);
    if (cur_line == 0)
        text_info->border_top = text_info->border_bottom;
    // VSFilter takes max \bordx into account for collision, even if far from edge
    text_info->border_x = FFMAX(text_info->border_x,
            (int) (state->border_scale_x * max_border_x + 0.5));
}


/**
 * This function goes through text_info and calculates text parameters.
 * The following text_info fields are filled:
 *   height
 *   border_top
 *   border_bottom
 *   border_x
 *   lines[].asc
 *   lines[].desc
 */
static void measure_text(RenderContext *state)
{
    ASS_Renderer *render_priv = state->renderer;
    TextInfo *text_info = &state->text_info;
    text_info->height = 0;
    text_info->border_x = 0;

    int cur_line = 0;
    double scale = 0.5 / 64;
    int max_asc = 0, max_desc = 0;
    double max_border_y = 0, max_border_x = 0;
    bool empty_trimmed_line = true;
    for (int i = 0; i < text_info->length; i++) {
        if (text_info->glyphs[i].linebreak) {
            measure_text_on_eol(state, scale, cur_line,
                    max_asc, max_desc, max_border_x, max_border_y);
            empty_trimmed_line = true;
            max_asc = max_desc = 0;
            max_border_y = max_border_x = 0;
            scale = 0.5 / 64;
            cur_line++;
        }
        GlyphInfo *cur = text_info->glyphs + i;
        // VSFilter ignores metrics of line-leading/trailing (trimmed)
        // whitespace, except when the line becomes empty after trimming
        if (empty_trimmed_line && !cur->is_trimmed_whitespace) {
            empty_trimmed_line = false;
            // Forget metrics of line-leading whitespace
            max_asc = max_desc = 0;
            max_border_y = max_border_x = 0;
        } else if (!empty_trimmed_line && cur->is_trimmed_whitespace) {
            // Ignore metrics of line-trailing whitespace
            continue;
        }
        max_asc  = FFMAX(max_asc,  cur->asc);
        max_desc = FFMAX(max_desc, cur->desc);
        max_border_y = FFMAX(max_border_y, cur->border_y);
        max_border_x = FFMAX(max_border_x, cur->border_x);
        if (cur->symbol != '\n')
            scale = 1.0 / 64;
    }
    assert(cur_line == text_info->n_lines - 1);
    measure_text_on_eol(state, scale, cur_line,
            max_asc, max_desc, max_border_x, max_border_y);
    text_info->height += cur_line * render_priv->settings.line_spacing;
}

/**
 * Mark extra whitespace for later removal.
 */
#define IS_WHITESPACE(x) ((x->symbol == ' ' || x->symbol == '\n') \
                          && !x->linebreak)
static void trim_whitespace(RenderContext *state)
{
    int i, j;
    GlyphInfo *cur;
    TextInfo *ti = &state->text_info;

    if (!ti->length)
      return;

    // Mark trailing spaces
    i = ti->length - 1;
    cur = ti->glyphs + i;
    while (i && IS_WHITESPACE(cur)) {
        cur->skip = true;
        cur->is_trimmed_whitespace = true;
        cur = ti->glyphs + --i;
    }

    // Mark leading whitespace
    i = 0;
    cur = ti->glyphs;
    while (i < ti->length && IS_WHITESPACE(cur)) {
        cur->skip = true;
        cur->is_trimmed_whitespace = true;
        cur = ti->glyphs + ++i;
    }
    if (i < ti->length)
        cur->starts_new_run = true;

    // Mark all extraneous whitespace inbetween
    // XXX: should this really start at 0 again?
    for (i = 0; i < ti->length; ++i) {
        cur = ti->glyphs + i;
        if (cur->linebreak) {
            // Mark whitespace before
            j = i - 1;
            cur = ti->glyphs + j;
            // Use > instead of >= to avoid UB from moving the pointer outside valid range.
            // White space at j == 0 was already trimmed in the "leading" loop before anyway.
            while (j > 0 && IS_WHITESPACE(cur)) {
                cur->skip = true;
                cur->is_trimmed_whitespace = true;
                cur = ti->glyphs + --j;
            }

            // A break itself can contain a whitespace, too
            cur = ti->glyphs + i;
            if (cur->symbol == ' ' || cur->symbol == '\n') {
                cur->skip = true;
                cur->is_trimmed_whitespace = true;
                // Mark whitespace after
                j = i + 1;
                cur = ti->glyphs + j;
                while (j < ti->length && IS_WHITESPACE(cur)) {
                    cur->skip = true;
                    cur->is_trimmed_whitespace = true;
                    cur = ti->glyphs + ++j;
                }
                i = j - 1;
            }
            if (cur < ti->glyphs + ti->length)
                cur->starts_new_run = true;
        }
    }
}
#undef IS_WHITESPACE

#ifdef CONFIG_UNIBREAK
    #define ALLOWBREAK(glyph, index) (unibrks ? unibrks[index] == LINEBREAK_ALLOWBREAK : glyph == ' ')
    #define FORCEBREAK(glyph, index) (unibrks ? unibrks[index] == LINEBREAK_MUSTBREAK  : glyph == '\n')
#else
    #define ALLOWBREAK(glyph, index) (glyph == ' ')
    #define FORCEBREAK(glyph, index) (glyph == '\n')
#endif

/*
 * Starts a new line on the first breakable character after overflow
 */
static void
wrap_lines_naive(RenderContext *state, double max_text_width, char *unibrks)
{
    ASS_Renderer *render_priv = state->renderer;
    TextInfo *text_info = &state->text_info;
    GlyphInfo *s1  = text_info->glyphs; // current line start
    int last_breakable = -1;
    int break_type = 0;

    text_info->n_lines = 1;
    for (int i = 0; i < text_info->length; ++i) {
        GlyphInfo *cur = text_info->glyphs + i;
        int break_at = -1;
        double s_offset = d6_to_double(s1->bbox.x_min + s1->pos.x);
        double len = d6_to_double(cur->bbox.x_max + cur->pos.x) - s_offset;

        if (FORCEBREAK(cur->symbol, i)) {
            break_type = 2;
            break_at = i;
            ass_msg(render_priv->library, MSGL_DBG2,
                    "forced line break at %d", break_at);
        } else if (len >= max_text_width &&
                   cur->symbol != ' ' /* get trimmed */ &&
                   (state->wrap_style != 2)) {
            break_type = 1;
            break_at = last_breakable;
            if (break_at >= 0)
                ass_msg(render_priv->library, MSGL_DBG2, "line break at %d",
                        break_at);
        }
        if (ALLOWBREAK(cur->symbol, i)) {
            last_breakable = i;
        }

        if (break_at != -1 && break_at + 1 < text_info->length) {
            // need to use one more line
            if (text_info->n_lines >= text_info->max_lines) {
                // Try to raise the maximum number of lines
                bool success = false;
                if (text_info->max_lines <= INT_MAX / 2) {
                    text_info->max_lines *= 2;
                    success = ASS_REALLOC_ARRAY(text_info->lines, text_info->max_lines);
                }
                // If realloc fails it's screwed and due to error-info not propagating (FIXME),
                // the best we can do is to avoid UB by discarding the previous break
                if (!success) {
                    s1->linebreak = 0;
                    text_info->n_lines--;
                }
            }

            // marking break_at+1 as start of a new line
            int lead = break_at + 1; // the first symbol of the new line
            text_info->glyphs[lead].linebreak = break_type;
            last_breakable = -1;
            s1 = text_info->glyphs + lead;
            text_info->n_lines++;
        }
    }
}

/*
 * Rewind from a linestart position back to the first non-whitespace (0x20)
 * character. Trailing ASCII whitespace gets trimmed in rendering.
 * Assumes both arguments are part of the same array.
 * start2 is never dereferenced.
 */
static inline GlyphInfo *rewind_trailing_spaces(GlyphInfo *start1, GlyphInfo* start2)
{
    GlyphInfo *g = start2;
    do {
        --g;
    } while ((g > start1) && (g->symbol == ' '));
    return g;
}

/*
 * Shift soft linebreaks to balance out line lengths
 * Does not change the linebreak count
 * FIXME: implement style 0 and 3 correctly
 */
static void
wrap_lines_rebalance(RenderContext *state, double max_text_width, char *unibrks)
{
    TextInfo *text_info = &state->text_info;
    int exit = 0;

#define DIFF(x,y) (((x) < (y)) ? (y - x) : (x - y))
    while (!exit && state->wrap_style != 1) {
        exit = 1;
        GlyphInfo  *s1, *s2, *s3;
        s3 = text_info->glyphs;
        s1 = s2 = 0;
        for (int i = 0; i <= text_info->length; ++i) {
            GlyphInfo *cur = text_info->glyphs + i;
            if ((i == text_info->length) || cur->linebreak) {
                s1 = s2;
                s2 = s3;
                // WARNING: this may point one past the end and thus
                // must ONLY be used for pointer comparison; never dereferenced!
                s3 = cur;
                if (s1 && (s2->linebreak == 1)) {       // have at least 2 lines, and linebreak is 'soft'
                    double l1, l2, l1_new, l2_new;

                    // Find last word of line and trim surrounding whitespace before measuring
                    // (whitespace ' ' will also get trimmed in rendering)
                    GlyphInfo *w = rewind_trailing_spaces(s1, s2);
                    GlyphInfo *e1_old = w;
                    while ((w > s1) && (!ALLOWBREAK(w->symbol, w - text_info->glyphs))) {
                        --w;
                    }
                    GlyphInfo *e1 = w;
                    while ((e1 > s1) && (e1->symbol == ' ')) {
                        --e1;
                    }
                    if (w->symbol == ' ')
                        ++w;
                    if (w == s1)
                        continue; // Merging linebreaks is never beneficial

                    GlyphInfo *e2 = rewind_trailing_spaces(s2, s3);

                    l1 = d6_to_double(
                        (e1_old->bbox.x_max + e1_old->pos.x) -
                        (s1->bbox.x_min + s1->pos.x));
                    l2 = d6_to_double(
                        (e2->bbox.x_max + e2->pos.x) -
                        (s2->bbox.x_min + s2->pos.x));
                    l1_new = d6_to_double(
                        (e1->bbox.x_max + e1->pos.x) -
                        (s1->bbox.x_min + s1->pos.x));
                    l2_new = d6_to_double(
                        (e2->bbox.x_max + e2->pos.x) -
                        (w->bbox.x_min + w->pos.x));

                    if (DIFF(l1_new, l2_new) < DIFF(l1, l2)) {
                        w->linebreak = 1;
                        s2->linebreak = 0;
                        s2 = w;
                        exit = 0;
                    }
                }
            }
            if (i == text_info->length)
                break;
        }

    }
    assert(text_info->n_lines >= 1);
#undef DIFF
}

#undef ALLOWBREAK
#undef FORCEBREAK

/**
 * \brief rearrange text between lines
 * \param max_text_width maximal text line width in pixels
 * The algo is similar to the one in libvo/sub.c:
 * 1. Place text, wrapping it when current line is full
 * 2. Try moving words from the end of a line to the beginning of the next one while it reduces
 * the difference in lengths between this two lines.
 * The result may not be optimal, but usually is good enough.
 *
 * FIXME: implement style 0 and 3 correctly
 */
static void
wrap_lines_smart(RenderContext *state, double max_text_width)
{
    char *unibrks = NULL;

#ifdef CONFIG_UNIBREAK
    ASS_Renderer *render_priv = state->renderer;
    TextInfo *text_info = &state->text_info;
    if (render_priv->track->parser_priv->feature_flags & FEATURE_MASK(ASS_FEATURE_WRAP_UNICODE)) {
        unibrks = text_info->breaks;
        set_linebreaks_utf32(
            text_info->event_text, text_info->length,
            render_priv->track->Language, unibrks);
#if UNIBREAK_VERSION < 0x0500UL
        // Prior to 5.0 libunibreaks always ended text with LINE_BREAKMUSTBREAK, matching
        // Unicode spec, but messing with our text-overflow detection.
        // Thus reevaluate the last char in a different context.
        // (Later versions set either MUSTBREAK or the newly added INDETERMINATE)
        unibrks[text_info->length - 1] = is_line_breakable(
            text_info->event_text[text_info->length - 1],
            ' ',
            render_priv->track->Language
        );
#endif
    }
#endif

    wrap_lines_naive(state, max_text_width, unibrks);
    wrap_lines_rebalance(state, max_text_width, unibrks);

    trim_whitespace(state);
    measure_text(state);
}

/**
 * \brief Calculate base point for positioning and rotation
 * \param bbox text bbox
 * \param alignment alignment
 * \param bx, by out: base point coordinates
 */
static void get_base_point(ASS_DRect *bbox, int alignment, double *bx, double *by)
{
    const int halign = alignment & 3;
    const int valign = alignment & 12;
    if (bx)
        switch (halign) {
        case HALIGN_LEFT:
            *bx = bbox->x_min;
            break;
        case HALIGN_CENTER:
            *bx = (bbox->x_max + bbox->x_min) / 2.0;
            break;
        case HALIGN_RIGHT:
            *bx = bbox->x_max;
            break;
        }
    if (by)
        switch (valign) {
        case VALIGN_TOP:
            *by = bbox->y_min;
            break;
        case VALIGN_CENTER:
            *by = (bbox->y_max + bbox->y_min) / 2.0;
            break;
        case VALIGN_SUB:
            *by = bbox->y_max;
            break;
        }
}

/**
 * \brief Adjust the glyph's font size and scale factors to ensure smooth
 *  scaling and handle pathological font sizes. The main problem here is
 *  freetype's grid fitting, which destroys animations by font size, or will
 *  result in incorrect final text size if font sizes are very small and
 *  scale factors very large. See Google Code issue #46.
 * \param priv guess what
 * \param glyph the glyph to be modified
 */
static void
fix_glyph_scaling(ASS_Renderer *priv, GlyphInfo *glyph)
{
    double ft_size;
    if (priv->settings.hinting == ASS_HINTING_NONE) {
        // arbitrary, not too small to prevent grid fitting rounding effects
        // XXX: this is a rather crude hack
        ft_size = 256.0;
    } else {
        // If hinting is enabled, we want to pass the real font size
        // to freetype. Normalize scale_y to 1.0.
        ft_size = glyph->scale_y * glyph->font_size;
    }

    if (!ft_size || !glyph->font_size)
        return;

    double mul = glyph->font_size / ft_size;
    glyph->scale_fix = 1 / mul;
    glyph->scale_x *= mul;
    glyph->scale_y *= mul;
    glyph->font_size = ft_size;
}

// Initial run splitting based purely on the characters' styles
static void split_style_runs(RenderContext *state)
{
    TextInfo *text_info = &state->text_info;
    Effect last_effect_type = text_info->glyphs[0].effect_type;
    text_info->glyphs[0].starts_new_run = true;
    for (int i = 1; i < text_info->length; i++) {
        GlyphInfo *info = text_info->glyphs + i;
        GlyphInfo *last = text_info->glyphs + (i - 1);
        Effect effect_type = info->effect_type;
        info->starts_new_run =
            info->effect_timing ||  // but ignore effect_skip_timing
            (effect_type != EF_NONE && effect_type != last_effect_type) ||
            info->drawing_text.str ||
            last->drawing_text.str ||
            !ass_string_equal(last->font->desc.family, info->font->desc.family) ||
            last->font->desc.vertical != info->font->desc.vertical ||
            last->font_size != info->font_size ||
            last->c[0] != info->c[0] ||
            last->c[1] != info->c[1] ||
            last->c[2] != info->c[2] ||
            last->c[3] != info->c[3] ||
            last->be != info->be ||
            last->blur != info->blur ||
            last->shadow_x != info->shadow_x ||
            last->shadow_y != info->shadow_y ||
            last->frx != info->frx ||
            last->fry != info->fry ||
            last->frz != info->frz ||
            last->fax != info->fax ||
            last->fay != info->fay ||
            last->scale_x != info->scale_x ||
            last->scale_y != info->scale_y ||
            last->border_style != info->border_style ||
            last->border_x != info->border_x ||
            last->border_y != info->border_y ||
            last->hspacing != info->hspacing ||
            last->italic != info->italic ||
            last->bold != info->bold ||
            ((last->flags ^ info->flags) & ~DECO_ROTATE);
        if (effect_type != EF_NONE)
            last_effect_type = effect_type;
    }
}

// Parse event text.
// Fill render_priv->text_info.
static bool parse_events(RenderContext *state, ASS_Event *event)
{
    TextInfo *text_info = &state->text_info;
    ASS_Renderer *render_priv = state->renderer;

    char *p = event->Text, *q;

    // Event parsing.
    while (true) {
        ASS_StringView drawing_text = {NULL, 0};

        // get next char, executing style override
        // this affects render_context
        unsigned code = 0;
        while (*p) {
            if (*p == '{') {
                q = p + 1;
                while (*q && *q != '}')
                    q++;
                if (*q == '}') {
                    p = ass_parse_tags(state, p, q, 1., false);
                    assert(*p == '}');
                    p++;
                    continue;
                }
            }
            if (state->drawing_scale) {
                q = p;
                if (*p == '{')
                    q++;
                while ((*q != '{') && (*q != 0))
                    q++;
                drawing_text.str = p;
                drawing_text.len = q - p;
                code = 0xfffc; // object replacement character
                p = q;
                break;
            } else {
                code = ass_get_next_char(state, &p);
                break;
            }
        }

        if (code == 0)
            break;

        // face could have been changed in get_next_char
        if (!state->font)
            goto fail;

        if (text_info->length >= text_info->max_glyphs) {
            // Raise maximum number of glyphs
            int new_max = 2 * FFMIN(FFMAX(text_info->max_glyphs, text_info->length / 2 + 1),
                                    INT_MAX / 2);
            if (text_info->length >= new_max)
                goto fail;
            if (!ASS_REALLOC_ARRAY(text_info->glyphs, new_max) ||
                    !ASS_REALLOC_ARRAY(text_info->event_text, new_max) ||
                    !ASS_REALLOC_ARRAY(text_info->breaks, new_max))
                goto fail;
            text_info->max_glyphs = new_max;
        }

        GlyphInfo *info = &text_info->glyphs[text_info->length];

        // Clear current GlyphInfo
        memset(info, 0, sizeof(GlyphInfo));

        // Parse drawing
        if (drawing_text.str) {
            info->drawing_text = drawing_text;
            info->drawing_scale = state->drawing_scale;
            info->drawing_pbo = state->pbo;
        }

        // Fill glyph information
        info->symbol = code;
        info->font = state->font;
        for (int i = 0; i < 4; i++)
            info->c[i] = state->c[i];

        info->effect_type = state->effect_type;
        info->effect_timing = state->effect_timing;
        info->effect_skip_timing = state->effect_skip_timing;
        info->reset_effect = state->reset_effect;
        // VSFilter compatibility: font glyphs use PlayResY scaling in both dimensions
        info->font_size =
            fabs(state->font_size * state->screen_scale_y);
        info->be = state->be;
        info->blur = state->blur;
        info->shadow_x = state->shadow_x;
        info->shadow_y = state->shadow_y;
        info->scale_x = state->scale_x;
        info->scale_y = state->scale_y;
        info->border_style = state->border_style;
        info->border_x = state->border_x;
        info->border_y = state->border_y;
        info->hspacing = state->hspacing;
        info->bold = state->bold;
        info->italic = state->italic;
        info->flags = state->flags;
        if (info->font->desc.vertical && code >= VERTICAL_LOWER_BOUND)
            info->flags |= DECO_ROTATE;
        info->frx = state->frx;
        info->fry = state->fry;
        info->frz = state->frz;
        info->fax = state->fax;
        info->fay = state->fay;
        info->fade = state->fade;

        info->hspacing_scaled = 0;
        info->scale_fix = 1;

        if (!drawing_text.str) {
            info->hspacing_scaled = double_to_d6(info->hspacing *
                    state->screen_scale_x / render_priv->par_scale_x *
                    info->scale_x);
            fix_glyph_scaling(render_priv, info);
        }

        text_info->length++;

        state->effect_type = EF_NONE;
        state->effect_timing = 0;
        state->effect_skip_timing = 0;
        state->reset_effect = false;
    }

    return true;

fail:
    free_render_context(state);
    return false;
}

// Process render_priv->text_info and load glyph outlines.
static void retrieve_glyphs(RenderContext *state)
{
    GlyphInfo *glyphs = state->text_info.glyphs;
    int i;

    for (i = 0; i < state->text_info.length; i++) {
        GlyphInfo *info = glyphs + i;
        do {
            get_outline_glyph(state, info);
            info = info->next;
        } while (info);
        info = glyphs + i;

        // Add additional space after italic to non-italic style changes
        if (i && glyphs[i - 1].italic && !info->italic) {
            int back = i - 1;
            GlyphInfo *og = &glyphs[back];
            while (back && og->bbox.x_max - og->bbox.x_min == 0
                    && og->italic)
                og = &glyphs[--back];
            if (og->bbox.x_max > og->cluster_advance.x)
                og->cluster_advance.x = og->bbox.x_max;
        }

        // add horizontal letter spacing
        info->cluster_advance.x += info->hspacing_scaled;
    }
}

// Preliminary layout (for line wrapping)
static void preliminary_layout(RenderContext *state)
{
    ASS_Vector pen = { 0, 0 };
    for (int i = 0; i < state->text_info.length; i++) {
        GlyphInfo *info = state->text_info.glyphs + i;
        ASS_Vector cluster_pen = pen;
        do {
            info->pos.x = cluster_pen.x;
            info->pos.y = cluster_pen.y;

            cluster_pen.x += info->advance.x;
            cluster_pen.y += info->advance.y;

            info = info->next;
        } while (info);
        info = state->text_info.glyphs + i;
        pen.x += info->cluster_advance.x;
        pen.y += info->cluster_advance.y;
    }
}

// Reorder text into visual order
static void reorder_text(RenderContext *state)
{
    ASS_Renderer *render_priv = state->renderer;
    TextInfo *text_info = &state->text_info;
    FriBidiStrIndex *cmap = ass_shaper_reorder(state->shaper, text_info);
    if (!cmap) {
        ass_msg(render_priv->library, MSGL_ERR, "Failed to reorder text");
        ass_shaper_cleanup(state->shaper, text_info);
        free_render_context(state);
        return;
    }

    // Reposition according to the map
    ASS_Vector pen = { 0, 0 };
    int lineno = 1;
    for (int i = 0; i < text_info->length; i++) {
        GlyphInfo *info = text_info->glyphs + cmap[i];
        if (text_info->glyphs[i].linebreak) {
            pen.x = 0;
            pen.y += double_to_d6(text_info->lines[lineno-1].desc);
            pen.y += double_to_d6(text_info->lines[lineno].asc);
            pen.y += double_to_d6(render_priv->settings.line_spacing);
            lineno++;
        }
        if (info->skip)
            continue;
        ASS_Vector cluster_pen = pen;
        pen.x += info->cluster_advance.x;
        pen.y += info->cluster_advance.y;
        while (info) {
            info->pos.x = info->offset.x + cluster_pen.x;
            info->pos.y = info->offset.y + cluster_pen.y;
            cluster_pen.x += info->advance.x;
            cluster_pen.y += info->advance.y;
            info = info->next;
        }
    }
}

static void apply_baseline_shear(RenderContext *state)
{
    ASS_Renderer *render_priv = state->renderer;
    TextInfo *text_info = &state->text_info;
    FriBidiStrIndex *cmap = ass_shaper_get_reorder_map(state->shaper);
    int32_t shear = 0;
    bool whole_text_layout =
        render_priv->track->parser_priv->feature_flags &
        FEATURE_MASK(ASS_FEATURE_WHOLE_TEXT_LAYOUT);
    for (int i = 0; i < text_info->length; i++) {
        GlyphInfo *info = text_info->glyphs + cmap[i];
        if (text_info->glyphs[i].linebreak ||
            (!whole_text_layout && text_info->glyphs[i].starts_new_run))
            shear = 0;
        if (!info->scale_x || !info->scale_y)
            info->skip = true;
        if (info->skip)
            continue;
        double fay = info->fay / info->scale_x * info->scale_y;
        for (GlyphInfo *cur = info; cur; cur = cur->next) {
            cur->pos.y += shear + fay * cur->offset.x;
            shear += fay * cur->advance.x;
        }
    }
}

static void align_lines(RenderContext *state, double max_text_width)
{
    TextInfo *text_info = &state->text_info;
    GlyphInfo *glyphs = text_info->glyphs;
    int i, j;
    double width = 0;
    int last_break = -1;
    int halign = state->alignment & 3;
    int justify = state->justify;
    double max_width = 0;

    if (state->evt_type & EVENT_HSCROLL) {
        justify = halign;
        halign = HALIGN_LEFT;
    }

    for (i = 0; i <= text_info->length; ++i) {   // (text_info->length + 1) is the end of the last line
        if ((i == text_info->length) || glyphs[i].linebreak) {
            max_width = FFMAX(max_width,width);
            width = 0;
        }
        if (i < text_info->length && !glyphs[i].skip &&
                glyphs[i].symbol != '\n' && glyphs[i].symbol != 0) {
            width += d6_to_double(glyphs[i].cluster_advance.x);
        }
    }
    for (i = 0; i <= text_info->length; ++i) {   // (text_info->length + 1) is the end of the last line
        if ((i == text_info->length) || glyphs[i].linebreak) {
            double shift = 0;
            if (halign == HALIGN_LEFT) {    // left aligned, no action
                if (justify == ASS_JUSTIFY_RIGHT) {
                    shift = max_width - width;
                } else if (justify == ASS_JUSTIFY_CENTER) {
                    shift = (max_width - width) / 2.0;
                } else {
                    shift = 0;
                }
            } else if (halign == HALIGN_RIGHT) {    // right aligned
                if (justify == ASS_JUSTIFY_LEFT) {
                    shift = max_text_width - max_width;
                } else if (justify == ASS_JUSTIFY_CENTER) {
                    shift = max_text_width - max_width + (max_width - width) / 2.0;
                } else {
                    shift = max_text_width - width;
                }
            } else if (halign == HALIGN_CENTER) {   // centered
                if (justify == ASS_JUSTIFY_LEFT) {
                    shift = (max_text_width - max_width) / 2.0;
                } else if (justify == ASS_JUSTIFY_RIGHT) {
                    shift = (max_text_width - max_width) / 2.0 + max_width - width;
                } else {
                    shift = (max_text_width - width) / 2.0;
                }
            }
            for (j = last_break + 1; j < i; ++j) {
                GlyphInfo *info = glyphs + j;
                while (info) {
                    info->pos.x += double_to_d6(shift);
                    info = info->next;
                }
            }
            last_break = i - 1;
            width = 0;
        }
        if (i < text_info->length && !glyphs[i].skip &&
                glyphs[i].symbol != '\n' && glyphs[i].symbol != 0) {
            width += d6_to_double(glyphs[i].cluster_advance.x);
        }
    }
}

static void calculate_rotation_params(RenderContext *state, ASS_DRect *bbox,
                                      double device_x, double device_y)
{
    ASS_Renderer *render_priv = state->renderer;
    ASS_DVector center;
    if (state->have_origin) {
        center.x = x2scr_pos(render_priv, state->org_x);
        center.y = y2scr_pos(render_priv, state->org_y);
    } else {
        double bx = 0., by = 0.;
        get_base_point(bbox, state->alignment, &bx, &by);
        center.x = device_x + bx;
        center.y = device_y + by;
    }

    TextInfo *text_info = &state->text_info;
    for (int i = 0; i < text_info->length; i++) {
        GlyphInfo *info = text_info->glyphs + i;
        while (info) {
            info->shift.x = info->pos.x + double_to_d6(device_x - center.x +
                    info->shadow_x * state->border_scale_x /
                    render_priv->par_scale_x);
            info->shift.y = info->pos.y + double_to_d6(device_y - center.y +
                    info->shadow_y * state->border_scale_y);
            info = info->next;
        }
    }
}


static int quantize_blur(double radius, int32_t *shadow_mask)
{
    // Gaussian filter kernel (1D):
    // G(x, r2) = exp(-x^2 / (2 * r2)) / sqrt(2 * pi * r2),
    // position unit is 1/64th of pixel, r = 64 * radius, r2 = r^2.

    // Difference between kernels with different but near r2:
    // G(x, r2 + dr2) - G(x, r2) ~= dr2 * G(x, r2) * (x^2 - r2) / (2 * r2^2).
    // Maximal possible error relative to full pixel value is half of
    // integral (from -inf to +inf) of absolute value of that difference.
    // E_max ~= dr2 / 2 * integral(G(x, r2) * |x^2 - r2| / (2 * r2^2), x)
    //  = dr2 / (4 * r2) * integral(G(y, 1) * |y^2 - 1|, y)
    //  = dr2 / (4 * r2) * 4 / sqrt(2 * pi * e)
    //  ~ dr2 / (4 * r2) ~= dr / (2 * r).
    // E_max ~ BLUR_PRECISION / 2 as we have 2 dimensions.

    // To get discretized blur radius solve the following
    // differential equation (n--quantization index):
    // dr(n) / dn = BLUR_PRECISION * r + POSITION_PRECISION, r(0) = 0,
    // r(n) = (exp(BLUR_PRECISION * n) - 1) * POSITION_PRECISION / BLUR_PRECISION,
    // n = log(1 + r * BLUR_PRECISION / POSITION_PRECISION) / BLUR_PRECISION.

    // To get shadow offset quantization estimate difference of
    // G(x + dx, r2) - G(x, r2) ~= dx * G(x, r2) * (-x / r2).
    // E_max ~= dx / 2 * integral(G(x, r2) * |x| / r2, x)
    //  = dx / sqrt(2 * pi * r2) ~ dx / (2 * r).
    // 2^ord ~ dx ~ BLUR_PRECISION * r + POSITION_PRECISION.

    const double scale = 64 * BLUR_PRECISION / POSITION_PRECISION;
    radius *= scale;

    int ord;
    // ord = floor(log2(BLUR_PRECISION * r + POSITION_PRECISION))
    //     = floor(log2(64 * radius * BLUR_PRECISION + POSITION_PRECISION))
    //     = floor(log2((radius * scale + 1) * POSITION_PRECISION)),
    // floor(log2(x)) = frexp(x) - 1 = frexp(x / 2).
    frexp((1 + radius) * (POSITION_PRECISION / 2), &ord);
    *shadow_mask = ((uint32_t) 1 << ord) - 1;
    return ass_lrint(log1p(radius) / BLUR_PRECISION);
}

static double restore_blur(int qblur)
{
    const double scale = 64 * BLUR_PRECISION / POSITION_PRECISION;
    double sigma = expm1(BLUR_PRECISION * qblur) / scale;
    return sigma * sigma;
}

struct composite_construct_ctx {
    ASS_Renderer *renderer;
    const CompositeHashValue *base;
};

static bool composite_clip_moving(ASS_Renderer *r, const CombinedBitmapInfo *info,
                                  int64_t x0, int64_t y0, int64_t x1, int64_t y1)
{
    const BitmapRef *bitmaps = info->bitmaps ? info->bitmaps : &info->inline_bitmap;
    const BitmapRef *first = &bitmaps[0];
    const BitmapRef *last = &bitmaps[info->bitmap_count - 1];
    uint64_t h = UINT64_C(1469598103934665603);
    const uint64_t words[6] = {
        (uint64_t) info->bitmap_count,
        (uint64_t) (uintptr_t) first->bm ^ ((uint64_t) (uintptr_t) first->bm_o << 1),
        (uint64_t) (uintptr_t) last->bm ^ ((uint64_t) (uintptr_t) last->bm_o << 1),
        ((uint64_t) (uint32_t) info->filter.blur_x << 32) | (uint32_t) info->filter.blur_y,
        (uint64_t) (uint32_t) info->filter.be,
        ((uint64_t) (uint32_t) info->x << 32) | (uint32_t) info->y,
    };
    for (int i = 0; i < 6; i++) {
        h ^= words[i];
        h *= UINT64_C(1099511628211);
        h ^= h >> 29;
    }
    if (!h)
        h = 1;
    ClipMemoEntry *m = &r->composite_clip_memo[(h ^ (h >> 32)) & (CLIP_MEMO_SIZE - 1)];
    if (m->hash != h) {
        m->hash = h;
        m->x0 = x0;
        m->y0 = y0;
        m->x1 = x1;
        m->y1 = y1;
        m->last_time = r->time;
        m->moving = false;
        return false;
    }
    if (!m->moving &&
        (m->x0 != x0 || m->y0 != y0 || m->x1 != x1 || m->y1 != y1))
        m->moving = true;
    m->last_time = r->time;
    return m->moving;
}

// Convert glyphs to bitmaps, combine them, apply blur, generate shadows.
static void set_composite_viewport(RenderContext *state, CombinedBitmapInfo *info,
                                    CompositeHashKey *key)
{
    ASS_Renderer *renderer = state->renderer;
    if (state->detect_collisions ||
        !(state->evt_type & EVENT_POSITIONED) ||
        (info->filter.flags & FILTER_BORDER_STYLE_3) ||
        info->filter.be < 0 || info->filter.be > 16 ||
        renderer->settings.frame_width <= 0 || renderer->settings.frame_height <= 0)
        return;
    double r2x = restore_blur(info->filter.blur_x);
    double r2y = restore_blur(info->filter.blur_y);
    if (!isfinite(r2x) || !isfinite(r2y) || r2x > 65536 || r2y > 65536)
        return;
    int px = 0, py = 0, ax = 1, ay = 1;
    int blur_mode = renderer->experimental_edges;
    ass_gaussian_blur_bounds(r2x, blur_mode, &px, &ax);
    ass_gaussian_blur_bounds(r2y, blur_mode, &py, &ay);
    int64_t pad_x = px + info->filter.be + 4 + (llabs((int64_t)info->filter.shadow.x) + 63) / 64;
    int64_t pad_y = py + info->filter.be + 4 + (llabs((int64_t)info->filter.shadow.y) + 63) / 64;
    int64_t vis_x0 = 0;
    int64_t vis_y0 = 0;
    int64_t vis_x1 = renderer->settings.frame_width;
    int64_t vis_y1 = renderer->settings.frame_height;
    if (!state->clip_mode) {
        vis_x0 = FFMAX(vis_x0, (int64_t) state->clip_x0);
        vis_y0 = FFMAX(vis_y0, (int64_t) state->clip_y0);
        vis_x1 = FFMIN(vis_x1, (int64_t) state->clip_x1);
        vis_y1 = FFMIN(vis_y1, (int64_t) state->clip_y1);
    }
    double vx0, vy0, vx1, vy1;
    bool has_vector_clip = get_vector_clip_bounds(state, &vx0, &vy0, &vx1, &vy1);
    if (has_vector_clip) {
        vis_x0 = FFMAX(vis_x0, (int64_t) floor(vx0));
        vis_y0 = FFMAX(vis_y0, (int64_t) floor(vy0));
        vis_x1 = FFMIN(vis_x1, (int64_t) ceil(vx1));
        vis_y1 = FFMIN(vis_y1, (int64_t) ceil(vy1));
    }
    int64_t bx0 = INT64_MAX, by0 = INT64_MAX;
    int64_t bx1 = INT64_MIN, by1 = INT64_MIN;
    const BitmapRef *bitmaps = info->bitmaps ? info->bitmaps : &info->inline_bitmap;
    for (int i = 0; i < info->bitmap_count; i++) {
        const BitmapRef *ref = bitmaps + i;
        Bitmap *bm[2] = {ref->bm, ref->bm_o};
        ASS_Vector pos[2] = {ref->pos, ref->pos_o};
        for (int j = 0; j < 2; j++) {
            if (!bm[j]) continue;
            int64_t left = (int64_t)pos[j].x + bm[j]->left;
            int64_t top = (int64_t)pos[j].y + bm[j]->top;
            bx0 = FFMIN(bx0, left);
            by0 = FFMIN(by0, top);
            bx1 = FFMAX(bx1, left + bm[j]->w);
            by1 = FFMAX(by1, top + bm[j]->h);
        }
    }
    if (bx1 <= bx0 || by1 <= by0)
        return;
    double area = (double)(bx1 - bx0) * (by1 - by0);
    int64_t x0 = vis_x0 - info->x - pad_x;
    int64_t y0 = vis_y0 - info->y - pad_y;
    int64_t x1 = vis_x1 - info->x + pad_x;
    int64_t y1 = vis_y1 - info->y + pad_y;
    bool conservative = !renderer->experimental_edges;
    if (conservative) {
        // Quantize outward to retain cache reuse while preserving the blur halo.
        int64_t q = area >= 1024 * 1024 ? 128 : 64;
        x0 &= -q;
        y0 &= -q;
        x1 = (x1 + q - 1) & -q;
        y1 = (y1 + q - 1) & -q;
    }
    if (x0 < INT32_MIN || y0 < INT32_MIN || x1 > INT32_MAX || y1 > INT32_MAX)
        return;
    int viewport_align = FFMAX(1 << renderer->engine.align_order, FFMAX(ax, ay));
    int64_t crop_guard = conservative ? viewport_align - 1 + info->filter.be : 0;
    double visible = (double)FFMAX(0, FFMIN(bx1, x1 + crop_guard) - FFMAX(bx0, x0 - crop_guard)) *
        FFMAX(0, FFMIN(by1, y1 + crop_guard) - FFMAX(by0, y0 - crop_guard));
    // Preserve position-independent cache reuse for shared/wiping runs, while
    // cropping huge off-screen runs and vector-clipped blurred runs tightly.
    bool giant_worthwhile = area >= 1024 * 1024 &&
        area - visible >= FFMAX(256 * 1024, area / 4);
    bool has_blur = r2x > 0.001 || r2y > 0.001 || info->filter.be > 0;
    bool clipped_worthwhile = has_blur &&
        ((visible == 0 && area >= 4096) ||
         (has_vector_clip && area >= 16 * 1024 &&
          visible <= area / 2 && area - visible >= 12 * 1024)) &&
        !composite_clip_moving(renderer, info, x0, y0, x1, y1);
    if (giant_worthwhile || clipped_worthwhile) {
        key->viewport = (ASS_Rect){x0, y0, x1, y1};
        key->viewport_align = viewport_align;
    }
}

static void render_and_combine_glyphs(RenderContext *state,
                                      double device_x, double device_y)
{
    ASS_Renderer *render_priv = state->renderer;
    TextInfo *text_info = &state->text_info;
    int left = render_priv->settings.left_margin;
    device_x = (device_x - left) * render_priv->par_scale_x + left;
    unsigned nb_bitmaps = 0;
    bool new_run = true;
    CombinedBitmapInfo *combined_info = text_info->combined_bitmaps;
    CombinedBitmapInfo *current_info = NULL;
    ASS_DVector offset;
    for (int i = 0; i < text_info->length; i++) {
        GlyphInfo *info = text_info->glyphs + i;
        if (info->starts_new_run) new_run = true;
        if (info->skip)
            continue;

        for (; info; info = info->next) {
            int flags = 0;
            if (info->border_style == 3)
                flags |= FILTER_BORDER_STYLE_3;
            if (info->border_x || info->border_y)
                flags |= FILTER_NONZERO_BORDER;
            uint32_t shad_c = info->c[3];
            ass_apply_fade(&shad_c, info->fade);
            bool shadow_visible = _a(shad_c) != 0xFF ||
                info->border_style == 3 || info->border_style == 4;
            if ((info->shadow_x || info->shadow_y) && shadow_visible)
                flags |= FILTER_NONZERO_SHADOW;
            if (flags & FILTER_NONZERO_SHADOW &&
                (info->effect_type == EF_KARAOKE_KF ||
                 info->effect_type == EF_KARAOKE_KO ||
                 _a(info->c[0]) != 0xFF ||
                 info->border_style == 3))
                flags |= FILTER_FILL_IN_SHADOW;
            if (!(flags & FILTER_NONZERO_BORDER) &&
                !(flags & FILTER_FILL_IN_SHADOW))
                flags &= ~FILTER_NONZERO_SHADOW;
            if ((flags & FILTER_NONZERO_BORDER &&
                 _a(info->c[0]) == 0 &&
                 _a(info->c[1]) == 0 &&
                 info->fade == 0) ||
                info->border_style == 3)
                flags |= FILTER_FILL_IN_BORDER;

            if (new_run) {
                if (nb_bitmaps >= text_info->max_bitmaps) {
                    size_t new_size = 2 * text_info->max_bitmaps;
                    if (!ASS_REALLOC_ARRAY(text_info->combined_bitmaps, new_size))
                        continue;

                    text_info->max_bitmaps = new_size;
                    combined_info = text_info->combined_bitmaps;
                }
                current_info = &combined_info[nb_bitmaps];

                memcpy(&current_info->c, &info->c, sizeof(info->c));
                for (int i = 0; i < 4; i++)
                    ass_apply_fade(&current_info->c[i], info->fade);

                current_info->effect_type = info->effect_type;
                current_info->effect_timing = info->effect_timing;
                current_info->leftmost_x = OUTLINE_MAX;

                FilterDesc *filter = &current_info->filter;
                filter->flags = flags;
                filter->be = info->be;

                int32_t shadow_mask_x, shadow_mask_y;
                double blur_radius_scale = 2 / sqrt(log(256));
                double blur_scale_x = state->blur_scale_x * blur_radius_scale;
                double blur_scale_y = state->blur_scale_y * blur_radius_scale;
                filter->blur_x = quantize_blur(info->blur * blur_scale_x, &shadow_mask_x);
                filter->blur_y = quantize_blur(info->blur * blur_scale_y, &shadow_mask_y);
                if (flags & FILTER_NONZERO_SHADOW) {
                    int32_t x = double_to_d6(info->shadow_x * state->border_scale_x);
                    int32_t y = double_to_d6(info->shadow_y * state->border_scale_y);
                    filter->shadow.x = (x + (shadow_mask_x >> 1)) & ~shadow_mask_x;
                    filter->shadow.y = (y + (shadow_mask_y >> 1)) & ~shadow_mask_y;
                } else
                    filter->shadow.x = filter->shadow.y = 0;

                current_info->x = current_info->y = INT_MAX;
                current_info->bm = current_info->bm_o = current_info->bm_s = NULL;
                current_info->image = NULL;

                current_info->bitmap_count = 0;
                current_info->bitmaps = NULL;
                current_info->max_bitmap_count = 1;

                nb_bitmaps++;
                new_run = false;
            }
            assert(current_info);

            ASS_Vector pos, pos_o;
            info->pos.x = double_to_d6(device_x + d6_to_double(info->pos.x) * render_priv->par_scale_x);
            info->pos.y = double_to_d6(device_y) + info->pos.y;
            get_bitmap_glyph(state, info, &current_info->leftmost_x, &pos, &pos_o,
                             &offset, !current_info->bitmap_count, flags, &current_info->filter);

            if (!info->bm && !info->bm_o)
                continue;

            if (current_info->bitmap_count >= current_info->max_bitmap_count) {
                if (!current_info->bitmaps) {
                    BitmapRef *allocated = malloc(MAX_SUB_BITMAPS_INITIAL * sizeof(BitmapRef));
                    if (!allocated)
                        continue;
                    allocated[0] = current_info->inline_bitmap;
                    current_info->bitmaps = allocated;
                    current_info->max_bitmap_count = MAX_SUB_BITMAPS_INITIAL;
                } else {
                    size_t new_size = 2 * current_info->max_bitmap_count;
                    if (!ASS_REALLOC_ARRAY(current_info->bitmaps, new_size))
                        continue;

                    current_info->max_bitmap_count = new_size;
                }
            }
            BitmapRef *dst_ref = current_info->bitmaps ?
                &current_info->bitmaps[current_info->bitmap_count] :
                &current_info->inline_bitmap;
            dst_ref->bm    = info->bm;
            dst_ref->bm_o  = info->bm_o;
            dst_ref->pos   = pos;
            dst_ref->pos_o = pos_o;
            current_info->bitmap_count++;

            current_info->x = FFMIN(current_info->x, pos.x);
            current_info->y = FFMIN(current_info->y, pos.y);
        }
    }

    for (int i = 0; i < nb_bitmaps; i++) {
        CombinedBitmapInfo *info = &combined_info[i];
        if (!info->bitmap_count) {
            free(info->bitmaps);
            continue;
        }

        if (info->effect_type == EF_KARAOKE_KF)
            info->effect_timing = lround(d6_to_double(info->leftmost_x) +
                d6_to_double(info->effect_timing) * render_priv->par_scale_x);

        BitmapRef *bitmaps = info->bitmaps ? info->bitmaps : &info->inline_bitmap;
        for (int j = 0; j < info->bitmap_count; j++) {
            bitmaps[j].pos.x -= info->x;
            bitmaps[j].pos.y -= info->y;
            bitmaps[j].pos_o.x -= info->x;
            bitmaps[j].pos_o.y -= info->y;
        }

        CompositeHashKey key = {0};
        key.filter = info->filter;
        key.bitmap_count = info->bitmap_count;
        if (!info->bitmaps) {
            key.bitmaps = NULL;
            key.single_bitmap = info->inline_bitmap;
        } else {
            key.bitmaps = info->bitmaps;
        }
        set_composite_viewport(state, info, &key);
        CompositeHashValue *val = ass_cache_find(render_priv->cache.composite_cache, &key);
        if (val) {
            free(info->bitmaps);
        } else {
            struct composite_construct_ctx ctx = { render_priv, NULL };
            int fl = key.filter.flags;
            if ((fl & FILTER_NONZERO_SHADOW) && !(fl & FILTER_BORDER_STYLE_3) &&
                (restore_blur(key.filter.blur_x) >= 1.5 ||
                 restore_blur(key.filter.blur_y) >= 1.5)) {
                CompositeHashKey base_key = key;
                base_key.filter.flags = fl & ~(FILTER_NONZERO_SHADOW | FILTER_FILL_IN_SHADOW);
                if (fl & FILTER_FILL_IN_SHADOW)
                    base_key.filter.flags |= FILTER_FILL_IN_BORDER;
                base_key.filter.shadow = (ASS_Vector) {0, 0};
                bool base_ok = true;
                if (key.bitmaps) {
                    base_key.bitmaps = malloc(key.bitmap_count * sizeof(BitmapRef));
                    if (base_key.bitmaps)
                        memcpy(base_key.bitmaps, key.bitmaps,
                               key.bitmap_count * sizeof(BitmapRef));
                    else
                        base_ok = false;
                }
                if (base_ok) {
                    struct composite_construct_ctx base_ctx = { render_priv, NULL };
                    ctx.base = ass_cache_get(render_priv->cache.composite_cache,
                                             &base_key, &base_ctx);
                }
            }
            val = ass_cache_get(render_priv->cache.composite_cache, &key, &ctx);
        }
        if (!val)
            continue;

        if (val->bm.buffer)
            info->bm = &val->bm;
        if (val->bm_o.buffer)
            info->bm_o = &val->bm_o;
        if (val->bm_s.buffer)
            info->bm_s = &val->bm_s;
        info->image = val;
        continue;
    }

    text_info->n_bitmaps = nb_bitmaps;
}

static inline void rectangle_combine(ASS_Rect *rect, const Bitmap *bm, ASS_Vector pos)
{
    pos.x += bm->left;
    pos.y += bm->top;
    rectangle_update(rect, pos.x, pos.y, pos.x + bm->w, pos.y + bm->h);
}

/*
 * To find these values, simulate blur on the border between two
 * half-planes, one zero-filled (background) and the other filled
 * with the maximum supported value (foreground). Keep incrementing
 * the \be argument. The necessary padding is the distance by which
 * the blurred foreground image extends beyond the original border
 * and into the background. Initially it increases along with \be,
 * but very soon it grinds to a halt. At some point, the blurred
 * image actually reaches a stationary point and stays unchanged
 * forever after, simply _shifting_ by one pixel for each \be
 * step--moving in the direction of the non-zero half-plane and
 * thus decreasing the necessary padding (although the large
 * padding is still needed for intermediate results). In practice,
 * images are finite rather than infinite like half-planes, but
 * this can only decrease the required padding. Half-planes filled
 * with extreme values are the theoretical limit of the worst case.
 * Make sure to use the right pixel value range in the simulation!
 */
int ass_be_padding(int be)
{
    if (be <= 3)
        return be;
    if (be <= 7)
        return 4;
    return 5;
}


static bool alloc_composite_scratch(ASS_Renderer *render_priv, Bitmap *bm,
                                    ASS_Rect rect, int border)
{
    int64_t left = (int64_t) rect.x_min - border;
    int64_t top = (int64_t) rect.y_min - border;
    int64_t width = (int64_t) rect.x_max - rect.x_min + 2 * border;
    int64_t height = (int64_t) rect.y_max - rect.y_min + 2 * border;
    if (width <= 0 || height <= 0 || width > INT_MAX || height > INT_MAX ||
        left < INT_MIN || top < INT_MIN || left > INT_MAX || top > INT_MAX)
        return false;
    unsigned align = 1U << render_priv->engine.align_order;
    size_t stride = ass_align(align, (size_t) width);
    if (stride > (INT_MAX - align) / (size_t) height)
        return false;
    size_t bytes = stride * (size_t) height + align;
    if (bytes > 8 * 1024 * 1024)
        return false;
    if (bytes > render_priv->composite_scratch_size) {
        size_t cap = ass_align(256 * 1024, bytes);
        void *buf = ass_aligned_alloc(align, cap, false);
        if (!buf)
            return false;
        ass_aligned_free(render_priv->composite_scratch);
        render_priv->composite_scratch = buf;
        render_priv->composite_scratch_size = cap;
    }
    memset(render_priv->composite_scratch, 0, bytes);
    bm->left = (int32_t) left;
    bm->top = (int32_t) top;
    bm->w = (int32_t) width;
    bm->h = (int32_t) height;
    bm->stride = (ptrdiff_t) stride;
    bm->buffer = render_priv->composite_scratch;
    return true;
}

static bool alloc_composite_bitmap(const BitmapEngine *engine, Bitmap *bm,
                                    ASS_Rect rect, int border,
                                    const CompositeHashKey *key)
{
    int64_t left = (int64_t) rect.x_min - border;
    int64_t top = (int64_t) rect.y_min - border;
    int64_t width = (int64_t) rect.x_max - rect.x_min + 2 * border;
    int64_t height = (int64_t) rect.y_max - rect.y_min + 2 * border;
    if (width <= 0 || height <= 0 || width > INT_MAX || height > INT_MAX ||
        left < INT_MIN || top < INT_MIN)
        return false;
    if (key->viewport_align) {
        // Keep the full bitmap's blur/dither origin and required filter halo.
        int64_t mask = -(int64_t) key->viewport_align;
        int64_t x0 = FFMINMAX(((int64_t) key->viewport.x_min - left) & mask, 0, width);
        int64_t y0 = FFMINMAX(((int64_t) key->viewport.y_min - top) & mask, 0, height);
        int64_t x1 = FFMINMAX(((int64_t) key->viewport.x_max - left +
                               key->viewport_align - 1) & mask, 0, width);
        int64_t y1 = FFMINMAX(((int64_t) key->viewport.y_max - top +
                               key->viewport_align - 1) & mask, 0, height);
        if (x1 <= x0 || y1 <= y0)
            return false;
        if (!key->filter.be || (x1 - x0 >= 2 && y1 - y0 >= 2)) {
            left += x0;
            top += y0;
            width = x1 - x0;
            height = y1 - y0;
        }
    }
    if (left > INT_MAX || top > INT_MAX ||
        !ass_alloc_bitmap(engine, bm, width, height, true))
        return false;
    bm->left = left;
    bm->top = top;
    return true;
}

static void add_composite_bitmap(const BitmapEngine *engine, Bitmap *dst,
                                  const Bitmap *src, ASS_Vector pos)
{
    if (!src)
        return;
    int64_t x = (int64_t) pos.x + src->left - dst->left;
    int64_t y = (int64_t) pos.y + src->top - dst->top;
    int x0 = FFMINMAX(-x, 0, src->w), x1 = FFMINMAX(dst->w - x, 0, src->w);
    int y0 = FFMINMAX(-y, 0, src->h), y1 = FFMINMAX(dst->h - y, 0, src->h);
    if (x1 <= x0 || y1 <= y0)
        return;
    engine->add_bitmaps(dst->buffer + (y + y0) * dst->stride + (x + x0), dst->stride,
                        src->buffer + (ptrdiff_t) y0 * src->stride + x0, src->stride,
                        x1 - x0, y1 - y0);
}

size_t ass_composite_construct(void *key, void *value, void *priv)
{
    struct composite_construct_ctx *ctx = priv;
    ASS_Renderer *render_priv = ctx->renderer;
    FLE_START;
    CompositeHashKey *k = key;
    CompositeHashValue *v = value;
    memset(v, 0, sizeof(*v));

    if (ctx->base) {
        const CompositeHashValue *base = ctx->base;
        int flags = k->filter.flags;
        ass_copy_bitmap(&render_priv->engine, &v->bm, &base->bm);
        ass_copy_bitmap(&render_priv->engine, &v->bm_o, &base->bm_o);
        if (flags & FILTER_NONZERO_SHADOW) {
            if (flags & FILTER_NONZERO_BORDER) {
                ass_copy_bitmap(&render_priv->engine, &v->bm_s, &v->bm_o);
                if ((flags & FILTER_FILL_IN_BORDER) && !(flags & FILTER_FILL_IN_SHADOW))
                    ass_fix_outline(&v->bm, &v->bm_s);
            } else {
                ass_copy_bitmap(&render_priv->engine, &v->bm_s, &v->bm);
            }
            v->bm_s.left += k->filter.shadow.x >> 6;
            v->bm_s.top  += k->filter.shadow.y >> 6;
            ass_shift_bitmap(&v->bm_s, k->filter.shadow.x & SUBPIXEL_MASK,
                             k->filter.shadow.y & SUBPIXEL_MASK);
        }
        if ((flags & FILTER_FILL_IN_SHADOW) && !(flags & FILTER_FILL_IN_BORDER))
            ass_fix_outline(&v->bm, &v->bm_o);
        FLE_PHASE(7);
        return sizeof(CompositeHashKey) + sizeof(CompositeHashValue) +
            (k->bitmaps ? k->bitmap_count * sizeof(BitmapRef) : 0) +
            bitmap_size(&v->bm) + bitmap_size(&v->bm_o) + bitmap_size(&v->bm_s);
    }

    ASS_Rect rect, rect_o;
    rectangle_reset(&rect);
    rectangle_reset(&rect_o);

    size_t n_bm = 0, n_bm_o = 0;
    const BitmapRef *bitmaps = k->bitmaps ? k->bitmaps : &k->single_bitmap;
    const BitmapRef *last = NULL, *last_o = NULL;
    for (int i = 0; i < k->bitmap_count; i++) {
        const BitmapRef *ref = &bitmaps[i];
        if (ref->bm) {
            rectangle_combine(&rect, ref->bm, ref->pos);
            last = ref;
            n_bm++;
        }
        if (ref->bm_o) {
            rectangle_combine(&rect_o, ref->bm_o, ref->pos_o);
            last_o = ref;
            n_bm_o++;
        }
    }

    int bord = ass_be_padding(k->filter.be);
    int flags = k->filter.flags;
    double r2x = restore_blur(k->filter.blur_x);
    double r2y = restore_blur(k->filter.blur_y);
    bool gaussian = r2x > 0.001 || r2y > 0.001;
    bool blur_fill = !(flags & FILTER_NONZERO_BORDER) || (flags & FILTER_BORDER_STYLE_3);
    bool fill_blurred = false, outline_blurred = false;
    if (!bord && n_bm == 1 && !k->viewport_align) {
        // Gaussian blur unpacks the source before allocating its output. Read
        // immutable cached bitmaps directly when no pre-blur cropping is needed.
        if (gaussian && blur_fill)
            fill_blurred = ass_gaussian_blur(render_priv, &v->bm, last->bm, r2x, r2y);
        if (!fill_blurred)
            ass_copy_bitmap(&render_priv->engine, &v->bm, last->bm);
        v->bm.left += last->pos.x;
        v->bm.top  += last->pos.y;
    } else if (n_bm && gaussian && blur_fill && !k->viewport_align) {
        Bitmap scratch = {0};
        if (alloc_composite_scratch(render_priv, &scratch, rect, bord)) {
            for (int i = 0; i < k->bitmap_count; i++)
                add_composite_bitmap(&render_priv->engine, &scratch,
                                     bitmaps[i].bm, bitmaps[i].pos);
            fill_blurred = ass_gaussian_blur(render_priv, &v->bm, &scratch, r2x, r2y);
            if (!fill_blurred)
                ass_copy_bitmap(&render_priv->engine, &v->bm, &scratch);
        } else if (alloc_composite_bitmap(&render_priv->engine, &v->bm, rect, bord, k)) {
            for (int i = 0; i < k->bitmap_count; i++)
                add_composite_bitmap(&render_priv->engine, &v->bm,
                                     bitmaps[i].bm, bitmaps[i].pos);
        }
    } else if (n_bm && alloc_composite_bitmap(&render_priv->engine, &v->bm, rect, bord, k)) {
        for (int i = 0; i < k->bitmap_count; i++)
            add_composite_bitmap(&render_priv->engine, &v->bm,
                                  bitmaps[i].bm, bitmaps[i].pos);
    }
    if (!bord && n_bm_o == 1 && !k->viewport_align) {
        if (gaussian)
            outline_blurred = ass_gaussian_blur(render_priv, &v->bm_o, last_o->bm_o, r2x, r2y);
        if (!outline_blurred)
            ass_copy_bitmap(&render_priv->engine, &v->bm_o, last_o->bm_o);
        v->bm_o.left += last_o->pos_o.x;
        v->bm_o.top  += last_o->pos_o.y;
    } else if (n_bm_o && gaussian && !k->viewport_align) {
        Bitmap scratch = {0};
        if (alloc_composite_scratch(render_priv, &scratch, rect_o, bord)) {
            for (int i = 0; i < k->bitmap_count; i++)
                add_composite_bitmap(&render_priv->engine, &scratch,
                                     bitmaps[i].bm_o, bitmaps[i].pos_o);
            outline_blurred = ass_gaussian_blur(render_priv, &v->bm_o, &scratch, r2x, r2y);
            if (!outline_blurred)
                ass_copy_bitmap(&render_priv->engine, &v->bm_o, &scratch);
        } else if (alloc_composite_bitmap(&render_priv->engine, &v->bm_o, rect_o, bord, k)) {
            for (int i = 0; i < k->bitmap_count; i++)
                add_composite_bitmap(&render_priv->engine, &v->bm_o,
                                     bitmaps[i].bm_o, bitmaps[i].pos_o);
        }
    } else if (n_bm_o && alloc_composite_bitmap(&render_priv->engine, &v->bm_o, rect_o, bord, k)) {
        for (int i = 0; i < k->bitmap_count; i++)
            add_composite_bitmap(&render_priv->engine, &v->bm_o,
                                  bitmaps[i].bm_o, bitmaps[i].pos_o);
    }

    if (blur_fill)
        ass_synth_blur(render_priv, &v->bm, k->filter.be,
                       fill_blurred ? 0 : r2x, fill_blurred ? 0 : r2y);
    ass_synth_blur(render_priv, &v->bm_o, k->filter.be,
                   outline_blurred ? 0 : r2x, outline_blurred ? 0 : r2y);

    if (!(flags & FILTER_FILL_IN_BORDER) && !(flags & FILTER_FILL_IN_SHADOW))
        ass_fix_outline(&v->bm, &v->bm_o);

    if (flags & FILTER_NONZERO_SHADOW) {
        if (flags & FILTER_NONZERO_BORDER) {
            ass_copy_bitmap(&render_priv->engine, &v->bm_s, &v->bm_o);
            if ((flags & FILTER_FILL_IN_BORDER) && !(flags & FILTER_FILL_IN_SHADOW))
                ass_fix_outline(&v->bm, &v->bm_s);
        } else if (flags & FILTER_BORDER_STYLE_3) {
            v->bm_s = v->bm_o;
            memset(&v->bm_o, 0, sizeof(v->bm_o));
        } else {
            ass_copy_bitmap(&render_priv->engine, &v->bm_s, &v->bm);
        }

        // Works right even for negative offsets
        // '>>' rounds toward negative infinity, '&' returns correct remainder
        v->bm_s.left += k->filter.shadow.x >> 6;
        v->bm_s.top  += k->filter.shadow.y >> 6;
        ass_shift_bitmap(&v->bm_s, k->filter.shadow.x & SUBPIXEL_MASK, k->filter.shadow.y & SUBPIXEL_MASK);
    }

    if ((flags & FILTER_FILL_IN_SHADOW) && !(flags & FILTER_FILL_IN_BORDER))
        ass_fix_outline(&v->bm, &v->bm_o);

    FLE_PHASE(7);
#ifdef ASS_FLE_PROFILE
    render_priv->fle_phase_ns[9] += (uint64_t) v->bm.w * v->bm.h +
            (uint64_t) v->bm_o.w * v->bm_o.h;
#endif
    return sizeof(CompositeHashKey) + sizeof(CompositeHashValue) +
        (k->bitmaps ? k->bitmap_count * sizeof(BitmapRef) : 0) +
        bitmap_size(&v->bm) + bitmap_size(&v->bm_o) + bitmap_size(&v->bm_s);
}

static void add_background(RenderContext *state, EventImages *event_images)
{
    ASS_Renderer *render_priv = state->renderer;
    int size_x = state->shadow_x > 0 ?
        lround(state->shadow_x * state->border_scale_x) : 0;
    int size_y = state->shadow_y > 0 ?
        lround(state->shadow_y * state->border_scale_y) : 0;
    int left    = event_images->left - size_x;
    int top     = event_images->top  - size_y;
    int right   = event_images->left + event_images->width  + size_x;
    int bottom  = event_images->top  + event_images->height + size_y;
    left        = FFMINMAX(left,   0, render_priv->width);
    top         = FFMINMAX(top,    0, render_priv->height);
    right       = FFMINMAX(right,  0, render_priv->width);
    bottom      = FFMINMAX(bottom, 0, render_priv->height);
    int w = right - left;
    int h = bottom - top;
    if (w < 1 || h < 1)
        return;
    void *nbuffer = ass_aligned_alloc(1, w * h, false);
    if (!nbuffer)
        return;
    memset(nbuffer, 0xFF, w * h);
    uint32_t clr = state->c[3];
    ass_apply_fade(&clr, state->fade);
    ASS_Image *img = my_draw_bitmap(nbuffer, w, h, w, left, top,
                                    clr, NULL);
    if (img) {
        img->next = event_images->imgs;
        event_images->imgs = img;
    }
}

static bool event_is_transparent(RenderContext *state, bool *can_cache_out)
{
    // Unpositioned events still need layout for collision avoidance. Style 4
    // also paints a separate background using the final render-context color.
    if (state->detect_collisions || !(state->evt_type & EVENT_POSITIONED) ||
        state->border_style == 4)
        return false;
    bool has_karaoke = false;
    for (int i = 0; i < state->text_info.length; i++) {
        if (state->text_info.glyphs[i].effect_type != EF_NONE) {
            has_karaoke = true;
            break;
        }
    }
    unsigned alpha_skip = state->renderer->experimental_edges ? 0xFE : 0xFF;
    bool all_primary = true;
    bool all_alphas_transparent = true;
    for (int i = 0; i < state->text_info.length; i++) {
        GlyphInfo *info = state->text_info.glyphs + i;
        if (info->border_style == 4)
            return false;
        if (!info->drawing_text.str && info->face_index != 0)
            all_primary = false;
        uint32_t c0 = info->c[0];
        ass_apply_fade(&c0, info->fade);
        if (_a(c0) < alpha_skip)
            return false;
        if (has_karaoke) {
            uint32_t c1 = info->c[1];
            ass_apply_fade(&c1, info->fade);
            if (_a(c1) < alpha_skip)
                return false;
        }
        uint32_t c2 = info->c[2];
        ass_apply_fade(&c2, info->fade);
        if (_a(c2) < alpha_skip) {
            all_alphas_transparent = false;
            if (info->border_x || info->border_y || info->border_style == 3)
                return false;
        }
        uint32_t c3 = info->c[3];
        ass_apply_fade(&c3, info->fade);
        if (_a(c3) < alpha_skip) {
            all_alphas_transparent = false;
            if (info->shadow_x || info->shadow_y)
                return false;
        }
    }
    if (can_cache_out)
        *can_cache_out = all_primary && all_alphas_transparent && !has_karaoke;
    return true;
}

/**
 * \brief Main ass rendering function, glues everything together
 * \param event event to render
 * \param event_images struct containing resulting images, will also be initialized
 * Process event, appending resulting ASS_Image's to images_root.
 */
static bool
render_event(RenderContext *state, ASS_Event *event,
             EventImages *event_images)
{
    ASS_Renderer *render_priv = state->renderer;
    if (event->Style < 0 || event->Style >= render_priv->track->n_styles) {
        ass_msg(render_priv->library, MSGL_WARN, "No style found");
        return false;
    }
    if (!event->Text) {
        ass_msg(render_priv->library, MSGL_WARN, "Empty event");
        return false;
    }

    free_render_context(state);
    init_render_context(state, event);
    FLE_START;

    if (!parse_events(state, event))
        return false;

    TextInfo *text_info = &state->text_info;
    if (text_info->length == 0) {
        free_render_context(state);
        return false;
    }

    split_style_runs(state);
    FLE_PHASE(0);

    // Find shape runs and shape text
    ass_shaper_set_base_direction(state->shaper,
            ass_resolve_base_direction(state->font_encoding));
    ass_shaper_find_runs(state->shaper, render_priv, text_info->glyphs,
            text_info->length);
    // Font fallback has ordered, persistent side effects, even for invisible text.
    bool all_primary = false;
    if (event_is_transparent(state, &all_primary)) {
        state->rendered_transparent = all_primary;
        free_render_context(state);
        return false;
    }
    if (!ass_shaper_shape(state->shaper, text_info)) {
        ass_msg(render_priv->library, MSGL_ERR, "Failed to shape text");
        free_render_context(state);
        return false;
    }

    retrieve_glyphs(state);
    FLE_PHASE(1);

    preliminary_layout(state);

    int valign = state->alignment & 12;

    int MarginL =
        (event->MarginL) ? event->MarginL : state->style->MarginL;
    int MarginR =
        (event->MarginR) ? event->MarginR : state->style->MarginR;
    int MarginV =
        (event->MarginV) ? event->MarginV : state->style->MarginV;

    // calculate max length of a line
    double max_text_width =
        x2scr_right(state, render_priv->track->PlayResX - MarginR) -
        x2scr_left(state, MarginL);

    // wrap lines
    wrap_lines_smart(state, max_text_width);

    // depends on glyph x coordinates being monotonous within runs, so it should be done before reorder
    ass_process_karaoke_effects(state);

    reorder_text(state);

    align_lines(state, max_text_width);

    // determine text bounding box
    ASS_DRect bbox;
    compute_string_bbox(text_info, &bbox);

    apply_baseline_shear(state);

    // determine device coordinates for text
    double device_x = 0;
    double device_y = 0;

    // handle positioned events first: an event can be both positioned and
    // scrolling, and the scrolling effect overrides the position on one axis
    if (state->evt_type & EVENT_POSITIONED) {
        double base_x = 0;
        double base_y = 0;
        get_base_point(&bbox, state->alignment, &base_x, &base_y);
        device_x =
            x2scr_pos(render_priv, state->pos_x) - base_x;
        device_y =
            y2scr_pos(render_priv, state->pos_y) - base_y;
    }

    // x coordinate
    if (state->evt_type & EVENT_HSCROLL) {
        if (state->scroll_direction == SCROLL_RL)
            device_x =
                x2scr_pos(render_priv,
                      render_priv->track->PlayResX -
                      state->scroll_shift);
        else if (state->scroll_direction == SCROLL_LR)
            device_x =
                x2scr_pos(render_priv, state->scroll_shift) -
                (bbox.x_max - bbox.x_min);
    } else if (!(state->evt_type & EVENT_POSITIONED)) {
        device_x = x2scr_left(state, MarginL);
    }

    // y coordinate
    if (state->evt_type & EVENT_VSCROLL) {
        if (state->scroll_direction == SCROLL_TB)
            device_y =
                y2scr(state,
                      state->scroll_y0 +
                      state->scroll_shift) -
                bbox.y_max;
        else if (state->scroll_direction == SCROLL_BT)
            device_y =
                y2scr(state,
                      state->scroll_y1 -
                      state->scroll_shift) -
                bbox.y_min;
    } else if (!(state->evt_type & EVENT_POSITIONED)) {
        if (valign == VALIGN_TOP) {     // toptitle
            device_y =
                y2scr_top(state,
                          MarginV) + text_info->lines[0].asc;
        } else if (valign == VALIGN_CENTER) {   // midtitle
            double scr_y =
                y2scr(state, render_priv->track->PlayResY / 2.0);
            device_y = scr_y - (bbox.y_max + bbox.y_min) / 2.0;
        } else {                // subtitle
            double line_pos = state->explicit ?
                0 : render_priv->settings.line_position;
            double scr_top, scr_bottom, scr_y0;
            if (valign != VALIGN_SUB)
                ass_msg(render_priv->library, MSGL_V,
                       "Invalid valign, assuming 0 (subtitle)");
            scr_bottom =
                y2scr_sub(state,
                          render_priv->track->PlayResY - MarginV);
            scr_top = y2scr_top(state, 0); //xxx not always 0?
            device_y = scr_bottom + (scr_top - scr_bottom) * line_pos / 100.0;
            device_y -= text_info->height;
            device_y += text_info->lines[0].asc;
            // clip to top to avoid confusion if line_position is very high,
            // turning the subtitle into a toptitle
            // also, don't change behavior if line_position is not used
            scr_y0 = scr_top + text_info->lines[0].asc;
            if (device_y < scr_y0 && line_pos > 0) {
                device_y = scr_y0;
            }
        }
    }

    // fix clip coordinates
    if (state->explicit || !render_priv->settings.use_margins) {
        state->clip_x0 =
            lround(x2scr_pos_scaled(render_priv, state->clip_x0));
        state->clip_x1 =
            lround(x2scr_pos_scaled(render_priv, state->clip_x1));
        state->clip_y0 =
            lround(y2scr_pos(render_priv, state->clip_y0));
        state->clip_y1 =
            lround(y2scr_pos(render_priv, state->clip_y1));

        if (state->explicit) {
            // we still need to clip against screen boundaries
            int zx = render_priv->settings.left_margin;
            int zy = render_priv->settings.top_margin;
            int sx = zx + render_priv->frame_content_width;
            int sy = zy + render_priv->frame_content_height;

            state->clip_x0 = FFMAX(state->clip_x0, zx);
            state->clip_y0 = FFMAX(state->clip_y0, zy);
            state->clip_x1 = FFMIN(state->clip_x1, sx);
            state->clip_y1 = FFMIN(state->clip_y1, sy);
        }
    } else {
        // no \clip (explicit==0) and use_margins => only clip to screen with margins
        state->clip_x0 = 0;
        state->clip_y0 = 0;
        state->clip_x1 = render_priv->settings.frame_width;
        state->clip_y1 = render_priv->settings.frame_height;
    }

    if (state->evt_type & EVENT_VSCROLL) {
        int y0 = lround(y2scr_pos(render_priv, state->scroll_y0));
        int y1 = lround(y2scr_pos(render_priv, state->scroll_y1));

        state->clip_y0 = FFMAX(state->clip_y0, y0);
        state->clip_y1 = FFMIN(state->clip_y1, y1);
    }

    calculate_rotation_params(state, &bbox, device_x, device_y);
    FLE_PHASE(2);

    render_and_combine_glyphs(state, device_x, device_y);
    FLE_PHASE(3);

    memset(event_images, 0, sizeof(*event_images));
    // VSFilter does *not* shift lines with a border > margin to be within the
    // frame, so negative values for top and left may occur
    event_images->top = device_y - text_info->lines[0].asc - text_info->border_top;
    event_images->height =
        text_info->height + text_info->border_bottom + text_info->border_top;
    event_images->left =
        (device_x + bbox.x_min) * render_priv->par_scale_x - text_info->border_x + 0.5;
    event_images->width =
        (bbox.x_max - bbox.x_min) * render_priv->par_scale_x
        + 2 * text_info->border_x + 0.5;
    event_images->detect_collisions = state->detect_collisions;
    event_images->shift_direction = (valign == VALIGN_SUB) ? -1 : 1;
    event_images->event = event;
    event_images->imgs = render_text(state);

    if (state->border_style == 4)
        add_background(state, event_images);
    FLE_PHASE(4);

    ass_shaper_cleanup(state->shaper, text_info);
    free_render_context(state);
    FLE_PHASE(5);

    return true;
}

static ASS_Image *ass_free_image(ASS_Image *img);

struct event_construct {
    RenderContext *state;
    ASS_Event *event;
    EventImages images;
    bool rendered, ok;
};

static void update_timed_plateau_hint(ASS_Renderer *render_priv, ASS_Event *event,
                                      const RenderContext *state)
{
    ASS_RenderPriv *known = event->render_priv;
    if (!known) {
        known = event->render_priv = calloc(1, sizeof(ASS_RenderPriv));
        if (!known)
            return;
        known->render_id = render_priv->render_id;
    }
    known->timed_text = event->Text;
    known->timed_look = render_priv->look_generation;
    known->transparent_from = 0;
    known->transparent_until = 0;
    long long rel = render_priv->time - event->Start;
    if (state->rendered_transparent && state->alpha_plateau_valid &&
        !state->plateau_unbounded && rel >= state->alpha_plateau_from &&
        rel < state->alpha_plateau_until &&
        state->alpha_plateau_until - rel >= EVENT_CACHE_MIN_MS) {
        known->transparent_from = state->alpha_plateau_from;
        known->transparent_until = state->alpha_plateau_until;
    }
    if (state->plateau_unbounded) {
        known->plateau_from = INT64_MAX;
        known->plateau_until = INT64_MAX;
    } else if (!state->plateau_valid) {
        known->plateau_from = FFMAX(rel + 1, state->plateau_active_end);
        known->plateau_until = -1;
    } else if (rel >= state->plateau_from && rel < state->plateau_until &&
               state->plateau_until - rel >= EVENT_CACHE_MIN_MS) {
        known->plateau_from = state->plateau_from;
        known->plateau_until = state->plateau_until;
    } else {
        known->plateau_from = FFMAX(rel + 1, state->plateau_until);
        known->plateau_until = -1;
    }
}

size_t ass_event_construct(void *key, void *value, void *priv)
{
    EventHashKey *k = key;
    EventHashValue *v = value;
    struct event_construct *c = priv;

    c->state->timed = false;
    c->ok = render_event(c->state, c->event, &c->images);
    c->rendered = true;
    *v = (EventHashValue) { .timed = c->state->timed, .rendered = c->ok };
    size_t size = sizeof(*v) + k->effect.len + k->text.len;
    if (!c->ok)
        return size;

    if (v->timed) {
        long long rel = c->state->renderer->time - c->event->Start;
        if (!c->state->plateau_valid || c->state->plateau_unbounded ||
            rel < c->state->plateau_from || rel >= c->state->plateau_until ||
            c->state->plateau_until - rel < EVENT_CACHE_MIN_MS)
            return size;
        v->plateau = true;
        v->duration = c->event->Duration;
        v->valid_from = c->state->plateau_from;
        v->valid_until = c->state->plateau_until;
    }

    v->imgs = c->images.imgs;
    ass_frame_ref(v->imgs);
    v->top = c->images.top;
    v->height = c->images.height;
    v->left = c->images.left;
    v->width = c->images.width;
    v->detect_collisions = c->images.detect_collisions;
    v->shift_direction = c->images.shift_direction;
    return size;
}

// Fast single-pass scan computing text length and hinting whether an event
// uses time-dependent tags/effects so timed events bypass event_cache on
// their very first frame without hashing or copying keys.
static bool event_has_timed_hint(const ASS_Event *event, size_t *text_len_out)
{
    if (event->Effect && event->Effect[0]) {
        if (!strncmp(event->Effect, "Banner;", 7) ||
            !strncmp(event->Effect, "Scroll up;", 10) ||
            !strncmp(event->Effect, "Scroll down;", 12))
            return true;
    }
    const char *p = event->Text;
    const char *start = p;
    bool in_tags = false;
    while (*p) {
        char c = *p++;
        if (c == '{') {
            in_tags = true;
        } else if (c == '}') {
            in_tags = false;
        } else if (in_tags && c == '\\') {
            while (*p == ' ' || *p == '\t')
                p++;
            switch (*p) {
            case 't':
                if (p[1] == '(')
                    return true;
                break;
            case 'm':
                if (p[1] == 'o' && p[2] == 'v' && p[3] == 'e' && p[4] == '(')
                    return true;
                break;
            case 'f':
                if (p[1] == 'a' && p[2] == 'd' &&
                    (p[3] == '(' || (p[3] == 'e' && p[4] == '(')))
                    return true;
                break;
            case 'k':
            case 'K':
                return true;
            default:
                break;
            }
        }
    }
    *text_len_out = (size_t) (p - start);
    return false;
}

/**
 * \brief Render an event, or copy its images from the event cache when it
 * does not change with time; collisions move the copies.
 */
static bool
ass_render_event(RenderContext *state, ASS_Event *event,
                 EventImages *event_images)
{
    ASS_Renderer *render_priv = state->renderer;
    ASS_RenderPriv *known = event->render_priv;
    if (!event->Text || event->Duration < EVENT_CACHE_MIN_MS)
        return render_event(state, event, event_images);

    long long rel = render_priv->time - event->Start;
    size_t text_len = 0;
    long long key_from = 0, key_until = 0;
    if (known && known->timed_text == event->Text &&
        known->timed_look == render_priv->look_generation) {
        if (known->transparent_until > known->transparent_from &&
            rel >= known->transparent_from && rel < known->transparent_until)
            return false;
        if (rel < known->plateau_from) {
            bool ok = render_event(state, event, event_images);
            if (!ok && state->rendered_transparent && state->timed)
                update_timed_plateau_hint(render_priv, event, state);
            return ok;
        }
        if (known->plateau_until < 0 || rel >= known->plateau_until) {
            bool ok = render_event(state, event, event_images);
            if (state->timed)
                update_timed_plateau_hint(render_priv, event, state);
            return ok;
        }
        // Inside a known static plateau: fall through to event_cache lookup.
        text_len = strlen(event->Text);
        key_from = known->plateau_from;
        key_until = known->plateau_until;
    } else if (event_has_timed_hint(event, &text_len)) {
        bool ok = render_event(state, event, event_images);
        if (state->timed)
            update_timed_plateau_hint(render_priv, event, state);
        return ok;
    }

    const char *effect = event->Effect ? event->Effect : "";
    EventHashKey key = {
        .look = render_priv->look_generation,
        .style = event->Style,
        .margin_l = event->MarginL,
        .margin_r = event->MarginR,
        .margin_v = event->MarginV,
        .plateau_from = key_from,
        .plateau_until = key_until,
        .effect = { effect, strlen(effect) },
        .text = { event->Text, text_len },
    };
    struct event_construct c = { .state = state, .event = event };
    EventHashValue *v = ass_cache_get(render_priv->cache.event_cache, &key, &c);
    if (!v)
        return render_event(state, event, event_images);
    if (v->timed) {
        if (c.rendered) {
            update_timed_plateau_hint(render_priv, event, state);
            if (!v->plateau) {
                if (!c.ok)
                    return false;
                *event_images = c.images;
                return true;
            }
        } else if (!v->plateau || v->duration != event->Duration ||
                   rel < v->valid_from || rel >= v->valid_until) {
            bool ok = render_event(state, event, event_images);
            if (state->timed)
                update_timed_plateau_hint(render_priv, event, state);
            return ok;
        }
    }
    if (!v->rendered)
        return false;

    ASS_Image *head = NULL, **tail = &head;
    for (ASS_Image *img = v->imgs; img; img = img->next) {
        // Its composite ages as it would had the event been rendered.
        void *composite = ((ASS_ImagePriv *) img)->source;
        if (composite)
            ass_cache_use(composite);
        ASS_Image *clone = clone_image(img, v);
        if (!clone) {
            while (head)
                head = ass_free_image(head);
            return false;
        }
        *tail = clone;
        tail = &clone->next;
    }

    memset(event_images, 0, sizeof(*event_images));
    event_images->imgs = head;
    event_images->top = v->top;
    event_images->height = v->height;
    event_images->left = v->left;
    event_images->width = v->width;
    event_images->detect_collisions = v->detect_collisions;
    event_images->shift_direction = v->shift_direction;
    event_images->event = event;
    return true;
}

static void effective_cache_budgets(ASS_Renderer *priv, CacheStore *cache,
                                    size_t extra_returned, bool update_growth,
                                    size_t *bitmap_out, size_t *composite_out)
{
    size_t bitmap_max = cache->bitmap_max_size;
    size_t composite_max = cache->composite_max_size;
#ifdef ASS_EVENT_THREADS
    if (!bitmap_max && !priv->is_event_worker) {
#else
    if (!bitmap_max) {
#endif
        // Bitmaps grow with the frame's area, so a smaller frame fits as many in less memory.
        double area = (double) priv->settings.frame_width * priv->settings.frame_height;
        double scale = FFMINMAX(area / DEFAULT_CACHE_FRAME_AREA,
                                (double) DEFAULT_CACHE_MIN_SIZE / (BITMAP_CACHE_MAX_SIZE + COMPOSITE_CACHE_MAX_SIZE), 1);
        if (update_growth) {
            size_t returned = ass_cache_take_returned(cache->bitmap_cache) +
                              ass_cache_take_returned(cache->composite_cache) +
                              extra_returned;
            // Evicted bitmaps that keep being made again outgrew the budget:
            // double it, up to the default for the largest frames.
            cache->returned -= cache->returned / CACHE_RETURN_DECAY;
            cache->returned = returned > SIZE_MAX - cache->returned ?
                              SIZE_MAX : cache->returned + returned;
        }
        double grown = FFMIN(scale * cache->growth, 1);
        if (update_growth && grown < 1 && cache->returned >
                (BITMAP_CACHE_MAX_SIZE + COMPOSITE_CACHE_MAX_SIZE) * grown / CACHE_RETURN_LIMIT) {
            cache->growth *= 2;
            cache->returned = 0;
            grown = FFMIN(scale * cache->growth, 1);
        }
        bitmap_max = BITMAP_CACHE_MAX_SIZE * grown;
        composite_max = COMPOSITE_CACHE_MAX_SIZE * grown;
    }
    *bitmap_out = bitmap_max;
    *composite_out = composite_max;
}

/**
 * \brief Check cache limits and reset cache if they are exceeded
 */
static void check_cache_limits(ASS_Renderer *priv, CacheStore *cache)
{
    size_t bitmap_max, composite_max;
    effective_cache_budgets(priv, cache, 0, true, &bitmap_max, &composite_max);
    // An event that stopped showing would keep its composites from being evicted.
    ass_cache_cut_unused(cache->event_cache, EVENT_CACHE_FRAMES);
    ass_cache_cut(cache->composite_cache, composite_max);
    ass_cache_cut(cache->bitmap_cache, bitmap_max);
    ass_cache_cut(cache->outline_cache, cache->glyph_max);
    ass_cache_cut(cache->shape_cache, cache->glyph_max ? SHAPE_CACHE_MAX_SIZE : 0);
}

static void setup_shaper(ASS_Shaper *shaper, ASS_Renderer *render_priv)
{
    ASS_Track *track = render_priv->track;

    ass_shaper_set_kerning(shaper, track->Kerning);
    ass_shaper_set_language(shaper, track->Language);
    ass_shaper_set_level(shaper, render_priv->settings.shaper);
#ifdef USE_FRIBIDI_EX_API
    ass_shaper_set_bidi_brackets(shaper,
            track->parser_priv->feature_flags & FEATURE_MASK(ASS_FEATURE_BIDI_BRACKETS));
#endif
    ass_shaper_set_whole_text_layout(shaper,
            track->parser_priv->feature_flags & FEATURE_MASK(ASS_FEATURE_WHOLE_TEXT_LAYOUT));
}

static void look_append(ASS_Renderer *priv, const void *data, size_t size)
{
    if (!size || !priv->next_look_ok)
        return;
    if (priv->next_look_size + size > priv->next_look_cap) {
        size_t cap = FFMAX(2 * priv->next_look_cap, priv->next_look_size + size);
        char *grown = realloc(priv->next_look, cap);
        if (!grown) {
            priv->next_look_ok = false;
            return;
        }
        priv->next_look = grown;
        priv->next_look_cap = cap;
    }
    memcpy(priv->next_look + priv->next_look_size, data, size);
    priv->next_look_size += size;
}

static void look_append_string(ASS_Renderer *priv, const char *str)
{
    size_t len = str ? strlen(str) : 0;
    look_append(priv, &len, sizeof(len));
    look_append(priv, str, len);
}

static inline void look_append_int(ASS_Renderer *priv, int val)
{
    look_append(priv, &val, sizeof(val));
}

static inline void look_append_u32(ASS_Renderer *priv, uint32_t val)
{
    look_append(priv, &val, sizeof(val));
}

static inline void look_append_size(ASS_Renderer *priv, size_t val)
{
    look_append(priv, &val, sizeof(val));
}

static inline void look_append_double(ASS_Renderer *priv, double val)
{
    look_append(priv, &val, sizeof(val));
}

static void look_append_style(ASS_Renderer *priv, const ASS_Style *style, bool include_name)
{
    look_append_double(priv, style->FontSize);
    look_append_u32(priv, style->PrimaryColour);
    look_append_u32(priv, style->SecondaryColour);
    look_append_u32(priv, style->OutlineColour);
    look_append_u32(priv, style->BackColour);
    look_append_int(priv, style->Bold);
    look_append_int(priv, style->Italic);
    look_append_int(priv, style->Underline);
    look_append_int(priv, style->StrikeOut);
    look_append_double(priv, style->ScaleX);
    look_append_double(priv, style->ScaleY);
    look_append_double(priv, style->Spacing);
    look_append_double(priv, style->Angle);
    look_append_int(priv, style->BorderStyle);
    look_append_double(priv, style->Outline);
    look_append_double(priv, style->Shadow);
    look_append_int(priv, style->Alignment);
    look_append_int(priv, style->MarginL);
    look_append_int(priv, style->MarginR);
    look_append_int(priv, style->MarginV);
    look_append_int(priv, style->Encoding);
    look_append_int(priv, style->treat_fontname_as_pattern);
    look_append_double(priv, style->Blur);
    look_append_int(priv, style->Justify);
    if (include_name)
        look_append_string(priv, style->Name);
    look_append_string(priv, style->FontName);
}

/**
 * \brief Change look_generation when the settings or track state that events'
 * looks depend on change.
 */
static void update_look(ASS_Renderer *priv)
{
    ASS_Track *track = priv->track;
    ASS_Settings *s = &priv->settings;

    priv->next_look_size = 0;
    priv->next_look_ok = true;
    look_append_int(priv, s->frame_width);
    look_append_int(priv, s->frame_height);
    look_append_int(priv, s->storage_width);
    look_append_int(priv, s->storage_height);
    look_append_double(priv, s->font_size_coeff);
    look_append_double(priv, s->line_spacing);
    look_append_double(priv, s->line_position);
    look_append_int(priv, s->top_margin);
    look_append_int(priv, s->bottom_margin);
    look_append_int(priv, s->left_margin);
    look_append_int(priv, s->right_margin);
    look_append_int(priv, s->use_margins);
    look_append_double(priv, s->par);
    look_append_int(priv, s->hinting);
    look_append_int(priv, s->shaper);
    look_append_int(priv, s->selective_style_overrides);
    look_append_string(priv, s->default_font);
    look_append_string(priv, s->default_family);
    look_append_style(priv, &priv->user_override_style, false);
    look_append_int(priv, priv->render_id);
    look_append_int(priv, priv->experimental_edges);
    look_append_size(priv, priv->num_emfonts);
    look_append_u32(priv, ass_fontselect_generation(priv->fontselect));
    look_append_double(priv, priv->par_scale_x);
    look_append_int(priv, track->track_type);
    look_append_int(priv, track->PlayResX);
    look_append_int(priv, track->PlayResY);
    look_append_double(priv, track->Timer);
    look_append_int(priv, track->WrapStyle);
    look_append_int(priv, track->ScaledBorderAndShadow);
    look_append_int(priv, track->Kerning);
    look_append_string(priv, track->Language);
    look_append_int(priv, track->YCbCrMatrix);
    look_append_int(priv, track->LayoutResX);
    look_append_int(priv, track->LayoutResY);
    look_append_u32(priv, track->parser_priv->feature_flags);
    for (int i = 0; i < track->n_styles; i++)
        look_append_style(priv, track->styles + i, true);

    if (!priv->next_look_ok) {
        // An incomplete look can't be compared, so nothing rendered with it is reused.
        free(priv->look);
        priv->look = NULL;
        priv->look_size = 0;
        priv->look_cap = 0;
        priv->look_generation++;
    } else if (!priv->look || priv->next_look_size != priv->look_size ||
               memcmp(priv->next_look, priv->look, priv->look_size)) {
        char *old = priv->look;
        size_t old_cap = priv->look_cap;
        priv->look = priv->next_look;
        priv->look_size = priv->next_look_size;
        priv->look_cap = priv->next_look_cap;
        priv->next_look = old;
        priv->next_look_cap = old_cap;
        priv->look_generation++;
    }
}

/**
 * \brief Start a new frame
 */
static bool
ass_start_frame(ASS_Renderer *render_priv, ASS_Track *track,
                long long now)
{
    if (!render_priv->settings.frame_width
        && !render_priv->settings.frame_height)
        return false;               // library not initialized

    if (!render_priv->fontselect)
        return false;

    if (render_priv->library != track->library)
        return false;

    if (track->n_events == 0)
        return false;               // nothing to do

    render_priv->track = track;
    render_priv->time = now;
    render_priv->state.last_font = NULL;

    ass_lazy_track_init(render_priv->library, render_priv->track);

    if (render_priv->library->num_fontdata != render_priv->num_emfonts) {
        assert(render_priv->library->num_fontdata > render_priv->num_emfonts);
        render_priv->num_emfonts = ass_update_embedded_fonts(
            render_priv->fontselect, render_priv->num_emfonts);
    }

    setup_shaper(render_priv->state.shaper, render_priv);

    // PAR correction
    double par = render_priv->settings.par;
    bool lr_track = track->LayoutResX > 0 && track->LayoutResY > 0;
    if (par == 0. || lr_track) {
        if (render_priv->frame_content_width && render_priv->frame_content_height && (lr_track ||
                (render_priv->settings.storage_width && render_priv->settings.storage_height))) {
            double dar = ((double) render_priv->frame_content_width) /
                         render_priv->frame_content_height;
            ASS_Vector layout_res = ass_layout_res(render_priv);
            double sar = ((double) layout_res.x) / layout_res.y;
            par = dar / sar;
        } else
            par = 1.0;
    }
    render_priv->par_scale_x = par;
#ifdef ASS_EVENT_THREADS
    if (!render_priv->is_event_worker)
#endif
        update_look(render_priv);

    render_priv->prev_images_root = render_priv->images_root;
    render_priv->images_root = NULL;

    check_cache_limits(render_priv, &render_priv->cache);

    return true;
}

#ifdef ASS_EVENT_THREADS
typedef struct {
    int event_index;
    unsigned lane;
    uint32_t cost_us;
} EventTask;

typedef struct {
    const ASS_Event *event;
    uint32_t cost_us;
} EventCostHint;

typedef struct {
    uint64_t range; // low 32 bits = head, high 32 bits = tail
    char pad[56];
} LaneDeque;

typedef struct {
    FilterFunc *plain;
    ParamFilterFunc *param_filter;
    int16_t *dst;
    const int16_t *src, *coeff;
    size_t width, src_h, dst_h, stripe;
    unsigned lanes;
} VerticalTask;

typedef struct {
    struct ass_event_pool *pool;
    ASS_Renderer *renderer;
    pthread_t thread;
    pthread_cond_t work;
    ASS_MsgBuffer messages;
    unsigned font_generation;
    int reconfigure_id, id;
    uint64_t batch_ns;
    bool prepared;
} EventWorker;

typedef struct {
    BlurSliceFunc *fn;
    void *ctx;
    unsigned lanes;
} BlurSliceTask;

struct ass_event_pool {
    ASS_Renderer *owner;
    EventWorker workers[3];
    pthread_mutex_t lock;
    pthread_cond_t done;
    uint64_t generation;
    int count, pending;
    bool stop, event_ready, legacy_events, cache_split, adaptive_default;
    unsigned job_mask;
    enum { JOB_IDLE, JOB_EVENTS, JOB_VERTICAL, JOB_SLICE, JOB_PREPARE } job_kind;
    VerticalTask vertical;
    BlurSliceTask slice;
    EventTask *events;
    int *lane_slots;
    LaneDeque lanes[4];
    size_t capacity;
    size_t glyph_budget, bitmap_budget, composite_budget;
    size_t worker_glyph_limit, worker_bitmap_limit, worker_composite_limit;
    int serial_frames;
    unsigned lane_shift;
    bool measure_events;
    const ASS_Track *cost_track;
    EventCostHint cost_hints[256];
};

static void set_pool_cache_limits(struct ass_event_pool *pool, size_t glyphs,
                                  size_t bitmaps, size_t composites)
{
    pool->glyph_budget = glyphs;
    pool->bitmap_budget = bitmaps;
    pool->composite_budget = composites;
    pool->cache_split = true;
    pool->serial_frames = 0;
    size_t lanes = (size_t) pool->count + 1;
    pool->worker_glyph_limit = glyphs / lanes;
    pool->worker_bitmap_limit = bitmaps / lanes;
    pool->worker_composite_limit = composites / lanes;
    for (int i = -1; i < pool->count; i++) {
        ASS_Renderer *renderer = i < 0 ? pool->owner : pool->workers[i].renderer;
        renderer->cache.glyph_max = pool->worker_glyph_limit;
        renderer->cache.bitmap_max_size = pool->worker_bitmap_limit;
        renderer->cache.composite_max_size = pool->worker_composite_limit;
    }
}

static void refresh_pool_adaptive_budgets(struct ass_event_pool *pool, bool update_growth)
{
    if (!pool->adaptive_default)
        return;
    ASS_Renderer *owner = pool->owner;
    size_t extra_returned = 0;
    if (update_growth) {
        for (int i = 0; i < pool->count; i++) {
            ASS_Renderer *w = pool->workers[i].renderer;
            if (w)
                extra_returned += ass_cache_take_returned(w->cache.bitmap_cache) +
                                  ass_cache_take_returned(w->cache.composite_cache);
        }
    }
    size_t saved_bm = owner->cache.bitmap_max_size;
    size_t saved_comp = owner->cache.composite_max_size;
    owner->cache.bitmap_max_size = 0;
    owner->cache.composite_max_size = 0;
    effective_cache_budgets(owner, &owner->cache, extra_returned, update_growth,
                            &pool->bitmap_budget, &pool->composite_budget);
    owner->cache.bitmap_max_size = saved_bm;
    owner->cache.composite_max_size = saved_comp;
}

void ass_event_pool_cache_limits(ASS_Renderer *renderer)
{
    struct ass_event_pool *pool = renderer->event_pool;
    if (!pool || !pool->event_ready)
        return;
    pool->adaptive_default = (renderer->cache.bitmap_max_size == 0);
    if (pool->adaptive_default) {
        refresh_pool_adaptive_budgets(pool, false);
    } else {
        pool->bitmap_budget = renderer->cache.bitmap_max_size;
        pool->composite_budget = renderer->cache.composite_max_size;
    }
    pool->glyph_budget = renderer->cache.glyph_max;
    if (pool->cache_split) {
        set_pool_cache_limits(pool, pool->glyph_budget,
                pool->bitmap_budget, pool->composite_budget);
        for (int i = 0; i < pool->count; i++)
            check_cache_limits(pool->workers[i].renderer,
                               &pool->workers[i].renderer->cache);
    }
}

static size_t shrink_worker_limit(size_t current, size_t initial, int steps_left)
{
    if (!current || steps_left <= 1)
        return 0;
    size_t step = (initial + 7) / 8;
    return current > step ? current - step : 0;
}

static void restore_serial_cache_limits(struct ass_event_pool *pool)
{
    if (!pool || !pool->cache_split)
        return;
    // Keep worker caches warm across brief gaps and spread eviction across
    // multiple serial frames to avoid a single-frame teardown stall while
    // keeping total budget strictly within pool->*_budget.
    if (++pool->serial_frames <= 8)
        return;
    int steps_left = 17 - pool->serial_frames;
    size_t lanes = (size_t) pool->count + 1;
    pool->worker_glyph_limit = shrink_worker_limit(
        pool->worker_glyph_limit, pool->glyph_budget / lanes, steps_left);
    pool->worker_bitmap_limit = shrink_worker_limit(
        pool->worker_bitmap_limit, pool->bitmap_budget / lanes, steps_left);
    pool->worker_composite_limit = shrink_worker_limit(
        pool->worker_composite_limit, pool->composite_budget / lanes, steps_left);

    for (int i = 0; i < pool->count; i++) {
        ASS_Renderer *worker = pool->workers[i].renderer;
        worker->cache.glyph_max = pool->worker_glyph_limit;
        worker->cache.bitmap_max_size = pool->worker_bitmap_limit;
        worker->cache.composite_max_size = pool->worker_composite_limit;
        check_cache_limits(worker, &worker->cache);
    }
    pool->owner->cache.glyph_max =
        pool->glyph_budget - pool->worker_glyph_limit * (size_t) pool->count;
    pool->owner->cache.bitmap_max_size =
        pool->bitmap_budget - pool->worker_bitmap_limit * (size_t) pool->count;
    pool->owner->cache.composite_max_size =
        pool->composite_budget - pool->worker_composite_limit * (size_t) pool->count;
    if (!pool->worker_glyph_limit && !pool->worker_bitmap_limit &&
        !pool->worker_composite_limit) {
        pool->cache_split = false;
        if (pool->adaptive_default) {
            pool->owner->cache.bitmap_max_size = 0;
            pool->owner->cache.composite_max_size = 0;
        }
    }
}

static inline unsigned event_worker_lane(const struct ass_event_pool *pool,
                                         const ASS_Event *event)
{
    return ((unsigned) event->ReadOrder >> pool->lane_shift) % (pool->count + 1);
}

static inline int lane_pop_head(LaneDeque *d)
{
    uint64_t cur = __atomic_load_n(&d->range, __ATOMIC_RELAXED);
    for (;;) {
        uint32_t head = (uint32_t) cur;
        uint32_t tail = (uint32_t) (cur >> 32);
        if (head >= tail)
            return -1;
        uint64_t next = ((uint64_t) tail << 32) | (head + 1);
        if (__atomic_compare_exchange_n(&d->range, &cur, next, true,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED))
            return (int) head;
    }
}

static inline int lane_steal_tail(LaneDeque *d)
{
    uint64_t cur = __atomic_load_n(&d->range, __ATOMIC_RELAXED);
    for (;;) {
        uint32_t head = (uint32_t) cur;
        uint32_t tail = (uint32_t) (cur >> 32);
        if (head >= tail)
            return -1;
        uint64_t next = ((uint64_t) (tail - 1) << 32) | head;
        if (__atomic_compare_exchange_n(&d->range, &cur, next, true,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED))
            return (int) (tail - 1);
    }
}

static int steal_task(struct ass_event_pool *pool, int self_id)
{
    int n_lanes = pool->count + 1;
    for (;;) {
        int best = -1;
        uint32_t best_rem = 0;
        for (int i = 0; i < n_lanes; i++) {
            if (i == self_id)
                continue;
            uint64_t r = __atomic_load_n(&pool->lanes[i].range, __ATOMIC_RELAXED);
            uint32_t head = (uint32_t) r;
            uint32_t tail = (uint32_t) (r >> 32);
            if (tail > head && tail - head > best_rem) {
                best_rem = tail - head;
                best = i;
            }
        }
        if (best < 0)
            return -1;
        int pos = lane_steal_tail(&pool->lanes[best]);
        if (pos >= 0)
            return pos;
    }
}

static uint64_t event_batch(struct ass_event_pool *pool, ASS_Renderer *renderer, int id)
{
    uint64_t start = fle_clock();
    bool measure = pool->measure_events;
    for (;;) {
        int pos = lane_pop_head(&pool->lanes[id]);
        if (pos < 0) {
            pos = steal_task(pool, id);
            if (pos < 0)
                break;
        }
        int slot = pool->lane_slots[pos];
        ASS_Event *event = pool->owner->track->events + pool->events[slot].event_index;
        EventImages *out = pool->owner->eimg + slot;
        uint64_t ev_start = measure ? fle_clock() : 0;
        if (!ass_render_event(&renderer->state, event, out))
            out->event = NULL;
        if (measure) {
            uint64_t elapsed = event_elapsed_ns(ev_start, fle_clock());
            pool->events[slot].cost_us = elapsed == UINT64_MAX ? 0 :
                (uint32_t) FFMIN(elapsed >> 10, UINT32_MAX);
        }
    }
    return event_elapsed_ns(start, fle_clock());
}

static void vertical_batch(const VerticalTask *task, unsigned id)
{
    if (id >= task->lanes)
        return;
    size_t stripes = (task->width + task->stripe - 1) / task->stripe;
    size_t begin = stripes / task->lanes * id + stripes % task->lanes * id / task->lanes;
    size_t end = stripes / task->lanes * (id + 1) +
                 stripes % task->lanes * (id + 1) / task->lanes;
    if (begin == end)
        return;
    size_t x = begin * task->stripe;
    // Keep full-height stripes intact: vertical kernels need no cross-stripe halo.
    size_t width = end == stripes ? task->width - x : (end - begin) * task->stripe;
    const int16_t *src = task->src + x * task->src_h;
    int16_t *dst = task->dst + x * task->dst_h;
    if (task->param_filter) {
        // The full stage already passed the pool's large-work threshold.
        if (task->stripe != 8 || !ass_trim_blur_vertical(task->param_filter,
                dst, src, width, task->src_h, task->dst_h, task->coeff))
            task->param_filter(dst, src, width, task->src_h, task->coeff);
    } else
        task->plain(dst, src, width, task->src_h);
}

static void *event_worker(void *arg)
{
    EventWorker *worker = arg;
    struct ass_event_pool *pool = worker->pool;
    uint64_t seen = 0;
    pthread_mutex_lock(&pool->lock);
    while (!pool->stop) {
        while (!pool->stop && (seen == pool->generation ||
                !(pool->job_mask & (1U << worker->id))))
            pthread_cond_wait(&worker->work, &pool->lock);
        if (pool->stop) break;
        seen = pool->generation;
        pthread_mutex_unlock(&pool->lock);
        ass_msg_buffer_begin(&worker->messages, pool->owner->library);
        if (pool->job_kind == JOB_VERTICAL)
            vertical_batch(&pool->vertical, worker->id);
        else if (pool->job_kind == JOB_SLICE)
            pool->slice.fn(pool->slice.ctx, worker->id, pool->slice.lanes);
        else if (pool->job_kind == JOB_PREPARE)
            worker->prepared = ass_clone_render_settings(worker->renderer,
                    pool->owner, &worker->font_generation, &worker->reconfigure_id);
        else
            worker->batch_ns = event_batch(pool, worker->renderer, worker->id);
        ass_msg_buffer_end(&worker->messages);
        pthread_mutex_lock(&pool->lock);
        if (!--pool->pending)
            pthread_cond_signal(&pool->done);
    }
    pthread_mutex_unlock(&pool->lock);
    return NULL;
}

// Called with the pool locked. Idle lanes keep their caches without being woken.
static void start_worker_job(struct ass_event_pool *pool, unsigned mask)
{
    pool->job_mask = mask;
    pool->pending = 0;
    pool->generation++;
    for (int i = 0; i < pool->count; i++) {
        if (mask & (1U << pool->workers[i].id)) {
            pool->pending++;
            pthread_cond_signal(&pool->workers[i].work);
        }
    }
}

static void drain_worker_messages(struct ass_event_pool *pool)
{
    for (int i = 0; i < pool->count; i++)
        ass_msg_buffer_drain(&pool->workers[i].messages);
}

static void event_pool_done(struct ass_event_pool *pool)
{
    if (!pool) return;
    pthread_mutex_lock(&pool->lock);
    pool->stop = true;
    for (int i = 0; i < pool->count; i++)
        pthread_cond_signal(&pool->workers[i].work);
    pthread_mutex_unlock(&pool->lock);
    for (int i = 0; i < pool->count; i++) {
        pthread_join(pool->workers[i].thread, NULL);
        ass_msg_buffer_drain(&pool->workers[i].messages);
        pthread_cond_destroy(&pool->workers[i].work);
        ass_renderer_done(pool->workers[i].renderer);
    }
    pthread_cond_destroy(&pool->done);
    pthread_mutex_destroy(&pool->lock);
    free(pool->lane_slots);
    free(pool->events);
    free(pool);
}

static struct ass_event_pool *event_pool_init(ASS_Renderer *owner)
{
    const char *value = getenv("LIBASS_RENDER_THREADS");
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    int threads = value ? atoi(value) : 4;
    threads = FFMIN(FFMIN(threads, 4), cpus);
    if (threads <= 1) return NULL;
    struct ass_event_pool *pool = calloc(1, sizeof(*pool));
    if (!pool) return NULL;
    pool->owner = owner;
    if (pthread_mutex_init(&pool->lock, NULL)) { free(pool); return NULL; }
    if (pthread_cond_init(&pool->done, NULL)) {
        pthread_mutex_destroy(&pool->lock); free(pool); return NULL;
    }
    for (int i = 0; i < threads - 1; i++) {
        EventWorker *worker = pool->workers + i;
        worker->pool = pool;
        worker->id = i + 1;
        worker->reconfigure_id = -1;
        if (pthread_cond_init(&worker->work, NULL)) {
            event_pool_done(pool);
            return NULL;
        }
        if (pthread_create(&worker->thread, NULL, event_worker, worker)) {
            pthread_cond_destroy(&worker->work);
            event_pool_done(pool);
            return NULL;
        }
        pool->count++;
    }
    return pool;
}

static bool event_pool_prepare_events(struct ass_event_pool *pool)
{
    if (pool->event_ready)
        return true;
    for (int i = 0; i < pool->count; i++) {
        ASS_Renderer *renderer = ass_renderer_init(pool->owner->library);
        if (!renderer) {
            for (int j = 0; j < i; j++) {
                ass_renderer_done(pool->workers[j].renderer);
                pool->workers[j].renderer = NULL;
            }
            return false;
        }
        renderer->is_event_worker = true;
        renderer->cache.glyph_max = 0;
        renderer->cache.bitmap_max_size = 0;
        renderer->cache.composite_max_size = 0;
        pool->workers[i].renderer = renderer;
    }
    pool->event_ready = true;
    ASS_Renderer *owner = pool->owner;
    pool->glyph_budget = owner->cache.glyph_max;
    pool->adaptive_default = (owner->cache.bitmap_max_size == 0);
    if (pool->adaptive_default) {
        refresh_pool_adaptive_budgets(pool, false);
    } else {
        pool->bitmap_budget = owner->cache.bitmap_max_size;
        pool->composite_budget = owner->cache.composite_max_size;
    }
    return true;
}

static bool certify_primary_events(ASS_Renderer *priv, ASS_Track *track,
                                    long long now, struct ass_event_pool *pool,
                                    unsigned occupied)
{
    RenderContext *state = &priv->state;
    priv->primary_preflight = true;
    priv->primary_preflight_failed = false;
    if (pool) {
        for (int w = 0; w < pool->count; w++) {
            if (!(occupied & (1U << pool->workers[w].id)))
                continue;
            if (pool->workers[w].renderer->experimental_edges != priv->experimental_edges)
                goto reject;
        }
    }
    for (int i = 0; i < track->n_events; i++) {
        ASS_Event *event = track->events + i;
        if (!(event->Start <= now && now < event->Start + event->Duration))
            continue;
        if (event->Style < 0 || event->Style >= track->n_styles || !event->Text)
            goto reject;
        free_render_context(state);
        priv->primary_preflight_failed = false;
        init_render_context(state, event);
        if (!parse_events(state, event))
            goto reject;
        ASS_Font *last_font = NULL;
        ASS_Font *last_matches[3] = {NULL, NULL, NULL};
        for (int j = 0; j < state->text_info.length; j++) {
            GlyphInfo *info = state->text_info.glyphs + j;
            if (info->drawing_text.str)
                continue;
            if (priv->primary_preflight_failed)
                goto reject;
            ASS_Font *font = info->font;
            if (!ass_font_primary_equal(font, font))
                goto reject;
            // Do not resolve fallback here, even on a rejected frame.
            FT_UInt glyph = 0;
            if (info->symbol >= 0x20) {
                if (!(glyph = ass_font_get_char_index(font, 0, info->symbol)))
                    goto reject;
            }
            if (!pool)
                continue;
            if (font != last_font) {
                ASS_FontDesc desc = font->desc;
                for (int w = 0; w < pool->count; w++) {
                    if (!(occupied & (1U << pool->workers[w].id))) {
                        last_matches[w] = NULL;
                        continue;
                    }
                    ASS_Font *match = ass_font_new(pool->workers[w].renderer, &desc);
                    if (!ass_font_primary_equal(font, match))
                        goto reject;
                    last_matches[w] = match;
                }
                last_font = font;
            }
            if (glyph) {
                for (int w = 0; w < pool->count; w++) {
                    ASS_Font *match = last_matches[w];
                    if (!match)
                        continue;
                    if (ass_font_get_char_index(match, 0, info->symbol) != glyph)
                        goto reject;
                }
            }
        }
        free_render_context(state);
    }
    priv->primary_preflight = priv->primary_preflight_failed = false;
    return true;

reject:
    free_render_context(state);
    priv->primary_preflight = priv->primary_preflight_failed = false;
    return false;
}

bool ass_run_vertical_filter(ASS_Renderer *renderer, FilterFunc *plain,
                             ParamFilterFunc *param_filter, int16_t *dst,
                             const int16_t *src, size_t width, size_t src_h,
                             size_t dst_h, const int16_t *coeff, size_t weight)
{
    if (renderer->is_event_worker || !width || !src_h || !dst_h || !weight ||
        !src || !dst || src == dst || !!plain == !!param_filter ||
        (param_filter && !coeff))
        return false;
    size_t min_pixels = (64 * 1024 + weight - 1) / weight;
    if (width < min_pixels && src_h < (min_pixels + width - 1) / width)
        return false;
    size_t stripe = (size_t) 1 << (renderer->engine.align_order - 1);
    if (width > SIZE_MAX - (stripe - 1))
        return false;
    size_t stripes = (width + stripe - 1) / stripe;
    if (stripes < 2 || src_h > PTRDIFF_MAX / sizeof(*src) / (stripes * stripe) ||
        dst_h > PTRDIFF_MAX / sizeof(*dst) / (stripes * stripe))
        return false;
    struct ass_event_pool *pool = renderer->event_pool;
    if (pool && pool->job_kind != JOB_IDLE)
        return false;
    const char *value = getenv("LIBASS_RENDER_THREADS");
    if (value && atoi(value) <= 1)
        return false;
    if (!pool) {
        pool = event_pool_init(renderer);
        if (!pool)
            return false;
        renderer->event_pool = pool;
    }
    pthread_mutex_lock(&pool->lock);
    pool->vertical = (VerticalTask) {plain, param_filter, dst, src, coeff,
        width, src_h, dst_h, stripe, (unsigned) FFMIN(stripes, (size_t) pool->count + 1)};
    pool->job_kind = JOB_VERTICAL;
    start_worker_job(pool, (1U << pool->vertical.lanes) - 1);
    pthread_mutex_unlock(&pool->lock);
    vertical_batch(&pool->vertical, 0);
    pthread_mutex_lock(&pool->lock);
    while (pool->pending)
        pthread_cond_wait(&pool->done, &pool->lock);
    pool->job_kind = JOB_IDLE;
    memset(&pool->vertical, 0, sizeof(pool->vertical));
    pthread_mutex_unlock(&pool->lock);
    drain_worker_messages(pool);
    return true;
}

bool ass_run_blur_slices(ASS_Renderer *renderer, BlurSliceFunc *fn, void *ctx,
                         size_t items, size_t width, size_t height, size_t weight)
{
    if (renderer->is_event_worker || !fn || items < 2 || !width || !height || !weight)
        return false;
    size_t min_pixels = (64 * 1024 + weight - 1) / weight;
    if (width < min_pixels && height < (min_pixels + width - 1) / width)
        return false;
    struct ass_event_pool *pool = renderer->event_pool;
    if (pool && pool->job_kind != JOB_IDLE)
        return false;
    const char *value = getenv("LIBASS_RENDER_THREADS");
    if (value && atoi(value) <= 1)
        return false;
    if (!pool) {
        pool = event_pool_init(renderer);
        if (!pool)
            return false;
        renderer->event_pool = pool;
    }
    unsigned lanes = (unsigned) FFMIN(items, (size_t) pool->count + 1);
    pthread_mutex_lock(&pool->lock);
    pool->slice = (BlurSliceTask) {fn, ctx, lanes};
    pool->job_kind = JOB_SLICE;
    start_worker_job(pool, (1U << lanes) - 1);
    pthread_mutex_unlock(&pool->lock);
    fn(ctx, 0, lanes);
    pthread_mutex_lock(&pool->lock);
    while (pool->pending)
        pthread_cond_wait(&pool->done, &pool->lock);
    pool->job_kind = JOB_IDLE;
    memset(&pool->slice, 0, sizeof(pool->slice));
    pthread_mutex_unlock(&pool->lock);
    drain_worker_messages(pool);
    return true;
}

static bool render_events_parallel(ASS_Renderer *priv, ASS_Track *track,
                                   long long now, int *rendered)
{
    struct ass_event_pool *pool = priv->event_pool;
    bool preflight_attempted = false;
    int prev_active = priv->last_active_events;
    int active = 0;
    if (priv->custom_font_provider)
        goto serial;
    for (int i = 0; i < track->n_events; i++) {
        ASS_Event *event = track->events + i;
        if (event->Start <= now && now < event->Start + event->Duration) {
            if (pool && active < pool->capacity)
                pool->events[active] = (EventTask) {i, 0, 0};
            active++;
        }
    }
    priv->last_active_events = active;
    bool collected = pool && active <= pool->capacity;
    if (active < 2)
        goto serial;
    bool spike = active >= 12 || (active >= 6 && active > prev_active + 4);
    bool expensive = active >= 2 && (priv->last_event_render_ns > 4000000 || spike);
    const char *value = getenv("LIBASS_RENDER_THREADS");
    if (value && atoi(value) <= 1)
        goto serial;
    bool legacy = pool && pool->legacy_events;
    legacy |= active >= 32 || (priv->experimental_edges && expensive) || value;
    bool restricted = !legacy;
    if (restricted) {
        if (priv->primary_retry_frames) {
            priv->primary_retry_frames--;
            goto serial;
        }
        if (priv->experimental_edges || !expensive)
            goto serial;
        preflight_attempted = true;
        if (!certify_primary_events(priv, track, now, NULL, 0))
            goto serial;
    }
    if (!pool || !pool->event_ready) {
        if (!pool) {
            priv->event_pool = event_pool_init(priv);
            if (!priv->event_pool) goto serial;
            pool = priv->event_pool;
        }
        if (!event_pool_prepare_events(pool))
            goto serial;
    }
    if (legacy)
        pool->legacy_events = true;
    unsigned desired_shift = pool->lane_shift;
    if (active >= 640)
        desired_shift = 3;
    else if (active < 320)
        desired_shift = 0;
    pool->lane_shift = desired_shift;
    if (active > pool->capacity) {
        if (!ASS_REALLOC_ARRAY(pool->events, active)) goto serial;
        if (!ASS_REALLOC_ARRAY(pool->lane_slots, active)) goto serial;
        pool->capacity = active;
        collected = false;
    }
    if (active > priv->eimg_size) {
        EventImages *images = ass_realloc_array(priv->eimg, active, sizeof(*images));
        if (!images) goto serial;
        priv->eimg = images;
        priv->eimg_size = active;
    }
    if (!collected) {
        int index = 0;
        for (int i = 0; i < track->n_events; i++) {
            ASS_Event *event = track->events + i;
            if (event->Start <= now && now < event->Start + event->Duration)
                pool->events[index++] = (EventTask) {i, 0, 0};
        }
    }
    int n_lanes = pool->count + 1;
    int lane_counts[4] = {0, 0, 0, 0};
    pool->measure_events = active <= 64;
    if (pool->cost_track != track) {
        memset(pool->cost_hints, 0, sizeof(pool->cost_hints));
        pool->cost_track = track;
    }
    bool use_lpt = false;
    int order[64];
    if (active <= 64) {
        uint64_t lane_load[4] = {0, 0, 0, 0};
        uint64_t total_load = 0;
        uint32_t max_ev = 0;
        int known = 0;
        for (int i = 0; i < active; i++) {
            ASS_Event *ev = track->events + pool->events[i].event_index;
            unsigned lane = event_worker_lane(pool, ev);
            pool->events[i].lane = lane;
            const EventCostHint *h = pool->cost_hints + ((unsigned) ev->ReadOrder & 255);
            uint32_t cost = 0;
            if (h->event == ev) {
                cost = h->cost_us;
                known++;
            }
            pool->events[i].cost_us = cost;
            lane_load[lane] += cost;
            total_load += cost;
            if (cost > max_ev)
                max_ev = cost;
            order[i] = i;
        }
        if (max_ev >= 2000 && known * 4 >= active * 3) {
            use_lpt = true;
            // Insertion sort active indices by descending previous render cost.
            for (int i = 1; i < active; i++) {
                int idx = order[i];
                uint32_t c = pool->events[idx].cost_us;
                int j = i - 1;
                while (j >= 0 && pool->events[order[j]].cost_us < c) {
                    order[j + 1] = order[j];
                    j--;
                }
                order[j + 1] = idx;
            }
            uint64_t max_load = 0;
            for (int l = 0; l < n_lanes; l++)
                if (lane_load[l] > max_load)
                    max_load = lane_load[l];
            // Rebalance with greedy LPT only when default affinity is significantly skewed.
            if (max_load > 8000 && max_load * (uint64_t) n_lanes * 4 > total_load * 5) {
                uint64_t lpt_load[4] = {0, 0, 0, 0};
                for (int k = 0; k < active; k++) {
                    int idx = order[k];
                    unsigned pref = pool->events[idx].lane;
                    unsigned best = pref;
                    uint64_t best_load = lpt_load[pref];
                    for (int l = 0; l < n_lanes; l++) {
                        if (lpt_load[l] < best_load) {
                            best = (unsigned) l;
                            best_load = lpt_load[l];
                        }
                    }
                    pool->events[idx].lane = best;
                    uint32_t c = pool->events[idx].cost_us;
                    lpt_load[best] += c ? c : 250;
                }
            }
        }
        for (int i = 0; i < active; i++)
            lane_counts[pool->events[i].lane]++;
    } else {
        for (int i = 0; i < active; i++) {
            unsigned lane = event_worker_lane(pool, track->events + pool->events[i].event_index);
            pool->events[i].lane = lane;
            lane_counts[lane]++;
        }
    }
    int lane_pos[4];
    int offset = 0;
    for (int l = 0; l < n_lanes; l++) {
        lane_pos[l] = offset;
        int end = offset + lane_counts[l];
        pool->lanes[l].range = ((uint64_t) (uint32_t) end << 32) | (uint32_t) offset;
        offset = end;
    }
    if (use_lpt) {
        for (int k = 0; k < active; k++) {
            int idx = order[k];
            pool->lane_slots[lane_pos[pool->events[idx].lane]++] = idx;
        }
    } else {
        for (int i = 0; i < active; i++)
            pool->lane_slots[lane_pos[pool->events[i].lane]++] = i;
    }

    unsigned occupied = 0;
    int active_lanes = 0;
    for (int i = 0; i < n_lanes; i++) {
        if (lane_counts[i] > 0) {
            occupied |= 1U << i;
            active_lanes++;
        }
    }
    int target_lanes = FFMIN(active, n_lanes);
    for (int i = 0; i < n_lanes && active_lanes < target_lanes; i++) {
        if (!(occupied & (1U << i))) {
            occupied |= 1U << i;
            active_lanes++;
        }
    }
    // One occupied lane gains nothing from event dispatch. The serial renderer
    // can instead use this pool for a large blur, regardless of previous scenes.
    if (!(occupied & (occupied - 1))) {
        preflight_attempted = false;
        goto serial;
    }
    bool prepare_parallel = ass_fontselect_can_clone_concurrently(priv->fontselect);
    bool prepare_fonts = false;
    for (int i = 0; i < pool->count; i++)
        prepare_fonts |= (occupied & (1U << pool->workers[i].id)) &&
            pool->workers[i].font_generation != priv->font_generation;
    prepare_parallel = prepare_parallel && prepare_fonts;
    if (prepare_parallel) {
        // Owner font import is complete; cloned selectors have private FT faces.
        pthread_mutex_lock(&pool->lock);
        pool->job_kind = JOB_PREPARE;
        start_worker_job(pool, occupied);
        while (pool->pending)
            pthread_cond_wait(&pool->done, &pool->lock);
        pool->job_kind = JOB_IDLE;
        pthread_mutex_unlock(&pool->lock);
        drain_worker_messages(pool);
    }
    // When already split, ass_start_frame(priv) skipped adaptive growth because
    // owner->cache.bitmap_max_size != 0, so update growth here across all lanes.
    // When transitioning from serial, ass_start_frame(priv) already ran growth;
    // just refresh the budget numbers without decaying twice.
    refresh_pool_adaptive_budgets(pool, pool->cache_split);
    size_t lanes = (size_t) pool->count + 1;
    size_t w_glyph = pool->glyph_budget / lanes;
    size_t w_bitmap = pool->bitmap_budget / lanes;
    size_t w_composite = pool->composite_budget / lanes;
    for (int i = 0; i < pool->count; i++) {
        EventWorker *worker = pool->workers + i;
        worker->batch_ns = 0;
#ifdef ASS_FLE_PROFILE
        memset(worker->renderer->fle_phase_ns, 0, sizeof(priv->fle_phase_ns));
#endif
        if (!(occupied & (1U << worker->id)))
            continue;
        worker->renderer->look_generation = priv->look_generation;
        worker->renderer->cache.glyph_max = w_glyph;
        worker->renderer->cache.bitmap_max_size = w_bitmap;
        worker->renderer->cache.composite_max_size = w_composite;
        if (!(prepare_parallel ? worker->prepared :
                ass_clone_render_settings(worker->renderer, priv,
                    &worker->font_generation, &worker->reconfigure_id)) ||
            !ass_start_frame(worker->renderer, track, now))
            goto serial;
    }
    // Resource readiness is not admission: certify every restricted frame.
    if (restricted && !certify_primary_events(priv, track, now, pool, occupied))
        goto serial;
    set_pool_cache_limits(pool, pool->glyph_budget, pool->bitmap_budget, pool->composite_budget);
    pthread_mutex_lock(&pool->lock);
    pool->job_kind = JOB_EVENTS;
    start_worker_job(pool, occupied);
    pthread_mutex_unlock(&pool->lock);
    uint64_t main_ns = event_batch(pool, priv, 0);
    pthread_mutex_lock(&pool->lock);
    while (pool->pending)
        pthread_cond_wait(&pool->done, &pool->lock);
    pool->job_kind = JOB_IDLE;
    pthread_mutex_unlock(&pool->lock);
    if (pool->measure_events) {
        for (int i = 0; i < active; i++) {
            ASS_Event *ev = track->events + pool->events[i].event_index;
            EventCostHint *h = pool->cost_hints + ((unsigned) ev->ReadOrder & 255);
            h->event = ev;
            h->cost_us = pool->events[i].cost_us;
        }
    }
    drain_worker_messages(pool);
    if (restricted) {
        uint64_t work_ns = main_ns;
        bool valid = main_ns != UINT64_MAX;
        for (int i = 0; i < pool->count; i++) {
            uint64_t batch_ns = pool->workers[i].batch_ns;
            if (batch_ns == UINT64_MAX) {
                valid = false;
                break;
            }
            work_ns = batch_ns > UINT64_MAX - work_ns ? UINT64_MAX : work_ns + batch_ns;
        }
        // Sum lane work, not parallel latency, to keep the admission estimate current.
        if (valid)
            priv->last_event_render_ns = work_ns;
    }
#ifdef ASS_FLE_PROFILE
    ass_msg(priv->library, MSGL_INFO, "FLE_WORKERS,%lld,%" PRIu64 ",%" PRIu64
            ",%" PRIu64 ",%" PRIu64, now, main_ns, pool->workers[0].batch_ns,
            pool->count > 1 ? pool->workers[1].batch_ns : 0,
            pool->count > 2 ? pool->workers[2].batch_ns : 0);
    for (int i = 0; i < pool->count; i++)
        for (int p = 0; p < 10; p++)
            priv->fle_phase_ns[p] += pool->workers[i].renderer->fle_phase_ns[p];
#endif
    int cnt = 0;
    for (int i = 0; i < active; i++)
        if (priv->eimg[i].event)
            priv->eimg[cnt++] = priv->eimg[i];
    *rendered = cnt;
    return true;

serial:
    // Bound repeated parse-only work on expensive, uncertifiable frames.
    if (preflight_attempted)
        priv->primary_retry_frames = 8;
    restore_serial_cache_limits(pool);
    return false;
}
#else
bool ass_run_vertical_filter(ASS_Renderer *renderer, FilterFunc *plain,
                             ParamFilterFunc *param_filter, int16_t *dst,
                             const int16_t *src, size_t width, size_t src_h,
                             size_t dst_h, const int16_t *coeff, size_t weight)
{
    (void) renderer;
    (void) plain;
    (void) param_filter;
    (void) dst;
    (void) src;
    (void) width;
    (void) src_h;
    (void) dst_h;
    (void) coeff;
    (void) weight;
    return false;
}

bool ass_run_blur_slices(ASS_Renderer *renderer, BlurSliceFunc *fn, void *ctx,
                         size_t items, size_t width, size_t height, size_t weight)
{
    (void) renderer; (void) fn; (void) ctx;
    (void) items; (void) width; (void) height; (void) weight;
    return false;
}
#endif

static int cmp_event_layer(const void *p1, const void *p2)
{
    ASS_Event *e1 = ((EventImages *) p1)->event;
    ASS_Event *e2 = ((EventImages *) p2)->event;
    if (e1->Layer < e2->Layer)
        return -1;
    if (e1->Layer > e2->Layer)
        return 1;
    if (e1->ReadOrder < e2->ReadOrder)
        return -1;
    if (e1->ReadOrder > e2->ReadOrder)
        return 1;
    return 0;
}

static ASS_RenderPriv *get_render_priv(ASS_Renderer *render_priv,
                                       ASS_Event *event)
{
    if (!event->render_priv) {
        event->render_priv = calloc(1, sizeof(ASS_RenderPriv));
        if (!event->render_priv)
            return NULL;
    }
    if (render_priv->render_id != event->render_priv->render_id) {
        const char *timed_text = event->render_priv->timed_text;
        uint32_t timed_look = event->render_priv->timed_look;
        long long plateau_from = event->render_priv->plateau_from;
        long long plateau_until = event->render_priv->plateau_until;
        long long transparent_from = event->render_priv->transparent_from;
        long long transparent_until = event->render_priv->transparent_until;
        memset(event->render_priv, 0, sizeof(ASS_RenderPriv));
        event->render_priv->render_id = render_priv->render_id;
        event->render_priv->timed_text = timed_text;
        event->render_priv->timed_look = timed_look;
        event->render_priv->plateau_from = plateau_from;
        event->render_priv->plateau_until = plateau_until;
        event->render_priv->transparent_from = transparent_from;
        event->render_priv->transparent_until = transparent_until;
    }

    return event->render_priv;
}

static int overlap(Rect *s1, Rect *s2)
{
    if (s1->y0 >= s2->y1 || s2->y0 >= s1->y1 ||
        s1->x0 >= s2->x1 || s2->x0 >= s1->x1)
        return 0;
    return 1;
}

static int cmp_rect_y0(const void *p1, const void *p2)
{
    return ((Rect *) p1)->y0 - ((Rect *) p2)->y0;
}

static void
shift_event(ASS_Renderer *render_priv, EventImages *ei, int shift)
{
    ASS_Image *cur = ei->imgs;
    while (cur) {
        cur->dst_y += shift;
        // clip top and bottom
        if (cur->dst_y < 0) {
            if (cur->dst_y <= -cur->h) {
                cur->h = 0;
            } else {
                int clip = -cur->dst_y;
                cur->h -= clip;
                cur->bitmap += (ptrdiff_t) clip * cur->stride;
            }
            cur->dst_y = 0;
        }
        if (cur->dst_y + cur->h >= render_priv->height) {
            int clip = cur->dst_y + cur->h - render_priv->height;
            cur->h -= clip;
        }
        if (cur->h <= 0) {
            cur->h = 0;
            cur->dst_y = 0;
        }
        cur = cur->next;
    }
    ei->top += shift;
}

// dir: 1 - move down
//      -1 - move up
static int fit_rect(Rect *s, Rect *fixed, int *cnt, int dir)
{
    int i;
    int shift = 0;

    if (dir == 1)               // move down
        for (i = 0; i < *cnt; ++i) {
            if (s->y1 + shift <= fixed[i].y0 || s->y0 + shift >= fixed[i].y1 ||
                s->x1 <= fixed[i].x0 || s->x0 >= fixed[i].x1)
                continue;
            shift = fixed[i].y1 - s->y0;
    } else                      // dir == -1, move up
        for (i = *cnt - 1; i >= 0; --i) {
            if (s->y1 + shift <= fixed[i].y0 || s->y0 + shift >= fixed[i].y1 ||
                s->x1 <= fixed[i].x0 || s->x0 >= fixed[i].x1)
                continue;
            shift = fixed[i].y0 - s->y1;
        }

    fixed[*cnt].y0 = s->y0 + shift;
    fixed[*cnt].y1 = s->y1 + shift;
    fixed[*cnt].x0 = s->x0;
    fixed[*cnt].x1 = s->x1;
    (*cnt)++;
    qsort(fixed, *cnt, sizeof(*fixed), cmp_rect_y0);

    return shift;
}

static void
fix_collisions(ASS_Renderer *render_priv, EventImages *imgs, int cnt)
{
    Rect *used = ass_realloc_array(NULL, cnt, sizeof(*used));
    int cnt_used = 0;
    int i, j;

    if (!used)
        return;

    // fill used[] with fixed events
    for (i = 0; i < cnt; ++i) {
        ASS_RenderPriv *priv;
        // VSFilter considers events colliding if their intersections area is non-zero,
        // zero-area events are therefore effectively fixed as well
        if (!imgs[i].detect_collisions || !imgs[i].height  || !imgs[i].width)
            continue;
        priv = get_render_priv(render_priv, imgs[i].event);
        if (priv && priv->height > 0) { // it's a fixed event
            Rect s;
            s.y0 = priv->top;
            s.y1 = priv->top + priv->height;
            s.x0 = priv->left;
            s.x1 = priv->left + priv->width;
            if (priv->height != imgs[i].height) {       // no, it's not
                ass_msg(render_priv->library, MSGL_WARN,
                        "Event height has changed");
                priv->top = 0;
                priv->height = 0;
                priv->left = 0;
                priv->width = 0;
            }
            for (j = 0; j < cnt_used; ++j)
                if (overlap(&s, used + j)) {    // no, it's not
                    priv->top = 0;
                    priv->height = 0;
                    priv->left = 0;
                    priv->width = 0;
                }
            if (priv->height > 0) {     // still a fixed event
                used[cnt_used].y0 = priv->top;
                used[cnt_used].y1 = priv->top + priv->height;
                used[cnt_used].x0 = priv->left;
                used[cnt_used].x1 = priv->left + priv->width;
                cnt_used++;
                shift_event(render_priv, imgs + i, priv->top - imgs[i].top);
            }
        }
    }
    qsort(used, cnt_used, sizeof(*used), cmp_rect_y0);

    // try to fit other events in free spaces
    for (i = 0; i < cnt; ++i) {
        ASS_RenderPriv *priv;
        if (!imgs[i].detect_collisions || !imgs[i].height  || !imgs[i].width)
            continue;
        priv = get_render_priv(render_priv, imgs[i].event);
        if (priv && priv->height == 0) {        // not a fixed event
            int shift;
            Rect s;
            s.y0 = imgs[i].top;
            s.y1 = imgs[i].top + imgs[i].height;
            s.x0 = imgs[i].left;
            s.x1 = imgs[i].left + imgs[i].width;
            shift = fit_rect(&s, used, &cnt_used, imgs[i].shift_direction);
            if (shift)
                shift_event(render_priv, imgs + i, shift);
            // make it fixed
            priv->top = imgs[i].top;
            priv->height = imgs[i].height;
            priv->left = imgs[i].left;
            priv->width = imgs[i].width;
        }

    }

    free(used);
}

/**
 * \brief compare two images
 * \param i1 first image
 * \param i2 second image
 * \return 0 if identical, 1 if different positions, 2 if different content
 */
static int ass_image_compare(ASS_Image *i1, ASS_Image *i2)
{
    if (i1->w != i2->w)
        return 2;
    if (i1->h != i2->h)
        return 2;
    if (i1->stride != i2->stride)
        return 2;
    if (i1->color != i2->color)
        return 2;
    if (i1->bitmap != i2->bitmap)
        return 2;
    if (i1->dst_x != i2->dst_x)
        return 1;
    if (i1->dst_y != i2->dst_y)
        return 1;
    return 0;
}

/**
 * \brief compare current and previous image list
 * \param priv library handle
 * \return 0 if identical, 1 if different positions, 2 if different content
 */
static int ass_detect_change(ASS_Renderer *priv)
{
    ASS_Image *img, *img2;
    int diff;

    img = priv->prev_images_root;
    img2 = priv->images_root;
    diff = 0;
    while (img && diff < 2) {
        ASS_Image *next, *next2;
        next = img->next;
        if (img2) {
            int d = ass_image_compare(img, img2);
            if (d > diff)
                diff = d;
            next2 = img2->next;
        } else {
            // previous list is shorter
            diff = 2;
            break;
        }
        img = next;
        img2 = next2;
    }

    // is the previous list longer?
    if (img2)
        diff = 2;

    return diff;
}

/**
 * \brief free a single image.
 * \param img image as returned by ass_render_frame().
 *        Only img will be freed, not img->next.
 * \return img->next
 * Should not be called outside of ass_frame_unref
 * once the image list's refcounting is set up.
 */
static ASS_Image *ass_free_image(ASS_Image *img) {
    ASS_Image *next = img->next;

    ASS_ImagePriv *priv = (ASS_ImagePriv *) img;
    ass_cache_dec_ref(priv->source);
    ass_aligned_free(priv->buffer);
    free(priv);

    return next;
}

/**
 * \brief render a frame
 * \param priv library handle
 * \param track track
 * \param now current video timestamp (ms)
 * \param detect_change a value describing how the new images differ from the previous ones will be written here:
 *        0 if identical, 1 if different positions, 2 if different content.
 *        Can be NULL, in that case no detection is performed.
 */
ASS_Image *ass_render_frame(ASS_Renderer *priv, ASS_Track *track,
                            long long now, int *detect_change)
{
#ifdef ASS_FLE_PROFILE
    memset(priv->fle_phase_ns, 0, sizeof(priv->fle_phase_ns));
#endif
    // init frame
    if (!ass_start_frame(priv, track, now)) {
        if (detect_change)
            *detect_change = 2;
        return NULL;
    }

    // render events separately
    int cnt = 0;
    bool parallel = false;
#ifdef ASS_EVENT_THREADS
    parallel = render_events_parallel(priv, track, now, &cnt);
    uint64_t event_start = !parallel ? fle_clock() : 0;
#endif
    if (!parallel)
    for (int i = 0; i < track->n_events; i++) {
        ASS_Event *event = track->events + i;
        if ((event->Start <= now)
            && (now < (event->Start + event->Duration))) {
            if (cnt >= priv->eimg_size) {
                priv->eimg_size += 100;
                priv->eimg =
                    realloc(priv->eimg,
                            priv->eimg_size * sizeof(EventImages));
            }
            if (ass_render_event(&priv->state, event, priv->eimg + cnt))
                cnt++;
        }
    }

#ifdef ASS_EVENT_THREADS
    if (!parallel) {
        uint64_t elapsed = event_elapsed_ns(event_start, fle_clock());
        if (elapsed != UINT64_MAX)
            priv->last_event_render_ns = elapsed;
    }
#endif

    // sort by layer
    if (cnt > 0)
        qsort(priv->eimg, cnt, sizeof(EventImages), cmp_event_layer);

    // call fix_collisions for each group of events with the same layer
    EventImages *last = priv->eimg;
    for (int i = 1; i < cnt; i++)
        if (last->event->Layer != priv->eimg[i].event->Layer) {
            fix_collisions(priv, last, priv->eimg + i - last);
            last = priv->eimg + i;
        }
    if (cnt > 0)
        fix_collisions(priv, last, priv->eimg + cnt - last);

    // concat lists, removing fully transparent bitmaps
    unsigned alpha_skip = priv->experimental_edges ? 0xFE : 0xFF;
    ASS_Image **tail = &priv->images_root;
    for (int i = 0; i < cnt; i++) {
        ASS_Image *cur = priv->eimg[i].imgs;
        while (cur) {
            if (_a(cur->color) >= alpha_skip) {
                cur = ass_free_image(cur);
                continue;
            }

            *tail = cur;
            tail = &cur->next;
            cur = cur->next;
        }
    }

    // If the last image was skipped in the above loop, *tail may not be NULL and needs to be set to NULL.
    *tail = NULL;

    ass_frame_ref(priv->images_root);

    if (detect_change)
        *detect_change = ass_detect_change(priv);

    // free the previous image list
    ass_frame_unref(priv->prev_images_root);
    priv->prev_images_root = NULL;

    if (track->parser_priv->prune_delay >= 0)
        ass_prune_events(track, now - track->parser_priv->prune_delay);

#ifdef ASS_FLE_PROFILE
    ass_msg(priv->library, MSGL_INFO,
            "FLE_PROFILE,%lld,%d,%" PRIu64 ",%" PRIu64 ",%" PRIu64
            ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
            ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64,
            now, cnt, priv->fle_phase_ns[0], priv->fle_phase_ns[1],
            priv->fle_phase_ns[2], priv->fle_phase_ns[3],
            priv->fle_phase_ns[4], priv->fle_phase_ns[5],
            priv->fle_phase_ns[6], priv->fle_phase_ns[7],
            priv->fle_phase_ns[8], priv->fle_phase_ns[9]);
#endif
    return priv->images_root;
}

/**
 * \brief Add reference to a frame image list.
 * \param image_list image list returned by ass_render_frame()
 */
void ass_frame_ref(ASS_Image *img)
{
    if (!img)
        return;
    ((ASS_ImagePriv *) img)->ref_count++;
}

/**
 * \brief Release reference to a frame image list.
 * \param image_list image list returned by ass_render_frame()
 */
void ass_frame_unref(ASS_Image *img)
{
    if (!img || --((ASS_ImagePriv *) img)->ref_count)
        return;
    do {
        img = ass_free_image(img);
    } while (img);
}
