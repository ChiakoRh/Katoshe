#ifndef KATOSHE_IO_H
#define KATOSHE_IO_H

#include <stddef.h>

/* Whole file into a malloc'd, NUL-terminated buffer. NULL on failure. */
char *katoshe_read_file(const char *path);

/* tmp + MoveFileExA(MOVEFILE_REPLACE_EXISTING): the destination is either the
   old file or the new one, never a half-written mixture. */
int katoshe_write_atomic(const char *path, const char *text, char *err, size_t errlen);

/* Creates every missing level. Returns 0 when the directory exists afterwards. */
int katoshe_mkdir_all(const char *path);

/* Directory part of a path, without the trailing separator ("C:\a\b.txt" ->
   "C:\a"). Empty when the path has no separator. */
void katoshe_dirname(const char *path, char *out, size_t cap);

#endif
