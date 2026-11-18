#ifndef KATOSHE_DOWNLOAD_H
#define KATOSHE_DOWNLOAD_H

#include <stddef.h>

/* Called while the transfer runs, at most a few times a second. `total` is 0
   when the server did not send a Content-Length. */
typedef void (*katoshe_progress_fn)(void *user, long long received, long long total);

typedef struct {
    const char *proxy;    /* NULL/empty = direct */
    const char *token;    /* NULL/empty = no Authorization header */
    int timeout_sec;      /* 0 = 60 */
    int retries;          /* 0 = 3 attempts after the first */
    katoshe_progress_fn progress; /* may be NULL */
    void *user;
} katoshe_download_opts;

typedef struct {
    long long bytes;
    char sha256[72];  /* lowercase hex of the bytes written */
    int resumed;      /* 1 when a 206 continued a previous .part */
} katoshe_download_result;

/* Downloads `url` into `<final_path>.part`, hashing while writing, then
   replaces `final_path` atomically. A non-NULL, non-empty expected_sha256 (64
   hex chars) must match or the .part is deleted and final_path is untouched.
   Returns 0 on success. */
int katoshe_download(const char *url, const char *final_path, const char *expected_sha256,
                     const katoshe_download_opts *opt, katoshe_download_result *result,
                     char *err, size_t errlen);

/* File-backed checks: wrong digest, replace, stale .part, missing source.
   Prints one "FAIL <name>" line per failure, returns the count. */
int katoshe_download_selftest(void);

#endif
