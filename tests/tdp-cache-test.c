/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "../src/ax_tdp.h"

struct onekvm_v4l2_transform { struct onekvm_v4l2_transform_config config; };
static struct onekvm_v4l2_transform transform;
static uint8_t pixels[(640 * 2 + 16) * 360];
static int creates, borrowed;
int onekvm_v4l2_transform_create(const char *device,
    const struct onekvm_v4l2_transform_config *config, struct onekvm_v4l2_transform **out)
{
    (void)device; assert(!borrowed); transform.config = *config; *out = &transform; creates++; return 0;
}
void onekvm_v4l2_transform_destroy(struct onekvm_v4l2_transform *t) { (void)t; assert(!borrowed); }
int onekvm_v4l2_transform_matches(const struct onekvm_v4l2_transform *t,
    const struct onekvm_v4l2_transform_config *config)
{
    return t && !memcmp(&t->config, config, sizeof(*config));
}
int onekvm_v4l2_transform_submit(struct onekvm_v4l2_transform *t, int fd, uint32_t offset)
{
    (void)t; (void)fd; (void)offset; assert(!borrowed); return 0;
}
int onekvm_v4l2_transform_receive(struct onekvm_v4l2_transform *t,
    struct onekvm_v4l2_transform_frame *frame, uint32_t timeout)
{
    (void)timeout;
    uint32_t w = t->config.output_width, h = t->config.output_height, stride = w * 2 + 16;
    assert(stride * h <= sizeof(pixels));
    memset(pixels, 0xa5, sizeof(pixels));
    for (uint32_t y = 0; y < h; y++) memset(pixels + y * stride, 0x39, w * 2);
    *frame = (struct onekvm_v4l2_transform_frame){pixels, w, h, stride, stride * h};
    borrowed = 1; return 0;
}
void onekvm_v4l2_transform_release(struct onekvm_v4l2_transform *t)
{
    (void)t; assert(borrowed); borrowed = 0;
}
static void check_display(struct ax_tdp *tdp, uint32_t sw, uint32_t sh)
{
    uint8_t out[640 * 360 * 2 + 2]; memset(out, 0xa5, sizeof(out));
    assert(!ax_tdp_convert(tdp, out + 1, 640, 360, 123, sw * 2, sw, sh));
    assert(!borrowed && out[0] == 0xa5 && out[sizeof(out)-1] == 0xa5);
    uint32_t w = transform.config.output_width, h = transform.config.output_height;
    uint32_t left = (640 - w) / 2, top = (360 - h) / 2;
    for (uint32_t y = 0; y < 360; y++)
        for (uint32_t x = 0; x < 640; x++) {
            uint8_t expected = y >= top && y < top+h && x >= left && x < left+w ? 0x39 : 0;
            assert(out[1+(y*640+x)*2] == expected && out[2+(y*640+x)*2] == expected);
        }
}
int main(void)
{
    struct ax_tdp tdp; ax_tdp_init(&tdp);
    check_display(&tdp, 1920, 1080); check_display(&tdp, 1920, 1080); assert(creates == 1);
    check_display(&tdp, 640, 480); check_display(&tdp, 1920, 480);
    check_display(&tdp, 642, 1080); /* asymmetric pillarbox after even-width rounding */
    struct onekvm_v4l2_transform_frame frame;
    assert(!ax_tdp_snapshot_acquire(&tdp, &frame, 320, 240, 123, 3840, 1920, 1080));
    assert(borrowed && frame.data == pixels && frame.stride == 656);
    assert(frame.data[0] == 0x39 && frame.data[640] == 0xa5);
    ax_tdp_snapshot_release(&tdp); assert(!borrowed);
    ax_tdp_close(&tdp);
    puts("TDP cache: reused mapping, strided borrowed snapshot, exact output and letterbox boundaries PASS");
}
