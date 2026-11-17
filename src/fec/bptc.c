// SPDX-License-Identifier: ISC
/*
 ============================================================================
 Name        : bptc.c
 Author      : MotoTRBO_Encrypted_DSD_Author
 Version     : 1.0
 Date        : 2017 - December - 11
 Copyright   : None
 Description : BPTC library source
 ============================================================================
 */

/* Include ------------------------------------------------------------------*/
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/fec/block_codes.h>
#include <dsd-neo/fec/bptc.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Define -------------------------------------------------------------------*/

/* Global variables ---------------------------------------------------------*/
const uint8_t BPTCDeInterleavingIndex[196] = {
    0,   13,  26,  39,  52,  65,  78,  91,  104, 117, 130, 143, 156, 169, 182, 195, 12,  25,  38,  51,  64,  77,
    90,  103, 116, 129, 142, 155, 168, 181, 194, 11,  24,  37,  50,  63,  76,  89,  102, 115, 128, 141, 154, 167,
    180, 193, 10,  23,  36,  49,  62,  75,  88,  101, 114, 127, 140, 153, 166, 179, 192, 9,   22,  35,  48,  61,
    74,  87,  100, 113, 126, 139, 152, 165, 178, 191, 8,   21,  34,  47,  60,  73,  86,  99,  112, 125, 138, 151,
    164, 177, 190, 7,   20,  33,  46,  59,  72,  85,  98,  111, 124, 137, 150, 163, 176, 189, 6,   19,  32,  45,
    58,  71,  84,  97,  110, 123, 136, 149, 162, 175, 188, 5,   18,  31,  44,  57,  70,  83,  96,  109, 122, 135,
    148, 161, 174, 187, 4,   17,  30,  43,  56,  69,  82,  95,  108, 121, 134, 147, 160, 173, 186, 3,   16,  29,
    42,  55,  68,  81,  94,  107, 120, 133, 146, 159, 172, 185, 2,   15,  28,  41,  54,  67,  80,  93,  106, 119,
    132, 145, 158, 171, 184, 1,   14,  27,  40,  53,  66,  79,  92,  105, 118, 131, 144, 157, 170, 183};

const uint8_t DeInterleaveReverseChannelBptc[32] = {0,  17, 2,  19, 4,  21, 6,  23, 8,  25, 10, 27, 12, 29, 14, 31,
                                                    16, 1,  18, 3,  20, 5,  22, 7,  24, 9,  26, 11, 28, 13, 30, 15};

const uint8_t DeInterleaveReverseChannelBptcPlacement[32] = {0,  16, 1,  17, 2,  18, 3,  19, 4,  20, 5,
                                                             21, 6,  22, 7,  23, 8,  24, 9,  25, 10, 26,
                                                             11, 27, 12, 28, 13, 29, 14, 30, 15, 31};

/* Functions ----------------------------------------------------------------*/

/*
 * @brief : This function deinterleave the DMR data by using BPTC (196,96)
 *
 * @param Input : Pointer of DMR input data interleaved (196 bytes)
 *
 * @param Output : Pointer where DMR deinterleaved data will be written (196 bytes)
 *
 * @return None
 */
void
BPTCDeInterleaveDMRData(const uint8_t* Input, uint8_t* Output) {
    uint32_t i;

    for (i = 0; i < 196; i++) {
        uint32_t DeInterleaveIndex = BPTCDeInterleavingIndex[i];
        Output[DeInterleaveIndex] = (Input[i] & 1);
    }
} /* End BPTCDeInterleaveDMRData() */

static void
bptc_196x96_fill_matrix(const uint8_t input[196], uint8_t matrix[13][15]) {
    uint32_t k = 1;
    for (uint32_t i = 0; i < 13; i++) {
        for (uint32_t j = 0; j < 15; j++) {
            matrix[i][j] = (input[k] & 1);
            k++;
        }
    }
}

/* One row and column pass over the whole matrix, the column-parity rows 9..12 included, that writes every corrected bit
 * back, parity bits included, so the next pass starts from a consistent matrix. A failed decode leaves its row or
 * column as received. */
static void
bptc_196x96_full_pass(uint8_t matrix[13][15]) {
    for (uint32_t i = 0; i < 13; i++) {
        (void)Hamming_15_11_decode(matrix[i], NULL, 1);
    }
    uint8_t col[13];
    for (uint32_t i = 0; i < 15; i++) {
        for (uint32_t j = 0; j < 13; j++) {
            col[j] = matrix[j][i];
        }
        (void)Hamming_13_9_decode(col, NULL, 1);
        for (uint32_t j = 0; j < 13; j++) {
            matrix[j][i] = col[j];
        }
    }
}

/* The rows and columns whose syndrome is not zero: 0 for a valid product codeword. Hamming(15,11) is perfect, so a row
 * decode always "succeeds"; only a zero syndrome, a decode that changes nothing, shows that a row is on the code. */
static uint32_t
bptc_196x96_off_code(uint8_t matrix[13][15]) {
    uint32_t off = 0;
    uint8_t line[15];
    for (uint32_t i = 0; i < 13; i++) {
        for (uint32_t j = 0; j < 15; j++) {
            line[j] = matrix[i][j];
        }
        if (Hamming_15_11_decode(line, NULL, 1) == false || memcmp(line, matrix[i], sizeof line) != 0) {
            off++;
        }
    }
    uint8_t col[13];
    uint8_t received_col[13];
    for (uint32_t i = 0; i < 15; i++) {
        for (uint32_t j = 0; j < 13; j++) {
            col[j] = matrix[j][i];
            received_col[j] = col[j];
        }
        if (Hamming_13_9_decode(col, NULL, 1) == false || memcmp(col, received_col, sizeof col) != 0) {
            off++;
        }
    }
    return off;
}

/* Decodes the matrix in place. A received codeword is its own nearest one and stays as it is; any other word gets two
 * full passes. Returns bptc_196x96_off_code() of the result, 0 when it is a valid codeword. */
static uint32_t
bptc_196x96_decode_matrix(uint8_t matrix[13][15]) {
    if (bptc_196x96_off_code(matrix) == 0U) {
        return 0;
    }
    bptc_196x96_full_pass(matrix);
    bptc_196x96_full_pass(matrix);
    return bptc_196x96_off_code(matrix);
}

static void
bptc_196x96_extract_payload_and_r(uint8_t matrix[13][15], uint8_t extracted[96], uint8_t r_bits[3]) {
    uint32_t k = 0;
    for (uint32_t i = 3; i < 11; i++) {
        extracted[k++] = matrix[0][i];
    }
    for (uint32_t i = 1; i < 9; i++) {
        for (uint32_t j = 0; j < 11; j++) {
            extracted[k++] = matrix[i][j];
        }
    }
    r_bits[0] = matrix[0][2];
    r_bits[1] = matrix[0][1];
    r_bits[2] = matrix[0][0];
}

/*
 * @brief : This function extract the 96 bits of a deinteleaved 196 bits
 *          buffer using BPTC (196,96)
 *
 * Every row and column, parity rows and columns included, is decoded with
 * full copy-back (bptc_196x96_decode_matrix()), and the result counts only
 * when it is a valid product codeword: a row or column decode that reports
 * success can still leave the matrix off the code, so a success count alone
 * would pass noise and miscorrections.
 *
 * @param InputDeInteleavedData : Pointer of DMR input data deinterleaved (196 bytes)
 *
 * @param DMRDataExtracted : Pointer where the DMR data will be written (96 bytes)
 *
 * @return 0 when the decode reached a valid codeword, whose payload and R are
 *         written; otherwise the number of rows and columns still off the code,
 *         with the decoder's best-effort payload and R written
 */
uint32_t
BPTC_196x96_Extract_Data(uint8_t InputDeInteleavedData[196], uint8_t DMRDataExtracted[96], uint8_t R[3]) {
    uint8_t DataMatrix[13][15];
    bptc_196x96_fill_matrix(InputDeInteleavedData, DataMatrix);
    const uint32_t off_code = bptc_196x96_decode_matrix(DataMatrix);
    bptc_196x96_extract_payload_and_r(DataMatrix, DMRDataExtracted, R);
    return off_code;
} /* End BPTC_196x96_Extract_Data() */

/*
 * @brief : Decode a deinterleaved 196 bits buffer to a full BPTC (196,96)
 *          codeword, for callers that need the codeword and its distance to
 *          the received bits rather than BPTC_196x96_Extract_Data()'s verdict.
 *
 * The decode is BPTC_196x96_Extract_Data()'s: two row and column passes on a
 * copy with every corrected bit written back. It is iterative, not
 * nearest-codeword: some patterns of few errors reach a different codeword
 * further away, so callers gate on the returned distance.
 *
 * @param input196 : DMR data deinterleaved (196 bytes, one bit each, unchanged)
 *
 * @param out96 : Where the codeword's 96 payload bits are written
 *
 * @param r3 : Where the codeword's reserved bits are written, in the order of
 *             BPTC_196x96_Extract_Data()'s R
 *
 * @param distance : Where the number of the 195 product-code bits (bit 0 is
 *                   not part of the code) that differ between input196 and the
 *                   codeword is written
 *
 * @return 1 when the passes reach a valid codeword; 0 otherwise, with out96,
 *         r3 and distance zeroed (nothing is written when an argument is NULL)
 */
int
BPTC_196x96_Decode_Codeword(const uint8_t* input196, uint8_t* out96, uint8_t* r3, uint32_t* distance) {
    if (input196 == NULL || out96 == NULL || r3 == NULL || distance == NULL) {
        return 0;
    }
    uint8_t received[13][15];
    uint8_t matrix[13][15];
    bptc_196x96_fill_matrix(input196, received);
    DSD_MEMCPY(matrix, received, sizeof matrix);
    if (bptc_196x96_decode_matrix(matrix) != 0U) {
        DSD_MEMSET(out96, 0, 96);
        DSD_MEMSET(r3, 0, 3);
        *distance = 0;
        return 0;
    }

    uint32_t diff = 0;
    for (uint32_t i = 0; i < 13; i++) {
        for (uint32_t j = 0; j < 15; j++) {
            diff += (uint32_t)(matrix[i][j] != received[i][j]);
        }
    }
    bptc_196x96_extract_payload_and_r(matrix, out96, r3);
    *distance = diff;
    return 1;
} /* End BPTC_196x96_Decode_Codeword() */

/* Hamming(16,11,4) decode of one 16-bit row in place, parity bits included, so the row stays a codeword the column
 * parity can check. A failed decode (an even number of errors) keeps the received row. */
static bool
bptc_hamming_16_11_4_row(uint8_t row[16]) {
    uint8_t line[16];
    for (uint32_t j = 0; j < 16; j++) {
        line[j] = row[j] & 1U;
    }
    if (Hamming_16_11_4_decode(line, NULL, 1) == false) {
        return false;
    }
    for (uint32_t j = 0; j < 16; j++) {
        row[j] = line[j];
    }
    return true;
}

/* The columns whose even parity over all 8 rows fails, flagged in @p bad; returns how many. */
static uint32_t
bptc_128x77_bad_columns(uint8_t matrix[8][16], uint8_t bad[16]) {
    uint32_t count = 0;
    for (uint32_t j = 0; j < 16; j++) {
        uint8_t parity = 0;
        for (uint32_t i = 0; i < 8; i++) {
            parity ^= matrix[i][j];
        }
        bad[j] = parity;
        count += parity;
    }
    return count;
}

/* Parity-guided repair: a row the Hamming code detected but could not correct holds an even number of errors. When it
 * is the only such row and exactly two columns fail their parity, those two bits are its errors, so both flip. */
static void
bptc_128x77_repair_row(uint8_t matrix[8][16], const uint8_t row_failed[8]) {
    uint32_t failed_rows = 0;
    uint32_t failed_row = 0;
    for (uint32_t i = 0; i < 8; i++) {
        if (row_failed[i]) {
            failed_rows++;
            failed_row = i;
        }
    }
    uint8_t bad_columns[16];
    if (failed_rows != 1U || bptc_128x77_bad_columns(matrix, bad_columns) != 2U) {
        return;
    }
    for (uint32_t j = 0; j < 16; j++) {
        matrix[failed_row][j] ^= bad_columns[j];
    }
}

/*
 * @brief : This function extract the 77 bits of a deinteleaved 128 bits
 *          buffer using BPTC (128,77).
 *
 * @note : See DMR standard ETSI TS 102 361-1 chapter B.2.1 for details. Rows
 *         0..6 are Hamming(16,11,4) codewords and row 7 holds the even parity
 *         of each column, so it is a codeword of the same linear code: every
 *         row is decoded with its parity bits written back, a single failed
 *         row with two failed columns is repaired, and the rows are decoded
 *         again before the column parity is checked.
 *
 * @param InputDeInteleavedData : Pointer of DMR input data deinterleaved (128 bytes
                                  act as a 16 x 8 byte data matrix)
 *
 * @param DMRDataExtracted : Pointer where the DMR data will be written (77 bytes)
 *                           DMRDataExtracted[0..71]  = Util data (hamming corrected)
 *                           DMRDataExtracted[72..76] = 5 bit of CRC extracted (to be checked)
 *
 * @return 0 = All is OK
 *         Other = Error - rows still failing their Hamming decode plus columns
 *                 failing their parity; a failed row keeps its received bits
 */
uint32_t
BPTC_128x77_Extract_Data(uint8_t InputDataMatrix[8][16], uint8_t DMRDataExtracted[77]) {
    uint8_t DataMatrix[8][16];
    uint8_t row_failed[8];
    uint32_t i, j, k;

    for (i = 0; i < 8; i++) {
        for (j = 0; j < 16; j++) {
            DataMatrix[i][j] = (InputDataMatrix[i][j] & 1);
        }
    }

    for (i = 0; i < 8; i++) {
        row_failed[i] = (uint8_t)!bptc_hamming_16_11_4_row(DataMatrix[i]);
    }
    bptc_128x77_repair_row(DataMatrix, row_failed);

    uint32_t HammingIrrecoverableErrorNb = 0;
    for (i = 0; i < 8; i++) {
        if (!bptc_hamming_16_11_4_row(DataMatrix[i])) {
            HammingIrrecoverableErrorNb++;
        }
    }

    /* Extract the DMR data (77 bit) from the matrix */
    k = 0;

    /* 2 first lines */
    for (i = 0; i < 2; i++) {
        for (j = 0; j < 11; j++) {
            DMRDataExtracted[k] = DataMatrix[i][j];
            k++;
        }
    }

    /* 5 Next lines */
    for (i = 2; i < 7; i++) {
        for (j = 0; j < 10; j++) {
            DMRDataExtracted[k] = DataMatrix[i][j];
            k++;
        }
    }

    /* 5 bit of CRC */
    for (i = 2; i < 7; i++) {
        DMRDataExtracted[k] = DataMatrix[i][10];
        k++;
    }

    uint8_t bad_columns[16];
    const uint32_t ParityCheckErrorNb = bptc_128x77_bad_columns(DataMatrix, bad_columns);

    /* Return the number of irrecoverable Hamming errors +
   * the number of parity check error */
    return (HammingIrrecoverableErrorNb + ParityCheckErrorNb);
} /* End BPTC_128x77_Extract_Data() */

/*
 * @brief : This function extract the 32 bits of a deinteleaved 32 bits
 *          buffer using Reverse Channel Single Burst BPTC.
 *
 * @note : See DMR standard ETSI TS 102 361-1 chapter B.2.2.2 for details
 *         (and B.2.2.1 for the non-RC form). Both rows are Hamming(16,11,4)
 *         codewords: the parity row equals the information row (even) or is
 *         its complement (odd), and the all-ones word is a codeword of this
 *         code. Each row is decoded with its parity bits written back; a
 *         failed row keeps its received bits and counts as an error.
 *
 * @param InputInterleavedData : Pointer of DMR input data interleaved (32 bytes
                                 act as a 16 x 2 bits data matrix)
 *
 * @param DMRDataExtracted : Pointer where the DMR data will be written (32 bytes)
 *                           DMRDataExtracted[0..10]  = Util data (Hamming corrected)
 *                           DMRDataExtracted[11..15] = Hamming data (corrected)
 *                           DMRDataExtracted[16..31] = 16 bits of parity (corrected, then checked)

 *
 * @param ParityCheckTypeOdd : 0 = Parity to check is even (used in Non-Reverse Channel Single Burst BPTC)
 *                             1 = Parity to check is odd (used in Reverse Channel Single Burst BPTC)
 *
 * @return 0 = All is OK
 *         Other = Error - Irrecoverable Hamming parity check error number
 */
uint32_t
BPTC_16x2_Extract_Data(uint8_t InputInterleavedData[32], uint8_t DMRDataExtracted[32], uint32_t ParityCheckTypeOdd) {
    uint32_t i;
    uint8_t DataMatrix[32];
    uint32_t HammingIrrecoverableErrorNb = 0;
    uint32_t ParityCheckOddErrorNb = 0;
    uint32_t ParityCheckEvenErrorNb = 0;

    //TODO: make this so we can load either rc interleave, or single burst interleave
    for (i = 0; i < 32; i++) {
        DataMatrix[DeInterleaveReverseChannelBptcPlacement[DeInterleaveReverseChannelBptc[i]]] =
            InputInterleavedData[i] & 1;
    }

    /* Copy the entire input buffer to the output buffer */
    for (i = 0; i < 32; i++) {
        DMRDataExtracted[i] = DataMatrix[i] & 1;
    }

    /* Apply Hamming (16,11,4) code correction to both rows. The output keeps the received bits of a row that fails,
     * and the failure counts in the return value, so the caller takes none of it. */
    for (size_t row = 0; row < 2U; row++) {
        if (!bptc_hamming_16_11_4_row(&DMRDataExtracted[row * 16U])) {
            HammingIrrecoverableErrorNb++;
        }
    }

    /* Check Parity bits */
    for (i = 0; i < 16; i++) {
        /* Odd parity ==> If data = 1 then parity = 0
     *                If data = 0 then parity = 1 */
        if ((DMRDataExtracted[i] & 1) == (DMRDataExtracted[i + 16] & 1)) {
            ParityCheckOddErrorNb++;
        }
        if ((DMRDataExtracted[i] & 1) != (DMRDataExtracted[i + 16] & 1)) {
            ParityCheckEvenErrorNb++;
        }
    }

    /* Return the number of irrecoverable Hamming errors +
   * the number of parity check error */
    if (ParityCheckTypeOdd) {
        return (HammingIrrecoverableErrorNb + ParityCheckOddErrorNb);
    } else {
        return (HammingIrrecoverableErrorNb + ParityCheckEvenErrorNb);
    }

} /* End BPTC_16x2_Extract_Data() */

/* End of file */
