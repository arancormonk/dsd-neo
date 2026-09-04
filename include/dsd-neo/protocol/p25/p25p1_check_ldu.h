// SPDX-License-Identifier: ISC
/**
 * @file
 * @brief P25 Phase 1 LDU validation helpers.
 */
#ifndef P25P1_CHECK_LDU_H_c1734445a67e47caa25673d7a4ce7520
#define P25P1_CHECK_LDU_H_c1734445a67e47caa25673d7a4ce7520

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Attempts to correct 12 hex words using the Reed-Solomon(24,12,13) FEC.
 * \param data The packed hex words, each of 6 chars, one after the other.
 * \param parity The corresponding 12 hex words with the parity information.
 * \return 1 if irrecoverable errors have been detected, 0 otherwise.
 */
int check_and_fix_reedsolomon_24_12_13(char* data, const char* parity);

/**
 * Attempts to correct 12 hex words using Reed-Solomon(24,12,13) plus erasure positions.
 * \param data The packed 12 data hex words, corrected in place on success.
 * \param parity The corresponding 12 parity hex words.
 * \param erasures RS codeword positions, 0-11 parity and 12-23 data.
 * \param n_erasures Number of erasure positions, max 12.
 * \return 1 if irrecoverable errors have been detected, 0 otherwise.
 */
int check_and_fix_reedsolomon_24_12_13_soft(char* data, const char* parity, const int* erasures, int n_erasures);

/**
 * Attempts to correct 16 hex words using the Reed-Solomon(24,16,9) FEC.
 * \param data The packed 16 hex words, each of 6 chars, one after the other.
 * \param parity The corresponding 8 hex words with the parity information.
 * \return 1 if irrecoverable errors have been detected, 0 otherwise.
 */
int check_and_fix_reedsolomon_24_16_9(char* data, const char* parity);

/**
 * Attempts to correct 16 hex words using Reed-Solomon(24,16,9) plus erasure positions.
 * \param data The packed 16 data hex words, corrected in place on success.
 * \param parity The corresponding 8 parity hex words.
 * \param erasures RS codeword positions, 0-7 parity and 8-23 data.
 * \param n_erasures Number of erasure positions, max 8.
 * \return 1 if irrecoverable errors have been detected, 0 otherwise.
 */
int check_and_fix_reedsolomon_24_16_9_soft(char* data, const char* parity, const int* erasures, int n_erasures);

#ifdef __cplusplus
}
#endif

#endif // P25P1_CHECK_LDU_H_c1734445a67e47caa25673d7a4ce7520
