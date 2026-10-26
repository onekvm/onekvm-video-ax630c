/* SPDX-License-Identifier: MIT */
#include "ax_edid_storage.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int stage_file(char *path, size_t capacity, const char *directory,
                       const uint8_t data[256])
{
    if (snprintf(path, capacity, "%s/.active.XXXXXX", directory) >= (int)capacity) {
        path[0] = 0;
        return -1;
    }
    int fd = mkstemp(path);
    if (fd < 0) { path[0] = 0; return -1; }
    size_t offset = 0;
    int result = 0;
    while (offset < 256) {
        ssize_t n = write(fd, data + offset, 256 - offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { result = -1; break; }
        offset += (size_t)n;
    }
    if (!result && fsync(fd)) result = -1;
    if (close(fd)) result = -1;
    return result;
}

int ax_edid_storage_finish(struct ax_edid_storage *s, int commit)
{
    int result = 0;
    if (!commit && s->published) {
        if (s->old_exists) {
            if (rename(s->backup, s->active)) result = -1;
            else s->backup[0] = 0;
        } else if (unlink(s->active) && errno != ENOENT) result = -1;
        if (s->dir_fd >= 0 && fsync(s->dir_fd)) result = -1;
    }
    if (s->pending[0]) unlink(s->pending);
    /* Retain the backup if rollback failed so diagnostics can recover it. */
    if (s->backup[0] && !result) unlink(s->backup);
    if (s->dir_fd >= 0) close(s->dir_fd);
    s->dir_fd = -1;
    return result;
}

int ax_edid_storage_prepare(struct ax_edid_storage *s, const char *directory,
                            const uint8_t data[256])
{
    memset(s, 0, sizeof(*s)); s->dir_fd = -1;
    if (snprintf(s->active, sizeof(s->active), "%s/active.bin", directory) >= (int)sizeof(s->active))
        return -1;
    if (mkdir(directory, 0755) && errno != EEXIST) return -1;
    s->dir_fd = open(directory, O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
    if (s->dir_fd < 0) return -1;
    int fd = open(s->active, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd >= 0) {
        uint8_t old[257]; size_t offset = 0;
        while (offset < sizeof(old)) {
            ssize_t n = read(fd, old + offset, sizeof(old) - offset);
            if (n < 0 && errno == EINTR) continue;
            if (n < 0) { close(fd); goto error; }
            if (!n) break;
            offset += (size_t)n;
        }
        close(fd);
        if (offset != 256) goto error;
        s->old_exists = 1;
        if (stage_file(s->backup, sizeof(s->backup), directory, old)) goto error;
    } else if (errno != ENOENT) goto error;
    if (stage_file(s->pending, sizeof(s->pending), directory, data)) goto error;
    return 0;
error:
    ax_edid_storage_finish(s, 0);
    return -1;
}

int ax_edid_storage_publish(struct ax_edid_storage *s)
{
    if (rename(s->pending, s->active)) return -1;
    s->pending[0] = 0; s->published = 1;
    return fsync(s->dir_fd) ? -1 : 0;
}
