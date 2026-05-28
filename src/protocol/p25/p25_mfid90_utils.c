// SPDX-License-Identifier: ISC
#include "p25_mfid90_utils.h"

int
p25_mfid90_base_station_id_decode(const uint8_t tsbk_byte[12], char* cwid, size_t cwid_size, uint16_t* channel) {
    if (!tsbk_byte) {
        return -1;
    }

    if (channel) {
        *channel = (uint16_t)((tsbk_byte[8] << 8) | tsbk_byte[9]);
    }

    if (cwid && cwid_size > 0) {
        size_t out = 0;
        cwid[0] = '\0';
        for (int field = 0; field < 8; field++) {
            uint8_t value = 0;
            int bit_start = field * 6;
            for (int bit = 0; bit < 6; bit++) {
                int bit_index = bit_start + bit;
                uint8_t octet = tsbk_byte[2 + (bit_index / 8)];
                int shift = 7 - (bit_index % 8);
                value = (uint8_t)((value << 1) | ((octet >> shift) & 1u));
            }
            if (value == 0) {
                continue;
            }
            if (out + 1u < cwid_size) {
                cwid[out++] = (char)(value + 43u);
            }
        }
        cwid[out] = '\0';
        return (int)out;
    }

    return 0;
}
