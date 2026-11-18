#include "selftest.h"

#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "gh.h"
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

static int has(const char *path, const char *needle) {
    char *text = slurp(path);
    int found = text && strstr(text, needle) != NULL;
    free(text);
    return found;
}

/* settings.json: per machine. A preset never carries any of this. */
static const char *k_settings =
    "{\n"
    "  \"version\": 1,\n"
    "  \"preset\": \"default\",\n"
    "  \"future_settings\": \"keep me\",\n"
    "  \"global\": {\n"
    "    \"download_dir\": \"dl\",\n"
    "    \"proxy\": \"socks5h://127.0.0.1:1080\",\n"
    "    \"jobs\": 3,\n"
    "    \"future_global\": 7\n"
    "  }\n"
    "}\n";

/* presets/default.json: the shareable half. */
static const char *k_default_preset =
    "{\n"
    "  \"version\": 1,\n"
    "  \"future_field\": \"keep me\",\n"
    "  \"repositories\": [\n"
    "    {\n"
    "      \"name\": \"X\",\n"
    "      \"url\": \"https://github.com/owner/X\",\n"
    "      \"include_prerelease\": true,\n"
    "      \"future_repo\": true,\n"
    "      \"assets\": [\n"
    "        { \"match\": \"*-v8.apk\", \"mode\": \"archive\", \"keep_versions\": 5, \"future_asset\": [1, 2] },\n"
    "        { \"match\": \"X-win-64.exe\", \"filename\": \"X-win64.exe\" }\n"
    "      ]\n"
    "    }\n"
    "  ]\n"
    "}\n";

static const char *k_other_preset =
    "{\n"
    "  \"version\": 1,\n"
    "  \"repositories\": [\n"
    "    { \"name\": \"A\", \"url\": \"https://github.com/owner/A\", \"assets\": [ { \"match\": \"a.apk\" } ] },\n"
    "    { \"name\": \"B\", \"url\": \"https://github.com/owner/B\", \"assets\": [ { \"match\": \"b.apk\" } ] }\n"
    "  ]\n"
    "}\n";

static const char *k_minimal = "{ \"version\": 1 }\n";

typedef struct {
    char root[MAX_PATH];
    char config_dir[MAX_PATH];
    char presets_dir[MAX_PATH];
    char settings[MAX_PATH];
    char default_preset[MAX_PATH];
    char other_preset[MAX_PATH];
} sandbox;

static int sandbox_init(sandbox *sb, const char *suffix) {
    char tmp[MAX_PATH];
    if (GetTempPathA(sizeof tmp, tmp) == 0) return -1;
    snprintf(sb->root, sizeof sb->root, "%skatoshe-selftest-%lu%s", tmp,
             (unsigned long)GetCurrentProcessId(), suffix);
    katoshe_mkdir(sb->root);
    katoshe_path_join(sb->config_dir, sizeof sb->config_dir, sb->root, "config");
    katoshe_mkdir(sb->config_dir);
    katoshe_path_join(sb->presets_dir, sizeof sb->presets_dir, sb->config_dir, "presets");
    katoshe_mkdir(sb->presets_dir);
    katoshe_path_join(sb->settings, sizeof sb->settings, sb->config_dir, "settings.json");
    katoshe_path_join(sb->default_preset, sizeof sb->default_preset, sb->presets_dir, "default.json");
    katoshe_path_join(sb->other_preset, sizeof sb->other_preset, sb->presets_dir, "other.json");
    return 0;
}

static void sandbox_drop(const sandbox *sb) {
    DeleteFileA(sb->settings);
    DeleteFileA(sb->default_preset);
    DeleteFileA(sb->other_preset);
    RemoveDirectoryA(sb->presets_dir);
    RemoveDirectoryA(sb->config_dir);
    RemoveDirectoryA(sb->root);
}

int katoshe_selftest(void) {
    sandbox a, b;
    char err[512];
    katoshe_config cfg;

    failures = 0;

    if (sandbox_init(&a, "-src") != 0 || sandbox_init(&b, "-out") != 0) {
        printf("FAIL temp directory\n");
        return 1;
    }
    put(a.settings, k_settings);
    put(a.default_preset, k_default_preset);
    put(a.other_preset, k_other_preset);

    /* 0. URL forms users actually paste */
    {
        static const struct {
            const char *in;
            const char *owner;
            const char *repo;
        } urls[] = {
            {"https://github.com/owner/repo", "owner", "repo"},
            {"https://github.com/owner/repo.git", "owner", "repo"},
            {"https://github.com/owner/repo/", "owner", "repo"},
            {"http://www.github.com/owner/repo", "owner", "repo"},
            {"git@github.com:owner/repo.git", "owner", "repo"},
            {"owner/repo", "owner", "repo"},
            {"github.com/owner/repo", "owner", "repo"},
            {"https://api.github.com/repos/owner/repo", "owner", "repo"},
            {NULL, NULL, NULL},
        };
        static const char *const bad[] = {
            "https://gitlab.com/owner/repo",
            "https://github.com/owner/repo/releases/tag/v1",
            "https://github.com/onlyowner",
            "owner",
            "",
            "https://github.com/../repo",
            "https://github.com/owner/..",
            NULL,
        };
        for (int i = 0; urls[i].in; i++) {
            char owner[128], repo[128];
            int ok = gh_parse_url(urls[i].in, owner, sizeof owner, repo, sizeof repo) == 0 &&
                     strcmp(owner, urls[i].owner) == 0 && strcmp(repo, urls[i].repo) == 0;
            if (!ok) printf("     (url case %d: %s)\n", i, urls[i].in);
            check(ok, "url parses to owner/repo");
        }
        for (int i = 0; bad[i]; i++) {
            char owner[128], repo[128];
            int rejected = gh_parse_url(bad[i], owner, sizeof owner, repo, sizeof repo) != 0;
            if (!rejected) printf("     (url case %d: %s)\n", i, bad[i]);
            check(rejected, "bad url rejected");
        }
    }

    /* 1. every field parses, absent fields keep their default */
    check(katoshe_config_load(&cfg, a.settings, a.presets_dir, NULL, err, sizeof err) == 0,
          "load settings + default preset");
    if (cfg.settings_doc && cfg.preset_doc) {
        check(strcmp(cfg.preset, "default") == 0, "active preset from settings");
        check(cfg.repo_count == 1, "one repository");
        check(cfg.repos[0].asset_count == 2, "two assets");
        check(cfg.repos[0].assets[0].mode == KATOSHE_MODE_ARCHIVE, "archive mode parsed");
        check(cfg.repos[0].assets[0].keep_versions == 5, "keep_versions parsed");
        check(cfg.repos[0].assets[1].mode == KATOSHE_MODE_REPLACE, "replace is the default mode");
        check(strcmp(cfg.repos[0].assets[1].filename, "X-win64.exe") == 0, "filename parsed");
        check(cfg.repos[0].include_prerelease == 1, "repo prerelease true");
        check(cfg.global.jobs == 3, "jobs parsed");
        check(cfg.global.retries == 3, "retries default kept");
        check(cfg.global.check_on_launch == true, "check_on_launch default kept");
        check(strcmp(cfg.global.download_dir, "dl") == 0, "download_dir parsed");

        /* 2. save into a second sandbox, reload, compare */
        check(katoshe_config_save(&cfg, b.settings, b.presets_dir, err, sizeof err) == 0, "save");
        katoshe_config_free(&cfg);

        check(katoshe_config_load(&cfg, b.settings, b.presets_dir, NULL, err, sizeof err) == 0,
              "reload saved");
        check(cfg.repo_count == 1 && cfg.repos[0].asset_count == 2, "shape survived");
        check(cfg.repos[0].assets[0].keep_versions == 5, "keep_versions survived");
        check(cfg.global.jobs == 3, "jobs survived");

        /* 3. neither file may drop a key this tool does not own */
        check(has(b.settings, "future_settings"), "unknown settings key preserved");
        check(has(b.settings, "future_global"), "unknown global key preserved");
        check(has(b.default_preset, "future_field"), "unknown preset key preserved");
        check(has(b.default_preset, "future_repo"), "unknown repo key preserved");
        check(has(b.default_preset, "future_asset"), "unknown asset key preserved");

        /* 4. a per-repo override leaves the global setting alone */
        put(b.other_preset, k_other_preset);
        check(katoshe_config_load(&cfg, b.settings, b.presets_dir, "other", err, sizeof err) == 0,
              "--preset loads another file");
        check(cfg.repo_count == 2, "override preset has its own repositories");
        check(cfg.repo_count == 2 && strcmp(cfg.repos[0].name, "A") == 0, "override preset content");
        check(strcmp(cfg.preset, "other") == 0, "override name recorded");
        katoshe_config_free(&cfg);
    } else {
        printf("FAIL source documents did not load: %s\n", err);
        failures++;
    }
    katoshe_config_free(&cfg);

    /* 5. a preset with only a version is a valid, empty preset */
    put(a.other_preset, k_minimal);
    check(katoshe_config_load(&cfg, a.settings, a.presets_dir, "other", err, sizeof err) == 0,
          "minimal preset loads");
    check(cfg.repo_count == 0, "minimal preset has no repositories");
    check(cfg.global.jobs == 3, "settings still apply");
    katoshe_config_free(&cfg);
    put(a.other_preset, k_other_preset);

    /* 6. rejections name the offending place */
    put(a.other_preset, "{ \"version\": 1, \"repositories\": [ { \"url\": \"https://github.com/a/b\","
                        " \"assets\": [ { \"match\": \"a.apk\", \"mode\": \"bad\" } ] } ] }");
    check(katoshe_config_load(&cfg, a.settings, a.presets_dir, "other", err, sizeof err) != 0 &&
              strstr(err, "repositories[0].assets[0]") != NULL &&
              strstr(err, "presets\\other.json") != NULL,
          "bad mode rejected with file and path");
    katoshe_config_free(&cfg);

    put(a.other_preset, "{ \"version\": 1, \"repositories\": [ { \"url\": \"https://github.com/a/b\","
                        " \"assets\": [ { \"match\": \"a.apk\" }, { \"match\": \"a.apk\" } ] } ] }");
    check(katoshe_config_load(&cfg, a.settings, a.presets_dir, "other", err, sizeof err) != 0 &&
              strstr(err, "duplicate match") != NULL,
          "duplicate match rejected");
    katoshe_config_free(&cfg);

    /* 7. a preset is untrusted input: paths may not escape the download dir */
    put(a.other_preset, "{ \"version\": 1, \"repositories\": [ { \"url\": \"https://github.com/a/b\","
                        " \"assets\": [ { \"match\": \"a.apk\", \"dir\": \"..\\\\..\\\\Windows\" } ] } ] }");
    check(katoshe_config_load(&cfg, a.settings, a.presets_dir, "other", err, sizeof err) != 0 &&
              strstr(err, "relative") != NULL,
          "traversal in asset dir rejected");
    katoshe_config_free(&cfg);

    put(a.other_preset, "{ \"version\": 1, \"repositories\": [ { \"url\": \"https://github.com/a/b\","
                        " \"dir\": \"C:\\\\Windows\", \"assets\": [] } ] }");
    check(katoshe_config_load(&cfg, a.settings, a.presets_dir, "other", err, sizeof err) != 0,
          "absolute repo dir rejected");
    katoshe_config_free(&cfg);

    put(a.other_preset, k_other_preset);
    check(katoshe_config_load(&cfg, a.settings, a.presets_dir, "..\\\\evil", err, sizeof err) != 0,
          "traversal in preset name rejected");
    katoshe_config_free(&cfg);

    /* 8. version mismatch and what is in the file is what the error names */
    put(a.other_preset, "{ \"version\": 2 }");
    check(katoshe_config_load(&cfg, a.settings, a.presets_dir, "other", err, sizeof err) != 0 &&
              strstr(err, "version must be 1") != NULL,
          "wrong version rejected");
    katoshe_config_free(&cfg);

    /* 9. a missing preset file says which path it wanted */
    DeleteFileA(a.other_preset);
    check(katoshe_config_load(&cfg, a.settings, a.presets_dir, "ghost", err, sizeof err) != 0 &&
              strstr(err, "ghost.json") != NULL,
          "missing preset names the path");
    katoshe_config_free(&cfg);

    sandbox_drop(&a);
    sandbox_drop(&b);
    return failures;
}
