// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Focused checks for dPMR voice frame layout helpers.
 */

#include <assert.h>
#include <dsd-neo/protocol/dpmr/dpmr_const.h>
#include <stdint.h>
#include <stdio.h>

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
        hash = fnv1a_update(hash, dpmr_ambe_interleave_w[i]);
    }
    for (uint32_t i = 0; i < 36; i++) {
        hash = fnv1a_update(hash, dpmr_ambe_interleave_x[i]);
    }
    for (uint32_t i = 0; i < 36; i++) {
        hash = fnv1a_update(hash, dpmr_ambe_interleave_y[i]);
    }
    for (uint32_t i = 0; i < 36; i++) {
        hash = fnv1a_update(hash, dpmr_ambe_interleave_z[i]);
    }

    assert(hash == 0x044c466bu);
}

static void
test_interleave_schedule_bounds_and_uniqueness(void) {
    uint8_t occupied[4][24] = {{0}};
    uint32_t count = 0;

    for (uint32_t i = 0; i < 36; i++) {
        assert(dpmr_ambe_interleave_w[i] >= 0 && dpmr_ambe_interleave_w[i] < 4);
        assert(dpmr_ambe_interleave_x[i] >= 0 && dpmr_ambe_interleave_x[i] < 24);
        assert(dpmr_ambe_interleave_y[i] >= 0 && dpmr_ambe_interleave_y[i] < 4);
        assert(dpmr_ambe_interleave_z[i] >= 0 && dpmr_ambe_interleave_z[i] < 24);

        assert(occupied[dpmr_ambe_interleave_w[i]][dpmr_ambe_interleave_x[i]] == 0);
        occupied[dpmr_ambe_interleave_w[i]][dpmr_ambe_interleave_x[i]] = 1;
        count++;

        assert(occupied[dpmr_ambe_interleave_y[i]][dpmr_ambe_interleave_z[i]] == 0);
        occupied[dpmr_ambe_interleave_y[i]][dpmr_ambe_interleave_z[i]] = 1;
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
