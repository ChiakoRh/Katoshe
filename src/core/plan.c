#include "plan.h"

#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "match.h"

static void copy_str(char *dst, size_t cap, const char *src) {
    size_t n = 0;
    if (!src) { if (cap) dst[0] = '\0'; return; }
    while (n + 1 < cap && src[n]) n++;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static int local_size(const char *path, long long *out) {
    WIN32_FILE_ATTRIBUTE_DATA info;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &info)) return -1;
    if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return -1;
    *out = ((long long)info.nFileSizeHigh << 32) | (long long)info.nFileSizeLow;
    return 0;
}

/* Exact name first, then the first match in name order. Unmatched assets are
   never touched (design §5). */
static const gh_asset *select_asset(const char *pattern, const gh_asset *assets, int count) {
    const gh_asset *best = NULL;

    for (int i = 0; i < count; i++) {
        if (strcmp(assets[i].name, pattern) == 0) return &assets[i];
    }
    for (int i = 0; i < count; i++) {
        if (!katoshe_match(pattern, assets[i].name)) continue;
        if (!best || strcmp(assets[i].name, best->name) < 0) best = &assets[i];
    }
    return best;
}

static int target_path(const katoshe_repo *repo, const katoshe_asset *a, const char *remote_name,
                       const char *download_dir, char *out, size_t cap) {
    const char *repo_dir = repo->dir[0] ? repo->dir : repo->name;
    const char *file_name = (a->mode == KATOSHE_MODE_ARCHIVE || !a->filename[0])
                                ? remote_name
                                : a->filename;

    if (a->dir[0]) {
        if (snprintf(out, cap, "%s\\%s\\%s\\%s", download_dir, repo_dir, a->dir, file_name) >=
            (int)cap) {
            return -1;
        }
    } else if (snprintf(out, cap, "%s\\%s\\%s", download_dir, repo_dir, file_name) >= (int)cap) {
        return -1;
    }
    return 0;
}

const char *katoshe_plan_action_name(plan_action action) {
    switch (action) {
        case PLAN_DOWNLOAD: return "download";
        case PLAN_UNCHANGED: return "unchanged";
        case PLAN_MISSING: return "missing";
    }
    return "?";
}

/* The decision table from IMPLEMENTATION_PLAN.md §6, in one place. */
static plan_action decide(const gh_asset *asset, const katoshe_state_asset *st,
                          long long release_id, long long state_release_id, int have_local,
                          long long local_bytes, const char **reason) {
    if (!have_local) {
        *reason = "new file";
        return PLAN_DOWNLOAD;
    }
    if (have_local && st && st->size > 0 && st->size != local_bytes) {
        *reason = "local size differs";
        return PLAN_DOWNLOAD;
    }
    if (st && release_id && state_release_id == release_id && st->asset_id == asset->id &&
        st->size == asset->size) {
        *reason = "same release";
        return PLAN_UNCHANGED;
    }
    if (!st || (!st->digest[0] && !st->sha256[0] && !st->asset_id)) {
        *reason = "no record";
        return PLAN_DOWNLOAD;
    }
    if (asset->digest[0] && st->digest[0]) {
        if (strcmp(asset->digest, st->digest) == 0) {
            *reason = "digest unchanged";
            return PLAN_UNCHANGED;
        }
        *reason = "digest changed";
        return PLAN_DOWNLOAD;
    }
    if (st->asset_id == asset->id && st->size == asset->size) {
        *reason = "asset unchanged";
        return PLAN_UNCHANGED;
    }
    *reason = "asset changed";
    return PLAN_DOWNLOAD;
}

int katoshe_plan_build(const katoshe_repo *repo, const char *download_dir,
                       const gh_release *release, const gh_asset *assets, int asset_count,
                       const katoshe_state_repo *state, plan *out, char *err, size_t errlen) {
    memset(out, 0, sizeof *out);
    if (repo->asset_count <= 0) return 0;

    out->items = (plan_item *)calloc((size_t)repo->asset_count, sizeof *out->items);
    if (!out->items) {
        snprintf(err, errlen, "out of memory");
        return -1;
    }

    for (int i = 0; i < repo->asset_count; i++) {
        const katoshe_asset *sel = &repo->assets[i];
        plan_item *item = &out->items[out->count];
        const gh_asset *asset = select_asset(sel->match, assets, asset_count);
        const katoshe_state_asset *st = state ? katoshe_state_asset_get((katoshe_state_repo *)state,
                                                                       sel->match, 0)
                                              : NULL;
        const char *reason = "";
        long long bytes = 0;
        int have_local;

        copy_str(item->match, sizeof item->match, sel->match);
        if (!asset) {
            item->action = PLAN_MISSING;
            copy_str(item->reason, sizeof item->reason, "not in this release");
            out->count++;
            continue;
        }

        copy_str(item->remote_name, sizeof item->remote_name, asset->name);
        copy_str(item->url, sizeof item->url, asset->url);
        copy_str(item->digest, sizeof item->digest, asset->digest);
        item->size = asset->size;
        item->asset_id = asset->id;

        if (target_path(repo, sel, asset->name, download_dir, item->path, sizeof item->path) != 0) {
            snprintf(err, errlen, "%s: target path too long", sel->match);
            katoshe_plan_free(out);
            return -1;
        }

        have_local = local_size(item->path, &bytes) == 0;
        item->action = decide(asset, st, release->id, state ? state->release_id : 0, have_local,
                              bytes, &reason);
        copy_str(item->reason, sizeof item->reason, reason);
        out->count++;
    }
    return 0;
}

void katoshe_plan_free(plan *p) {
    if (!p) return;
    free(p->items);
    memset(p, 0, sizeof *p);
}
