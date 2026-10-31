/* SPDX-License-Identifier: MIT */
#include "ax_no_signal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/stat.h>

struct ax_frame_spec {
    const char *name;
    unsigned width;
    unsigned height;
};

static const struct ax_frame_spec ax_frames[] = {
    {"1920x1080", 1920, 1080},
    {"1280x720", 1280, 720},
    {"640x480", 640, 480},
};

static const char *ax_cache_root(void)
{
    const char *env = getenv("ONEKVM_NO_SIGNAL_CACHE");

    if (env && env[0])
        return env;
    return "/data/onekvm/cache/no-signal";
}

static const struct ax_frame_spec *ax_nearest_frame(unsigned width, unsigned height)
{
    const struct ax_frame_spec *best = NULL;
    unsigned long long best_distance = ~0ULL;
    size_t i;

    for (i = 0; i < sizeof(ax_frames) / sizeof(ax_frames[0]); i++) {
        long long dw = (long long)ax_frames[i].width - width;
        long long dh = (long long)ax_frames[i].height - height;
        unsigned long long distance = (unsigned long long)(dw * dw + dh * dh);

        if (distance < best_distance) {
            best = &ax_frames[i];
            best_distance = distance;
        }
    }
    return best;
}

static int ax_frame_path(const struct ax_frame_spec *frame, char *path, size_t path_size)
{
    int written = snprintf(path, path_size, "%s/current/%s.nv21", ax_cache_root(), frame->name);

    return written > 0 && (size_t)written < path_size ? 0 : -1;
}

void ax_no_signal_token(unsigned width, unsigned height, struct ax_no_signal_token *token)
{
    const struct ax_frame_spec *frame;
    char path[512];
    struct stat info;

    if (!token)
        return;
    memset(token, 0, sizeof(*token));
    frame = ax_nearest_frame(width, height);
    if (!frame || ax_frame_path(frame, path, sizeof(path)) != 0)
        return;
    if (stat(path, &info) != 0)
        return;
    token->device = (uint64_t)info.st_dev;
    token->inode = (uint64_t)info.st_ino;
    token->mtime_sec = (int64_t)info.st_mtim.tv_sec;
    token->mtime_nsec = (int64_t)info.st_mtim.tv_nsec;
}

static uint8_t *ax_load_nv21(const struct ax_frame_spec *frame)
{
    char path[512];
    FILE *file;
    size_t pixels, expect, read_bytes;
    uint8_t *nv21;
    unsigned char extra;

    if (ax_frame_path(frame, path, sizeof(path)) != 0)
        return NULL;
    pixels = (size_t)frame->width * frame->height;
    expect = pixels + pixels / 2;
    file = fopen(path, "rb");
    if (!file)
        return NULL;
    nv21 = malloc(expect);
    if (!nv21) {
        fclose(file);
        return NULL;
    }
    read_bytes = fread(nv21, 1, expect, file);
    extra = 0;
    if (read_bytes != expect || fread(&extra, 1, 1, file) == 1) {
        free(nv21);
        nv21 = NULL;
    }
    fclose(file);
    return nv21;
}

static void ax_fill_black_yuyv(uint8_t *dst, unsigned width, unsigned height, unsigned stride)
{
    unsigned y, x;

    for (y = 0; y < height; y++) {
        uint8_t *row = dst + (size_t)y * stride;

        for (x = 0; x < width; x += 2) {
            row[x * 2] = 16;
            row[x * 2 + 1] = 128;
            row[x * 2 + 2] = 16;
            row[x * 2 + 3] = 128;
        }
    }
}

int ax_render_no_signal_yuyv(uint8_t *dst, size_t capacity,
                             unsigned width, unsigned height, unsigned stride)
{
    const struct ax_frame_spec *frame;
    uint8_t *nv21 = NULL;
    size_t pixels = 0;
    unsigned src_w = 0, src_h = 0;
    unsigned fit_w, fit_h, x0, y0, y, x;

    if (!dst || !width || !height || (width & 1) || (height & 1) ||
        width > 4096 || height > 2160 || stride < width * 2 ||
        capacity < (size_t)stride * height)
        return -1;
    frame = ax_nearest_frame(width, height);
    if (frame)
        nv21 = ax_load_nv21(frame);
    if (!nv21) {
        ax_fill_black_yuyv(dst, width, height, stride);
        return 0;
    }
    src_w = frame->width;
    src_h = frame->height;
    pixels = (size_t)src_w * src_h;

    if ((unsigned long long)src_w * height <= (unsigned long long)src_h * width) {
        fit_h = height;
        fit_w = (unsigned)((unsigned long long)src_w * height / src_h) & ~1u;
    } else {
        fit_w = width;
        fit_h = (unsigned)((unsigned long long)src_h * width / src_w) & ~1u;
    }
    if (fit_w < 2 || fit_h < 2) {
        free(nv21);
        ax_fill_black_yuyv(dst, width, height, stride);
        return 0;
    }
    x0 = ((width - fit_w) / 2) & ~1u;
    y0 = ((height - fit_h) / 2) & ~1u;

    for (y = 0; y < height; y++) {
        uint8_t *row = dst + (size_t)y * stride;

        for (x = 0; x < width; x += 2) {
            uint8_t y0_value = 16, y1_value = 16, u = 128, v = 128;

            if (y >= y0 && y < y0 + fit_h && x >= x0 && x + 1 < x0 + fit_w) {
                unsigned sy = (unsigned)((unsigned long long)(y - y0) * src_h / fit_h);
                unsigned sx0 = (unsigned)((unsigned long long)(x - x0) * src_w / fit_w);
                unsigned sx1 = (unsigned)((unsigned long long)(x + 1 - x0) * src_w / fit_w);
                size_t chroma = pixels + (size_t)(sy / 2) * src_w + (sx0 & ~1u);

                y0_value = nv21[(size_t)sy * src_w + sx0];
                y1_value = nv21[(size_t)sy * src_w + sx1];
                v = nv21[chroma];
                u = nv21[chroma + 1];
            }
            row[x * 2] = y0_value;
            row[x * 2 + 1] = u;
            row[x * 2 + 2] = y1_value;
            row[x * 2 + 3] = v;
        }
    }
    free(nv21);
    return 0;
}
