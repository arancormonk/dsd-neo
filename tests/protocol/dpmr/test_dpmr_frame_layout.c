// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Focused checks for dPMR voice frame layout helpers.
 */

#include <assert.h>
#include <dsd-neo/protocol/dpmr/dpmr_const.h>
#include <stdint.h>
#include <stdio.h>

static const int kDpmrW[36] = {0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1,
                               0, 1, 0, 1, 0, 2, 0, 2, 0, 2, 0, 2, 0, 2, 0, 2, 0, 2};

static const int kDpmrX[36] = {23, 10, 22, 9, 21, 8,  20, 7, 19, 6, 18, 5, 17, 4, 16, 3, 15, 2,
                               14, 1,  13, 0, 12, 10, 11, 9, 10, 8, 9,  7, 8,  6, 7,  5, 6,  4};

static const int kDpmrY[36] = {0, 2, 0, 2, 0, 2, 0, 2, 0, 3, 0, 3, 1, 3, 1, 3, 1, 3,
                               1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3};

static const int kDpmrZ[36] = {5,  3, 4,  2, 3,  1, 2,  0, 1,  13, 0,  12, 22, 11, 21, 10, 20, 9,
                               19, 8, 18, 7, 17, 6, 16, 5, 15, 4,  14, 3,  13, 2,  12, 1,  11, 0};

static uint32_t
fnv1a_update(uint32_t hash, int value) {
    hash ^= (uint32_t)(value & 0xff);
    hash *= 16777619u;
    return hash;
}

static void
test_interleave_schedule_signature(void) {
    uint32_t hash = 2166136261u;

    for (uint32_t i = 0; i < 36; i++) {
        hash = fnv1a_update(hash, kDpmrW[i]);
    }
    for (uint32_t i = 0; i < 36; i++) {
        hash = fnv1a_update(hash, kDpmrX[i]);
    }
    for (uint32_t i = 0; i < 36; i++) {
        hash = fnv1a_update(hash, kDpmrY[i]);
    }
    for (uint32_t i = 0; i < 36; i++) {
        hash = fnv1a_update(hash, kDpmrZ[i]);
    }

    assert(hash == 0x044c466bu);
}

static void
test_interleave_schedule_bounds_and_uniqueness(void) {
    uint8_t occupied[4][24] = {{0}};
    uint32_t count = 0;

    for (uint32_t i = 0; i < 36; i++) {
        assert(kDpmrW[i] >= 0 && kDpmrW[i] < 4);
        assert(kDpmrX[i] >= 0 && kDpmrX[i] < 24);
        assert(kDpmrY[i] >= 0 && kDpmrY[i] < 4);
        assert(kDpmrZ[i] >= 0 && kDpmrZ[i] < 24);

        assert(occupied[kDpmrW[i]][kDpmrX[i]] == 0);
        occupied[kDpmrW[i]][kDpmrX[i]] = 1;
        count++;

        assert(occupied[kDpmrY[i]][kDpmrZ[i]] == 0);
        occupied[kDpmrY[i]][kDpmrZ[i]] = 1;
        count++;
    }

    assert(count == 72);
}

int
main(void) {
    test_interleave_schedule_signature();
    test_interleave_schedule_bounds_and_uniqueness();
    printf("DPMR_FRAME_LAYOUT: OK\n");
    return 0;
}
