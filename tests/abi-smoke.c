/* SPDX-License-Identifier: MIT */
#include <onekvm/video_backend_v1.h>

#include <dlfcn.h>
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
    if (!(api->codec_mask & ONEKVM_VIDEO_CODEC_MASK_H264) ||
        !(api->codec_mask & ONEKVM_VIDEO_CODEC_MASK_AUTO))
        return fail("codecs", "auto and h264 required");
    memset(&source_config, 0, sizeof(source_config));
    source_config.struct_size = sizeof(source_config);
    source_config.device = "/dev/null";
    if (!api->source_create ||
        api->source_create(&source_config, &source, error, sizeof(error)) != 0 ||
        !source)
        return fail("source_create", error);
    if (api->source_signal_present && api->source_signal_present(source) != 0)
        return fail("signal", "host smoke must not report a locked HDMI mode");
    /* An idle configuration must work without /dev/es_venc. Repeated unbind
     * and reset before the first viewer must also be safe. */
    memset(&encoder_config, 0, sizeof(encoder_config));
    encoder_config.struct_size = sizeof(encoder_config);
    encoder_config.codec = ONEKVM_VIDEO_CODEC_H264;
    encoder_config.fps = 60;
    if (api->encoder_create(&encoder_config, &encoder, error, sizeof(error)) != 0)
        return fail("idle encoder_create", error);
    if (api->encoder_unbind_source(encoder, error, sizeof(error)) != 0 ||
        api->encoder_unbind_source(encoder, error, sizeof(error)) != 0)
        return fail("idle encoder_unbind", error);
    if (api->encoder_reset(encoder, &encoder_config, error, sizeof(error)) != 0)
        return fail("idle encoder_reset", error);
    api->encoder_destroy(encoder);
    api->source_destroy(source);
    dlclose(library);
    return 0;
}
