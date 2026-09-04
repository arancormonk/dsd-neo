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
#include <dsd-neo/runtime/trunk_scan_hooks.h>
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

static const char* g_active_chan_csv;

static const char*
test_active_chan_csv(const dsd_state* state) {
    (void)state;
    return g_active_chan_csv;
}

static void
set_scan_chan_csv_hook(const char* path) {
    g_active_chan_csv = path;
    dsd_trunk_scan_hooks hooks = {0};
    hooks.active_chan_csv = test_active_chan_csv;
    dsd_trunk_scan_hooks_set(hooks);
}

/*
 * Under trunk scan the global -C channel map is rejected, so opts->chan_in_file is always empty
 * and the per-target chan_csv lives in the coordinator. The diagnostics have to ask the
 * coordinator for it, or they silently never report a missing channel mapping for a scan target.
 */
static int
test_chan_map_path_resolves_under_trunk_scan(void) {
    static dsd_opts opts;
    static dsd_state state;
    int rc = 0;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    opts.trunk_scan_enabled = 1;
    set_scan_chan_csv_hook("targets/site.csv");
    nxdn_trunk_diag_log_missing_channel_once(&opts, &state, 40, "grant");
    rc |= expect_eq_size("scan-target-path-records", nxdn_trunk_diag_collect_unmapped_channels(&state, NULL, 0), 1);

    /* A scan target without a chan_csv has no map to report against. */
    set_scan_chan_csv_hook(NULL);
    nxdn_trunk_diag_log_missing_channel_once(&opts, &state, 41, "grant");
    rc |= expect_eq_size("scan-target-no-csv-skipped", nxdn_trunk_diag_collect_unmapped_channels(&state, NULL, 0), 1);

    /* An empty string is the same as no channel map. */
    set_scan_chan_csv_hook("");
    nxdn_trunk_diag_log_missing_channel_once(&opts, &state, 42, "grant");
    rc |=
        expect_eq_size("scan-target-empty-csv-skipped", nxdn_trunk_diag_collect_unmapped_channels(&state, NULL, 0), 1);

    /* Outside trunk scan the coordinator hook is not the authority; -C alone is. */
    opts.trunk_scan_enabled = 0;
    set_scan_chan_csv_hook("targets/site.csv");
    nxdn_trunk_diag_log_missing_channel_once(&opts, &state, 43, "grant");
    rc |= expect_eq_size("non-scan-ignores-hook", nxdn_trunk_diag_collect_unmapped_channels(&state, NULL, 0), 1);

    /* A global -C still wins wherever it is set. */
    (void)DSD_SNPRINTF(opts.chan_in_file, sizeof opts.chan_in_file, "%s", "global.csv");
    nxdn_trunk_diag_log_missing_channel_once(&opts, &state, 44, "grant");
    rc |= expect_eq_size("global-chan-map-records", nxdn_trunk_diag_collect_unmapped_channels(&state, NULL, 0), 2);

    dsd_trunk_scan_hooks_set((dsd_trunk_scan_hooks){0});
    g_active_chan_csv = NULL;
    dsd_state_ext_free_all(&state);
    return rc;
}

/*
 * The ledger is per decoder-state, but trunk scan rotates several NXDN targets through one state,
 * so the coordinator has to be able to park and restore each target's ledger.
 */
static int
test_ledger_save_and_restore_round_trip(void) {
    static dsd_state state;
    nxdn_trunk_diag_ledger ledger;
    int rc = 0;

    DSD_MEMSET(&ledger, 0xAB, sizeof(ledger));
    nxdn_trunk_diag_ledger_save(&state, &ledger);
    rc |= expect_eq_size("empty-state-saves-empty-ledger", (size_t)ledger.missing_unique, 0);

    rc |= expect_eq_int("note-ch60", nxdn_trunk_diag_note_missing_channel(&state, 60), 1);
    rc |= expect_eq_int("note-ch61", nxdn_trunk_diag_note_missing_channel(&state, 61), 1);
    nxdn_trunk_diag_ledger_save(&state, &ledger);
    rc |= expect_eq_size("saved-two", (size_t)ledger.missing_unique, 2);

    /* Switching to another target starts from that target's (empty) ledger. */
    nxdn_trunk_diag_ledger empty;
    DSD_MEMSET(&empty, 0, sizeof(empty));
    nxdn_trunk_diag_ledger_restore(&state, &empty);
    rc |= expect_eq_size("restored-empty", nxdn_trunk_diag_collect_unmapped_channels(&state, NULL, 0), 0);
    rc |= expect_eq_int("note-ch60-again-on-other-target", nxdn_trunk_diag_note_missing_channel(&state, 60), 1);

    /* Switching back restores what the first target had already seen. */
    nxdn_trunk_diag_ledger_restore(&state, &ledger);
    uint16_t out[4];
    DSD_MEMSET(out, 0, sizeof out);
    rc |= expect_eq_size("restored-two", nxdn_trunk_diag_collect_unmapped_channels(&state, out, 4), 2);
    rc |= expect_eq_u16("restored-out0", out[0], 60);
    rc |= expect_eq_u16("restored-out1", out[1], 61);
    rc |= expect_eq_int("restored-dedup-holds", nxdn_trunk_diag_note_missing_channel(&state, 60), 0);

    dsd_state_ext_free_all(&state);
    return rc;
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

    rc |= test_chan_map_path_resolves_under_trunk_scan();
    rc |= test_ledger_save_and_restore_round_trip();
    return rc;
}
