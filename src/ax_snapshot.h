/* SPDX-License-Identifier: MIT */
#ifndef AX_SNAPSHOT_INCLUDED
#define AX_SNAPSHOT_INCLUDED
#include <stddef.h>
#include <stdint.h>

struct ax_jpeg;
/* Lazily created, reusable compressor; calls must be serialized. Input is a
 * cached little-endian RGB565 view whose storage stays valid through return. */
int ax_snapshot_jpeg(struct ax_jpeg **context, const uint8_t *rgb,
                     uint32_t width, uint32_t height, uint32_t stride,
                     double quality, uint8_t *out, size_t capacity, size_t *size);
void ax_snapshot_jpeg_destroy(struct ax_jpeg *context);
void ax_snapshot_expand_rgb565(uint8_t *out, const uint8_t *rgb, uint32_t width);
#endif
