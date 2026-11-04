// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/app_control/import_files.h>
#include <dsd-neo/core/csv_validate.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/radioreference_import.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define IMPORT_PATH_SEP '\\'
#else
#define IMPORT_PATH_SEP '/'
#endif

typedef struct {
    const char* dir;
    dsd_app_import_visit_fn visit;
    void* user;
    char path[DSD_APP_IMPORT_PATH_MAX]; /* the entry's path, one buffer for the whole walk */
    dsd_rr_provenance prov;             /* its sidecar, likewise */
} import_list_ctx;

/* dsd_dir_list() callback: every ".csv" with a readable sidecar goes to the visitor. */
static int
import_list_entry(const char* name, void* user) {
    import_list_ctx* ctx = (import_list_ctx*)user;
    const size_t len = (name != NULL) ? strlen(name) : 0U;
    if (len < 5U || strcmp(name + (len - 4U), ".csv") != 0) {
        return 0;
    }
    const int n = DSD_SNPRINTF(ctx->path, sizeof ctx->path, "%s%c%s", ctx->dir, IMPORT_PATH_SEP, name);
    if (n <= 0 || (size_t)n >= sizeof ctx->path) {
        return 0; /* a truncated path would name a different file */
    }
    DSD_MEMSET(&ctx->prov, 0, sizeof ctx->prov);
    if (dsd_rr_provenance_read(ctx->path, &ctx->prov) != 0) {
        return 0; /* no sidecar: not a managed import */
    }
    return ctx->visit(ctx->path, name, &ctx->prov, ctx->user);
}

int
dsd_app_imports_list(const char* dir, dsd_app_import_visit_fn visit, void* user) {
    if (dir == NULL || dir[0] == '\0' || visit == NULL) {
        return -1;
    }
    import_list_ctx ctx;
    DSD_MEMSET(&ctx, 0, sizeof ctx);
    ctx.dir = dir;
    ctx.visit = visit;
    ctx.user = user;
    if (dsd_dir_list(dir, import_list_entry, &ctx) == 0) {
        return 0;
    }
    /* An imports directory that does not exist yet is the first-run state, not a fault: dsd_user_imports_dir() only
       resolves a path, and nothing creates it until the first import completes. A directory that IS there and still
       could not be walked is a real fault. */
    dsd_stat_t st;
    return (dsd_stat_path(dir, &st) != 0) ? 1 : -1;
}

int
dsd_app_imports_dir_create(void) {
    return dsd_user_imports_dir_create() == 0 ? 0 : -1;
}

int
dsd_app_import_remove(const char* csv_path) {
    if (csv_path == NULL || csv_path[0] == '\0') {
        return 0;
    }
    /* Truncation is rejected rather than shortened: a shortened path names a DIFFERENT file and this hands it straight
       to remove(). */
    char side[DSD_APP_IMPORT_PATH_MAX + 4];
    const int gone = (remove(csv_path) == 0) ? 1 : 0;
    const int n = DSD_SNPRINTF(side, sizeof side, "%s.rr", csv_path);
    if (n > 0 && (size_t)n < sizeof side) {
        (void)remove(side);
    }
    return gone;
}

/*
 * Write @p text into a private temp sibling of @p final_path, leaving its name in @p tmp. Never a bare fopen or rename:
 * semgrep dsd-neo.no-raw-file-open bans fopen/open/mkstemp under src/, and the helper picks the ".tmp.XXXXXX" name
 * itself, refusing with ENAMETOOLONG when it does not fit. "wb": a text-mode write turns the generator's '\n' into CRLF
 * on Windows, and a refreshed file would stop matching the imported one byte for byte.
 * Returns 0, or -1 with the temp file removed.
 */
static int
import_write_staging(const char* final_path, const char* text, size_t len, char* tmp, size_t tmp_size) {
    tmp[0] = '\0';
    FILE* fp = dsd_fopen_private_temp_for_replace(final_path, tmp, tmp_size, "wb");
    if (fp == NULL) {
        return -1;
    }
    int bad = (len > 0U && text != NULL && fwrite(text, 1U, len, fp) != len) ? 1 : 0;
    if (!bad && fflush(fp) != 0) {
        bad = 1;
    }
    const int fd = dsd_fileno(fp);
    if (!bad && fd >= 0 && dsd_fsync(fd) != 0) {
        bad = 1;
    }
    if (fclose(fp) != 0) {
        bad = 1;
    }
    if (bad) {
        (void)remove(tmp);
        return -1;
    }
    return 0;
}

int
dsd_app_import_write(const char* final_path, const char* text, size_t len) {
    if (final_path == NULL || final_path[0] == '\0') {
        return -1;
    }
    char tmp[DSD_APP_IMPORT_PATH_MAX];
    if (import_write_staging(final_path, text, len, tmp, sizeof tmp) != 0) {
        return -1;
    }
    /* A bare rename() fails on Windows whenever the destination exists, which is every re-import. */
    if (dsd_replace_file_with_temp(tmp, final_path) != 0) {
        (void)remove(tmp);
        return -1;
    }
    return 0;
}

int
dsd_app_import_replace_checked(const char* path, const char* text, size_t len, int is_chan) {
    if (path == NULL || text == NULL) {
        return -1;
    }
    char tmp[DSD_APP_IMPORT_PATH_MAX];
    if (import_write_staging(path, text, len, tmp, sizeof tmp) != 0) {
        return -1;
    }
    /* "accepted > 0", not only rc == 0: a header-only channel map validates cleanly with zero accepted rows, because
       chan_import_stats() (src/core/file/dsd_import.c) skips row 1 as the label line. dsd_csv_validate_*_file()
       allocate their own throwaway state. */
    dsd_csv_validation counts = {0U, 0U, 0U};
    const int rc = is_chan ? dsd_csv_validate_chan_file(tmp, &counts) : dsd_csv_validate_group_file(tmp, &counts);
    if (rc != 0 || counts.accepted == 0U) {
        (void)remove(tmp);
        return -2;
    }
    if (dsd_replace_file_with_temp(tmp, path) != 0) {
        (void)remove(tmp);
        return -1;
    }
    return 0;
}

int
dsd_app_import_provenance_read(const char* csv_path, dsd_rr_provenance* out) {
    if (csv_path == NULL || out == NULL) {
        return -1;
    }
    return dsd_rr_provenance_read(csv_path, out);
}

int
dsd_app_import_provenance_write(const char* csv_path, const dsd_rr_provenance* prov) {
    if (csv_path == NULL || prov == NULL) {
        return -1;
    }
    return dsd_rr_provenance_write(csv_path, prov);
}

int
dsd_app_import_path_conflicts(const char* path, int sid, const char* site_ids) {
    dsd_stat_t st;
    if (path == NULL || dsd_stat_path(path, &st) != 0) {
        return 0; /* nothing there; an orphan ".rr" with no CSV is not a conflict */
    }
    dsd_rr_provenance prov;
    DSD_MEMSET(&prov, 0, sizeof(prov));
    if (dsd_rr_provenance_read(path, &prov) != 0) {
        return 1; /* no readable sidecar: a hand-made user file, never overwritten */
    }
    /* Same system AND same sites: a re-import of this very file, which overwrites in place so a config reference keeps
       pointing at it. */
    return (prov.sid == sid && site_ids != NULL && strcmp(prov.site_ids, site_ids) == 0) ? 0 : 1;
}
