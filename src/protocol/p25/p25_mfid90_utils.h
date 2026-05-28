// SPDX-License-Identifier: ISC
#ifndef DSD_NEO_PROTOCOL_P25_MFID90_UTILS_H
#define DSD_NEO_PROTOCOL_P25_MFID90_UTILS_H

#include <stddef.h>
#include <stdint.h>

int p25_mfid90_base_station_id_decode(const uint8_t tsbk_byte[12], char* cwid, size_t cwid_size, uint16_t* channel);

#endif // DSD_NEO_PROTOCOL_P25_MFID90_UTILS_H
