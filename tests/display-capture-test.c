/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
/* Exercise display-only capture with a fake completed VIN buffer; no encoder. */
#include <assert.h>
#include <stdarg.h>
#include "../src/ax630c_video.c"

static int pending, dequeues, requeues;
static int conversion_error;
static const uint8_t test_yuyv[] = {63, 102, 63, 240, 63, 102, 63, 240};

int __wrap_ax_tdp_convert(struct ax_tdp *tdp, uint8_t *out, uint32_t dw, uint32_t dh,
                          int fd, uint32_t stride, uint32_t sw, uint32_t sh)
{
    (void)tdp; (void)fd;
    if (conversion_error) return -EIO;
    return ax_display_convert(out, dw, dh, test_yuyv, sizeof(test_yuyv), stride, sw, sh);
}

struct cached_reader {
    struct ax_source *source;
    struct onekvm_video_display_request_v1 request;
    struct onekvm_video_display_result_v1 result;
    uint8_t *rgb;
    atomic_int finished;
    int rc;
};

static void *read_cached_frame(void *arg)
{
    struct cached_reader *reader = arg;
    char error[128];
    reader->rc = ax_source_display(reader->source, &reader->request,
        reader->rgb, AX_DISPLAY_BYTES, &reader->result, error, sizeof(error));
    atomic_store(&reader->finished, 1);
    return NULL;
}

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
    void *argument = va_arg(args, void *);
    va_end(args);
    if (request == VIDIOC_EXPBUF) {
        struct v4l2_exportbuffer *exp = argument;
        exp->fd = dup(STDIN_FILENO);
        assert(exp->fd >= 0);
        return 0;
    }
    struct v4l2_buffer *buf = argument;
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
    uint8_t yuyv[] = {63, 102, 63, 240, 63, 102, 63, 240};
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
    /* A live encoded stream owns VIN: an empty RGB cache must not steal
     * completed buffers or perform DMA recovery, even after its stall timer. */
    request.flags = ONEKVM_VIDEO_DISPLAY_REQUEST_CACHE_ONLY;
    request.timeout_ms = 5;
    src.display_stalled_since_ms = 1;
    assert(ax_source_display(&src, &request, rgb, AX_DISPLAY_BYTES,
                             &result, error, sizeof(error)) == ONEKVM_VIDEO_RESOURCE_TIMEOUT);
    assert(pending == 3 && dequeues == 0 && requeues == 0);
    src.display_stalled_since_ms = 0;
    request.flags = 0;
    request.timeout_ms = 100;
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
    /* Cache-only consumers receive frames published by the encoded path. */
    src.display_published_ms = 0; src.display_attempted_ms = 0;
    ax_publish_display(&src, yuyv, sizeof(yuyv), 4, 2, 2, 0);
    request.flags = ONEKVM_VIDEO_DISPLAY_REQUEST_CACHE_ONLY;
    int before = dequeues;
    /* The producer may own VIN while waiting for its next encoded frame.
     * Reading an already published RGB frame must never queue behind it. */
    struct cached_reader reader = {
        .source = &src, .request = request, .result = result, .rgb = rgb,
    };
    atomic_init(&reader.finished, 0);
    pthread_t thread;
    pthread_mutex_lock(&src.lock);
    assert(pthread_create(&thread, NULL, read_cached_frame, &reader) == 0);
    start = ax_now_ms();
    while (!atomic_load(&reader.finished) && ax_now_ms() - start < 100)
        usleep(1000);
    int finished_with_vin_locked = atomic_load(&reader.finished);
    pthread_mutex_unlock(&src.lock);
    pthread_join(thread, NULL);
    assert(finished_with_vin_locked && reader.rc == 0);
    assert(dequeues == before);
    /* A failed hardware job must not publish a CPU-converted replacement. */
    conversion_error = 1;
    uint64_t old_sequence = src.display_seq;
    src.display_published_ms = 0; src.display_attempted_ms = 0;
    assert(ax_publish_display(&src, yuyv, sizeof(yuyv), 4, 2, 2, 0) == 0);
    assert(src.display_seq == old_sequence);
    conversion_error = 0;
    request.flags = 0;
    /* An encoded producer must advance while RGB conversion is blocked.
     * Return a borrowed slot only after both encoder and preview release it. */
    assert(ax_start_display_worker(&src) == 0);
    pthread_mutex_lock(&src.lock);
    pthread_mutex_lock(&src.display_lock);
    src.display_requested_ms = ax_now_ms();
    src.display_published_ms = 0; src.display_attempted_ms = 0;
    src.held = 1; src.held_index = 0;
    int queued = requeues;
    ax_schedule_display(&src, 4);
    assert(src.display_job_index == 0);
    ax_requeue(&src);
    assert(!src.held && requeues == queued);
    src.held = 1; src.held_index = 1;
    ax_requeue(&src);
    assert(requeues == queued + 1);
    pthread_mutex_unlock(&src.display_lock);
    /* Teardown also holds VIN: finishing a job must not need that lock. */
    ax_collect_display_job(&src, 1, 1);
    assert(src.display_job_index == -1 && requeues == queued + 2);
    /* Reverse completion order: preview finishes before the encoder. */
    src.display_requested_ms = ax_now_ms(); src.display_published_ms = 0; src.display_attempted_ms = 0;
    src.held = 1; src.held_index = 0;
    ax_schedule_display(&src, 4);
    ax_collect_display_job(&src, 1, 1);
    assert(requeues == queued + 2);
    ax_requeue(&src);
    assert(requeues == queued + 3);
    /* Capture teardown drains its borrow without returning it to VIN. */
    src.display_requested_ms = ax_now_ms(); src.display_published_ms = 0; src.display_attempted_ms = 0;
    src.held = 1; src.held_index = 0;
    ax_schedule_display(&src, 4);
    ax_requeue(&src);
    ax_collect_display_job(&src, 1, 0);
    assert(requeues == queued + 3 && src.display_job_index == -1);
    ax_stop_display_worker(&src);
    pthread_mutex_unlock(&src.lock);
    /* The bound no-signal artwork remains available without encoded packets. */
    src.streaming = 0; src.display_placeholder = 1;
    src.display_last_read_ms = 0;
    assert(ax_source_display(&src, &request, rgb, AX_DISPLAY_BYTES,
                             &result, error, sizeof(error)) == 0);
    uint64_t pts = result.pts_ns;
    request.timeout_ms = 250;
    start = ax_now_ms();
    assert(ax_source_display(&src, &request, rgb, AX_DISPLAY_BYTES,
                             &result, error, sizeof(error)) == 0);
    assert(result.pts_ns > pts && dequeues == 3);
    assert(ax_now_ms() - start >= 190);
    atomic_store(&src.interrupted, 1);
    assert(ax_source_display(&src, &request, rgb, AX_DISPLAY_BYTES,
                             &result, error, sizeof(error)) == ONEKVM_VIDEO_RESOURCE_TIMEOUT);
    free(rgb);
    pthread_cond_destroy(&src.display_cv);
    pthread_cond_destroy(&src.display_job_cv);
    pthread_mutex_destroy(&src.display_job_lock);
    pthread_mutex_destroy(&src.display_lock);
    pthread_mutex_destroy(&src.lock);
    return 0;
}
