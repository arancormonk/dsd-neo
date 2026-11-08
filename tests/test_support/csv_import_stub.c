// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/csv_import.h>

#include <dsd-neo/core/talkgroup_policy.h>

#include <stdint.h>

#include "../../src/core/util/talkgroup_policy_internal.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

int
csvGroupImportPath(const char* group_file_path, dsd_state* state) {
    (void)group_file_path;
    (void)state;
    return -1;
}

int
csvGroupImport(const dsd_opts* opts, dsd_state* state) {
    (void)opts;
    return csvGroupImportPath("", state);
}

int
dsd_tg_policy_load(const char* path, dsd_tg_policy_store** out) {
    (void)path;
    (void)out;
    return -1;
}

int
dsd_tg_policy_load_counted(const char* path, dsd_tg_policy_store** out, dsd_tg_policy_load_report* report) {
    (void)path;
    (void)out;
    if (report) {
        *report = (dsd_tg_policy_load_report){0};
    }
    return -1;
}

/* The importer's rows-hash side: no file is read, so no row is hashed. */
int
dsd_tg_policy_rows_hash_line(uint64_t* hash, const char* path, const char* header_line, const char* line) {
    (void)hash;
    (void)path;
    (void)header_line;
    (void)line;
    return 0;
}

int
dsd_tg_policy_rows_hash_file(const char* path, uint64_t* hash, dsd_tg_policy_load_report* report) {
    (void)path;
    (void)hash;
    if (report) {
        *report = (dsd_tg_policy_load_report){0};
    }
    return -1;
}
