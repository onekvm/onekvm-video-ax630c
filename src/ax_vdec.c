/* SPDX-License-Identifier: MIT
 * Platform ABI adapter for the standard V4L2 stateless H.264 Request API.
 * GStreamer's H264/DPB parser drives V4L2; no hardware registers or private
 * kernel ioctls cross this boundary. Explicit hardware element, no decodebin.
 */
#include <onekvm/vdec_backend_v1.h>
#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>
#include <gst/allocators/gstfdmemory.h>
#include <onekvm/v4l2_transform.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define VDEC_NODE "/dev/onekvm-vdec"
#define MEDIA_NODE "/dev/onekvm-vdec-media"
#define PACKET_MAX (4U * 1024 * 1024)
#define INPUT_BUFFERS 24U /* DPB/reorder can retain up to 16 input AUs. */

static const GstMetaInfo *stamp_info;
static pthread_once_t gst_once = PTHREAD_ONCE_INIT;
static char gst_error[256];
static atomic_uint sessions;

struct decoder {
    struct onekvm_vdec_config_v1 config;
    GstElement *pipeline;
    GstAppSrc *source; /* owned by pipeline after construction */
    GstAppSink *sink;
    GstBus *bus;
    GstBufferPool *input_pool;
    size_t input_capacity;
    GstSample *picture;
    GstVideoFrame mapped;
    int mapped_valid, converting;
    struct onekvm_v4l2_transform *transform;
    pthread_mutex_t events;
    pthread_cond_t changed;
    int failed, idle, eos, drained;
    char error[256];
    uint64_t token;
};

static int error(int result, char *out, uint32_t n, const char *message) {
    if (out && n) snprintf(out, n, "%s", message);
    return result;
}

static void init_gst(void) {
    GError *err = NULL;
    if (!gst_init_check(NULL, NULL, &err)) {
        snprintf(gst_error, sizeof(gst_error), "%s", err ? err->message : "GStreamer initialization failed");
        g_clear_error(&err);
        return;
    }
    /* Untagged metadata follows the original codec frame through B-frame
     * reordering, including streams with duplicate presentation timestamps.
     */
    static const gchar *tags[] = { NULL };
    /* The registry survives backend dlclose/reload. Use GStreamer's default
     * custom-meta transform, whose callbacks live in libgstreamer, rather
     * than registering function pointers into this unloadable backend DSO.
     */
    stamp_info = gst_meta_get_info("OneKvmVdecStampV1");
    if (!stamp_info)
        stamp_info = gst_meta_register_custom("OneKvmVdecStampV1", tags, NULL, NULL, NULL);
    if (!stamp_info) snprintf(gst_error, sizeof(gst_error), "register VDEC frame metadata failed");
}

static void input_idle(GstAppSrc *source, guint length, gpointer data) {
    (void)source; (void)length;
    struct decoder *d = data;
    pthread_mutex_lock(&d->events);
    d->idle = 1;
    pthread_cond_broadcast(&d->changed);
    pthread_mutex_unlock(&d->events);
}

static GstFlowReturn output_ready(GstAppSink *sink, gpointer data) {
    (void)sink;
    struct decoder *d = data;
    pthread_mutex_lock(&d->events);
    pthread_cond_broadcast(&d->changed);
    pthread_mutex_unlock(&d->events);
    return GST_FLOW_OK;
}

static void output_eos(GstAppSink *sink, gpointer data) {
    (void)sink;
    struct decoder *d = data;
    pthread_mutex_lock(&d->events);
    d->drained = 1;
    pthread_cond_broadcast(&d->changed);
    pthread_mutex_unlock(&d->events);
}

static GstBusSyncReply bus_message(GstBus *bus, GstMessage *message, gpointer data) {
    (void)bus;
    struct decoder *d = data;
    if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
        GError *err = NULL; gchar *detail = NULL;
        gst_message_parse_error(message, &err, &detail);
        pthread_mutex_lock(&d->events);
        d->failed = ONEKVM_VDEC_HARDWARE_ERROR;
        snprintf(d->error, sizeof(d->error), "%s: %s", GST_OBJECT_NAME(message->src),
                 err ? err->message : "V4L2 decoding failed");
        pthread_cond_broadcast(&d->changed);
        pthread_mutex_unlock(&d->events);
        g_clear_error(&err); g_free(detail);
    }
    /* No unbounded bus queue when the ABI caller is not receiving pictures. */
    gst_message_unref(message);
    return GST_BUS_DROP;
}

static int check_device(char *err, uint32_t n) {
    int fd = open(VDEC_NODE, O_RDWR | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return error(ONEKVM_VDEC_HARDWARE_ERROR, err, n, "open " VDEC_NODE " failed");
    struct v4l2_capability caps = {0};
    int ret = ioctl(fd, VIDIOC_QUERYCAP, &caps);
    close(fd);
    if (ret || !(caps.device_caps & V4L2_CAP_VIDEO_M2M_MPLANE) ||
        !(caps.device_caps & V4L2_CAP_STREAMING))
        return error(ONEKVM_VDEC_UNSUPPORTED, err, n, VDEC_NODE " is not a V4L2 mem2mem decoder");
    return 0;
}

static GstElement *hardware_element(void) {
    GList *factories = gst_element_factory_list_get_elements(GST_ELEMENT_FACTORY_TYPE_DECODER, GST_RANK_NONE);
    GstElement *element = NULL;
    for (GList *entry = factories; entry && !element; entry = entry->next) {
        GstElementFactory *factory = entry->data;
        const char *name = gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory));
        if (!g_str_has_prefix(name, "v4l2sl") || !g_str_has_suffix(name, "h264dec")) continue;
        const char *props[] = { "video-device", "media-device" };
        GValue values[2] = { G_VALUE_INIT, G_VALUE_INIT };
        for (unsigned i = 0; i < 2; ++i) g_value_init(&values[i], G_TYPE_STRING);
        g_value_set_static_string(&values[0], VDEC_NODE);
        g_value_set_static_string(&values[1], MEDIA_NODE);
        element = gst_element_factory_create_with_properties(factory, 2, props, values);
        for (unsigned i = 0; i < 2; ++i) g_value_unset(&values[i]);
    }
    gst_plugin_feature_list_free(factories);
    return element;
}

static GstPadProbeReturn output_allocation(GstPad *pad, GstPadProbeInfo *probe, gpointer data) {
    (void)pad; (void)data;
    GstQuery *query = GST_PAD_PROBE_INFO_QUERY(probe);
    if (GST_QUERY_TYPE(query) != GST_QUERY_ALLOCATION) return GST_PAD_PROBE_OK;
    /* The ABI consumer handles padded strides and codec crops itself. Tell
     * the decoder this explicitly so it can loan its own capture buffers,
     * rather than allocating/copying a tightly packed frame for appsink.
     */
    gst_query_add_allocation_meta(query, GST_VIDEO_META_API_TYPE, NULL);
    gst_query_add_allocation_meta(query, GST_VIDEO_CROP_META_API_TYPE, NULL);
    return GST_PAD_PROBE_HANDLED;
}

static int input_pool(struct decoder *d, size_t needed) {
    if (needed <= d->input_capacity) return 0;
    size_t capacity = 64 * 1024;
    while (capacity < needed) capacity *= 2;
    GstBufferPool *pool = gst_buffer_pool_new();
    GstStructure *config = gst_buffer_pool_get_config(pool);
    gst_buffer_pool_config_set_params(config, NULL, capacity, 2, INPUT_BUFFERS);
    if (!gst_buffer_pool_set_config(pool, config) || !gst_buffer_pool_set_active(pool, TRUE)) {
        gst_object_unref(pool); return ONEKVM_VDEC_NO_MEMORY;
    }
    /* Deactivation can wait for held codec frames. Keep the old pool active;
     * its final outstanding buffer returns its final reference automatically.
     */
    if (d->input_pool) gst_object_unref(d->input_pool);
    d->input_pool = pool; d->input_capacity = capacity;
    return 0;
}

static void release_picture(struct decoder *d) {
    if (!d->picture) return;
    if (d->converting) {
        /* STREAMOFF must finish DMA before the decoder can recycle its DPB. */
        onekvm_v4l2_transform_destroy(d->transform);
        d->transform = NULL;
    } else {
        onekvm_v4l2_transform_release(d->transform);
    }
    d->converting = 0;
    if (d->mapped_valid) gst_video_frame_unmap(&d->mapped);
    d->mapped_valid = 0;
    gst_sample_unref(d->picture);
    d->picture = NULL;
}

static void stop_pipeline(struct decoder *d) {
    release_picture(d);
    onekvm_v4l2_transform_destroy(d->transform); d->transform = NULL;
    if (d->pipeline) {
        gst_element_set_state(d->pipeline, GST_STATE_NULL);
        gst_object_unref(d->pipeline);
        d->pipeline = NULL;
    }
    if (d->bus) {
        gst_bus_set_sync_handler(d->bus, NULL, NULL, NULL);
        gst_object_unref(d->bus);
        d->bus = NULL;
    }
    if (d->input_pool) {
        gst_buffer_pool_set_active(d->input_pool, FALSE);
        gst_object_unref(d->input_pool);
        d->input_pool = NULL;
    }
    d->input_capacity = 0;
    d->source = NULL;
    d->sink = NULL;
}

/* Appsrc remains in EOS after gst_app_src_end_of_stream().  Moving the
 * pipeline back to READY does not clear that state, so a subsequent submit
 * fails with GST_FLOW_EOS. Recreate the small pipeline on reset; this also
 * guarantees that V4L2 releases all request and capture buffers before the
 * next session. */
static int start_pipeline(struct decoder *d, char *err, uint32_t n) {
    d->pipeline = gst_pipeline_new(NULL);
    GstElement *source = gst_element_factory_make("appsrc", NULL);
    /* The stateless decoder expects parsed input caps. In particular,
     * h264parse carries SPS VUI colourimetry/range into its output state;
     * bare appsrc caps otherwise make SD-sized BT.709 streams look BT.601.
     */
    GstElement *parser = gst_element_factory_make("h264parse", NULL);
    GstElement *sink = gst_element_factory_make("appsink", NULL);
    GstElement *codec = hardware_element();
    if (!d->pipeline || !source || !parser || !sink || !codec) {
        if (source) gst_object_unref(source);
        if (parser) gst_object_unref(parser);
        if (sink) gst_object_unref(sink);
        if (codec) gst_object_unref(codec);
        stop_pipeline(d);
        return error(ONEKVM_VDEC_UNSUPPORTED, err, n,
                     "H.264 parser or V4L2 stateless hardware plugin is unavailable");
    }
    d->source = GST_APP_SRC(source);
    d->sink = GST_APP_SINK(sink);
    gst_bin_add_many(GST_BIN(d->pipeline), source, parser, codec, sink, NULL);
    GstPad *sink_pad = gst_element_get_static_pad(sink, "sink");
    gst_pad_add_probe(sink_pad, GST_PAD_PROBE_TYPE_QUERY_DOWNSTREAM,
                      output_allocation, NULL, NULL);
    gst_object_unref(sink_pad);
    if (!gst_element_link_many(source, parser, codec, sink, NULL)) {
        stop_pipeline(d);
        return error(ONEKVM_VDEC_UNSUPPORTED, err, n,
                     "link V4L2 decoder failed");
    }
    GstCaps *caps = gst_caps_new_simple("video/x-h264",
                                        "stream-format", G_TYPE_STRING, "byte-stream",
                                        "alignment", G_TYPE_STRING, "au", NULL);
    gst_app_src_set_caps(d->source, caps);
    gst_caps_unref(caps);
    gst_app_src_set_max_buffers(d->source, 4);
    gst_app_src_set_max_bytes(d->source, PACKET_MAX * 4ULL);
    g_object_set(source, "format", GST_FORMAT_TIME, "is-live", TRUE,
                 "block", FALSE, "emit-signals", FALSE, NULL);
    caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING,
                               "NV12", NULL);
    gst_app_sink_set_caps(d->sink, caps);
    gst_caps_unref(caps);
    gst_app_sink_set_max_buffers(d->sink, 2);
    g_object_set(sink, "sync", FALSE, "async", FALSE, "emit-signals", FALSE,
                 "wait-on-eos", FALSE, "enable-last-sample", FALSE, NULL);
    GstAppSrcCallbacks source_callbacks = { .need_data = input_idle };
    GstAppSinkCallbacks sink_callbacks = { .eos = output_eos,
                                           .new_sample = output_ready };
    gst_app_src_set_callbacks(d->source, &source_callbacks, d, NULL);
    gst_app_sink_set_callbacks(d->sink, &sink_callbacks, d, NULL);
    d->bus = gst_element_get_bus(d->pipeline);
    gst_bus_set_sync_handler(d->bus, bus_message, d, NULL);
    int ret = input_pool(d, 64 * 1024);
    if (ret || gst_element_set_state(d->pipeline, GST_STATE_PLAYING) ==
                   GST_STATE_CHANGE_FAILURE) {
        ret = ret ?: ONEKVM_VDEC_HARDWARE_ERROR;
        if (!ret) snprintf(err, n, "%s", d->error);
        stop_pipeline(d);
        return ret;
    }
    return 0;
}

static void destroy(void *handle) {
    struct decoder *d = handle;
    if (!d) return;
    stop_pipeline(d);
    pthread_cond_destroy(&d->changed); pthread_mutex_destroy(&d->events);
    atomic_fetch_sub(&sessions, 1);
    g_free(d);
}

static int32_t capabilities(struct onekvm_vdec_caps_v1 *caps, char *err, uint32_t n) {
    if (!caps || caps->struct_size < sizeof(*caps)) return error(ONEKVM_VDEC_INVALID, err, n, "short VDEC capabilities");
    *caps = (struct onekvm_vdec_caps_v1) { .struct_size = sizeof(*caps),
        .codec_mask = ONEKVM_VIDEO_CODEC_MASK_H264,
        .pixel_format_mask = ONEKVM_VDEC_PIXEL_MASK(ONEKVM_VIDEO_PIXEL_NV12) |
                             ONEKVM_VDEC_PIXEL_MASK(ONEKVM_VIDEO_PIXEL_RGB565),
        .max_width = 1920, .max_height = 1088, .max_sessions = 4,
        .max_inflight_frames = 1, .max_packet_bytes = PACKET_MAX };
    return 0;
}

static int32_t create(const struct onekvm_vdec_config_v1 *config, void **out, char *err, uint32_t n) {
    if (out) *out = NULL;
    if (!out || !config || config->struct_size < sizeof(*config)) return ONEKVM_VDEC_INVALID;
    if (config->flags || config->codec != ONEKVM_VIDEO_CODEC_H264 ||
        (config->pixel_format != ONEKVM_VIDEO_PIXEL_NV12 &&
         config->pixel_format != ONEKVM_VIDEO_PIXEL_RGB565) || !config->width || config->width > 1920 ||
        !config->height || config->height > 1088) return error(ONEKVM_VDEC_UNSUPPORTED, err, n, "unsupported Pro VDEC configuration");
    int ret = check_device(err, n);
    if (ret) return ret;
    pthread_once(&gst_once, init_gst);
    if (!stamp_info) return error(ONEKVM_VDEC_INTERNAL, err, n, gst_error);
    if (atomic_fetch_add(&sessions, 1) >= 4) { atomic_fetch_sub(&sessions, 1); return ONEKVM_VDEC_BUSY; }
    struct decoder *d = g_try_new0(struct decoder, 1);
    if (!d) { atomic_fetch_sub(&sessions, 1); return ONEKVM_VDEC_NO_MEMORY; }
    d->config = *config; d->idle = 1;
    pthread_mutex_init(&d->events, NULL);
    pthread_condattr_t attr; pthread_condattr_init(&attr); pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&d->changed, &attr); pthread_condattr_destroy(&attr);
    ret = start_pipeline(d, err, n);
    if (ret) { destroy(d); return ret; }
    *out = d;
    return 0;
}

static int32_t submit(void *handle, const struct onekvm_vdec_packet_v1 *packet,
                      uint32_t timeout, char *err, uint32_t n) {
    struct decoder *d = handle;
    if (!d || !packet || packet->struct_size < sizeof(*packet) || timeout > 1000 ||
        packet->flags & ~(ONEKVM_VDEC_PACKET_EOS | ONEKVM_VDEC_PACKET_KEYFRAME) || packet->size > PACKET_MAX ||
        ((packet->flags & ONEKVM_VDEC_PACKET_EOS) ? packet->size || packet->data : !packet->size || !packet->data))
        return error(ONEKVM_VDEC_INVALID, err, n, "invalid VDEC packet");
    if (pthread_mutex_trylock(&d->events)) return ONEKVM_VDEC_AGAIN;
    int failed = d->failed;
    if (failed) error(failed, err, n, d->error);
    pthread_mutex_unlock(&d->events);
    if (failed) return failed;
    if (d->eos) return error(ONEKVM_VDEC_INVALID, err, n, "VDEC is already draining");
    if (packet->flags & ONEKVM_VDEC_PACKET_EOS) {
        d->eos = 1;
        return gst_app_src_end_of_stream(d->source) == GST_FLOW_OK ? 0 : ONEKVM_VDEC_HARDWARE_ERROR;
    }
    if (gst_app_src_get_current_level_buffers(d->source) >= 4) return ONEKVM_VDEC_AGAIN;
    int ret = input_pool(d, packet->size);
    if (ret) return ret;
    GstBuffer *buffer = NULL;
    GstBufferPoolAcquireParams params = { .flags = GST_BUFFER_POOL_ACQUIRE_FLAG_DONTWAIT };
    if (gst_buffer_pool_acquire_buffer(d->input_pool, &buffer, &params) != GST_FLOW_OK) return ONEKVM_VDEC_AGAIN;
    gst_buffer_set_size(buffer, packet->size);
    if (gst_buffer_fill(buffer, 0, packet->data, packet->size) != packet->size) {
        gst_buffer_unref(buffer); return ONEKVM_VDEC_INTERNAL;
    }
    GstCustomMeta *stamp = (void *)gst_buffer_get_meta(buffer, stamp_info->api);
    if (!stamp) {
        stamp = (void *)gst_buffer_add_meta(buffer, stamp_info, NULL);
        if (stamp) GST_META_FLAG_SET(stamp, GST_META_FLAG_POOLED);
    }
    if (!stamp) { gst_buffer_unref(buffer); return ONEKVM_VDEC_NO_MEMORY; }
    gst_structure_set(gst_custom_meta_get_structure(stamp),
        "pts", G_TYPE_UINT64, packet->pts_ns, "tag", G_TYPE_UINT64, packet->tag, NULL);
    GST_BUFFER_PTS(buffer) = packet->pts_ns;
    if (!(packet->flags & ONEKVM_VDEC_PACKET_KEYFRAME)) GST_BUFFER_FLAG_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT);
    pthread_mutex_lock(&d->events); d->idle = 0; pthread_mutex_unlock(&d->events);
    return gst_app_src_push_buffer(d->source, buffer) == GST_FLOW_OK ? 0 :
        error(ONEKVM_VDEC_HARDWARE_ERROR, err, n, "V4L2 input pipeline failed");
}

static int32_t receive(void *handle, struct onekvm_vdec_frame_v1 *frame,
                       uint32_t timeout, char *err, uint32_t n) {
    struct decoder *d = handle;
    if (!d || !frame || frame->struct_size < sizeof(*frame) || timeout > 1000) return ONEKVM_VDEC_INVALID;
    if (d->picture && !d->converting) return error(ONEKVM_VDEC_BUSY, err, n, "release the previous VDEC picture first");
    struct timespec until; clock_gettime(CLOCK_MONOTONIC, &until);
    until.tv_sec += timeout / 1000; until.tv_nsec += (long)(timeout % 1000) * 1000000;
    if (until.tv_nsec >= 1000000000) { ++until.tv_sec; until.tv_nsec -= 1000000000; }
    GstSample *sample = d->picture;
    if (!sample) {
        if (pthread_mutex_trylock(&d->events)) return ONEKVM_VDEC_AGAIN;
        for (;;) {
            if (d->failed) {
                int ret = error(d->failed, err, n, d->error);
                pthread_mutex_unlock(&d->events); return ret;
            }
            sample = gst_app_sink_try_pull_sample(d->sink, 0);
            if (sample) break;
            if (d->drained || (!d->eos && d->idle) || !timeout ||
                pthread_cond_timedwait(&d->changed, &d->events, &until) == ETIMEDOUT) {
                int ret = d->drained ? ONEKVM_VDEC_EOF : ONEKVM_VDEC_AGAIN;
                pthread_mutex_unlock(&d->events); return ret;
            }
        }
        pthread_mutex_unlock(&d->events);
    }
    GstBuffer *buffer = gst_sample_get_buffer(sample);
    GstCustomMeta *stamp = (void *)gst_buffer_get_meta(buffer, stamp_info->api);
    const GstStructure *metadata = stamp ? gst_custom_meta_get_structure(stamp) : NULL;
    guint64 pts = 0, tag = 0;
    GstVideoInfo info;
    if (!metadata || !gst_structure_get_uint64(metadata, "pts", &pts) ||
        !gst_structure_get_uint64(metadata, "tag", &tag) || GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_CORRUPTED) ||
        !gst_video_info_from_caps(&info, gst_sample_get_caps(sample)) ||
        GST_VIDEO_INFO_FORMAT(&info) != GST_VIDEO_FORMAT_NV12 ||
        GST_VIDEO_INFO_WIDTH(&info) > d->config.width || GST_VIDEO_INFO_HEIGHT(&info) > d->config.height) {
        if (d->picture) release_picture(d); else gst_sample_unref(sample);
        return error(ONEKVM_VDEC_HARDWARE_ERROR, err, n, "invalid, corrupt or untagged V4L2 picture");
    }
    d->picture = sample;
    uint32_t w = info.width, h = info.height, x = 0, y = 0;
    GstVideoCropMeta *crop = gst_buffer_get_video_crop_meta(buffer);
    if (crop) { x = crop->x; y = crop->y; w = crop->width; h = crop->height; }
    GstVideoMeta *video = gst_buffer_get_video_meta(buffer);
    int ys = video ? video->stride[0] : info.stride[0];
    int uvs = video ? video->stride[1] : info.stride[1];
    if (!w || !h || (uint64_t)x + w > (uint32_t)info.width || (uint64_t)y + h > (uint32_t)info.height ||
        ((x | y) & 1) || ys < (int)(x + w) || uvs < (int)(x + ((w + 1) / 2) * 2)) {
        release_picture(d); return error(ONEKVM_VDEC_INTERNAL, err, n, "invalid V4L2 crop or strides");
    }
    if (d->config.pixel_format == ONEKVM_VIDEO_PIXEL_RGB565) {
        if (!d->converting) {
            gsize offset = 0, maximum = 0;
            GstMemory *memory = gst_buffer_peek_memory(buffer, 0);
            gsize y_offset = video ? video->offset[0] : info.offset[0];
            gsize uv_offset = video ? video->offset[1] : info.offset[1];
            if (gst_buffer_n_memory(buffer) != 1 || !gst_is_fd_memory(memory) || ys != uvs ||
                uv_offset <= y_offset || (uv_offset - y_offset) % ys) {
                release_picture(d);
                return error(ONEKVM_VDEC_UNSUPPORTED, err, n, "V4L2 RGB requires contiguous NV12 DMA-BUF");
            }
            gst_memory_get_sizes(memory, &offset, &maximum);
            uint64_t source_offset = offset + y_offset;
            uint64_t coded_height = (uv_offset - y_offset) / ys;
            if (source_offset > UINT32_MAX || coded_height < (uint64_t)y + h || coded_height > 2400) {
                release_picture(d);
                return error(ONEKVM_VDEC_INTERNAL, err, n, "invalid V4L2 DMA-BUF layout");
            }
            GstVideoColorMatrix matrix = info.colorimetry.matrix;
            if (matrix != GST_VIDEO_COLOR_MATRIX_BT601 && matrix != GST_VIDEO_COLOR_MATRIX_BT709 &&
                matrix != GST_VIDEO_COLOR_MATRIX_UNKNOWN) {
                release_picture(d);
                return error(ONEKVM_VDEC_UNSUPPORTED, err, n, "unsupported V4L2 RGB colour matrix");
            }
            struct onekvm_v4l2_transform_config config = {
                .width = info.width, .height = coded_height, .stride = ys,
                .pixel_format = V4L2_PIX_FMT_NV12, .left = x, .top = y,
                .crop_width = w, .crop_height = h, .output_width = w, .output_height = h,
                .colorspace = matrix == GST_VIDEO_COLOR_MATRIX_BT709 ? V4L2_COLORSPACE_REC709 : V4L2_COLORSPACE_SMPTE170M,
                .ycbcr_enc = matrix == GST_VIDEO_COLOR_MATRIX_BT709 ? V4L2_YCBCR_ENC_709 : V4L2_YCBCR_ENC_601,
                .quantization = info.colorimetry.range == GST_VIDEO_COLOR_RANGE_0_255 ?
                    V4L2_QUANTIZATION_FULL_RANGE : V4L2_QUANTIZATION_LIM_RANGE,
            };
            if (!onekvm_v4l2_transform_matches(d->transform, &config)) {
                onekvm_v4l2_transform_destroy(d->transform); d->transform = NULL;
                int rc = onekvm_v4l2_transform_create("/dev/onekvm-vproc", &config, &d->transform);
                if (rc) {
                    release_picture(d);
                    snprintf(err, n, "open V4L2 RGB converter failed: %s", strerror(-rc));
                    return ONEKVM_VDEC_HARDWARE_ERROR;
                }
            }
            int rc = onekvm_v4l2_transform_submit(d->transform, gst_fd_memory_get_fd(memory), source_offset);
            if (rc) {
                /* A failed submit may already have queued CAPTURE. */
                onekvm_v4l2_transform_destroy(d->transform); d->transform = NULL;
                release_picture(d);
                snprintf(err, n, "submit V4L2 RGB conversion failed: %s", strerror(-rc));
                return ONEKVM_VDEC_HARDWARE_ERROR;
            }
            d->converting = 1;
        }
        struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
        int64_t remaining = (until.tv_sec - now.tv_sec) * 1000LL + (until.tv_nsec - now.tv_nsec) / 1000000;
        struct onekvm_v4l2_transform_frame rgb;
        int rc = onekvm_v4l2_transform_receive(d->transform, &rgb, remaining > 0 ? remaining : 0);
        if (rc == -EAGAIN) return ONEKVM_VDEC_AGAIN;
        if (rc) {
            release_picture(d);
            snprintf(err, n, "receive V4L2 RGB conversion failed: %s", strerror(-rc));
            return ONEKVM_VDEC_HARDWARE_ERROR;
        }
        d->converting = 0;
        ++d->token; if (!d->token) ++d->token;
        *frame = (struct onekvm_vdec_frame_v1){ .struct_size = sizeof(*frame),
            .width = rgb.width, .height = rgb.height, .pixel_format = ONEKVM_VIDEO_PIXEL_RGB565,
            .plane_count = 1, .bit_depth = 8, .color_range = ONEKVM_VIDEO_COLOR_RANGE_FULL,
            .pts_ns = pts, .tag = tag, .token = d->token,
            .planes = {{ .data = rgb.data, .stride = rgb.stride,
                .size = (uint64_t)(rgb.height - 1) * rgb.stride + rgb.width * 2, .dma_buf_fd = -1 }} };
        return 0;
    }
    if (!gst_video_frame_map(&d->mapped, &info, buffer, GST_MAP_READ)) {
        release_picture(d);
        return error(ONEKVM_VDEC_HARDWARE_ERROR, err, n, "map V4L2 NV12 picture failed");
    }
    d->mapped_valid = 1;
    ++d->token; if (!d->token) ++d->token;
    *frame = (struct onekvm_vdec_frame_v1) { .struct_size = sizeof(*frame),
        .width = w, .height = h, .pixel_format = ONEKVM_VIDEO_PIXEL_NV12, .plane_count = 2, .bit_depth = 8,
        .color_range = info.colorimetry.range == GST_VIDEO_COLOR_RANGE_0_255 ?
            ONEKVM_VIDEO_COLOR_RANGE_FULL : ONEKVM_VIDEO_COLOR_RANGE_LIMITED,
        .pts_ns = pts, .tag = tag, .token = d->token,
        .planes = {
            { .data = (uint8_t *)GST_VIDEO_FRAME_PLANE_DATA(&d->mapped, 0) + (uint64_t)y * ys + x,
              .size = (uint64_t)(h - 1) * ys + w, .stride = ys, .dma_buf_fd = -1 },
            { .data = (uint8_t *)GST_VIDEO_FRAME_PLANE_DATA(&d->mapped, 1) + (uint64_t)(y / 2) * uvs + x,
              .size = (uint64_t)((h + 1) / 2 - 1) * uvs + ((w + 1) / 2) * 2, .stride = uvs, .dma_buf_fd = -1 },
        } };
    return 0;
}

static void frame_release(void *handle, uint64_t token) {
    struct decoder *d = handle;
    if (d && !d->converting && token == d->token) release_picture(d);
}

static int32_t reset(void *handle, char *err, uint32_t n) {
    struct decoder *d = handle;
    if (!d) return ONEKVM_VDEC_INVALID;
    if (d->picture && !d->converting) return ONEKVM_VDEC_BUSY;
    stop_pipeline(d);
    pthread_mutex_lock(&d->events);
    d->failed = d->eos = d->drained = 0; d->idle = 1; d->error[0] = 0;
    pthread_mutex_unlock(&d->events);
    return start_pipeline(d, err, n);
}

static const struct onekvm_vdec_backend_v1 backend = {
    .struct_size = sizeof(backend), .abi_version = ONEKVM_VDEC_BACKEND_ABI_V1,
    .driver_id = "ax630c", .backend_name = "AX630C V4L2 H.264 decoder + RGB converter",
    .features = ONEKVM_VDEC_FEATURE_HARDWARE,
    .capabilities = capabilities, .create = create, .submit = submit, .receive = receive,
    .frame_release = frame_release, .reset = reset, .destroy = destroy,
};

ONEKVM_VIDEO_EXPORT int32_t onekvm_vdec_backend_query(uint32_t abi, const void **out) {
    if (!out || abi != ONEKVM_VDEC_BACKEND_ABI_V1) return ONEKVM_VDEC_UNSUPPORTED;
    *out = &backend;
    return 0;
}
