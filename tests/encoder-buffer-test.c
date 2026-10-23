/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include <assert.h>
#include <stdarg.h>
#include "../src/ax630c_video.c"

static struct ax_encoder *current;
static uint32_t slice_size;
static int idr, hevc, fail_wait, allocations, check_input, waits;
static uint8_t *raw;
static size_t raw_size;
int __wrap_poll(struct pollfd *fds, nfds_t count, int timeout)
{
    (void)timeout;
    for (nfds_t i = 0; i < count; i++) fds[i].revents = 0;
    assert(count && fds[0].fd == 123); fds[0].revents = POLLIN; return 1;
}
void *__real_malloc(size_t);
void *__real_calloc(size_t, size_t);
void *__real_realloc(void *, size_t);
void *__wrap_malloc(size_t n) { allocations++; return __real_malloc(n); }
void *__wrap_calloc(size_t n, size_t size) { allocations++; return __real_calloc(n, size); }
void *__wrap_realloc(void *p, size_t n) { allocations++; return __real_realloc(p, n); }
int __wrap_ioctl(int fd, unsigned long request, ...)
{
    va_list args; va_start(args, request);
    void *arg = va_arg(args, void *); va_end(args);
    if (fd == 123) {
        if (request == VIDIOC_DQBUF) ((struct v4l2_buffer *)arg)->index = 0;
        else {
            assert(request == VIDIOC_QBUF);
            /* VIN may overwrite immediately after QBUF. */
            memset(raw, 0xa5, raw_size);
        }
        return 0;
    }
    assert(fd == 456);
    if (request == HANTRO_IOCH_RESERVE_CMDBUF) {
        ((struct exchange_parameter *)arg)->cmdbuf_id = 0;
    } else if (request == HANTRO_IOCH_WAIT_CMDBUF) {
        waits++;
        if (fail_wait) return -1;
        if (check_input) {
            for (size_t i = 0; i < (size_t)1920 * 1080 * 2; i++)
                assert(current->fb_map[current->geom.off_in + i] == 0x7b);
            assert(raw[0] == 0xa5);
        }
        uint8_t *slice = current->fb_map + current->geom.off_out + AX_AU_HEADROOM;
        memset(slice, 0x5a, slice_size);
        slice[0] = slice[1] = slice[2] = 0; slice[3] = 1;
        slice[4] = hevc ? (idr ? 19 : 1) << 1 : idr ? 0x65 : 0x41;
        uint32_t *regs = (uint32_t *)(current->status + STATUS_SLOT_REG_OFF(0, 0));
        regs[9] = slice_size; regs[82] = 100;
        *(uint16_t *)arg = CMDBUF_EXE_STATUS_OK;
    } else {
        assert(request == HANTRO_IOCH_LINK_RUN_CMDBUF || request == HANTRO_IOCH_RELEASE_CMDBUF);
    }
    return 0;
}
int main(void)
{
    struct ax_encoder enc = {.venc_fd = 456, .gop = 30, .fps = 60, .seq = 1, .logged = 8};
    struct ax_source src; ax_source_init(&src); src.fps = 60;
    enc.bound = &src;
    assert(!vcenc_geom_build(&enc.geom, 1920, 1080));
    enc.fb.size = enc.geom.span; enc.fb.bus_addr = 0x60000000;
    enc.fb_map = malloc(enc.fb.size);
    enc.cmd = calloc(1, 0x2000); enc.status = calloc(1, 0x2000);
    assert(enc.fb_map && enc.cmd && enc.status);
    enc.mem.cmd_unit_size = 0x2000;
    struct onekvm_video_encoder_config_v1 config = {
        .struct_size = sizeof(config), .quality_factor = .5, .fps = 60,
    };
    current = &enc;
    uint32_t output_end = enc.geom.off_out + AX_AU_HEADROOM + enc.geom.out_limit;
    assert(output_end <= enc.geom.off_s10);
    for (hevc = 0; hevc <= 1; hevc++) {
        enc.codec = hevc ? ONEKVM_VIDEO_CODEC_H265 : ONEKVM_VIDEO_CODEC_H264;
        assert(!ax_rate_configure(&enc.rate, &config));
        for (int n = 0; n < 20; n++) {
            idr = n % 3 == 0;
            /* Exercise small and maximum-sized slices without resizing. */
            slice_size = n % 2 ? 128 : enc.geom.out_limit;
            memset(enc.fb_map, 0xa5, enc.fb.size);
            allocations = 0;
            assert(!ax_encode_yuyv(&enc, NULL, 3840, 0x50000000, idr, n % 30));
            assert(allocations == 0);
            uint8_t *slice = enc.fb_map + enc.geom.off_out + AX_AU_HEADROOM;
            assert(enc.publish <= slice && enc.publish >= enc.fb_map + enc.geom.off_out);
            assert(enc.publish + enc.packet_len == slice + slice_size);
            assert(!idr || enc.publish < slice);
            assert(enc.publish[0] == 0 && enc.publish[3] == 1);
            assert(enc.packet_key == (uint32_t)idr);
            /* Neither parameter sets nor output may touch the next region. */
            for (uint32_t i = output_end; i < enc.geom.off_s10 + 64; i++)
                assert(enc.fb_map[i] == 0xa5);
            ax_encoder_release_packet(&enc);
            assert(enc.publish[4] != 0xa5);
        }
        fail_wait = 1;
        assert(ax_encode_yuyv(&enc, NULL, 3840, 0x50000000, 0, 1) < 0);
        fail_wait = 0;
        assert(!ax_encode_yuyv(&enc, NULL, 3840, 0x50000000, idr, 1));
    }
    /* Import failure / padded stride uses fixed DMA input, survives VIN
     * reuse and refuses short capture buffers before hardware submission. */
    src.streaming = 1; src.cap_fd = 123; src.nbufs = 1;
    src.width = 1920; src.height = 1080; src.bytesperline = 3856;
    raw_size = (size_t)src.bytesperline * src.height;
    raw = malloc(raw_size); assert(raw);
    memset(raw, 0xa5, raw_size);
    for (uint32_t y = 0; y < 1080; y++) memset(raw + y * 3856, 0x7b, 3840);
    src.map[0] = raw; src.map_len[0] = raw_size;
    enc.have_geom = 1; enc.codec = ONEKVM_VIDEO_CODEC_H264;
    hevc = idr = 0; slice_size = 128; check_input = 1;
    struct onekvm_video_packet_v1 packet = {.struct_size = sizeof(packet)};
    char error[128]; allocations = 0;
    assert(!ax_encoder_read_packet(&enc, &packet, error, sizeof(error)));
    assert(!allocations && !src.held && packet.data == enc.publish);
    int prior_waits = waits;
    src.map_len[0] = 3840 * 1080 - 1;
    assert(ax_encoder_read_packet(&enc, &packet, error, sizeof(error)) < 0);
    assert(!src.held && waits == prior_waits);
    src.map_len[0] = raw_size; src.bytesperline = 3839;
    assert(ax_encoder_read_packet(&enc, &packet, error, sizeof(error)) < 0);
    assert(!src.held && waits == prior_waits);
    free(raw);
    free(enc.fb_map); free(enc.cmd); free(enc.status);
    puts("encoder buffers: 1080p H.264/HEVC fixed output, maximum slice, bounds, recovery, import fallback, short frames, no frame allocation PASS");
}
