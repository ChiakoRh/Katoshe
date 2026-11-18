#include "io.h"

#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *katoshe_read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    long n;
    size_t got;
    char *buf;

    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    n = ftell(f);
    if (n < 0) { fclose(f); return NULL; }
    rewind(f);
    buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    return buf;
}

int katoshe_write_atomic(const char *path, const char *text, char *err, size_t errlen) {
    char tmp[1024];
    HANDLE h;
    DWORD len, written = 0;
    BOOL ok;

    if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp) {
        snprintf(err, errlen, "%s: path too long", path);
        return -1;
    }
    h = CreateFileA(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        snprintf(err, errlen, "%s: cannot create the temporary file", tmp);
        return -1;
    }
    len = (DWORD)strlen(text);
    ok = WriteFile(h, text, len, &written, NULL);
    CloseHandle(h);
    if (!ok || written != len) {
        DeleteFileA(tmp);
        snprintf(err, errlen, "%s: cannot write the temporary file", tmp);
        return -1;
    }
    if (!MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileA(tmp);
        snprintf(err, errlen, "%s: cannot replace the file", path);
        return -1;
    }
    return 0;
}

int katoshe_mkdir_all(const char *path) {
    char buf[1024];
    size_t n;

    if (!path[0]) return -1;
    if (strlen(path) >= sizeof buf) return -1;
    snprintf(buf, sizeof buf, "%s", path);

    /* Walk forward, creating each prefix. A drive prefix ("C:") is skipped. */
    for (n = 0; buf[n]; n++) {
        if (buf[n] != '\\' && buf[n] != '/') continue;
        if (n == 0) continue;
        if (n == 2 && buf[1] == ':') continue;
        if (buf[n - 1] == '\\' || buf[n - 1] == '/') continue;
        buf[n] = '\0';
        if (!CreateDirectoryA(buf, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) return -1;
        buf[n] = '\\';
    }
    if (!CreateDirectoryA(buf, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) return -1;
    return 0;
}

void katoshe_dirname(const char *path, char *out, size_t cap) {
    const char *last = NULL;
    size_t n;

    for (const char *p = path; *p; p++) {
        if (*p == '\\' || *p == '/') last = p;
    }
    if (!last) {
        if (cap) out[0] = '\0';
        return;
    }
    n = (size_t)(last - path);
    if (n >= cap) n = cap - 1;
    memcpy(out, path, n);
    out[n] = '\0';
}
