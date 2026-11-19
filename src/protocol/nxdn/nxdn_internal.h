// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file
 * @brief Private NXDN decode helpers shared by protocol sources and focused tests.
 */

#ifndef DSD_NEO_SRC_PROTOCOL_NXDN_NXDN_INTERNAL_H_
#define DSD_NEO_SRC_PROTOCOL_NXDN_NXDN_INTERNAL_H_

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/nxdn_received_iv.h>
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

/* Whether the canonical call on slot 0 is an NXDN call: the only one an NXDN message may end or give crypto. Another
   protocol's call there is not NXDN's to touch, whatever an NXDN sync read on its carrier says. */
static inline int
nxdn_live_call_is_nxdn(const dsd_state* state) {
    dsd_call_snapshot call;
    return dsd_call_state_get(state, 0U, &call) > 0 && DSD_SYNC_IS_NXDN(call.protocol);
}

/* Record an IV a CRC-verified VCALL_IV carried: for the active NXDN call's transmission, or, with no call open yet
   (late entry), for the call that opens next (nxdn_live_call_received_mi()). */
static inline void
nxdn_received_iv_note(dsd_state* state, unsigned long long iv) {
    state->nxdn_rx_iv = iv;
    state->nxdn_rx_iv_valid = 1U;
    state->nxdn_rx_iv_transmission = dsd_nxdn_active_call_transmission(state);
}

/* The IV the active NXDN call on slot 0 received over the air (<dsd-neo/core/nxdn_received_iv.h>), 0 when none: the
   only MI an NXDN crypto publication carries. One received just before the call opened binds to it here. The
   decoder's working register (dsd_state::payload_miN) is never one: on a scrambled call it is loaded from the key. */
static inline unsigned long long
nxdn_live_call_received_mi(dsd_state* state) {
    unsigned long long iv = 0ULL;
    if (!dsd_nxdn_received_iv_for_active_call(state, &iv)) {
        return 0ULL;
    }
    if (state->nxdn_rx_iv_transmission == 0U) {
        state->nxdn_rx_iv_transmission = dsd_nxdn_active_call_transmission(state);
    }
    return iv;
}

/* The active call moved from transmission @p before to @p after while staying the same call (an INFO3 source update,
   a target message naming the call's own target): its received IV follows it. Another call's never does. */
static inline void
nxdn_received_iv_follow(dsd_state* state, uint64_t before, uint64_t after) {
    if (state->nxdn_rx_iv_valid != 0U && before != 0U && state->nxdn_rx_iv_transmission == before) {
        state->nxdn_rx_iv_transmission = after;
    }
}

/* Whether an SCCH's 25 bits carry content NXDN TS 1-E defines (§6.5; Table 9.1-1): not an INFO2, INFO3 or busy INFO4
   with the reserved prefix 31 (Table 6.6-4), not an INFO1 with its Spare bit set. Such an SCCH is printed but is no
   evidence of a transmission and changes nothing past its frame. The outbound status messages (idle, halt, free
   repeater, site ID: IDs 2041, 2044-2046 in an outbound INFO4 or INFO2) have no prefix at bits 8-12. */
static inline int
nxdn_scch_content_valid(const uint8_t* bits, uint8_t direction) {
    const unsigned sf = ((unsigned)bits[0] << 1) | bits[1];
    unsigned prefix = 0U;
    unsigned id = 0U;
    for (size_t i = 8U; i < 13U; i++) {
        prefix = (prefix << 1) | bits[i];
    }
    for (size_t i = 13U; i < 24U; i++) {
        id = (id << 1) | bits[i];
    }
    if (sf == 3U) {
        return bits[24] == 0U;
    }
    const int status =
        direction == 1U && (sf == 0U || sf == 2U) && (id == 2041U || id == 2044U || id == 2045U || id == 2046U);
    return status || prefix != 31U;
}

/* End the canonical call on slot 0 for NXDN end signalling, an NXDN call only, and sync its event row; returns whether
   a call ended. */
static inline int
nxdn_end_live_call(const dsd_opts* opts, dsd_state* state, dsd_call_end_reason reason) {
    if (!nxdn_live_call_is_nxdn(state) || dsd_call_state_end_ex(state, 0U, 0.0, reason) <= 0) {
        return 0;
    }
    dsd_nxdn_received_iv_clear(state); /* the IV was this transmission's */
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
