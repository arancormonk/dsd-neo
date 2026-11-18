// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file
 * @brief Private NXDN decode helpers shared by protocol sources and focused tests.
 */

#ifndef DSD_NEO_SRC_PROTOCOL_NXDN_NXDN_INTERNAL_H_
#define DSD_NEO_SRC_PROTOCOL_NXDN_NXDN_INTERNAL_H_

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/synctype_ids.h>

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
int nxdn_sacch_part_of_frame(uint8_t sf);
int nxdn_ran_from_trellis(const uint8_t* trellis_buf);
void nxdn_reset_payload_seed_if_forced(dsd_state* state);
/* Forget a data call's header, packet assembly, crypto and LRRP identities (nxdn_data_call.c). */
void nxdn_reset_data_call_state(dsd_state* state);
void nxdn_prepare_sacch_payload_seed(dsd_state* state, int part_of_frame);

/* The epoch of the canonical call on slot 0 (0 with none). */
static inline uint64_t
nxdn_live_call_epoch(const dsd_state* state) {
    uint64_t epoch = 0U;
    uint64_t revision = 0U;
    (void)dsd_call_state_get_revision(state, 0U, &epoch, &revision);
    return epoch;
}

/* An IV an NXDN DES or AES call sent over the air was received (@p iv), for the call slot 0 carries now. */
static inline void
nxdn_note_received_iv(dsd_state* state, uint64_t iv) {
    state->nxdn_rx_iv = iv;
    state->nxdn_rx_iv_epoch = nxdn_live_call_epoch(state);
    state->nxdn_rx_iv_valid = 1U;
}

/* The MI a call of NXDN cipher @p cipher publishes (call state, event history and every output built on them): the IV
   a DES or AES call sent over the air for this call (dsd_state::nxdn_rx_iv, nxdn_note_received_iv()), and 0 until one
   came. Never payload_miN: a followed assignment loads the destination's key there, and the scrambler's LFSR, seeded
   from the key, runs there, neither of which is an MI or goes to any output. 0 for every cipher but DES and AES. */
static inline uint64_t
nxdn_published_mi(const dsd_state* state, uint8_t cipher) {
    if ((cipher != 2U && cipher != 3U) || state->nxdn_rx_iv_valid == 0U
        || state->nxdn_rx_iv_epoch != nxdn_live_call_epoch(state)) {
        return 0U;
    }
    return state->nxdn_rx_iv;
}

/* Whether the canonical call on slot 0 is an NXDN call: the only one an NXDN message may end or give crypto. Another
   protocol's call there is not NXDN's to touch, whatever an NXDN sync read on its carrier says. */
static inline int
nxdn_live_call_is_nxdn(const dsd_state* state) {
    dsd_call_snapshot call;
    return dsd_call_state_get(state, 0U, &call) > 0 && DSD_SYNC_IS_NXDN(call.protocol);
}

/* End the canonical call on slot 0 for NXDN end signalling, an NXDN call only, and sync its event row; returns whether
   a call ended. */
static inline int
nxdn_end_live_call(const dsd_opts* opts, dsd_state* state, dsd_call_end_reason reason) {
    if (!nxdn_live_call_is_nxdn(state) || dsd_call_state_end_ex(state, 0U, 0.0, reason) <= 0) {
        return 0;
    }
    dsd_event_sync_slot((dsd_opts*)opts, state, 0U);
    return 1;
}

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

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_SRC_PROTOCOL_NXDN_NXDN_INTERNAL_H_ */
