/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ax_rate.h"
#include "vcenc_encode.h"

static struct onekvm_video_encoder_config_v1 config(void)
{
    return (struct onekvm_video_encoder_config_v1) {
        .struct_size = sizeof(struct onekvm_video_encoder_config_v1),
        .codec = ONEKVM_VIDEO_CODEC_H265, .quality_factor = 0.5, .fps = 60,
        .initial_qp = 22, .min_qp = 12, .max_qp = 48,
    };
}

static void bounds_and_feedback(void)
{
    for (unsigned codec = 0; codec < 2; codec++) {
        struct ax_rate rate;
        struct vcenc_rc next;
        struct onekvm_video_encoder_config_v1 c = config();
        c.min_qp = 30;
        c.max_qp = 36;
        assert(ax_rate_configure(&rate, &c) == 0);
        assert(rate.initial_qp == 30);
        for (unsigned i = 0; i < 600; i++) {
            unsigned qp = ax_rate_prepare(&rate, &next, codec, 1920, 1080, 60, 30, i % 30 == 0);
            assert(qp >= 30 && qp <= 36);
            /* A rich scene must reach the configured ceiling. */
            if (i == 29)
                assert(qp == 36);
            if (i == 599)
                assert(qp == 30);
            ax_rate_commit(&rate, &next, i < 300 ? 500000 : 32);
        }
        assert(rate.controller.frames == 600);
    }
}

static void config_and_retarget(void)
{
    struct ax_rate rate;
    struct vcenc_rc next, retry;
    struct onekvm_video_encoder_config_v1 c = config();
    assert(ax_rate_configure(&rate, &c) == 0);
    assert(rate.min_qp == 16 && rate.max_qp == 48 && rate.initial_qp == 22);
    assert(ax_rate_target_kbps(&rate, 1920, 1080, 60) == 5000);
    assert(ax_rate_target_kbps(&rate, 3840, 2160, 30) == 10000);
    assert(ax_rate_set_quality(&rate, 0.1) == 0);
    assert(ax_rate_target_kbps(&rate, 1920, 1080, 60) == 1000);
    assert(ax_rate_prepare(&rate, &next, 1, 1920, 1080, 60, 30, 1) == 22);
    /* An encode failure must not advance feedback or the GOP's QP state. */
    assert(ax_rate_prepare(&rate, &retry, 1, 1920, 1080, 60, 30, 1) == 22);
    assert(rate.controller.frames == 0 && memcmp(&next, &retry, sizeof(next)) == 0);
    ax_rate_commit(&rate, &next, 25000);
    assert(ax_rate_set_quality(&rate, 0.8) == 0);
    ax_rate_prepare(&rate, &next, 1, 1920, 1080, 30, 30, 0);
    assert(rate.controller.frames == 1 && rate.controller.bitrate == 4000000);
    assert(rate.controller.fps == 30);
    ax_rate_commit(&rate, &next, 1000);
    ax_rate_prepare(&rate, &next, 1, 1280, 720, 60, 30, 1);
    assert(next.frames == 0 && next.w == 1280 && next.h == 720);

    c.bitrate_kbps = 2400;
    assert(ax_rate_configure(&rate, &c) == 0);
    ax_rate_set_quality(&rate, 0.01);
    assert(ax_rate_target_kbps(&rate, 3840, 2160, 30) == 2400);
    c.struct_size = offsetof(struct onekvm_video_encoder_config_v1, bitrate_kbps);
    assert(ax_rate_configure(&rate, &c) == 0 && rate.bitrate_kbps == 0);
    assert(rate.min_qp == 16 && rate.max_qp == 51 && rate.initial_qp == 32);
    c = config();
    c.max_qp = 12;
    assert(ax_rate_configure(&rate, &c) != 0);
    c = config();
    c.min_qp = 40;
    c.max_qp = 30;
    assert(ax_rate_configure(&rate, &c) != 0);
    c = config();
    c.quality_factor = NAN;
    assert(ax_rate_configure(&rate, &c) != 0);
    assert(ax_rate_set_quality(&rate, INFINITY) != 0);
    assert(ax_rate_set_quality(&rate, -0.1) != 0);
}

static void pps_and_frame_qp_are_independent(void)
{
    vcenc_geom geom;
    uint32_t words[ENC_CMDBUF_WORDS];
    assert(vcenc_geom_build(&geom, 1920, 1080) == 0);
    for (unsigned codec = 0; codec < 2; codec++) {
        struct vcenc_frame frame = {
            .frame_num = 1, .qp = 38, .pic_init_qp = 22,
            .rc_mode = ENC_RC_FIXQP, .codec = codec,
        };
        assert(vcenc_build_encode_cmdbuf(words, 0x50000000, &geom, 0x60000000, &frame));
        uint32_t sw7 = words[5 + 7 - 1];
        assert((sw7 >> 26) == 22 && ((sw7 >> 8) & 63) == 38);
        assert((words[5 + 6 - 1] & 1) == 0);
    }
}

int main(void)
{
    bounds_and_feedback();
    config_and_retarget();
    pps_and_frame_qp_are_independent();
    puts("AX630C rate control: PASS");
    return 0;
}
