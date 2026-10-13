/* SPDX-License-Identifier: MIT */
#ifndef AX_NO_SIGNAL_H
#define AX_NO_SIGNAL_H

#include <stddef.h>
#include <stdint.h>

/* Render the MMF no-signal artwork into the VC8000E's packed YUYV input. */
int ax_render_no_signal_yuyv(uint8_t *dst, size_t capacity,
                             unsigned width, unsigned height, unsigned stride);

#endif
