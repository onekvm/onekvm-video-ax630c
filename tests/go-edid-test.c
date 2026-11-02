/* SPDX-License-Identifier: MIT */
#include "../src/ax_edid.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static uint8_t snapshot[512], programmed[512];
static size_t available, offset, chip_offset, written;
static const char *chip = "lt7911exc\n";
static int paused, writes, read_error;

int ax_edid_capture_begin(char *error, uint32_t capacity)
{
    (void)error; (void)capacity;
    assert(!paused); paused = 1; return 0;
}
void ax_edid_capture_end(void) { assert(paused); paused = 0; }
int __wrap_access(const char *path, int mode)
{
    assert(mode == W_OK && !strcmp(path, "/proc/lt7911_info/edid"));
    return 0;
}
int __wrap_open(const char *path, int flags, ...)
{
    if (!strcmp(path, "/proc/lt7911_info/chip_id")) {
        assert(flags == (O_RDONLY | O_CLOEXEC));
        chip_offset = 0; return 700;
    }
    if (flags == (O_RDONLY | O_CLOEXEC)) {
        assert(!strcmp(path, "/proc/lt7911_info/edid_snapshot"));
        offset = 0; return 701;
    }
    assert(flags == (O_WRONLY | O_CLOEXEC) && paused);
    assert(!strcmp(path, "/proc/lt7911_info/edid"));
    return 702;
}
ssize_t __wrap_read(int fd, void *data, size_t size)
{
    if (fd == 700) {
        if (size > strlen(chip) - chip_offset) size = strlen(chip) - chip_offset;
        memcpy(data, chip + chip_offset, size); chip_offset += size;
        return size;
    }
    assert(fd == 701);
    if (read_error && offset >= 128) { errno = EIO; return -1; }
    if (size > 17) size = 17;
    if (size > available - offset) size = available - offset;
    memcpy(data, snapshot + offset, size); offset += size;
    return size;
}
ssize_t __wrap_write(int fd, const void *data, size_t size)
{
    assert(fd == 702 && paused && size <= sizeof(programmed));
    memcpy(programmed, data, size); written = size; writes++;
    return size;
}
int __wrap_close(int fd) { assert(fd >= 700 && fd <= 702); return 0; }

static void make_edid(unsigned size)
{
    memset(snapshot, 0, sizeof(snapshot));
    memset(snapshot + 1, 0xff, 6);
    snapshot[126] = size / 128 - 1;
    for (unsigned block = 0; block < size / 128; block++) {
        uint8_t sum = 0;
        if (block) snapshot[block * 128] = 2;
        for (unsigned i = 0; i < 127; i++) sum += snapshot[block * 128 + i];
        snapshot[block * 128 + 127] = -sum;
    }
    available = size;
}

int main(void)
{
    char error[128];
    struct onekvm_video_edid_caps_v1 caps = { .struct_size = sizeof(caps) };
    struct onekvm_video_edid_blob_v1 blob = { .struct_size = sizeof(blob) };
    struct onekvm_video_edid_apply_result_v1 apply = { .struct_size = sizeof(apply) };
    assert(ax_edid_capabilities(&caps) == 0);
    assert(!strcmp(caps.chip_id, "LT7911EXC") && !strcmp(caps.board_id, "go"));
    assert(caps.min_bytes == 128 && caps.max_bytes == 512);
    assert(caps.flags == (ONEKVM_VIDEO_EDID_CAP_WRITABLE |
        ONEKVM_VIDEO_EDID_CAP_PERSISTENT | ONEKVM_VIDEO_EDID_CAP_HOTPLUG));
    for (unsigned size = 128; size <= 512; size += 128) {
        make_edid(size);
        assert(ax_edid_get(&blob, error, sizeof(error)) == 0 && blob.size == size);
        assert(ax_edid_set(&blob, &apply, error, sizeof(error)) == 0);
        assert(written == size && !memcmp(programmed, snapshot, size) && !paused);
        available--;
        assert(ax_edid_get(&blob, error, sizeof(error)) != 0 && blob.size == 0);
        available = size;
        snapshot[size - 1]++;
        assert(ax_edid_get(&blob, error, sizeof(error)) != 0);
    }
    make_edid(256);
    read_error = 1;
    assert(ax_edid_get(&blob, error, sizeof(error)) != 0 && blob.size == 0);
    read_error = 0;
    assert(ax_edid_get(&blob, error, sizeof(error)) == 0);
    int before = writes;
    chip = "lt7911d\n";
    assert(ax_edid_capabilities(&caps) == 0 && caps.flags == 0);
    assert(ax_edid_set(&blob, &apply, error, sizeof(error)) != 0 && writes == before);
#ifdef ONEKVM_VIDEO_EDID_CAP_TEMPORARY
    chip = "lt7911exc\n";
    assert(ax_edid_set_mode(&blob, ONEKVM_VIDEO_EDID_WRITE_TEMPORARY,
        &apply, error, sizeof(error)) != 0 && writes == before);
#endif
    puts("Go EDID block length, truncation and write checks passed");
    return 0;
}
