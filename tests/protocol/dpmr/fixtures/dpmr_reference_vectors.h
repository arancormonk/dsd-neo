// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * dPMR CCH reference vectors -- generated, do not edit by hand.
 *
 * Produced by tests/protocol/dpmr/fixtures/generate_dpmr_reference_vectors.py from an
 * independent model of the ETSI TS 102 658 CCH encode direction, cross-checked against
 * DSDcc. See the README in this directory for provenance and regeneration.
 */

#ifndef DSD_NEO_TESTS_PROTOCOL_DPMR_FIXTURES_DPMR_REFERENCE_VECTORS_H_
#define DSD_NEO_TESTS_PROTOCOL_DPMR_FIXTURES_DPMR_REFERENCE_VECTORS_H_

#include <stdint.h>

static const char DPMR_REF_DSDCC_COMMIT[] = "f27b32d2df131ae3a376fe72d3fb880ae1f9ede1";

/* Scrambler: X^9 + X^5 + 1, all-ones seed, the 72 bits one CCH half is masked with. */
static const uint32_t DPMR_REF_SCRAMBLER_SEED = 0x1FFU;
static const uint32_t DPMR_REF_SCRAMBLER_FINAL_STATE = 0x1B3U;
static const uint8_t DPMR_REF_SCRAMBLER_KEYSTREAM[72] = {
    1U, 1U, 1U, 1U, 1U, 1U, 1U, 1U, 1U, 0U, 0U, 0U, 0U, 0U, 1U, 1U, 1U, 1U, 0U, 1U, 1U, 1U, 1U, 1U,
    0U, 0U, 0U, 1U, 0U, 1U, 1U, 1U, 0U, 0U, 1U, 1U, 0U, 0U, 1U, 0U, 0U, 0U, 0U, 0U, 1U, 0U, 0U, 1U,
    0U, 1U, 0U, 0U, 1U, 1U, 1U, 0U, 1U, 1U, 0U, 1U, 0U, 0U, 0U, 1U, 1U, 1U, 1U, 0U, 0U, 1U, 1U, 1U,
};

/* Block interleave: the position each of the 72 coded bits is transmitted in. */
static const uint8_t DPMR_REF_INTERLEAVE_INDEX[72] = {
    0U,  6U,  12U, 18U, 24U, 30U, 36U, 42U, 48U, 54U, 60U, 66U, 1U,  7U,  13U, 19U, 25U, 31U,
    37U, 43U, 49U, 55U, 61U, 67U, 2U,  8U,  14U, 20U, 26U, 32U, 38U, 44U, 50U, 56U, 62U, 68U,
    3U,  9U,  15U, 21U, 27U, 33U, 39U, 45U, 51U, 57U, 63U, 69U, 4U,  10U, 16U, 22U, 28U, 34U,
    40U, 46U, 52U, 58U, 64U, 70U, 5U,  11U, 17U, 23U, 29U, 35U, 41U, 47U, 53U, 59U, 65U, 71U,
};

/* Shortened Hamming(12,8): 8 data bits, then 4 parity bits. */
typedef struct {
    uint8_t data[8];
    uint8_t codeword[12];
} dpmr_ref_hamming_vector;

#define DPMR_REF_HAMMING_VECTOR_COUNT 10
static const dpmr_ref_hamming_vector DPMR_REF_HAMMING_VECTORS[10] = {
    {{0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U}, {0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U}},
    {{1U, 1U, 1U, 1U, 1U, 1U, 1U, 1U}, {1U, 1U, 1U, 1U, 1U, 1U, 1U, 1U, 0U, 1U, 0U, 0U}},
    {{0U, 0U, 0U, 0U, 0U, 0U, 0U, 1U}, {0U, 0U, 0U, 0U, 0U, 0U, 0U, 1U, 0U, 0U, 1U, 1U}},
    {{1U, 0U, 0U, 0U, 0U, 0U, 0U, 0U}, {1U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 1U, 1U, 1U, 0U}},
    {{0U, 1U, 0U, 1U, 1U, 0U, 1U, 0U}, {0U, 1U, 0U, 1U, 1U, 0U, 1U, 0U, 1U, 1U, 1U, 1U}},
    {{1U, 0U, 1U, 0U, 0U, 1U, 0U, 1U}, {1U, 0U, 1U, 0U, 0U, 1U, 0U, 1U, 1U, 0U, 1U, 1U}},
    {{0U, 0U, 0U, 0U, 1U, 1U, 1U, 1U}, {0U, 0U, 0U, 0U, 1U, 1U, 1U, 1U, 0U, 0U, 1U, 0U}},
    {{1U, 1U, 1U, 1U, 0U, 0U, 0U, 0U}, {1U, 1U, 1U, 1U, 0U, 0U, 0U, 0U, 0U, 1U, 1U, 0U}},
    {{0U, 0U, 1U, 1U, 0U, 1U, 1U, 1U}, {0U, 0U, 1U, 1U, 0U, 1U, 1U, 1U, 0U, 1U, 1U, 0U}},
    {{1U, 1U, 0U, 0U, 1U, 0U, 0U, 0U}, {1U, 1U, 0U, 0U, 1U, 0U, 0U, 0U, 0U, 0U, 1U, 0U}},
};

/* The syndrome a single-bit error in each position produces, most significant bit first. */
static const uint8_t DPMR_REF_HAMMING_SYNDROME_BY_POSITION[12] = {
    14U, 7U, 10U, 5U, 11U, 12U, 6U, 3U, 8U, 4U, 2U, 1U,
};
/* The syndromes the code cannot place, and so must report uncorrectable. */
static const uint8_t DPMR_REF_HAMMING_UNCORRECTABLE_SYNDROMES[3] = {9U, 13U, 15U};

/* CRC-7: X^7 + X^3 + 1 over the 41 payload bits, zero seed, no final inversion. */
typedef struct {
    uint8_t payload[41];
    uint8_t crc;
} dpmr_ref_crc_vector;

#define DPMR_REF_CRC_VECTOR_COUNT 6
static const dpmr_ref_crc_vector DPMR_REF_CRC_VECTORS[6] = {
    {{0U, 0U, 0U, 0U, 0U, 1U, 1U, 0U, 1U, 1U, 1U, 0U, 0U, 1U, 0U, 0U, 1U, 0U, 0U, 0U, 1U,
      0U, 0U, 0U, 1U, 1U, 0U, 1U, 0U, 0U, 0U, 1U, 0U, 1U, 0U, 1U, 1U, 0U, 0U, 1U, 1U},
     0x31U},
    {{0U, 1U, 0U, 0U, 0U, 1U, 1U, 1U, 1U, 1U, 1U, 1U, 0U, 1U, 0U, 0U, 1U, 0U, 0U, 0U, 1U,
      0U, 0U, 0U, 0U, 1U, 1U, 0U, 0U, 0U, 1U, 0U, 0U, 1U, 1U, 0U, 1U, 0U, 1U, 0U, 1U},
     0x4AU},
    {{1U, 0U, 1U, 0U, 1U, 0U, 1U, 0U, 1U, 0U, 1U, 1U, 1U, 1U, 0U, 0U, 1U, 0U, 0U, 0U, 1U,
      0U, 0U, 1U, 1U, 1U, 1U, 1U, 0U, 0U, 1U, 1U, 0U, 1U, 1U, 1U, 1U, 0U, 1U, 1U, 1U},
     0x28U},
    {{1U, 1U, 1U, 1U, 0U, 1U, 1U, 0U, 1U, 1U, 1U, 0U, 1U, 1U, 0U, 0U, 1U, 0U, 0U, 0U, 1U,
      0U, 0U, 1U, 0U, 1U, 0U, 0U, 0U, 1U, 0U, 0U, 1U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 1U},
     0x77U},
    {{0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U,
      0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U},
     0x00U},
    {{1U, 1U, 1U, 1U, 1U, 1U, 1U, 1U, 1U, 1U, 1U, 1U, 1U, 1U, 1U, 1U, 1U, 1U, 1U, 1U, 1U,
      1U, 1U, 1U, 1U, 1U, 1U, 1U, 1U, 1U, 1U, 1U, 1U, 1U, 1U, 1U, 1U, 1U, 1U, 1U, 1U},
     0x2BU},
};

/* dPMR frame sync 2, as dibits: the 12 symbols that precede each frame below. */
static const uint8_t DPMR_REF_FS2_DIBITS[12] = {
    1U, 1U, 3U, 3U, 3U, 3U, 1U, 3U, 1U, 3U, 3U, 1U,
};

/* The channel code carried between the two TCH groups. */
static const int32_t DPMR_REF_COLOUR_CODE = 12;

/* A complete FS2 superframe part: 372 body dibits, and what decoding them must yield. */
typedef struct {
    const char* name;
    uint8_t body_dibits[372];
    uint8_t cch_decoded[2][48];
    uint8_t crc[2];
    uint32_t frame_number[2];
    uint32_t communication_mode[2];
    uint32_t version[2];
    uint32_t comms_format[2];
    uint32_t emergency[2];
    uint32_t id_value;
    const char* id_string;
} dpmr_ref_frame;

#define DPMR_REF_FRAME_COUNT 2
static const dpmr_ref_frame DPMR_REF_FRAMES[2] = {
    {
        .name = "called-id",
        .body_dibits =
            {
                2U, 0U, 2U, 2U, 3U, 2U, 1U, 3U, 2U, 1U, 2U, 0U, 0U, 3U, 3U, 0U, 0U, 3U, 2U, 2U, 0U, 0U, 3U, 2U, 2U,
                1U, 1U, 3U, 0U, 1U, 3U, 0U, 0U, 3U, 2U, 3U, 3U, 2U, 3U, 2U, 1U, 0U, 0U, 3U, 0U, 3U, 2U, 2U, 2U, 1U,
                1U, 2U, 0U, 0U, 3U, 0U, 3U, 0U, 2U, 1U, 0U, 1U, 3U, 3U, 3U, 3U, 3U, 0U, 0U, 1U, 0U, 0U, 0U, 0U, 3U,
                0U, 2U, 0U, 1U, 0U, 2U, 0U, 3U, 1U, 3U, 1U, 3U, 3U, 0U, 0U, 2U, 0U, 3U, 0U, 0U, 0U, 3U, 2U, 2U, 1U,
                3U, 3U, 1U, 2U, 0U, 3U, 3U, 3U, 0U, 3U, 2U, 3U, 1U, 0U, 1U, 1U, 0U, 2U, 3U, 1U, 1U, 3U, 1U, 1U, 2U,
                0U, 2U, 2U, 3U, 1U, 1U, 1U, 0U, 0U, 2U, 2U, 1U, 0U, 3U, 0U, 2U, 2U, 0U, 2U, 3U, 0U, 0U, 0U, 3U, 2U,
                2U, 1U, 0U, 0U, 3U, 3U, 1U, 1U, 2U, 1U, 1U, 0U, 1U, 0U, 3U, 1U, 2U, 1U, 3U, 3U, 0U, 2U, 0U, 3U, 0U,
                3U, 0U, 0U, 3U, 2U, 1U, 1U, 3U, 1U, 1U, 1U, 3U, 1U, 1U, 1U, 1U, 1U, 2U, 1U, 2U, 0U, 3U, 3U, 1U, 2U,
                1U, 0U, 3U, 3U, 0U, 3U, 2U, 0U, 0U, 3U, 2U, 3U, 3U, 2U, 2U, 1U, 2U, 1U, 3U, 0U, 1U, 0U, 3U, 0U, 3U,
                2U, 2U, 3U, 2U, 1U, 0U, 0U, 2U, 0U, 1U, 1U, 2U, 3U, 2U, 1U, 3U, 3U, 3U, 2U, 1U, 2U, 3U, 1U, 1U, 3U,
                0U, 1U, 2U, 2U, 2U, 3U, 0U, 2U, 0U, 2U, 0U, 3U, 0U, 2U, 0U, 2U, 0U, 2U, 3U, 0U, 1U, 1U, 3U, 2U, 3U,
                2U, 2U, 2U, 3U, 3U, 2U, 2U, 3U, 3U, 3U, 0U, 3U, 2U, 3U, 1U, 0U, 1U, 0U, 3U, 1U, 2U, 3U, 0U, 2U, 2U,
                3U, 1U, 3U, 2U, 2U, 3U, 1U, 2U, 3U, 0U, 3U, 0U, 0U, 1U, 2U, 2U, 0U, 2U, 3U, 3U, 0U, 2U, 1U, 0U, 1U,
                0U, 3U, 2U, 1U, 3U, 3U, 3U, 2U, 1U, 3U, 0U, 3U, 2U, 0U, 1U, 2U, 2U, 0U, 0U, 1U, 1U, 1U, 3U, 0U, 3U,
                2U, 0U, 3U, 2U, 1U, 2U, 3U, 2U, 1U, 0U, 3U, 2U, 1U, 2U, 0U, 1U, 2U, 3U, 3U, 0U, 1U, 1U,
            },
        .cch_decoded =
            {
                {
                    0U, 0U, 0U, 0U, 0U, 1U, 1U, 0U, 1U, 1U, 1U, 0U, 0U, 1U, 0U, 0U, 1U, 0U, 0U, 0U, 1U, 0U, 0U, 0U,
                    1U, 1U, 0U, 1U, 0U, 0U, 0U, 1U, 0U, 1U, 0U, 1U, 1U, 0U, 0U, 1U, 1U, 0U, 1U, 1U, 0U, 0U, 0U, 1U,
                },
                {
                    0U, 1U, 0U, 0U, 0U, 1U, 1U, 1U, 1U, 1U, 1U, 1U, 0U, 1U, 0U, 0U, 1U, 0U, 0U, 0U, 1U, 0U, 0U, 0U,
                    0U, 1U, 1U, 0U, 0U, 0U, 1U, 0U, 0U, 1U, 1U, 0U, 1U, 0U, 1U, 0U, 1U, 1U, 0U, 0U, 1U, 0U, 1U, 0U,
                },
            },
        .crc = {0x31U, 0x4AU},
        .frame_number = {0U, 1U},
        .communication_mode = {1U, 1U},
        .version = {0U, 0U},
        .comms_format = {1U, 1U},
        .emergency = {0U, 0U},
        .id_value = 1806845U,
        .id_string = "1234567",
    },
    {
        .name = "calling-id",
        .body_dibits =
            {
                0U, 0U, 2U, 3U, 3U, 2U, 3U, 2U, 0U, 0U, 2U, 1U, 3U, 3U, 2U, 2U, 0U, 3U, 2U, 3U, 2U, 0U, 1U, 3U, 3U,
                0U, 2U, 3U, 1U, 2U, 1U, 2U, 0U, 0U, 0U, 2U, 1U, 3U, 1U, 0U, 0U, 1U, 3U, 3U, 0U, 0U, 0U, 3U, 3U, 0U,
                1U, 2U, 0U, 1U, 0U, 2U, 2U, 0U, 2U, 3U, 1U, 1U, 1U, 1U, 1U, 2U, 2U, 0U, 2U, 0U, 2U, 3U, 2U, 2U, 1U,
                0U, 1U, 2U, 0U, 2U, 1U, 2U, 3U, 1U, 1U, 0U, 2U, 2U, 3U, 1U, 0U, 2U, 1U, 1U, 1U, 0U, 2U, 0U, 0U, 1U,
                1U, 1U, 0U, 2U, 1U, 1U, 2U, 0U, 2U, 1U, 3U, 3U, 2U, 1U, 3U, 1U, 1U, 0U, 1U, 2U, 2U, 1U, 2U, 0U, 3U,
                2U, 0U, 0U, 2U, 2U, 3U, 2U, 3U, 2U, 2U, 1U, 2U, 0U, 3U, 2U, 1U, 3U, 0U, 2U, 0U, 1U, 0U, 2U, 0U, 0U,
                0U, 2U, 3U, 0U, 3U, 1U, 1U, 2U, 1U, 2U, 2U, 2U, 0U, 0U, 1U, 3U, 0U, 0U, 1U, 2U, 3U, 3U, 1U, 3U, 3U,
                2U, 1U, 2U, 3U, 0U, 1U, 1U, 3U, 1U, 1U, 1U, 3U, 1U, 1U, 1U, 1U, 1U, 0U, 1U, 0U, 0U, 3U, 1U, 3U, 3U,
                2U, 3U, 3U, 2U, 1U, 3U, 1U, 0U, 1U, 2U, 2U, 2U, 1U, 0U, 0U, 0U, 3U, 1U, 0U, 1U, 1U, 2U, 0U, 2U, 0U,
                2U, 0U, 2U, 3U, 2U, 3U, 2U, 2U, 2U, 1U, 1U, 2U, 1U, 0U, 3U, 0U, 2U, 3U, 2U, 1U, 3U, 1U, 3U, 0U, 3U,
                0U, 3U, 3U, 3U, 0U, 1U, 2U, 1U, 3U, 2U, 2U, 2U, 2U, 1U, 2U, 0U, 2U, 2U, 1U, 2U, 0U, 3U, 2U, 3U, 0U,
                2U, 0U, 1U, 2U, 2U, 2U, 2U, 1U, 1U, 2U, 1U, 1U, 2U, 1U, 2U, 2U, 1U, 2U, 0U, 0U, 3U, 3U, 3U, 1U, 2U,
                1U, 0U, 0U, 2U, 3U, 3U, 3U, 3U, 0U, 2U, 1U, 1U, 1U, 3U, 3U, 1U, 1U, 0U, 1U, 1U, 3U, 3U, 0U, 1U, 1U,
                1U, 3U, 1U, 2U, 3U, 3U, 1U, 1U, 2U, 3U, 3U, 0U, 3U, 1U, 0U, 3U, 1U, 2U, 0U, 1U, 2U, 1U, 1U, 0U, 0U,
                1U, 1U, 0U, 0U, 0U, 2U, 1U, 0U, 3U, 3U, 2U, 1U, 3U, 3U, 1U, 1U, 2U, 2U, 0U, 2U, 1U, 3U,
            },
        .cch_decoded =
            {
                {
                    1U, 0U, 1U, 0U, 1U, 0U, 1U, 0U, 1U, 0U, 1U, 1U, 1U, 1U, 0U, 0U, 1U, 0U, 0U, 0U, 1U, 0U, 0U, 1U,
                    1U, 1U, 1U, 1U, 0U, 0U, 1U, 1U, 0U, 1U, 1U, 1U, 1U, 0U, 1U, 1U, 1U, 0U, 1U, 0U, 1U, 0U, 0U, 0U,
                },
                {
                    1U, 1U, 1U, 1U, 0U, 1U, 1U, 0U, 1U, 1U, 1U, 0U, 1U, 1U, 0U, 0U, 1U, 0U, 0U, 0U, 1U, 0U, 0U, 1U,
                    0U, 1U, 0U, 0U, 0U, 1U, 0U, 0U, 1U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 1U, 1U, 1U, 1U, 0U, 1U, 1U, 1U,
                },
            },
        .crc = {0x28U, 0x77U},
        .frame_number = {2U, 3U},
        .communication_mode = {1U, 1U},
        .version = {0U, 0U},
        .comms_format = {1U, 1U},
        .emergency = {0U, 0U},
        .id_value = 11206075U,
        .id_string = "7654321",
    },
};

#endif /* DSD_NEO_TESTS_PROTOCOL_DPMR_FIXTURES_DPMR_REFERENCE_VECTORS_H_ */
