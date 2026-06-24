// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief NXDN deinterleave/depunc/CRC and DCR helper APIs.
 *
 * Declares the NXDN helper APIs implemented in `src/protocol/nxdn/`.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_NXDN_NXDN_DEPERM_H_
#define DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_NXDN_NXDN_DEPERM_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void nxdn_descramble(uint8_t dibits[], int len);
void nxdn_descramble_with_seed(uint8_t dibits[], int len, uint16_t seed);

void nxdn_deperm_facch_soft(dsd_opts* opts, dsd_state* state, uint8_t bits[144], const uint8_t reliab[144],
                            uint8_t frame);

void nxdn_deperm_sacch_soft(dsd_opts* opts, dsd_state* state, uint8_t bits[60], const uint8_t reliab[60]);

void nxdn_deperm_sacch2_soft(const dsd_opts* opts, dsd_state* state, uint8_t bits[60], const uint8_t reliab[60]);

void nxdn_deperm_pich_tch_soft(const dsd_opts* opts, dsd_state* state, uint8_t bits[144], const uint8_t reliab[144],
                               uint8_t lich);

void nxdn_deperm_facch2_udch_soft(dsd_opts* opts, dsd_state* state, uint8_t bits[348], const uint8_t reliab[348],
                                  uint8_t type);

void nxdn_deperm_facch3_udch2_soft(dsd_opts* opts, dsd_state* state, uint8_t bits[288], const uint8_t reliab[288],
                                   uint8_t type);

void nxdn_deperm_scch_soft(dsd_opts* opts, dsd_state* state, uint8_t bits[60], const uint8_t reliab[60],
                           uint8_t direction);

void nxdn_deperm_cac_soft(dsd_opts* opts, dsd_state* state, uint8_t bits[300], const uint8_t reliab[300]);

#ifdef DSD_NEO_TEST_HOOKS
void dsd_neo_nxdn_test_depermute_12_5(const uint8_t input[60], const uint8_t reliab[60], uint8_t deperm[60],
                                      uint8_t deperm_rel[60]);
void dsd_neo_nxdn_test_depermute_16_9(const uint8_t input[144], const uint8_t reliab[144], uint8_t deperm[144],
                                      uint8_t deperm_rel[144]);
void dsd_neo_nxdn_test_depermute_12_25(const uint8_t input[300], const uint8_t reliab[300], uint8_t deperm[300],
                                       uint8_t deperm_rel[300]);
void dsd_neo_nxdn_test_depermute_12_29(const uint8_t input[348], const uint8_t reliab[348], uint8_t deperm[348],
                                       uint8_t deperm_rel[348]);
void dsd_neo_nxdn_test_depuncture_12_5(const uint8_t deperm[60], const uint8_t deperm_rel[60], uint8_t depunc[72],
                                       uint8_t depunc_rel[72]);
void dsd_neo_nxdn_test_depuncture_16_9(const uint8_t deperm[144], const uint8_t deperm_rel[144], uint8_t depunc[192],
                                       uint8_t depunc_rel[192]);
void dsd_neo_nxdn_test_depuncture_12_group(const uint8_t* deperm, const uint8_t* deperm_rel, size_t groups,
                                           uint8_t* depunc, uint8_t* depunc_rel);
int dsd_neo_nxdn_test_dcr_is_sb0_message_type(uint8_t message_type);
void dsd_neo_nxdn_test_unpack_bytes_msb(const uint8_t* bytes, size_t byte_count, uint8_t* bits);
void dsd_neo_nxdn_test_pack_bits_msb(const uint8_t* bits, size_t byte_count, uint8_t* bytes);
uint16_t dsd_neo_nxdn_test_bits_to_u16(const uint8_t* bits, int len);
int dsd_neo_nxdn_test_sacch_part_of_frame(uint8_t sf);
int dsd_neo_nxdn_test_ran_from_trellis(const uint8_t* trellis_buf);
void dsd_neo_nxdn_test_reset_payload_seed_if_forced(dsd_state* state);
void dsd_neo_nxdn_test_prepare_sacch_payload_seed(dsd_state* state, int part_of_frame);
void dsd_neo_nxdn_test_sacch_state_update(dsd_opts* opts, dsd_state* state, const uint8_t trellis_buf[32],
                                          const uint8_t m_data[5], uint8_t crc, uint8_t check);
void dsd_neo_nxdn_test_sacch2_state_update(const dsd_opts* opts, dsd_state* state, const uint8_t trellis_buf[32],
                                           const uint8_t m_data[5], uint8_t crc, uint8_t check);
void dsd_neo_nxdn_test_cac_state_update(dsd_opts* opts, dsd_state* state, const uint8_t trellis_buf[176],
                                        const uint8_t m_data[22], uint16_t crc);
void dsd_neo_nxdn_test_pich_tch_state_update(const dsd_opts* opts, dsd_state* state, const uint8_t trellis_buf[96],
                                             const uint8_t m_data[12], uint16_t crc, uint16_t check, uint8_t lich);
void dsd_neo_nxdn_test_facch2_udch_state_update(dsd_opts* opts, dsd_state* state, const uint8_t trellis_buf[208],
                                                const uint8_t m_data[26], uint16_t crc, uint16_t check, uint8_t type);
void dsd_neo_nxdn_test_facch3_udch2_state_update(dsd_opts* opts, dsd_state* state, const uint8_t bits[160],
                                                 const uint8_t bytes[24], uint16_t crc0, uint16_t check0, uint16_t crc1,
                                                 uint16_t check1, uint8_t type);
void dsd_neo_nxdn_test_facch3_udch2_store_block(uint8_t bits[160], uint8_t bytes[24], size_t block,
                                                const uint8_t trellis_buf[96], const uint8_t m_data[12]);
#endif

const char* nxdn_message_type_label(uint8_t message_type);
void nxdn_message_type(const dsd_opts* opts, dsd_state* state, uint8_t MessageType);

void NXDN_SACCH_Full_decode(dsd_opts* opts, dsd_state* state);
void NXDN_Elements_Content_decode(dsd_opts* opts, dsd_state* state, uint8_t CrcCorrect, const uint8_t* ElementsContent,
                                  size_t elements_bits);
void NXDN_decode_scch(dsd_opts* opts, dsd_state* state, const uint8_t* Message, uint8_t direction);
void NXDN_decode_VCALL_ASSGN(dsd_opts* opts, dsd_state* state, const uint8_t* Message);

int load_i(const uint8_t val[], int len);
uint8_t crc6(const uint8_t buf[], int len);
uint16_t crc12f(const uint8_t buf[], int len);
uint16_t crc15(const uint8_t buf[], int len);
uint16_t crc16cac(const uint8_t buf[], int len);
uint8_t crc7_scch(const uint8_t bits[], int len);
uint32_t nxdn_message_crc32(const uint8_t* input, int len);

/**
 * Read the 7-bit SCCH CRC check field from decoded trellis bits.
 */
uint8_t nxdn_scch_crc7_check_from_trellis(const uint8_t trellis_bits[32]);

/**
 * Read/compute FACCH-family decoded-bit CRC fields after Viterbi offset correction.
 */
uint16_t nxdn_facch_crc12_payload_from_trellis(const uint8_t trellis_bits[96]);
uint16_t nxdn_facch_crc12_check_from_trellis(const uint8_t trellis_bits[96]);

/**
 * Read/compute FACCH2/UDCH decoded-bit CRC fields after Viterbi offset correction.
 */
uint16_t nxdn_facch2_udch_crc15_payload_from_trellis(const uint8_t trellis_bits[208]);
uint16_t nxdn_facch2_udch_crc15_check_from_trellis(const uint8_t trellis_bits[208]);

/**
 * Validate the expected SACCH superframe segment order.
 */
int nxdn_sacch_segment_sequence_is_valid(uint8_t crc_ok, int previous_part_of_frame, int part_of_frame);

/**
 * Decode DCR SB0 Call Sign Memory (CSM) digits from trellis bits.
 *
 * On success, writes `CSM ddddddddd` to @p out.
 *
 * @return 1 when decode succeeds, 0 on invalid input or insufficient output space.
 */
int nxdn_dcr_decode_csm_alias(const uint8_t trellis_bits[96], char* out, size_t out_sz);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_NXDN_NXDN_DEPERM_H_ */
