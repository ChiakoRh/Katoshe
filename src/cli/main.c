#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "archive.h"
#include "config.h"
#include "download.h"
#include "gh.h"
#include "io.h"
#include "katoshe.h"
#include "paths.h"
#include "plan.h"
#include "preset.h"
#include "secrets.h"
#include "selftest.h"
#include "state.h"

static void usage(void) {
    printf("katoshe %s - GitHub release asset manager\n\n", katoshe_version());
    printf("usage: katoshe-cli <command> [options]\n\n");
    printf("  check   [repo...]   report what would change; writes nothing\n");
    printf("  update  [repo...]   download and update selected assets\n");
    printf("  list    <owner/repo> show the target release and its assets\n");
    printf("  validate            parse and validate the configuration\n");
    printf("  add     <url>       add a repository to the active preset\n");
    printf("  preset  list        show the presets, * marks the active one\n");
    printf("  preset  new <name>  create an empty preset\n");
    printf("  preset  use <name>  make a preset active\n");
    printf("  preset  export <name> [file]  write the preset file or print a katoshe:// link\n");
    printf("  preset  import <src>          from a path, a URL or a katoshe:// link\n\n");
    printf("  --root <dir>        use <dir> instead of the exe folder or %%APPDATA%%\n");
    printf("  --preset <name>     use this preset for this run only\n");
    printf("  --token <token>     GitHub token, overrides the environment and secrets.json\n");
    printf("  --version           print the version and exit\n");
    printf("  --help              print this text and exit\n");
}

typedef struct {
    const char *root;
    const char *preset;
    const char *token;
} cli_options;

static cli_options take_options(int argc, char **argv, const char **rest, int *rest_count) {
    cli_options opt = {NULL, NULL, NULL};
    *rest_count = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--root") == 0 && i + 1 < argc) {
            opt.root = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--preset") == 0 && i + 1 < argc) {
            opt.preset = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--token") == 0 && i + 1 < argc) {
            opt.token = argv[++i];
            continue;
        }
        rest[(*rest_count)++] = argv[i];
    }
    return opt;
}

static const char *env_or_null(const char *name) {
    const char *v = getenv(name);
    return (v && v[0]) ? v : NULL;
}

static void copy_str(char *dst, size_t cap, const char *src) {    size_t n = 0;
    if (!src) { if (cap) dst[0] = '\0'; return; }
    while (n + 1 < cap && src[n]) n++;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* --token, then KATOSHE_GITHUB_TOKEN, then config/secrets.json. */
static void resolve_token(const cli_options *opt, const char *config_dir,
                          char *out, size_t cap) {
    char err[256];
    const char *env = env_or_null("KATOSHE_GITHUB_TOKEN");

    out[0] = '\0';
    if (opt->token && opt->token[0]) {
        copy_str(out, cap, opt->token);
        return;
    }
    if (env) {
        copy_str(out, cap, env);
        return;
    }
    if (katoshe_token_from_file(config_dir, out, cap, err, sizeof err) != 0) out[0] = '\0';
}

/* Per-repository setting, then KATOSHE_PROXY, then the global setting. */
static const char *resolve_proxy(const katoshe_config *cfg, const char *owner,
                                 const char *repo) {
    const char *env = env_or_null("KATOSHE_PROXY");

    if (cfg) {
        for (int i = 0; i < cfg->repo_count; i++) {
            const katoshe_repo *r = &cfg->repos[i];
            char ro[128], rr[128];
            if (r->proxy[0] && gh_parse_url(r->url, ro, sizeof ro, rr, sizeof rr) == 0 &&
                strcmp(ro, owner) == 0 && strcmp(rr, repo) == 0) {
                return r->proxy;
            }
        }
    }
    if (env) return env;
    if (cfg && cfg->global.proxy[0]) return cfg->global.proxy;
    return NULL;
}

static int is_absolute_path(const char *path) {
    if (!path[0]) return 0;
    if (path[0] == '\\' || path[0] == '/') return 1;
    return path[1] == ':';
}

/* The configured download directory, anchored to the base directory unless the
   user gave an absolute path. */
static int resolve_download_dir(const char *base, const char *configured, char *out, size_t cap) {
    if (is_absolute_path(configured)) {
        return snprintf(out, cap, "%s", configured) < (int)cap ? 0 : -1;
    }
    return katoshe_path_join(out, cap, base, configured);
}

static int filter_ok(const katoshe_repo *r, const char *const *args, int arg_count) {
    char owner[128], repo[128], key[300];
    int parsed;

    if (arg_count == 0) return 1;
    parsed = gh_parse_url(r->url, owner, sizeof owner, repo, sizeof repo) == 0;
    if (parsed) snprintf(key, sizeof key, "%s/%s", owner, repo);

    for (int i = 0; i < arg_count; i++) {
        if (strcmp(args[i], r->name) == 0) return 1;
        if (parsed && strcmp(args[i], key) == 0) return 1;
        if (strcmp(args[i], r->url) == 0) return 1;
    }
    return 0;
}

static int cmd_validate(const cli_options *opt) {
    katoshe_paths paths;
    katoshe_config cfg;
    char err[512];
    char settings_path[600];
    char summary[512];

    if (katoshe_paths_init(&paths, opt->root, err, sizeof err) != 0) {
        fprintf(stderr, "error: %s\n", err);
        return 2;
    }
    if (katoshe_path_join(settings_path, sizeof settings_path, paths.config_dir, "settings.json") != 0) {
        fprintf(stderr, "error: config path too long\n");
        return 2;
    }
    if (katoshe_config_load(&cfg, settings_path, paths.presets_dir, opt->preset, err, sizeof err) != 0) {
        fprintf(stderr, "error: %s\n", err);
        if (GetFileAttributesA(settings_path) == INVALID_FILE_ATTRIBUTES) {
            fprintf(stderr, "hint: copy config\\settings.example.json to %s\n", settings_path);
            fprintf(stderr, "      and config\\presets\\default.example.json to %s\\default.json\n",
                    paths.presets_dir);
        } else if (strstr(err, "cannot read") != NULL) {
            fprintf(stderr, "hint: a missing preset starts empty; copy "
                            "config\\presets\\default.example.json into %s\n",
                    paths.presets_dir);
        }
        return 2;
    }

    katoshe_config_summary(&cfg, summary, sizeof summary);
    printf("ok   %s\n", settings_path);
    printf("     base %s (%s)\n", paths.base, paths.portable ? "portable" : "appdata");
    printf("     %s\n", summary);
    katoshe_config_free(&cfg);
    return 0;
}

static int cmd_list(const cli_options *opt, const char *target) {
    katoshe_paths paths;
    katoshe_config cfg;
    char err[512];
    char owner[128];
    char repo[128];
    char token[256];
    char settings_path[600];
    char size_text[32];
    gh_options gh_opt;
    gh_meta meta;
    gh_release release;
    gh_asset *assets = NULL;
    int asset_count = 0;
    int have_config = 0;
    int rc;

    if (gh_parse_url(target, owner, sizeof owner, repo, sizeof repo) != 0) {
        fprintf(stderr, "error: '%s' is not a GitHub repository\n", target);
        fprintf(stderr, "hint: use owner/repo or https://github.com/owner/repo\n");
        return 2;
    }
    if (katoshe_paths_init(&paths, opt->root, err, sizeof err) != 0) {
        fprintf(stderr, "error: %s\n", err);
        return 2;
    }

    katoshe_path_join(settings_path, sizeof settings_path, paths.config_dir, "settings.json");
    if (opt->preset && opt->preset[0] && !katoshe_preset_name_ok(opt->preset)) {
        fprintf(stderr, "error: '%s' is not a valid preset name\n", opt->preset);
        return 2;
    }
    if (katoshe_config_load(&cfg, settings_path, paths.presets_dir, opt->preset, err,
                            sizeof err) == 0) {
        have_config = 1;
    } else {
        fprintf(stderr, "note: no usable configuration (%s)\n", err);
        katoshe_config_defaults(&cfg);
    }

    resolve_token(opt, paths.config_dir, token, sizeof token);
    memset(&gh_opt, 0, sizeof gh_opt);
    gh_opt.proxy = resolve_proxy(have_config ? &cfg : NULL, owner, repo);
    gh_opt.token = token[0] ? token : NULL;
    gh_opt.timeout_sec = have_config ? cfg.global.timeout_sec : 60;

    rc = gh_fetch_release(owner, repo, have_config ? cfg.global.include_prerelease : false,
                          &gh_opt, &meta, &release, &assets, &asset_count, err, sizeof err);
    if (rc != GH_OK) {
        fprintf(stderr, "error: %s\n", err);
        katoshe_config_free(&cfg);
        return 1;
    }

    printf("%s/%s -> %s", owner, repo, release.tag);
    if (release.name[0] && strcmp(release.name, release.tag) != 0) printf("  %s", release.name);
    if (release.prerelease) printf("  [prerelease]");
    if (release.published_at[0]) printf("  %s", release.published_at);
    printf("\n");
    for (int i = 0; i < asset_count; i++) {
        gh_format_size(assets[i].size, size_text, sizeof size_text);
        printf("  %10s  %s%s%s\n", size_text, assets[i].name,
               assets[i].digest[0] ? "  " : "", assets[i].digest);
    }
    if (asset_count == 0) printf("  (no assets on this release)\n");

    gh_release_free(assets);
    katoshe_config_free(&cfg);
    return 0;
}

/* Shared by check and update: load config and state, then walk the repositories
   the arguments select. */
typedef struct {
    katoshe_paths paths;
    katoshe_config cfg;
    katoshe_state state;
    char lockfile_path[600];
    char download_dir[KATOSHE_PATH_MAX];
    char token[256];
} run_context;

static int run_begin(const cli_options *opt, run_context *ctx, char *err, size_t errlen) {
    char settings_path[600];

    if (katoshe_paths_init(&ctx->paths, opt->root, err, errlen) != 0) return -1;
    if (katoshe_path_join(settings_path, sizeof settings_path, ctx->paths.config_dir,
                          "settings.json") != 0) {
        snprintf(err, errlen, "config path too long");
        return -1;
    }
    if (katoshe_config_load(&ctx->cfg, settings_path, ctx->paths.presets_dir, opt->preset, err,
                            errlen) != 0) {
        return -1;
    }
    if (katoshe_path_join(ctx->lockfile_path, sizeof ctx->lockfile_path, ctx->paths.data_dir,
                          "lockfile.json") != 0) {
        snprintf(err, errlen, "lockfile path too long");
        return -1;
    }
    if (katoshe_state_load(&ctx->state, ctx->lockfile_path, err, errlen) != 0) return -1;
    if (resolve_download_dir(ctx->paths.base, ctx->cfg.global.download_dir, ctx->download_dir,
                             sizeof ctx->download_dir) != 0) {
        snprintf(err, errlen, "download directory path too long");
        return -1;
    }
    resolve_token(opt, ctx->paths.config_dir, ctx->token, sizeof ctx->token);
    return 0;
}

static void run_end(run_context *ctx) {
    katoshe_state_free(&ctx->state);
    katoshe_config_free(&ctx->cfg);
}

typedef struct {
    int download;
    int unchanged;
    int missing;
    int failed;
} run_totals;

static void print_item(const plan_item *item) {
    char size_text[32];
    gh_format_size(item->size, size_text, sizeof size_text);
    printf("  %-9s %-44s %10s  %s\n", katoshe_plan_action_name(item->action),
           item->action == PLAN_MISSING ? item->match : item->remote_name,
           item->action == PLAN_MISSING ? "-" : size_text, item->reason);
}

static int cmd_check(const cli_options *opt, const char *const *args, int arg_count) {
    run_context ctx;
    char err[512];
    run_totals totals = {0, 0, 0, 0};

    if (run_begin(opt, &ctx, err, sizeof err) != 0) {
        fprintf(stderr, "error: %s\n", err);
        return 2;
    }

    for (int i = 0; i < ctx.cfg.repo_count; i++) {
        const katoshe_repo *repo = &ctx.cfg.repos[i];
        char owner[128], repo_name[128], key[300];
        katoshe_state_repo *state_repo;
        gh_options gh_opt;
        gh_meta meta;
        gh_release release;
        gh_asset *assets = NULL;
        int asset_count = 0;
        int prerelease;
        plan p;
        int rc;

        if (!filter_ok(repo, args, arg_count)) continue;
        if (gh_parse_url(repo->url, owner, sizeof owner, repo_name, sizeof repo_name) != 0) {
            fprintf(stderr, "error: %s: url is not a GitHub repository\n", repo->name);
            totals.failed++;
            continue;
        }
        snprintf(key, sizeof key, "%s/%s", owner, repo_name);
        state_repo = katoshe_state_repo_get(&ctx.state, key, 0);

        memset(&gh_opt, 0, sizeof gh_opt);
        gh_opt.proxy = resolve_proxy(&ctx.cfg, owner, repo_name);
        gh_opt.token = ctx.token[0] ? ctx.token : NULL;
        gh_opt.timeout_sec = ctx.cfg.global.timeout_sec;
        gh_opt.etag = (state_repo && state_repo->etag[0]) ? state_repo->etag : NULL;
        prerelease = repo->include_prerelease >= 0 ? repo->include_prerelease
                                                   : ctx.cfg.global.include_prerelease;

        rc = gh_fetch_release(owner, repo_name, prerelease != 0, &gh_opt, &meta, &release, &assets,
                              &asset_count, err, sizeof err);
        if (rc == GH_NOT_MODIFIED) {
            printf("%s  %s  (not modified since the last check)\n", key,
                   state_repo && state_repo->tag[0] ? state_repo->tag : "?");
            continue;
        }
        if (rc != GH_OK) {
            fprintf(stderr, "error: %s: %s\n", repo->name, err);
            totals.failed++;
            continue;
        }

        printf("%s  %s%s\n", key, release.tag, release.prerelease ? "  [prerelease]" : "");
        if (katoshe_plan_build(repo, ctx.download_dir, &release, assets, asset_count, state_repo,
                               &p, err, sizeof err) != 0) {
            fprintf(stderr, "error: %s: %s\n", repo->name, err);
            totals.failed++;
            gh_release_free(assets);
            continue;
        }
        for (int j = 0; j < p.count; j++) {
            print_item(&p.items[j]);
            if (p.items[j].action == PLAN_DOWNLOAD) totals.download++;
            else if (p.items[j].action == PLAN_UNCHANGED) totals.unchanged++;
            else totals.missing++;
        }
        katoshe_plan_free(&p);
        gh_release_free(assets);
    }

    printf("\n%d to download, %d unchanged, %d missing", totals.download, totals.unchanged,
           totals.missing);
    if (totals.failed) printf(", %d repositories failed", totals.failed);
    printf("\n");

    run_end(&ctx);
    return totals.failed ? 1 : 0;
}

static int mkdir_for(const char *path) {
    char dir[KATOSHE_PATH_MAX];
    katoshe_dirname(path, dir, sizeof dir);
    return dir[0] ? katoshe_mkdir_all(dir) : 0;
}

/* "sha256:<hex>" as GitHub reports it, or the hex alone. */
static void digest_hex(const char *digest, char *out, size_t cap) {
    const char *p = digest;
    out[0] = '\0';
    if (_strnicmp(p, "sha256:", 7) == 0) p += 7;
    if (strlen(p) != 64) return;
    copy_str(out, cap, p);
}

typedef struct {
    const char *name;
    long long received;
} progress_ctx;

static void on_progress(void *user, long long received, long long total) {
    progress_ctx *p = (progress_ctx *)user;
    static const char spinner[] = "|/-\\";
    static int tick;
    static int checked;
    static int console = 1;

    if (!checked) {
        DWORD mode;
        console = GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &mode) ? 1 : 0;
        checked = 1;
    }
    /* Carriage returns only overwrite a real terminal; a log file would keep
       every frame. There the "done" line is the progress. */
    if (!console) return;

    if (total <= 0) {
        printf("\r    %s  %lld KB", p->name, received / 1024);
    } else {
        printf("\r    %s  %3d%%  %lld / %lld KB  %c", p->name,
               (int)(received * 100 / total), received / 1024, total / 1024,
               spinner[tick++ & 3]);
    }
    fflush(stdout);
}

static void now_iso(char *out, size_t cap) {
    SYSTEMTIME t;
    GetSystemTime(&t);
    snprintf(out, cap, "%04u-%02u-%02uT%02u:%02u:%02uZ", t.wYear, t.wMonth, t.wDay, t.wHour,
             t.wMinute, t.wSecond);
}

static int cmd_update(const cli_options *opt, const char *const *args, int arg_count) {
    run_context ctx;
    char err[512];
    int failed = 0, downloaded = 0, skipped = 0, missing = 0;
    int touched = 0;

    if (run_begin(opt, &ctx, err, sizeof err) != 0) {
        fprintf(stderr, "error: %s\n", err);
        return 2;
    }

    for (int i = 0; i < ctx.cfg.repo_count; i++) {
        const katoshe_repo *repo = &ctx.cfg.repos[i];
        char owner[128], repo_name[128], key[300];
        katoshe_state_repo *state_repo;
        gh_options gh_opt;
        gh_meta meta;
        gh_release release;
        gh_asset *assets = NULL;
        int asset_count = 0;
        plan p;

        if (!filter_ok(repo, args, arg_count)) continue;
        if (gh_parse_url(repo->url, owner, sizeof owner, repo_name, sizeof repo_name) != 0) {
            fprintf(stderr, "error: %s: url is not a GitHub repository\n", repo->name);
            failed++;
            continue;
        }
        snprintf(key, sizeof key, "%s/%s", owner, repo_name);
        state_repo = katoshe_state_repo_get(&ctx.state, key, 1);
        if (!state_repo) {
            fprintf(stderr, "error: %s: out of memory\n", repo->name);
            failed++;
            continue;
        }

        memset(&gh_opt, 0, sizeof gh_opt);
        gh_opt.proxy = resolve_proxy(&ctx.cfg, owner, repo_name);
        gh_opt.token = ctx.token[0] ? ctx.token : NULL;
        gh_opt.timeout_sec = ctx.cfg.global.timeout_sec;
        /* No ETag here: update needs the asset list even when the release did
           not change, so it always asks for the body. */

        if (gh_fetch_release(owner, repo_name,
                             repo->include_prerelease >= 0
                                 ? repo->include_prerelease != 0
                                 : ctx.cfg.global.include_prerelease,
                             &gh_opt, &meta, &release, &assets, &asset_count, err,
                             sizeof err) != GH_OK) {
            fprintf(stderr, "error: %s: %s\n", repo->name, err);
            failed++;
            continue;
        }

        printf("%s  %s%s\n", key, release.tag, release.prerelease ? "  [prerelease]" : "");
        if (katoshe_plan_build(repo, ctx.download_dir, &release, assets, asset_count, state_repo,
                               &p, err, sizeof err) != 0) {
            fprintf(stderr, "error: %s: %s\n", repo->name, err);
            failed++;
            gh_release_free(assets);
            continue;
        }

        for (int j = 0; j < p.count; j++) {
            plan_item *item = &p.items[j];
            katoshe_download_opts dl;
            katoshe_download_result res;
            char expected[72];
            char size_text[32];
            progress_ctx pc;

            if (item->action == PLAN_UNCHANGED) {
                skipped++;
                print_item(item);
                continue;
            }
            if (item->action == PLAN_MISSING) {
                missing++;
                print_item(item);
                continue;
            }

            gh_format_size(item->size, size_text, sizeof size_text);
            printf("  download  %-44s %10s\n", item->remote_name, size_text);
            if (mkdir_for(item->path) != 0) {
                fprintf(stderr, "error: %s: cannot create the directory for %s\n", repo->name,
                        item->path);
                failed++;
                continue;
            }

            memset(&dl, 0, sizeof dl);
            dl.proxy = resolve_proxy(&ctx.cfg, owner, repo_name);
            dl.token = ctx.token[0] ? ctx.token : NULL;
            dl.timeout_sec = ctx.cfg.global.timeout_sec;
            dl.retries = ctx.cfg.global.retries;
            dl.progress = on_progress;
            dl.user = &pc;
            pc.name = item->remote_name;
            pc.received = 0;
            digest_hex(item->digest, expected, sizeof expected);

            if (katoshe_download(item->url, item->path, expected[0] ? expected : NULL, &dl, &res,
                                 err, sizeof err) != 0) {
                printf("\n");
                fprintf(stderr, "error: %s: %s\n", repo->name, err);
                failed++;
                continue;
            }
            printf("\r    %-58s %s\n", item->remote_name, res.resumed ? "resumed" : "done");
            downloaded++;

            /* Archive mode: old versions pile up, keep the newest N. Item j is
               selection j — plan_build emits one item per asset, in order. */
            if (repo->assets[j].mode == KATOSHE_MODE_ARCHIVE) {
                char dir[KATOSHE_PATH_MAX];
                int pruned;
                katoshe_dirname(item->path, dir, sizeof dir);
                pruned = katoshe_archive_prune(dir, item->match, repo->assets[j].keep_versions,
                                               err, sizeof err);
                if (pruned < 0) {
                    fprintf(stderr, "warning: %s: %s\n", repo->name, err);
                } else if (pruned > 0) {
                    printf("    pruned %d older version%s\n", pruned, pruned == 1 ? "" : "s");
                }
            }

            {
                katoshe_state_asset *sa = katoshe_state_asset_get(state_repo, item->match, 1);
                if (sa) {
                    sa->asset_id = item->asset_id;
                    sa->size = res.bytes;
                    copy_str(sa->remote_name, sizeof sa->remote_name, item->remote_name);
                    copy_str(sa->path, sizeof sa->path, item->path);
                    copy_str(sa->digest, sizeof sa->digest, item->digest);
                    copy_str(sa->sha256, sizeof sa->sha256, res.sha256);
                    touched = 1;
                }
            }
        }

        state_repo->release_id = release.id;
        copy_str(state_repo->tag, sizeof state_repo->tag, release.tag);
        copy_str(state_repo->etag, sizeof state_repo->etag, meta.etag);
        now_iso(state_repo->checked_at, sizeof state_repo->checked_at);
        touched = 1;

        katoshe_plan_free(&p);
        gh_release_free(assets);
    }

    if (touched && katoshe_state_save(&ctx.state, ctx.lockfile_path, err, sizeof err) != 0) {
        fprintf(stderr, "error: %s\n", err);
        failed++;
    }

    printf("\n%d downloaded, %d unchanged, %d missing", downloaded, skipped, missing);
    if (failed) printf(", %d failed", failed);
    printf("\n");

    run_end(&ctx);
    return failed ? 1 : 0;
}

static int cmd_preset(const cli_options *opt, const char *const *args, int arg_count) {
    katoshe_paths paths;
    char err[512];

    if (katoshe_paths_init(&paths, opt->root, err, sizeof err) != 0) {
        fprintf(stderr, "error: %s\n", err);
        return 2;
    }
    if (arg_count == 0) {
        fprintf(stderr, "error: preset needs list, new, use, export or import\n");
        return 2;
    }

    if (strcmp(args[0], "list") == 0) {
        katoshe_preset_info *list = NULL;
        int count = 0;
        char active[KATOSHE_PRESET_MAX] = "";
        char settings_path[600];
        katoshe_config cfg;

        katoshe_path_join(settings_path, sizeof settings_path, paths.config_dir, "settings.json");
        if (katoshe_config_load(&cfg, settings_path, paths.presets_dir, NULL, err, sizeof err) ==
            0) {
            copy_str(active, sizeof active, cfg.preset);
            katoshe_config_free(&cfg);
        }
        if (katoshe_preset_list(paths.presets_dir, &list, &count, err, sizeof err) != 0) {
            fprintf(stderr, "error: %s\n", err);
            return 1;
        }
        if (count == 0) {
            printf("%s\n", err[0] ? err : "no presets");
        }
        for (int i = 0; i < count; i++) {
            printf("%s %-24s %2d repositories, %3d assets\n",
                   strcmp(list[i].name, active) == 0 ? "*" : " ", list[i].name,
                   list[i].repo_count, list[i].asset_count);
        }
        katoshe_preset_list_free(list);
        return 0;
    }

    if (strcmp(args[0], "new") == 0 || strcmp(args[0], "use") == 0) {
        char settings_path[600];
        char preset_path[600];
        katoshe_config cfg;

        if (arg_count < 2) {
            fprintf(stderr, "error: preset %s needs a name\n", args[0]);
            return 2;
        }
        if (!katoshe_preset_name_ok(args[1])) {
            fprintf(stderr, "error: '%s' is not a valid preset name\n", args[1]);
            return 2;
        }
        katoshe_path_join(settings_path, sizeof settings_path, paths.config_dir, "settings.json");
        if (katoshe_path_join(preset_path, sizeof preset_path, paths.presets_dir, args[1]) != 0) {
            fprintf(stderr, "error: preset path too long\n");
            return 2;
        }
        strncat(preset_path, ".json", sizeof preset_path - strlen(preset_path) - 1);

        if (strcmp(args[0], "new") == 0) {
            if (GetFileAttributesA(preset_path) != INVALID_FILE_ATTRIBUTES) {
                fprintf(stderr, "error: preset '%s' already exists\n", args[1]);
                return 1;
            }
            katoshe_config_defaults(&cfg);
            copy_str(cfg.preset, sizeof cfg.preset, args[1]);
            if (katoshe_preset_write(&cfg, preset_path, err, sizeof err) != 0) {
                fprintf(stderr, "error: %s\n", err);
                return 1;
            }
            katoshe_config_free(&cfg);
            printf("created %s\n", preset_path);
            printf("activate with: preset use %s\n", args[1]);
            return 0;
        }

        if (GetFileAttributesA(preset_path) == INVALID_FILE_ATTRIBUTES) {
            fprintf(stderr, "error: preset '%s' does not exist\n", args[1]);
            return 1;
        }
        if (katoshe_config_load(&cfg, settings_path, paths.presets_dir, args[1], err,
                                sizeof err) != 0) {
            fprintf(stderr, "error: %s\n", err);
            return 1;
        }
        copy_str(cfg.preset, sizeof cfg.preset, args[1]);
        if (katoshe_config_save(&cfg, settings_path, paths.presets_dir, err, sizeof err) != 0) {
            fprintf(stderr, "error: %s\n", err);
            katoshe_config_free(&cfg);
            return 1;
        }
        katoshe_config_free(&cfg);
        printf("active preset: %s\n", args[1]);
        return 0;
    }

    if (strcmp(args[0], "export") == 0) {
        char settings_path[600];
        katoshe_config cfg;

        if (arg_count < 2) {
            fprintf(stderr, "error: preset export needs a name\n");
            return 2;
        }
        katoshe_path_join(settings_path, sizeof settings_path, paths.config_dir, "settings.json");
        /* Loading by name also validates the file before anything is emitted. */
        if (katoshe_config_load(&cfg, settings_path, paths.presets_dir, args[1], err,
                                sizeof err) != 0) {
            fprintf(stderr, "error: %s\n", err);
            return 1;
        }
        if (arg_count >= 3) {
            if (katoshe_preset_write(&cfg, args[2], err, sizeof err) != 0) {
                fprintf(stderr, "error: %s\n", err);
                katoshe_config_free(&cfg);
                return 1;
            }
            printf("%s\n", args[2]);
        } else {
            char *link = (char *)malloc(1 << 20);
            if (!link) {
                fprintf(stderr, "error: out of memory\n");
                katoshe_config_free(&cfg);
                return 1;
            }
            if (katoshe_preset_to_link(&cfg, link, 1 << 20, err, sizeof err) != 0) {
                fprintf(stderr, "error: %s\n", err);
                free(link);
                katoshe_config_free(&cfg);
                return 1;
            }
            printf("%s\n", link);
            free(link);
        }
        katoshe_config_free(&cfg);
        return 0;
    }

    if (strcmp(args[0], "import") == 0) {
        char name[KATOSHE_PRESET_MAX];
        char temp[MAX_PATH];
        const char *source;

        temp[0] = '\0';
        if (arg_count < 2) {
            fprintf(stderr, "error: preset import needs a path, a URL or a katoshe:// link\n");
            return 2;
        }
        source = args[1];
        if (_strnicmp(source, "http://", 7) == 0 || _strnicmp(source, "https://", 8) == 0) {
            katoshe_download_opts dl;
            katoshe_download_result res;

            snprintf(temp, sizeof temp, "%s\\katoshe-import-%lu.json",
                     getenv("TEMP") ? getenv("TEMP") : ".", (unsigned long)GetCurrentProcessId());
            memset(&dl, 0, sizeof dl);
            dl.timeout_sec = 60;
            if (katoshe_download(source, temp, NULL, &dl, &res, err, sizeof err) != 0) {
                fprintf(stderr, "error: %s\n", err);
                return 1;
            }
            source = temp;
        }

        if (katoshe_preset_import(source, paths.presets_dir, name, sizeof name, err,
                                  sizeof err) != 0) {
            fprintf(stderr, "error: %s\n", err);
            DeleteFileA(temp);
            return 1;
        }
        if (source == temp) DeleteFileA(temp);

        {
            katoshe_preset_info *list = NULL;
            int count = 0;
            if (katoshe_preset_list(paths.presets_dir, &list, &count, err, sizeof err) == 0) {
                for (int i = 0; i < count; i++) {
                    if (strcmp(list[i].name, name) == 0) {
                        printf("imported as '%s': %d repositories, %d assets\n", name,
                               list[i].repo_count, list[i].asset_count);
                        printf("activate with: preset use %s\n", name);
                    }
                }
                katoshe_preset_list_free(list);
            }
        }
        return 0;
    }

    fprintf(stderr, "error: '%s' is not a preset command\n", args[0]);
    return 2;
}

static int cmd_add(const cli_options *opt, const char *url) {
    run_context ctx;
    char err[512];
    char owner[128], repo_name[128];
    katoshe_repo *repos;

    if (gh_parse_url(url, owner, sizeof owner, repo_name, sizeof repo_name) != 0) {
        fprintf(stderr, "error: '%s' is not a GitHub repository\n", url);
        return 2;
    }
    if (run_begin(opt, &ctx, err, sizeof err) != 0) {
        fprintf(stderr, "error: %s\n", err);
        return 2;
    }
    for (int i = 0; i < ctx.cfg.repo_count; i++) {
        if (_stricmp(ctx.cfg.repos[i].url, url) == 0) {
            fprintf(stderr, "error: %s is already in preset '%s'\n", url, ctx.cfg.preset);
            run_end(&ctx);
            return 1;
        }
    }

    repos = (katoshe_repo *)realloc(ctx.cfg.repos, sizeof *repos * (size_t)(ctx.cfg.repo_count + 1));
    if (!repos) {
        fprintf(stderr, "error: out of memory\n");
        run_end(&ctx);
        return 1;
    }
    ctx.cfg.repos = repos;
    memset(&repos[ctx.cfg.repo_count], 0, sizeof *repos);
    copy_str(repos[ctx.cfg.repo_count].name, sizeof repos[0].name, repo_name);
    copy_str(repos[ctx.cfg.repo_count].url, sizeof repos[0].url, url);
    repos[ctx.cfg.repo_count].include_prerelease = -1;
    ctx.cfg.repo_count++;

    {
        char settings_path[600];
        katoshe_path_join(settings_path, sizeof settings_path, ctx.paths.config_dir, "settings.json");
        if (katoshe_config_save(&ctx.cfg, settings_path, ctx.paths.presets_dir, err,
                                sizeof err) != 0) {
            fprintf(stderr, "error: %s\n", err);
            run_end(&ctx);
            return 1;
        }
    }
    printf("added %s/%s to preset '%s'\n", owner, repo_name, ctx.cfg.preset);
    /* config_save writes the pointer along with the repositories, so a one-off
       --preset here becomes the active one. Say so instead of doing it quietly. */
    if (opt->preset && opt->preset[0]) printf("active preset is now '%s'\n", ctx.cfg.preset);
    run_end(&ctx);
    return 0;
}

int main(int argc, char **argv) {
    const char *args[16];
    int arg_count = 0;
    cli_options opt = take_options(argc, argv, args, &arg_count);

    if (arg_count < 1) {
        usage();
        return 2;
    }
    if (strcmp(args[0], "--version") == 0 || strcmp(args[0], "-V") == 0) {
        printf("katoshe %s\n", katoshe_version());
        return 0;
    }
    if (strcmp(args[0], "--help") == 0 || strcmp(args[0], "-h") == 0) {
        usage();
        return 0;
    }
    if (strcmp(args[0], "validate") == 0) {
        return cmd_validate(&opt);
    }
    if (strcmp(args[0], "list") == 0) {
        if (arg_count < 2) {
            fprintf(stderr, "error: list needs an owner/repo argument\n");
            return 2;
        }
        return cmd_list(&opt, args[1]);
    }
    if (strcmp(args[0], "check") == 0) {
        return cmd_check(&opt, args + 1, arg_count - 1);
    }
    if (strcmp(args[0], "update") == 0) {
        return cmd_update(&opt, args + 1, arg_count - 1);
    }
    if (strcmp(args[0], "add") == 0) {
        if (arg_count < 2) {
            fprintf(stderr, "error: add needs a repository URL\n");
            return 2;
        }
        return cmd_add(&opt, args[1]);
    }
    if (strcmp(args[0], "preset") == 0) {
        return cmd_preset(&opt, args + 1, arg_count - 1);
    }
    if (strcmp(args[0], "selftest") == 0) {
        int failed = katoshe_selftest();
        printf(failed ? "selftest: %d failed\n" : "selftest: all checks passed\n", failed);
        return failed ? 1 : 0;
    }

    fprintf(stderr, "katoshe-cli: '%s' is not implemented yet\n\n", args[0]);
    usage();
    return 2;
}
