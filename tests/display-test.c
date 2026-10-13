/* SPDX-License-Identifier: MIT */
#include "ax_display.h"
#include <assert.h>
#include <string.h>

static uint16_t pixel(const uint8_t *out, unsigned x, unsigned y, unsigned w)
{
    size_t offset = (y * w + x) * 2;
    return (uint16_t)(out[offset] | out[offset + 1] << 8);
}

int main(void)
{
    /* Limited-range black/white and red; padding must not become pixels. */
    const uint8_t bw[] = {16, 128, 235, 128, 255, 255, 255, 255};
    uint8_t out[4 * 4 * 2];
    memset(out, 0xff, sizeof(out));
    assert(ax_display_convert(out, 4, 4, bw, sizeof(bw), 8, 2, 1) == 0);
    for (unsigned x = 0; x < 4; x++) {
        assert(pixel(out, x, 0, 4) == 0);
        assert(pixel(out, x, 3, 4) == 0);
    }
    assert(pixel(out, 0, 1, 4) == 0);
    assert(pixel(out, 3, 2, 4) == 0xffff);
    const uint8_t red[] = {81, 90, 81, 240};
    assert(ax_display_convert(out, 2, 1, red, sizeof(red), 4, 2, 1) == 0);
    assert((pixel(out, 0, 0, 2) & 0xf800) == 0xf800);
    assert((pixel(out, 0, 0, 2) & 0x07ff) == 0);
    /* Reject a short last row, an invalid stride, and incomplete YUYV pairs. */
    assert(ax_display_convert(out, 2, 2, bw, 11, 8, 2, 2) != 0);
    assert(ax_display_convert(out, 2, 2, bw, sizeof(bw), 3, 2, 1) != 0);
    assert(ax_display_convert(out, 2, 2, bw, sizeof(bw), 8, 3, 1) != 0);
    return 0;
}
