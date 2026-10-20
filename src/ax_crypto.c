/* SPDX-License-Identifier: MIT */
/* Native ABI v1 over the EIP-130 fd/ioctl UAPI. AES runs on EIP-130;
 * OpenSSL's GCM construction supplies GHASH through a hardware-only block
 * callback. The kernel selects the hardware driver by its exact name.
 */
#define _GNU_SOURCE
#include <onekvm/crypto_backend_v1.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/axera_eip130.h>
#include <openssl/modes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define AX_MAX_INPUT 65536u
#define MAX_BATCH 32u
#define DEVICE_PATH "/dev/axera-eip130"
#define STREAM_CAPACITY (AX_MAX_INPUT + 16u)
#define RECOVER_PATH "/sys/bus/platform/devices/4900000.crypto/recover"

/* Recovery does not retry this packet. The caller receives the original
 * failure, drops the whole batch, and can retry hardware on the same fd
 * after its backoff. An unprivileged caller simply cannot request recovery. */
static void recover_engine(int code)
{
    int fd;

    if (code != EIO && code != ETIMEDOUT)
        return;
    fd = open(RECOVER_PATH, O_WRONLY | O_CLOEXEC);
    if (fd >= 0) {
        ssize_t written = write(fd, "1\n", 2);
        (void)written;
        close(fd);
    }
}

struct ax_session {
    int fd;
    bool ctr;
    GCM128_CONTEXT *gcm;
    uint8_t hash_subkey[16];
    uint8_t *counters, *stream;
    uint8_t nonce[12];
    uint32_t active_offset, active_size;
    bool initializing, block_error;
};

static int32_t run(struct ax_session *session, const uint8_t *iv, uint32_t iv_size,
                   const void *src, uint32_t src_size, void *dst,
                   char *error, uint32_t capacity);

/* No software AES: H is computed by EIP-130 at session creation; every other
 * AES block is read from a completed hardware ECB batch. Reject unexpected
 * OpenSSL counter requests instead of ever selecting a CPU cipher. */
static void hardware_block(const unsigned char in[16], unsigned char out[16],
                           const void *key)
{
    struct ax_session *session = (struct ax_session *)key;
    uint32_t counter, offset;

    if (session->initializing) {
        memcpy(out, session->hash_subkey, 16);
        return;
    }
    counter = (uint32_t)in[12] << 24 | (uint32_t)in[13] << 16 |
              (uint32_t)in[14] << 8 | in[15];
    offset = (counter - 1) * 16u;
    if (!counter || counter > session->active_size / 16 ||
        memcmp(in, session->nonce, 12)) {
        session->block_error = true;
        memset(out, 0, 16);
        return;
    }
    memcpy(out, session->stream + session->active_offset + offset, 16);
}

static int32_t fail(char *error, uint32_t capacity, const char *what, int code)
{
    if (error && capacity)
        snprintf(error, capacity, "%s: %s", what, strerror(code));
    return -code;
}

static void destroy(void *opaque)
{
    struct ax_session *session = opaque;
    if (!session)
        return;
    if (session->fd >= 0)
        close(session->fd);
    CRYPTO_gcm128_release(session->gcm);
    if (session->counters) {
        explicit_bzero(session->counters, STREAM_CAPACITY);
        free(session->counters);
    }
    if (session->stream) {
        explicit_bzero(session->stream, STREAM_CAPACITY);
        free(session->stream);
    }
    explicit_bzero(session, sizeof(*session));
    free(session);
}

static int32_t create(bool ctr, const uint8_t *key, uint32_t key_size,
                      void **out, char *error, uint32_t capacity)
{
    struct axera_eip130_key parameters = {
        .version = AXERA_EIP130_ABI,
        .algorithm = ctr ? AXERA_EIP130_ALG_CTR : AXERA_EIP130_ALG_ECB,
        .key_size = key_size,
    };
    struct ax_session *session;
    int code;

    if (out)
        *out = NULL;
    if (!out || !key || (key_size != 16 && key_size != 32 &&
                         !(ctr && key_size == 24)))
        return fail(error, capacity, "invalid AES key/session", EINVAL);
    session = calloc(1, sizeof(*session));
    if (!session)
        return fail(error, capacity, "allocate crypto session", ENOMEM);
    session->fd = open(DEVICE_PATH, O_RDWR | O_CLOEXEC);
    session->ctr = ctr;
    if (session->fd < 0)
        goto failed;
    memcpy(parameters.key, key, key_size);
    code = ioctl(session->fd, AXERA_EIP130_SET_KEY, &parameters);
    explicit_bzero(&parameters, sizeof(parameters));
    if (code < 0)
        goto failed;
    if (!ctr) {
        uint8_t zero[16] = {0};

        session->counters = malloc(STREAM_CAPACITY);
        session->stream = malloc(STREAM_CAPACITY);
        if (!session->counters || !session->stream) {
            errno = ENOMEM;
            goto failed;
        }
        code = run(session, NULL, 0, zero, 16, session->hash_subkey, error, capacity);
        if (code) {
            destroy(session);
            return code;
        }
        session->initializing = true;
        session->gcm = CRYPTO_gcm128_new(session, hardware_block);
        session->initializing = false;
        if (!session->gcm) {
            errno = ENOMEM;
            goto failed;
        }
    }
    *out = session;
    return 0;
failed:
    code = errno;
    destroy(session);
    recover_engine(code);
    return fail(error, capacity, "EIP-130 native session", code);
}

static int32_t session_create(const uint8_t *key, uint32_t key_size,
                              uint32_t tag_size, void **session,
                              char *error, uint32_t capacity)
{
    if (session)
        *session = NULL;
    if (tag_size != 16)
        return fail(error, capacity, "GCM requires a 16-byte tag", EINVAL);
    return create(false, key, key_size, session, error, capacity);
}

static int32_t run(struct ax_session *session, const uint8_t *iv, uint32_t iv_size,
                   const void *src, uint32_t src_size, void *dst,
                   char *error, uint32_t capacity)
{
    struct axera_eip130_request request = {
        .version = AXERA_EIP130_ABI,
        .src = (uintptr_t)src, .dst = (uintptr_t)dst, .size = src_size,
        .iv_size = iv_size,
    };
    int code;

    if (iv_size)
        memcpy(request.iv, iv, iv_size);
    /* A synchronous ioctl owns one complete request. After a failure there
     * is no socket record or unread output to accidentally reuse. Do not
     * retry this operation (including EINTR) or substitute software AES. */
    code = ioctl(session->fd, AXERA_EIP130_RUN, &request);
    if (code == 0 && request.output_size == src_size)
        return 0;
    code = code < 0 ? errno : EIO;
    recover_engine(code);
    return fail(error, capacity, "EIP-130 request", code);
}

static int32_t validate(struct ax_session *session,
                        struct onekvm_crypto_request_v1 *request,
                        char *error, uint32_t capacity)
{
    if (!request || request->struct_size < sizeof(*request))
        return fail(error, capacity, "invalid GCM request size", EINVAL);
    request->output_size = 0;
    if (!session || session->ctr || request->flags || request->nonce_size != 12 ||
        (uint64_t)request->aad_size + request->src_size > AX_MAX_INPUT ||
        (uint64_t)request->src_size + 16 > request->dst_capacity ||
        !request->dst_ptr || (request->src_size && !request->src_ptr) ||
        (request->aad_size && !request->aad_ptr))
        return fail(error, capacity, "invalid GCM request", EINVAL);
    return 0;
}

static int32_t seal_batch(void *opaque, struct onekvm_crypto_request_v1 *requests,
                          uint32_t count, uint32_t *completed,
                          char *error, uint32_t capacity)
{
    struct ax_session *session = opaque;
    uint32_t i, end, start, used, offset[MAX_BATCH], size[MAX_BATCH], block;
    int32_t code, pending;

    if (completed)
        *completed = 0;
    if (!opaque || !completed || (count && !requests) || count > MAX_BATCH)
        return fail(error, capacity, "invalid GCM batch", EINVAL);
    while (*completed < count) {
        start = end = *completed;
        used = 0;
        pending = 0;
        while (end < count) {
            struct onekvm_crypto_request_v1 *r = &requests[end];
            uint32_t bytes;

            pending = validate(session, r, error, capacity);
            if (pending)
                break;
            bytes = ((r->src_size + 15u) / 16u + 1u) * 16u;
            if (bytes > STREAM_CAPACITY - used)
                break;
            offset[end - start] = used;
            size[end - start] = bytes;
            /* J0 first, then inc32(J0) for each payload block. */
            for (block = 0; block < bytes / 16; block++) {
                uint8_t *counter = session->counters + used + block * 16;
                uint32_t value = block + 1;

                memcpy(counter, r->nonce, 12);
                counter[12] = value >> 24; counter[13] = value >> 16;
                counter[14] = value >> 8; counter[15] = value;
            }
            used += bytes;
            end++;
        }
        if (end == start)
            return pending ?: fail(error, capacity, "GCM counter batch overflow", EOVERFLOW);
        code = run(session, NULL, 0, session->counters, used, session->stream,
                   error, capacity);
        if (!code) {
            for (i = start; i < end; i++) {
                struct onekvm_crypto_request_v1 *r = &requests[i];
                uint8_t *dst = (void *)(uintptr_t)r->dst_ptr;

                session->active_offset = offset[i - start];
                session->active_size = size[i - start];
                memcpy(session->nonce, r->nonce, 12);
                session->block_error = false;
                CRYPTO_gcm128_setiv(session->gcm, r->nonce, 12);
                if (CRYPTO_gcm128_aad(session->gcm, (void *)(uintptr_t)r->aad_ptr, r->aad_size) ||
                    CRYPTO_gcm128_encrypt(session->gcm, (void *)(uintptr_t)r->src_ptr, dst, r->src_size) ||
                    session->block_error) {
                    code = fail(error, capacity, "hardware GCM construction", EIO);
                    break;
                }
                CRYPTO_gcm128_tag(session->gcm, dst + r->src_size, 16);
                r->output_size = r->src_size + 16;
                *completed = i + 1;
            }
        }
        /* The keystream and GHASH key are secret key material. Wipe each
         * staging group before returning, including on hardware failure. */
        session->active_size = 0;
        explicit_bzero(session->counters, used);
        explicit_bzero(session->stream, used);
        if (code || pending)
            return code ?: pending;
    }
    return 0;
}

static int32_t seal(void *opaque, struct onekvm_crypto_request_v1 *request,
                    char *error, uint32_t capacity)
{
    uint32_t completed;

    if (!request)
        return fail(error, capacity, "invalid GCM request", EINVAL);
    return seal_batch(opaque, request, 1, &completed, error, capacity);
}

static int32_t primitive_create(uint32_t algorithm, const uint8_t *key,
                               uint32_t key_size, void **session,
                               char *error, uint32_t capacity)
{
    if (session)
        *session = NULL;
    if (algorithm != ONEKVM_CRYPTO_ALGORITHM_AES_CTR)
        return fail(error, capacity, "unsupported crypto primitive", ENOTSUP);
    return create(true, key, key_size, session, error, capacity);
}

static int32_t primitive_run(void *opaque,
                             struct onekvm_crypto_primitive_request_v1 *request,
                             char *error, uint32_t capacity)
{
    struct ax_session *session = opaque;
    int32_t code;

    if (!request || request->struct_size < sizeof(*request))
        return fail(error, capacity, "invalid CTR request size", EINVAL);
    request->output_size = 0;
    if (!session || !session->ctr || request->flags || request->iv_size != 16 ||
        request->src_size > AX_MAX_INPUT || request->src_size > request->dst_capacity ||
        (request->src_size && (!request->src_ptr || !request->dst_ptr)))
        return fail(error, capacity, "invalid CTR request", EINVAL);
    /* An empty CTR input needs no AES blocks. */
    if (!request->src_size)
        return 0;
    code = run(session, request->iv, 16,
               (const void *)(uintptr_t)request->src_ptr, request->src_size,
               (void *)(uintptr_t)request->dst_ptr, error, capacity);
    if (!code)
        request->output_size = request->src_size;
    return code;
}

static int32_t probe(char *error, uint32_t capacity)
{
    static const uint8_t expected[32] = {
        0x03,0x88,0xda,0xce,0x60,0xb6,0xa3,0x92,0xf3,0x28,0xc2,0xb9,0x71,0xb2,0xfe,0x78,
        0xab,0x6e,0x47,0xd4,0x2c,0xec,0x13,0xbd,0xf5,0x3a,0x67,0xb2,0x12,0x57,0xbd,0xdf
    };
    uint8_t key[16] = {0}, input[16] = {0}, output[32];
    void *session = NULL;
    struct onekvm_crypto_request_v1 request = {
        .struct_size = sizeof(request), .nonce_size = 12,
        .src_ptr = (uintptr_t)input, .src_size = sizeof(input),
        .dst_ptr = (uintptr_t)output, .dst_capacity = sizeof(output),
    };
    int32_t code = session_create(key, sizeof(key), 16, &session, error, capacity);

    if (!code)
        code = seal(session, &request, error, capacity);
    if (!code && memcmp(output, expected, sizeof(expected)))
        code = fail(error, capacity, "EIP-130 GCM self-test", EIO);
    destroy(session);
    return code;
}

static const struct onekvm_crypto_backend_v1 backend = {
    .struct_size = sizeof(backend), .abi_version = ONEKVM_CRYPTO_BACKEND_ABI_V1,
    .driver_id = "ax630c", .backend_name = "AX630C EIP-130 batch AES / GHASH",
    .features = ONEKVM_CRYPTO_FEATURE_AES_GCM_TX |
                ONEKVM_CRYPTO_FEATURE_AES_GCM_TX_BATCH | ONEKVM_CRYPTO_FEATURE_AES_CTR |
                ONEKVM_CRYPTO_FEATURE_CONCURRENT_H265_VIDEO,
    .max_batch = MAX_BATCH, .max_input = AX_MAX_INPUT, .payload_alignment = 1,
    .probe = probe, .session_create = session_create, .session_seal = seal,
    .session_seal_batch = seal_batch, .session_destroy = destroy,
    .primitive_create = primitive_create, .primitive_run = primitive_run,
    .max_primitive_input = AX_MAX_INPUT,
};

int32_t onekvm_crypto_backend_query(uint32_t requested_abi, const void **api)
{
    if (!api)
        return -EINVAL;
    *api = NULL;
    if (requested_abi != ONEKVM_CRYPTO_BACKEND_ABI_V1)
        return -ENOTSUP;
    *api = &backend;
    return 0;
}
