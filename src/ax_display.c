/* SPDX-License-Identifier: MIT */
#include "ax_display.h"
#include <string.h>

enum { MAX_DISPLAY_DIMENSION = 8192 };

static uint32_t clip(int value)
{
    return value < 0 ? 0u : value > 255 ? 255u : (uint32_t)value;
}

int ax_display_convert(uint8_t *out, uint32_t dw, uint32_t dh,
                       const uint8_t *in, size_t size, uint32_t stride,
                       uint32_t sw, uint32_t sh)
{
    uint32_t w, h, left, top;
    uint8_t cached_row[MAX_DISPLAY_DIMENSION * 2];
    uint16_t source_x[MAX_DISPLAY_DIMENSION];
    uint32_t cached_y = UINT32_MAX;
    if (!out || !in || !dw || !dh || !sw || !sh || (sw & 1u) ||
        sw > MAX_DISPLAY_DIMENSION || sh > MAX_DISPLAY_DIMENSION ||
        dw > MAX_DISPLAY_DIMENSION || dh > MAX_DISPLAY_DIMENSION ||
        stride < sw * 2u || (uint64_t)(sh - 1u) * stride + sw * 2u > size)
        return -1;
    if ((uint64_t)sw * dh > (uint64_t)sh * dw) {
        w = dw; h = (uint32_t)((uint64_t)sh * dw / sw);
    } else {
        h = dh; w = (uint32_t)((uint64_t)sw * dh / sh);
    }
    if (!w || !h) return -1;
    left = (dw - w) / 2u; top = (dh - h) / 2u;
    memset(out, 0, (size_t)dw * dh * 2u);
    for (uint32_t x = 0; x < w; x++)
        source_x[x] = (uint16_t)((uint64_t)x * sw / w);
    for (uint32_t y = 0; y < h; y++) {
        uint32_t sy = (uint32_t)((uint64_t)y * sh / h);
        /* VIN's coherent DMA mapping is uncached. Repeated byte loads from
         * it serialize bus transactions and stall the primary encoder. Copy
         * each sampled row in bursts, then do the pixel work in cached RAM.
         * Do not read stride padding: the final row need not include it. */
        if (cached_y != sy) {
            memcpy(cached_row, in + (size_t)sy * stride, (size_t)sw * 2u);
            cached_y = sy;
        }
        for (uint32_t x = 0; x < w; x++) {
            uint32_t sx = source_x[x];
            const uint8_t *p = cached_row + (sx & ~1u) * 2u;
            int c = (int)p[(sx & 1u) * 2u] - 16;
            int u = (int)p[1] - 128, v = (int)p[3] - 128;
            /* Match the bridge's limited-range BT.709 encoder input. */
            uint32_t r = clip((298 * c + 459 * v + 128) >> 8);
            uint32_t g = clip((298 * c - 55 * u - 136 * v + 128) >> 8);
            uint32_t b = clip((298 * c + 541 * u + 128) >> 8);
            uint16_t rgb = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
            size_t offset = ((size_t)(top + y) * dw + left + x) * 2u;
            out[offset] = (uint8_t)rgb;
            out[offset + 1] = (uint8_t)(rgb >> 8);
        }
    }
    return 0;
}
