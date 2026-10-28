// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
/**
 * @file
 * @brief Source aliases (the radio ID list): CSV grammar `id,name[,tags]`, or with policy columns
 * `id,name,mode,audio,record,stream,tags` when the header's third column is `mode`. The id is a decimal uint32_t ID,
 * an inclusive `start-end` range or a digit pattern. See docs/csv-formats.md, "Source ID List CSV".
 */
#include <dsd-neo/core/csv_import.h>
#include <dsd-neo/core/csv_validate.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/source_alias.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/runtime/log.h>
#include <stdint.h>
#include <stdio.h>
#include "../util/talkgroup_policy_internal.h"
#include "csv_parse_internal.h"

#ifdef DSD_NEO_TEST_HOOKS
static long read_remaining = -1;

void
dsd_source_alias_test_read_fail_after_lines(long successful_lines) {
    read_remaining = successful_lines;
}
#endif
_Static_assert((int)CSV_ID_EXACT == (int)DSD_TG_POLICY_ROW_EXACT && (int)CSV_ID_RANGE == (int)DSD_TG_POLICY_ROW_RANGE
                   && (int)CSV_ID_PATTERN == (int)DSD_TG_POLICY_ROW_PATTERN,
               "csv_parse_id_field() kinds are radio-list row kinds");

/* The policy columns a header opts into: none unless column 3 is `mode`, then the ordered prefix
 * mode,audio,record,stream,tags up to the first header column that breaks it (the group list's prefix rule). */
typedef struct {
    int policy;
    unsigned prefix_len; /* 1 mode, 2 +audio, 3 +record, 4 +stream, 5 +tags */
} alias_header;

static alias_header
alias_parse_header(char* line) {
    static const char* const expected[] = {"mode", "audio", "record", "stream", "tags"};
    alias_header info = {0, 0u};
    char* fields[8];
    const size_t count = csv_split_preserve_empty(line, fields, sizeof(fields) / sizeof(fields[0]));
    for (size_t i = 2; i < count && info.prefix_len < sizeof(expected) / sizeof(expected[0]); ++i) {
        if (csv_ascii_casecmp(trim_ws(fields[i]), expected[info.prefix_len]) != 0) {
            break;
        }
        info.prefix_len++;
    }
    info.policy = info.prefix_len > 0u;
    return info;
}

/* One row's policy cells: mode, then audio/record/stream (defaults from mode) as far as the header reaches. */
static void
alias_parse_policy(const alias_header* header, char** fields, size_t count, const char* filename, unsigned row,
                   int quiet, dsd_source_alias_entry* entry) {
    static const char* const labels[] = {"audio", "record", "stream"};
    uint8_t* const flags[] = {&entry->audio, &entry->record, &entry->stream};
    int given[3] = {0, 0, 0};
    entry->has_policy = 1u;
    if (count > 2) {
        DSD_SNPRINTF(entry->mode, sizeof(entry->mode), "%s", trim_ws(fields[2]));
    }
    const int blocking = dsd_tg_policy_mode_blocks(entry->mode);
    for (size_t i = 0; i < 3; ++i) {
        *flags[i] = blocking ? 0u : 1u;
        if (header->prefix_len < 2u + i || count <= 3u + i) {
            continue;
        }
        int value = 0;
        const int rc = csv_parse_bool_cell(fields[3 + i], &value);
        if (rc == CSV_CELL_OK) {
            *flags[i] = value ? 1u : 0u;
            given[i] = 1;
        } else if (rc == CSV_CELL_INVALID && !quiet) {
            LOG_WARN("WARNING: Source ID file '%s' row %u has invalid %s value '%s'; using default.\n", filename, row,
                     labels[i], fields[3 + i]);
        }
    }
    csv_enforce_media_cells("Source ID", filename, row, blocking, given[0], given[1], given[2], &entry->audio,
                            &entry->record, &entry->stream, quiet);
}

/* Returns 0 stored, 1 invalid (skipped), -1 allocation failure. Diagnostics are suppressed when @p quiet. */
static int
alias_parse_row(char* buffer, const alias_header* header, const char* filename, unsigned row, int quiet,
                dsd_source_alias_store* store) {
    char* fields[8];
    dsd_source_alias_entry entry = {0};
    int kind = 0;
    dsd_id_pattern pattern = {{0}, 0, 0};
    const size_t count = csv_split_preserve_empty(buffer, fields, sizeof(fields) / sizeof(fields[0]));
    if (count < 2 || !csv_parse_id_field(fields[0], &entry.id_start, &entry.id_end, &kind, &pattern)) {
        return 1;
    }
    char* name = trim_ws(fields[1]);
    if (!name[0] && !header->policy) {
        return 1;
    }
    entry.kind = (uint8_t)kind;
    if (kind == CSV_ID_PATTERN) {
        DSD_SNPRINTF(entry.pattern, sizeof(entry.pattern), "%s", pattern.text);
    }
    DSD_SNPRINTF(entry.name, sizeof(entry.name), "%s", name);
    if (header->policy) {
        alias_parse_policy(header, fields, count, filename, row, quiet, &entry);
    }
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
    alias_header header = {0, 0u};
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
        if (row == 1) {
            if (!overlong) {
                header = alias_parse_header(buffer);
            }
            continue;
        }
        if (!csv_data_row_ready(buffer, overlong, resolved, row, stats)) {
            continue;
        }
        int parsed = alias_parse_row(buffer, &header, resolved, row, stats != NULL, store);
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
