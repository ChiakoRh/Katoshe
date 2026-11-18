#ifndef KATOSHE_ENV_H
#define KATOSHE_ENV_H

#include <stddef.h>

#include "config.h"

/* Settings that both front ends need to interpret the same way. Anything here
   used to live in the CLI, where the GUI could only have copied it. */

/* getenv, with an empty value treated as missing. Returns NULL when unset. */
const char *katoshe_env(const char *name);

/* Bounded copy that never leaves a partial character sequence, and accepts the
   NULL pointer. */
void katoshe_copy_str(char *dst, size_t cap, const char *src);

/* The token to use: the explicit one, then KATOSHE_GITHUB_TOKEN, then
   config/secrets.json. Always leaves `out` empty or usable. */
void katoshe_resolve_token(const char *explicit_token, const char *config_dir,
                           char *out, size_t cap);

/* The proxy for owner/repo: that repository's own setting, then KATOSHE_PROXY,
   then the global setting. NULL means "connect directly". */
const char *katoshe_resolve_proxy(const katoshe_config *cfg, const char *owner,
                                  const char *repo);

/* Where downloads land: an absolute configured path as-is, otherwise anchored
   to `base`. */
int katoshe_download_dir(const char *base, const char *configured, char *out, size_t cap);

#endif
