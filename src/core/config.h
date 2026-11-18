#ifndef KATOSHE_CONFIG_H
#define KATOSHE_CONFIG_H

#include <stdbool.h>
#include <stddef.h>

#define KATOSHE_PATH_MAX 512
#define KATOSHE_NAME_MAX 256

#define KATOSHE_MODE_REPLACE 0
#define KATOSHE_MODE_ARCHIVE 1

typedef struct {
    char download_dir[KATOSHE_PATH_MAX];
    char proxy[KATOSHE_PATH_MAX];
    bool include_prerelease;
    bool check_on_launch;
    int jobs;
    int timeout_sec;
    int retries;
    char log_level[16];
} katoshe_global;

typedef struct {
    char match[KATOSHE_NAME_MAX];    /* stable identity of the selection */
    char filename[KATOSHE_NAME_MAX]; /* empty = remote filename (replace mode) */
    char dir[KATOSHE_PATH_MAX];      /* empty = repository directory */
    int mode;                        /* KATOSHE_MODE_* */
    int keep_versions;               /* archive mode only */
} katoshe_asset;

typedef struct {
    char name[128];
    char url[KATOSHE_PATH_MAX];
    char dir[KATOSHE_NAME_MAX];      /* empty = repository name */
    int include_prerelease;          /* -1 = inherit the global setting */
    char proxy[KATOSHE_PATH_MAX];    /* empty = inherit the global setting */
    katoshe_asset *assets;
    int asset_count;
} katoshe_repo;

typedef struct {
    katoshe_global global;
    katoshe_repo *repos;
    int repo_count;
    struct cJSON *doc; /* parsed document; kept so save() can carry unknown keys forward */
} katoshe_config;

void katoshe_config_defaults(katoshe_config *cfg);

/* Loads and validates. Returns 0 on success, -1 with a message naming the JSON path. */
int katoshe_config_load(katoshe_config *cfg, const char *path, char *err, size_t errlen);

/* Writes atomically (tmp + replace). Unknown fields from the loaded document survive. */
int katoshe_config_save(katoshe_config *cfg, const char *path, char *err, size_t errlen);

void katoshe_config_free(katoshe_config *cfg);

/* "3 repositories, 7 assets - proxy socks5h://..., jobs 2" */
void katoshe_config_summary(const katoshe_config *cfg, char *out, size_t cap);

#endif
