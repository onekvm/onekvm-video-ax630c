/* SPDX-License-Identifier: MIT */
#ifndef AX_NO_SIGNAL_H
#define AX_NO_SIGNAL_H

#include <stddef.h>
#include <stdint.h>

/* Identity of the NV21 file behind current/<WxH>.nv21. Zero when it is absent. */
struct ax_no_signal_token {
    uint64_t device;
    uint64_t inode;
    int64_t mtime_sec;
    int64_t mtime_nsec;
};

void ax_no_signal_token(unsigned width, unsigned height, struct ax_no_signal_token *token);

/* Render the cached no-signal NV21 into the VC8000E's packed YUYV input.
 * A missing cache becomes a black frame. */
int ax_render_no_signal_yuyv(uint8_t *dst, size_t capacity,
                             unsigned width, unsigned height, unsigned stride);

#endif
