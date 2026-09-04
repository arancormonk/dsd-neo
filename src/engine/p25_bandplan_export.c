// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Export of the learned P25 band plan (IDEN tables) to the band-plan CSV.
 */

#include <dsd-neo/core/csv_import.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/engine/p25_bandplan_export.h>
#include <dsd-neo/engine/trunk_scan.h>
#include <dsd-neo/runtime/log.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

int
dsd_engine_p25_bandplan_export(const dsd_opts* opts, const dsd_state* state, const char* path) {
    (void)opts;
    if (!state || !path || path[0] == '\0') {
        LOG_WARN("WARNING: P25 band plan export: no destination path.\n");
        return -1;
    }
    p25_bandplan_row_t rows[DSD_P25_BANDPLAN_MAX_ROWS];
    DSD_MEMSET(rows, 0, sizeof rows);
    /* The live tables first: under trunk scan they belong to the active target, whose parked
     * snapshot may be stale, so the coordinator walks only the other targets. */
    int count =
        dsd_p25_bandplan_append_tables(rows, 0, DSD_P25_BANDPLAN_MAX_ROWS, state->p25_iden_fdma, state->p25_iden_tdma);
    count = dsd_engine_trunk_scan_append_p25_idens(state, rows, count, DSD_P25_BANDPLAN_MAX_ROWS);
    if (count <= 0) {
        LOG_WARN("WARNING: P25 band plan export: no identifier has been learned yet; nothing written to '%s'.\n", path);
        return -1;
    }
    if (csvP25BandplanExportRows(path, rows, count) != 0) {
        return -1;
    }
    return count;
}
