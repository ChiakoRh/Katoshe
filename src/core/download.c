#include "download.h"

#include <curl/curl.h>
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "io.h"
#include "katoshe.h"
#include "sha256.h"

#define DL_CHUNK 65536u
#define DL_PATH_MAX 1024
#define DL_PROGRESS_MS 250

static int curl_ready;

typedef struct {
    FILE *f;
    const char *part;
    katoshe_sha256 sha;
    long response;
    int resume_sent;
    int started;
    int restarted;
    int write_failed;
    long long total; /* bytes on disk for this file, resumed prefix included */
    long long base;  /* size of the resumed prefix for this response */
    katoshe_progress_fn progress;
    void *user;
    ULONGLONG last_progress;
} dl_state;

static void set_error(char *err, size_t errlen, const char *url, const char *msg) {
    if (err && errlen) snprintf(err, errlen, "%s: %s", url, msg);
}

static int add_header(struct curl_slist **list, const char *line) {
    struct curl_slist *next = curl_slist_append(*list, line);
    if (!next) return -1;
    *list = next;
    return 0;
}

/* Truncates the .part and starts a fresh hash. Used when the server answers a
   Range request with a full 200. */
static int restart_part(dl_state *s) {
    if (s->f) {
        fclose(s->f);
        s->f = NULL;
    }
    s->f = fopen(s->part, "wb");
    if (!s->f) return -1;
    katoshe_sha256_free(&s->sha);
    if (katoshe_sha256_init(&s->sha) != 0) return -1;
    s->total = 0;
    s->base = 0;
    s->restarted = 1;
    return 0;
}

static int hash_prefix(const char *path, katoshe_sha256 *sha, long long *size) {
    FILE *f;
    char buf[DL_CHUNK];
    size_t n;
    long long total = 0;

    f = fopen(path, "rb");
    if (!f) return -1;
    if (katoshe_sha256_init(sha) != 0) {
        fclose(f);
        return -1;
    }
    for (;;) {
        n = fread(buf, 1, sizeof buf, f);
        if (n == 0) break;
        if (katoshe_sha256_update(sha, buf, n) != 0) {
            fclose(f);
            katoshe_sha256_free(sha);
            return -1;
        }
        total += (long long)n;
    }
    if (ferror(f)) {
        fclose(f);
        katoshe_sha256_free(sha);
        return -1;
    }
    fclose(f);
    *size = total;
    return 0;
}

static size_t on_write(char *ptr, size_t size, size_t nmemb, void *userdata) {
    dl_state *s = (dl_state *)userdata;
    size_t n = size * nmemb;

    if (!s->started) {
        s->started = 1;
        /* Only a 206 continues a .part. Anything else (200, or no HTTP at all)
           describes the whole file, so the stale prefix is dropped. */
        if (s->resume_sent && s->response != 206 && restart_part(s) != 0) {
            s->write_failed = 1;
            return 0;
        }
    }
    /* Redirect and error bodies must never reach the .part. */
    if (s->response >= 300) return n;
    if (!s->f) {
        s->write_failed = 1;
        return 0;
    }
    if (fwrite(ptr, 1, n, s->f) != n) {
        s->write_failed = 1;
        return 0;
    }
    if (katoshe_sha256_update(&s->sha, ptr, n) != 0) {
        s->write_failed = 1;
        return 0;
    }
    s->total += (long long)n;
    return n;
}

static size_t on_header(char *buffer, size_t size, size_t nitems, void *userdata) {
    dl_state *s = (dl_state *)userdata;
    size_t n = size * nitems;

    if (n >= 5 && strncmp(buffer, "HTTP/", 5) == 0) {
        const char *space = (const char *)memchr(buffer, ' ', n);
        const char *p = space ? space + 1 : buffer;
        long code = 0;
        /* The header block is not NUL-terminated, so stop at the line end. */
        for (; p < buffer + n && *p >= '0' && *p <= '9'; p++) {
            code = code * 10 + (*p - '0');
        }
        s->response = code;
        s->started = 0; /* a redirect starts a new body */
    }
    return n;
}

static int on_progress(void *userdata, curl_off_t dltotal, curl_off_t dlnow,
                       curl_off_t ultotal, curl_off_t ulnow) {
    dl_state *s = (dl_state *)userdata;
    ULONGLONG now;

    (void)dlnow;
    (void)ultotal;
    (void)ulnow;
    if (!s->progress) return 0;
    now = GetTickCount64();
    if (s->last_progress && now - s->last_progress < DL_PROGRESS_MS) return 0;
    s->last_progress = now ? now : 1;
    s->progress(s->user, s->total,
                dltotal > 0 ? s->base + (long long)dltotal : 0);
    return 0;
}

static int rc_retryable(CURLcode rc) {
    switch (rc) {
    case CURLE_COULDNT_RESOLVE_PROXY:
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_COULDNT_CONNECT:
    case CURLE_OPERATION_TIMEDOUT:
    case CURLE_SEND_ERROR:
    case CURLE_RECV_ERROR:
    case CURLE_GOT_NOTHING:
    case CURLE_PARTIAL_FILE:
        return 1;
    default:
        return 0;
    }
}

static void backoff_sleep(int attempt) {
    int seconds = attempt >= 6 ? 30 : 1 << (attempt - 1);
    Sleep((DWORD)seconds * 1000u);
}

/* One transfer. On success the .part holds the full, hashed file and the
   result describes it; on failure the .part is left for the next attempt. */
static int dl_attempt(const char *url, const char *part, const katoshe_download_opts *opt,
                      katoshe_download_result *result, int *retryable,
                      char *err, size_t errlen) {
    dl_state s;
    CURL *curl = NULL;
    struct curl_slist *headers = NULL;
    char *auth = NULL;
    char agent[64];
    char range[64];
    char hex[65];
    char curl_err[CURL_ERROR_SIZE];
    WIN32_FILE_ATTRIBUTE_DATA fad;
    CURLcode rc;
    long status = 0;
    long timeout;
    int prc = -1;

    memset(&s, 0, sizeof s);
    s.part = part;
    s.progress = opt->progress;
    s.user = opt->user;
    *retryable = 0;
    result->resumed = 0;

    if (GetFileAttributesExA(part, GetFileExInfoStandard, &fad) &&
        (fad.nFileSizeHigh > 0 || fad.nFileSizeLow > 0)) {
        long long size = 0;
        if (hash_prefix(part, &s.sha, &size) != 0) {
            set_error(err, errlen, url, "cannot read the existing .part file");
            return -1;
        }
        s.total = size;
        s.base = size;
        s.f = fopen(part, "ab");
        if (!s.f) {
            set_error(err, errlen, url, "cannot open the .part file for appending");
            katoshe_sha256_free(&s.sha);
            return -1;
        }
    } else {
        if (katoshe_sha256_init(&s.sha) != 0) {
            set_error(err, errlen, url, "cannot initialise SHA-256");
            return -1;
        }
        s.f = fopen(part, "wb");
        if (!s.f) {
            set_error(err, errlen, url, "cannot create the .part file");
            katoshe_sha256_free(&s.sha);
            return -1;
        }
    }

    if (!curl_ready) {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0) {
            set_error(err, errlen, url, "cannot initialise libcurl");
            goto done;
        }
        curl_ready = 1;
    }
    curl = curl_easy_init();
    if (!curl) {
        set_error(err, errlen, url, "cannot initialise libcurl");
        goto done;
    }

    snprintf(agent, sizeof agent, "katoshe/%s", katoshe_version());
    if (add_header(&headers, "Accept-Encoding: identity") != 0) {
        set_error(err, errlen, url, "out of memory");
        goto done;
    }
    if (opt->token && opt->token[0]) {
        size_t n = strlen(opt->token) + 32;
        auth = (char *)malloc(n);
        if (!auth) {
            set_error(err, errlen, url, "out of memory");
            goto done;
        }
        snprintf(auth, n, "Authorization: Bearer %s", opt->token);
        if (add_header(&headers, auth) != 0) {
            set_error(err, errlen, url, "out of memory");
            goto done;
        }
    }
    if (s.base > 0) {
        snprintf(range, sizeof range, "Range: bytes=%lld-", s.base);
        if (add_header(&headers, range) != 0) {
            set_error(err, errlen, url, "out of memory");
            goto done;
        }
        s.resume_sent = 1;
    }

    timeout = opt->timeout_sec > 0 ? opt->timeout_sec : 60;
    curl_err[0] = '\0';

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, agent);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &s);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, on_header);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &s);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, on_progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &s);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 0L); /* status codes are needed */
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_err);
    /* No total timeout: a 200 MB asset over a slow link is not a hang. A
       transfer that stops moving for `timeout` seconds is. */
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, (long)timeout);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, timeout < 20 ? timeout : 20L);
    if (opt->proxy && opt->proxy[0]) curl_easy_setopt(curl, CURLOPT_PROXY, opt->proxy);

    rc = curl_easy_perform(curl);

    /* A resume request answered with anything but a 206 and no body: drop the
       stale prefix before finalising. */
    if (rc == CURLE_OK && s.resume_sent && s.response != 206 && !s.restarted && !s.started) {
        if (restart_part(&s) != 0) s.write_failed = 1;
    }
    if (s.f) {
        if (fclose(s.f) != 0) s.write_failed = 1;
        s.f = NULL;
    }

    if (s.write_failed) {
        snprintf(err, errlen, "%s: cannot write %s", url, part);
        goto done;
    }

    if (rc != CURLE_OK) {
        const char *why = curl_err[0] ? curl_err : curl_easy_strerror(rc);
        if (opt->proxy && opt->proxy[0]) {
            snprintf(err, errlen, "%s: %s (via proxy %s)", url, why, opt->proxy);
        } else {
            snprintf(err, errlen, "%s: %s", url, why);
        }
        *retryable = rc_retryable(rc);
        goto done;
    }

    if (curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status) != CURLE_OK) status = s.response;

    if (status == 416) {
        DeleteFileA(part);
        snprintf(err, errlen, "%s: HTTP 416, discarding the incomplete .part file", url);
        *retryable = 1;
        goto done;
    }
    if (status == 206 && !s.resume_sent) {
        DeleteFileA(part);
        snprintf(err, errlen, "%s: HTTP 206 without a range request", url);
        goto done;
    }
    /* 0 means the transfer was not HTTP at all (file:// in the selftest). */
    if (status != 0 && (status < 200 || status >= 300)) {
        snprintf(err, errlen, "%s: HTTP %ld", url, status);
        *retryable = status >= 500;
        goto done;
    }

    if (katoshe_sha256_final(&s.sha, hex, sizeof hex) != 0) {
        snprintf(err, errlen, "%s: cannot finish the SHA-256", url);
        goto done;
    }
    result->bytes = s.total;
    snprintf(result->sha256, sizeof result->sha256, "%s", hex);
    result->resumed = s.resume_sent && status == 206;
    prc = 0;

done:
    if (s.f) fclose(s.f);
    katoshe_sha256_free(&s.sha);
    if (curl) curl_easy_cleanup(curl);
    curl_slist_free_all(headers);
    free(auth);
    return prc;
}

static int replace_file(const char *url, const char *part, const char *final_path,
                        char *err, size_t errlen) {
    if (GetFileAttributesA(final_path) != INVALID_FILE_ATTRIBUTES) {
        if (ReplaceFileA(final_path, part, NULL, 0, NULL, NULL)) return 0;
        if (MoveFileExA(part, final_path, MOVEFILE_REPLACE_EXISTING)) return 0;
        snprintf(err, errlen, "%s: cannot replace %s", url, final_path);
        return -1;
    }
    if (!MoveFileExA(part, final_path, MOVEFILE_REPLACE_EXISTING)) {
        snprintf(err, errlen, "%s: cannot move the .part file to %s", url, final_path);
        return -1;
    }
    return 0;
}

int katoshe_download(const char *url, const char *final_path, const char *expected_sha256,
                     const katoshe_download_opts *opt, katoshe_download_result *result,
                     char *err, size_t errlen) {
    katoshe_download_opts defaults;
    const katoshe_download_opts *o = opt;
    char part[DL_PATH_MAX];
    char dir[DL_PATH_MAX];
    int retries;
    int attempt;
    int rc = -1;

    if (err && errlen) err[0] = '\0';
    if (!url || !url[0] || !final_path || !final_path[0] || !result) {
        if (err && errlen) snprintf(err, errlen, "download: invalid arguments");
        return -1;
    }
    memset(result, 0, sizeof *result);
    if (!o) {
        memset(&defaults, 0, sizeof defaults);
        o = &defaults;
    }
    if (expected_sha256 && expected_sha256[0] && strlen(expected_sha256) != 64) {
        snprintf(err, errlen, "%s: the expected SHA-256 must be 64 hex characters", url);
        return -1;
    }
    if (snprintf(part, sizeof part, "%s.part", final_path) >= (int)sizeof part) {
        snprintf(err, errlen, "%s: path too long", final_path);
        return -1;
    }
    katoshe_dirname(final_path, dir, sizeof dir);
    if (dir[0] && katoshe_mkdir_all(dir) != 0) {
        snprintf(err, errlen, "%s: cannot create the directory", final_path);
        return -1;
    }

    retries = o->retries > 0 ? o->retries : 3;
    for (attempt = 0; attempt <= retries; attempt++) {
        int retryable = 0;
        if (attempt > 0) backoff_sleep(attempt);
        if (dl_attempt(url, part, o, result, &retryable, err, errlen) == 0) {
            rc = 0;
            break;
        }
        if (!retryable) break;
    }

    if (rc == 0 && expected_sha256 && expected_sha256[0] &&
        _stricmp(expected_sha256, result->sha256) != 0) {
        DeleteFileA(part);
        snprintf(err, errlen, "%s: digest mismatch (got %s, expected %s)", url,
                 result->sha256, expected_sha256);
        rc = -1;
    }

    if (rc == 0) {
        WIN32_FILE_ATTRIBUTE_DATA fad;
        if (replace_file(url, part, final_path, err, errlen) != 0) {
            rc = -1;
        } else if (GetFileAttributesExA(final_path, GetFileExInfoStandard, &fad)) {
            result->bytes = ((long long)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
        }
    }
    return rc;
}

/* ---- self-check: file:// URLs, so no network is involved ---- */

static int failures;

static void check(int cond, const char *name) {
    if (!cond) {
        printf("FAIL %s\n", name);
        failures++;
    }
}

static void write_bytes(const char *path, const char *buf, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(buf, 1, n, f);
    fclose(f);
}

static long file_size(const char *path) {
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fad)) return -1;
    return (long)(((long long)fad.nFileSizeHigh << 32) | fad.nFileSizeLow);
}

static int file_is(const char *path, const char *buf, size_t n) {
    FILE *f = fopen(path, "rb");
    char *tmp;
    size_t got;
    int same;

    if (!f) return 0;
    tmp = (char *)malloc(n ? n : 1);
    if (!tmp) { fclose(f); return 0; }
    got = fread(tmp, 1, n, f);
    same = got == n && memcmp(tmp, buf, n) == 0;
    free(tmp);
    fclose(f);
    return same;
}

int katoshe_download_selftest(void) {
    char dir[MAX_PATH];
    char src[MAX_PATH];
    char dest[MAX_PATH];
    char part[MAX_PATH];
    char url[700];
    char hex[65];
    char wrong[65];
    char old[] = "OLD";
    katoshe_download_opts opt;
    katoshe_download_result res;
    char err[512];
    char *data;
    size_t total = 200000;
    size_t i;

    failures = 0;
    memset(&opt, 0, sizeof opt);
    opt.timeout_sec = 20;
    opt.retries = 0; /* one attempt: no test should sit through a backoff */

    snprintf(dir, sizeof dir, "%s\\katoshe-dl-%lu", getenv("TEMP") ? getenv("TEMP") : ".",
             (unsigned long)GetCurrentProcessId());
    katoshe_mkdir_all(dir);
    snprintf(src, sizeof src, "%s\\source.bin", dir);
    snprintf(dest, sizeof dest, "%s\\payload.bin", dir);
    snprintf(part, sizeof part, "%s.part", dest);

    data = (char *)malloc(total);
    if (!data) {
        printf("FAIL download: out of memory\n");
        return 1;
    }
    for (i = 0; i < total; i++) data[i] = (char)((i * 31 + 7) & 0xff);
    write_bytes(src, data, total);

    snprintf(url, sizeof url, "file:///%s", src);
    for (i = 0; url[i]; i++) if (url[i] == '\\') url[i] = '/';

    DeleteFileA(dest);
    DeleteFileA(part);
    write_bytes(dest, old, sizeof old - 1);

    /* 1. a digest that does not match leaves the destination alone */
    if (katoshe_sha256_file(src, hex, sizeof hex, err, sizeof err) != 0) {
        printf("FAIL download: cannot hash the source\n");
        failures++;
    } else {
        int rc;

        snprintf(wrong, sizeof wrong, "%s", hex);
        wrong[0] = wrong[0] == '0' ? '1' : '0';
        rc = katoshe_download(url, dest, wrong, &opt, &res, err, sizeof err);
        if (rc == 0) printf("     (%s)\n", "a mismatched digest was accepted");
        else if (!strstr(err, "digest")) printf("     (%s)\n", err);
        check(rc != 0, "a wrong digest fails");
        check(strstr(err, "digest") != NULL, "the mismatch is named as a digest problem");
        check(file_is(dest, old, sizeof old - 1), "the old file survives a mismatch");
        check(GetFileAttributesA(part) == INVALID_FILE_ATTRIBUTES, "the .part is deleted");

        /* 2. the right digest lands the file, replacing what was there */
        rc = katoshe_download(url, dest, hex, &opt, &res, err, sizeof err);
        if (rc != 0) printf("     (%s)\n", err);
        check(rc == 0, "the right digest succeeds");
        check(file_is(dest, data, total), "the destination holds the source bytes");
        check(res.bytes == (long long)total, "the result counts every byte");
        check(strcmp(res.sha256, hex) == 0, "the result carries the digest");

        /* 3. a stale .part answered with a full 200 is truncated, not appended */
        write_bytes(part, data, 50000);
        check(katoshe_download(url, dest, hex, &opt, &res, err, sizeof err) == 0,
              "a stale .part still completes");
        check(file_is(dest, data, total), "the stale prefix was dropped");
        check(res.bytes == (long long)total, "the byte count is the whole file");
    }

    /* 4. a file that is not there fails and names the reason */
    {
        char gone[700];
        snprintf(gone, sizeof gone, "%s", url);
        gone[strlen(gone) - 3] = 'x';
        check(katoshe_download(gone, dest, NULL, &opt, &res, err, sizeof err) != 0,
              "a missing source fails");
        check(err[0] != '\0', "the failure carries a message");
    }

    free(data);
    DeleteFileA(dest);
    DeleteFileA(part);
    DeleteFileA(src);
    RemoveDirectoryA(dir);
    return failures;
}
