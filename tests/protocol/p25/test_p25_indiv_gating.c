// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Verify on_indiv_grant gating for data/private/enc policies. */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
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
expect_true(const char* tag, int cond) {
    if (!cond) {
        DSD_FPRINTF(stderr, "%s: expected true\n", tag);
        return 1;
    }
    return 0;
}

int
main(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state st;
    install_trunk_tuning_hooks();
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&st, 0, sizeof st);
    opts.trunk_enable = 1;
    st.p25_cc_freq = 851000000;
    // FDMA IDEN
    int id = 1;
    st.p25_chan_iden = id;
    // Populate new dual-array
    st.p25_iden_fdma[id].base_freq = 851000000 / 5;
    st.p25_iden_fdma[id].chan_type = 1;
    st.p25_iden_fdma[id].chan_spac = 100;
    st.p25_iden_fdma[id].trust = 2;
    st.p25_iden_fdma[id].populated = 1;
    st.p25_chan_tdma_explicit[id] = 1; // FDMA known
    int ch = (id << 12) | 0x000A;

    // Case A: private tuning disabled → block
    opts.trunk_tune_private_calls = 0;
    unsigned before = st.p25_sm_tune_count;
    p25_sm_event(p25_sm_get_ctx(), &opts, &st,
                 &(p25_sm_event_t){.type = P25_SM_EV_GRANT,
                                   .slot = -1,
                                   .channel = ch,
                                   .dst = 1001,
                                   .src = 1002,
                                   .svc_bits = 0x00,
                                   .is_group = 0});
    rc |= expect_true("private off", st.p25_sm_tune_count == before);

    // Case B: private on but data disabled → block when 0x10 set
    opts.trunk_tune_private_calls = 1;
    opts.trunk_tune_data_calls = 0;
    before = st.p25_sm_tune_count;
    p25_sm_event(p25_sm_get_ctx(), &opts, &st,
                 &(p25_sm_event_t){.type = P25_SM_EV_GRANT,
                                   .slot = -1,
                                   .channel = ch,
                                   .dst = 1001,
                                   .src = 1002,
                                   .svc_bits = 0x10,
                                   .is_group = 0});
    rc |= expect_true("data off", st.p25_sm_tune_count == before);

    // Case C: with ENC lockout enabled, an otherwise allowed encrypted private
    // grant tunes as a silent classification probe.
    opts.trunk_tune_data_calls = 1;
    opts.trunk_tune_enc_calls = 0;
    before = st.p25_sm_tune_count;
    p25_sm_event(p25_sm_get_ctx(), &opts, &st,
                 &(p25_sm_event_t){.type = P25_SM_EV_GRANT,
                                   .slot = -1,
                                   .channel = ch,
                                   .dst = 1001,
                                   .src = 1002,
                                   .svc_bits = 0x40,
                                   .is_group = 0});
    rc |= expect_true("enc lockout probes", st.p25_sm_tune_count == before + 1);
    rc |= expect_true("enc lockout probe pending", st.p25_crypto_state[0] == DSD_P25_CRYPTO_ENCRYPTED_PENDING);

    // Case D: all enabled → tune
    p25_sm_release(p25_sm_get_ctx(), &opts, &st, "explicit-release");
    opts.trunk_is_tuned = 0;
    p25_sm_init_ctx(p25_sm_get_ctx(), &opts, &st);
    opts.trunk_tune_private_calls = 1;
    opts.trunk_tune_data_calls = 1;
    opts.trunk_tune_enc_calls = 1;
    before = st.p25_sm_tune_count;
    p25_sm_event(p25_sm_get_ctx(), &opts, &st,
                 &(p25_sm_event_t){.type = P25_SM_EV_GRANT,
                                   .slot = -1,
                                   .channel = ch,
                                   .dst = 1001,
                                   .src = 1002,
                                   .svc_bits = 0x40,
                                   .is_group = 0});
    rc |= expect_true("all on tunes", st.p25_sm_tune_count == before + 1);

    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
