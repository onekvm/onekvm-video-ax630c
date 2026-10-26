/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include <assert.h>
#include <stdarg.h>
#include "../src/ax630c_video.c"
static int stops, closes, stop_error;
static atomic_int destroy_started, destroy_finished;
int __wrap_ioctl(int fd, unsigned long request, ...) {
    assert((fd == 123 || fd == 124) && request == VIDIOC_STREAMOFF);
    stops++;
    if (stop_error) { errno = EIO; return -1; }
    return 0;
}
int __wrap_close(int fd) { assert(fd == 123 || fd == 124); closes++; return 0; }
static void *destroy_source(void *source) {
    atomic_store(&destroy_started, 1);
    ax_source_destroy(source);
    atomic_store(&destroy_finished, 1);
    return NULL;
}
int main(void) {
    struct ax_source first, second;
    ax_source_init(&first); ax_source_init(&second);
    first.cap_fd = 123; second.cap_fd = 124;
    first.streaming = second.streaming = 1;
    first.edid_next = &second; ax_edid_sources = &first;
    char error[128];
    second.held = 1;
    assert(ax_edid_capture_begin(error, sizeof(error)) < 0 && !stops && !closes);
    assert(first.streaming && second.streaming);
    second.held = 0;
    assert(!ax_edid_capture_begin(error, sizeof(error)));
    assert(stops == 2 && closes == 2 && !first.streaming && !second.streaming);
    assert(pthread_mutex_trylock(&first.lock) == EBUSY);
    assert(pthread_mutex_trylock(&second.lock) == EBUSY);
    ax_edid_capture_end();
    first.cap_fd = 123; first.streaming = 1; stop_error = 1;
    assert(ax_edid_capture_begin(error, sizeof(error)) < 0);
    assert(!first.streaming && first.cap_fd == -1);
    assert(!pthread_mutex_trylock(&first.lock)); pthread_mutex_unlock(&first.lock);
    stop_error = 0;
    struct ax_source *owned = calloc(1, sizeof(*owned)); assert(owned);
    ax_source_init(owned); owned->edid_next = ax_edid_sources; ax_edid_sources = owned;
    assert(!ax_edid_capture_begin(error, sizeof(error)));
    pthread_t thread; assert(!pthread_create(&thread, NULL, destroy_source, owned));
    while (!atomic_load(&destroy_started)) sched_yield();
    usleep(10000); assert(!atomic_load(&destroy_finished));
    ax_edid_capture_end(); pthread_join(thread, NULL);
    assert(atomic_load(&destroy_finished) && ax_edid_sources == &first);
    ax_edid_sources = NULL;
    puts("EDID capture: borrow rejection, STREAMOFF, error release and source lifetime PASS");
    return 0;
}
