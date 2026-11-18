#ifndef KATOSHE_CONFIG_H
#define KATOSHE_CONFIG_H

#include <stdbool.h>
#include <stddef.h>

#define KATOSHE_PATH_MAX 512
#define KATOSHE_NAME_MAX 256
#define KATOSHE_PRESET_MAX 64

#define KATOSHE_MODE_REPLACE 0
#define KATOSHE_MODE_ARCHIVE 1

/* config/settings.json - per machine, never shared.
   config/presets/<name>.json - shareable: repositories and selected assets only. */

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
    char preset[KATOSHE_PRESET_MAX]; /* active preset name */
    katoshe_global global;
    katoshe_repo *repos;
    int repo_count;
    /* Parsed documents, kept so save() can carry unknown keys forward. */
    struct cJSON *settings_doc;
    struct cJSON *preset_doc;
} katoshe_config;

void katoshe_config_defaults(katoshe_config *cfg);

/* Loads settings then the active preset. `preset_override` (may be NULL) wins
   over the name stored in settings. Returns 0 on success, -1 with a message
   naming the offending file. */
int katoshe_config_load(katoshe_config *cfg, const char *settings_path,
                        const char *presets_dir, const char *preset_override,
                        char *err, size_t errlen);

/* Writes both files atomically (tmp + replace). Unknown fields survive.
   The active preset keeps its name; the repositories always go to its file. */
int katoshe_config_save(katoshe_config *cfg, const char *settings_path,
                        const char *presets_dir, char *err, size_t errlen);

void katoshe_config_free(katoshe_config *cfg);

/* Preset names become file names, so they are held to [A-Za-z0-9._-]. */
bool katoshe_preset_name_ok(const char *name);

/* "preset default: 3 repositories, 7 assets, jobs 2, proxy socks5h://..." */
void katoshe_config_summary(const katoshe_config *cfg, char *out, size_t cap);

#endif
