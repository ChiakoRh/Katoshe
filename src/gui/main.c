/* Katoshe, window front end.

   Four pages over the engine the CLI uses: repositories and their asset
   selections, the target release of one repository, a run page with the live
   log, and the per-machine settings. Network work happens on a worker thread;
   the frame function only reads a snapshot of it, so a 12 MB download never
   blocks the window.

   The worker reloads the configuration from disk and calls the same core
   functions in the same order as `katoshe-cli update`, which is why the two
   front ends cannot disagree about what to download. */

#include <windows.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cui/cui.h>
#include <cui/cui_app.h>

#include "archive.h"
#include "config.h"
#include "cjson/cJSON.h"
#include "download.h"
#include "env.h"
#include "gh.h"
#include "io.h"
#include "katoshe.h"
#include "match.h"
#include "paths.h"
#include "plan.h"
#include "preset.h"
#include "state.h"

#define JOB_NONE 0
#define JOB_CHECK 1
#define JOB_UPDATE 2
#define JOB_FETCH 3

#define LOG_LINES 400
#define LOG_LEN 200
#define STATUS_MAX 256
#define REMOTE_MAX 256
#define KEY_LEN 360

#define PAGE_REPOS 0
#define PAGE_ASSETS 1
#define PAGE_RUN 2
#define PAGE_SETTINGS 3

typedef struct {
    char key[KEY_LEN]; /* "<repository name>/<match>" */
    int action;        /* plan_action, -1 before the plan ran */
    long long received;
    long long total;
    long long size;
    char note[64];
} gui_status;

typedef struct {
    SRWLOCK lock;
    HANDLE thread;
    int kind;
    int running;
    int cancel; /* checked between files; a transfer in flight finishes */
    int done;
    int exit_code;

    char log[LOG_LINES][LOG_LEN];
    int log_count;

    gui_status status[STATUS_MAX];
    int status_count;

    char current[260];
    long long received, total;

    /* assets fetched for the Assets page */
    int fetch_valid;
    int fetch_repo;
    char fetch_key[KEY_LEN];
    char fetch_tag[128];
    char fetch_error[256];
    gh_asset fetch[REMOTE_MAX];
    int fetch_count;
} gui_job;

typedef struct {
    cui_app *app;

    katoshe_paths paths;
    char settings_path[600];
    char lockfile_path[600];
    char download_dir[KATOSHE_PATH_MAX];
    katoshe_config cfg;
    katoshe_state state;

    char root_input[KATOSHE_PATH_MAX];
    char token_input[256];
    char preset_override[KATOSHE_PRESET_MAX];
    int token_dirty;
    int dirty; /* configuration changed, save at the end of the frame */

    int page;
    int sel_repo;
    int theme;         /* 0 system, 1 light, 2 dark */
    int theme_applied;
    int platform_mask; /* asset quick pick, bit per platform_rules entry */
    int no_jump; /* --page was given: do not move the user off it */
    char err[512];

    katoshe_preset_info *presets; /* config/presets/*.json, reloaded with the config */
    int preset_count;
    int preset_choice;
    char preset_source[512]; /* import: a path, a URL or a katoshe:// link */
    char preset_msg[192];
    char pending_preset[KATOSHE_PRESET_MAX];

    char new_url[512];
    char new_name[128];
    char new_match[KATOSHE_NAME_MAX];
    char new_dir[KATOSHE_PATH_MAX];

    gui_job job;
} gui_state;

/* A copy of the worker state, taken under the lock so the frame never holds it
   while drawing. BSS, not stack: the log alone is 80 KB. */
typedef struct {
    int running, done, exit_code, kind, cancel;
    int log_count;
    char log[LOG_LINES][LOG_LEN];
    int status_count;
    gui_status status[STATUS_MAX];
    char current[260];
    long long received, total;
    int fetch_valid, fetch_repo, fetch_count;
    char fetch_key[KEY_LEN];
    char fetch_tag[128];
    char fetch_error[256];
    gh_asset fetch[REMOTE_MAX];
} job_view;

static job_view view;

/* Quick pick for the asset list. A keyword hit is a guess, not a real
   fingerprint: it only decides what floats to the top. */
typedef struct {
    const char *chip;
    const char *badge;
    const char *keys[6];
} platform_rule;

static const platform_rule platform_rules[] = {
    {"Android arm64-v8a", "arm64-v8a", {"arm64-v8a", "aarch64", NULL}},
    {"Android armeabi-v7a", "armeabi-v7a", {"armeabi-v7a", "armv7", NULL}},
    {"Windows", "windows", {"windows", "win64", "win32", "win-x64", ".exe", NULL}},
    {"Linux", "linux", {"linux", ".appimage", ".deb", NULL}},
    {"macOS", "macos", {"macos", "darwin", ".dmg", NULL}},
};
#define PLATFORM_COUNT ((int)(sizeof platform_rules / sizeof platform_rules[0]))

static void job_log(gui_job *j, const char *fmt, ...) {
    va_list ap;

    AcquireSRWLockExclusive(&j->lock);
    if (j->log_count < LOG_LINES) {
        va_start(ap, fmt);
        vsnprintf(j->log[j->log_count], LOG_LEN, fmt, ap);
        va_end(ap);
        j->log_count++;
    }
    ReleaseSRWLockExclusive(&j->lock);
}

/* Caller holds the lock. */
static gui_status *job_status(gui_job *j, const char *key, int create) {
    for (int i = 0; i < j->status_count; i++) {
        if (strcmp(j->status[i].key, key) == 0) return &j->status[i];
    }
    if (!create || j->status_count >= STATUS_MAX) return NULL;

    gui_status *st = &j->status[j->status_count++];
    memset(st, 0, sizeof *st);
    katoshe_copy_str(st->key, sizeof st->key, key);
    st->action = -1;
    return st;
}

typedef struct {
    gui_job *job;
    char key[KEY_LEN];
} progress_slot;

static void on_progress(void *user, long long received, long long total) {
    progress_slot *p = user;

    AcquireSRWLockExclusive(&p->job->lock);
    gui_status *st = job_status(p->job, p->key, 1);
    if (st) {
        st->received = received;
        st->total = total;
    }
    if (p->job->current[0] == '\0') katoshe_copy_str(p->job->current, sizeof p->job->current, p->key);
    ReleaseSRWLockExclusive(&p->job->lock);
}

static void now_iso(char *out, size_t cap) {
    SYSTEMTIME t;

    GetSystemTime(&t);
    snprintf(out, cap, "%04u-%02u-%02uT%02u:%02u:%02uZ", t.wYear, t.wMonth, t.wDay, t.wHour,
             t.wMinute, t.wSecond);
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
    katoshe_copy_str(out, cap, p);
}

static void job_begin_common(gui_job *j) {
    j->log_count = 0;
    j->status_count = 0;
    j->current[0] = '\0';
    j->received = 0;
    j->total = 0;
    j->cancel = 0;
    j->done = 0;
    j->exit_code = -1;
}

/* ------------------------------------------------------------------------- */
/* Worker                                                                    */
/* ------------------------------------------------------------------------- */

static void job_run_fetch(gui_state *s) {
    gui_job *j = &s->job;
    katoshe_config cfg;
    char err[512] = "";
    char owner[128], repo_name[128], key[KEY_LEN];
    gh_options opt;
    gh_meta meta;
    gh_release release;
    gh_asset *assets = NULL;
    int asset_count = 0;
    int index;

    AcquireSRWLockShared(&j->lock);
    index = j->fetch_repo;
    ReleaseSRWLockShared(&j->lock);

    if (katoshe_config_load(&cfg, s->settings_path, s->paths.presets_dir, NULL, err,
                            sizeof err) != 0) {
        job_log(j, "error: %s", err);
        j->exit_code = 2;
        return;
    }
    if (index < 0 || index >= cfg.repo_count) {
        job_log(j, "error: no such repository");
        j->exit_code = 2;
        katoshe_config_free(&cfg);
        return;
    }
    if (gh_parse_url(cfg.repos[index].url, owner, sizeof owner, repo_name, sizeof repo_name) !=
        0) {
        job_log(j, "error: %s: the URL is not a GitHub repository", cfg.repos[index].name);
        j->exit_code = 2;
        katoshe_config_free(&cfg);
        return;
    }
    snprintf(key, sizeof key, "%s %s/%s", cfg.repos[index].name, owner, repo_name);

    memset(&opt, 0, sizeof opt);
    opt.proxy = katoshe_resolve_proxy(&cfg, owner, repo_name);
    opt.token = s->token_input[0] ? s->token_input : NULL;
    opt.timeout_sec = cfg.global.timeout_sec;

    job_log(j, "fetching %s/%s", owner, repo_name);
    if (gh_fetch_release(owner, repo_name,
                         cfg.repos[index].include_prerelease >= 0
                             ? cfg.repos[index].include_prerelease != 0
                             : cfg.global.include_prerelease,
                         &opt, &meta, &release, &assets, &asset_count, err, sizeof err) != GH_OK) {
        job_log(j, "error: %s", err);
        AcquireSRWLockExclusive(&j->lock);
        katoshe_copy_str(j->fetch_error, sizeof j->fetch_error, err);
        j->fetch_valid = 0;
        ReleaseSRWLockExclusive(&j->lock);
        j->exit_code = 1;
        katoshe_config_free(&cfg);
        return;
    }

    AcquireSRWLockExclusive(&j->lock);
    katoshe_copy_str(j->fetch_key, sizeof j->fetch_key, key);
    katoshe_copy_str(j->fetch_tag, sizeof j->fetch_tag, release.tag);
    j->fetch_repo = index;
    j->fetch_count = asset_count < REMOTE_MAX ? asset_count : REMOTE_MAX;
    for (int i = 0; i < j->fetch_count; i++) j->fetch[i] = assets[i];
    j->fetch_error[0] = '\0';
    j->fetch_valid = 1;
    ReleaseSRWLockExclusive(&j->lock);

    job_log(j, "%s  %s%s", key, release.tag, release.prerelease ? "  [prerelease]" : "");
    job_log(j, "%d assets in this release", asset_count);
    j->exit_code = 0;

    gh_release_free(assets);
    katoshe_config_free(&cfg);
}

static void job_run_check_update(gui_state *s) {
    gui_job *j = &s->job;
    katoshe_config cfg;
    katoshe_state st;
    char err[512] = "";
    char dl_dir[KATOSHE_PATH_MAX];
    char token[256];
    char lockfile[600];
    int check_only;
    int failed = 0, downloaded = 0, to_download = 0, unchanged = 0, missing = 0, touched = 0;

    AcquireSRWLockShared(&j->lock);
    check_only = j->kind == JOB_CHECK;
    ReleaseSRWLockShared(&j->lock);

    if (katoshe_config_load(&cfg, s->settings_path, s->paths.presets_dir, NULL, err,
                            sizeof err) != 0) {
        job_log(j, "error: %s", err);
        j->exit_code = 2;
        return;
    }
    if (katoshe_state_load(&st, s->lockfile_path, err, sizeof err) != 0) {
        job_log(j, "error: %s", err);
        j->exit_code = 2;
        katoshe_config_free(&cfg);
        return;
    }
    if (katoshe_download_dir(s->paths.base, cfg.global.download_dir, dl_dir, sizeof dl_dir) != 0) {
        job_log(j, "error: the download directory path is too long");
        j->exit_code = 2;
        katoshe_state_free(&st);
        katoshe_config_free(&cfg);
        return;
    }
    katoshe_copy_str(lockfile, sizeof lockfile, s->lockfile_path);
    katoshe_resolve_token(s->token_input[0] ? s->token_input : NULL, s->paths.config_dir, token,
                          sizeof token);

    job_log(j, "preset %s: %d repositories%s", cfg.preset, cfg.repo_count,
            check_only ? "  (check only, nothing is written)" : "");

    for (int i = 0; i < cfg.repo_count; i++) {
        const katoshe_repo *repo = &cfg.repos[i];
        char owner[128], repo_name[128], key[300];
        katoshe_state_repo *state_repo;
        gh_options opt;
        gh_meta meta;
        gh_release release;
        gh_asset *assets = NULL;
        int asset_count = 0;
        plan p;
        int rc;

        AcquireSRWLockShared(&j->lock);
        int cancelled = j->cancel;
        ReleaseSRWLockShared(&j->lock);
        if (cancelled) {
            job_log(j, "cancelled");
            break;
        }

        if (gh_parse_url(repo->url, owner, sizeof owner, repo_name, sizeof repo_name) != 0) {
            job_log(j, "error: %s: the URL is not a GitHub repository", repo->name);
            failed++;
            continue;
        }
        snprintf(key, sizeof key, "%s/%s", owner, repo_name);
        state_repo = katoshe_state_repo_get(&st, key, check_only ? 0 : 1);

        memset(&opt, 0, sizeof opt);
        opt.proxy = katoshe_resolve_proxy(&cfg, owner, repo_name);
        opt.token = token[0] ? token : NULL;
        opt.timeout_sec = cfg.global.timeout_sec;
        opt.etag = (check_only && state_repo && state_repo->etag[0]) ? state_repo->etag : NULL;

        job_log(j, "%s", key);
        rc = gh_fetch_release(owner, repo_name,
                              repo->include_prerelease >= 0 ? repo->include_prerelease != 0
                                                            : cfg.global.include_prerelease,
                              &opt, &meta, &release, &assets, &asset_count, err, sizeof err);
        if (rc == GH_NOT_MODIFIED) {
            job_log(j, "    not modified since the last check");
            continue;
        }
        if (rc != GH_OK) {
            job_log(j, "error: %s", err);
            failed++;
            continue;
        }
        job_log(j, "    %s%s", release.tag, release.prerelease ? "  [prerelease]" : "");

        if (katoshe_plan_build(repo, dl_dir, &release, assets, asset_count, state_repo, &p, err,
                               sizeof err) != 0) {
            job_log(j, "error: %s", err);
            failed++;
            gh_release_free(assets);
            continue;
        }

        for (int k = 0; k < p.count; k++) {
            plan_item *item = &p.items[k];
            char status_key[KEY_LEN];
            char size_text[32];
            char expected[72];
            katoshe_download_result res;
            gui_status *st_item;

            snprintf(status_key, sizeof status_key, "%s/%s", repo->name, item->match);
            if (item->action == PLAN_UNCHANGED) {
                unchanged++;
                AcquireSRWLockExclusive(&j->lock);
                st_item = job_status(j, status_key, 1);
                if (st_item) {
                    st_item->action = PLAN_UNCHANGED;
                    katoshe_copy_str(st_item->note, sizeof st_item->note, "up to date");
                }
                katoshe_copy_str(j->current, sizeof j->current, item->remote_name);
                ReleaseSRWLockExclusive(&j->lock);
                job_log(j, "    unchanged  %s", item->remote_name);
                continue;
            }
            if (item->action == PLAN_MISSING) {
                missing++;
                AcquireSRWLockExclusive(&j->lock);
                st_item = job_status(j, status_key, 1);
                if (st_item) {
                    st_item->action = PLAN_MISSING;
                    katoshe_copy_str(st_item->note, sizeof st_item->note, "no matching asset");
                }
                ReleaseSRWLockExclusive(&j->lock);
                job_log(j, "    missing    %s", item->match);
                continue;
            }

            gh_format_size(item->size, size_text, sizeof size_text);
            AcquireSRWLockExclusive(&j->lock);
            st_item = job_status(j, status_key, 1);
            if (st_item) {
                st_item->action = PLAN_DOWNLOAD;
                st_item->size = item->size;
                st_item->received = 0;
                st_item->total = 0;
                st_item->note[0] = '\0';
            }
            katoshe_copy_str(j->current, sizeof j->current, item->remote_name);
            ReleaseSRWLockExclusive(&j->lock);

            to_download++;
            if (check_only) {
                job_log(j, "    download   %s  (%s)", item->remote_name, size_text);
                continue;
            }

            job_log(j, "    download   %s  (%s)", item->remote_name, size_text);
            if (mkdir_for(item->path) != 0) {
                job_log(j, "error: cannot create the directory for %s", item->path);
                AcquireSRWLockExclusive(&j->lock);
                st_item = job_status(j, status_key, 1);
                if (st_item) katoshe_copy_str(st_item->note, sizeof st_item->note, "mkdir failed");
                ReleaseSRWLockExclusive(&j->lock);
                failed++;
                continue;
            }

            AcquireSRWLockShared(&j->lock);
            cancelled = j->cancel;
            ReleaseSRWLockShared(&j->lock);
            if (cancelled) {
                job_log(j, "cancelled");
                break;
            }

            {
                katoshe_download_opts dl;
                progress_slot slot;

                memset(&dl, 0, sizeof dl);
                memset(&slot, 0, sizeof slot);
                dl.proxy = katoshe_resolve_proxy(&cfg, owner, repo_name);
                dl.token = token[0] ? token : NULL;
                dl.timeout_sec = cfg.global.timeout_sec;
                dl.retries = cfg.global.retries;
                dl.progress = on_progress;
                dl.user = &slot;
                slot.job = j;
                katoshe_copy_str(slot.key, sizeof slot.key, status_key);
                digest_hex(item->digest, expected, sizeof expected);

                if (katoshe_download(item->url, item->path, expected[0] ? expected : NULL, &dl,
                                     &res, err, sizeof err) != 0) {
                    job_log(j, "error: %s", err);
                    AcquireSRWLockExclusive(&j->lock);
                    st_item = job_status(j, status_key, 1);
                    if (st_item) katoshe_copy_str(st_item->note, sizeof st_item->note, err);
                    ReleaseSRWLockExclusive(&j->lock);
                    failed++;
                    continue;
                }
            }

            downloaded++;
            job_log(j, "    %-10s %s", res.resumed ? "resumed" : "done", item->remote_name);

            if (repo->assets[k].mode == KATOSHE_MODE_ARCHIVE) {
                char dir[KATOSHE_PATH_MAX];
                int pruned;
                katoshe_dirname(item->path, dir, sizeof dir);
                pruned = katoshe_archive_prune(dir, item->match, repo->assets[k].keep_versions,
                                               err, sizeof err);
                if (pruned > 0) job_log(j, "    pruned %d older version%s", pruned,
                                        pruned == 1 ? "" : "s");
                else if (pruned < 0) job_log(j, "warning: %s", err);
            }

            AcquireSRWLockExclusive(&j->lock);
            st_item = job_status(j, status_key, 1);
            if (st_item) katoshe_copy_str(st_item->note, sizeof st_item->note, "done");
            ReleaseSRWLockExclusive(&j->lock);

            {
                katoshe_state_asset *sa = katoshe_state_asset_get(state_repo, item->match, 1);
                if (sa) {
                    if (repo->assets[k].mode != KATOSHE_MODE_ARCHIVE) {
                        char removed[KATOSHE_NAME_MAX];
                        if (katoshe_replaced_old_file(sa->path, item->path, removed,
                                                      sizeof removed)) {
                            job_log(j, "    replaced %s", removed);
                        }
                    }
                    sa->asset_id = item->asset_id;
                    sa->size = res.bytes;
                    katoshe_copy_str(sa->remote_name, sizeof sa->remote_name, item->remote_name);
                    katoshe_copy_str(sa->path, sizeof sa->path, item->path);
                    katoshe_copy_str(sa->digest, sizeof sa->digest, item->digest);
                    katoshe_copy_str(sa->sha256, sizeof sa->sha256, res.sha256);
                    touched = 1;
                }
            }
        }

        if (!check_only) {
            state_repo->release_id = release.id;
            katoshe_copy_str(state_repo->tag, sizeof state_repo->tag, release.tag);
            katoshe_copy_str(state_repo->etag, sizeof state_repo->etag, meta.etag);
            now_iso(state_repo->checked_at, sizeof state_repo->checked_at);
            touched = 1;
        }

        katoshe_plan_free(&p);
        gh_release_free(assets);
    }

    if (!check_only && touched && katoshe_state_save(&st, lockfile, err, sizeof err) != 0) {
        job_log(j, "error: %s", err);
        failed++;
    }

    if (check_only) {
        job_log(j, "%d to download, %d unchanged, %d missing%s", to_download, unchanged, missing,
                failed ? ", some failed" : "");
    } else {
        job_log(j, "%d downloaded, %d unchanged, %d missing%s", downloaded, unchanged, missing,
                failed ? ", some failed" : "");
    }
    j->exit_code = failed ? 1 : 0;

    katoshe_state_free(&st);
    katoshe_config_free(&cfg);
}

static DWORD WINAPI job_thread(void *param) {
    gui_state *s = param;
    gui_job *j = &s->job;
    int kind;

    AcquireSRWLockShared(&j->lock);
    kind = j->kind;
    ReleaseSRWLockShared(&j->lock);

    if (kind == JOB_FETCH) job_run_fetch(s);
    else job_run_check_update(s);

    AcquireSRWLockExclusive(&j->lock);
    j->running = 0;
    j->done = 1;
    ReleaseSRWLockExclusive(&j->lock);
    return 0;
}

static void job_start(gui_state *s, int kind) {
    gui_job *j = &s->job;

    AcquireSRWLockExclusive(&j->lock);
    if (j->running) {
        ReleaseSRWLockExclusive(&j->lock);
        return;
    }
    if (j->thread) {
        CloseHandle(j->thread);
        j->thread = NULL;
    }
    j->kind = kind;
    j->running = 1;
    job_begin_common(j);
    ReleaseSRWLockExclusive(&j->lock);

    s->err[0] = '\0';
    j->thread = CreateThread(NULL, 0, job_thread, s, 0, NULL);
    if (!j->thread) {
        AcquireSRWLockExclusive(&j->lock);
        j->running = 0;
        j->done = 1;
        j->exit_code = 2;
        ReleaseSRWLockExclusive(&j->lock);
        katoshe_copy_str(s->err, sizeof s->err, "cannot start the worker thread");
    }
}

static void job_cancel(gui_state *s) {
    AcquireSRWLockExclusive(&s->job.lock);
    s->job.cancel = 1;
    ReleaseSRWLockExclusive(&s->job.lock);
}

static void view_take(gui_state *s) {
    gui_job *j = &s->job;

    AcquireSRWLockShared(&j->lock);
    view.running = j->running;
    view.done = j->done;
    view.exit_code = j->exit_code;
    view.kind = j->kind;
    view.cancel = j->cancel;
    view.log_count = j->log_count;
    memcpy(view.log, j->log, sizeof view.log);
    view.status_count = j->status_count;
    memcpy(view.status, j->status, sizeof view.status);
    katoshe_copy_str(view.current, sizeof view.current, j->current);
    view.received = j->received;
    view.total = j->total;
    view.fetch_valid = j->fetch_valid;
    view.fetch_repo = j->fetch_repo;
    view.fetch_count = j->fetch_count;
    katoshe_copy_str(view.fetch_key, sizeof view.fetch_key, j->fetch_key);
    katoshe_copy_str(view.fetch_tag, sizeof view.fetch_tag, j->fetch_tag);
    katoshe_copy_str(view.fetch_error, sizeof view.fetch_error, j->fetch_error);
    memcpy(view.fetch, j->fetch, sizeof view.fetch);
    ReleaseSRWLockShared(&j->lock);

    AcquireSRWLockExclusive(&j->lock);
    j->done = 0;
    ReleaseSRWLockExclusive(&j->lock);
}

/* ------------------------------------------------------------------------- */
/* Configuration                                                             */
/* ------------------------------------------------------------------------- */

static void save_token(gui_state *s) {
    char path[600], err[256];
    cJSON *root;
    char *text;
    int ok;

    if (!s->token_dirty) return;
    if (katoshe_path_join(path, sizeof path, s->paths.config_dir, "secrets.json") != 0) return;

    root = cJSON_CreateObject();
    if (!root) return;
    cJSON_AddStringToObject(root, "token", s->token_input);
    text = cJSON_Print(root);
    cJSON_Delete(root);
    if (!text) return;

    ok = katoshe_write_atomic(path, text, err, sizeof err) == 0;
    free(text);
    if (ok) s->token_dirty = 0;
    else katoshe_copy_str(s->err, sizeof s->err, err);
}

static void gui_save(gui_state *s) {
    char err[512];

    if (!s->dirty) {
        save_token(s);
        return;
    }
    if (katoshe_config_save(&s->cfg, s->settings_path, s->paths.presets_dir, err, sizeof err) !=
        0) {
        katoshe_copy_str(s->err, sizeof s->err, err);
        return;
    }
    s->dirty = 0;
    save_token(s);
}

static void gui_load(gui_state *s) {
    char err[512];

    s->err[0] = '\0';
    katoshe_config_free(&s->cfg);
    katoshe_state_free(&s->state);

    if (katoshe_paths_init(&s->paths, s->root_input[0] ? s->root_input : NULL, err,
                           sizeof err) != 0) {
        katoshe_copy_str(s->err, sizeof s->err, err);
        return;
    }
    katoshe_copy_str(s->root_input, sizeof s->root_input, s->paths.base);
    if (katoshe_path_join(s->settings_path, sizeof s->settings_path, s->paths.config_dir,
                          "settings.json") != 0 ||
        katoshe_path_join(s->lockfile_path, sizeof s->lockfile_path, s->paths.data_dir,
                          "lockfile.json") != 0) {
        katoshe_copy_str(s->err, sizeof s->err, "the configuration path is too long");
        return;
    }

    int fresh = GetFileAttributesA(s->settings_path) == INVALID_FILE_ATTRIBUTES;
    if (katoshe_config_load(&s->cfg, s->settings_path, s->paths.presets_dir,
                            s->preset_override[0] ? s->preset_override : NULL, err,
                            sizeof err) != 0) {
        /* A fresh install has no files at all: that is not an error, and the
           empty configuration is written at the end of the first frame. A file
           that exists but does not parse is left untouched. */
        if (!fresh) katoshe_copy_str(s->err, sizeof s->err, err);
        katoshe_config_free(&s->cfg);
        katoshe_config_defaults(&s->cfg);
    }
    katoshe_download_dir(s->paths.base, s->cfg.global.download_dir, s->download_dir,
                         sizeof s->download_dir);
    katoshe_resolve_token(NULL, s->paths.config_dir, s->token_input, sizeof s->token_input);
    s->theme = 0;
    if (s->cfg.settings_doc) {
        cJSON *t = cJSON_GetObjectItemCaseSensitive(s->cfg.settings_doc, "gui_theme");
        if (cJSON_IsString(t) && t->valuestring) {
            if (strcmp(t->valuestring, "dark") == 0) s->theme = 2;
            else if (strcmp(t->valuestring, "light") == 0) s->theme = 1;
        }
    }
    s->theme_applied = -1;
    s->platform_mask = 0;
    if (s->cfg.settings_doc) {
        cJSON *pl = cJSON_GetObjectItemCaseSensitive(s->cfg.settings_doc, "gui_platforms");
        if (cJSON_IsArray(pl)) {
            cJSON *it = NULL;
            cJSON_ArrayForEach(it, pl) {
                if (!cJSON_IsString(it) || !it->valuestring) continue;
                for (int p = 0; p < PLATFORM_COUNT; p++) {
                    if (strcmp(platform_rules[p].chip, it->valuestring) == 0) {
                        s->platform_mask |= 1 << p;
                    }
                }
            }
        }
    }
    s->dirty = fresh ? 1 : 0;
    s->token_dirty = 0;
    if (s->sel_repo >= s->cfg.repo_count) s->sel_repo = 0;

    katoshe_preset_list_free(s->presets);
    s->presets = NULL;
    s->preset_count = 0;
    s->preset_choice = -1;
    if (katoshe_preset_list(s->paths.presets_dir, &s->presets, &s->preset_count, err,
                            sizeof err) != 0) {
        s->presets = NULL;
        s->preset_count = 0;
    }
    for (int i = 0; i < s->preset_count; i++) {
        if (strcmp(s->presets[i].name, s->cfg.preset) == 0) s->preset_choice = i;
    }
}

/* The theme lives in settings.json under an unknown top-level key, which
   katoshe_config_save carries through untouched for the CLI. */
static void set_theme(gui_state *s, int theme) {
    cJSON *doc = s->cfg.settings_doc;

    if (!doc) doc = s->cfg.settings_doc = cJSON_CreateObject();
    if (!doc) return;
    s->theme = theme;
    cJSON_DeleteItemFromObjectCaseSensitive(doc, "gui_theme");
    cJSON_AddStringToObject(doc, "gui_theme",
                            theme == 2 ? "dark" : (theme == 1 ? "light" : "system"));
    s->dirty = 1;
}

/* Same trick for the asset quick pick: an unknown top-level key the CLI keeps
   but never reads. */
static void set_platforms(gui_state *s, int mask) {
    cJSON *doc = s->cfg.settings_doc;
    cJSON *arr;

    s->platform_mask = mask;
    if (!doc) doc = s->cfg.settings_doc = cJSON_CreateObject();
    if (!doc) return;
    cJSON_DeleteItemFromObjectCaseSensitive(doc, "gui_platforms");
    arr = cJSON_AddArrayToObject(doc, "gui_platforms");
    for (int p = 0; arr && p < PLATFORM_COUNT; p++) {
        if (mask & (1 << p)) {
            cJSON_AddItemToArray(arr, cJSON_CreateString(platform_rules[p].chip));
        }
    }
    s->dirty = 1;
}

/* Points settings.json at another preset without writing the preset files: the
   imported or chosen file is the source of truth, never the in-memory copy. */
static int point_at_preset(gui_state *s, const char *name, char *err, size_t errlen) {
    char *text = katoshe_read_file(s->settings_path);
    cJSON *doc = text ? cJSON_Parse(text) : NULL;
    char *printed;
    int rc;

    free(text);
    if (!doc) doc = cJSON_CreateObject();
    if (!doc) {
        katoshe_copy_str(err, errlen, "out of memory");
        return -1;
    }
    cJSON_DeleteItemFromObjectCaseSensitive(doc, "preset");
    cJSON_AddStringToObject(doc, "preset", name);
    printed = cJSON_Print(doc);
    cJSON_Delete(doc);
    if (!printed) {
        katoshe_copy_str(err, errlen, "out of memory");
        return -1;
    }
    rc = katoshe_write_atomic(s->settings_path, printed, err, errlen);
    free(printed);
    return rc;
}

static void import_preset(gui_state *s) {
    char name[KATOSHE_PRESET_MAX];
    char err[512];

    if (!s->preset_source[0]) {
        katoshe_copy_str(s->preset_msg, sizeof s->preset_msg, "Enter a file, URL or link first.");
        return;
    }
    if (katoshe_preset_import(s->preset_source, s->paths.presets_dir, name, sizeof name, err,
                              sizeof err) != 0) {
        snprintf(s->preset_msg, sizeof s->preset_msg, "%s", err);
        return;
    }
    snprintf(s->preset_msg, sizeof s->preset_msg, "Imported %s; switching to it.", name);
    s->preset_source[0] = '\0';
    katoshe_copy_str(s->pending_preset, sizeof s->pending_preset, name);
}

static void add_repo(gui_state *s) {
    char owner[128], repo_name[128], name[128];
    katoshe_repo *grown;
    katoshe_repo *r;

    if (!s->new_url[0]) {
        katoshe_copy_str(s->err, sizeof s->err, "Enter a repository URL first.");
        return;
    }
    if (gh_parse_url(s->new_url, owner, sizeof owner, repo_name, sizeof repo_name) != 0) {
        katoshe_copy_str(s->err, sizeof s->err,
                         "That is not a GitHub repository. Use https://github.com/owner/repo.");
        return;
    }
    snprintf(name, sizeof name, "%s/%s", owner, repo_name);
    for (int i = 0; i < s->cfg.repo_count; i++) {
        char o2[128], r2[128];
        if (gh_parse_url(s->cfg.repos[i].url, o2, sizeof o2, r2, sizeof r2) == 0 &&
            strcmp(o2, owner) == 0 && strcmp(r2, repo_name) == 0) {
            katoshe_copy_str(s->err, sizeof s->err, "That repository is already in the preset.");
            return;
        }
    }

    grown = realloc(s->cfg.repos, sizeof *grown * (size_t)(s->cfg.repo_count + 1));
    if (!grown) {
        katoshe_copy_str(s->err, sizeof s->err, "Out of memory.");
        return;
    }
    s->cfg.repos = grown;
    r = &s->cfg.repos[s->cfg.repo_count];
    memset(r, 0, sizeof *r);
    katoshe_copy_str(r->name, sizeof r->name, s->new_name[0] ? s->new_name : name);
    katoshe_copy_str(r->url, sizeof r->url, s->new_url);
    r->include_prerelease = -1;
    s->cfg.repo_count++;
    s->new_url[0] = '\0';
    s->new_name[0] = '\0';
    s->err[0] = '\0';
    s->dirty = 1;
}

static void remove_repo(gui_state *s, int index) {
    if (index < 0 || index >= s->cfg.repo_count) return;
    free(s->cfg.repos[index].assets);
    memmove(&s->cfg.repos[index], &s->cfg.repos[index + 1],
            sizeof *s->cfg.repos * (size_t)(s->cfg.repo_count - index - 1));
    s->cfg.repo_count--;
    if (s->sel_repo >= s->cfg.repo_count && s->sel_repo > 0) s->sel_repo--;
    s->dirty = 1;
}

/* Adds or drops a selection whose pattern is one exact asset name. */
static void toggle_selection(gui_state *s, katoshe_repo *r, const char *name, int on) {
    int found = -1;

    for (int i = 0; i < r->asset_count; i++) {
        if (strcmp(r->assets[i].match, name) == 0) found = i;
    }
    if (on && found < 0) {
        katoshe_asset *grown =
            realloc(r->assets, sizeof *grown * (size_t)(r->asset_count + 1));
        if (!grown) return;
        r->assets = grown;
        memset(&r->assets[r->asset_count], 0, sizeof *r->assets);
        katoshe_copy_str(r->assets[r->asset_count].match, sizeof r->assets[0].match, name);
        r->assets[r->asset_count].mode = KATOSHE_MODE_REPLACE;
        r->assets[r->asset_count].keep_versions = 3;
        r->asset_count++;
        s->dirty = 1;
    } else if (!on && found >= 0) {
        memmove(&r->assets[found], &r->assets[found + 1],
                sizeof *r->assets * (size_t)(r->asset_count - found - 1));
        r->asset_count--;
        s->dirty = 1;
    }
}

static void add_selection(gui_state *s, katoshe_repo *r) {
    katoshe_asset *grown;

    if (!s->new_match[0]) return;
    grown = realloc(r->assets, sizeof *grown * (size_t)(r->asset_count + 1));
    if (!grown) return;
    r->assets = grown;
    memset(&r->assets[r->asset_count], 0, sizeof *r->assets);
    katoshe_copy_str(r->assets[r->asset_count].match, sizeof r->assets[0].match, s->new_match);
    r->assets[r->asset_count].mode = KATOSHE_MODE_REPLACE;
    r->assets[r->asset_count].keep_versions = 3;
    r->asset_count++;
    s->new_match[0] = '\0';
    s->dirty = 1;
}

static void remove_selection(gui_state *s, katoshe_repo *r, int index) {
    if (index < 0 || index >= r->asset_count) return;
    memmove(&r->assets[index], &r->assets[index + 1],
            sizeof *r->assets * (size_t)(r->asset_count - index - 1));
    r->asset_count--;
    s->dirty = 1;
}

/* ------------------------------------------------------------------------- */
/* Pages                                                                     */
/* ------------------------------------------------------------------------- */

static const char *const page_names[] = {"Repositories", "Assets", "Run", "Settings"};
static const char *const theme_names[] = {"system", "light", "dark"};
static const char *const mode_names[] = {"replace", "archive"};
static const char *const prerelease_names[] = {"inherit", "stable", "prerelease"};

static void ui_header(cui_ctx *ui, gui_state *s, const job_view *v) {
    cui_row(ui, .width = CUI_FILL, .pad = 16, .gap = 12, .align = CUI_ALIGN_CENTER) {
        cui_heading(ui, "Katoshe", 3);
        cui_badge(ui, s->cfg.preset, .tone = CUI_TONE_ACCENT);
        cui_muted(ui, katoshe_version());
        cui_spacer(ui);
        if (v->running) {
            cui_spinner(ui, 16);
            cui_muted(ui, v->kind == JOB_FETCH ? "reading release" : "working");
        }
        if (cui_button(ui, "Check", .icon = CUI_ICON_REFRESH_CW, .disabled = v->running)) {
            job_start(s, JOB_CHECK);
        }
        if (cui_button(ui, "Update", .icon = CUI_ICON_DOWNLOAD, .variant = CUI_BUTTON_PRIMARY,
                       .disabled = v->running || s->cfg.repo_count == 0)) {
            job_start(s, JOB_UPDATE);
        }
    }
}

static void page_repositories(cui_ctx *ui, gui_state *s, const job_view *v) {
    int remove = -1, open = -1;
    int running = v->running;

    cui_row(ui, .width = CUI_FILL, .gap = 8, .align = CUI_ALIGN_END) {
        cui_input(ui, "new-url", s->new_url, sizeof s->new_url, .label = "Repository URL",
                  .placeholder = "https://github.com/owner/repo", .width = CUI_GROW(1),
                  .disabled = running);
        cui_input(ui, "new-name", s->new_name, sizeof s->new_name, .label = "Name",
                  .placeholder = "owner/repo", .width = CUI_PX(200), .disabled = running);
        if (cui_button(ui, "Add", .icon = CUI_ICON_PLUS, .disabled = running)) add_repo(s);
    }

    if (s->err[0]) cui_text(ui, s->err, .color = CUI_COLOR_DANGER, .size = 13);
    cui_separator(ui);

    if (s->cfg.repo_count == 0) {
        cui_empty_state(ui, "No repositories yet",
                        .icon = CUI_ICON_GIT_BRANCH,
                        .description = "Add one above, or import a preset with "
                                       "`katoshe-cli preset import`.");
        return;
    }

    cui_scroll(ui, .id = "repos", .width = CUI_FILL, .height = CUI_GROW(1), .gap = 8, .pad = 2) {
        for (int i = 0; i < s->cfg.repo_count; i++) {
            katoshe_repo *r = &s->cfg.repos[i];
            int prerelease = r->include_prerelease + 1;
            char assets_text[32];

            cui_push_id_int(ui, i);
            cui_card(ui, .width = CUI_FILL, .pad = 12, .gap = 8) {
                cui_row(ui, .width = CUI_FILL, .gap = 8, .align = CUI_ALIGN_CENTER) {
                    cui_label(ui, r->name);
                    snprintf(assets_text, sizeof assets_text, "%d asset%s", r->asset_count,
                             r->asset_count == 1 ? "" : "s");
                    cui_badge(ui, assets_text, .tone = CUI_TONE_NEUTRAL);
                    cui_spacer(ui);
                    if (cui_button(ui, "Assets", .size = CUI_SIZE_SM, .icon = CUI_ICON_LIST)) {
                        open = i;
                    }
                    if (cui_icon_button(ui, CUI_ICON_TRASH_2, .variant = CUI_BUTTON_GHOST,
                                        .tooltip = "Remove this repository", .disabled = running)) {
                        remove = i;
                    }
                }
                if (cui_input(ui, "url", r->url, sizeof r->url, .label = "URL",
                              .width = CUI_FILL, .disabled = running)) {
                    s->dirty = 1;
                }
                cui_row(ui, .width = CUI_FILL, .gap = 8, .align = CUI_ALIGN_END) {
                    if (cui_input(ui, "dir", r->dir, sizeof r->dir, .label = "Subdirectory",
                                  .placeholder = "default", .width = CUI_PX(220),
                                  .disabled = running)) {
                        s->dirty = 1;
                    }
                    if (cui_input(ui, "proxy", r->proxy, sizeof r->proxy,
                                  .label = "Proxy override", .placeholder = "inherit",
                                  .width = CUI_GROW(1), .disabled = running)) {
                        s->dirty = 1;
                    }
                    cui_select(ui, "prerelease", prerelease_names, 3, &prerelease,
                               .label = "Releases", .width = CUI_PX(160), .disabled = running);
                }
            }
            cui_pop_id(ui);

            if (prerelease - 1 != r->include_prerelease) {
                r->include_prerelease = prerelease - 1;
                s->dirty = 1;
            }
        }
    }

    if (remove >= 0) remove_repo(s, remove);
    if (open >= 0) {
        s->sel_repo = open;
        s->page = PAGE_ASSETS;
    }
}

/* True when any selection of the repository matches the asset name. */
static int matched(katoshe_repo *r, const char *name) {
    for (int i = 0; i < r->asset_count; i++) {
        if (strcmp(r->assets[i].match, name) == 0) return 1;
    }
    return 0;
}

/* True when a wildcard selection already takes this asset. */
static int covered(katoshe_repo *r, const char *name) {
    for (int i = 0; i < r->asset_count; i++) {
        if (katoshe_match(r->assets[i].match, name)) return 1;
    }
    return 0;
}

static int has_ci(const char *hay, const char *needle) {
    size_t n = strlen(needle);

    if (n == 0) return 0;
    for (; *hay; hay++) {
        if (_strnicmp(hay, needle, n) == 0) return 1;
    }
    return 0;
}

static int platform_of(const char *name) {
    for (int p = 0; p < PLATFORM_COUNT; p++) {
        for (int k = 0; platform_rules[p].keys[k]; k++) {
            if (has_ci(name, platform_rules[p].keys[k])) return p;
        }
    }
    return -1;
}

static int platform_hit(const gui_state *s, const char *name) {
    int p = platform_of(name);

    return p >= 0 && (s->platform_mask & (1 << p)) != 0;
}

/* Assets the quick pick would add: platform hit, not already selected by name
   and not already taken by a wildcard. */
static int platform_candidates(gui_state *s, katoshe_repo *r, const job_view *v) {
    int n = 0;

    for (int i = 0; i < v->fetch_count; i++) {
        const char *name = v->fetch[i].name;
        if (!platform_hit(s, name)) continue;
        if (matched(r, name) || covered(r, name)) continue;
        n++;
    }
    return n;
}

static void select_platform_assets(gui_state *s, katoshe_repo *r, const job_view *v) {
    for (int i = 0; i < v->fetch_count; i++) {
        const char *name = v->fetch[i].name;
        if (!platform_hit(s, name)) continue;
        if (matched(r, name) || covered(r, name)) continue;
        toggle_selection(s, r, name, 1);
    }
}

static void page_assets(cui_ctx *ui, gui_state *s, const job_view *v) {
    const char *names[64];
    int count = s->cfg.repo_count;
    int sel;
    int remove_sel = -1;
    int running = v->running;
    katoshe_repo *r;

    if (count == 0) {
        cui_empty_state(ui, "Nothing to select", .icon = CUI_ICON_LIST,
                        .description = "Add a repository on the Repositories page first.");
        return;
    }
    if (count > 64) count = 64;
    for (int i = 0; i < count; i++) names[i] = s->cfg.repos[i].name;
    sel = s->sel_repo < count ? s->sel_repo : 0;

    cui_row(ui, .width = CUI_FILL, .gap = 8, .align = CUI_ALIGN_END) {
        cui_select(ui, "repo", names, count, &sel, .label = "Repository", .width = CUI_PX(320));
        if (cui_button(ui, "Fetch release assets", .icon = CUI_ICON_REFRESH_CW,
                       .disabled = running)) {
            AcquireSRWLockExclusive(&s->job.lock);
            s->job.fetch_repo = sel;
            ReleaseSRWLockExclusive(&s->job.lock);
            job_start(s, JOB_FETCH);
        }
        cui_spacer(ui);
        cui_muted(ui, "Only the checked assets are downloaded.");
    }
    if (sel != s->sel_repo) s->sel_repo = sel;
    r = &s->cfg.repos[sel];

    if (v->fetch_valid && v->fetch_repo == sel) {
        int order[REMOTE_MAX];
        int order_count = 0;
        int candidates = platform_candidates(s, r, v);

        cui_row(ui, .width = CUI_FILL, .gap = 8, .align = CUI_ALIGN_CENTER) {
            cui_label(ui, v->fetch_tag);
            cui_badge(ui, "target release", .tone = CUI_TONE_SUCCESS);
            cui_muted(ui, "Tick an asset to download it on every update.");
        }

        cui_row(ui, .width = CUI_FILL, .gap = 6, .align = CUI_ALIGN_CENTER) {
            cui_muted(ui, "Platforms");
            for (int p = 0; p < PLATFORM_COUNT; p++) {
                bool on = (s->platform_mask & (1 << p)) != 0;
                if (cui_chip(ui, platform_rules[p].chip, &on)) {
                    set_platforms(s, s->platform_mask ^ (1 << p));
                }
            }
            cui_spacer(ui);
            if (s->platform_mask && candidates > 0) {
                char action[64];
                snprintf(action, sizeof action, "Select %d matching", candidates);
                if (cui_button(ui, action, .icon = CUI_ICON_CHECK, .disabled = running)) {
                    select_platform_assets(s, r, v);
                }
            }
        }

        /* Chosen platforms float to the top; the rest keep their order. */
        for (int pass = 0; pass < 2; pass++) {
            for (int i = 0; i < v->fetch_count; i++) {
                int hit = platform_hit(s, v->fetch[i].name) != 0;
                if (hit == (pass == 0)) order[order_count++] = i;
            }
        }

        cui_scroll(ui, .id = "remote", .width = CUI_FILL,
                   .height = CUI_PX(v->fetch_count * 34.0f + 16.0f > 320.0f
                                        ? 320.0f
                                        : v->fetch_count * 34.0f + 16.0f),
                   .gap = 2) {
            for (int n = 0; n < order_count; n++) {
                int i = order[n];
                char label[400];
                char size_text[32];
                bool on = matched(r, v->fetch[i].name) != 0;
                const char *pattern = NULL;
                int platform = platform_hit(s, v->fetch[i].name) ? platform_of(v->fetch[i].name)
                                                                 : -1;

                if (!on) {
                    for (int k = 0; k < r->asset_count; k++) {
                        if (katoshe_match(r->assets[k].match, v->fetch[i].name)) {
                            pattern = r->assets[k].match;
                            break;
                        }
                    }
                }
                gh_format_size(v->fetch[i].size, size_text, sizeof size_text);
                snprintf(label, sizeof label, "%s  (%s)", v->fetch[i].name, size_text);
                cui_push_id_int(ui, i);
                cui_row(ui, .width = CUI_FILL, .gap = 8, .align = CUI_ALIGN_CENTER) {
                    if (pattern) {
                        cui_text(ui, CUI_ICON_CHECK, .color = CUI_COLOR_SUCCESS);
                        cui_text(ui, label);
                        cui_textf(ui, "matched by %s", pattern);
                    } else if (cui_checkbox(ui, label, &on)) {
                        toggle_selection(s, r, v->fetch[i].name, on);
                    }
                    if (platform >= 0) {
                        cui_badge(ui, platform_rules[platform].badge, .tone = CUI_TONE_INFO);
                    }
                }
                cui_pop_id(ui);
            }
        }
    } else if (v->fetch_error[0] && v->fetch_repo == sel && !running) {
        cui_text(ui, v->fetch_error, .color = CUI_COLOR_DANGER, .size = 13);
    } else {
        cui_muted(ui, "Fetch the release to see its assets as checkboxes, or add a "
                      "wildcard selection below.");
    }

    cui_separator(ui);

    cui_row(ui, .width = CUI_FILL, .gap = 8, .align = CUI_ALIGN_END) {
        cui_input(ui, "new-match", s->new_match, sizeof s->new_match, .label = "Add a selection",
                  .placeholder = "*.apk or an exact asset name", .width = CUI_GROW(1),
                  .disabled = running);
        if (cui_button(ui, "Add", .icon = CUI_ICON_PLUS, .disabled = running)) {
            add_selection(s, r);
        }
    }

    if (r->asset_count == 0) {
        cui_muted(ui, "No selections for this repository.");
        return;
    }

    cui_scroll(ui, .id = "selections", .width = CUI_FILL, .height = CUI_GROW(1), .gap = 8,
               .pad = 2) {
        for (int i = 0; i < r->asset_count; i++) {
            katoshe_asset *a = &r->assets[i];
            int mode = a->mode == KATOSHE_MODE_ARCHIVE ? 1 : 0;
            double keep = a->keep_versions > 0 ? (double)a->keep_versions : 3;
            char status_key[KEY_LEN];

            snprintf(status_key, sizeof status_key, "%s/%s", r->name, a->match);

            cui_push_id_int(ui, i);
            cui_card(ui, .width = CUI_FILL, .pad = 12, .gap = 8) {
                cui_row(ui, .width = CUI_FILL, .gap = 8, .align = CUI_ALIGN_CENTER) {
                    cui_label(ui, a->match);
                    for (int k = 0; k < v->status_count; k++) {
                        if (strcmp(v->status[k].key, status_key) == 0 &&
                            v->status[k].action >= 0) {
                            cui_badge(ui, katoshe_plan_action_name((plan_action)v->status[k].action),
                                      .tone = v->status[k].action == PLAN_MISSING
                                                  ? CUI_TONE_DANGER
                                                  : (v->status[k].action == PLAN_UNCHANGED
                                                         ? CUI_TONE_SUCCESS
                                                         : CUI_TONE_INFO));
                        }
                    }
                    cui_spacer(ui);
                    if (cui_icon_button(ui, CUI_ICON_TRASH_2, .variant = CUI_BUTTON_GHOST,
                                        .tooltip = "Remove this selection",
                                        .disabled = running)) {
                        remove_sel = i;
                    }
                }
                cui_row(ui, .width = CUI_FILL, .gap = 8, .align = CUI_ALIGN_END) {
                    if (cui_input(ui, "match", a->match, sizeof a->match, .label = "Match",
                                  .width = CUI_GROW(1), .disabled = running)) {
                        s->dirty = 1;
                    }
                    if (cui_input(ui, "filename", a->filename, sizeof a->filename,
                                  .label = "File name", .placeholder = "remote name",
                                  .width = CUI_PX(240), .hint = "replace mode only",
                                  .disabled = running)) {
                        s->dirty = 1;
                    }
                }
                cui_row(ui, .width = CUI_FILL, .gap = 8, .align = CUI_ALIGN_END) {
                    if (cui_input(ui, "dir", a->dir, sizeof a->dir, .label = "Subdirectory",
                                  .placeholder = "repository directory", .width = CUI_GROW(1),
                                  .disabled = running)) {
                        s->dirty = 1;
                    }
                    if (cui_select(ui, "mode", mode_names, 2, &mode, .label = "Mode",
                                   .width = CUI_PX(140), .disabled = running)) {
                        a->mode = mode == 1 ? KATOSHE_MODE_ARCHIVE : KATOSHE_MODE_REPLACE;
                        s->dirty = 1;
                    }
                    if (mode == 1) {
                        if (cui_number(ui, "keep", &keep, 1, 99, .label = "Versions to keep",
                                       .width = CUI_PX(150), .disabled = running)) {
                            a->keep_versions = (int)keep;
                            s->dirty = 1;
                        }
                    }
                }
            }
            cui_pop_id(ui);

            if (mode != a->mode) {
                a->mode = mode;
                s->dirty = 1;
            }
        }
    }

    if (remove_sel >= 0) remove_selection(s, r, remove_sel);
}

static const char *status_note(const gui_status *st) {
    return st->note[0] ? st->note : "";
}

static void page_run(cui_ctx *ui, gui_state *s, const job_view *v) {
    static bool check_only;
    int running = v->running;

    cui_row(ui, .width = CUI_FILL, .gap = 12, .align = CUI_ALIGN_CENTER) {
        cui_switch(ui, "Check only (write nothing)", &check_only);
        cui_spacer(ui);
        if (cui_button(ui, running ? "Running" : "Run", .icon = CUI_ICON_PLAY,
                       .variant = CUI_BUTTON_PRIMARY, .disabled = running || s->cfg.repo_count == 0,
                       .loading = running)) {
            job_start(s, check_only ? JOB_CHECK : JOB_UPDATE);
        }
        if (cui_button(ui, "Cancel", .icon = CUI_ICON_X, .variant = CUI_BUTTON_OUTLINE,
                       .disabled = !running)) {
            job_cancel(s);
        }
    }

    if (running) {
        cui_progress(ui, v->total > 0 ? (float)v->received / (float)v->total : 0.0f,
                     .label = v->current[0] ? v->current : "starting",
                     .show_value = v->total > 0);
    } else if (v->done && v->exit_code == 0) {
        cui_text(ui, "The last run finished without errors.", .color = CUI_COLOR_SUCCESS);
    }

    if (v->status_count == 0) {
        cui_muted(ui, "Nothing has run yet. Check reports what would change; Update downloads it.");
    }

    cui_scroll(ui, .id = "status", .width = CUI_FILL, .height = CUI_GROW(1), .gap = 2, .pad = 2) {
        for (int i = 0; i < v->status_count; i++) {
            const gui_status *st = &v->status[i];
            cui_row(ui, .width = CUI_FILL, .gap = 8, .align = CUI_ALIGN_CENTER) {
                const char *action = st->action >= 0 ? katoshe_plan_action_name((plan_action)st->action)
                                                     : "pending";
                cui_badge(ui, action,
                          .tone = st->action == PLAN_DOWNLOAD
                                      ? CUI_TONE_INFO
                                      : (st->action == PLAN_UNCHANGED
                                             ? CUI_TONE_SUCCESS
                                             : (st->action == PLAN_MISSING ? CUI_TONE_DANGER
                                                                           : CUI_TONE_NEUTRAL)));
                cui_text(ui, st->key, .nowrap = true);
                cui_spacer(ui);
                if (st->total > 0) {
                    char size_text[32];
                    gh_format_size(st->received, size_text, sizeof size_text);
                    cui_muted(ui, size_text);
                } else if (status_note(st)[0]) {
                    cui_muted(ui, status_note(st));
                }
            }
        }
    }

    cui_separator(ui);
    cui_label(ui, "Log");
    cui_scroll(ui, .id = "log", .width = CUI_FILL, .height = CUI_GROW(2), .pad = 8,) {
        int first = v->log_count > 20 ? v->log_count - 20 : 0;
        if (v->log_count == 0) cui_muted(ui, "No output yet.");
        for (int i = first; i < v->log_count; i++) {
            cui_text(ui, v->log[i], .font = CUI_FONT_MONO, .size = 12, .nowrap = true);
        }
    }
}

static void page_settings(cui_ctx *ui, gui_state *s, const job_view *v) {
    int running = v->running;
    double jobs = (double)s->cfg.global.jobs;

    cui_scroll(ui, .id = "settings", .width = CUI_FILL, .height = CUI_GROW(1), .gap = 16) {
        cui_card(ui, .width = CUI_FILL, .pad = 16, .gap = 10) {
            cui_label(ui, "Files");
            cui_input(ui, "root", s->root_input, sizeof s->root_input, .label = "Root folder",
                      .width = CUI_FILL, .disabled = running,
                      .hint = "Where config, data and downloads live. Portable by default.");
            cui_row(ui, .width = CUI_FILL, .gap = 8) {
                if (cui_button(ui, "Choose folder", .icon = CUI_ICON_FOLDER,
                               .disabled = running)) {
                    char picked[KATOSHE_PATH_MAX] = "";
                    if (cui_app_file_dialog(s->app,
                                            &(cui_file_dialog_opts){.kind = CUI_FILE_FOLDER,
                                                                    .title = "Choose the "
                                                                             "Katoshe root folder"},
                                            picked, sizeof picked) &&
                        picked[0]) {
                        katoshe_copy_str(s->root_input, sizeof s->root_input, picked);
                        gui_load(s);
                    }
                }
                if (cui_button(ui, "Use this folder", .disabled = running)) gui_load(s);
            }
            cui_muted(ui, s->paths.portable ? "portable: config sits next to the exe"
                                            : "installed: config sits in %APPDATA%\\Katoshe");
            cui_textf(ui, "config   %s", s->paths.config_dir);
            cui_textf(ui, "data     %s", s->paths.data_dir);
            cui_textf(ui, "downloads %s", s->download_dir);
        }

        cui_card(ui, .width = CUI_FILL, .pad = 16, .gap = 10) {
            const char *preset_names[64];
            int preset_count = s->preset_count;
            int choice = s->preset_choice;

            cui_label(ui, "Presets");
            cui_muted(ui, "A preset is the part you share: repositories and assets only. "
                          "Switching one reloads the repository list.");
            if (preset_count > 64) preset_count = 64;
            for (int i = 0; i < preset_count; i++) preset_names[i] = s->presets[i].name;
            cui_row(ui, .width = CUI_FILL, .gap = 8, .align = CUI_ALIGN_END) {
                if (preset_count > 0) {
                    if (cui_select(ui, "preset", preset_names, preset_count, &choice,
                                   .label = "Active preset", .width = CUI_PX(260),
                                   .disabled = running) &&
                        choice >= 0 && choice < preset_count) {
                        katoshe_copy_str(s->pending_preset, sizeof s->pending_preset,
                                         s->presets[choice].name);
                    }
                } else {
                    cui_muted(ui, "No preset files next to the settings yet.");
                }
                cui_input(ui, "preset-source", s->preset_source, sizeof s->preset_source,
                          .label = "Import", .placeholder = "file, URL or katoshe:// link",
                          .width = CUI_GROW(1), .disabled = running);
                if (cui_icon_button(ui, CUI_ICON_FOLDER, .variant = CUI_BUTTON_GHOST,
                                    .tooltip = "Pick a preset file",
                                    .disabled = running)) {
                    char picked[KATOSHE_PATH_MAX] = "";
                    if (cui_app_file_dialog(s->app,
                                            &(cui_file_dialog_opts){.kind = CUI_FILE_OPEN,
                                                                    .title = "Open a preset",
                                                                    .filter_name = "Preset",
                                                                    .filter = "json"},
                                            picked, sizeof picked) &&
                        picked[0]) {
                        katoshe_copy_str(s->preset_source, sizeof s->preset_source, picked);
                        import_preset(s);
                    }
                }
                if (cui_button(ui, "Import", .icon = CUI_ICON_PLUS, .disabled = running)) {
                    import_preset(s);
                }
            }
            if (s->pending_preset[0]) {
                cui_muted(ui, "Switching preset…");
            } else if (s->preset_msg[0]) {
                cui_muted(ui, s->preset_msg);
            }
        }

        cui_card(ui, .width = CUI_FILL, .pad = 16, .gap = 10) {
            cui_label(ui, "Appearance");
            int theme = s->theme;
            if (cui_select(ui, "theme", theme_names, 3, &theme, .label = "Theme",
                           .width = CUI_PX(200))) {
                set_theme(s, theme);
            }
            cui_muted(ui, "Saved in settings.json; `system` follows the Windows setting.");
        }

        cui_card(ui, .width = CUI_FILL, .pad = 16, .gap = 10) {
            cui_label(ui, "Network");
            if (cui_input(ui, "proxy", s->cfg.global.proxy, sizeof s->cfg.global.proxy,
                          .label = "Proxy", .placeholder = "socks5h://127.0.0.1:10808",
                          .width = CUI_FILL, .disabled = running,
                          .hint = "Per-repository overrides win over this one.")) {
                s->dirty = 1;
            }
            if (cui_input(ui, "token", s->token_input, sizeof s->token_input,
                          .label = "GitHub token", .password = true, .clearable = true,
                          .width = CUI_FILL, .disabled = running,
                          .hint = "Stored in config/secrets.json. No scopes are enough for "
                                  "public repositories.")) {
                s->token_dirty = 1;
            }
            if (cui_number(ui, "jobs", &jobs, 1, 16, .label = "Parallel downloads",
                           .width = CUI_PX(200), .disabled = running,
                           .suffix = "files")) {
                s->cfg.global.jobs = (int)jobs;
                s->dirty = 1;
            }
            if (cui_switch(ui, "Include pre-releases by default",
                           &s->cfg.global.include_prerelease)) {
                s->dirty = 1;
            }
            if (cui_switch(ui, "Check on launch", &s->cfg.global.check_on_launch)) s->dirty = 1;
            if (cui_input(ui, "log-level", s->cfg.global.log_level, sizeof s->cfg.global.log_level,
                          .label = "Log level", .placeholder = "info", .width = CUI_PX(200),
                          .disabled = running)) {
                s->dirty = 1;
            }
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Frame                                                                     */
/* ------------------------------------------------------------------------- */

static void frame(cui_ctx *ui, void *user) {
    gui_state *s = user;
    int finished = 0;

    view_take(s);

    if (s->theme != s->theme_applied) {
        s->theme_applied = s->theme;
        if (s->theme == 0) {
            cui_app_follow_system_theme(s->app, true);
        } else {
            cui_app_follow_system_theme(s->app, false);
            cui_set_theme(ui, s->theme == 2 ? CUI_THEME_DARK : CUI_THEME_LIGHT);
        }
    }

    if (view.done) {
        finished = 1;
        if (view.exit_code == 0) {
            cui_toast(ui, view.kind == JOB_FETCH ? "Release assets fetched"
                                                : (view.kind == JOB_CHECK ? "Check finished"
                                                                          : "Update finished"),
                      .tone = CUI_TONE_SUCCESS);
        } else {
            cui_toast(ui, "Finished with errors", .description = "See the Run page.",
                      .tone = CUI_TONE_DANGER);
        }
        if (view.kind != JOB_FETCH && !s->no_jump) s->page = PAGE_RUN;
    }

    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .bg = CUI_COLOR_BACKGROUND) {
        ui_header(ui, s, &view);
        cui_row(ui, .width = CUI_FILL, .pad_x = 16) {
            cui_tabs(ui, "pages", page_names, 4, &s->page, .full_width = false);
        }
        cui_separator(ui);
        cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .pad = 16, .gap = 12) {
            if (s->page == PAGE_REPOS) page_repositories(ui, s, &view);
            else if (s->page == PAGE_ASSETS) page_assets(ui, s, &view);
            else if (s->page == PAGE_RUN) page_run(ui, s, &view);
            else page_settings(ui, s, &view);
        }
    }

    if (s->pending_preset[0]) {
        char name[KATOSHE_PRESET_MAX];
        char err[512];

        katoshe_copy_str(name, sizeof name, s->pending_preset);
        s->pending_preset[0] = '\0';
        if (point_at_preset(s, name, err, sizeof err) == 0) {
            gui_load(s);
        } else {
            katoshe_copy_str(s->err, sizeof s->err, err);
        }
    } else if (s->dirty || s->token_dirty) {
        gui_save(s);
    }
    if (view.running) cui_request_wakeup(ui, 0.1f);
    if (finished) cui_request_redraw(ui);
}

int main(int argc, char **argv) {
    static gui_state s;
    cui_app_desc desc;
    cui_app *app;
    int exit_code;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--root") == 0 && i + 1 < argc) {
            katoshe_copy_str(s.root_input, sizeof s.root_input, argv[++i]);
        } else if (strcmp(argv[i], "--preset") == 0 && i + 1 < argc) {
            katoshe_copy_str(s.preset_override, sizeof s.preset_override, argv[++i]);
        } else if (strcmp(argv[i], "--page") == 0 && i + 1 < argc) {
            const char *page = argv[++i];
            if (strcmp(page, "assets") == 0) s.page = PAGE_ASSETS;
            else if (strcmp(page, "run") == 0) s.page = PAGE_RUN;
            else if (strcmp(page, "settings") == 0) s.page = PAGE_SETTINGS;
            else s.page = PAGE_REPOS;
            s.no_jump = 1;
        }
    }

    memset(&desc, 0, sizeof desc);
    desc.title = "Katoshe";
    desc.width = 1060;
    desc.height = 760;
    desc.min_width = 720;
    desc.min_height = 480;

    app = cui_app_create(&desc);
    if (!app) {
        fprintf(stderr, "error: cannot open the window\n");
        return 2;
    }
    s.app = app;
    gui_load(&s);

    if (s.page == PAGE_ASSETS && s.cfg.repo_count > 0) {
        /* Opening straight on Assets means the release list should be there. */
        s.job.fetch_repo = s.sel_repo;
        job_start(&s, JOB_FETCH);
    } else if (s.cfg.global.check_on_launch && s.cfg.repo_count > 0) {
        job_start(&s, JOB_CHECK);
    }

    while (cui_app_frame(app)) frame(cui_app_ui(app), &s);

    job_cancel(&s);
    if (s.job.thread) {
        WaitForSingleObject(s.job.thread, 5000);
        CloseHandle(s.job.thread);
    }
    katoshe_config_free(&s.cfg);
    katoshe_state_free(&s.state);
    exit_code = 0;
    cui_app_destroy(app);
    return exit_code;
}
