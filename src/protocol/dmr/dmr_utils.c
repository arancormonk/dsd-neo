// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

//DMR CRC/Utility Functions
//Original File - dmr_sync.c
//ConvertBitIntoBytes, ComputeCrcCCITT, ComputeCrc5Bit, ComputeAndCorrectFullLinkControlCrc, CRC32, CRC9
//Original Source - https://github.com/LouisErigHerve/dsd

//Additional Functions
//Hamming17123, crc7, crc8, crc8ok functions
//Original Souce - https://github.com/boatbod/op25

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/fec/rs_12_9.h>
#include <dsd-neo/protocol/dmr/dmr.h>
#include <dsd-neo/protocol/dmr/dmr_utils_api.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

typedef struct {
    uint8_t syndrome;
    uint8_t bit_index;
} dmr_hamming17123_correction_t;

static const dmr_hamming17123_correction_t kHamming17123Corrections[] = {
    {0x01U, 12U}, {0x02U, 13U}, {0x04U, 14U}, {0x08U, 15U}, {0x10U, 16U}, {0x1BU, 0U},
    {0x1FU, 1U},  {0x17U, 2U},  {0x07U, 3U},  {0x0EU, 4U},  {0x1CU, 5U},  {0x11U, 6U},
    {0x0BU, 7U},  {0x16U, 8U},  {0x05U, 9U},  {0x0AU, 10U}, {0x14U, 11U},
};

static bool
dmr_hamming17123_apply_correction(uint8_t* d, uint8_t syndrome) {
    for (uint32_t i = 0; i < (sizeof(kHamming17123Corrections) / sizeof(kHamming17123Corrections[0])); i++) {
        if (kHamming17123Corrections[i].syndrome == syndrome) {
            uint8_t bit_index = kHamming17123Corrections[i].bit_index;
            d[bit_index] = !d[bit_index];
            return true;
        }
    }
    return false;
}

static uint8_t
dmr_debug_payload_nibble(const int payload[144], size_t no_cach_nibble_index) {
    const size_t dibit_pair_index = 6U + no_cach_nibble_index;
    const size_t payload_index = dibit_pair_index * 2U;
    const uint8_t hi = (uint8_t)(payload[payload_index] & 0x03);
    const uint8_t lo = (uint8_t)(payload[payload_index + 1U] & 0x03);
    return (uint8_t)((hi << 2U) | lo);
}

size_t
dmr_debug_format_burst_payload(char* out, size_t out_size, const int payload[144], uint8_t slot_index,
                               uint8_t burst_type) {
    if (out == NULL || out_size == 0U || payload == NULL || slot_index > 1U) {
        return 0U;
    }

    int n = DSD_SNPRINTF(out, out_size, "Debug Demod +Sync slot=%u type=0x%02X: ", (unsigned int)slot_index + 1U,
                         (unsigned int)burst_type);
    if (n < 0) {
        out[0] = '\0';
        return 0U;
    }

    size_t pos = (size_t)n;
    if (pos >= out_size) {
        out[out_size - 1U] = '\0';
        return pos;
    }

    for (size_t byte_index = 0; byte_index < 33U; byte_index++) {
        const uint8_t hi = dmr_debug_payload_nibble(payload, byte_index * 2U);
        const uint8_t lo = dmr_debug_payload_nibble(payload, (byte_index * 2U) + 1U);
        const uint8_t byte = (uint8_t)((hi << 4U) | lo);
        n = DSD_SNPRINTF(out + pos, out_size - pos, "[%02X]", (unsigned int)byte);
        if (n < 0) {
            out[pos] = '\0';
            return pos;
        }
        pos += (size_t)n;
        if (pos >= out_size) {
            out[out_size - 1U] = '\0';
            return pos;
        }
    }

    return pos;
}

size_t
dmr_debug_format_burst(char* out, size_t out_size, const dsd_state* state, uint8_t slot_index, uint8_t burst_type) {
    if (state == NULL) {
        return 0U;
    }
    return dmr_debug_format_burst_payload(out, out_size, state->dmr_stereo_payload, slot_index, burst_type);
}

void
dmr_debug_dump_burst(const dsd_opts* opts, const dsd_state* state, uint8_t slot_index, uint8_t burst_type) {
    if (opts == NULL || state == NULL || opts->dmr_debug_burst == 0 || slot_index > 1U) {
        return;
    }

    char line[192];
    if (dmr_debug_format_burst(line, sizeof(line), state, slot_index, burst_type) == 0U) {
        return;
    }
    DSD_FPRINTF(stderr, "%s\n", line);
}

uint16_t
ComputeCrcCCITT16d(const uint8_t* buf, uint32_t len) {
    uint32_t i;
    uint16_t CRC = 0x0000; /* Initialization value = 0x0000 */
    /* Polynomial x^16 + x^12 + x^5 + 1
   * Normal     = 0x1021
   * Reciprocal = 0x0811
   * Reversed   = 0x8408
   * Reversed reciprocal = 0x8810 */
    uint16_t Polynome = 0x1021;
    for (i = 0; i < len; i++) {
        if (((CRC >> 15) & 1) ^ (buf[i] & 1)) {
            CRC = (CRC << 1) ^ Polynome;
        } else {
            CRC <<= 1;
        }
    }

    /* Invert the CRC */
    CRC ^= 0xFFFF;

    /* Return the CRC */
    return CRC;
} /* End ComputeCrcCCITTd() */

// A Hamming (17,12,3) Check for completed SLC message
bool
Hamming17123(uint8_t* d) {

    // Calculate the checksum this column should have
    bool c0 = d[0] ^ d[1] ^ d[2] ^ d[3] ^ d[6] ^ d[7] ^ d[9];
    bool c1 = d[0] ^ d[1] ^ d[2] ^ d[3] ^ d[4] ^ d[7] ^ d[8] ^ d[10];
    bool c2 = d[1] ^ d[2] ^ d[3] ^ d[4] ^ d[5] ^ d[8] ^ d[9] ^ d[11];
    bool c3 = d[0] ^ d[1] ^ d[4] ^ d[5] ^ d[7] ^ d[10];
    bool c4 = d[0] ^ d[1] ^ d[2] ^ d[5] ^ d[6] ^ d[8] ^ d[11];

    // Compare these with the actual bits
    unsigned char n = 0x00U;
    n |= (c0 != d[12]) ? 0x01U : 0x00U;
    n |= (c1 != d[13]) ? 0x02U : 0x00U;
    n |= (c2 != d[14]) ? 0x04U : 0x00U;
    n |= (c3 != d[15]) ? 0x08U : 0x00U;
    n |= (c4 != d[16]) ? 0x10U : 0x00U;

    if (n == 0x00U) {
        return true;
    }
    return dmr_hamming17123_apply_correction(d, n);
}

uint8_t
crc8(uint8_t bits[], unsigned int len) {
    uint8_t crc = 0;
    unsigned int K = 8;
    const uint8_t poly[9] = {1, 0, 0, 0, 0, 0, 1, 1, 1}; // crc8 poly
    uint8_t buf[256];
    if (len + K > sizeof(buf)) {
        return 0;
    }
    DSD_MEMSET(buf, 0, sizeof(buf));
    for (unsigned int i = 0; i < len; i++) {
        buf[i] = bits[i];
    }
    for (unsigned int i = 0; i < len; i++) {
        if (buf[i]) {
            for (unsigned int j = 0; j < K + 1; j++) {
                buf[i + j] ^= poly[j];
            }
        }
    }
    for (unsigned int i = 0; i < K; i++) {
        crc = (crc << 1) + buf[len + i];
    }
    return crc;
}

bool
crc8_ok(uint8_t bits[], unsigned int len) {
    uint16_t crc = 0;
    for (unsigned int i = 0; i < 8; i++) {
        crc = (crc << 1) + bits[len + i];
    }
    return (crc == crc8(bits, len));
}

uint8_t
crc7(uint8_t bits[], unsigned int len) {
    uint8_t crc = 0;
    unsigned int K = 7;
    // CRC7 polynomial: x7 + x5 + x2 + x + 1   check poly below for correct (dmr rc crc7)
    const uint8_t poly[8] = {1, 0, 1, 0, 0, 1, 1, 1}; // crc7 poly
    uint8_t buf[256];
    if (len + K > sizeof(buf)) {
        return 0;
    }
    DSD_MEMSET(buf, 0, sizeof(buf));
    for (unsigned int i = 0; i < len; i++) {
        buf[i] = bits[i];
    }
    for (unsigned int i = 0; i < len; i++) {
        if (buf[i]) {
            for (unsigned int j = 0; j < K + 1; j++) {
                buf[i + j] ^= poly[j];
            }
        }
    }
    for (unsigned int i = 0; i < K; i++) {
        crc = (crc << 1) + buf[len + i];
    }
    return crc;
}

/*
 * @brief : This function compute the CRC-CCITT of the DMR data
 *          by using the polynomial x^16 + x^12 + x^5 + 1
 *
 * @param Input : A buffer pointer of the DMR data (80 bytes)
 *
 * @return The 16 bit CRC
 */

uint16_t
ComputeCrcCCITT(const uint8_t* DMRData) {
    uint32_t i;
    uint16_t CRC = 0x0000; /* Initialization value = 0x0000 */
    /* Polynomial x^16 + x^12 + x^5 + 1
   * Normal     = 0x1021
   * Reciprocal = 0x0811
   * Reversed   = 0x8408
   * Reversed reciprocal = 0x8810 */
    uint16_t Polynome = 0x1021;
    for (i = 0; i < 80; i++) {
        if (((CRC >> 15) & 1) ^ (DMRData[i] & 1)) {
            CRC = (CRC << 1) ^ Polynome;
        } else {
            CRC <<= 1;
        }
    }

    /* Invert the CRC */
    CRC ^= 0xFFFF;

    /* Return the CRC */
    return CRC;
} /* End ComputeCrcCCITT() */

/*
 * @brief : This function compute the CRC-24 bit of the full
 *          link control by using the Reed-Solomon(12,9) FEC
 *
 * @param FullLinkControlDataBytes : A buffer pointer of the DMR data bytes (12 bytes)
 *
 * @param CRCComputed : A 32 bit pointer where the computed CRC 24-bit will be stored
 *
 * @param CRCMask : The 24 bit CRC mask to apply
 *
 * @return 0 = CRC error
 *         1 = CRC is correct
 */

uint32_t
ComputeAndCorrectFullLinkControlCrc(uint8_t* FullLinkControlDataBytes, uint32_t* CRCComputed, uint32_t CRCMask) {
    uint32_t i;
    rs_12_9_codeword_t VoiceLCHeaderStr;
    rs_12_9_poly_t syndrome;
    uint8_t errors_found = 0;
    rs_12_9_correct_errors_result_t result = RS_12_9_CORRECT_ERRORS_RESULT_NO_ERRORS_FOUND;
    uint32_t CrcIsCorrect = 0;

    for (i = 0; i < 12; i++) {
        VoiceLCHeaderStr.data[i] = FullLinkControlDataBytes[i];

        /* Apply CRC mask on each 3 last bytes
     * of the full link control */
        if (i == 9) {
            VoiceLCHeaderStr.data[i] ^= (uint8_t)(CRCMask >> 16);
        } else if (i == 10) {
            VoiceLCHeaderStr.data[i] ^= (uint8_t)(CRCMask >> 8);
        } else if (i == 11) {
            VoiceLCHeaderStr.data[i] ^= (uint8_t)(CRCMask);
        } else {
            /* Nothing to do */
        }
    }

    /* Check and correct the full link LC control with Reed Solomon (12,9) FEC */
    rs_12_9_calc_syndrome(&VoiceLCHeaderStr, &syndrome);
    if (rs_12_9_check_syndrome(&syndrome) != 0) {
        result = rs_12_9_correct_errors(&VoiceLCHeaderStr, &syndrome, &errors_found);
    }

    /* Reconstitue the CRC */
    *CRCComputed = (uint32_t)((VoiceLCHeaderStr.data[9] << 16) & 0xFF0000);
    *CRCComputed |= (uint32_t)((VoiceLCHeaderStr.data[10] << 8) & 0x00FF00);
    *CRCComputed |= (uint32_t)((VoiceLCHeaderStr.data[11] << 0) & 0x0000FF);

    if ((result == RS_12_9_CORRECT_ERRORS_RESULT_NO_ERRORS_FOUND)
        || (result == RS_12_9_CORRECT_ERRORS_RESULT_ERRORS_CORRECTED)) {
        CrcIsCorrect = 1;

        /* Reconstitue full link control data after FEC correction */
        for (i = 0; i < 12; i++) {
            FullLinkControlDataBytes[i] = VoiceLCHeaderStr.data[i];

            /* Apply CRC mask on each 3 last bytes
       * of the full link control */
            if (i == 9) {
                FullLinkControlDataBytes[i] ^= (uint8_t)(CRCMask >> 16);
            } else if (i == 10) {
                FullLinkControlDataBytes[i] ^= (uint8_t)(CRCMask >> 8);
            } else if (i == 11) {
                FullLinkControlDataBytes[i] ^= (uint8_t)(CRCMask);
            } else {
                /* Nothing to do */
            }
        }
    } else {
        CrcIsCorrect = 0;
    }

    /* Return the CRC status */
    return CrcIsCorrect;
} /* End ComputeAndCorrectFullLinkControlCrc() */

/*
 * @brief : This function compute the 5 bit CRC of the DMR voice burst data
 *          See ETSI TS 102 361-1 chapter B.3.11
 *
 * @param Input : A buffer pointer of the DMR data (72 bytes)
 *
 * @return The 5 bit CRC
 */

uint8_t
ComputeCrc5Bit(const uint8_t* DMRData) {
    uint32_t i, j, k;
    uint8_t Buffer[9];
    uint32_t Sum;
    uint8_t CRC = 0;

    /* Convert the 72 bit into 9 bytes */
    k = 0;
    for (i = 0; i < 9; i++) {
        Buffer[i] = 0;
        for (j = 0; j < 8; j++) {
            Buffer[i] = Buffer[i] << 1;
            Buffer[i] = Buffer[i] | DMRData[k++];
        }
    }

    /* Add all 9 bytes */
    Sum = 0;
    for (i = 0; i < 9; i++) {
        Sum += (uint32_t)Buffer[i];
    }

    /* Sum MOD 31 = CRC */
    CRC = (uint8_t)(Sum % 31);

    /* Return the CRC */
    return CRC;
} /* End ComputeCrc5Bit() */

/* Pack 8 single-bit elements (MSB first) into a byte value */
static inline uint64_t
dsd_pack8_bits_msb(const uint8_t* b) {
    return ((uint64_t)(b[0] & 1) << 7) | ((uint64_t)(b[1] & 1) << 6) | ((uint64_t)(b[2] & 1) << 5)
           | ((uint64_t)(b[3] & 1) << 4) | ((uint64_t)(b[4] & 1) << 3) | ((uint64_t)(b[5] & 1) << 2)
           | ((uint64_t)(b[6] & 1) << 1) | ((uint64_t)(b[7] & 1) << 0);
}

uint64_t
ConvertBitIntoBytes(const uint8_t* BufferIn, uint32_t BitLength) {
    uint64_t out = 0;
    const uint8_t* p = BufferIn;
    uint32_t n = BitLength;

    /* Fast path: process full bytes (8 bits) at a time */
    while (n >= 8) {
        out = (out << 8) | dsd_pack8_bits_msb(p);
        p += 8;
        n -= 8;
    }
    /* Remainder bits */
    while (n--) {
        out = (out << 1) | (uint64_t)(*p++ & 1);
    }
    return out;
} /* End ConvertBitIntoBytes() */

/*
 * @brief : This function compute the CRC-9 of the DMR data
 *          by using the polynomial x^9 + x^6 + x^4 + x^3 + 1
 *
 * @param Input : A buffer pointer of the DMR data (80 bytes)
 *        Rate 1/2 coded confirmed (10 data octets): 80 bit sequence (80 bytes)
 *        Rate 3/4 coded confirmed (16 data octets): 128 bit sequence (120 bytes)
 *        Rate 1 coded confirmed (22 data octets): 176 bit sequence (176 bytes)
 *
 * @param NbData : The number of bit to compute
 *        Rate 1/2 coded confirmed (10 data octets): 80 bit sequence (80 bytes)
 *        Rate 3/4 coded confirmed (16 data octets): 128 bit sequence (120 bytes)
 *        Rate 1 coded confirmed (22 data octets): 176 bit sequence (176 bytes)
 *
 * @return The 9 bit CRC
 */
uint16_t
ComputeCrc9Bit(const uint8_t* DMRData, uint32_t NbData) {
    uint32_t i;
    uint16_t CRC = 0x0000; /* Initialization value = 0x0000 */
    /* Polynomial x^9 + x^6 + x^4 + x^3 + 1
   * Normal     = 0x059
   * Reciprocal = 0x134
   * Reversed reciprocal = 0x12C */
    uint16_t Polynome = 0x059;
    for (i = 0; i < NbData; i++) {
        if (((CRC >> 8) & 1) ^ (DMRData[i] & 1)) {
            CRC = (CRC << 1) ^ Polynome;
        } else {
            CRC <<= 1;
        }
    }

    /* Conserve only the 9 LSBs */
    CRC &= 0x01FF;

    /* Invert the CRC */
    CRC ^= 0x01FF;

    /* Return the CRC */
    return CRC;
} /* End ComputeCrc9Bit() */

/*
 * @brief : This function compute the CRC-32 of the DMR data
 *          by using the polynomial x^32 + x^26 + x^23 + x^22 + x^16 + x^12 + x^11 + x^10 + x^8 + x^7 + x^5 + x^4 + x^2 + x + 1
 *
 * @param Input : A buffer pointer of the DMR data (80 bytes)
 *        Rate 1/2 coded confirmed (10 data octets): 80 bit sequence (80 bytes)
 *        Rate 3/4 coded confirmed (16 data octets): 128 bit sequence (120 bytes)
 *        Rate 1 coded confirmed (22 data octets): 176 bit sequence (176 bytes)
 *
 * @param NbData : The number of bit to compute
 *        Rate 1/2 coded confirmed (10 data octets): 80 bit sequence (80 bytes)
 *        Rate 3/4 coded confirmed (16 data octets): 128 bit sequence (120 bytes)
 *        Rate 1 coded confirmed (22 data octets): 176 bit sequence (176 bytes)
 *
 * @return The 32 bit CRC
 */
uint32_t
ComputeCrc32Bit(const uint8_t* DMRData, uint32_t NbData) {
    uint32_t i;
    uint32_t CRC = 0x00000000; /* Initialization value = 0x00000000 */
    /* Polynomial x^32 + x^26 + x^23 + x^22 + x^16 + x^12 + x^11 + x^10 + x^8 + x^7 + x^5 + x^4 + x^2 + x + 1
   * Normal     = 0x04C11DB7
   * Reversed   = 0xEDB88320
   * Reciprocal = 0xDB710641
   * Reversed reciprocal = 0x82608EDB */
    uint32_t Polynome = 0x04C11DB7;
    for (i = 0; i < NbData; i++) {
        if (((CRC >> 31) & 1) ^ (DMRData[i] & 1)) {
            CRC = (CRC << 1) ^ Polynome;
        } else {
            CRC <<= 1;
        }
    }

    // For whatever reason, we get the CRC returned in a reversed byte order (MSO LSO b***s***)
    uint32_t a, b, c, d;
    a = (CRC & 0xFF) << 24;
    b = (CRC & 0xFF00) >> 8;
    b = b << 16;
    c = (CRC & 0xFF0000) >> 16;
    c = c << 8;
    d = (CRC & 0xFF000000) >> 24;

    CRC = a + b + c + d;
    /* Return the CRC */
    return CRC;
} /* End ComputeCrc32Bit() */
