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

/* Network 86/28 with KSG TEA1, an all-zero cipher key, colour code COLOUR and location area LA, on carrier CARRIER, and
 * the clock at TN 1, FN 1, MN 1 of hyperframe 0. */
static dsd_state*
new_encrypted_state(void) {
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
    tetra_air_ksg_remember(st, 86, 28, 0);
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
    CHECK(!tetra_air_key_usable(st), "nor is the key usable without it");
    tetra_st(st)->sysinfo_known = 1;
    CHECK(tetra_air_key_usable(st), "the key is usable with it");
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
    free(g_opts);
    printf("%s\n", g_fail ? "FAILED" : "ALL TESTS PASSED");
    return g_fail ? 1 : 0;
}
