#include "paths.h"

#include <windows.h>

#include <stdio.h>
#include <string.h>

static int wide_to_utf8(const wchar_t *in, char *out, size_t cap) {
    int n = WideCharToMultiByte(CP_UTF8, 0, in, -1, out, (int)cap, NULL, NULL);
    return n > 0 ? 0 : -1;
}

/* Shim for snprintf-style formatting into a fixed buffer. */
static int fmt(char *out, size_t cap, const char *a, const char *b) {
    return katoshe_path_join(out, cap, a, b);
}

int katoshe_path_join(char *out, size_t cap, const char *a, const char *b) {
    const char *sep = (a[0] && a[strlen(a) - 1] == '\\') ? "" : "\\";
    int n = snprintf(out, cap, "%s%s%s", a, sep, b);
    return (n > 0 && (size_t)n < cap) ? 0 : -1;
}

int katoshe_exe_dir(char *out, size_t cap) {
    wchar_t wpath[1024];
    DWORD n = GetModuleFileNameW(NULL, wpath, (DWORD)(sizeof wpath / sizeof wpath[0]));
    if (n == 0 || n >= sizeof wpath / sizeof wpath[0]) return -1;
    for (DWORD i = n; i > 0; i--) {
        if (wpath[i - 1] == L'\\' || wpath[i - 1] == L'/') {
            wpath[i - 1] = L'\0';
            break;
        }
    }
    return wide_to_utf8(wpath, out, cap);
}

int katoshe_mkdir(const char *path) {
    if (CreateDirectoryA(path, NULL)) return 0;
    return (GetLastError() == ERROR_ALREADY_EXISTS) ? 0 : -1;
}

int katoshe_paths_init(katoshe_paths *p, const char *root_override, char *err, size_t errlen) {
    char exe[512];
    char probe[600];
    wchar_t wbuf[512];

    memset(p, 0, sizeof *p);

    if (root_override && root_override[0]) {
        snprintf(p->base, sizeof p->base, "%s", root_override);
        p->portable = true;
    } else {
        if (katoshe_exe_dir(exe, sizeof exe) != 0) {
            snprintf(err, errlen, "cannot determine the executable directory");
            return -1;
        }
        if (fmt(probe, sizeof probe, exe, "config") != 0) {
            snprintf(err, errlen, "path too long: %s\\config", exe);
            return -1;
        }
        if (GetFileAttributesA(probe) != INVALID_FILE_ATTRIBUTES) {
            snprintf(p->base, sizeof p->base, "%s", exe);
            p->portable = true;
        } else {
            DWORD n = GetEnvironmentVariableW(L"APPDATA", wbuf,
                                              (DWORD)(sizeof wbuf / sizeof wbuf[0]));
            if (n == 0) {
                snprintf(err, errlen, "APPDATA is not set and no config directory sits beside the exe");
                return -1;
            }
            if (wide_to_utf8(wbuf, p->base, sizeof p->base) != 0 ||
                fmt(p->base, sizeof p->base, p->base, "Katoshe") != 0) {
                snprintf(err, errlen, "cannot build the %%APPDATA%% path");
                return -1;
            }
            p->portable = false;
        }
    }

    if (fmt(p->config_dir, sizeof p->config_dir, p->base, "config") != 0 ||
        fmt(p->data_dir, sizeof p->data_dir, p->base, "data") != 0) {
        snprintf(err, errlen, "path too long under %s", p->base);
        return -1;
    }
    if (katoshe_mkdir(p->base) != 0) {
        snprintf(err, errlen, "cannot create %s", p->base);
        return -1;
    }
    if (katoshe_mkdir(p->config_dir) != 0) {
        snprintf(err, errlen, "cannot create %s", p->config_dir);
        return -1;
    }
    if (katoshe_mkdir(p->data_dir) != 0) {
        snprintf(err, errlen, "cannot create %s", p->data_dir);
        return -1;
    }
    return 0;
}
