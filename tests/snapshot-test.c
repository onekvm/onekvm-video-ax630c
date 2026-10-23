/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include <assert.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <jpeglib.h>
#include "../src/ax630c_video.c"

static int dequeues, requeues, conversion_error, borrowed, released;
static uint8_t converted[(1920 * 2 + 16) * 1080];
static atomic_int conversion_started, conversion_hold;
int __wrap_poll(struct pollfd *fds, nfds_t count, int timeout)
{
    (void)timeout;
    for (nfds_t i = 0; i < count; i++) fds[i].revents = 0;
    if (count && fds[0].fd == 123) { fds[0].revents = POLLIN; return 1; }
    return 0;
}
int __wrap_ioctl(int fd, unsigned long request, ...)
{
    assert(fd == 123);
    va_list args; va_start(args, request);
    void *argument = va_arg(args, void *); va_end(args);
    if (request == VIDIOC_EXPBUF) {
        ((struct v4l2_exportbuffer *)argument)->fd = dup(STDIN_FILENO);
        return 0;
    }
    if (request == VIDIOC_DQBUF) {
        ((struct v4l2_buffer *)argument)->index = 0; dequeues++;
    } else { assert(request == VIDIOC_QBUF); requeues++; }
    return 0;
}
int __wrap_ax_tdp_snapshot_acquire(struct ax_tdp *tdp, struct onekvm_v4l2_transform_frame *frame,
                                   uint32_t dw, uint32_t dh,
                           int fd, uint32_t stride, uint32_t sw, uint32_t sh)
{
    (void)tdp; (void)fd; (void)stride; (void)sw; (void)sh;
    atomic_store(&conversion_started, 1);
    while (atomic_load(&conversion_hold)) usleep(1000);
    if (conversion_error) return -EIO;
    assert(!borrowed);
    uint32_t pitch = dw * 2 + 16;
    assert((size_t)pitch * dh <= sizeof(converted));
    memset(converted, 0xa5, sizeof(converted));
    for (uint32_t y = 0; y < dh; y++)
        for (uint32_t x = 0; x < dw; x++) {
            converted[y*pitch+x*2] = 0; converted[y*pitch+x*2+1] = 0xf8;
        }
    *frame = (struct onekvm_v4l2_transform_frame){.data = converted,
        .width = dw, .height = dh, .stride = pitch, .size = pitch * dh};
    borrowed = 1;
    return 0;
}
void __wrap_ax_tdp_snapshot_release(struct ax_tdp *tdp)
{
    (void)tdp;
    assert(borrowed); borrowed = 0; released++;
}
struct shot {
    struct ax_source *src;
    struct onekvm_video_snapshot_request_v1 req;
    struct onekvm_video_snapshot_result_v1 res;
    uint8_t data[65536];
    int rc;
};
static void *snapshot_thread(void *arg)
{
    struct shot *shot = arg;
    char error[128];
    shot->rc = ax_source_snapshot(shot->src, &shot->req, shot->data,
                                  sizeof(shot->data), &shot->res, error, sizeof(error));
    return NULL;
}
static void wait_pending(struct ax_source *src)
{
    uint64_t start = ax_now_ms();
    while (!atomic_load(&src->snapshot_pending) && ax_now_ms() - start < 500) usleep(1000);
    assert(atomic_load(&src->snapshot_pending));
}
static void check_jpeg(struct shot *shot, unsigned w, unsigned h)
{
    assert(shot->rc == 0 && shot->res.width == (int)w && shot->res.height == (int)h);
    assert(shot->res.pts_ns && shot->res.data_size > 100);
    struct jpeg_decompress_struct jpeg = {0};
    struct jpeg_error_mgr error;
    jpeg.err = jpeg_std_error(&error);
    jpeg_create_decompress(&jpeg);
    jpeg_mem_src(&jpeg, shot->data, shot->res.data_size);
    assert(jpeg_read_header(&jpeg, TRUE) == JPEG_HEADER_OK);
    assert(jpeg.image_width == w && jpeg.image_height == h);
    jpeg_start_decompress(&jpeg);
    uint8_t *row = malloc(w * 3);
    while (jpeg.output_scanline < h) {
        JSAMPROW p = row;
        jpeg_read_scanlines(&jpeg, &p, 1);
        for (unsigned x = 0; x < w; x++)
            assert(row[x*3] > 240 && row[x*3+1] < 15 && row[x*3+2] < 15);
    }
    free(row); jpeg_finish_decompress(&jpeg); jpeg_destroy_decompress(&jpeg);
}
int main(void)
{
    struct ax_source src;
    ax_source_init(&src);
    src.streaming = 1; src.cap_fd = 123; src.nbufs = 3;
    src.width = 1920; src.height = 1080; src.bytesperline = 3840;
    uint8_t fake[8]; src.map[0] = fake; src.map_len[0] = sizeof(fake);
    assert(ax_start_display_worker(&src) == 0);
    struct shot shot = {.src = &src,
        .req = {.struct_size = sizeof(shot.req), .quality_factor = .8, .timeout_ms = 300},
        .res = {.struct_size = sizeof(shot.res)}};
    char error[128];
    /* Pending snapshots never dequeue from an active encoder. */
    atomic_store(&src.encoded_requested_ms, ax_now_ms());
    pthread_t thread;
    assert(!pthread_create(&thread, NULL, snapshot_thread, &shot));
    wait_pending(&src);
    assert(dequeues == 0);
    assert(ax_source_snapshot(&src, &shot.req, shot.data, sizeof(shot.data),
                              &shot.res, error, sizeof(error)) == ONEKVM_VIDEO_RESOURCE_BUSY);
    atomic_store(&conversion_hold, 1);
    pthread_mutex_lock(&src.lock);
    src.held = 1; src.held_index = 0;
    ax_schedule_display(&src, src.bytesperline);
    uint64_t start = ax_now_ms();
    while (!atomic_load(&conversion_started) && ax_now_ms() - start < 500) usleep(1000);
    assert(atomic_load(&conversion_started));
    ax_requeue(&src);
    assert(requeues == 0 && !src.held);
    /* Encoding can advance with another buffer while conversion is blocked. */
    src.held = 1; src.held_index = 1; ax_requeue(&src);
    assert(requeues == 1);
    atomic_store(&conversion_hold, 0);
    ax_collect_display_job(&src, 1, 1);
    pthread_mutex_unlock(&src.lock);
    pthread_join(thread, NULL);
    assert(dequeues == 0 && requeues == 2);
    check_jpeg(&shot, 1920, 1080);
    assert(!borrowed && released == 1);
    struct ax_jpeg *jpeg_context = src.snapshot_jpeg;
    /* Display-only capture shares the queue; default size follows video config. */
    atomic_store(&src.encoded_requested_ms, 0);
    src.output_resolution = 480;
    snapshot_thread(&shot);
    assert(dequeues == 1 && requeues == 3 && src.streaming);
    check_jpeg(&shot, 640, 480);
    assert(src.snapshot_jpeg == jpeg_context && !borrowed && released == 2);
    shot.req.width = 320; shot.req.height = 240;
    snapshot_thread(&shot); check_jpeg(&shot, 320, 240);
    /* Bounded output, error propagation and no stale replay after timeout. */
    struct { uint8_t data[32]; uint8_t guard[32]; } tiny;
    memset(&tiny, 0xa5, sizeof(tiny));
    assert(ax_source_snapshot(&src, &shot.req, tiny.data, sizeof(tiny.data), &shot.res,
                              error, sizeof(error)) == ONEKVM_VIDEO_RESOURCE_BUFFER_TOO_SMALL);
    for (unsigned i = 0; i < sizeof(tiny.guard); i++) assert(tiny.guard[i] == 0xa5);
    assert(!borrowed);
    /* An overflow must release the TDP view and leave JPEG reusable. */
    snapshot_thread(&shot); check_jpeg(&shot, 320, 240);
    assert(!borrowed && src.snapshot_jpeg == jpeg_context);
    conversion_error = 1; snapshot_thread(&shot);
    assert(shot.rc == ONEKVM_VIDEO_RESOURCE_INTERNAL);
    conversion_error = 0;
    atomic_store(&src.encoded_requested_ms, ax_now_ms());
    shot.req.timeout_ms = 10;
    snapshot_thread(&shot);
    assert(shot.rc == ONEKVM_VIDEO_RESOURCE_TIMEOUT && !src.snapshot_frame.data && !borrowed);
    uint64_t old_generation = atomic_load(&src.snapshot_generation);
    shot.req.timeout_ms = 300;
    assert(!pthread_create(&thread, NULL, snapshot_thread, &shot));
    wait_pending(&src);
    ax_capture_snapshot(&src, 0, 3840, 1920, 1080, 0, old_generation);
    assert(atomic_load(&src.snapshot_pending));
    pthread_mutex_lock(&src.lock);
    src.held = 1; src.held_index = 0;
    ax_schedule_display(&src, 3840); ax_requeue(&src);
    ax_collect_display_job(&src, 1, 1);
    pthread_mutex_unlock(&src.lock);
    pthread_join(thread, NULL); check_jpeg(&shot, 320, 240);
    shot.req.width = 321;
    snapshot_thread(&shot); assert(shot.rc == ONEKVM_VIDEO_RESOURCE_INVALID);
    shot.req.width = 320; shot.req.quality_factor = NAN;
    snapshot_thread(&shot); assert(shot.rc == ONEKVM_VIDEO_RESOURCE_INVALID);
    ax_stop_display_worker(&src);
    ax_tdp_close(&src.snapshot_hw);
    ax_snapshot_jpeg_destroy(src.snapshot_jpeg);
    puts("snapshot: shared producer frame, JPEG decode, sizes, bounded output, busy, timeout/cancel, hardware errors PASS");
    return 0;
}
