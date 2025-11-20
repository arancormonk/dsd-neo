// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Minimal call-follower core transitions: GRANT -> ARMED -> FOLLOW -> HANG -> RETURN
 * Also verifies GRANT-without-voice times out to RETURN.
 */

#include <stdio.h>
#include <string.h>
#include <time.h>

#define main dsd_neo_main_decl
#include <dsd-neo/core/dsd.h>
#undef main

#include <dsd-neo/protocol/p25/p25_p2_sm_min.h>

static int g_tunes = 0;
static int g_returns = 0;

static void
on_tune(dsd_opts* opts, dsd_state* st, long f, int ch) {
    (void)opts;
    (void)st;
    (void)f;
    (void)ch;
    g_tunes++;
}

static void
on_return(dsd_opts* opts, dsd_state* st) {
    (void)opts;
    (void)st;
    g_returns++;
}

static int
expect_eq_int(const char* tag, int got, int want) {
    if (got != want) {
        fprintf(stderr, "%s: got %d want %d\n", tag, got, want);
        return 1;
    }
    return 0;
}

int
main(void) {
    int rc = 0;
    dsd_p25p2_min_sm sm;
    dsd_opts opts;
    dsd_state st;
    memset(&opts, 0, sizeof opts);
    memset(&st, 0, sizeof st);
    dsd_p25p2_min_init(&sm);
    dsd_p25p2_min_set_callbacks(&sm, on_tune, on_return, NULL);
    dsd_p25p2_min_configure_ex(&sm, /*hang*/ 0.5, /*grace*/ 0.1, /*dwell*/ 0.1, /*gvt*/ 0.2, /*backoff*/ 0.1);

    // GRANT triggers ARMED and tune callback
    dsd_p25p2_min_evt evg = {DSD_P25P2_MIN_EV_GRANT, -1, 0x2001, 851000000};
    g_tunes = g_returns = 0;
    dsd_p25p2_min_handle_event(&sm, &opts, &st, &evg);
    rc |= expect_eq_int("grant->tune", g_tunes, 1);
    rc |= expect_eq_int("state armed", dsd_p25p2_min_get_state(&sm), DSD_P25P2_MIN_ARMED);

    // No voice: simulate time past grant_voice_timeout -> tick should return
    sm.t_last_tune = time(NULL) - 1; // > 0.2s
    dsd_p25p2_min_tick(&sm, &opts, &st);
    rc |= expect_eq_int("armed timeout return", g_returns > 0, 1);

    // New GRANT then ACTIVE -> FOLLOW (clear backoff to allow immediate retune)
    g_tunes = g_returns = 0;
    sm.last_return_freq = 0;
    sm.t_last_return = 0;
    dsd_p25p2_min_handle_event(&sm, &opts, &st, &evg);
    dsd_p25p2_min_evt eva = {DSD_P25P2_MIN_EV_ACTIVE, 0, 0, 0};
    dsd_p25p2_min_handle_event(&sm, &opts, &st, &eva);
    rc |= expect_eq_int("state follow", dsd_p25p2_min_get_state(&sm), DSD_P25P2_MIN_FOLLOWING_VC);

    // IDLE (slot quiet) -> HANG
    dsd_p25p2_min_evt evi = {DSD_P25P2_MIN_EV_IDLE, 0, 0, 0};
    dsd_p25p2_min_handle_event(&sm, &opts, &st, &evi);
    rc |= expect_eq_int("state hang", dsd_p25p2_min_get_state(&sm), DSD_P25P2_MIN_HANG);

    // Simulate hangtime elapsed -> tick returns
    sm.t_hang_start = time(NULL) - 1; // > 0.5s
    dsd_p25p2_min_tick(&sm, &opts, &st);
    rc |= expect_eq_int("hang->return", g_returns > 0, 1);

    return rc;
}
