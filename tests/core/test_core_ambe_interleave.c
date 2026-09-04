// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <dsd-neo/core/ambe_interleave.h>
#include <stddef.h>
#include <stdint.h>

static uint64_t
fnv1a_step(uint64_t hash, uint8_t value) {
    hash ^= value;
    return hash * UINT64_C(1099511628211);
}

int
main(void) {
    uint64_t hash = UINT64_C(1469598103934665603);
    uint8_t occupied[4][24] = {{0}};
    unsigned int row_counts[4] = {0};

    _Static_assert(sizeof dsd_ambe_2450_dibit_map / sizeof dsd_ambe_2450_dibit_map[0] == DSD_AMBE_2450_DIBITS,
                   "AMBE 2450 dibit map length");

    for (size_t i = 0U; i < DSD_AMBE_2450_DIBITS; i++) {
        const dsd_ambe_2450_dibit_map_entry* map = &dsd_ambe_2450_dibit_map[i];
        const uint8_t coordinates[4] = {map->high_row, map->high_col, map->low_row, map->low_col};
        for (size_t coordinate = 0U; coordinate < 4U; coordinate++) {
            hash = fnv1a_step(hash, coordinates[coordinate]);
        }

        assert(map->high_row < 4U);
        assert(map->high_col < 24U);
        assert(map->low_row < 4U);
        assert(map->low_col < 24U);
        assert(occupied[map->high_row][map->high_col] == 0U);
        assert(occupied[map->low_row][map->low_col] == 0U);
        occupied[map->high_row][map->high_col] = 1U;
        occupied[map->low_row][map->low_col] = 1U;
        row_counts[map->high_row]++;
        row_counts[map->low_row]++;
    }

    assert(hash == UINT64_C(0x1B74EC30C96673C3));
    assert(row_counts[0] == 24U);
    assert(row_counts[1] == 23U);
    assert(row_counts[2] == 11U);
    assert(row_counts[3] == 14U);
    return 0;
}
