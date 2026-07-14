// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief P25 Phase 1 PDU decoder interfaces.
 *
 * Declares P25 PDU helpers implemented in `src/protocol/p25/phase1/p25p1_pdu_data.c`.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_P25_P25_PDU_H_
#define DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_P25_P25_PDU_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint8_t p25_decrypt_pdu(const dsd_opts* opts, const dsd_state* state, uint8_t* input, uint8_t alg_id, uint16_t key_id,
                        unsigned long long int mi, int len);
uint8_t p25_decode_es_header(const dsd_opts* opts, dsd_state* state, uint8_t* input, uint8_t* sap, int* ptr, int len);
void p25_decode_extended_address(dsd_opts* opts, dsd_state* state, const uint8_t* input, uint8_t* sap, int* ptr);
void p25_decode_pdu_header(dsd_opts* opts, dsd_state* state, const uint8_t* input);
void p25_decode_pdu_data(dsd_opts* opts, dsd_state* state, uint8_t* input, int len);
void p25_decode_rsp(uint8_t C, uint8_t T, uint8_t S, char* rsp_string, size_t rsp_string_size);
void p25_decode_sap(uint8_t SAP, char* sap_string, size_t sap_string_size);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_P25_P25_PDU_H_ */
