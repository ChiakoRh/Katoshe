#include "env.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gh.h"
#include "paths.h"
#include "secrets.h"

const char *katoshe_env(const char *name) {
    const char *v = getenv(name);
    return (v && v[0]) ? v : NULL;
}

void katoshe_copy_str(char *dst, size_t cap, const char *src) {
    size_t n = 0;

    if (!cap) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    while (n + 1 < cap && src[n]) n++;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

void katoshe_resolve_token(const char *explicit_token, const char *config_dir,
                           char *out, size_t cap) {
    char err[256];
    const char *env = katoshe_env("KATOSHE_GITHUB_TOKEN");

    if (!cap) return;
    out[0] = '\0';
    if (explicit_token && explicit_token[0]) {
        katoshe_copy_str(out, cap, explicit_token);
        return;
    }
    if (env) {
        katoshe_copy_str(out, cap, env);
        return;
    }
    if (katoshe_token_from_file(config_dir, out, cap, err, sizeof err) != 0) out[0] = '\0';
}

const char *katoshe_resolve_proxy(const katoshe_config *cfg, const char *owner,
                                  const char *repo) {
    const char *env = katoshe_env("KATOSHE_PROXY");

    if (cfg) {
        for (int i = 0; i < cfg->repo_count; i++) {
            const katoshe_repo *r = &cfg->repos[i];
            char ro[128], rr[128];
            if (r->proxy[0] && gh_parse_url(r->url, ro, sizeof ro, rr, sizeof rr) == 0 &&
                strcmp(ro, owner) == 0 && strcmp(rr, repo) == 0) {
                return r->proxy;
            }
        }
    }
    if (env) return env;
    if (cfg && cfg->global.proxy[0]) return cfg->global.proxy;
    return NULL;
}

static int is_absolute_path(const char *path) {
    if (!path[0]) return 0;
    if (path[0] == '\\' || path[0] == '/') return 1;
    return path[1] == ':';
}

int katoshe_download_dir(const char *base, const char *configured, char *out, size_t cap) {
    if (is_absolute_path(configured)) {
        return snprintf(out, cap, "%s", configured) < (int)cap ? 0 : -1;
    }
    return katoshe_path_join(out, cap, base, configured);
}
