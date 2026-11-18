#include <stdio.h>
#include <string.h>

#include "katoshe.h"

static void usage(void) {
    printf("katoshe %s - GitHub release asset manager\n\n", katoshe_version());
    printf("usage: katoshe-cli <command> [options]\n\n");
    printf("  check   [repo...]   check for missing files and updates, write nothing\n");
    printf("  update  [repo...]   download and update selected assets\n");
    printf("  list    <owner/repo> show the target release and its assets\n");
    printf("  validate            parse and validate the configuration\n");
    printf("  add     <url>       add a repository to the configuration\n\n");
    printf("  --version           print the version and exit\n");
    printf("  --help              print this text and exit\n");
}

int main(int argc, char **argv) {
    if (argc < 2) {
        usage();
        return 2;
    }
    if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0) {
        printf("katoshe %s\n", katoshe_version());
        return 0;
    }
    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        usage();
        return 0;
    }
    fprintf(stderr, "katoshe-cli: unknown command '%s'\n\n", argv[1]);
    usage();
    return 2;
}
