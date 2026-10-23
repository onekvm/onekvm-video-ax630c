/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include <onekvm/crypto_backend_v1.h>
#include <dlfcn.h>
#include <errno.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static const struct onekvm_crypto_backend_v1 *api;
#define CHECK(cond) do { if (!(cond)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; \
} } while (0)

static int reference(int ctr, const uint8_t *key, unsigned int keylen,
                      const uint8_t *iv, const uint8_t *aad, size_t aadlen,
                      const uint8_t *src, size_t len, uint8_t *dst)
{
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    const EVP_CIPHER *cipher = ctr ? (keylen == 16 ? EVP_aes_128_ctr() :
        keylen == 24 ? EVP_aes_192_ctr() : EVP_aes_256_ctr()) :
        (keylen == 16 ? EVP_aes_128_gcm() : EVP_aes_256_gcm());
    int n = 0, end = 0;
    CHECK(ctx);
    CHECK(EVP_EncryptInit_ex(ctx, cipher, NULL, key, iv));
    if (!ctr && aadlen)
        CHECK(EVP_EncryptUpdate(ctx, NULL, &n, aad, aadlen));
    CHECK(EVP_EncryptUpdate(ctx, dst, &n, src, len));
    CHECK((size_t)n == len);
    CHECK(EVP_EncryptFinal_ex(ctx, dst + n, &end));
    CHECK(end == 0);
    if (!ctr)
        CHECK(EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, dst + n));
    EVP_CIPHER_CTX_free(ctx);
    return 0;
}

static int vector_case(int ctr, unsigned int keylen, unsigned int len,
                        unsigned int aadlen, int inplace, int wrap)
{
    uint8_t key[32], iv[16], aad[65536], input[65568], expected[65568], output[65568];
    char error[256] = {0};
    void *session = NULL;
    unsigned int i;
    int code;

    for (i = 0; i < sizeof(key); i++) key[i] = 3 * i + 7;
    for (i = 0; i < sizeof(iv); i++) iv[i] = 0xf3 - i;
    for (i = 0; i < sizeof(aad); i++) aad[i] = 7 * i + 13;
    for (i = 0; i < sizeof(input); i++) input[i] = 13 * i + 5;
    if (wrap) {
        memset(iv + 8, 0xff, 8);
        iv[15] = 0xfe;
    }
    CHECK(!reference(ctr, key, keylen, iv, aad, aadlen, input, len, expected));
    code = ctr ? api->primitive_create(ONEKVM_CRYPTO_ALGORITHM_AES_CTR,
        key, keylen, &session, error, sizeof(error)) :
        api->session_create(key, keylen, 16, &session, error, sizeof(error));
    if (code) fprintf(stderr, "create: %s\n", error);
    CHECK(!code && session);
    memset(output, 0xa5, sizeof(output));
    if (inplace) memcpy(output + 1, input, len);
    /* Deliberately unaligned src/dst; guard bytes detect excess writes. */
    if (ctr) {
        struct onekvm_crypto_primitive_request_v1 r = {
            .struct_size = sizeof(r), .src_ptr = (uintptr_t)(inplace ? output + 1 : input),
            .src_size = len, .dst_ptr = (uintptr_t)(output + 1), .dst_capacity = len,
            .iv_size = 16,
        };
        memcpy(r.iv, iv, 16);
        code = api->primitive_run(session, &r, error, sizeof(error));
        CHECK(code || r.output_size == len);
    } else {
        struct onekvm_crypto_request_v1 r = {
            .struct_size = sizeof(r), .src_ptr = (uintptr_t)(inplace ? output + 1 : input),
            .src_size = len, .dst_ptr = (uintptr_t)(output + 1), .dst_capacity = len + 16,
            .aad_ptr = aadlen ? (uintptr_t)aad : 0, .aad_size = aadlen, .nonce_size = 12,
        };
        memcpy(r.nonce, iv, 12);
        code = api->session_seal(session, &r, error, sizeof(error));
        CHECK(code || r.output_size == len + 16);
    }
    api->session_destroy(session);
    if (code) fprintf(stderr, "run (%d,%u,%u,%u): %s\n", ctr, keylen, len, aadlen, error);
    CHECK(!code);
    CHECK(!memcmp(output + 1, expected, len + (ctr ? 0 : 16)));
    CHECK(output[0] == 0xa5 && output[1 + len + (ctr ? 0 : 16)] == 0xa5);
    return 0;
}

static int boundaries(void)
{
    uint8_t key[16] = {0}, src[16] = {0}, dst[3][32];
    struct onekvm_crypto_request_v1 requests[3];
    char error[128];
    void *session = NULL;
    uint32_t completed = 99;
    unsigned int i;

    CHECK(api->session_create(key, 15, 16, &session, error, sizeof(error)) < 0 && !session);
    CHECK(api->session_create(key, 16, 12, &session, error, sizeof(error)) < 0 && !session);
    CHECK(api->primitive_create(999, key, 16, &session, error, sizeof(error)) < 0 && !session);
    CHECK(!api->session_create(key, 16, 16, &session, error, sizeof(error)));
    for (i = 0; i < 3; i++) {
        requests[i] = (struct onekvm_crypto_request_v1){
            .struct_size = sizeof(requests[i]), .src_ptr = (uintptr_t)src,
            .src_size = 16, .dst_ptr = (uintptr_t)dst[i], .dst_capacity = 32,
            .nonce_size = 12,
        };
    }
    memset(dst, 0xa5, sizeof(dst));
    requests[1].dst_capacity = 31;
    CHECK(api->session_seal_batch(session, requests, 3, &completed, error, sizeof(error)) < 0);
    CHECK(completed == 1 && requests[0].output_size == 32 && requests[1].output_size == 0);
    CHECK(dst[1][0] == 0xa5 && dst[2][0] == 0xa5);
    requests[1].dst_capacity = 32;
    CHECK(!api->session_seal_batch(session, requests, 3, &completed, error, sizeof(error)));
    CHECK(completed == 3);
    requests[0].flags = 1;
    CHECK(api->session_seal(session, requests, error, sizeof(error)) < 0);
    requests[0].flags = 0;
    requests[0].src_ptr = 0;
    CHECK(api->session_seal(session, requests, error, sizeof(error)) < 0);
    requests[0].src_size = UINT32_MAX;
    CHECK(api->session_seal(session, requests, error, sizeof(error)) < 0);
    requests[0].struct_size = 8;
    CHECK(api->session_seal(session, requests, error, sizeof(error)) < 0);
    CHECK(api->session_seal_batch(session, requests, api->max_batch + 1,
                                  &completed, error, sizeof(error)) < 0 && !completed);
    api->session_destroy(session);
    return 0;
}

static int mixed_batches(unsigned int keylen)
{
    static const unsigned int lengths[] = {0,1,15,16,17,1199,1200,1201,32768,65535,65536};
    uint8_t key[32] = {0}, input[65536], aad[12] = {0};
    uint8_t (*output)[65552] = calloc(32, 65552), *expected = malloc(65552);
    struct onekvm_crypto_request_v1 requests[32];
    void *session = NULL;
    char error[256];
    uint32_t completed;
    unsigned int i, len;

    CHECK(output && expected);
    for (i = 0; i < sizeof(input); i++) input[i] = i * 17 + 11;
    CHECK(!api->session_create(key, keylen, 16, &session, error, sizeof(error)));
    for (i = 0; i < 32; i++) {
        len = lengths[i % (sizeof(lengths) / sizeof(lengths[0]))];
        memcpy(output[i], input, len);
        requests[i] = (struct onekvm_crypto_request_v1){
            .struct_size = sizeof(requests[i]), .src_size = len,
            .src_ptr = (uintptr_t)(i % 2 ? output[i] : input),
            .dst_ptr = (uintptr_t)output[i], .dst_capacity = 65552,
            .aad_ptr = (uintptr_t)aad, .aad_size = len > 65524 ? 0 : 12,
            .nonce_size = 12,
        };
        requests[i].nonce[11] = i;
    }
    CHECK(!api->session_seal_batch(session, requests, 32, &completed, error, sizeof(error)));
    CHECK(completed == 32);
    for (i = 0; i < 32; i++) {
        len = requests[i].src_size;
        CHECK(!reference(0, key, keylen, requests[i].nonce, aad, requests[i].aad_size,
                          input, len, expected));
        CHECK(requests[i].output_size == len + 16);
        CHECK(!memcmp(output[i], expected, len + 16));
    }
    api->session_destroy(session);
    free(expected); free(output);
    return 0;
}

/* Authenticate native hardware output independently. The TX-only ABI has no
 * decrypt entry point; a receiver must reject a changed tag or changed AAD. */
static int tag_rejection(void)
{
    uint8_t key[16] = {0}, plain[17] = {0}, aad[13] = {0};
    uint8_t encrypted[33], out[33];
    struct onekvm_crypto_request_v1 request = {
        .struct_size = sizeof(request), .nonce_size = 12,
        .aad_ptr = (uintptr_t)aad, .aad_size = sizeof(aad),
        .src_ptr = (uintptr_t)plain, .src_size = sizeof(plain),
        .dst_ptr = (uintptr_t)encrypted, .dst_capacity = sizeof(encrypted),
    };
    void *session = NULL;
    char error[256];
    int attempt, n, end, authenticated;

    CHECK(!api->session_create(key, 16, 16, &session, error, sizeof(error)));
    CHECK(!api->session_seal(session, &request, error, sizeof(error)));
    CHECK(request.output_size == sizeof(encrypted));
    api->session_destroy(session);
    for (attempt = 0; attempt < 3; attempt++) {
        EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();

        if (attempt == 1) encrypted[32] ^= 1;
        if (attempt == 2) { encrypted[32] ^= 1; aad[0] ^= 1; }
        CHECK(ctx);
        CHECK(EVP_DecryptInit_ex(ctx, EVP_aes_128_gcm(), NULL, key, request.nonce));
        CHECK(EVP_DecryptUpdate(ctx, NULL, &n, aad, sizeof(aad)));
        CHECK(EVP_DecryptUpdate(ctx, out, &n, encrypted, sizeof(plain)));
        CHECK(EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, encrypted + 17));
        authenticated = EVP_DecryptFinal_ex(ctx, out + n, &end);
        CHECK(attempt ? !authenticated : authenticated);
        if (!attempt) CHECK(n == sizeof(plain) && !memcmp(out, plain, sizeof(plain)));
        EVP_CIPHER_CTX_free(ctx);
    }
    return 0;
}

static int cm_batches(void)
{
    static const unsigned int lengths[] = {0,1,15,16,17,1199,1200,1201,32768,65504};
    uint8_t key[36], input[65536], expected[65568], digest[20];
    uint8_t (*output)[65568] = calloc(32, 65568);
    struct onekvm_crypto_cm_request_v1 requests[32];
    char error[256];
    void *session = NULL;
    uint32_t completed;

    CHECK(output && api->cm_seal_batch);
    CHECK(api->features & ONEKVM_CRYPTO_FEATURE_AES_CM_TX_BATCH);
    CHECK(!(api->features & ONEKVM_CRYPTO_FEATURE_HMAC_SHA1));
    for (unsigned int i = 0; i < sizeof(key); i++) key[i] = i * 7 + 5;
    for (unsigned int i = 0; i < sizeof(input); i++) input[i] = i * 13 + 3;
    CHECK(api->primitive_create(3, key, 35, &session, error, sizeof(error)) < 0 && !session);
    CHECK(!api->primitive_create(3, key, 36, &session, error, sizeof(error)));
    CHECK(!api->cm_seal_batch(session, NULL, 0, &completed, error, sizeof(error)) && !completed);
    for (unsigned int iteration = 0; iteration < 2; iteration++) {
        for (unsigned int i = 0; i < 32; i++) {
            uint32_t header = i % 2 ? 28 : 12, length = lengths[i % 10];
            memset(output[i], 0xa5, 65568);
            memcpy(output[i] + 1, input, header + length);
            requests[i] = (struct onekvm_crypto_cm_request_v1){
                .struct_size = sizeof(requests[i]), .src_size = header + length,
                .header_size = header, .roc = i % 2 ? 0x1234ffff : 0xffffffff,
                .src_ptr = (uintptr_t)(i % 2 ? output[i] + 1 : input),
                .dst_ptr = (uintptr_t)(output[i] + 1), .dst_capacity = 65566,
                .tag_size = i % 2 ? 4 : 10,
            };
            memset(requests[i].iv, 0xff, 16);
            requests[i].iv[0] = i + iteration;
            requests[i].iv[15] = 0xfe; /* carries through the low 64 bits */
        }
        CHECK(!api->cm_seal_batch(session, requests, 32, &completed, error, sizeof(error)));
        CHECK(completed == 32);
        for (unsigned int i = 0; i < 32; i++) {
            struct onekvm_crypto_cm_request_v1 *r = &requests[i];
            unsigned int digest_size;
            memcpy(expected, input, r->header_size);
            CHECK(!reference(1, key, 16, r->iv, NULL, 0, input + r->header_size,
                             r->src_size - r->header_size, expected + r->header_size));
            expected[r->src_size] = r->roc >> 24;
            expected[r->src_size + 1] = r->roc >> 16;
            expected[r->src_size + 2] = r->roc >> 8;
            expected[r->src_size + 3] = r->roc;
            CHECK(HMAC(EVP_sha1(), key + 16, 20, expected, r->src_size + 4, digest, &digest_size));
            CHECK(digest_size == 20 && r->output_size == r->src_size + r->tag_size);
            CHECK(!memcmp(output[i] + 1, expected, r->src_size));
            CHECK(!memcmp(output[i] + 1 + r->src_size, digest, r->tag_size));
            CHECK(output[i][0] == 0xa5 && output[i][1 + r->output_size] == 0xa5);
        }
    }
    /* Invalid metadata must leave the failed packet and suffix unpublished. */
    requests[1].tag_size = 16;
    requests[1].output_size = 0;
    CHECK(api->cm_seal_batch(session, requests, 3, &completed, error, sizeof(error)) < 0);
    CHECK(completed == 1 && !requests[1].output_size);
    requests[0].header_size = requests[0].src_size + 1;
    CHECK(api->cm_seal_batch(session, requests, 1, &completed, error, sizeof(error)) < 0 && !completed);
    requests[0].header_size = 12;
    requests[0].src_size = UINT32_MAX;
    CHECK(api->cm_seal_batch(session, requests, 1, &completed, error, sizeof(error)) < 0 && !completed);
    CHECK(api->cm_seal_batch(session, requests, 33, &completed, error, sizeof(error)) < 0 && !completed);
    struct onekvm_crypto_primitive_request_v1 ctr = {.struct_size = sizeof(ctr), .iv_size = 16};
    CHECK(api->primitive_run(session, &ctr, error, sizeof(error)) < 0);
    api->session_destroy(session);
    free(output);
    return 0;
}

/* Host model additionally proves submission count and failure publication. */
static int cm_submission_checks(void *library)
{
    unsigned int (*run_count)(void) = dlsym(library, "crypto_mock_run_count");
    void (*fail_next)(void) = dlsym(library, "crypto_mock_fail_next");
    if (!run_count || !fail_next) return 0;
    uint8_t key[36] = {0}, src[1212] = {0}, dst[8][1222];
    struct onekvm_crypto_cm_request_v1 requests[8];
    void *session = NULL;
    char error[128];
    uint32_t completed;
    CHECK(!api->primitive_create(3, key, 36, &session, error, sizeof(error)));
    for (unsigned int i = 0; i < 8; i++) {
        requests[i] = (struct onekvm_crypto_cm_request_v1){
            .struct_size = sizeof(requests[i]), .src_ptr = (uintptr_t)src,
            .dst_ptr = (uintptr_t)dst[i], .src_size = sizeof(src), .header_size = 12,
            .dst_capacity = sizeof(dst[i]), .tag_size = 10,
        };
        requests[i].iv[13] = i;
    }
    unsigned int before = run_count();
    CHECK(!api->cm_seal_batch(session, requests, 8, &completed, error, sizeof(error)));
    CHECK(completed == 8 && run_count() == before + 1);
    fail_next();
    CHECK(api->cm_seal_batch(session, requests, 8, &completed, error, sizeof(error)) == -EIO);
    CHECK(!completed);
    for (unsigned int i = 0; i < 8; i++) CHECK(!requests[i].output_size);
    CHECK(!api->cm_seal_batch(session, requests, 8, &completed, error, sizeof(error)));
    CHECK(completed == 8); /* failed submission was not retried */
    before = run_count();
    for (unsigned int i = 0; i < 8; i++) requests[i].src_size = 12;
    CHECK(!api->cm_seal_batch(session, requests, 8, &completed, error, sizeof(error)));
    CHECK(completed == 8 && run_count() == before); /* header-only packets */
    api->session_destroy(session);
    return 0;
}

static void *concurrent(void *index)
{
    unsigned int i;
    for (i = 0; i < 24; i++)
        if (vector_case(i % 2, 16, 1199 + ((uintptr_t)index + i) % 8, i % 27, i % 2, 0))
            return (void *)1;
    if (cm_batches())
        return (void *)1;
    return NULL;
}

static double seconds(clockid_t clock)
{
    struct timespec value;
    clock_gettime(clock, &value);
    return value.tv_sec + value.tv_nsec / 1e9;
}

static int benchmark(unsigned int length)
{
    uint8_t key[16] = {0}, src[65536] = {0}, dst[65552];
    char error[128];
    void *session = NULL;
    struct onekvm_crypto_request_v1 request = {
        .struct_size = sizeof(request), .src_ptr = (uintptr_t)src, .src_size = length,
        .dst_ptr = (uintptr_t)dst, .dst_capacity = sizeof(dst), .nonce_size = 12,
    };
    double wall, cpu, elapsed;
    unsigned int count = 0;

    CHECK(!api->session_create(key, 16, 16, &session, error, sizeof(error)));
    wall = seconds(CLOCK_MONOTONIC); cpu = seconds(CLOCK_PROCESS_CPUTIME_ID);
    do {
        /* This test key is never used outside the benchmark. */
        memcpy(request.nonce, &count, sizeof(count));
        CHECK(!api->session_seal(session, &request, error, sizeof(error)));
        count++;
    } while (seconds(CLOCK_MONOTONIC) - wall < .35);
    elapsed = seconds(CLOCK_MONOTONIC) - wall;
    printf("GCM %u B: %.2f MiB/s, process CPU %.1f%% of one core, %.1f us/request\n",
           length, count * (double)length / elapsed / 1048576,
           100 * (seconds(CLOCK_PROCESS_CPUTIME_ID) - cpu) / elapsed,
           elapsed * 1e6 / count);
    api->session_destroy(session);
    return 0;
}

int main(int argc, char **argv)
{
    const unsigned int lengths[] = {0,1,15,16,17,31,32,33,1199,1200,1201,8191,32768,65472,65536};
    const unsigned int aads[] = {0,1,12,20,33,64,65536};
    void *library = dlopen(argc > 1 ? argv[1] : "/usr/lib/onekvm/video-backends/ax630c.so",
                            RTLD_NOW | RTLD_LOCAL);
    const void *raw = (void *)1;
    onekvm_crypto_backend_query_fn query;
    char error[256] = {0};
    unsigned int i, j, keylen, ctr;
    pthread_t threads[4];
    pthread_attr_t attributes;
    void *result;

    if (!library) fprintf(stderr, "%s\n", dlerror());
    CHECK(library);
    *(void **)(&query) = dlsym(library, ONEKVM_CRYPTO_BACKEND_QUERY_SYMBOL);
    CHECK(query);
    CHECK(query(99, &raw) < 0 && raw == NULL);
    CHECK(query(1, NULL) < 0);
    CHECK(!query(1, &raw)); api = raw;
    CHECK(api && api->struct_size >= sizeof(*api) && !strcmp(api->driver_id, "ax630c"));
    CHECK(api->probe(error, sizeof(error)) == 0 || (fprintf(stderr, "%s\n", error), 0));
    CHECK(!boundaries());
    for (ctr = 0; ctr < 2; ctr++)
        for (keylen = 16; keylen <= 32; keylen += 8) {
            if (!ctr && keylen == 24) continue;
            for (i = 0; i < sizeof(lengths)/sizeof(*lengths); i++)
                for (j = 0; j < (ctr ? 1 : sizeof(aads)/sizeof(*aads)); j++) {
                    if (lengths[i] + aads[j] > api->max_input) continue;
                    CHECK(!vector_case(ctr, keylen, lengths[i], aads[j], 0, 0));
                    CHECK(!vector_case(ctr, keylen, lengths[i], aads[j], 1, 0));
                }
        }
    CHECK(!vector_case(1, 16, 1201, 0, 0, 1));
    CHECK(!vector_case(1, 32, 65472, 0, 1, 1));
    CHECK(!mixed_batches(16));
    CHECK(!mixed_batches(32));
    CHECK(!cm_batches());
    CHECK(!cm_submission_checks(library));
    CHECK(!tag_rejection());
    /* musl's default pthread stack is smaller than these test vector buffers. */
    CHECK(!pthread_attr_init(&attributes));
    CHECK(!pthread_attr_setstacksize(&attributes, 1024 * 1024));
    for (i = 0; i < 4; i++) CHECK(!pthread_create(&threads[i], &attributes, concurrent, (void *)(uintptr_t)i));
    CHECK(!pthread_attr_destroy(&attributes));
    for (i = 0; i < 4; i++) { CHECK(!pthread_join(threads[i], &result)); CHECK(!result); }
    puts("PASS: ABI, AES-CTR/GCM/CM vectors, arbitrary lengths, AAD, in-place, counter carry, mixed 32-item batches, batch prefix, CM tags/ROC, tag/AAD rejection, 4 concurrent sessions");
    CHECK(!benchmark(1200)); CHECK(!benchmark(32768));
    dlclose(library);
    return 0;
}
