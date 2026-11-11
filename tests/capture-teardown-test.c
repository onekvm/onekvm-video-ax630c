/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include <assert.h>
#include <stdarg.h>
#include "../src/ax630c_video.c"

static struct ax_source *current;
static unsigned pins, closes;

void __wrap_onekvm_v4l2_transform_destroy(struct onekvm_v4l2_transform *transform)
{
    if (!transform) return;
    /* The preview must have finished before dropping its imports. */
    assert(current->display_job_index == -1);
    assert(pins > 0);
    pins--;
}

int __wrap_ioctl(int fd, unsigned long request, ...)
{
    assert(fd == 123 && pins == 0);
    assert(request == VIDIOC_STREAMOFF || request == VIDIOC_REQBUFS);
    return 0;
}

int __wrap_close(int fd)
{
    assert(fd == 123 && pins == 0);
    closes++;
    return 0;
}

int main(void)
{
    struct ax_source src;
    ax_source_init(&src);
    current = &src;
    /* Both preview and full-resolution snapshots can retain an entire old
     * VIN pool. Model their references after the conversion has completed. */
    for (int failed_open = 0; failed_open <= 1; failed_open++) {
        src.display_hw.transform = (struct onekvm_v4l2_transform *)(uintptr_t)1;
        src.snapshot_hw.transform = (struct onekvm_v4l2_transform *)(uintptr_t)2;
        pins = 2;
        src.display_job_index = 0;
        src.display_job_complete = 1;
        src.cap_fd = failed_open ? -1 : 123;
        src.streaming = !failed_open;
        src.nbufs = failed_open ? 0 : AX_MAX_BUFS;
        ax_capture_stop(&src);
        assert(pins == 0 && !src.display_hw.transform && !src.snapshot_hw.transform);
        assert(src.cap_fd == -1 && !src.streaming && src.nbufs == 0);
        ax_capture_stop(&src);  /* Repeated teardown must be harmless. */
    }
    assert(closes == 1);

    /* The encoder's TDP scaler also caches imports of the whole ring and is
     * used outside src->lock: the encoder thread drops it before VIN frees
     * the old pool for a same-size restart or a new mode. */
    struct ax_encoder enc = { .venc_fd = -1 };
    enc.scale_hw.transform = (struct onekvm_v4l2_transform *)(uintptr_t)3;
    pins = 1;
    src.cap_fd = 123;
    src.streaming = 1;
    src.nbufs = AX_MAX_BUFS;
    snprintf(src.device, sizeof(src.device), "/nonexistent/onekvm-vin");
    assert(ax_encoder_capture_start(&enc, &src, 2560, 1440, NULL, 0) != 0);
    assert(pins == 0 && !enc.scale_hw.transform && src.cap_fd == -1);
    assert(closes == 2);
    puts("capture teardown: drain previews, release all DMA-BUF borrowers (preview, snapshot, encoder scaler) before VIN, failed-open cleanup PASS");
    return 0;
}
