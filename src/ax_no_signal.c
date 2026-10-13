/* SPDX-License-Identifier: MIT */
#include "ax_no_signal.h"

#include <stdlib.h>
#include <string.h>

/* Generated from the approved MMF no-signal PNGs. Keep this file in sync with
 * onekvm-video-soph-mmf/src/no_signal_frames.inc. */
#include "no_signal_frames.inc"

static const no_signal_asset_t *nearest_asset(unsigned width, unsigned height)
{
    const no_signal_asset_t *best = NULL;
    unsigned long long best_distance = ~0ULL;
    size_t i;

    for (i = 0; i < sizeof(no_signal_assets) / sizeof(no_signal_assets[0]); i++) {
        const no_signal_asset_t *asset = &no_signal_assets[i];
        long long dw = (long long)asset->width - width;
        long long dh = (long long)asset->height - height;
        unsigned long long distance = (unsigned long long)(dw * dw + dh * dh);

        if (distance < best_distance) {
            best = asset;
            best_distance = distance;
        }
    }
    return best;
}

static int unpack(const no_signal_asset_t *asset, uint8_t *out, size_t size)
{
    size_t input = 0, output = 0;

    while (input < asset->packed_size) {
        uint8_t control = asset->packed[input++];
        size_t length;

        if (control & 0x80) {
            length = (control & 0x7f) + 3;
            if (input >= asset->packed_size || length > size - output)
                return -1;
            memset(out + output, asset->packed[input++], length);
        } else {
            length = control + 1;
            if (length > asset->packed_size - input || length > size - output)
                return -1;
            memcpy(out + output, asset->packed + input, length);
            input += length;
        }
        output += length;
    }
    return output == size ? 0 : -1;
}

int ax_render_no_signal_yuyv(uint8_t *dst, size_t capacity,
                             unsigned width, unsigned height, unsigned stride)
{
    const no_signal_asset_t *asset;
    uint8_t *nv21;
    size_t pixels, asset_size;
    unsigned fit_w, fit_h, x0, y0, y, x;

    if (!dst || !width || !height || (width & 1) || (height & 1) ||
        width > 4096 || height > 2160 || stride < width * 2 ||
        capacity < (size_t)stride * height)
        return -1;
    asset = nearest_asset(width, height);
    if (!asset)
        return -1;
    pixels = (size_t)asset->width * asset->height;
    asset_size = pixels + pixels / 2;
    nv21 = malloc(asset_size);
    if (!nv21)
        return -1;
    if (unpack(asset, nv21, asset_size) != 0) {
        free(nv21);
        return -1;
    }

    if ((unsigned long long)asset->width * height <=
        (unsigned long long)asset->height * width) {
        fit_h = height;
        fit_w = (unsigned)((unsigned long long)asset->width * height / asset->height) & ~1u;
    } else {
        fit_w = width;
        fit_h = (unsigned)((unsigned long long)asset->height * width / asset->width) & ~1u;
    }
    x0 = ((width - fit_w) / 2) & ~1u;
    y0 = ((height - fit_h) / 2) & ~1u;

    for (y = 0; y < height; y++) {
        uint8_t *row = dst + (size_t)y * stride;

        for (x = 0; x < width; x += 2) {
            uint8_t y0_value = 16, y1_value = 16, u = 128, v = 128;

            if (y >= y0 && y < y0 + fit_h && x >= x0 && x + 1 < x0 + fit_w) {
                unsigned sy = (unsigned)((unsigned long long)(y - y0) * asset->height / fit_h);
                unsigned sx0 = (unsigned)((unsigned long long)(x - x0) * asset->width / fit_w);
                unsigned sx1 = (unsigned)((unsigned long long)(x + 1 - x0) * asset->width / fit_w);
                size_t chroma = pixels + (size_t)(sy / 2) * asset->width + (sx0 & ~1u);

                y0_value = nv21[(size_t)sy * asset->width + sx0];
                y1_value = nv21[(size_t)sy * asset->width + sx1];
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
