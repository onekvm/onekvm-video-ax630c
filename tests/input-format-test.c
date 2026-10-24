/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include <assert.h>
#include "../src/ax630c_video.c"

static const char *mode_width = "1920\n", *mode_height = "1080\n";
static const char *mode_fps = "60\n", *mode_status = "stable\n";
static unsigned reads;

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
    assert(data != NULL);
    reads++;
    return fmemopen((void *)data, strlen(data), "r");
}

int __wrap_open(const char *path, int flags, ...)
{
    (void)path; (void)flags;
    assert(!"input metadata must never open a capture/encoder device");
    return -1;
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
    puts("input format: idle HDMI changes, loss/recovery, nonblocking queries and ABI bounds passed");
    return 0;
}
