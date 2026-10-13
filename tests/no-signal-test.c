/* SPDX-License-Identifier: MIT */
#include "../src/ax_no_signal.h"

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>

int main(void)
{
    static const unsigned sizes[][2] = {
        {1920, 1080}, {1280, 720}, {640, 480}, {1024, 768}, {3840, 2160},
    };
    size_t i;

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
