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
#endif
