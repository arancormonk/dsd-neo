// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

/*
 * Engine-private pieces of the typed-frequency match (issue #644, <dsd-neo/engine/scan_entry.h>): the tally both scans
 * feed their rows on the frequency into, in list order, and what each scan contributes. The tally is inline so that a
 * test building one scan's source alone needs nothing more.
 */
#ifndef DSD_NEO_SRC_ENGINE_SCAN_ENTRY_INTERNAL_H
#define DSD_NEO_SRC_ENGINE_SCAN_ENTRY_INTERNAL_H

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/engine/scan_entry.h>
#include <stdint.h>

/* The rows on one frequency, as a scan walks them in list order. */
typedef struct {
    dsd_scan_entry_match_t* out;
    int seen; /* a row on the frequency was walked, eligible or not */
} dsd_scan_entry_tally;

static inline void
dsd_scan_entry_tally_begin(dsd_scan_entry_tally* tally, dsd_scan_entry_match_t* out) {
    DSD_MEMSET(out, 0, sizeof(*out));
    out->row = -1;
    out->reason = DSD_SCAN_ENTRY_NO_ROW;
    tally->out = out;
    tally->seen = 0;
}

static inline void
dsd_scan_entry_tally_name(dsd_scan_entry_match_t* out, int row, int trunked, const char* label) {
    out->row = row;
    out->trunked = trunked;
    DSD_SNPRINTF(out->label, sizeof out->label, "%s", label ? label : "");
}

/* The next row on the frequency in list order, of @p scanner: @p why it is not eligible (DSD_SCAN_ENTRY_OK when it is),
   whether it is confirmed on air, whether it is a trunked target, and its name. The first eligible row is chosen,
   unless a later one is confirmed on air and it is not; with none eligible, the first row walked names why. */
static inline void
dsd_scan_entry_tally_row(dsd_scan_entry_tally* tally, dsd_scan_entry_scanner scanner, int row,
                         dsd_scan_entry_reason why, int on_air, int trunked, const char* label) {
    dsd_scan_entry_match_t* out = tally->out;
    const int first = !tally->seen;
    tally->seen = 1;
    if (why != DSD_SCAN_ENTRY_OK) {
        if (first) {
            out->reason = why;
            dsd_scan_entry_tally_name(out, row, trunked, label);
        }
        return;
    }
    out->count++;
    if (out->count == 1U || (on_air && !out->on_air)) {
        out->scanner = scanner;
        out->reason = DSD_SCAN_ENTRY_OK;
        out->ordinal = out->count;
        out->on_air = on_air ? 1 : 0;
        dsd_scan_entry_tally_name(out, row, trunked, label);
    }
}

/* 1 when a row was chosen. */
static inline int
dsd_scan_entry_tally_end(const dsd_scan_entry_tally* tally) {
    return tally->out->scanner != DSD_SCAN_ENTRY_NONE;
}

/* Whether -Y row @p row runs a width that fits (the width part of the rule by which a step skips a row at every visit,
   without its avoid check): any row but an analog one, and an analog one whose effective width, of the kind its
   demodulator runs, the front end takes at @p dsp_rate_hz, and that something demodulates. channel_scan.c. */
int dsd_engine_channel_scan_row_width_fits(const dsd_opts* opts, const dsd_state* state, int row, int dsp_rate_hz);

/* Walk the trunk-scan targets on @p hz into @p tally, in list order; nothing without a coordinator. trunk_scan.c. */
void dsd_engine_trunk_scan_entry_rows(const dsd_opts* opts, const dsd_state* state, uint32_t hz,
                                      dsd_scan_entry_tally* tally);

#endif /* DSD_NEO_SRC_ENGINE_SCAN_ENTRY_INTERNAL_H */
