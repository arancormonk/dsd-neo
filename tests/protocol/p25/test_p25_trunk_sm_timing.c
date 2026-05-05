// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * P25 trunk SM tuning timing tests (TDMA vs FDMA).
 * Verifies samplesPerSymbol, symbolCenter, and active slot assignment.
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

struct RtlSdrContext;

// Stubs
bool
SetFreq(int sockfd, long int freq) {
    (void)sockfd;
    (void)freq;
    return false;
}

bool
SetModulation(int sockfd, int bandwidth) {
    (void)sockfd;
    (void)bandwidth;
    return false;
}

void
return_to_cc(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}
struct RtlSdrContext* g_rtl_ctx = 0;

int
rtl_stream_tune(struct RtlSdrContext* ctx, uint32_t center_freq_hz) {
    (void)ctx;
    (void)center_freq_hz;
    return 0;
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
    static dsd_opts opts;
    static dsd_state st;
    memset(&opts, 0, sizeof opts);
    memset(&st, 0, sizeof st);
    opts.p25_trunk = 1;
    opts.trunk_tune_group_calls = 1;
    st.p25_cc_freq = 851000000;

    // TDMA IDEN: id=2, type=3 => denom=2
    int id_t = 2;
    st.p25_chan_iden = id_t;
    // Populate new dual-array
    st.p25_iden_tdma[id_t].base_freq = 851000000 / 5;
    st.p25_iden_tdma[id_t].chan_type = 3;
    st.p25_iden_tdma[id_t].chan_spac = 100;
    st.p25_iden_tdma[id_t].trust = 2;
    st.p25_iden_tdma[id_t].populated = 1;
    st.p25_chan_tdma_explicit[id_t] = 2; // TDMA known

    // Odd channel low bit -> slot 1
    int ch_tdma = (id_t << 12) | 0x0001;
    p25_sm_on_group_grant(&opts, &st, ch_tdma, 0, 1234, 5678);
    rc |= expect_eq_int("tdma sps", st.samplesPerSymbol, 8);
    rc |= expect_eq_int("tdma center", st.symbolCenter, 3);
    rc |= expect_eq_int("tdma slot", st.p25_p2_active_slot, 1);
    p25_sm_on_release(&opts, &st);
    opts.p25_is_tuned = 0;

    // FDMA IDEN: id=1, type=1 => denom=1
    int id_f = 1;
    st.p25_chan_iden = id_f;
    // Populate new dual-array
    st.p25_iden_fdma[id_f].base_freq = 851000000 / 5;
    st.p25_iden_fdma[id_f].chan_type = 1;
    st.p25_iden_fdma[id_f].chan_spac = 100;
    st.p25_iden_fdma[id_f].trust = 2;
    st.p25_iden_fdma[id_f].populated = 1;
    st.p25_chan_tdma_explicit[id_f] = 1; // FDMA known
    int ch_fdma = (id_f << 12) | 0x000A;
    p25_sm_on_group_grant(&opts, &st, ch_fdma, 0, 555, 666);
    rc |= expect_eq_int("fdma sps", st.samplesPerSymbol, 10);
    rc |= expect_eq_int("fdma center", st.symbolCenter, 4);
    rc |= expect_eq_int("fdma slot unset", st.p25_p2_active_slot, -1);

    return rc;
}
