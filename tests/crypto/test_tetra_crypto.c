// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * TETRA TEA wrapper over the pinned TETRA_crypto generators: the EN 300 392-7
 * clause 6.3.2.1 IV packing the wrapper implements itself, and the keystream
 * entry point's dispatch and argument handling. Keystream vectors are the
 * published TEA1/2/3 cases from Midnight Blue's Apache-2.0 tests.
 */

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/crypto/tetra_tea.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int g_fail = 0;

static void
check(int cond, const char* label) {
    if (!cond) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", label);
        g_fail++;
    }
}

static uint32_t
expected_iv(uint32_t tn, uint32_t fn, uint32_t mn, uint32_t hn, uint32_t dir) {
    return (tn - 1u) | (fn << 2) | (mn << 7) | ((hn & 0x7FFFu) << 13) | (dir << 28);
}

static void
test_iv_packing(void) {
    uint32_t mismatches = 0;
    check(tetra_tea_build_iv(1, 1, 1, 0, 0) == ((1u << 2) | (1u << 7)), "lowest downlink IV");
    check(tetra_tea_build_iv(4, 18, 60, 0x7FFF, 1) == (3u | (18u << 2) | (60u << 7) | (0x7FFFu << 13) | (1u << 28)),
          "highest uplink IV");
    check(tetra_tea_build_iv(2, 5, 33, 0x1234, 0) == expected_iv(2, 5, 33, 0x1234, 0), "mid-range IV");
    /* Only 15 hyperframe bits are carried; bit 15 must not reach the direction bit. */
    check(tetra_tea_build_iv(3, 7, 11, 0xFFFF, 0) == tetra_tea_build_iv(3, 7, 11, 0x7FFF, 0), "HN bit 15 is dropped");
    check((tetra_tea_build_iv(3, 7, 11, 0xFFFF, 0) & (1u << 28)) == 0, "HN cannot set the direction bit");

    for (uint32_t tn = 1; tn <= 4; tn++) {
        for (uint32_t fn = 1; fn <= 18; fn++) {
            for (uint32_t mn = 1; mn <= 60; mn++) {
                for (uint32_t dir = 0; dir <= 1; dir++) {
                    uint32_t hn = (tn * 7919u + fn * 104729u + mn * 1299709u + dir) & 0xFFFFu;
                    if (tetra_tea_build_iv((uint8_t)tn, (uint8_t)fn, (uint8_t)mn, (uint16_t)hn, (uint8_t)dir)
                        != expected_iv(tn, fn, mn, hn, dir)) {
                        mismatches++;
                    }
                }
            }
        }
    }
    check(mismatches == 0, "every legal slot, frame, multiframe and direction packs per clause 6.3.2.1");

    check(tetra_tea_build_iv(0, 1, 1, 0, 0) == 0, "timeslot 0 is rejected");
    check(tetra_tea_build_iv(5, 1, 1, 0, 0) == 0, "timeslot 5 is rejected");
    check(tetra_tea_build_iv(1, 0, 1, 0, 0) == 0, "frame 0 is rejected");
    check(tetra_tea_build_iv(1, 19, 1, 0, 0) == 0, "frame 19 is rejected");
    check(tetra_tea_build_iv(1, 1, 0, 0, 0) == 0, "multiframe 0 is rejected");
    check(tetra_tea_build_iv(1, 1, 61, 0, 0) == 0, "multiframe 61 is rejected");
    check(tetra_tea_build_iv(1, 1, 1, 0, 2) == 0, "direction 2 is rejected");
}

static int
keystream_is(int alg, uint32_t iv, const uint8_t key[10], const uint8_t expect[10]) {
    uint8_t out[10];
    DSD_MEMSET(out, 0, sizeof(out));
    tetra_tea_keystream(alg, key, iv, out, (int)sizeof(out));
    return memcmp(out, expect, sizeof(out)) == 0;
}

static void
test_keystream(void) {
    static const uint8_t zero_key[10] = {0};
    static const uint8_t tea1_key[10] = {0xA7, 0x98, 0x39, 0xE4, 0xBA, 0x88, 0xEE, 0x54, 0xA0, 0x29};
    static const uint8_t tea1_ks[10] = {0x1d, 0xec, 0x9c, 0x7e, 0xc6, 0x22, 0x3d, 0x87, 0xc2, 0xcc};
    static const uint8_t tea2_key[10] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xAA};
    static const uint8_t tea2_ks[10] = {0x64, 0x70, 0x4E, 0xA9, 0xD7, 0xDC, 0x25, 0x60, 0x81, 0x39};
    static const uint8_t tea3_zero_ks[10] = {0x06, 0xA6, 0x58, 0x8C, 0x5D, 0x9A, 0x99, 0x6D, 0xD2, 0x5E};
    uint8_t key[10];
    uint8_t out[10];
    uint8_t untouched[10];

    check(keystream_is(0, 0x01234567u, tea1_key, tea1_ks), "TEA1 keyed vector");
    check(keystream_is(1, 0x12345678u, tea2_key, tea2_ks), "TEA2 keyed vector");
    check(keystream_is(2, 0x11111111u, zero_key, tea3_zero_ks), "TEA3 zero key vector");

    /* TEA2 and TEA3 take a mutable key; the caller's key must come back unchanged. */
    DSD_MEMCPY(key, tea2_key, sizeof(key));
    for (int alg = 0; alg <= 2; alg++) {
        tetra_tea_keystream(alg, key, 0x12345678u, out, (int)sizeof(out));
    }
    check(memcmp(key, tea2_key, sizeof(key)) == 0, "the caller's key is not modified");

    DSD_MEMSET(untouched, 0x5A, sizeof(untouched));
    DSD_MEMCPY(out, untouched, sizeof(out));
    tetra_tea_keystream(3, tea1_key, 0x01234567u, out, (int)sizeof(out));
    tetra_tea_keystream(-1, tea1_key, 0x01234567u, out, (int)sizeof(out));
    check(memcmp(out, untouched, sizeof(out)) == 0, "an unknown KSG number writes nothing");
    tetra_tea_keystream(0, NULL, 0x01234567u, out, (int)sizeof(out));
    tetra_tea_keystream(0, tea1_key, 0x01234567u, out, 0);
    tetra_tea_keystream(0, tea1_key, 0x01234567u, out, -4);
    check(memcmp(out, untouched, sizeof(out)) == 0, "a missing key or empty length writes nothing");
    tetra_tea_keystream(0, tea1_key, 0x01234567u, NULL, (int)sizeof(out));

    /* A shorter request is a prefix of the longer keystream. */
    DSD_MEMCPY(out, untouched, sizeof(out));
    tetra_tea_keystream(0, tea1_key, 0x01234567u, out, 4);
    check(memcmp(out, tea1_ks, 4) == 0 && memcmp(out + 4, untouched + 4, sizeof(out) - 4) == 0,
          "the generator writes exactly the requested bytes");
}

int
main(void) {
    test_iv_packing();
    test_keystream();
    if (g_fail != 0) {
        DSD_FPRINTF(stderr, "%d TETRA crypto check(s) failed\n", g_fail);
        return 1;
    }
    printf("TETRA crypto: all checks passed\n");
    return 0;
}
