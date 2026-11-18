#include "state.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cjson/cJSON.h"
#include "io.h"

#define KATOSHE_STATE_VERSION 1

static void copy_str(char *dst, size_t cap, const char *src) {
    size_t n = 0;
    if (!src) { if (cap) dst[0] = '\0'; return; }
    while (n + 1 < cap && src[n]) n++;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static const char *str_of(const cJSON *obj, const char *key) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return (cJSON_IsString(v) && v->valuestring) ? v->valuestring : "";
}

static long long int_of(const cJSON *obj, const char *key) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(v) ? (long long)v->valuedouble : 0;
}

void katoshe_state_free(katoshe_state *st) {
    if (!st) return;
    for (int i = 0; i < st->repo_count; i++) free(st->repos[i].assets);
    free(st->repos);
    memset(st, 0, sizeof *st);
}

katoshe_state_repo *katoshe_state_repo_get(katoshe_state *st, const char *key, int create) {
    for (int i = 0; i < st->repo_count; i++) {
        if (strcmp(st->repos[i].key, key) == 0) return &st->repos[i];
    }
    if (!create) return NULL;
    {
        katoshe_state_repo *grown =
            (katoshe_state_repo *)realloc(st->repos, sizeof *st->repos * (size_t)(st->repo_count + 1));
        katoshe_state_repo *r;
        if (!grown) return NULL;
        st->repos = grown;
        r = &st->repos[st->repo_count];
        memset(r, 0, sizeof *r);
        copy_str(r->key, sizeof r->key, key);
        st->repo_count++;
        return r;
    }
}

katoshe_state_asset *katoshe_state_asset_get(katoshe_state_repo *r, const char *match, int create) {
    for (int i = 0; i < r->asset_count; i++) {
        if (strcmp(r->assets[i].match, match) == 0) return &r->assets[i];
    }
    if (!create) return NULL;
    {
        katoshe_state_asset *grown =
            (katoshe_state_asset *)realloc(r->assets, sizeof *r->assets * (size_t)(r->asset_count + 1));
        katoshe_state_asset *a;
        if (!grown) return NULL;
        r->assets = grown;
        a = &r->assets[r->asset_count];
        memset(a, 0, sizeof *a);
        copy_str(a->match, sizeof a->match, match);
        r->asset_count++;
        return a;
    }
}

int katoshe_state_load(katoshe_state *st, const char *path, char *err, size_t errlen) {
    char *text;
    cJSON *doc;
    const cJSON *repos;
    const cJSON *version;

    memset(st, 0, sizeof *st);

    text = katoshe_read_file(path);
    if (!text) return 0; /* first run */

    doc = cJSON_Parse(text);
    free(text);
    if (!doc) {
        snprintf(err, errlen, "%s is not valid JSON; delete it to start over", path);
        return -1;
    }

    version = cJSON_GetObjectItemCaseSensitive(doc, "version");
    if (!cJSON_IsNumber(version) || (int)version->valuedouble != KATOSHE_STATE_VERSION) {
        snprintf(err, errlen, "%s has an unsupported version; delete it to start over", path);
        cJSON_Delete(doc);
        return -1;
    }

    repos = cJSON_GetObjectItemCaseSensitive(doc, "repositories");
    if (cJSON_IsObject(repos)) {
        const cJSON *entry = NULL;
        cJSON_ArrayForEach(entry, repos) {
            katoshe_state_repo *r;
            const cJSON *assets;
            const cJSON *asset;
            if (!entry->string) continue;
            r = katoshe_state_repo_get(st, entry->string, 1);
            if (!r) { cJSON_Delete(doc); snprintf(err, errlen, "out of memory"); return -1; }
            r->release_id = int_of(entry, "release_id");
            copy_str(r->tag, sizeof r->tag, str_of(entry, "release_tag"));
            copy_str(r->etag, sizeof r->etag, str_of(entry, "etag"));
            copy_str(r->checked_at, sizeof r->checked_at, str_of(entry, "checked_at"));

            assets = cJSON_GetObjectItemCaseSensitive(entry, "assets");
            if (!cJSON_IsObject(assets)) continue;
            cJSON_ArrayForEach(asset, assets) {
                katoshe_state_asset *a;
                if (!asset->string) continue;
                a = katoshe_state_asset_get(r, asset->string, 1);
                if (!a) { cJSON_Delete(doc); snprintf(err, errlen, "out of memory"); return -1; }
                a->asset_id = int_of(asset, "asset_id");
                a->size = int_of(asset, "size");
                copy_str(a->remote_name, sizeof a->remote_name, str_of(asset, "remote_name"));
                copy_str(a->path, sizeof a->path, str_of(asset, "path"));
                copy_str(a->digest, sizeof a->digest, str_of(asset, "digest"));
                copy_str(a->sha256, sizeof a->sha256, str_of(asset, "sha256"));
                copy_str(a->downloaded_at, sizeof a->downloaded_at, str_of(asset, "downloaded_at"));
            }
        }
    }

    cJSON_Delete(doc);
    return 0;
}

int katoshe_state_save(katoshe_state *st, const char *path, char *err, size_t errlen) {
    cJSON *doc = cJSON_CreateObject();
    cJSON *repos;
    char *text;
    int rc;

    if (!doc) {
        snprintf(err, errlen, "out of memory");
        return -1;
    }
    cJSON_AddNumberToObject(doc, "version", KATOSHE_STATE_VERSION);
    repos = cJSON_CreateObject();
    cJSON_AddItemToObject(doc, "repositories", repos);

    for (int i = 0; i < st->repo_count; i++) {
        const katoshe_state_repo *r = &st->repos[i];
        cJSON *ro = cJSON_CreateObject();
        cJSON *assets = cJSON_CreateObject();

        cJSON_AddNumberToObject(ro, "release_id", (double)r->release_id);
        cJSON_AddStringToObject(ro, "release_tag", r->tag);
        if (r->etag[0]) cJSON_AddStringToObject(ro, "etag", r->etag);
        if (r->checked_at[0]) cJSON_AddStringToObject(ro, "checked_at", r->checked_at);

        for (int j = 0; j < r->asset_count; j++) {
            const katoshe_state_asset *a = &r->assets[j];
            cJSON *ao = cJSON_CreateObject();
            cJSON_AddNumberToObject(ao, "asset_id", (double)a->asset_id);
            cJSON_AddStringToObject(ao, "remote_name", a->remote_name);
            cJSON_AddStringToObject(ao, "path", a->path);
            cJSON_AddNumberToObject(ao, "size", (double)a->size);
            if (a->digest[0]) cJSON_AddStringToObject(ao, "digest", a->digest);
            if (a->sha256[0]) cJSON_AddStringToObject(ao, "sha256", a->sha256);
            if (a->downloaded_at[0]) cJSON_AddStringToObject(ao, "downloaded_at", a->downloaded_at);
            cJSON_AddItemToObject(assets, a->match, ao);
        }

        cJSON_AddItemToObject(ro, "assets", assets);
        cJSON_AddItemToObject(repos, r->key, ro);
    }

    text = cJSON_PrintBuffered(doc, 4096, 1);
    cJSON_Delete(doc);
    if (!text) {
        snprintf(err, errlen, "out of memory while printing the lockfile");
        return -1;
    }
    rc = katoshe_write_atomic(path, text, err, errlen);
    cJSON_free(text);
    return rc;
}
