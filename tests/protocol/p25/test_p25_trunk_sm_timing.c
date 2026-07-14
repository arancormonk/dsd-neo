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
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

static dsd_trunk_tune_result
test_tune_request(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps, uint64_t request_id) {
    (void)opts;
    (void)state;
    (void)ted_sps;
    (void)request_id;
    return freq > 0 ? DSD_TRUNK_TUNE_RESULT_OK : DSD_TRUNK_TUNE_RESULT_FAILED;
}

static dsd_trunk_tune_result
test_return_request(dsd_opts* opts, dsd_state* state, uint64_t request_id) {
    (void)opts;
    (void)state;
    (void)request_id;
    return DSD_TRUNK_TUNE_RESULT_OK;
}

static void
install_trunk_tuning_hooks(void) {
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){
        .tune_to_freq_request = test_tune_request,
        .tune_to_cc_request = test_tune_request,
        .return_to_cc_request = test_return_request,
    });
}

static int
expect_eq_int(const char* tag, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %d want %d\n", tag, got, want);
        return 1;
    }
    return 0;
}

static void
setup_opts(dsd_opts* opts, int input_rate_hz) {
    DSD_MEMSET(opts, 0, sizeof(*opts));
    opts->trunk_enable = 1;
    opts->trunk_tune_group_calls = 1;
    if (input_rate_hz > 0) {
        opts->audio_in_type = AUDIO_IN_WAV;
        opts->wav_sample_rate = input_rate_hz;
        opts->wav_decimator = 48000;
    }
}

static void
setup_tdma_grant(dsd_state* st, int* out_channel) {
    // TDMA IDEN: id=2, type=3 => denom=2
    int id_t = 2;
    st->p25_chan_iden = id_t;
    // Populate new dual-array
    st->p25_iden_tdma[id_t].base_freq = 851000000 / 5;
    st->p25_iden_tdma[id_t].chan_type = 3;
    st->p25_iden_tdma[id_t].chan_spac = 100;
    st->p25_iden_tdma[id_t].trust = 2;
    st->p25_iden_tdma[id_t].populated = 1;
    st->p25_chan_tdma_explicit[id_t] = 2; // TDMA known

    // Odd channel low bit -> slot 1
    *out_channel = (id_t << 12) | 0x0001;
}

static void
setup_fdma_grant(dsd_state* st, int* out_channel) {
    // FDMA IDEN: id=1, type=1 => denom=1
    int id_f = 1;
    st->p25_chan_iden = id_f;
    // Populate new dual-array
    st->p25_iden_fdma[id_f].base_freq = 851000000 / 5;
    st->p25_iden_fdma[id_f].chan_type = 1;
    st->p25_iden_fdma[id_f].chan_spac = 100;
    st->p25_iden_fdma[id_f].trust = 2;
    st->p25_iden_fdma[id_f].populated = 1;
    st->p25_chan_tdma_explicit[id_f] = 1; // FDMA known
    *out_channel = (id_f << 12) | 0x000A;
}

static int
run_grant_case(const char* label, int input_rate_hz, int is_tdma, int expected_sps, int expected_slot) {
    static dsd_opts opts;
    static dsd_state st;
    static p25_sm_ctx_t ctx;
    int channel = 0;
    int rc = 0;

    setup_opts(&opts, input_rate_hz);
    dsd_state_ext_free_all(&st);
    DSD_MEMSET(&st, 0, sizeof st);
    DSD_MEMSET(&ctx, 0, sizeof ctx);
    st.p25_cc_freq = 851000000;
    st.trunk_cc_freq = 851000000;

    if (is_tdma) {
        setup_tdma_grant(&st, &channel);
    } else {
        setup_fdma_grant(&st, &channel);
    }

    p25_sm_init_ctx(&ctx, &opts, &st);
    p25_sm_event_t ev = p25_sm_ev_group_grant(channel, 0, 1234, 5678, 0);
    p25_sm_event(&ctx, &opts, &st, &ev);

    rc |= expect_eq_int(label, st.samplesPerSymbol, expected_sps);
    rc |= expect_eq_int("symbol center", st.symbolCenter, dsd_opts_symbol_center(expected_sps));
    rc |= expect_eq_int("active slot", st.p25_p2_active_slot, expected_slot);
    dsd_state_ext_free_all(&st);
    return rc;
}

static int
run_timing_case(int input_rate_hz, int expected_tdma_sps, int expected_fdma_sps) {
    int rc = 0;
    rc |= run_grant_case("tdma sps", input_rate_hz, 1, expected_tdma_sps, 1);
    rc |= run_grant_case("fdma sps", input_rate_hz, 0, expected_fdma_sps, -1);
    return rc;
}

int
main(void) {
    int rc = 0;
    install_trunk_tuning_hooks();

    rc |= run_timing_case(0, 8, 10);
    rc |= run_timing_case(96000, 16, 20);

    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
