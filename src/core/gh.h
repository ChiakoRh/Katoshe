#ifndef KATOSHE_GH_H
#define KATOSHE_GH_H

#include <stdbool.h>
#include <stddef.h>

#define GH_OK 0
#define GH_ERROR (-1)
#define GH_NOT_MODIFIED 1

typedef struct {
    long long id;
    char tag[128];
    char name[192];
    bool prerelease;
    char published_at[32];
} gh_release;

typedef struct {
    long long id;
    char name[256];
    long long size;
    char digest[80]; /* "sha256:..." as GitHub reports it, empty when absent */
    char url[1024];
} gh_asset;

typedef struct {
    char etag[128];   /* empty when the response did not carry one */
    long remaining;   /* rate limit remaining, -1 when unknown */
    long long reset;  /* unix seconds, 0 when unknown */
} gh_meta;

typedef struct {
    const char *proxy;       /* NULL or empty means direct */
    const char *token;       /* NULL or empty means anonymous */
    const char *etag;        /* NULL means unconditional */
    int timeout_sec;
} gh_options;

/* owner/repo from any of: https://github.com/o/r, .../o/r.git, git@github.com:o/r,
   github.com/o/r, o/r. Returns 0 on success. */
int gh_parse_url(const char *url, char *owner, size_t owner_cap, char *repo, size_t repo_cap);

/* Fetches the release the tool would install: /releases/latest, or the first
   non-draft entry of /releases when prereleases are included. `assets` is
   allocated and must go through gh_release_free. */
int gh_fetch_release(const char *owner, const char *repo, bool include_prerelease,
                     const gh_options *opt, gh_meta *meta, gh_release *release,
                     gh_asset **assets, int *asset_count, char *err, size_t errlen);

void gh_release_free(gh_asset *assets);

/* Human-readable size, e.g. "12.4 MB". */
void gh_format_size(long long bytes, char *out, size_t cap);

#endif
