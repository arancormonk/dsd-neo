// SPDX-License-Identifier: ISC
// Coverage fixtures intentionally use private-source inclusion, synthetic sentinels,
// invalid-value negative vectors, or wrapper symbols to exercise guarded behavior.
// NOLINTBEGIN(bugprone-suspicious-include)
/*
 * Focused P25 Phase 2 XCCH tests for MAC PTT/END/IDLE dispatch and slot state.
 */

#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/protocol/p25/p25_crc.h>
#include <dsd-neo/protocol/p25/p25_crypto.h>
#include <dsd-neo/protocol/p25/p25_lfsr.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/protocol/p25/p25_vpdu.h>
#include <dsd-neo/runtime/p25_p2_audio_ring.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int g_audio_allow;
static int g_crc12_result;
static int g_crc16_result;
static int g_vpdu_count;
static int g_vpdu_type;
static int g_vpdu_entry_lasttg[2];
static int g_vpdu_entry_lastsrc[2];
static int g_vpdu_grant_newer_slot;
static int g_vpdu_enc_pending_slot;
static unsigned long long int g_vpdu_mac[24];
static int g_ptt_count[2];
static int g_active_count[2];
static int g_end_count[2];
static int g_idle_count[2];
static int g_enc_count[2];
static int g_enc_algid[2];
static int g_enc_keyid[2];
static int g_enc_tg[2];
static int g_close_l_count;
static int g_close_r_count;
static int g_flush_count;
static int g_flush_burst_l;
static int g_flush_burst_r;
static int g_flush_gate_l;
static int g_flush_gate_r;
static int g_flush_slot;
static int g_flush_crypto_state;
static int g_flush_close_l_count;
static int g_flush_close_r_count;
static int g_ring_reset_count[2];
static int g_lfsr_count[2];
static int g_slot_grant_newer[2];
static uint64_t g_now_ns;

uint64_t
dsd_time_monotonic_ns(void) {
    g_now_ns += 1000000000ULL;
    return g_now_ns;
}

int
crc12_xb_bridge(const int* payload, int len) {
    (void)payload;
    (void)len;
    return g_crc12_result;
}

int
crc16_lb_bridge(const int* payload, int len) {
    (void)payload;
    (void)len;
    return g_crc16_result;
}

void
process_MAC_VPDU(dsd_opts* opts, dsd_state* state, int type, unsigned long long int mac[24]) {
    (void)opts;
    g_vpdu_count++;
    g_vpdu_type = type;
    if (state) {
        g_vpdu_entry_lasttg[0] = state->lasttg;
        g_vpdu_entry_lasttg[1] = state->lasttgR;
        g_vpdu_entry_lastsrc[0] = state->lastsrc;
        g_vpdu_entry_lastsrc[1] = state->lastsrcR;
    }
    for (int i = 0; i < 24; i++) {
        g_vpdu_mac[i] = mac[i];
    }
    if (g_vpdu_grant_newer_slot >= 0 && g_vpdu_grant_newer_slot <= 1) {
        g_slot_grant_newer[g_vpdu_grant_newer_slot] = 1;
    }
    if (state && g_vpdu_enc_pending_slot >= 0 && g_vpdu_enc_pending_slot <= 1) {
        const int slot = g_vpdu_enc_pending_slot;
        state->p25_crypto_state[slot] = DSD_P25_CRYPTO_ENCRYPTED_PENDING;
        state->p25_p2_enc_lockout_muted[slot] = 1U;
        state->p25_p2_audio_allowed[slot] = 0;
        p25_p2_audio_ring_reset(state, slot);
    }
}

int
dsd_p25p2_decode_audio_allowed(const dsd_opts* opts, const dsd_state* state, int slot, int alg) {
    (void)opts;
    (void)state;
    (void)slot;
    (void)alg;
    return g_audio_allow;
}

void
p25_p2_audio_ring_reset(dsd_state* state, int slot) {
    if (slot >= 0 && slot <= 1) {
        state->p25_p2_audio_ring_count[slot] = 0;
        g_ring_reset_count[slot]++;
        return;
    }

    state->p25_p2_audio_ring_count[0] = 0;
    state->p25_p2_audio_ring_count[1] = 0;
    g_ring_reset_count[0]++;
    g_ring_reset_count[1]++;
}

void
p25_lfsr128_slot(dsd_state* state, int slot) {
    (void)state;
    if (slot >= 0 && slot <= 1) {
        g_lfsr_count[slot]++;
    }
}

void
p25_sm_emit_ptt(dsd_opts* opts, dsd_state* state, int slot) {
    (void)opts;
    (void)state;
    if (slot >= 0 && slot <= 1) {
        g_ptt_count[slot]++;
    }
}

void
p25_sm_emit_active(dsd_opts* opts, dsd_state* state, int slot) {
    (void)opts;
    (void)state;
    if (slot >= 0 && slot <= 1) {
        g_active_count[slot]++;
    }
}

void
p25_sm_emit_end(dsd_opts* opts, dsd_state* state, int slot) {
    (void)opts;
    (void)state;
    if (slot >= 0 && slot <= 1) {
        g_end_count[slot]++;
    }
}

void
p25_sm_emit_idle(dsd_opts* opts, dsd_state* state, int slot) {
    (void)opts;
    (void)state;
    if (slot >= 0 && slot <= 1) {
        g_idle_count[slot]++;
    }
}

void
p25_sm_emit_idle_at(dsd_opts* opts, dsd_state* state, int slot, double observed_m) {
    (void)observed_m;
    p25_sm_emit_idle(opts, state, slot);
}

int
p25_sm_slot_grant_newer_than(int slot, double observed_m) {
    (void)observed_m;
    if (slot < 0 || slot > 1) {
        return 0;
    }
    return g_slot_grant_newer[slot] ? 1 : 0;
}

void
p25_sm_emit_enc(dsd_opts* opts, dsd_state* state, int slot, int algid, int keyid, int tg) {
    (void)opts;
    (void)state;
    if (slot >= 0 && slot <= 1) {
        g_enc_count[slot]++;
        g_enc_algid[slot] = algid;
        g_enc_keyid[slot] = keyid;
        g_enc_tg[slot] = tg;
    }
}

dsd_p25_crypto_state
p25_crypto_resolve(dsd_opts* opts, dsd_state* state, dsd_p25_crypto_phase phase, int slot, int algid, int keyid,
                   uint64_t mi, int talkgroup) {
    (void)phase;
    if (!state || slot < 0 || slot > 1) {
        return DSD_P25_CRYPTO_UNKNOWN;
    }
    if (algid == 0) {
        return state->p25_crypto_state[slot];
    }

    if (slot == 0) {
        state->payload_algid = algid;
        state->payload_keyid = keyid;
        state->payload_miP = mi;
    } else {
        state->payload_algidR = algid;
        state->payload_keyidR = keyid;
        state->payload_miN = mi;
    }

    const unsigned long long scalar_key = (slot == 0) ? state->R : state->RR;
    const int scalar_ready = (algid == 0xAA || algid == 0x81) && scalar_key != 0ULL;
    const int aes_ready = state->aes_key_loaded[slot] == 1
                          && ((algid == 0x89 && state->aes_key_segments[slot] >= 2U)
                              || (algid == 0x84 && state->aes_key_segments[slot] >= 4U));
    const dsd_p25_crypto_state resolved =
        (algid == 0x80) ? DSD_P25_CRYPTO_CLEAR
                        : ((scalar_ready || aes_ready) ? DSD_P25_CRYPTO_DECRYPTABLE : DSD_P25_CRYPTO_BLOCKED);
    state->p25_crypto_state[slot] = resolved;
    state->p25_p2_enc_lockout_muted[slot] = (resolved == DSD_P25_CRYPTO_BLOCKED) ? 1U : 0U;
    if (!p25_crypto_audio_ready(state, slot)) {
        state->p25_p2_audio_allowed[slot] = 0;
    }
    if (algid != 0x80) {
        p25_sm_emit_enc(opts, state, slot, algid, keyid, talkgroup);
    }
    return resolved;
}

void
p25_crypto_reset_slot(dsd_state* state, int slot) {
    if (!state || slot < 0 || slot > 1) {
        return;
    }
    if (slot == 0) {
        state->payload_algid = 0;
        state->payload_keyid = 0;
        state->payload_miP = 0ULL;
    } else {
        state->payload_algidR = 0;
        state->payload_keyidR = 0;
        state->payload_miN = 0ULL;
    }
    state->p25_crypto_state[slot] = DSD_P25_CRYPTO_UNKNOWN;
    state->p25_p2_audio_allowed[slot] = 0;
    state->p25_p2_enc_lockout_muted[slot] = 0U;
    if (state->keyloader == 1) {
        if (slot == 0) {
            state->R = 0ULL;
        } else {
            state->RR = 0ULL;
        }
        state->A1[slot] = 0ULL;
        state->A2[slot] = 0ULL;
        state->A3[slot] = 0ULL;
        state->A4[slot] = 0ULL;
        state->aes_key_loaded[slot] = 0;
        state->aes_key_segments[slot] = 0U;
    }
}

void
closeMbeOutFile(dsd_opts* opts, dsd_state* state) {
    (void)state;
    g_close_l_count++;
    opts->mbe_out_f = NULL;
}

void
closeMbeOutFileR(dsd_opts* opts, dsd_state* state) {
    (void)state;
    g_close_r_count++;
    opts->mbe_out_fR = NULL;
}

void
dsd_p25p2_flush_partial_audio(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    g_flush_count++;

    if (!state) {
        return;
    }

    g_flush_burst_l = (int)state->dmrburstL;
    g_flush_burst_r = (int)state->dmrburstR;
    g_flush_gate_l = state->p25_p2_audio_allowed[0];
    g_flush_gate_r = state->p25_p2_audio_allowed[1];
    g_flush_close_l_count = g_close_l_count;
    g_flush_close_r_count = g_close_r_count;

    state->p25_p2_audio_allowed[0] = 0;
    state->p25_p2_audio_allowed[1] = 0;
    state->voice_counter[0] = 0;
    state->voice_counter[1] = 0;
    DSD_MEMSET(state->s_l4, 0, sizeof(state->s_l4));
    DSD_MEMSET(state->s_r4, 0, sizeof(state->s_r4));
}

void
dsd_p25p2_flush_partial_audio_slot(dsd_opts* opts, dsd_state* state, int slot) {
    (void)opts;
    g_flush_count++;
    g_flush_slot = slot;

    if (!state || slot < 0 || slot > 1) {
        return;
    }

    g_flush_burst_l = (int)state->dmrburstL;
    g_flush_burst_r = (int)state->dmrburstR;
    g_flush_gate_l = state->p25_p2_audio_allowed[0];
    g_flush_gate_r = state->p25_p2_audio_allowed[1];
    g_flush_crypto_state = state->p25_crypto_state[slot];
    g_flush_close_l_count = g_close_l_count;
    g_flush_close_r_count = g_close_r_count;

    state->voice_counter[slot] = 0;
    if (slot == 0) {
        DSD_MEMSET(state->s_l4, 0, sizeof(state->s_l4));
    } else {
        DSD_MEMSET(state->s_r4, 0, sizeof(state->s_r4));
    }
}

#include "../../../src/protocol/p25/phase2/p25p2_xcch.c"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

static void
reset_stubs(void) {
    g_audio_allow = 1;
    g_crc12_result = 0;
    g_crc16_result = 0;
    g_vpdu_count = 0;
    g_vpdu_type = -1;
    g_vpdu_entry_lasttg[0] = -1;
    g_vpdu_entry_lasttg[1] = -1;
    g_vpdu_entry_lastsrc[0] = -1;
    g_vpdu_entry_lastsrc[1] = -1;
    g_vpdu_grant_newer_slot = -1;
    g_vpdu_enc_pending_slot = -1;
    DSD_MEMSET(g_vpdu_mac, 0, sizeof(g_vpdu_mac));
    DSD_MEMSET(g_ptt_count, 0, sizeof(g_ptt_count));
    DSD_MEMSET(g_active_count, 0, sizeof(g_active_count));
    DSD_MEMSET(g_end_count, 0, sizeof(g_end_count));
    DSD_MEMSET(g_idle_count, 0, sizeof(g_idle_count));
    DSD_MEMSET(g_enc_count, 0, sizeof(g_enc_count));
    DSD_MEMSET(g_enc_algid, 0, sizeof(g_enc_algid));
    DSD_MEMSET(g_enc_keyid, 0, sizeof(g_enc_keyid));
    DSD_MEMSET(g_enc_tg, 0, sizeof(g_enc_tg));
    g_close_l_count = 0;
    g_close_r_count = 0;
    g_flush_count = 0;
    g_flush_burst_l = -1;
    g_flush_burst_r = -1;
    g_flush_gate_l = -1;
    g_flush_gate_r = -1;
    g_flush_slot = -1;
    g_flush_crypto_state = -1;
    g_flush_close_l_count = -1;
    g_flush_close_r_count = -1;
    DSD_MEMSET(g_ring_reset_count, 0, sizeof(g_ring_reset_count));
    DSD_MEMSET(g_lfsr_count, 0, sizeof(g_lfsr_count));
    DSD_MEMSET(g_slot_grant_newer, 0, sizeof(g_slot_grant_newer));
    g_now_ns = 0;
}

static int
expect_int(const char* label, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "FAIL: %s got %d want %d\n", label, got, want);
        return 1;
    }
    return 0;
}

static int
expect_u64(const char* label, unsigned long long int got, unsigned long long int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "FAIL: %s got 0x%016llX want 0x%016llX\n", label, got, want);
        return 1;
    }
    return 0;
}

static void
fill_mac(unsigned long long int mac[24], int algid, int keyid, int src, int tg) {
    DSD_MEMSET(mac, 0, sizeof(unsigned long long int) * 24U);
    mac[1] = 0x11;
    mac[2] = 0x22;
    mac[3] = 0x33;
    mac[4] = 0x44;
    mac[5] = 0x55;
    mac[6] = 0x66;
    mac[7] = 0x77;
    mac[8] = 0x88;
    mac[10] = (unsigned long long int)algid;
    mac[11] = (unsigned long long int)((keyid >> 8) & 0xFF);
    mac[12] = (unsigned long long int)(keyid & 0xFF);
    mac[13] = (unsigned long long int)((src >> 16) & 0xFF);
    mac[14] = (unsigned long long int)((src >> 8) & 0xFF);
    mac[15] = (unsigned long long int)(src & 0xFF);
    mac[16] = (unsigned long long int)((tg >> 8) & 0xFF);
    mac[17] = (unsigned long long int)(tg & 0xFF);
}

static void
pack_payload_from_mac(int* payload, int bit_count, const unsigned long long int mac[24], int opcode, int mac_offset,
                      int res) {
    int k = 0;
    int bytes = bit_count / 8;

    for (int i = 0; i < bit_count; i++) {
        payload[i] = 0;
    }

    for (int j = 0; j < bytes; j++) {
        unsigned long long int byte = mac[j] & 0xFFULL;
        for (int i = 7; i >= 0; i--) {
            payload[k++] = (int)((byte >> i) & 1ULL);
        }
    }

    payload[0] = (opcode >> 2) & 1;
    payload[1] = (opcode >> 1) & 1;
    payload[2] = opcode & 1;
    payload[3] = (mac_offset >> 2) & 1;
    payload[4] = (mac_offset >> 1) & 1;
    payload[5] = mac_offset & 1;
    payload[6] = (res >> 1) & 1;
    payload[7] = res & 1;
}

static int
test_slot_ptt_and_end_helpers(void) {
    static dsd_opts opts;
    static dsd_state state;
    unsigned long long int mac[24];
    int rc = 0;

    reset_stubs();
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.floating_point = 1;
    opts.audio_gain = 3;
    state.aout_gain = 0.5F;
    state.aout_gainR = 0.25F;
    state.p25_p2_enc_lockout_muted[0] = 1;
    state.fourv_counter[0] = 9;
    state.voice_counter[0] = 7;
    state.p25_policy_tg[0] = 0x5678;
    state.A1[0] = 1;
    state.A2[0] = 2;
    state.A3[0] = 3;
    state.A4[0] = 4;
    state.aes_key_loaded[0] = 1;
    state.aes_key_segments[0] = 4U;
    fill_mac(mac, 0x84, 0x2468, 0x010203, 0x1234);

    p25p2_xcch_handle_ptt_slot(&opts, &state, mac, 0, 0);
    rc |= expect_int("slot0 lastsrc", state.lastsrc, 0x010203);
    rc |= expect_int("slot0 lasttg", state.lasttg, 0x1234);
    rc |= expect_int("slot0 policy tg preserved", (int)state.p25_policy_tg[0], 0x5678);
    rc |= expect_int("slot0 algid", state.payload_algid, 0x84);
    rc |= expect_int("slot0 keyid", state.payload_keyid, 0x2468);
    rc |= expect_u64("slot0 mi", state.payload_miP, 0x1122334455667788ULL);
    rc |= expect_int("slot0 drop", state.dropL, 256);
    rc |= expect_int("slot0 burst", (int)state.dmrburstL, 20);
    rc |= expect_int("slot0 audio gate", state.p25_p2_audio_allowed[0], 1);
    rc |= expect_int("slot0 mute marker", state.p25_p2_enc_lockout_muted[0], 0);
    rc |= expect_int("slot0 fourv reset", state.fourv_counter[0], 0);
    rc |= expect_int("slot0 voice reset", state.voice_counter[0], 0);
    rc |= expect_int("slot0 gain reset", (int)state.aout_gain, 3);
    rc |= expect_int("slot0 enc emitted", g_enc_count[0], 1);
    rc |= expect_int("slot0 enc algid", g_enc_algid[0], 0x84);
    rc |= expect_int("slot0 lfsr", g_lfsr_count[0], 1);

    reset_stubs();
    g_audio_allow = 0;
    DSD_MEMSET(&state, 0, sizeof(state));
    state.lastsrcR = 0xAAAAAA;
    state.p25_p2_enc_lockout_muted[1] = 1;
    fill_mac(mac, 0x80, 0x1111, 0, 0x4567);

    p25p2_xcch_handle_ptt_slot(&opts, &state, mac, 1, 1);
    rc |= expect_int("slot1 zero source preserves lastsrcR", state.lastsrcR, 0xAAAAAA);
    rc |= expect_int("slot1 lasttgR", state.lasttgR, 0x4567);
    rc |= expect_int("slot1 algid", state.payload_algidR, 0x80);
    rc |= expect_int("slot1 keyid", state.payload_keyidR, 0x1111);
    rc |= expect_int("slot1 burst forced", (int)state.dmrburstR, 20);
    rc |= expect_int("slot1 audio closed", state.p25_p2_audio_allowed[1], 0);
    rc |= expect_int("slot1 clear alg skips enc", g_enc_count[1], 0);

    reset_stubs();
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.floating_point = 0;
    opts.pulse_digi_rate_out = 8000;
    opts.audio_gain = 4;
    opts.mbe_out_f = (FILE*)0x1;
    state.keyloader = 1;
    state.lastsrc = 0x112233;
    state.lasttg = 0x2222;
    state.payload_algid = 0x81;
    state.payload_keyid = 0x9999;
    state.payload_miP = 0x0102030405060708ULL;
    state.R = 0x1234567890ULL;
    state.A1[0] = 1;
    state.A2[0] = 2;
    state.A3[0] = 3;
    state.A4[0] = 4;
    state.aes_key_loaded[0] = 1;
    state.aes_key_segments[0] = 4;
    state.p25_p2_audio_allowed[0] = 1;
    state.p25_p2_enc_lockout_muted[0] = 1;
    state.p25_crypto_state[0] = DSD_P25_CRYPTO_DECRYPTABLE;
    state.fourv_counter[0] = 8;
    state.voice_counter[0] = 6;
    state.s_l4[0][0] = 321;
    DSD_SNPRINTF(state.call_string[0], sizeof(state.call_string[0]), "%s", "active call");
    DSD_SNPRINTF(state.dmr_embedded_gps[0], sizeof(state.dmr_embedded_gps[0]), "%s", "gps");
    DSD_SNPRINTF(state.dmr_lrrp_gps[0], sizeof(state.dmr_lrrp_gps[0]), "%s", "lrrp");

    p25p2_xcch_handle_end_slot(&opts, &state, 0, 1);
    rc |= expect_int("end slot0 src clear", state.lastsrc, 0);
    rc |= expect_int("end slot0 tg clear", state.lasttg, 0);
    rc |= expect_int("end slot0 alg clear", state.payload_algid, 0);
    rc |= expect_int("end slot0 key clear", state.payload_keyid, 0);
    rc |= expect_int("end slot0 drop", state.dropL, 256);
    rc |= expect_int("end slot0 burst", (int)state.dmrburstL, 23);
    rc |= expect_int("end slot0 audio clear", state.p25_p2_audio_allowed[0], 0);
    rc |= expect_int("end slot0 tail flush", g_flush_count, 1);
    rc |= expect_int("end slot0 tail flush slot", g_flush_slot, 0);
    rc |= expect_int("end slot0 tail flush before crypto reset", g_flush_crypto_state, DSD_P25_CRYPTO_DECRYPTABLE);
    rc |= expect_int("end slot0 tail flush before close", g_flush_close_l_count, 0);
    rc |= expect_int("end slot0 tail flush sees gate", g_flush_gate_l, 1);
    rc |= expect_int("end slot0 tail sample drained", state.s_l4[0][0], 0);
    rc |= expect_int("end slot0 close", g_close_l_count, 1);
    rc |= expect_int("end slot0 key clear", (int)state.R, 0);
    rc |= expect_int("end slot0 aes clear", state.aes_key_loaded[0], 0);
    rc |= expect_int("end slot0 gps clear", state.dmr_embedded_gps[0][0], '\0');
    rc |= expect_int("end slot0 lrrp clear", state.dmr_lrrp_gps[0][0], '\0');
    rc |= expect_int("end slot0 call blank", strncmp(state.call_string[0], P25P2_EMPTY_CALL_STRING, 21), 0);

    return rc;
}

static int
test_facch_public_dispatch_and_crc_gates(void) {
    static dsd_opts opts;
    static dsd_state state;
    unsigned long long int mac[24];
    int payload[156];
    int rc = 0;

    reset_stubs();
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    fill_mac(mac, 0x80, 0x1357, 0x010204, 0x2468);
    pack_payload_from_mac(payload, 156, mac, 0x1, 0x5, 0x2);
    state.currentslot = 1;

    process_FACCH_MAC_PDU(&opts, &state, payload);
    rc |= expect_int("facch ptt count", g_ptt_count[1], 0);
    rc |= expect_int("facch slot1 tg", state.lasttgR, 0x2468);
    rc |= expect_int("facch slot1 src", state.lastsrcR, 0x010204);
    rc |= expect_int("facch slot1 key", state.payload_keyidR, 0x1357);
    rc |= expect_int("facch slot1 gate", state.p25_p2_audio_allowed[1], 1);
    rc |= expect_int("facch slot1 burst", (int)state.dmrburstR, 20);

    reset_stubs();
    DSD_MEMSET(&state, 0, sizeof(state));
    state.currentslot = 1;
    state.lastsrcR = 0x010203;
    state.lasttgR = 77;
    state.p25_p2_audio_allowed[1] = 1;
    state.p25_p2_audio_ring_count[1] = 3;
    state.p25_call_is_packet[1] = 1;
    state.p25_policy_tg[1] = 0x5678;
    state.p25_service_options_valid[1] = 1;
    state.dmr_soR = 0x52;
    DSD_SNPRINTF(state.call_string[1], sizeof(state.call_string[1]), "%s", "unit call");
    pack_payload_from_mac(payload, 156, mac, 0x3, 0, 0);

    process_FACCH_MAC_PDU(&opts, &state, payload);
    rc |= expect_int("facch idle emitted", g_idle_count[1], 1);
    rc |= expect_int("facch idle vpdu", g_vpdu_count, 1);
    rc |= expect_int("facch idle vpdu type", g_vpdu_type, 0);
    rc |= expect_int("facch idle vpdu entry src clear", g_vpdu_entry_lastsrc[1], 0);
    rc |= expect_int("facch idle vpdu entry tg clear", g_vpdu_entry_lasttg[1], 0);
    rc |= expect_int("facch idle src clear", state.lastsrcR, 0);
    rc |= expect_int("facch idle tg clear", state.lasttgR, 0);
    rc |= expect_int("facch idle gate clear", state.p25_p2_audio_allowed[1], 0);
    rc |= expect_int("facch idle ring reset", g_ring_reset_count[1], 1);
    rc |= expect_int("facch idle packet clear", state.p25_call_is_packet[1], 0);
    rc |= expect_int("facch idle policy clear", (int)state.p25_policy_tg[1], 0);
    rc |= expect_int("facch idle service valid clear", state.p25_service_options_valid[1], 0);
    rc |= expect_int("facch idle service clear", state.dmr_soR, 0);
    rc |= expect_int("facch idle call blank", strncmp(state.call_string[1], P25P2_EMPTY_CALL_STRING, 21), 0);

    reset_stubs();
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(&opts, 0, sizeof(opts));
    state.currentslot = 1;
    state.lastsrcR = 0x010204;
    state.lasttgR = 0x2468;
    state.p25_p2_audio_allowed[1] = 1;
    state.p25_p2_audio_ring_count[1] = 5;
    state.p25_call_is_packet[1] = 1;
    state.p25_policy_tg[1] = 0x6789;
    state.p25_service_options_valid[1] = 1;
    state.dmr_soR = 0x93;
    state.p25_p2_enc_lockout_muted[1] = 1;
    g_vpdu_grant_newer_slot = 1;
    DSD_SNPRINTF(state.call_string[1], sizeof(state.call_string[1]), "%s", "grant");
    pack_payload_from_mac(payload, 156, mac, 0x3, 0, 0);

    process_FACCH_MAC_PDU(&opts, &state, payload);
    rc |= expect_int("facch idle grant emitted", g_idle_count[1], 1);
    rc |= expect_int("facch idle grant vpdu entry src clear", g_vpdu_entry_lastsrc[1], 0);
    rc |= expect_int("facch idle grant vpdu entry tg clear", g_vpdu_entry_lasttg[1], 0);
    rc |= expect_int("facch idle grant gate clear", state.p25_p2_audio_allowed[1], 0);
    rc |= expect_int("facch idle grant ring reset", g_ring_reset_count[1], 1);
    rc |= expect_int("facch idle grant src preserved", state.lastsrcR, 0x010204);
    rc |= expect_int("facch idle grant tg preserved", state.lasttgR, 0x2468);
    rc |= expect_int("facch idle grant packet preserved", state.p25_call_is_packet[1], 1);
    rc |= expect_int("facch idle grant policy preserved", (int)state.p25_policy_tg[1], 0x6789);
    rc |= expect_int("facch idle grant service valid preserved", state.p25_service_options_valid[1], 1);
    rc |= expect_int("facch idle grant service preserved", state.dmr_soR, 0x93);
    rc |= expect_int("facch idle grant mute clear", state.p25_p2_enc_lockout_muted[1], 0);
    rc |= expect_int("facch idle grant call preserved", strncmp(state.call_string[1], "grant", 5), 0);

    reset_stubs();
    DSD_MEMSET(&state, 0, sizeof(state));
    state.currentslot = 1;
    g_crc12_result = 1;
    mac[1] = 0x44;
    pack_payload_from_mac(payload, 156, mac, 0x1, 0, 0);

    process_FACCH_MAC_PDU(&opts, &state, payload);
    rc |= expect_int("facch crc abort leaves tg", state.lasttgR, 0);
    rc |= expect_int("facch crc abort no vpdu", g_vpdu_count, 0);
    rc |= expect_int("facch crc abort no gate", state.p25_p2_audio_allowed[1], 0);

    return rc;
}

static int
test_sacch_dispatch_and_lcch_crc_abort(void) {
    static dsd_opts opts;
    static dsd_state state;
    unsigned long long int mac[24];
    int payload[180];
    int rc = 0;

    reset_stubs();
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    fill_mac(mac, 0x80, 0x2222, 0x030405, 0x3456);
    pack_payload_from_mac(payload, 180, mac, 0x1, 0x1, 0x3);
    state.currentslot = 0;

    process_SACCH_MAC_PDU(&opts, &state, payload);
    rc |= expect_int("sacch opposite slot src", state.lastsrcR, 0x030405);
    rc |= expect_int("sacch opposite slot tg", state.lasttgR, 0x3456);
    rc |= expect_int("sacch ptt emitted", g_ptt_count[1], 1);
    rc |= expect_int("sacch last active stamped", state.p25_p2_last_mac_active_m[1] > 0.0 ? 1 : 0, 1);

    reset_stubs();
    DSD_MEMSET(&state, 0, sizeof(state));
    state.currentslot = 0;
    state.p2_is_lcch = 1;
    opts.aggressive_framesync = 1;
    state.p25_p2_audio_allowed[1] = 1;
    state.p25_p2_audio_ring_count[1] = 4;
    g_crc16_result = 1;
    mac[1] = 0x55;
    pack_payload_from_mac(payload, 180, mac, 0x0, 0, 0);

    process_SACCH_MAC_PDU(&opts, &state, payload);
    rc |= expect_int("lcch crc abort clears lcch", state.p2_is_lcch, 0);
    rc |= expect_int("lcch crc abort clears gate", state.p25_p2_audio_allowed[1], 0);
    rc |= expect_int("lcch crc abort resets ring", g_ring_reset_count[1], 1);
    rc |= expect_int("lcch crc abort no vpdu", g_vpdu_count, 0);

    return rc;
}

static int
test_sacch_end_idle_active_hangtime_dispatch(void) {
    static dsd_opts opts;
    static dsd_state state;
    unsigned long long int mac[24];
    int payload[180];
    int rc = 0;

    reset_stubs();
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    state.currentslot = 0;
    state.keyloader = 1;
    state.lastsrcR = 0x445566;
    state.lasttgR = 0x3344;
    state.payload_algidR = 0x84;
    state.payload_keyidR = 0x2468;
    state.RR = 0x123456789ABCULL;
    state.A1[1] = 1;
    state.A2[1] = 2;
    state.A3[1] = 3;
    state.A4[1] = 4;
    state.aes_key_loaded[1] = 1;
    state.aes_key_segments[1] = 4;
    state.p25_p2_audio_allowed[1] = 1;
    opts.mbe_out_fR = (FILE*)0x1;
    DSD_SNPRINTF(state.call_string[1], sizeof(state.call_string[1]), "%s", "right call");
    DSD_SNPRINTF(state.dmr_embedded_gps[1], sizeof(state.dmr_embedded_gps[1]), "%s", "gps");
    DSD_SNPRINTF(state.dmr_lrrp_gps[1], sizeof(state.dmr_lrrp_gps[1]), "%s", "lrrp");
    fill_mac(mac, 0x80, 0, 0, 0);
    pack_payload_from_mac(payload, 180, mac, 0x2, 0, 0);

    process_SACCH_MAC_PDU(&opts, &state, payload);
    rc |= expect_int("sacch end emitted", g_end_count[1], 1);
    rc |= expect_int("sacch end src clear", state.lastsrcR, 0);
    rc |= expect_int("sacch end tg clear", state.lasttgR, 0);
    rc |= expect_int("sacch end alg clear", state.payload_algidR, 0);
    rc |= expect_int("sacch end keyid clear", state.payload_keyidR, 0);
    rc |= expect_int("sacch end gate clear", state.p25_p2_audio_allowed[1], 0);
    rc |= expect_int("sacch end burst", (int)state.dmrburstR, 23);
    rc |= expect_int("sacch end close right", g_close_r_count, 1);
    rc |= expect_int("sacch end key clear", (int)state.RR, 0);
    rc |= expect_int("sacch end aes clear", state.aes_key_loaded[1], 0);
    rc |= expect_int("sacch end gps clear", state.dmr_embedded_gps[1][0], '\0');
    rc |= expect_int("sacch end lrrp clear", state.dmr_lrrp_gps[1][0], '\0');
    rc |= expect_int("sacch end call blank", strncmp(state.call_string[1], P25P2_EMPTY_CALL_STRING, 21), 0);

    reset_stubs();
    DSD_MEMSET(&state, 0, sizeof(state));
    state.currentslot = 0;
    state.p25_p2_audio_allowed[1] = 1;
    state.p25_p2_enc_lockout_muted[1] = 1;
    state.p25_crypto_state[1] = DSD_P25_CRYPTO_ENCRYPTED_PENDING;
    state.p25_call_is_packet[1] = 1;
    state.p25_policy_tg[1] = 0x5678;
    state.p25_service_options_valid[1] = 1;
    state.dmr_soR = 0x52;
    DSD_SNPRINTF(state.call_string[1], sizeof(state.call_string[1]), "%s", "packet");
    pack_payload_from_mac(payload, 180, mac, 0x3, 0, 0);

    process_SACCH_MAC_PDU(&opts, &state, payload);
    rc |= expect_int("sacch idle emitted", g_idle_count[1], 1);
    rc |= expect_int("sacch idle vpdu", g_vpdu_count, 1);
    rc |= expect_int("sacch idle vpdu type", g_vpdu_type, 1);
    rc |= expect_int("sacch idle burst", (int)state.dmrburstR, 24);
    rc |= expect_int("sacch idle gate clear", state.p25_p2_audio_allowed[1], 0);
    rc |= expect_int("sacch idle packet clear", state.p25_call_is_packet[1], 0);
    rc |= expect_int("sacch idle policy clear", (int)state.p25_policy_tg[1], 0);
    rc |= expect_int("sacch idle service valid clear", state.p25_service_options_valid[1], 0);
    rc |= expect_int("sacch idle service clear", state.dmr_soR, 0);
    rc |= expect_int("sacch idle mute clear", state.p25_p2_enc_lockout_muted[1], 0);
    rc |= expect_int("sacch idle call blank", strncmp(state.call_string[1], P25P2_EMPTY_CALL_STRING, 21), 0);

    reset_stubs();
    DSD_MEMSET(&state, 0, sizeof(state));
    state.currentslot = 0;
    state.p25_p2_audio_allowed[1] = 1;
    state.p25_p2_enc_lockout_muted[1] = 1;
    state.p25_crypto_state[1] = DSD_P25_CRYPTO_ENCRYPTED_PENDING;
    state.p25_call_is_packet[1] = 1;
    state.p25_policy_tg[1] = 0x6789;
    state.p25_service_options_valid[1] = 1;
    state.dmr_soR = 0x93;
    g_slot_grant_newer[1] = 1;
    DSD_SNPRINTF(state.call_string[1], sizeof(state.call_string[1]), "%s", "grant");
    pack_payload_from_mac(payload, 180, mac, 0x3, 0, 0);

    process_SACCH_MAC_PDU(&opts, &state, payload);
    rc |= expect_int("sacch idle grant emitted", g_idle_count[1], 1);
    rc |= expect_int("sacch idle grant gate clear", state.p25_p2_audio_allowed[1], 0);
    rc |= expect_int("sacch idle grant packet preserved", state.p25_call_is_packet[1], 1);
    rc |= expect_int("sacch idle grant policy preserved", (int)state.p25_policy_tg[1], 0x6789);
    rc |= expect_int("sacch idle grant service valid preserved", state.p25_service_options_valid[1], 1);
    rc |= expect_int("sacch idle grant service preserved", state.dmr_soR, 0x93);
    rc |= expect_int("sacch idle grant mute preserved", state.p25_p2_enc_lockout_muted[1], 1);
    rc |= expect_int("sacch idle grant crypto preserved", state.p25_crypto_state[1], DSD_P25_CRYPTO_ENCRYPTED_PENDING);
    rc |= expect_int("sacch idle grant call preserved", strncmp(state.call_string[1], "grant", 5), 0);

    reset_stubs();
    DSD_MEMSET(&state, 0, sizeof(state));
    state.currentslot = 0;
    state.payload_algidR = 0x81;
    state.payload_keyidR = 0x2222;
    state.lasttgR = 0x3456;
    state.p25_crypto_state[1] = DSD_P25_CRYPTO_DECRYPTABLE;
    pack_payload_from_mac(payload, 180, mac, 0x4, 0, 0);

    process_SACCH_MAC_PDU(&opts, &state, payload);
    rc |= expect_int("sacch active vpdu", g_vpdu_count, 1);
    rc |= expect_int("sacch active vpdu type", g_vpdu_type, 1);
    rc |= expect_int("sacch active gate", state.p25_p2_audio_allowed[1], 1);
    rc |= expect_int("sacch active burst", (int)state.dmrburstR, 21);
    rc |= expect_int("sacch active does not re-emit enc", g_enc_count[1], 0);
    rc |= expect_int("sacch active timestamp", state.p25_p2_last_mac_active_m[1] > 0.0 ? 1 : 0, 1);

    reset_stubs();
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(&opts, 0, sizeof(opts));
    state.currentslot = 0;
    state.dmrburstL = 21;
    state.dmrburstR = 21;
    state.p25_p2_audio_allowed[0] = 1;
    state.p25_p2_audio_allowed[1] = 1;
    state.voice_counter[0] = 5;
    state.voice_counter[1] = 1;
    state.s_l4[0][0] = 456;
    state.s_r4[0][0] = -123;
    opts.pulse_digi_rate_out = 8000;
    opts.mbe_out_fR = (FILE*)0x1;
    pack_payload_from_mac(payload, 180, mac, 0x6, 0, 0);

    process_SACCH_MAC_PDU(&opts, &state, payload);
    rc |= expect_int("sacch hangtime vpdu", g_vpdu_count, 1);
    rc |= expect_int("sacch hangtime vpdu type", g_vpdu_type, 1);
    rc |= expect_int("sacch hangtime flush", g_flush_count, 1);
    rc |= expect_int("sacch hangtime flush slot", g_flush_slot, 1);
    rc |= expect_int("sacch hangtime flush before burst", g_flush_burst_r, 21);
    rc |= expect_int("sacch hangtime flush before close", g_flush_close_r_count, 0);
    rc |= expect_int("sacch hangtime flush sees gate", g_flush_gate_r, 1);
    rc |= expect_int("sacch hangtime burst right", (int)state.dmrburstR, 22);
    rc |= expect_int("sacch hangtime close right", g_close_r_count, 1);
    rc |= expect_int("sacch hangtime gate preserved", state.p25_p2_audio_allowed[1], 1);
    rc |= expect_int("sacch hangtime other gate preserved", state.p25_p2_audio_allowed[0], 1);
    rc |= expect_int("sacch hangtime other voice counter preserved", state.voice_counter[0], 5);
    rc |= expect_int("sacch hangtime voice counter reset", state.voice_counter[1], 0);
    rc |= expect_int("sacch hangtime other sample preserved", state.s_l4[0][0], 456);
    rc |= expect_int("sacch hangtime sample cleared", state.s_r4[0][0], 0);

    return rc;
}

static int
test_facch_active_end_hangtime_and_invalid_slot_guards(void) {
    static dsd_opts opts;
    static dsd_state state;
    unsigned long long int mac[24];
    int payload[156];
    int rc = 0;

    reset_stubs();
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    fill_mac(mac, 0x80, 0, 0, 0);

    p25p2_xcch_handle_facch_mac_end(&opts, &state, 2);
    p25p2_xcch_handle_facch_mac_idle(&opts, &state, 2, mac);
    p25p2_xcch_handle_facch_mac_active(&opts, &state, 2, mac);
    DSD_SNPRINTF(state.call_string[0], sizeof(state.call_string[0]), "%s", "left call");
    p25p2_xcch_clear_idle_metadata_if_stale(&state, 2, dsd_time_now_monotonic_s(), 1);
    rc |= expect_int("facch invalid no end", g_end_count[0] + g_end_count[1], 0);
    rc |= expect_int("facch invalid no idle", g_idle_count[0] + g_idle_count[1], 0);
    rc |= expect_int("facch invalid no active", g_active_count[0] + g_active_count[1], 0);
    rc |= expect_int("facch invalid no vpdu", g_vpdu_count, 0);
    rc |= expect_int("facch invalid no burst left", (int)state.dmrburstL, 0);
    rc |= expect_int("facch invalid idle metadata guard", strncmp(state.call_string[0], "left call", 9), 0);

    reset_stubs();
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(&opts, 0, sizeof(opts));
    state.currentslot = 0;
    state.keyloader = 1;
    state.lastsrc = 0x123456;
    state.lasttg = 0x4567;
    state.payload_algid = 0x81;
    state.payload_keyid = 0x5555;
    state.R = 0x12345678ULL;
    state.A1[0] = 5;
    state.aes_key_loaded[0] = 1;
    state.aes_key_segments[0] = 2;
    state.p25_p2_audio_allowed[0] = 1;
    opts.mbe_out_f = (FILE*)0x1;
    DSD_SNPRINTF(state.call_string[0], sizeof(state.call_string[0]), "%s", "left call");
    pack_payload_from_mac(payload, 156, mac, 0x2, 0, 0);

    process_FACCH_MAC_PDU(&opts, &state, payload);
    rc |= expect_int("facch end emitted", g_end_count[0], 1);
    rc |= expect_int("facch end src clear", state.lastsrc, 0);
    rc |= expect_int("facch end tg clear", state.lasttg, 0);
    rc |= expect_int("facch end gate clear", state.p25_p2_audio_allowed[0], 0);
    rc |= expect_int("facch end burst", (int)state.dmrburstL, 23);
    rc |= expect_int("facch end close left", g_close_l_count, 1);
    rc |= expect_int("facch end key clear", (int)state.R, 0);
    rc |= expect_int("facch end call blank", strncmp(state.call_string[0], P25P2_EMPTY_CALL_STRING, 21), 0);

    reset_stubs();
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(&opts, 0, sizeof(opts));
    state.currentslot = 1;
    state.payload_algidR = 0x84;
    state.payload_keyidR = 0x7777;
    state.lasttgR = 0x7654;
    state.p25_crypto_state[1] = DSD_P25_CRYPTO_DECRYPTABLE;
    pack_payload_from_mac(payload, 156, mac, 0x4, 0, 0);

    process_FACCH_MAC_PDU(&opts, &state, payload);
    rc |= expect_int("facch active emitted", g_active_count[1], 1);
    rc |= expect_int("facch active vpdu", g_vpdu_count, 1);
    rc |= expect_int("facch active vpdu type", g_vpdu_type, 0);
    rc |= expect_int("facch active gate", state.p25_p2_audio_allowed[1], 1);
    rc |= expect_int("facch active burst", (int)state.dmrburstR, 21);
    rc |= expect_int("facch active does not re-emit enc", g_enc_count[1], 0);

    reset_stubs();
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(&opts, 0, sizeof(opts));
    state.currentslot = 1;
    state.dmrburstL = 21;
    state.dmrburstR = 21;
    state.p25_p2_audio_allowed[0] = 1;
    state.p25_p2_audio_allowed[1] = 1;
    state.voice_counter[0] = 4;
    state.voice_counter[1] = 1;
    state.s_l4[0][0] = -345;
    state.s_r4[0][0] = 234;
    opts.pulse_digi_rate_out = 8000;
    opts.mbe_out_fR = (FILE*)0x1;
    pack_payload_from_mac(payload, 156, mac, 0x6, 0, 0);

    process_FACCH_MAC_PDU(&opts, &state, payload);
    rc |= expect_int("facch hangtime vpdu", g_vpdu_count, 1);
    rc |= expect_int("facch hangtime vpdu type", g_vpdu_type, 0);
    rc |= expect_int("facch hangtime flush", g_flush_count, 1);
    rc |= expect_int("facch hangtime flush slot", g_flush_slot, 1);
    rc |= expect_int("facch hangtime flush before burst", g_flush_burst_r, 21);
    rc |= expect_int("facch hangtime flush before close", g_flush_close_r_count, 0);
    rc |= expect_int("facch hangtime flush sees gate", g_flush_gate_r, 1);
    rc |= expect_int("facch hangtime burst right", (int)state.dmrburstR, 22);
    rc |= expect_int("facch hangtime close right", g_close_r_count, 1);
    rc |= expect_int("facch hangtime gate preserved", state.p25_p2_audio_allowed[1], 1);
    rc |= expect_int("facch hangtime other gate preserved", state.p25_p2_audio_allowed[0], 1);
    rc |= expect_int("facch hangtime other voice counter preserved", state.voice_counter[0], 4);
    rc |= expect_int("facch hangtime voice counter reset", state.voice_counter[1], 0);
    rc |= expect_int("facch hangtime other sample preserved", state.s_l4[0][0], -345);
    rc |= expect_int("facch hangtime sample cleared", state.s_r4[0][0], 0);

    return rc;
}

static int
test_encrypted_voice_user_stays_locked_through_mac_active(void) {
    static dsd_opts opts;
    static dsd_state state;
    unsigned long long int mac[24];
    int facch_payload[156];
    int sacch_payload[180];
    int rc = 0;

    fill_mac(mac, 0, 0, 0, 0);
    pack_payload_from_mac(facch_payload, 156, mac, 0x4, 0, 0);
    pack_payload_from_mac(sacch_payload, 180, mac, 0x4, 0, 0);

    for (int slot = 0; slot < 2; slot++) {
        reset_stubs();
        DSD_MEMSET(&opts, 0, sizeof(opts));
        DSD_MEMSET(&state, 0, sizeof(state));
        state.currentslot = slot;
        state.p25_crypto_state[slot] = DSD_P25_CRYPTO_CLEAR;
        state.p25_crypto_state[slot ^ 1] = DSD_P25_CRYPTO_CLEAR;
        state.p25_p2_audio_allowed[slot] = 1;
        state.p25_p2_audio_allowed[slot ^ 1] = 1;
        state.p25_p2_audio_ring_count[slot] = 2;
        state.p25_p2_audio_ring_count[slot ^ 1] = 3;
        if (slot == 0) {
            state.payload_algid = 0x81;
            state.payload_keyid = 0x2468;
            state.payload_miP = 0x1122334455667788ULL;
        } else {
            state.payload_algidR = 0x81;
            state.payload_keyidR = 0x2468;
            state.payload_miN = 0x1122334455667788ULL;
        }
        g_vpdu_enc_pending_slot = slot;

        process_FACCH_MAC_PDU(&opts, &state, facch_payload);
        rc |= expect_int("facch encrypted user state pending", state.p25_crypto_state[slot],
                         DSD_P25_CRYPTO_ENCRYPTED_PENDING);
        rc |= expect_int("facch encrypted user gate stays closed", state.p25_p2_audio_allowed[slot], 0);
        rc |= expect_int("facch encrypted user marker stays set", state.p25_p2_enc_lockout_muted[slot], 1);
        rc |= expect_int("facch encrypted user ring purged", state.p25_p2_audio_ring_count[slot], 0);
        rc |= expect_int("facch encrypted user still emits activity", g_active_count[slot], 1);
        rc |= expect_int("facch companion gate preserved", state.p25_p2_audio_allowed[slot ^ 1], 1);
        rc |= expect_int("facch companion ring preserved", state.p25_p2_audio_ring_count[slot ^ 1], 3);
        rc |= expect_u64("facch encrypted user MI preserved", slot == 0 ? state.payload_miP : state.payload_miN,
                         0x1122334455667788ULL);

        reset_stubs();
        DSD_MEMSET(&opts, 0, sizeof(opts));
        DSD_MEMSET(&state, 0, sizeof(state));
        state.currentslot = slot ^ 1;
        state.p25_crypto_state[slot] = DSD_P25_CRYPTO_CLEAR;
        state.p25_crypto_state[slot ^ 1] = DSD_P25_CRYPTO_CLEAR;
        state.p25_p2_audio_allowed[slot] = 1;
        state.p25_p2_audio_allowed[slot ^ 1] = 1;
        state.p25_p2_audio_ring_count[slot] = 2;
        state.p25_p2_audio_ring_count[slot ^ 1] = 3;
        if (slot == 0) {
            state.payload_algid = 0x81;
            state.payload_keyid = 0x2468;
            state.payload_miP = 0x8877665544332211ULL;
        } else {
            state.payload_algidR = 0x81;
            state.payload_keyidR = 0x2468;
            state.payload_miN = 0x8877665544332211ULL;
        }
        g_vpdu_enc_pending_slot = slot;

        process_SACCH_MAC_PDU(&opts, &state, sacch_payload);
        rc |= expect_int("sacch encrypted user state pending", state.p25_crypto_state[slot],
                         DSD_P25_CRYPTO_ENCRYPTED_PENDING);
        rc |= expect_int("sacch encrypted user gate stays closed", state.p25_p2_audio_allowed[slot], 0);
        rc |= expect_int("sacch encrypted user marker stays set", state.p25_p2_enc_lockout_muted[slot], 1);
        rc |= expect_int("sacch encrypted user ring purged", state.p25_p2_audio_ring_count[slot], 0);
        rc |= expect_int("sacch companion gate preserved", state.p25_p2_audio_allowed[slot ^ 1], 1);
        rc |= expect_int("sacch companion ring preserved", state.p25_p2_audio_ring_count[slot ^ 1], 3);
        rc |= expect_u64("sacch encrypted user MI preserved", slot == 0 ? state.payload_miP : state.payload_miN,
                         0x8877665544332211ULL);
    }

    return rc;
}

static int
test_voice_counter_reset_helpers(void) {
    static dsd_state state;
    int rc = 0;

    reset_stubs();
    DSD_MEMSET(&state, 0, sizeof(state));
    state.currentslot = 0;
    state.payload_algid = 0x81;
    state.payload_algidR = 0x80;
    state.DMRvcL = 11;
    state.DMRvcR = 12;
    p25p2_xcch_reset_ptt_voice_counter_sacch(&state);
    rc |= expect_int("sacch encrypted opposite resets right", state.DMRvcR, 0);
    rc |= expect_int("sacch preserves left", state.DMRvcL, 11);

    state.currentslot = 1;
    state.payload_algid = 0x80;
    state.payload_algidR = 0x84;
    state.DMRvcL = 13;
    state.DMRvcR = 14;
    p25p2_xcch_reset_ptt_voice_counter_sacch(&state);
    rc |= expect_int("sacch encrypted opposite resets left", state.DMRvcL, 0);
    rc |= expect_int("sacch preserves right", state.DMRvcR, 14);

    state.currentslot = 0;
    state.payload_algid = 0x89;
    state.payload_algidR = 0x80;
    state.DMRvcL = 21;
    state.DMRvcR = 22;
    p25p2_xcch_reset_ptt_voice_counter_facch(&state);
    rc |= expect_int("facch encrypted current resets left", state.DMRvcL, 0);
    rc |= expect_int("facch preserves right", state.DMRvcR, 22);

    state.currentslot = 1;
    state.payload_algid = 0x80;
    state.payload_algidR = 0x81;
    state.DMRvcL = 23;
    state.DMRvcR = 24;
    p25p2_xcch_reset_ptt_voice_counter_facch(&state);
    rc |= expect_int("facch encrypted current resets right", state.DMRvcR, 0);
    rc |= expect_int("facch preserves left", state.DMRvcL, 23);

    state.currentslot = 0;
    state.payload_algid = 0x80;
    state.DMRvcL = 31;
    state.DMRvcR = 32;
    p25p2_xcch_reset_ptt_voice_counter_facch(&state);
    rc |= expect_int("facch clear alg preserves left", state.DMRvcL, 31);
    rc |= expect_int("facch clear alg preserves right", state.DMRvcR, 32);

    return rc;
}

int
main(void) {
    int rc = 0;

    rc |= test_slot_ptt_and_end_helpers();
    rc |= test_facch_public_dispatch_and_crc_gates();
    rc |= test_sacch_dispatch_and_lcch_crc_abort();
    rc |= test_sacch_end_idle_active_hangtime_dispatch();
    rc |= test_facch_active_end_hangtime_and_invalid_slot_guards();
    rc |= test_encrypted_voice_user_stays_locked_through_mac_active();
    rc |= test_voice_counter_reset_helpers();

    if (rc != 0) {
        return 1;
    }

    printf("P25_P2_XCCH_HELPERS: OK\n");
    return 0;
}

// NOLINTEND(bugprone-suspicious-include)
