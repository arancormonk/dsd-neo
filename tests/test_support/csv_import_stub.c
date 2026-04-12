// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/csv_import.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

int
csvGroupImportPath(const char* group_file_path, dsd_state* state) {
    (void)group_file_path;
    (void)state;
    return -1;
}

int
csvGroupImport(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    return csvGroupImportPath("", state);
}
