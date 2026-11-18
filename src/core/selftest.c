#include "selftest.h"

#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "paths.h"

static int failures;

static void check(int cond, const char *name) {
    if (!cond) {
        printf("FAIL %s\n", name);
        failures++;
    }
}

static void put(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(text, 1, strlen(text), f);
    fclose(f);
}

static char *slurp(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f);
    buf[got] = '\0';
    fclose(f);
    return buf;
}

static const char *k_source =
    "{\n"
    "  \"version\": 1,\n"
    "  \"future_field\": \"keep me\",\n"
    "  \"global\": {\n"
    "    \"download_dir\": \"dl\",\n"
    "    \"proxy\": \"socks5h://127.0.0.1:1080\",\n"
    "    \"jobs\": 3,\n"
    "    \"future_global\": 7\n"
    "  },\n"
    "  \"repositories\": [\n"
    "    {\n"
    "      \"name\": \"X\",\n"
    "      \"url\": \"https://github.com/owner/X\",\n"
    "      \"future_repo\": true,\n"
    "      \"assets\": [\n"
    "        { \"match\": \"*-v8.apk\", \"mode\": \"archive\", \"keep_versions\": 5, \"future_asset\": [1, 2] },\n"
    "        { \"match\": \"X-win-64.exe\", \"filename\": \"X-win64.exe\" }\n"
    "      ]\n"
    "    }\n"
    "  ]\n"
    "}\n";

static const char *k_minimal = "{ \"version\": 1 }\n";

int katoshe_selftest(void) {
    char dir[MAX_PATH];
    char src[600], mid[600], out[600];
    char err[512];
    katoshe_config cfg;

    failures = 0;

    if (GetTempPathA(sizeof dir, dir) == 0) {
        printf("FAIL GetTempPath\n");
        return 1;
    }
    snprintf(src, sizeof src, "%skatoshe-selftest-%lu-src.json", dir,
             (unsigned long)GetCurrentProcessId());
    snprintf(mid, sizeof mid, "%skatoshe-selftest-%lu-mid.json", dir,
             (unsigned long)GetCurrentProcessId());
    snprintf(out, sizeof out, "%skatoshe-selftest-%lu-out.json", dir,
             (unsigned long)GetCurrentProcessId());

    /* 1. parse every field and the defaults that are absent */
    put(src, k_source);
    check(katoshe_config_load(&cfg, src, err, sizeof err) == 0, "load source");
    if (cfg.doc) {
        check(cfg.repo_count == 1, "one repository");
        check(cfg.repos[0].asset_count == 2, "two assets");
        check(cfg.repos[0].assets[0].mode == KATOSHE_MODE_ARCHIVE, "archive mode parsed");
        check(cfg.repos[0].assets[0].keep_versions == 5, "keep_versions parsed");
        check(cfg.repos[0].assets[1].mode == KATOSHE_MODE_REPLACE, "replace is the default mode");
        check(strcmp(cfg.repos[0].assets[1].filename, "X-win64.exe") == 0, "filename parsed");
        check(cfg.global.jobs == 3, "jobs parsed");
        check(cfg.global.retries == 3, "retries default kept");
        check(cfg.global.check_on_launch == true, "check_on_launch default kept");
        check(strcmp(cfg.global.download_dir, "dl") == 0, "download_dir parsed");

        /* 2. save, then read back: values survive and unknown keys are not dropped */
        check(katoshe_config_save(&cfg, mid, err, sizeof err) == 0, "save");
        katoshe_config_free(&cfg);

        check(katoshe_config_load(&cfg, mid, err, sizeof err) == 0, "reload saved");
        check(cfg.repo_count == 1 && cfg.repos[0].asset_count == 2, "shape survived");
        check(cfg.repos[0].assets[0].keep_versions == 5, "keep_versions survived");
        check(cfg.global.jobs == 3, "jobs survived");
        katoshe_config_free(&cfg);

        char *text = slurp(mid);
        check(text != NULL, "read saved text");
        if (text) {
            check(strstr(text, "future_field") != NULL, "unknown top-level key preserved");
            check(strstr(text, "future_global") != NULL, "unknown global key preserved");
            check(strstr(text, "future_repo") != NULL, "unknown repo key preserved");
            check(strstr(text, "future_asset") != NULL, "unknown asset key preserved");
            free(text);
        }
    }

    /* 3. defaults when the document carries only a version */
    put(src, k_minimal);
    check(katoshe_config_load(&cfg, src, err, sizeof err) == 0, "load minimal");
    check(cfg.global.jobs == 2, "default jobs");
    check(strcmp(cfg.global.proxy, "socks5h://127.0.0.1:10808") == 0, "default proxy");
    katoshe_config_free(&cfg);

    /* 4. rejections, each with the JSON path in the message */
    put(src, "{ \"version\": 1, \"repositories\": [ { \"url\": \"https://github.com/a/b\","
              " \"assets\": [ { \"match\": \"a.apk\", \"mode\": \"bad\" } ] } ] }");
    check(katoshe_config_load(&cfg, src, err, sizeof err) != 0 &&
              strstr(err, "repositories[0].assets[0]") != NULL,
          "bad mode rejected with path");
    katoshe_config_free(&cfg);

    put(src, "{ \"version\": 1, \"repositories\": [ { \"url\": \"https://github.com/a/b\","
              " \"assets\": [ { \"match\": \"a.apk\" }, { \"match\": \"a.apk\" } ] } ] }");
    check(katoshe_config_load(&cfg, src, err, sizeof err) != 0 &&
              strstr(err, "duplicate match") != NULL,
          "duplicate match rejected");
    katoshe_config_free(&cfg);

    put(src, "{ \"version\": 2 }");
    check(katoshe_config_load(&cfg, src, err, sizeof err) != 0, "wrong version rejected");
    katoshe_config_free(&cfg);

    put(src, "{ \"version\": 1, \"repositories\": [ { \"name\": \"a\" } ] }");
    check(katoshe_config_load(&cfg, src, err, sizeof err) != 0, "missing url rejected");
    katoshe_config_free(&cfg);

    DeleteFileA(src);
    DeleteFileA(mid);
    DeleteFileA(out);
    return failures;
}
