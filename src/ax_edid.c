/* SPDX-License-Identifier: MIT */
#include "ax_edid.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define EDID_BYTES 256u
#define EDID_DEVICE "/proc/lt6911_info/edid"
#define EDID_SHADOW "/proc/lt6911_info/edid_shadow"
static pthread_mutex_t edid_update_lock = PTHREAD_MUTEX_INITIALIZER;

#define EDID_SNAPSHOT "/proc/lt6911_info/edid_snapshot"
#define CHIP_ID_DEVICE "/proc/lt6911_info/chip_id"

/* The driver serves the detected ID from memory; this never probes I2C. */
static const char *bridge_chip(void)
{
    char id[16];
    size_t size = 0;
    int fd = open(CHIP_ID_DEVICE, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return "LT6911";
    while (size < sizeof(id) - 1) {
        ssize_t n = read(fd, id + size, sizeof(id) - 1 - size);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0) { size = 0; break; }
        if (n == 0)
            break;
        size += (size_t)n;
    }
    close(fd);
    id[size] = '\0';
    id[strcspn(id, "\r\n")] = '\0';
    if (!strcmp(id, "lt6911uxc")) return "LT6911UXC";
    if (!strcmp(id, "lt6911d")) return "LT6911D";
    if (!strcmp(id, "lt6911c")) return "LT6911C";
    return "LT6911";
}

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

static int read_snapshot(uint8_t *data)
{
    /* Reading Flash /edid resets the bridge. The driver seeds this snapshot at
     * probe and refreshes it after verified writes; UI reads must use it. */
    int fd = open(EDID_SNAPSHOT, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    size_t size = 0;
    while (size < EDID_BYTES) {
        ssize_t n = read(fd, data + size, EDID_BYTES - size);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        size += (size_t)n;
    }
    close(fd);
    return size == EDID_BYTES ? 0 : -1;
}

static int bridge_flash_supported(const char *chip)
{
    if (!strcmp(chip, "LT6911C")) {
        /* Blank is the firmware-default slot, not an EDID to show as active.
         * All reads are from RAM; writes recheck the whole erase envelope. */
        uint8_t cached[EDID_BYTES];
        if (read_snapshot(cached))
            return 0;
        if (valid_edid(cached, EDID_BYTES))
            return 1;
        for (unsigned i = 0; i < EDID_BYTES; i++)
            if (cached[i] != 0xff)
                return 0;
        return 1;
    }
    return !strcmp(chip, "LT6911UXC") || !strcmp(chip, "LT6911D");
}

static int bridge_shadow_supported(const char *chip)
{
    uint8_t cached[EDID_BYTES];
    return !strcmp(chip, "LT6911UXC") && access(EDID_SHADOW, W_OK) == 0 &&
           read_snapshot(cached) == 0 && valid_edid(cached, EDID_BYTES);
}

int32_t ax_edid_capabilities(struct onekvm_video_edid_caps_v1 *caps)
{
    if (!caps || caps->struct_size < sizeof(*caps))
        return -1;
    caps->chip_id = bridge_chip();
    caps->flags = 0;
    caps->apply_policy = ONEKVM_VIDEO_EDID_APPLY_NONE;
    if (bridge_flash_supported(caps->chip_id)) {
        caps->flags = ONEKVM_VIDEO_EDID_CAP_PERSISTENT | ONEKVM_VIDEO_EDID_CAP_HOTPLUG;
        if (access(EDID_DEVICE, W_OK) == 0)
            caps->flags |= ONEKVM_VIDEO_EDID_CAP_WRITABLE;
        caps->apply_policy = ONEKVM_VIDEO_EDID_APPLY_HOTPLUG;
#ifdef ONEKVM_VIDEO_EDID_CAP_TEMPORARY
        if (bridge_shadow_supported(caps->chip_id))
            caps->flags |= ONEKVM_VIDEO_EDID_CAP_TEMPORARY;
#endif
    }
    caps->min_bytes = caps->max_bytes = EDID_BYTES;
    caps->board_id = "pro";
    return 0;
}

int32_t ax_edid_get(struct onekvm_video_edid_blob_v1 *edid,
                    char *error, uint32_t capacity)
{
    if (!edid || edid->struct_size < sizeof(*edid))
        return edid_error(error, capacity, "invalid EDID buffer");
    edid->size = 0;
    if (read_snapshot(edid->data))
        return edid_error(error, capacity, "open or read HDMI EDID snapshot");
    if (!valid_edid(edid->data, EDID_BYTES))
        return edid_error(error, capacity, "HDMI EDID snapshot is incomplete or invalid");
    edid->size = EDID_BYTES;
    return 0;
}

static int32_t ax_edid_set_impl(const struct onekvm_video_edid_blob_v1 *edid,
                    struct onekvm_video_edid_apply_result_v1 *apply,
                    char *error, uint32_t capacity, int temporary)
{
    if (!edid || !apply || edid->struct_size < sizeof(*edid) ||
        apply->struct_size < sizeof(*apply))
        return edid_error(error, capacity, "invalid EDID request");
    if (!valid_edid(edid->data, edid->size))
        return edid_error(error, capacity, "EDID must contain two valid 128-byte blocks");
    const char *chip = bridge_chip();
    if (temporary && (!bridge_shadow_supported(chip)))
        return edid_error(error, capacity, "temporary EDID requires a UXC Shadow interface");
    if (!bridge_flash_supported(chip) || (!temporary && access(EDID_DEVICE, W_OK)))
        return edid_error(error, capacity, "HDMI bridge EDID interface is unavailable");
    pthread_mutex_lock(&edid_update_lock);
    int paused = 0, result = -1;
    const char *failure = "write or verify HDMI EDID failed";
    if (ax_edid_capture_begin(error, capacity)) {
        failure = NULL;
        goto out;
    }
    paused = 1;
    int fd = open(temporary ? EDID_SHADOW : EDID_DEVICE, O_WRONLY | O_CLOEXEC);
    if (fd < 0) goto out;
    ssize_t written;
    do {
        written = write(fd, edid->data, EDID_BYTES);
    } while (written < 0 && errno == EINTR);
    int closed = close(fd);
    if (written != EDID_BYTES || closed) goto out;
    apply->apply_required = ONEKVM_VIDEO_EDID_APPLY_HOTPLUG;
    result = 0;
out:
    if (paused) ax_edid_capture_end();
    pthread_mutex_unlock(&edid_update_lock);
    if (result && failure) return edid_error(error, capacity, failure);
    return result;
}

int32_t ax_edid_set(const struct onekvm_video_edid_blob_v1 *edid,
                    struct onekvm_video_edid_apply_result_v1 *apply,
                    char *error, uint32_t capacity)
{
    return ax_edid_set_impl(edid, apply, error, capacity, 0);
}

#ifdef ONEKVM_VIDEO_EDID_CAP_TEMPORARY
/* The optional shared ABI is supplied by Core. Older headers retain the
 * permanent API without advertising a callback that they cannot describe. */
int32_t ax_edid_set_mode(const struct onekvm_video_edid_blob_v1 *edid,
                         uint32_t mode,
                         struct onekvm_video_edid_apply_result_v1 *apply,
                         char *error, uint32_t capacity)
{
    if (mode != ONEKVM_VIDEO_EDID_WRITE_PERMANENT &&
        mode != ONEKVM_VIDEO_EDID_WRITE_TEMPORARY)
        return edid_error(error, capacity, "unknown EDID write mode");
    return ax_edid_set_impl(edid, apply, error, capacity,
                           mode == ONEKVM_VIDEO_EDID_WRITE_TEMPORARY);
}
#endif
