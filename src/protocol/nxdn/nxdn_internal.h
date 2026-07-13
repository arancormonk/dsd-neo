// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file
 * @brief Private NXDN decode helpers shared by protocol sources and focused tests.
 */

#ifndef DSD_NEO_SRC_PROTOCOL_NXDN_NXDN_INTERNAL_H_
#define DSD_NEO_SRC_PROTOCOL_NXDN_NXDN_INTERNAL_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void nxdn_depermute_rel(const uint8_t* input, const uint8_t* reliab, size_t len, const uint16_t* perm, uint8_t* deperm,
                        uint8_t* deperm_rel);
void nxdn_depermute_rel_u8(const uint8_t* input, const uint8_t* reliab, size_t len, const uint8_t* perm,
                           uint8_t* deperm, uint8_t* deperm_rel);
void nxdn_depuncture_12_5_rel(const uint8_t* deperm, const uint8_t* deperm_rel, uint8_t* depunc, uint8_t* depunc_rel);
void nxdn_depuncture_16_9_rel(const uint8_t* deperm, const uint8_t* deperm_rel, uint8_t* depunc, uint8_t* depunc_rel);
void nxdn_depuncture_12_group_rel(const uint8_t* deperm, const uint8_t* deperm_rel, size_t groups, uint8_t* depunc,
                                  uint8_t* depunc_rel);

int nxdn_dcr_is_sb0_message_type(uint8_t message_type);
void nxdn_unpack_bytes_msb(const uint8_t* bytes, size_t byte_count, uint8_t* bits);
void nxdn_pack_bits_msb(const uint8_t* bits, size_t byte_count, uint8_t* bytes);
uint16_t nxdn_bits_to_u16(const uint8_t* bits, int len);
int nxdn_sacch_part_of_frame(uint8_t sf);
int nxdn_ran_from_trellis(const uint8_t* trellis_buf);
void nxdn_reset_payload_seed_if_forced(dsd_state* state);
void nxdn_prepare_sacch_payload_seed(dsd_state* state, int part_of_frame);

void nxdn_handle_sacch(dsd_opts* opts, dsd_state* state, const uint8_t* trellis_buf, const uint8_t* m_data, uint8_t crc,
                       uint8_t check);
void nxdn_handle_sacch2(const dsd_opts* opts, dsd_state* state, const uint8_t* trellis_buf, const uint8_t* m_data,
                        uint8_t crc, uint8_t check);
void nxdn_handle_cac(dsd_opts* opts, dsd_state* state, const uint8_t* trellis_buf, const uint8_t* m_data, uint16_t crc);
void nxdn_handle_pich_tch(const dsd_opts* opts, dsd_state* state, const uint8_t* trellis_buf, const uint8_t* m_data,
                          uint16_t crc, uint16_t check, uint8_t lich);
void nxdn_handle_facch2_udch(dsd_opts* opts, dsd_state* state, const uint8_t* trellis_buf, const uint8_t* m_data,
                             uint16_t crc, uint16_t check, uint8_t type);

struct nxdn_facch3_udch2_message {
    uint8_t bits[160];
    uint8_t bytes[24];
    uint16_t crc[2];
    uint16_t check[2];
};

void nxdn_store_facch3_udch2_block(struct nxdn_facch3_udch2_message* message, size_t block, const uint8_t* trellis_buf,
                                   const uint8_t* m_data);
void nxdn_handle_facch3_udch2_soft(dsd_opts* opts, dsd_state* state, const struct nxdn_facch3_udch2_message* message,
                                   uint8_t type);

int nxdn_route_decoded_lich(dsd_opts* opts, dsd_state* state, uint8_t lich, const uint8_t bits[364],
                            const uint8_t reliab[364]);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_SRC_PROTOCOL_NXDN_NXDN_INTERNAL_H_ */
