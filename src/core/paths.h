#ifndef KATOSHE_PATHS_H
#define KATOSHE_PATHS_H

#include <stdbool.h>
#include <stddef.h>

/* Where the tool keeps its files.
   portable: a `config` directory sits next to the exe, so everything lives there.
   otherwise: %APPDATA%\Katoshe. `--root` overrides both. */
typedef struct {
    char base[512];
    char config_dir[512];
    char data_dir[512];
    char presets_dir[512];
    bool portable;
} katoshe_paths;

/* Returns 0 on success; -1 fills err. Creates the directories it promises. */
int katoshe_paths_init(katoshe_paths *p, const char *root_override, char *err, size_t errlen);

/* join(out, cap, a, b) with a single backslash, no trailing separator on a. */
int katoshe_path_join(char *out, size_t cap, const char *a, const char *b);

/* Directory the running exe lives in (UTF-8). */
int katoshe_exe_dir(char *out, size_t cap);

/* Creates one directory level. Returns 0 if it exists afterwards. */
int katoshe_mkdir(const char *path);

#endif
