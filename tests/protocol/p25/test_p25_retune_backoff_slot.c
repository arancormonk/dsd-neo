// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Verify retune backoff applies per-slot on TDMA voice channels:
 * - After returning from a VC with no voice observed, a short retune backoff
 *   is applied for the same RF frequency and slot.
 * - A subsequent grant to the opposite slot at the same RF is allowed
 *   immediately (no backoff).
 * - Multiple failed VC/slot pairs remain blocked concurrently instead of the
 *   newest failure replacing the previous one.
 */

#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

struct RtlSdrContext;

// --- IO control stubs (rigctl/RTL) ---

bool
// NOLINTNEXTLINE(misc-use-internal-linkage)
SetFreq(int sockfd, long int freq) {
    (void)sockfd;
    (void)freq;
    return false;
}

bool
// NOLINTNEXTLINE(misc-use-internal-linkage)
SetModulation(int sockfd, int bandwidth) {
    (void)sockfd;
    (void)bandwidth;
    return false;
}

struct RtlSdrContext* g_rtl_ctx = 0; // NOLINT(misc-use-internal-linkage)

static long g_last_tuned_vc = 0;
static int g_tune_to_freq_calls = 0;
static int g_return_to_cc_calls = 0;
static dsd_trunk_tune_result g_return_to_cc_result = DSD_TRUNK_TUNE_RESULT_OK;

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
rtl_stream_tune(struct RtlSdrContext* ctx, uint32_t center_freq_hz) {
    (void)ctx;
    (void)center_freq_hz;
    return 0;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
return_to_cc(dsd_opts* opts, dsd_state* state) {
    if (opts) {
        opts->p25_is_tuned = 0;
        opts->trunk_is_tuned = 0;
    }
    if (state) {
        state->p25_vc_freq[0] = state->p25_vc_freq[1] = 0;
        state->trunk_vc_freq[0] = state->trunk_vc_freq[1] = 0;
    }
}

static dsd_trunk_tune_result
tune_to_freq_result(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps) {
    (void)ted_sps;
    g_tune_to_freq_calls++;
    g_last_tuned_vc = freq;
    if (opts) {
        opts->p25_is_tuned = 1;
        opts->trunk_is_tuned = 1;
    }
    if (state) {
        state->p25_vc_freq[0] = state->p25_vc_freq[1] = freq;
        state->trunk_vc_freq[0] = state->trunk_vc_freq[1] = freq;
    }
    return DSD_TRUNK_TUNE_RESULT_OK;
}

static dsd_trunk_tune_result
return_to_cc_result(dsd_opts* opts, dsd_state* state) {
    g_return_to_cc_calls++;
    if (!dsd_trunk_tune_result_is_ok(g_return_to_cc_result)) {
        return g_return_to_cc_result;
    }
    return_to_cc(opts, state);
    return g_return_to_cc_result;
}

static void
install_tuning_hooks(void) {
    dsd_trunk_tuning_hooks hooks = {0};
    hooks.tune_to_freq_result = tune_to_freq_result;
    hooks.return_to_cc_result = return_to_cc_result;
    dsd_trunk_tuning_hooks_set(hooks);
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
    install_tuning_hooks();

    // Enable trunking and seed a CC
    opts.p25_trunk = 1;
    opts.trunk_tune_group_calls = 1;
    opts.trunk_hangtime = 0.2f;
    opts.p25_grant_voice_to_s = 0.5; // apply backoff when dt_since_tune >= 0.5s
    opts.p25_retune_backoff_s = 2.0; // backoff window
    st.p25_cc_freq = 851000000;      // known CC

    // TDMA IDEN: id=2, type=3 => denom=2; trusted
    int id = 2;
    st.p25_chan_iden = id;
    // Populate new dual-array
    st.p25_iden_tdma[id].base_freq = 851000000 / 5;
    st.p25_iden_tdma[id].chan_type = 3;
    st.p25_iden_tdma[id].chan_spac = 100;
    st.p25_iden_tdma[id].trust = 2;
    st.p25_iden_tdma[id].populated = 1;
    st.p25_chan_tdma_explicit[id] = 2; // TDMA known

    // Two channels mapping to the same RF: low bit selects slot
    int ch_slot0 = (id << 12) | 0x0002; // slot 0
    int ch_slot1 = (id << 12) | 0x0003; // slot 1 (same RF)

    p25_sm_ctx_t ctx;
    p25_sm_init_ctx(&ctx, &opts, &st);

    // 1) Grant on slot 1 -> tune
    p25_sm_event_t ev_slot1 = p25_sm_ev_group_grant(ch_slot1, 0, 1001, 2002, 0);
    p25_sm_event(&ctx, &opts, &st, &ev_slot1);
    rc |= expect_true("initial tune", st.p25_sm_tune_count == 1 && opts.p25_is_tuned == 1 && g_tune_to_freq_calls == 1);

    // 2) Let the grant timeout without any voice/VC sync; this arms backoff for slot 1.
    ctx.t_tune_m = dsd_time_now_monotonic_s() - 1.0;
    ctx.t_voice_m = 0.0;
    p25_sm_tick_ctx(&ctx, &opts, &st);
    rc |= expect_true("returned", opts.p25_is_tuned == 0 && g_return_to_cc_calls == 1 && ctx.state == P25_SM_ON_CC);
    rc |= expect_true("backoff armed", st.p25_retune_block_freq == g_last_tuned_vc && st.p25_retune_block_slot == 1
                                           && st.p25_retune_block_until > time(NULL));

    // 3) Same-slot repeat grant on the same RF should be blocked during backoff.
    p25_sm_event(&ctx, &opts, &st, &ev_slot1);
    rc |= expect_true("same-slot blocked", g_tune_to_freq_calls == 1 && opts.p25_is_tuned == 0);

    // 4) Opposite-slot grant on the same RF should be allowed immediately.
    p25_sm_event_t ev_slot0 = p25_sm_ev_group_grant(ch_slot0, 0, 1001, 2002, 0);
    p25_sm_event(&ctx, &opts, &st, &ev_slot0);
    rc |= expect_true("other-slot allowed", g_tune_to_freq_calls == 2 && opts.p25_is_tuned == 1);

    // 5) When the second slot also fails before voice, both recent failures
    // should remain blocked. This catches single-entry backoff regressions.
    ctx.t_tune_m = dsd_time_now_monotonic_s() - 1.0;
    ctx.t_voice_m = 0.0;
    p25_sm_tick_ctx(&ctx, &opts, &st);
    rc |= expect_true("second returned",
                      opts.p25_is_tuned == 0 && g_return_to_cc_calls == 2 && ctx.state == P25_SM_ON_CC);
    rc |=
        expect_true("second backoff armed", st.p25_retune_block_freq == g_last_tuned_vc && st.p25_retune_block_slot == 0
                                                && st.p25_retune_block_until > time(NULL));

    p25_sm_event(&ctx, &opts, &st, &ev_slot1);
    rc |= expect_true("first failed slot still blocked", g_tune_to_freq_calls == 2 && opts.p25_is_tuned == 0);
    p25_sm_event(&ctx, &opts, &st, &ev_slot0);
    rc |= expect_true("second failed slot blocked", g_tune_to_freq_calls == 2 && opts.p25_is_tuned == 0);

    // 6) A decoder/frame-sync forced release before grant timeout still records
    // the failed VC because no voice was observed.
    static dsd_opts forced_opts;
    static dsd_state forced_st;
    DSD_MEMSET(&forced_opts, 0, sizeof forced_opts);
    DSD_MEMSET(&forced_st, 0, sizeof forced_st);
    forced_opts.p25_trunk = 1;
    forced_opts.trunk_tune_group_calls = 1;
    forced_opts.trunk_hangtime = 0.2f;
    forced_opts.p25_grant_voice_to_s = 10.0;
    forced_opts.p25_retune_backoff_s = 2.0;
    forced_st.p25_cc_freq = 851000000;
    forced_st.p25_chan_iden = id;
    forced_st.p25_iden_tdma[id] = st.p25_iden_tdma[id];
    forced_st.p25_chan_tdma_explicit[id] = 2;

    p25_sm_ctx_t forced_ctx;
    p25_sm_init_ctx(&forced_ctx, &forced_opts, &forced_st);
    g_last_tuned_vc = 0;
    g_tune_to_freq_calls = 0;
    g_return_to_cc_calls = 0;
    p25_sm_event(&forced_ctx, &forced_opts, &forced_st, &ev_slot1);
    rc |= expect_true("forced initial tune",
                      forced_st.p25_sm_tune_count == 1 && forced_opts.p25_is_tuned == 1 && g_tune_to_freq_calls == 1);

    forced_ctx.t_tune_m = dsd_time_now_monotonic_s() - 1.0;
    forced_ctx.t_voice_m = 0.0;
    forced_st.p25_sm_force_release = 1;
    p25_sm_release(&forced_ctx, &forced_opts, &forced_st, "explicit-release");
    rc |= expect_true("forced returned",
                      forced_opts.p25_is_tuned == 0 && g_return_to_cc_calls == 1 && forced_ctx.state == P25_SM_ON_CC);
    rc |= expect_true("forced backoff armed", forced_st.p25_retune_block_freq == g_last_tuned_vc
                                                  && forced_st.p25_retune_block_slot == 1
                                                  && forced_st.p25_retune_block_until > time(NULL));

    // 7) A transient return-to-CC failure during grant timeout must not record
    // a failed VC until the retry is accepted.
    static const dsd_trunk_tune_result transient_returns[] = {
        DSD_TRUNK_TUNE_RESULT_DEFERRED,
        DSD_TRUNK_TUNE_RESULT_FAILED,
    };

    for (size_t i = 0; i < sizeof(transient_returns) / sizeof(transient_returns[0]); i++) {
        static dsd_opts transient_opts;
        static dsd_state transient_st;
        DSD_MEMSET(&transient_opts, 0, sizeof transient_opts);
        DSD_MEMSET(&transient_st, 0, sizeof transient_st);
        transient_opts.p25_trunk = 1;
        transient_opts.trunk_tune_group_calls = 1;
        transient_opts.trunk_hangtime = 0.2f;
        transient_opts.p25_grant_voice_to_s = 0.5;
        transient_opts.p25_retune_backoff_s = 2.0;
        transient_st.p25_cc_freq = 851000000;
        transient_st.p25_chan_iden = id;
        transient_st.p25_iden_tdma[id] = st.p25_iden_tdma[id];
        transient_st.p25_chan_tdma_explicit[id] = 2;

        p25_sm_ctx_t transient_ctx;
        p25_sm_init_ctx(&transient_ctx, &transient_opts, &transient_st);
        g_last_tuned_vc = 0;
        g_tune_to_freq_calls = 0;
        g_return_to_cc_calls = 0;
        g_return_to_cc_result = transient_returns[i];
        p25_sm_event(&transient_ctx, &transient_opts, &transient_st, &ev_slot1);
        rc |=
            expect_true("transient initial tune", transient_st.p25_sm_tune_count == 1
                                                      && transient_opts.p25_is_tuned == 1 && g_tune_to_freq_calls == 1);

        transient_ctx.t_tune_m = dsd_time_now_monotonic_s() - 1.0;
        transient_ctx.t_voice_m = 0.0;
        p25_sm_tick_ctx(&transient_ctx, &transient_opts, &transient_st);
        rc |= expect_true("transient return preserved vc", transient_opts.p25_is_tuned == 1 && g_return_to_cc_calls == 1
                                                               && transient_ctx.state == P25_SM_TUNED);
        rc |= expect_true("transient return did not arm backoff",
                          transient_st.p25_retune_block_until == 0 && transient_st.p25_retune_block_freq == 0);

        g_return_to_cc_result = DSD_TRUNK_TUNE_RESULT_OK;
        p25_sm_tick_ctx(&transient_ctx, &transient_opts, &transient_st);
        rc |= expect_true("transient retry returned", transient_opts.p25_is_tuned == 0 && g_return_to_cc_calls == 2
                                                          && transient_ctx.state == P25_SM_ON_CC);
        rc |= expect_true("transient retry backoff armed", transient_st.p25_retune_block_freq == g_last_tuned_vc
                                                               && transient_st.p25_retune_block_slot == 1
                                                               && transient_st.p25_retune_block_until > time(NULL));
        dsd_state_ext_free_all(&transient_st);
    }
    g_return_to_cc_result = DSD_TRUNK_TUNE_RESULT_OK;

    dsd_state_ext_free_all(&forced_st);
    dsd_state_ext_free_all(&st);

    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
