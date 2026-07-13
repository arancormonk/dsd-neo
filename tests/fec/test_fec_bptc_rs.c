// SPDX-License-Identifier: GPL-3.0-or-later
// Coverage fixtures intentionally use private-source inclusion, synthetic sentinels,
// or invalid-value negative vectors to exercise guarded behavior.
// NOLINTBEGIN(bugprone-implicit-widening-of-multiplication-result)
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <dsd-neo/fec/block_codes.h>
#include <dsd-neo/fec/bptc.h>
#include <dsd-neo/fec/rs_12_9.h>
#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/safe_api.h"

/* Fixed DMR BPTC(196,96) reference codeword. Bit 0 is the reserved bit; the
 * remaining bits are the row-major 13x15 product code consumed by the decoder. */
static const uint8_t k_bptc_196_codeword[196] = {
    0, 1, 0, 1, 0, 1, 0, 1, 0, 0, 1, 0, 0, 0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 0, 1, 0, 0, 0,
    1, 0, 1, 0, 0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0, 1, 0, 0,
    1, 0, 1, 0, 0, 1, 0, 1, 0, 0, 0, 1, 0, 0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 0, 0,
    1, 0, 1, 0, 0, 1, 0, 0, 0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 0,
    1, 0, 1, 0, 1, 0, 1, 1, 0, 0, 1, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 1, 1, 1, 0, 1, 1, 0, 1, 1,
    0, 0, 1, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 0, 0, 1, 1, 1, 1, 1, 0, 1, 1, 1, 0, 0, 0, 0,
};

static int
test_bptc_196x96_extract(void) {
    InitAllFecFunction();
    uint8_t payload[96];
    static const uint8_t r_bits[3] = {1, 0, 1};
    for (int i = 0; i < 96; i++) {
        payload[i] = (uint8_t)(((i * 17) + (i / 5)) & 1U);
    }

    uint8_t input[196];
    uint8_t extracted[96];
    uint8_t got_r[3];
    DSD_MEMCPY(input, k_bptc_196_codeword, sizeof(input));

    uint32_t irr = BPTC_196x96_Extract_Data(input, extracted, got_r);
    assert(irr == 0);
    for (int i = 0; i < 96; i++) {
        assert(extracted[i] == payload[i]);
    }
    for (int i = 0; i < 3; i++) {
        assert(got_r[i] == r_bits[i]);
    }

    uint8_t correctable[196];
    DSD_MEMCPY(correctable, k_bptc_196_codeword, sizeof(correctable));
    correctable[1 + (4 * 15) + 5] ^= 1U;
    irr = BPTC_196x96_Extract_Data(correctable, extracted, got_r);
    assert(irr == 0);
    for (int i = 0; i < 96; i++) {
        assert(extracted[i] == payload[i]);
    }

    uint8_t uncorrectable[196];
    DSD_MEMCPY(uncorrectable, k_bptc_196_codeword, sizeof(uncorrectable));
    for (int row = 0; row < 5; row++) {
        for (int col = 0; col < 5; col++) {
            uncorrectable[1 + (row * 15) + col] ^= 1U;
        }
    }
    irr = BPTC_196x96_Extract_Data(uncorrectable, extracted, got_r);
    assert(irr > 0);

    return 0;
}

static int
test_bptc_128x77(void) {
    InitAllFecFunction();
    static const uint8_t reference[8][16] = {
        {1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 1, 0, 0, 0, 1}, {0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 1, 0, 1},
        {0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 0, 0}, {1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0},
        {1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 0, 1, 0, 0, 0}, {0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 0, 1, 1, 1, 0},
        {0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 0, 0}, {1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 0, 1, 0, 1, 0, 0},
    };
    uint8_t mat[8][16];
    DSD_MEMCPY(mat, reference, sizeof(mat));

    // Message layout per extractor:
    // rows 0..1: 11 bits each; rows 2..6: 10 bits each; CRC bits at mat[i][10] for i=2..6
    uint8_t data_bits72[72];
    uint8_t crc_bits5[5] = {0, 0, 0, 0, 0}; // choose all-zero CRC placeholder
    for (int i = 0; i < 72; i++) {
        data_bits72[i] = (uint8_t)((0xA5 >> (i % 8)) & 1U);
    }

    // Call extractor
    uint8_t extracted[77] = {0};
    uint32_t irr = BPTC_128x77_Extract_Data(mat, extracted);
    assert(irr == 0);

    // Verify mapping of data part (first 72 bits) as implemented in extractor
    int idx = 0;
    for (int row = 0; row < 2; row++) {
        for (int col = 0; col < 11; col++, idx++) {
            assert(extracted[idx] == (mat[row][col] & 1U));
        }
    }
    for (int row = 2; row < 7; row++) {
        for (int col = 0; col < 10; col++, idx++) {
            assert(extracted[idx] == (mat[row][col] & 1U));
        }
    }
    // Last 5 bits are column 10 of rows 2..6 and equal chosen crc_bits5
    for (int row = 2; row < 7; row++, idx++) {
        assert(extracted[idx] == crc_bits5[row - 2]);
    }

    // Additionally ensure extracted data equals our intended data_bits72
    for (int i = 0; i < 72; i++) {
        assert(extracted[i] == (data_bits72[i] & 1U));
    }

    // Inject a single-bit error in a data row and ensure correction path yields no irrecoverables
    uint8_t mat_err[8][16];
    DSD_MEMCPY(mat_err, mat, sizeof(mat_err));
    mat_err[1][3] ^= 1U; // single flip in row 1
    irr = BPTC_128x77_Extract_Data(mat_err, extracted);
    assert(irr == 0);

    return 0;
}

static int
test_bptc_16x2(void) {
    InitAllFecFunction();

    static const uint8_t info[11] = {1, 1, 0, 1, 0, 1, 0, 1, 1, 1, 0};
    static const uint8_t codeword[16] = {1, 1, 0, 1, 0, 1, 0, 1, 1, 1, 0, 0, 0, 0, 1, 0};

    // Build DataMatrix (deinterleaved logical order)
    uint8_t dmat[32] = {0};
    for (int i = 0; i < 16; i++) {
        dmat[i] = codeword[i];
    }

    // Case 1: odd parity (second half is complement)
    for (int i = 0; i < 16; i++) {
        dmat[16 + i] = dmat[i] ^ 1U;
    }

    // Build InputInterleavedData by inverting placement applied in BPTC_16x2_Extract_Data
    uint8_t interleaved[32];
    for (int i = 0; i < 32; i++) {
        interleaved[i] = dmat[DeInterleaveReverseChannelBptcPlacement[DeInterleaveReverseChannelBptc[i]]];
    }

    uint8_t outbits[32];
    uint32_t irr = BPTC_16x2_Extract_Data(interleaved, outbits, 1 /* odd */);
    assert(irr == 0);
    for (int i = 0; i < 11; i++) {
        assert(outbits[i] == (info[i] & 1U));
    }

    // Case 2: even parity (second half equals first half)
    for (int i = 0; i < 16; i++) {
        dmat[16 + i] = dmat[i];
    }
    for (int i = 0; i < 32; i++) {
        interleaved[i] = dmat[DeInterleaveReverseChannelBptcPlacement[DeInterleaveReverseChannelBptc[i]]];
    }
    irr = BPTC_16x2_Extract_Data(interleaved, outbits, 0 /* even */);
    assert(irr == 0);
    for (int i = 0; i < 11; i++) {
        assert(outbits[i] == (info[i] & 1U));
    }

    return 0;
}

static int
test_bptc_196x96_deinterleave(void) {
    // Simple mapping property test
    uint8_t in[196], out[196];
    for (int i = 0; i < 196; i++) {
        in[i] = (uint8_t)((i * 37) & 1U);
    }
    BPTCDeInterleaveDMRData(in, out);
    for (int i = 0; i < 196; i++) {
        uint32_t j = BPTCDeInterleavingIndex[i];
        assert(out[j] == (in[i] & 1U));
    }
    return 0;
}

static int
test_rs_12_9(void) {
    rs_12_9_codeword_t cw = {{3, 20, 37, 54, 71, 88, 105, 122, 139, 208, 63, 250}};

    rs_12_9_poly_t syn = {0};
    rs_12_9_calc_syndrome(&cw, &syn);
    assert(rs_12_9_check_syndrome(&syn) == 0);

    // Single erroneous byte -> corrected
    cw.data[2] ^= 0x55;
    rs_12_9_calc_syndrome(&cw, &syn);
    assert(rs_12_9_check_syndrome(&syn) == 1);
    uint8_t fixed = 0;
    rs_12_9_correct_errors_result_t rc = rs_12_9_correct_errors(&cw, &syn, &fixed);
    assert(rc == RS_12_9_CORRECT_ERRORS_RESULT_ERRORS_CORRECTED);

    // Two erroneous bytes -> uncorrectable
    cw.data[1] ^= 0x22;
    cw.data[7] ^= 0x11;
    rs_12_9_calc_syndrome(&cw, &syn);
    assert(rs_12_9_check_syndrome(&syn) == 1);
    rc = rs_12_9_correct_errors(&cw, &syn, &fixed);
    assert(rc == RS_12_9_CORRECT_ERRORS_RESULT_ERRORS_CANT_BE_CORRECTED);

    return 0;
}

int
main(void) {
    if (test_bptc_196x96_extract() != 0) {
        return 1;
    }
    if (test_bptc_128x77() != 0) {
        return 1;
    }
    if (test_bptc_16x2() != 0) {
        return 1;
    }
    if (test_bptc_196x96_deinterleave() != 0) {
        return 1;
    }
    if (test_rs_12_9() != 0) {
        return 1;
    }
    printf("FEC BPTC+RS tests passed.\n");
    return 0;
}

// NOLINTEND(bugprone-implicit-widening-of-multiplication-result)
