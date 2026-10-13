/* SPDX-License-Identifier: MIT */
#ifndef AX_DISPLAY_INCLUDED
#define AX_DISPLAY_INCLUDED
#include <stddef.h>
#include <stdint.h>

#define AX_DISPLAY_W 640u
#define AX_DISPLAY_H 360u
#define AX_DISPLAY_BYTES (AX_DISPLAY_W * AX_DISPLAY_H * 2u)

/* Packed YUYV -> little-endian RGB565, fit inside the destination with black
 * bars. Validate the capture extent before reading a dequeued DMA buffer. */
int ax_display_convert(uint8_t *out, uint32_t dw, uint32_t dh,
                       const uint8_t *in, size_t size, uint32_t stride,
                       uint32_t sw, uint32_t sh);
#endif
