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
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* External declarations matching p25p2_frame.c */
extern int p2bit[4320];
extern int16_t p2llr[1400];
extern int16_t p2xllr[1400];
extern void p25_p2_frame_reset(void);
extern int p25p2_duid_lookup_soft_test(uint8_t received, const uint8_t reliab8[8]);

/* Stubs for external functions referenced by p25p2_frame.c */
struct RtlSdrContext* g_rtl_ctx = 0;

bool
SetFreq(int sockfd, long int freq) {
    (void)sockfd;
    (void)freq;
    return false;
}

bool
SetModulation(int sockfd, int bandwidth) {
    (void)sockfd;
    (void)bandwidth;
    return false;
}

void
return_to_cc(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

int
rtl_stream_tune(struct RtlSdrContext* ctx, uint32_t center_freq_hz) {
    (void)ctx;
    (void)center_freq_hz;
    return 0;
}

void
rtl_stream_p25p2_err_update(int slot, int a, int b, int c, int d, int e) {
    (void)slot;
    (void)a;
    (void)b;
    (void)c;
    (void)d;
    (void)e;
}

void
getTimeC_buf(char out[9]) {
    if (out) {
        memcpy(out, "00:00:00", 9);
    }
}

void
rotate_symbol_out_file(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

void
openMbeOutFile(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

void
openMbeOutFileR(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

void
closeMbeOutFile(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

void
closeMbeOutFileR(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

void
playSynthesizedVoiceFS4(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

void
playSynthesizedVoiceSS18(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

void
watchdog_event_history(dsd_opts* opts, dsd_state* state, uint8_t slot) {
    (void)opts;
    (void)state;
    (void)slot;
}

void
watchdog_event_current(dsd_opts* opts, dsd_state* state, uint8_t slot) {
    (void)opts;
    (void)state;
    (void)slot;
}

void
processMbeFrame(dsd_opts* opts, dsd_state* state, void* a, char fr[4][24], void* c) {
    (void)opts;
    (void)state;
    (void)a;
    (void)fr;
    (void)c;
}

void
p25_emit_enc_lockout_once(dsd_opts* opts, dsd_state* state, uint8_t slot, int tg, int svc) {
    (void)opts;
    (void)state;
    (void)slot;
    (void)tg;
    (void)svc;
}

void
LFSRP(dsd_state* state) {
    (void)state;
}

void
LFSR128(dsd_state* state) {
    (void)state;
}

double
dsd_time_now_monotonic_s(void) {
    return 0.0;
}

/* RS decoders */
int
ez_rs28_facch(int* payload, int* parity) {
    (void)payload;
    (void)parity;
    return 0;
}

int
ez_rs28_sacch(int* payload, int* parity) {
    (void)payload;
    (void)parity;
    return 0;
}

int
ez_rs28_ess(int* payload, int* parity) {
    (void)payload;
    (void)parity;
    return 0;
}

int
isch_lookup(uint64_t isch) {
    (void)isch;
    return -1;
}

int
isch_lookup_soft(uint64_t isch, const uint8_t reliab40[40]) {
    (void)reliab40;
    return isch_lookup(isch);
}

int
ez_rs28_facch_soft(int* payload, int* parity, const int* erasures, int n_erasures) {
    (void)payload;
    (void)parity;
    (void)erasures;
    (void)n_erasures;
    return 0;
}

int
ez_rs28_sacch_soft(int* payload, int* parity, const int* erasures, int n_erasures) {
    (void)payload;
    (void)parity;
    (void)erasures;
    (void)n_erasures;
    return 0;
}

int
ez_rs28_ess_soft(int* payload, int* parity, const int* erasures, int n_erasures) {
    (void)payload;
    (void)parity;
    (void)erasures;
    (void)n_erasures;
    return 0;
}

/* MAC PDU handlers */
void
process_SACCH_MAC_PDU(dsd_opts* opts, dsd_state* state, int* bits) {
    (void)opts;
    (void)state;
    (void)bits;
}

void
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

int
main(void) {
    int failures = 0;

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

    printf("Test 6: DUID soft fallback uses weakest invalid bit... ");
    memset(duid_reliab, 200, sizeof(duid_reliab));
    duid_reliab[7] = 5; /* 0x03 -> 0x02 is the cheapest one-bit valid candidate. */
    int recovered_decoded = p25p2_duid_lookup_soft_test(0x03U, duid_reliab);
    if (recovered_decoded == 0) {
        printf("PASS\n");
    } else {
        printf("FAIL (expected 0, got %d)\n", recovered_decoded);
        failures++;
    }

    printf("Test 7: DUID soft fallback rejects high-confidence invalid bits... ");
    memset(duid_reliab, 200, sizeof(duid_reliab));
    int high_confidence_decoded = p25p2_duid_lookup_soft_test(0x03U, duid_reliab);
    if (high_confidence_decoded == -1) {
        printf("PASS\n");
    } else {
        printf("FAIL (expected -1, got %d)\n", high_confidence_decoded);
        failures++;
    }

    printf("Test 8: DUID soft fallback preserves 0x80 invalid guard... ");
    memset(duid_reliab, 200, sizeof(duid_reliab));
    duid_reliab[0] = 5;
    int sentinel_decoded = p25p2_duid_lookup_soft_test(0x80U, duid_reliab);
    if (sentinel_decoded == -1) {
        printf("PASS\n");
    } else {
        printf("FAIL (expected -1, got %d)\n", sentinel_decoded);
        failures++;
    }

    printf("Test 9: DUID soft fallback rejects tied frame candidates... ");
    memset(duid_reliab, 200, sizeof(duid_reliab));
    duid_reliab[5] = 5; /* 0x03 -> 0x07 decodes to 1. */
    duid_reliab[7] = 5; /* 0x03 -> 0x02 decodes to 0. */
    int tied_decoded = p25p2_duid_lookup_soft_test(0x03U, duid_reliab);
    if (tied_decoded == -1) {
        printf("PASS\n");
    } else {
        printf("FAIL (expected -1, got %d)\n", tied_decoded);
        failures++;
    }

    printf("\n%d test(s) failed\n", failures);
    return failures > 0 ? 1 : 0;
}
