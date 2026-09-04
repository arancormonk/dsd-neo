// SPDX-License-Identifier: GPL-3.0-or-later

#include "dmr_hytera.h"

uint8_t
dmr_hytera_checksum(const uint8_t* bytes, size_t length) {
    uint8_t checksum = 0;
    for (size_t index = 0; index < length; index++) {
        checksum = (uint8_t)(checksum + bytes[index]);
    }
    return (uint8_t)(0U - checksum);
}
