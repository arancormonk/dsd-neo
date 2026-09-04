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

#include <dsd-neo/platform/platform.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief NID decode result codes.
 *
 * Returned in p25p1_nid_result to indicate the outcome of BCH decoding,
 * DUID validation, and parity checking. Dispatch code uses these to
 * decide whether to accept or reject the frame.
 *
 * Values are chosen so that simple truth checks still work:
 *   - Positive values (NID_OK, NID_PARITY_OVERRIDE) indicate an accepted frame.
 *   - Zero or negative values (NID_DECODE_FAIL, NID_PARITY_MISMATCH) indicate rejection.
 */
enum DSD_ATTR_PACKED NidResult {
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
 * @brief Typed result of P25 Phase 1 NID decoding.
 */
struct p25p1_nid_result {
    enum NidResult status;
    int nac;
    uint8_t duid;
    int error_count;
};

/**
 * @brief Decode and validate a P25 NID with optional recovery inputs.
 *
 * Performs hard BCH decoding first. After a BCH failure, a valid known channel
 * NAC can replace the received NAC field for one hard retry. When reliability
 * is available and hard validation still fails, a bounded Chase retry flips
 * low-cost candidate bits, where 0 is uncertain and 255 is confident.
 *
 * @param bch_code      Input. An array of 63 bytes, each containing one bit of the NID.
 * @param reliab63      Per-bit reliability for @p bch_code, or NULL for hard-only decode.
 * @param observed_nac  Known 12-bit NAC to use for retry, or 0 when unavailable.
 * @param parity         The 64th parity bit read from the air interface.
 * @param parity_reliab  Reliability of the final parity bit.
 * @return Typed status, numeric NAC/DUID, and BCH correction count. NAC and
 *         DUID are valid when status is positive; error_count is zero on failure.
 */
struct p25p1_nid_result p25p1_nid_decode(const char bch_code[63], const uint8_t reliab63[63], int observed_nac,
                                         unsigned char parity, uint8_t parity_reliab);

#ifdef __cplusplus
}
#endif

#endif // P25P1_CHECK_NID_H_3af071e917ea43fdb51326e2cbfbde0a
