/* SPDX-License-Identifier: MIT */
#include <onekvm/video_backend_v1.h>

#include <dlfcn.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static int fail(const char *what, const char *detail)
{
    fprintf(stderr, "%s: %s\n", what, detail && detail[0] ? detail : "failed");
    return 1;
}

int main(int argc, char **argv)
{
    void *library;
    const void *raw = NULL;
    const struct onekvm_video_backend_v1 *api;
    union {
        void *symbol;
        onekvm_video_backend_query_fn fn;
    } loaded;
    struct onekvm_video_source_config_v1 source_config;
    struct onekvm_video_encoder_config_v1 encoder_config;
    void *source = NULL;
    void *encoder = NULL;
    char error[256];

    if (argc < 2) {
        fprintf(stderr, "usage: %s ax630c.so\n", argv[0]);
        return 1;
    }
    library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!library)
        return fail("dlopen", dlerror());
    if (dlsym(library, "onekvm_crypto_backend_query") != NULL)
        return fail("crypto", "production backend must not expose hardware crypto");
    loaded.symbol = dlsym(library, ONEKVM_VIDEO_BACKEND_QUERY_SYMBOL);
    if (!loaded.fn)
        return fail("dlsym", dlerror());
    if (loaded.fn(99u, &raw) == 0 || raw != NULL)
        return fail("query", "ABI 99 must be rejected");
    if (loaded.fn(ONEKVM_VIDEO_BACKEND_ABI_V1, &raw) != 0 || !raw)
        return fail("query", "ABI v1 unavailable");
    api = raw;
    if (!api->driver_id || strcmp(api->driver_id, "ax630c") != 0)
        return fail("driver_id", api->driver_id ? api->driver_id : "null");
    if (!(api->features & ONEKVM_VIDEO_FEATURE_BOUND_ENCODER) ||
        !api->encoder_bind_source || !api->encoder_read_packet ||
        !api->encoder_unbind_source)
        return fail("features", "bound encoder ABI missing");
    if (!(api->features & ONEKVM_VIDEO_FEATURE_EDID) ||
        !api->edid_capabilities || !api->edid_get || !api->edid_set)
        return fail("EDID", "library-level EDID ABI missing");
    if (!(api->codec_mask & ONEKVM_VIDEO_CODEC_MASK_H265) ||
        !(api->codec_mask & ONEKVM_VIDEO_CODEC_MASK_H264) ||
        !(api->codec_mask & ONEKVM_VIDEO_CODEC_MASK_AUTO))
        return fail("codecs", "auto, h264 and h265 required");
    if (api->struct_size < offsetof(struct onekvm_video_backend_v1, encoder_color_info) +
                               sizeof(api->encoder_color_info) ||
        !(api->features & ONEKVM_VIDEO_FEATURE_ENCODER_COLOR_INFO) ||
        !api->encoder_color_info)
        return fail("color info", "optional tail missing");
    if (!(api->features & ONEKVM_VIDEO_FEATURE_SOURCE_SNAPSHOT) || !api->source_snapshot)
        return fail("snapshot", "JPEG snapshot ABI missing");
    memset(&source_config, 0, sizeof(source_config));
    source_config.struct_size = sizeof(source_config);
    source_config.device = "/dev/null";
    if (!api->source_create ||
        api->source_create(&source_config, &source, error, sizeof(error)) != 0 ||
        !source)
        return fail("source_create", error);
    if (api->source_signal_present && api->source_signal_present(source) != 0)
        return fail("signal", "host smoke must not report a locked HDMI mode");
    if (!(api->features & ONEKVM_VIDEO_FEATURE_SOURCE_DISPLAY) ||
        !(api->features & ONEKVM_VIDEO_FEATURE_SOURCE_DISPLAY_CACHE_ONLY) || !api->source_display)
        return fail("display", "RGB565 ABI missing");
    struct onekvm_video_display_request_v1 request = {
        .struct_size = sizeof(request), .width = 640, .height = 360,
        .pixel_format = ONEKVM_VIDEO_PIXEL_RGB565, .timeout_ms = 5,
    };
    struct onekvm_video_display_result_v1 display = {.struct_size = sizeof(display)};
    uint8_t *rgb = malloc(640 * 360 * 2);
    if (!rgb) return fail("display", "allocation");
    if (api->source_display(source, &request, rgb, 640 * 360 * 2, &display, error, sizeof(error)) != ONEKVM_VIDEO_RESOURCE_TIMEOUT)
        return fail("display idle", error);
    request.width = -2;
    if (api->source_display(source, &request, rgb, 640 * 360 * 2, &display, error, sizeof(error)) != ONEKVM_VIDEO_RESOURCE_INVALID)
        return fail("display size", error);
    request.width = 640;
    if (api->source_display(source, &request, rgb, 1, &display, error, sizeof(error)) != ONEKVM_VIDEO_RESOURCE_BUFFER_TOO_SMALL)
        return fail("display capacity", error);
    request.pixel_format = ONEKVM_VIDEO_PIXEL_NV21;
    if (api->source_display(source, &request, rgb, 640 * 360 * 2, &display, error, sizeof(error)) != ONEKVM_VIDEO_RESOURCE_UNSUPPORTED)
        return fail("display format", error);
    free(rgb);
    /* An idle configuration must work without /dev/es_venc. Repeated unbind
     * and reset before the first viewer must also be safe. */
    memset(&encoder_config, 0, sizeof(encoder_config));
    encoder_config.struct_size = sizeof(encoder_config);
    encoder_config.codec = ONEKVM_VIDEO_CODEC_H264;
    encoder_config.fps = 60;
    if (api->encoder_create(&encoder_config, &encoder, error, sizeof(error)) != 0)
        return fail("idle encoder_create", error);
    struct {
        struct onekvm_video_color_info_v1 info;
        uint32_t guard;
    } color = {{sizeof(color), 99, 99}, 0x1a2b3c4d};
    if (api->encoder_color_info(encoder, &color.info) != 0 ||
        color.info.bit_depth != 0 || color.info.color_range != ONEKVM_VIDEO_COLOR_RANGE_UNKNOWN ||
        color.guard != 0x1a2b3c4d || color.info.struct_size != sizeof(color))
        return fail("color idle", "must be unknown before first AU; preserve caller tail");
    color.info.struct_size = sizeof(color.info) - 1;
    color.info.bit_depth = color.info.color_range = 99;
    if (api->encoder_color_info(encoder, &color.info) != ONEKVM_VIDEO_RESOURCE_INVALID ||
        color.info.bit_depth != 99 || color.info.color_range != 99 ||
        api->encoder_color_info(NULL, &color.info) != ONEKVM_VIDEO_RESOURCE_INVALID ||
        api->encoder_color_info(encoder, NULL) != ONEKVM_VIDEO_RESOURCE_INVALID)
        return fail("color size", "invalid output buffer accepted or modified");
    if (api->encoder_unbind_source(encoder, error, sizeof(error)) != 0 ||
        api->encoder_unbind_source(encoder, error, sizeof(error)) != 0)
        return fail("idle encoder_unbind", error);
    if (api->encoder_reset(encoder, &encoder_config, error, sizeof(error)) != 0)
        return fail("idle encoder_reset", error);
    const uint32_t codecs[] = {ONEKVM_VIDEO_CODEC_H265, ONEKVM_VIDEO_CODEC_H264,
                              ONEKVM_VIDEO_CODEC_AUTO, ONEKVM_VIDEO_CODEC_H265};
    for (unsigned i = 0; i < sizeof(codecs) / sizeof(codecs[0]); i++) {
        encoder_config.codec = codecs[i];
        if (api->encoder_reset(encoder, &encoder_config, error, sizeof(error)) != 0)
            return fail("codec reset", error);
        uint32_t expected = codecs[i] == ONEKVM_VIDEO_CODEC_AUTO
            ? ONEKVM_VIDEO_CODEC_H264 : codecs[i];
        if (api->encoder_codec(encoder) != expected)
            return fail("codec reset", "reported codec differs from configured codec");
        color.info.struct_size = sizeof(color.info);
        if (api->encoder_color_info(encoder, &color.info) != 0 ||
            color.info.bit_depth || color.info.color_range != ONEKVM_VIDEO_COLOR_RANGE_UNKNOWN)
            return fail("color reset", "stale color information after codec change");
    }
    api->encoder_destroy(encoder);
    encoder = NULL;
    encoder_config.codec = ONEKVM_VIDEO_CODEC_H265;
    if (api->encoder_create(&encoder_config, &encoder, error, sizeof(error)) != 0 ||
        api->encoder_codec(encoder) != ONEKVM_VIDEO_CODEC_H265)
        return fail("idle HEVC encoder_create", error);
    api->encoder_destroy(encoder);
    api->source_destroy(source);
    dlclose(library);
    return 0;
}
