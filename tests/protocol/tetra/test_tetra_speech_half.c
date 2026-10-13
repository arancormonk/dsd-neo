// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Half-slot TCH/S (EN 300 395-2 clause 5.6) loopback: encode one 137-bit speech
 * frame (51 class 0 + 56 class 1 + 30 class 2), decode it, compare. The CRC bit
 * order matches the one verified on real full-slot DMO speech bursts.
 */

#include <dsd-neo/protocol/tetra/tetra_fec.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
par5(int v) {
    v ^= v >> 4;
    v ^= v >> 2;
    v ^= v >> 1;
    return v & 1;
}

/* Mother code V(3(k-1)+i), i = 1..3: G1 = 1+D+D^4... expressed as the same tap sets the decoder uses. */
static void
mother(const uint8_t* in, int n, uint8_t* out /* 3*n */) {
    int st = 0;
    for (int k = 0; k < n; k++) {
        const int involved = (in[k] << 4) | st;
        out[3 * k + 0] = (uint8_t)par5(involved & 0x1f);
        out[3 * k + 1] = (uint8_t)par5(involved & 0x1b);
        out[3 * k + 2] = (uint8_t)par5(involved & 0x15);
        st = (in[k] << 3) | (st >> 1);
    }
}

static uint8_t
crc4_ref(const uint8_t* d30) {
    /* Spec form: F(X) = X^4 I(X) mod (1 + X + X^4), I(X) = sum d[i] X^i with d[0] lowest (b1 = X^0). */
    uint8_t rem[34];
    memset(rem, 0, sizeof(rem));
    for (int i = 0; i < 30; i++) {
        rem[4 + i] = d30[i];
    }
    for (int deg = 33; deg >= 4; deg--) {
        if (rem[deg]) {
            rem[deg] ^= 1;
            rem[deg - 3] ^= 1; /* X */
            rem[deg - 4] ^= 1; /* 1 */
        }
    }
    return (uint8_t)(rem[0] | (rem[1] << 1) | (rem[2] << 2) | (rem[3] << 3));
}

/* Build 216 type-4 hard bits for the given 137 bits. */
static void
encode_half(const uint8_t* frame137, uint8_t* type4) {
    uint8_t t3[216];
    uint8_t info[94];
    memcpy(t3, frame137, 51);
    memcpy(info, frame137 + 51, 56);
    memcpy(info + 56, frame137 + 107, 30);
    const uint8_t crc = crc4_ref(frame137 + 107);
    for (int i = 0; i < 4; i++) {
        info[86 + i] = (uint8_t)((crc >> i) & 1);
    }
    memset(info + 90, 0, 4);

    uint8_t v[94 * 3];
    mother(info, 94, v);
    int out = 51;
    for (int k = 0; k < 94; k++) {
        const int phase = (k < 56 ? k : k - 56) & 7;
        for (int lane = 0; lane < 3; lane++) {
            int keep;
            if (k < 56) {
                keep = lane == 0 || (lane == 1 && (phase & 1) == 0);
            } else {
                keep = lane < 2 || phase == 0;
            }
            if (keep) {
                t3[out++] = v[3 * k + lane];
            }
        }
    }
    CHECK(out == 216, "encoder produced 216 type-3 bits");
    tetra_block_interleave(t3, type4, 216, 101, 0);
}

int
main(void) {
    printf("=== TETRA half-slot TCH/S ===\n");
    srand(7);
    uint8_t frame[137];
    for (int i = 0; i < 137; i++) {
        frame[i] = (uint8_t)(rand() & 1);
    }

    uint8_t type4[216];
    encode_half(frame, type4);

    uint16_t soft[216], deint[216];
    for (int i = 0; i < 216; i++) {
        soft[i] = type4[i] ? 0xffffu : 0u;
    }
    tetra_block_deinterleave_soft(soft, deint, 216, 101, 0);

    uint8_t out[137];
    int crc_ok = 0, mm = -1;
    CHECK(tetra_speech_half_decode(deint, out, &crc_ok, &mm) == 137, "decode returns 137 bits");
    CHECK(memcmp(out, frame, 137) == 0, "clean round trip");
    CHECK(crc_ok == 1, "class-2 CRC matches");
    CHECK(mm == 0, "no re-encode mismatches on a clean block");

    /* A few channel errors in the protected parts are corrected. */
    uint16_t noisy[216];
    memcpy(noisy, deint, sizeof(noisy));
    const int flips[] = {60, 90, 130, 170, 200};
    for (size_t i = 0; i < sizeof(flips) / sizeof(flips[0]); i++) {
        noisy[flips[i]] = noisy[flips[i]] ? 0u : 0xffffu;
    }
    crc_ok = 0;
    CHECK(tetra_speech_half_decode(noisy, out, &crc_ok, &mm) == 137, "decode with errors");
    CHECK(memcmp(out, frame, 137) == 0 && crc_ok, "five channel errors corrected");
    CHECK(mm == 5, "mismatch count equals injected errors in coded bits");

    /* Random blocks are rejected by the CRC/mismatch pair most of the time. */
    int accepted = 0;
    for (int n = 0; n < 400; n++) {
        for (int i = 0; i < 216; i++) {
            noisy[i] = (rand() & 1) ? 0xffffu : 0u;
        }
        if (tetra_speech_half_decode(noisy, out, &crc_ok, &mm) == 137 && crc_ok && mm <= 16) {
            accepted++;
        }
    }
    CHECK(accepted <= 2, "random blocks are almost never accepted");

    printf("%s\n", g_fail ? "FAILED" : "ALL TESTS PASSED");
    return g_fail ? 1 : 0;
}
