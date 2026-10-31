/* SPDX-License-Identifier: MIT */
#include "../src/ax_no_signal.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/stat.h>

static void write_nv21(const char *path, unsigned width, unsigned height)
{
    size_t luma = (size_t)width * height;
    FILE *file = fopen(path, "wb");
    uint8_t *frame = calloc(1, luma * 3 / 2);
    size_t i;

    assert(file != NULL && frame != NULL);
    for (i = 0; i < luma; i++)
        frame[i] = 220;
    memset(frame + luma, 128, luma / 2);
    assert(fwrite(frame, 1, luma * 3 / 2, file) == luma * 3 / 2);
    fclose(file);
    free(frame);
}

static void seed_cache(void)
{
    char root[256];
    char dir[320];
    char path[384];
    char link[320];

    snprintf(root, sizeof(root), "/tmp/onekvm-ax-nosig-%ld", (long)getpid());
    snprintf(dir, sizeof(dir), "%s/en", root);
    assert(mkdir(root, 0700) == 0);
    assert(mkdir(dir, 0700) == 0);
    snprintf(path, sizeof(path), "%s/1920x1080.nv21", dir);
    write_nv21(path, 1920, 1080);
    snprintf(path, sizeof(path), "%s/1280x720.nv21", dir);
    write_nv21(path, 1280, 720);
    snprintf(path, sizeof(path), "%s/640x480.nv21", dir);
    write_nv21(path, 640, 480);
    snprintf(link, sizeof(link), "%s/current", root);
    assert(symlink("en", link) == 0);
    assert(setenv("ONEKVM_NO_SIGNAL_CACHE", root, 1) == 0);
}

int main(void)
{
    static const unsigned sizes[][2] = {
        {1920, 1080}, {1280, 720}, {640, 480}, {1024, 768}, {3840, 2160},
    };
    size_t i;

    seed_cache();
    for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        unsigned width = sizes[i][0], height = sizes[i][1];
        size_t bytes = (size_t)width * height * 2;
        uint8_t *frame = malloc(bytes);
        size_t p;
        unsigned nonblack = 0;

        assert(frame != NULL);
        assert(ax_render_no_signal_yuyv(frame, bytes, width, height, width * 2) == 0);
        for (p = 0; p < bytes; p += 4)
            if (frame[p] != 16)
                nonblack++;
        assert(nonblack > 0);
        assert(ax_render_no_signal_yuyv(frame, bytes - 1, width, height, width * 2) != 0);
        free(frame);
    }
    return 0;
}
