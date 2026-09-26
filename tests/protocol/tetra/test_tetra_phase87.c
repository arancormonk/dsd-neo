// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * TETRA Phase 87 test suite.
 * 
 * Verifies the ACELP class reorder fix for TCH/FS and the new pipeline constants.
 */

#include <dsd-neo/protocol/tetra/tetra_acelp.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/opts.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, msg) \
    do { \
        if (cond) { \
            printf("  PASS: %s\n", msg); \
            g_pass++; \
        } else { \
            printf("  FAIL: %s (line %d)\n", msg, __LINE__); \
            g_fail++; \
        } \
    } while (0)

/* Exact position tables from EN 300 395-2 V1.3.1 Table 4 */
static const uint8_t class0_positions[] = {
    35, 36, 37, 38, 39, 40, 41, 42, 43, 47, 48,
    56, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 74,
    75, 83, 88, 89, 90, 91, 92, 93, 94, 95, 96,
    97, 101, 102, 110, 115, 116, 117, 118, 119,
    120, 121, 122, 123, 124, 128, 129, 137
};

static const uint8_t class1_positions[] = {
    58, 85, 112,
    54, 81, 108, 135,
    50, 77,
    104, 131,
    45, 72, 99, 126,
    55, 82, 109, 136,
    5, 13, 34,
    8, 16, 17, 22, 23, 24, 25, 26,
    6, 14, 7, 15,
    60, 87, 114,
    46,
    73, 100, 127,
    44, 71, 98, 125,
    33, 49,
    76, 103, 130,
    59, 86, 113,
    57, 84, 111
};

static const uint8_t class2_positions[] = {
    18, 19, 20, 21,
    31, 32,
    53, 80, 107, 134,
    1, 2, 3, 4,
    9, 10, 11, 12,
    27, 28, 29, 30,
    52, 79, 106, 133,
    51, 78, 105, 132
};

#define TETRA_TCH_FRAME_BITS 137

static void test_acelp_reorder(void) {
    printf("--- ACELP Class Reorder Test ---\n");
    
    /* 274 sensitivity-ordered speech bits */
    uint8_t in[274];
    for (int i = 0; i < 274; i++) {
        in[i] = (uint8_t)(i & 0xFF);  /* unique marker per bit */
    }

    uint8_t out[2 * TETRA_TCH_FRAME_BITS]; /* 274 bytes */
    memset(out, 0xFF, sizeof(out));

    tetra_acelp_reorder(in, out, 274);

    /* 
     * Verify no out-of-bounds writes or frame overlaps.
     * The three classes cover all 137 positions in both codec frames.
     */
    
    /* Recreate the index presence map */
    uint8_t written[2 * TETRA_TCH_FRAME_BITS];
    memset(written, 0, sizeof(written));
    
    int in_idx = 0;
    
    /* Class 0 */
    for (int i = 0; i < 51; i++) {
        for (int f = 0; f < 2; f++) {
            int out_idx = f * TETRA_TCH_FRAME_BITS + class0_positions[i] - 1;
            CHECK(out[out_idx] == in[in_idx], "Class 0 mapped correctly");
            written[out_idx]++;
            in_idx++;
        }
    }
    
    /* Class 1 */
    for (int i = 0; i < 56; i++) {
        for (int f = 0; f < 2; f++) {
            int out_idx = f * TETRA_TCH_FRAME_BITS + class1_positions[i] - 1;
            CHECK(out[out_idx] == in[in_idx], "Class 1 mapped correctly");
            written[out_idx]++;
            in_idx++;
        }
    }
    
    /* Class 2 */
    for (int i = 0; i < 30; i++) {
        for (int f = 0; f < 2; f++) {
            int out_idx = f * TETRA_TCH_FRAME_BITS + class2_positions[i] - 1;
            CHECK(out[out_idx] == in[in_idx], "Class 2 mapped correctly");
            written[out_idx]++;
            in_idx++;
        }
    }

    CHECK(in_idx == 274, "Consumed exactly 274 bits");

    int double_writes = 0;
    int unwritten = 0;
    for (int f = 0; f < 2; f++) {
        for (int i = 0; i < TETRA_TCH_FRAME_BITS; i++) {
            int idx = f * TETRA_TCH_FRAME_BITS + i;
            if (written[idx] > 1) {
                double_writes++;
                printf("  Double write at frame %d pos %d\n", f, i+1);
            }
            if (written[idx] == 0) {
                unwritten++;
            }
        }
    }

    CHECK(double_writes == 0, "No overlapping bit positions (stride is correct)");
    CHECK(unwritten == 0, "All 274 codec-bit positions were written");
    memset(out, 0xA5, sizeof(out));
    tetra_acelp_reorder(in, out, 273);
    int short_unchanged = 1;
    for (size_t i = 0; i < sizeof(out); i++) {
        if (out[i] != 0xA5) short_unchanged = 0;
    }
    CHECK(short_unchanged == 1, "Short ACELP input is rejected without output writes");
}

int main(void) {
    printf("=== TETRA Phase 87 Tests ===\n");
    test_acelp_reorder();
    printf("\n=== Phase 87 Results: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
