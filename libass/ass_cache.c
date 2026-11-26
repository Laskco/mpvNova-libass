/*
 * Copyright (C) 2006 Evgeniy Stepanov <eugeni.stepanov@gmail.com>
 * Copyright (C) 2011 Grigori Goronzy <greg@chown.ath.cx>
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

#include <inttypes.h>
#include <ft2build.h>
#include FT_OUTLINE_H
#include <assert.h>

#include "ass_utils.h"
#include "ass_font.h"
#include "ass_outline.h"
#include "ass_cache.h"
#include "ass_render.h"

// Always enable native-endian mode, since we don't care about cross-platform consistency of the hash
#define WYHASH_LITTLE_ENDIAN 1
#include "wyhash.h"

// With wyhash any arbitrary 64 bit value will suffice
#define ASS_HASH_INIT 0xb3e46a540bd36cd4ULL

static inline ass_hashcode ass_hash_buf(const void *buf, size_t len, ass_hashcode hval)
{
    return wyhash(buf, len, hval, _wyp);
}

// type-specific functions
// create hash/compare functions for bitmap, outline and composite cache
#define CREATE_HASH_FUNCTIONS
#include "ass_cache_template.h"
#define CREATE_COMPARISON_FUNCTIONS
#include "ass_cache_template.h"

// font cache
static bool font_key_move(void *dst, void *src)
{
    ASS_FontDesc *d = dst, *s = src;
    if (!d)
        return true;

    *d = *s;
    d->family.str = ass_copy_string(s->family);
    return d->family.str;
}

static void font_destruct(void *key, void *value)
{
    ass_font_clear(value);
}

size_t ass_font_construct(void *key, void *value, void *priv);

const CacheDesc font_cache_desc = {
    .hash_func = font_hash,
    .compare_func = font_compare,
    .key_move_func = font_key_move,
    .construct_func = ass_font_construct,
    .destruct_func = font_destruct,
    .key_size = sizeof(ASS_FontDesc),
    .value_size = sizeof(ASS_Font)
};


// bitmap cache
static bool bitmap_key_move(void *dst, void *src)
{
    BitmapHashKey *d = dst, *s = src;
    if (d) {
        *d = *s;
        ass_cache_inc_ref(d->outline);
    }
    return true;
}

static void bitmap_destruct(void *key, void *value)
{
    BitmapHashKey *k = key;
    ass_free_bitmap(value);
    ass_cache_dec_ref(k->outline);
}

size_t ass_bitmap_construct(void *key, void *value, void *priv);

const CacheDesc bitmap_cache_desc = {
    .hash_func = bitmap_hash,
    .compare_func = bitmap_compare,
    .key_move_func = bitmap_key_move,
    .construct_func = ass_bitmap_construct,
    .destruct_func = bitmap_destruct,
    .key_size = sizeof(BitmapHashKey),
    .value_size = sizeof(Bitmap)
};


// composite cache
static inline const BitmapRef *composite_bitmaps(const CompositeHashKey *k)
{
    return k->bitmaps ? k->bitmaps : &k->single_bitmap;
}

static ass_hashcode composite_hash(void *key, ass_hashcode hval)
{
    CompositeHashKey *k = key;
    const BitmapRef *bitmaps = composite_bitmaps(k);
    hval = filter_hash(&k->filter, hval);
    hval = ass_hash_buf(&k->viewport, sizeof(k->viewport), hval);
    hval = ass_hash_buf(&k->viewport_align, sizeof(k->viewport_align), hval);
    for (size_t i = 0; i < k->bitmap_count; i++)
        hval = bitmap_ref_hash((void *) &bitmaps[i], hval);
    return hval;
}

static bool composite_compare(void *a, void *b)
{
    CompositeHashKey *ak = a;
    CompositeHashKey *bk = b;
    if (!filter_compare(&ak->filter, &bk->filter))
        return false;
    if (ak->bitmap_count != bk->bitmap_count)
        return false;
    if (ak->viewport_align != bk->viewport_align ||
        memcmp(&ak->viewport, &bk->viewport, sizeof(ak->viewport)))
        return false;
    const BitmapRef *ab = composite_bitmaps(ak);
    const BitmapRef *bb = composite_bitmaps(bk);
    for (size_t i = 0; i < ak->bitmap_count; i++)
        if (!bitmap_ref_compare((void *) &ab[i], (void *) &bb[i]))
            return false;
    return true;
}

static bool composite_key_move(void *dst, void *src)
{
    CompositeHashKey *d = dst, *s = src;
    if (d) {
        *d = *s;
        const BitmapRef *bitmaps = composite_bitmaps(d);
        for (size_t i = 0; i < d->bitmap_count; i++) {
            ass_cache_inc_ref(bitmaps[i].bm);
            ass_cache_inc_ref(bitmaps[i].bm_o);
        }
        return true;
    }

    free(s->bitmaps);
    return true;
}

static void composite_destruct(void *key, void *value)
{
    CompositeHashValue *v = value;
    CompositeHashKey *k = key;
    ass_free_bitmap(&v->bm);
    ass_free_bitmap(&v->bm_o);
    ass_free_bitmap(&v->bm_s);
    const BitmapRef *bitmaps = composite_bitmaps(k);
    for (size_t i = 0; i < k->bitmap_count; i++) {
        ass_cache_dec_ref(bitmaps[i].bm);
        ass_cache_dec_ref(bitmaps[i].bm_o);
    }
    free(k->bitmaps);
}

size_t ass_composite_construct(void *key, void *value, void *priv);

const CacheDesc composite_cache_desc = {
    .hash_func = composite_hash,
    .compare_func = composite_compare,
    .key_move_func = composite_key_move,
    .construct_func = ass_composite_construct,
    .destruct_func = composite_destruct,
    .key_size = sizeof(CompositeHashKey),
    .value_size = sizeof(CompositeHashValue)
};


// outline cache
static ass_hashcode outline_hash(void *key, ass_hashcode hval)
{
    OutlineHashKey *k = key;
    switch (k->type) {
    case OUTLINE_GLYPH:
        return glyph_hash(&k->u, hval);
    case OUTLINE_DRAWING:
        return drawing_hash(&k->u, hval);
    case OUTLINE_BORDER:
        return border_hash(&k->u, hval);
    default:  // OUTLINE_BOX
        return hval;
    }
}

static bool outline_compare(void *a, void *b)
{
    OutlineHashKey *ak = a;
    OutlineHashKey *bk = b;
    if (ak->type != bk->type)
        return false;
    switch (ak->type) {
    case OUTLINE_GLYPH:
        return glyph_compare(&ak->u, &bk->u);
    case OUTLINE_DRAWING:
        return drawing_compare(&ak->u, &bk->u);
    case OUTLINE_BORDER:
        return border_compare(&ak->u, &bk->u);
    default:  // OUTLINE_BOX
        return true;
    }
}

static bool outline_key_move(void *dst, void *src)
{
    OutlineHashKey *d = dst, *s = src;
    if (!d)
        return true;

    *d = *s;
    if (s->type == OUTLINE_DRAWING) {
        d->u.drawing.text.str = ass_copy_string(s->u.drawing.text);
        return d->u.drawing.text.str;
    }
    if (s->type == OUTLINE_BORDER)
        ass_cache_inc_ref(s->u.border.outline);
    else if (s->type == OUTLINE_GLYPH)
        ass_cache_inc_ref(s->u.glyph.font);
    return true;
}

static void outline_destruct(void *key, void *value)
{
    OutlineHashValue *v = value;
    OutlineHashKey *k = key;
    ass_outline_free(&v->outline[0]);
    ass_outline_free(&v->outline[1]);
    for (int i = 0; i < OUTLINE_LOD_SLOTS; i++) {
        ass_outline_free(&v->lod[i].outline[0]);
        ass_outline_free(&v->lod[i].outline[1]);
    }
    switch (k->type) {
    case OUTLINE_GLYPH:
        ass_cache_dec_ref(k->u.glyph.font);
        break;
    case OUTLINE_DRAWING:
        free((char *) k->u.drawing.text.str);
        break;
    case OUTLINE_BORDER:
        ass_cache_dec_ref(k->u.border.outline);
        break;
    default:  // OUTLINE_BOX
        break;
    }
}

size_t ass_outline_construct(void *key, void *value, void *priv);

const CacheDesc outline_cache_desc = {
    .hash_func = outline_hash,
    .compare_func = outline_compare,
    .key_move_func = outline_key_move,
    .construct_func = ass_outline_construct,
    .destruct_func = outline_destruct,
    .key_size = sizeof(OutlineHashKey),
    .value_size = sizeof(OutlineHashValue)
};


// font-face size metric cache
static bool face_size_metrics_key_move(void *dst, void *src)
{
    FaceSizeMetricsHashKey *d = dst, *s = src;
    if (!d)
        return true;

    *d = *s;
    ass_cache_inc_ref(s->font);
    return true;
}

static void face_size_metrics_destruct(void *key, void *value)
{
    FaceSizeMetricsHashKey *k = key;
    ass_cache_dec_ref(k->font);
}

size_t ass_face_size_metrics_construct(void *key, void *value, void *priv);

const CacheDesc face_size_metrics_cache_desc = {
    .hash_func = face_size_metrics_hash,
    .compare_func = face_size_metrics_compare,
    .key_move_func = face_size_metrics_key_move,
    .construct_func = ass_face_size_metrics_construct,
    .destruct_func = face_size_metrics_destruct,
    .key_size = sizeof(FaceSizeMetricsHashKey),
    .value_size = sizeof(FT_Size_Metrics)
};


// glyph metric cache
static bool glyph_metrics_key_move(void *dst, void *src)
{
    GlyphMetricsHashKey *d = dst, *s = src;
    if (!d)
        return true;

    *d = *s;
    ass_cache_inc_ref(s->font);
    return true;
}

static void glyph_metrics_destruct(void *key, void *value)
{
    GlyphMetricsHashKey *k = key;
    ass_cache_dec_ref(k->font);
}

size_t ass_glyph_metrics_construct(void *key, void *value, void *priv);

const CacheDesc glyph_metrics_cache_desc = {
    .hash_func = glyph_metrics_hash,
    .compare_func = glyph_metrics_compare,
    .key_move_func = glyph_metrics_key_move,
    .construct_func = ass_glyph_metrics_construct,
    .destruct_func = glyph_metrics_destruct,
    .key_size = sizeof(GlyphMetricsHashKey),
    .value_size = sizeof(FT_Glyph_Metrics)
};


// shaped run cache
static bool shape_key_move(void *dst, void *src)
{
    ShapeHashKey *d = dst, *s = src;
    if (!d)
        return true;

    *d = *s;
    d->text.str = ass_copy_string(s->text);
    if (!d->text.str)
        return false;
    ass_cache_inc_ref(s->font);
    return true;
}

static void shape_destruct(void *key, void *value)
{
    ShapeHashKey *k = key;
    ShapeHashValue *v = value;
    free(v->glyphs);
    free((char *) k->text.str);
    ass_cache_dec_ref(k->font);
}

size_t ass_shape_construct(void *key, void *value, void *priv);

const CacheDesc shape_cache_desc = {
    .hash_func = shape_hash,
    .compare_func = shape_compare,
    .key_move_func = shape_key_move,
    .construct_func = ass_shape_construct,
    .destruct_func = shape_destruct,
    .key_size = sizeof(ShapeHashKey),
    .value_size = sizeof(ShapeHashValue)
};


// rendered event cache
static bool event_key_move(void *dst, void *src)
{
    EventHashKey *d = dst, *s = src;
    if (!d)
        return true;

    *d = *s;
    d->effect.str = ass_copy_string(s->effect);
    d->text.str = ass_copy_string(s->text);
    if (d->effect.str && d->text.str)
        return true;
    free((char *) d->effect.str);
    free((char *) d->text.str);
    return false;
}

static void event_destruct(void *key, void *value)
{
    EventHashKey *k = key;
    EventHashValue *v = value;
    ass_frame_unref(v->imgs);
    free((char *) k->effect.str);
    free((char *) k->text.str);
}

size_t ass_event_construct(void *key, void *value, void *priv);

const CacheDesc event_cache_desc = {
    .hash_func = event_hash,
    .compare_func = event_compare,
    .key_move_func = event_key_move,
    .construct_func = ass_event_construct,
    .destruct_func = event_destruct,
    .key_size = sizeof(EventHashKey),
    .value_size = sizeof(EventHashValue)
};



// Cache data
typedef struct cache_item {
    Cache *cache;
    const CacheDesc *desc;
    struct cache_item *next, **prev;
    struct cache_item *queue_next, **queue_prev;
    size_t size, ref_count;
    ass_hashcode hash;
    unsigned frame;
} CacheItem;

struct cache {
    unsigned buckets;
    CacheItem **map;
    CacheItem *queue_first, **queue_last;

    const CacheDesc *desc;

    size_t cache_size;
    unsigned frame;

    ass_hashcode *evicted;      // hashes of evicted items, one per slot
    unsigned evicted_mask;
    size_t returned;
};

#define CACHE_ALIGN 8
#define CACHE_ITEM_SIZE ((sizeof(CacheItem) + (CACHE_ALIGN - 1)) & ~(CACHE_ALIGN - 1))

static inline size_t align_cache(size_t size)
{
    return (size + (CACHE_ALIGN - 1)) & ~(CACHE_ALIGN - 1);
}

static inline CacheItem *value_to_item(void *value)
{
    return (CacheItem *) ((char *) value - CACHE_ITEM_SIZE);
}


// Create a cache with type-specific hash/compare/destruct/size functions
Cache *ass_cache_create(const CacheDesc *desc)
{
    Cache *cache = calloc(1, sizeof(*cache));
    if (!cache)
        return NULL;
    cache->buckets = 0x10000;
    cache->queue_last = &cache->queue_first;
    cache->desc = desc;
    cache->map = calloc(cache->buckets, sizeof(CacheItem *));
    if (!cache->map) {
        free(cache);
        return NULL;
    }

    return cache;
}

// Look up an existing value without constructing or taking ownership of key.
void *ass_cache_find(Cache *cache, void *key)
{
    const CacheDesc *desc = cache->desc;
    size_t key_offs = CACHE_ITEM_SIZE + align_cache(desc->value_size);
    ass_hashcode hash = desc->hash_func(key, ASS_HASH_INIT);
    hash ^= hash >> 32;
    hash ^= hash >> 16;
    unsigned bucket = (unsigned) hash & (cache->buckets - 1);
    CacheItem *item = cache->map[bucket];
    while (item) {
        if (desc->compare_func(key, (char *) item + key_offs)) {
            assert(item->size);
            item->frame = cache->frame;
            if (!item->queue_prev || item->queue_next) {
                if (item->queue_prev) {
                    item->queue_next->queue_prev = item->queue_prev;
                    *item->queue_prev = item->queue_next;
                } else
                    item->ref_count++;
                *cache->queue_last = item;
                item->queue_prev = cache->queue_last;
                cache->queue_last = &item->queue_next;
                item->queue_next = NULL;
            }
            return (char *) item + CACHE_ITEM_SIZE;
        }
        item = item->next;
    }
    return NULL;
}

// Retrieve a value corresponding to a particular cache key,
// creating one if it does not already exist.
// The returned item is guaranteed to be valid until the next ass_cache_cut call;
// to extend its lifetime further, call ass_cache_inc_ref().
void *ass_cache_get(Cache *cache, void *key, void *priv)
{
    const CacheDesc *desc = cache->desc;
    size_t key_offs = CACHE_ITEM_SIZE + align_cache(desc->value_size);
    ass_hashcode raw_hash = desc->hash_func(key, ASS_HASH_INIT);
    ass_hashcode hash = raw_hash;
    hash ^= hash >> 32;
    hash ^= hash >> 16;
    unsigned bucket = (unsigned) hash & (cache->buckets - 1);
    CacheItem *item = cache->map[bucket];
    while (item) {
        if (desc->compare_func(key, (char *) item + key_offs)) {
            assert(item->size);
            item->frame = cache->frame;
            if (!item->queue_prev || item->queue_next) {
                if (item->queue_prev) {
                    item->queue_next->queue_prev = item->queue_prev;
                    *item->queue_prev = item->queue_next;
                } else
                    item->ref_count++;
                *cache->queue_last = item;
                item->queue_prev = cache->queue_last;
                cache->queue_last = &item->queue_next;
                item->queue_next = NULL;
            }
            desc->key_move_func(NULL, key);

            return (char *) item + CACHE_ITEM_SIZE;
        }
        item = item->next;
    }

    item = malloc(key_offs + desc->key_size);
    if (!item) {
        desc->key_move_func(NULL, key);
        return NULL;
    }
    item->cache = cache;
    item->desc = desc;
    ass_hashcode ev_hash = raw_hash ? raw_hash : 1;
    item->hash = ev_hash;
    item->frame = cache->frame;
    void *new_key = (char *) item + key_offs;
    if (!desc->key_move_func(new_key, key)) {
        free(item);
        return NULL;
    }
    void *value = (char *) item + CACHE_ITEM_SIZE;
    item->size = desc->construct_func(new_key, value, priv);
    assert(item->size);

    CacheItem **bucketptr = &cache->map[bucket];
    if (*bucketptr)
        (*bucketptr)->prev = &item->next;
    item->prev = bucketptr;
    item->next = *bucketptr;
    *bucketptr = item;

    *cache->queue_last = item;
    item->queue_prev = cache->queue_last;
    cache->queue_last = &item->queue_next;
    item->queue_next = NULL;
    item->ref_count = 1;

    size_t cost = item->size + (item->size == 1 ? 0 : CACHE_ITEM_SIZE);
    cache->cache_size += cost;
    if (cache->evicted && cache->evicted[ev_hash & cache->evicted_mask] == ev_hash)
        cache->returned += cost;
    return value;
}

void ass_cache_use(void *value)
{
    if (!value)
        return;
    CacheItem *item = value_to_item(value);
    Cache *cache = item->cache;
    if (!cache)
        return;
    item->frame = cache->frame;
    if (!item->queue_prev || item->queue_next) {
        if (item->queue_prev) {
            item->queue_next->queue_prev = item->queue_prev;
            *item->queue_prev = item->queue_next;
        } else
            item->ref_count++;
        *cache->queue_last = item;
        item->queue_prev = cache->queue_last;
        cache->queue_last = &item->queue_next;
        item->queue_next = NULL;
    }
}

bool ass_cache_watch_returns(Cache *cache, unsigned slots_log2)
{
    free(cache->evicted);
    cache->evicted = calloc(1u << slots_log2, sizeof(*cache->evicted));
    cache->evicted_mask = cache->evicted ? (1u << slots_log2) - 1 : 0;
    cache->returned = 0;
    return cache->evicted != NULL;
}

size_t ass_cache_take_returned(Cache *cache)
{
    size_t ret = cache->returned;
    cache->returned = 0;
    return ret;
}

void *ass_cache_key(void *value)
{
    CacheItem *item = value_to_item(value);
    return (char *) value + align_cache(item->desc->value_size);
}

static inline void destroy_item(const CacheDesc *desc, CacheItem *item)
{
    assert(item->desc == desc);
    char *value = (char *) item + CACHE_ITEM_SIZE;
    desc->destruct_func(value + align_cache(desc->value_size), value);
    free(item);
}

void ass_cache_inc_ref(void *value)
{
    if (!value)
        return;
    CacheItem *item = value_to_item(value);
    assert(item->size && item->ref_count);
    item->ref_count++;
}

void ass_cache_dec_ref(void *value)
{
    if (!value)
        return;
    CacheItem *item = value_to_item(value);
    assert(item->size && item->ref_count);
    if (--item->ref_count)
        return;

    Cache *cache = item->cache;
    if (cache) {
        if (item->next)
            item->next->prev = item->prev;
        *item->prev = item->next;

        cache->cache_size -= item->size + (item->size == 1 ? 0 : CACHE_ITEM_SIZE);
        if (cache->evicted)
            cache->evicted[item->hash & cache->evicted_mask] = item->hash;
    }
    destroy_item(item->desc, item);
}

static void evict_queue_first(Cache *cache)
{
    CacheItem *item = cache->queue_first;
    assert(item->size);

    cache->queue_first = item->queue_next;
    if (--item->ref_count) {
        item->queue_prev = NULL;
        return;
    }

    if (item->next)
        item->next->prev = item->prev;
    *item->prev = item->next;

    cache->cache_size -= item->size + (item->size == 1 ? 0 : CACHE_ITEM_SIZE);
    if (cache->evicted)
        cache->evicted[item->hash & cache->evicted_mask] = item->hash;
    destroy_item(cache->desc, item);
}

void ass_cache_cut(Cache *cache, size_t max_size)
{
    if (cache->cache_size > max_size) {
        do {
            if (!cache->queue_first)
                break;
            evict_queue_first(cache);
        } while (cache->cache_size > max_size);
        if (cache->queue_first)
            cache->queue_first->queue_prev = &cache->queue_first;
        else
            cache->queue_last = &cache->queue_first;
    }
    cache->frame++;
}

void ass_cache_cut_unused(Cache *cache, unsigned frames)
{
    while (cache->queue_first && cache->frame - cache->queue_first->frame >= frames)
        evict_queue_first(cache);
    if (cache->queue_first)
        cache->queue_first->queue_prev = &cache->queue_first;
    else
        cache->queue_last = &cache->queue_first;
    cache->frame++;
}

void ass_cache_empty(Cache *cache)
{
    for (int i = 0; i < cache->buckets; i++) {
        CacheItem *item = cache->map[i];
        while (item) {
            assert(item->size);
            CacheItem *next = item->next;
            if (item->queue_prev)
                item->ref_count--;
            if (item->ref_count)
                item->cache = NULL;
            else
                destroy_item(cache->desc, item);
            item = next;
        }
        cache->map[i] = NULL;
    }

    cache->queue_first = NULL;
    cache->queue_last = &cache->queue_first;
    cache->cache_size = 0;
    if (cache->evicted)
        memset(cache->evicted, 0, (cache->evicted_mask + 1) * sizeof(*cache->evicted));
    cache->returned = 0;
}

void ass_cache_done(Cache *cache)
{
    if (!cache)
        return;
    ass_cache_empty(cache);
    free(cache->evicted);
    free(cache->map);
    free(cache);
}

// Type-specific creation function
Cache *ass_font_cache_create(void)
{
    return ass_cache_create(&font_cache_desc);
}

Cache *ass_outline_cache_create(void)
{
    return ass_cache_create(&outline_cache_desc);
}

Cache *ass_glyph_metrics_cache_create(void)
{
    return ass_cache_create(&glyph_metrics_cache_desc);
}

Cache *ass_shape_cache_create(void)
{
    return ass_cache_create(&shape_cache_desc);
}

Cache *ass_face_size_metrics_cache_create(void)
{
    return ass_cache_create(&face_size_metrics_cache_desc);
}

Cache *ass_bitmap_cache_create(void)
{
    return ass_cache_create(&bitmap_cache_desc);
}

Cache *ass_composite_cache_create(void)
{
    return ass_cache_create(&composite_cache_desc);
}

Cache *ass_event_cache_create(void)
{
    return ass_cache_create(&event_cache_desc);
}
