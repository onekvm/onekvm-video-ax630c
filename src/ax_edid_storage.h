/* SPDX-License-Identifier: MIT */
#ifndef ONEKVM_AX_EDID_STORAGE_H
#define ONEKVM_AX_EDID_STORAGE_H
#include <stdint.h>
#include <limits.h>
struct ax_edid_storage {
    int dir_fd, old_exists, published;
    char active[PATH_MAX], pending[PATH_MAX], backup[PATH_MAX];
};
int ax_edid_storage_prepare(struct ax_edid_storage *state, const char *directory,
                            const uint8_t data[256]);
int ax_edid_storage_publish(struct ax_edid_storage *state);
int ax_edid_storage_finish(struct ax_edid_storage *state, int commit);
#endif
