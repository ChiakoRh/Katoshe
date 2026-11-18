#ifndef KATOSHE_SHA256_H
#define KATOSHE_SHA256_H

#include <stddef.h>

/* SHA-256 through Windows CNG (BCrypt), no bundled implementation. */
typedef struct {
    void *impl;
} katoshe_sha256;

/* All return 0 on success. */
int katoshe_sha256_init(katoshe_sha256 *h);
int katoshe_sha256_update(katoshe_sha256 *h, const void *data, size_t len);
/* Writes 64 lowercase hex digits plus NUL; releases the handle. */
int katoshe_sha256_final(katoshe_sha256 *h, char *hex, size_t cap);

/* Safe on a zeroed struct and after a successful final. */
void katoshe_sha256_free(katoshe_sha256 *h);

/* Streams a file, never reading it twice. */
int katoshe_sha256_file(const char *path, char *hex, size_t cap, char *err, size_t errlen);

/* Standard vectors plus a chunked update across block boundaries.
   Prints one "FAIL <name>" line per failure, returns the failure count. */
int katoshe_sha256_selftest(void);

#endif
