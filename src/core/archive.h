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

/* Self-check: prints one "FAIL <name>" line per failure, returns the count. */
int katoshe_archive_selftest(void);

#endif
