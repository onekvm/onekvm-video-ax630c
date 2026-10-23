/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include <assert.h>
#include <stdarg.h>
#include "../src/ax630c_video.c"

static unsigned pending[AX_MAX_BUFS], next, count, queued[AX_MAX_BUFS], requeues;
static int never_empty;
int __real_poll(struct pollfd *, nfds_t, int);
int __wrap_poll(struct pollfd *fds, nfds_t nfd, int timeout)
{
    if (nfd && fds[0].fd == 123) {
        fds[0].revents = next < count ? POLLIN : 0;
        return fds[0].revents ? 1 : 0;
    }
    return __real_poll(fds, nfd, timeout);
}
int __wrap_ioctl(int fd, unsigned long request, ...)
{
    assert(fd == 123);
    va_list args; va_start(args, request);
    struct v4l2_buffer *buf = va_arg(args, struct v4l2_buffer *); va_end(args);
    if (request == VIDIOC_DQBUF) {
        if (next >= count && !never_empty) { errno = EAGAIN; return -1; }
        buf->index = never_empty ? (next++ + 1) % AX_MAX_BUFS : pending[next++];
        return 0;
    }
    assert(request == VIDIOC_QBUF && requeues < AX_MAX_BUFS);
    queued[requeues++] = buf->index;
    return 0;
}
struct waiter { struct ax_encoder *enc; struct ax_source *src; int rc; };
static void *wait_slot(void *arg)
{
    struct waiter *waiter = arg;
    char error[64];
    waiter->rc = ax_wait_encode_slot(waiter->enc, waiter->src, error, sizeof(error));
    return NULL;
}
int main(void)
{
    /* A 60 Hz source paced before encoding delivers the requested count,
     * without catch-up bursts after a long stall or reset. */
    for (unsigned fps = 1; fps <= 60; fps++) {
        struct ax_pacer pacer = {0};
        unsigned frames = 0;
        for (uint64_t now = 1000000000; now < 2000000000; now += 1000000) {
            uint64_t deadline = ax_pacer_deadline(&pacer, fps, 60);
            if (!pacer.fps) continue;
            if (!deadline || now >= deadline) { frames++; ax_pacer_advance(&pacer, now); }
        }
        if (fps < 60) assert(frames == fps);
        else assert(frames == 0); /* Native capture keeps its own clock. */
    }
    struct ax_pacer pacer = {0};
    assert(!ax_pacer_deadline(&pacer, 30, 60));
    ax_pacer_advance(&pacer, 1000000000);
    ax_pacer_advance(&pacer, 4000000000);
    assert(pacer.next_ns > 4000000000 && pacer.next_ns < 4034000000);
    assert(!ax_pacer_deadline(&pacer, 60, 60));
    assert(!ax_pacer_deadline(&pacer, 30, 60));
    assert(ax_nominal_fps(59) == 60 && ax_nominal_fps(29) == 30);
    assert(ax_nominal_fps(24) == 24 && ax_nominal_fps(25) == 25);

    struct ax_source src;
    ax_source_init(&src);
    uint8_t frames[AX_MAX_BUFS][8];
    src.streaming = 1; src.cap_fd = 123; src.nbufs = AX_MAX_BUFS; src.fps = 60;
    for (unsigned i = 0; i < AX_MAX_BUFS; i++) src.map[i] = frames[i];
    src.held = 1; src.held_index = 0;
    /* Slot 1 is still borrowed by the preview worker, never queued or drained. */
    src.display_job_index = 1; src.display_job_complete = 0;
    pending[0] = 2; pending[1] = 3; pending[2] = 4; count = 3;
    const uint8_t *frame = frames[0];
    char error[128];
    assert(ax_latest_capture(&src, &frame, error, sizeof(error)) == 0);
    assert(frame == frames[4] && src.held_index == 4 && src.held);
    assert(next == 3 && requeues == 3 && queued[0] == 0 && queued[1] == 2 && queued[2] == 3);
    ax_requeue(&src); assert(queued[3] == 4 && requeues == 4);
    /* The drain remains bounded even if completed buffers keep arriving. */
    src.display_job_index = -1; src.held = 1; src.held_index = 0;
    next = requeues = 0; never_empty = 1;
    assert(ax_latest_capture(&src, &frame, error, sizeof(error)) == 0);
    assert(next == AX_MAX_BUFS - 1 && requeues == AX_MAX_BUFS - 1);
    ax_requeue(&src);
    never_empty = 0;
    /* Invalid driver indices do not leak the buffer already held. */
    src.held = 1; src.held_index = 0; next = requeues = 0;
    pending[0] = AX_MAX_BUFS; count = 1;
    assert(ax_latest_capture(&src, &frame, error, sizeof(error)) < 0);
    assert(!src.held && requeues == 1 && queued[0] == 0);

    /* Even without an RGB consumer, low-FPS waits must keep VIN moving. */
    next = requeues = 0; pending[0] = 1; count = 1;
    ax_service_auxiliary(&src);
    assert(next == 1 && requeues == 1 && queued[0] == 1 && !src.held);
    next = count = requeues = 0;

    struct ax_encoder enc = {.fps = 1};
    enc.pacer.fps = 1; enc.pacer.next_ns = ax_now_ns() + 1000000000;
    src.wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    assert(src.wake_fd >= 0);
    struct waiter waiter = {.enc = &enc, .src = &src};
    pthread_t thread; assert(!pthread_create(&thread, NULL, wait_slot, &waiter));
    usleep(10000);
    assert(!pthread_mutex_trylock(&src.lock)); pthread_mutex_unlock(&src.lock);
    uint64_t start = ax_now_ms(); ax_source_interrupt(&src);
    pthread_join(thread, NULL);
    assert(waiter.rc < 0 && ax_now_ms() - start < 100);
    close(src.wake_fd);
#ifdef ONEKVM_VIDEO_PACKET_V1_BASE_SIZE
    /* Older cores provide only the base packet: never write the new tail. */
    struct onekvm_video_packet_v1 packet = {.struct_size = ONEKVM_VIDEO_PACKET_V1_BASE_SIZE};
    packet.duration_ns = 0xaabbccdd11223344ull;
    ax_packet_duration(&enc, &src, &packet);
    assert(packet.duration_ns == 0xaabbccdd11223344ull);
    packet.struct_size = sizeof(packet); enc.fps = 30; src.fps = 59;
    ax_packet_duration(&enc, &src, &packet); assert(packet.duration_ns == 1000000000ull / 30);
    enc.fps = 60; ax_packet_duration(&enc, &src, &packet);
    assert(packet.duration_ns == 1000000000ull / 60);
    enc.placeholder = 1; ax_packet_duration(&enc, &src, &packet);
    assert(packet.duration_ns == 200000000);
    enc.fps = 1; ax_packet_duration(&enc, &src, &packet);
    assert(packet.duration_ns == 1000000000);
#endif
    puts("pacing/capture: 1-60 FPS, stalled deadlines, latest VIN, borrowed slots, bounded drain, interrupt, old ABI PASS");
    return 0;
}
