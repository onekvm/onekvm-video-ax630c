/* SPDX-License-Identifier: MIT */
#ifndef AX_TDP_INCLUDED
#define AX_TDP_INCLUDED
#include <stddef.h>
#include <stdint.h>
#include <onekvm/v4l2_transform.h>

struct ax_tdp {
    struct onekvm_v4l2_transform *transform;
};

void ax_tdp_init(struct ax_tdp *tdp);
void ax_tdp_close(struct ax_tdp *tdp);
/* Fit and centre a DMA-BUF input in the output. A hardware error is returned
 * as a negative errno; this function never performs software conversion. */
int ax_tdp_convert(struct ax_tdp *tdp, uint8_t *out, uint32_t dw, uint32_t dh,
                   int input_fd, uint32_t stride, uint32_t sw, uint32_t sh);
/* Full-size snapshot: cached output is borrowed until release. The caller
 * must serialize acquire/release and keep the transform alive while borrowed. */
int ax_tdp_snapshot_acquire(struct ax_tdp *tdp, struct onekvm_v4l2_transform_frame *frame,
                            uint32_t dw, uint32_t dh,
                    int input_fd, uint32_t stride, uint32_t sw, uint32_t sh);
void ax_tdp_snapshot_release(struct ax_tdp *tdp);
/* Hardware scale-only: packed YUYV in, packed YUYV out, no colour conversion.
 * The output stays owned by the transform until ax_tdp_snapshot_release().
 * *recreated is set when the transform was (re)built, i.e. any earlier export
 * or import of its output is stale. */
int ax_tdp_scale_yuyv(struct ax_tdp *tdp, struct onekvm_v4l2_transform_frame *frame,
                      uint32_t dw, uint32_t dh, int input_fd, uint32_t stride,
                      uint32_t sw, uint32_t sh, int *recreated);
int ax_tdp_export_output(struct ax_tdp *tdp, int *dma_buf_fd);
#endif
