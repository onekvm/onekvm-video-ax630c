/* SPDX-License-Identifier: MIT */
#include "ax_edid.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define EDID_BYTES 256u
#define EDID_DEVICE "/proc/lt6911_info/edid"
#define EDID_SNAPSHOT "/proc/lt6911_info/edid_snapshot"

static int edid_error(char *error, uint32_t capacity, const char *message)
{
    if (error && capacity)
        snprintf(error, capacity, "%s", message);
    return -1;
}

static int valid_edid(const uint8_t *data, uint32_t size)
{
    static const uint8_t header[] = {0, 255, 255, 255, 255, 255, 255, 0};
    if (size != EDID_BYTES || memcmp(data, header, sizeof(header)) || data[126] != 1)
        return 0;
    for (unsigned block = 0; block < 2; block++) {
        uint8_t sum = 0;
        for (unsigned i = 0; i < 128; i++)
            sum += data[block * 128 + i];
        if (sum)
            return 0;
    }
    return 1;
}

int32_t ax_edid_capabilities(struct onekvm_video_edid_caps_v1 *caps)
{
    if (!caps || caps->struct_size < sizeof(*caps))
        return -1;
    caps->flags = ONEKVM_VIDEO_EDID_CAP_PERSISTENT | ONEKVM_VIDEO_EDID_CAP_HOTPLUG;
    if (access(EDID_DEVICE, W_OK) == 0)
        caps->flags |= ONEKVM_VIDEO_EDID_CAP_WRITABLE;
    caps->min_bytes = caps->max_bytes = EDID_BYTES;
    caps->apply_policy = ONEKVM_VIDEO_EDID_APPLY_HOTPLUG;
    caps->chip_id = "LT6911UXC";
    caps->board_id = "pro";
    return 0;
}

int32_t ax_edid_get(struct onekvm_video_edid_blob_v1 *edid,
                    char *error, uint32_t capacity)
{
    if (!edid || edid->struct_size < sizeof(*edid))
        return edid_error(error, capacity, "invalid EDID buffer");
    edid->size = 0;
    /* Reading /edid resets the bridge. The driver seeds this snapshot at
     * probe and refreshes it after verified writes; UI reads must use it. */
    int fd = open(EDID_SNAPSHOT, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return edid_error(error, capacity, "open HDMI EDID snapshot");
    size_t size = 0;
    while (size < EDID_BYTES) {
        ssize_t n = read(fd, edid->data + size, EDID_BYTES - size);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        size += (size_t)n;
    }
    close(fd);
    if (!valid_edid(edid->data, size))
        return edid_error(error, capacity, "HDMI EDID snapshot is incomplete or invalid");
    edid->size = EDID_BYTES;
    return 0;
}

int32_t ax_edid_set(const struct onekvm_video_edid_blob_v1 *edid,
                    struct onekvm_video_edid_apply_result_v1 *apply,
                    char *error, uint32_t capacity)
{
    if (!edid || !apply || edid->struct_size < sizeof(*edid) ||
        apply->struct_size < sizeof(*apply))
        return edid_error(error, capacity, "invalid EDID request");
    if (!valid_edid(edid->data, edid->size))
        return edid_error(error, capacity, "EDID must contain two valid 128-byte blocks");
    int fd = open(EDID_DEVICE, O_WRONLY | O_CLOEXEC);
    if (fd < 0)
        return edid_error(error, capacity, "open HDMI EDID for writing");
    /* One complete write: the driver preserves the firmware version, writes
     * flash, verifies readback and cycles the bridge to advertise the EDID. */
    ssize_t written;
    do {
        written = write(fd, edid->data, EDID_BYTES);
    } while (written < 0 && errno == EINTR);
    int closed = close(fd);
    if (written != EDID_BYTES || closed != 0)
        return edid_error(error, capacity, "write or verify HDMI EDID failed");
    apply->apply_required = ONEKVM_VIDEO_EDID_APPLY_HOTPLUG;
    return 0;
}
