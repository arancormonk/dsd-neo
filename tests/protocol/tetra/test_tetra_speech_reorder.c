// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Type-1 speech bits to speech frames (EN 300 395-2 tables 4 to 6): every class bit lands on its frame bit number,
 * the full slot alternating frames A and B within each class, the half slot taking one frame in order, and no frame
 * bit is written twice or left out.
 */

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/protocol/tetra/tetra_voice.h>

#include <stdint.h>
#include <stdio.h>

enum { FRAME_BITS = 137 };

/* Frame bit numbers (B1 = 1) of each class, in table 5's order with the A/B pairs collapsed (table 6's order). */
static const uint8_t k_class0[] = {
    35, 36,  37,  38,  39,  40,  41,  42,  43,  47,  48,  56,  61,  62,  63,  64,  65,
    66, 67,  68,  69,  70,  74,  75,  83,  88,  89,  90,  91,  92,  93,  94,  95,  96,
    97, 101, 102, 110, 115, 116, 117, 118, 119, 120, 121, 122, 123, 124, 128, 129, 137,
};
static const uint8_t k_class1[] = {
    58, 85,  112, 54, 81, 108, 135, 50, 77, 104, 131, 45,  72, 99, 126, 55, 82, 109, 136,
    5,  13,  34,  8,  16, 17,  22,  23, 24, 25,  26,  6,   14, 7,  15,  60, 87, 114, 46,
    73, 100, 127, 44, 71, 98,  125, 33, 49, 76,  103, 130, 59, 86, 113, 57, 84, 111,
};
static const uint8_t k_class2[] = {
    18, 19, 20, 21, 31, 32, 53, 80, 107, 134, 1,   2,  3,  4,   9,
    10, 11, 12, 27, 28, 29, 30, 52, 79,  106, 133, 51, 78, 105, 132,
};

static int g_fail = 0;

static void
expect(int ok, const char* what) {
    if (!ok) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", what);
        g_fail++;
    }
}

/* Which input bit each output position took, recovered by feeding single set bits. */
static void
check_layout(int nframes) {
    const int nbits = nframes * FRAME_BITS;
    uint8_t in[2 * FRAME_BITS];
    uint8_t out[2 * FRAME_BITS];
    int source[2 * FRAME_BITS];
    for (int i = 0; i < nbits; i++) {
        source[i] = -1;
    }
    for (int bit = 0; bit < nbits; bit++) {
        DSD_MEMSET(in, 0, sizeof(in));
        DSD_MEMSET(out, 0, sizeof(out));
        in[bit] = 1;
        if (nframes == 2) {
            tetra_voice_reorder_full(in, out);
        } else {
            tetra_voice_reorder_half(in, out);
        }
        int hits = 0;
        for (int pos = 0; pos < nbits; pos++) {
            if (out[pos]) {
                hits++;
                source[pos] = bit;
            }
        }
        expect(hits == 1, "each input bit lands on exactly one frame bit");
    }

    /* Walk the classes as the type-1 order does and check where each bit went. */
    const uint8_t* const classes[3] = {k_class0, k_class1, k_class2};
    const int sizes[3] = {(int)sizeof(k_class0), (int)sizeof(k_class1), (int)sizeof(k_class2)};
    int input = 0;
    for (int c = 0; c < 3; c++) {
        for (int i = 0; i < sizes[c]; i++) {
            for (int frame = 0; frame < nframes; frame++) {
                const int pos = (frame * FRAME_BITS) + classes[c][i] - 1;
                expect(source[pos] == input, nframes == 2 ? "full slot: class bit on its frame bit number"
                                                          : "half slot: class bit on its frame bit number");
                input++;
            }
        }
    }
    expect(input == nbits, "the classes cover every type-1 bit");
}

int
main(void) {
    check_layout(2);
    check_layout(1);
    uint8_t out[2 * FRAME_BITS];
    DSD_MEMSET(out, 0xA5, sizeof(out));
    tetra_voice_reorder_full(NULL, out);
    tetra_voice_reorder_half(NULL, out);
    expect(out[0] == 0xA5 && out[(2 * FRAME_BITS) - 1] == 0xA5, "no input writes nothing");
    if (g_fail) {
        DSD_FPRINTF(stderr, "%d check(s) failed\n", g_fail);
        return 1;
    }
    puts("PASS test_tetra_speech_reorder");
    return 0;
}
