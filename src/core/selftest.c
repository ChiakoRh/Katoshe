#include "selftest.h"

#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "download.h"
#include "gh.h"
#include "io.h"
#include "match.h"
#include "paths.h"
#include "plan.h"
#include "preset.h"
#include "sha256.h"
#include "archive.h"
#include "state.h"

static int failures;

static void check(int cond, const char *name) {
    if (!cond) {
        printf("FAIL %s\n", name);
        failures++;
    }
}

static void copy_str_into(char *dst, size_t cap, const char *src) {
    size_t n = 0;
    while (n + 1 < cap && src[n]) n++;
    memcpy(dst, src, n);
    dst[n] = '\0';
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

    /* 10. wildcard matcher: the table from the design, including the traps */
    {
        static const struct {
            const char *pattern;
            const char *name;
            int hit;
        } cases[] = {
            {"*.apk", "app-v8.apk", 1},
            {"*.apk", "app-v8.apk.idsig", 0},
            {"app-*.zip", "app-win64.zip", 1},
            {"app-*.zip", "APP-win64.zip", 0},      /* case sensitive */
            {"app?.txt", "app1.txt", 1},
            {"app?.txt", "app12.txt", 0},
            {"[ab]x", "ax", 1},
            {"[ab]x", "cx", 0},
            {"[!ab]x", "cx", 1},
            {"[a-c]x", "bx", 1},
            {"*", "anything", 1},
            {"*", "", 1},
            {"a*b*c", "aXXbYYc", 1},
            {"a*b*c", "aXXbYY", 0},
            {"", "", 1},
            {"", "x", 0},
            {"[unterminated", "u", 0},
        };
        for (int i = 0; i < (int)(sizeof cases / sizeof *cases); i++) {
            int got = katoshe_match(cases[i].pattern, cases[i].name) ? 1 : 0;
            if (got != cases[i].hit) printf("     (match case %d: %s / %s)\n", i, cases[i].pattern,
                                            cases[i].name);
            check(got == cases[i].hit, "wildcard matches the table");
        }
    }

    /* 11. lockfile round trip */
    {
        katoshe_state st, back;
        char path[MAX_PATH];
        katoshe_state_repo *r;
        katoshe_state_asset *as;

        memset(&st, 0, sizeof st);
        r = katoshe_state_repo_get(&st, "owner/X", 1);
        check(r != NULL, "state repo created");
        if (r) {
            r->release_id = 777;
            copy_str_into(r->tag, sizeof r->tag, "v1.4.2");
            copy_str_into(r->etag, sizeof r->etag, "W/\"abc\"");
            as = katoshe_state_asset_get(r, "*.apk", 1);
            check(as != NULL, "state asset created");
            if (as) {
                as->asset_id = 999;
                as->size = 123456;
                copy_str_into(as->remote_name, sizeof as->remote_name, "X-1.4.2-v8.apk");
                copy_str_into(as->path, sizeof as->path, "downloads/X/X-app.apk");
                copy_str_into(as->digest, sizeof as->digest, "sha256:9f2c");
                copy_str_into(as->sha256, sizeof as->sha256, "9f2c");
            }
        }
        snprintf(path, sizeof path, "%s\\lockfile.json", a.root);
        check(katoshe_state_save(&st, path, err, sizeof err) == 0, "lockfile saved");
        katoshe_state_free(&st);

        check(katoshe_state_load(&back, path, err, sizeof err) == 0, "lockfile loaded");
        check(back.repo_count == 1, "lockfile keeps the repository");
        if (back.repo_count == 1) {
            r = &back.repos[0];
            check(strcmp(r->key, "owner/X") == 0, "lockfile keeps the key");
            check(r->release_id == 777, "lockfile keeps the release id");
            check(strcmp(r->etag, "W/\"abc\"") == 0, "lockfile keeps the etag");
            check(r->asset_count == 1, "lockfile keeps the asset");
            if (r->asset_count == 1) {
                as = &r->assets[0];
                check(strcmp(as->match, "*.apk") == 0, "lockfile keys on match, not the remote name");
                check(as->asset_id == 999 && as->size == 123456, "lockfile keeps id and size");
                check(strcmp(as->remote_name, "X-1.4.2-v8.apk") == 0, "lockfile keeps the remote name");
            }
        }
        katoshe_state_free(&back);

        /* a corrupt lockfile is an error, not a silent reset */
        put(path, "{ not json");
        check(katoshe_state_load(&back, path, err, sizeof err) != 0 &&
                  strstr(err, "delete it") != NULL,
              "corrupt lockfile reported");
        katoshe_state_free(&back);
    }

    /* 12. the decision table, against real files */
    {
        katoshe_repo repo;
        katoshe_state state;
        katoshe_state_repo *state_repo;
        katoshe_state_asset *state_asset;
        gh_release release;
        gh_asset remote[2];
        char download_dir[MAX_PATH];
        char local[MAX_PATH];
        plan p;

        snprintf(download_dir, sizeof download_dir, "%s\\dl", a.root);
        check(katoshe_mkdir_all(download_dir) == 0, "download dir created");

        memset(&repo, 0, sizeof repo);
        copy_str_into(repo.name, sizeof repo.name, "X");
        repo.assets = (katoshe_asset *)calloc(3, sizeof *repo.assets);
        repo.asset_count = 3;
        copy_str_into(repo.assets[0].match, sizeof repo.assets[0].match, "X-app.apk");
        copy_str_into(repo.assets[1].match, sizeof repo.assets[1].match, "*.zip");
        copy_str_into(repo.assets[2].match, sizeof repo.assets[2].match, "absent.zip");

        memset(&release, 0, sizeof release);
        release.id = 10;
        copy_str_into(release.tag, sizeof release.tag, "v1");

        memset(remote, 0, sizeof remote);
        remote[0].id = 100;
        remote[0].size = 5;
        copy_str_into(remote[0].name, sizeof remote[0].name, "X-app.apk");
        copy_str_into(remote[0].digest, sizeof remote[0].digest, "sha256:aa");
        remote[1].id = 101;
        remote[1].size = 7;
        copy_str_into(remote[1].name, sizeof remote[1].name, "X-win.zip");

        memset(&state, 0, sizeof state);
        state_repo = katoshe_state_repo_get(&state, "owner/X", 1);
        state_repo->release_id = 10;
        state_asset = katoshe_state_asset_get(state_repo, "X-app.apk", 1);
        state_asset->asset_id = 100;
        state_asset->size = 5;
        copy_str_into(state_asset->digest, sizeof state_asset->digest, "sha256:aa");

        snprintf(local, sizeof local, "%s\\X\\X-app.apk", download_dir);
        {
            char dir[MAX_PATH];
            katoshe_dirname(local, dir, sizeof dir);
            check(katoshe_mkdir_all(dir) == 0, "repo dir created");
        }
        put(local, "12345");

        check(katoshe_plan_build(&repo, download_dir, &release, remote, 2, state_repo, &p, err,
                                 sizeof err) == 0,
              "plan built");
        check(p.count == 3, "one plan item per selection");
        if (p.count == 3) {
            check(p.items[0].action == PLAN_UNCHANGED, "same release is unchanged");
            check(p.items[1].action == PLAN_DOWNLOAD, "absent file is a download");
            check(strcmp(p.items[1].remote_name, "X-win.zip") == 0, "wildcard picked the zip");
            check(p.items[2].action == PLAN_MISSING, "unmatched selection is missing");
        }
        katoshe_plan_free(&p);

        /* a size that disagrees with the lockfile means a damaged file */
        put(local, "123");
        check(katoshe_plan_build(&repo, download_dir, &release, remote, 2, state_repo, &p, err,
                                 sizeof err) == 0,
              "plan rebuilt");
        if (p.count == 3) check(p.items[0].action == PLAN_DOWNLOAD, "size mismatch downloads");
        katoshe_plan_free(&p);

        /* a digest change in a new release means the file must be fetched again */
        release.id = 11;
        copy_str_into(remote[0].digest, sizeof remote[0].digest, "sha256:bb");
        put(local, "12345");
        check(katoshe_plan_build(&repo, download_dir, &release, remote, 2, NULL, &p, err,
                                 sizeof err) == 0,
              "plan rebuilt without state");
        if (p.count == 3) check(p.items[0].action == PLAN_DOWNLOAD, "no record downloads");
        katoshe_plan_free(&p);

        free(repo.assets);
        katoshe_state_free(&state);
    }

    printf("\n-- archive --\n");
    failures += katoshe_archive_selftest();
    printf("\n-- download --\n");
    failures += katoshe_download_selftest();
    printf("\n-- sha256 --\n");
    failures += katoshe_sha256_selftest();
    printf("\n-- presets --\n");
    failures += katoshe_preset_selftest();

    sandbox_drop(&a);
    sandbox_drop(&b);
    return failures;
}