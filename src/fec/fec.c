// SPDX-License-Identifier: GPL-3.0-or-later
///////////////////////////////////////////////////////////////////////////////////
// Copyright (C) 2016 Edouard Griffiths, F4EXB.                                  //
//                                                                               //
// This program is free software; you can redistribute it and/or modify          //
// it under the terms of the GNU General Public License as published by          //
// the Free Software Foundation as version 3 of the License, or                  //
//                                                                               //
// This program is distributed in the hope that it will be useful,               //
// but WITHOUT ANY WARRANTY; without even the implied warranty of                //
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the                  //
// GNU General Public License V3 for more details.                               //
//                                                                               //
// You should have received a copy of the GNU General Public License             //
// along with this program. If not, see <http://www.gnu.org/licenses/>.          //
///////////////////////////////////////////////////////////////////////////////////

#include <dsd-neo/fec/block_codes.h>
#include <stdbool.h>
#include <string.h>
#include "dsd-neo/core/safe_api.h"

static unsigned char Hamming_7_4_m_corr[8]; //!< single bit error correction by syndrome index

//!< Parity check matrix of bits
static const unsigned char Hamming_7_4_m_H[7 * 3] = {
    1, 1, 1, 0, 1, 0, 0, 0, 1, 1, 1,
    0, 1, 0, 1, 1, 0, 1, 0, 0, 1
    //      0  1  2  3 <- correctable bit positions
};

// ========================================================================================

static unsigned char Hamming_12_8_m_corr[16]; //!< single bit error correction by syndrome index

//!< Parity check matrix of bits
static const unsigned char Hamming_12_8_m_H[12 * 4] = {
    1, 0, 1, 0, 1, 1, 0, 0, 1, 0, 0, 0, 1, 1, 0, 1, 0, 1, 1, 0, 0, 1, 0, 0, 1,
    1, 1, 0, 1, 0, 1, 1, 0, 0, 1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 0, 0, 0, 1
    //      0  1  2  3  4  5  6  7 <- correctable bit positions
};

// ========================================================================================

static unsigned char Hamming_13_9_m_corr[16]; //!< single bit error correction by syndrome index

//!< Parity check matrix of bits
static const unsigned char Hamming_13_9_m_H[13 * 4] = {
    1, 1, 0, 1, 0, 1, 1, 0, 0, 1, 0, 0, 0, 1, 1, 1, 0, 1, 0, 1, 1, 0, 0, 1, 0, 0,
    1, 1, 1, 1, 0, 1, 0, 1, 1, 0, 0, 1, 0, 1, 0, 1, 0, 1, 1, 0, 0, 1, 0, 0, 0, 1,
    //      0  1  2  3  4  5  6  7  8  <- correctable bit positions
};

// ========================================================================================

static unsigned char Hamming_15_11_m_corr[16]; //!< single bit error correction by syndrome index

//!< Parity check matrix of bits
static const unsigned char Hamming_15_11_m_H[15 * 4] = {
    1, 1, 1, 1, 0, 1, 0, 1, 1, 0, 0, 1, 0, 0, 0, 0, 1, 1, 1, 1, 0, 1, 0, 1, 1, 0, 0, 1, 0, 0,
    0, 0, 1, 1, 1, 1, 0, 1, 0, 1, 1, 0, 0, 1, 0, 1, 1, 1, 0, 1, 0, 1, 1, 0, 0, 1, 0, 0, 0, 1,
    //      0  1  2  3  4  5  6  7  8  9 10  <- correctable bit positions
};

// ========================================================================================

static unsigned char Hamming_16_11_4_m_corr[32]; //!< single bit error correction by syndrome index

//!< Parity check matrix of bits
static const unsigned char Hamming_16_11_4_m_H[16 * 5] = {1, 1, 1, 1, 0, 1, 0, 1, 1, 0, 0, 1, 0, 0, 0, 0, 0, 1, 1, 1,
                                                          1, 0, 1, 0, 1, 1, 0, 0, 1, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0, 1,
                                                          0, 1, 1, 0, 0, 1, 0, 0, 1, 1, 1, 0, 1, 0, 1, 1, 0, 0, 1, 0,
                                                          0, 0, 1, 0, 1, 0, 1, 0, 0, 1, 1, 0, 1, 1, 1, 0, 0, 0, 0, 1};

// ========================================================================================

static unsigned char Golay_20_8_m_corr[4096][3]; //!< up to 3 bit error correction by syndrome index

//!< Parity check matrix of bits
static const unsigned char Golay_20_8_m_H[20 * 12] = {
    0, 1, 0, 0, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 1, 1, 0, 1, 0, 0, 0,
    0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 0, 1, 1, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 1, 1,
    0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0,
    1, 1, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 1, 1, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0,
    0, 1, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 1, 0, 0, 1, 1, 1, 1, 1, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 1, 1, 1, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1,
};

// ========================================================================================

static unsigned char Golay_24_12_m_corr[4096][3]; //!< up to 3 bit error correction by syndrome index

//!< Generator matrix of bits
static const unsigned char Golay_24_12_m_G[24 * 12] = {
    1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 1, 1, 1, 0, 1, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 1, 1, 0, 0, 0, 1, 1, 1, 0, 1, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0, 1, 1, 0, 1, 0, 0, 0,
    0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0, 1, 1, 0, 1, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 1, 1, 1, 1, 0, 1, 1, 0, 1, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 1, 1, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1,
    0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 1, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0,
    0, 0, 1, 1, 0, 1, 1, 0, 0, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 1, 0, 1, 1, 1, 0, 0, 0, 1, 1, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 1, 0, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0,
    1, 0, 0, 1, 0, 0, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 1, 1, 1, 0, 1, 0, 1, 1,
};

//!< Parity check matrix of bits
static const unsigned char Golay_24_12_m_H[24 * 12] = {
    1, 0, 1, 0, 0, 1, 0, 0, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0, 1, 1, 0, 1, 0, 0, 0,
    0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0, 1, 1, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 1, 1, 1, 1, 0, 1, 1, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0, 1, 1, 0, 1,
    0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 0, 1, 0, 1, 1, 1, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0,
    1, 1, 1, 1, 0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 1, 0, 1, 1, 1, 0, 0, 0, 1, 1, 0,
    0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 1, 0, 1, 1, 1, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0,
    1, 0, 0, 1, 0, 0, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 1, 0, 0, 1, 1, 1, 1, 1,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 1, 0, 0, 0, 1, 1, 1, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1,
};

// ========================================================================================

static unsigned char QR_16_7_6_m_corr[512][2]; //!< up to 2 bit error correction by syndrome index

//!< Parity check matrix of bits
static const unsigned char QR_16_7_6_m_H[16 * 9] = {
    0, 1, 1, 1, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 1,
    1, 1, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 1, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 1, 0,
    0, 0, 0, 1, 0, 0, 0, 0, 1, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 1, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0,
    0, 1, 0, 0, 1, 1, 1, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 0, 1, 0, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 1,
};

// ========================================================================================

void
Hamming_7_4_init(void) {
    // correctable bit positions given syndrome bits as index (see above)
    DSD_MEMSET(Hamming_7_4_m_corr, 0xFF, 8); // initialize with all invalid positions
    Hamming_7_4_m_corr[5] = 0;
    Hamming_7_4_m_corr[7] = 1;
    Hamming_7_4_m_corr[6] = 2;
    Hamming_7_4_m_corr[3] = 3;
    Hamming_7_4_m_corr[4] = 4;
    Hamming_7_4_m_corr[2] = 5;
    Hamming_7_4_m_corr[1] = 6;
}

bool
Hamming_7_4_decode(unsigned char* rxBits) // corrects in place
{
    unsigned int syndromeI = 0; // syndrome index

    for (int is = 0; is < 3; is++) {
        syndromeI += (((rxBits[0] * Hamming_7_4_m_H[7 * is + 0]) + (rxBits[1] * Hamming_7_4_m_H[7 * is + 1])
                       + (rxBits[2] * Hamming_7_4_m_H[7 * is + 2]) + (rxBits[3] * Hamming_7_4_m_H[7 * is + 3])
                       + (rxBits[4] * Hamming_7_4_m_H[7 * is + 4]) + (rxBits[5] * Hamming_7_4_m_H[7 * is + 5])
                       + (rxBits[6] * Hamming_7_4_m_H[7 * is + 6]))
                      % 2)
                     << (2 - is);
    }

    if (syndromeI > 0) {
        if (Hamming_7_4_m_corr[syndromeI] == 0xFF) {
            return false;
        }
        rxBits[Hamming_7_4_m_corr[syndromeI]] ^= 1; // flip bit
    }

    return true;
}

// ========================================================================================

void
Hamming_12_8_init(void) {
    // correctable bit positions given syndrome bits as index (see above)
    DSD_MEMSET(Hamming_12_8_m_corr, 0xFF, 16); // initialize with all invalid positions
    Hamming_12_8_m_corr[14] = 0;
    Hamming_12_8_m_corr[7] = 1;
    Hamming_12_8_m_corr[10] = 2;
    Hamming_12_8_m_corr[5] = 3;
    Hamming_12_8_m_corr[11] = 4;
    Hamming_12_8_m_corr[12] = 5;
    Hamming_12_8_m_corr[6] = 6;
    Hamming_12_8_m_corr[3] = 7;
    Hamming_12_8_m_corr[8] = 8;
    Hamming_12_8_m_corr[4] = 9;
    Hamming_12_8_m_corr[2] = 10;
    Hamming_12_8_m_corr[1] = 11;
}

bool
Hamming_12_8_decode(unsigned char* rxBits, unsigned char* decodedBits, int nbCodewords) {
    bool correctable = true;

    for (int ic = 0; ic < nbCodewords; ic++) {
        // calculate syndrome

        int syndromeI = 0; // syndrome index

        for (int is = 0; is < 4; is++) {
            syndromeI += (((rxBits[12 * ic + 0] * Hamming_12_8_m_H[12 * is + 0])
                           + (rxBits[12 * ic + 1] * Hamming_12_8_m_H[12 * is + 1])
                           + (rxBits[12 * ic + 2] * Hamming_12_8_m_H[12 * is + 2])
                           + (rxBits[12 * ic + 3] * Hamming_12_8_m_H[12 * is + 3])
                           + (rxBits[12 * ic + 4] * Hamming_12_8_m_H[12 * is + 4])
                           + (rxBits[12 * ic + 5] * Hamming_12_8_m_H[12 * is + 5])
                           + (rxBits[12 * ic + 6] * Hamming_12_8_m_H[12 * is + 6])
                           + (rxBits[12 * ic + 7] * Hamming_12_8_m_H[12 * is + 7])
                           + (rxBits[12 * ic + 8] * Hamming_12_8_m_H[12 * is + 8])
                           + (rxBits[12 * ic + 9] * Hamming_12_8_m_H[12 * is + 9])
                           + (rxBits[12 * ic + 10] * Hamming_12_8_m_H[12 * is + 10])
                           + (rxBits[12 * ic + 11] * Hamming_12_8_m_H[12 * is + 11]))
                          % 2)
                         << (3 - is);
        }

        // correct bit

        if (syndromeI > 0) // single bit error correction
        {
            if (Hamming_12_8_m_corr[syndromeI] == 0xFF) // uncorrectable error
            {
                correctable = false;
            } else {
                rxBits[Hamming_12_8_m_corr[syndromeI]] ^= 1; // flip bit
            }
        }

        // move information bits
        DSD_MEMCPY(&decodedBits[(size_t)ic * 8u], &rxBits[(size_t)ic * 12u], (size_t)8u);
    }

    return correctable;
}

// ========================================================================================

void
Hamming_13_9_init(void) {
    // correctable bit positions given syndrome bits as index (see above)
    DSD_MEMSET(Hamming_13_9_m_corr, 0xFF, 16); // initialize with all invalid positions
    Hamming_13_9_m_corr[15] = 0;
    Hamming_13_9_m_corr[14] = 1;
    Hamming_13_9_m_corr[7] = 2;
    Hamming_13_9_m_corr[10] = 3;
    Hamming_13_9_m_corr[5] = 4;
    Hamming_13_9_m_corr[11] = 5;
    Hamming_13_9_m_corr[12] = 6;
    Hamming_13_9_m_corr[6] = 7;
    Hamming_13_9_m_corr[3] = 8;
    Hamming_13_9_m_corr[8] = 9;
    Hamming_13_9_m_corr[4] = 10;
    Hamming_13_9_m_corr[2] = 11;
    Hamming_13_9_m_corr[1] = 12;
}

bool
Hamming_13_9_decode(unsigned char* rxBits, unsigned char* decodedBits, int nbCodewords) {
    bool correctable = true;

    for (int ic = 0; ic < nbCodewords; ic++) {
        // calculate syndrome

        int syndromeI = 0; // syndrome index

        for (int is = 0; is < 4; is++) {
            syndromeI += (((rxBits[13 * ic + 0] * Hamming_13_9_m_H[13 * is + 0])
                           + (rxBits[13 * ic + 1] * Hamming_13_9_m_H[13 * is + 1])
                           + (rxBits[13 * ic + 2] * Hamming_13_9_m_H[13 * is + 2])
                           + (rxBits[13 * ic + 3] * Hamming_13_9_m_H[13 * is + 3])
                           + (rxBits[13 * ic + 4] * Hamming_13_9_m_H[13 * is + 4])
                           + (rxBits[13 * ic + 5] * Hamming_13_9_m_H[13 * is + 5])
                           + (rxBits[13 * ic + 6] * Hamming_13_9_m_H[13 * is + 6])
                           + (rxBits[13 * ic + 7] * Hamming_13_9_m_H[13 * is + 7])
                           + (rxBits[13 * ic + 8] * Hamming_13_9_m_H[13 * is + 8])
                           + (rxBits[13 * ic + 9] * Hamming_13_9_m_H[13 * is + 9])
                           + (rxBits[13 * ic + 10] * Hamming_13_9_m_H[13 * is + 10])
                           + (rxBits[13 * ic + 11] * Hamming_13_9_m_H[13 * is + 11])
                           + (rxBits[13 * ic + 12] * Hamming_13_9_m_H[13 * is + 12]))
                          % 2)
                         << (3 - is);
        }

        // correct bit

        if (syndromeI > 0) // single bit error correction
        {
            if (Hamming_13_9_m_corr[syndromeI] == 0xFF) // uncorrectable error
            {
                correctable = false;
                break;
            } else {
                rxBits[Hamming_13_9_m_corr[syndromeI]] ^= 1; // flip bit
            }
        }

        // move information bits
        if (decodedBits) {
            DSD_MEMCPY(&decodedBits[(size_t)ic * 9u], &rxBits[(size_t)ic * 13u], (size_t)9u);
        }
    }

    return correctable;
}

// ========================================================================================

void
Hamming_15_11_init(void) {
    // correctable bit positions given syndrome bits as index (see above)
    DSD_MEMSET(Hamming_15_11_m_corr, 0xFF, 16); // initialize with all invalid positions
    Hamming_15_11_m_corr[9] = 0;
    Hamming_15_11_m_corr[13] = 1;
    Hamming_15_11_m_corr[15] = 2;
    Hamming_15_11_m_corr[14] = 3;
    Hamming_15_11_m_corr[7] = 4;
    Hamming_15_11_m_corr[10] = 5;
    Hamming_15_11_m_corr[5] = 6;
    Hamming_15_11_m_corr[11] = 7;
    Hamming_15_11_m_corr[12] = 8;
    Hamming_15_11_m_corr[6] = 9;
    Hamming_15_11_m_corr[3] = 10;
    Hamming_15_11_m_corr[8] = 11;
    Hamming_15_11_m_corr[4] = 12;
    Hamming_15_11_m_corr[2] = 13;
    Hamming_15_11_m_corr[1] = 14;
}

bool
Hamming_15_11_decode(unsigned char* rxBits, unsigned char* decodedBits, int nbCodewords) {
    bool correctable = true;

    for (int ic = 0; ic < nbCodewords; ic++) {
        // calculate syndrome

        int syndromeI = 0; // syndrome index

        for (int is = 0; is < 4; is++) {
            syndromeI += (((rxBits[15 * ic + 0] * Hamming_15_11_m_H[15 * is + 0])
                           + (rxBits[15 * ic + 1] * Hamming_15_11_m_H[15 * is + 1])
                           + (rxBits[15 * ic + 2] * Hamming_15_11_m_H[15 * is + 2])
                           + (rxBits[15 * ic + 3] * Hamming_15_11_m_H[15 * is + 3])
                           + (rxBits[15 * ic + 4] * Hamming_15_11_m_H[15 * is + 4])
                           + (rxBits[15 * ic + 5] * Hamming_15_11_m_H[15 * is + 5])
                           + (rxBits[15 * ic + 6] * Hamming_15_11_m_H[15 * is + 6])
                           + (rxBits[15 * ic + 7] * Hamming_15_11_m_H[15 * is + 7])
                           + (rxBits[15 * ic + 8] * Hamming_15_11_m_H[15 * is + 8])
                           + (rxBits[15 * ic + 9] * Hamming_15_11_m_H[15 * is + 9])
                           + (rxBits[15 * ic + 10] * Hamming_15_11_m_H[15 * is + 10])
                           + (rxBits[15 * ic + 11] * Hamming_15_11_m_H[15 * is + 11])
                           + (rxBits[15 * ic + 12] * Hamming_15_11_m_H[15 * is + 12])
                           + (rxBits[15 * ic + 13] * Hamming_15_11_m_H[15 * is + 13])
                           + (rxBits[15 * ic + 14] * Hamming_15_11_m_H[15 * is + 14]))
                          % 2)
                         << (3 - is);
        }

        // correct bit

        if (syndromeI > 0) // single bit error correction
        {
            if (Hamming_15_11_m_corr[syndromeI] == 0xFF) // uncorrectable error
            {
                correctable = false;
                break;
            } else {
                rxBits[Hamming_15_11_m_corr[syndromeI]] ^= 1; // flip bit
            }
        }

        // move information bits
        if (decodedBits) {
            DSD_MEMCPY(&decodedBits[(size_t)ic * 11u], &rxBits[(size_t)ic * 15u], (size_t)11u);
        }
    }

    return correctable;
}

// ========================================================================================

void
Hamming_16_11_4_init(void) {
    // correctable bit positions given syndrome bits as index (see above)
    DSD_MEMSET(Hamming_16_11_4_m_corr, 0xFF, 32); // initialize with all invalid positions
    Hamming_16_11_4_m_corr[19] = 0;
    Hamming_16_11_4_m_corr[26] = 1;
    Hamming_16_11_4_m_corr[31] = 2;
    Hamming_16_11_4_m_corr[28] = 3;
    Hamming_16_11_4_m_corr[14] = 4;
    Hamming_16_11_4_m_corr[21] = 5;
    Hamming_16_11_4_m_corr[11] = 6;
    Hamming_16_11_4_m_corr[22] = 7;
    Hamming_16_11_4_m_corr[25] = 8;
    Hamming_16_11_4_m_corr[13] = 9;
    Hamming_16_11_4_m_corr[7] = 10;
    Hamming_16_11_4_m_corr[16] = 11;
    Hamming_16_11_4_m_corr[8] = 12;
    Hamming_16_11_4_m_corr[4] = 13;
    Hamming_16_11_4_m_corr[2] = 14;
    Hamming_16_11_4_m_corr[1] = 15;
}

bool
Hamming_16_11_4_decode(unsigned char* rxBits, unsigned char* decodedBits, int nbCodewords) {
    bool correctable = true;

    for (int ic = 0; ic < nbCodewords; ic++) {
        // calculate syndrome
        int syndromeI = 0; // syndrome index
        for (int is = 0; is < 5; is++) {
            syndromeI += (((rxBits[16 * ic + 0] * Hamming_16_11_4_m_H[16 * is + 0])
                           + (rxBits[16 * ic + 1] * Hamming_16_11_4_m_H[16 * is + 1])
                           + (rxBits[16 * ic + 2] * Hamming_16_11_4_m_H[16 * is + 2])
                           + (rxBits[16 * ic + 3] * Hamming_16_11_4_m_H[16 * is + 3])
                           + (rxBits[16 * ic + 4] * Hamming_16_11_4_m_H[16 * is + 4])
                           + (rxBits[16 * ic + 5] * Hamming_16_11_4_m_H[16 * is + 5])
                           + (rxBits[16 * ic + 6] * Hamming_16_11_4_m_H[16 * is + 6])
                           + (rxBits[16 * ic + 7] * Hamming_16_11_4_m_H[16 * is + 7])
                           + (rxBits[16 * ic + 8] * Hamming_16_11_4_m_H[16 * is + 8])
                           + (rxBits[16 * ic + 9] * Hamming_16_11_4_m_H[16 * is + 9])
                           + (rxBits[16 * ic + 10] * Hamming_16_11_4_m_H[16 * is + 10])
                           + (rxBits[16 * ic + 11] * Hamming_16_11_4_m_H[16 * is + 11])
                           + (rxBits[16 * ic + 12] * Hamming_16_11_4_m_H[16 * is + 12])
                           + (rxBits[16 * ic + 13] * Hamming_16_11_4_m_H[16 * is + 13])
                           + (rxBits[16 * ic + 14] * Hamming_16_11_4_m_H[16 * is + 14])
                           + (rxBits[16 * ic + 15] * Hamming_16_11_4_m_H[16 * is + 15]))
                          % 2)
                         << (4 - is);
        }

        // correct bit

        if (syndromeI > 0) // single bit error correction
        {
            if (Hamming_16_11_4_m_corr[syndromeI] == 0xFF) // uncorrectable error
            {
                correctable = false;
                break;
            } else {
                rxBits[Hamming_16_11_4_m_corr[syndromeI]] ^= 1; // flip bit
            }
        }

        // move information bits
        if (decodedBits) {
            DSD_MEMCPY(&decodedBits[(size_t)ic * 11u], &rxBits[(size_t)ic * 16u], (size_t)11u);
        }
    }

    return correctable;
}

void
Golay_20_8_init(void) {
    DSD_MEMSET(Golay_20_8_m_corr, 0xFF, sizeof(Golay_20_8_m_corr));

    for (int i1 = 0; i1 < 8; i1++) {
        for (int i2 = i1 + 1; i2 < 8; i2++) {
            for (int i3 = i2 + 1; i3 < 8; i3++) {
                // 3 bit patterns
                int syndromeI = 0;

                for (int ir = 0; ir < 12; ir++) {
                    syndromeI +=
                        ((Golay_20_8_m_H[20 * ir + i1] + Golay_20_8_m_H[20 * ir + i2] + Golay_20_8_m_H[20 * ir + i3])
                         % 2)
                        << (11 - ir);
                }

                Golay_20_8_m_corr[syndromeI][0] = i1;
                Golay_20_8_m_corr[syndromeI][1] = i2;
                Golay_20_8_m_corr[syndromeI][2] = i3;
            }

            // 2 bit patterns
            int syndromeI = 0;

            for (int ir = 0; ir < 12; ir++) {
                syndromeI += ((Golay_20_8_m_H[20 * ir + i1] + Golay_20_8_m_H[20 * ir + i2]) % 2) << (11 - ir);
            }

            Golay_20_8_m_corr[syndromeI][0] = i1;
            Golay_20_8_m_corr[syndromeI][1] = i2;

            // 1 possible bit flip left in the parity part
            for (int ip = 0; ip < 12; ip++) {
                int syndromeIP = syndromeI ^ (1 << (11 - ip));
                Golay_20_8_m_corr[syndromeIP][0] = i1;
                Golay_20_8_m_corr[syndromeIP][1] = i2;
                Golay_20_8_m_corr[syndromeIP][2] = 8 + ip;
            }
        }

        // single bit patterns
        int syndromeI = 0;

        for (int ir = 0; ir < 12; ir++) {
            syndromeI += Golay_20_8_m_H[20 * ir + i1] << (11 - ir);
        }

        Golay_20_8_m_corr[syndromeI][0] = i1;

        for (int ip1 = 0; ip1 < 12; ip1++) // 1 more bit flip in parity
        {
            int syndromeIP1 = syndromeI ^ (1 << (11 - ip1));
            Golay_20_8_m_corr[syndromeIP1][0] = i1;
            Golay_20_8_m_corr[syndromeIP1][1] = 8 + ip1;

            for (int ip2 = ip1 + 1; ip2 < 12; ip2++) // 1 more bit flip in parity
            {
                int syndromeIP2 = syndromeIP1 ^ (1 << (11 - ip2));
                Golay_20_8_m_corr[syndromeIP2][0] = i1;
                Golay_20_8_m_corr[syndromeIP2][1] = 8 + ip1;
                Golay_20_8_m_corr[syndromeIP2][2] = 8 + ip2;
            }
        }
    }

    // no bit patterns (in message) -> all in parity
    for (int ip1 = 0; ip1 < 12; ip1++) // 1 bit flip in parity
    {
        int syndromeIP1 = (1 << (11 - ip1));
        Golay_20_8_m_corr[syndromeIP1][0] = 8 + ip1;

        for (int ip2 = ip1 + 1; ip2 < 12; ip2++) // 1 more bit flip in parity
        {
            int syndromeIP2 = syndromeIP1 ^ (1 << (11 - ip2));
            Golay_20_8_m_corr[syndromeIP2][0] = 8 + ip1;
            Golay_20_8_m_corr[syndromeIP2][1] = 8 + ip2;

            for (int ip3 = ip2 + 1; ip3 < 12; ip3++) // 1 more bit flip in parity
            {
                int syndromeIP3 = syndromeIP2 ^ (1 << (11 - ip3));
                Golay_20_8_m_corr[syndromeIP3][0] = 8 + ip1;
                Golay_20_8_m_corr[syndromeIP3][1] = 8 + ip2;
                Golay_20_8_m_corr[syndromeIP3][2] = 8 + ip3;
            }
        }
    }
}

// Golay (20,8) has Hamming weight 6 and reliably corrects at most two bit errors.
bool
Golay_20_8_decode(unsigned char* rxBits) {
    unsigned int syndromeI = 0; // syndrome index

    for (int is = 0; is < 12; is++) {
        syndromeI += (((rxBits[0] * Golay_20_8_m_H[20 * is + 0]) + (rxBits[1] * Golay_20_8_m_H[20 * is + 1])
                       + (rxBits[2] * Golay_20_8_m_H[20 * is + 2]) + (rxBits[3] * Golay_20_8_m_H[20 * is + 3])
                       + (rxBits[4] * Golay_20_8_m_H[20 * is + 4]) + (rxBits[5] * Golay_20_8_m_H[20 * is + 5])
                       + (rxBits[6] * Golay_20_8_m_H[20 * is + 6]) + (rxBits[7] * Golay_20_8_m_H[20 * is + 7])
                       + (rxBits[8] * Golay_20_8_m_H[20 * is + 8]) + (rxBits[9] * Golay_20_8_m_H[20 * is + 9])
                       + (rxBits[10] * Golay_20_8_m_H[20 * is + 10]) + (rxBits[11] * Golay_20_8_m_H[20 * is + 11])
                       + (rxBits[12] * Golay_20_8_m_H[20 * is + 12]) + (rxBits[13] * Golay_20_8_m_H[20 * is + 13])
                       + (rxBits[14] * Golay_20_8_m_H[20 * is + 14]) + (rxBits[15] * Golay_20_8_m_H[20 * is + 15])
                       + (rxBits[16] * Golay_20_8_m_H[20 * is + 16]) + (rxBits[17] * Golay_20_8_m_H[20 * is + 17])
                       + (rxBits[18] * Golay_20_8_m_H[20 * is + 18]) + (rxBits[19] * Golay_20_8_m_H[20 * is + 19]))
                      % 2)
                     << (11 - is);
    }

    if (syndromeI > 0) {
        int i = 0;
        //tally corrections made, exceeding 2 will return false
        int correction = 0;

        for (; i < 3; i++) {
            if (Golay_20_8_m_corr[syndromeI][i] == 0xFF) {
                break;
            } else {
                rxBits[Golay_20_8_m_corr[syndromeI][i]] ^= 1; // flip bit
                correction++;
            }
        }

        if (i == 0) {
            return false;
        }

        //being stingy and only allowing 1 error, may wreck some good data decodes,
        //decided to play it safe with 2
        if (correction > 2) {
            return false;
        }
    }

    return true;
}

// ========================================================================================

void
Golay_24_12_init(void) {
    DSD_MEMSET(Golay_24_12_m_corr, 0xFF, sizeof(Golay_24_12_m_corr));

    for (int i1 = 0; i1 < 12; i1++) {
        for (int i2 = i1 + 1; i2 < 12; i2++) {
            for (int i3 = i2 + 1; i3 < 12; i3++) {
                // 3 bit patterns
                int syndromeI = 0;

                for (int ir = 0; ir < 12; ir++) {
                    syndromeI +=
                        ((Golay_24_12_m_H[24 * ir + i1] + Golay_24_12_m_H[24 * ir + i2] + Golay_24_12_m_H[24 * ir + i3])
                         % 2)
                        << (11 - ir);
                }

                Golay_24_12_m_corr[syndromeI][0] = i1;
                Golay_24_12_m_corr[syndromeI][1] = i2;
                Golay_24_12_m_corr[syndromeI][2] = i3;
            }

            // 2 bit patterns
            int syndromeI = 0;

            for (int ir = 0; ir < 12; ir++) {
                syndromeI += ((Golay_24_12_m_H[24 * ir + i1] + Golay_24_12_m_H[24 * ir + i2]) % 2) << (11 - ir);
            }

            Golay_24_12_m_corr[syndromeI][0] = i1;
            Golay_24_12_m_corr[syndromeI][1] = i2;

            // 1 possible bit flip left in the parity part
            for (int ip = 0; ip < 12; ip++) {
                int syndromeIP = syndromeI ^ (1 << (11 - ip));
                Golay_24_12_m_corr[syndromeIP][0] = i1;
                Golay_24_12_m_corr[syndromeIP][1] = i2;
                Golay_24_12_m_corr[syndromeIP][2] = 12 + ip;
            }
        }

        // single bit patterns
        int syndromeI = 0;

        for (int ir = 0; ir < 12; ir++) {
            syndromeI += Golay_24_12_m_H[24 * ir + i1] << (11 - ir);
        }

        Golay_24_12_m_corr[syndromeI][0] = i1;

        for (int ip1 = 0; ip1 < 12; ip1++) // 1 more bit flip in parity
        {
            int syndromeIP1 = syndromeI ^ (1 << (11 - ip1));
            Golay_24_12_m_corr[syndromeIP1][0] = i1;
            Golay_24_12_m_corr[syndromeIP1][1] = 12 + ip1;

            for (int ip2 = ip1 + 1; ip2 < 12; ip2++) // 1 more bit flip in parity
            {
                int syndromeIP2 = syndromeIP1 ^ (1 << (11 - ip2));
                Golay_24_12_m_corr[syndromeIP2][0] = i1;
                Golay_24_12_m_corr[syndromeIP2][1] = 12 + ip1;
                Golay_24_12_m_corr[syndromeIP2][2] = 12 + ip2;
            }
        }
    }

    // no bit patterns (in message) -> all in parity
    for (int ip1 = 0; ip1 < 12; ip1++) // 1 bit flip in parity
    {
        int syndromeIP1 = (1 << (11 - ip1));
        Golay_24_12_m_corr[syndromeIP1][0] = 12 + ip1;

        for (int ip2 = ip1 + 1; ip2 < 12; ip2++) // 1 more bit flip in parity
        {
            int syndromeIP2 = syndromeIP1 ^ (1 << (11 - ip2));
            Golay_24_12_m_corr[syndromeIP2][0] = 12 + ip1;
            Golay_24_12_m_corr[syndromeIP2][1] = 12 + ip2;

            for (int ip3 = ip2 + 1; ip3 < 12; ip3++) // 1 more bit flip in parity
            {
                int syndromeIP3 = syndromeIP2 ^ (1 << (11 - ip3));
                Golay_24_12_m_corr[syndromeIP3][0] = 12 + ip1;
                Golay_24_12_m_corr[syndromeIP3][1] = 12 + ip2;
                Golay_24_12_m_corr[syndromeIP3][2] = 12 + ip3;
            }
        }
    }
}

// Encode the extended Golay codeword used by the M17 LICH path.
void
Golay_24_12_encode(const unsigned char* origBits, unsigned char* encodedBits) {
    DSD_MEMSET(encodedBits, 0, 24);

    for (int i = 0; i < 12; i++) {
        for (int j = 0; j < 24; j++) {
            encodedBits[j] += origBits[i] * Golay_24_12_m_G[24 * i + j];
        }
    }
    for (int i = 0; i < 24; i++) {
        encodedBits[i] %= 2;
    }
}

bool
Golay_24_12_decode(unsigned char* rxBits) {
    unsigned int syndromeI = 0; // syndrome index

    for (int is = 0; is < 12; is++) {
        syndromeI += (((rxBits[0] * Golay_24_12_m_H[24 * is + 0]) + (rxBits[1] * Golay_24_12_m_H[24 * is + 1])
                       + (rxBits[2] * Golay_24_12_m_H[24 * is + 2]) + (rxBits[3] * Golay_24_12_m_H[24 * is + 3])
                       + (rxBits[4] * Golay_24_12_m_H[24 * is + 4]) + (rxBits[5] * Golay_24_12_m_H[24 * is + 5])
                       + (rxBits[6] * Golay_24_12_m_H[24 * is + 6]) + (rxBits[7] * Golay_24_12_m_H[24 * is + 7])
                       + (rxBits[8] * Golay_24_12_m_H[24 * is + 8]) + (rxBits[9] * Golay_24_12_m_H[24 * is + 9])
                       + (rxBits[10] * Golay_24_12_m_H[24 * is + 10]) + (rxBits[11] * Golay_24_12_m_H[24 * is + 11])
                       + (rxBits[12] * Golay_24_12_m_H[24 * is + 12]) + (rxBits[13] * Golay_24_12_m_H[24 * is + 13])
                       + (rxBits[14] * Golay_24_12_m_H[24 * is + 14]) + (rxBits[15] * Golay_24_12_m_H[24 * is + 15])
                       + (rxBits[16] * Golay_24_12_m_H[24 * is + 16]) + (rxBits[17] * Golay_24_12_m_H[24 * is + 17])
                       + (rxBits[18] * Golay_24_12_m_H[24 * is + 18]) + (rxBits[19] * Golay_24_12_m_H[24 * is + 19])
                       + (rxBits[20] * Golay_24_12_m_H[24 * is + 20]) + (rxBits[21] * Golay_24_12_m_H[24 * is + 21])
                       + (rxBits[22] * Golay_24_12_m_H[24 * is + 22]) + (rxBits[23] * Golay_24_12_m_H[24 * is + 23]))
                      % 2)
                     << (11 - is);
    }

    if (syndromeI > 0) {
        int i = 0;

        for (; i < 3; i++) {
            if (Golay_24_12_m_corr[syndromeI][i] == 0xFF) {
                break;
            } else {
                rxBits[Golay_24_12_m_corr[syndromeI][i]] ^= 1; // flip bit
            }
        }

        if (i == 0) {
            return false;
        }
    }

    return true;
}

// ========================================================================================

void
QR_16_7_6_init(void) {
    DSD_MEMSET(QR_16_7_6_m_corr, 0xFF, sizeof(QR_16_7_6_m_corr));

    for (int i1 = 0; i1 < 7; i1++) {
        for (int i2 = i1 + 1; i2 < 7; i2++) {
            // 2 bit patterns
            int syndromeI = 0;

            for (int ir = 0; ir < 9; ir++) {
                syndromeI += ((QR_16_7_6_m_H[16 * ir + i1] + QR_16_7_6_m_H[16 * ir + i2]) % 2) << (8 - ir);
            }

            QR_16_7_6_m_corr[syndromeI][0] = i1;
            QR_16_7_6_m_corr[syndromeI][1] = i2;
        }

        // single bit patterns
        int syndromeI = 0;

        for (int ir = 0; ir < 9; ir++) {
            syndromeI += QR_16_7_6_m_H[16 * ir + i1] << (8 - ir);
        }

        QR_16_7_6_m_corr[syndromeI][0] = i1;

        // 1 possible bit flip left in the parity part
        for (int ip = 0; ip < 9; ip++) {
            int syndromeIP = syndromeI ^ (1 << (8 - ip));
            QR_16_7_6_m_corr[syndromeIP][0] = i1;
            QR_16_7_6_m_corr[syndromeIP][1] = 7 + ip;
        }
    }

    // no bit patterns (in message) -> all in parity
    for (int ip1 = 0; ip1 < 9; ip1++) // 1 bit flip in parity
    {
        int syndromeIP1 = (1 << (8 - ip1));
        QR_16_7_6_m_corr[syndromeIP1][0] = 7 + ip1;

        for (int ip2 = ip1 + 1; ip2 < 9; ip2++) // 1 more bit flip in parity
        {
            int syndromeIP2 = syndromeIP1 ^ (1 << (8 - ip2));
            QR_16_7_6_m_corr[syndromeIP2][0] = 7 + ip1;
            QR_16_7_6_m_corr[syndromeIP2][1] = 7 + ip2;
        }
    }
}

bool
QR_16_7_6_decode(unsigned char* rxBits) {
    //2 bit errors or less
    unsigned int syndromeI = 0; // syndrome index
    int corrections = 0;
    for (int is = 0; is < 9; is++) {
        syndromeI += (((rxBits[0] * QR_16_7_6_m_H[16 * is + 0]) + (rxBits[1] * QR_16_7_6_m_H[16 * is + 1])
                       + (rxBits[2] * QR_16_7_6_m_H[16 * is + 2]) + (rxBits[3] * QR_16_7_6_m_H[16 * is + 3])
                       + (rxBits[4] * QR_16_7_6_m_H[16 * is + 4]) + (rxBits[5] * QR_16_7_6_m_H[16 * is + 5])
                       + (rxBits[6] * QR_16_7_6_m_H[16 * is + 6]) + (rxBits[7] * QR_16_7_6_m_H[16 * is + 7])
                       + (rxBits[8] * QR_16_7_6_m_H[16 * is + 8]) + (rxBits[9] * QR_16_7_6_m_H[16 * is + 9])
                       + (rxBits[10] * QR_16_7_6_m_H[16 * is + 10]) + (rxBits[11] * QR_16_7_6_m_H[16 * is + 11])
                       + (rxBits[12] * QR_16_7_6_m_H[16 * is + 12]) + (rxBits[13] * QR_16_7_6_m_H[16 * is + 13])
                       + (rxBits[14] * QR_16_7_6_m_H[16 * is + 14]) + (rxBits[15] * QR_16_7_6_m_H[16 * is + 15]))
                      % 2)
                     << (8 - is);
    }

    if (syndromeI > 0) {
        int i = 0;

        for (; i < 2; i++) {
            if (QR_16_7_6_m_corr[syndromeI][i] == 0xFF) {
                break;
            } else {
                rxBits[QR_16_7_6_m_corr[syndromeI][i]] ^= 1; // flip bit
                corrections++;
            }
        }

        if (i == 0) {
            return false;
        }
    }

    (void)corrections;

    return true;
}

void
InitAllFecFunction(void) {
    Hamming_7_4_init();
    Hamming_12_8_init();
    Hamming_13_9_init();
    Hamming_15_11_init();
    Hamming_16_11_4_init();
    Golay_20_8_init();
    Golay_24_12_init();
    QR_16_7_6_init();
} /* End InitAllFEC() */

/* End of file */
