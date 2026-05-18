// SPDX-License-Identifier: ISC
/**
 * @file
 * @brief P25 Phase 1 NID validation helpers.
 *
 * Provides NID helpers for decoding and validating the 64-bit Network
 * Identifier field on every P25 Phase 1 data unit. The NID carries the NAC
 * (Network Access Code) and DUID (Data Unit Identifier) protected by a
 * BCH(63,16,23) code plus a single parity bit.
 */
#ifndef P25P1_CHECK_NID_H_3af071e917ea43fdb51326e2cbfbde0a
#define P25P1_CHECK_NID_H_3af071e917ea43fdb51326e2cbfbde0a

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief NID decode result codes.
 *
 * Returned by check_NID() to indicate the outcome of BCH decoding,
 * DUID validation, and parity checking. Dispatch code uses these to
 * decide whether to accept or reject the frame.
 *
 * Values are chosen so that simple truth checks still work:
 *   - Positive values (NID_OK, NID_PARITY_OVERRIDE) indicate an accepted frame.
 *   - Zero or negative values (NID_DECODE_FAIL, NID_PARITY_MISMATCH) indicate rejection.
 */
enum NidResult {
    NID_PARITY_MISMATCH = -1, /**< BCH decoded, valid DUID, but parity disagrees (rejected).
                                   Reserved for callers that choose to enforce the final
                                   parity bit; the default decoder accepts parity-bit
                                   disagreement after successful BCH correction. */
    NID_DECODE_FAIL = 0,      /**< BCH decode failed (>11 errors) or decoded DUID is not
                                   in the valid set from TIA-102.BAAA-A Table 8-4. */
    NID_OK = 1,               /**< Success: BCH correction + parity + DUID all valid. */
    NID_PARITY_OVERRIDE = 2   /**< BCH decoded, valid DUID, parity disagrees but accepted. */
};

/**
 * @brief Decode and validate a P25 NID codeword with correction count reporting.
 *
 * Performs BCH(63,16,23) error correction on the 63-bit NID codeword,
 * validates the decoded DUID against the set of defined frame types,
 * checks the parity bit for consistency, and applies parity override logic.
 * The final parity bit is not part of the BCH(63,16)
 * codeword; after successful BCH correction and DUID validation, parity
 * disagreement is accepted and reported as NID_PARITY_OVERRIDE.
 *
 * @param bch_code    Input. An array of 63 bytes, each containing one bit of the NID.
 *                    This includes the NAC (12 bits), DUID (4 bits) and 47 bits of BCH parity.
 * @param new_nac     Output. Pointer to store the decoded 12-bit NAC value after error correction.
 * @param new_duid    Output. Pointer to a 3-char buffer to store the decoded DUID as a
 *                    null-terminated string (e.g., "11" for LDU1, "22" for LDU2).
 * @param parity      Input. The 64th parity bit read from the air interface.
 * @param error_count Output. Pointer to store the number of BCH errors corrected.
 *                    Valid when the return value is positive (NID_OK or NID_PARITY_OVERRIDE).
 *                    Set to 0 on decode failure.
 * @return NidResult code indicating decode outcome. Positive values indicate acceptance,
 *         zero or negative values indicate rejection.
 */
int check_NID_with_error_count(char* bch_code, int* new_nac, char* new_duid, unsigned char parity, int* error_count);

/**
 * @brief Decode and validate a P25 NID, retrying with a known NAC after BCH failure.
 *
 * Mirrors sdrtrunk's P25 Phase 1 NID recovery path: first decode the received
 * codeword normally. If BCH correction fails and @p observed_nac is a valid
 * known channel NAC, replace the received NAC field with @p observed_nac and
 * retry BCH correction once. The retry is not used for decoded-but-invalid
 * DUIDs.
 *
 * @param bch_code     Input. An array of 63 bytes, each containing one bit of the NID.
 * @param observed_nac Known 12-bit NAC to use for retry, or 0 when unavailable.
 * @param new_nac      Output. Pointer to store the decoded 12-bit NAC value.
 * @param new_duid     Output. Pointer to a 3-char buffer for the decoded DUID string.
 * @param parity       Input. The 64th parity bit read from the air interface.
 * @param error_count  Output. Pointer to store the number of BCH errors corrected.
 * @return NidResult code (same semantics as check_NID_with_error_count()).
 */
int check_NID_with_observed_nac(char* bch_code, int observed_nac, int* new_nac, char* new_duid, unsigned char parity,
                                int* error_count);

/**
 * @brief Decode and validate a P25 NID with reliability-guided fallback.
 *
 * Runs the hard observed-NAC path first. If hard BCH/NID validation fails,
 * a bounded Chase retry flips only low-cost candidate bits selected from
 * @p reliab63, scoring each candidate with the P25 soft-information convention
 * where 0 is uncertain and 255 is confident.
 *
 * @param bch_code      Input. An array of 63 bytes, each containing one bit of the NID.
 * @param reliab63      Input. Per-bit reliability for @p bch_code, or NULL to use hard-only decode.
 * @param observed_nac  Known 12-bit NAC to use for retry, or 0 when unavailable.
 * @param new_nac       Output. Pointer to store the decoded 12-bit NAC value.
 * @param new_duid      Output. Pointer to a 3-char buffer for the decoded DUID string.
 * @param parity        Input. The 64th parity bit read from the air interface.
 * @param parity_reliab Input. Reliability of the final parity bit.
 * @param error_count   Output. Pointer to store the number of BCH errors corrected.
 * @return NidResult code (same semantics as check_NID_with_error_count()).
 */
int check_NID_with_observed_nac_soft(char* bch_code, const uint8_t* reliab63, int observed_nac, int* new_nac,
                                     char* new_duid, unsigned char parity, uint8_t parity_reliab, int* error_count);

/**
 * @brief Decode and validate a P25 NID codeword.
 *
 * Calls the full check_NID_with_error_count() interface with a local dummy
 * error_count. Provided for callers that do not need correction count
 * information.
 *
 * @param bch_code Input. An array of 63 bytes, each containing one bit of the NID.
 * @param new_nac  Output. Pointer to store the decoded 12-bit NAC value.
 * @param new_duid Output. Pointer to a 3-char buffer for the decoded DUID string.
 * @param parity   Input. The 64th parity bit read from the air interface.
 * @return NidResult code (same semantics as check_NID_with_error_count()).
 */
int check_NID(char* bch_code, int* new_nac, char* new_duid, unsigned char parity);

#ifdef __cplusplus
}
#endif

#endif // P25P1_CHECK_NID_H_3af071e917ea43fdb51326e2cbfbde0a
