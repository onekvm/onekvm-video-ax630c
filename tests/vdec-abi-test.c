/* SPDX-License-Identifier: MIT
 * Diagnostic AU records: little-endian length:u32, pts_ns:u64, payload bytes.
 * Input is prepared once from an MP4; the hot loop reuses its packet buffer.
 */
#include <onekvm/vdec_backend_v1.h>
#include <linux/videodev2.h>
#include <linux/media.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

static unsigned frames;
static unsigned pixel_format=ONEKVM_VIDEO_PIXEL_NV12;
static uint64_t last_pts, changes, hash;
static const struct onekvm_vdec_backend_v1 *api;
static void *decoder;
static char err[256];
#define CHECK(test) do { if (!(test)) { fprintf(stderr,"FAIL %s:%d: %s errno=%d\n",__FILE__,__LINE__,#test,errno); return 1; } } while (0)
static int ioctl_checks(void) {
    int fd=open("/dev/onekvm-vdec",O_RDWR|O_CLOEXEC|O_NONBLOCK);
    CHECK(fd>=0);
    struct v4l2_capability caps={0};
    CHECK(ioctl(fd,VIDIOC_QUERYCAP,&caps)==0);
    CHECK(caps.device_caps & V4L2_CAP_VIDEO_M2M_MPLANE);
    CHECK(caps.device_caps & V4L2_CAP_STREAMING);
    printf("V4L2 driver=%s card=%s bus=%s\n",caps.driver,caps.card,caps.bus_info);
    struct v4l2_fmtdesc fmt={ .type=V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE };
    CHECK(ioctl(fd,VIDIOC_ENUM_FMT,&fmt)==0 && fmt.pixelformat==V4L2_PIX_FMT_H264_SLICE);
    fmt=(struct v4l2_fmtdesc) { .type=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE };
    CHECK(ioctl(fd,VIDIOC_ENUM_FMT,&fmt)==0 && fmt.pixelformat==V4L2_PIX_FMT_NV12);
    struct v4l2_format config={ .type=V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE };
    config.fmt.pix_mp.pixelformat=V4L2_PIX_FMT_H264_SLICE;
    config.fmt.pix_mp.width=480; config.fmt.pix_mp.height=368;
    CHECK(ioctl(fd,VIDIOC_S_FMT,&config)==0);
    CHECK(config.fmt.pix_mp.num_planes==1 && config.fmt.pix_mp.plane_fmt[0].sizeimage);
    struct v4l2_requestbuffers request={ .type=V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
        .memory=V4L2_MEMORY_MMAP,.count=2 };
    CHECK(ioctl(fd,VIDIOC_REQBUFS,&request)==0 && request.count>=2);
    CHECK(request.capabilities & V4L2_BUF_CAP_SUPPORTS_REQUESTS);
    struct v4l2_plane plane={0};
    struct v4l2_buffer buffer={ .type=request.type,.memory=V4L2_MEMORY_MMAP,
        .length=1,.m.planes=&plane };
    CHECK(ioctl(fd,VIDIOC_QUERYBUF,&buffer)==0 && plane.length);
    uint8_t *memory=mmap(NULL,plane.length,PROT_READ|PROT_WRITE,MAP_SHARED,fd,plane.m.mem_offset);
    CHECK(memory!=MAP_FAILED);
    memory[0]=17; memory[plane.length-1]=42;
    buffer.flags=V4L2_BUF_FLAG_REQUEST_FD; buffer.request_fd=-1; plane.bytesused=4;
    CHECK(ioctl(fd,VIDIOC_QBUF,&buffer)<0);
    CHECK(memory[0]==17 && memory[plane.length-1]==42);
    CHECK(munmap(memory,plane.length)==0);
    request.count=0;
    CHECK(ioctl(fd,VIDIOC_REQBUFS,&request)==0);
    CHECK(ioctl(fd,0x1234,&buffer)<0 && errno==ENOTTY);
    int media=open("/dev/onekvm-vdec-media",O_RDWR|O_CLOEXEC);
    CHECK(media>=0);
    struct media_device_info info={0};
    CHECK(ioctl(media,MEDIA_IOC_DEVICE_INFO,&info)==0);
    int request_fd=-1;
    CHECK(ioctl(media,MEDIA_IOC_REQUEST_ALLOC,&request_fd)==0 && request_fd>=0);
    CHECK(ioctl(request_fd,MEDIA_REQUEST_IOC_QUEUE,NULL)<0);
    CHECK(ioctl(request_fd,MEDIA_REQUEST_IOC_REINIT,NULL)==0);
    close(request_fd); close(media); close(fd);
    puts("standard V4L2 formats, MMAP buffers, Request API and rejected-input checks passed");
    return 0;
}
static double seconds(clockid_t clock) {
    struct timespec t; clock_gettime(clock,&t); return t.tv_sec+t.tv_nsec/1e9;
}
static int drain(unsigned wait) {
    struct onekvm_vdec_frame_v1 f={ .struct_size=sizeof(f) };
    int ret=api->receive(decoder,&f,wait,err,sizeof(err));
    if (ret) return ret;
    if (!f.token || f.pixel_format!=pixel_format || f.plane_count!=(pixel_format==ONEKVM_VIDEO_PIXEL_NV12?2:1) || !f.planes[0].data)
        return -6;
    uint64_t h=1469598103934665603ULL;
    for (unsigned y=0; y<f.height; y+=8)
        for (unsigned x=0; x<f.width; x+=8) { h^=f.planes[0].data[y*f.planes[0].stride+x*(pixel_format==ONEKVM_VIDEO_PIXEL_RGB565?2:1)]; h*=1099511628211ULL; }
    if (frames && hash!=h) ++changes;
    if (f.tag!=f.pts_ns || (frames && f.pts_ns<last_pts)) {
        api->frame_release(decoder,f.token);
        snprintf(err,sizeof(err),"VDEC changed picture metadata or presentation order");
        return ONEKVM_VDEC_INTERNAL;
    }
    hash=h; last_pts=f.pts_ns;
    if (!frames || frames==60 || frames==179 || frames==1000) {
        char path[96];
        snprintf(path,sizeof(path),"/tmp/onekvm-vdec-%u.%s",frames,pixel_format==ONEKVM_VIDEO_PIXEL_NV12?"nv12":"rgb565");
        FILE *out=fopen(path,"wb");
        if (out) {
            for (unsigned p=0; p<f.plane_count; ++p)
                for (unsigned y=0; y<(p ? (f.height+1)/2 : f.height); ++y)
                    fwrite(f.planes[p].data+y*f.planes[p].stride,1,f.width*(pixel_format==ONEKVM_VIDEO_PIXEL_RGB565?2:1),out);
            fclose(out);
        }
        printf("first picture %ux%u stride=%u range=%u pts=%llu\n",f.width,f.height,
               f.planes[0].stride,f.color_range,(unsigned long long)f.pts_ns);
    }
    ++frames;
    api->frame_release(decoder,f.token);
    return 0;
}
static int send(const uint8_t *data, unsigned size, uint64_t pts, unsigned flags) {
    for (unsigned i=0; data && i+4<size; ++i) {
        unsigned prefix=0;
        if (data[i]==0 && data[i+1]==0) {
            if (data[i+2]==1) prefix=3;
            else if (data[i+2]==0 && data[i+3]==1) prefix=4;
        }
        if (prefix && i+prefix<size && (data[i+prefix]&31)==5)
            flags |= ONEKVM_VDEC_PACKET_KEYFRAME;
    }
    struct onekvm_vdec_packet_v1 p={ .struct_size=sizeof(p), .data=data, .size=size, .pts_ns=pts, .tag=pts, .flags=flags };
    for (unsigned attempt=0; attempt<100; ++attempt) {
        int ret=api->submit(decoder,&p,20,err,sizeof(err));
        if (!ret) return 0;
        if (ret!=ONEKVM_VDEC_AGAIN) return ret;
        ret=drain(20);
        if (ret<0) return ret;
        if (ret==ONEKVM_VDEC_AGAIN) {
            struct timespec pause={ .tv_nsec=1000000 };
            nanosleep(&pause,NULL);
        }
    }
    return ONEKVM_VDEC_TIMEOUT;
}
int main(int argc, char **argv) {
    if (argc==2 && strcmp(argv[1],"--ioctl")==0) return ioctl_checks();
    if (argc!=5 && argc!=6) { fprintf(stderr,"usage: %s backend.so access-units width height [rgb565]\n",argv[0]); return 2; }
    if (argc==6) { CHECK(!strcmp(argv[5],"rgb565")); pixel_format=ONEKVM_VIDEO_PIXEL_RGB565; }
    void *lib=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);
    if (!lib) { fprintf(stderr,"%s\n",dlerror()); return 1; }
    onekvm_vdec_backend_query_fn query=(onekvm_vdec_backend_query_fn)dlsym(lib,ONEKVM_VDEC_BACKEND_QUERY_SYMBOL);
    if (!query || query(1,(const void **)&api)) return 1;
    struct onekvm_vdec_caps_v1 caps={ .struct_size=sizeof(caps) };
    if (api->capabilities(&caps,err,sizeof(err))) return 1;
    struct onekvm_vdec_config_v1 config={ .struct_size=sizeof(config), .codec=ONEKVM_VIDEO_CODEC_H264,
        .width=(atoi(argv[3])+15)&~15, .height=(atoi(argv[4])+15)&~15, .pixel_format=pixel_format };
    int ret=api->create(&config,&decoder,err,sizeof(err));
    if (ret) { fprintf(stderr,"create: %d %s\n",ret,err); return 1; }
    FILE *input=fopen(argv[2],"rb");
    uint8_t *bytes=malloc(caps.max_packet_bytes);
    if (!input || !bytes) return 1;
    int failed=0;
    for (unsigned iteration=0; iteration<2; ++iteration) {
    rewind(input);
    frames=0; last_pts=changes=hash=0;
    double cpu=seconds(CLOCK_PROCESS_CPUTIME_ID), wall=seconds(CLOCK_MONOTONIC);
    unsigned packets=0;
    for (;;) {
        uint8_t header[12];
        if (fread(header,1,12,input)!=12) break;
        uint32_t size=(uint32_t)header[0]|(uint32_t)header[1]<<8|(uint32_t)header[2]<<16|(uint32_t)header[3]<<24;
        uint64_t pts=0; for (unsigned i=0;i<8;++i) pts|=(uint64_t)header[4+i]<<(8*i);
        if (!size || size>caps.max_packet_bytes || fread(bytes,1,size,input)!=size) { ret=-1; break; }
        ret=send(bytes,size,pts,0); if (ret) break;
        ++packets;
        while ((ret=drain(20))==0) {}
        if (ret<0) break;
    }
    if (ret>=0) ret=send(NULL,0,0,ONEKVM_VDEC_PACKET_EOS);
    if (!ret) {
        for (unsigned attempt=0; attempt<200; ++attempt) {
            ret=drain(20);
            if (ret==ONEKVM_VDEC_EOF || ret<0) break;
        }
        if (ret!=ONEKVM_VDEC_EOF) ret=ret<0 ? ret : ONEKVM_VDEC_TIMEOUT;
        else ret=0;
    }
    cpu=seconds(CLOCK_PROCESS_CPUTIME_ID)-cpu; wall=seconds(CLOCK_MONOTONIC)-wall;
    printf("iteration=%u packets=%u frames=%u changes=%llu cpu_s=%.6f wall_s=%.6f decode_cpu_ms_per_frame=%.4f result=%d %s\n",
        iteration+1,packets,frames,(unsigned long long)changes,cpu,wall,frames ? cpu*1000/frames : 0,ret,ret<0 ? err : "");
    int reset=api->reset(decoder,err,sizeof(err));
    printf("reset=%d\n",reset);
    if (ret<0 || reset || !frames || frames!=packets) { failed=1; break; }
    }
    api->destroy(decoder); free(bytes); fclose(input); dlclose(lib);
    return failed;
}
