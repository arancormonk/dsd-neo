// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Verify on_indiv_grant gating for data/private/enc policies. */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

struct RtlSdrContext;

bool
SetFreq(int sockfd, long int freq) { // NOLINT(misc-use-internal-linkage)
    (void)sockfd;
    (void)freq;
    return false;
}

bool
SetModulation(int sockfd, int bandwidth) { // NOLINT(misc-use-internal-linkage)
    (void)sockfd;
    (void)bandwidth;
    return false;
}
struct RtlSdrContext* g_rtl_ctx = 0; // NOLINT(misc-use-internal-linkage)

int
rtl_stream_tune(struct RtlSdrContext* ctx, uint32_t center_freq_hz) { // NOLINT(misc-use-internal-linkage)
    (void)ctx;
    (void)center_freq_hz;
    return 0;
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
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&st, 0, sizeof st);
    opts.p25_trunk = 1;
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
    p25_sm_on_indiv_grant(&opts, &st, ch, /*svc*/ 0x00, /*dst*/ 1001, /*src*/ 1002);
    rc |= expect_true("private off", st.p25_sm_tune_count == before);

    // Case B: private on but data disabled → block when 0x10 set
    opts.trunk_tune_private_calls = 1;
    opts.trunk_tune_data_calls = 0;
    before = st.p25_sm_tune_count;
    p25_sm_on_indiv_grant(&opts, &st, ch, /*svc*/ 0x10, /*dst*/ 1001, /*src*/ 1002);
    rc |= expect_true("data off", st.p25_sm_tune_count == before);

    // Case C: with ENC lockout enabled, an otherwise allowed encrypted private
    // grant tunes as a silent classification probe.
    opts.trunk_tune_data_calls = 1;
    opts.trunk_tune_enc_calls = 0;
    before = st.p25_sm_tune_count;
    p25_sm_on_indiv_grant(&opts, &st, ch, /*svc*/ 0x40, /*dst*/ 1001, /*src*/ 1002);
    rc |= expect_true("enc lockout probes", st.p25_sm_tune_count == before + 1);
    rc |= expect_true("enc lockout probe pending", st.p25_crypto_state[0] == DSD_P25_CRYPTO_ENCRYPTED_PENDING);

    // Case D: all enabled → tune
    p25_sm_on_release(&opts, &st);
    opts.p25_is_tuned = 0;
    opts.trunk_is_tuned = 0;
    p25_sm_init(&opts, &st);
    opts.trunk_tune_private_calls = 1;
    opts.trunk_tune_data_calls = 1;
    opts.trunk_tune_enc_calls = 1;
    before = st.p25_sm_tune_count;
    p25_sm_on_indiv_grant(&opts, &st, ch, /*svc*/ 0x40, /*dst*/ 1001, /*src*/ 1002);
    rc |= expect_true("all on tunes", st.p25_sm_tune_count == before + 1);

    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
