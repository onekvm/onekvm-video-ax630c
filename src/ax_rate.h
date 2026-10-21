/* SPDX-License-Identifier: MIT */
#ifndef AX_RATE_H
#define AX_RATE_H

#include <stddef.h>
#include <onekvm/video_backend_v1.h>
#include "vcenc_rc.h"

struct ax_rate {
    double quality;
    uint32_t bitrate_kbps, fps, initial_qp, min_qp, max_qp;
    struct vcenc_rc controller;
    int ready;
};

int ax_rate_configure(struct ax_rate *rate,
                      const struct onekvm_video_encoder_config_v1 *config);
int ax_rate_set_quality(struct ax_rate *rate, double quality);
uint32_t ax_rate_target_kbps(const struct ax_rate *rate, uint32_t w, uint32_t h,
                             uint32_t fps);
uint32_t ax_rate_prepare(struct ax_rate *rate, struct vcenc_rc *next,
                         uint32_t codec, uint32_t w, uint32_t h,
                         uint32_t input_fps, uint32_t gop, int idr);
void ax_rate_commit(struct ax_rate *rate, struct vcenc_rc *next, size_t bytes);

#endif
