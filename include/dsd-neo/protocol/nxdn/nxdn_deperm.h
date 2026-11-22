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

/**
 * @brief What a frame's SCCH or superframe SACCH tells the rest of its frame (nxdn_frame()): frame-local, never kept.
 */
typedef struct nxdn_channel_result {
    /** The superframe position + 1 a CRC-verified, valid SCCH or a CRC-verified superframe SACCH segment gave (0: none),
        where the frame places the scrambler register once its signalling chose the cipher and key. */
    uint8_t seed_part;
    /** A CRC-passing SCCH whose content NXDN TS 1-E does not define (nxdn_scch_content_valid()): the Type-D layout it
        names stays with this frame. */
    uint8_t scch_invalid;
} nxdn_channel_result;

/**
 * @brief A frame's 60-bit SCCH or SACCH field (both take the same place and code), depermuted and Viterbi-decoded as
 * one or the other: its 32 trellis bits, the decoded bytes the payload log prints, and its CRC (an SCCH's CRC-7, a
 * SACCH's CRC-6) as computed and as received. Nothing has acted on it.
 */
typedef struct nxdn_field_decode {
    uint8_t trellis[32];
    uint8_t m_data[5];
    uint8_t crc;
    uint8_t check;
} nxdn_field_decode;

/**
 * @brief A FACCH1 half, depermuted and Viterbi-decoded: its 96 trellis bits (80 element bits, the CRC-12 and the tail),
 * the decoded bytes the payload log prints, and its CRC-12 as computed and as received. Nothing has acted on it.
 */
typedef struct nxdn_facch1_decode {
    uint8_t trellis[96];
    uint8_t m_data[12];
    uint16_t crc;
    uint16_t check;
} nxdn_facch1_decode;

void nxdn_descramble_with_seed(uint8_t dibits[], int len, uint16_t seed);

/**
 * @brief Decode a FACCH1 half (FEC and CRC-12) without acting on it: nxdn_frame() reads both halves of a frame before
 * either is committed (nxdn_handle_facch1()).
 */
nxdn_facch1_decode nxdn_facch1_decode_soft(const uint8_t bits[144], const uint8_t reliab[144]);

/**
 * @brief Whether a decoded FACCH1 half says the system is Type-C: a CRC-verified CCH_INFO (0x1A), a Type-C message a
 * Type-D system does not send. A FACCH1's 80 elements always hold a whole CCH_INFO, and never a whole SITE_INFO.
 */
int nxdn_facch1_says_type_c(const nxdn_facch1_decode* facch1);

/** @brief Decode a FACCH1 half and act on it at once (nxdn_facch1_decode_soft(), then nxdn_handle_facch1()). */
void nxdn_deperm_facch_soft(dsd_opts* opts, dsd_state* state, uint8_t bits[144], const uint8_t reliab[144],
                            uint8_t frame);

/** @brief Decode a frame's field as a SACCH (FEC and CRC-6) without acting on it (nxdn_handle_sacch() commits it). */
nxdn_field_decode nxdn_sacch_decode_soft(const uint8_t bits[60], const uint8_t reliab[60]);

/** @brief Decode a frame's field as an SCCH (FEC and CRC-7) without acting on it (nxdn_scch_commit() commits it). */
nxdn_field_decode nxdn_scch_decode_soft(const uint8_t bits[60], const uint8_t reliab[60]);

/**
 * @brief Act on a decoded SCCH: a CRC-passing one with defined content gives the frame its position and its weak and
 * SCCH-streak evidence, and its messages decode (NXDN_decode_scch()).
 */
nxdn_channel_result nxdn_scch_commit(dsd_opts* opts, dsd_state* state, const nxdn_field_decode* scch,
                                     uint8_t direction);

void nxdn_deperm_sacch2_soft(const dsd_opts* opts, dsd_state* state, uint8_t bits[60], const uint8_t reliab[60]);

void nxdn_deperm_pich_tch_soft(const dsd_opts* opts, dsd_state* state, uint8_t bits[144], const uint8_t reliab[144],
                               uint8_t lich);

void nxdn_deperm_facch2_udch_soft(dsd_opts* opts, dsd_state* state, uint8_t bits[348], const uint8_t reliab[348],
                                  uint8_t type);

/** @brief Decode an SCCH and act on it at once (nxdn_scch_decode_soft(), then nxdn_scch_commit()). */
nxdn_channel_result nxdn_deperm_scch_soft(dsd_opts* opts, dsd_state* state, uint8_t bits[60], const uint8_t reliab[60],
                                          uint8_t direction);

void nxdn_deperm_cac_soft(dsd_opts* opts, dsd_state* state, uint8_t bits[300], const uint8_t reliab[300]);

const char* nxdn_message_type_label(uint8_t message_type);
void nxdn_message_type(const dsd_opts* opts, dsd_state* state, uint8_t MessageType);

void NXDN_SACCH_Full_decode(dsd_opts* opts, dsd_state* state);

/**
 * @brief Decode an NXDN element content block and dispatch it by message type.
 *
 * Carries no CRC verdict of its own: callers pass CRC-verified content only. Every channel
 * decoder in `nxdn_deperm.c` checks its own CRC -- SACCH CRC-6, FACCH1 CRC-12, FACCH2/UDCH
 * CRC-15, CAC CRC-16, FACCH3/UDCH2 -- and reports it to `nxdn_confirm_note_evidence()` before
 * calling here, as does `NXDN_SACCH_Full_decode()` for an assembled superframe. The gate lives
 * in those callers; `nxdn_confirm_is_confirmed()` is the frame-level check at the sites that
 * publish a call or refresh a scan hold. The one deliberate exception is the synthesized
 * disconnect in `nxdn_vcall_run_enc_lockout()`, which fabricates a DISC element locally to drive
 * the same teardown a received one would.
 */
void NXDN_Elements_Content_decode(dsd_opts* opts, dsd_state* state, const uint8_t* ElementsContent,
                                  size_t elements_bits);
void NXDN_decode_scch(dsd_opts* opts, dsd_state* state, const uint8_t* Message, uint8_t direction);
void NXDN_decode_VCALL_ASSGN(dsd_opts* opts, dsd_state* state, const uint8_t* Message);

uint8_t crc6(const uint8_t buf[], int len);
uint16_t crc12f(const uint8_t buf[], int len);
uint16_t crc15(const uint8_t buf[], int len);
uint16_t crc16cac(const uint8_t buf[], int len);
uint8_t crc7_scch(const uint8_t bits[], int len);

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
