#ifndef KATOSHE_STATE_H
#define KATOSHE_STATE_H

#include <stddef.h>

#include "config.h"

/* data/lockfile.json: what was last successfully installed, so the next run can
   tell "already up to date" from "download this". Keys are the selection's match
   string, never the remote file name, so a renamed asset is still recognised. */

typedef struct {
    char match[KATOSHE_NAME_MAX];
    long long asset_id;
    char remote_name[KATOSHE_NAME_MAX];
    char path[KATOSHE_PATH_MAX];
    long long size;
    char digest[80];     /* as GitHub reported it, may be empty */
    char sha256[72];     /* as computed locally, may be empty */
    char downloaded_at[32];
} katoshe_state_asset;

typedef struct {
    char key[300]; /* owner/repo */
    long long release_id;
    char tag[128];
    char etag[128];
    char checked_at[32];
    katoshe_state_asset *assets;
    int asset_count;
} katoshe_state_repo;

typedef struct {
    katoshe_state_repo *repos;
    int repo_count;
} katoshe_state;

/* A missing file is an empty state, not an error. A corrupt one is an error. */
int katoshe_state_load(katoshe_state *st, const char *path, char *err, size_t errlen);

int katoshe_state_save(katoshe_state *st, const char *path, char *err, size_t errlen);

void katoshe_state_free(katoshe_state *st);

/* Find or, when create is non-zero, append. NULL when the caller owns the data
   and nothing matched. */
katoshe_state_repo *katoshe_state_repo_get(katoshe_state *st, const char *key, int create);
katoshe_state_asset *katoshe_state_asset_get(katoshe_state_repo *r, const char *match, int create);

#endif
