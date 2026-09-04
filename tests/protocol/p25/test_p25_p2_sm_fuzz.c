// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * P25 Phase 2 trunk SM fuzzer: interleave MAC-like activity/idle windows,
 * flip both slots' audio gates rapidly, occasionally set ENC pending flags,
 * and assert the state machine returns to CC within hangtime once both slots
 * are idle and activity has ceased. Also verifies forced release behavior.
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
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

// Strong stubs to observe tunes and releases
static int g_vc_tunes = 0;
static int g_cc_returns = 0;

dsd_trunk_tune_result
// NOLINTNEXTLINE(misc-use-internal-linkage)
trunk_tune_to_freq(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps, uint64_t request_id) {
    (void)request_id;
    (void)freq;
    (void)ted_sps;
    g_vc_tunes++;
    if (opts) {
        opts->trunk_is_tuned = 1;
    }
    if (state) {
        state->last_vc_sync_time = time(NULL);
    }
    return DSD_TRUNK_TUNE_RESULT_OK;
}

dsd_trunk_tune_result
// NOLINTNEXTLINE(misc-use-internal-linkage)
return_to_cc(dsd_opts* opts, dsd_state* state, uint64_t request_id) {
    (void)request_id;
    g_cc_returns++;
    if (opts) {
        opts->trunk_is_tuned = 0;
    }
    if (state) {
        state->p25_vc_freq[0] = state->p25_vc_freq[1] = 0;
        state->trunk_vc_freq[0] = state->trunk_vc_freq[1] = 0;
    }
    return DSD_TRUNK_TUNE_RESULT_OK;
}

static void
install_trunk_tuning_hooks(void) {
    dsd_trunk_tuning_hooks hooks = {0};
    hooks.tune_to_freq_request = trunk_tune_to_freq;
    hooks.return_to_cc_request = return_to_cc;
    dsd_trunk_tuning_hooks_set(hooks);
}

// Deterministic xorshift RNG
static uint32_t s_rng = 0xC0FFEEu;

static uint32_t
rnd(void) {
    uint32_t x = s_rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s_rng = x;
    return x;
}

static int
expect_true(const char* tag, int cond) {
    if (!cond) {
        DSD_FPRINTF(stderr, "%s: failed\n", tag);
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
    opts.trunk_hangtime = 1.0f;
    st.p25_cc_freq = 851000000;

    // Seed a TDMA IDEN so channel->freq mapping works for TDMA grants
    int id = 2;
    st.p25_iden_tdma[id].base_freq = 851000000 / 5;
    st.p25_iden_tdma[id].chan_type = 3;
    st.p25_iden_tdma[id].chan_spac = 100;
    st.p25_iden_tdma[id].trust = 2;
    st.p25_iden_tdma[id].populated = 1;
    st.p25_chan_tdma_explicit[id] = 2; // TDMA known

    // Run randomized trials
    const int trials = 128;
    for (int t = 0; t < trials; t++) {
        // TDMA channel: use low-bit as slot hint; vary channel index
        int low = (int)(rnd() % 16);
        int ch = (id << 12) | low;

        // Ensure untuned; request a grant
        opts.trunk_is_tuned = 0;
        int before = g_vc_tunes;
        p25_sm_event(p25_sm_get_ctx(), &opts, &st,
                     &(p25_sm_event_t){.type = P25_SM_EV_GRANT,
                                       .slot = -1,
                                       .channel = ch,
                                       .tg = 40000 + (t & 0xFF),
                                       .src = 1000 + (t & 0xFF),
                                       .svc_bits = 0x00,
                                       .is_group = 1});
        int tuned = (g_vc_tunes > before) && (opts.trunk_is_tuned == 1);

        if (!tuned) {
            // If not tuned (e.g., mapping refused), skip this trial
            continue;
        }

        // Simulate MAC_SIGNAL bursts by refreshing activity; randomly flip audio gates
        st.p25_p2_active_slot = (ch & 1) ? 1 : 0;
        for (int k = 0; k < 8; k++) {
            st.p25_p2_audio_allowed[0] = (rnd() & 1);
            st.p25_p2_audio_allowed[1] = (rnd() & 1);
            st.last_vc_sync_time = time(NULL);
        }

        // Now simulate MAC_IDLE: both slots idle, activity stale beyond hangtime
        st.p25_p2_audio_allowed[0] = 0;
        st.p25_p2_audio_allowed[1] = 0;
        st.last_vc_sync_time = time(NULL) - 3; // > hangtime

        // Give the SM a few ticks to release
        int steps = 0;
        const int max_steps = 5;
        int before_rtc = g_cc_returns;
        while (opts.trunk_is_tuned == 1 && steps++ < max_steps) {
            p25_sm_tick_ctx(p25_sm_get_ctx(), &opts, &st);
        }
        rc |= expect_true("p2-release", g_cc_returns > before_rtc && opts.trunk_is_tuned == 0);

        // Forced release should always return to CC even if timers are borderline
        opts.trunk_is_tuned = 1; // pretend we’re back on VC
        st.p25_p2_audio_allowed[0] = st.p25_p2_audio_allowed[1] = 0;
        st.p25_sm_force_release = 1;
        before_rtc = g_cc_returns;
        p25_sm_release(p25_sm_get_ctx(), &opts, &st, "explicit-release");
        rc |= expect_true("p2-forced-release", g_cc_returns > before_rtc && opts.trunk_is_tuned == 0);
    }

    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
