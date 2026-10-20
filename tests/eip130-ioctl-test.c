/* SPDX-License-Identifier: MIT */
#include <linux/axera_eip130.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %d: %s (errno=%d)\n", \
    __LINE__, #x, errno); return 1; } } while (0)
static const uint8_t aes_zero[16] = {
    0x66,0xe9,0x4b,0xd4,0xef,0x8a,0x2c,0x3b,0x88,0x4c,0xfa,0x59,0xca,0x34,0x2b,0x2e
};
static int keyed_fd(void)
{
    struct axera_eip130_key key = {
        .version = AXERA_EIP130_ABI, .algorithm = AXERA_EIP130_ALG_ECB, .key_size = 16,
    };
    int fd = open("/dev/axera-eip130", O_RDWR | O_CLOEXEC);
    if (fd >= 0 && ioctl(fd, AXERA_EIP130_SET_KEY, &key)) { close(fd); return -1; }
    return fd;
}
static int known_vector(int fd)
{
    uint8_t data[18] = {0};
    struct axera_eip130_request r = {
        .version = AXERA_EIP130_ABI, .src = (uintptr_t)(data + 1),
        .dst = (uintptr_t)(data + 1), .size = 16,
    };
    data[0] = data[17] = 0xa5;
    CHECK(!ioctl(fd, AXERA_EIP130_RUN, &r) && r.output_size == 16);
    CHECK(!memcmp(data + 1, aes_zero, 16) && data[0] == 0xa5 && data[17] == 0xa5);
    return 0;
}
static void *shared_fd(void *arg)
{
    unsigned int i;
    for (i = 0; i < 32; i++) if (known_vector(*(int *)arg)) return (void *)1;
    return NULL;
}
static int boundaries(void)
{
    struct axera_eip130_key key = {
        .version = AXERA_EIP130_ABI, .algorithm = AXERA_EIP130_ALG_ECB, .key_size = 16,
    };
    uint8_t src[16] = {0}, dst[16];
    struct axera_eip130_request r = {
        .version = AXERA_EIP130_ABI, .src = (uintptr_t)src,
        .dst = (uintptr_t)dst, .size = 16, .output_size = 99,
    };
    int fd = open("/dev/axera-eip130", O_RDWR | O_CLOEXEC), i;
    pthread_t threads[4];
    void *result;

    CHECK(fd >= 0);
    CHECK(ioctl(fd, AXERA_EIP130_RUN, &r) < 0 && errno == ENOKEY && !r.output_size);
    CHECK(ioctl(fd, AXERA_EIP130_SET_KEY, (void *)1) < 0 && errno == EFAULT);
    key.version++;
    CHECK(ioctl(fd, AXERA_EIP130_SET_KEY, &key) < 0 && errno == EINVAL);
    key.version--; key.reserved = 1;
    CHECK(ioctl(fd, AXERA_EIP130_SET_KEY, &key) < 0 && errno == EINVAL);
    key.reserved = 0; key.key_size = 15;
    CHECK(ioctl(fd, AXERA_EIP130_SET_KEY, &key) < 0 && errno == EINVAL);
    key.key_size = 16; key.algorithm = 999;
    CHECK(ioctl(fd, AXERA_EIP130_SET_KEY, &key) < 0 && errno == EOPNOTSUPP);
    key.algorithm = AXERA_EIP130_ALG_ECB;
    CHECK(!ioctl(fd, AXERA_EIP130_SET_KEY, &key));
    CHECK(ioctl(fd, AXERA_EIP130_SET_KEY, &key) < 0 && errno == EBUSY);
    CHECK(ioctl(fd, _IO('E', 0x7f), NULL) < 0 && errno == ENOTTY);
    CHECK(ioctl(fd, AXERA_EIP130_RUN, (void *)1) < 0 && errno == EFAULT);
    memset(dst, 0xa5, sizeof(dst));
    for (i = 0; i < 8; i++) {
        struct axera_eip130_request invalid = r;
        if (i == 0) invalid.version++;
        if (i == 1) invalid.flags = 1;
        if (i == 2) invalid.reserved = 1;
        if (i == 3) invalid.size = AXERA_EIP130_MAX_INPUT + 1;
        if (i == 4) invalid.size = 15;
        if (i == 5) invalid.iv_size = 16;
        if (i == 6) invalid.src = 0;
        if (i == 7) invalid.dst = 0;
        CHECK(ioctl(fd, AXERA_EIP130_RUN, &invalid) < 0 && errno == EINVAL);
        CHECK(!invalid.output_size && dst[0] == 0xa5);
    }
    r.src = 1;
    CHECK(ioctl(fd, AXERA_EIP130_RUN, &r) < 0 && errno == EFAULT && !r.output_size);
    r.src = (uintptr_t)src; r.dst = 1;
    CHECK(ioctl(fd, AXERA_EIP130_RUN, &r) < 0 && errno == EFAULT && !r.output_size);
    CHECK(!known_vector(fd));
    for (i = 0; i < 4; i++) CHECK(!pthread_create(&threads[i], NULL, shared_fd, &fd));
    for (i = 0; i < 4; i++) CHECK(!pthread_join(threads[i], &result) && !result);
    close(fd);
    return 0;
}
static int write_control(const char *path)
{
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    ssize_t n;
    CHECK(fd >= 0);
    n = write(fd, "1\n", 2); close(fd);
    CHECK(n == 2);
    return 0;
}
static int fault_recovery(void)
{
    uint8_t src[16] = {0}, dst[16];
    struct axera_eip130_request r = {
        .version = AXERA_EIP130_ABI, .src = (uintptr_t)src,
        .dst = (uintptr_t)dst, .size = 16,
    };
    int fd = keyed_fd();
    CHECK(fd >= 0 && !known_vector(fd));
    memset(dst, 0xa5, sizeof(dst));
    CHECK(!write_control("/sys/module/axera_eip130/parameters/fault_timeout_once"));
    CHECK(ioctl(fd, AXERA_EIP130_RUN, &r) < 0 && errno == ETIMEDOUT);
    CHECK(!r.output_size && dst[0] == 0xa5 && dst[15] == 0xa5);
    CHECK(ioctl(fd, AXERA_EIP130_RUN, &r) < 0 && errno == EIO && !r.output_size);
    CHECK(!write_control("/sys/bus/platform/devices/4900000.crypto/recover"));
    CHECK(!known_vector(fd));
    close(fd);
    return 0;
}
int main(int argc, char **argv)
{
    _Static_assert(sizeof(struct axera_eip130_key) == 48, "key ABI size");
    _Static_assert(sizeof(struct axera_eip130_request) == 56, "request ABI size");
    CHECK(!boundaries());
    if (argc > 1 && !strcmp(argv[1], "--fault")) CHECK(!fault_recovery());
    puts("PASS: native ioctl validation, bad pointers, in-place guards, shared-fd concurrency and requested fault recovery");
    return 0;
}
