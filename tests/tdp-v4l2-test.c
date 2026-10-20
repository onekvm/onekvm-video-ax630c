/* SPDX-License-Identifier: MIT
 * Real V4L2 DMA-BUF conversion tests. No vendor ioctl or physical address.
 * Exercise NV12/YUYV, BT.601/709, full/limited range, crop, scale, reuse and
 * close with DMA queued. Run only on an explicitly selected test device.
 */
#include <onekvm/v4l2_transform.h>
#include <linux/videodev2.h>
#include <linux/dma-buf.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d: %s errno=%d\n",__LINE__,#x,errno); exit(1); } } while (0)
static const uint8_t bars[8][3] = {
    {16,128,128}, {235,128,128}, {126,128,128}, {63,102,240},
    {173,42,26}, {32,240,118}, {188,154,16}, {78,214,230},
};
static int clip(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }
static void expected(unsigned b, int bt709, int full, int rgb[3]) {
    int y = bars[b][0] - (full ? 0 : 16), u = bars[b][1]-128, v = bars[b][2]-128;
    int cy = full ? 256 : 298;
    int rv = bt709 ? (full ? 403 : 459) : (full ? 359 : 409);
    int gu = bt709 ? (full ? -48 : -55) : (full ? -88 : -100);
    int gv = bt709 ? (full ? -120 : -136) : (full ? -183 : -208);
    int bu = bt709 ? (full ? 475 : 541) : (full ? 454 : 516);
    rgb[0] = clip((cy*y+rv*v+128)>>8);
    rgb[1] = clip((cy*y+gu*u+gv*v+128)>>8);
    rgb[2] = clip((cy*y+bu*u+128)>>8);
}
struct input { int video, dma; uint8_t *data; unsigned length, stride; };
static struct input input_create(unsigned w, unsigned h, unsigned pixel) {
    struct input in = { .video = -1, .dma = -1 };
    in.video = open("/dev/onekvm-vproc", O_RDWR|O_CLOEXEC|O_NONBLOCK);
    CHECK(in.video >= 0);
    struct v4l2_format fmt = { .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
        .fmt.pix_mp = { .width=w, .height=h, .pixelformat=pixel, .num_planes=1,
            .colorspace=V4L2_COLORSPACE_REC709 } };
    CHECK(ioctl(in.video, VIDIOC_S_FMT, &fmt) == 0);
    CHECK(fmt.fmt.pix_mp.width == w && fmt.fmt.pix_mp.height == h);
    in.stride = fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
    /* These test inputs are filled by the CPU, never QBUF'ed on their
     * allocating queue. vb2-dma-contig's exported DMA_BUF_SYNC hooks are
     * no-ops, so use coherent source pages. Real VIN/VDEC inputs are filled
     * by hardware and follow their producer's queue ownership rules.
     */
    struct v4l2_requestbuffers req = { .type=fmt.type, .memory=V4L2_MEMORY_MMAP,
        .count=1 };
    CHECK(ioctl(in.video, VIDIOC_REQBUFS, &req) == 0 && req.count);
    struct v4l2_plane plane = {0};
    struct v4l2_buffer buf = { .type=fmt.type, .memory=V4L2_MEMORY_MMAP, .length=1, .m.planes=&plane };
    CHECK(ioctl(in.video, VIDIOC_QUERYBUF, &buf) == 0);
    in.length = plane.length;
    in.data = mmap(NULL, in.length, PROT_READ|PROT_WRITE, MAP_SHARED, in.video, plane.m.mem_offset);
    CHECK(in.data != MAP_FAILED);
    struct v4l2_exportbuffer exp = { .type=fmt.type, .flags=O_RDWR|O_CLOEXEC };
    CHECK(ioctl(in.video, VIDIOC_EXPBUF, &exp) == 0);
    in.dma = exp.fd;
    struct dma_buf_sync sync = { .flags=DMA_BUF_SYNC_START|DMA_BUF_SYNC_WRITE };
    CHECK(ioctl(in.dma, DMA_BUF_IOCTL_SYNC, &sync) == 0);
    memset(in.data, 0, in.length);
    for (unsigned y=0; y<h; ++y) for (unsigned x=0; x<w; x+=2) {
        unsigned b=(uint64_t)x*8/w;
        if (pixel == V4L2_PIX_FMT_NV12) {
            in.data[(size_t)y*in.stride+x] = bars[b][0];
            in.data[(size_t)y*in.stride+x+1] = bars[b][0];
            if (!(y&1)) {
                uint8_t *uv=in.data+(size_t)in.stride*h+(size_t)(y/2)*in.stride+x;
                uv[0]=bars[b][1]; uv[1]=bars[b][2];
            }
        } else {
            uint8_t *p=in.data+(size_t)y*in.stride+x*2;
            p[0]=p[2]=bars[b][0]; p[1]=bars[b][1]; p[3]=bars[b][2];
        }
    }
    sync.flags = DMA_BUF_SYNC_END|DMA_BUF_SYNC_WRITE;
    CHECK(ioctl(in.dma, DMA_BUF_IOCTL_SYNC, &sync) == 0);
    return in;
}
static void input_destroy(struct input *in) {
    close(in->dma); munmap(in->data,in->length); close(in->video);
}
static double seconds(clockid_t clock) {
    struct timespec t; clock_gettime(clock,&t); return t.tv_sec+t.tv_nsec/1e9;
}
static void run(unsigned pixel, int bt709, int full, int crop, unsigned jobs) {
    unsigned w=480,h=368,dw=crop?318:480,dh=crop?174:368;
    struct input in=input_create(w,h,pixel);
    struct onekvm_v4l2_transform_config c = {
        .width=w,.height=h,.stride=in.stride,.pixel_format=pixel,
        .left=crop?2:0,.top=crop?2:0,.crop_width=crop?476:w,.crop_height=crop?360:h,
        .output_width=dw,.output_height=dh,
        .colorspace=bt709?V4L2_COLORSPACE_REC709:V4L2_COLORSPACE_SMPTE170M,
        .ycbcr_enc=bt709?V4L2_YCBCR_ENC_709:V4L2_YCBCR_ENC_601,
        .quantization=full?V4L2_QUANTIZATION_FULL_RANGE:V4L2_QUANTIZATION_LIM_RANGE,
    };
    struct onekvm_v4l2_transform *t=NULL;
    CHECK(onekvm_v4l2_transform_create("/dev/onekvm-vproc",&c,&t)==0);
    CHECK(onekvm_v4l2_transform_matches(t,&c));
    CHECK(onekvm_v4l2_transform_submit(t,-1,0)==-EINVAL);
    struct onekvm_v4l2_transform_frame f;
    CHECK(onekvm_v4l2_transform_receive(t,&f,0)==-EAGAIN);
    double wall=seconds(CLOCK_MONOTONIC),cpu=seconds(CLOCK_PROCESS_CPUTIME_ID);
    const uint8_t *first=NULL;
    for (unsigned i=0;i<jobs;++i) {
        CHECK(onekvm_v4l2_transform_submit(t,in.dma,0)==0);
        CHECK(onekvm_v4l2_transform_submit(t,in.dma,0)==-EBUSY);
        int ready=onekvm_v4l2_transform_receive(t,&f,0);
        CHECK(ready==0 || ready==-EAGAIN);
        if (ready==-EAGAIN) CHECK(onekvm_v4l2_transform_receive(t,&f,250)==0);
        if (!first) first=f.data;
        CHECK(f.data==first && f.width==dw && f.height==dh && f.stride>=dw*2);
        CHECK(onekvm_v4l2_transform_receive(t,&f,0)==-EBUSY);
        unsigned maximum=0;
        for (unsigned b=0;b<8;++b) {
            unsigned x=(uint64_t)(2*b+1)*dw/16;
            unsigned sx=c.left+(uint64_t)x*c.crop_width/dw;
            unsigned bar=(uint64_t)sx*8/w;
            const uint8_t *p=f.data+(size_t)(dh/2)*f.stride+x*2;
            unsigned v=p[0]|(unsigned)p[1]<<8;
            int got[3]={((v>>11)&31)*255/31,((v>>5)&63)*255/63,(v&31)*255/31},want[3];
            expected(bar,bt709,full,want);
            for (unsigned k=0;k<3;++k) {
                unsigned delta=abs(got[k]-want[k]);
                if (delta>maximum) maximum=delta;
                if (delta>9) {
                    fprintf(stderr,"colour mismatch pixel=%c%c%c%c 709=%d full=%d crop=%d bar=%u got=%d,%d,%d want=%d,%d,%d\n",
                        pixel&255,(pixel>>8)&255,(pixel>>16)&255,pixel>>24,bt709,full,crop,bar,
                        got[0],got[1],got[2],want[0],want[1],want[2]);
                    exit(1);
                }
            }
        }
        onekvm_v4l2_transform_release(t);
    }
    printf("PASS %c%c%c%c BT.%s %s crop=%d %ux%u jobs=%u wall_ms=%.3f cpu_ms=%.3f\n",
        pixel&255,(pixel>>8)&255,(pixel>>16)&255,pixel>>24,bt709?"709":"601",full?"full":"limited",crop,dw,dh,jobs,
        (seconds(CLOCK_MONOTONIC)-wall)*1000,(seconds(CLOCK_PROCESS_CPUTIME_ID)-cpu)*1000);
    /* Closing a queued conversion must drain before releasing the DMA-BUF. */
    CHECK(onekvm_v4l2_transform_submit(t,in.dma,0)==0);
    onekvm_v4l2_transform_destroy(t);
    input_destroy(&in);
}
static void source_rebind(void) {
    struct input inputs[34];
    struct onekvm_v4l2_transform_config c={
        .width=32,.height=16,.stride=32,.pixel_format=V4L2_PIX_FMT_NV12,
        .crop_width=32,.crop_height=16,.output_width=32,.output_height=16,
        .colorspace=V4L2_COLORSPACE_SMPTE170M,.ycbcr_enc=V4L2_YCBCR_ENC_601,
        .quantization=V4L2_QUANTIZATION_FULL_RANGE,
    };
    for (unsigned i=0;i<34;++i) {
        inputs[i]=input_create(32,16,V4L2_PIX_FMT_NV12);
        struct dma_buf_sync sync={ .flags=DMA_BUF_SYNC_START|DMA_BUF_SYNC_WRITE };
        CHECK(ioctl(inputs[i].dma,DMA_BUF_IOCTL_SYNC,&sync)==0);
        memset(inputs[i].data,25+i*6,32*16);
        memset(inputs[i].data+32*16,128,32*8);
        sync.flags=DMA_BUF_SYNC_END|DMA_BUF_SYNC_WRITE;
        CHECK(ioctl(inputs[i].dma,DMA_BUF_IOCTL_SYNC,&sync)==0);
    }
    struct onekvm_v4l2_transform *t=NULL;
    CHECK(onekvm_v4l2_transform_create("/dev/onekvm-vproc",&c,&t)==0);
    /* Exceed the attachment cache and rebind the same vb2 slots repeatedly.
     * vb2 calls buf_cleanup before preparing the replacement DMA-BUF.
     */
    for (unsigned round=0;round<3;++round) for (unsigned i=0;i<34;++i) {
        struct onekvm_v4l2_transform_frame f;
        CHECK(onekvm_v4l2_transform_submit(t,inputs[i].dma,0)==0);
        CHECK(onekvm_v4l2_transform_receive(t,&f,250)==0);
        unsigned v=f.data[0]|(unsigned)f.data[1]<<8;
        CHECK(abs((int)((v>>11)&31)*255/31-(int)(25+i*6))<=8);
        CHECK(abs((int)((v>>5)&63)*255/63-(int)(25+i*6))<=4);
        CHECK(abs((int)(v&31)*255/31-(int)(25+i*6))<=8);
        onekvm_v4l2_transform_release(t);
    }
    onekvm_v4l2_transform_destroy(t);
    for (unsigned i=0;i<34;++i) input_destroy(&inputs[i]);
    puts("DMA-BUF slot eviction and repeated source rebinding PASS (34 buffers, 102 jobs)");
}
int main(int argc,char **argv) {
    unsigned jobs=argc>1?strtoul(argv[1],NULL,10):10;
    CHECK(jobs && jobs<=1000);
    source_rebind();
    for(unsigned format=0;format<2;++format)
        for(int matrix=0;matrix<2;++matrix)
            for(int full=0;full<2;++full)
                for(int crop=0;crop<2;++crop)
                    run(format?V4L2_PIX_FMT_NV12:V4L2_PIX_FMT_YUYV,matrix,full,crop,jobs);
    puts("V4L2 DMA-BUF colour, scale, crop, reuse, ownership and queued-close checks PASS");
    return 0;
}
