/* SPDX-License-Identifier: MIT */
#include "ax_snapshot.h"
#include <errno.h>
#include <math.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <jpeglib.h>
#if defined(__aarch64__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#include <arm_neon.h>
#endif

struct ax_jpeg_error {
    struct jpeg_error_mgr base;
    jmp_buf jump;
    int status;
};

struct ax_jpeg {
    struct jpeg_compress_struct codec;
    struct ax_jpeg_error error;
    struct jpeg_destination_mgr dest;
    /* Fixed maximum scanline, reused across resolutions and requests. */
    uint8_t row[4096 * 3];
    int initialized;
};

static void ax_jpeg_fail(j_common_ptr jpeg)
{
    struct ax_jpeg_error *error = (struct ax_jpeg_error *)jpeg->err;
    longjmp(error->jump, 1);
}

static void ax_jpeg_dest_init(j_compress_ptr jpeg) { (void)jpeg; }
static void ax_jpeg_dest_end(j_compress_ptr jpeg) { (void)jpeg; }
static boolean ax_jpeg_dest_full(j_compress_ptr jpeg)
{
    ((struct ax_jpeg_error *)jpeg->err)->status = -ENOSPC;
    ax_jpeg_fail((j_common_ptr)jpeg);
    return FALSE;
}

void ax_snapshot_expand_rgb565(uint8_t *out, const uint8_t *src, uint32_t width)
{
    uint32_t x = 0;
#if defined(__aarch64__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    for (; x + 8 <= width; x += 8) {
        uint16x8_t p = vreinterpretq_u16_u8(vld1q_u8(src + x * 2));
        uint8x8_t r = vmovn_u16(vshrq_n_u16(p, 11));
        uint8x8_t g = vand_u8(vshrn_n_u16(p, 5), vdup_n_u8(63));
        uint8x8_t b = vand_u8(vmovn_u16(p), vdup_n_u8(31));
        uint8x8x3_t rgb = {{
            vorr_u8(vshl_n_u8(r, 3), vshr_n_u8(r, 2)),
            vorr_u8(vshl_n_u8(g, 2), vshr_n_u8(g, 4)),
            vorr_u8(vshl_n_u8(b, 3), vshr_n_u8(b, 2)),
        }};
        vst3_u8(out + x * 3, rgb);
    }
#endif
    for (; x < width; x++) {
        uint16_t p = src[x * 2] | ((uint16_t)src[x * 2 + 1] << 8);
        unsigned r = p >> 11, g = (p >> 5) & 63, b = p & 31;
        out[x * 3] = (r << 3) | (r >> 2);
        out[x * 3 + 1] = (g << 2) | (g >> 4);
        out[x * 3 + 2] = (b << 3) | (b >> 2);
    }
}

void ax_snapshot_jpeg_destroy(struct ax_jpeg *state)
{
    if (!state) return;
    if (state->initialized) jpeg_destroy_compress(&state->codec);
    free(state);
}

int ax_snapshot_jpeg(struct ax_jpeg **context, const uint8_t *rgb,
                     uint32_t width, uint32_t height, uint32_t stride,
                     double quality, uint8_t *out, size_t capacity, size_t *size)
{
    if (!context || !rgb || !out || !size || !width || !height || width > 4096 ||
        height > 2400 || stride < width * 2 ||
        !isfinite(quality) || quality < 0 || quality > 1)
        return -EINVAL;
    *size = 0;
    if (!capacity) return -ENOSPC;
    if (!*context) {
        *context = calloc(1, sizeof(**context));
        if (!*context) return -ENOMEM;
    }
    /* The source's snapshot gate serializes this context. Heap state remains
     * well-defined across libjpeg's longjmp, including destination overflow. */
    struct ax_jpeg *state = *context;
    struct jpeg_compress_struct *jpeg = &state->codec;
    jpeg->err = jpeg_std_error(&state->error.base);
    state->error.base.error_exit = ax_jpeg_fail;
    state->error.status = -EIO;
    if (setjmp(state->error.jump)) {
        int rc = state->error.status;
        jpeg_destroy_compress(jpeg);
        state->initialized = 0;
        return rc;
    }
    if (!state->initialized) {
        jpeg_create_compress(jpeg);
        state->initialized = 1;
    }
    state->dest = (struct jpeg_destination_mgr){
        .next_output_byte = out, .free_in_buffer = capacity,
        .init_destination = ax_jpeg_dest_init,
        .empty_output_buffer = ax_jpeg_dest_full,
        .term_destination = ax_jpeg_dest_end,
    };
    jpeg->dest = &state->dest;
    jpeg->image_width = width; jpeg->image_height = height;
    jpeg->input_components = 3; jpeg->in_color_space = JCS_RGB;
    jpeg_set_defaults(jpeg);
    jpeg_set_quality(jpeg, (int)(55 + 35 * quality), TRUE);
    jpeg->dct_method = JDCT_FASTEST;
    jpeg_start_compress(jpeg, TRUE);
    while (jpeg->next_scanline < height) {
        ax_snapshot_expand_rgb565(state->row, rgb + (size_t)jpeg->next_scanline * stride, width);
        JSAMPROW scanline = state->row;
        jpeg_write_scanlines(jpeg, &scanline, 1);
    }
    jpeg_finish_compress(jpeg);
    *size = capacity - state->dest.free_in_buffer;
    /* jpeg_finish_compress frees libjpeg's image pool. Permanent tables and
     * our scanline/context remain; this is not a promise of no library malloc. */
    return 0;
}
