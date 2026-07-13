// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief DMR protocol decode entrypoints.
 *
 * Declares the DMR handlers implemented in `src/protocol/dmr/`.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_DMR_DMR_H_H
#define DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_DMR_DMR_H_H

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void dmrBS(dsd_opts* opts, dsd_state* state);
void dmrMS(dsd_opts* opts, dsd_state* state);
void dmrBSBootstrap(dsd_opts* opts, dsd_state* state);
void dmrMSBootstrap(dsd_opts* opts, dsd_state* state);
void dmrMSData(dsd_opts* opts, dsd_state* state);

void dmr_data_sync(dsd_opts* opts, dsd_state* state);
void dmr_data_burst_handler(dsd_opts* opts, dsd_state* state, uint8_t info[196], uint8_t databurst,
                            const uint8_t* reliab98);

void dmr_pi(dsd_opts* opts, dsd_state* state, uint8_t PI_BYTE[], uint32_t CRCCorrect, uint32_t IrrecoverableErrors);
void dmr_flco(dsd_opts* opts, dsd_state* state, uint8_t lc_bits[], uint32_t CRCCorrect, uint32_t* IrrecoverableErrors,
              uint8_t type);
uint8_t dmr_cach(dsd_opts* opts, dsd_state* state, uint8_t cach_bits[25]);

void dmr_cspdu(dsd_opts* opts, dsd_state* state, uint8_t cs_pdu_bits[], uint8_t cs_pdu[], uint32_t CRCCorrect,
               uint32_t IrrecoverableErrors);

void dmr_dheader(dsd_opts* opts, dsd_state* state, uint8_t dheader[], uint8_t dheader_bits[], uint32_t CRCCorrect,
                 uint32_t IrrecoverableErrors);
void dmr_block_assembler(dsd_opts* opts, dsd_state* state, uint8_t block_bytes[], uint8_t block_len, uint8_t databurst,
                         uint8_t type);
void dmr_reset_blocks(dsd_opts* opts, dsd_state* state);
void dmr_confidence_reset(dsd_state* state);
size_t dmr_debug_format_burst(char* out, size_t out_size, const dsd_state* state, uint8_t slot_index,
                              uint8_t burst_type);
size_t dmr_debug_format_burst_payload(char* out, size_t out_size, const int payload[144], uint8_t slot_index,
                                      uint8_t burst_type);
void dmr_debug_dump_burst(const dsd_opts* opts, const dsd_state* state, uint8_t slot_index, uint8_t burst_type);

void dmr_sd_pdu(dsd_opts* opts, dsd_state* state, uint16_t len, const uint8_t* DMR_PDU);
void dmr_udp_comp_pdu(dsd_opts* opts, dsd_state* state, uint16_t len, const uint8_t* DMR_PDU);
void dmr_lrrp(const dsd_opts* opts, dsd_state* state, uint16_t len, uint32_t source, uint32_t dest,
              const uint8_t* DMR_PDU, uint8_t pdu_crc_ok);
void dmr_locn(const dsd_opts* opts, dsd_state* state, uint16_t len, const uint8_t* DMR_PDU);

uint32_t dmr_34(const uint8_t* input, uint8_t treturn[18]);

void dmr_alg_reset(dsd_opts* opts, dsd_state* state);
void dmr_alg_refresh(dsd_opts* opts, dsd_state* state);
void dmr_refresh_algids_on_error(dsd_opts* opts, dsd_state* state);
void dmr_late_entry_mi_fragment(dsd_opts* opts, dsd_state* state, uint8_t vc, uint8_t ambe_fr[4][24],
                                uint8_t ambe_fr2[4][24], uint8_t ambe_fr3[4][24]);
void dmr_late_entry_mi(dsd_opts* opts, dsd_state* state);
void dmr_sbrc(const dsd_opts* opts, dsd_state* state, uint8_t power);
void LFSR(dsd_state* state);
void LFSR64(dsd_state* state);
void LFSR128d(dsd_state* state);
void hytera_enhanced_alg_refresh(dsd_state* state);
uint32_t kirisun_lfsr(unsigned long long int mi);

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_DMR_DMR_H_H */
