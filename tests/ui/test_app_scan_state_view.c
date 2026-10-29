// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

/* The scan-state view (issue #671): which scan the session has and whether it runs or is paused, for every state a
 * -Y scan and a --trunk-scan can be in, and the API spellings of each. */

#include <assert.h>
#include <dsd-neo/app_control/scan_state_view.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void
expect_view(const char* label, const dsd_opts* opts, const dsd_state* state, int kind, int running, int paused,
            int resume_pending, int targets, int operator_tuned, const char* state_name) {
    dsd_app_scan_state_view v;
    DSD_MEMSET(&v, 0xA5, sizeof v);
    assert(dsd_app_scan_state_view_get(opts, state, &v) == 0);
    if (v.kind != kind || v.running != running || v.paused != paused || v.resume_pending != resume_pending
        || v.target_count != targets || v.operator_tuned != operator_tuned
        || strcmp(dsd_app_scan_state_name(&v), state_name) != 0) {
        DSD_FPRINTF(stderr,
                    "%s: kind=%u running=%u paused=%u pending=%u targets=%u operator=%u state=%s, want "
                    "%d/%d/%d/%d/%d/%d/%s\n",
                    label, v.kind, v.running, v.paused, v.resume_pending, v.target_count, v.operator_tuned,
                    dsd_app_scan_state_name(&v), kind, running, paused, resume_pending, targets, operator_tuned,
                    state_name);
        assert(0);
    }
}

int
main(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof *opts);
    dsd_state* state = (dsd_state*)calloc(1, sizeof *state);
    assert(opts != NULL && state != NULL);

    dsd_app_scan_state_view v;
    DSD_MEMSET(&v, 0xA5, sizeof v);
    assert(dsd_app_scan_state_view_get(NULL, state, &v) == -1 && v.kind == 0U && v.running == 0U);
    assert(dsd_app_scan_state_view_get(opts, NULL, &v) == -1);
    assert(dsd_app_scan_state_view_get(opts, state, NULL) == -1);
    assert(dsd_app_scan_state_name(NULL) != NULL && strcmp(dsd_app_scan_state_name(NULL), "off") == 0);

    /* No scan. A list loaded for trunking is not a scan. */
    state->lcn_freq_count = 4;
    expect_view("no scan", opts, state, DSD_APP_SCAN_KIND_NONE, 0, 0, 0, 0, 0, "off");
    /* A tuner release with no scan (Explore under -T) still says the operator holds the tuner. */
    state->operator_tuned = 1U;
    expect_view("no scan, released", opts, state, DSD_APP_SCAN_KIND_NONE, 0, 0, 0, 0, 1, "off");
    state->operator_tuned = 0U;

    /* -Y running, paused, resuming, and stopped with its list kept. */
    opts->scanner_mode = 1;
    expect_view("-Y running", opts, state, DSD_APP_SCAN_KIND_CHANNEL, 1, 0, 0, 4, 0, "running");
    opts->scanner_mode = 0;
    state->lcn_scan_paused = 1U;
    state->operator_tuned = 1U;
    expect_view("-Y paused", opts, state, DSD_APP_SCAN_KIND_CHANNEL, 0, 1, 0, 4, 1, "paused");
    state->lcn_scan_resume_pending = 1U;
    expect_view("-Y resume pending", opts, state, DSD_APP_SCAN_KIND_CHANNEL, 0, 1, 1, 4, 1, "paused");
    state->lcn_scan_resume_pending = 0U;
    state->lcn_scan_paused = 0U;
    expect_view("-Y stopped", opts, state, DSD_APP_SCAN_KIND_NONE, 0, 0, 0, 0, 1, "off");
    /* A row count the published field cannot hold is clamped. */
    opts->scanner_mode = 1;
    state->lcn_freq_count = 70000;
    expect_view("-Y huge list", opts, state, DSD_APP_SCAN_KIND_CHANNEL, 1, 0, 0, 65535, 1, "running");
    state->lcn_freq_count = 4;
    opts->scanner_mode = 0;
    state->operator_tuned = 0U;

    /* --trunk-scan running and paused; its targets, not the -Y list, are counted. */
    opts->trunk_scan_enabled = 1;
    state->trunk_scan_target_count = 3U;
    expect_view("trunk scan running", opts, state, DSD_APP_SCAN_KIND_TRUNK, 1, 0, 0, 3, 0, "running");
    state->trunk_scan_paused = 1U;
    state->operator_tuned = 1U;
    expect_view("trunk scan paused", opts, state, DSD_APP_SCAN_KIND_TRUNK, 0, 1, 0, 3, 1, "paused");
    /* A -Y resume flag never makes a trunk scan's state. */
    state->lcn_scan_resume_pending = 1U;
    expect_view("trunk scan ignores -Y flags", opts, state, DSD_APP_SCAN_KIND_TRUNK, 0, 1, 0, 3, 1, "paused");

    assert(strcmp(dsd_app_scan_kind_name(DSD_APP_SCAN_KIND_NONE), "none") == 0);
    assert(strcmp(dsd_app_scan_kind_name(DSD_APP_SCAN_KIND_CHANNEL), "channel") == 0);
    assert(strcmp(dsd_app_scan_kind_name(DSD_APP_SCAN_KIND_TRUNK), "trunk") == 0);
    assert(strcmp(dsd_app_scan_kind_name(99U), "none") == 0);

    free(state);
    free(opts);
    printf("APP_SCAN_STATE_VIEW: OK\n");
    return 0;
}
