#ifndef KATOSHE_SECRETS_H
#define KATOSHE_SECRETS_H

#include <stddef.h>

/* Reads `config/secrets.json` and copies its "token" into out.
   0 = found, 1 = no file or no token, -1 = the file exists but is unusable. */
int katoshe_token_from_file(const char *config_dir, char *out, size_t cap,
                            char *err, size_t errlen);

#endif
