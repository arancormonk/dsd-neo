// SPDX-License-Identifier: GPL-3.0-or-later
/* Trunked mode air interface decryption with an operator key: initial value packing (EN 300 392-7 clause 6.3.2.1), the
 * key stream segment allocation of speech (table 6.4) and the TEA1/2/3 vectors.
 * Vectors are the published TEA1/2/3 cases from Midnight Blue's Apache-2.0 tests. */
#include <dsd-neo/core/key_set.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/crypto/tetra_tea.h>
#include <dsd-neo/protocol/tetra/tetra_air.h>
#include <dsd-neo/protocol/tetra/tetra_mac.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tetra_state.h"

static int g_fail = 0;

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

/* An encrypted call on network 86/28 with KSG TEA1, an all-zero operator key, and the clock at TN 1, FN 1, MN 1 of
 * hyperframe 0. */
static dsd_state*
new_encrypted_state(void) {
    dsd_state* st = (dsd_state*)calloc(1, sizeof(dsd_state));
    tetra_state_t* ts = tetra_st(st);
    ts->enc_mode = TETRA_ENC_MODE_EVEN;
    ts->net_known = 1;
    ts->mcc = 86;
    ts->mnc = 28;
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
    dsd_state* st = (dsd_state*)calloc(1, sizeof(dsd_state));

    printf("[when speech is decrypted]\n");
    DSD_MEMSET(bits, 1, sizeof(bits));
    tetra_st(st)->enc_mode = TETRA_ENC_MODE_EVEN;
    tetra_st(st)->net_known = 1;
    tetra_st(st)->mcc = 86;
    tetra_st(st)->mnc = 28;
    tetra_st(st)->hyperframe_valid = 1;
    tetra_st(st)->tdma_valid = 1;
    tetra_st(st)->tn = 1;
    tetra_st(st)->fn = 1;
    tetra_st(st)->mn = 1;
    CHECK(tetra_air_decrypt_voice(st, bits, 8, 0) == 0, "encrypted voice without a KSG is unchanged");
    tetra_air_ksg_remember(st, 86, 28, 3);
    CHECK(tetra_air_decrypt_voice(st, bits, 8, 0) == 0, "TEA4 is named but not decrypted");
    tetra_air_ksg_remember(st, 86, 28, 0);
    CHECK(tetra_air_ksg_lookup(st, 86, 28) == 0 && tetra_air_ksg_lookup(st, 86, 29) == -1,
          "a KSG is remembered per network");
    CHECK(tetra_air_decrypt_voice(st, bits, 8, 0) == 0, "TEA1 without an operator key is unchanged");
    CHECK(dsd_key_apply_direct(st, DSD_KEY_TYPE_TETRA_AIR, "86,28,00000000000000000000", DSD_KEY_APPLY_OVERLAY)
              == DSD_KEY_DIRECT_OK,
          "an operator key loads for network 86/28");
    CHECK(tetra_air_decrypt_voice(st, bits, 8, 0) == 1, "TEA1 with a key decrypts trunked voice");

    tetra_st(st)->dmo = 1;
    DSD_MEMSET(bits, 1, sizeof(bits));
    CHECK(tetra_air_decrypt_voice(st, bits, 8, 0) == 0 && bits[0] == 1, "direct mode is not decrypted here");
    tetra_st(st)->dmo = 0;
    tetra_st(st)->tdma_valid = 0;
    CHECK(tetra_air_decrypt_voice(st, bits, 8, 0) == 0 && bits[0] == 1, "no TDMA clock, no IV");
    tetra_st(st)->tdma_valid = 1;
    tetra_st(st)->hyperframe_valid = 0;
    CHECK(tetra_air_decrypt_voice(st, bits, 8, 0) == 0 && bits[0] == 1, "no hyperframe number, no IV");
    tetra_st(st)->hyperframe_valid = 1;
    tetra_st(st)->fn = 0;
    CHECK(tetra_air_decrypt_voice(st, bits, 8, 0) == 0 && bits[0] == 1, "a reserved frame number is no IV");
    tetra_st(st)->fn = 1;
    tetra_st(st)->enc_mode = TETRA_ENC_MODE_ODD;
    CHECK(tetra_air_decrypt_voice(st, bits, 8, 0) == 1, "the odd key version is encrypted too");
    tetra_st(st)->enc_mode = TETRA_ENC_MODE_RESERVED;
    DSD_MEMSET(bits, 1, sizeof(bits));
    CHECK(tetra_air_decrypt_voice(st, bits, 8, 0) == 0 && bits[0] == 1, "the reserved mode is not decrypted");
    tetra_st(st)->enc_mode = TETRA_ENC_MODE_NONE;
    CHECK(tetra_air_decrypt_voice(st, bits, 8, 0) == 0 && bits[0] == 1, "clear call is unchanged");
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
    int same = 1;

    printf("[key stream segment allocation]\n");
    DSD_MEMSET(ks, 0, sizeof(ks));
    tetra_tea_keystream(0, zero_key, iv, ks, (int)sizeof(ks));
    DSD_MEMSET(bits, 0, sizeof(bits));
    CHECK(tetra_air_decrypt_voice(st, bits, 137, TETRA_AIR_KSS_TCH_S_HALF) == 1, "half-slot speech decrypts");
    for (int i = 0; i < 137; i++) {
        const int bit = TETRA_AIR_KSS_TCH_S_HALF + i;
        same = same && bits[i] == ((ks[bit / 8] >> (7 - (bit % 8))) & 1u);
    }
    CHECK(same, "half-slot speech takes KSS(216..352)");

    DSD_MEMSET(bits, 0, sizeof(bits));
    CHECK(tetra_air_decrypt_voice(st, bits, 137, TETRA_AIR_KSS_TCH_S) == 1 && bits[0] == ((ks[0] >> 7) & 1u),
          "full-slot speech starts at KSS(0)");
    CHECK(tetra_air_decrypt_voice(st, bits, 137, 296) == 0, "a fragment past KSS(431) is refused");
    CHECK(tetra_air_decrypt_voice(st, bits, 137, -1) == 0 && tetra_air_decrypt_voice(st, NULL, 137, 0) == 0
              && tetra_air_decrypt_voice(NULL, bits, 137, 0) == 0,
          "bad arguments are refused");
    free_state(st);
}

int
main(void) {
    printf("TETRA air interface\n");
    test_vectors();
    test_preconditions();
    test_kss_allocation();
    printf("%s\n", g_fail ? "FAILED" : "ALL TESTS PASSED");
    return g_fail ? 1 : 0;
}
