/* SPDX-License-Identifier: MIT */
/*
 * NanoKVM Pro (AX630C) OneKVM video backend.
 *
 * Captures packed YUYV from the open VIN node and submits it to the open
 * VC8000E command engine. The register program and parameter-set builders are the
 * MIT-or-GPL-2.0 headers under third_party/vcenc-ewl (open-nanokvm-pro
 * vcenc-ewl). This file does not copy the GPL-3 kvm-encoder sources.
 *
 * Same idea as the SG200x VI→VENC bind: the pixels stay in the capture
 * pool. Each buffer is exported once (VIDIOC_EXPBUF) and imported as the
 * encoder input bus address. The dequeued buffer is not returned until
 * that encode has finished reading it. A failed import falls back to
 * copying the frame.
 *
 * The web player uses the bound-encoder path: source_create succeeds as soon
 * as the capture node can be named. Bind starts live capture when HDMI is
 * locked, or an encoded no-signal placeholder until the input returns.
 */
#define _GNU_SOURCE

#include <onekvm/video_backend_v1.h>

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "vcenc_encode.h"
#include "vcenc_header.h"
#include "vcenc_hevc_header.h"
#include "ax_no_signal.h"
#include "ax_display.h"
#include "ax_tdp.h"
#include "ax_edid.h"
#include "ax_rate.h"
#include "ax_snapshot.h"
#include "ax_pacing.h"

#define AX_DRIVER_ID "ax630c"
#define AX_CAP_DEV "/dev/video0"
/* Power-of-two, matching the capture carveout's DMA bitmap slots. */
#define AX_MAX_BUFS 8
/* Parameter sets precede the slice in the fixed encoder output region.
 * Core copies the borrowed AU before the next encode; clients cannot pin it. */
#define AX_AU_HEADROOM 256
#define AX_PROC_W "/proc/lt6911_info/width"
#define AX_PROC_H "/proc/lt6911_info/height"
#define AX_PROC_FPS "/proc/lt6911_info/fps"
#define AX_PROC_RX "/proc/lt6911_info/hdmi_rx_status"
#define AX_PROC_STATUS "/proc/lt6911_info/status"
#define AX_PROC_POWER "/proc/lt6911_info/power"
#define AX_PROC_RX_GEOMETRY "/proc/lt6911_info/rx_geometry"
#define AX_PLACEHOLDER_W 1920u
#define AX_PLACEHOLDER_H 1080u
/* The bridge needs about 2.5 s from "disappear" to a new CSI mode. Sending a
 * placeholder IDR midway can make browsers buffer the next 60+ live frames. */
#define AX_SIGNAL_LOSS_GRACE_MS 3200u
#define AX_MODE_POLL_INTERVAL_MS 100u

struct ax_source {
    char device[256];
    pthread_mutex_t lock;
    /* Encoded-session display reads only this cache; display-only capture
     * takes the VIN lock separately before touching the dequeue queue. */
    pthread_mutex_t display_lock;
    pthread_cond_t display_cv;
    uint8_t display_rgb[AX_DISPLAY_BYTES];
    uint64_t display_seq, display_read_seq, display_pts;
    atomic_uint_fast64_t display_requested_ms, display_published_ms, display_attempted_ms;
    uint64_t display_last_read_ms;
    uint64_t display_stalled_since_ms;
    uint64_t display_error_ms;
    struct ax_tdp display_hw;
    int display_placeholder;
    /* One bounded preview job borrows a capture slot. Only the VIN owner
     * returns slots; the worker never takes the VIN lock or queues buffers. */
    pthread_mutex_t display_job_lock;
    pthread_cond_t display_job_cv;
    pthread_t display_worker;
    int display_worker_started, display_worker_stop;
    int display_job_index, display_job_complete;
    uint32_t display_job_stride, display_job_width, display_job_height;
    int32_t display_job_resolution;
    uint64_t display_job_snapshot_generation;
    /* One request borrows the TDP cached RGB output through compression.
     * VIN is returned as soon as conversion completes, before JPEG encoding. */
    pthread_mutex_t snapshot_gate, snapshot_lock;
    pthread_cond_t snapshot_cv;
    atomic_int snapshot_pending;
    atomic_uint_fast64_t encoded_requested_ms;
    struct onekvm_v4l2_transform_frame snapshot_frame;
    struct ax_jpeg *snapshot_jpeg;
    uint32_t snapshot_width, snapshot_height;
    atomic_uint_fast64_t snapshot_generation;
    uint64_t snapshot_pts;
    int snapshot_status;
    struct ax_tdp snapshot_hw;
    int cap_fd;
    int wake_fd;
    atomic_int interrupted;
    unsigned nbufs;
    void *map[AX_MAX_BUFS];
    size_t map_len[AX_MAX_BUFS];
    uint32_t width;
    uint32_t height;
    uint32_t bytesperline;
    uint32_t sizeimage;
    uint32_t fps;
    int32_t output_resolution;
    int streaming;
    int held;
    uint32_t held_index;
    uint64_t capture_ns;
    /* Bus address of each capture buffer, imported once for the stream. */
    int import_venc_fd;
    uint32_t dma_phys[AX_MAX_BUFS];
};

struct ax_encoder {
    int venc_fd;
    uint32_t *cmd;
    uint8_t *status;
    struct cmdbuf_mem_parameter mem;
    struct framebuf_parameter fb;
    uint8_t *fb_map;
    vcenc_geom geom;
    int have_geom;
    uint32_t codec;
    atomic_uint color_info;
    uint32_t qp;
    struct ax_rate rate;
    uint32_t gop;
    uint32_t fps;
    uint32_t seq;
    struct ax_pacer pacer;
    int force_idr;
    struct ax_source *bound;
    uint32_t bound_w;
    uint32_t bound_h;
    uint64_t last_mode_ms;
    uint64_t signal_loss_since_ms;
    uint64_t last_placeholder_ms;
    int early_capture;
    int early_frame_seen;
    uint64_t early_started_ms;
    uint32_t early_prev_w;
    uint32_t early_prev_h;
    int placeholder;
    size_t packet_len;
    uint32_t packet_key;
    uint64_t packet_pts;
    uint64_t encode_ns;
    uint8_t *publish;
};

static void ax_err(char *error, uint32_t cap, const char *msg)
{
    if (!error || cap == 0)
        return;
    snprintf(error, cap, "%s", msg ? msg : "ax630c video failed");
}

static void ax_err_errno(char *error, uint32_t cap, const char *what)
{
    if (!error || cap == 0)
        return;
    snprintf(error, cap, "%s: %s", what, strerror(errno));
}

static uint64_t ax_now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

static uint64_t ax_now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int ax_read_u32_file(const char *path, uint32_t *out)
{
    FILE *f;
    unsigned int v;

    f = fopen(path, "r");
    if (!f)
        return -1;
    if (fscanf(f, "%u", &v) != 1) {
        fclose(f);
        return -1;
    }
    fclose(f);
    *out = v;
    return 0;
}

/* Locked means the bridge says the RX plug is mated and a real mode is up. */
static int ax_hdmi_mode(uint32_t *w, uint32_t *h, uint32_t *fps)
{
    char status[32];
    FILE *f;
    uint32_t width = 0, height = 0, rate = 0;

    f = fopen(AX_PROC_RX, "r");
    if (!f)
        return -1;
    if (!fgets(status, sizeof(status), f)) {
        fclose(f);
        return -1;
    }
    fclose(f);
    if (strncmp(status, "access", 6) != 0)
        return -1;
    f = fopen(AX_PROC_POWER, "r");
    if (!f)
        return -1;
    if (!fgets(status, sizeof(status), f)) {
        fclose(f);
        return -1;
    }
    fclose(f);
    if (strncmp(status, "on", 2) != 0)
        return -1;
    f = fopen(AX_PROC_STATUS, "r");
    if (!f)
        return -1;
    if (!fgets(status, sizeof(status), f)) {
        fclose(f);
        return -1;
    }
    fclose(f);
    if (strncmp(status, "new res", 7) != 0 &&
        strncmp(status, "stable", 6) != 0)
        return -1;
    if (ax_read_u32_file(AX_PROC_W, &width) ||
        ax_read_u32_file(AX_PROC_H, &height) ||
        ax_read_u32_file(AX_PROC_FPS, &rate))
        return -1;
    if (width == 0 || height == 0)
        return -1;
    *w = width;
    *h = height;
    /* LT6911 publishes the new dimensions before its fps counter settles.
     * VIN can already be armed for that geometry; a later poll replaces
     * this provisional rate with the measured value. */
    *fps = rate ? rate : 60;
    return 0;
}

/* RX timing precedes CSI pixels. Only the stalled capture path reads it. */
static int ax_rx_geometry(uint32_t *w, uint32_t *h)
{
    unsigned width, height;
    FILE *file = fopen(AX_PROC_RX_GEOMETRY, "r");
    int found;

    if (!file)
        return -1;
    found = fscanf(file, "%u %u", &width, &height);
    fclose(file);
    if (found != 2 || width < 320 || height < 200 ||
        width > 3840 || height > 2400 || (width & 1) || (height & 1))
        return -1;
    *w = width;
    *h = height;
    return 0;
}

static int ax_confirm_new_rx_geometry(struct ax_encoder *enc, struct ax_source *src,
                                      uint32_t *w, uint32_t *h)
{
    uint32_t first_w, first_h;

    if (enc->early_capture || ax_rx_geometry(&first_w, &first_h) != 0 ||
        (first_w == src->width && first_h == src->height))
        return -1;
    usleep(10000);
    if (ax_rx_geometry(w, h) != 0 || *w != first_w || *h != first_h)
        return 1;
    return 0;
}

/* Publish only while a display consumer is active, at most 15 fps. The input
 * remains dequeued until this copy and the encoder have both finished. */
static int ax_publish_display(struct ax_source *src, const uint8_t *frame,
                               size_t size, uint32_t stride,
                               uint32_t width, uint32_t height, int capture_index)
{
    int published = 0;
    pthread_mutex_lock(&src->display_lock);
    uint64_t now = ax_now_ms();
    if (src->display_requested_ms && now - src->display_requested_ms < 2000 &&
        (!src->display_attempted_ms || now - src->display_attempted_ms >= 66)) {
        src->display_attempted_ms = now;
        int rc;
        if (capture_index >= 0) {
            struct v4l2_exportbuffer exp = {
                .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
                .index = (unsigned)capture_index, .flags = O_CLOEXEC,
            };
            if (ioctl(src->cap_fd, VIDIOC_EXPBUF, &exp) < 0) {
                rc = -errno;
            } else {
                rc = ax_tdp_convert(&src->display_hw, src->display_rgb,
                                    AX_DISPLAY_W, AX_DISPLAY_H, exp.fd,
                                    stride, width, height);
                close(exp.fd);
            }
            if (rc && (!src->display_error_ms || now - src->display_error_ms >= 5000)) {
                fprintf(stderr, "ax630c: TDP preview: %s\n", strerror(-rc));
                src->display_error_ms = now;
            }
        } else {
            /* Static no-signal artwork has no VIN DMA-BUF. */
            rc = ax_display_convert(src->display_rgb, AX_DISPLAY_W, AX_DISPLAY_H,
                                    frame, size, stride, width, height);
        }
        if (rc == 0) {
            src->display_pts = ax_now_ns();
            src->display_published_ms = now;
            src->display_seq++;
            published = 1;
            pthread_cond_broadcast(&src->display_cv);
        }
    }
    pthread_mutex_unlock(&src->display_lock);
    return published;
}

static void ax_queue_capture_index(struct ax_source *src, uint32_t index)
{
    struct v4l2_buffer buf = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
        .index = index,
    };
    if (ioctl(src->cap_fd, VIDIOC_QBUF, &buf) < 0)
        fprintf(stderr, "ax630c: QBUF %u: %s\n", index, strerror(errno));
}

static void ax_output_size(int32_t resolution, uint32_t input_w, uint32_t input_h,
                           uint32_t *width, uint32_t *height);

/* The worker borrows the producer's dequeued buffer. Never acquires VIN. */
static void ax_capture_snapshot(struct ax_source *src, unsigned index,
                                uint32_t stride, uint32_t width, uint32_t height,
                                int32_t resolution, uint64_t generation)
{
    pthread_mutex_lock(&src->snapshot_lock);
    if (!atomic_load(&src->snapshot_pending) ||
        generation != atomic_load(&src->snapshot_generation)) {
        pthread_mutex_unlock(&src->snapshot_lock);
        return;
    }
    uint32_t w = src->snapshot_width, h = src->snapshot_height;
    if (!w) ax_output_size(resolution, width, height, &w, &h);
    int rc = -EINVAL;
    if (w && h && !(w & 1u) && w <= 4096 && h <= 2400) {
        struct v4l2_exportbuffer exp = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .index = index, .flags = O_CLOEXEC,
        };
        rc = -EIO;
        if (ioctl(src->cap_fd, VIDIOC_EXPBUF, &exp) == 0) {
            rc = ax_tdp_snapshot_acquire(&src->snapshot_hw, &src->snapshot_frame,
                                         w, h, exp.fd, stride, width, height);
            close(exp.fd);
        }
    }
    src->snapshot_width = w; src->snapshot_height = h;
    src->snapshot_pts = ax_now_ns();
    src->snapshot_status = rc;
    atomic_store(&src->snapshot_pending, 0);
    pthread_cond_broadcast(&src->snapshot_cv);
    pthread_mutex_unlock(&src->snapshot_lock);
}

static void *ax_display_worker(void *arg)
{
    struct ax_source *src = arg;
    pthread_setname_np(pthread_self(), "ax-rgb-preview");
    pthread_mutex_lock(&src->display_job_lock);
    for (;;) {
        while (!src->display_worker_stop &&
               (src->display_job_index < 0 || src->display_job_complete))
            pthread_cond_wait(&src->display_job_cv, &src->display_job_lock);
        if (src->display_worker_stop)
            break;
        unsigned index = (unsigned)src->display_job_index;
        const uint8_t *frame = src->map[index];
        size_t size = src->map_len[index];
        uint32_t stride = src->display_job_stride;
        uint32_t width = src->display_job_width, height = src->display_job_height;
        int32_t resolution = src->display_job_resolution;
        uint64_t generation = src->display_job_snapshot_generation;
        pthread_mutex_unlock(&src->display_job_lock);
        ax_capture_snapshot(src, index, stride, width, height, resolution, generation);
        ax_publish_display(src, frame, size, stride, width, height, (int)index);
        pthread_mutex_lock(&src->display_job_lock);
        src->display_job_complete = 1;
        pthread_cond_broadcast(&src->display_job_cv);
    }
    pthread_mutex_unlock(&src->display_job_lock);
    return NULL;
}

static int ax_start_display_worker(struct ax_source *src)
{
    int rc = pthread_create(&src->display_worker, NULL, ax_display_worker, src);
    if (!rc)
        src->display_worker_started = 1;
    return rc;
}

/* Call with VIN locked. Completion never needs VIN, so teardown can wait
 * without exposing half-reconfigured capture state to the encoder. */
static void ax_collect_display_job(struct ax_source *src, int wait, int requeue)
{
    pthread_mutex_lock(&src->display_job_lock);
    while (wait && src->display_job_index >= 0 && !src->display_job_complete)
        pthread_cond_wait(&src->display_job_cv, &src->display_job_lock);
    if (src->display_job_index >= 0 && src->display_job_complete) {
        uint32_t index = (uint32_t)src->display_job_index;
        src->display_job_index = -1;
        src->display_job_complete = 0;
        if (requeue && src->streaming && src->cap_fd >= 0 &&
            (!src->held || src->held_index != index))
            ax_queue_capture_index(src, index);
    }
    pthread_mutex_unlock(&src->display_job_lock);
}

/* Only the encoded producer submits jobs, while holding VIN. Never wait
 * for conversion or the RGB cache lock on the primary encoding thread. */
static void ax_schedule_display(struct ax_source *src, uint32_t stride)
{
    uint64_t requested = atomic_load(&src->display_requested_ms);
    uint64_t attempted = atomic_load(&src->display_attempted_ms);
    uint64_t now = ax_now_ms();
    if (!src->display_worker_started || !src->held || src->nbufs < 3 ||
        atomic_load(&src->interrupted) ||
        (!atomic_load(&src->snapshot_pending) &&
         (!requested || now - requested >= 2000 ||
          (attempted && now - attempted < 66))))
        return;
    ax_collect_display_job(src, 0, 1);
    pthread_mutex_lock(&src->display_job_lock);
    if (src->display_job_index < 0) {
        src->display_job_index = (int)src->held_index;
        src->display_job_complete = 0;
        src->display_job_stride = stride;
        src->display_job_width = src->width;
        src->display_job_height = src->height;
        src->display_job_resolution = src->output_resolution;
        src->display_job_snapshot_generation = atomic_load(&src->snapshot_generation);
        pthread_cond_signal(&src->display_job_cv);
    }
    pthread_mutex_unlock(&src->display_job_lock);
}

/* Drain the job before stopping or unmapping capture. */
static void ax_stop_display_worker(struct ax_source *src)
{
    if (!src->display_worker_started)
        return;
    pthread_mutex_lock(&src->display_job_lock);
    src->display_worker_stop = 1;
    pthread_cond_signal(&src->display_job_cv);
    pthread_mutex_unlock(&src->display_job_lock);
    pthread_join(src->display_worker, NULL);
    src->display_worker_started = 0;
}

static int ax_grab_frame_budget(struct ax_source *src, const uint8_t **data,
                                uint32_t *stride, char *error, uint32_t cap,
                                uint32_t timeout_ms);
static void ax_requeue(struct ax_source *src);
static int ax_capture_start(struct ax_source *src, uint32_t width, uint32_t height,
                            char *error, uint32_t cap);
static int ax_import_capture(struct ax_source *src, int venc_fd);

static int32_t ax_source_display(void *source,
    const struct onekvm_video_display_request_v1 *request,
    uint8_t *data, uint64_t capacity,
    struct onekvm_video_display_result_v1 *result, char *error, uint32_t cap)
{
    struct ax_source *src = source;
    struct timespec until;
    uint32_t timeout;
    uint64_t deadline;
    int cache_only;
    if (!src || !request || !result || !data ||
        request->struct_size < sizeof(*request) || result->struct_size < sizeof(*result) ||
        request->width < 0 || request->height < 0 ||
        (request->width == 0) != (request->height == 0) ||
        (request->width & 1) || (request->height & 1)) {
        ax_err(error, cap, "invalid display request");
        return ONEKVM_VIDEO_RESOURCE_INVALID;
    }
    if ((request->flags & ~ONEKVM_VIDEO_DISPLAY_REQUEST_CACHE_ONLY) ||
        (request->pixel_format && request->pixel_format != ONEKVM_VIDEO_PIXEL_RGB565)) {
        ax_err(error, cap, "unsupported display format");
        return ONEKVM_VIDEO_RESOURCE_UNSUPPORTED;
    }
    if (capacity < AX_DISPLAY_BYTES) {
        ax_err(error, cap, "display buffer is too small");
        return ONEKVM_VIDEO_RESOURCE_BUFFER_TOO_SMALL;
    }
    cache_only = (request->flags & ONEKVM_VIDEO_DISPLAY_REQUEST_CACHE_ONLY) != 0;
    timeout = request->timeout_ms ? request->timeout_ms : 250;
    if (timeout > 1000) timeout = 1000;
    deadline = ax_now_ms() + timeout;
    for (;;) {
        uint64_t now = ax_now_ms();
        const uint8_t *frame = NULL;
        uint32_t stride = 0;
        int available;

        if (!cache_only)
            pthread_mutex_lock(&src->lock);
        pthread_mutex_lock(&src->display_lock);
        now = ax_now_ms();
        src->display_requested_ms = now;
        available = (src->display_placeholder &&
                     (!src->display_last_read_ms || now - src->display_last_read_ms >= 200)) ||
            (!src->display_placeholder && src->display_seq != src->display_read_seq &&
             now - src->display_published_ms < 200);
        pthread_mutex_unlock(&src->display_lock);
        /* Only display-only demand may dequeue VIN. With an encoded session,
         * wait for the RGB copy published by its producer instead of draining
         * frames or performing stall recovery under the primary stream. */
        if (!available && !cache_only &&
            !atomic_load(&src->interrupted) && src->streaming &&
            now < deadline && (!src->display_published_ms ||
                               now - src->display_published_ms >= 66)) {
            uint32_t budget = (uint32_t)(deadline - now);
            if (budget > 50) budget = 50;
            int captured = ax_grab_frame_budget(src, &frame, &stride, error, cap, budget);
            if (captured == 0) {
                /* A 15 fps panel must not work through a 60 fps FIFO. Drop
                 * already completed older frames, keeping the newest borrow. */
                for (unsigned skipped = 1; skipped < src->nbufs; skipped++) {
                    struct pollfd pending = { .fd = src->cap_fd, .events = POLLIN };
                    if (poll(&pending, 1, 0) <= 0 || !(pending.revents & POLLIN))
                        break;
                    ax_requeue(src);
                    if (ax_grab_frame_budget(src, &frame, &stride, error, cap, 1) != 0) {
                        frame = NULL;
                        break;
                    }
                }
                if (frame) {
                    available = ax_publish_display(src, frame, src->map_len[src->held_index],
                                       stride, src->width, src->height, (int)src->held_index);
                    ax_requeue(src);
                }
            } else if (captured == 2) {
                uint32_t w, h, fps;
                if (!src->display_stalled_since_ms)
                    src->display_stalled_since_ms = ax_now_ms();
                /* Published geometry can precede usable CSI. Recover the
                 * dead DMA ring even when ReadEncodedVideo is never called.
                 * Do not restart during bridge signal loss or a mode change. */
                if (ax_now_ms() - src->display_stalled_since_ms >= 300 &&
                    ax_hdmi_mode(&w, &h, &fps) == 0 &&
                    w == src->width && h == src->height) {
                    int import_fd = src->import_venc_fd;
                    fprintf(stderr, "ax630c: display capture stalled, restarting DMA ring\n");
                    if (ax_capture_start(src, w, h, error, cap) == 0 && import_fd >= 0)
                        ax_import_capture(src, import_fd);
                    src->display_stalled_since_ms = 0;
                }
            }
        }
        pthread_mutex_lock(&src->display_lock);
        if (available && !atomic_load(&src->interrupted)) {
            if (!cache_only)
                src->display_stalled_since_ms = 0;
            memcpy(data, src->display_rgb, AX_DISPLAY_BYTES);
            src->display_read_seq = src->display_seq;
            src->display_last_read_ms = ax_now_ms();
            result->pts_ns = src->display_placeholder ? ax_now_ns() : src->display_pts;
            pthread_mutex_unlock(&src->display_lock);
            if (!cache_only)
                pthread_mutex_unlock(&src->lock);
            break;
        }
        if (!cache_only)
            pthread_mutex_unlock(&src->lock);
        now = ax_now_ms();
        if (now >= deadline || atomic_load(&src->interrupted)) {
            pthread_mutex_unlock(&src->display_lock);
            ax_err(error, cap, "display timeout");
            return ONEKVM_VIDEO_RESOURCE_TIMEOUT;
        }
        uint64_t wake = now + 15;
        if (wake > deadline) wake = deadline;
        until.tv_sec = (time_t)(wake / 1000);
        until.tv_nsec = (long)(wake % 1000) * 1000000L;
        pthread_cond_timedwait(&src->display_cv, &src->display_lock, &until);
        pthread_mutex_unlock(&src->display_lock);
    }
    result->data_size = AX_DISPLAY_BYTES;
    result->width = AX_DISPLAY_W;
    result->height = AX_DISPLAY_H;
    result->pixel_format = ONEKVM_VIDEO_PIXEL_RGB565;
    result->flags = 0;
    return ONEKVM_VIDEO_RESOURCE_OK;
}

static void ax_release_imports(struct ax_source *src);

static void ax_capture_stop(struct ax_source *src)
{
    ax_collect_display_job(src, 1, 0);
    /* Transform queues cache DMA-BUF imports beyond the last conversion.
     * Release every VIN borrower before freeing/reallocating its pool: an
     * old 4K pool pinned by previews can otherwise prevent even 1080p REQBUFS.
     * Also run after a failed capture open, when cap_fd is already -1. */
    ax_tdp_close(&src->display_hw);
    ax_tdp_close(&src->snapshot_hw);
    pthread_mutex_lock(&src->display_lock);
    src->display_placeholder = 0;
    pthread_mutex_unlock(&src->display_lock);
    src->display_stalled_since_ms = 0;
    unsigned int i;
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    ax_release_imports(src);

    if (src->cap_fd < 0)
        return;
    src->held = 0;
    if (src->streaming) {
        ioctl(src->cap_fd, VIDIOC_STREAMOFF, &type);
        src->streaming = 0;
    }
    for (i = 0; i < src->nbufs; i++) {
        if (src->map[i] && src->map[i] != MAP_FAILED)
            munmap(src->map[i], src->map_len[i]);
        src->map[i] = NULL;
        src->map_len[i] = 0;
    }
    if (src->nbufs) {
        struct v4l2_requestbuffers req;

        memset(&req, 0, sizeof(req));
        req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_MMAP;
        ioctl(src->cap_fd, VIDIOC_REQBUFS, &req);
        src->nbufs = 0;
    }
    close(src->cap_fd);
    src->cap_fd = -1;
}

static int ax_capture_start(struct ax_source *src, uint32_t width, uint32_t height,
                            char *error, uint32_t error_cap)
{
    struct v4l2_format fmt;
    struct v4l2_requestbuffers req;
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    unsigned int i;

    ax_capture_stop(src);
    src->cap_fd = open(src->device, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (src->cap_fd < 0) {
        ax_err_errno(error, error_cap, "open capture");
        return -1;
    }

    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = width;
    fmt.fmt.pix.height = height;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    if (ioctl(src->cap_fd, VIDIOC_S_FMT, &fmt) < 0) {
        ax_err_errno(error, error_cap, "VIDIOC_S_FMT");
        ax_capture_stop(src);
        return -1;
    }
    if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_YUYV ||
        fmt.fmt.pix.width < 64 || fmt.fmt.pix.height < 64) {
        ax_err(error, error_cap, "capture rejected YUYV geometry");
        ax_capture_stop(src);
        return -1;
    }
    src->width = fmt.fmt.pix.width;
    src->height = fmt.fmt.pix.height;
    src->bytesperline = fmt.fmt.pix.bytesperline;
    src->sizeimage = fmt.fmt.pix.sizeimage;

    memset(&req, 0, sizeof(req));
    req.count = AX_MAX_BUFS;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(src->cap_fd, VIDIOC_REQBUFS, &req) < 0 || req.count < 3) {
        ax_err_errno(error, error_cap, "VIDIOC_REQBUFS");
        ax_capture_stop(src);
        return -1;
    }
    if (req.count > AX_MAX_BUFS)
        req.count = AX_MAX_BUFS;
    src->nbufs = req.count;

    for (i = 0; i < src->nbufs; i++) {
        struct v4l2_buffer buf;

        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if (ioctl(src->cap_fd, VIDIOC_QUERYBUF, &buf) < 0) {
            ax_err_errno(error, error_cap, "VIDIOC_QUERYBUF");
            ax_capture_stop(src);
            return -1;
        }
        src->map_len[i] = buf.length;
        src->map[i] = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED,
                           src->cap_fd, buf.m.offset);
        if (src->map[i] == MAP_FAILED) {
            src->map[i] = NULL;
            ax_err_errno(error, error_cap, "mmap capture");
            ax_capture_stop(src);
            return -1;
        }
        if (ioctl(src->cap_fd, VIDIOC_QBUF, &buf) < 0) {
            ax_err_errno(error, error_cap, "VIDIOC_QBUF");
            ax_capture_stop(src);
            return -1;
        }
    }
    if (ioctl(src->cap_fd, VIDIOC_STREAMON, &type) < 0) {
        ax_err_errno(error, error_cap, "VIDIOC_STREAMON");
        ax_capture_stop(src);
        return -1;
    }
    src->streaming = 1;
    return 0;
}

static void *ax_map_pool(int fd, uint64_t phys, uint32_t size)
{
    void *p;

    if (!phys || !size)
        return NULL;
    p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)phys);
    return p == MAP_FAILED ? NULL : p;
}

static void ax_encoder_release_framebuf(struct ax_encoder *enc)
{
    atomic_store(&enc->color_info, 0);
    if (enc->fb_map && enc->fb_map != MAP_FAILED)
        munmap(enc->fb_map, enc->fb.size ? enc->fb.size : enc->geom.span);
    enc->fb_map = NULL;
    if (enc->venc_fd >= 0 && enc->fb.bus_addr) {
        ioctl(enc->venc_fd, HANTRO_IOCH_FREE_FRAMEBUF, &enc->fb);
        enc->fb.bus_addr = 0;
        enc->fb.size = 0;
    }
    enc->have_geom = 0;
}

static int ax_encoder_alloc_framebuf(struct ax_encoder *enc, uint32_t width,
                                     uint32_t height, char *error, uint32_t error_cap)
{
    const char *why = NULL;

    ax_encoder_release_framebuf(enc);
    if (vcenc_geom_check((int)width, (int)height, &why) ||
        vcenc_geom_build(&enc->geom, (int)width, (int)height)) {
        ax_err(error, error_cap, why ? why : "geometry unsupported");
        return -1;
    }
    enc->fb.size = enc->geom.span;
    enc->fb.bus_addr = 0;
    if (ioctl(enc->venc_fd, HANTRO_IOCH_ALLOC_FRAMEBUF, &enc->fb) < 0) {
        ax_err_errno(error, error_cap, "ALLOC_FRAMEBUF");
        enc->fb.bus_addr = 0;
        return -1;
    }
    enc->fb_map = ax_map_pool(enc->venc_fd, enc->fb.bus_addr, enc->fb.size);
    if (!enc->fb_map) {
        ax_err(error, error_cap, "mmap framebuf failed");
        ax_encoder_release_framebuf(enc);
        return -1;
    }
    enc->have_geom = 1;
    enc->rate.ready = 0;
    return 0;
}

static void ax_copy_yuyv(uint8_t *dst, uint32_t dst_stride, const uint8_t *src,
                         uint32_t src_stride, uint32_t width, uint32_t height)
{
    uint32_t y;
    uint32_t row = width * 2;

    for (y = 0; y < height; y++)
        memcpy(dst + (size_t)y * dst_stride, src + (size_t)y * src_stride, row);
}

/* VC8000E consumes packed YUYV. Keep direct DMA for 1:1; selected output
 * sizes use a small nearest-neighbour conversion into its input framebuf. */
static void ax_scale_yuyv(uint8_t *dst, uint32_t dst_w, uint32_t dst_h,
                          const uint8_t *src, uint32_t src_stride,
                          uint32_t src_w, uint32_t src_h)
{
    uint32_t y, x;

    for (y = 0; y < dst_h; y++) {
        const uint8_t *src_row = src + (size_t)(uint64_t)y * src_h / dst_h * src_stride;
        uint8_t *dst_row = dst + (size_t)y * dst_w * 2;

        for (x = 0; x < dst_w; x += 2) {
            uint32_t src_x = (uint32_t)((uint64_t)x * src_w / dst_w) & ~1u;

            memcpy(dst_row + (size_t)x * 2, src_row + (size_t)src_x * 2, 4);
        }
    }
}

static void ax_output_size(int32_t resolution, uint32_t input_w, uint32_t input_h,
                           uint32_t *width, uint32_t *height)
{
    if (resolution == 480) {
        *width = 640; *height = 480;
    } else if (resolution == 720) {
        *width = 1280; *height = 720;
    } else if (resolution == 1080) {
        *width = 1920; *height = 1080;
    } else if (resolution >= 10000) {
        *width = (uint32_t)(resolution / 10000);
        *height = (uint32_t)(resolution % 10000);
    } else {
        *width = input_w;
        *height = input_h;
    }
}

static uint32_t ax_write_parameter_sets(struct ax_encoder *enc, uint8_t *out)
{
    uint32_t size;

    if (enc->codec == ONEKVM_VIDEO_CODEC_H265)
        return vcenc_write_hevc_headers(out, enc->geom.w, enc->geom.h, enc->rate.initial_qp,
                                       vcenc_hevc_level_idc(enc->geom.w, enc->geom.h));
    size = vcenc_write_sps(out, enc->geom.w, enc->geom.h);
    return size + vcenc_write_pps(out + size, enc->rate.initial_qp);
}

static int ax_encode_yuyv(struct ax_encoder *enc, const uint8_t *yuyv,
                          uint32_t src_stride, uint32_t input_phys,
                          int is_idr, uint32_t gopn)
{
    struct exchange_parameter ex;
    struct vcenc_frame fr;
    struct vcenc_rc next_rate;
    uint16_t id, wid;
    uint32_t *slot;
    uint64_t core_dst;
    volatile uint32_t *regs;
    volatile uint8_t *slice;
    uint32_t swreg9, swreg82, hlen = 0;
    uint64_t t0;
    int nut;
    int hevc = enc->codec == ONEKVM_VIDEO_CODEC_H265;

    if (!input_phys) {
        if (!yuyv)
            return -1;
        ax_copy_yuyv(enc->fb_map + enc->geom.off_in, enc->geom.w * 2, yuyv,
                     src_stride, enc->geom.w, enc->geom.h);
    }

    memset(&ex, 0, sizeof(ex));
    ex.module_type = VCMD_TYPE_ENCODER;
    if (ioctl(enc->venc_fd, HANTRO_IOCH_RESERVE_CMDBUF, &ex) < 0)
        return -1;
    id = ex.cmdbuf_id;
    core_dst = enc->mem.status_hw_addr + (uint64_t)id * 0x2000 +
               (SUBMODULE_MAIN_ADDR / 2);
    memset(enc->status + STATUS_SLOT_REG_OFF(id, 0), 0, 512 * 4);

    memset(&fr, 0, sizeof(fr));
    fr.frame_num = gopn;
    enc->qp = ax_rate_prepare(&enc->rate, &next_rate,
                              hevc ? ENC_CODEC_HEVC : ENC_CODEC_H264,
                              enc->geom.w, enc->geom.h,
                              enc->placeholder ? 5 : ax_nominal_fps(enc->bound->fps),
                              enc->gop, is_idr);
    fr.qp = enc->qp;
    fr.pic_init_qp = enc->rate.initial_qp;
    fr.codec = hevc ? ENC_CODEC_HEVC : ENC_CODEC_H264;
    /* Frame-level feedback selects QP; the hardware writes its slice delta
     * against the stable PPS QP in the upper sw7 field. */
    fr.rc_mode = ENC_RC_FIXQP;
    fr.input_phys = input_phys;
    /* Pack parameter sets immediately before the hardware slice. The ABI
     * borrows one AU until the next encoder call, so the existing floorplan
     * output is sufficient; no separate DMA ring or heap copy is needed. */
    fr.output_phys = (uint32_t)enc->fb.bus_addr + enc->geom.off_out + AX_AU_HEADROOM;
    slot = enc->cmd + (size_t)id * (enc->mem.cmd_unit_size / 4);
    ex.cmdbuf_size = vcenc_build_encode_cmdbuf(slot, (uint32_t)enc->fb.bus_addr,
                                               &enc->geom, core_dst, &fr);
    ex.numa_id = 0;
    if (!ex.cmdbuf_size) {
        wid = id;
        ioctl(enc->venc_fd, HANTRO_IOCH_RELEASE_CMDBUF, &wid);
        return -1;
    }
    if (ioctl(enc->venc_fd, HANTRO_IOCH_LINK_RUN_CMDBUF, &ex) < 0) {
        wid = id;
        ioctl(enc->venc_fd, HANTRO_IOCH_RELEASE_CMDBUF, &wid);
        return -1;
    }
    wid = id;
    t0 = ax_now_ns();
    if (ioctl(enc->venc_fd, HANTRO_IOCH_WAIT_CMDBUF, &wid) < 0) {
        uint16_t rid = id;
        ioctl(enc->venc_fd, HANTRO_IOCH_RELEASE_CMDBUF, &rid);
        return -1;
    }
    /* Preserve this AU's observed hardware completion through publication. */
    enc->packet_pts = ax_now_ns();
    enc->encode_ns = enc->packet_pts - t0;

    regs = (volatile uint32_t *)(enc->status + STATUS_SLOT_REG_OFF(id, 0));
    swreg9 = regs[9];
    swreg82 = regs[82];
    slice = enc->fb_map + enc->geom.off_out + AX_AU_HEADROOM;
    nut = swreg9 > (hevc ? 5u : 4u)
        ? (hevc ? (slice[4] >> 1) & 0x3f : slice[4] & 0x1f) : -1;
    {
        uint16_t rid = id;
        ioctl(enc->venc_fd, HANTRO_IOCH_RELEASE_CMDBUF, &rid);
    }
    if (wid != CMDBUF_EXE_STATUS_OK || swreg82 == 0 || swreg9 < 5 ||
        swreg9 > enc->geom.out_limit ||
        (is_idr ? nut != (hevc ? 19 : 5) : nut != 1)) {
        fprintf(stderr,
                "ax630c: encode reject idr=%d wait=%u bytes=%u cycles=%u nal=%d\n",
                is_idr, wid, swreg9, swreg82, nut);
        return -1;
    }

    if (is_idr) {
        uint8_t hdr[AX_AU_HEADROOM];
        hlen = ax_write_parameter_sets(enc, hdr);
        if (hlen > AX_AU_HEADROOM)
            return -1;
        memcpy((uint8_t *)slice - hlen, hdr, hlen);
    }
    enc->publish = (uint8_t *)slice - hlen;
    enc->packet_len = hlen + swreg9;
    enc->packet_key = is_idr ? 1u : 0u;
    ax_rate_commit(&enc->rate, &next_rate, enc->packet_len);
    if (is_idr) {
        /* Published only after a successful AU with our actual SPS/VUI.
         * This describes the encoded stream, never the HDMI input depth. */
        uint32_t range = VCENC_VIDEO_FULL_RANGE
            ? ONEKVM_VIDEO_COLOR_RANGE_FULL : ONEKVM_VIDEO_COLOR_RANGE_LIMITED;
        atomic_store(&enc->color_info, VCENC_COMPONENT_BITS | (range << 8));
    }
    return 0;
}

static int ax_grab_frame_budget(struct ax_source *src, const uint8_t **data,
                                uint32_t *stride, char *error, uint32_t error_cap,
                                uint32_t timeout_ms)
{
    struct pollfd pfd[2];
    struct v4l2_buffer buf;
    int rc;
    uint64_t t0;

    ax_collect_display_job(src, 0, 1);
    if (atomic_load(&src->interrupted)) {
        ax_err(error, error_cap, "capture interrupted");
        return -1;
    }
    pfd[0].fd = src->cap_fd;
    pfd[0].events = POLLIN;
    pfd[0].revents = 0;
    pfd[1].fd = src->wake_fd;
    pfd[1].events = POLLIN;
    pfd[1].revents = 0;
    t0 = ax_now_ns();
    /* Three frame times. A dead DMA ring must not sit for a full second. */
    {
        unsigned fps = src->fps >= 10 && src->fps <= 240 ? src->fps : 60;
        unsigned budget = (3000u + fps - 1u) / fps;

        if (budget < 40)
            budget = 40;
        if (timeout_ms && budget > timeout_ms)
            budget = timeout_ms;
        rc = poll(pfd, src->wake_fd >= 0 ? 2 : 1, (int)budget);
    }
    src->capture_ns = ax_now_ns() - t0;
    if (rc < 0) {
        if (errno == EINTR)
            return 1;
        ax_err_errno(error, error_cap, "poll capture");
        return -1;
    }
    if (atomic_load(&src->interrupted) || (pfd[1].revents & POLLIN)) {
        uint64_t junk;
        if (src->wake_fd >= 0)
            read(src->wake_fd, &junk, sizeof(junk));
        ax_err(error, error_cap, "capture interrupted");
        return -1;
    }
    if (rc == 0)
        return 2;
    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    if (ioctl(src->cap_fd, VIDIOC_DQBUF, &buf) < 0) {
        if (errno == EAGAIN)
            return 1;
        ax_err_errno(error, error_cap, "VIDIOC_DQBUF");
        return -1;
    }
    if (buf.index >= src->nbufs || !src->map[buf.index]) {
        ax_err(error, error_cap, "capture buffer index");
        if (ioctl(src->cap_fd, VIDIOC_QBUF, &buf) < 0)
            ax_err_errno(error, error_cap, "VIDIOC_QBUF");
        return -1;
    }
    /* Leave the buffer dequeued. The caller copies it, then ax_requeue. */
    src->held_index = buf.index;
    src->held = 1;
    *data = src->map[buf.index];
    *stride = src->bytesperline ? src->bytesperline : src->width * 2;
    return 0;
}

static int ax_grab_frame(struct ax_source *src, const uint8_t **data,
                         uint32_t *stride, char *error, uint32_t error_cap)
{
    return ax_grab_frame_budget(src, data, stride, error, error_cap, 0);
}

/* VIN is nonblocking. Discard only completed raw frames, before either the
 * encoder or preview borrows them. Bound the drain to one capture pool so a
 * fast producer cannot keep this thread in the loop indefinitely. */
static int ax_latest_capture(struct ax_source *src, const uint8_t **data,
                              char *error, uint32_t error_cap)
{
    for (unsigned i = 1; i < src->nbufs; i++) {
        struct v4l2_buffer buf = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_MMAP,
        };
        if (ioctl(src->cap_fd, VIDIOC_DQBUF, &buf) < 0) {
            if (errno == EAGAIN || errno == EINTR) return 0;
            ax_err_errno(error, error_cap, "drain capture");
            ax_requeue(src);
            return -1;
        }
        if (buf.index >= src->nbufs || !src->map[buf.index] ||
            buf.index == src->held_index) {
            ax_err(error, error_cap, "drain capture buffer index");
            if (buf.index < src->nbufs && buf.index != src->held_index)
                ax_queue_capture_index(src, buf.index);
            ax_requeue(src);
            return -1;
        }
        ax_requeue(src);
        src->held = 1;
        src->held_index = buf.index;
        *data = src->map[buf.index];
    }
    return 0;
}

/* At very low video rates, drain VIN while waiting so a full DONE queue cannot
 * freeze the newest available frame for most of a one-second interval. Also
 * service snapshot/small-display demand without running VENC for these frames. */
static void ax_service_auxiliary(struct ax_source *src)
{
    if (pthread_mutex_trylock(&src->lock) != 0)
        return;
    if (src->streaming && !atomic_load(&src->interrupted)) {
        const uint8_t *frame;
        uint32_t stride;
        char error[128];
        if (ax_grab_frame_budget(src, &frame, &stride, error, sizeof(error), 1) == 0) {
            if (ax_latest_capture(src, &frame, error, sizeof(error)) == 0)
                ax_schedule_display(src, stride);
            ax_requeue(src);
        }
    }
    pthread_mutex_unlock(&src->lock);
}

/* Wait without VIN locked. Source interruption wakes this wait immediately,
 * including low configured frame rates. Do not report pacing as capture time. */
static int ax_wait_encode_slot(struct ax_encoder *enc, struct ax_source *src,
                               char *error, uint32_t error_cap)
{
    uint64_t deadline = ax_pacer_deadline(&enc->pacer, enc->fps,
                                         ax_nominal_fps(src->fps));
    while (deadline && ax_now_ns() < deadline) {
        if (atomic_load(&src->interrupted)) return -1;
        uint64_t now = ax_now_ns();
        if (now >= deadline) break;
        if (enc->fps < 15) ax_service_auxiliary(src);
        now = ax_now_ns();
        if (now >= deadline) break;
        int wait_ms = (int)((deadline - now + 999999) / 1000000);
        if (enc->fps < 15 && wait_ms > 20) wait_ms = 20;
        struct pollfd wake = {.fd = src->wake_fd, .events = POLLIN};
        int rc = poll(&wake, src->wake_fd >= 0 ? 1 : 0, wait_ms);
        if (rc < 0 && errno != EINTR) {
            ax_err_errno(error, error_cap, "wait video cadence");
            return -1;
        }
        if (rc > 0 && (wake.revents & POLLIN)) {
            uint64_t junk;
            if (src->wake_fd >= 0) read(src->wake_fd, &junk, sizeof(junk));
            ax_err(error, error_cap, "capture interrupted");
            return -1;
        }
    }
    return atomic_load(&src->interrupted) ? -1 : 0;
}

static void ax_packet_duration(struct ax_encoder *enc, struct ax_source *src,
                               struct onekvm_video_packet_v1 *packet)
{
#ifdef ONEKVM_VIDEO_PACKET_V1_BASE_SIZE
    /* Optional ABI tail; deployed older Core callers still pass the base size. */
    if (packet->struct_size >= offsetof(struct onekvm_video_packet_v1, duration_ns) +
                               sizeof(packet->duration_ns)) {
        uint32_t input = enc->placeholder ? 5 : ax_nominal_fps(src->fps);
        uint32_t fps = input < enc->fps ? input : enc->fps;
        packet->duration_ns = 1000000000ull / (fps ? fps : 60);
    }
#else
    (void)enc; (void)src; (void)packet;
#endif
}

static void ax_requeue(struct ax_source *src)
{
    ax_collect_display_job(src, 0, 1);
    if (!src->held || src->cap_fd < 0)
        return;
    if ((int)src->held_index != src->display_job_index)
        ax_queue_capture_index(src, src->held_index);
    src->held = 0;
}

static void ax_release_imports(struct ax_source *src)
{
    unsigned int i;

    if (!src)
        return;
    for (i = 0; i < AX_MAX_BUFS; i++) {
        if (src->dma_phys[i] && src->import_venc_fd >= 0) {
            struct dmabuf_import_parameter im;

            memset(&im, 0, sizeof(im));
            im.bus_addr = src->dma_phys[i];
            ioctl(src->import_venc_fd, HANTRO_IOCH_RELEASE_DMABUF, &im);
        }
        src->dma_phys[i] = 0;
    }
    src->import_venc_fd = -1;
}

static int ax_import_capture(struct ax_source *src, int venc_fd)
{
    unsigned int i;
    uint32_t need;

    ax_release_imports(src);
    if (venc_fd < 0 || !src->streaming || !src->nbufs)
        return -1;
    /* The encoder reads packed YUYV at stride width*2 (sw13 = +2WH). */
    if (src->bytesperline && src->bytesperline != src->width * 2)
        return -1;
    need = src->width * src->height * 2;
    src->import_venc_fd = venc_fd;
    for (i = 0; i < src->nbufs; i++) {
        struct v4l2_exportbuffer exp;
        struct dmabuf_import_parameter im;

        memset(&exp, 0, sizeof(exp));
        exp.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        exp.index = i;
        exp.flags = O_CLOEXEC;
        if (ioctl(src->cap_fd, VIDIOC_EXPBUF, &exp) < 0)
            goto fail;
        memset(&im, 0, sizeof(im));
        im.fd = exp.fd;
        if (ioctl(venc_fd, HANTRO_IOCH_IMPORT_DMABUF, &im) < 0) {
            close(exp.fd);
            goto fail;
        }
        close(exp.fd);
        if (im.size < need || im.bus_addr > 0xfffffffful) {
            struct dmabuf_import_parameter rel;

            memset(&rel, 0, sizeof(rel));
            rel.bus_addr = im.bus_addr;
            ioctl(venc_fd, HANTRO_IOCH_RELEASE_DMABUF, &rel);
            errno = EINVAL;
            goto fail;
        }
        src->dma_phys[i] = (uint32_t)im.bus_addr;
    }
    fprintf(stderr, "ax630c: zero-copy YUYV %u buffers %ux%u\n",
            src->nbufs, src->width, src->height);
    return 0;
fail:
    fprintf(stderr, "ax630c: capture dma-buf import failed (%s), copying frames\n",
            strerror(errno));
    ax_release_imports(src);
    return -1;
}

/* No encoded producer: serialize with display-only capture. Temporary VIN
 * capture is fully stopped before dropping the lock; a new viewer can then
 * bind normally, with no cleanup race against its capture generation. */
static void ax_pump_snapshot(struct ax_source *src, uint64_t deadline,
                             char *error, uint32_t error_cap)
{
    uint64_t encoded = atomic_load(&src->encoded_requested_ms);
    if ((encoded && ax_now_ms() - encoded < 2000) ||
        pthread_mutex_trylock(&src->lock) != 0)
        return;
    encoded = atomic_load(&src->encoded_requested_ms);
    if ((encoded && ax_now_ms() - encoded < 2000) ||
        !atomic_load(&src->snapshot_pending) || atomic_load(&src->interrupted)) {
        pthread_mutex_unlock(&src->lock);
        return;
    }
    int temporary = !src->streaming;
    if (temporary) {
        uint32_t w, h, fps;
        if (ax_hdmi_mode(&w, &h, &fps) != 0 ||
            ax_capture_start(src, w, h, error, error_cap) != 0) {
            pthread_mutex_unlock(&src->lock);
            return;
        }
        src->fps = fps;
    }
    do {
        uint64_t now = ax_now_ms();
        if (now >= deadline) break;
        const uint8_t *frame;
        uint32_t stride;
        int rc = ax_grab_frame_budget(src, &frame, &stride, error, error_cap,
                                      (uint32_t)(deadline - now));
        if (rc == 0) {
            ax_capture_snapshot(src, src->held_index, stride, src->width,
                                 src->height, src->output_resolution,
                                 atomic_load(&src->snapshot_generation));
            ax_requeue(src);
            break;
        }
        if (rc < 0) break;
    } while (temporary && atomic_load(&src->snapshot_pending));
    if (temporary) ax_capture_stop(src);
    pthread_mutex_unlock(&src->lock);
}

static int32_t ax_source_snapshot(void *source,
    const struct onekvm_video_snapshot_request_v1 *request,
    uint8_t *data, uint64_t capacity,
    struct onekvm_video_snapshot_result_v1 *result, char *error, uint32_t error_cap)
{
    struct ax_source *src = source;
    if (!src || !request || request->struct_size < sizeof(*request) ||
        !result || result->struct_size < sizeof(*result) || !data ||
        capacity > SIZE_MAX || request->flags || request->width < 0 ||
        request->height < 0 || !!request->width != !!request->height ||
        request->width > 4096 || request->height > 2400 || (request->width & 1) ||
        !isfinite(request->quality_factor) || request->quality_factor < 0 ||
        request->quality_factor > 1 || request->timeout_ms > 1000) {
        ax_err(error, error_cap, "invalid snapshot request");
        return ONEKVM_VIDEO_RESOURCE_INVALID;
    }
    if (!capacity) return ONEKVM_VIDEO_RESOURCE_BUFFER_TOO_SMALL;
    if (pthread_mutex_trylock(&src->snapshot_gate) != 0)
        return ONEKVM_VIDEO_RESOURCE_BUSY;
    uint64_t deadline = ax_now_ms() + (request->timeout_ms ? request->timeout_ms : 1000);
    pthread_mutex_lock(&src->snapshot_lock);
    src->snapshot_width = request->width; src->snapshot_height = request->height;
    src->snapshot_status = -ETIMEDOUT;
    src->snapshot_frame = (struct onekvm_v4l2_transform_frame){0};
    atomic_fetch_add(&src->snapshot_generation, 1);
    atomic_store(&src->snapshot_pending, 1);
    while (atomic_load(&src->snapshot_pending) && !atomic_load(&src->interrupted)) {
        uint64_t now = ax_now_ms();
        if (now >= deadline) break;
        pthread_mutex_unlock(&src->snapshot_lock);
        ax_pump_snapshot(src, deadline, error, error_cap);
        pthread_mutex_lock(&src->snapshot_lock);
        now = ax_now_ms();
        if (!atomic_load(&src->snapshot_pending) || now >= deadline) break;
        uint64_t until = now + 10 < deadline ? now + 10 : deadline;
        struct timespec ts = {.tv_sec = until / 1000, .tv_nsec = (until % 1000) * 1000000};
        pthread_cond_timedwait(&src->snapshot_cv, &src->snapshot_lock, &ts);
    }
    /* Cancellation and conversion share this lock; the gate retains the
     * TDP output until JPEG finishes and prevents a subsequent conversion. */
    atomic_store(&src->snapshot_pending, 0);
    int rc = src->snapshot_status;
    struct onekvm_v4l2_transform_frame frame = src->snapshot_frame;
    uint32_t w = src->snapshot_width, h = src->snapshot_height;
    uint64_t pts = src->snapshot_pts;
    src->snapshot_frame = (struct onekvm_v4l2_transform_frame){0};
    pthread_mutex_unlock(&src->snapshot_lock);
    size_t size = 0;
    if (!rc) rc = ax_snapshot_jpeg(&src->snapshot_jpeg, frame.data, w, h, frame.stride,
                                  request->quality_factor, data, (size_t)capacity, &size);
    if (frame.data) ax_tdp_snapshot_release(&src->snapshot_hw);
    pthread_mutex_unlock(&src->snapshot_gate);
    if (rc) {
        ax_err(error, error_cap, "snapshot unavailable");
        if (rc == -ENOSPC) return ONEKVM_VIDEO_RESOURCE_BUFFER_TOO_SMALL;
        if (rc == -ENOMEM) return ONEKVM_VIDEO_RESOURCE_NO_MEMORY;
        if (rc == -ETIMEDOUT) return ONEKVM_VIDEO_RESOURCE_TIMEOUT;
        return ONEKVM_VIDEO_RESOURCE_INTERNAL;
    }
    result->data_size = size; result->width = w; result->height = h; result->pts_ns = pts;
    return ONEKVM_VIDEO_RESOURCE_OK;
}

static void ax_source_init(struct ax_source *src)
{
    memset(src, 0, sizeof(*src));
    src->cap_fd = -1;
    src->wake_fd = -1;
    src->import_venc_fd = -1;
    ax_tdp_init(&src->display_hw);
    ax_tdp_init(&src->snapshot_hw);
    pthread_mutex_init(&src->snapshot_gate, NULL);
    pthread_mutex_init(&src->snapshot_lock, NULL);
    atomic_init(&src->snapshot_pending, 0);
    atomic_init(&src->snapshot_generation, 0);
    atomic_init(&src->encoded_requested_ms, 0);
    pthread_mutex_init(&src->lock, NULL);
    pthread_mutex_init(&src->display_lock, NULL);
    pthread_mutex_init(&src->display_job_lock, NULL);
    pthread_cond_init(&src->display_job_cv, NULL);
    src->display_job_index = -1;
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&src->display_cv, &attr);
    pthread_cond_init(&src->snapshot_cv, &attr);
    pthread_condattr_destroy(&attr);
    atomic_init(&src->interrupted, 0);
    atomic_init(&src->display_requested_ms, 0);
    atomic_init(&src->display_published_ms, 0);
    atomic_init(&src->display_attempted_ms, 0);
}

static int32_t ax_source_create(const struct onekvm_video_source_config_v1 *config,
                                void **source, char *error, uint32_t error_cap)
{
    struct ax_source *src;

    if (!config || !source || !config->device || !config->device[0]) {
        ax_err(error, error_cap, "capture device missing");
        return -1;
    }
    src = calloc(1, sizeof(*src));
    if (!src) {
        ax_err(error, error_cap, "out of memory");
        return -1;
    }
    ax_source_init(src);
    snprintf(src->device, sizeof(src->device), "%s", config->device);
    src->output_resolution = config->resolution;
    src->wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (src->wake_fd < 0) {
        ax_err_errno(error, error_cap, "eventfd");
        goto fail;
    }
    int rc = ax_start_display_worker(src);
    if (rc) {
        errno = rc;
        ax_err_errno(error, error_cap, "display worker");
        goto fail;
    }
    *source = src;
    return 0;
fail:
    if (src->wake_fd >= 0)
        close(src->wake_fd);
    pthread_cond_destroy(&src->display_job_cv);
    pthread_mutex_destroy(&src->display_job_lock);
    pthread_cond_destroy(&src->snapshot_cv);
    pthread_mutex_destroy(&src->snapshot_lock);
    pthread_mutex_destroy(&src->snapshot_gate);
    pthread_cond_destroy(&src->display_cv);
    pthread_mutex_destroy(&src->display_lock);
    pthread_mutex_destroy(&src->lock);
    free(src);
    return -1;
}

static int32_t ax_source_reset(void *source,
                               const struct onekvm_video_source_config_v1 *config,
                               char *error, uint32_t error_cap)
{
    struct ax_source *src = source;

    if (!src || !config || !config->device || !config->device[0]) {
        ax_err(error, error_cap, "capture device missing");
        return -1;
    }
    pthread_mutex_lock(&src->lock);
    ax_capture_stop(src);
    snprintf(src->device, sizeof(src->device), "%s", config->device);
    src->output_resolution = config->resolution;
    atomic_store(&src->interrupted, 0);
    atomic_store(&src->encoded_requested_ms, 0);
    pthread_mutex_unlock(&src->lock);
    return 0;
}

static int32_t ax_source_read(void *source, struct onekvm_video_frame_v1 *frame,
                              char *error, uint32_t error_cap)
{
    (void)source;
    (void)frame;
    ax_err(error, error_cap, "ax630c publishes encoded frames on the bound path");
    return -1;
}

static void ax_source_release(void *source, uint64_t token)
{
    (void)source;
    (void)token;
}

static int32_t ax_source_signal_present(void *source)
{
    uint32_t w, h, fps;

    (void)source;
    return ax_hdmi_mode(&w, &h, &fps) == 0 ? 1 : 0;
}

static void ax_source_interrupt(void *source)
{
    struct ax_source *src = source;
    uint64_t one = 1;

    if (!src)
        return;
    atomic_store(&src->interrupted, 1);
    pthread_mutex_lock(&src->display_lock);
    pthread_cond_broadcast(&src->display_cv);
    pthread_mutex_unlock(&src->display_lock);
    if (src->wake_fd >= 0)
        write(src->wake_fd, &one, sizeof(one));
}

static void ax_source_destroy(void *source)
{
    struct ax_source *src = source;

    if (!src)
        return;
    pthread_mutex_lock(&src->lock);
    ax_capture_stop(src);
    ax_stop_display_worker(src);
    pthread_mutex_unlock(&src->lock);
    if (src->wake_fd >= 0)
        close(src->wake_fd);
    ax_tdp_close(&src->display_hw);
    ax_tdp_close(&src->snapshot_hw);
    ax_snapshot_jpeg_destroy(src->snapshot_jpeg);
    pthread_cond_destroy(&src->snapshot_cv);
    pthread_mutex_destroy(&src->snapshot_lock);
    pthread_mutex_destroy(&src->snapshot_gate);
    pthread_cond_destroy(&src->display_cv);
    pthread_cond_destroy(&src->display_job_cv);
    pthread_mutex_destroy(&src->display_job_lock);
    pthread_mutex_destroy(&src->display_lock);
    pthread_mutex_destroy(&src->lock);
    free(src);
}

static int32_t ax_source_input_format(void *source, struct onekvm_video_format_v1 *format)
{
    struct ax_source *src = source;

    if (!src || !format)
        return -1;
    format->width = (int32_t)src->width;
    format->height = (int32_t)src->height;
    format->fps = (int32_t)(src->fps ? src->fps : 0);
    format->pixel_format = ONEKVM_VIDEO_PIXEL_UNKNOWN;
    format->flags = 0;
    return 0;
}

static int32_t ax_source_latency(void *source, struct onekvm_video_latency_v1 *latency)
{
    struct ax_source *src = source;

    if (!src || !latency)
        return -1;
    latency->capture_ns = src->capture_ns;
    latency->encode_ns = 0;
    return 0;
}

static void ax_encoder_close(struct ax_encoder *enc)
{
    ax_encoder_release_framebuf(enc);
    if (enc->cmd && enc->cmd != MAP_FAILED)
        munmap(enc->cmd, enc->mem.cmd_total_size);
    if (enc->status && enc->status != MAP_FAILED)
        munmap(enc->status, enc->mem.status_total_size);
    enc->cmd = NULL;
    enc->status = NULL;
    if (enc->venc_fd >= 0)
        close(enc->venc_fd);
    enc->venc_fd = -1;
    enc->packet_len = 0;
    enc->publish = NULL;
}

/* Opening the device holds the kernel's runtime-PM reference. Keep the
 * configuration object cheap while idle and reopen only on a real bind. */
static int ax_encoder_open(struct ax_encoder *enc, char *error, uint32_t error_cap)
{
    struct config_parameter cfg;

    if (enc->venc_fd >= 0)
        return 0;

    enc->venc_fd = open(VCMD_DEV_NODE, O_RDWR | O_CLOEXEC);
    if (enc->venc_fd < 0) {
        ax_err_errno(error, error_cap, "open " VCMD_DEV_NODE);
        return -1;
    }
    memset(&cfg, 0, sizeof(cfg));
    cfg.module_type = VCMD_TYPE_ENCODER;
    if (ioctl(enc->venc_fd, HANTRO_IOCH_GET_VCMD_PARAMETER, &cfg) < 0) {
        ax_err_errno(error, error_cap, "GET_VCMD_PARAMETER");
        ax_encoder_close(enc);
        return -1;
    }
    memset(&enc->mem, 0, sizeof(enc->mem));
    if (ioctl(enc->venc_fd, HANTRO_IOCH_GET_CMDBUF_PARAMETER, &enc->mem) < 0) {
        ax_err_errno(error, error_cap, "GET_CMDBUF_PARAMETER");
        ax_encoder_close(enc);
        return -1;
    }
    enc->cmd = ax_map_pool(enc->venc_fd, enc->mem.cmd_phy_addr, enc->mem.cmd_total_size);
    enc->status = ax_map_pool(enc->venc_fd, enc->mem.status_phy_addr,
                              enc->mem.status_total_size);
    if (!enc->cmd || !enc->status) {
        ax_err(error, error_cap, "mmap VCMD pools failed");
        ax_encoder_close(enc);
        return -1;
    }
    fprintf(stderr, "ax630c: venc hwid=0x%08x main=0x%x cmd=0x%llx status=0x%llx\n",
            cfg.vcmd_hw_version_id, cfg.submodule_main_addr,
            (unsigned long long)enc->mem.cmd_phy_addr,
            (unsigned long long)enc->mem.status_phy_addr);
    return 0;
}

static int32_t ax_encoder_create(const struct onekvm_video_encoder_config_v1 *config,
                                 void **encoder, char *error, uint32_t error_cap)
{
    struct ax_encoder *enc;

    if (!config || !encoder) {
        ax_err(error, error_cap, "encoder config missing");
        return -1;
    }
    if (config->codec != ONEKVM_VIDEO_CODEC_AUTO &&
        config->codec != ONEKVM_VIDEO_CODEC_H264 &&
        config->codec != ONEKVM_VIDEO_CODEC_H265) {
        ax_err(error, error_cap, "ax630c backend encodes H.264 and H.265");
        return -1;
    }
    enc = calloc(1, sizeof(*enc));
    if (!enc) {
        ax_err(error, error_cap, "out of memory");
        return -1;
    }
    enc->venc_fd = -1;
    atomic_init(&enc->color_info, 0);
    enc->codec = config->codec == ONEKVM_VIDEO_CODEC_H265
        ? ONEKVM_VIDEO_CODEC_H265 : ONEKVM_VIDEO_CODEC_H264;
    if (ax_rate_configure(&enc->rate, config)) {
        free(enc);
        ax_err(error, error_cap, "invalid AX630C rate control configuration");
        return -1;
    }
    enc->qp = enc->rate.initial_qp;
    enc->gop = 30;
    enc->fps = 60;
    if (config->struct_size >= offsetof(struct onekvm_video_encoder_config_v1, gop) +
                                   sizeof(int32_t) &&
        config->gop > 1)
        enc->gop = (uint32_t)config->gop;
    if (config->fps > 0)
        enc->fps = (uint32_t)config->fps;
    *encoder = enc;
    return 0;
}

static int32_t ax_encoder_reset(void *encoder,
                                const struct onekvm_video_encoder_config_v1 *config,
                                char *error, uint32_t error_cap)
{
    struct ax_encoder *enc = encoder;

    if (!enc || !config) {
        ax_err(error, error_cap, "encoder config missing");
        return -1;
    }
    if (config->codec != ONEKVM_VIDEO_CODEC_AUTO &&
        config->codec != ONEKVM_VIDEO_CODEC_H264 &&
        config->codec != ONEKVM_VIDEO_CODEC_H265) {
        ax_err(error, error_cap, "ax630c backend encodes H.264 and H.265");
        return -1;
    }
    if (ax_rate_configure(&enc->rate, config)) {
        ax_err(error, error_cap, "invalid AX630C rate control configuration");
        return -1;
    }
    if (config->gop > 1)
        enc->gop = (uint32_t)config->gop;
    if (config->fps > 0)
        enc->fps = (uint32_t)config->fps;
    enc->qp = enc->rate.initial_qp;
    enc->codec = config->codec == ONEKVM_VIDEO_CODEC_H265
        ? ONEKVM_VIDEO_CODEC_H265 : ONEKVM_VIDEO_CODEC_H264;
    enc->force_idr = 1;
    enc->pacer = (struct ax_pacer){0};
    atomic_store(&enc->color_info, 0);
    return 0;
}

static int32_t ax_encoder_encode(void *encoder, const struct onekvm_video_frame_v1 *frame,
                                 struct onekvm_video_packet_v1 *packet,
                                 char *error, uint32_t error_cap)
{
    (void)encoder;
    (void)frame;
    (void)packet;
    ax_err(error, error_cap, "ax630c encodes from the bound HDMI source");
    return -1;
}

static int32_t ax_encoder_set_quality(void *encoder, double quality_factor,
                                      char *error, uint32_t error_cap)
{
    struct ax_encoder *enc = encoder;

    if (!enc || ax_rate_set_quality(&enc->rate, quality_factor)) {
        ax_err(error, error_cap, "invalid AX630C quality factor");
        return -1;
    }
    return 0;
}

static int32_t ax_encoder_request_keyframe(void *encoder, char *error, uint32_t error_cap)
{
    struct ax_encoder *enc = encoder;

    (void)error;
    (void)error_cap;
    if (!enc)
        return -1;
    enc->force_idr = 1;
    return 0;
}

static uint32_t ax_encoder_codec(void *encoder)
{
    const struct ax_encoder *enc = encoder;

    return enc ? enc->codec : ONEKVM_VIDEO_CODEC_H264;
}

static void ax_encoder_destroy(void *encoder)
{
    struct ax_encoder *enc = encoder;

    if (!enc)
        return;
    enc->bound = NULL;
    ax_encoder_close(enc);
    free(enc);
}

/* Keep the encoder bound while HDMI is absent. The placeholder is encoded by
 * the same VC8000E channel as live capture, with a fresh IDR on transition. */
static int ax_enter_placeholder(struct ax_encoder *enc, struct ax_source *src,
                                char *error, uint32_t error_cap)
{
    uint32_t width, height;

    pthread_mutex_lock(&src->lock);
    if (enc->placeholder && enc->bound == src && enc->have_geom) {
        pthread_mutex_unlock(&src->lock);
        return 0;
    }
    ax_output_size(src->output_resolution,
                   enc->have_geom ? enc->geom.w : AX_PLACEHOLDER_W,
                   enc->have_geom ? enc->geom.h : AX_PLACEHOLDER_H,
                   &width, &height);
    ax_capture_stop(src);
    if (!enc->have_geom || enc->geom.w != width || enc->geom.h != height) {
        if (ax_encoder_alloc_framebuf(enc, width, height, error, error_cap) != 0) {
            pthread_mutex_unlock(&src->lock);
            return -1;
        }
    }
    if (ax_render_no_signal_yuyv(enc->fb_map + enc->geom.off_in,
                                  enc->fb.size - enc->geom.off_in,
                                  width, height, width * 2) != 0) {
        ax_err(error, error_cap, "render no-signal frame failed");
        pthread_mutex_unlock(&src->lock);
        return -1;
    }
    pthread_mutex_lock(&src->display_lock);
    src->display_placeholder = ax_display_convert(src->display_rgb,
        AX_DISPLAY_W, AX_DISPLAY_H, enc->fb_map + enc->geom.off_in,
        enc->fb.size - enc->geom.off_in, width * 2, width, height) == 0;
    src->display_pts = ax_now_ns();
    src->display_seq++;
    pthread_cond_broadcast(&src->display_cv);
    pthread_mutex_unlock(&src->display_lock);
    enc->bound = src;
    enc->placeholder = 1;
    enc->early_capture = 0;
    enc->early_frame_seen = 0;
    enc->last_placeholder_ms = 0;
    enc->pacer = (struct ax_pacer){0};
    enc->seq = 0;
    enc->force_idr = 1;
    fprintf(stderr, "ax630c: no HDMI signal, encoding %ux%u placeholder\n",
            width, height);
    pthread_mutex_unlock(&src->lock);
    return 0;
}

static void ax_encoder_release_packet(void *encoder)
{
    (void)encoder;
}

/* Called with src->lock held, for both the published mode and an RX mode
 * confirmed while the old VIN has stopped delivering frames. */
static int ax_bind_capture_locked(struct ax_encoder *enc, struct ax_source *src,
                                  uint32_t w, uint32_t h, uint32_t fps,
                                  int early, char *error, uint32_t error_cap)
{
    uint32_t output_w, output_h;
    uint32_t previous_w = src->width, previous_h = src->height;

    atomic_store(&src->interrupted, 0);
    if (ax_capture_start(src, w, h, error, error_cap) != 0)
        return -1;
    src->fps = fps;
    ax_output_size(src->output_resolution, src->width, src->height,
                   &output_w, &output_h);
    if (!enc->have_geom || enc->geom.w != output_w || enc->geom.h != output_h) {
        if (ax_encoder_alloc_framebuf(enc, output_w, output_h, error, error_cap) != 0) {
            ax_capture_stop(src);
            return -1;
        }
    }
    /* Import is best-effort. A failure leaves dma_phys clear and the
     * read path copies the frame instead. */
    if (output_w == src->width && output_h == src->height)
        ax_import_capture(src, enc->venc_fd);
    enc->bound = src;
    enc->placeholder = 0;
    enc->early_capture = early;
    enc->early_frame_seen = 0;
    enc->early_started_ms = early ? ax_now_ms() : 0;
    enc->early_prev_w = early ? previous_w : 0;
    enc->early_prev_h = early ? previous_h : 0;
    enc->bound_w = src->width;
    enc->bound_h = src->height;
    enc->seq = 0;
    enc->force_idr = 1;
    enc->pacer = (struct ax_pacer){0};
    fprintf(stderr, "ax630c: HDMI %s %ux%u @ %u, capture %ux%u stride %u, output %ux%u\n",
            early ? "prearmed" : "locked", w, h, fps, src->width, src->height, src->bytesperline,
            output_w, output_h);
    return 0;
}

static int32_t ax_encoder_bind_source(void *encoder, void *source,
                                      char *error, uint32_t error_cap)
{
    struct ax_encoder *enc = encoder;
    struct ax_source *src = source;
    uint32_t w = 0, h = 0, fps = 0;
    uint64_t now;
    int rc;

    if (!enc || !src) {
        ax_err(error, error_cap, "encoder or source missing");
        return -1;
    }
    if (ax_encoder_open(enc, error, error_cap) != 0)
        return -1;
    now = ax_now_ms();
    if (enc->bound == src && src->streaming && enc->have_geom &&
        now - enc->last_mode_ms < AX_MODE_POLL_INTERVAL_MS)
        return 0;
    enc->last_mode_ms = now;
    if (ax_hdmi_mode(&w, &h, &fps) != 0) {
        if (enc->bound == src && src->streaming) {
            if (!enc->signal_loss_since_ms)
                enc->signal_loss_since_ms = now;
            if (now - enc->signal_loss_since_ms < AX_SIGNAL_LOSS_GRACE_MS)
                return 0;
        }
        return ax_enter_placeholder(enc, src, error, error_cap);
    }
    enc->signal_loss_since_ms = 0;
    pthread_mutex_lock(&src->lock);
    if (enc->early_capture &&
        (w != src->width || h != src->height) &&
        w == enc->early_prev_w && h == enc->early_prev_h &&
        now - enc->early_started_ms < 1000) {
        /* Only the old published mode may linger after RX has switched. */
        pthread_mutex_unlock(&src->lock);
        return 0;
    }
    if (enc->bound == src && src->streaming && enc->have_geom &&
        src->width == w && src->height == h) {
        src->fps = fps;
        enc->bound_w = w;
        enc->bound_h = h;
        if (enc->early_capture) {
            if (enc->early_frame_seen) {
                enc->early_capture = 0;
            } else {
                /* A published mode with no VIN frame needs a fresh DMA ring.
                 * The RX size alone can precede usable CSI on some modes. */
                fprintf(stderr, "ax630c: prearmed VIN stalled at %ux%u, restarting DMA ring\n",
                        w, h);
                if (ax_capture_start(src, w, h, error, error_cap) != 0) {
                    pthread_mutex_unlock(&src->lock);
                    return -1;
                }
                ax_import_capture(src, enc->venc_fd);
                enc->force_idr = 1;
                enc->early_capture = 0;
            }
        }
        pthread_mutex_unlock(&src->lock);
        return 0;
    }
    rc = ax_bind_capture_locked(enc, src, w, h, fps, 0, error, error_cap);
    pthread_mutex_unlock(&src->lock);
    return rc;
}

static int32_t ax_encoder_read_packet(void *encoder, struct onekvm_video_packet_v1 *packet,
                                      char *error, uint32_t error_cap)
{
    struct ax_encoder *enc = encoder;
    struct ax_source *src;
    const uint8_t *frame = NULL;
    uint32_t stride = 0;
    uint32_t gopn;
    int is_idr;
    int rc;

    if (!enc || !packet) {
        ax_err(error, error_cap, "encoder missing");
        return -1;
    }
    src = enc->bound;
    if (!src || !enc->have_geom) {
        ax_err(error, error_cap, "encoder is not bound to a video source");
        return -1;
    }
    atomic_store(&src->encoded_requested_ms, ax_now_ms());
    if (enc->placeholder) {
        uint64_t now = ax_now_ms();

        uint32_t placeholder_fps = enc->fps < 5 ? enc->fps : 5;
        uint32_t interval_ms = 1000u / (placeholder_fps ? placeholder_fps : 5);
        if (enc->last_placeholder_ms &&
            now - enc->last_placeholder_ms < interval_ms) {
            struct pollfd wake = { .fd = src->wake_fd, .events = POLLIN };
            int remaining = (int)(interval_ms -
                                  (now - enc->last_placeholder_ms));

            poll(&wake, src->wake_fd >= 0 ? 1 : 0, remaining);
        }
        if (atomic_load(&src->interrupted)) {
            ax_err(error, error_cap, "capture interrupted");
            return -1;
        }
        gopn = enc->gop > 1 ? enc->seq % enc->gop : enc->seq % 30u;
        if (enc->force_idr) {
            gopn = 0;
            enc->force_idr = 0;
            enc->seq = 0;
        }
        is_idr = gopn == 0;
        ax_publish_display(src, enc->fb_map + enc->geom.off_in,
                           (size_t)enc->geom.w * enc->geom.h * 2u,
                           enc->geom.w * 2u, enc->geom.w, enc->geom.h, -1);
        rc = ax_encode_yuyv(enc, NULL, 0,
                            (uint32_t)enc->fb.bus_addr + enc->geom.off_in,
                            is_idr, gopn);
        if (rc != 0) {
            ax_err(error, error_cap, "VC8000E placeholder encode failed");
            return -1;
        }
        enc->last_placeholder_ms = ax_now_ms();
        goto encoded;
    }
    if (!src->streaming) {
        ax_err(error, error_cap, "capture is not streaming");
        return -1;
    }
    if (ax_wait_encode_slot(enc, src, error, error_cap) != 0)
        return -1;
    pthread_mutex_lock(&src->lock);
    rc = ax_grab_frame(src, &frame, &stride, error, error_cap);
    if (rc == 2 && src->width >= 64 && src->height >= 64) {
        uint32_t mode_w, mode_h, mode_fps;
        uint32_t rx_w, rx_h;
        int rx_probe = ax_confirm_new_rx_geometry(enc, src, &rx_w, &rx_h);

        if (rx_probe == 0) {
            /* RX geometry is only a candidate. Keep waiting for a real VIN
             * frame; a successful STREAMON alone is not signal proof. */
            if (ax_bind_capture_locked(enc, src, rx_w, rx_h,
                                       src->fps ? src->fps : 60, 1,
                                       error, error_cap) == 0)
                rc = ax_grab_frame(src, &frame, &stride, error, error_cap);
            else
                rc = -1;
        } else if (enc->early_capture || rx_probe == 1) {
            /* Do not tear down the newly armed or soon-to-change DMA ring. */
        } else {
            /* A missing HDMI mode is handled by the placeholder after the
             * signal-loss grace; do not rebuild VIN on every retry. */
            if (ax_hdmi_mode(&mode_w, &mode_h, &mode_fps) == 0) {
                if (mode_w != src->width || mode_h != src->height) {
                    /* Published geometry has changed: skip the old ring. */
                    pthread_mutex_unlock(&src->lock);
                    enc->last_mode_ms = 0;
                    if (ax_encoder_bind_source(enc, src, error, error_cap) != 0)
                        return -1;
                    pthread_mutex_lock(&src->lock);
                    rc = ax_grab_frame(src, &frame, &stride, error, error_cap);
                } else {
                    fprintf(stderr, "ax630c: capture stalled, restarting DMA ring\n");
                    if (ax_capture_start(src, src->width, src->height, error, error_cap) == 0) {
                        ax_import_capture(src, enc->venc_fd);
                        enc->force_idr = 1;
                        rc = ax_grab_frame(src, &frame, &stride, error, error_cap);
                    }
                }
            }
        }
    }
    if (rc == 0)
        rc = ax_latest_capture(src, &frame, error, error_cap);
    if (rc != 0) {
        pthread_mutex_unlock(&src->lock);
        return -1;
    }
    ax_pacer_advance(&enc->pacer, ax_now_ns());
    ax_schedule_display(src, stride);
    if (enc->early_capture) {
        enc->early_frame_seen = 1;
        enc->signal_loss_since_ms = 0;
    }
    if (enc->gop < 2)
        enc->gop = 30;
    gopn = enc->seq % enc->gop;
    if (enc->force_idr) {
        gopn = 0;
        enc->force_idr = 0;
        enc->seq = 0;
    }
    is_idr = gopn == 0;

    /*
     * Zero-copy: the VPU DMA-reads the still-dequeued capture buffer.
     * Returning it first would let VIN overwrite the frame mid-encode.
     */
    if (enc->geom.w != src->width || enc->geom.h != src->height) {
        ax_scale_yuyv(enc->fb_map + enc->geom.off_in,
                      enc->geom.w, enc->geom.h, frame, stride,
                      src->width, src->height);
        rc = ax_encode_yuyv(enc, NULL, 0,
                            (uint32_t)enc->fb.bus_addr + enc->geom.off_in,
                            is_idr, gopn);
        ax_requeue(src);
        pthread_mutex_unlock(&src->lock);
        if (rc != 0) {
            ax_err(error, error_cap, "VC8000E scaled encode failed");
            return -1;
        }
        goto encoded;
    }
    if (src->held && src->dma_phys[src->held_index] &&
        (!src->bytesperline || src->bytesperline == src->width * 2)) {
        uint32_t phys = src->dma_phys[src->held_index];

        rc = ax_encode_yuyv(enc, NULL, stride, phys, is_idr, gopn);
        ax_requeue(src);
        pthread_mutex_unlock(&src->lock);
        if (rc != 0) {
            ax_err(error, error_cap, "VC8000E encode failed");
            return -1;
        }
        goto encoded;
    }

    /* Failed DMA import / padded capture stride: copy once into the fixed
     * encoder input while VIN is held, then release VIN before hardware wait. */
    size_t row = (size_t)enc->geom.w * 2;
    size_t required = (size_t)(enc->geom.h - 1) * stride + row;
    if (stride < row || !src->held || required > src->map_len[src->held_index]) {
        ax_requeue(src);
        pthread_mutex_unlock(&src->lock);
        ax_err(error, error_cap, "capture frame is too small");
        return -1;
    }
    ax_copy_yuyv(enc->fb_map + enc->geom.off_in, enc->geom.w * 2, frame,
                 stride, enc->geom.w, enc->geom.h);
    ax_requeue(src);
    pthread_mutex_unlock(&src->lock);
    rc = ax_encode_yuyv(enc, NULL, enc->geom.w * 2,
                         (uint32_t)enc->fb.bus_addr + enc->geom.off_in, is_idr, gopn);
    if (rc != 0) {
        ax_err(error, error_cap, "VC8000E encode failed");
        return -1;
    }
encoded:
    enc->seq++;
    packet->data = enc->publish;
    packet->data_size = enc->packet_len;
    packet->codec = enc->codec;
    packet->key_frame = enc->packet_key;
    packet->pts_ns = enc->packet_pts;
    ax_packet_duration(enc, src, packet);
    return 0;
}

static int32_t ax_encoder_unbind_source(void *encoder, char *error, uint32_t error_cap)
{
    struct ax_encoder *enc = encoder;

    (void)error;
    (void)error_cap;
    if (!enc)
        return -1;
    if (enc->bound) {
        pthread_mutex_lock(&enc->bound->lock);
        atomic_store(&enc->bound->encoded_requested_ms, 0);
        ax_capture_stop(enc->bound);
        pthread_mutex_unlock(&enc->bound->lock);
        enc->bound = NULL;
    }
    /* The video loop serializes reads/unbind. Its borrowed packet is no
     * longer in use here, and capture imports were released before close. */
    ax_encoder_close(enc);
    enc->placeholder = 0;
    enc->early_capture = 0;
    enc->early_frame_seen = 0;
    enc->signal_loss_since_ms = 0;
    enc->last_mode_ms = 0;
    enc->last_placeholder_ms = 0;
    enc->pacer = (struct ax_pacer){0};
    enc->seq = 0;
    enc->force_idr = 1;
    return 0;
}

static int32_t ax_encoder_latency(void *encoder, struct onekvm_video_latency_v1 *latency)
{
    struct ax_encoder *enc = encoder;

    if (!enc || !latency)
        return -1;
    latency->capture_ns = enc->bound ? enc->bound->capture_ns : 0;
    latency->encode_ns = enc->encode_ns;
    return 0;
}

static int32_t ax_encoder_color_info(void *encoder,
                                    struct onekvm_video_color_info_v1 *info)
{
    struct ax_encoder *enc = encoder;
    if (!enc || !info || info->struct_size < sizeof(*info))
        return ONEKVM_VIDEO_RESOURCE_INVALID;
    uint32_t cached = atomic_load(&enc->color_info);
    info->bit_depth = cached & 0xffu;
    info->color_range = cached >> 8;
    return ONEKVM_VIDEO_RESOURCE_OK;
}

static const struct onekvm_video_format_v1 ax_formats[] = {
    {sizeof(struct onekvm_video_format_v1), 1920, 1080, 60, ONEKVM_VIDEO_PIXEL_UNKNOWN, 0},
    {sizeof(struct onekvm_video_format_v1), 1280, 720, 60, ONEKVM_VIDEO_PIXEL_UNKNOWN, 0},
    {sizeof(struct onekvm_video_format_v1), 3840, 2160, 30, ONEKVM_VIDEO_PIXEL_UNKNOWN, 0},
};

static const struct onekvm_video_backend_v1 ax_backend = {
    .struct_size = sizeof(struct onekvm_video_backend_v1),
    .abi_version = ONEKVM_VIDEO_BACKEND_ABI_V1,
    .driver_id = AX_DRIVER_ID,
    .codec_mask = ONEKVM_VIDEO_CODEC_MASK_AUTO | ONEKVM_VIDEO_CODEC_MASK_H264 |
                  ONEKVM_VIDEO_CODEC_MASK_H265,
    .features = ONEKVM_VIDEO_FEATURE_SIGNAL_PRESENT |
                ONEKVM_VIDEO_FEATURE_INTERRUPT_READ |
                ONEKVM_VIDEO_FEATURE_KEYFRAME |
                ONEKVM_VIDEO_FEATURE_BORROWED_PACKET |
                ONEKVM_VIDEO_FEATURE_BOUND_ENCODER |
                ONEKVM_VIDEO_FEATURE_LATENCY |
                ONEKVM_VIDEO_FEATURE_EDID |
                ONEKVM_VIDEO_FEATURE_BOUND_ENCODED_TIME |
                ONEKVM_VIDEO_FEATURE_ENCODER_COLOR_INFO |
                ONEKVM_VIDEO_FEATURE_SOURCE_SNAPSHOT |
                ONEKVM_VIDEO_FEATURE_SOURCE_DISPLAY |
                ONEKVM_VIDEO_FEATURE_SOURCE_DISPLAY_CACHE_ONLY,
    .formats = ax_formats,
    .format_count = (uint32_t)(sizeof(ax_formats) / sizeof(ax_formats[0])),
    .source_create = ax_source_create,
    .source_reset = ax_source_reset,
    .source_read = ax_source_read,
    .source_release = ax_source_release,
    .source_signal_present = ax_source_signal_present,
    .source_interrupt = ax_source_interrupt,
    .source_destroy = ax_source_destroy,
    .encoder_create = ax_encoder_create,
    .encoder_reset = ax_encoder_reset,
    .encoder_encode = ax_encoder_encode,
    .encoder_set_quality = ax_encoder_set_quality,
    .encoder_request_keyframe = ax_encoder_request_keyframe,
    .encoder_codec = ax_encoder_codec,
    .encoder_destroy = ax_encoder_destroy,
    .encoder_release_packet = ax_encoder_release_packet,
    .encoder_prepare = NULL,
    .encoder_bind_source = ax_encoder_bind_source,
    .encoder_read_packet = ax_encoder_read_packet,
    .encoder_unbind_source = ax_encoder_unbind_source,
    .source_input_format = ax_source_input_format,
    .source_latency = ax_source_latency,
    .source_display = ax_source_display,
    .source_snapshot = ax_source_snapshot,
    .encoder_latency = ax_encoder_latency,
    .edid_capabilities = ax_edid_capabilities,
    .edid_get = ax_edid_get,
    .edid_set = ax_edid_set,
    .encoder_color_info = ax_encoder_color_info,
};

ONEKVM_VIDEO_EXPORT int32_t onekvm_video_backend_query(uint32_t requested_abi,
                                                       const void **backend_api)
{
    if (!backend_api)
        return -1;
    *backend_api = NULL;
    if (requested_abi != ONEKVM_VIDEO_BACKEND_ABI_V1)
        return -1;
    *backend_api = &ax_backend;
    return 0;
}

#ifdef AX630C_STANDALONE
int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "/tmp/ax630c.h264";
    int want = argc > 2 ? atoi(argv[2]) : 8;
    struct onekvm_video_source_config_v1 sc;
    struct onekvm_video_encoder_config_v1 ec;
    struct onekvm_video_packet_v1 pkt;
    void *source = NULL;
    void *encoder = NULL;
    char error[256];
    FILE *out;
    int got = 0;
    int spins = 0;

    if (argc > 3 && strcmp(argv[3], "h264") && strcmp(argv[3], "h265")) {
        fprintf(stderr, "usage: %s output frames [h264|h265]\n", argv[0]);
        return 1;
    }

    memset(&sc, 0, sizeof(sc));
    sc.struct_size = sizeof(sc);
    sc.device = AX_CAP_DEV;
    memset(&ec, 0, sizeof(ec));
    ec.struct_size = sizeof(ec);
    ec.codec = argc > 3 && strcmp(argv[3], "h265") == 0
        ? ONEKVM_VIDEO_CODEC_H265 : ONEKVM_VIDEO_CODEC_H264;
    ec.gop = 30;
    ec.fps = 60;
    ec.quality_factor = 0.5;
    if (ax_encoder_create(&ec, &encoder, error, sizeof(error)) != 0) {
        fprintf(stderr, "encoder_create: %s\n", error);
        return 1;
    }
    if (ax_source_create(&sc, &source, error, sizeof(error)) != 0) {
        fprintf(stderr, "source_create: %s\n", error);
        ax_encoder_destroy(encoder);
        return 1;
    }
    out = fopen(path, "wb");
    if (!out) {
        perror(path);
        ax_source_destroy(source);
        ax_encoder_destroy(encoder);
        return 1;
    }
    while (got < want && spins < want * 50) {
        memset(&pkt, 0, sizeof(pkt));
        pkt.struct_size = sizeof(pkt);
        if (ax_encoder_bind_source(encoder, source, error, sizeof(error)) != 0) {
            fprintf(stderr, "bind: %s\n", error);
            usleep(200000);
            spins++;
            continue;
        }
        if (ax_encoder_read_packet(encoder, &pkt, error, sizeof(error)) != 0) {
            fprintf(stderr, "read: %s\n", error);
            usleep(50000);
            spins++;
            continue;
        }
        if (pkt.data && pkt.data_size) {
            fwrite(pkt.data, 1, pkt.data_size, out);
            got++;
        }
    }
    fclose(out);
    fprintf(stderr, "ax630c: wrote %d frames to %s\n", got, path);
    struct onekvm_video_color_info_v1 color = {.struct_size = sizeof(color)};
    if (ax_encoder_color_info(encoder, &color) != 0)
        return 1;
    fprintf(stderr, "ax630c: encoded color depth=%u range=%u\n", color.bit_depth, color.color_range);
    ax_encoder_unbind_source(encoder, error, sizeof(error));
    if (ax_encoder_color_info(encoder, &color) != 0 || color.bit_depth || color.color_range)
        return 1;
    ax_source_destroy(source);
    ax_encoder_destroy(encoder);
    return got > 0 ? 0 : 2;
}
#endif
