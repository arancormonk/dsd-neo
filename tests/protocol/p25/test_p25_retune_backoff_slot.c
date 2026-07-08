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
 * - Explicit data grants bypass stale failed-voice backoff on the same slot.
 */

#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/platform/posix_compat.h>
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

static int
retune_backoff_empty(const dsd_state* state) {
    if (!state || state->p25_retune_block_until != 0 || state->p25_retune_block_freq != 0) {
        return 0;
    }
    for (int i = 0; i < DSD_P25_RETUNE_BLOCK_HISTORY_DEPTH; i++) {
        if (state->p25_retune_block_history_until[i] != 0 || state->p25_retune_block_history_freq[i] != 0) {
            return 0;
        }
    }
    return 1;
}

static int
retune_backoff_history_has_slot(const dsd_state* state, long freq, int slot) {
    if (!state || freq <= 0) {
        return 0;
    }
    for (int i = 0; i < DSD_P25_RETUNE_BLOCK_HISTORY_DEPTH; i++) {
        if (state->p25_retune_block_history_freq[i] == freq && state->p25_retune_block_history_slot[i] == slot
            && state->p25_retune_block_history_until[i] > time(NULL)) {
            return 1;
        }
    }
    return 0;
}

static int
seed_exact(dsd_state* st, uint32_t id, const char* mode, const char* name, int priority, int preempt) {
    dsd_tg_policy_entry row;
    if (dsd_tg_policy_make_exact_entry(id, mode, name, DSD_TG_POLICY_SOURCE_IMPORTED, &row) != 0) {
        return 1;
    }
    row.priority = priority;
    row.preempt = preempt ? 1u : 0u;
    return dsd_tg_policy_upsert_exact(st, &row, DSD_TG_POLICY_UPSERT_REPLACE_FIRST);
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

    // 5b) Channel 0 is a valid IDEN 0 TDMA slot. When both slots on the same
    // RF fail together, channel 0/slot 0 must remain in the per-slot backoff.
    {
        static dsd_opts zero_opts;
        static dsd_state zero_st;
        DSD_MEMSET(&zero_opts, 0, sizeof zero_opts);
        DSD_MEMSET(&zero_st, 0, sizeof zero_st);
        zero_opts.p25_trunk = 1;
        zero_opts.trunk_tune_group_calls = 1;
        zero_opts.trunk_hangtime = 0.2f;
        zero_opts.p25_grant_voice_to_s = 0.5;
        zero_opts.p25_retune_backoff_s = 2.0;
        zero_st.p25_cc_freq = 851000000;
        zero_st.p25_chan_iden = 0;
        zero_st.p25_iden_tdma[0].base_freq = 851000000 / 5;
        zero_st.p25_iden_tdma[0].chan_type = 3;
        zero_st.p25_iden_tdma[0].chan_spac = 100;
        zero_st.p25_iden_tdma[0].trust = 2;
        zero_st.p25_iden_tdma[0].populated = 1;
        zero_st.p25_chan_tdma_explicit[0] = 2;

        p25_sm_ctx_t zero_ctx;
        p25_sm_init_ctx(&zero_ctx, &zero_opts, &zero_st);
        g_last_tuned_vc = 0;
        g_tune_to_freq_calls = 0;
        g_return_to_cc_calls = 0;

        p25_sm_event_t zero_ev_slot1 = p25_sm_ev_group_grant(0x0001, 0, 3001, 4001, 0);
        p25_sm_event_t zero_ev_slot0 = p25_sm_ev_group_grant(0x0000, 0, 3000, 4000, 0);
        p25_sm_event(&zero_ctx, &zero_opts, &zero_st, &zero_ev_slot1);
        rc |= expect_true("zero channel initial tune", g_tune_to_freq_calls == 1 && zero_opts.p25_is_tuned == 1);
        p25_sm_event(&zero_ctx, &zero_opts, &zero_st, &zero_ev_slot0);
        rc |= expect_true("zero channel same-carrier no retune", g_tune_to_freq_calls == 1);
        rc |= expect_true("zero channel both slots active",
                          zero_ctx.slots[0].grant_active && zero_ctx.slots[1].grant_active);

        zero_ctx.t_tune_m = dsd_time_now_monotonic_s() - 1.0;
        zero_ctx.t_voice_m = 0.0;
        p25_sm_tick_ctx(&zero_ctx, &zero_opts, &zero_st);
        rc |= expect_true("zero channel returned",
                          zero_opts.p25_is_tuned == 0 && g_return_to_cc_calls == 1 && zero_ctx.state == P25_SM_ON_CC);
        rc |= expect_true("zero channel slot0 backoff remembered",
                          retune_backoff_history_has_slot(&zero_st, g_last_tuned_vc, 0));
        rc |= expect_true("zero channel slot1 backoff remembered",
                          retune_backoff_history_has_slot(&zero_st, g_last_tuned_vc, 1));

        p25_sm_event(&zero_ctx, &zero_opts, &zero_st, &zero_ev_slot0);
        rc |= expect_true("zero channel slot0 blocked", g_tune_to_freq_calls == 1 && zero_opts.p25_is_tuned == 0);
        p25_sm_event(&zero_ctx, &zero_opts, &zero_st, &zero_ev_slot1);
        rc |= expect_true("zero channel slot1 blocked", g_tune_to_freq_calls == 1 && zero_opts.p25_is_tuned == 0);

        dsd_state_ext_free_all(&zero_st);
    }

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

    // 7) Data grants time out and forced-release without arming failed-voice backoff.
    static dsd_opts data_opts;
    static dsd_state data_st;
    DSD_MEMSET(&data_opts, 0, sizeof data_opts);
    DSD_MEMSET(&data_st, 0, sizeof data_st);
    data_opts.p25_trunk = 1;
    data_opts.trunk_tune_group_calls = 1;
    data_opts.trunk_tune_data_calls = 1;
    data_opts.trunk_hangtime = 0.2f;
    data_opts.p25_grant_voice_to_s = 0.5;
    data_opts.p25_retune_backoff_s = 2.0;
    data_st.p25_cc_freq = 851000000;
    data_st.p25_chan_iden = id;
    data_st.p25_iden_tdma[id] = st.p25_iden_tdma[id];
    data_st.p25_chan_tdma_explicit[id] = 2;

    p25_sm_ctx_t data_ctx;
    p25_sm_init_ctx(&data_ctx, &data_opts, &data_st);
    g_last_tuned_vc = 0;
    g_tune_to_freq_calls = 0;
    g_return_to_cc_calls = 0;
    p25_sm_event_t data_ev_slot1 = p25_sm_ev_group_data_grant(ch_slot1, 0, 1001, 2002, P25_SM_SVC_UNKNOWN);
    p25_sm_event(&data_ctx, &data_opts, &data_st, &data_ev_slot1);
    rc |= expect_true("data initial tune", data_st.p25_sm_tune_count == 1 && data_opts.p25_is_tuned == 1
                                               && g_tune_to_freq_calls == 1 && data_ctx.vc_data_call == 1);

    data_ctx.t_tune_m = dsd_time_now_monotonic_s() - 1.0;
    data_ctx.t_voice_m = 0.0;
    p25_sm_tick_ctx(&data_ctx, &data_opts, &data_st);
    rc |= expect_true("data returned",
                      data_opts.p25_is_tuned == 0 && g_return_to_cc_calls == 1 && data_ctx.state == P25_SM_ON_CC);
    rc |= expect_true("data timeout does not arm backoff", retune_backoff_empty(&data_st));

    p25_sm_event(&data_ctx, &data_opts, &data_st, &ev_slot1);
    rc |= expect_true("voice grant after data timeout allowed",
                      g_tune_to_freq_calls == 2 && data_opts.p25_is_tuned == 1 && data_ctx.vc_data_call == 0);

    static dsd_opts data_forced_opts;
    static dsd_state data_forced_st;
    DSD_MEMSET(&data_forced_opts, 0, sizeof data_forced_opts);
    DSD_MEMSET(&data_forced_st, 0, sizeof data_forced_st);
    data_forced_opts.p25_trunk = 1;
    data_forced_opts.trunk_tune_group_calls = 1;
    data_forced_opts.trunk_tune_data_calls = 1;
    data_forced_opts.trunk_hangtime = 0.2f;
    data_forced_opts.p25_grant_voice_to_s = 10.0;
    data_forced_opts.p25_retune_backoff_s = 2.0;
    data_forced_st.p25_cc_freq = 851000000;
    data_forced_st.p25_chan_iden = id;
    data_forced_st.p25_iden_tdma[id] = st.p25_iden_tdma[id];
    data_forced_st.p25_chan_tdma_explicit[id] = 2;

    p25_sm_ctx_t data_forced_ctx;
    p25_sm_init_ctx(&data_forced_ctx, &data_forced_opts, &data_forced_st);
    g_last_tuned_vc = 0;
    g_tune_to_freq_calls = 0;
    g_return_to_cc_calls = 0;
    p25_sm_event(&data_forced_ctx, &data_forced_opts, &data_forced_st, &data_ev_slot1);
    rc |= expect_true("forced data initial tune", data_forced_st.p25_sm_tune_count == 1
                                                      && data_forced_opts.p25_is_tuned == 1 && g_tune_to_freq_calls == 1
                                                      && data_forced_ctx.vc_data_call == 1);

    data_forced_ctx.t_tune_m = dsd_time_now_monotonic_s() - 1.0;
    data_forced_ctx.t_voice_m = 0.0;
    data_forced_st.p25_sm_force_release = 1;
    p25_sm_release(&data_forced_ctx, &data_forced_opts, &data_forced_st, "explicit-data-release");
    rc |= expect_true("forced data returned", data_forced_opts.p25_is_tuned == 0 && g_return_to_cc_calls == 1
                                                  && data_forced_ctx.state == P25_SM_ON_CC);
    rc |= expect_true("forced data does not arm backoff", retune_backoff_empty(&data_forced_st));

    // 8) A stale failed-voice backoff must not suppress an explicit data grant
    // on the same TDMA channel/slot.
    static dsd_opts mixed_opts;
    static dsd_state mixed_st;
    DSD_MEMSET(&mixed_opts, 0, sizeof mixed_opts);
    DSD_MEMSET(&mixed_st, 0, sizeof mixed_st);
    mixed_opts.p25_trunk = 1;
    mixed_opts.trunk_tune_group_calls = 1;
    mixed_opts.trunk_tune_data_calls = 1;
    mixed_opts.trunk_hangtime = 0.2f;
    mixed_opts.p25_grant_voice_to_s = 0.5;
    mixed_opts.p25_retune_backoff_s = 2.0;
    mixed_st.p25_cc_freq = 851000000;
    mixed_st.p25_chan_iden = id;
    mixed_st.p25_iden_tdma[id] = st.p25_iden_tdma[id];
    mixed_st.p25_chan_tdma_explicit[id] = 2;

    p25_sm_ctx_t mixed_ctx;
    p25_sm_init_ctx(&mixed_ctx, &mixed_opts, &mixed_st);
    g_last_tuned_vc = 0;
    g_tune_to_freq_calls = 0;
    g_return_to_cc_calls = 0;
    p25_sm_event(&mixed_ctx, &mixed_opts, &mixed_st, &ev_slot1);
    rc |= expect_true("mixed voice initial tune", mixed_st.p25_sm_tune_count == 1 && mixed_opts.p25_is_tuned == 1
                                                      && g_tune_to_freq_calls == 1 && mixed_ctx.vc_data_call == 0);

    mixed_ctx.t_tune_m = dsd_time_now_monotonic_s() - 1.0;
    mixed_ctx.t_voice_m = 0.0;
    p25_sm_tick_ctx(&mixed_ctx, &mixed_opts, &mixed_st);
    rc |= expect_true("mixed voice returned",
                      mixed_opts.p25_is_tuned == 0 && g_return_to_cc_calls == 1 && mixed_ctx.state == P25_SM_ON_CC);
    rc |= expect_true("mixed voice backoff armed", mixed_st.p25_retune_block_freq == g_last_tuned_vc
                                                       && mixed_st.p25_retune_block_slot == 1
                                                       && mixed_st.p25_retune_block_until > time(NULL));

    p25_sm_event(&mixed_ctx, &mixed_opts, &mixed_st, &data_ev_slot1);
    rc |= expect_true("data grant bypasses voice backoff",
                      g_tune_to_freq_calls == 2 && mixed_opts.p25_is_tuned == 1 && mixed_ctx.vc_data_call == 1);

    // 9) A same-carrier data grant must not hide a pending failed voice grant
    // on the other TDMA slot when the carrier returns to the CC.
    {
        static dsd_opts mixed_data_opts;
        static dsd_state mixed_data_st;
        DSD_MEMSET(&mixed_data_opts, 0, sizeof mixed_data_opts);
        DSD_MEMSET(&mixed_data_st, 0, sizeof mixed_data_st);
        mixed_data_opts.p25_trunk = 1;
        mixed_data_opts.trunk_tune_group_calls = 1;
        mixed_data_opts.trunk_tune_data_calls = 1;
        mixed_data_opts.trunk_hangtime = 0.2f;
        mixed_data_opts.p25_grant_voice_to_s = 0.5;
        mixed_data_opts.p25_retune_backoff_s = 2.0;
        mixed_data_st.p25_cc_freq = 851000000;
        mixed_data_st.p25_chan_iden = id;
        mixed_data_st.p25_iden_tdma[id] = st.p25_iden_tdma[id];
        mixed_data_st.p25_chan_tdma_explicit[id] = 2;

        p25_sm_ctx_t mixed_data_ctx;
        p25_sm_init_ctx(&mixed_data_ctx, &mixed_data_opts, &mixed_data_st);
        g_last_tuned_vc = 0;
        g_tune_to_freq_calls = 0;
        g_return_to_cc_calls = 0;
        p25_sm_event_t mixed_voice_slot0 = p25_sm_ev_group_grant(ch_slot0, 0, 3301, 4301, 0);
        p25_sm_event_t mixed_data_slot1 = p25_sm_ev_group_data_grant(ch_slot1, 0, 3302, 4302, P25_SM_SVC_UNKNOWN);

        p25_sm_event(&mixed_data_ctx, &mixed_data_opts, &mixed_data_st, &mixed_voice_slot0);
        rc |= expect_true("mixed data voice tune", g_tune_to_freq_calls == 1 && mixed_data_opts.p25_is_tuned == 1
                                                       && mixed_data_ctx.slots[0].grant_active
                                                       && mixed_data_ctx.vc_data_call == 0);
        p25_sm_event(&mixed_data_ctx, &mixed_data_opts, &mixed_data_st, &mixed_data_slot1);
        rc |= expect_true("mixed data same-carrier no retune", g_tune_to_freq_calls == 1
                                                                   && mixed_data_ctx.vc_data_call == 1
                                                                   && mixed_data_ctx.slots[1].data_call);

        double stale_grant_m = dsd_time_now_monotonic_s() - 1.0;
        mixed_data_ctx.t_tune_m = stale_grant_m;
        mixed_data_ctx.t_voice_m = 0.0;
        mixed_data_ctx.slots[0].last_grant_m = stale_grant_m;
        p25_sm_tick_ctx(&mixed_data_ctx, &mixed_data_opts, &mixed_data_st);
        rc |= expect_true("mixed data returned", mixed_data_opts.p25_is_tuned == 0 && g_return_to_cc_calls == 1
                                                     && mixed_data_ctx.state == P25_SM_ON_CC);
        rc |= expect_true("mixed data voice slot backoff",
                          retune_backoff_history_has_slot(&mixed_data_st, g_last_tuned_vc, 0));

        dsd_state_ext_free_all(&mixed_data_st);
    }

    // 10) When one slot ends while the other slot has a pending voice grant,
    // the pending grant stays on the grant-timeout path rather than inheriting
    // the ended slot's hangtime.
    {
        static dsd_opts pending_opts;
        static dsd_state pending_st;
        DSD_MEMSET(&pending_opts, 0, sizeof pending_opts);
        DSD_MEMSET(&pending_st, 0, sizeof pending_st);
        pending_opts.p25_trunk = 1;
        pending_opts.trunk_tune_group_calls = 1;
        pending_opts.trunk_hangtime = 0.2f;
        pending_opts.p25_grant_voice_to_s = 0.8;
        pending_opts.p25_retune_backoff_s = 2.0;
        pending_st.p25_cc_freq = 851000000;
        pending_st.p25_chan_iden = id;
        pending_st.p25_iden_tdma[id] = st.p25_iden_tdma[id];
        pending_st.p25_chan_tdma_explicit[id] = 2;

        p25_sm_ctx_t pending_ctx;
        p25_sm_init_ctx(&pending_ctx, &pending_opts, &pending_st);
        g_last_tuned_vc = 0;
        g_tune_to_freq_calls = 0;
        g_return_to_cc_calls = 0;
        p25_sm_event_t pending_active_slot0 = p25_sm_ev_group_grant(ch_slot0, 0, 3401, 4401, 0);
        p25_sm_event_t pending_voice_slot1 = p25_sm_ev_group_grant(ch_slot1, 0, 3402, 4402, 0);
        p25_sm_event_t ptt_slot0 = p25_sm_ev_ptt(0);
        p25_sm_event_t end_slot0 = p25_sm_ev_end(0);

        p25_sm_event(&pending_ctx, &pending_opts, &pending_st, &pending_active_slot0);
        rc |= expect_true("pending initial tune", g_tune_to_freq_calls == 1 && pending_opts.p25_is_tuned == 1);
        p25_sm_event(&pending_ctx, &pending_opts, &pending_st, &ptt_slot0);
        pending_st.p25_p2_audio_allowed[0] = 1;
        p25_sm_event(&pending_ctx, &pending_opts, &pending_st, &pending_voice_slot1);
        rc |= expect_true("pending same-carrier grant", g_tune_to_freq_calls == 1 && pending_ctx.slots[1].grant_active
                                                            && pending_ctx.slots[1].voice_active == 0);
        p25_sm_event(&pending_ctx, &pending_opts, &pending_st, &end_slot0);

        double now_m = dsd_time_now_monotonic_s();
        pending_ctx.t_voice_m = now_m - 1.0;
        pending_ctx.t_tune_m = now_m - 0.2;
        pending_ctx.slots[1].last_grant_m = pending_ctx.t_tune_m;
        p25_sm_tick_ctx(&pending_ctx, &pending_opts, &pending_st);
        rc |= expect_true("pending grant not released on ended-slot hangtime",
                          pending_opts.p25_is_tuned == 1 && g_return_to_cc_calls == 0
                              && pending_ctx.state == P25_SM_TUNED && pending_ctx.slots[1].grant_active);

        now_m = dsd_time_now_monotonic_s();
        pending_ctx.t_tune_m = now_m - 1.0;
        pending_ctx.slots[1].last_grant_m = pending_ctx.t_tune_m;
        p25_sm_tick_ctx(&pending_ctx, &pending_opts, &pending_st);
        rc |= expect_true("pending grant timeout returned", pending_opts.p25_is_tuned == 0 && g_return_to_cc_calls == 1
                                                                && pending_ctx.state == P25_SM_ON_CC);
        rc |= expect_true("pending grant timeout backoff",
                          retune_backoff_history_has_slot(&pending_st, g_last_tuned_vc, 1));

        dsd_state_ext_free_all(&pending_st);
    }

    // 11) A transient return-to-CC failure during grant timeout must not record
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

    // 12) Same-carrier preemption releases to the CC first. The replacement
    // grant must retune the VC instead of reusing a carrier that is no longer tuned.
    {
        static dsd_opts preempt_opts;
        static dsd_state preempt_st;
        DSD_MEMSET(&preempt_opts, 0, sizeof preempt_opts);
        DSD_MEMSET(&preempt_st, 0, sizeof preempt_st);
        preempt_opts.p25_trunk = 1;
        preempt_opts.trunk_tune_group_calls = 1;
        preempt_opts.trunk_hangtime = 0.2f;
        preempt_opts.p25_retune_backoff_s = 2.0;
        preempt_st.p25_cc_freq = 851000000;
        preempt_st.p25_chan_iden = id;
        preempt_st.p25_iden_tdma[id] = st.p25_iden_tdma[id];
        preempt_st.p25_chan_tdma_explicit[id] = 2;

        rc |= expect_true("seed preempt active", seed_exact(&preempt_st, 4001, "A", "PREEMPT-ACTIVE", 10, 0) == 0);
        rc |=
            expect_true("seed preempt candidate", seed_exact(&preempt_st, 4002, "A", "PREEMPT-CANDIDATE", 90, 1) == 0);
        (void)dsd_setenv("DSD_NEO_TG_PREEMPT_MIN_DWELL_MS", "0", 1);
        (void)dsd_setenv("DSD_NEO_TG_PREEMPT_COOLDOWN_MS", "0", 1);

        p25_sm_ctx_t preempt_ctx;
        p25_sm_init_ctx(&preempt_ctx, &preempt_opts, &preempt_st);
        g_last_tuned_vc = 0;
        g_tune_to_freq_calls = 0;
        g_return_to_cc_calls = 0;
        p25_sm_event_t preempt_active = p25_sm_ev_group_grant(ch_slot0, 0, 4001, 5001, 0);
        p25_sm_event_t preempt_candidate = p25_sm_ev_group_grant(ch_slot0, 0, 4002, 5002, 0);

        p25_sm_event(&preempt_ctx, &preempt_opts, &preempt_st, &preempt_active);
        rc |= expect_true("preempt active tuned", g_tune_to_freq_calls == 1 && preempt_opts.p25_is_tuned == 1
                                                      && preempt_ctx.state == P25_SM_TUNED);
        preempt_ctx.slots[0].voice_active = 1;
        preempt_ctx.slots[0].last_active_m = dsd_time_now_monotonic_s();
        preempt_ctx.t_voice_m = preempt_ctx.slots[0].last_active_m;
        preempt_st.p25_p2_audio_allowed[0] = 1;

        p25_sm_event(&preempt_ctx, &preempt_opts, &preempt_st, &preempt_candidate);
        rc |= expect_true("preempt returned to cc first", g_return_to_cc_calls == 1);
        rc |= expect_true("preempt retuned vc", g_tune_to_freq_calls == 2 && preempt_opts.p25_is_tuned == 1
                                                    && preempt_st.p25_sm_tune_count == 2);
        rc |= expect_true("preempt replacement active", preempt_ctx.state == P25_SM_TUNED && preempt_ctx.vc_tg == 4002
                                                            && preempt_ctx.slots[0].grant_active
                                                            && preempt_ctx.slots[0].target_id == 4002);

        static const dsd_trunk_tune_result preempt_release_failures[] = {
            DSD_TRUNK_TUNE_RESULT_DEFERRED,
            DSD_TRUNK_TUNE_RESULT_FAILED,
        };
        for (size_t i = 0; i < sizeof(preempt_release_failures) / sizeof(preempt_release_failures[0]); i++) {
            static dsd_opts failed_preempt_opts;
            static dsd_state failed_preempt_st;
            DSD_MEMSET(&failed_preempt_opts, 0, sizeof failed_preempt_opts);
            DSD_MEMSET(&failed_preempt_st, 0, sizeof failed_preempt_st);
            failed_preempt_opts.p25_trunk = 1;
            failed_preempt_opts.trunk_tune_group_calls = 1;
            failed_preempt_opts.trunk_hangtime = 0.2f;
            failed_preempt_opts.p25_retune_backoff_s = 2.0;
            failed_preempt_st.p25_cc_freq = 851000000;
            failed_preempt_st.p25_chan_iden = id;
            failed_preempt_st.p25_iden_tdma[id] = st.p25_iden_tdma[id];
            failed_preempt_st.p25_chan_tdma_explicit[id] = 2;

            rc |= expect_true("seed failed-preempt active",
                              seed_exact(&failed_preempt_st, 4101, "A", "PREEMPT-FAIL-ACTIVE", 10, 0) == 0);
            rc |= expect_true("seed failed-preempt candidate",
                              seed_exact(&failed_preempt_st, 4102, "A", "PREEMPT-FAIL-CAND", 90, 1) == 0);

            p25_sm_ctx_t failed_preempt_ctx;
            p25_sm_init_ctx(&failed_preempt_ctx, &failed_preempt_opts, &failed_preempt_st);
            g_last_tuned_vc = 0;
            g_tune_to_freq_calls = 0;
            g_return_to_cc_calls = 0;
            g_return_to_cc_result = preempt_release_failures[i];

            p25_sm_event_t failed_preempt_active = p25_sm_ev_group_grant(ch_slot0, 0, 4101, 5101, 0);
            p25_sm_event_t failed_preempt_candidate = p25_sm_ev_group_grant(ch_slot0, 0, 4102, 5102, 0);
            p25_sm_event(&failed_preempt_ctx, &failed_preempt_opts, &failed_preempt_st, &failed_preempt_active);
            rc |= expect_true("failed-preempt active tuned", g_tune_to_freq_calls == 1
                                                                 && failed_preempt_opts.p25_is_tuned == 1
                                                                 && failed_preempt_ctx.state == P25_SM_TUNED);
            failed_preempt_ctx.slots[0].voice_active = 1;
            failed_preempt_ctx.slots[0].last_active_m = dsd_time_now_monotonic_s();
            failed_preempt_ctx.t_voice_m = failed_preempt_ctx.slots[0].last_active_m;
            failed_preempt_st.p25_p2_audio_allowed[0] = 1;

            p25_sm_event(&failed_preempt_ctx, &failed_preempt_opts, &failed_preempt_st, &failed_preempt_candidate);
            rc |= expect_true("failed-preempt attempted release", g_return_to_cc_calls == 1);
            rc |= expect_true("failed-preempt did not reuse carrier",
                              g_tune_to_freq_calls == 1 && failed_preempt_st.p25_sm_tune_count == 1
                                  && failed_preempt_ctx.vc_tg == 4101 && failed_preempt_ctx.slots[0].target_id == 4101);
            rc |= expect_true("failed-preempt preserved active call",
                              failed_preempt_opts.p25_is_tuned == 1 && failed_preempt_ctx.state == P25_SM_TUNED
                                  && failed_preempt_ctx.slots[0].voice_active == 1);

            dsd_state_ext_free_all(&failed_preempt_st);
        }
        g_return_to_cc_result = DSD_TRUNK_TUNE_RESULT_OK;

        (void)dsd_unsetenv("DSD_NEO_TG_PREEMPT_MIN_DWELL_MS");
        (void)dsd_unsetenv("DSD_NEO_TG_PREEMPT_COOLDOWN_MS");
        dsd_state_ext_free_all(&preempt_st);
    }

    dsd_state_ext_free_all(&mixed_st);
    dsd_state_ext_free_all(&data_forced_st);
    dsd_state_ext_free_all(&data_st);
    dsd_state_ext_free_all(&forced_st);
    dsd_state_ext_free_all(&st);

    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
