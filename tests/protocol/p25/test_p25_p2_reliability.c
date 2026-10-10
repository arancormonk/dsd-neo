// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Unit tests for P25P2 frame reset and soft-decision recovery paths. */

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/p25p2_playout.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/time_format.h>
#include <dsd-neo/core/vocoder.h>
#include <dsd-neo/platform/posix_compat.h>
#include <dsd-neo/protocol/p25/p25.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/protocol/p25/p25p2_frame.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <dsd-neo/runtime/udp_audio_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "p25p2_frame_internal.h"

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

/* What the tuner answers a return to the control channel. */
static dsd_trunk_tune_result g_return_result = DSD_TRUNK_TUNE_RESULT_OK;

static int g_return_calls = 0;

static dsd_trunk_tune_result
test_return_request(dsd_opts* opts, dsd_state* state, uint64_t request_id) {
    (void)opts;
    (void)state;
    (void)request_id;
    g_return_calls++;
    return g_return_result;
}

static void
install_trunk_tuning_hooks(void) {
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){
        .tune_to_freq_request = test_tune_request,
        .tune_to_cc_request = test_tune_request,
        .return_to_cc_request = test_return_request,
    });
}

static void
seed_p25p2_call(dsd_state* state, uint8_t slot, uint64_t target, uint64_t source, uint16_t service_options,
                uint8_t emergency, uint8_t priority) {
    const dsd_call_observation observation = {
        .protocol = DSD_SYNC_P25P2_POS,
        .slot = slot,
        .kind = DSD_CALL_KIND_GROUP_VOICE,
        .ota_target_id = target,
        .policy_target_id = target,
        .ota_source_id = source,
        .service_options = service_options,
        .emergency = emergency,
        .priority = priority,
        .has_service_metadata = 1U,
    };
    if (dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN) <= 0) {
        DSD_FPRINTF(stderr, "FAIL: could not seed canonical P25 Phase 2 call\n");
        abort();
    }
}

static int
expect_call_state(const char* tag, const dsd_state* state, uint8_t slot, dsd_call_phase phase, uint64_t target,
                  uint64_t source, uint8_t emergency, uint8_t priority) {
    dsd_call_snapshot call;
    if (dsd_call_state_get(state, slot, &call) <= 0 || call.phase != phase || call.kind != DSD_CALL_KIND_GROUP_VOICE
        || call.ota_target_id != target || call.policy_target_id != target || call.ota_source_id != source
        || call.emergency != emergency || call.priority != priority) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", tag);
        return 1;
    }
    return 0;
}

/* External declarations matching p25p2_frame.c */
extern int p2bit[4320];
extern int16_t p2llr[1400];
extern int16_t p2xllr[1400];
extern int ess_a[2][168];
extern int16_t ess_a_llr[2][168];

static int g_ess_hard_rc = 0;
static int g_ess_soft_min_success = -1;
static int g_ess_soft_success_rc = 0;
static int g_ess_soft_calls = 0;
static int g_ess_soft_last_n = 0;
static int g_ess_soft_mutate_algid = -1;
static int g_lfsrp_calls = 0;
static int g_lfsr128_calls = 0;
static int g_lfsr128_last_slot = -1;
static int g_facch_min_success = 0;
static int g_facch_success_rc = 0;
static int g_facch_calls = 0;
static int g_facch_last_erasures = 0;
static int g_sacch_min_success = 0;
static int g_sacch_success_rc = 0;
static int g_sacch_calls = 0;
static int g_sacch_last_erasures = 0;
static int g_facch_mac_calls = 0;
static int g_facch_mac_last_opcode = -1;
static int g_sacch_mac_calls = 0;
static int g_sacch_mac_last_opcode = -1;
static int g_isch_lookup_result = -1;
static uint8_t g_isch_last_reliab[40] = {0};
/* Blocks the Phase 2 playout emitted, and the slot-0 crypto tuple and first sample when the first one played. */
static int g_out_calls = 0;
static int g_out_pending_at_call = -1;
static int g_out_keyid_at_call = -1;
static int g_out_first_left = 0;

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
rtl_stream_p25p2_err_update(int slot, int a, int b, int c, int d, int e) {
    (void)slot;
    (void)a;
    (void)b;
    (void)c;
    (void)d;
    (void)e;
}

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_format_local_datetime(time_t timestamp, dsd_local_datetime_format format, char* out, size_t out_size) {
    (void)timestamp;
    (void)format;
    if (!out || out_size == 0) {
        return 0;
    }
    DSD_SNPRINTF(out, out_size, "%s", "00:00:00");
    return 1;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
rotate_symbol_out_file(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
openMbeOutFile(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
openMbeOutFileR(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
closeMbeOutFile(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
closeMbeOutFileR(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

static void
capture_output(const dsd_opts* opts, dsd_state* state, size_t bytes, const void* data) {
    (void)opts;
    if (g_out_calls == 0) {
        g_out_pending_at_call = state ? state->p25_p2_rekey[0].pending : -1;
        g_out_keyid_at_call = state ? state->payload_keyid : -1;
        g_out_first_left = (data && bytes >= sizeof(short)) ? ((const short*)data)[0] : 0;
    }
    g_out_calls++;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
watchdog_event_history(dsd_opts* opts, dsd_state* state, uint8_t slot) {
    (void)opts;
    (void)state;
    (void)slot;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
watchdog_event_current(dsd_opts* opts, dsd_state* state, uint8_t slot) {
    (void)opts;
    (void)state;
    (void)slot;
}

/* Set by the MAC PDU stubs to end the slots' calls, as a MAC_END_PTT does; and whether a render of a slot found its
   call still active with the seed proven, which is what lets the call's row keep the NAC (issue #575). */
static int g_mac_ends_calls = 0;
static int g_render_saw_proven_active_call = 0;

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_event_sync_slot(dsd_opts* opts, dsd_state* state, uint8_t slot) {
    (void)opts;
    dsd_call_snapshot call;
    if (state != NULL && state->p2_cc_verified != 0U && dsd_call_state_get(state, slot, &call) > 0
        && call.phase == DSD_CALL_PHASE_ACTIVE) {
        g_render_saw_proven_active_call = 1;
    }
}

static void
mac_stub_end_calls(dsd_state* state) {
    if (!g_mac_ends_calls || state == NULL) {
        return;
    }
    for (uint8_t slot = 0; slot < 2; slot++) {
        (void)dsd_call_state_end(state, slot, 0.0);
    }
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
    return 0;
}

void
processMbeFrame(dsd_opts* opts, dsd_state* state, char imbe_fr[8][23], char ambe_fr[4][24], char imbe7100_fr[7][24]) {
    (void)opts;
    (void)state;
    (void)imbe_fr;
    (void)ambe_fr;
    (void)imbe7100_fr;
}

void
processMbeFrameSoft(dsd_opts* opts, dsd_state* state, dsd_vocoder_soft_bit imbe_fr[8][23],
                    dsd_vocoder_soft_bit ambe_fr[4][24], dsd_vocoder_soft_bit imbe7100_fr[7][24]) {
    (void)opts;
    (void)state;
    (void)imbe_fr;
    (void)ambe_fr;
    (void)imbe7100_fr;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_mbe_log_ambe_soft_frame(dsd_opts* opts, dsd_state* state, dsd_vocoder_soft_bit ambe_fr[4][24]) {
    (void)opts;
    (void)state;
    (void)ambe_fr;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
LFSRP(dsd_state* state) {
    (void)state;
    g_lfsrp_calls++;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
LFSR128(dsd_state* state) {
    (void)state;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
p25_lfsr128_slot(dsd_state* state, int slot) {
    (void)state;
    g_lfsr128_calls++;
    g_lfsr128_last_slot = slot;
}

/* Set to 0 while a fed window is read: the window's first ISCH then decodes as channel 1's first I-ISCH, which proves
   the window's position in the superframe (issue #651: a window whose position nothing proves dispatches no
   slot-attributed burst). -1 otherwise. */
static int g_isch_feed_calls = -1;

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
isch_lookup(uint64_t isch) {
    (void)isch;
    if (g_isch_feed_calls >= 0) {
        return (g_isch_feed_calls++ == 0) ? ((1 << 5) | (0 << 3)) : -1;
    }
    return g_isch_lookup_result;
}

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
isch_lookup_soft(uint64_t isch, const uint8_t reliab40[40]) {
    if (reliab40) {
        DSD_MEMCPY(g_isch_last_reliab, reliab40, sizeof(g_isch_last_reliab));
    }
    return isch_lookup(isch);
}

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
ez_rs28_facch(int* payload, int* parity, const int* erasures, int n_erasures) {
    (void)payload;
    (void)parity;
    (void)erasures;
    g_facch_calls++;
    g_facch_last_erasures = n_erasures;
    return n_erasures >= g_facch_min_success ? g_facch_success_rc : -1;
}

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
ez_rs28_sacch(int* payload, int* parity, const int* erasures, int n_erasures) {
    (void)payload;
    (void)parity;
    (void)erasures;
    g_sacch_calls++;
    g_sacch_last_erasures = n_erasures;
    return n_erasures >= g_sacch_min_success ? g_sacch_success_rc : -1;
}

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
ez_rs28_ess(int* payload, int* parity, const int* erasures, int n_erasures) {
    (void)parity;
    (void)erasures;
    if (n_erasures == 0) {
        return g_ess_hard_rc;
    }
    g_ess_soft_calls++;
    g_ess_soft_last_n = n_erasures;
    if (g_ess_soft_min_success >= 0 && n_erasures >= g_ess_soft_min_success) {
        if (g_ess_soft_mutate_algid >= 0) {
            for (int i = 0; i < 8; i++) {
                payload[i] = (g_ess_soft_mutate_algid >> (7 - i)) & 1;
            }
        }
        return g_ess_soft_success_rc;
    }
    return -1;
}

/* MAC PDU handlers. A hook set on the SACCH one runs as its MAC PDU does: a grant accepted inside it, for one. The
   FACCH one sees the state its burst dispatches in. */
static void (*g_sacch_mac_hook)(dsd_opts* opts, dsd_state* state) = NULL;
static void (*g_facch_mac_hook)(dsd_opts* opts, dsd_state* state) = NULL;

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
process_SACCH_MAC_PDU(dsd_opts* opts, dsd_state* state, int* bits) {
    if (g_sacch_mac_hook) {
        g_sacch_mac_hook(opts, state);
    }
    mac_stub_end_calls(state);
    g_sacch_mac_calls++;
    g_sacch_mac_last_opcode = (bits[0] << 2) | (bits[1] << 1) | bits[2];
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
process_FACCH_MAC_PDU(dsd_opts* opts, dsd_state* state, int* bits) {
    if (g_facch_mac_hook) {
        g_facch_mac_hook(opts, state);
    }
    mac_stub_end_calls(state);
    g_facch_mac_calls++;
    g_facch_mac_last_opcode = (bits[0] << 2) | (bits[1] << 1) | bits[2];
}

/* Dibit acquisition. While a feed is set, the reads return its dibits in turn, as processP2() collects a superframe;
   the read at g_dibit_feed_boundary_at adopts a retune its capture recorded, where the carrier boundary moves the
   carrier count (dsd_engine_carrier_boundary(), issue #575). */
static uint8_t g_dibit_feed[700];
static int g_dibit_feed_active = 0;
static int g_dibit_feed_pos = 0;
static int g_dibit_feed_boundary_at = -1;

int
getDibitSoft(dsd_opts* opts, dsd_state* state, dsd_dibit_soft_t* out_soft) {
    (void)opts;
    int dibit = 0;
    if (g_dibit_feed_active && g_dibit_feed_pos < (int)sizeof g_dibit_feed) {
        if (g_dibit_feed_pos == g_dibit_feed_boundary_at && state) {
            state->carrier_seq++;
        }
        dibit = g_dibit_feed[g_dibit_feed_pos++];
        if (state) {
            state->symbolcnt++; /* as the symbol reader counts every dibit */
        }
    }
    if (out_soft) {
        out_soft->reliability = 128;
        out_soft->llr[0] = -128;
        out_soft->llr[1] = -128;
    }
    return dibit;
}

static void
set_p25p2_threshold(int threshold) {
    char value[16];
    DSD_SNPRINTF(value, sizeof(value), "%d", threshold);
    dsd_setenv("DSD_NEO_P25P2_SOFT_ERASURE_THRESHOLD", value, 1);
    dsd_neo_config_init();
}

/*
 * VC grace is resolved from the runtime config only. `seconds` must sit inside
 * the config's own accepted range (0..10 s) or the knob reads as unset and the
 * decoder falls back to its 0.75 s default, which would make a test silently
 * measure the default instead of the value it asked for. Pass NULL to clear it
 * again so the setting cannot leak into a later case.
 */
static void
set_p25_vc_grace(const char* seconds) {
    if (seconds) {
        dsd_setenv("DSD_NEO_P25_VC_GRACE", seconds, 1);
    } else {
        (void)dsd_unsetenv("DSD_NEO_P25_VC_GRACE");
    }
    dsd_neo_config_init();
}

static void
reset_ess_stubs(void) {
    g_ess_hard_rc = 0;
    g_ess_soft_min_success = -1;
    g_ess_soft_success_rc = 0;
    g_ess_soft_calls = 0;
    g_ess_soft_last_n = 0;
    g_ess_soft_mutate_algid = -1;
    g_lfsrp_calls = 0;
    g_lfsr128_calls = 0;
    g_lfsr128_last_slot = -1;
}

static void
reset_xcch_stubs(void) {
    g_facch_min_success = 0;
    g_facch_success_rc = 0;
    g_facch_calls = 0;
    g_facch_last_erasures = 0;
    g_sacch_min_success = 0;
    g_sacch_success_rc = 0;
    g_sacch_calls = 0;
    g_sacch_last_erasures = 0;
    g_facch_mac_calls = 0;
    g_facch_mac_last_opcode = -1;
    g_sacch_mac_calls = 0;
    g_sacch_mac_last_opcode = -1;
    g_isch_lookup_result = -1;
    DSD_MEMSET(g_isch_last_reliab, 0, sizeof(g_isch_last_reliab));
    g_sacch_mac_hook = NULL;
    g_facch_mac_hook = NULL;
}

static void
reset_playback_stub(void) {
    g_out_calls = 0;
    g_out_pending_at_call = -1;
    g_out_keyid_at_call = -1;
    g_out_first_left = 0;
}

/* The int16 output the playout writes, captured through the UDP blast hook. */
static void
enable_captured_output(dsd_opts* opts) {
    opts->floating_point = 0;
    opts->pulse_digi_rate_out = 8000;
    opts->audio_out = 1;
    opts->audio_out_type = 8;
    opts->slot1_on = 1;
    opts->slot2_on = 1;
    dsd_udp_audio_hooks_set((dsd_udp_audio_hooks){.blast = capture_output});
}

static void
set_p2bit(int index, int bit) {
    p2bit[index] = bit ? 1 : 0;
}

static void
seed_xcch_opcode(int timeslot_index, int opcode) {
    int base = timeslot_index * 360;
    set_p2bit(base + 2, (opcode >> 2) & 1);
    set_p2bit(base + 3, (opcode >> 1) & 1);
    set_p2bit(base + 4, opcode & 1);
}

static void
seed_duid_bits(int timeslot_index, uint8_t duid) {
    static const int duid_offsets[8] = {0, 1, 74, 75, 244, 245, 318, 319};
    int base = timeslot_index * 360;
    for (int i = 0; i < 8; i++) {
        int abs_bit = base + duid_offsets[i];
        set_p2bit(abs_bit, (duid >> (7 - i)) & 1U);
        p2llr[abs_bit] = 200;
    }
}

static int
expect_int(const char* tag, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %d want %d\n", tag, got, want);
        return 1;
    }
    return 0;
}

static void
prepare_ess_soft_inputs(dsd_state* state) {
    p25_p2_frame_reset();
    DSD_MEMSET(state, 0, sizeof(*state));
    state->currentslot = 0;
    state->dmrburstL = 20;
    for (int i = 0; i < 96; i++) {
        state->ess_b[0][i] = 0;
        state->ess_b_llr[0][i] = 1;
    }
    for (int i = 0; i < 168; i++) {
        ess_a[0][i] = 0;
        ess_a_llr[0][i] = 1;
    }
}

static void
set_ess_payload_bits(dsd_state* state, int slot, int algid, int keyid, uint64_t mi) {
    uint64_t essb_hex1 = ((uint64_t)(algid & 0xFF) << 24) | ((uint64_t)(keyid & 0xFFFF) << 8) | ((mi >> 56) & 0xFFU);
    uint64_t essb_hex2 = (mi & 0x00FFFFFFFFFFFFFFULL) << 8;

    for (int i = 0; i < 32; i++) {
        state->ess_b[slot][i] = (int)((essb_hex1 >> (31 - i)) & 1U);
    }
    for (int i = 0; i < 64; i++) {
        state->ess_b[slot][32 + i] = (int)((essb_hex2 >> (63 - i)) & 1U);
    }
}

static int
test_ess_soft_accepts_deep_erasure(void) {
    printf("Test 12: ESS accepts deep soft erasure success... ");
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    prepare_ess_soft_inputs(&state);
    reset_ess_stubs();
    set_p25p2_threshold(64);

    g_ess_hard_rc = -1;
    g_ess_soft_min_success = 20;
    g_ess_soft_success_rc = 20;
    g_ess_soft_mutate_algid = 0x80;

    p25p2_process_ess(&opts, &state, 0);

    if (state.p25_p2_rs_ess_ok == 1 && state.p25_p2_rs_ess_err == 0 && state.p25_p2_rs_ess_corr == 20
        && state.p25_p2_soft_ess_ok == 1 && state.p25_p2_soft_ess_max_depth == 20 && state.payload_algid == 0x80
        && g_ess_soft_last_n == 20) {
        printf("PASS\n");
        return 0;
    }
    printf("FAIL (ok=%u err=%u corr=%u soft=%u depth=%u alg=0x%02X last_n=%d calls=%d)\n", state.p25_p2_rs_ess_ok,
           state.p25_p2_rs_ess_err, state.p25_p2_rs_ess_corr, state.p25_p2_soft_ess_ok, state.p25_p2_soft_ess_max_depth,
           state.payload_algid, g_ess_soft_last_n, g_ess_soft_calls);
    return 1;
}

static int
test_ess_soft_failure_counts_once(void) {
    printf("Test 13: ESS hard/soft failure counts once... ");
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    prepare_ess_soft_inputs(&state);
    reset_ess_stubs();
    set_p25p2_threshold(64);

    g_ess_hard_rc = -1;
    g_ess_soft_min_success = -1;

    p25p2_process_ess(&opts, &state, 0);

    if (state.p25_p2_rs_ess_ok == 0 && state.p25_p2_rs_ess_err == 1 && state.p25_p2_soft_ess_ok == 0) {
        printf("PASS\n");
        return 0;
    }
    printf("FAIL (ok=%u err=%u soft=%u calls=%d)\n", state.p25_p2_rs_ess_ok, state.p25_p2_rs_ess_err,
           state.p25_p2_soft_ess_ok, g_ess_soft_calls);
    return 1;
}

static int
test_ess_des_manual_key_preserves_audio_gate(void) {
    printf("Test 14: ESS DES manual key preserves audio gate... ");
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    prepare_ess_soft_inputs(&state);
    reset_ess_stubs();
    set_p25p2_threshold(64);

    opts.trunk_enable = 1;
    opts.trunk_is_tuned = 1;
    opts.trunk_tune_enc_calls = 0;
    seed_p25p2_call(&state, 0U, 1234U, 4321U, 0x40U, 0U, 0U);
    state.R = 0x0123456789ABCDEFULL;
    state.dmrburstL = 20;
    set_ess_payload_bits(&state, 0, 0x81, 0x2468, 0x1122334455667788ULL);

    p25p2_process_ess(&opts, &state, 0);

    if (state.payload_algid == 0x81 && state.payload_keyid == 0x2468 && state.payload_miP == 0x1122334455667788ULL
        && state.p25_p2_audio_allowed[0] == 1 && state.p25_p2_rs_ess_ok == 1) {
        dsd_state_ext_free_all(&state);
        printf("PASS\n");
        return 0;
    }

    printf("FAIL (alg=0x%02X keyid=0x%04X mi=0x%016llX gate=%d ok=%u)\n", state.payload_algid, state.payload_keyid,
           state.payload_miP, state.p25_p2_audio_allowed[0], state.p25_p2_rs_ess_ok);
    dsd_state_ext_free_all(&state);
    return 1;
}

static int
test_ess_aes_slot1_loaded_key_preserves_audio_gate(void) {
    printf("Test 15: ESS AES slot 1 loaded key preserves audio gate... ");
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    prepare_ess_soft_inputs(&state);
    reset_ess_stubs();
    set_p25p2_threshold(64);

    opts.trunk_enable = 1;
    opts.trunk_is_tuned = 1;
    opts.trunk_tune_enc_calls = 0;
    state.currentslot = 1;
    seed_p25p2_call(&state, 1U, 5678U, 8765U, 0x40U, 0U, 0U);
    state.dmrburstR = 21;
    state.aes_key_loaded[1] = 1;
    state.aes_key_segments[1] = 4U;
    set_ess_payload_bits(&state, 1, 0x84, 0x1357, 0x0123456789ABCDEFULL);

    p25p2_process_ess(&opts, &state, 0);

    if (state.payload_algidR == 0x84 && state.payload_keyidR == 0x1357 && state.payload_miN == 0x0123456789ABCDEFULL
        && state.p25_p2_audio_allowed[1] == 1 && state.p25_p2_rs_ess_ok == 1 && g_lfsr128_calls == 1
        && g_lfsr128_last_slot == 1) {
        dsd_state_ext_free_all(&state);
        printf("PASS\n");
        return 0;
    }

    printf("FAIL (alg=0x%02X keyid=0x%04X mi=0x%016llX gate=%d ok=%u lfsr128=%d slot=%d)\n", state.payload_algidR,
           state.payload_keyidR, state.payload_miN, state.p25_p2_audio_allowed[1], state.p25_p2_rs_ess_ok,
           g_lfsr128_calls, g_lfsr128_last_slot);
    dsd_state_ext_free_all(&state);
    return 1;
}

// The ESS gate must re-open a slot's audio from the canonical call state, not
// from the slot's burst hint. dmrburstL/R only records the last MAC PDU decoded
// for the slot, so a slot still mid-call whose hint reads MAC_END (23),
// MAC_IDLE (24), the LCCH marker (30) or a hint cleared by a teardown could
// never re-open its gate once its ESS resolved the classification it had been
// waiting on -- and the old fallback read a file-scope voice flag, letting one
// slot's burst decide the other slot's gate.
static int
run_ess_burst_hint_case(int slot, int burst) {
    static dsd_opts opts;
    static dsd_state state;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    prepare_ess_soft_inputs(&state);
    reset_ess_stubs();
    set_p25p2_threshold(64);

    opts.trunk_enable = 1;
    opts.trunk_is_tuned = 1;
    opts.trunk_tune_enc_calls = 0;
    state.currentslot = slot;
    for (int i = 0; i < 96; i++) {
        state.ess_b[slot][i] = 0;
        state.ess_b_llr[slot][i] = 1;
    }
    for (int i = 0; i < 168; i++) {
        ess_a[slot][i] = 0;
        ess_a_llr[slot][i] = 1;
    }
    seed_p25p2_call(&state, (uint8_t)slot, 1234U, 4321U, 0x00U, 0U, 0U);
    if (slot == 0) {
        state.dmrburstL = burst;
    } else {
        state.dmrburstR = burst;
    }
    set_ess_payload_bits(&state, slot, 0x80, 0x0000, 0x0000000000000000ULL);

    p25p2_process_ess(&opts, &state, 0);

    const int gate = state.p25_p2_audio_allowed[slot];
    const int crypto = (int)state.p25_crypto_state[slot];
    const unsigned int ess_ok = state.p25_p2_rs_ess_ok;
    dsd_state_ext_free_all(&state);

    if (gate == 1 && crypto == DSD_P25_CRYPTO_CLEAR && ess_ok == 1) {
        return 0;
    }
    DSD_FPRINTF(stderr, "\n  FAIL slot=%d burst=%d gate=%d crypto=%d ess_ok=%u", slot, burst, gate, crypto, ess_ok);
    return 1;
}

static int
test_ess_opens_audio_gate_for_every_burst_hint(void) {
    static const int bursts[] = {0, 20, 21, 22, 23, 24, 30};
    const size_t count = sizeof(bursts) / sizeof(bursts[0]);
    int rc = 0;

    printf("Test 31: ESS opens the audio gate in every MAC state... ");
    for (int slot = 0; slot < 2; slot++) {
        for (size_t i = 0; i < count; i++) {
            rc |= run_ess_burst_hint_case(slot, bursts[i]);
        }
    }
    printf("%s\n", rc == 0 ? "PASS" : "FAIL");
    return rc;
}

static int
test_ess_allow_list_blocks_clear_audio_gate(void) {
    printf("Test 16: ESS allow-list blocks clear audio gate... ");
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    prepare_ess_soft_inputs(&state);
    reset_ess_stubs();
    set_p25p2_threshold(64);

    opts.trunk_use_allow_list = 1;
    seed_p25p2_call(&state, 0U, 1234U, 4321U, 0x00U, 0U, 0U);
    state.tg_hold = 9999;
    state.dmrburstL = 20;
    set_ess_payload_bits(&state, 0, 0x80, 0x0000, 0x0000000000000000ULL);

    p25p2_process_ess(&opts, &state, 0);

    if (state.payload_algid == 0x80 && state.p25_p2_audio_allowed[0] == 0
        && state.p25_crypto_state[0] == DSD_P25_CRYPTO_CLEAR && state.p25_p2_rs_ess_ok == 1) {
        dsd_state_ext_free_all(&state);
        printf("PASS\n");
        return 0;
    }

    printf("FAIL (alg=0x%02X gate=%d crypto=%d ok=%u)\n", state.payload_algid, state.p25_p2_audio_allowed[0],
           (int)state.p25_crypto_state[0], state.p25_p2_rs_ess_ok);
    dsd_state_ext_free_all(&state);
    return 1;
}

static int
test_ess_decode_failure_refreshes_existing_crypto_state(void) {
    printf("Test 17: ESS failure refreshes existing crypto state... ");
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    prepare_ess_soft_inputs(&state);
    reset_ess_stubs();
    set_p25p2_threshold(64);

    state.currentslot = 0;
    state.payload_algid = 0x81;
    state.payload_keyid = 0x2468;
    state.payload_miP = 0x1122334455667788ULL;
    state.R = 0x0102030405060708ULL;
    state.p25_crypto_state[0] = DSD_P25_CRYPTO_DECRYPTABLE;
    state.p25_p2_audio_allowed[0] = 1;
    g_ess_hard_rc = -1;
    g_ess_soft_min_success = -1;

    p25p2_process_ess(&opts, &state, 0);

    if (state.p25_p2_rs_ess_err == 1 && state.p25_p2_rs_ess_ok == 0 && g_lfsrp_calls == 1 && g_lfsr128_calls == 0
        && state.p25_crypto_state[0] == DSD_P25_CRYPTO_DECRYPTABLE && state.p25_p2_audio_allowed[0] == 1) {
        printf("PASS\n");
        return 0;
    }

    printf("FAIL (err=%u ok=%u lfsrp=%d lfsr128=%d)\n", state.p25_p2_rs_ess_err, state.p25_p2_rs_ess_ok, g_lfsrp_calls,
           g_lfsr128_calls);
    return 1;
}

static int
test_ess_decode_failure_refreshes_existing_aes_state(void) {
    printf("Test 18: ESS failure refreshes existing AES state... ");
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    prepare_ess_soft_inputs(&state);
    reset_ess_stubs();
    set_p25p2_threshold(64);

    state.currentslot = 1;
    state.payload_algidR = 0x89;
    state.payload_keyidR = 0x1357;
    state.payload_miN = 0x0123456789ABCDEFULL;
    state.aes_key_loaded[1] = 1;
    state.aes_key_segments[1] = 2U;
    state.p25_crypto_state[1] = DSD_P25_CRYPTO_DECRYPTABLE;
    state.p25_p2_audio_allowed[1] = 1;
    g_ess_hard_rc = -1;
    g_ess_soft_min_success = -1;

    p25p2_process_ess(&opts, &state, 0);

    if (state.p25_p2_rs_ess_err == 1 && g_lfsrp_calls == 1 && g_lfsr128_calls == 1 && g_lfsr128_last_slot == 1
        && state.p25_crypto_state[1] == DSD_P25_CRYPTO_DECRYPTABLE && state.p25_p2_audio_allowed[1] == 1) {
        printf("PASS\n");
        return 0;
    }

    printf("FAIL (err=%u lfsrp=%d lfsr128=%d slot=%d)\n", state.p25_p2_rs_ess_err, g_lfsrp_calls, g_lfsr128_calls,
           g_lfsr128_last_slot);
    return 1;
}

static int
test_facchc_success_routes_opcode_and_counters(void) {
    printf("Test 19: FACCHc success routes opcode and counters... ");
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    p25_p2_frame_reset();
    reset_xcch_stubs();

    state.currentslot = 1;
    g_facch_success_rc = 3;
    seed_xcch_opcode(2, 5);

    p25p2_process_facchc(&opts, &state, 2);

    int rc = 0;
    rc |= expect_int("facch ok", (int)state.p25_p2_rs_facch_ok, 1);
    rc |= expect_int("facch err", (int)state.p25_p2_rs_facch_err, 0);
    rc |= expect_int("facch corr", (int)state.p25_p2_rs_facch_corr, 3);
    rc |= expect_int("facch slot1 opcode", state.dmr_soR, 5);
    rc |= expect_int("facch mac calls", g_facch_mac_calls, 1);
    rc |= expect_int("facch mac opcode", g_facch_mac_last_opcode, 5);
    rc |= expect_int("facch fixed erasures", g_facch_last_erasures, 18);
    if (rc == 0) {
        printf("PASS\n");
    } else {
        printf("FAIL\n");
    }
    return rc;
}

static int
test_facchc_failure_preserves_mac_and_counts_error(void) {
    printf("Test 20: FACCHc failure counts error without MAC dispatch... ");
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    p25_p2_frame_reset();
    reset_xcch_stubs();

    state.currentslot = 0;
    state.dmr_so = 99;
    g_facch_min_success = 99;
    seed_xcch_opcode(0, 3);

    p25p2_process_facchc(&opts, &state, 0);

    int rc = 0;
    rc |= expect_int("facch failure ok", (int)state.p25_p2_rs_facch_ok, 0);
    rc |= expect_int("facch failure err", (int)state.p25_p2_rs_facch_err, 1);
    rc |= expect_int("facch failure opcode captured", state.dmr_so, 3);
    rc |= expect_int("facch failure no mac", g_facch_mac_calls, 0);
    rc |= expect_int("facch failure tried ranked erasures", g_facch_calls > 1, 1);
    if (rc == 0) {
        printf("PASS\n");
    } else {
        printf("FAIL\n");
    }
    return rc;
}

static int
test_sacchc_dynamic_erasure_success_maps_inverse_slot(void) {
    printf("Test 21: SACCHc dynamic erasure success maps inverse slot... ");
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    p25_p2_frame_reset();
    reset_xcch_stubs();
    set_p25p2_threshold(64);

    state.currentslot = 0;
    g_sacch_min_success = 12;
    g_sacch_success_rc = 4;
    seed_xcch_opcode(1, 6);

    p25p2_process_sacchc(&opts, &state, 1);

    int rc = 0;
    rc |= expect_int("sacch ok", (int)state.p25_p2_rs_sacch_ok, 1);
    rc |= expect_int("sacch err", (int)state.p25_p2_rs_sacch_err, 0);
    rc |= expect_int("sacch corr", (int)state.p25_p2_rs_sacch_corr, 4);
    rc |= expect_int("sacch dynamic erasure", (int)state.p25_p2_soft_erasure_ok, 1);
    rc |= expect_int("sacch inverse opcode", state.dmr_soR, 6);
    rc |= expect_int("sacch mac calls", g_sacch_mac_calls, 1);
    rc |= expect_int("sacch mac opcode", g_sacch_mac_last_opcode, 6);
    rc |= expect_int("sacch erasure depth", g_sacch_last_erasures, 12);
    rc |= expect_int("sacch dynamic erasure retried after the first attempt", g_sacch_calls > 1, 1);
    if (rc == 0) {
        printf("PASS\n");
    } else {
        printf("FAIL\n");
    }
    return rc;
}

static int
test_isch_channel_one_location_sets_scramble_offset(void) {
    printf("Test 22: ISCH channel/location sets scramble offset... ");
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    p25_p2_frame_reset();
    reset_xcch_stubs();

    p2bit[320] = 1;
    g_isch_lookup_result = (1 << 5) | (0 << 3);
    state.p2_scramble_offset = -1;
    p25p2_process_isch(&opts, &state, 3);

    int rc = 0;
    rc |= expect_int("isch loc0 channel", state.p2_vch_chan_num, 1);
    rc |= expect_int("isch loc0 offset", state.p2_scramble_offset, 9);

    g_isch_lookup_result = (1 << 5) | (1 << 3);
    state.p2_scramble_offset = -1;
    p25p2_process_isch(&opts, &state, 1);
    rc |= expect_int("isch loc1 offset", state.p2_scramble_offset, 3);

    g_isch_lookup_result = (1 << 5) | (2 << 3);
    state.p2_scramble_offset = -1;
    p25p2_process_isch(&opts, &state, 2);
    rc |= expect_int("isch loc2 offset", state.p2_scramble_offset, 6);

    g_isch_lookup_result = -1;
    state.p2_vch_chan_num = 7;
    state.p2_scramble_offset = 42;
    p25p2_process_isch(&opts, &state, 0);
    rc |= expect_int("isch invalid preserves channel", state.p2_vch_chan_num, 7);
    rc |= expect_int("isch invalid preserves offset", state.p2_scramble_offset, 42);

    if (rc == 0) {
        printf("PASS\n");
    } else {
        printf("FAIL\n");
    }
    return rc;
}

static int
test_duid_exact_or_null_soft_metrics_preserve_hard_decision(void) {
    printf("Test 25: DUID exact/null soft metrics preserve hard decision... ");
    uint8_t unreliable[8] = {0, 0, 0, 0, 0, 0, 0, 0};

    int rc = 0;
    rc |= expect_int("duid exact with unreliable metrics", p25p2_duid_lookup_soft(0x17U, unreliable), 1);
    rc |= expect_int("duid null metrics uses hard table", p25p2_duid_lookup_soft(0x17U, NULL), 1);

    if (rc == 0) {
        printf("PASS\n");
    } else {
        printf("FAIL\n");
    }
    return rc;
}

static int
test_isch_reliability_clamps_llr_magnitude(void) {
    printf("Test 26: ISCH reliability clamps LLR magnitude... ");
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    p25_p2_frame_reset();
    reset_xcch_stubs();

    p2bit[320] = 1;
    p2llr[320] = 300;
    p2llr[321] = -123;
    g_isch_lookup_result = -1;

    p25p2_process_isch(&opts, &state, 0);

    int rc = 0;
    rc |= expect_int("isch clamp high reliability", g_isch_last_reliab[0], 255);
    rc |= expect_int("isch abs negative reliability", g_isch_last_reliab[1], 123);
    rc |= expect_int("isch failed decode preserves channel", state.p2_vch_chan_num, 0);

    if (rc == 0) {
        printf("PASS\n");
    } else {
        printf("FAIL\n");
    }
    return rc;
}

static void
seed_teardown_dirty_state(dsd_state* state) {
    state->p25_p2_audio_allowed[0] = 0;
    state->p25_p2_audio_allowed[1] = 0;
    state->p25_crypto_state[0] = DSD_P25_CRYPTO_BLOCKED;
    state->p25_crypto_state[1] = DSD_P25_CRYPTO_BLOCKED;
    state->p25_p2_audio_ring_count[0] = 2;
    state->p25_p2_audio_ring_count[1] = 3;
    state->p25p2_playout.slot[0].open = 1U;
    state->p25p2_playout.slot[0].phase_2v = 3;
    state->p25p2_playout.slot[1].open = 1U;
    state->p25p2_playout.slot[1].fill_debt = 2;
    state->p25_p2_last_mac_active[0] = 111;
    state->p25_p2_last_mac_active[1] = 222;
    state->p25_p2_last_end_ptt[0] = 333;
    state->p25_p2_last_end_ptt[1] = 444;
    seed_p25p2_call(state, 0U, 4100U, 5100U, 0xC5U, 1U, 5U);
    seed_p25p2_call(state, 1U, 4200U, 5200U, 0xC6U, 1U, 6U);
    state->payload_algid = 0x81;
    state->payload_keyid = 0x2468;
    state->payload_miP = 0x1122334455667788ULL;
    state->payload_algidR = 0x84;
    state->payload_keyidR = 0x1357;
    state->payload_miN = 0x0123456789ABCDEFULL;
}

static int
expect_teardown_common_reset(const dsd_state* state) {
    int rc = 0;
    rc |= expect_int("teardown gate left", state->p25_p2_audio_allowed[0], 0);
    rc |= expect_int("teardown gate right", state->p25_p2_audio_allowed[1], 0);
    rc |= expect_int("teardown crypto left", state->p25_crypto_state[0], DSD_P25_CRYPTO_UNKNOWN);
    rc |= expect_int("teardown crypto right", state->p25_crypto_state[1], DSD_P25_CRYPTO_UNKNOWN);
    rc |= expect_int("teardown ring left", state->p25_p2_audio_ring_count[0], 0);
    rc |= expect_int("teardown ring right", state->p25_p2_audio_ring_count[1], 0);
    rc |= expect_int("teardown playout left closed", state->p25p2_playout.slot[0].open, 0);
    rc |= expect_int("teardown playout left phase reset", state->p25p2_playout.slot[0].phase_2v, -1);
    rc |= expect_int("teardown playout left empty", dsd_p25p2_playout_level(state, 0), 0);
    rc |= expect_int("teardown playout right closed", state->p25p2_playout.slot[1].open, 0);
    rc |= expect_int("teardown playout right debt reset", state->p25p2_playout.slot[1].fill_debt, 0);
    rc |= expect_int("teardown playout right empty", dsd_p25p2_playout_level(state, 1), 0);
    rc |= expect_int("teardown mac active left", (int)state->p25_p2_last_mac_active[0], 0);
    rc |= expect_int("teardown mac active right", (int)state->p25_p2_last_mac_active[1], 0);
    rc |= expect_int("teardown end ptt left", (int)state->p25_p2_last_end_ptt[0], 0);
    rc |= expect_int("teardown end ptt right", (int)state->p25_p2_last_end_ptt[1], 0);
    rc |= expect_int("teardown alg left", state->payload_algid, 0);
    rc |= expect_int("teardown key left", state->payload_keyid, 0);
    rc |= expect_int("teardown mi left", state->payload_miP != 0ULL, 0);
    rc |= expect_int("teardown alg right", state->payload_algidR, 0);
    rc |= expect_int("teardown key right", state->payload_keyidR, 0);
    rc |= expect_int("teardown mi right", state->payload_miN != 0ULL, 0);
    return rc;
}

static int
test_teardown_flushes_partial_int16_audio_and_resets_call_state(void) {
    printf("Test 23: teardown flushes partial int16 audio and resets call state... ");
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_playback_stub();

    enable_captured_output(&opts);
    seed_teardown_dirty_state(&state);
    /* One frame each slot admitted while its call was clear, still queued when the call tears down. */
    state.p25_crypto_state[0] = DSD_P25_CRYPTO_CLEAR;
    state.p25_crypto_state[1] = DSD_P25_CRYPTO_CLEAR;
    for (int i = 0; i < 160; i++) {
        state.s_l[i] = 123;
        state.s_r[i] = -234;
    }
    dsd_p25p2_playout_stage(&opts, &state, 0, 2, 1U, NULL);
    dsd_p25p2_playout_stage(&opts, &state, 1, 2, 1U, NULL);
    state.p25_crypto_state[0] = DSD_P25_CRYPTO_BLOCKED;
    state.p25_crypto_state[1] = DSD_P25_CRYPTO_BLOCKED;

    p25p2_teardown_call(&opts, &state);

    int rc = 0;
    rc |= expect_int("teardown plays the queued frames", g_out_calls, 1);
    rc |= expect_int("teardown plays the admitted audio", g_out_first_left, 123);
    rc |= expect_teardown_common_reset(&state);
    rc |= expect_call_state("teardown retains left call until release", &state, 0U, DSD_CALL_PHASE_ACTIVE, 4100U, 5100U,
                            1U, 5U);
    rc |= expect_call_state("teardown retains right call until release", &state, 1U, DSD_CALL_PHASE_ACTIVE, 4200U,
                            5200U, 1U, 6U);
    dsd_udp_audio_hooks_set((dsd_udp_audio_hooks){0});

    if (rc == 0) {
        printf("PASS\n");
    } else {
        printf("FAIL\n");
    }
    dsd_state_ext_free_all(&state);
    return rc;
}

static int
test_teardown_without_partial_int16_audio_skips_playback_but_clears_state(void) {
    printf("Test 24: teardown without partial int16 audio skips playback but clears state... ");
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_playback_stub();

    enable_captured_output(&opts);
    seed_teardown_dirty_state(&state);

    p25p2_teardown_call(&opts, &state);

    int rc = 0;
    rc |= expect_int("teardown no-audio playback calls", g_out_calls, 0);
    rc |= expect_teardown_common_reset(&state);
    dsd_udp_audio_hooks_set((dsd_udp_audio_hooks){0});
    rc |= expect_call_state("no-audio teardown retains left call until release", &state, 0U, DSD_CALL_PHASE_ACTIVE,
                            4100U, 5100U, 1U, 5U);
    rc |= expect_call_state("no-audio teardown retains right call until release", &state, 1U, DSD_CALL_PHASE_ACTIVE,
                            4200U, 5200U, 1U, 6U);

    if (rc == 0) {
        printf("PASS\n");
    } else {
        printf("FAIL\n");
    }
    dsd_state_ext_free_all(&state);
    return rc;
}

static int
test_duid_invalid_burst_aborts_without_reopening_crypto(void) {
    printf("Test 27: DUID repeated invalid bursts abort without reopening crypto... ");
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    p25_p2_frame_reset();

    state.currentslot = 0;
    state.payload_algid = 0x81;
    state.payload_keyid = 0x2468;
    state.payload_algidR = 0x84;
    state.payload_keyidR = 0x1357;
    state.p2_is_lcch = 1;
    state.p25_crypto_state[0] = DSD_P25_CRYPTO_BLOCKED;
    state.p25_crypto_state[1] = DSD_P25_CRYPTO_BLOCKED;
    state.fourv_counter[0] = 3;
    state.fourv_counter[1] = 4;
    dsd_p25p2_playout_reset(&state, -1);
    seed_duid_bits(0, 0x03U);
    seed_duid_bits(1, 0x03U);

    p25p2_process_duid(&opts, &state);

    int rc = 0;
    rc |= expect_int("invalid abort alg left preserved", state.payload_algid, 0x81);
    rc |= expect_int("invalid abort key left preserved", state.payload_keyid, 0x2468);
    rc |= expect_int("invalid abort alg right preserved", state.payload_algidR, 0x84);
    rc |= expect_int("invalid abort key right preserved", state.payload_keyidR, 0x1357);
    rc |= expect_int("invalid abort lcch", state.p2_is_lcch, 0);
    rc |= expect_int("invalid abort state left sticky", state.p25_crypto_state[0], DSD_P25_CRYPTO_BLOCKED);
    rc |= expect_int("invalid abort state right sticky", state.p25_crypto_state[1], DSD_P25_CRYPTO_BLOCKED);
    rc |= expect_int("invalid abort fourv left", state.fourv_counter[0], 0);
    rc |= expect_int("invalid abort fourv right", state.fourv_counter[1], 0);
    /* The aborted window's timeslots retire through the playout: both slots reached the window's last pair. */
    rc |= expect_int("invalid abort retires slot 1's timeslots", state.p25p2_playout.slot[0].cur_pair, 1);
    rc |= expect_int("invalid abort retires slot 2's timeslots", state.p25p2_playout.slot[1].cur_pair, 1);
    rc |= expect_int("invalid abort retired the last timeslot", state.p25p2_playout.slot[1].cur_pair_done, 1);
    if (rc == 0) {
        printf("PASS\n");
    } else {
        printf("FAIL\n");
    }
    return rc;
}

static int
test_duid_abort_resolves_staged_rekey(void) {
    printf("Test 28: DUID abort plays the retired pair and then resolves a staged rekey... ");
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    p25_p2_frame_reset();
    reset_ess_stubs();
    reset_xcch_stubs();
    reset_playback_stub();

    enable_captured_output(&opts);
    opts.trunk_tune_enc_calls = 0;
    dsd_p25p2_playout_reset(&state, -1);
    seed_p25p2_call(&state, 0U, 4100U, 5100U, 0x40U, 0U, 4U);
    state.currentslot = 0;
    state.p2_wacn = 1;
    state.p2_cc = 0x123;
    state.p2_sysid = 1;
    state.payload_algid = 0x81;
    state.payload_keyid = 0x2468;
    state.payload_miP = 0x1122334455667788ULL;
    state.R = 0x0102030405060708ULL;
    state.dmr_so = 0x40;
    state.dmrburstL = 21;
    state.p25_crypto_state[0] = DSD_P25_CRYPTO_DECRYPTABLE;
    state.p25_p2_audio_allowed[0] = 1;
    state.s_l[0] = 321;
    set_ess_payload_bits(&state, 0, 0xAA, 0x1357, 0x8877665544332211ULL);

    seed_duid_bits(0, 0x03U); // first invalid DUID
    seed_duid_bits(1, 0x39U); // SACCHs keeps processing the frame
    seed_duid_bits(2, 0x65U); // 2V stages the new ESS identity
    seed_duid_bits(3, 0x03U); // second invalid DUID aborts before post-timeslot

    p25p2_process_duid(&opts, &state);

    int rc = 0;
    // The 2V's two frames, decoded under the old identity, play when the abort retires the pair; only then is the
    // new identity promoted.
    rc |= expect_int("abort rekey plays the 2V frames", g_out_calls, 2);
    rc |= expect_int("abort rekey plays the old stream's audio", g_out_first_left, 321);
    rc |= expect_int("abort rekey pending during output", g_out_pending_at_call, 1);
    rc |= expect_int("abort rekey old key during output", g_out_keyid_at_call, 0x2468);
    rc |= expect_int("abort rekey transition cleared", state.p25_p2_rekey[0].pending, 0);
    rc |= expect_int("abort rekey alg promoted", state.payload_algid, 0xAA);
    rc |= expect_int("abort rekey key promoted", state.payload_keyid, 0x1357);
    rc |= expect_int("abort rekey playout emptied", dsd_p25p2_playout_level(&state, 0), 0);
    dsd_udp_audio_hooks_set((dsd_udp_audio_hooks){0});
    if (rc == 0) {
        printf("PASS\n");
    } else {
        printf("FAIL\n");
    }
    return rc;
}

/* One superframe of four bursts with the given DUID codewords, on a site whose seed is set. */
static void
run_seeded_superframe(dsd_opts* opts, dsd_state* state, const uint8_t duids[4]) {
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    p25_p2_frame_reset();
    reset_ess_stubs();
    reset_playback_stub();
    state->p2_wacn = 1;
    state->p2_sysid = 1;
    state->p2_cc = 0x123;
    for (int i = 0; i < 4; i++) {
        seed_duid_bits(i, duids[i]);
    }
    p25p2_process_frame_scramble(opts, state);
    p25p2_process_duid(opts, state);
}

/* The ESS fixture's buffer, descrambled with the site's seed, as processP2() descrambles every superframe before its
   bursts decode (issue #575). */
static void
descramble_with_site_seed(dsd_opts* opts, dsd_state* state) {
    state->p2_wacn = 1;
    state->p2_sysid = 1;
    state->p2_cc = 0x123;
    p25p2_process_frame_scramble(opts, state);
}

/* The same, with the seed changed after the buffer was descrambled, as a network status broadcast decoded from an
   earlier burst of the buffer changes it (under -F even one that failed its CRC). */
static void
run_reseeded_superframe(dsd_opts* opts, dsd_state* state, const uint8_t duids[4]) {
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    p25_p2_frame_reset();
    reset_ess_stubs();
    reset_playback_stub();
    state->p2_wacn = 1;
    state->p2_sysid = 1;
    state->p2_cc = 0x123;
    for (int i = 0; i < 4; i++) {
        seed_duid_bits(i, duids[i]);
    }
    p25p2_process_frame_scramble(opts, state);
    state->p2_cc = 0x456;
    p25p2_process_duid(opts, state);
}

/*
 * Issue #575: p2_cc is the NAC a Phase 2 call was heard with only once a burst descrambled with it passed its
 * Reed-Solomon check -- a scrambled FACCH (DUID 9) or SACCH (DUID 3), or an ESS, which bits descrambled with a wrong
 * seed fail. An unscrambled FACCH (DUID 15) or SACCH (DUID 12) never tests the seed, however well it decodes, and a
 * scrambled burst that fails its check proves nothing either.
 */
static int
test_seed_proof_needs_a_descrambled_burst(void) {
    printf("Test 32: only a descrambled burst that decodes proves the seed... ");
    static dsd_opts opts;
    static dsd_state state;
    static const uint8_t unscrambled[4] = {0xFFU, 0xC6U, 0xFFU, 0xC6U};
    static const uint8_t scrambled_facch[4] = {0x9AU, 0x9AU, 0x9AU, 0x9AU};
    static const uint8_t scrambled_sacch[4] = {0x39U, 0x39U, 0x39U, 0x39U};
    int rc = 0;

    reset_xcch_stubs();
    run_seeded_superframe(&opts, &state, unscrambled);
    rc |= expect_int("unscrambled bursts decode", g_facch_mac_calls > 0 && g_sacch_mac_calls > 0, 1);
    rc |= expect_int("unscrambled bursts prove nothing", state.p2_cc_verified, 0);

    reset_xcch_stubs();
    g_facch_min_success = 99;
    g_sacch_min_success = 99;
    run_seeded_superframe(&opts, &state, scrambled_facch);
    rc |= expect_int("failed scrambled FACCH proves nothing", state.p2_cc_verified, 0);
    run_seeded_superframe(&opts, &state, scrambled_sacch);
    rc |= expect_int("failed scrambled SACCH proves nothing", state.p2_cc_verified, 0);

    reset_xcch_stubs();
    run_seeded_superframe(&opts, &state, scrambled_facch);
    rc |= expect_int("scrambled FACCH decodes", g_facch_mac_calls > 0, 1);
    rc |= expect_int("scrambled FACCH proves the seed", state.p2_cc_verified, 1);
    reset_xcch_stubs();
    run_seeded_superframe(&opts, &state, scrambled_sacch);
    rc |= expect_int("scrambled SACCH proves the seed", state.p2_cc_verified, 1);

    /* A scrambled burst that decodes only once soft erasures are added checked less of its parity than its fixed
       erasures leave, possibly none of it: it still decodes, but proves nothing. */
    reset_xcch_stubs();
    g_facch_min_success = 19; /* one dynamic erasure past the 18 fixed */
    run_seeded_superframe(&opts, &state, scrambled_facch);
    rc |= expect_int("soft-erasure scrambled FACCH decodes", g_facch_mac_calls > 0, 1);
    rc |= expect_int("soft-erasure scrambled FACCH proves nothing", state.p2_cc_verified, 0);
    reset_xcch_stubs();
    g_sacch_min_success = 12; /* one dynamic erasure past the 11 fixed */
    run_seeded_superframe(&opts, &state, scrambled_sacch);
    rc |= expect_int("soft-erasure scrambled SACCH decodes", g_sacch_mac_calls > 0, 1);
    rc |= expect_int("soft-erasure scrambled SACCH proves nothing", state.p2_cc_verified, 0);

    prepare_ess_soft_inputs(&state);
    reset_ess_stubs();
    g_ess_hard_rc = -1;
    p25p2_process_ess(&opts, &state, 0);
    rc |= expect_int("failed ESS proves nothing", state.p2_cc_verified, 0);
    prepare_ess_soft_inputs(&state);
    descramble_with_site_seed(&opts, &state);
    reset_ess_stubs();
    g_ess_hard_rc = 0;
    p25p2_process_ess(&opts, &state, 0);
    rc |= expect_int("ESS proves the seed", state.p2_cc_verified, 1);
    prepare_ess_soft_inputs(&state);
    descramble_with_site_seed(&opts, &state);
    reset_ess_stubs();
    g_ess_hard_rc = -1;
    g_ess_soft_min_success = 1;
    g_ess_soft_success_rc = 0;
    p25p2_process_ess(&opts, &state, 0);
    rc |= expect_int("soft-erasure ESS decodes", g_ess_soft_calls > 0 && state.p25_p2_rs_ess_ok == 1U, 1);
    rc |= expect_int("soft-erasure ESS proves nothing", state.p2_cc_verified, 0);

    /* A burst decoded from a buffer descrambled with another seed proves that seed, not the one in force now. */
    reset_xcch_stubs();
    run_reseeded_superframe(&opts, &state, scrambled_facch);
    rc |= expect_int("reseeded scrambled FACCH decodes", g_facch_mac_calls > 0, 1);
    rc |= expect_int("reseeded scrambled FACCH proves nothing", state.p2_cc_verified, 0);
    reset_xcch_stubs();
    run_reseeded_superframe(&opts, &state, scrambled_sacch);
    rc |= expect_int("reseeded scrambled SACCH proves nothing", state.p2_cc_verified, 0);
    prepare_ess_soft_inputs(&state);
    descramble_with_site_seed(&opts, &state);
    state.p2_cc = 0x456;
    reset_ess_stubs();
    g_ess_hard_rc = 0;
    p25p2_process_ess(&opts, &state, 0);
    rc |= expect_int("reseeded ESS proves nothing", state.p2_cc_verified, 0);

    reset_xcch_stubs();
    reset_ess_stubs();
    if (rc == 0) {
        printf("PASS\n");
    } else {
        printf("FAIL\n");
    }
    return rc;
}

/* processP2() reading the superframe the feed holds, with the carrier boundary at read @p boundary_at (-1: none). */
static void
read_fed_superframe(dsd_opts* opts, dsd_state* state, int boundary_at) {
    g_dibit_feed_pos = 0;
    g_dibit_feed_boundary_at = boundary_at;
    g_dibit_feed_active = 1;
    g_isch_feed_calls = 0;
    processP2(opts, state);
    g_isch_feed_calls = -1;
    g_dibit_feed_active = 0;
}

/* The fed window again as the next one read on the carrier, @p gap dibits after the previous window ended (its sync
   alone is 20), with its ISCH decoded or not (issue #651). */
static void
read_fed_window(dsd_opts* opts, dsd_state* state, uint32_t gap, int isch_located) {
    state->symbolcnt += gap;
    g_dibit_feed_pos = 0;
    g_dibit_feed_boundary_at = -1;
    g_dibit_feed_active = 1;
    g_isch_feed_calls = isch_located ? 0 : -1;
    processP2(opts, state);
    g_isch_feed_calls = -1;
    g_dibit_feed_active = 0;
}

/* A superframe of four bursts with the given DUID codewords, fed to processP2() on a site whose seed is set. */
static void
run_fed_superframe(dsd_opts* opts, dsd_state* state, const uint8_t duids[4], int boundary_at) {
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    p25_p2_frame_reset();
    reset_ess_stubs();
    reset_playback_stub();
    reset_xcch_stubs();
    for (int i = 0; i < 4; i++) {
        seed_duid_bits(i, duids[i]);
    }
    for (size_t i = 0; i < sizeof g_dibit_feed; i++) {
        const size_t bit = i * 2U;
        g_dibit_feed[i] = (uint8_t)((p2bit[bit] << 1) | p2bit[bit + 1U]);
    }
    state->p2_wacn = 1;
    state->p2_sysid = 1;
    state->p2_cc = 0x123;
    read_fed_superframe(opts, state, boundary_at);
}

/*
 * Issue #575: processP2() collects four bursts before any of them decodes. A replay read that adopts a retune its
 * capture recorded runs the carrier boundary inside that collection; the bursts read before it belong to the carrier
 * left, and decoding them afterwards would open the call the boundary just ended and prove the seed for the carrier
 * the receiver moved to. The superframe is dropped whole, as on a sync loss. A boundary between superframes drops what
 * the slots gathered on the carrier left: ESS fragments, a partial voice superframe and a staged rekey.
 */
static int
test_superframe_split_by_a_carrier_boundary_is_dropped(void) {
    printf("Test 33: a superframe the carrier boundary splits is dropped... ");
    static dsd_opts opts;
    static dsd_state state;
    static const uint8_t scrambled_facch[4] = {0x9AU, 0x9AU, 0x9AU, 0x9AU};
    int rc = 0;

    run_fed_superframe(&opts, &state, scrambled_facch, -1);
    rc |= expect_int("whole superframe decodes its FACCHs", g_facch_mac_calls, 4);
    rc |= expect_int("whole superframe proves the seed", (int)state.p2_cc_verified, 1);

    run_fed_superframe(&opts, &state, scrambled_facch, 400);
    rc |= expect_int("split superframe decodes no FACCH", g_facch_calls, 0);
    rc |= expect_int("split superframe opens no call", g_facch_mac_calls, 0);
    rc |= expect_int("split superframe proves nothing", (int)state.p2_cc_verified, 0);
    rc |= expect_int("split superframe leaves the stereo mark", state.dmr_stereo, 0);

    /* The boundary at the first read: nothing of the carrier left was collected, but the superframe still straddles
       the move as far as the decoder can tell, and goes too. */
    run_fed_superframe(&opts, &state, scrambled_facch, 0);
    rc |= expect_int("superframe split at its first read decodes nothing", g_facch_mac_calls, 0);

    /* Between superframes: the next one decodes, and what the slots gathered before the move is gone. */
    run_fed_superframe(&opts, &state, scrambled_facch, -1);
    state.fourv_counter[0] = 2;
    state.p25p2_playout.slot[0].open = 1U;
    state.p25p2_playout.slot[0].phase_2v = 2;
    state.p25_p2_rekey[0].pending = 1U;
    state.carrier_seq++;
    reset_xcch_stubs();
    read_fed_superframe(&opts, &state, -1);
    rc |= expect_int("next superframe decodes", g_facch_mac_calls, 4);
    rc |= expect_int("ESS fragments of the carrier left go", state.fourv_counter[0], 0);
    rc |= expect_int("voice stream of the carrier left goes", state.p25p2_playout.slot[0].open, 0);
    rc |= expect_int("voice phase of the carrier left goes", state.p25p2_playout.slot[0].phase_2v, -1);
    rc |= expect_int("staged rekey of the carrier left goes", (int)state.p25_p2_rekey[0].pending, 0);

    /* No move: the slots keep what they gathered. */
    state.fourv_counter[0] = 2;
    state.p25p2_playout.slot[0].phase_2v = 2;
    reset_xcch_stubs();
    read_fed_superframe(&opts, &state, -1);
    rc |= expect_int("same carrier keeps the ESS fragments", state.fourv_counter[0], 2);
    rc |= expect_int("same carrier keeps the voice phase", state.p25p2_playout.slot[0].phase_2v, 2);

    reset_xcch_stubs();
    reset_ess_stubs();
    if (rc == 0) {
        printf("PASS\n");
    } else {
        printf("FAIL\n");
    }
    return rc;
}

/*
 * Issue #575: the first burst descrambled with a proven seed may itself carry the MAC_END_PTT that ends its call. The
 * committed row reads the live NAC only while the call is active, so the slots are rendered as soon as the proof is set,
 * before the burst's MAC PDU is dispatched; otherwise the row of a call ended by the burst that proved its NAC kept none.
 * Both the scrambled FACCH and the scrambled SACCH prove it this way.
 */
static int
test_seed_proof_reaches_the_call_its_burst_ends(void) {
    printf("Test 34: the seed a terminating burst proves reaches the call it ends... ");
    static dsd_opts opts;
    static dsd_state state;
    static const uint8_t scrambled_facch[4] = {0x9AU, 0x9AU, 0x9AU, 0x9AU};
    static const uint8_t scrambled_sacch[4] = {0x39U, 0x39U, 0x39U, 0x39U};
    const uint8_t* const duids[2] = {scrambled_facch, scrambled_sacch};
    static const char* const labels[2] = {"scrambled FACCH", "scrambled SACCH"};
    int rc = 0;

    for (int path = 0; path < 2; path++) {
        DSD_MEMSET(&opts, 0, sizeof(opts));
        DSD_MEMSET(&state, 0, sizeof(state));
        p25_p2_frame_reset();
        reset_ess_stubs();
        reset_playback_stub();
        reset_xcch_stubs();
        seed_p25p2_call(&state, 0U, 1201U, 1202U, 0U, 0U, 0U);
        state.p2_wacn = 1;
        state.p2_sysid = 1;
        state.p2_cc = 0x123;
        for (int i = 0; i < 4; i++) {
            seed_duid_bits(i, duids[path][i]);
        }
        g_mac_ends_calls = 1;
        g_render_saw_proven_active_call = 0;
        p25p2_process_frame_scramble(&opts, &state);
        p25p2_process_duid(&opts, &state);
        g_mac_ends_calls = 0;

        dsd_call_snapshot call;
        char tag[96];
        DSD_SNPRINTF(tag, sizeof(tag), "%s proves the seed", labels[path]);
        rc |= expect_int(tag, (int)state.p2_cc_verified, 1);
        DSD_SNPRINTF(tag, sizeof(tag), "%s ends the call", labels[path]);
        rc |= expect_int(tag, dsd_call_state_get(&state, 0U, &call) > 0 && call.phase == DSD_CALL_PHASE_ENDED, 1);
        DSD_SNPRINTF(tag, sizeof(tag), "%s renders the call with the proven NAC before ending it", labels[path]);
        rc |= expect_int(tag, g_render_saw_proven_active_call, 1);
        dsd_state_ext_free_all(&state);
    }

    reset_xcch_stubs();
    reset_ess_stubs();
    if (rc == 0) {
        printf("PASS\n");
    } else {
        printf("FAIL\n");
    }
    return rc;
}

static void
prepare_lcch_release_duids(void) {
    p25_p2_frame_reset();
    for (int i = 0; i < 4; i++) {
        seed_duid_bits(i, 0xD1U);
        seed_xcch_opcode(i, 0);
    }
}

static int
test_duid_lcch_release_defers_during_vc_grace(void) {
    printf("Test 29: DUID LCCH release defers during VC grace... ");
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_xcch_stubs();
    prepare_lcch_release_duids();

    time_t now = time(NULL);
    opts.floating_point = 0;
    opts.pulse_digi_rate_out = 8000;
    opts.trunk_enable = 1;
    opts.trunk_is_tuned = 1;
    opts.trunk_hangtime = 1;
    state.currentslot = 0;
    state.last_vc_sync_time = now - 10;
    /*
     * Tuned 5 s ago against a 10 s grace. The offset is deliberately larger than
     * the 0.75 s built-in default: tuning "now" would defer the release under
     * any grace at all, so the case would pass even if the configured value
     * never reached the decoder, which is exactly the bug this wiring can have.
     */
    state.p25_last_vc_tune_time = now - 5;
    set_p25_vc_grace("10.0");

    p25p2_process_duid(&opts, &state);
    set_p25_vc_grace(NULL);

    int rc = 0;
    rc |= expect_int("grace release force", state.p25_sm_force_release, 0);
    rc |= expect_int("grace tuned remains", opts.trunk_is_tuned, 1);
    rc |= expect_int("grace lcch seen", state.p2_is_lcch, 1);
    rc |= expect_int("grace sacch dispatches", g_sacch_mac_calls, 4);
    if (rc == 0) {
        printf("PASS\n");
    } else {
        printf("FAIL\n");
    }
    return rc;
}

static int
test_duid_lcch_release_tears_down_after_vc_grace(void) {
    printf("Test 30: DUID LCCH release tears down after VC grace... ");
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_xcch_stubs();
    reset_playback_stub();
    prepare_lcch_release_duids();

    time_t now = time(NULL);
    opts.floating_point = 0;
    opts.pulse_digi_rate_out = 8000;
    opts.trunk_enable = 1;
    opts.trunk_is_tuned = 1;
    opts.trunk_hangtime = 1;
    state.currentslot = 0;
    state.last_vc_sync_time = now - 10;
    state.p25_last_vc_tune_time = now - 10;
    seed_teardown_dirty_state(&state);
    state.last_vc_sync_time = now - 10;
    state.p25_last_vc_tune_time = now - 10;
    /* Tuned 10 s ago, so a 0.25 s grace has long expired. Set after
     * seed_teardown_dirty_state() only for symmetry with the case above; the
     * grace now lives in the runtime config, which that helper does not touch. */
    set_p25_vc_grace("0.25");

    p25p2_process_duid(&opts, &state);
    set_p25_vc_grace(NULL);

    int rc = 0;
    rc |= expect_int("post-grace release consumed", state.p25_sm_force_release, 0);
    rc |= expect_int("post-grace tuned cleared", opts.trunk_is_tuned, 0);
    rc |= expect_teardown_common_reset(&state);
    rc |=
        expect_call_state("post-grace release ends left call", &state, 0U, DSD_CALL_PHASE_ENDED, 4100U, 5100U, 1U, 5U);
    rc |=
        expect_call_state("post-grace release ends right call", &state, 1U, DSD_CALL_PHASE_ENDED, 4200U, 5200U, 1U, 6U);
    rc |= expect_int("post-grace sacch dispatches", g_sacch_mac_calls >= 1, 1);
    if (rc == 0) {
        printf("PASS\n");
    } else {
        printf("FAIL\n");
    }
    dsd_state_ext_free_all(&state);
    return rc;
}

/* A grant accepted inside a burst's MAC PDU retunes: the engine runs p25_p2_frame_reset() for a TDMA voice channel,
   which here the hook does once. */
static int g_retune_hook_fired = 0;

static void
sacch_hook_retune_once(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    if (!g_retune_hook_fired) {
        g_retune_hook_fired = 1;
        p25_p2_frame_reset();
    }
}

/* An assignment accepted on the tuned TDMA carrier (its idle slot): no retune, but the grant refreshes the carrier's
   voice and tune times, as p25_grant_refresh_reused_carrier_watchdogs() does. */
static void
sacch_hook_reused_carrier_grant_once(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    if (!g_retune_hook_fired && state) {
        g_retune_hook_fired = 1;
        state->last_vc_sync_time = dsd_decode_time();
        state->p25_last_vc_tune_time = dsd_decode_time();
        state->last_vc_sync_time_m = dsd_decode_now_mono_s();
        state->p25_last_vc_tune_time_m = dsd_decode_now_mono_s();
    }
}

/* A retune accepted inside a burst ends the superframe's dispatch (issue #651): p25_p2_frame_reset() zeroes the loop's
   timeslot counter and the bit buffers, and the rest of the window used to replay as phantom 4V bursts (all-zero
   DUIDs decode as 4V), publishing voice activity against the new assignment. */
static int
test_retune_inside_dispatch_stops_the_window(void) {
    printf("Test 40: retune inside dispatch stops the window... ");
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_xcch_stubs();
    prepare_lcch_release_duids();
    state.p2_wacn = 1;
    state.p2_sysid = 1;
    state.p2_cc = 0x123;
    state.currentslot = 0;
    g_retune_hook_fired = 0;
    g_sacch_mac_hook = sacch_hook_retune_once;

    p25p2_process_duid(&opts, &state);

    int rc = 0;
    rc |= expect_int("retune one burst dispatched", g_sacch_mac_calls, 1);
    rc |= expect_int("retune no phantom 4V slot1", state.fourv_counter[0], 0);
    rc |= expect_int("retune no phantom 4V slot2", state.fourv_counter[1], 0);
    rc |= expect_int("retune no phantom voice time", state.last_vc_sync_time != 0, 0);
    printf(rc == 0 ? "PASS\n" : "FAIL\n");
    g_sacch_mac_hook = NULL;
    dsd_state_ext_free_all(&state);
    return rc;
}

/* An aged LCCH whose own MAC PDU accepted a retune or an assignment on the tuned carrier must not then tear down and
   release on the verdict computed before the burst (issue #651). */
static int
test_lcch_timeout_reevaluated_after_its_mac_pdu(void) {
    printf("Test 41: LCCH timeout re-evaluated after its MAC PDU... ");
    int rc = 0;
    for (int variant = 0; variant < 2; variant++) {
        static dsd_opts opts;
        static dsd_state state;
        DSD_MEMSET(&opts, 0, sizeof(opts));
        DSD_MEMSET(&state, 0, sizeof(state));
        reset_xcch_stubs();
        reset_playback_stub();
        prepare_lcch_release_duids();

        time_t now = time(NULL);
        opts.floating_point = 0;
        opts.pulse_digi_rate_out = 8000;
        opts.trunk_enable = 1;
        opts.trunk_is_tuned = 1;
        opts.trunk_hangtime = 1;
        state.currentslot = 0;
        seed_p25p2_call(&state, 0U, 4100U, 5100U, 0x00U, 0U, 0U);
        state.last_vc_sync_time = now - 10;
        state.p25_last_vc_tune_time = now - 10;
        set_p25_vc_grace("0.25");
        g_retune_hook_fired = 0;
        g_sacch_mac_hook = (variant == 0) ? sacch_hook_retune_once : sacch_hook_reused_carrier_grant_once;

        p25p2_process_duid(&opts, &state);
        set_p25_vc_grace(NULL);

        const char* const tags[2][3] = {
            {"retune no release force", "retune call kept", "retune not released"},
            {"reused grant no release force", "reused grant call kept", "reused grant not released"}};
        rc |= expect_int(tags[variant][0], state.p25_sm_force_release, 0);
        rc |= expect_call_state(tags[variant][1], &state, 0U, DSD_CALL_PHASE_ACTIVE, 4100U, 5100U, 0U, 0U);
        rc |= expect_int(tags[variant][2], opts.trunk_is_tuned, 1);
        g_sacch_mac_hook = NULL;
        dsd_state_ext_free_all(&state);
    }
    printf(rc == 0 ? "PASS\n" : "FAIL\n");
    return rc;
}

/* Issue #651: where a window sits in the superframe follows from the symbol count since the previous window on the
   carrier. A window whose ISCH failed descrambles at the timeslot continuity proves, not at the previous window's
   offset; whole timeslots a missed sync skipped advance it; a displacement no whole number of timeslots explains
   proves nothing, and a window nothing proves dispatches no slot-attributed burst until an ISCH locates one again. */
static int
test_window_position_follows_the_symbol_count(void) {
    printf("Test 42: window position follows the symbol count between windows... ");
    static dsd_opts opts;
    static dsd_state state;
    static const uint8_t scrambled_facch[4] = {0x9AU, 0x9AU, 0x9AU, 0x9AU};
    int rc = 0;

    run_fed_superframe(&opts, &state, scrambled_facch, -1);
    rc |= expect_int("located window starts the superframe", state.p2_scramble_offset % 12, 0);
    rc |= expect_int("located window dispatches its FACCHs", g_facch_mac_calls, 4);

    reset_xcch_stubs();
    read_fed_window(&opts, &state, 20U, 0);
    rc |= expect_int("contiguous window without ISCH sits 4 timeslots on", state.p2_scramble_offset, 4);
    rc |= expect_int("contiguous window dispatches its FACCHs", g_facch_mac_calls, 4);

    reset_xcch_stubs();
    read_fed_window(&opts, &state, 20U + 720U, 0);
    rc |= expect_int("a missed window skips 4 timeslots", state.p2_scramble_offset, 0);
    rc |= expect_int("after a missed window the FACCHs dispatch", g_facch_mac_calls, 4);

    reset_xcch_stubs();
    read_fed_window(&opts, &state, 20U + 180U, 0);
    rc |= expect_int("reacquired a timeslot late: odd start", state.p2_scramble_offset, 5);
    rc |= expect_int("odd start begins on slot 2", (int)state.currentslot, 1);
    rc |= expect_int("odd start dispatches its FACCHs", g_facch_mac_calls, 4);

    reset_xcch_stubs();
    read_fed_window(&opts, &state, 20U + 7U, 0);
    rc |= expect_int("unexplained displacement dispatches no FACCH", g_facch_mac_calls, 0);
    reset_xcch_stubs();
    read_fed_window(&opts, &state, 20U, 0);
    rc |= expect_int("nothing to measure from after it", g_facch_mac_calls, 0);
    reset_xcch_stubs();
    read_fed_window(&opts, &state, 20U, 1);
    rc |= expect_int("an ISCH locates the window again", g_facch_mac_calls, 4);

    reset_xcch_stubs();
    printf(rc == 0 ? "PASS\n" : "FAIL\n");
    return rc;
}

/* Issue #651: the timeslots of a window a missed sync skipped retire as lost, so an open voice stream fills each voice
   burst it missed in place, and the pairs they complete play: here slot 1's pairs 2 and 3 (4 frames each) and, in the
   window read, its pair 4 (its 2V, 2 frames) taken by a FACCH. */
static int
test_missed_window_fills_in_place(void) {
    printf("Test 43: a missed window's voice bursts fill in place... ");
    static dsd_opts opts;
    static dsd_state state;
    static const uint8_t scrambled_facch[4] = {0x9AU, 0x9AU, 0x9AU, 0x9AU};
    int rc = 0;

    run_fed_superframe(&opts, &state, scrambled_facch, -1);
    reset_playback_stub();
    enable_captured_output(&opts);
    seed_p25p2_call(&state, 0U, 4100U, 5100U, 0x00U, 0U, 0U);
    dsd_call_snapshot call;
    rc |= expect_int("missed window call", dsd_call_state_get(&state, 0U, &call) > 0, 1);
    state.p25_p2_audio_allowed[0] = 1;
    state.p25_crypto_state[0] = DSD_P25_CRYPTO_CLEAR;
    dsd_p25p2_playout_slot* stream = &state.p25p2_playout.slot[0];
    stream->open = 1U;
    stream->epoch = call.epoch;
    stream->phase_2v = 4;
    stream->phase_proven = 1U;
    stream->provisional = -1;
    stream->verdict = (dsd_p25p2_playout_verdict){0U, 0U, 1U, 1U};
    stream->cur_pair = 1;
    stream->cur_pair_done = 1U;
    state.p25p2_playout.slot[1].cur_pair = -1;
    state.p25p2_playout.slot[1].phase_2v = -1;
    state.p25p2_playout.slot[1].provisional = -1;

    reset_xcch_stubs();
    read_fed_window(&opts, &state, 20U + 720U, 0);
    rc |= expect_int("missed window plays its lost bursts as silence", g_out_calls, 4 + 4 + 2);
    rc |= expect_int("missed window silence", g_out_first_left, 0);
    rc |= expect_int("missed window stream stays open", stream->open, 1);

    dsd_udp_audio_hooks_set((dsd_udp_audio_hooks){0});
    reset_xcch_stubs();
    dsd_state_ext_free_all(&state);
    printf(rc == 0 ? "PASS\n" : "FAIL\n");
    return rc;
}

/* Issue #651: a window nothing locates still dispatches its LCCH (unscrambled, it decodes without the offset), but no
   MAC message assembled across windows may complete in it: the slot it would complete against is a guess. */
static int
test_unproven_window_keeps_lcch_drops_assemblies(void) {
    printf("Test 44: an unproven window keeps its LCCH and drops pending assemblies... ");
    static dsd_opts opts;
    static dsd_state state;
    static const uint8_t lcch[4] = {0xD1U, 0xD1U, 0xD1U, 0xD1U};
    int rc = 0;

    run_fed_superframe(&opts, &state, lcch, -1);
    const int located_calls = g_sacch_mac_calls;
    rc |= expect_int("located LCCH window dispatches", located_calls > 0, 1);
    state.p25_mac_frag[0].active = 1U;
    state.p25_mac_frag[0].collected = 7U;
    state.p25_mac_frag[1].active = 1U;
    state.p25_mac_frag[1].collected = 9U;

    reset_xcch_stubs();
    read_fed_window(&opts, &state, 20U + 7U, 0);
    rc |= expect_int("unproven window still dispatches its LCCH", g_sacch_mac_calls, located_calls);
    rc |= expect_int("unproven window drops slot 1's assembly", state.p25_mac_frag[0].active, 0);
    rc |= expect_int("unproven window drops slot 2's assembly", state.p25_mac_frag[1].active, 0);

    reset_xcch_stubs();
    printf(rc == 0 ? "PASS\n" : "FAIL\n");
    return rc;
}

/* Issue #651: an LCCH timeout releases to the control channel before it tears the call down. A return the tuner
   refuses keeps the voice channel, and with it the slots' gates, crypto, calls and voice streams; the release stays
   latched for the next tick. */
static int
test_lcch_timeout_refused_return_keeps_the_channel(void) {
    printf("Test 45: LCCH timeout with a refused return keeps the voice channel... ");
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_xcch_stubs();
    reset_playback_stub();
    prepare_lcch_release_duids();

    time_t now = time(NULL);
    opts.floating_point = 0;
    opts.pulse_digi_rate_out = 8000;
    opts.trunk_enable = 1;
    opts.trunk_is_tuned = 1;
    opts.trunk_hangtime = 1;
    state.currentslot = 0;
    seed_p25p2_call(&state, 0U, 4100U, 5100U, 0x00U, 0U, 0U);
    state.p25_p2_audio_allowed[0] = 1;
    state.p25_crypto_state[0] = DSD_P25_CRYPTO_CLEAR;
    dsd_p25p2_playout_reset(&state, -1);
    dsd_p25p2_playout_stage(&opts, &state, 0, 0, 1U, NULL);
    state.last_vc_sync_time = now - 10;
    state.p25_last_vc_tune_time = now - 10;
    set_p25_vc_grace("0.25");
    p25_sm_ctx_t* sm = p25_sm_get_ctx();
    p25_sm_init_ctx(sm, &opts, &state);
    sm->state = P25_SM_TUNED;
    sm->vc_is_tdma = 1;
    sm->vc_freq_hz = 852000000;
    state.p25_cc_freq = 851000000;
    g_return_result = DSD_TRUNK_TUNE_RESULT_FAILED;

    p25p2_process_duid(&opts, &state);
    g_return_result = DSD_TRUNK_TUNE_RESULT_OK;
    set_p25_vc_grace(NULL);

    int rc = 0;
    rc |= expect_int("refused return keeps the channel tuned", opts.trunk_is_tuned, 1);
    rc |= expect_int("refused return stays latched", state.p25_sm_force_release, 1);
    rc |= expect_int("refused return keeps the gate", state.p25_p2_audio_allowed[0], 1);
    rc |= expect_int("refused return keeps the crypto", state.p25_crypto_state[0], DSD_P25_CRYPTO_CLEAR);
    rc |= expect_int("refused return keeps the voice stream", state.p25p2_playout.slot[0].open, 1);
    rc |= expect_call_state("refused return keeps the call", &state, 0U, DSD_CALL_PHASE_ACTIVE, 4100U, 5100U, 0U, 0U);
    p25_sm_init_ctx(sm, &opts, &state);
    reset_xcch_stubs();
    printf(rc == 0 ? "PASS\n" : "FAIL\n");
    dsd_state_ext_free_all(&state);
    return rc;
}

/* A tuned TDMA voice channel whose @p slot carries a call that claims encrypted service, assigned in the state machine,
   with a rekey staged to an algorithm it holds no key for: once promoted, encryption lockout releases the channel, the
   companion slot being idle. */
static void
arm_lockout_release(dsd_opts* opts, dsd_state* state, int slot) {
    opts->trunk_enable = 1;
    opts->trunk_is_tuned = 1;
    opts->trunk_tune_enc_calls = 0;
    opts->trunk_tune_group_calls = 1;
    state->lastsynctype = DSD_SYNC_P25P2_POS;
    state->p25_cc_freq = 851000000;
    seed_p25p2_call(state, (uint8_t)slot, 5678U, 9000U, 0x40U, 0U, 0U);
    p25_sm_ctx_t* sm = p25_sm_get_ctx();
    p25_sm_init_ctx(sm, opts, state);
    sm->state = P25_SM_TUNED;
    sm->vc_is_tdma = 1;
    sm->vc_freq_hz = 852000000;
    sm->config.hangtime_s = 2.0;
    sm->t_tune_m = 1.0;
    sm->t_voice_m = 1.0;
    sm->slots[slot].grant_active = 1;
    sm->slots[slot].freq_hz = sm->vc_freq_hz;
    sm->slots[slot].channel = 0x2000 | slot;
    sm->slots[slot].target_id = 5678;
    sm->slots[slot].ota_tg = 5678;
    sm->slots[slot].tg = 5678;
    sm->slots[slot].is_group = 1;
    state->p25_p2_rekey[slot].pending = 1U;
    state->p25_p2_rekey[slot].algid = 0x84U;
    state->p25_p2_rekey[slot].keyid = 0x2710U;
    state->p25_p2_rekey[slot].mi = 0x1111222233334444ULL;
}

static int g_rekey_pending_at_first_facch = -1;

static void
facch_hook_note_rekey(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    if (g_rekey_pending_at_first_facch < 0) {
        g_rekey_pending_at_first_facch = state->p25_p2_rekey[0].pending;
    }
}

/* Issue #651: a pair that retiring the timeslots a missed sync skipped completes promotes a deferred rekey there, as a
   dispatched pair does, so the next window's first burst decodes under the new identity. A promotion that locks its
   call out and returns to the control channel ends the window: nothing of it dispatches on the channel left. */
static int
test_retired_pair_promotes_a_deferred_rekey(void) {
    printf("Test 46: a retired pair promotes a deferred rekey... ");
    static dsd_opts opts;
    static dsd_state state;
    static const uint8_t scrambled_facch[4] = {0x9AU, 0x9AU, 0x9AU, 0x9AU};
    int rc = 0;

    // Slot 1's last burst of an odd window staged a rekey to clear; slot 2's timeslot that completes the pair is missed.
    run_fed_superframe(&opts, &state, scrambled_facch, -1);
    reset_xcch_stubs();
    read_fed_window(&opts, &state, 20U + 180U, 0);
    rc |= expect_int("rekey window starts odd", state.p2_scramble_offset, 5);
    state.payload_algid = 0xAA;
    state.payload_keyid = 0x1234;
    state.p25_p2_rekey[0].pending = 1U;
    state.p25_p2_rekey[0].algid = 0x80U;
    state.p25_p2_rekey[0].keyid = 0U;
    state.p25_p2_rekey[0].mi = 0ULL;
    reset_xcch_stubs();
    g_rekey_pending_at_first_facch = -1;
    g_facch_mac_hook = facch_hook_note_rekey;
    read_fed_window(&opts, &state, 20U + 180U, 0);
    rc |= expect_int("the missed timeslot completes the pair", state.p2_scramble_offset, 10);
    rc |= expect_int("the rekey is promoted before the next burst", g_rekey_pending_at_first_facch, 0);
    rc |= expect_int("the promoted identity", state.payload_algid, 0x80);
    rc |= expect_int("the window dispatches", g_facch_mac_calls, 4);
    dsd_state_ext_free_all(&state);

    // Slot 2's staged rekey locks its call out; with slot 1 idle the channel is released to the control channel.
    run_fed_superframe(&opts, &state, scrambled_facch, -1);
    reset_xcch_stubs();
    read_fed_window(&opts, &state, 20U + 180U, 0);
    arm_lockout_release(&opts, &state, 1);
    reset_xcch_stubs();
    g_return_calls = 0;
    read_fed_window(&opts, &state, 20U + 180U, 0);
    rc |= expect_int("the lockout returns to the control channel", g_return_calls, 1);
    rc |= expect_int("nothing of the window dispatches after", g_facch_mac_calls, 0);

    p25_sm_init_ctx(p25_sm_get_ctx(), &opts, &state);
    reset_xcch_stubs();
    dsd_state_ext_free_all(&state);
    printf(rc == 0 ? "PASS\n" : "FAIL\n");
    return rc;
}

/* Slot 1's rekey to clear staged by the last burst of an odd window read after a fed superframe (issue #651). */
static void
stage_rekey_after_an_odd_window(dsd_opts* opts, dsd_state* state, const uint8_t duids[4]) {
    run_fed_superframe(opts, state, duids, -1);
    reset_xcch_stubs();
    read_fed_window(opts, state, 20U + 180U, 0);
    state->payload_algid = 0xAA;
    state->payload_keyid = 0x1234;
    state->p25_p2_rekey[0].pending = 1U;
    state->p25_p2_rekey[0].algid = 0x80U;
    state->p25_p2_rekey[0].keyid = 0U;
    state->p25_p2_rekey[0].mi = 0ULL;
    reset_xcch_stubs();
    g_rekey_pending_at_first_facch = -1;
    g_facch_mac_hook = facch_hook_note_rekey;
}

/* Issue #651: a window that breaks continuity (a displacement no whole number of timeslots explains) leaves the pair a
   deferred rekey waits for unfinished: the rekey is promoted before the window's first burst decodes, whether an ISCH
   locates that window or only a later one, and the playout's timeline breaks there. */
static int
test_broken_continuity_promotes_a_deferred_rekey(void) {
    printf("Test 47: a break in continuity promotes a deferred rekey... ");
    static dsd_opts opts;
    static dsd_state state;
    static const uint8_t scrambled_facch[4] = {0x9AU, 0x9AU, 0x9AU, 0x9AU};
    int rc = 0;

    stage_rekey_after_an_odd_window(&opts, &state, scrambled_facch);
    const uint32_t clock_before = state.p25p2_playout.pair_clock;
    read_fed_window(&opts, &state, 20U + 7U, 1);
    rc |= expect_int("the located window dispatches", g_facch_mac_calls, 4);
    // Its two pairs, after the break the playout marks: what it queues starts a later pair than any tail before it.
    rc |= expect_int("the break starts a later pair", (int)(state.p25p2_playout.pair_clock - clock_before), 3);
    rc |= expect_int("its first burst decodes under the new identity", g_rekey_pending_at_first_facch, 0);
    rc |= expect_int("the promoted identity", state.payload_algid, 0x80);
    dsd_state_ext_free_all(&state);

    stage_rekey_after_an_odd_window(&opts, &state, scrambled_facch);
    read_fed_window(&opts, &state, 20U + 7U, 0);
    rc |= expect_int("the unproven window dispatches no FACCH", g_facch_mac_calls, 0);
    read_fed_window(&opts, &state, 20U, 1);
    rc |= expect_int("the next located window dispatches", g_facch_mac_calls, 4);
    rc |= expect_int("its first burst decodes under the new identity", g_rekey_pending_at_first_facch, 0);

    reset_xcch_stubs();
    dsd_state_ext_free_all(&state);
    printf(rc == 0 ? "PASS\n" : "FAIL\n");
    return rc;
}

int
main(void) {
    int failures = 0;
    install_trunk_tuning_hooks();
    reset_ess_stubs();
    set_p25p2_threshold(64);

    printf("P25P2 Frame and Soft-Decision Tests\n");
    printf("===================================\n\n");

    /* Test 1: Reset clears soft-decision buffers */
    printf("Test 1: Reset clears soft-decision buffers... ");
    for (int i = 0; i < 1400; i++) {
        p2llr[i] = 123;
        p2xllr[i] = -123;
    }
    p25_p2_frame_reset();

    int non_zero = 0;
    for (int i = 0; i < 1400; i++) {
        if (p2llr[i] != 0) {
            non_zero++;
        }
        if (p2xllr[i] != 0) {
            non_zero++;
        }
    }
    if (non_zero == 0) {
        printf("PASS\n");
    } else {
        printf("FAIL (%d non-zero entries)\n", non_zero);
        failures++;
    }

    /* Test 2: P2 DUID soft fallback only recovers invalid hard decisions */
    printf("Test 2: DUID soft fallback preserves valid hard decisions... ");
    uint8_t duid_reliab[8] = {200, 200, 200, 200, 200, 200, 200, 5};
    int valid_decoded = p25p2_duid_lookup_soft(0x07U, duid_reliab);
    if (valid_decoded == 1) {
        printf("PASS\n");
    } else {
        printf("FAIL (expected 1, got %d)\n", valid_decoded);
        failures++;
    }

    printf("Test 3: DUID soft fallback uses weakest invalid bits... ");
    DSD_MEMSET(duid_reliab, 200, sizeof(duid_reliab));
    duid_reliab[6] = 5;
    duid_reliab[7] = 5; /* 0x03 -> 0x00 is the cheapest canonical candidate. */
    int recovered_decoded = p25p2_duid_lookup_soft(0x03U, duid_reliab);
    if (recovered_decoded == 0) {
        printf("PASS\n");
    } else {
        printf("FAIL (expected 0, got %d)\n", recovered_decoded);
        failures++;
    }

    printf("Test 4: DUID soft fallback rejects high-confidence invalid bits... ");
    DSD_MEMSET(duid_reliab, 200, sizeof(duid_reliab));
    int high_confidence_decoded = p25p2_duid_lookup_soft(0x03U, duid_reliab);
    if (high_confidence_decoded == -1) {
        printf("PASS\n");
    } else {
        printf("FAIL (expected -1, got %d)\n", high_confidence_decoded);
        failures++;
    }

    printf("Test 5: DUID soft fallback recovers weak 0x80 MSB... ");
    DSD_MEMSET(duid_reliab, 200, sizeof(duid_reliab));
    duid_reliab[0] = 5;
    int sentinel_decoded = p25p2_duid_lookup_soft(0x80U, duid_reliab);
    if (sentinel_decoded == 0) {
        printf("PASS\n");
    } else {
        printf("FAIL (expected 0, got %d)\n", sentinel_decoded);
        failures++;
    }

    printf("Test 6: DUID soft fallback preserves high-confidence 0x80 guard... ");
    DSD_MEMSET(duid_reliab, 200, sizeof(duid_reliab));
    sentinel_decoded = p25p2_duid_lookup_soft(0x80U, duid_reliab);
    if (sentinel_decoded == -1) {
        printf("PASS\n");
    } else {
        printf("FAIL (expected -1, got %d)\n", sentinel_decoded);
        failures++;
    }

    printf("Test 7: DUID soft fallback rejects weak non-MSB 0x80 guard... ");
    DSD_MEMSET(duid_reliab, 200, sizeof(duid_reliab));
    duid_reliab[0] = 5;
    duid_reliab[1] = 5;
    sentinel_decoded = p25p2_duid_lookup_soft(0x80U, duid_reliab);
    if (sentinel_decoded == -1) {
        printf("PASS\n");
    } else {
        printf("FAIL (expected -1, got %d)\n", sentinel_decoded);
        failures++;
    }

    printf("Test 8: DUID soft fallback rejects tied frame candidates... ");
    DSD_MEMSET(duid_reliab, 200, sizeof(duid_reliab));
    duid_reliab[3] = 5; /* 0x03 -> 0x17 decodes to 1. */
    duid_reliab[5] = 5;
    duid_reliab[6] = 5; /* 0x03 -> 0x00 decodes to 0. */
    duid_reliab[7] = 5;
    int tied_decoded = p25p2_duid_lookup_soft(0x03U, duid_reliab);
    if (tied_decoded == -1) {
        printf("PASS\n");
    } else {
        printf("FAIL (expected -1, got %d)\n", tied_decoded);
        failures++;
    }

    failures += test_ess_soft_accepts_deep_erasure();
    failures += test_ess_soft_failure_counts_once();
    failures += test_ess_des_manual_key_preserves_audio_gate();
    failures += test_ess_aes_slot1_loaded_key_preserves_audio_gate();
    failures += test_ess_opens_audio_gate_for_every_burst_hint();
    failures += test_ess_allow_list_blocks_clear_audio_gate();
    failures += test_ess_decode_failure_refreshes_existing_crypto_state();
    failures += test_ess_decode_failure_refreshes_existing_aes_state();
    failures += test_facchc_success_routes_opcode_and_counters();
    failures += test_facchc_failure_preserves_mac_and_counts_error();
    failures += test_sacchc_dynamic_erasure_success_maps_inverse_slot();
    failures += test_isch_channel_one_location_sets_scramble_offset();
    failures += test_duid_exact_or_null_soft_metrics_preserve_hard_decision();
    failures += test_isch_reliability_clamps_llr_magnitude();
    failures += test_teardown_flushes_partial_int16_audio_and_resets_call_state();
    failures += test_teardown_without_partial_int16_audio_skips_playback_but_clears_state();
    failures += test_duid_invalid_burst_aborts_without_reopening_crypto();
    failures += test_duid_abort_resolves_staged_rekey();
    failures += test_duid_lcch_release_defers_during_vc_grace();
    failures += test_duid_lcch_release_tears_down_after_vc_grace();
    failures += test_retune_inside_dispatch_stops_the_window();
    failures += test_lcch_timeout_reevaluated_after_its_mac_pdu();
    failures += test_seed_proof_needs_a_descrambled_burst();
    failures += test_superframe_split_by_a_carrier_boundary_is_dropped();
    failures += test_seed_proof_reaches_the_call_its_burst_ends();
    failures += test_window_position_follows_the_symbol_count();
    failures += test_missed_window_fills_in_place();
    failures += test_unproven_window_keeps_lcch_drops_assemblies();
    failures += test_lcch_timeout_refused_return_keeps_the_channel();
    failures += test_retired_pair_promotes_a_deferred_rekey();
    failures += test_broken_continuity_promotes_a_deferred_rekey();

    printf("\n%d test(s) failed\n", failures);
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
    return failures > 0 ? 1 : 0;
}
#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
