// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
/**
 * @file
 * @brief Source aliases: CSV grammar `id,name[,tags]`, decimal uint32_t ID or inclusive
 * `start-end` range. See docs/csv-formats.md, "Source ID List CSV".
 */
#include <dsd-neo/core/csv_import.h>
#include <dsd-neo/core/csv_validate.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/source_alias.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/runtime/log.h>
#include <stdint.h>
#include <stdio.h>
#include "csv_parse_internal.h"

#ifdef DSD_NEO_TEST_HOOKS
static long read_remaining = -1;

void
dsd_source_alias_test_read_fail_after_lines(long successful_lines) {
    read_remaining = successful_lines;
}
#endif
static int
alias_parse_row(char* buffer, dsd_source_alias_store* store) {
    char* fields[8];
    dsd_source_alias_entry entry = {0};
    int range = 0;
    if (csv_split_preserve_empty(buffer, fields, 8) < 2
        || !csv_parse_id_field(fields[0], &entry.id_start, &entry.id_end, &range)) {
        return 1;
    }
    char* name = trim_ws(fields[1]);
    if (!name[0]) {
        return 1;
    }
    entry.is_range = (uint8_t)range;
    DSD_SNPRINTF(entry.name, sizeof(entry.name), "%s", name);
    return dsd_source_alias_store_append(store, &entry);
}

static void
alias_count_row(dsd_csv_validation* stats, int parsed) {
    if (!stats) {
        return;
    }
    if (parsed) {
        ++stats->skipped;
    } else {
        ++stats->accepted;
    }
}

static int
src_alias_import_path(const char* path, dsd_source_alias_store* store, dsd_csv_validation* stats) {
    char resolved[CSV_IMPORT_PATH_MAX];
    FILE* fp = csv_open_user_read_file("source ID file", path, resolved, sizeof(resolved));
    if (!fp) {
        return -1;
    }
    char buffer[BSIZE];
    unsigned row = 0;
    int rc = 0;
    for (;;) {
        int overlong = 0;
#ifdef DSD_NEO_TEST_HOOKS
        if (read_remaining == 0) {
            rc = -1;
            break;
        }
        if (read_remaining > 0) {
            --read_remaining;
        }
#endif
        int read_result = csv_read_line(fp, buffer, sizeof(buffer), &overlong);
        if (read_result <= 0) {
            rc = read_result;
            break;
        }
        ++row;
        trim_eol(buffer);
        if (row == 1 || !csv_data_row_ready(buffer, overlong, resolved, row, stats)) {
            continue;
        }
        int parsed = alias_parse_row(buffer, store);
        if (parsed < 0) {
            rc = -1;
            break;
        }
        alias_count_row(stats, parsed);
        if (!stats && parsed) {
            LOG_WARN("Source ID file '%s' row %u is invalid; skipped.\n", resolved, row);
        }
    }
    if (ferror(fp)) {
        rc = -1;
    }
    fclose(fp);
    return rc;
}

static int
alias_load_stats(const char* path, dsd_source_alias_store** out, dsd_csv_validation* stats) {
    if (!out) {
        return -1;
    }
    dsd_source_alias_store* store = dsd_source_alias_store_create();
    if (!store) {
        return -1;
    }
    if (src_alias_import_path(path, store, stats) != 0) {
        dsd_source_alias_store_free(store);
        return -1;
    }
    *out = store;
    return 0;
}

int
dsd_source_alias_load(const char* path, dsd_source_alias_store** out) {
    return alias_load_stats(path, out, NULL);
}

int
csvSrcImportPath(const char* path, dsd_state* state) {
    dsd_source_alias_store* store = NULL;
    if (!state || dsd_source_alias_load(path, &store) != 0) {
        return -1;
    }
    dsd_source_alias_install(state, store);
    return 0;
}

int
csvSrcImport(const dsd_opts* opts, dsd_state* state) {
    return opts ? csvSrcImportPath(opts->src_in_file, state) : -1;
}

int
dsd_csv_validate_src_file(const char* path, dsd_csv_validation* out) {
    if (!out) {
        return -1;
    }
    DSD_MEMSET(out, 0, sizeof(*out));
    dsd_source_alias_store* store = NULL;
    dsd_csv_validation counts = {0};
    int rc = alias_load_stats(path, &store, &counts);
    if (rc == 0) {
        *out = counts;
    }
    dsd_source_alias_store_free(store);
    return rc;
}
