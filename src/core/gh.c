#include "gh.h"

#include <curl/curl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cjson/cJSON.h"
#include "katoshe.h"

#define GH_MAX_BODY (8u * 1024u * 1024u)
#define GH_API "https://api.github.com"

typedef struct {
    char *data;
    size_t len;
    size_t cap;
    int overflow;
} membuf;

static void copy_str(char *dst, size_t cap, const char *src, size_t max) {
    size_t n = 0;
    if (!src) { if (cap) dst[0] = '\0'; return; }
    while (n + 1 < cap && n < max && src[n]) n++;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static bool name_char_ok(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '.' || c == '_' || c == '-';
}

/* "." and ".." are valid path characters but never a repository name. */
static bool all_dots(const char *s, size_t len) {
    if (len == 0) return true;
    for (size_t i = 0; i < len; i++) if (s[i] != '.') return false;
    return true;
}

static bool host_is_github(const char *host, size_t len) {
    return (len == 10 && strncmp(host, "github.com", 10) == 0) ||
           (len == 14 && strncmp(host, "www.github.com", 14) == 0) ||
           (len == 14 && strncmp(host, "api.github.com", 14) == 0);
}

static int split_owner_repo(const char *path, char *owner, size_t owner_cap,
                            char *repo, size_t repo_cap) {
    char buf[512];
    const char *slash;
    size_t owner_len;

    copy_str(buf, sizeof buf, path, sizeof buf - 1);
    /* drop query/fragment, then a trailing slash or .git */
    for (char *q = buf; *q; q++) {
        if (*q == '?' || *q == '#') { *q = '\0'; break; }
    }
    for (size_t n = strlen(buf); n > 0; n--) {
        if (buf[n - 1] == '/') { buf[n - 1] = '\0'; continue; }
        if (n >= 4 && strcmp(buf + n - 4, ".git") == 0) { buf[n - 4] = '\0'; }
        break;
    }

    slash = strchr(buf, '/');
    if (!slash) return -1;
    owner_len = (size_t)(slash - buf);
    if (owner_len == 0 || owner_len >= owner_cap) return -1;
    if (strchr(slash + 1, '/')) return -1; /* still a path: caller stripped too little */
    if (!slash[1] || strlen(slash + 1) >= repo_cap) return -1;

    for (size_t i = 0; i < owner_len; i++) if (!name_char_ok(buf[i])) return -1;
    for (const char *r = slash + 1; *r; r++) if (!name_char_ok(*r)) return -1;
    if (all_dots(buf, owner_len)) return -1;
    if (all_dots(slash + 1, strlen(slash + 1))) return -1;

    memcpy(owner, buf, owner_len);
    owner[owner_len] = '\0';
    copy_str(repo, repo_cap, slash + 1, repo_cap - 1);
    return 0;
}

int gh_parse_url(const char *url, char *owner, size_t owner_cap, char *repo, size_t repo_cap) {
    const char *path = NULL;
    char work[512];

    if (!url || !url[0]) return -1;
    copy_str(work, sizeof work, url, sizeof work - 1);

    /* git@github.com:owner/repo */
    if (strncmp(work, "git@", 4) == 0) {
        const char *colon = strchr(work, ':');
        if (!colon) return -1;
        path = colon + 1;
    } else if (strstr(work, "://") != NULL) {
        const char *host = strstr(work, "://") + 3;
        size_t host_len = strcspn(host, "/");
        if (!host_is_github(host, host_len)) return -1;
        path = strchr(host, '/');
        if (!path) return -1;
        path++;
    } else {
        path = work;
        if (strncmp(path, "github.com/", 11) == 0) path += 11;
        else if (strncmp(path, "www.github.com/", 15) == 0) path += 15;
    }

    /* api.github.com/repos/owner/repo is not a URL the user should have to type,
       but strip it anyway rather than failing with a confusing message. */
    if (strncmp(path, "repos/", 6) == 0) path += 6;

    return split_owner_repo(path, owner, owner_cap, repo, repo_cap);
}

static size_t on_write(char *ptr, size_t size, size_t nmemb, void *userdata) {
    membuf *buf = (membuf *)userdata;
    size_t total = size * nmemb;

    if (buf->len + total + 1 > GH_MAX_BODY) {
        buf->overflow = 1;
        return 0; /* aborts the transfer */
    }
    if (buf->len + total + 1 > buf->cap) {
        size_t cap = buf->cap ? buf->cap : 4096;
        while (cap < buf->len + total + 1) cap *= 2;
        char *grown = (char *)realloc(buf->data, cap);
        if (!grown) return 0;
        buf->data = grown;
        buf->cap = cap;
    }
    memcpy(buf->data + buf->len, ptr, total);
    buf->len += total;
    buf->data[buf->len] = '\0';
    return total;
}

static bool header_value(const char *line, size_t len, const char *name, char *out, size_t cap) {
    size_t n = strlen(name);
    if (len <= n + 1) return false;
    for (size_t i = 0; i < n; i++) {
        char a = line[i], b = name[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return false;
    }
    if (line[n] != ':') return false;
    size_t start = n + 1;
    while (start < len && (line[start] == ' ' || line[start] == '\t')) start++;
    size_t end = len;
    while (end > start && (line[end - 1] == '\r' || line[end - 1] == '\n' ||
                           line[end - 1] == ' ' || line[end - 1] == '\t')) end--;
    size_t count = end - start;
    if (count >= cap) count = cap - 1;
    memcpy(out, line + start, count);
    out[count] = '\0';
    return true;
}

static size_t on_header(char *buffer, size_t size, size_t nitems, void *userdata) {
    gh_meta *meta = (gh_meta *)userdata;
    size_t len = size * nitems;
    char value[160];

    if (header_value(buffer, len, "x-ratelimit-remaining", value, sizeof value)) {
        meta->remaining = strtol(value, NULL, 10);
    } else if (header_value(buffer, len, "x-ratelimit-reset", value, sizeof value)) {
        meta->reset = strtoll(value, NULL, 10);
    } else if (header_value(buffer, len, "etag", value, sizeof value)) {
        copy_str(meta->etag, sizeof meta->etag, value, sizeof meta->etag - 1);
    }
    return len;
}

static void format_reset(long long reset, char *out, size_t cap) {
    time_t t = (time_t)reset;
    struct tm tm;
    if (reset <= 0 || localtime_s(&tm, &t) != 0) {
        copy_str(out, cap, "an unknown time", cap - 1);
        return;
    }
    strftime(out, cap, "%Y-%m-%d %H:%M", &tm);
}

static int http_get(const char *url, const gh_options *opt, membuf *body, long *status,
                    gh_meta *meta, char *err, size_t errlen) {
    CURL *curl = curl_easy_init();
    CURLcode rc;
    char agent[64];
    char *tok_header = NULL;
    struct curl_slist *headers = NULL;
    char curl_err[CURL_ERROR_SIZE];

    if (!curl) {
        snprintf(err, errlen, "cannot initialise libcurl");
        return GH_ERROR;
    }

    memset(body, 0, sizeof *body);
    memset(meta, 0, sizeof *meta);
    meta->remaining = -1;
    *status = 0;
    curl_err[0] = '\0';

    snprintf(agent, sizeof agent, "katoshe/%s", katoshe_version());
    headers = curl_slist_append(headers, "Accept: application/vnd.github+json");
    headers = curl_slist_append(headers, "X-GitHub-Api-Version: 2022-11-28");
    headers = curl_slist_append(headers, "Accept-Encoding: identity"); /* no zlib in this libcurl */
    if (opt->token && opt->token[0]) {
        size_t n = strlen(opt->token) + 32;
        tok_header = (char *)malloc(n);
        if (tok_header) {
            snprintf(tok_header, n, "Authorization: Bearer %s", opt->token);
            headers = curl_slist_append(headers, tok_header);
        }
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, agent);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, body);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, on_header);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, meta);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_err);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)(opt->timeout_sec > 0 ? opt->timeout_sec : 60));
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
    if (opt->etag && opt->etag[0]) {
        size_t n = strlen(opt->etag) + 32;
        char *h = (char *)malloc(n);
        if (h) {
            snprintf(h, n, "If-None-Match: %s", opt->etag);
            headers = curl_slist_append(headers, h);
        }
    }
    if (opt->proxy && opt->proxy[0]) {
        curl_easy_setopt(curl, CURLOPT_PROXY, opt->proxy);
    }

    rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        if (body->overflow) {
            snprintf(err, errlen, "response too large from %s", url);
        } else {
            snprintf(err, errlen, "%s: %s", url,
                     curl_err[0] ? curl_err : curl_easy_strerror(rc));
            if (opt->proxy && opt->proxy[0]) {
                size_t used = strlen(err);
                snprintf(err + used, errlen - used, " (via proxy %s)", opt->proxy);
            }
        }
        free(body->data);
        body->data = NULL;
        curl_slist_free_all(headers);
        free(tok_header);
        curl_easy_cleanup(curl);
        return GH_ERROR;
    }

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, status);
    curl_slist_free_all(headers);
    free(tok_header);
    curl_easy_cleanup(curl);
    return GH_OK;
}

static const char *json_string(const cJSON *obj, const char *key) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return (cJSON_IsString(v) && v->valuestring) ? v->valuestring : "";
}

/* Turns a non-200 into something that says what to do next. */
static int explain_status(long status, const membuf *body, const gh_meta *meta,
                          const char *what, char *err, size_t errlen) {
    char reset[64];
    char api_message[256];

    api_message[0] = '\0';
    if (body->data) {
        cJSON *doc = cJSON_Parse(body->data);
        if (doc) {
            copy_str(api_message, sizeof api_message, json_string(doc, "message"),
                     sizeof api_message - 1);
            cJSON_Delete(doc);
        }
    }

    if (status == 404) {
        snprintf(err, errlen, "%s (404): no such repository, a private repository that needs "
                              "a token, or a repository with no published release", what);
    } else if (status == 401) {
        snprintf(err, errlen, "GitHub rejected the token (401): %s",
                 api_message[0] ? api_message : "bad credentials");
    } else if ((status == 403 || status == 429) && meta->remaining == 0) {
        format_reset(meta->reset, reset, sizeof reset);
        snprintf(err, errlen, "GitHub API rate limit exhausted, resets at %s%s%s", reset,
                 api_message[0] ? " - " : "", api_message);
    } else if (status == 403) {
        snprintf(err, errlen, "GitHub refused the request (403): %s",
                 api_message[0] ? api_message : what);
    } else if (status >= 400) {
        snprintf(err, errlen, "GitHub returned HTTP %ld for %s%s%s", status, what,
                 api_message[0] ? ": " : "", api_message);
    }
    return GH_ERROR;
}

static int parse_release(const cJSON *rel, gh_release *out, gh_asset **assets,
                         int *asset_count, char *err, size_t errlen) {
    const cJSON *assets_json = cJSON_GetObjectItemCaseSensitive(rel, "assets");
    const cJSON *draft = cJSON_GetObjectItemCaseSensitive(rel, "draft");
    const cJSON *pre = cJSON_GetObjectItemCaseSensitive(rel, "prerelease");
    int n;

    if (cJSON_IsTrue(draft)) return GH_NOT_MODIFIED; /* skip, caller keeps looking */
    {
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(rel, "id");
        out->id = cJSON_IsNumber(id) ? (long long)id->valuedouble : 0;
    }
    copy_str(out->tag, sizeof out->tag, json_string(rel, "tag_name"), sizeof out->tag - 1);
    copy_str(out->name, sizeof out->name, json_string(rel, "name"), sizeof out->name - 1);
    copy_str(out->published_at, sizeof out->published_at, json_string(rel, "published_at"),
             sizeof out->published_at - 1);
    out->prerelease = cJSON_IsTrue(pre) != 0;

    n = cJSON_IsArray(assets_json) ? cJSON_GetArraySize(assets_json) : 0;
    if (n <= 0) return GH_OK;

    *assets = (gh_asset *)calloc((size_t)n, sizeof **assets);
    if (!*assets) {
        snprintf(err, errlen, "out of memory");
        return GH_ERROR;
    }
    for (int i = 0; i < n; i++) {
        const cJSON *item = cJSON_GetArrayItem(assets_json, i);
        gh_asset *a = &(*assets)[i];
        const cJSON *size = cJSON_GetObjectItemCaseSensitive(item, "size");
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(item, "id");
        a->id = cJSON_IsNumber(id) ? (long long)id->valuedouble : 0;
        copy_str(a->name, sizeof a->name, json_string(item, "name"), sizeof a->name - 1);
        copy_str(a->url, sizeof a->url, json_string(item, "browser_download_url"),
                 sizeof a->url - 1);
        copy_str(a->digest, sizeof a->digest, json_string(item, "digest"), sizeof a->digest - 1);
        if (cJSON_IsNumber(size)) a->size = (long long)size->valuedouble;
    }
    *asset_count = n;
    return GH_OK;
}

int gh_fetch_release(const char *owner, const char *repo, bool include_prerelease,
                     const gh_options *opt, gh_meta *meta, gh_release *release,
                     gh_asset **assets, int *asset_count, char *err, size_t errlen) {
    static int curl_ready;
    char url[512];
    char what[300];
    membuf body;
    long status = 0;
    int rc;
    cJSON *doc;
    const cJSON *rel;

    memset(release, 0, sizeof *release);
    memset(meta, 0, sizeof *meta);
    meta->remaining = -1;
    *assets = NULL;
    *asset_count = 0;
    err[0] = '\0';

    if (!curl_ready) {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0) {
            snprintf(err, errlen, "cannot initialise libcurl");
            return GH_ERROR;
        }
        curl_ready = 1;
    }

    snprintf(what, sizeof what, "%s/%s", owner, repo);
    if (include_prerelease) {
        snprintf(url, sizeof url, GH_API "/repos/%s/%s/releases?per_page=30", owner, repo);
    } else {
        snprintf(url, sizeof url, GH_API "/repos/%s/%s/releases/latest", owner, repo);
    }

    rc = http_get(url, opt, &body, &status, meta, err, errlen);
    if (rc != GH_OK) return rc;

    if (status == 304) {
        free(body.data);
        return GH_NOT_MODIFIED;
    }
    if (status != 200) {
        rc = explain_status(status, &body, meta, what, err, errlen);
        free(body.data);
        return rc;
    }

    doc = cJSON_Parse(body.data ? body.data : "");
    free(body.data);
    if (!doc) {
        snprintf(err, errlen, "GitHub returned a response that is not JSON for %s", what);
        return GH_ERROR;
    }

    if (include_prerelease) {
        if (!cJSON_IsArray(doc)) {
            cJSON_Delete(doc);
            snprintf(err, errlen, "unexpected response shape for the release list of %s", what);
            return GH_ERROR;
        }
        rel = NULL;
        const cJSON *item = NULL;
        cJSON_ArrayForEach(item, doc) {
            int prc = parse_release(item, release, assets, asset_count, err, errlen);
            if (prc == GH_OK) { rel = item; break; }
            if (prc == GH_ERROR) { cJSON_Delete(doc); return prc; }
            /* GH_NOT_MODIFIED here means "draft", so keep looking */
        }
        if (!rel) {
            cJSON_Delete(doc);
            snprintf(err, errlen, "%s has no published release", what);
            return GH_ERROR;
        }
    } else {
        if (!cJSON_IsObject(doc)) {
            cJSON_Delete(doc);
            snprintf(err, errlen, "unexpected response shape for the latest release of %s", what);
            return GH_ERROR;
        }
        if (parse_release(doc, release, assets, asset_count, err, errlen) != GH_OK) {
            cJSON_Delete(doc);
            return GH_ERROR;
        }
    }

    cJSON_Delete(doc);
    if (!release->tag[0]) {
        snprintf(err, errlen, "%s has no published release", what);
        return GH_ERROR;
    }
    return GH_OK;
}

void gh_release_free(gh_asset *assets) {
    free(assets);
}

void gh_format_size(long long bytes, char *out, size_t cap) {
    static const char *const units[] = {"B", "KB", "MB", "GB", "TB"};
    double value = (double)bytes;
    int unit = 0;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        unit++;
    }
    if (unit == 0) snprintf(out, cap, "%lld %s", bytes, units[unit]);
    else snprintf(out, cap, "%.1f %s", value, units[unit]);
}
