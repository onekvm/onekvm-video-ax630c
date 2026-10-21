/* SPDX-License-Identifier: MIT */
#include "ax_rate.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

int ax_rate_set_quality(struct ax_rate *rate, double quality)
{
    if (!rate || !isfinite(quality) || quality < 0 || quality > 1)
        return -1;
    rate->quality = quality;
    return 0;
}

int ax_rate_configure(struct ax_rate *rate,
                      const struct onekvm_video_encoder_config_v1 *config)
{
    struct onekvm_video_encoder_config_v1 c = {0};
    struct ax_rate next = {0};
    const size_t base = offsetof(struct onekvm_video_encoder_config_v1, bitrate_kbps);

    if (!rate || !config || config->struct_size < base)
        return -1;
    memcpy(&c, config, base);
    /* Optional fields may be absent on older ABI callers. */
#define COPY_FIELD(field) \
    if (config->struct_size >= offsetof(struct onekvm_video_encoder_config_v1, field) + sizeof(c.field)) \
        c.field = config->field
    COPY_FIELD(bitrate_kbps);
    COPY_FIELD(initial_qp);
    COPY_FIELD(min_qp);
    COPY_FIELD(max_qp);
#undef COPY_FIELD
    if (ax_rate_set_quality(&next, c.quality_factor) ||
        (c.bitrate_kbps && (c.bitrate_kbps < 100 || c.bitrate_kbps > 50000)) ||
        c.initial_qp < 0 || c.initial_qp > 51 ||
        c.min_qp < 0 || c.min_qp > 51 || c.max_qp < 0 || c.max_qp > 51 ||
        (c.min_qp && c.max_qp && c.min_qp > c.max_qp) || c.fps < 0 || c.fps > 240)
        return -1;
    next.min_qp = c.min_qp > (int)VCENC_QP_MIN ? (uint32_t)c.min_qp : VCENC_QP_MIN;
    next.max_qp = c.max_qp ? (uint32_t)c.max_qp : VCENC_QP_MAX;
    if (next.min_qp > next.max_qp)
        return -1;
    next.initial_qp = vcenc_rc_clampu(c.initial_qp ? c.initial_qp : 32,
                                     next.min_qp, next.max_qp);
    next.bitrate_kbps = (uint32_t)c.bitrate_kbps;
    next.fps = c.fps ? (uint32_t)c.fps : 60;
    *rate = next;
    return 0;
}

uint32_t ax_rate_target_kbps(const struct ax_rate *rate, uint32_t w, uint32_t h,
                             uint32_t fps)
{
    double target;

    if (rate->bitrate_kbps)
        return rate->bitrate_kbps;
    /* 1080p60 at full quality targets 10 Mbps; scale bits per pixel/frame. */
    target = (double)w * h * 10000.0 / (1920.0 * 1080.0) *
             rate->quality * fps / 60.0;
    if (target < 100)
        return 100;
    if (target > 50000)
        return 50000;
    return (uint32_t)target;
}

uint32_t ax_rate_prepare(struct ax_rate *rate, struct vcenc_rc *next,
                         uint32_t codec, uint32_t w, uint32_t h,
                         uint32_t input_fps, uint32_t gop, int idr)
{
    struct vcenc_rc *rc = &rate->controller;
    uint32_t fps = input_fps && input_fps < rate->fps ? input_fps : rate->fps;
    double target = (double)ax_rate_target_kbps(rate, w, h, fps) * 1000.0;

    if (!rate->ready || rc->codec != codec || rc->w != w || rc->h != h) {
        vcenc_rc_init(rc, codec, w, h, fps, target, gop, VCENC_RC_CBR);
        rc->qp_min = rate->min_qp;
        rc->qp_max = rate->max_qp;
        rc->qp_seed = rc->qp_i = rc->qp_last = rate->initial_qp;
        rate->ready = 1;
    } else {
        vcenc_rc_set_target(rc, target, fps);
        vcenc_rc_set_gop(rc, gop);
    }
    /* Commit feedback only after a complete, validated access unit. */
    *next = *rc;
    return vcenc_rc_pick_qp(next, idr);
}

void ax_rate_commit(struct ax_rate *rate, struct vcenc_rc *next, size_t bytes)
{
    vcenc_rc_update(next, (double)bytes * 8.0, next->pending_qp);
    rate->controller = *next;
}
