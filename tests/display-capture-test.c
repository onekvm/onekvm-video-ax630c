/* SPDX-License-Identifier: MIT */
/* Exercise display-only capture with a fake completed VIN buffer; no encoder. */
#include <assert.h>
#include <stdarg.h>
#include "../src/ax630c_video.c"

static int pending, dequeues, requeues;
int __wrap_poll(struct pollfd *fds, nfds_t count, int timeout)
{
    (void)timeout;
    for (nfds_t i = 0; i < count; i++) fds[i].revents = 0;
    if (pending && count && fds[0].fd == 123) {
        fds[0].revents = POLLIN;
        return 1;
    }
    return 0;
}
int __wrap_ioctl(int fd, unsigned long request, ...)
{
    assert(fd == 123);
    va_list args; va_start(args, request);
    struct v4l2_buffer *buf = va_arg(args, struct v4l2_buffer *);
    va_end(args);
    if (request == VIDIOC_DQBUF) {
        assert(pending); pending--; buf->index = 0; dequeues++;
    } else {
        assert(request == VIDIOC_QBUF); requeues++;
    }
    return 0;
}
int main(void)
{
    struct ax_source src;
    ax_source_init(&src);
    uint8_t yuyv[] = {81, 90, 81, 240, 81, 90, 81, 240};
    src.streaming = 1; src.cap_fd = 123; src.nbufs = 3;
    src.width = src.height = 2; src.bytesperline = 4;
    src.map[0] = yuyv; src.map_len[0] = sizeof(yuyv);
    uint8_t *rgb = malloc(AX_DISPLAY_BYTES);
    assert(rgb);
    struct onekvm_video_display_request_v1 request = {
        .struct_size = sizeof(request), .width = 640, .height = 360,
        .pixel_format = ONEKVM_VIDEO_PIXEL_RGB565, .timeout_ms = 100,
    };
    struct onekvm_video_display_result_v1 result = {.struct_size = sizeof(result)};
    char error[128];
    pending = 3;
    assert(ax_source_display(&src, &request, rgb, AX_DISPLAY_BYTES,
                             &result, error, sizeof(error)) == 0);
    assert(dequeues == 3 && requeues == 3 && !src.held);
    size_t pixel = (50 * AX_DISPLAY_W + 200) * 2;
    uint16_t red = rgb[pixel] | ((uint16_t)rgb[pixel + 1] << 8);
    assert((red >> 11) >= 30 && ((red >> 5) & 63) <= 2 && (red & 31) <= 2);
    assert(result.width == 640 && result.height == 360);
    /* No new capture must time out instead of replaying the consumed frame. */
    request.timeout_ms = 5;
    uint64_t start = ax_now_ms();
    assert(ax_source_display(&src, &request, rgb, AX_DISPLAY_BYTES,
                             &result, error, sizeof(error)) == ONEKVM_VIDEO_RESOURCE_TIMEOUT);
    assert(ax_now_ms() - start < 30);
    /* The bound no-signal artwork remains available without encoded packets. */
    src.streaming = 0; src.display_placeholder = 1;
    assert(ax_source_display(&src, &request, rgb, AX_DISPLAY_BYTES,
                             &result, error, sizeof(error)) == 0);
    uint64_t pts = result.pts_ns;
    assert(ax_source_display(&src, &request, rgb, AX_DISPLAY_BYTES,
                             &result, error, sizeof(error)) == 0);
    assert(result.pts_ns > pts && dequeues == 3);
    atomic_store(&src.interrupted, 1);
    assert(ax_source_display(&src, &request, rgb, AX_DISPLAY_BYTES,
                             &result, error, sizeof(error)) == ONEKVM_VIDEO_RESOURCE_TIMEOUT);
    free(rgb);
    pthread_cond_destroy(&src.display_cv);
    pthread_mutex_destroy(&src.display_lock);
    pthread_mutex_destroy(&src.lock);
    return 0;
}
