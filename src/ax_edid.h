/* SPDX-License-Identifier: MIT */
#ifndef ONEKVM_AX_EDID_H
#define ONEKVM_AX_EDID_H

#include <onekvm/video_backend_v1.h>

int32_t ax_edid_capabilities(struct onekvm_video_edid_caps_v1 *caps);
int32_t ax_edid_get(struct onekvm_video_edid_blob_v1 *edid,
                    char *error, uint32_t capacity);
int32_t ax_edid_set(const struct onekvm_video_edid_blob_v1 *edid,
                    struct onekvm_video_edid_apply_result_v1 *apply,
                    char *error, uint32_t capacity);

#ifdef ONEKVM_VIDEO_EDID_CAP_TEMPORARY
int32_t ax_edid_set_mode(const struct onekvm_video_edid_blob_v1 *edid,
                         uint32_t mode,
                         struct onekvm_video_edid_apply_result_v1 *apply,
                         char *error, uint32_t capacity);
#endif
int ax_edid_capture_begin(char *error, uint32_t capacity);
void ax_edid_capture_end(void);

#endif
