// SPDX-License-Identifier: GPL-3.0-or-later
/* Operator-key air-interface decrypt: IV packing and TEA1/2/3 vectors.
 * Vectors are the published TEA1/2/3 cases from Midnight Blue's Apache-2.0 tests. */
#include <dsd-neo/core/key_set.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/crypto/tetra_tea.h>
#include <dsd-neo/protocol/tetra/tetra_air.h>

#include <stdio.h>
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
    memset(out, 0, sizeof(out));
    tetra_tea_keystream(alg, key, iv, out, 10);
    return memcmp(out, expect, 10) == 0;
}

int
main(void) {
    static const uint8_t zero_key[10] = {0};
    static const uint8_t tea1_key[10] = {0xA7, 0x98, 0x39, 0xE4, 0xBA, 0x88, 0xEE, 0x54, 0xA0, 0x29};
    static const uint8_t tea1_zero_ks[10] = {0xd3, 0x3f, 0xd8, 0xa6, 0x05, 0xa0, 0xa1, 0xbb, 0x90, 0x23};
    static const uint8_t tea1_ks[10] = {0x1d, 0xec, 0x9c, 0x7e, 0xc6, 0x22, 0x3d, 0x87, 0xc2, 0xcc};
    static const uint8_t tea2_key[10] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xAA};
    static const uint8_t tea2_zero_ks[10] = {0xA7, 0x98, 0x39, 0xE4, 0xBA, 0x88, 0xEE, 0x54, 0xA0, 0x29};
    static const uint8_t tea2_ks[10] = {0x64, 0x70, 0x4E, 0xA9, 0xD7, 0xDC, 0x25, 0x60, 0x81, 0x39};
    static const uint8_t tea3_zero_ks[10] = {0x06, 0xA6, 0x58, 0x8C, 0x5D, 0x9A, 0x99, 0x6D, 0xD2, 0x5E};
    static const uint8_t tea3_ks[10] = {0x02, 0x49, 0x1e, 0xf5, 0x57, 0xc5, 0x1c, 0x17, 0x73, 0x0c};
    static dsd_state state;
    uint8_t bits[8];

    printf("TETRA air interface\n");
    CHECK(tetra_air_build_iv(1, 1, 1, 0, 0) == ((1u << 2) | (1u << 7)), "downlink IV packs slot frame and multiframe");
    CHECK(tetra_air_build_iv(4, 18, 60, 0x7FFF, 1) == (3u | (18u << 2) | (60u << 7) | (0x7FFFu << 13) | (1u << 28)),
          "uplink IV packs the direction bit");
    CHECK(tetra_air_build_iv(0, 1, 1, 0, 0) == 0, "timeslot 0 is not an IV");

    CHECK(ks_eq(0, 0x11111111u, zero_key, tea1_zero_ks), "TEA1 zero key vector");
    CHECK(ks_eq(0, 0x01234567u, tea1_key, tea1_ks), "TEA1 keyed vector");
    CHECK(ks_eq(1, 0x12345678u, zero_key, tea2_zero_ks), "TEA2 zero key vector");
    CHECK(ks_eq(1, 0x12345678u, tea2_key, tea2_ks), "TEA2 keyed vector");
    CHECK(ks_eq(2, 0x11111111u, zero_key, tea3_zero_ks), "TEA3 zero key vector");
    CHECK(ks_eq(2, 0x01234567u, tea1_key, tea3_ks), "TEA3 keyed vector");

    memset(&state, 0, sizeof(state));
    memset(bits, 1, sizeof(bits));
    tetra_st(&state)->enc_mode = 1;
    tetra_st(&state)->net_known = 1;
    tetra_st(&state)->mcc = 86;
    tetra_st(&state)->mnc = 28;
    tetra_st(&state)->hyperframe_valid = 1;
    tetra_st(&state)->tdma_valid = 1;
    tetra_st(&state)->tn = 1;
    tetra_st(&state)->fn = 1;
    tetra_st(&state)->mn = 1;
    CHECK(tetra_air_decrypt_voice(&state, bits, 8, 0) == 0, "encrypted voice without a KSG is unchanged");
    tetra_air_ksg_remember(&state, 86, 28, 3);
    CHECK(tetra_air_decrypt_voice(&state, bits, 8, 0) == 0, "TEA4 is named but not decrypted");
    tetra_air_ksg_remember(&state, 86, 28, 0);
    CHECK(tetra_air_decrypt_voice(&state, bits, 8, 0) == 0, "TEA1 without an operator key is unchanged");
    CHECK(dsd_key_apply_direct(&state, DSD_KEY_TYPE_TETRA_AIR, "86,28,00000000000000000000", DSD_KEY_APPLY_OVERLAY)
              == DSD_KEY_DIRECT_OK,
          "an operator key loads for network 86/28");
    CHECK(tetra_air_decrypt_voice(&state, bits, 8, 0) == 1, "TEA1 with a key decrypts trunked voice");
    tetra_st(&state)->dmo = 1;
    memset(bits, 1, sizeof(bits));
    CHECK(tetra_air_decrypt_voice(&state, bits, 8, 0) == 0 && bits[0] == 1, "direct mode is not air-interface TEA");

    printf("%s\n", g_fail ? "FAILED" : "ALL TESTS PASSED");
    return g_fail ? 1 : 0;
}
