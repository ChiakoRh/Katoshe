#include "config.h"

#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cjson/cJSON.h"

#define KATOSHE_CONFIG_VERSION 1

void katoshe_config_defaults(katoshe_config *cfg) {
    memset(cfg, 0, sizeof *cfg);
    snprintf(cfg->global.download_dir, sizeof cfg->global.download_dir, "downloads");
    snprintf(cfg->global.proxy, sizeof cfg->global.proxy, "socks5h://127.0.0.1:10808");
    cfg->global.include_prerelease = false;
    cfg->global.check_on_launch = true;
    cfg->global.jobs = 2;
    cfg->global.timeout_sec = 60;
    cfg->global.retries = 3;
    snprintf(cfg->global.log_level, sizeof cfg->global.log_level, "info");
}

static void err_at(char *err, size_t errlen, const char *where, const char *what) {
    snprintf(err, errlen, "%s: %s", where, what);
}

static char *read_file(const char *path, size_t *len_out) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n < 0) { fclose(f); return NULL; }
    rewind(f);
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    if (len_out) *len_out = got;
    return buf;
}

/* Every getter keeps the default when the key is missing or has the wrong type. */
static void get_str(const cJSON *obj, const char *key, char *dst, size_t cap) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(v) && v->valuestring) snprintf(dst, cap, "%s", v->valuestring);
}

static void get_bool(const cJSON *obj, const char *key, bool *dst) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsBool(v)) *dst = cJSON_IsTrue(v) != 0;
}

static void get_int(const cJSON *obj, const char *key, int *dst) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(v)) *dst = (int)v->valuedouble;
}

static int parse_assets(katoshe_repo *repo, const cJSON *arr, const char *where,
                        char *err, size_t errlen) {
    int n = cJSON_GetArraySize(arr);
    if (n <= 0) return 0;

    repo->assets = (katoshe_asset *)calloc((size_t)n, sizeof *repo->assets);
    if (!repo->assets) { err_at(err, errlen, where, "out of memory"); return -1; }

    for (int i = 0; i < n; i++) {
        const cJSON *item = cJSON_GetArrayItem(arr, i);
        katoshe_asset *a = &repo->assets[repo->asset_count];
        char at[160];
        snprintf(at, sizeof at, "%s.assets[%d]", where, i);

        if (!cJSON_IsObject(item)) { err_at(err, errlen, at, "expected an object"); return -1; }

        a->mode = KATOSHE_MODE_REPLACE;
        a->keep_versions = 3;
        get_str(item, "match", a->match, sizeof a->match);
        get_str(item, "filename", a->filename, sizeof a->filename);
        get_str(item, "dir", a->dir, sizeof a->dir);
        get_int(item, "keep_versions", &a->keep_versions);

        const cJSON *mode = cJSON_GetObjectItemCaseSensitive(item, "mode");
        if (cJSON_IsString(mode) && mode->valuestring) {
            if (strcmp(mode->valuestring, "replace") == 0) a->mode = KATOSHE_MODE_REPLACE;
            else if (strcmp(mode->valuestring, "archive") == 0) a->mode = KATOSHE_MODE_ARCHIVE;
            else { err_at(err, errlen, at, "mode must be \"replace\" or \"archive\""); return -1; }
        }

        if (!a->match[0]) { err_at(err, errlen, at, "match is required"); return -1; }
        if (strpbrk(a->match, "\\/")) {
            err_at(err, errlen, at, "match must not contain a path separator");
            return -1;
        }
        if (a->mode == KATOSHE_MODE_ARCHIVE && (a->keep_versions < 1 || a->keep_versions > 99)) {
            err_at(err, errlen, at, "keep_versions must be 1..99");
            return -1;
        }
        for (int j = 0; j < repo->asset_count; j++) {
            if (strcmp(repo->assets[j].match, a->match) == 0) {
                err_at(err, errlen, at, "duplicate match string in this repository");
                return -1;
            }
        }
        repo->asset_count++;
    }
    return 0;
}

static int parse_repos(katoshe_config *cfg, const cJSON *arr, char *err, size_t errlen) {
    int n = cJSON_GetArraySize(arr);
    if (n <= 0) return 0;

    cfg->repos = (katoshe_repo *)calloc((size_t)n, sizeof *cfg->repos);
    if (!cfg->repos) { err_at(err, errlen, "repositories", "out of memory"); return -1; }

    for (int i = 0; i < n; i++) {
        const cJSON *item = cJSON_GetArrayItem(arr, i);
        katoshe_repo *r = &cfg->repos[cfg->repo_count];
        char where[64];
        snprintf(where, sizeof where, "repositories[%d]", i);

        if (!cJSON_IsObject(item)) { err_at(err, errlen, where, "expected an object"); return -1; }

        r->include_prerelease = -1;
        get_str(item, "name", r->name, sizeof r->name);
        get_str(item, "url", r->url, sizeof r->url);
        get_str(item, "dir", r->dir, sizeof r->dir);
        get_str(item, "proxy", r->proxy, sizeof r->proxy);
        get_int(item, "include_prerelease", &r->include_prerelease);

        if (!r->url[0]) {
            err_at(err, errlen, where, "url is required");
            return -1;
        }
        if (strstr(r->url, "github.com") == NULL) {
            err_at(err, errlen, where, "url must point at github.com");
            return -1;
        }
        if (!r->name[0]) {
            /* Best effort until Phase 3 owns real URL parsing: last path segment. */
            const char *slash = strrchr(r->url, '/');
            snprintf(r->name, sizeof r->name, "%s", (slash && slash[1]) ? slash + 1 : r->url);
        }
        for (int j = 0; j < cfg->repo_count; j++) {
            if (strcmp(cfg->repos[j].name, r->name) == 0) {
                err_at(err, errlen, where, "duplicate repository name");
                return -1;
            }
        }

        const cJSON *assets = cJSON_GetObjectItemCaseSensitive(item, "assets");
        if (assets && !cJSON_IsArray(assets)) {
            err_at(err, errlen, where, "assets must be an array");
            return -1;
        }
        if (assets && parse_assets(r, assets, where, err, errlen) != 0) return -1;

        cfg->repo_count++;
    }
    return 0;
}

int katoshe_config_load(katoshe_config *cfg, const char *path, char *err, size_t errlen) {
    katoshe_config_defaults(cfg);

    char *text = read_file(path, NULL);
    if (!text) {
        snprintf(err, errlen, "cannot read %s", path);
        return -1;
    }

    cJSON *doc = cJSON_Parse(text);
    free(text);
    if (!doc) {
        snprintf(err, errlen, "%s is not valid JSON", path);
        return -1;
    }
    if (!cJSON_IsObject(doc)) {
        cJSON_Delete(doc);
        snprintf(err, errlen, "the top level of %s must be an object", path);
        return -1;
    }

    const cJSON *version = cJSON_GetObjectItemCaseSensitive(doc, "version");
    if (!cJSON_IsNumber(version) || (int)version->valuedouble != KATOSHE_CONFIG_VERSION) {
        cJSON_Delete(doc);
        snprintf(err, errlen, "version must be %d", KATOSHE_CONFIG_VERSION);
        return -1;
    }

    const cJSON *glob = cJSON_GetObjectItemCaseSensitive(doc, "global");
    if (glob && !cJSON_IsObject(glob)) {
        cJSON_Delete(doc);
        err_at(err, errlen, "global", "must be an object");
        return -1;
    }
    if (glob) {
        get_str(glob, "download_dir", cfg->global.download_dir, sizeof cfg->global.download_dir);
        get_str(glob, "proxy", cfg->global.proxy, sizeof cfg->global.proxy);
        get_bool(glob, "include_prerelease", &cfg->global.include_prerelease);
        get_bool(glob, "check_on_launch", &cfg->global.check_on_launch);
        get_int(glob, "jobs", &cfg->global.jobs);
        get_int(glob, "timeout_sec", &cfg->global.timeout_sec);
        get_int(glob, "retries", &cfg->global.retries);
        get_str(glob, "log_level", cfg->global.log_level, sizeof cfg->global.log_level);
    }
    if (cfg->global.jobs < 1 || cfg->global.jobs > 16) {
        cJSON_Delete(doc);
        err_at(err, errlen, "global.jobs", "must be 1..16");
        return -1;
    }
    if (cfg->global.retries < 0 || cfg->global.retries > 10) {
        cJSON_Delete(doc);
        err_at(err, errlen, "global.retries", "must be 0..10");
        return -1;
    }
    if (cfg->global.timeout_sec < 1 || cfg->global.timeout_sec > 3600) {
        cJSON_Delete(doc);
        err_at(err, errlen, "global.timeout_sec", "must be 1..3600");
        return -1;
    }
    if (!cfg->global.download_dir[0]) {
        cJSON_Delete(doc);
        err_at(err, errlen, "global.download_dir", "must not be empty");
        return -1;
    }

    const cJSON *repos = cJSON_GetObjectItemCaseSensitive(doc, "repositories");
    if (repos && !cJSON_IsArray(repos)) {
        cJSON_Delete(doc);
        err_at(err, errlen, "repositories", "must be an array");
        return -1;
    }
    if (repos && parse_repos(cfg, repos, err, errlen) != 0) {
        cJSON_Delete(doc);
        katoshe_config_free(cfg);
        return -1;
    }

    cfg->doc = doc;
    return 0;
}

static void set_str(cJSON *obj, const char *key, const char *value) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (v && cJSON_IsString(v) && strcmp(v->valuestring ? v->valuestring : "", value) == 0) return;
    cJSON_DeleteItemFromObjectCaseSensitive(obj, key);
    cJSON_AddStringToObject(obj, key, value);
}

static void set_int(cJSON *obj, const char *key, int value) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (v && cJSON_IsNumber(v) && (int)v->valuedouble == value) return;
    cJSON_DeleteItemFromObjectCaseSensitive(obj, key);
    cJSON_AddNumberToObject(obj, key, value);
}

static void set_bool(cJSON *obj, const char *key, bool value) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (v && cJSON_IsBool(v) && (cJSON_IsTrue(v) != 0) == value) return;
    cJSON_DeleteItemFromObjectCaseSensitive(obj, key);
    cJSON_AddBoolToObject(obj, key, value);
}

/* Copies keys this tool does not own, so a hand-written extra field is not lost. */
static void copy_unknown(const cJSON *src, cJSON *dst, const char *const *known, int known_count) {
    const cJSON *child = NULL;
    if (!src || !cJSON_IsObject(src)) return;
    for (child = src->child; child; child = child->next) {
        bool skip = false;
        for (int i = 0; i < known_count; i++) {
            if (child->string && strcmp(child->string, known[i]) == 0) { skip = true; break; }
        }
        if (!skip && child->string) {
            cJSON *copy = cJSON_Duplicate(child, 1);
            if (copy) cJSON_AddItemToObject(dst, child->string, copy);
        }
    }
}

static cJSON *find_repo_source(const cJSON *doc, const char *name) {
    const cJSON *repos = cJSON_GetObjectItemCaseSensitive(doc, "repositories");
    const cJSON *item = NULL;
    if (!cJSON_IsArray(repos)) return NULL;
    for (item = repos->child; item; item = item->next) {
        const cJSON *n = cJSON_GetObjectItemCaseSensitive(item, "name");
        if (cJSON_IsString(n) && n->valuestring && strcmp(n->valuestring, name) == 0) {
            return (cJSON *)item;
        }
    }
    return NULL;
}

static cJSON *find_asset_source(const cJSON *repo, const char *match) {
    const cJSON *assets = cJSON_GetObjectItemCaseSensitive(repo, "assets");
    const cJSON *item = NULL;
    if (!cJSON_IsArray(assets)) return NULL;
    for (item = assets->child; item; item = item->next) {
        const cJSON *m = cJSON_GetObjectItemCaseSensitive(item, "match");
        if (cJSON_IsString(m) && m->valuestring && strcmp(m->valuestring, match) == 0) {
            return (cJSON *)item;
        }
    }
    return NULL;
}

static int build_document(katoshe_config *cfg, cJSON **out, char *err, size_t errlen) {
    static const char *const global_keys[] = {"download_dir", "proxy", "include_prerelease",
                                              "check_on_launch", "jobs", "timeout_sec",
                                              "retries", "log_level"};
    static const char *const repo_keys[] = {"name", "url", "dir", "include_prerelease",
                                            "proxy", "assets"};
    static const char *const asset_keys[] = {"match", "filename", "dir", "mode",
                                             "keep_versions"};
    static const char *const top_keys[] = {"version", "global", "repositories"};

    const cJSON *old_global = cfg->doc ? cJSON_GetObjectItemCaseSensitive(cfg->doc, "global") : NULL;

    cJSON *doc = cJSON_CreateObject();
    if (!doc) { err_at(err, errlen, "config", "out of memory"); return -1; }

    copy_unknown(cfg->doc, doc, top_keys, (int)(sizeof top_keys / sizeof *top_keys));
    cJSON_AddNumberToObject(doc, "version", KATOSHE_CONFIG_VERSION);

    cJSON *g = cJSON_CreateObject();
    cJSON_AddItemToObject(doc, "global", g);
    copy_unknown(old_global, g, global_keys, (int)(sizeof global_keys / sizeof *global_keys));
    set_str(g, "download_dir", cfg->global.download_dir);
    set_str(g, "proxy", cfg->global.proxy);
    set_bool(g, "include_prerelease", cfg->global.include_prerelease);
    set_bool(g, "check_on_launch", cfg->global.check_on_launch);
    set_int(g, "jobs", cfg->global.jobs);
    set_int(g, "timeout_sec", cfg->global.timeout_sec);
    set_int(g, "retries", cfg->global.retries);
    set_str(g, "log_level", cfg->global.log_level);

    cJSON *repos = cJSON_CreateArray();
    cJSON_AddItemToObject(doc, "repositories", repos);

    for (int i = 0; i < cfg->repo_count; i++) {
        const katoshe_repo *r = &cfg->repos[i];
        cJSON *old_repo = find_repo_source(cfg->doc, r->name);
        cJSON *ro = cJSON_CreateObject();

        copy_unknown(old_repo, ro, repo_keys, (int)(sizeof repo_keys / sizeof *repo_keys));
        set_str(ro, "name", r->name);
        set_str(ro, "url", r->url);
        if (r->dir[0]) set_str(ro, "dir", r->dir);
        if (r->include_prerelease >= 0) set_int(ro, "include_prerelease", r->include_prerelease);
        if (r->proxy[0]) set_str(ro, "proxy", r->proxy);

        cJSON *assets = cJSON_CreateArray();
        cJSON_AddItemToObject(ro, "assets", assets);
        for (int j = 0; j < r->asset_count; j++) {
            const katoshe_asset *a = &r->assets[j];
            cJSON *old_asset = find_asset_source(old_repo, a->match);
            cJSON *ao = cJSON_CreateObject();

            copy_unknown(old_asset, ao, asset_keys, (int)(sizeof asset_keys / sizeof *asset_keys));
            set_str(ao, "match", a->match);
            if (a->filename[0]) set_str(ao, "filename", a->filename);
            if (a->dir[0]) set_str(ao, "dir", a->dir);
            if (a->mode == KATOSHE_MODE_ARCHIVE) {
                set_str(ao, "mode", "archive");
                set_int(ao, "keep_versions", a->keep_versions);
            }
            cJSON_AddItemToArray(assets, ao);
        }
        cJSON_AddItemToArray(repos, ro);
    }

    *out = doc;
    return 0;
}

static int write_atomic(const char *path, const char *text, char *err, size_t errlen) {
    char tmp[KATOSHE_PATH_MAX + 8];
    if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp) {
        err_at(err, errlen, path, "path too long");
        return -1;
    }
    HANDLE h = CreateFileA(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        err_at(err, errlen, tmp, "cannot create the temporary file");
        return -1;
    }
    DWORD len = (DWORD)strlen(text);
    DWORD written = 0;
    BOOL ok = WriteFile(h, text, len, &written, NULL);
    CloseHandle(h);
    if (!ok || written != len) {
        DeleteFileA(tmp);
        err_at(err, errlen, tmp, "cannot write the temporary file");
        return -1;
    }
    if (!MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileA(tmp);
        err_at(err, errlen, path, "cannot replace the file");
        return -1;
    }
    return 0;
}

int katoshe_config_save(katoshe_config *cfg, const char *path, char *err, size_t errlen) {
    cJSON *doc = NULL;
    if (build_document(cfg, &doc, err, errlen) != 0) return -1;

    char *text = cJSON_PrintBuffered(doc, 4096, 1);
    if (!text) {
        cJSON_Delete(doc);
        err_at(err, errlen, path, "out of memory while printing");
        return -1;
    }

    int rc = write_atomic(path, text, err, errlen);
    cJSON_free(text);
    if (rc == 0) {
        if (cfg->doc) cJSON_Delete(cfg->doc);
        cfg->doc = doc; /* keep the new document for the next save */
    } else {
        cJSON_Delete(doc);
    }
    return rc;
}

void katoshe_config_free(katoshe_config *cfg) {
    if (!cfg) return;
    for (int i = 0; i < cfg->repo_count; i++) free(cfg->repos[i].assets);
    free(cfg->repos);
    if (cfg->doc) cJSON_Delete(cfg->doc);
    memset(cfg, 0, sizeof *cfg);
}

void katoshe_config_summary(const katoshe_config *cfg, char *out, size_t cap) {
    int assets = 0;
    for (int i = 0; i < cfg->repo_count; i++) assets += cfg->repos[i].asset_count;
    snprintf(out, cap, "%d repositories, %d assets, jobs %d, proxy %s",
             cfg->repo_count, assets, cfg->global.jobs,
             cfg->global.proxy[0] ? cfg->global.proxy : "(direct)");
}
