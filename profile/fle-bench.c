#include <dirent.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../libass/ass.h"

static uint64_t clock_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static void message(int level, const char *fmt, va_list args, void *data)
{
    if (level <= 3 || !strncmp(fmt, "FLE_PROFILE,", 12)) {
        vfprintf(stderr, fmt, args);
        fputc('\n', stderr);
    }
}

static void load_fonts(ASS_Library *library, const char *directory)
{
    struct dirent **entries;
    int count = scandir(directory, &entries, NULL, alphasort);
    if (count < 0) { perror(directory); exit(1); }
    for (int i = 0; i < count; i++) {
        const char *name = entries[i]->d_name;
        if (name[0] != '.') {
            char path[4096];
            if (snprintf(path, sizeof(path), "%s/%s", directory, name) >= sizeof(path))
                exit(1);
            FILE *file = fopen(path, "rb");
            if (!file || fseek(file, 0, SEEK_END)) exit(1);
            long size = ftell(file);
            if (size <= 0 || size > 64 * 1024 * 1024) exit(1);
            rewind(file);
            char *bytes = malloc(size);
            if (!bytes || fread(bytes, 1, size, file) != size) exit(1);
            ass_add_font(library, name, bytes, size);
            fclose(file);
            free(bytes);
        }
        free(entries[i]);
    }
    free(entries);
}

static uint64_t hash_byte(uint64_t hash, unsigned char value)
{
    return (hash ^ value) * UINT64_C(1099511628211);
}

/* Hash visible bitmap bytes and geometry in composition order, not pointers/padding. */
static uint64_t hash_image(uint64_t hash, ASS_Image *img)
{
    uint32_t geometry[] = {img->w, img->h, img->dst_x, img->dst_y, img->color};
    for (unsigned i = 0; i < sizeof(geometry) / sizeof(geometry[0]); i++)
        for (int shift = 0; shift < 32; shift += 8)
            hash = hash_byte(hash, geometry[i] >> shift);
    for (int y = 0; y < img->h; y++)
        for (int x = 0; x < img->w; x++)
            hash = hash_byte(hash, img->bitmap[(size_t) y * img->stride + x]);
    return hash;
}

int main(int argc, char **argv)
{
    if (argc < 7) {
        fprintf(stderr, "usage: %s track.ass fonts start_ms end_ms width height [hash]\n", argv[0]);
        return 1;
    }
    int start = atoi(argv[3]), end = atoi(argv[4]);
    int width = atoi(argv[5]), height = atoi(argv[6]);
    int hash = argc > 7 && !strcmp(argv[7], "hash");
    if (start < 0 || end <= start || width < 1 || height < 1) return 1;
    ASS_Library *library = ass_library_init();
    if (!library) return 1;
    ass_set_message_cb(library, message, NULL);
    load_fonts(library, argv[2]);
    ASS_Renderer *renderer = ass_renderer_init(library);
    if (!renderer) return 1;
    ass_set_frame_size(renderer, width, height);
    ass_set_storage_size(renderer, 1920, 1080);
    ass_set_fonts(renderer, NULL, "sans-serif", ASS_FONTPROVIDER_NONE, NULL, 1);
    ASS_Track *track = ass_read_file(library, argv[1], "UTF-8");
    if (!track) return 1;
    puts("pts_ms,render_ms,images,pixels,hash");
    for (int frame = 0; ; frame++) {
        long long pts = start + ((int64_t) frame * 1001000 + 12000) / 24000;
        if (pts >= end) break;
        int changed;
        uint64_t before = clock_ns();
        ASS_Image *images = ass_render_frame(renderer, track, pts, &changed);
        uint64_t elapsed = clock_ns() - before;
        size_t count = 0, pixels = 0;
        uint64_t digest = UINT64_C(14695981039346656037);
        for (ASS_Image *img = images; img; img = img->next) {
            count++;
            pixels += (size_t) img->w * img->h;
            if (hash) digest = hash_image(digest, img);
        }
        printf("%lld,%.6f,%zu,%zu,%016" PRIx64 "\n", pts,
               elapsed / 1000000.0, count, pixels, hash ? digest : 0);
    }
    ass_free_track(track);
    ass_renderer_done(renderer);
    ass_library_done(library);
    return 0;
}
