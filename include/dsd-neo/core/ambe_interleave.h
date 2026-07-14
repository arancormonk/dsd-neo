// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2010 DSD Author
 */

/**
 * @file
 * @brief Canonical AMBE 3600x2450 dibit-to-frame interleave schedule.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_CORE_AMBE_INTERLEAVE_H_
#define DSD_NEO_INCLUDE_DSD_NEO_CORE_AMBE_INTERLEAVE_H_

#include <stdint.h>

#define DSD_AMBE_2450_DIBITS 36

typedef struct {
    uint8_t high_row;
    uint8_t high_col;
    uint8_t low_row;
    uint8_t low_col;
} dsd_ambe_2450_dibit_map_entry;

static const dsd_ambe_2450_dibit_map_entry dsd_ambe_2450_dibit_map[DSD_AMBE_2450_DIBITS] = {
    {0, 23, 0, 5},  {1, 10, 2, 3}, {0, 22, 0, 4},  {1, 9, 2, 2},  {0, 21, 0, 3},  {1, 8, 2, 1},
    {0, 20, 0, 2},  {1, 7, 2, 0},  {0, 19, 0, 1},  {1, 6, 3, 13}, {0, 18, 0, 0},  {1, 5, 3, 12},
    {0, 17, 1, 22}, {1, 4, 3, 11}, {0, 16, 1, 21}, {1, 3, 3, 10}, {0, 15, 1, 20}, {1, 2, 3, 9},
    {0, 14, 1, 19}, {1, 1, 3, 8},  {0, 13, 1, 18}, {1, 0, 3, 7},  {0, 12, 1, 17}, {2, 10, 3, 6},
    {0, 11, 1, 16}, {2, 9, 3, 5},  {0, 10, 1, 15}, {2, 8, 3, 4},  {0, 9, 1, 14},  {2, 7, 3, 3},
    {0, 8, 1, 13},  {2, 6, 3, 2},  {0, 7, 1, 12},  {2, 5, 3, 1},  {0, 6, 1, 11},  {2, 4, 3, 0},
};

#endif /* DSD_NEO_INCLUDE_DSD_NEO_CORE_AMBE_INTERLEAVE_H_ */
