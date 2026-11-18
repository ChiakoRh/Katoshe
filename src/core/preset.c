#include "preset.h"

#include <windows.h>
#include <wincrypt.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cjson/cJSON.h"
#include "io.h"
#include "paths.h"

#define PRESET_VERSION 1
#define PRESET_NAME_LEN (KATOSHE_PRESET_MAX - 1)
#define LINK_PREFIX "katoshe://"

static void sayf(char *err, size_t errlen, const char *fmt, ...) {
    va_list ap;
    if (!err || errlen == 0) return;
    va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
}

static void fail(char *err, size_t errlen, const char *where, const char *what) {
    sayf(err, errlen, "%s: %s", where, what);
}

/* A preset is untrusted input, so relative paths only: no drive, no root slash,
   no .. anywhere. Same rule config.c applies when loading. */
static bool dir_is_safe(const char *dir) {
    if (!dir[0]) return true;
    if (dir[0] == '/' || dir[0] == '\\') return false;
    if (dir[1] == ':') return false;
    return strstr(dir, "..") == NULL;
}

static void get_str(const cJSON *obj, const char *key, char *dst, size_t cap) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(v) && v->valuestring) snprintf(dst, cap, "%s", v->valuestring);
}

static void get_int(const cJSON *obj, const char *key, int *dst) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(v)) *dst = (int)v->valuedouble;
}

/* Last path segment without a .json extension, truncated to the preset limit. */
static void path_stem(const char *path, char *out, size_t cap) {
    const char *base = path;
    size_t n;

    for (const char *p = path; *p; p++) {
        if (*p == '\\' || *p == '/') base = p + 1;
    }
    n = strlen(base);
    if (n > 5 && _stricmp(base + n - 5, ".json") == 0) n -= 5;
    if (n >= cap) n = cap - 1;
    memcpy(out, base, n);
    out[n] = '\0';
}

/* Anything that is not [A-Za-z0-9._-] becomes a dash, because the name is a file
   name on the other side too. Trailing dots and dashes are dropped so "name."
   cannot become a hidden or invalid stem. */
static void sanitize_name(const char *in, char *out, size_t cap) {
    size_t n = 0;

    while (in && *in && n + 1 < cap) {
        char c = *in++;
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        out[n++] = ok ? c : '-';
    }
    out[n] = '\0';
    while (n > 0 && (out[n - 1] == '.' || out[n - 1] == '-')) out[--n] = '\0';
    if (!katoshe_preset_name_ok(out)) snprintf(out, cap, "preset");
}

static int preset_path(char *out, size_t cap, const char *dir, const char *name) {
    char file[KATOSHE_PRESET_MAX + 8];
    if (snprintf(file, sizeof file, "%s.json", name) >= (int)sizeof file) return -1;
    return katoshe_path_join(out, cap, dir, file);
}

/* Trims surrounding whitespace so a link pasted with a newline still works. */
static char *trim_dup(const char *s) {
    const char *a = s;
    const char *b;
    size_t n;
    char *out;

    while (*a == ' ' || *a == '\t' || *a == '\r' || *a == '\n') a++;
    b = a + strlen(a);
    while (b > a && (b[-1] == ' ' || b[-1] == '\t' || b[-1] == '\r' || b[-1] == '\n')) b--;
    n = (size_t)(b - a);
    out = (char *)malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, a, n);
    out[n] = '\0';
    return out;
}

/* The wire format. Optional keys appear only when they carry a value. */
static cJSON *build_doc(const katoshe_config *cfg) {
    cJSON *doc = cJSON_CreateObject();
    cJSON *repos;

    if (!doc) return NULL;
    cJSON_AddNumberToObject(doc, "version", PRESET_VERSION);
    /* The name travels with the packet so a shared link keeps its identity. */
    if (cfg->preset[0]) cJSON_AddStringToObject(doc, "name", cfg->preset);
    repos = cJSON_CreateArray();
    if (!repos) goto oom;
    cJSON_AddItemToObject(doc, "repositories", repos);

    for (int i = 0; i < cfg->repo_count; i++) {
        const katoshe_repo *r = &cfg->repos[i];
        cJSON *ro = cJSON_CreateObject();
        cJSON *assets;

        if (!ro) goto oom;
        cJSON_AddItemToArray(repos, ro);
        cJSON_AddStringToObject(ro, "name", r->name);
        cJSON_AddStringToObject(ro, "url", r->url);
        if (r->dir[0]) cJSON_AddStringToObject(ro, "dir", r->dir);
        if (r->include_prerelease >= 0) {
            cJSON_AddBoolToObject(ro, "include_prerelease", r->include_prerelease != 0);
        }
        if (r->proxy[0]) cJSON_AddStringToObject(ro, "proxy", r->proxy);

        assets = cJSON_CreateArray();
        if (!assets) goto oom;
        cJSON_AddItemToObject(ro, "assets", assets);
        for (int j = 0; j < r->asset_count; j++) {
            const katoshe_asset *a = &r->assets[j];
            cJSON *ao = cJSON_CreateObject();

            if (!ao) goto oom;
            cJSON_AddItemToArray(assets, ao);
            cJSON_AddStringToObject(ao, "match", a->match);
            if (a->filename[0]) cJSON_AddStringToObject(ao, "filename", a->filename);
            if (a->dir[0]) cJSON_AddStringToObject(ao, "dir", a->dir);
            if (a->mode == KATOSHE_MODE_ARCHIVE) {
                int keep = a->keep_versions;
                if (keep < 1 || keep > 99) keep = 3;
                cJSON_AddStringToObject(ao, "mode", "archive");
                cJSON_AddNumberToObject(ao, "keep_versions", keep);
            }
        }
    }
    return doc;

oom:
    cJSON_Delete(doc);
    return NULL;
}

int katoshe_preset_write(const katoshe_config *cfg, const char *path,
                         char *err, size_t errlen) {
    cJSON *doc;
    char *text;
    int rc;

    if (err && errlen) err[0] = '\0';
    if (!cfg) { sayf(err, errlen, "no configuration"); return -1; }
    if (!path || !path[0]) { sayf(err, errlen, "no output path"); return -1; }

    doc = build_doc(cfg);
    if (!doc) { fail(err, errlen, path, "out of memory"); return -1; }
    text = cJSON_PrintBuffered(doc, 4096, 1);
    cJSON_Delete(doc);
    if (!text) { fail(err, errlen, path, "out of memory while printing"); return -1; }

    rc = katoshe_write_atomic(path, text, err, errlen);
    cJSON_free(text);
    return rc;
}

int katoshe_preset_to_link(const katoshe_config *cfg, char *out, size_t cap,
                           char *err, size_t errlen) {
    cJSON *doc;
    char *json;
    char *b64 = NULL;
    DWORD b64len = 0;
    size_t need;
    int rc = -1;

    if (err && errlen) err[0] = '\0';
    if (!cfg) { sayf(err, errlen, "no configuration"); return -1; }
    if (!out || cap == 0) { sayf(err, errlen, "no output buffer"); return -1; }
    out[0] = '\0';

    doc = build_doc(cfg);
    if (!doc) { sayf(err, errlen, "out of memory"); return -1; }
    json = cJSON_PrintBuffered(doc, 4096, 0);
    cJSON_Delete(doc);
    if (!json) { sayf(err, errlen, "out of memory while printing"); return -1; }

    if (!CryptBinaryToStringA((const BYTE *)json, (DWORD)strlen(json),
                              CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, NULL, &b64len)) {
        sayf(err, errlen, "cannot size the base64 encoding");
        goto done;
    }
    b64 = (char *)malloc(b64len);
    if (!b64) { sayf(err, errlen, "out of memory"); goto done; }
    if (!CryptBinaryToStringA((const BYTE *)json, (DWORD)strlen(json),
                              CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, b64, &b64len)) {
        sayf(err, errlen, "cannot base64 encode the preset");
        goto done;
    }

    need = strlen(LINK_PREFIX) + strlen(b64) + 1;
    if (need > cap) {
        sayf(err, errlen, "the link needs %llu bytes, the buffer holds %llu",
             (unsigned long long)need, (unsigned long long)cap);
        goto done;
    }
    snprintf(out, cap, "%s%s", LINK_PREFIX, b64);
    rc = 0;

done:
    free(b64);
    cJSON_free(json);
    return rc;
}

/* The payload has to be canonical base64 and nothing else: a truncated paste or
   a chat client's mangling then fails on the decode, which is the honest error. */
static char *decode_link(const char *b64, char *err, size_t errlen) {
    size_t len = strlen(b64);
    DWORD bytes = 0;
    char *out;

    if (len == 0) { sayf(err, errlen, "the link has no payload"); return NULL; }
    if (len > 4u * 1024u * 1024u) { sayf(err, errlen, "the link is too long"); return NULL; }
    if (!CryptStringToBinaryA(b64, (DWORD)len, CRYPT_STRING_BASE64 | CRYPT_STRING_STRICT,
                              NULL, &bytes, NULL, NULL) ||
        bytes == 0) {
        sayf(err, errlen, "the link payload is not valid base64");
        return NULL;
    }
    out = (char *)malloc((size_t)bytes + 1);
    if (!out) { sayf(err, errlen, "out of memory"); return NULL; }
    if (!CryptStringToBinaryA(b64, (DWORD)len, CRYPT_STRING_BASE64 | CRYPT_STRING_STRICT,
                              (BYTE *)out, &bytes, NULL, NULL)) {
        free(out);
        sayf(err, errlen, "the link payload is not valid base64");
        return NULL;
    }
    out[bytes] = '\0';
    if (strlen(out) != (size_t)bytes) {
        free(out);
        sayf(err, errlen, "the link payload contains a NUL byte");
        return NULL;
    }
    return out;
}

static int parse_assets(katoshe_repo *repo, const cJSON *arr, const char *where,
                        char *err, size_t errlen) {
    int n = cJSON_GetArraySize(arr);
    if (n <= 0) return 0;

    repo->assets = (katoshe_asset *)calloc((size_t)n, sizeof *repo->assets);
    if (!repo->assets) { fail(err, errlen, where, "out of memory"); return -1; }

    for (int i = 0; i < n; i++) {
        const cJSON *item = cJSON_GetArrayItem(arr, i);
        katoshe_asset *a = &repo->assets[repo->asset_count];
        char at[192];

        snprintf(at, sizeof at, "%s.assets[%d]", where, i);
        if (!cJSON_IsObject(item)) { fail(err, errlen, at, "expected an object"); return -1; }

        a->mode = KATOSHE_MODE_REPLACE;
        a->keep_versions = 3;
        get_str(item, "match", a->match, sizeof a->match);
        get_str(item, "filename", a->filename, sizeof a->filename);
        get_str(item, "dir", a->dir, sizeof a->dir);
        get_int(item, "keep_versions", &a->keep_versions);

        {
            const cJSON *mode = cJSON_GetObjectItemCaseSensitive(item, "mode");
            if (cJSON_IsString(mode) && mode->valuestring) {
                if (strcmp(mode->valuestring, "replace") == 0) a->mode = KATOSHE_MODE_REPLACE;
                else if (strcmp(mode->valuestring, "archive") == 0) a->mode = KATOSHE_MODE_ARCHIVE;
                else { fail(err, errlen, at, "mode must be \"replace\" or \"archive\""); return -1; }
            }
        }

        if (!a->match[0]) { fail(err, errlen, at, "match is required"); return -1; }
        if (strpbrk(a->match, "\\/")) {
            fail(err, errlen, at, "match must not contain a path separator");
            return -1;
        }
        if (!dir_is_safe(a->dir)) {
            fail(err, errlen, at, "dir must be a relative path without \"..\"");
            return -1;
        }
        if (a->mode == KATOSHE_MODE_ARCHIVE && (a->keep_versions < 1 || a->keep_versions > 99)) {
            fail(err, errlen, at, "keep_versions must be 1..99");
            return -1;
        }
        for (int j = 0; j < repo->asset_count; j++) {
            if (strcmp(repo->assets[j].match, a->match) == 0) {
                fail(err, errlen, at, "duplicate match string in this repository");
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
    if (!cfg->repos) { fail(err, errlen, "repositories", "out of memory"); return -1; }

    for (int i = 0; i < n; i++) {
        const cJSON *item = cJSON_GetArrayItem(arr, i);
        katoshe_repo *r = &cfg->repos[cfg->repo_count];
        char where[64];

        snprintf(where, sizeof where, "repositories[%d]", i);
        if (!cJSON_IsObject(item)) { fail(err, errlen, where, "expected an object"); return -1; }

        r->include_prerelease = -1;
        get_str(item, "name", r->name, sizeof r->name);
        get_str(item, "url", r->url, sizeof r->url);
        get_str(item, "dir", r->dir, sizeof r->dir);
        get_str(item, "proxy", r->proxy, sizeof r->proxy);
        get_int(item, "include_prerelease", &r->include_prerelease);
        {
            const cJSON *pr = cJSON_GetObjectItemCaseSensitive(item, "include_prerelease");
            if (cJSON_IsBool(pr)) r->include_prerelease = cJSON_IsTrue(pr) ? 1 : 0;
        }

        if (!r->url[0]) { fail(err, errlen, where, "url is required"); return -1; }
        if (strstr(r->url, "github.com") == NULL) {
            fail(err, errlen, where, "url must point at github.com");
            return -1;
        }
        if (!dir_is_safe(r->dir)) {
            fail(err, errlen, where, "dir must be a relative path without \"..\"");
            return -1;
        }
        if (!r->name[0]) {
            const char *slash = strrchr(r->url, '/');
            snprintf(r->name, sizeof r->name, "%s", (slash && slash[1]) ? slash + 1 : r->url);
        }
        for (int j = 0; j < cfg->repo_count; j++) {
            if (strcmp(cfg->repos[j].name, r->name) == 0) {
                fail(err, errlen, where, "duplicate repository name");
                return -1;
            }
        }

        {
            const cJSON *assets = cJSON_GetObjectItemCaseSensitive(item, "assets");
            if (assets && !cJSON_IsArray(assets)) {
                fail(err, errlen, where, "assets must be an array");
                return -1;
            }
            if (assets && parse_assets(r, assets, where, err, errlen) != 0) {
                /* Not counted in repo_count yet, so free it here. */
                free(r->assets);
                r->assets = NULL;
                return -1;
            }
        }
        cfg->repo_count++;
    }
    return 0;
}

/* Parses and validates one preset document into cfg. On failure cfg still needs
   katoshe_config_free, which this function leaves in a consistent state. */
static int parse_config(const char *json, katoshe_config *cfg, char *err, size_t errlen) {
    cJSON *doc;
    const cJSON *repos;

    katoshe_config_defaults(cfg);
    doc = cJSON_Parse(json);
    if (!doc) { sayf(err, errlen, "not valid JSON"); return -1; }
    if (!cJSON_IsObject(doc)) { sayf(err, errlen, "the top level must be an object"); goto bad; }
    {
        const cJSON *version = cJSON_GetObjectItemCaseSensitive(doc, "version");
        if (!cJSON_IsNumber(version) || (int)version->valuedouble != PRESET_VERSION) {
            sayf(err, errlen, "version must be %d", PRESET_VERSION);
            goto bad;
        }
    }
    repos = cJSON_GetObjectItemCaseSensitive(doc, "repositories");
    if (repos && !cJSON_IsArray(repos)) {
        fail(err, errlen, "repositories", "must be an array");
        goto bad;
    }
    if (repos && parse_repos(cfg, repos, err, errlen) != 0) goto bad;

    cfg->preset_doc = doc;
    return 0;

bad:
    cJSON_Delete(doc);
    for (int i = 0; i < cfg->repo_count; i++) free(cfg->repos[i].assets);
    free(cfg->repos);
    cfg->repos = NULL;
    cfg->repo_count = 0;
    return -1;
}

int katoshe_preset_import_json(const char *json, const char *name_hint,
                               const char *presets_dir, char *name_out, size_t name_cap,
                               char *err, size_t errlen) {
    katoshe_config cfg;
    cJSON *doc;
    char *text;
    char base[KATOSHE_PRESET_MAX];
    char pick[KATOSHE_PRESET_MAX];
    char path[KATOSHE_PATH_MAX];
    const char *hint = name_hint;
    int found = 0;
    int rc = -1;

    if (err && errlen) err[0] = '\0';
    if (!json || !json[0]) { sayf(err, errlen, "the preset document is empty"); return -1; }
    if (!presets_dir || !presets_dir[0]) { sayf(err, errlen, "no presets directory"); return -1; }
    if (!name_out || name_cap == 0) { sayf(err, errlen, "no buffer for the preset name"); return -1; }
    name_out[0] = '\0';

    if (parse_config(json, &cfg, err, errlen) != 0) return -1;

    /* A "name" in the packet wins over the file stem the caller passed. */
    {
        const cJSON *nm = cJSON_GetObjectItemCaseSensitive(cfg.preset_doc, "name");
        if (cJSON_IsString(nm) && nm->valuestring && nm->valuestring[0]) hint = nm->valuestring;
    }
    sanitize_name(hint, base, sizeof base);

    if (katoshe_mkdir_all(presets_dir) != 0) {
        sayf(err, errlen, "%s: cannot create the presets directory", presets_dir);
        goto done;
    }

    for (int i = 0; i < 99; i++) {
        if (i == 0) {
            snprintf(pick, sizeof pick, "%s", base);
        } else {
            int room = PRESET_NAME_LEN - 4; /* "-99" plus the terminator */
            if (room < 1) room = 1;
            snprintf(pick, sizeof pick, "%.*s-%d", room, base, i + 1);
        }
        if (preset_path(path, sizeof path, presets_dir, pick) != 0) {
            sayf(err, errlen, "%s: the preset path is too long", presets_dir);
            goto done;
        }
        if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) { found = 1; break; }
    }
    if (!found) {
        sayf(err, errlen, "%s: every name from %s to %s-99 is taken", presets_dir, base, base);
        goto done;
    }
    if (strlen(pick) + 1 > name_cap) {
        sayf(err, errlen, "the name buffer is too small for \"%s\"", pick);
        goto done;
    }

    doc = build_doc(&cfg);
    if (!doc) { sayf(err, errlen, "out of memory"); goto done; }
    text = cJSON_PrintBuffered(doc, 4096, 1);
    cJSON_Delete(doc);
    if (!text) { sayf(err, errlen, "out of memory while printing"); goto done; }
    rc = katoshe_write_atomic(path, text, err, errlen);
    cJSON_free(text);
    if (rc == 0) snprintf(name_out, name_cap, "%s", pick);

done:
    katoshe_config_free(&cfg);
    return rc;
}

static int wrap_msg(char *err, size_t errlen, const char *label) {
    char tmp[512];
    if (err && errlen) {
        if (err[0]) snprintf(tmp, sizeof tmp, "%s: %s", label, err);
        else snprintf(tmp, sizeof tmp, "%s: rejected", label);
        snprintf(err, errlen, "%s", tmp);
    }
    return -1;
}

int katoshe_preset_import(const char *source, const char *presets_dir,
                          char *name_out, size_t name_cap, char *err, size_t errlen) {
    char *work;
    char *json = NULL;
    char *decoded = NULL;
    char *file_text = NULL;
    char stem[KATOSHE_PRESET_MAX];
    const char *hint = NULL;
    const char *label = "preset";
    int rc;

    if (err && errlen) err[0] = '\0';
    if (!source || !source[0]) { sayf(err, errlen, "empty preset source"); return -1; }

    work = trim_dup(source);
    if (!work) { sayf(err, errlen, "out of memory"); return -1; }
    stem[0] = '\0';

    if (_strnicmp(work, LINK_PREFIX, (int)strlen(LINK_PREFIX)) == 0) {
        label = "link";
        decoded = decode_link(work + strlen(LINK_PREFIX), err, errlen);
        if (!decoded) { free(work); return -1; }
        json = decoded;
    } else if (work[0] == '{' || work[0] == '[') {
        json = work;
    } else {
        label = work;
        file_text = katoshe_read_file(work);
        if (!file_text) {
            sayf(err, errlen, "%s: cannot read the file", work);
            free(work);
            return -1;
        }
        json = file_text;
        path_stem(work, stem, sizeof stem);
        hint = stem;
    }

    rc = katoshe_preset_import_json(json, hint, presets_dir, name_out, name_cap, err, errlen);
    if (rc != 0) wrap_msg(err, errlen, label);

    free(decoded);
    free(file_text);
    free(work);
    return rc;
}

void katoshe_preset_list_free(katoshe_preset_info *list) {
    free(list);
}

static int by_name(const void *a, const void *b) {
    const katoshe_preset_info *x = (const katoshe_preset_info *)a;
    const katoshe_preset_info *y = (const katoshe_preset_info *)b;
    return strcmp(x->name, y->name);
}

static void warn_add(char *buf, size_t cap, size_t *used, const char *line) {
    int n;
    if (*used + 3 >= cap) return;
    n = snprintf(buf + *used, cap - *used, "%s%s", *used ? "; " : "", line);
    if (n > 0) *used += (size_t)n;
}

int katoshe_preset_list(const char *presets_dir, katoshe_preset_info **out, int *count,
                        char *err, size_t errlen) {
    char pattern[KATOSHE_PATH_MAX + 8];
    char warn[1024];
    size_t warn_used = 0;
    WIN32_FIND_DATAA fd;
    HANDLE h;
    katoshe_preset_info *items = NULL;
    int n = 0;
    int cap = 0;

    if (err && errlen) err[0] = '\0';
    if (!out || !count) { sayf(err, errlen, "no output for the preset list"); return -1; }
    *out = NULL;
    *count = 0;
    if (!presets_dir || !presets_dir[0]) { sayf(err, errlen, "no presets directory"); return -1; }
    if (katoshe_path_join(pattern, sizeof pattern, presets_dir, "*.json") != 0) {
        sayf(err, errlen, "%s: the directory path is too long", presets_dir);
        return -1;
    }

    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        sayf(err, errlen, "%s: no preset files", presets_dir);
        return 0;
    }

    do {
        char name[KATOSHE_PRESET_MAX];
        char path[KATOSHE_PATH_MAX];
        char *text;
        katoshe_config cfg;
        char line[320];
        int assets = 0;

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (strlen(fd.cFileName) >= sizeof name) {
            snprintf(line, sizeof line, "%s: the file name is too long", fd.cFileName);
            warn_add(warn, sizeof warn, &warn_used, line);
            continue;
        }
        path_stem(fd.cFileName, name, sizeof name);
        if (!katoshe_preset_name_ok(name)) continue;

        if (katoshe_path_join(path, sizeof path, presets_dir, fd.cFileName) != 0) {
            snprintf(line, sizeof line, "%s: the path is too long", fd.cFileName);
            warn_add(warn, sizeof warn, &warn_used, line);
            continue;
        }
        text = katoshe_read_file(path);
        if (!text) {
            snprintf(line, sizeof line, "%s: cannot read the file", fd.cFileName);
            warn_add(warn, sizeof warn, &warn_used, line);
            continue;
        }
        {
            char msg[256];
            int bad = parse_config(text, &cfg, msg, sizeof msg);
            free(text);
            if (bad != 0) {
                snprintf(line, sizeof line, "%s: %s", fd.cFileName, msg);
                warn_add(warn, sizeof warn, &warn_used, line);
                continue;
            }
        }

        for (int i = 0; i < cfg.repo_count; i++) assets += cfg.repos[i].asset_count;
        if (n == cap) {
            int ncap = cap ? cap * 2 : 8;
            katoshe_preset_info *tmp =
                (katoshe_preset_info *)realloc(items, (size_t)ncap * sizeof *items);
            if (!tmp) {
                katoshe_config_free(&cfg);
                free(items);
                FindClose(h);
                sayf(err, errlen, "out of memory");
                return -1;
            }
            items = tmp;
            cap = ncap;
        }
        memset(&items[n], 0, sizeof items[n]);
        snprintf(items[n].name, sizeof items[n].name, "%s", name);
        snprintf(items[n].path, sizeof items[n].path, "%s", path);
        items[n].repo_count = cfg.repo_count;
        items[n].asset_count = assets;
        n++;
        katoshe_config_free(&cfg);
    } while (FindNextFileA(h, &fd));
    FindClose(h);

    if (n > 1) qsort(items, (size_t)n, sizeof *items, by_name);
    if (n == 0 && warn_used) {
        if (err && errlen) snprintf(err, errlen, "%s", warn);
    }
    *out = items;
    *count = n;
    return 0;
}

/* Self-check. Everything here stays inside a directory under GetTempPathA. */

static int failures;

static void check(int cond, const char *name) {
    if (!cond) {
        printf("FAIL %s\n", name);
        failures++;
    }
}

static int count_of(const char *text, const char *needle) {
    int n = 0;
    size_t len = strlen(needle);
    const char *p = text;
    while (len && (p = strstr(p, needle)) != NULL) {
        n++;
        p += len;
    }
    return n;
}

typedef struct {
    char root[MAX_PATH];
    char presets[KATOSHE_PATH_MAX];
    char warn_dir[KATOSHE_PATH_MAX];
} preset_sandbox;

static void sandbox_wipe(const char *dir) {
    char pattern[KATOSHE_PATH_MAX + 8];
    WIN32_FIND_DATAA fd;
    HANDLE h;

    if (katoshe_path_join(pattern, sizeof pattern, dir, "*.json") != 0) return;
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        char path[KATOSHE_PATH_MAX];
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (katoshe_path_join(path, sizeof path, dir, fd.cFileName) == 0) DeleteFileA(path);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static int sandbox_init(preset_sandbox *sb, const char *suffix) {
    char tmp[MAX_PATH];

    if (GetTempPathA(sizeof tmp, tmp) == 0) return -1;
    memset(sb, 0, sizeof *sb);
    snprintf(sb->root, sizeof sb->root, "%skatoshe-preset-%lu%s", tmp,
             (unsigned long)GetCurrentProcessId(), suffix);
    if (katoshe_mkdir_all(sb->root) != 0) return -1;
    if (katoshe_path_join(sb->presets, sizeof sb->presets, sb->root, "presets") != 0) return -1;
    if (katoshe_path_join(sb->warn_dir, sizeof sb->warn_dir, sb->root, "warn") != 0) return -1;
    if (katoshe_mkdir_all(sb->presets) != 0 || katoshe_mkdir_all(sb->warn_dir) != 0) return -1;
    sandbox_wipe(sb->presets);
    sandbox_wipe(sb->warn_dir);
    return 0;
}

static void sandbox_drop(preset_sandbox *sb) {
    sandbox_wipe(sb->presets);
    sandbox_wipe(sb->warn_dir);
    RemoveDirectoryA(sb->presets);
    RemoveDirectoryA(sb->warn_dir);
    RemoveDirectoryA(sb->root);
}

static int fill_cfg(katoshe_config *cfg) {
    katoshe_config_defaults(cfg);
    cfg->repos = (katoshe_repo *)calloc(2, sizeof *cfg->repos);
    if (!cfg->repos) return -1;
    cfg->repo_count = 2;

    {
        katoshe_repo *r = &cfg->repos[0];
        snprintf(r->name, sizeof r->name, "alpha");
        snprintf(r->url, sizeof r->url, "https://github.com/owner/alpha");
        r->include_prerelease = 1;
        r->assets = (katoshe_asset *)calloc(2, sizeof *r->assets);
        if (!r->assets) return -1;
        r->asset_count = 2;
        snprintf(r->assets[0].match, sizeof r->assets[0].match, "*.apk");
        r->assets[0].mode = KATOSHE_MODE_REPLACE;
        r->assets[0].keep_versions = 3;
        snprintf(r->assets[1].match, sizeof r->assets[1].match, "*.zip");
        snprintf(r->assets[1].filename, sizeof r->assets[1].filename, "alpha.zip");
        snprintf(r->assets[1].dir, sizeof r->assets[1].dir, "sub");
        r->assets[1].mode = KATOSHE_MODE_ARCHIVE;
        r->assets[1].keep_versions = 5;
    }
    {
        katoshe_repo *r = &cfg->repos[1];
        snprintf(r->name, sizeof r->name, "beta");
        snprintf(r->url, sizeof r->url, "https://github.com/owner/beta");
        snprintf(r->dir, sizeof r->dir, "beta-dir");
        snprintf(r->proxy, sizeof r->proxy, "socks5h://127.0.0.1:1080");
        r->include_prerelease = -1;
        r->assets = (katoshe_asset *)calloc(1, sizeof *r->assets);
        if (!r->assets) return -1;
        r->asset_count = 1;
        snprintf(r->assets[0].match, sizeof r->assets[0].match, "*.exe");
        r->assets[0].mode = KATOSHE_MODE_REPLACE;
        r->assets[0].keep_versions = 3;
    }
    return 0;
}

static int same_cfg(const katoshe_config *a, const katoshe_config *b) {
    if (a->repo_count != b->repo_count) return 0;
    for (int i = 0; i < a->repo_count; i++) {
        const katoshe_repo *x = &a->repos[i];
        const katoshe_repo *y = &b->repos[i];

        if (strcmp(x->name, y->name) != 0 || strcmp(x->url, y->url) != 0 ||
            strcmp(x->dir, y->dir) != 0 || strcmp(x->proxy, y->proxy) != 0 ||
            x->include_prerelease != y->include_prerelease) {
            return 0;
        }
        if (x->asset_count != y->asset_count) return 0;
        for (int j = 0; j < x->asset_count; j++) {
            const katoshe_asset *p = &x->assets[j];
            const katoshe_asset *q = &y->assets[j];
            if (strcmp(p->match, q->match) != 0 || strcmp(p->filename, q->filename) != 0 ||
                strcmp(p->dir, q->dir) != 0 || p->mode != q->mode ||
                p->keep_versions != q->keep_versions) {
                return 0;
            }
        }
    }
    return 1;
}

int katoshe_preset_selftest(void) {
    preset_sandbox sb;
    katoshe_config cfg;
    char err[512];
    char path[KATOSHE_PATH_MAX];
    char link[8192];
    char name[KATOSHE_PRESET_MAX];

    failures = 0;
    if (sandbox_init(&sb, "-preset") != 0) {
        printf("FAIL temp directory\n");
        return 1;
    }
    if (fill_cfg(&cfg) != 0) {
        printf("FAIL build a test config\n");
        katoshe_config_free(&cfg);
        sandbox_drop(&sb);
        return 1;
    }

    /* 1. write -> read -> parse preserves the whole selection */
    check(preset_path(path, sizeof path, sb.presets, "rt") == 0, "round-trip path");
    check(katoshe_preset_write(&cfg, path, err, sizeof err) == 0, "write a preset");
    {
        char *text = katoshe_read_file(path);
        check(text != NULL, "read the written preset");
        if (text) {
            katoshe_config back;
            check(parse_config(text, &back, err, sizeof err) == 0, "the written preset parses");
            check(same_cfg(&cfg, &back), "repositories and assets survived the round trip");
            katoshe_config_free(&back);

            check(count_of(text, "\"version\"") == 1, "version is written");
            check(count_of(text, "\"repositories\"") == 1, "repositories are written");
            check(count_of(text, "\"include_prerelease\"") == 1,
                  "include_prerelease is written only when set");
            check(strstr(text, "\": \"\"") == NULL, "empty optional values are not written");
            free(text);
        }
    }

    /* 2. the same packet as a link, imported from scratch */
    check(katoshe_preset_to_link(&cfg, link, sizeof link, err, sizeof err) == 0, "encode a link");
    check(strncmp(link, LINK_PREFIX, strlen(LINK_PREFIX)) == 0, "the link carries the scheme");
    check(katoshe_preset_import(link, sb.presets, name, sizeof name, err, sizeof err) == 0,
          "import a link");
    check(strcmp(name, "default") == 0, "the packet keeps the preset name");
    {
        char *text = NULL;
        katoshe_config back;
        if (preset_path(path, sizeof path, sb.presets, name) == 0) text = katoshe_read_file(path);
        check(text != NULL, "the imported file exists");
        if (text) {
            check(parse_config(text, &back, err, sizeof err) == 0 && same_cfg(&cfg, &back),
                  "a link carries the same repositories");
            katoshe_config_free(&back);
            free(text);
        }
    }

    /* 3. never overwrite: the second copy gets a suffix */
    check(katoshe_preset_import(link, sb.presets, name, sizeof name, err, sizeof err) == 0,
          "import the same link twice");
    check(strcmp(name, "default-2") == 0, "a taken name becomes name-2");

    /* 4. a name hint is sanitised into a legal file stem */
    {
        const char *json = "{\"version\":1,\"repositories\":[{\"name\":\"gamma\","
                           "\"url\":\"https://github.com/owner/gamma\",\"assets\":[]}]}";
        check(katoshe_preset_import_json(json, "my pack!", sb.presets, name, sizeof name,
                                         err, sizeof err) == 0,
              "import json with a hint");
        check(katoshe_preset_name_ok(name), "the imported name is a legal stem");
        check(strcmp(name, "my-pack") == 0, "the hint is sanitised");
    }

    /* 5. untrusted fields are validated like config.c validates them */
    {
        const char *traversal = "{\"version\":1,\"repositories\":[{\"name\":\"evil\","
                                "\"url\":\"https://github.com/o/e\",\"assets\":"
                                "[{\"match\":\"a.apk\",\"dir\":\"..\\\\..\\\\Windows\"}]}]}";
        const char *dupmatch = "{\"version\":1,\"repositories\":[{\"name\":\"d\","
                               "\"url\":\"https://github.com/o/d\",\"assets\":"
                               "[{\"match\":\"a.apk\"},{\"match\":\"a.apk\"}]}]}";
        const char *dupname = "{\"version\":1,\"repositories\":["
                              "{\"name\":\"same\",\"url\":\"https://github.com/o/a\",\"assets\":[]},"
                              "{\"name\":\"same\",\"url\":\"https://github.com/o/b\",\"assets\":[]}]}";
        const char *foreign = "{\"version\":1,\"repositories\":[{\"name\":\"g\","
                              "\"url\":\"https://gitlab.com/o/r\",\"assets\":[]}]}";

        check(katoshe_preset_import_json(traversal, "evil", sb.presets, name, sizeof name,
                                         err, sizeof err) != 0 &&
                  strstr(err, "relative") != NULL,
              "a traversal in dir is rejected");
        check(katoshe_path_join(path, sizeof path, sb.presets, "evil.json") == 0 &&
                  GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES,
              "a rejected import writes nothing");

        check(katoshe_preset_import_json(dupmatch, "d", sb.presets, name, sizeof name,
                                         err, sizeof err) != 0 &&
                  strstr(err, "duplicate match") != NULL,
              "a duplicate match is rejected");
        check(katoshe_preset_import_json(dupname, "n", sb.presets, name, sizeof name,
                                         err, sizeof err) != 0 &&
                  strstr(err, "duplicate repository name") != NULL,
              "a duplicate repository name is rejected");
        check(katoshe_preset_import_json(foreign, "g", sb.presets, name, sizeof name,
                                         err, sizeof err) != 0 &&
                  strstr(err, "github.com") != NULL,
              "a non-github url is rejected");
    }

    /* 6. broken links say what is broken */
    check(katoshe_preset_import("katoshe://not base64!!", sb.presets, name, sizeof name,
                                err, sizeof err) != 0 &&
              strstr(err, "base64") != NULL,
          "garbage in a link names base64");
    check(katoshe_preset_import("katoshe://AAAAA", sb.presets, name, sizeof name,
                                err, sizeof err) != 0 &&
              err[0] != '\0',
          "an unterminated link is rejected with a message");
    {
        size_t n = strlen(link);
        if (n > 32) {
            char cut[sizeof link];
            snprintf(cut, sizeof cut, "%s", link);
            cut[n - 6] = '\0';
            check(katoshe_preset_import(cut, sb.presets, name, sizeof name, err, sizeof err) != 0,
                  "a truncated link is rejected");
        } else {
            check(0, "a truncated link is rejected");
        }
    }

    /* 7. limits and missing inputs */
    {
        char tiny[16];
        check(katoshe_preset_to_link(&cfg, tiny, sizeof tiny, err, sizeof err) != 0 &&
                  strstr(err, "bytes") != NULL,
              "a short link buffer is reported");
    }
    check(katoshe_preset_import_json("{\"version\":2,\"repositories\":[]}", NULL, sb.presets,
                                     name, sizeof name, err, sizeof err) != 0 &&
              strstr(err, "version must be 1") != NULL,
          "a foreign version is rejected");
    check(katoshe_preset_write(NULL, path, err, sizeof err) != 0, "write rejects no config");
    check(katoshe_preset_to_link(&cfg, NULL, 0, err, sizeof err) != 0, "link rejects no buffer");
    check(katoshe_preset_import("", sb.presets, name, sizeof name, err, sizeof err) != 0,
          "import rejects an empty source");
    check(katoshe_preset_list(NULL, NULL, NULL, err, sizeof err) != 0, "list rejects no output");

    /* 8. listing reads each file and reports broken ones only when nothing works */
    {
        katoshe_preset_info *list = NULL;
        int count = 0;

        check(katoshe_preset_list(sb.presets, &list, &count, err, sizeof err) == 0 && count == 4,
              "list sees every preset");
        check(list && count == 4 && strcmp(list[0].name, "default") == 0 &&
                  strcmp(list[3].name, "rt") == 0,
              "the list is sorted by name");
        check(list && count == 4 && list[3].repo_count == 2 && list[3].asset_count == 3,
              "the list counts repositories and assets");
        katoshe_preset_list_free(list);

        if (preset_path(path, sizeof path, sb.warn_dir, "broken") == 0) {
            check(katoshe_write_atomic(path, "{ not json", err, sizeof err) == 0,
                  "seed a broken preset file");
        }
        list = NULL;
        count = -1;
        check(katoshe_preset_list(sb.warn_dir, &list, &count, err, sizeof err) == 0 && count == 0,
              "a directory of broken files holds no presets");
        check(err[0] && strstr(err, "broken.json") != NULL,
              "broken files are reported as warnings");
        katoshe_preset_list_free(list);
    }

    katoshe_config_free(&cfg);
    sandbox_drop(&sb);
    return failures;
}
