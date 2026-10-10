// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * P25 Phase 2: what the voice playout still holds plays when the receiver leaves the channel.
 *
 * A short call can end with frames still queued (a slot runs up to two frames ahead of its companion). Verify that
 * p25_sm_release() and a capped departure drain the playout through the real flush, so the tail is heard, while a
 * voice end only closes the slot's stream and leaves its frames to play in time order (issue #651).
 */

#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/p25p2_playout.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/p25_optional_hooks.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <dsd-neo/runtime/udp_audio_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dsd-neo/core/enc_lockout.h"
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
closeMbeOutFile(dsd_opts* opts, dsd_state* state) {
    (void)state;
    opts->mbe_out_f = NULL;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
closeMbeOutFileR(dsd_opts* opts, dsd_state* state) {
    (void)state;
    opts->mbe_out_fR = NULL;
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

/* The return the engine installs: it releases the tuned call state, ending the canonical calls, before the release's
   playout drain runs. */
static dsd_trunk_tune_result
return_request_ending_calls(dsd_opts* opts, dsd_state* state, uint64_t request_id) {
    (void)opts;
    (void)request_id;
    g_return_to_cc_called++;
    (void)dsd_call_state_end(state, 0U, dsd_decode_now_mono_s());
    (void)dsd_call_state_end(state, 1U, dsd_decode_now_mono_s());
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
static int g_blocks = 0;
static int g_first_left = 0;
static int g_first_right = 0;

// The release hook, counted and ordered, around the real flush.
static void
counting_flush(dsd_opts* opts, dsd_state* state) {
    g_p25p2_flush_called++;
    record_end_order('F');
    dsd_p25p2_flush_partial_audio(opts, state);
}

static void
install_p25_optional_hooks(void) {
    dsd_p25_optional_hooks hooks = {0};
    hooks.p25p2_flush_partial_audio = counting_flush;
    dsd_p25_optional_hooks_set(hooks);
}

static void
capture_blast(const dsd_opts* opts, dsd_state* state, size_t bytes, const void* data) {
    (void)opts;
    (void)state;
    if (g_blocks == 0 && data && bytes >= 2U * sizeof(short)) {
        g_first_left = ((const short*)data)[0];
        g_first_right = ((const short*)data)[1];
    }
    g_blocks++;
}

static void
reset_capture(void) {
    g_blocks = 0;
    g_first_left = 0;
    g_first_right = 0;
}

// One admitted frame per slot, its samples all @p left (slot 1) and @p right (slot 2).
static void
queue_frames(dsd_opts* opts, dsd_state* state, short left, short right, uint32_t serial) {
    state->p25_crypto_state[0] = DSD_P25_CRYPTO_CLEAR;
    state->p25_crypto_state[1] = DSD_P25_CRYPTO_CLEAR;
    for (int i = 0; i < 160; i++) {
        state->s_l[i] = left;
        state->s_r[i] = right;
    }
    state->mbe_short_silenced[0] = 0U;
    state->mbe_short_silenced[1] = 0U;
    dsd_p25p2_playout_stage(opts, state, 0, 0, serial, NULL);
    dsd_p25p2_playout_stage(opts, state, 1, 0, serial, NULL);
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
    opts.audio_out = 1;
    opts.audio_out_type = 8;
    dsd_udp_audio_hooks_set((dsd_udp_audio_hooks){.blast = capture_blast});
    dsd_p25p2_playout_reset(&st, -1);

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
    // The SM defers TDMA grants until the descrambler seed (WACN/SYSID/NAC)
    // has been decoded from the control channel.
    st.p2_wacn = 0xBEE00;
    st.p2_sysid = 0x1A2;
    st.p2_cc = 0x293;
    st.event_history_s = event_history;

    p25_sm_init_ctx(p25_sm_get_ctx(), &opts, &st);
    int ch_tdma = (id << 12) | 0x0000;

    // A synthetic pre-tune lockout is a notice, not the end of a call.
    g_finalizing_notice_called = 0;
    g_nonfinalizing_notice_called = 0;
    p25_emit_enc_lockout_once_typed(&opts, &st, 0U, 4321, 0x40, 1, DSD_ENC_LOCKOUT_ALGID_UNKNOWN, 0);
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

    // A voice end closes the slot's stream; its queued frame stays to play in time order with the companion.
    queue_frames(&opts, &st, 77, -77, 1U);
    reset_capture();
    g_end_order_len = 0U;
    g_end_order[0] = '\0';
    g_track_end_order = 1;
    rc |= expect_eq_int("explicit end accepted", p25_sm_emit_end_call_at(&opts, &st, 0, 1234, 5678, 0.0), 1);
    g_track_end_order = 0;
    rc |= expect_eq_int("voice end closes the slot's stream", st.p25p2_playout.slot[0].open, 0);
    rc |= expect_eq_int("voice end keeps the slot's queued frame", dsd_p25p2_playout_level(&st, 0), 1);
    rc |= expect_eq_int("voice end plays nothing out of turn", g_blocks, 0);
    rc |= expect_eq_int("voice end syncs the call event", strcmp(g_end_order, "E"), 0);
    dsd_p25p2_playout_reset(&st, -1);

    // A short call ends with frames still queued, its gates already cleared.
    queue_frames(&opts, &st, 123, -456, 2U);
    st.p25_p2_audio_allowed[0] = 0;
    st.p25_p2_audio_allowed[1] = 0;

    g_return_to_cc_called = 0;
    g_p25p2_flush_called = 0;
    reset_capture();
    p25_sm_release(p25_sm_get_ctx(), &opts, &st, "explicit-release");
    rc |= expect_eq_int("return_to_cc called", g_return_to_cc_called, 1);
    rc |= expect_eq_int("flush called", g_p25p2_flush_called, 1);

    // The flush plays the queued tail, both slots together, and leaves the playout empty.
    rc |= expect_eq_int("release plays the queued tail", g_blocks, 1);
    rc |= expect_eq_int("release tail left", g_first_left, 123);
    rc |= expect_eq_int("release tail right", g_first_right, -456);
    rc |= expect_eq_int("release empties slot 1", dsd_p25p2_playout_level(&st, 0), 0);
    rc |= expect_eq_int("release empties slot 2", dsd_p25p2_playout_level(&st, 1), 0);

    // A capped departure flushes before ending the call, without tuning back to the CC.
    st.p25_last_cc_msg_time_m = dsd_decode_now_mono_s();
    p25_sm_event(p25_sm_get_ctx(), &opts, &st,
                 &(p25_sm_event_t){.type = P25_SM_EV_GRANT,
                                   .slot = -1,
                                   .channel = ch_tdma + 2,
                                   .tg = 4321,
                                   .src = 8765,
                                   .svc_bits = 0,
                                   .is_group = 1});
    rc |= expect_eq_int("capped PTT accepted", p25_sm_emit_ptt_call(&opts, &st, 0, 4321, 0, 8765, 1, 0), 1);
    queue_frames(&opts, &st, 321, -654, 3U);
    reset_capture();
    g_p25p2_flush_called = 0;
    g_return_to_cc_called = 0;
    g_end_order_len = 0U;
    g_end_order[0] = '\0';
    g_track_end_order = 1;
    p25_sm_abandon_carrier(p25_sm_get_ctx(), &opts, &st, "scan-visit-limit");
    g_track_end_order = 0;
    rc |= expect_eq_int("cap flushes partial audio", g_p25p2_flush_called, 1);
    rc |= expect_eq_int("cap flush precedes call end", strcmp(g_end_order, "FE"), 0);
    rc |= expect_eq_int("cap does not return to CC", g_return_to_cc_called, 0);
    rc |= expect_eq_int("cap plays the queued tail", g_blocks, 1);
    rc |= expect_eq_int("cap tail left", g_first_left, 321);
    rc |= expect_eq_int("cap tail right", g_first_right, -654);
    rc |= expect_eq_int("cap empties slot 1", dsd_p25p2_playout_level(&st, 0), 0);
    rc |= expect_eq_int("cap empties slot 2", dsd_p25p2_playout_level(&st, 1), 0);
    p25_sm_abandon_carrier(p25_sm_get_ctx(), &opts, &st, "idle-visit-limit");
    rc |= expect_eq_int("idle cap does not flush again", g_p25p2_flush_called, 1);

    // A call blocked after its frames were queued, then released through a return that ends the calls before the
    // drain: its tail stays silent beside the companion's (issue #651).
    st.p25_last_cc_msg_time_m = dsd_decode_now_mono_s();
    p25_sm_event(p25_sm_get_ctx(), &opts, &st,
                 &(p25_sm_event_t){.type = P25_SM_EV_GRANT,
                                   .slot = -1,
                                   .channel = ch_tdma + 4,
                                   .tg = 5555,
                                   .src = 6666,
                                   .svc_bits = 0,
                                   .is_group = 1});
    rc |= expect_eq_int("blocked release PTT accepted", p25_sm_emit_ptt_call(&opts, &st, 0, 5555, 0, 6666, 1, 0), 1);
    queue_frames(&opts, &st, 555, -555, 4U);
    rc |= expect_eq_int("blocked release lockout applied", dsd_tg_policy_set_mode(&st, 5555, 5555, "B"), 0);
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){.tune_to_freq_request = test_tune_request,
                                                        .tune_to_cc_request = test_tune_request,
                                                        .return_to_cc_request = return_request_ending_calls});
    reset_capture();
    g_return_to_cc_called = 0;
    p25_sm_release(p25_sm_get_ctx(), &opts, &st, "blocked-release");
    rc |= expect_eq_int("blocked release returned to CC", g_return_to_cc_called, 1);
    rc |= expect_eq_int("blocked release plays the companion alone", g_blocks, 1);
    rc |= expect_eq_int("blocked release keeps the blocked tail silent (left)", g_first_left, -555);
    rc |= expect_eq_int("blocked release keeps the blocked tail silent (right)", g_first_right, -555);

    dsd_p25_optional_hooks_set((dsd_p25_optional_hooks){0});
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
    dsd_udp_audio_hooks_set((dsd_udp_audio_hooks){0});
    dsd_state_ext_free_all(&st);
    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
