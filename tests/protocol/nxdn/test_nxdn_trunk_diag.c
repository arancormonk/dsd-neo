// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * NXDN trunking diagnostics: missing channel->frequency mapping tracking.
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/protocol/nxdn/nxdn_trunk_diag.h>
#include <stdint.h>
#include <stdio.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

static int
expect_eq_int(const char* tag, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %d want %d\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
expect_eq_size(const char* tag, size_t got, size_t want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %zu want %zu\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
expect_eq_u16(const char* tag, uint16_t got, uint16_t want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %u want %u\n", tag, (unsigned)got, (unsigned)want);
        return 1;
    }
    return 0;
}

int
main(void) {
    int rc = 0;

    static dsd_state state;
    static dsd_opts opts;
    uint16_t out[8];

    rc |= expect_eq_int("note-null-state", nxdn_trunk_diag_note_missing_channel(NULL, 12), 0);
    rc |= expect_eq_int("note-zero-channel", nxdn_trunk_diag_note_missing_channel(&state, 0), 0);
    rc |= expect_eq_int("note-max-channel", nxdn_trunk_diag_note_missing_channel(&state, UINT16_MAX), 0);
    rc |= expect_eq_size("collect-null-state", nxdn_trunk_diag_collect_unmapped_channels(NULL, out, 8), 0);

    DSD_MEMSET(out, 0, sizeof out);
    rc |= expect_eq_size("empty-total", nxdn_trunk_diag_collect_unmapped_channels(&state, out, 8), 0);

    rc |= expect_eq_int("note-ch12-first", nxdn_trunk_diag_note_missing_channel(&state, 12), 1);
    rc |= expect_eq_int("note-ch12-again", nxdn_trunk_diag_note_missing_channel(&state, 12), 0);
    rc |= expect_eq_int("note-ch13-first", nxdn_trunk_diag_note_missing_channel(&state, 13), 1);

    DSD_MEMSET(out, 0, sizeof out);
    rc |= expect_eq_size("total-2", nxdn_trunk_diag_collect_unmapped_channels(&state, out, 8), 2);
    rc |= expect_eq_u16("out0-ch12", out[0], 12);
    rc |= expect_eq_u16("out1-ch13", out[1], 13);

    // If a channel becomes mapped later in the run, the summary should no longer report it.
    state.trunk_chan_map[12] = 851000000;
    DSD_MEMSET(out, 0, sizeof out);
    rc |= expect_eq_size("total-1-after-map", nxdn_trunk_diag_collect_unmapped_channels(&state, out, 8), 1);
    rc |= expect_eq_u16("out0-ch13-after-map", out[0], 13);

    nxdn_trunk_diag_log_missing_channel_once(NULL, &state, 14, "null-opts");
    nxdn_trunk_diag_log_missing_channel_once(&opts, NULL, 14, "null-state");
    nxdn_trunk_diag_log_summary(NULL, &state);
    nxdn_trunk_diag_log_summary(&opts, NULL);
    nxdn_trunk_diag_log_summary(&opts, &state);
    rc |= expect_eq_size("empty-opts-does-not-log-or-record",
                         nxdn_trunk_diag_collect_unmapped_channels(&state, NULL, 0), 1);

    (void)DSD_SNPRINTF(opts.chan_in_file, sizeof opts.chan_in_file, "%s", "channels.csv");

    static dsd_state log_state;
    log_state.trunk_chan_map[21] = 851000000;
    nxdn_trunk_diag_log_missing_channel_once(&opts, &log_state, 0, "invalid-zero");
    nxdn_trunk_diag_log_missing_channel_once(&opts, &log_state, UINT16_MAX, "invalid-max");
    nxdn_trunk_diag_log_missing_channel_once(&opts, &log_state, 21, "mapped");
    rc |= expect_eq_size("invalid-and-mapped-log-skipped",
                         nxdn_trunk_diag_collect_unmapped_channels(&log_state, NULL, 0), 0);

    nxdn_trunk_diag_log_missing_channel_once(&opts, &log_state, 20, "grant");
    nxdn_trunk_diag_log_missing_channel_once(&opts, &log_state, 20, "");
    nxdn_trunk_diag_log_missing_channel_once(&opts, &log_state, 22, NULL);

    DSD_MEMSET(out, 0, sizeof out);
    rc |= expect_eq_size("log-recorded-two", nxdn_trunk_diag_collect_unmapped_channels(&log_state, out, 8), 2);
    rc |= expect_eq_u16("log-out0-ch20", out[0], 20);
    rc |= expect_eq_u16("log-out1-ch22", out[1], 22);
    rc |=
        expect_eq_size("log-recorded-two-null-out", nxdn_trunk_diag_collect_unmapped_channels(&log_state, NULL, 0), 2);
    nxdn_trunk_diag_log_summary(&opts, &log_state);

    static dsd_state overflow_state;
    for (uint16_t ch = 30; ch < 50; ch++) {
        rc |= expect_eq_int("overflow-note", nxdn_trunk_diag_note_missing_channel(&overflow_state, ch), 1);
    }
    DSD_MEMSET(out, 0, sizeof out);
    rc |= expect_eq_size("overflow-total-cap3", nxdn_trunk_diag_collect_unmapped_channels(&overflow_state, out, 3), 20);
    rc |= expect_eq_u16("overflow-out0", out[0], 30);
    rc |= expect_eq_u16("overflow-out1", out[1], 31);
    rc |= expect_eq_u16("overflow-out2", out[2], 32);
    nxdn_trunk_diag_log_summary(&opts, &overflow_state);

    dsd_state_ext_free_all(&state);
    dsd_state_ext_free_all(&log_state);
    dsd_state_ext_free_all(&overflow_state);
    return rc;
}
