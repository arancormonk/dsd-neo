// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * P25 grant trust clamp tests.
 * Ensures untrusted IDENs block tuning unless provisional (provenance unset)
 * on current CC, in which case tuning is allowed.
 */

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
    install_trunk_tuning_hooks();
    const int iden = 1;
    const int channel = (iden << 12) | 0x000A; // ch=10

    // Common IDEN params and CC freq
    static dsd_opts opts;
    static dsd_state st;
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&st, 0, sizeof st);
    opts.trunk_enable = 1;
    opts.trunk_tune_group_calls = 1;
    st.p25_cc_freq = 851000000;
    // Populate new dual-array
    st.p25_iden_fdma[iden].base_freq = 851000000 / 5;
    st.p25_iden_fdma[iden].chan_type = 1;
    st.p25_iden_fdma[iden].chan_spac = 100;
    st.p25_iden_fdma[iden].populated = 1;
    st.p25_chan_tdma_explicit[iden] = 1;

    // Case: trust<2 but on CC and provenance unset → allowed
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&st, 0, sizeof st);
    opts.trunk_enable = 1;
    opts.trunk_tune_group_calls = 1;
    st.p25_cc_freq = 851000000;
    // Populate new dual-array
    st.p25_iden_fdma[iden].base_freq = 851000000 / 5;
    st.p25_iden_fdma[iden].chan_type = 1;
    st.p25_iden_fdma[iden].chan_spac = 100;
    st.p25_iden_fdma[iden].trust = 1;
    st.p25_iden_fdma[iden].populated = 1;
    st.p25_chan_tdma_explicit[iden] = 1;
    unsigned int before = st.p25_sm_tune_count;
    p25_sm_event(p25_sm_get_ctx(), &opts, &st,
                 &(p25_sm_event_t){.type = P25_SM_EV_GRANT,
                                   .slot = -1,
                                   .channel = channel,
                                   .tg = 1234,
                                   .src = 5678,
                                   .svc_bits = 0,
                                   .is_group = 1});
    rc |= expect_true("tune allowed provisional", st.p25_sm_tune_count == before + 1);
    rc |= expect_true("tuned flag set", opts.trunk_is_tuned == 1);
    rc |= expect_true("vc freq set", st.p25_vc_freq[0] != 0);

    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
