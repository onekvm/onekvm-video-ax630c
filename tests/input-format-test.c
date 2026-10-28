/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include <assert.h>
#include "../src/ax630c_video.c"

static const char *mode_width = "1920\n", *mode_height = "1080\n";
static const char *mode_fps = "60\n", *mode_status = "stable\n";
static unsigned reads;
static const char *rx_geometry = "3840 2160\n";
static const char *capture_device;

FILE *__wrap_fopen(const char *path, const char *mode)
{
    const char *data = NULL;
    assert(strcmp(mode, "r") == 0);
    if (strcmp(path, AX_PROC_RX) == 0) data = "access\n";
    else if (strcmp(path, AX_PROC_POWER) == 0) data = "on\n";
    else if (strcmp(path, AX_PROC_STATUS) == 0) data = mode_status;
    else if (strcmp(path, AX_PROC_W) == 0) data = mode_width;
    else if (strcmp(path, AX_PROC_H) == 0) data = mode_height;
    else if (strcmp(path, AX_PROC_FPS) == 0) data = mode_fps;
    else if (strcmp(path, AX_PROC_RX_GEOMETRY) == 0) data = rx_geometry;
    assert(data != NULL);
    reads++;
    return fmemopen((void *)data, strlen(data), "r");
}

int __wrap_open(const char *path, int flags, ...)
{
    if (capture_device) {
        assert(strcmp(path, capture_device) == 0);
        assert(flags & O_NONBLOCK);
        errno = ENOENT;
        return -1;
    }
    assert(!"input metadata must never open a capture/encoder device");
    return -1;
}

static void expect_capture_device(struct ax_source *src, const char *device)
{
    char error[128];
    capture_device = device;
    /* Observe the actual open path without touching a capture device. */
    assert(ax_capture_start(src, 1920, 1080, error, sizeof(error)) == -1);
    assert(strstr(error, "open capture") != NULL);
    assert(src->cap_fd == -1 && !src->streaming);
    capture_device = NULL;
}

static void check_capture_devices(void)
{
    static const struct { const char *config, *expected; } devices[] = {
        {"/dev/video0", "/dev/onekvm-vin"},
        {"/dev/onekvm-vin", "/dev/onekvm-vin"},
        {"/dev/video7", "/dev/video7"},
    };
    for (unsigned i = 0; i < sizeof(devices) / sizeof(devices[0]); i++) {
        struct onekvm_video_source_config_v1 config = {
            .struct_size = sizeof(config), .device = devices[i].config,
        };
        char error[128];
        void *source = NULL;
        assert(ax_source_create(&config, &source, error, sizeof(error)) == 0);
        expect_capture_device(source, devices[i].expected);
        for (unsigned j = 0; j < sizeof(devices) / sizeof(devices[0]); j++) {
            config.device = devices[j].config;
            assert(ax_source_reset(source, &config, error, sizeof(error)) == 0);
            expect_capture_device(source, devices[j].expected);
        }
        ax_source_destroy(source);
    }
}

static void expect_format(struct ax_source *src, int width, int height, int fps)
{
    struct onekvm_video_format_v1 format = {.struct_size = sizeof(format)};
    unsigned before = reads;
    /* Metadata is also queried while display/capture owns the VIN lock. */
    pthread_mutex_lock(&src->lock);
    assert(ax_source_input_format(src, &format) == 0);
    pthread_mutex_unlock(&src->lock);
    assert(format.width == width && format.height == height && format.fps == fps);
    assert(reads == before);
    assert(src->cap_fd == -1 && !src->streaming);
}

int main(void)
{
    check_capture_devices();
    struct ax_source src;
    ax_source_init(&src);
    struct onekvm_video_format_v1 format = {.struct_size = sizeof(format)};
    assert(ax_source_input_format(&src, &format) == -1);

    /* Start and change HDMI while capture stays stopped throughout. */
    assert(ax_source_signal_present(&src) == 1);
    expect_format(&src, 1920, 1080, 60);
    src.width = 1920; src.height = 1080; src.fps = 60;
    src.output_resolution = 720;
    mode_width = "3840\n"; mode_height = "2160\n"; mode_fps = "30\n";
    assert(ax_source_signal_present(&src) == 1);
    expect_format(&src, 3840, 2160, 30);
    assert(src.width == 1920 && src.height == 1080);

    /* Retain the last valid input during transient loss or incomplete timing. */
    mode_status = "disappear\n";
    assert(ax_source_signal_present(&src) == 0);
    expect_format(&src, 3840, 2160, 30);
    mode_status = "new res\n"; mode_width = "0\n";
    assert(ax_source_signal_present(&src) == 0);
    expect_format(&src, 3840, 2160, 30);
    mode_width = "1280\n"; mode_height = "720\n"; mode_fps = "60\n";
    assert(ax_source_signal_present(&src) == 1);
    expect_format(&src, 1280, 720, 60);

    format.struct_size = ONEKVM_VIDEO_FORMAT_V1_BASE_SIZE;
    format.flags = 0xdeadbeef;
    assert(ax_source_input_format(&src, &format) == 0);
    assert(format.flags == 0xdeadbeef);
    format.struct_size--;
    assert(ax_source_input_format(&src, &format) == -1);
    /* RX candidate remains usable even if firmware CSI metadata is stale. */
    struct ax_encoder enc = {0};
    src.width = 1280; src.height = 720;
    uint32_t rw, rh;
    assert(ax_confirm_new_rx_geometry(&enc, &src, &rw, &rh) == 0);
    assert(rw == 3840 && rh == 2160);
    assert(ax_recovery_due(&src, 1000));
    assert(!ax_recovery_due(&src, 1249));
    assert(ax_recovery_due(&src, 1250));
    assert(!ax_recovery_due(&src, 1749));
    /* STREAMON and a single frame must never publish the candidate. */
    src.width = 3840; src.height = 2160; src.bytesperline = 7680;
    struct v4l2_buffer buf = {.sequence = 0, .bytesused = 7680 * 2160, .flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC};
    buf.timestamp.tv_sec = 10;
    ax_observe_capture(&src, &buf);
    assert(!atomic_load(&src.confirmed_mode));
    for (unsigned i = 1; i <= 12; i++) {
        uint64_t us = 10000000ull + i * 1000000ull / 30;
        buf.timestamp.tv_sec = us / 1000000;
        buf.timestamp.tv_usec = us % 1000000;
        buf.sequence = i;
        ax_observe_capture(&src, &buf);
    }
    assert(src.fps == 30 && src.recovery_attempts == 0);
    /* Old CSI metadata cannot overwrite the real VIN-confirmed tuple. */
    atomic_store(&src.streaming, 1);
    assert(ax_source_signal_present(&src) == 1);
    format.struct_size = sizeof(format);
    assert(ax_source_input_format(&src, &format) == 0);
    assert(format.width == 3840 && format.height == 2160 && format.fps == 30);
    buf.flags = V4L2_BUF_FLAG_ERROR;
    buf.sequence = 100;
    ax_observe_capture(&src, &buf);
    assert(src.fps == 30);
    buf.flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
    buf.bytesused = 100;
    ax_observe_capture(&src, &buf);
    assert(src.timing_frames == 0);
    atomic_store(&src.streaming, 0);
    assert(ax_capture_stop(&src) == 0);
    assert(!atomic_load(&src.confirmed_mode));
    puts("input format: idle HDMI changes, loss/recovery, nonblocking queries and ABI bounds passed");
    return 0;
}
