#include <windows.h>

#include <stdio.h>
#include <string.h>

#include "config.h"
#include "katoshe.h"
#include "paths.h"
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
    printf("  --version           print the version and exit\n");
    printf("  --help              print this text and exit\n");
}

typedef struct {
    const char *root;
    const char *preset;
} cli_options;

static cli_options take_options(int argc, char **argv, const char **rest, int *rest_count) {
    cli_options opt = {NULL, NULL};
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
        rest[(*rest_count)++] = argv[i];
    }
    return opt;
}

static int cmd_validate(const cli_options *opt) {
    katoshe_paths paths;
    katoshe_config cfg;
    char err[512];
    char settings_path[600];
    char summary[512];
    int rc;

    if (katoshe_paths_init(&paths, opt->root, err, sizeof err) != 0) {
        fprintf(stderr, "error: %s\n", err);
        return 2;
    }
    if (katoshe_path_join(settings_path, sizeof settings_path, paths.config_dir, "settings.json") != 0) {
        fprintf(stderr, "error: config path too long\n");
        return 2;
    }
    rc = katoshe_config_load(&cfg, settings_path, paths.presets_dir, opt->preset, err, sizeof err);
    if (rc != 0) {
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
    if (strcmp(args[0], "selftest") == 0) {
        int failed = katoshe_selftest();
        printf(failed ? "selftest: %d failed\n" : "selftest: all checks passed\n", failed);
        return failed ? 1 : 0;
    }

    fprintf(stderr, "katoshe-cli: '%s' is not implemented yet\n\n", args[0]);
    usage();
    return 2;
}
