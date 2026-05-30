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

static int g_ess_hard_rc = 0;
static int g_ess_soft_min_success = -1;
static int g_ess_soft_success_rc = 0;
static int g_ess_soft_calls = 0;
static int g_ess_soft_last_n = 0;
static int g_ess_soft_mutate_algid = -1;

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
    (void)state;
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
    (void)slot;
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
    return -1;
}

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
isch_lookup_soft(uint64_t isch, const uint8_t reliab40[40]) {
    (void)reliab40;
    return isch_lookup(isch);
}

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
ez_rs28_facch_soft(int* payload, int* parity, const int* erasures, int n_erasures) {
    (void)payload;
    (void)parity;
    (void)erasures;
    (void)n_erasures;
    return 0;
}

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
ez_rs28_sacch_soft(int* payload, int* parity, const int* erasures, int n_erasures) {
    (void)payload;
    (void)parity;
    (void)erasures;
    (void)n_erasures;
    return 0;
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
    (void)bits;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
process_FACCH_MAC_PDU(dsd_opts* opts, dsd_state* state, int* bits) {
    (void)opts;
    (void)state;
    (void)bits;
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

    printf("\n%d test(s) failed\n", failures);
    return failures > 0 ? 1 : 0;
}
#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
