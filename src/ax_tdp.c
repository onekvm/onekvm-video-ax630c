/* SPDX-License-Identifier: MIT */
#include "ax_tdp.h"
#include <linux/videodev2.h>
#include <errno.h>
#include <string.h>

void ax_tdp_init(struct ax_tdp *tdp)
{
    *tdp = (struct ax_tdp){0};
}

void ax_tdp_close(struct ax_tdp *tdp)
{
    onekvm_v4l2_transform_destroy(tdp->transform);
    ax_tdp_init(tdp);
}

int ax_tdp_convert(struct ax_tdp *tdp, uint8_t *out, uint32_t dw, uint32_t dh,
                   int input_fd, uint32_t stride, uint32_t sw, uint32_t sh)
{
    uint32_t w, h;
    int rc;
    if (!out || !dw || !dh || dw > 640 || dh > 360 ||
        !sw || !sh || sw > 4096 || sh > 2400 || input_fd < 0)
        return -EINVAL;
    if ((uint64_t)sw * dh > (uint64_t)sh * dw) {
        w = dw; h = (uint32_t)((uint64_t)sh * dw / sw);
    } else {
        h = dh; w = (uint32_t)((uint64_t)sw * dh / sh);
    }
    w &= ~1u;
    if (!w || !h)
        return -EINVAL;
    struct onekvm_v4l2_transform_config config = {
        .width = sw, .height = sh, .stride = stride, .pixel_format = V4L2_PIX_FMT_YUYV,
        .crop_width = sw, .crop_height = sh, .output_width = w, .output_height = h,
        .colorspace = V4L2_COLORSPACE_REC709, .ycbcr_enc = V4L2_YCBCR_ENC_709,
        .quantization = V4L2_QUANTIZATION_LIM_RANGE, .xfer_func = V4L2_XFER_FUNC_709,
    };
    if (!onekvm_v4l2_transform_matches(tdp->transform, &config)) {
        ax_tdp_close(tdp);
        rc = onekvm_v4l2_transform_create("/dev/onekvm-vproc", &config, &tdp->transform);
        if (rc) return rc;
    }
    rc = onekvm_v4l2_transform_submit(tdp->transform, input_fd, 0);
    if (rc) { ax_tdp_close(tdp); return rc; }
    struct onekvm_v4l2_transform_frame frame;
    rc = onekvm_v4l2_transform_receive(tdp->transform, &frame, 250);
    if (rc) { ax_tdp_close(tdp); return rc; }
    uint32_t left = (dw - w) / 2u, top = (dh - h) / 2u;
    memset(out, 0, (size_t)dw * dh * 2u);
    for (uint32_t y = 0; y < h; y++)
        memcpy(out + ((size_t)(top + y) * dw + left) * 2u,
               frame.data + (size_t)y * frame.stride, (size_t)w * 2u);
    onekvm_v4l2_transform_release(tdp->transform);
    return 0;
}
