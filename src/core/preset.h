#ifndef KATOSHE_PRESET_H
#define KATOSHE_PRESET_H

#include <stddef.h>

#include "config.h"

/* Shareable preset packets: presets_dir/<name>.json, or "katoshe://<base64>".
   A preset never carries per-machine settings. */

typedef struct {
    char name[KATOSHE_PRESET_MAX]; /* file stem */
    char path[KATOSHE_PATH_MAX];
    int repo_count;
    int asset_count;
} katoshe_preset_info;

/* Reads every presets_dir/<name>.json, sorted by name. Files that do not parse
   are skipped and reported through err as a warning list only when nothing
   usable was found. Returns 0 on success (possibly zero presets). *out is
   calloc'd and must be freed with katoshe_preset_list_free. */
int katoshe_preset_list(const char *presets_dir, katoshe_preset_info **out, int *count,
                        char *err, size_t errlen);
void katoshe_preset_list_free(katoshe_preset_info *list);

/* Writes the repositories of an in-memory config as a standalone preset file. */
int katoshe_preset_write(const katoshe_config *cfg, const char *path,
                         char *err, size_t errlen);

/* "katoshe://" + base64 of the preset JSON. cap must be >= 4*len/3+16. */
int katoshe_preset_to_link(const katoshe_config *cfg, char *out, size_t cap,
                           char *err, size_t errlen);

/* Accepts a .json path, a raw "katoshe://<base64>" string, or a JSON document
   already in memory. Writes presets_dir/<name>.json without ever overwriting:
   a taken name becomes name-2 ... name-99. Returns the chosen name in name_out. */
int katoshe_preset_import(const char *source, const char *presets_dir,
                          char *name_out, size_t name_cap, char *err, size_t errlen);

/* Same, from an explicit JSON buffer with an optional name hint (may be NULL). */
int katoshe_preset_import_json(const char *json, const char *name_hint,
                               const char *presets_dir, char *name_out, size_t name_cap,
                               char *err, size_t errlen);

/* Self-check without a framework. Prints one "FAIL <name>" line per failure and
   returns the failure count, 0 when everything passed. */
int katoshe_preset_selftest(void);

#endif
