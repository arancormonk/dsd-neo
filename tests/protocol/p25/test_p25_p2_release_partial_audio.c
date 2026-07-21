// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * P25 Phase 2: ensure partial SS18 audio is flushed on release.
 *
 * Short P25p2 calls can end before a full 18-frame superframe is available for
 * playSynthesizedVoiceSS18(), causing the buffered audio to be dropped when
 * returning to the control channel. Verify that p25_sm_release() triggers a
 * best-effort flush that clears the buffered short frames so short calls are
 * still audible.
 */

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/runtime/p25_optional_hooks.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

static int g_return_to_cc_called = 0;
static int g_finalizing_notice_called = 0;
static int g_nonfinalizing_notice_called = 0;
static int g_track_end_order = 0;
static char g_end_order[4];
static size_t g_end_order_len = 0U;

static void
record_end_order(char step) {
    if (g_track_end_order && g_end_order_len + 1U < sizeof(g_end_order)) {
        g_end_order[g_end_order_len++] = step;
        g_end_order[g_end_order_len] = '\0';
    }
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_event_sync_slot(dsd_opts* opts, dsd_state* state, uint8_t slot) {
    (void)opts;
    (void)state;
    (void)slot;
    record_end_order('E');
}

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_event_emit_call_notice(dsd_opts* opts, dsd_state* state, uint8_t slot, const dsd_call_snapshot* call,
                           const char* detail) {
    (void)opts;
    (void)state;
    (void)slot;
    (void)call;
    (void)detail;
    g_finalizing_notice_called++;
    return 0;
}

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_event_emit_call_notice_nonfinalizing(dsd_opts* opts, dsd_state* state, uint8_t slot, const dsd_call_snapshot* call,
                                         const char* detail) {
    (void)opts;
    (void)state;
    (void)slot;
    (void)call;
    (void)detail;
    g_nonfinalizing_notice_called++;
    return 0;
}

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
    (void)request_id;
    (void)opts;
    (void)state;
    g_return_to_cc_called++;
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

static int g_p25p2_flush_called = 0;
static int g_p25p2_slot_flush_called = 0;

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_p25p2_flush_partial_audio(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    g_p25p2_flush_called++;
    if (!state) {
        return;
    }
    state->voice_counter[0] = 0;
    state->voice_counter[1] = 0;
    DSD_MEMSET(state->s_l4, 0, sizeof(state->s_l4));
    DSD_MEMSET(state->s_r4, 0, sizeof(state->s_r4));
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_p25p2_flush_partial_audio_slot(dsd_opts* opts, dsd_state* state, int slot) {
    (void)opts;
    g_p25p2_slot_flush_called++;
    record_end_order('F');
    if (state && slot >= 0 && slot < 2) {
        state->voice_counter[slot] = 0;
    }
}

static void
install_p25_optional_hooks(void) {
    dsd_p25_optional_hooks hooks = {0};
    hooks.p25p2_flush_partial_audio = dsd_p25p2_flush_partial_audio;
    dsd_p25_optional_hooks_set(hooks);
}

static int
expect_eq_int(const char* tag, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %d want %d\n", tag, got, want);
        return 1;
    }
    return 0;
}

int
main(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state st;
    static Event_History_I event_history[2];
    install_trunk_tuning_hooks();
    install_p25_optional_hooks();
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&st, 0, sizeof st);

    opts.trunk_enable = 1;
    opts.trunk_tune_group_calls = 1;
    opts.floating_point = 0;
    opts.pulse_digi_rate_out = 8000;
    opts.slot1_on = 1;
    opts.slot2_on = 1;

    // Establish a TDMA VC context so the SM release path executes P25p2 logic.
    st.p25_cc_freq = 851000000;
    int id = 2;
    st.p25_chan_iden = id;
    // Populate new dual-array
    st.p25_iden_tdma[id].base_freq = 851000000 / 5;
    st.p25_iden_tdma[id].chan_type = 3;
    st.p25_iden_tdma[id].chan_spac = 100;
    st.p25_iden_tdma[id].trust = 2;
    st.p25_iden_tdma[id].populated = 1;
    st.p25_chan_tdma_explicit[id] = 2; // TDMA known
    st.event_history_s = event_history;

    p25_sm_init_ctx(p25_sm_get_ctx(), &opts, &st);
    int ch_tdma = (id << 12) | 0x0000;

    // A synthetic pre-tune lockout is a notice, not the end of a call.
    g_finalizing_notice_called = 0;
    g_nonfinalizing_notice_called = 0;
    p25_emit_enc_lockout_once_typed(&opts, &st, 0U, 4321, 0x40, 1);
    rc |= expect_eq_int("pre-tune lockout uses nonfinalizing notice", g_nonfinalizing_notice_called, 1);
    rc |= expect_eq_int("pre-tune lockout avoids finalizing notice", g_finalizing_notice_called, 0);

    p25_sm_event(p25_sm_get_ctx(), &opts, &st,
                 &(p25_sm_event_t){.type = P25_SM_EV_GRANT,
                                   .slot = -1,
                                   .channel = ch_tdma,
                                   .tg = 1234,
                                   .src = 5678,
                                   .svc_bits = 0,
                                   .is_group = 1});
    rc |= expect_eq_int("PTT accepted", p25_sm_emit_ptt_call(&opts, &st, 0, 1234, 0, 5678, 1, 0), 1);

    g_p25p2_slot_flush_called = 0;
    g_end_order_len = 0U;
    g_end_order[0] = '\0';
    g_track_end_order = 1;
    rc |= expect_eq_int("explicit end accepted", p25_sm_emit_end_call_at(&opts, &st, 0, 1234, 5678, 0.0), 1);
    g_track_end_order = 0;
    rc |= expect_eq_int("slot flush called on voice end", g_p25p2_slot_flush_called, 1);
    rc |= expect_eq_int("voice end sync follows slot flush", strcmp(g_end_order, "FE"), 0);

    // Simulate a short call that buffered some audio but ended before the
    // normal SS18 playback cadence. Also simulate gates already cleared.
    st.s_l4[0][0] = 123;
    st.s_r4[0][0] = -456;
    st.voice_counter[0] = 1;
    st.voice_counter[1] = 1;
    st.p25_p2_audio_allowed[0] = 0;
    st.p25_p2_audio_allowed[1] = 0;

    g_return_to_cc_called = 0;
    g_p25p2_flush_called = 0;
    p25_sm_release(p25_sm_get_ctx(), &opts, &st, "explicit-release");
    rc |= expect_eq_int("return_to_cc called", g_return_to_cc_called, 1);
    rc |= expect_eq_int("flush called", g_p25p2_flush_called, 1);

    // Flush should clear buffered samples and reset counters.
    rc |= expect_eq_int("s_l4 cleared", st.s_l4[0][0], 0);
    rc |= expect_eq_int("s_r4 cleared", st.s_r4[0][0], 0);
    rc |= expect_eq_int("voice_counter[0] reset", st.voice_counter[0], 0);
    rc |= expect_eq_int("voice_counter[1] reset", st.voice_counter[1], 0);

    dsd_p25_optional_hooks_set((dsd_p25_optional_hooks){0});
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
    dsd_state_ext_free_all(&st);
    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
