// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief OP25-compatible P25 CQPSK dibit orientation maps.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_CORE_P25_CQPSK_DIBIT_H_
#define DSD_NEO_INCLUDE_DSD_NEO_CORE_P25_CQPSK_DIBIT_H_

#include <stdint.h>

#define DSD_P25_CQPSK_DIBIT_MAP_IDENTITY 0u
#define DSD_P25_CQPSK_DIBIT_MAP_REV_P    1u
#define DSD_P25_CQPSK_DIBIT_MAP_X2400    2u
#define DSD_P25_CQPSK_DIBIT_MAP_N1200    3u
#define DSD_P25_CQPSK_DIBIT_MAP_P1200    4u
#define DSD_P25_CQPSK_DIBIT_MAP_COUNT    5u

static inline uint8_t
dsd_p25_cqpsk_dibit_map_index(uint8_t map_idx) {
    return (map_idx < DSD_P25_CQPSK_DIBIT_MAP_COUNT) ? map_idx : DSD_P25_CQPSK_DIBIT_MAP_IDENTITY;
}

static inline uint8_t
dsd_p25_cqpsk_correct_dibit(uint8_t map_idx, uint8_t dibit) {
    static const uint8_t maps[DSD_P25_CQPSK_DIBIT_MAP_COUNT][4] = {
        {0, 1, 2, 3}, /* normal */
        {2, 3, 0, 1}, /* reverse polarity */
        {3, 2, 1, 0}, /* OP25 X2400 */
        {1, 3, 0, 2}, /* OP25 N1200 */
        {2, 0, 3, 1}, /* OP25 P1200 */
    };
    return maps[dsd_p25_cqpsk_dibit_map_index(map_idx)][dibit & 0x3u];
}

static inline uint8_t
dsd_p25_cqpsk_raw_dibit_for_corrected(uint8_t map_idx, uint8_t corrected_dibit) {
    uint8_t wanted = corrected_dibit & 0x3u;
    uint8_t idx = dsd_p25_cqpsk_dibit_map_index(map_idx);
    for (uint8_t raw = 0; raw < 4u; raw++) {
        if (dsd_p25_cqpsk_correct_dibit(idx, raw) == wanted) {
            return raw;
        }
    }
    return wanted;
}

#endif /* DSD_NEO_INCLUDE_DSD_NEO_CORE_P25_CQPSK_DIBIT_H_ */
