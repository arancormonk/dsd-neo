// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DSD_NEO_PROTOCOL_TETRA_SYNC_H
#define DSD_NEO_PROTOCOL_TETRA_SYNC_H

#include <stdint.h>

// TETRA sync patterns (from osmo-tetra src/phy/tetra_burst.c)
static const uint8_t TETRA_N_BITS[22] = {
    1,1, 0,1, 0,0, 0,0, 1,1, 1,0, 1,0, 0,1, 1,1, 0,1, 0,0
};
static const uint8_t TETRA_P_BITS[22] = {
    0,1, 1,1, 1,0, 1,0, 0,1, 0,0, 0,0, 1,1, 0,1, 1,1, 1,0
};
static const uint8_t TETRA_Q_BITS[22] = {
    1,0, 1,1, 0,1, 1,1, 0,0, 0,0, 0,1, 1,0, 1,0, 1,1, 0,1
};
static const uint8_t TETRA_Y_BITS[38] = {
    1,1, 0,0, 0,0, 0,1, 1,0, 0,1, 1,1, 0,0, 1,1, 1,0, 1,0,
    0,1, 1,1, 0,0, 0,0, 0,1, 1,0, 0,1, 1,1
};

#endif // DSD_NEO_PROTOCOL_TETRA_SYNC_H
