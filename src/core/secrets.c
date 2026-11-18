#include "secrets.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cjson/cJSON.h"
#include "io.h"
#include "paths.h"

int katoshe_token_from_file(const char *config_dir, char *out, size_t cap,
                            char *err, size_t errlen) {
    char path[600];
    char *text;
    cJSON *doc;
    const cJSON *token;

    out[0] = '\0';
    if (katoshe_path_join(path, sizeof path, config_dir, "secrets.json") != 0) {
        snprintf(err, errlen, "secrets path too long");
        return -1;
    }
    text = katoshe_read_file(path);
    if (!text) return 1;

    doc = cJSON_Parse(text);
    free(text);
    if (!doc) {
        snprintf(err, errlen, "%s is not valid JSON", path);
        return -1;
    }
    token = cJSON_GetObjectItemCaseSensitive(doc, "token");
    if (!cJSON_IsString(token) || !token->valuestring || !token->valuestring[0]) {
        token = cJSON_GetObjectItemCaseSensitive(doc, "github_token"); /* older spelling */
    }
    if (!cJSON_IsString(token) || !token->valuestring || !token->valuestring[0]) {
        cJSON_Delete(doc);
        return 1;
    }
    snprintf(out, cap, "%s", token->valuestring);
    cJSON_Delete(doc);
    return 0;
}
