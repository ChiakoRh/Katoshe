#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "gh.h"
#include "katoshe.h"
#include "paths.h"
#include "secrets.h"
#include "selftest.h"

static void usage(void) {
    printf("katoshe %s - GitHub release asset manager\n\n", katoshe_version());
    printf("usage: katoshe-cli <command> [options]\n\n");
    printf("  check   [repo...]   check for missing files and updates, write nothing\n");
    printf("  update  [repo...]   download and update selected assets\n");
    printf("  list    <owner/repo> show the target release and its assets\n");
    printf("  validate            parse and validate the configuration\n");
    printf("  add     <url>       add a repository to the active preset\n\n");
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

/* --token, then KATOSHE_GITHUB_TOKEN, then config/secrets.json. */
static void resolve_token(const cli_options *opt, const char *config_dir,
                          char *out, size_t cap) {
    char err[256];
    const char *env = env_or_null("KATOSHE_GITHUB_TOKEN");

    out[0] = '\0';
    if (opt->token && opt->token[0]) {
        snprintf(out, cap, "%s", opt->token);
        return;
    }
    if (env) {
        snprintf(out, cap, "%s", env);
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
                          &gh_opt, &release, &assets, &asset_count, err, sizeof err);
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
    if (strcmp(args[0], "selftest") == 0) {
        int failed = katoshe_selftest();
        printf(failed ? "selftest: %d failed\n" : "selftest: all checks passed\n", failed);
        return failed ? 1 : 0;
    }

    fprintf(stderr, "katoshe-cli: '%s' is not implemented yet\n\n", args[0]);
    usage();
    return 2;
}
