#include "sha256.h"

#include <windows.h>
#include <bcrypt.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    BCRYPT_ALG_HANDLE alg;
    BCRYPT_HASH_HANDLE hash;
    PUCHAR object;
    DWORD object_len;
} sha_impl;

#define SHA_CHUNK (1024u * 1024u)

static void hex_encode(const unsigned char *in, size_t len, char *out) {
    static const char digits[] = "0123456789abcdef";
    size_t i;

    for (i = 0; i < len; i++) {
        out[i * 2] = digits[in[i] >> 4];
        out[i * 2 + 1] = digits[in[i] & 0x0f];
    }
    out[len * 2] = '\0';
}

int katoshe_sha256_init(katoshe_sha256 *h) {
    sha_impl *impl;
    DWORD got = 0;
    NTSTATUS st;

    if (!h) return -1;
    h->impl = NULL;

    impl = (sha_impl *)calloc(1, sizeof *impl);
    if (!impl) return -1;

    st = BCryptOpenAlgorithmProvider(&impl->alg, BCRYPT_SHA256_ALGORITHM, NULL, 0);
    if (!BCRYPT_SUCCESS(st)) goto fail;
    st = BCryptGetProperty(impl->alg, BCRYPT_OBJECT_LENGTH, (PUCHAR)&impl->object_len,
                           (ULONG)sizeof impl->object_len, &got, 0);
    if (!BCRYPT_SUCCESS(st) || impl->object_len == 0) goto fail;
    impl->object = (PUCHAR)malloc(impl->object_len);
    if (!impl->object) goto fail;
    st = BCryptCreateHash(impl->alg, &impl->hash, impl->object, impl->object_len, NULL, 0, 0);
    if (!BCRYPT_SUCCESS(st)) goto fail;

    h->impl = impl;
    return 0;

fail:
    if (impl->hash) BCryptDestroyHash(impl->hash);
    free(impl->object);
    if (impl->alg) BCryptCloseAlgorithmProvider(impl->alg, 0);
    free(impl);
    return -1;
}

int katoshe_sha256_update(katoshe_sha256 *h, const void *data, size_t len) {
    sha_impl *impl;
    const unsigned char *p = (const unsigned char *)data;

    if (!h || !h->impl || (!data && len > 0)) return -1;
    impl = (sha_impl *)h->impl;

    while (len > 0) {
        ULONG n = len > SHA_CHUNK ? SHA_CHUNK : (ULONG)len;
        if (!BCRYPT_SUCCESS(BCryptHashData(impl->hash, (PUCHAR)p, n, 0))) return -1;
        p += n;
        len -= n;
    }
    return 0;
}

int katoshe_sha256_final(katoshe_sha256 *h, char *hex, size_t cap) {
    sha_impl *impl;
    unsigned char digest[32];
    NTSTATUS st;

    if (!hex || cap < sizeof digest * 2 + 1) return -1;
    if (!h || !h->impl) return -1;

    impl = (sha_impl *)h->impl;
    st = BCryptFinishHash(impl->hash, digest, (ULONG)sizeof digest, 0);
    katoshe_sha256_free(h);
    if (!BCRYPT_SUCCESS(st)) {
        hex[0] = '\0';
        return -1;
    }
    hex_encode(digest, sizeof digest, hex);
    return 0;
}

void katoshe_sha256_free(katoshe_sha256 *h) {
    sha_impl *impl;

    if (!h || !h->impl) return;
    impl = (sha_impl *)h->impl;
    if (impl->hash) BCryptDestroyHash(impl->hash);
    free(impl->object);
    if (impl->alg) BCryptCloseAlgorithmProvider(impl->alg, 0);
    free(impl);
    h->impl = NULL;
}

int katoshe_sha256_file(const char *path, char *hex, size_t cap, char *err, size_t errlen) {
    katoshe_sha256 h;
    FILE *f;
    char buf[65536];
    size_t n;
    int rc;

    if (err && errlen) err[0] = '\0';
    h.impl = NULL;

    if (!path || !path[0] || !hex || cap < 65) {
        if (err && errlen) snprintf(err, errlen, "sha256: invalid arguments");
        return -1;
    }
    f = fopen(path, "rb");
    if (!f) {
        if (err && errlen) snprintf(err, errlen, "%s: cannot open for hashing", path);
        return -1;
    }
    rc = katoshe_sha256_init(&h);
    while (rc == 0) {
        n = fread(buf, 1, sizeof buf, f);
        if (n == 0) break;
        rc = katoshe_sha256_update(&h, buf, n);
    }
    if (rc == 0 && ferror(f)) rc = -1;
    fclose(f);

    if (rc == 0) rc = katoshe_sha256_final(&h, hex, cap);
    katoshe_sha256_free(&h);
    if (rc != 0 && err && errlen) snprintf(err, errlen, "%s: cannot hash the file", path);
    return rc;
}

static int expect(const char *name, const char *got, const char *want) {
    if (strcmp(got, want) == 0) return 0;
    printf("FAIL %s\n", name);
    return 1;
}

int katoshe_sha256_selftest(void) {
    static const char empty_sha[] = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    static const char abc_sha[] = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    static const char two_block_sha[] = "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1";
    static const char million_sha[] = "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0";
    static const char two_block[] =
        "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    katoshe_sha256 h;
    char hex[65];
    char chunk[7777];
    long remaining;
    int failures = 0;

    h.impl = NULL;
    if (katoshe_sha256_init(&h) != 0 || katoshe_sha256_final(&h, hex, sizeof hex) != 0) {
        printf("FAIL empty\n");
        failures++;
    } else {
        failures += expect("empty", hex, empty_sha);
    }

    h.impl = NULL;
    if (katoshe_sha256_init(&h) != 0 || katoshe_sha256_update(&h, "abc", 3) != 0 ||
        katoshe_sha256_final(&h, hex, sizeof hex) != 0) {
        printf("FAIL abc\n");
        failures++;
    } else {
        failures += expect("abc", hex, abc_sha);
    }

    h.impl = NULL;
    if (katoshe_sha256_init(&h) != 0 ||
        katoshe_sha256_update(&h, two_block, strlen(two_block)) != 0 ||
        katoshe_sha256_final(&h, hex, sizeof hex) != 0) {
        printf("FAIL two-block\n");
        failures++;
    } else {
        failures += expect("two-block", hex, two_block_sha);
    }

    memset(chunk, 'a', sizeof chunk);
    h.impl = NULL;
    if (katoshe_sha256_init(&h) != 0) {
        printf("FAIL million-a\n");
        failures++;
    } else {
        for (remaining = 1000000; remaining > 0; remaining -= (long)sizeof chunk) {
            size_t n = (size_t)(remaining < (long)sizeof chunk ? remaining : (long)sizeof chunk);
            if (katoshe_sha256_update(&h, chunk, n) != 0) break;
        }
        if (remaining > 0 || katoshe_sha256_final(&h, hex, sizeof hex) != 0) {
            printf("FAIL million-a\n");
            failures++;
        } else {
            failures += expect("million-a", hex, million_sha);
        }
        katoshe_sha256_free(&h);
    }

    return failures;
}
