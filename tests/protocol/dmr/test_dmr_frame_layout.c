// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <dsd-neo/protocol/dmr/dmr_const.h>
#include <stdint.h>
#include <stdio.h>

static uint64_t
fnv1a_step(uint64_t hash, int value) {
    hash ^= (uint64_t)(uint8_t)value;
    return hash * 1099511628211ULL;
}

static void
test_dmr_ambe_interleave_schedule_signature(void) {
    uint64_t hash = 1469598103934665603ULL;
    for (size_t i = 0; i < 36U; i++) {
        hash = fnv1a_step(hash, dmr_ambe_interleave_w[i]);
        hash = fnv1a_step(hash, dmr_ambe_interleave_x[i]);
        hash = fnv1a_step(hash, dmr_ambe_interleave_y[i]);
        hash = fnv1a_step(hash, dmr_ambe_interleave_z[i]);
    }
    assert(hash == 0x1b74ec30c96673c3ULL);
}

static void
test_dmr_ambe_interleave_schedule_bounds(void) {
    uint8_t occupied[4][24] = {{0}};
    unsigned int row_counts[4] = {0};

    for (size_t i = 0; i < 36U; i++) {
        const int pairs[2][2] = {
            {dmr_ambe_interleave_w[i], dmr_ambe_interleave_x[i]},
            {dmr_ambe_interleave_y[i], dmr_ambe_interleave_z[i]},
        };

        for (size_t bit = 0; bit < 2U; bit++) {
            int row = pairs[bit][0];
            int col = pairs[bit][1];
            assert(row >= 0 && row < 4);
            assert(col >= 0 && col < 24);
            assert(occupied[row][col] == 0U);
            occupied[row][col] = 1U;
            row_counts[row]++;
        }
    }

    assert(row_counts[0] == 24U);
    assert(row_counts[1] == 23U);
    assert(row_counts[2] == 11U);
    assert(row_counts[3] == 14U);
}

int
main(void) {
    test_dmr_ambe_interleave_schedule_signature();
    test_dmr_ambe_interleave_schedule_bounds();
    printf("DMR frame layout: OK\n");
    return 0;
}
