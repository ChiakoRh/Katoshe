#ifndef KATOSHE_ARCHIVE_H
#define KATOSHE_ARCHIVE_H

#include <stddef.h>

/* Archive mode keeps every release side by side in one directory. After a
   successful download this drops the oldest files of that selection so that
   `keep` of them remain, the newest (the one just downloaded) included.

   Only regular files whose name matches `pattern` are considered, so two
   selections that share a directory keep to themselves as long as their
   patterns do not overlap. Returns the number deleted, or -1 with a message. */
int katoshe_archive_prune(const char *dir, const char *pattern, int keep,
                          char *err, size_t errlen);

/* Replace mode keeps one file per selection. When the remote name changes the
   target path moves with it, so the previous file (the path the lockfile
   remembers) is deleted once the new one has landed. `previous_path` may be
   empty. Returns the number of files removed (0 or 1); never an error. */
int katoshe_replaced_old_file(const char *previous_path, const char *new_path,
                              char *removed_name, size_t name_cap);

/* Self-check: prints one "FAIL <name>" line per failure, returns the count. */
int katoshe_archive_selftest(void);

#endif
