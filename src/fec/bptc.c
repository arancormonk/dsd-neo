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

static uint32_t
bptc_196x96_decode_rows(uint8_t matrix[13][15]) {
    uint8_t line_uncorrected[15];
    uint8_t line_corrected[11];
    uint32_t errors = 0;
    for (uint32_t i = 0; i < 9; i++) {
        for (uint32_t j = 0; j < 15; j++) {
            line_uncorrected[j] = matrix[i][j];
        }
        if (Hamming_15_11_decode(line_uncorrected, line_corrected, 1) == false) {
            errors++;
        }
        for (uint32_t j = 0; j < 11; j++) {
            matrix[i][j] = line_corrected[j];
        }
    }
    return errors;
}

static uint32_t
bptc_196x96_decode_cols(uint8_t matrix[13][15]) {
    uint8_t col_uncorrected[13];
    uint8_t col_corrected[9];
    uint32_t errors = 0;
    for (uint32_t i = 0; i < 15; i++) {
        for (uint32_t j = 0; j < 13; j++) {
            col_uncorrected[j] = matrix[j][i];
        }
        if (Hamming_13_9_decode(col_uncorrected, col_corrected, 1) == false) {
            errors++;
            // The decoder leaves col_corrected unwritten on failure. Keep the
            // received column rather than uninitialized or previous-column data.
            continue;
        }
        for (uint32_t j = 0; j < 9; j++) {
            matrix[j][i] = col_corrected[j];
        }
    }
    return errors;
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
 * @param InputDeInteleavedData : Pointer of DMR input data deinterleaved (196 bytes)
 *
 * @param DMRDataExtracted : Pointer where the DMR data will be written (96 bytes)
 *
 * @return The total number of irrecoverable Hamming check errors of the second
 *         pass, or 0 when BPTC_196x96_Decode_Codeword() then finds a codeword
 *         within DSD_BPTC_196x96_UNIQUE_RADIUS, whose payload and R are written
 */
uint32_t
BPTC_196x96_Extract_Data(uint8_t InputDeInteleavedData[196], uint8_t DMRDataExtracted[96], uint8_t R[3]) {
    uint8_t DataMatrix[13][15];
    bptc_196x96_fill_matrix(InputDeInteleavedData, DataMatrix);
    (void)bptc_196x96_decode_rows(DataMatrix);
    (void)bptc_196x96_decode_cols(DataMatrix);

    uint32_t hamming_irrecoverable_error_nb = 0;
    hamming_irrecoverable_error_nb += bptc_196x96_decode_rows(DataMatrix);
    hamming_irrecoverable_error_nb += bptc_196x96_decode_cols(DataMatrix);

    bptc_196x96_extract_payload_and_r(DataMatrix, DMRDataExtracted, R);
    if (hamming_irrecoverable_error_nb != 0) {
        // Only data bits are copied back above, so a row- or column-parity error
        // can still fail the second pass with the payload right. A codeword
        // within the unique radius is the nearest one: take it instead.
        uint8_t codeword_bits[96];
        uint8_t codeword_r[3];
        uint32_t distance = 0;
        if (BPTC_196x96_Decode_Codeword(InputDeInteleavedData, codeword_bits, codeword_r, &distance) == 1
            && distance <= DSD_BPTC_196x96_UNIQUE_RADIUS) {
            DSD_MEMCPY(DMRDataExtracted, codeword_bits, sizeof codeword_bits);
            DSD_MEMCPY(R, codeword_r, sizeof codeword_r);
            return 0;
        }
    }
    return hamming_irrecoverable_error_nb;
} /* End BPTC_196x96_Extract_Data() */

/* One row and column pass that writes every corrected bit back, parity bits
 * included. A failed decode leaves its row or column as received. */
static void
bptc_196x96_full_pass(uint8_t matrix[13][15]) {
    for (uint32_t i = 0; i < 9; i++) {
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

/* True when every row and column has a zero syndrome. The column-parity rows
 * 9-12 follow from the others by linearity; they are checked all the same. */
static bool
bptc_196x96_is_codeword(uint8_t matrix[13][15]) {
    uint8_t line[15];
    for (uint32_t i = 0; i < 13; i++) {
        DSD_MEMCPY(line, matrix[i], sizeof line);
        if (Hamming_15_11_decode(line, NULL, 1) == false || memcmp(line, matrix[i], sizeof line) != 0) {
            return false;
        }
    }
    uint8_t col[13];
    for (uint32_t i = 0; i < 15; i++) {
        for (uint32_t j = 0; j < 13; j++) {
            col[j] = matrix[j][i];
        }
        if (Hamming_13_9_decode(col, NULL, 1) == false) {
            return false;
        }
        for (uint32_t j = 0; j < 13; j++) {
            if (col[j] != matrix[j][i]) {
                return false;
            }
        }
    }
    return true;
}

/*
 * @brief : Decode a deinterleaved 196 bits buffer to a full BPTC (196,96)
 *          codeword, for callers that need the codeword itself rather than
 *          the best-effort payload of BPTC_196x96_Extract_Data().
 *
 * Two row and column passes run on a copy with every corrected bit written
 * back, parity bits included. The decode is iterative, not nearest-codeword:
 * some patterns of few errors reach a different codeword further away, so
 * callers gate on the returned distance.
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
    bptc_196x96_full_pass(matrix);
    bptc_196x96_full_pass(matrix);
    if (!bptc_196x96_is_codeword(matrix)) {
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

/*
 * @brief : This function extract the 77 bits of a deinteleaved 128 bits
 *          buffer using BPTC (128,77).
 *
 * @note : See DMR standard ETSI TS 102 361-1 chapter B.2.1 for details
 *
 * @param InputDeInteleavedData : Pointer of DMR input data deinterleaved (128 bytes
                                  act as a 16 x 8 byte data matrix)
 *
 * @param DMRDataExtracted : Pointer where the DMR data will be written (77 bytes)
 *                           DMRDataExtracted[0..71]  = Util data (hamming corrected)
 *                           DMRDataExtracted[72..76] = 5 bit of CRC extracted (to be checked)
 *
 * @return 0 = All is OK
 *         Other = Error - Irrecoverable Hamming parity check error number
 */
uint32_t
BPTC_128x77_Extract_Data(uint8_t InputDataMatrix[8][16], uint8_t DMRDataExtracted[77]) {
    uint32_t i, j, k;
    uint8_t DataMatrix[8][16];
    uint8_t LineUncorrected[16];
    uint8_t LineCorrected[11];
    uint32_t HammingIrrecoverableErrorNb = 0;
    uint32_t ParityCheckErrorNb = 0;

    /* First step : Reconstitute the BPTC 16x8 matrix */
    for (i = 0; i < 8; i++) {
        for (j = 0; j < 16; j++) {
            /* Only the LSBit of the byte is stored */
            DataMatrix[i][j] = (InputDataMatrix[i][j] & 1);
        }
    }

    /* Init the Hamming (16,11,4) library
   * Not needed because it has already been done
   * in the "InitAllFecFunction()" function */

    /* Process the the Hamming (16,11,4) code
   * check on each line.
   * Do not check the last line (line nb 8) */
    for (i = 0; i < 7; i++) {
        /* Get a full line */
        for (j = 0; j < 16; j++) {
            LineUncorrected[j] = DataMatrix[i][j];
        }

        /* Apply Hamming (16,11,4) code correction */
        if (Hamming_16_11_4_decode(LineUncorrected, LineCorrected, 1) == false) {
            HammingIrrecoverableErrorNb++;
            // The decoder leaves LineCorrected unwritten on failure. Preserve the
            // received row rather than copying uninitialized or previous-row data.
            continue;
        }

        /* Re-inject the line in the matrix (only the util data [11 bit],
     * not the Hamming part [4 bit]) */
        for (j = 0; j < 11; j++) {
            DataMatrix[i][j] = LineCorrected[j];
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

    /* Verify the data integrity by checking
   * all column parity bit */
    ParityCheckErrorNb = 0;
    for (i = 0; i < 16; i++) {
        uint32_t NbOfOne = 0;

        /* Get a full column */
        for (j = 0; j < 7; j++) {
            NbOfOne += DataMatrix[j][i];
        }

        /* Check the parity bit (number of "1"
     * must be even) */
        if ((NbOfOne % 2) != DataMatrix[7][i]) {
            ParityCheckErrorNb++;
        }
    }

    /* Return the number of irrecoverable Hamming errors +
   * the number of parity check error */
    return (HammingIrrecoverableErrorNb + ParityCheckErrorNb);
} /* End BPTC_128x77_Extract_Data() */

/*
 * @brief : This function extract the 32 bits of a deinteleaved 32 bits
 *          buffer using Reverse Channel Single Burst BPTC.
 *
 * @note : See DMR standard ETSI TS 102 361-1 chapter B.2.2.2 for details
 *
 * @param InputInterleavedData : Pointer of DMR input data interleaved (32 bytes
                                 act as a 16 x 2 bits data matrix)
 *
 * @param DMRDataExtracted : Pointer where the DMR data will be written (77 bytes)
 *                           DMRDataExtracted[0..10]  = Util data (Hamming corrected)
 *                           DMRDataExtracted[11..15] = Hamming data
 *                           DMRDataExtracted[16..31] = 16 bits of odd parity (to be checked)
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
    uint8_t LineUncorrected[16];
    uint8_t LineCorrected[11];
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

    /* Prepare the line to be corrected */
    for (i = 0; i < 16; i++) {
        LineUncorrected[i] = DataMatrix[i] & 1;
    }

    /* Apply Hamming (16,11,4) code correction */
    if (Hamming_16_11_4_decode(LineUncorrected, LineCorrected, 1) == false) {
        HammingIrrecoverableErrorNb++;
    }

    /* Copy the corrected content to the output buffer */
    for (i = 0; i < 11; i++) {
        DMRDataExtracted[i] = LineCorrected[i] & 1;
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
