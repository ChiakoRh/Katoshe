#include "archive.h"

#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "io.h"
#include "match.h"

typedef struct {
    char *path;
    ULONGLONG stamp; /* last write, so the newest file sorts first */
    char name[MAX_PATH];
} entry;

static int keeps_newest(const void *a, const void *b) {
    const entry *x = (const entry *)a;
    const entry *y = (const entry *)b;
    if (x->stamp != y->stamp) return x->stamp > y->stamp ? -1 : 1;
    return strcmp(y->name, x->name);
}

static void free_entries(entry *list, int count) {
    for (int i = 0; i < count; i++) free(list[i].path);
    free(list);
}

int katoshe_archive_prune(const char *dir, const char *pattern, int keep,
                          char *err, size_t errlen) {
    char probe[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    entry *list = NULL;
    int count = 0;
    int cap = 0;
    int deleted = 0;

    if (err && errlen) err[0] = '\0';
    if (!dir || !dir[0] || !pattern || !pattern[0]) {
        if (err && errlen) snprintf(err, errlen, "archive: invalid arguments");
        return -1;
    }
    if (keep < 1) keep = 1;

    if (snprintf(probe, sizeof probe, "%s\\*", dir) >= (int)sizeof probe) {
        if (err && errlen) snprintf(err, errlen, "%s: path too long", dir);
        return -1;
    }

    h = FindFirstFileA(probe, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0; /* nothing on disk yet */

    do {
        size_t len;
        entry *slot;

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        len = strlen(fd.cFileName);
        if (len >= sizeof fd.cFileName) continue;
        /* Never touch an unfinished transfer or another selection's files. */
        if (len > 5 && _stricmp(fd.cFileName + len - 5, ".part") == 0) continue;
        if (!katoshe_match(pattern, fd.cFileName)) continue;

        if (count == cap) {
            int next = cap ? cap * 2 : 8;
            entry *grown = (entry *)realloc(list, (size_t)next * sizeof *grown);
            if (!grown) goto oom;
            list = grown;
            cap = next;
        }
        slot = &list[count];
        memset(slot, 0, sizeof *slot);
        if (snprintf(slot->name, sizeof slot->name, "%s", fd.cFileName) >= (int)sizeof slot->name) {
            continue;
        }
        slot->path = (char *)malloc(strlen(dir) + len + 2);
        if (!slot->path) goto oom;
        sprintf(slot->path, "%s\\%s", dir, fd.cFileName);
        slot->stamp = ((ULONGLONG)fd.ftLastWriteTime.dwHighDateTime << 32) |
                      fd.ftLastWriteTime.dwLowDateTime;
        count++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);

    if (count > keep) {
        qsort(list, (size_t)count, sizeof *list, keeps_newest);
        for (int i = keep; i < count; i++) {
            if (DeleteFileA(list[i].path)) {
                deleted++;
            } else if (err && errlen) {
                snprintf(err, errlen, "%s: cannot delete the old version", list[i].path);
            }
        }
    }
    free_entries(list, count);
    return deleted;

oom:
    FindClose(h);
    free_entries(list, count);
    if (err && errlen) snprintf(err, errlen, "archive: out of memory");
    return -1;
}

/* ---- self-check ---- */

static int failures;

static void check(int cond, const char *name) {
    if (!cond) {
        printf("FAIL %s\n", name);
        failures++;
    }
}

static void put(const char *path, const char *text, FILETIME when) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(text, 1, strlen(text), f);
    fclose(f);
    {
        HANDLE h = CreateFileA(path, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            SetFileTime(h, NULL, NULL, &when);
            CloseHandle(h);
        }
    }
}

int katoshe_archive_selftest(void) {
    char dir[MAX_PATH];
    char path[MAX_PATH];
    char err[512];
    FILETIME base;
    int deleted;

    failures = 0;
    snprintf(dir, sizeof dir, "%s\\katoshe-archive-%lu", getenv("TEMP") ? getenv("TEMP") : ".",
             (unsigned long)GetCurrentProcessId());
    katoshe_mkdir_all(dir);

    /* Remove anything a previous run of this test left behind. */
    for (int i = 1; i <= 4; i++) {
        snprintf(path, sizeof path, "%s\\app-1.%d.0.zip", dir, i);
        DeleteFileA(path);
    }

    GetSystemTimeAsFileTime(&base);

    /* Four versions, deliberately written out of order so mtime decides. */
    for (int i = 1; i <= 4; i++) {
        FILETIME when = base;
        when.dwLowDateTime += (DWORD)i * 10000000u; /* one second apart */
        snprintf(path, sizeof path, "%s\\app-1.%d.0.zip", dir, i);
        put(path, "x", when);
    }
    snprintf(path, sizeof path, "%s\\app-1.4.0.zip.part", dir);
    put(path, "x", base);

    deleted = katoshe_archive_prune(dir, "app-*.zip", 3, err, sizeof err);
    check(deleted == 1, "prune deletes exactly the oldest");
    snprintf(path, sizeof path, "%s\\app-1.1.0.zip", dir);
    check(GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES, "the oldest is gone");
    snprintf(path, sizeof path, "%s\\app-1.4.0.zip", dir);
    check(GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES, "the newest stays");
    snprintf(path, sizeof path, "%s\\app-1.4.0.zip.part", dir);
    check(GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES, "a .part is never pruned");

    deleted = katoshe_archive_prune(dir, "app-*.zip", 3, err, sizeof err);
    check(deleted == 0, "a second prune is a no-op");

    deleted = katoshe_archive_prune(dir, "nothing-*.apk", 1, err, sizeof err);
    check(deleted == 0, "an unmatched pattern deletes nothing");

    check(katoshe_archive_prune(dir, "*.zip", 0, err, sizeof err) == 2,
          "keep < 1 is clamped to one");
    snprintf(path, sizeof path, "%s\\app-1.4.0.zip", dir);
    check(GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES, "the newest survives keep=0");
    DeleteFileA(path);

    {
        char missing[MAX_PATH];
        snprintf(missing, sizeof missing, "%s\\no-such-dir", dir);
        check(katoshe_archive_prune(missing, "*.zip", 2, err, sizeof err) == 0,
              "a missing directory is not an error");
    }
    DeleteFileA(path);
    snprintf(path, sizeof path, "%s\\app-1.4.0.zip.part", dir);
    DeleteFileA(path);
    RemoveDirectoryA(dir);
    return failures;
}
