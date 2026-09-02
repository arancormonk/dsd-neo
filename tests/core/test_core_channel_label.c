// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/channel_label.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

static int
expect_true(const char* tag, int cond) {
    if (!cond) {
        DSD_FPRINTF(stderr, "%s failed\n", tag);
        return 1;
    }
    return 0;
}

static void
free_test_state(dsd_state* st) {
    if (st) {
        dsd_state_ext_free_all(st);
        dsd_state_trunk_lcn_free(st);
    }
    free(st);
}

/* A -Y scan list of three rows, the middle and last ones named, positioned on row `roll`. */
static int
seed_scan_list(dsd_state* st) {
    st->lcn_freq_count = 3;
    st->trunk_lcn_freq[0] = 851000000L;
    st->trunk_lcn_freq[1] = 851012500L;
    st->trunk_lcn_freq[2] = 851025000L;
    if (dsd_state_trunk_lcn_name_set(st, 1, "Bravo") != 0) {
        return 1;
    }
    if (dsd_state_trunk_lcn_name_set(st, 2, "Charlie") != 0) {
        return 1;
    }
    return 0;
}

static int
test_missing_inputs_report_no_label(void) {
    int rc = 0;
    char out[DSD_CHANNEL_LABEL_SIZE];
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* st = (dsd_state*)calloc(1, sizeof(*st));
    if (!opts || !st) {
        free(opts);
        free_test_state(st);
        return 1;
    }

    DSD_SNPRINTF(out, sizeof(out), "%s", "junk");
    rc |= expect_true("NULL opts reports none", dsd_channel_label_current(NULL, st, out, sizeof(out)) == 0);
    rc |= expect_true("NULL opts clears out", out[0] == '\0');

    DSD_SNPRINTF(out, sizeof(out), "%s", "junk");
    rc |= expect_true("NULL state reports none", dsd_channel_label_current(opts, NULL, out, sizeof(out)) == 0);
    rc |= expect_true("NULL state clears out", out[0] == '\0');

    // Neither source enabled: a junk-prefilled buffer must still end up empty.
    DSD_SNPRINTF(out, sizeof(out), "%s", "junk");
    rc |= expect_true("idle reports none", dsd_channel_label_current(opts, st, out, sizeof(out)) == 0);
    rc |= expect_true("idle clears out", out[0] == '\0');

    free(opts);
    free_test_state(st);
    return rc;
}

static int
test_trunk_scan_target_wins(void) {
    int rc = 0;
    char out[DSD_CHANNEL_LABEL_SIZE];
    char small[8];
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* st = (dsd_state*)calloc(1, sizeof(*st));
    if (!opts || !st) {
        free(opts);
        free_test_state(st);
        return 1;
    }
    if (seed_scan_list(st) != 0) {
        free(opts);
        free_test_state(st);
        return 1;
    }

    opts->trunk_scan_enabled = 1;
    opts->scanner_mode = 1;
    st->lcn_freq_roll = 2;
    DSD_SNPRINTF(st->trunk_scan_active_id, sizeof(st->trunk_scan_active_id), "%s", "county-fire");

    rc |= expect_true("trunk scan reports a label", dsd_channel_label_current(opts, st, out, sizeof(out)) == 1);
    rc |= expect_true("trunk scan wins over -Y", strcmp(out, "county-fire") == 0);

    // A caller only asking whether a label exists still gets the answer.
    rc |= expect_true("NULL out still reports", dsd_channel_label_current(opts, st, NULL, sizeof(out)) == 1);
    small[0] = 'x';
    rc |= expect_true("zero-size out still reports", dsd_channel_label_current(opts, st, small, 0) == 1);
    rc |= expect_true("zero-size out untouched", small[0] == 'x');

    rc |= expect_true("short out still reports", dsd_channel_label_current(opts, st, small, sizeof(small)) == 1);
    rc |= expect_true("short out truncates", strcmp(small, "county-") == 0);

    // Trunk scan on but nothing selected yet, and no -Y underneath it.
    opts->scanner_mode = 0;
    st->trunk_scan_active_id[0] = '\0';
    rc |= expect_true("empty target id reports none", dsd_channel_label_current(opts, st, out, sizeof(out)) == 0);
    rc |= expect_true("empty target id clears out", out[0] == '\0');

    free(opts);
    free_test_state(st);
    return rc;
}

static int
test_scanner_row_name(void) {
    int rc = 0;
    char out[DSD_CHANNEL_LABEL_SIZE];
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* st = (dsd_state*)calloc(1, sizeof(*st));
    if (!opts || !st) {
        free(opts);
        free_test_state(st);
        return 1;
    }
    if (seed_scan_list(st) != 0) {
        free(opts);
        free_test_state(st);
        return 1;
    }
    opts->scanner_mode = 1;

    // lcn_freq_roll is advanced past the row just tuned, so roll N names row N-1.
    st->lcn_freq_roll = 0;
    rc |= expect_true("roll 0 reports none", dsd_channel_label_current(opts, st, out, sizeof(out)) == 0);

    st->lcn_freq_roll = st->lcn_freq_count + 1;
    rc |= expect_true("roll past the list reports none", dsd_channel_label_current(opts, st, out, sizeof(out)) == 0);

    st->lcn_freq_roll = 1;
    rc |= expect_true("unnamed row reports none", dsd_channel_label_current(opts, st, out, sizeof(out)) == 0);
    rc |= expect_true("unnamed row clears out", out[0] == '\0');

    st->lcn_freq_roll = 2;
    rc |= expect_true("named row reports a label", dsd_channel_label_current(opts, st, out, sizeof(out)) == 1);
    rc |= expect_true("named row label", strcmp(out, "Bravo") == 0);

    // The last row of the list is on air once roll has caught up with the count: the bound is
    // inclusive, so this row is named like any other.
    st->lcn_freq_roll = st->lcn_freq_count;
    rc |= expect_true("roll at the end reports a label", dsd_channel_label_current(opts, st, out, sizeof(out)) == 1);
    rc |= expect_true("roll at the end names the last row", strcmp(out, "Charlie") == 0);

    // A row the importer kept for its numbering but could not use: the scanner parks on the
    // frequency it is already on rather than tuning this one, so its name would credit the
    // wrong channel for a whole hangtime.
    st->lcn_freq_roll = 2;
    st->trunk_lcn_freq[1] = 0L;
    rc |= expect_true("placeholder row reports none", dsd_channel_label_current(opts, st, out, sizeof(out)) == 0);
    rc |= expect_true("placeholder row clears out", out[0] == '\0');
    st->trunk_lcn_freq[1] = 851012500L;

    // A map imported without a name column leaves no store at all.
    dsd_state_trunk_lcn_name_free(st);
    rc |= expect_true("no name store reports none", dsd_channel_label_current(opts, st, out, sizeof(out)) == 0);

    free(opts);
    free_test_state(st);
    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_missing_inputs_report_no_label();
    rc |= test_trunk_scan_target_wins();
    rc |= test_scanner_row_name();
    return rc;
}
