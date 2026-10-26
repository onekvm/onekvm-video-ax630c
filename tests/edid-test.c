/* SPDX-License-Identifier: MIT */
#include "../src/ax_edid.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static uint8_t snapshot[256], programmed[256];
static size_t offset, available = sizeof(snapshot);
static int writable = 1, missing, interrupted, write_error, short_write, writes, opens;
static const char *chip_id = "lt6911uxc\n";
static size_t chip_offset;
static int chip_missing, chip_read_error;

int __wrap_access(const char *path, int mode)
{
    assert(!strcmp(path, "/proc/lt6911_info/edid") && mode == W_OK);
    return writable ? 0 : -1;
}

int __wrap_open(const char *path, int flags, ...)
{
    opens++;
    if (!strcmp(path, "/proc/lt6911_info/chip_id")) {
        assert(flags == (O_RDONLY | O_CLOEXEC));
        if (chip_missing) { errno = ENOENT; return -1; }
        chip_offset = 0;
        return 700;
    }
    if (flags == (O_RDONLY | O_CLOEXEC)) {
        /* Never read the live flash interface: it resets HDMI. */
        assert(!strcmp(path, "/proc/lt6911_info/edid_snapshot"));
        if (missing) { errno = ENOENT; return -1; }
        offset = 0;
        return 701;
    }
    assert(flags == (O_WRONLY | O_CLOEXEC));
    assert(!strcmp(path, "/proc/lt6911_info/edid"));
    if (!writable) { errno = EACCES; return -1; }
    return 702;
}

ssize_t __wrap_read(int fd, void *data, size_t size)
{
    if (fd == 700) {
        if (chip_read_error) { errno = EIO; return -1; }
        if (interrupted) { interrupted = 0; errno = EINTR; return -1; }
        if (size > 3) size = 3;
        if (size > strlen(chip_id) - chip_offset)
            size = strlen(chip_id) - chip_offset;
        memcpy(data, chip_id + chip_offset, size);
        chip_offset += size;
        return size;
    }
    assert(fd == 701);
    if (interrupted) { interrupted = 0; errno = EINTR; return -1; }
    if (size > 17) size = 17;
    if (size > available - offset) size = available - offset;
    memcpy(data, snapshot + offset, size);
    offset += size;
    return size;
}

ssize_t __wrap_write(int fd, const void *data, size_t size)
{
    assert(fd == 702 && size == sizeof(programmed));
    writes++;
    if (interrupted) { interrupted = 0; errno = EINTR; return -1; }
    if (write_error) { errno = EIO; return -1; }
    if (short_write) return size / 2;
    memcpy(programmed, data, size);
    return size;
}

int __wrap_close(int fd)
{
    assert(fd == 700 || fd == 701 || fd == 702);
    return 0;
}

int main(void)
{
    FILE *fixture = fopen("../onekvm-nanokvm-machine/share/edid/1080p60.edid.bin", "rb");
    assert(fixture && fread(snapshot, 1, sizeof(snapshot), fixture) == sizeof(snapshot));
    fclose(fixture);
    char error[128];
    struct onekvm_video_edid_caps_v1 caps = {.struct_size = sizeof(caps)};
    struct onekvm_video_edid_blob_v1 blob = {.struct_size = sizeof(blob)};
    struct onekvm_video_edid_apply_result_v1 apply = {.struct_size = sizeof(apply)};

    assert(ax_edid_capabilities(&caps) == 0);
    assert(caps.min_bytes == 256 && caps.max_bytes == 256);
    assert(caps.flags == (ONEKVM_VIDEO_EDID_CAP_WRITABLE |
                         ONEKVM_VIDEO_EDID_CAP_PERSISTENT | ONEKVM_VIDEO_EDID_CAP_HOTPLUG));
    assert(caps.apply_policy == ONEKVM_VIDEO_EDID_APPLY_HOTPLUG);
    assert(!strcmp(caps.chip_id, "LT6911UXC") && !strcmp(caps.board_id, "pro"));
    assert(opens == 1);
    chip_id = "lt6911d\n";
    interrupted = 1;
    assert(ax_edid_capabilities(&caps) == 0 && !strcmp(caps.chip_id, "LT6911D"));
    assert(caps.flags & ONEKVM_VIDEO_EDID_CAP_WRITABLE);
    chip_id = "lt6911c\n";
    assert(ax_edid_capabilities(&caps) == 0 && !strcmp(caps.chip_id, "LT6911C"));
    assert(caps.flags == 0 && caps.apply_policy == ONEKVM_VIDEO_EDID_APPLY_NONE);
    chip_id = "unknown\n";
    assert(ax_edid_capabilities(&caps) == 0 && !strcmp(caps.chip_id, "LT6911"));
    assert(caps.flags == 0);
    chip_missing = 1;
    assert(ax_edid_capabilities(&caps) == 0 && caps.flags == 0);
    chip_missing = 0;
    chip_read_error = 1;
    assert(ax_edid_capabilities(&caps) == 0 && caps.flags == 0);
    chip_read_error = 0;
    chip_id = "lt6911uxc\n";
    writable = 0;
    assert(ax_edid_capabilities(&caps) == 0 && !(caps.flags & ONEKVM_VIDEO_EDID_CAP_WRITABLE));
    writable = 1;
    caps.struct_size--;
    assert(ax_edid_capabilities(&caps) != 0 && ax_edid_capabilities(NULL) != 0);

    interrupted = 1;
    assert(ax_edid_get(&blob, error, sizeof(error)) == 0);
    assert(blob.size == 256 && !memcmp(blob.data, snapshot, 256) && writes == 0);
    available = 255;
    assert(ax_edid_get(&blob, error, sizeof(error)) != 0 && blob.size == 0);
    available = 256;
    missing = 1;
    assert(ax_edid_get(&blob, NULL, 0) != 0);
    missing = 0;
    snapshot[255]++;
    assert(ax_edid_get(&blob, error, sizeof(error)) != 0);
    snapshot[255]--;
    assert(ax_edid_get(&blob, error, sizeof(error)) == 0);

    blob.size = 128;
    assert(ax_edid_set(&blob, &apply, error, sizeof(error)) != 0 && writes == 0);
    blob.size = 256;
    blob.data[0]++;
    assert(ax_edid_set(&blob, &apply, error, sizeof(error)) != 0 && writes == 0);
    blob.data[0]--;
    blob.data[200]++;
    assert(ax_edid_set(&blob, &apply, error, sizeof(error)) != 0 && writes == 0);
    blob.data[200]--;
    apply.struct_size--;
    assert(ax_edid_set(&blob, &apply, error, sizeof(error)) != 0 && writes == 0);
    apply.struct_size++;
    writable = 0;
    assert(ax_edid_set(&blob, &apply, error, sizeof(error)) != 0 && writes == 0);
    writable = 1;
    chip_id = "lt6911c\n";
    assert(ax_edid_set(&blob, &apply, error, sizeof(error)) != 0 && writes == 0);
    chip_id = "unknown\n";
    assert(ax_edid_set(&blob, &apply, error, sizeof(error)) != 0 && writes == 0);
    chip_id = "lt6911d\n";
    interrupted = 1;
    assert(ax_edid_set(&blob, &apply, error, sizeof(error)) == 0 && writes == 1);
    assert(!memcmp(programmed, snapshot, 256));
    assert(apply.apply_required == ONEKVM_VIDEO_EDID_APPLY_HOTPLUG);
    write_error = 1;
    apply.apply_required = 99;
    assert(ax_edid_set(&blob, &apply, error, sizeof(error)) != 0 && apply.apply_required == 99);
    write_error = 0;
    short_write = 1;
    int before = writes;
    assert(ax_edid_set(&blob, &apply, error, sizeof(error)) != 0 && writes == before + 1);
    assert(ax_edid_get(NULL, error, sizeof(error)) != 0);
    assert(ax_edid_set(NULL, &apply, error, sizeof(error)) != 0);
    assert(ax_edid_set(&blob, NULL, error, sizeof(error)) != 0);
    puts("EDID ABI checks passed");
    return 0;
}
