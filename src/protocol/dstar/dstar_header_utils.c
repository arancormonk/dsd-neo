// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/protocol/dstar/dstar_header_utils.h>

#include <string.h>

// D-STAR scrambler: 7-bit LFSR with polynomial x^7 + x^4 + 1, seeded with 0b0000111.
// We output the MSb (reg[6]) each step and clock once per bit. The sequence repeats every 127 bits.
static void
dstar_fill_scrambler_sequence(int* seq) {
    unsigned int reg = 0x07U; // lower three bits set
    for (size_t i = 0; i < DSD_DSTAR_SCRAMBLER_PERIOD; i++) {
        seq[i] = (reg >> 6) & 0x1;
        unsigned int feedback = ((reg >> 6) & 0x1) ^ ((reg >> 3) & 0x1);
        reg = ((reg << 1) & 0x7eU) | feedback;
    }
}

void
dstar_scramble_header_bits(const int* in, int* out, size_t bit_count) {
    int scrambler[DSD_DSTAR_SCRAMBLER_PERIOD];
    dstar_fill_scrambler_sequence(scrambler);
    for (size_t i = 0; i < bit_count; i++) {
        out[i] = in[i] ^ scrambler[i % DSD_DSTAR_SCRAMBLER_PERIOD];
    }
}

// Header interleave uses a 24-column diagonal with wrap rules defined by the D-STAR spec.
// This is the receive-side deinterleave (inverse). For transmit, use the same mapping but
// write into the input order instead of reading from it.
void
dstar_deinterleave_header_bits(const int* in, int* out, size_t bit_count) {
    // The spec fixes the header to 660 coded bits; guard against misuse.
    if (bit_count != DSD_DSTAR_HEADER_CODED_BITS) {
        return;
    }

    size_t k = 0;
    for (size_t i = 0; i < bit_count; i++) {
        out[k] = in[i];
        k += 24;
        if (k >= 672) {
            k -= 671; // wrap after the 27th column
        } else if (k >= 660) {
            k -= 647; // wrap after the last full column
        }
    }
}

static inline int
branch_metric(int sym1, int sym0, int ref1, int ref0) {
    return (sym1 ^ ref1) + (sym0 ^ ref0);
}

// Rate 1/2, K=3, generator polynomials (7,5)_octal.
size_t
dstar_header_viterbi_decode(const int* symbols, size_t symbol_count, int* out_bits, size_t out_capacity) {
    if (symbol_count != DSD_DSTAR_HEADER_CODED_BITS || out_capacity < DSD_DSTAR_HEADER_INFO_BITS) {
        return 0;
    }

    int path_metric[4] = {0, 0, 0, 0};
    int path_memory[4][DSD_DSTAR_HEADER_INFO_BITS];

    size_t n = 0;
    for (size_t i = 0; i < symbol_count; i += 2, n++) {
        int s1 = symbols[i];
        int s0 = symbols[i + 1];

        int temp_metric[4];

        // Next state S0 can come from S0 (00) or S2 (10)
        int m1 = branch_metric(s1, s0, 0, 0) + path_metric[0];
        int m2 = branch_metric(s1, s0, 1, 1) + path_metric[2];
        if (m1 <= m2) {
            path_memory[0][n] = 0;
            temp_metric[0] = m1;
        } else {
            path_memory[0][n] = 1;
            temp_metric[0] = m2;
        }

        // Next state S1 can come from S0 (11) or S2 (00)
        m1 = branch_metric(s1, s0, 1, 1) + path_metric[0];
        m2 = branch_metric(s1, s0, 0, 0) + path_metric[2];
        if (m1 <= m2) {
            path_memory[1][n] = 0;
            temp_metric[1] = m1;
        } else {
            path_memory[1][n] = 1;
            temp_metric[1] = m2;
        }

        // Next state S2 can come from S1 (11) or S3 (01)
        m1 = branch_metric(s1, s0, 1, 0) + path_metric[1];
        m2 = branch_metric(s1, s0, 0, 1) + path_metric[3];
        if (m1 <= m2) {
            path_memory[2][n] = 0;
            temp_metric[2] = m1;
        } else {
            path_memory[2][n] = 1;
            temp_metric[2] = m2;
        }

        // Next state S3 can come from S1 (01) or S3 (10)
        m1 = branch_metric(s1, s0, 0, 1) + path_metric[1];
        m2 = branch_metric(s1, s0, 1, 0) + path_metric[3];
        if (m1 <= m2) {
            path_memory[3][n] = 0;
            temp_metric[3] = m1;
        } else {
            path_memory[3][n] = 1;
            temp_metric[3] = m2;
        }

        for (int s = 0; s < 4; s++) {
            path_metric[s] = temp_metric[s];
        }
    }

    // Traceback: pick the best-metric end state (headers are not tail-padded).
    int state = 0;
    int best_metric = path_metric[0];
    for (int s = 1; s < 4; s++) {
        if (path_metric[s] < best_metric) {
            best_metric = path_metric[s];
            state = s;
        }
    }
    for (int i = (int)DSD_DSTAR_HEADER_INFO_BITS - 1; i >= 0; i--) {
        int decision = path_memory[state][i];
        switch (state) {
            case 0:
                state = decision ? 2 : 0;
                out_bits[i] = 0;
                break;
            case 1:
                state = decision ? 2 : 0;
                out_bits[i] = 1;
                break;
            case 2:
                state = decision ? 3 : 1;
                out_bits[i] = 0;
                break;
            case 3:
                state = decision ? 3 : 1;
                out_bits[i] = 1;
                break;
            default:
                state = 0;
                out_bits[i] = 0;
                break;
        }
    }

    return DSD_DSTAR_HEADER_INFO_BITS;
}

uint16_t
dstar_crc16(const uint8_t* data, size_t len) {
    // CRC-16/X25: poly 0x1021 reflected (0x8408), init 0xffff, xorout 0xffff.
    uint16_t crc = 0xffffU;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            if (crc & 0x0001U) {
                crc = (crc >> 1) ^ 0x8408U;
            } else {
                crc >>= 1;
            }
        }
    }
    crc = (uint16_t)~crc;
    return crc;
}
