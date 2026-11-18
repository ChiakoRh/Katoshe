#include "secrets.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cjson/cJSON.h"
#include "paths.h"

int katoshe_token_from_file(const char *config_dir, char *out, size_t cap,
                            char *err, size_t errlen) {
    char path[600];
    char *text;
    long n;
    FILE *f;
    cJSON *doc;
    const cJSON *token;

    out[0] = '\0';
    if (katoshe_path_join(path, sizeof path, config_dir, "secrets.json") != 0) {
        snprintf(err, errlen, "secrets path too long");
        return -1;
    }
    f = fopen(path, "rb");
    if (!f) return 1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 1; }
    n = ftell(f);
    if (n <= 0) { fclose(f); return 1; }
    rewind(f);
    text = (char *)malloc((size_t)n + 1);
    if (!text) { fclose(f); return -1; }
    size_t got = fread(text, 1, (size_t)n, f);
    fclose(f);
    text[got] = '\0';

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
