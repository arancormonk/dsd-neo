// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * P25 Phase 1 TDULC parser test → LCW retune (format 0x44).
 *
 * Feeds deterministic 6×12-bit data words via read_word() stub to form an LCW
 * with format 0x44, service=0x00, TG=0x4567, CHAN-T=0x100A. Stubs FEC and
 * analog readers to bypass error correction. Asserts trunk SM gets a group
 * grant when LCW retune is enabled and CC is known.
 */

#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm_api.h>
#include <dsd-neo/protocol/p25/p25p1_soft.h>
#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

void processTDULC(dsd_opts* opts, dsd_state* state);

// Trunk SM hooks capture
static int g_called = 0;
static int g_channel = -1;
static int g_svc = -1;
static int g_tg = -1;
static int g_src = -1;

static void
sm_noop_init(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

static void
sm_on_group_grant_capture(dsd_opts* opts, dsd_state* state, int channel, int svc_bits, int tg, int src) {
    (void)opts;
    (void)state;
    g_called++;
    g_channel = channel;
    g_svc = svc_bits;
    g_tg = tg;
    g_src = src;
}

static void
sm_noop_on_indiv_grant(dsd_opts* opts, dsd_state* state, int channel, int svc_bits, int dst, int src) {
    (void)opts;
    (void)state;
    (void)channel;
    (void)svc_bits;
    (void)dst;
    (void)src;
}

static void
sm_noop_on_release(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

static void
sm_noop_on_neighbor_update(dsd_opts* opts, dsd_state* state, const long* freqs, int count) {
    (void)opts;
    (void)state;
    (void)freqs;
    (void)count;
}

static void
sm_noop_tick(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

static int
sm_noop_next_cc_candidate(dsd_state* state, long* out_freq) {
    (void)state;
    (void)out_freq;
    return 0;
}

static p25_sm_api
sm_test_api(void) {
    p25_sm_api api = {0};
    api.init = sm_noop_init;
    api.on_group_grant = sm_on_group_grant_capture;
    api.on_indiv_grant = sm_noop_on_indiv_grant;
    api.on_release = sm_noop_on_release;
    api.on_neighbor_update = sm_noop_on_neighbor_update;
    api.next_cc_candidate = sm_noop_next_cc_candidate;
    api.tick = sm_noop_tick;
    return api;
}

// Alias helpers referenced by LCW path
void
// NOLINTNEXTLINE(misc-use-internal-linkage)
apx_embedded_alias_header_phase1(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t* lc_bits) {
    (void)opts;
    (void)state;
    (void)slot;
    (void)lc_bits;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
apx_embedded_alias_blocks_phase1(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t* lc_bits) {
    (void)opts;
    (void)state;
    (void)slot;
    (void)lc_bits;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
l3h_embedded_alias_blocks_phase1(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t* lc_bits) {
    (void)opts;
    (void)state;
    (void)slot;
    (void)lc_bits;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
apx_embedded_gps(dsd_opts* opts, dsd_state* state, uint8_t* lc_bits) {
    (void)opts;
    (void)state;
    (void)lc_bits;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
nmea_harris(dsd_opts* opts, dsd_state* state, uint8_t* input, uint32_t src, int slot) {
    (void)opts;
    (void)state;
    (void)input;
    (void)src;
    (void)slot;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
tait_iso7_embedded_alias_decode(dsd_opts* opts, dsd_state* state, uint8_t slot, int16_t len, uint8_t* input) {
    (void)opts;
    (void)state;
    (void)slot;
    (void)len;
    (void)input;
}

// Minimal utility used by p25_lcw (MSB-first)
uint64_t
// NOLINTNEXTLINE(misc-use-internal-linkage)
ConvertBitIntoBytes(const uint8_t* BufferIn, uint32_t BitLength) {
    uint64_t v = 0;
    for (uint32_t i = 0; i < BitLength; i++) {
        v = (v << 1) | (uint64_t)(BufferIn[i] & 1);
    }
    return v;
}

// FEC stubs (bypass corrections)
int
// NOLINTNEXTLINE(misc-use-internal-linkage)
check_and_fix_golay_24_12(char* dodeca, char* parity, int* fixed_errors) {
    (void)dodeca;
    (void)parity;
    if (fixed_errors) {
        *fixed_errors = 0;
    }
    return 0; // no irrecoverable errors
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
encode_golay_24_12(char* data, char* parity) {
    (void)data;
    (void)parity;
}

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
check_and_fix_reedsolomon_24_12_13(char* data, char* parity) {
    (void)data;
    (void)parity;
    return 0; // no irrecoverable errors
}

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
check_and_fix_reedsolomon_24_12_13_soft(char* data, char* parity, const int* erasures, int n_erasures) {
    (void)data;
    (void)parity;
    (void)erasures;
    (void)n_erasures;
    return 0;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
encode_reedsolomon_24_12_13(char* data, char* parity) {
    (void)data;
    (void)parity;
}

int
getDibit(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    return 0;
}

int
getDibitSoft(dsd_opts* opts, dsd_state* state, dsd_dibit_soft_t* out_soft) {
    (void)opts;
    (void)state;
    if (out_soft) {
        out_soft->reliability = 255;
        out_soft->llr[0] = -255;
        out_soft->llr[1] = -255;
    }
    return 0;
}

// Scripted 12-bit words fed into read_word() in the order TDULC expects:
// dodeca_data[5]..[0], then dodeca_parity[5]..[0]
static char g_words[12][12];
static int g_word_index = 0;

// Helper: write MSB-first bits of an 8- or 16-bit value into an array
static void
bits_from_u16(uint16_t v, int nbits, char* out_bits) {
    for (int i = 0; i < nbits; i++) {
        out_bits[i] = (char)((v >> (nbits - 1 - i)) & 1);
    }
}

// Build the 6×12-bit data words for LCW format 0x44, MFID=0x00, SVC, TG, CHAN-T, CHAN-R.
static void
build_lcw_words(uint8_t lc_format, uint8_t mfid, uint8_t svc, uint16_t group1, uint16_t channelt, uint16_t channelr) {
    // Prepare bit arrays
    char fmt8[8], mf8[8], sv8[8], tg16[16], ct16[16], cr16[16];
    bits_from_u16(lc_format, 8, fmt8);
    bits_from_u16(mfid, 8, mf8);
    bits_from_u16(svc, 8, sv8);
    bits_from_u16(group1, 16, tg16);
    bits_from_u16(channelt, 16, ct16);
    bits_from_u16(channelr, 16, cr16);

    // Clear all words
    DSD_MEMSET(g_words, 0, sizeof(g_words));

    // Map into dodeca_data[5..0] per TDULC packing
    // data[5]
    for (int i = 0; i < 8; i++) {
        g_words[0][i] = fmt8[i]; // lcformat[0..7]
    }
    for (int i = 0; i < 4; i++) {
        g_words[0][8 + i] = mf8[i]; // mfid bits 0..3
    }
    // data[4]
    for (int i = 0; i < 4; i++) {
        g_words[1][i] = mf8[4 + i]; // mfid bits 4..7
    }
    for (int i = 0; i < 8; i++) {
        g_words[1][4 + i] = sv8[i]; // svc 8 bits
    }
    // data[3]
    for (int i = 0; i < 12; i++) {
        g_words[2][i] = tg16[i]; // group bits [0..11]
    }
    // data[2]
    for (int i = 0; i < 4; i++) {
        g_words[3][i] = tg16[12 + i]; // group bits [12..15]
    }
    for (int i = 0; i < 8; i++) {
        g_words[3][4 + i] = ct16[i]; // channelt bits [0..7]
    }
    // data[1]
    for (int i = 0; i < 8; i++) {
        g_words[4][i] = ct16[8 + i]; // channelt bits [8..15]
    }
    for (int i = 0; i < 4; i++) {
        g_words[4][8 + i] = cr16[i]; // channelr bits [0..3]
    }
    // data[0]
    for (int i = 0; i < 12; i++) {
        g_words[5][i] = cr16[4 + i]; // channelr bits [4..15]
    }

    // Shift the assembled data words into read order: index 0..5 should be data[5]..data[0]
    char ordered[6][12];
    DSD_MEMCPY(ordered[0], g_words[0], 12); // data[5]
    DSD_MEMCPY(ordered[1], g_words[1], 12); // data[4]
    DSD_MEMCPY(ordered[2], g_words[2], 12); // data[3]
    DSD_MEMCPY(ordered[3], g_words[3], 12); // data[2]
    DSD_MEMCPY(ordered[4], g_words[4], 12); // data[1]
    DSD_MEMCPY(ordered[5], g_words[5], 12); // data[0]
    DSD_MEMCPY(g_words, ordered, sizeof(ordered));

    // Parity words (not used by stubs): fill with zeros for indices 6..11 in read order parity[5]..[0]
    for (int w = 6; w < 12; w++) {
        DSD_MEMSET(g_words[w], 0, 12);
    }

    g_word_index = 0;
}

// Reader stubs used by TDULC
void
// NOLINTNEXTLINE(misc-use-internal-linkage)
read_word(dsd_opts* opts, dsd_state* state, char* word, unsigned int length, int* status_count,
          P25P1SoftDibit* soft_dibits, int* soft_dibit_index) {
    (void)opts;
    (void)state;
    (void)status_count;
    (void)soft_dibits;
    (void)soft_dibit_index;
    if (length != 12 || g_word_index >= 12) {
        DSD_MEMSET(word, 0, length);
        return;
    }
    DSD_MEMCPY(word, g_words[g_word_index], 12);
    g_word_index++;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
read_golay24_parity(dsd_opts* opts, dsd_state* state, char* parity, int* status_count, P25P1SoftDibit* soft_dibits,
                    int* soft_dibit_index) {
    (void)opts;
    (void)state;
    (void)status_count;
    (void)soft_dibits;
    (void)soft_dibit_index;
    DSD_MEMSET(parity, 0, 12);
}

static int
expect_eq_int(const char* tag, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %d want %d\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
expect_true(const char* tag, int ok) {
    if (!ok) {
        DSD_FPRINTF(stderr, "%s: condition failed\n", tag);
        return 1;
    }
    return 0;
}

static int
expect_eq_float(const char* tag, float got, float want) {
    const float delta = got - want;
    const float abs_delta = (delta < 0.0f) ? -delta : delta;
    if (!(abs_delta <= 0.0001f)) {
        DSD_FPRINTF(stderr, "%s: got %.3f want %.3f\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
expect_blank_call_string(const char* tag, const char* value) {
    for (int i = 0; i < 21; i++) {
        if (value[i] != ' ') {
            DSD_FPRINTF(stderr, "%s: byte %d got 0x%02X want 0x20\n", tag, i, (unsigned char)value[i]);
            return 1;
        }
    }
    if (value[21] != '\0') {
        DSD_FPRINTF(stderr, "%s: byte 21 got 0x%02X want NUL\n", tag, (unsigned char)value[21]);
        return 1;
    }
    return 0;
}

int
main(void) {
    int rc = 0;

    {
        p25_sm_api api = sm_test_api();
        p25_sm_set_api(&api);
    }

    // Case 1: Retune enabled (baseline)
    build_lcw_words(0x44, 0x00, 0x00, 0x4567, 0x100A, 0x0000);
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.p25_trunk = 1;
    opts.p25_lcw_retune = 1;
    opts.trunk_tune_group_calls = 1;
    opts.trunk_tune_enc_calls = 1;
    opts.p25_is_tuned = 0;
    opts.floating_point = 1;
    opts.audio_gain = 3.5F;
    state.p25_cc_freq = 851000000;
    state.tg_hold = 0;
    int lastsrc = 0x00ABCDEF;
    state.lastsrc = (unsigned long long)lastsrc;
    state.synctype = DSD_SYNC_P25P1_POS;
    state.p25_chan_iden = 1;
    state.p25_iden_fdma[1].chan_type = 1;
    state.p25_iden_fdma[1].chan_spac = 100;
    state.p25_iden_fdma[1].base_freq = 851000000 / 5;
    state.p25_iden_fdma[1].trust = 2;
    state.p25_iden_fdma[1].populated = 1;
    state.p25_chan_tdma_explicit[1] = 1; // FDMA known
    state.p25_call_emergency[0] = 1;
    state.p25_call_priority[0] = 7;
    state.p25_call_is_packet[0] = 1;
    state.aout_gain = 0.25F;
    DSD_SNPRINTF(state.call_string[0], sizeof(state.call_string[0]), "%s", "left active");
    DSD_SNPRINTF(state.call_string[1], sizeof(state.call_string[1]), "%s", "right active");
    g_called = 0;
    processTDULC(&opts, &state);
    rc |= expect_eq_int("grant called", g_called, 1);
    rc |= expect_eq_int("grant channel", g_channel, 0x100A);
    rc |= expect_eq_int("grant svc", g_svc, 0x00);
    rc |= expect_eq_int("grant tg", g_tg, 0x4567);
    rc |= expect_eq_int("grant src", g_src, lastsrc);
    rc |= expect_eq_int("tdulc duid count", (int)state.p25_p1_duid_tdulc, 1);
    rc |= expect_eq_int("tdulc emergency cleared", state.p25_call_emergency[0], 0);
    rc |= expect_eq_int("tdulc priority cleared", state.p25_call_priority[0], 0);
    rc |= expect_eq_int("tdulc packet cleared", state.p25_call_is_packet[0], 0);
    rc |= expect_blank_call_string("tdulc left call string blanked", state.call_string[0]);
    rc |= expect_blank_call_string("tdulc right call string blanked", state.call_string[1]);
    rc |= expect_eq_float("tdulc gain reset", state.aout_gain, opts.audio_gain);
    rc |= expect_true("tdulc wall time recorded", state.p25_p1_last_tdu != 0);
    rc |= expect_true("tdulc monotonic time recorded", state.p25_p1_last_tdu_m > 0.0);
    rc |= expect_true("tdulc vc sync time refreshed", state.last_vc_sync_time_m > 0.0);

    // Case 2: Retune disabled → no grant
    build_lcw_words(0x44, 0x00, 0x00, 0x1234, 0x100A, 0x0000);
    opts.p25_lcw_retune = 0;
    g_called = 0;
    processTDULC(&opts, &state);
    rc |= expect_eq_int("retune disabled", g_called, 0);

    // Case 3: Encrypted svc, enc tuning disabled → no grant
    build_lcw_words(0x44, 0x00, 0x40 /*ENC*/, 0x2222, 0x100A, 0x0000);
    opts.p25_lcw_retune = 1;
    opts.trunk_tune_enc_calls = 0;
    g_called = 0;
    processTDULC(&opts, &state);
    rc |= expect_eq_int("enc gating", g_called, 0);

    // Case 4: Malformed/unsupported format (0x00) → no grant
    build_lcw_words(0x00, 0x00, 0x00, 0x3333, 0x100A, 0x0000);
    opts.trunk_tune_enc_calls = 1;
    g_called = 0;
    processTDULC(&opts, &state);
    rc |= expect_eq_int("unsupported format", g_called, 0);

    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
