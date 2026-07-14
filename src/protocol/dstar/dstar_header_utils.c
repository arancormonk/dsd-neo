// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/protocol/dstar/dstar_header_utils.h>

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

// Scramble soft costs with PN sequence.
// XOR with scrambler bit inverts the soft cost: 0x0000 <-> 0xFFFF
void
dstar_scramble_soft_costs(const uint16_t* in, uint16_t* out, size_t bit_count) {
    int scrambler[DSD_DSTAR_SCRAMBLER_PERIOD];
    dstar_fill_scrambler_sequence(scrambler);
    for (size_t i = 0; i < bit_count; i++) {
        if (scrambler[i % DSD_DSTAR_SCRAMBLER_PERIOD]) {
            // Scrambler bit is 1: invert the soft cost
            out[i] = (uint16_t)(0xFFFF - in[i]);
        } else {
            out[i] = in[i];
        }
    }
}

// Deinterleave soft costs (same permutation as hard bits)
void
dstar_deinterleave_soft_costs(const uint16_t* in, uint16_t* out, size_t bit_count) {
    if (bit_count != DSD_DSTAR_HEADER_CODED_BITS) {
        return;
    }

    size_t k = 0;
    for (size_t i = 0; i < bit_count; i++) {
        out[k] = in[i];
        k += 24;
        if (k >= 672) {
            k -= 671;
        } else if (k >= 660) {
            k -= 647;
        }
    }
}

#define DSTAR_VITERBI_STATE_COUNT 4

static void
dstar_traceback_header_bits(int path_memory[DSTAR_VITERBI_STATE_COUNT][DSD_DSTAR_HEADER_INFO_BITS], int state,
                            int* out_bits) {
    static const int prev_state[2][DSTAR_VITERBI_STATE_COUNT] = {
        {0, 0, 1, 1},
        {2, 2, 3, 3},
    };
    static const int decoded_bit[DSTAR_VITERBI_STATE_COUNT] = {0, 1, 0, 1};

    for (int i = (int)DSD_DSTAR_HEADER_INFO_BITS - 1; i >= 0; i--) {
        int decision = path_memory[state][i];
        out_bits[i] = decoded_bit[state];
        state = prev_state[decision][state];
    }
}

// Soft-decision branch metric: compute distance from expected soft cost.
// ref=0 means expected 0 (cost should be near 0x0000), ref=1 means expected 1 (cost should be near 0xFFFF)
static inline uint32_t
soft_branch_metric(uint16_t sym1, uint16_t sym0, int ref1, int ref0) {
    uint32_t d1, d0;
    // Expected cost for bit 0 is 0x0000, for bit 1 is 0xFFFF
    if (ref1 == 0) {
        d1 = (uint32_t)sym1; // distance from 0x0000
    } else {
        d1 = (uint32_t)(0xFFFF - sym1); // distance from 0xFFFF
    }
    if (ref0 == 0) {
        d0 = (uint32_t)sym0;
    } else {
        d0 = (uint32_t)(0xFFFF - sym0);
    }
    return d1 + d0;
}

static inline void
dstar_select_survivor_u32(uint32_t metric_a, uint32_t metric_b, int* decision, uint32_t* survivor_metric) {
    if (metric_a <= metric_b) {
        *decision = 0;
        *survivor_metric = metric_a;
        return;
    }
    *decision = 1;
    *survivor_metric = metric_b;
}

static void
dstar_viterbi_step_soft(uint16_t s1, uint16_t s0, const uint32_t path_metric[DSTAR_VITERBI_STATE_COUNT],
                        uint32_t temp_metric[DSTAR_VITERBI_STATE_COUNT],
                        int path_memory[DSTAR_VITERBI_STATE_COUNT][DSD_DSTAR_HEADER_INFO_BITS], size_t step_idx) {
    uint32_t m_from_s0 = soft_branch_metric(s1, s0, 0, 0) + path_metric[0];
    uint32_t m_from_s2 = soft_branch_metric(s1, s0, 1, 1) + path_metric[2];
    dstar_select_survivor_u32(m_from_s0, m_from_s2, &path_memory[0][step_idx], &temp_metric[0]);

    m_from_s0 = soft_branch_metric(s1, s0, 1, 1) + path_metric[0];
    m_from_s2 = soft_branch_metric(s1, s0, 0, 0) + path_metric[2];
    dstar_select_survivor_u32(m_from_s0, m_from_s2, &path_memory[1][step_idx], &temp_metric[1]);

    uint32_t m_from_s1 = soft_branch_metric(s1, s0, 1, 0) + path_metric[1];
    uint32_t m_from_s3 = soft_branch_metric(s1, s0, 0, 1) + path_metric[3];
    dstar_select_survivor_u32(m_from_s1, m_from_s3, &path_memory[2][step_idx], &temp_metric[2]);

    m_from_s1 = soft_branch_metric(s1, s0, 0, 1) + path_metric[1];
    m_from_s3 = soft_branch_metric(s1, s0, 1, 0) + path_metric[3];
    dstar_select_survivor_u32(m_from_s1, m_from_s3, &path_memory[3][step_idx], &temp_metric[3]);
}

static int
dstar_select_best_state_u32(const uint32_t path_metric[DSTAR_VITERBI_STATE_COUNT]) {
    int state = 0;
    uint32_t best_metric = path_metric[0];

    for (int s = 1; s < DSTAR_VITERBI_STATE_COUNT; s++) {
        if (path_metric[s] < best_metric) {
            best_metric = path_metric[s];
            state = s;
        }
    }
    return state;
}

// Soft-decision Viterbi decoder for D-STAR header.
// Rate 1/2, K=3, generator polynomials (7,5)_octal.
// Input: soft costs where 0x0000 = strong 0, 0xFFFF = strong 1, 0x7FFF = uncertain
size_t
dstar_header_viterbi_decode_soft(const uint16_t* soft_symbols, size_t symbol_count, int* out_bits,
                                 size_t out_capacity) {
    if (symbol_count != DSD_DSTAR_HEADER_CODED_BITS || out_capacity < DSD_DSTAR_HEADER_INFO_BITS) {
        return 0;
    }

    uint32_t path_metric[DSTAR_VITERBI_STATE_COUNT] = {0, 0, 0, 0};
    int path_memory[DSTAR_VITERBI_STATE_COUNT][DSD_DSTAR_HEADER_INFO_BITS];

    for (size_t i = 0, n = 0; i < symbol_count; i += 2, n++) {
        uint16_t s1 = soft_symbols[i];
        uint16_t s0 = soft_symbols[i + 1];
        uint32_t temp_metric[DSTAR_VITERBI_STATE_COUNT];
        dstar_viterbi_step_soft(s1, s0, path_metric, temp_metric, path_memory, n);
        path_metric[0] = temp_metric[0];
        path_metric[1] = temp_metric[1];
        path_metric[2] = temp_metric[2];
        path_metric[3] = temp_metric[3];
    }

    int state = dstar_select_best_state_u32(path_metric);
    dstar_traceback_header_bits(path_memory, state, out_bits);

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
    return (uint16_t)((crc << 8) | (crc >> 8));
}
