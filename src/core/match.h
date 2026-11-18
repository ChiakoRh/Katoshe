#ifndef KATOSHE_MATCH_H
#define KATOSHE_MATCH_H

#include <stdbool.h>

/* Git-style glob over a file name: '*' any run, '?' one character, '[abc]' and
   '[a-z]' sets, '[!abc]' negated. Case sensitive; no path semantics. */
bool katoshe_match(const char *pattern, const char *name);

#endif
