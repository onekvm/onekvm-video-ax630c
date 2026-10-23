/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../src/ax_snapshot.h"

/* Unaligned input, every RGB565 value and vector tails; run on target too. */
static uint8_t input[65536 * 2 + 1], output[65536 * 3 + 2];
static void check(uint32_t width)
{
    memset(output, 0xa5, sizeof(output));
    ax_snapshot_expand_rgb565(output + 1, input + 1, width);
    for (uint32_t x = 0; x < width; x++) {
        unsigned p = input[x*2+1] | (input[x*2+2] << 8);
        unsigned r = p >> 11, g = (p >> 5) & 63, b = p & 31;
        assert(output[x*3+1] == ((r << 3) | (r >> 2)));
        assert(output[x*3+2] == ((g << 2) | (g >> 4)));
        assert(output[x*3+3] == ((b << 3) | (b >> 2)));
    }
    assert(output[0] == 0xa5 && output[width*3+1] == 0xa5);
}
int main(void)
{
    for (unsigned p = 0; p < 65536; p++) {
        input[p*2+1] = p; input[p*2+2] = p >> 8;
    }
    check(65536);
    for (unsigned w = 1; w <= 17; w++) check(w);
#if defined(__aarch64__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    puts("RGB565 NEON: all 65536 colours, unaligned input/output, vector tails PASS");
#else
    puts("RGB565 scalar: all 65536 colours, unaligned input/output, tails PASS");
#endif
}
