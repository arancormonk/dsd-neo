// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Unit tests for P25P2 soft metric buffer handling.
 *
 * Validates that:
 * 1. p25_p2_frame_reset() clears soft-decision buffers
 * 2. Buffer sizes are consistent with 700-dibit/1400-bit capture scope
 * 3. Soft-decision buffers are distinct from bit buffers
 *
 * This test compiles p25p2_frame.c directly with stubs to avoid
 * dragging in full library dependencies.
 */

#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/vocoder.h>
#include <dsd-neo/platform/posix_compat.h>
#include <dsd-neo/protocol/p25/p25p2_frame.h>
#include <dsd-neo/runtime/config.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

struct RtlSdrContext;

/* External declarations matching p25p2_frame.c */
extern int p2bit[4320];
extern int16_t p2llr[1400];
extern int16_t p2xllr[1400];
extern int ess_a[2][168];
extern int16_t ess_a_llr[2][168];
extern void p25_p2_frame_reset(void);
extern void process_ESS(dsd_opts* opts, dsd_state* state);
extern void p25p2_test_teardown_call(dsd_opts* opts, dsd_state* state);
extern void p25p2_test_process_facchc(dsd_opts* opts, dsd_state* state, int timeslot_index);
extern void p25p2_test_process_isch(dsd_opts* opts, dsd_state* state, int framing_index);
extern void p25p2_test_process_p2_duid(dsd_opts* opts, dsd_state* state);
extern void p25p2_test_process_sacchc(dsd_opts* opts, dsd_state* state, int timeslot_index);

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
static int g_ss18_calls = 0;
static int g_ss18_allowed_l = -1;
static int g_ss18_allowed_r = -1;

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

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
return_to_cc(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
rtl_stream_tune(struct RtlSdrContext* ctx, uint32_t center_freq_hz) {
    (void)ctx;
    (void)center_freq_hz;
    return 0;
}

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

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
getTimeC_buf(char out[9]) {
    if (out) {
        DSD_MEMCPY(out, "00:00:00", 9);
    }
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

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
playSynthesizedVoiceFS4(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
playSynthesizedVoiceSS18(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    g_ss18_calls++;
    g_ss18_allowed_l = state ? state->p25_p2_audio_allowed[0] : -1;
    g_ss18_allowed_r = state ? state->p25_p2_audio_allowed[1] : -1;
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
p25_emit_enc_lockout_once(dsd_opts* opts, dsd_state* state, uint8_t slot, int tg, int svc) {
    (void)opts;
    (void)state;
    (void)slot;
    (void)tg;
    (void)svc;
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

/* RS decoders */
int
// NOLINTNEXTLINE(misc-use-internal-linkage)
ez_rs28_facch(int* payload, int* parity) {
    (void)payload;
    (void)parity;
    return 0;
}

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
ez_rs28_sacch(int* payload, int* parity) {
    (void)payload;
    (void)parity;
    return 0;
}

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
ez_rs28_ess(int* payload, int* parity) {
    (void)payload;
    (void)parity;
    return g_ess_hard_rc;
}

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
isch_lookup(uint64_t isch) {
    (void)isch;
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
ez_rs28_facch_soft(int* payload, int* parity, const int* erasures, int n_erasures) {
    (void)payload;
    (void)parity;
    (void)erasures;
    g_facch_calls++;
    g_facch_last_erasures = n_erasures;
    return n_erasures >= g_facch_min_success ? g_facch_success_rc : -1;
}

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
ez_rs28_sacch_soft(int* payload, int* parity, const int* erasures, int n_erasures) {
    (void)payload;
    (void)parity;
    (void)erasures;
    g_sacch_calls++;
    g_sacch_last_erasures = n_erasures;
    return n_erasures >= g_sacch_min_success ? g_sacch_success_rc : -1;
}

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
ez_rs28_ess_soft(int* payload, int* parity, const int* erasures, int n_erasures) {
    (void)parity;
    (void)erasures;
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

/* MAC PDU handlers */
void
// NOLINTNEXTLINE(misc-use-internal-linkage)
process_SACCH_MAC_PDU(dsd_opts* opts, dsd_state* state, int* bits) {
    (void)opts;
    (void)state;
    g_sacch_mac_calls++;
    g_sacch_mac_last_opcode = (bits[0] << 2) | (bits[1] << 1) | bits[2];
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
process_FACCH_MAC_PDU(dsd_opts* opts, dsd_state* state, int* bits) {
    (void)opts;
    (void)state;
    g_facch_mac_calls++;
    g_facch_mac_last_opcode = (bits[0] << 2) | (bits[1] << 1) | bits[2];
}

/* Dibit acquisition */
int
getDibitSoft(dsd_opts* opts, dsd_state* state, dsd_dibit_soft_t* out_soft) {
    (void)opts;
    (void)state;
    if (out_soft) {
        out_soft->reliability = 128;
        out_soft->llr[0] = -128;
        out_soft->llr[1] = -128;
    }
    return 0;
}

int
getDibitWithReliability(dsd_opts* opts, dsd_state* state, uint8_t* out_reliability) {
    (void)opts;
    (void)state;
    if (out_reliability) {
        *out_reliability = 128;
    }
    return 0;
}

static void
set_p25p2_threshold(int threshold) {
    char value[16];
    DSD_SNPRINTF(value, sizeof(value), "%d", threshold);
    dsd_setenv("DSD_NEO_P25P2_SOFT_ERASURE_THRESHOLD", value, 1);
    dsd_neo_config_init(NULL);
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
}

static void
reset_playback_stub(void) {
    g_ss18_calls = 0;
    g_ss18_allowed_l = -1;
    g_ss18_allowed_r = -1;
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

static int
expect_str(const char* tag, const char* got, const char* want) {
    if (strcmp(got, want) != 0) {
        DSD_FPRINTF(stderr, "%s: got \"%s\" want \"%s\"\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
expect_s16_clear(const char* tag, short frames[18][160]) {
    for (int j = 0; j < 18; j++) {
        for (int i = 0; i < 160; i++) {
            if (frames[j][i] != 0) {
                DSD_FPRINTF(stderr, "%s: frame[%d][%d] remained %d\n", tag, j, i, frames[j][i]);
                return 1;
            }
        }
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

    process_ESS(&opts, &state);

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

    process_ESS(&opts, &state);

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

    opts.p25_trunk = 1;
    opts.p25_is_tuned = 1;
    opts.trunk_tune_enc_calls = 0;
    state.lasttg = 1234;
    state.R = 0x0123456789ABCDEFULL;
    state.dmrburstL = 20;
    set_ess_payload_bits(&state, 0, 0x81, 0x2468, 0x1122334455667788ULL);

    process_ESS(&opts, &state);

    if (state.payload_algid == 0x81 && state.payload_keyid == 0x2468 && state.payload_miP == 0x1122334455667788ULL
        && state.p25_p2_audio_allowed[0] == 1 && state.p25_p2_rs_ess_ok == 1) {
        printf("PASS\n");
        return 0;
    }

    printf("FAIL (alg=0x%02X keyid=0x%04X mi=0x%016llX gate=%d ok=%u)\n", state.payload_algid, state.payload_keyid,
           state.payload_miP, state.p25_p2_audio_allowed[0], state.p25_p2_rs_ess_ok);
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

    opts.p25_trunk = 1;
    opts.p25_is_tuned = 1;
    opts.trunk_tune_enc_calls = 0;
    state.currentslot = 1;
    state.lasttgR = 5678;
    state.dmrburstR = 21;
    state.aes_key_loaded[1] = 1;
    set_ess_payload_bits(&state, 1, 0x84, 0x1357, 0x0123456789ABCDEFULL);

    process_ESS(&opts, &state);

    if (state.payload_algidR == 0x84 && state.payload_keyidR == 0x1357 && state.payload_miN == 0x0123456789ABCDEFULL
        && state.p25_p2_audio_allowed[1] == 1 && state.p25_p2_rs_ess_ok == 1 && g_lfsr128_calls == 1
        && g_lfsr128_last_slot == 1) {
        printf("PASS\n");
        return 0;
    }

    printf("FAIL (alg=0x%02X keyid=0x%04X mi=0x%016llX gate=%d ok=%u lfsr128=%d slot=%d)\n", state.payload_algidR,
           state.payload_keyidR, state.payload_miN, state.p25_p2_audio_allowed[1], state.p25_p2_rs_ess_ok,
           g_lfsr128_calls, g_lfsr128_last_slot);
    return 1;
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
    state.lasttg = 1234;
    state.tg_hold = 9999;
    state.dmrburstL = 20;
    set_ess_payload_bits(&state, 0, 0x80, 0x0000, 0x0000000000000000ULL);

    process_ESS(&opts, &state);

    if (state.payload_algid == 0x80 && state.p25_p2_audio_allowed[0] == 0 && state.p25_p2_enc_lockout_muted[0] == 0
        && state.p25_p2_rs_ess_ok == 1) {
        printf("PASS\n");
        return 0;
    }

    printf("FAIL (alg=0x%02X gate=%d marker=%u ok=%u)\n", state.payload_algid, state.p25_p2_audio_allowed[0],
           state.p25_p2_enc_lockout_muted[0], state.p25_p2_rs_ess_ok);
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
    g_ess_hard_rc = -1;
    g_ess_soft_min_success = -1;

    process_ESS(&opts, &state);

    if (state.p25_p2_rs_ess_err == 1 && state.p25_p2_rs_ess_ok == 0 && g_lfsrp_calls == 1 && g_lfsr128_calls == 0) {
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
    g_ess_hard_rc = -1;
    g_ess_soft_min_success = -1;

    process_ESS(&opts, &state);

    if (state.p25_p2_rs_ess_err == 1 && g_lfsrp_calls == 1 && g_lfsr128_calls == 1 && g_lfsr128_last_slot == 1) {
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

    p25p2_test_process_facchc(&opts, &state, 2);

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

    p25p2_test_process_facchc(&opts, &state, 0);

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

    p25p2_test_process_sacchc(&opts, &state, 1);

    int rc = 0;
    rc |= expect_int("sacch ok", (int)state.p25_p2_rs_sacch_ok, 1);
    rc |= expect_int("sacch err", (int)state.p25_p2_rs_sacch_err, 0);
    rc |= expect_int("sacch corr", (int)state.p25_p2_rs_sacch_corr, 4);
    rc |= expect_int("sacch dynamic erasure", (int)state.p25_p2_soft_erasure_ok, 1);
    rc |= expect_int("sacch inverse opcode", state.dmr_soR, 6);
    rc |= expect_int("sacch mac calls", g_sacch_mac_calls, 1);
    rc |= expect_int("sacch mac opcode", g_sacch_mac_last_opcode, 6);
    rc |= expect_int("sacch erasure depth", g_sacch_last_erasures, 12);
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
    p25p2_test_process_isch(&opts, &state, 3);

    int rc = 0;
    rc |= expect_int("isch loc0 channel", state.p2_vch_chan_num, 1);
    rc |= expect_int("isch loc0 offset", state.p2_scramble_offset, 9);

    g_isch_lookup_result = (1 << 5) | (1 << 3);
    state.p2_scramble_offset = -1;
    p25p2_test_process_isch(&opts, &state, 1);
    rc |= expect_int("isch loc1 offset", state.p2_scramble_offset, 3);

    g_isch_lookup_result = (1 << 5) | (2 << 3);
    state.p2_scramble_offset = -1;
    p25p2_test_process_isch(&opts, &state, 2);
    rc |= expect_int("isch loc2 offset", state.p2_scramble_offset, 6);

    g_isch_lookup_result = -1;
    state.p2_vch_chan_num = 7;
    state.p2_scramble_offset = 42;
    p25p2_test_process_isch(&opts, &state, 0);
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
    rc |= expect_int("duid exact with unreliable metrics", p25p2_duid_lookup_soft_test(0x17U, unreliable), 1);
    rc |= expect_int("duid null metrics uses hard table", p25p2_duid_lookup_soft_test(0x17U, NULL), 1);

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

    p25p2_test_process_isch(&opts, &state, 0);

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
    state->p25_p2_enc_lockout_muted[0] = 1;
    state->p25_p2_enc_lockout_muted[1] = 1;
    state->p25_p2_audio_ring_count[0] = 2;
    state->p25_p2_audio_ring_count[1] = 3;
    state->voice_counter[0] = 7;
    state->voice_counter[1] = 9;
    state->p25_p2_last_mac_active[0] = 111;
    state->p25_p2_last_mac_active[1] = 222;
    state->p25_p2_last_end_ptt[0] = 333;
    state->p25_p2_last_end_ptt[1] = 444;
    state->p25_call_is_packet[0] = 1;
    state->p25_call_is_packet[1] = 1;
    state->p25_call_emergency[0] = 1;
    state->p25_call_emergency[1] = 1;
    state->p25_call_priority[0] = 5;
    state->p25_call_priority[1] = 6;
    state->payload_algid = 0x81;
    state->payload_keyid = 0x2468;
    state->payload_miP = 0x1122334455667788ULL;
    state->payload_algidR = 0x84;
    state->payload_keyidR = 0x1357;
    state->payload_miN = 0x0123456789ABCDEFULL;
    DSD_SNPRINTF(state->call_string[0], sizeof state->call_string[0], "%s", "left call");
    DSD_SNPRINTF(state->call_string[1], sizeof state->call_string[1], "%s", "right call");
}

static int
expect_teardown_common_reset(const dsd_state* state) {
    int rc = 0;
    rc |= expect_int("teardown gate left", state->p25_p2_audio_allowed[0], 0);
    rc |= expect_int("teardown gate right", state->p25_p2_audio_allowed[1], 0);
    rc |= expect_int("teardown marker left", state->p25_p2_enc_lockout_muted[0], 0);
    rc |= expect_int("teardown marker right", state->p25_p2_enc_lockout_muted[1], 0);
    rc |= expect_int("teardown ring left", state->p25_p2_audio_ring_count[0], 0);
    rc |= expect_int("teardown ring right", state->p25_p2_audio_ring_count[1], 0);
    rc |= expect_int("teardown voice counter left", state->voice_counter[0], 0);
    rc |= expect_int("teardown voice counter right", state->voice_counter[1], 0);
    rc |= expect_int("teardown mac active left", (int)state->p25_p2_last_mac_active[0], 0);
    rc |= expect_int("teardown mac active right", (int)state->p25_p2_last_mac_active[1], 0);
    rc |= expect_int("teardown end ptt left", (int)state->p25_p2_last_end_ptt[0], 0);
    rc |= expect_int("teardown end ptt right", (int)state->p25_p2_last_end_ptt[1], 0);
    rc |= expect_int("teardown packet left", state->p25_call_is_packet[0], 0);
    rc |= expect_int("teardown packet right", state->p25_call_is_packet[1], 0);
    rc |= expect_int("teardown emergency left", state->p25_call_emergency[0], 0);
    rc |= expect_int("teardown emergency right", state->p25_call_emergency[1], 0);
    rc |= expect_int("teardown priority left", state->p25_call_priority[0], 0);
    rc |= expect_int("teardown priority right", state->p25_call_priority[1], 0);
    rc |= expect_int("teardown alg left", state->payload_algid, 0);
    rc |= expect_int("teardown key left", state->payload_keyid, 0);
    rc |= expect_int("teardown mi left", state->payload_miP != 0ULL, 0);
    rc |= expect_int("teardown alg right", state->payload_algidR, 0);
    rc |= expect_int("teardown key right", state->payload_keyidR, 0);
    rc |= expect_int("teardown mi right", state->payload_miN != 0ULL, 0);
    rc |= expect_str("teardown call string left", state->call_string[0], "                     ");
    rc |= expect_str("teardown call string right", state->call_string[1], "                     ");
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

    opts.floating_point = 0;
    opts.pulse_digi_rate_out = 8000;
    seed_teardown_dirty_state(&state);
    state.s_l4[2][17] = 123;
    state.s_r4[4][31] = -234;

    p25p2_test_teardown_call(&opts, &state);

    int rc = 0;
    rc |= expect_int("teardown playback calls", g_ss18_calls, 1);
    rc |= expect_int("teardown playback gate left", g_ss18_allowed_l, 1);
    rc |= expect_int("teardown playback gate right", g_ss18_allowed_r, 1);
    rc |= expect_teardown_common_reset(&state);
    rc |= expect_s16_clear("teardown clear left short audio", state.s_l4);
    rc |= expect_s16_clear("teardown clear right short audio", state.s_r4);

    if (rc == 0) {
        printf("PASS\n");
    } else {
        printf("FAIL\n");
    }
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

    opts.floating_point = 0;
    opts.pulse_digi_rate_out = 8000;
    seed_teardown_dirty_state(&state);

    p25p2_test_teardown_call(&opts, &state);

    int rc = 0;
    rc |= expect_int("teardown no-audio playback calls", g_ss18_calls, 0);
    rc |= expect_int("teardown no-audio playback left unchanged", g_ss18_allowed_l, -1);
    rc |= expect_int("teardown no-audio playback right unchanged", g_ss18_allowed_r, -1);
    rc |= expect_teardown_common_reset(&state);

    if (rc == 0) {
        printf("PASS\n");
    } else {
        printf("FAIL\n");
    }
    return rc;
}

static int
test_duid_invalid_burst_aborts_and_clears_crypto_state(void) {
    printf("Test 27: DUID repeated invalid bursts abort and clear crypto state... ");
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
    state.p25_p2_enc_lockout_muted[0] = 1;
    state.p25_p2_enc_lockout_muted[1] = 1;
    state.fourv_counter[0] = 3;
    state.fourv_counter[1] = 4;
    state.voice_counter[0] = 5;
    state.voice_counter[1] = 6;
    seed_duid_bits(0, 0x03U);
    seed_duid_bits(1, 0x03U);

    p25p2_test_process_p2_duid(&opts, &state);

    int rc = 0;
    rc |= expect_int("invalid abort alg left", state.payload_algid, 0);
    rc |= expect_int("invalid abort key left", state.payload_keyid, 0);
    rc |= expect_int("invalid abort alg right", state.payload_algidR, 0);
    rc |= expect_int("invalid abort key right", state.payload_keyidR, 0);
    rc |= expect_int("invalid abort lcch", state.p2_is_lcch, 0);
    rc |= expect_int("invalid abort marker left", state.p25_p2_enc_lockout_muted[0], 0);
    rc |= expect_int("invalid abort marker right", state.p25_p2_enc_lockout_muted[1], 0);
    rc |= expect_int("invalid abort fourv left", state.fourv_counter[0], 0);
    rc |= expect_int("invalid abort fourv right", state.fourv_counter[1], 0);
    rc |= expect_int("invalid abort voice left", state.voice_counter[0], 0);
    rc |= expect_int("invalid abort voice right", state.voice_counter[1], 0);
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
    printf("Test 28: DUID LCCH release defers during VC grace... ");
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_xcch_stubs();
    prepare_lcch_release_duids();

    time_t now = time(NULL);
    opts.floating_point = 0;
    opts.pulse_digi_rate_out = 8000;
    opts.p25_trunk = 1;
    opts.p25_is_tuned = 1;
    opts.trunk_hangtime = 1;
    state.currentslot = 0;
    state.last_vc_sync_time = now - 10;
    state.p25_last_vc_tune_time = now;
    state.p25_cfg_vc_grace_s = 60.0;

    p25p2_test_process_p2_duid(&opts, &state);

    int rc = 0;
    rc |= expect_int("grace release force", state.p25_sm_force_release, 0);
    rc |= expect_int("grace tuned remains", opts.p25_is_tuned, 1);
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
    printf("Test 29: DUID LCCH release tears down after VC grace... ");
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
    opts.p25_trunk = 1;
    opts.p25_is_tuned = 1;
    opts.trunk_hangtime = 1;
    state.currentslot = 0;
    state.last_vc_sync_time = now - 10;
    state.p25_last_vc_tune_time = now - 10;
    state.p25_cfg_vc_grace_s = 0.25;
    seed_teardown_dirty_state(&state);
    state.last_vc_sync_time = now - 10;
    state.p25_last_vc_tune_time = now - 10;
    state.p25_cfg_vc_grace_s = 0.25;

    p25p2_test_process_p2_duid(&opts, &state);

    int rc = 0;
    rc |= expect_int("post-grace release force", state.p25_sm_force_release, 1);
    rc |= expect_teardown_common_reset(&state);
    rc |= expect_int("post-grace sacch dispatches", g_sacch_mac_calls >= 1, 1);
    if (rc == 0) {
        printf("PASS\n");
    } else {
        printf("FAIL\n");
    }
    return rc;
}

int
main(void) {
    int failures = 0;
    reset_ess_stubs();
    set_p25p2_threshold(64);

    printf("P25P2 Soft Metric Buffer Tests\n");
    printf("===============================\n\n");

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

    /* Test 2: Buffer sizes are consistent with 700-dibit/1400-bit capture scope */
    printf("Test 2: Buffer size consistency... ");
    size_t p2bit_size = sizeof(p2bit) / sizeof(p2bit[0]);
    size_t p2llr_size = sizeof(p2llr) / sizeof(p2llr[0]);
    size_t p2xllr_size = sizeof(p2xllr) / sizeof(p2xllr[0]);

    if (p2bit_size == 4320 && p2llr_size == 1400 && p2xllr_size == 1400) {
        printf("PASS (p2bit=4320 bits, llr=1400 bits)\n");
    } else {
        printf("FAIL (p2bit=%zu, p2llr=%zu, p2xllr=%zu)\n", p2bit_size, p2llr_size, p2xllr_size);
        failures++;
    }

    /* Test 3: Soft-decision buffers are distinct from bit buffers */
    printf("Test 3: Buffer address separation... ");
    void* p_p2bit = (void*)p2bit;
    void* p_p2llr = (void*)p2llr;
    void* p_p2xllr = (void*)p2xllr;
    if (p_p2bit != p_p2llr && p_p2bit != p_p2xllr && p_p2llr != p_p2xllr) {
        printf("PASS\n");
    } else {
        printf("FAIL (overlapping buffers)\n");
        failures++;
    }

    /* Test 4: LLR descramble preserves confidence magnitudes */
    printf("Test 4: LLR descramble preserves magnitudes... ");
    p25_p2_frame_reset();

    /* Simulate soft values from dibit capture */
    for (int i = 0; i < 1400; i++) {
        p2llr[i] = (int16_t)((i & 1) ? -(100 + (i % 50)) : (100 + (i % 50)));
    }

    /* Manually transform to p2xllr as process_Frame_Scramble would. */
    for (int i = 0; i < 1400; i++) {
        p2xllr[i] = (i % 3) == 0 ? (int16_t)-p2llr[i] : p2llr[i];
    }

    int mismatch = 0;
    for (int i = 0; i < 1400; i++) {
        int p2_mag = p2llr[i] < 0 ? -p2llr[i] : p2llr[i];
        int p2x_mag = p2xllr[i] < 0 ? -p2xllr[i] : p2xllr[i];
        if (p2x_mag != p2_mag) {
            mismatch++;
        }
    }
    if (mismatch == 0) {
        printf("PASS\n");
    } else {
        printf("FAIL (%d mismatches)\n", mismatch);
        failures++;
    }

    /* Test 5: P2 DUID soft fallback only recovers invalid hard decisions */
    printf("Test 5: DUID soft fallback preserves valid hard decisions... ");
    uint8_t duid_reliab[8] = {200, 200, 200, 200, 200, 200, 200, 5};
    int valid_decoded = p25p2_duid_lookup_soft_test(0x07U, duid_reliab);
    if (valid_decoded == 1) {
        printf("PASS\n");
    } else {
        printf("FAIL (expected 1, got %d)\n", valid_decoded);
        failures++;
    }

    printf("Test 6: DUID soft fallback uses weakest invalid bits... ");
    DSD_MEMSET(duid_reliab, 200, sizeof(duid_reliab));
    duid_reliab[6] = 5;
    duid_reliab[7] = 5; /* 0x03 -> 0x00 is the cheapest canonical candidate. */
    int recovered_decoded = p25p2_duid_lookup_soft_test(0x03U, duid_reliab);
    if (recovered_decoded == 0) {
        printf("PASS\n");
    } else {
        printf("FAIL (expected 0, got %d)\n", recovered_decoded);
        failures++;
    }

    printf("Test 7: DUID soft fallback rejects high-confidence invalid bits... ");
    DSD_MEMSET(duid_reliab, 200, sizeof(duid_reliab));
    int high_confidence_decoded = p25p2_duid_lookup_soft_test(0x03U, duid_reliab);
    if (high_confidence_decoded == -1) {
        printf("PASS\n");
    } else {
        printf("FAIL (expected -1, got %d)\n", high_confidence_decoded);
        failures++;
    }

    printf("Test 8: DUID soft fallback recovers weak 0x80 MSB... ");
    DSD_MEMSET(duid_reliab, 200, sizeof(duid_reliab));
    duid_reliab[0] = 5;
    int sentinel_decoded = p25p2_duid_lookup_soft_test(0x80U, duid_reliab);
    if (sentinel_decoded == 0) {
        printf("PASS\n");
    } else {
        printf("FAIL (expected 0, got %d)\n", sentinel_decoded);
        failures++;
    }

    printf("Test 9: DUID soft fallback preserves high-confidence 0x80 guard... ");
    DSD_MEMSET(duid_reliab, 200, sizeof(duid_reliab));
    sentinel_decoded = p25p2_duid_lookup_soft_test(0x80U, duid_reliab);
    if (sentinel_decoded == -1) {
        printf("PASS\n");
    } else {
        printf("FAIL (expected -1, got %d)\n", sentinel_decoded);
        failures++;
    }

    printf("Test 10: DUID soft fallback rejects weak non-MSB 0x80 guard... ");
    DSD_MEMSET(duid_reliab, 200, sizeof(duid_reliab));
    duid_reliab[0] = 5;
    duid_reliab[1] = 5;
    sentinel_decoded = p25p2_duid_lookup_soft_test(0x80U, duid_reliab);
    if (sentinel_decoded == -1) {
        printf("PASS\n");
    } else {
        printf("FAIL (expected -1, got %d)\n", sentinel_decoded);
        failures++;
    }

    printf("Test 11: DUID soft fallback rejects tied frame candidates... ");
    DSD_MEMSET(duid_reliab, 200, sizeof(duid_reliab));
    duid_reliab[3] = 5; /* 0x03 -> 0x17 decodes to 1. */
    duid_reliab[5] = 5;
    duid_reliab[6] = 5; /* 0x03 -> 0x00 decodes to 0. */
    duid_reliab[7] = 5;
    int tied_decoded = p25p2_duid_lookup_soft_test(0x03U, duid_reliab);
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
    failures += test_duid_invalid_burst_aborts_and_clears_crypto_state();
    failures += test_duid_lcch_release_defers_during_vc_grace();
    failures += test_duid_lcch_release_tears_down_after_vc_grace();

    printf("\n%d test(s) failed\n", failures);
    return failures > 0 ? 1 : 0;
}
#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
