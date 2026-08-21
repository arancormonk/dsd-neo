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

/*
 * Column bounds. The floors keep a column readable; the ceilings stop one long
 * name from pushing the frequency off a wide terminal.
 */
#define RR_LIB_NAME_MIN      12
#define RR_LIB_NAME_MAX      40
#define RR_LIB_SITE_MIN      10
#define RR_LIB_SITE_MAX      28

/*
 * Everything a row spends that is not one of the two variable columns: the
 * gutter, the gaps, the protocol column and the widest detail cell
 * ("1234.56789 scan"). RR_LIB_FIXED_COLS covers the gap before the site column
 * as well; RR_LIB_FIXED_NO_SITE is the same row without one.
 */
#define RR_LIB_DETAIL_COLS   15
#define RR_LIB_FIXED_NO_SITE (2 + 2 + DSD_RR_PROTO_SHORT_NAME_MAX + 2 + RR_LIB_DETAIL_COLS)
#define RR_LIB_FIXED_COLS    (RR_LIB_FIXED_NO_SITE + 2)

void
rr_library_init(RrLibrary* lib) {
    if (lib != NULL) {
        DSD_MEMSET(lib, 0, sizeof(*lib));
    }
}

/** @brief The row for @p stem, or NULL when it is not present yet. */
static RrLibrarySystem*
rr_library_find(RrLibrary* lib, const char* stem) {
    for (int i = 0; i < lib->count; i++) {
        if (strcmp(lib->systems[i].stem, stem) == 0) {
            return &lib->systems[i];
        }
    }
    return NULL;
}

/**
 * @brief The grouping key: @p csv_path's leaf minus its " group.csv" /
 *        " chan.csv" half suffix.
 *
 * @param csv_path Absolute path of a generated CSV.
 * @param is_chan  Which half it is, from the sidecar's kind.
 * @param out      Receives the stem; always NUL-terminated on return.
 * @param out_sz   Destination size in bytes, passed explicitly.
 * @return 0 on success, -1 when the leaf does not carry the expected suffix or
 *         the stem does not fit (a truncated stem would merge two rows).
 */
static int
rr_library_stem(const char* csv_path, int is_chan, char* out, size_t out_sz) {
    const char* slash = strrchr(csv_path, RR_LIB_PATH_SEP);
#if defined(_WIN32)
    const char* fwd = strrchr(csv_path, '/');
    if (fwd != NULL && (slash == NULL || fwd > slash)) {
        slash = fwd;
    }
#endif
    const char* leaf = (slash != NULL) ? slash + 1 : csv_path;
    const char* suffix = is_chan ? " chan.csv" : " group.csv";
    const size_t leaf_len = strlen(leaf);
    const size_t suffix_len = strlen(suffix);
    if (leaf_len <= suffix_len || strcmp(leaf + (leaf_len - suffix_len), suffix) != 0) {
        return -1;
    }
    const size_t stem_len = leaf_len - suffix_len;
    if (stem_len >= out_sz) {
        return -1;
    }
    (void)DSD_SNPRINTF(out, out_sz, "%.*s", (int)stem_len, leaf);
    return 0;
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

/** @brief Find @p stem's row, or start one. NULL when the library is full. */
static RrLibrarySystem*
rr_library_intern(RrLibrary* lib, const char* stem, const dsd_rr_provenance* prov) {
    RrLibrarySystem* sys = rr_library_find(lib, stem);
    if (sys != NULL) {
        return sys;
    }
    if (lib->count >= RR_LIBRARY_MAX) {
        lib->overflow = 1;
        return NULL;
    }
    sys = &lib->systems[lib->count++];
    DSD_MEMSET(sys, 0, sizeof(*sys));
    DSD_STRNCPY(sys->stem, stem, sizeof(sys->stem) - 1);
    sys->sid = prov->sid;
    DSD_STRNCPY(sys->name, prov->system_name, sizeof(sys->name) - 1);
    DSD_STRNCPY(sys->site_label, prov->site_label, sizeof(sys->site_label) - 1);
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
    if (sys->site_label[0] == '\0' && prov->site_label[0] != '\0') {
        DSD_STRNCPY(sys->site_label, prov->site_label, sizeof(sys->site_label) - 1);
    }
}

/**
 * @brief Record one half's path.
 *
 * No newest-wins arbitration: the row is keyed on the stem, and a stem plus a
 * half IS a filename, so two files can never contend for the same slot. When
 * RadioReference renames a system the old pair keeps its own stem and gets its
 * own row, rather than being hidden behind the new one where nothing could
 * delete it.
 */
static void
rr_library_record_half(RrLibrarySystem* sys, const char* csv_path, int is_chan) {
    /* The two halves are spelled out rather than selected through a char* alias:
       DSD_STRNCPY bounds the copy with __builtin_object_size(dst, 1), which only
       sees the real size when the destination IS the array. Handing it a pointer
       silently shortens every path. */
    if (is_chan) {
        sys->has_chan = 1;
        DSD_STRNCPY(sys->chan_path, csv_path, sizeof(sys->chan_path) - 1);
        return;
    }
    sys->has_group = 1;
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
    char stem[RR_LIBRARY_STEM_MAX];
    if (rr_library_stem(csv_path, is_chan, stem, sizeof(stem)) != 0) {
        return -1;
    }
    RrLibrarySystem* sys = rr_library_intern(lib, stem, prov);
    if (sys == NULL) {
        return -1; /* full; overflow already flagged */
    }
    rr_library_merge_meta(sys, prov);
    rr_library_record_half(sys, csv_path, is_chan);
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
    const int by_site = strcmp(a->site_label, b->site_label);
    if (by_site != 0) {
        return by_site;
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

void
rr_library_display_name(const RrLibrarySystem* s, char* out, size_t out_sz) {
    if (out == NULL || out_sz == 0U) {
        return;
    }
    if (s == NULL) {
        out[0] = '\0';
        return;
    }
    if (s->site_label[0] != '\0') {
        (void)DSD_SNPRINTF(out, out_sz, "%s - %s", s->name, s->site_label);
        return;
    }
    (void)DSD_SNPRINTF(out, out_sz, "%s", s->name);
}

/**
 * @brief Write @p text into a @p width-wide cell, truncated with "..".
 *
 * The cut is by byte, but the text is free text fetched from RadioReference, so
 * it backs off to a UTF-8 lead byte first: emitting half a multi-byte sequence
 * makes ncurses draw a replacement glyph and shifts the rest of the row.
 * (Width-aware column fitting is a bigger job; this only guarantees the bytes
 * we do emit are well-formed.)
 *
 * @param text   Cell text.
 * @param width  Column width in bytes; <= 0 writes nothing.
 * @param out    Destination.
 * @param out_sz Destination size in bytes, passed explicitly.
 */
static void
rr_library_write_cell(const char* text, int width, char* out, size_t out_sz) {
    if (width <= 0) {
        out[0] = '\0';
        return;
    }
    const size_t len = strlen(text);
    if (len <= (size_t)width) {
        (void)DSD_SNPRINTF(out, out_sz, "%-*s", width, text);
        return;
    }
    if (width < 3) {
        /* No room for text plus its truncation mark. rr_library_layout() never
           produces this; a hand-made layout would otherwise reach a negative
           precision below, which printf reads as "no precision at all" and which
           would print the whole string straight through the column. */
        (void)DSD_SNPRINTF(out, out_sz, "%*s", width, "");
        return;
    }
    int keep = width - 2;
    while (keep > 0 && ((unsigned char)text[keep] & 0xC0U) == 0x80U) {
        keep--;
    }
    (void)DSD_SNPRINTF(out, out_sz, "%.*s..%*s", keep, text, width - keep - 2, "");
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

/** @brief Clamp @p want into [@p lo, @p hi]. */
static int
rr_library_clamp(int want, int lo, int hi) {
    if (want < lo) {
        return lo;
    }
    return (want > hi) ? hi : want;
}

/** @brief The longest name and site label stored, in bytes. */
static void
rr_library_measure(const RrLibrary* lib, int* out_name, int* out_site) {
    int name = 0;
    int site = 0;
    for (int i = 0; i < lib->count; i++) {
        const int n = (int)strlen(lib->systems[i].name);
        const int t = (int)strlen(lib->systems[i].site_label);
        name = (n > name) ? n : name;
        site = (t > site) ? t : site;
    }
    *out_name = name;
    *out_site = site;
}

void
rr_library_layout(const RrLibrary* lib, int avail_cols, RrLibraryLayout* out) {
    if (out == NULL) {
        return;
    }
    out->name = RR_LIB_NAME_MIN;
    out->site = 0;
    if (lib == NULL) {
        return;
    }

    int longest_name = 0;
    int longest_site = 0;
    rr_library_measure(lib, &longest_name, &longest_site);

    /* Nothing names a site: every stored import predates the label, so the
       column would be a column of nothing. Drop it and give the width back. */
    if (longest_site == 0) {
        out->name = rr_library_clamp(longest_name, RR_LIB_NAME_MIN, RR_LIB_NAME_MAX);
        const int room = avail_cols - RR_LIB_FIXED_NO_SITE;
        if (room > RR_LIB_NAME_MIN && out->name > room) {
            out->name = room;
        }
        return;
    }

    const int want_name = rr_library_clamp(longest_name, RR_LIB_NAME_MIN, RR_LIB_NAME_MAX);
    const int want_site = rr_library_clamp(longest_site, RR_LIB_SITE_MIN, RR_LIB_SITE_MAX);
    int budget = avail_cols - RR_LIB_FIXED_COLS;
    /* A terminal too narrow for the floors still renders; the chooser clips the
       row rather than this returning a column nothing can be read out of. */
    if (budget < RR_LIB_NAME_MIN + RR_LIB_SITE_MIN) {
        budget = RR_LIB_NAME_MIN + RR_LIB_SITE_MIN;
    }
    if (want_name + want_site <= budget) {
        out->name = want_name;
        out->site = want_site;
        return;
    }
    /* Both want more than there is. Whichever fits in half takes what it needs
       and the other takes the rest; when neither does, they split it. */
    const int half = budget / 2;
    if (want_name <= half) {
        out->name = want_name;
        out->site = budget - want_name;
    } else if (want_site <= budget - half) {
        out->site = want_site;
        out->name = budget - want_site;
    } else {
        out->name = half;
        out->site = budget - half;
    }
    /* An odd split can land a column just under its floor. budget is at least
       the two floors together, so restoring one cannot starve the other. */
    if (out->name < RR_LIB_NAME_MIN) {
        out->name = RR_LIB_NAME_MIN;
        out->site = budget - out->name;
    }
    if (out->site < RR_LIB_SITE_MIN) {
        out->site = RR_LIB_SITE_MIN;
        out->name = budget - out->site;
    }
}

int
rr_library_row_format(const RrLibrarySystem* s, const RrLibraryLayout* layout, int in_use, char* out, size_t out_sz) {
    if (s == NULL || layout == NULL || out == NULL || out_sz == 0) {
        if (out != NULL && out_sz > 0) {
            out[0] = '\0';
        }
        return 0;
    }

    char name_col[RR_LIB_NAME_MAX + 8];
    char site_col[RR_LIB_SITE_MAX + 8];
    char detail_col[48];
    rr_library_write_cell(s->name, layout->name, name_col, sizeof name_col);
    rr_library_write_cell(s->site_label, layout->site, site_col, sizeof site_col);
    rr_library_write_detail(s, detail_col, sizeof detail_col);

    const char* proto = s->recipe.present ? dsd_rr_protocol_short_name(s->recipe.protocol) : NULL;
    if (proto == NULL) {
        proto = "-";
    }

    /* The gutter leads. Two columns, and the same two whether or not the row is
       in use, so the columns after it stay aligned down the list. */
    const int n = (layout->site > 0) ? DSD_SNPRINTF(out, out_sz, "%s%s  %s  %-*s  %s", in_use ? "* " : "  ", name_col,
                                                    site_col, DSD_RR_PROTO_SHORT_NAME_MAX, proto, detail_col)
                                     : DSD_SNPRINTF(out, out_sz, "%s%s  %-*s  %s", in_use ? "* " : "  ", name_col,
                                                    DSD_RR_PROTO_SHORT_NAME_MAX, proto, detail_col);
    if (n < 0) {
        out[0] = '\0';
        return 0;
    }
    return ((size_t)n < out_sz) ? n : (int)(out_sz - 1);
}
