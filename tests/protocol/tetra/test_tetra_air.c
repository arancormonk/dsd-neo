// SPDX-License-Identifier: GPL-3.0-or-later
/* Trunked mode air interface decryption with an operator's cipher key: initial value packing (EN 300 392-7 clause
 * 6.3.2.1), the encryption cipher key TB5 derives from it for the carrier being received (clause 6.3.2.2), the key
 * stream segment allocation of speech (table 6.4) and the TEA1/2/3 vectors.
 * Vectors are the published TEA1/2/3 cases from Midnight Blue's Apache-2.0 tests. */
#include <dsd-neo/core/key_set.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/crypto/tetra_tea.h>
#include <dsd-neo/protocol/tetra/tetra_air.h>
#include <dsd-neo/protocol/tetra/tetra_mac.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tetra_state.h"

static int g_fail = 0;

/* The receiver, tuned to carrier 700 of band 4 with offset -6,25 kHz (EN 300 392-2 clause 21.4.4.1). */
static dsd_opts* g_opts = NULL;
#define CARRIER    700u
#define CARRIER_HZ 417493750u
#define LA         0x1DCCu
#define COLOUR     5u

#define CHECK(cond, msg)                                                                                               \
    do {                                                                                                               \
        if (cond) {                                                                                                    \
            printf("  PASS: %s\n", msg);                                                                               \
        } else {                                                                                                       \
            printf("  FAIL: %s (line %d)\n", msg, __LINE__);                                                           \
            g_fail++;                                                                                                  \
        }                                                                                                              \
    } while (0)

static int
ks_eq(int alg, uint32_t iv, const uint8_t key[10], const uint8_t expect[10]) {
    uint8_t out[10];
    DSD_MEMSET(out, 0, sizeof(out));
    tetra_tea_keystream(alg, key, iv, out, 10);
    return memcmp(out, expect, 10) == 0;
}

/* Network 86/28 with colour code COLOUR and location area LA, on carrier CARRIER, the clock at TN 1, FN 1, MN 1 of
 * hyperframe 0, and the network's signalling naming KSG @p ksg (none when negative). */
static dsd_state*
new_cell_state(int ksg) {
    dsd_state* st = (dsd_state*)calloc(1, sizeof(dsd_state));
    tetra_state_t* ts = tetra_st(st);
    ts->net_known = 1;
    ts->mcc = 86;
    ts->mnc = 28;
    ts->colour = COLOUR;
    ts->sysinfo_known = 1;
    ts->la = LA;
    ts->freq_band = 4;
    ts->freq_offset = 2;
    ts->hyperframe_valid = 1;
    ts->tdma_valid = 1;
    ts->tn = 1;
    ts->fn = 1;
    ts->mn = 1;
    if (ksg >= 0) {
        tetra_air_ksg_remember(st, 86, 28, (uint8_t)ksg);
    }
    return st;
}

/* The cell of new_cell_state() with KSG TEA1 and an all-zero cipher key. */
static dsd_state*
new_encrypted_state(void) {
    dsd_state* st = new_cell_state(0);
    (void)dsd_key_apply_direct(st, DSD_KEY_TYPE_TETRA_AIR, "86,28,00000000000000000000", DSD_KEY_APPLY_OVERLAY);
    return st;
}

static void
free_state(dsd_state* st) {
    if (st) {
        dsd_state_ext_free_all(st);
    }
    free(st);
}

static void
test_vectors(void) {
    static const uint8_t zero_key[10] = {0};
    static const uint8_t tea1_key[10] = {0xA7, 0x98, 0x39, 0xE4, 0xBA, 0x88, 0xEE, 0x54, 0xA0, 0x29};
    static const uint8_t tea1_zero_ks[10] = {0xd3, 0x3f, 0xd8, 0xa6, 0x05, 0xa0, 0xa1, 0xbb, 0x90, 0x23};
    static const uint8_t tea1_ks[10] = {0x1d, 0xec, 0x9c, 0x7e, 0xc6, 0x22, 0x3d, 0x87, 0xc2, 0xcc};
    static const uint8_t tea2_key[10] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xAA};
    static const uint8_t tea2_zero_ks[10] = {0xA7, 0x98, 0x39, 0xE4, 0xBA, 0x88, 0xEE, 0x54, 0xA0, 0x29};
    static const uint8_t tea2_ks[10] = {0x64, 0x70, 0x4E, 0xA9, 0xD7, 0xDC, 0x25, 0x60, 0x81, 0x39};
    static const uint8_t tea3_zero_ks[10] = {0x06, 0xA6, 0x58, 0x8C, 0x5D, 0x9A, 0x99, 0x6D, 0xD2, 0x5E};
    static const uint8_t tea3_ks[10] = {0x02, 0x49, 0x1e, 0xf5, 0x57, 0xc5, 0x1c, 0x17, 0x73, 0x0c};

    printf("[initial value and TEA vectors]\n");
    CHECK(tetra_air_build_iv(1, 1, 1, 0, 0) == ((1u << 2) | (1u << 7)), "downlink IV packs slot frame and multiframe");
    CHECK(tetra_air_build_iv(4, 18, 60, 0x7FFF, 1) == (3u | (18u << 2) | (60u << 7) | (0x7FFFu << 13) | (1u << 28)),
          "uplink IV packs the direction bit");
    CHECK(tetra_air_build_iv(4, 18, 60, 0xFFFF, 0) == tetra_air_build_iv(4, 18, 60, 0x7FFF, 0),
          "only the low 15 hyperframe bits enter the IV");
    CHECK(tetra_air_build_iv(0, 1, 1, 0, 0) == 0 && tetra_air_build_iv(1, 0, 1, 0, 0) == 0
              && tetra_air_build_iv(1, 1, 0, 0, 0) == 0 && tetra_air_build_iv(1, 19, 1, 0, 0) == 0
              && tetra_air_build_iv(1, 1, 61, 0, 0) == 0,
          "out-of-range slot, frame or multiframe is not an IV");

    CHECK(ks_eq(0, 0x11111111u, zero_key, tea1_zero_ks), "TEA1 zero key vector");
    CHECK(ks_eq(0, 0x01234567u, tea1_key, tea1_ks), "TEA1 keyed vector");
    CHECK(ks_eq(1, 0x12345678u, zero_key, tea2_zero_ks), "TEA2 zero key vector");
    CHECK(ks_eq(1, 0x12345678u, tea2_key, tea2_ks), "TEA2 keyed vector");
    CHECK(ks_eq(2, 0x11111111u, zero_key, tea3_zero_ks), "TEA3 zero key vector");
    CHECK(ks_eq(2, 0x01234567u, tea1_key, tea3_ks), "TEA3 keyed vector");

    CHECK(tetra_air_ksg_name(0) != NULL && strcmp(tetra_air_ksg_name(3), "TEA4") == 0 && tetra_air_ksg_name(4) == NULL
              && tetra_air_ksg_name(-1) == NULL,
          "KSG names");
}

static void
test_preconditions(void) {
    uint8_t bits[8];
    uint8_t mode = TETRA_ENC_MODE_EVEN;
    dsd_state* st = (dsd_state*)calloc(1, sizeof(dsd_state));

    printf("[when speech is decrypted]\n");
    DSD_MEMSET(bits, 1, sizeof(bits));
    tetra_st(st)->net_known = 1;
    tetra_st(st)->mcc = 86;
    tetra_st(st)->mnc = 28;
    tetra_st(st)->colour = COLOUR;
    tetra_st(st)->sysinfo_known = 1;
    tetra_st(st)->la = LA;
    tetra_st(st)->freq_band = 4;
    tetra_st(st)->freq_offset = 2;
    tetra_st(st)->hyperframe_valid = 1;
    tetra_st(st)->tdma_valid = 1;
    tetra_st(st)->tn = 1;
    tetra_st(st)->fn = 1;
    tetra_st(st)->mn = 1;
    CHECK(tetra_air_decrypt_voice(g_opts, st, mode, bits, 8, 0) == 0, "encrypted voice without a KSG is unchanged");
    tetra_air_ksg_remember(st, 86, 28, 3);
    CHECK(tetra_air_decrypt_voice(g_opts, st, mode, bits, 8, 0) == 0, "TEA4 is named but not decrypted");
    /* A session whose signalling named TEA1 alone: TEA4 and TEA1 would be two algorithms (test_key_choice()). */
    DSD_MEMSET(tetra_st(st)->air_ksg, 0, sizeof(tetra_st(st)->air_ksg));
    tetra_air_ksg_remember(st, 86, 28, 0);
    CHECK(tetra_air_ksg_lookup(st, 86, 28) == 0 && tetra_air_ksg_lookup(st, 86, 29) == -1,
          "a KSG is remembered per network");
    CHECK(tetra_air_decrypt_voice(g_opts, st, mode, bits, 8, 0) == 0, "TEA1 without an operator key is unchanged");
    CHECK(dsd_key_apply_direct(st, DSD_KEY_TYPE_TETRA_AIR, "86,28,00000000000000000000", DSD_KEY_APPLY_OVERLAY)
              == DSD_KEY_DIRECT_OK,
          "an operator key loads for network 86/28");
    CHECK(tetra_air_decrypt_voice(g_opts, st, mode, bits, 8, 0) == 1, "TEA1 with a key decrypts trunked voice");

    tetra_st(st)->dmo = 1;
    DSD_MEMSET(bits, 1, sizeof(bits));
    CHECK(tetra_air_decrypt_voice(g_opts, st, mode, bits, 8, 0) == 0 && bits[0] == 1,
          "direct mode is not decrypted here");
    tetra_st(st)->dmo = 0;
    tetra_st(st)->tdma_valid = 0;
    CHECK(tetra_air_decrypt_voice(g_opts, st, mode, bits, 8, 0) == 0 && bits[0] == 1, "no TDMA clock, no IV");
    tetra_st(st)->tdma_valid = 1;
    tetra_st(st)->hyperframe_valid = 0;
    CHECK(tetra_air_decrypt_voice(g_opts, st, mode, bits, 8, 0) == 0 && bits[0] == 1, "no hyperframe number, no IV");
    tetra_st(st)->hyperframe_valid = 1;
    tetra_st(st)->fn = 0;
    CHECK(tetra_air_decrypt_voice(g_opts, st, mode, bits, 8, 0) == 0 && bits[0] == 1,
          "a reserved frame number is no IV");
    tetra_st(st)->fn = 1;
    mode = TETRA_ENC_MODE_ODD;
    CHECK(tetra_air_decrypt_voice(g_opts, st, mode, bits, 8, 0) == 1, "the odd key version is encrypted too");
    mode = TETRA_ENC_MODE_RESERVED;
    DSD_MEMSET(bits, 1, sizeof(bits));
    CHECK(tetra_air_decrypt_voice(g_opts, st, mode, bits, 8, 0) == 0 && bits[0] == 1,
          "the reserved mode is not decrypted");
    mode = TETRA_ENC_MODE_NONE;
    CHECK(tetra_air_decrypt_voice(g_opts, st, mode, bits, 8, 0) == 0 && bits[0] == 1, "clear speech is unchanged");

    /* TB5 needs the carrier number, the location area and the colour code of the carrier being received. */
    mode = TETRA_ENC_MODE_EVEN;
    tetra_st(st)->sysinfo_known = 0;
    CHECK(tetra_air_decrypt_voice(g_opts, st, mode, bits, 8, 0) == 0 && bits[0] == 1, "no location area, no ECK");
    CHECK(!tetra_air_key_usable_on(st, (int)CARRIER), "nor is the key usable without it");
    tetra_st(st)->sysinfo_known = 1;
    CHECK(tetra_air_key_usable_on(st, (int)CARRIER), "the key is usable with it");
    g_opts->rtlsdr_center_freq = CARRIER_HZ + 1u;
    CHECK(tetra_air_decrypt_voice(g_opts, st, mode, bits, 8, 0) == 0 && bits[0] == 1,
          "a tuned frequency that is no carrier gives no carrier number");
    g_opts->rtlsdr_center_freq = CARRIER_HZ;
    CHECK(tetra_air_decrypt_voice(NULL, st, mode, bits, 8, 0) == 0 && bits[0] == 1,
          "not trunking and no tuned frequency, no carrier number");
    CHECK(tetra_air_decrypt_voice(g_opts, st, mode, bits, 8, 0) == 1, "the tuned carrier's number");
    free_state(st);
}

/* Speech takes KSS(0..273) in a full slot and KSS(216..352) in the second half of a stolen slot (EN 300 392-7 table
 * 6.4). */
static void
test_kss_allocation(void) {
    uint8_t ks[54];
    uint8_t bits[137];
    dsd_state* st = new_encrypted_state();
    const uint32_t iv = tetra_air_build_iv(1, 1, 1, 0, 0);
    static const uint8_t zero_key[10] = {0};
    uint8_t eck[10];
    int same = 1;

    printf("[key stream segment allocation]\n");
    tetra_tea_derive_eck(CARRIER, LA, COLOUR, zero_key, eck);
    DSD_MEMSET(ks, 0, sizeof(ks));
    tetra_tea_keystream(0, eck, iv, ks, (int)sizeof(ks));
    DSD_MEMSET(bits, 0, sizeof(bits));
    CHECK(tetra_air_decrypt_voice(g_opts, st, TETRA_ENC_MODE_EVEN, bits, 137, TETRA_AIR_KSS_TCH_S_HALF) == 1,
          "half-slot speech decrypts");
    for (int i = 0; i < 137; i++) {
        const int bit = TETRA_AIR_KSS_TCH_S_HALF + i;
        same = same && bits[i] == ((ks[bit / 8] >> (7 - (bit % 8))) & 1u);
    }
    CHECK(same, "half-slot speech takes KSS(216..352)");

    DSD_MEMSET(bits, 0, sizeof(bits));
    CHECK(tetra_air_decrypt_voice(g_opts, st, TETRA_ENC_MODE_EVEN, bits, 137, TETRA_AIR_KSS_TCH_S) == 1
              && bits[0] == ((ks[0] >> 7) & 1u),
          "full-slot speech starts at KSS(0)");
    CHECK(tetra_air_decrypt_voice(g_opts, st, TETRA_ENC_MODE_EVEN, bits, 137, 296) == 0,
          "a fragment past KSS(431) is refused");
    CHECK(tetra_air_decrypt_voice(g_opts, st, TETRA_ENC_MODE_EVEN, bits, 137, -1) == 0
              && tetra_air_decrypt_voice(g_opts, st, TETRA_ENC_MODE_EVEN, NULL, 137, 0) == 0
              && tetra_air_decrypt_voice(g_opts, NULL, TETRA_ENC_MODE_EVEN, bits, 137, 0) == 0,
          "bad arguments are refused");
    free_state(st);
}

/* A real cipher key decrypts only through TB5: the key stream comes from the ECK of the carrier being received
 * (TETRA_crypto's first TB5 case: carrier 0x2BC, location area 0x1DCC, colour code 5), never from the key itself. */
static void
test_cipher_key(void) {
    static const uint8_t ck[10] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF, 0xAA, 0xBB};
    static const uint8_t eck[10] = {0x76, 0x13, 0xEA, 0x62, 0xA2, 0x6A, 0x87, 0x1F, 0xF8, 0x07};
    uint8_t want[18];
    uint8_t raw[18];
    uint8_t bits[137];
    dsd_state* st = new_encrypted_state();
    DSD_SECURE_ZERO(st->tetra_air_keys, sizeof(st->tetra_air_keys));
    CHECK(dsd_key_apply_direct(st, DSD_KEY_TYPE_TETRA_AIR, "86,28,0123456789ABCDEFAABB", DSD_KEY_APPLY_OVERLAY)
              == DSD_KEY_DIRECT_OK,
          "the cipher key loads for network 86/28");
    const uint32_t iv = tetra_air_build_iv(1, 1, 1, 0, 0);

    printf("[cipher key through TB5]\n");
    tetra_tea_keystream(0, eck, iv, want, (int)sizeof(want));
    tetra_tea_keystream(0, ck, iv, raw, (int)sizeof(raw));
    DSD_MEMSET(bits, 0, sizeof(bits));
    CHECK(tetra_air_decrypt_voice(g_opts, st, TETRA_ENC_MODE_ODD, bits, 137, TETRA_AIR_KSS_TCH_S) == 1,
          "speech decrypts");
    int from_eck = 1;
    int from_ck = 1;
    for (int i = 0; i < 137; i++) {
        from_eck = from_eck && bits[i] == ((want[i / 8] >> (7 - (i % 8))) & 1u);
        from_ck = from_ck && bits[i] == ((raw[i / 8] >> (7 - (i % 8))) & 1u);
    }
    CHECK(from_eck && !from_ck, "with the key stream of the derived ECK, not of the cipher key");

    /* Another carrier of the cell is another ECK. */
    g_opts->rtlsdr_center_freq = CARRIER_HZ + 25000u;
    DSD_MEMSET(bits, 0, sizeof(bits));
    (void)tetra_air_decrypt_voice(g_opts, st, TETRA_ENC_MODE_ODD, bits, 137, TETRA_AIR_KSS_TCH_S);
    from_eck = 1;
    for (int i = 0; i < 137; i++) {
        from_eck = from_eck && bits[i] == ((want[i / 8] >> (7 - (i % 8))) & 1u);
    }
    CHECK(!from_eck, "the next carrier takes another ECK");
    g_opts->rtlsdr_center_freq = CARRIER_HZ;
    free_state(st);
}

/* @p nbits zero speech bits decrypted from KSS(0) on the carrier g_opts is tuned to, compared with the key stream
 * @p ks: 1 when every bit is the key stream bit, 0 when not or when nothing was decrypted. */
static int
decrypts_to(dsd_state* st, const uint8_t* ks, int nbits) {
    uint8_t bits[137];
    DSD_MEMSET(bits, 0, sizeof(bits));
    if (tetra_air_decrypt_voice(g_opts, st, TETRA_ENC_MODE_EVEN, bits, nbits, TETRA_AIR_KSS_TCH_S) != 1) {
        return 0;
    }
    for (int i = 0; i < nbits; i++) {
        if (bits[i] != ((ks[i / 8] >> (7 - (i % 8))) & 1u)) {
            return 0;
        }
    }
    return 1;
}

/* The key stream an 80-bit key gives under KSG @p ksg on @p carrier of the test cell, from TN 1, FN 1, MN 1. */
static void
ck_stream(int ksg, const uint8_t ck[10], uint16_t carrier, uint8_t* ks, int nbytes) {
    uint8_t eck[10];
    tetra_tea_derive_eck(carrier, LA, COLOUR, ck, eck);
    tetra_tea_keystream(ksg, eck, tetra_air_build_iv(1, 1, 1, 0, 0), ks, nbytes);
}

/* The network's algorithm is what its signalling named: every KSG it named is kept, and one is the network's only while
 * it is the only one. Reserved KSG numbers are refused; proprietary ones are kept but never decrypted. */
static void
test_ksg_evidence(void) {
    dsd_state* st = (dsd_state*)calloc(1, sizeof(dsd_state));
    printf("[KSG evidence]\n");
    CHECK(tetra_air_ksg_seen(st, 86, 28) == 0U && tetra_air_ksg_lookup(st, 86, 28) == -1, "nothing named yet");
    CHECK(tetra_air_ksg_remember(st, 86, 28, 0) == 1 && tetra_air_ksg_lookup(st, 86, 28) == 0, "TEA1 named");
    CHECK(tetra_air_ksg_remember(st, 86, 28, 0) == 1 && tetra_air_ksg_lookup(st, 86, 28) == 0, "named again");
    CHECK(tetra_air_ksg_remember(st, 86, 28, 1) == 1 && tetra_air_ksg_lookup(st, 86, 28) == -1
              && tetra_air_ksg_seen(st, 86, 28) == 0x0003U,
          "TEA1 and TEA2 both named: no single algorithm");
    CHECK(tetra_air_ksg_remember(st, 86, 28, 5) == 0 && tetra_air_ksg_seen(st, 86, 28) == 0x0003U,
          "a reserved KSG is refused");
    CHECK(tetra_air_ksg_remember(st, 86, 29, 9) == 1 && tetra_air_ksg_seen(st, 86, 29) == 0x0200U
              && tetra_air_ksg_lookup(st, 86, 29) == 9,
          "a proprietary KSG is kept for its own network");
    CHECK(tetra_air_ksg_remember(st, 86, 30, 16) == 0, "a KSG wider than four bits is refused");
    free_state(st);
}

/* What radios proposed on a network's uplink is kept apart from what its signalling named: it never makes or breaks the
 * network's single algorithm. It shares the network's entry, so a keyed network keeps it and a full cache evicts it with
 * the oldest keyless network. */
static void
test_ksg_requested(void) {
    char spec[48];
    dsd_state* st = (dsd_state*)calloc(1, sizeof(dsd_state));
    printf("[KSG requested]\n");
    CHECK(tetra_air_ksg_request(st, 86, 28, 0) == 1 && tetra_air_ksg_requested(st, 86, 28) == 0x0001U
              && tetra_air_ksg_seen(st, 86, 28) == 0U && tetra_air_ksg_lookup(st, 86, 28) == -1,
          "a proposed TEA1 names no algorithm of the network");
    CHECK(tetra_air_ksg_remember(st, 86, 28, 1) == 1 && tetra_air_ksg_lookup(st, 86, 28) == 1
              && tetra_air_ksg_requested(st, 86, 28) == 0x0001U,
          "the network's own TEA2 stays its single algorithm beside the proposal");
    CHECK(tetra_air_ksg_request(st, 86, 28, 6) == 0 && tetra_air_ksg_requested(st, 86, 28) == 0x0001U,
          "a reserved KSG is refused");
    CHECK(tetra_air_ksg_request(st, 86, 28, 9) == 1 && tetra_air_ksg_requested(st, 86, 28) == 0x0201U,
          "a proprietary one is kept");

    DSD_SNPRINTF(spec, sizeof spec, "1,100,00000000000000000000");
    (void)dsd_key_apply_direct(st, DSD_KEY_TYPE_TETRA_AIR, spec, DSD_KEY_APPLY_OVERLAY);
    (void)tetra_air_ksg_request(st, 1, 100, 2);
    for (unsigned mnc = 200U; mnc < 240U; mnc++) {
        (void)tetra_air_ksg_request(st, 2, (uint16_t)mnc, 0);
    }
    CHECK(tetra_air_ksg_requested(st, 1, 100) == 0x0004U, "a keyed network keeps its proposals");
    CHECK(tetra_air_ksg_requested(st, 86, 28) == 0U && tetra_air_ksg_seen(st, 86, 28) == 0U,
          "a full cache evicts the oldest keyless network, proposals and all");
    CHECK(tetra_air_ksg_requested(st, 2, 239) == 0x0001U, "and keeps the newest");
    free_state(st);
}

/* Twenty networks of evidence: the evidence of a network an air key is loaded for is never evicted, so observations of
 * other networks (a foreign OTAR) cannot erase it, nor turn its two algorithms back into one. */
static void
test_ksg_evidence_cache(void) {
    char spec[48];
    dsd_state* st = (dsd_state*)calloc(1, sizeof(dsd_state));
    printf("[KSG evidence cache]\n");
    for (unsigned mnc = 100U; mnc < 116U; mnc++) {
        DSD_SNPRINTF(spec, sizeof spec, "1,%u,00000000000000000000", mnc);
        (void)dsd_key_apply_direct(st, DSD_KEY_TYPE_TETRA_AIR, spec, DSD_KEY_APPLY_OVERLAY);
        (void)tetra_air_ksg_remember(st, 1, (uint16_t)mnc, 1);
    }
    (void)tetra_air_ksg_remember(st, 1, 100, 2);
    for (unsigned mnc = 200U; mnc < 230U; mnc++) {
        CHECK(tetra_air_ksg_remember(st, 2, (uint16_t)mnc, 0) == 1, "a keyless network's KSG is kept");
    }
    int kept = 1;
    for (unsigned mnc = 101U; mnc < 116U; mnc++) {
        kept = kept && tetra_air_ksg_lookup(st, 1, (uint16_t)mnc) == 1;
    }
    CHECK(kept, "every keyed network keeps its evidence");
    CHECK(tetra_air_ksg_seen(st, 1, 100) == 0x0006U && tetra_air_ksg_lookup(st, 1, 100) == -1,
          "and a keyed network's two algorithms stay two");
    CHECK(tetra_air_ksg_lookup(st, 2, 229) == 0 && tetra_air_ksg_lookup(st, 2, 226) == 0
              && tetra_air_ksg_lookup(st, 2, 225) == -1,
          "keyless networks take the four other entries, the oldest evicted first");
    free_state(st);
}

/* Which key decrypts which speech (D3): a key that names its algorithm runs it whatever the network named; one that
 * names none runs the network's single TEA1-TEA3; a TEA1 reduced key decrypts only on its own channel, where it is
 * preferred over the network's 80-bit key. */
static void
test_key_choice(void) {
    static const uint8_t ck[10] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF, 0xAA, 0xBB};
    uint8_t ks_tea1[18];
    uint8_t ks_tea2[18];
    uint8_t ks_tea2_next[18];
    uint8_t ks_reduced[18];
    const uint32_t iv = tetra_air_build_iv(1, 1, 1, 0, 0);
    ck_stream(0, ck, CARRIER, ks_tea1, (int)sizeof ks_tea1);
    ck_stream(1, ck, CARRIER, ks_tea2, (int)sizeof ks_tea2);
    ck_stream(1, ck, CARRIER + 1u, ks_tea2_next, (int)sizeof ks_tea2_next);
    tetra_tea1_keystream_reduced(0x67757D92u, iv, ks_reduced, (int)sizeof ks_reduced);

    printf("[which key decrypts]\n");
    dsd_state* st = new_encrypted_state();
    DSD_SECURE_ZERO(st->tetra_air_keys, sizeof(st->tetra_air_keys));
    (void)dsd_key_apply_direct(st, DSD_KEY_TYPE_TETRA_AIR, "86,28,0123456789ABCDEFAABB", DSD_KEY_APPLY_OVERLAY);
    CHECK(decrypts_to(st, ks_tea1, 137), "a key naming no algorithm runs the network's TEA1");
    (void)tetra_air_ksg_remember(st, 86, 28, 1);
    CHECK(!decrypts_to(st, ks_tea1, 137) && !decrypts_to(st, ks_tea2, 137)
              && !tetra_air_key_usable_on(st, (int)CARRIER),
          "nor anything once the network named two algorithms");
    (void)dsd_key_apply_direct(st, DSD_KEY_TYPE_TETRA_AIR, "86,28,0123456789ABCDEFAABB,tea2", DSD_KEY_APPLY_OVERLAY);
    CHECK(decrypts_to(st, ks_tea2, 137) && tetra_air_key_usable_on(st, (int)CARRIER), "a key naming TEA2 runs TEA2");
    free_state(st);

    st = new_encrypted_state();
    DSD_SECURE_ZERO(st->tetra_air_keys, sizeof(st->tetra_air_keys));
    (void)dsd_key_apply_direct(st, DSD_KEY_TYPE_TETRA_AIR, "86,28,0123456789ABCDEFAABB,TEA2", DSD_KEY_APPLY_OVERLAY);
    CHECK(decrypts_to(st, ks_tea2, 137), "a key's own TEA2 wins over the network's TEA1");
    free_state(st);

    st = new_cell_state(3);
    (void)dsd_key_apply_direct(st, DSD_KEY_TYPE_TETRA_AIR, "86,28,0123456789ABCDEFAABB", DSD_KEY_APPLY_OVERLAY);
    CHECK(!tetra_air_key_usable_on(st, (int)CARRIER), "a key naming none cannot run a TEA4 network");
    free_state(st);

    /* A TEA1 reduced key of carrier 700 in location area 0x1DCC (7628), colour code 5. */
    st = new_encrypted_state();
    DSD_SECURE_ZERO(st->tetra_air_keys, sizeof(st->tetra_air_keys));
    CHECK(dsd_key_apply_direct(st, DSD_KEY_TYPE_TETRA_AIR, "86,28,67757D92,la=7628,cc=5,carrier=700",
                               DSD_KEY_APPLY_OVERLAY)
              == DSD_KEY_DIRECT_OK,
          "a reduced key loads for carrier 700");
    CHECK(decrypts_to(st, ks_reduced, 137), "it decrypts carrier 700 with TEA1 from its register");
    CHECK(tetra_air_key_usable_on(st, (int)CARRIER) && !tetra_air_key_usable_on(st, (int)CARRIER + 1)
              && !tetra_air_key_usable_on(st, -1),
          "it is usable on its carrier only, not on an unknown one");
    g_opts->rtlsdr_center_freq = CARRIER_HZ + 25000u;
    CHECK(!decrypts_to(st, ks_reduced, 137), "the next carrier is not decrypted");
    g_opts->rtlsdr_center_freq = CARRIER_HZ;
    tetra_st(st)->la = LA + 1u;
    CHECK(!decrypts_to(st, ks_reduced, 137) && !tetra_air_key_usable_on(st, (int)CARRIER),
          "another location area is another channel");
    tetra_st(st)->la = LA;
    tetra_st(st)->colour = COLOUR + 1u;
    CHECK(!decrypts_to(st, ks_reduced, 137), "another colour code is another channel");
    tetra_st(st)->colour = COLOUR;
    (void)tetra_air_ksg_remember(st, 86, 28, 1);
    CHECK(decrypts_to(st, ks_reduced, 137), "the network naming TEA2 too does not stop a reduced key");
    tetra_st(st)->tdma_valid = 0;
    CHECK(tetra_air_key_usable_on(st, (int)CARRIER) && !decrypts_to(st, ks_reduced, 137),
          "without a clock the key stays usable but nothing decrypts until there is an IV");
    tetra_st(st)->tdma_valid = 1;

    /* Beside the network's 80-bit TEA2 key, loaded after the reduced key and before it. */
    (void)dsd_key_apply_direct(st, DSD_KEY_TYPE_TETRA_AIR, "86,28,0123456789ABCDEFAABB,tea2", DSD_KEY_APPLY_OVERLAY);
    CHECK(decrypts_to(st, ks_reduced, 137), "the reduced key is preferred on its channel");
    g_opts->rtlsdr_center_freq = CARRIER_HZ + 25000u;
    CHECK(decrypts_to(st, ks_tea2_next, 137), "and the 80-bit key decrypts the other carriers");
    g_opts->rtlsdr_center_freq = CARRIER_HZ;
    free_state(st);
    st = new_encrypted_state();
    DSD_SECURE_ZERO(st->tetra_air_keys, sizeof(st->tetra_air_keys));
    (void)dsd_key_apply_direct(st, DSD_KEY_TYPE_TETRA_AIR, "86,28,0123456789ABCDEFAABB,tea2", DSD_KEY_APPLY_OVERLAY);
    (void)dsd_key_apply_direct(st, DSD_KEY_TYPE_TETRA_AIR, "86,28,67757D92,la=7628,cc=5,carrier=700",
                               DSD_KEY_APPLY_OVERLAY);
    CHECK(decrypts_to(st, ks_reduced, 137), "whichever was loaded first");
    CHECK(tetra_air_key_usable_on(st, -1), "the 80-bit key answers for an unknown carrier");
    tetra_st(st)->dmo = 1;
    CHECK(!tetra_air_key_usable_on(st, (int)CARRIER), "no key applies in direct mode");
    free_state(st);
}

int
main(void) {
    printf("TETRA air interface\n");
    g_opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    if (!g_opts) {
        return 1;
    }
    g_opts->audio_in_type = AUDIO_IN_RTL;
    g_opts->rtlsdr_center_freq = CARRIER_HZ;
    test_vectors();
    test_preconditions();
    test_kss_allocation();
    test_cipher_key();
    test_ksg_evidence();
    test_ksg_evidence_cache();
    test_ksg_requested();
    test_key_choice();
    free(g_opts);
    printf("%s\n", g_fail ? "FAILED" : "ALL TESTS PASSED");
    return g_fail ? 1 : 0;
}
