/* Host-only synchronous EIP-130 model. Never built into device packages. */
#define _GNU_SOURCE
#include <linux/axera_eip130.h>
#include <openssl/evp.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>

#define FD_BASE 100000
static struct axera_eip130_key keys[64];
static unsigned char active[64];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned int calls;
static int inject_failure;

__attribute__((visibility("default"))) unsigned int crypto_mock_run_count(void) { return calls; }
__attribute__((visibility("default"))) void crypto_mock_fail_next(void) { inject_failure = 1; }

int __wrap_open(const char *path, int flags, ...)
{
    (void)flags;
    if (strcmp(path, "/dev/axera-eip130")) { errno = EACCES; return -1; }
    pthread_mutex_lock(&lock);
    for (int i = 0; i < 64; i++) if (!active[i]) {
        active[i] = 1;
        pthread_mutex_unlock(&lock);
        return FD_BASE + i;
    }
    pthread_mutex_unlock(&lock);
    errno = EMFILE;
    return -1;
}

int __wrap_close(int fd)
{
    pthread_mutex_lock(&lock);
    if (fd >= FD_BASE && fd < FD_BASE + 64) {
        explicit_bzero(&keys[fd - FD_BASE], sizeof(keys[0]));
        active[fd - FD_BASE] = 0;
    }
    pthread_mutex_unlock(&lock);
    return 0;
}

int __wrap_ioctl(int fd, unsigned long command, ...)
{
    va_list args;
    void *argument;
    va_start(args, command);
    argument = va_arg(args, void *);
    va_end(args);
    if (fd < FD_BASE || fd >= FD_BASE + 64 || !active[fd - FD_BASE]) {
        errno = EBADF;
        return -1;
    }
    struct axera_eip130_key *key = &keys[fd - FD_BASE];
    if (command == AXERA_EIP130_SET_KEY) {
        *key = *(struct axera_eip130_key *)argument;
        return 0;
    }
    if (command != AXERA_EIP130_RUN) { errno = ENOTTY; return -1; }
    pthread_mutex_lock(&lock);
    calls++;
    int failure = inject_failure;
    inject_failure = 0;
    pthread_mutex_unlock(&lock);
    if (failure) { errno = EIO; return -1; }
    struct axera_eip130_request *r = argument;
    const EVP_CIPHER *cipher;
    if (key->algorithm == AXERA_EIP130_ALG_ECB)
        cipher = key->key_size == 16 ? EVP_aes_128_ecb() : EVP_aes_256_ecb();
    else
        cipher = key->key_size == 16 ? EVP_aes_128_ctr() :
                 key->key_size == 24 ? EVP_aes_192_ctr() : EVP_aes_256_ctr();
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    int n = 0, end = 0;
    int ok = ctx && EVP_EncryptInit_ex(ctx, cipher, NULL, key->key, r->iv) &&
        EVP_CIPHER_CTX_set_padding(ctx, 0) &&
        EVP_EncryptUpdate(ctx, (void *)(uintptr_t)r->dst, &n, (void *)(uintptr_t)r->src, r->size) &&
        EVP_EncryptFinal_ex(ctx, (uint8_t *)(uintptr_t)r->dst + n, &end);
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) { errno = EIO; return -1; }
    r->output_size = n + end;
    return 0;
}
