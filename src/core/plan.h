#ifndef KATOSHE_PLAN_H
#define KATOSHE_PLAN_H

#include <stddef.h>

#include "config.h"
#include "gh.h"
#include "state.h"

typedef enum {
    PLAN_DOWNLOAD,  /* fetch it */
    PLAN_UNCHANGED, /* already correct on disk */
    PLAN_MISSING    /* the selection matches nothing in this release */
} plan_action;

typedef struct {
    char match[KATOSHE_NAME_MAX];
    char remote_name[KATOSHE_NAME_MAX];
    char url[1024];
    char path[KATOSHE_PATH_MAX]; /* absolute target */
    long long size;
    long long asset_id;
    char digest[80];
    plan_action action;
    char reason[48];
} plan_item;

typedef struct {
    plan_item *items;
    int count;
} plan;

/* Builds one item per selection in repo->assets, in configuration order.
   `download_dir` must be absolute; `state` may be NULL. The plan is freshly
   allocated, so release the previous contents with katoshe_plan_free first. */
int katoshe_plan_build(const katoshe_repo *repo, const char *download_dir,
                       const gh_release *release, const gh_asset *assets, int asset_count,
                       const katoshe_state_repo *state, plan *out, char *err, size_t errlen);

void katoshe_plan_free(plan *p);

/* "download", "unchanged", "missing". */
const char *katoshe_plan_action_name(plan_action action);

#endif
