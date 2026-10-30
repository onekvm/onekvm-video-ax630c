/* SPDX-License-Identifier: MIT */
/* Development-only LD_PRELOAD tracer. No extra VIN/VENC consumer or queue. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <linux/videodev2.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include "vcmd_abi.h"

#ifdef __GLIBC__
typedef unsigned long ioctl_request;
#else
typedef int ioctl_request;
#endif

static int (*next_ioctl)(int, ioctl_request, ...);
static pthread_once_t once = PTHREAD_ONCE_INIT;
static _Thread_local uint64_t captured_ns, dequeued_ns, reserved_ns, submitted_ns;
static _Thread_local uint32_t sequence;

static void resolve_ioctl(void)
{
    *(void **)(&next_ioctl) = dlsym(RTLD_NEXT, "ioctl");
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + ts.tv_nsec;
}

__attribute__((visibility("default")))
int ioctl(int fd, ioctl_request request, ...)
{
    va_list args;
    va_start(args, request);
    void *arg = va_arg(args, void *);
    va_end(args);
    pthread_once(&once, resolve_ioctl);
    if (!next_ioctl) { errno = ENOSYS; return -1; }
    uint64_t before = now_ns();
    int result = next_ioctl(fd, request, arg);
    int saved_errno = errno;
    uint64_t after = now_ns();
    uint32_t code = (uint32_t)request;
    if (!result && code == VIDIOC_DQBUF) {
        struct v4l2_buffer *buffer = arg;
        if (buffer->type == V4L2_BUF_TYPE_VIDEO_CAPTURE &&
            (buffer->flags & V4L2_BUF_FLAG_TIMESTAMP_MASK) == V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC) {
            captured_ns = (uint64_t)buffer->timestamp.tv_sec * 1000000000u +
                          (uint64_t)buffer->timestamp.tv_usec * 1000u;
            dequeued_ns = after;
            sequence = buffer->sequence;
        }
    } else if (!result && code == HANTRO_IOCH_RESERVE_CMDBUF) {
        reserved_ns = before;
    } else if (!result && code == HANTRO_IOCH_LINK_RUN_CMDBUF) {
        submitted_ns = before;
    } else if (code == HANTRO_IOCH_WAIT_CMDBUF) {
        if (!result && captured_ns && dequeued_ns >= captured_ns &&
            submitted_ns >= dequeued_ns && after >= submitted_ns)
            fprintf(stderr, "ax-schedule: seq=%u age_us=%llu submit_us=%llu prepare_us=%llu wait_us=%llu complete_us=%llu\n",
                sequence, (unsigned long long)((dequeued_ns - captured_ns) / 1000u),
                (unsigned long long)((submitted_ns - dequeued_ns) / 1000u),
                (unsigned long long)((submitted_ns - reserved_ns) / 1000u),
                (unsigned long long)((after - before) / 1000u),
                (unsigned long long)((after - captured_ns) / 1000u));
        captured_ns = dequeued_ns = 0;
    }
    errno = saved_errno;
    return result;
}
