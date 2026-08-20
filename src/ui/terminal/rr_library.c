// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Curses-free model of the imports directory. See rr_library.h.
 */

#include "rr_library.h"

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/string_utils.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/runtime/radioreference_generate.h>
#include <dsd-neo/runtime/radioreference_import.h>

#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define RR_LIB_PATH_SEP '\\'
#else
#define RR_LIB_PATH_SEP '/'
#endif

/** @brief Name column width; longer names are truncated with "..". */
#define RR_LIB_NAME_COL 30

void
rr_library_init(RrLibrary* lib) {
    if (lib != NULL) {
        DSD_MEMSET(lib, 0, sizeof(*lib));
    }
}

/** @brief The system for @p sid, or NULL when it is not present yet. */
static RrLibrarySystem*
rr_library_find(RrLibrary* lib, int sid) {
    for (int i = 0; i < lib->count; i++) {
        if (lib->systems[i].sid == sid) {
            return &lib->systems[i];
        }
    }
    return NULL;
}

/**
 * @brief Whether this sidecar names a half the browser can act on.
 *
 * @param prov        Parsed provenance.
 * @param out_is_chan [out] 1 for the channel-map half, 0 for the group half.
 * @return 0 when actionable, -1 otherwise.
 */
static int
rr_library_classify(const dsd_rr_provenance* prov, int* out_is_chan) {
    /* sid is both the grouping key and what rr_wizard_core_begin_refresh()
       requires to be > 0. Without this, every sidecar missing a sid folds into
       one bogus row that no action can complete. */
    if (prov->sid <= 0) {
        return -1;
    }
    const int is_chan = (strcmp(prov->kind, "chan") == 0);
    if (!is_chan && strcmp(prov->kind, "group") != 0) {
        return -1; /* only the two halves an import writes are actionable */
    }
    *out_is_chan = is_chan;
    return 0;
}

/** @brief Find @p prov's system, or start one. NULL when the library is full. */
static RrLibrarySystem*
rr_library_intern(RrLibrary* lib, const dsd_rr_provenance* prov) {
    RrLibrarySystem* sys = rr_library_find(lib, prov->sid);
    if (sys != NULL) {
        return sys;
    }
    if (lib->count >= RR_LIBRARY_MAX) {
        lib->overflow = 1;
        return NULL;
    }
    sys = &lib->systems[lib->count++];
    DSD_MEMSET(sys, 0, sizeof(*sys));
    sys->sid = prov->sid;
    DSD_STRNCPY(sys->name, prov->system_name, sizeof(sys->name) - 1);
    sys->partial_enc_as_de = prov->partial_enc_as_de;
    return sys;
}

/** @brief Fold the system-wide fields either half may carry. */
static void
rr_library_merge_meta(RrLibrarySystem* sys, const dsd_rr_provenance* prov) {
    /* The recipe describes the system, so any half carrying one is authoritative;
       adopt it once and let a files-only half not overwrite it. */
    if (!sys->recipe.present && prov->recipe.present) {
        sys->recipe = prov->recipe;
    }
    /* A blank name from one half must not blank an already-known one. */
    if (sys->name[0] == '\0' && prov->system_name[0] != '\0') {
        DSD_STRNCPY(sys->name, prov->system_name, sizeof(sys->name) - 1);
    }
}

/**
 * @brief Record one half's path, newest sidecar wins.
 *
 * Newest-wins, not last-seen-wins: dsd_dir_list() reports entries in whatever
 * order the platform hands back, and two pairs can legitimately share a sid (a
 * renamed system re-imports under a new stem and leaves the old pair on disk).
 * Keying on the sidecar's own timestamp makes the row - and therefore which files
 * "Use this system", "Refresh" and "Delete" act on - the same on every run.
 */
static void
rr_library_record_half(RrLibrarySystem* sys, const char* csv_path, const dsd_rr_provenance* prov, int is_chan) {
    /* The two halves are spelled out rather than selected through a char* alias:
       DSD_STRNCPY bounds the copy with __builtin_object_size(dst, 1), which only
       sees the real size when the destination IS the array. Handing it a pointer
       silently shortens every path. */
    if (is_chan) {
        if (sys->has_chan && prov->imported_at < sys->chan_at) {
            return; /* an older duplicate: keep what is already recorded */
        }
        sys->has_chan = 1;
        sys->chan_at = prov->imported_at;
        DSD_STRNCPY(sys->chan_path, csv_path, sizeof(sys->chan_path) - 1);
        return;
    }
    if (sys->has_group && prov->imported_at < sys->group_at) {
        return;
    }
    sys->has_group = 1;
    sys->group_at = prov->imported_at;
    DSD_STRNCPY(sys->group_path, csv_path, sizeof(sys->group_path) - 1);
}

int
rr_library_add(RrLibrary* lib, const char* csv_path, const dsd_rr_provenance* prov) {
    if (lib == NULL || csv_path == NULL || prov == NULL) {
        return -1;
    }
    int is_chan = 0;
    if (rr_library_classify(prov, &is_chan) != 0) {
        return -1;
    }
    RrLibrarySystem* sys = rr_library_intern(lib, prov);
    if (sys == NULL) {
        return -1; /* full; overflow already flagged */
    }
    rr_library_merge_meta(sys, prov);
    rr_library_record_half(sys, csv_path, prov, is_chan);
    return 0;
}

typedef struct {
    RrLibrary* lib;
    char dir[1024];
} RrLibraryScanCtx;

/** @brief dsd_dir_list callback: fold every ".csv" that has a readable sidecar. */
static int
rr_library_scan_entry(const char* name, void* user) {
    RrLibraryScanCtx* ctx = (RrLibraryScanCtx*)user;
    const size_t len = (name != NULL) ? strlen(name) : 0U;
    if (len < 5U || strcmp(name + (len - 4U), ".csv") != 0) {
        return 0;
    }

    char path[1024];
    const int n = DSD_SNPRINTF(path, sizeof path, "%s%c%s", ctx->dir, RR_LIB_PATH_SEP, name);
    if (n <= 0 || (size_t)n >= sizeof path) {
        /* A truncated path would name a different file; skip rather than guess. */
        return 0;
    }

    dsd_rr_provenance prov;
    DSD_MEMSET(&prov, 0, sizeof prov);
    if (dsd_rr_provenance_read(path, &prov) != 0) {
        return 0; /* no sidecar: not a managed import */
    }
    (void)rr_library_add(ctx->lib, path, &prov);
    return 0;
}

int
rr_library_scan(RrLibrary* lib, const char* dir) {
    if (lib == NULL) {
        return -1;
    }
    rr_library_init(lib);
    if (dir == NULL || dir[0] == '\0') {
        return -1;
    }

    RrLibraryScanCtx ctx;
    ctx.lib = lib;
    const int n = DSD_SNPRINTF(ctx.dir, sizeof ctx.dir, "%s", dir);
    if (n <= 0 || (size_t)n >= sizeof ctx.dir) {
        return -1;
    }
    if (dsd_dir_list(dir, rr_library_scan_entry, &ctx) != 0) {
        /* An imports directory that does not exist yet is the first-run state,
           not a fault: dsd_user_imports_dir() only resolves a path (it does no
           I/O), and nothing creates it until the first import completes. Report
           it as an empty library so the caller says "no imports found" rather
           than "could not read", which is what the file-per-row chooser this
           replaced did by ignoring the walk's result outright. A directory that
           IS there and still could not be walked is a real fault. */
        dsd_stat_t st;
        if (dsd_stat_path(dir, &st) != 0) {
            return 0;
        }
        return -1;
    }
    return lib->count;
}

static int
rr_library_cmp(const void* lhs, const void* rhs) {
    const RrLibrarySystem* a = (const RrLibrarySystem*)lhs;
    const RrLibrarySystem* b = (const RrLibrarySystem*)rhs;
    const int by_name = strcmp(a->name, b->name);
    if (by_name != 0) {
        return by_name;
    }
    return (a->sid > b->sid) - (a->sid < b->sid);
}

void
rr_library_sort(RrLibrary* lib) {
    if (lib == NULL || lib->count < 2) {
        return;
    }
    qsort(lib->systems, (size_t)lib->count, sizeof lib->systems[0], rr_library_cmp);
}

int
rr_library_system_in_use(const RrLibrarySystem* s, const char* chan_in_use, const char* group_in_use) {
    if (s == NULL) {
        return 0;
    }
    if (s->has_chan && chan_in_use != NULL && chan_in_use[0] != '\0' && strcmp(s->chan_path, chan_in_use) == 0) {
        return 1;
    }
    if (s->has_group && group_in_use != NULL && group_in_use[0] != '\0' && strcmp(s->group_path, group_in_use) == 0) {
        return 1;
    }
    return 0;
}

/** @brief Write the name column, padded to RR_LIB_NAME_COL, truncated with "..". */
static void
rr_library_write_name(const RrLibrarySystem* s, char* col, size_t col_sz) {
    const size_t name_len = strlen(s->name);
    if (name_len <= RR_LIB_NAME_COL) {
        (void)DSD_SNPRINTF(col, col_sz, "%-*s", RR_LIB_NAME_COL, s->name);
        return;
    }
    /* Keep RR_LIB_NAME_COL columns exactly: (RR_LIB_NAME_COL - 2) name bytes + "..".
       The cut is by byte, but system_name is free text fetched from
       RadioReference, so back off to a UTF-8 lead byte first: emitting half a
       multi-byte sequence makes ncurses draw a replacement glyph and shifts the
       rest of the row. (Width-aware column fitting is a bigger job; this only
       guarantees the bytes we do emit are well-formed.) */
    int keep = RR_LIB_NAME_COL - 2;
    while (keep > 0 && ((unsigned char)s->name[keep] & 0xC0U) == 0x80U) {
        keep--;
    }
    char head[RR_LIB_NAME_COL + 1];
    (void)DSD_SNPRINTF(head, sizeof head, "%.*s..", keep, s->name);
    (void)DSD_SNPRINTF(col, col_sz, "%-*s", RR_LIB_NAME_COL, head);
}

/** @brief Write the detail cell: a start frequency, "<freq> scan", or "-". */
static void
rr_library_write_detail(const RrLibrarySystem* s, char* col, size_t col_sz) {
    char freq[32];
    if (!s->recipe.present || dsd_rr_hz_to_mhz_text(s->recipe.tune_hz, freq, sizeof freq) != 0 || freq[0] == '\0') {
        (void)DSD_SNPRINTF(col, col_sz, "%s", "-");
        return;
    }
    if (s->recipe.scan_list) {
        (void)DSD_SNPRINTF(col, col_sz, "%s scan", freq);
    } else {
        (void)DSD_SNPRINTF(col, col_sz, "%s", freq);
    }
}

int
rr_library_row_format(const RrLibrarySystem* s, int in_use, char* out, size_t out_sz) {
    if (s == NULL || out == NULL || out_sz == 0) {
        if (out != NULL && out_sz > 0) {
            out[0] = '\0';
        }
        return 0;
    }

    char name_col[64];
    char detail_col[48];
    rr_library_write_name(s, name_col, sizeof name_col);
    rr_library_write_detail(s, detail_col, sizeof detail_col);

    const char* proto = s->recipe.present ? dsd_rr_protocol_short_name(s->recipe.protocol) : NULL;
    if (proto == NULL) {
        proto = "-";
    }

    const int n = DSD_SNPRINTF(out, out_sz, "%s  %-*s  %s%s", name_col, DSD_RR_PROTO_SHORT_NAME_MAX, proto, detail_col,
                               in_use ? "  * in use" : "");
    if (n < 0) {
        out[0] = '\0';
        return 0;
    }
    return ((size_t)n < out_sz) ? n : (int)(out_sz - 1);
}
