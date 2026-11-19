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
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/protocol/nxdn/nxdn_deperm.h>

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

/* The MI an NXDN call publishes (call state, snapshots, events, history, the API feed): the IV a CRC-verified VCALL_IV
   carried over the air while the working register still holds it (dsd_state::nxdn_iv_on_air), else 0. The register a
   scrambled call loads from the key, and runs on from there, is never one. */
static inline unsigned long long
nxdn_published_mi(const dsd_state* state) {
    return state->nxdn_iv_on_air != 0U ? state->nxdn_payload_mi : 0ULL;
}

/* The value of SCCH bits [@p start, @p start + @p len), most significant first. */
static inline unsigned
nxdn_scch_field(const uint8_t* bits, size_t start, size_t len) {
    unsigned value = 0U;
    for (size_t i = start; i < start + len; i++) {
        value = (value << 1) | bits[i];
    }
    return value;
}

/* A Type-D site: Site Type 01-11 (00 is reserved, NXDN TS 1-E §6.6.4 Table 6.6-2) and Site Code 1-250 (0 and 251-255 are
   reserved, §6.6.5 Table 6.6-3). The site ID message's evidence and its writes read the same pair. */
static inline int
nxdn_scch_site_valid(unsigned site_type, unsigned site_code) {
    return site_type != 0U && site_code != 0U && site_code <= 250U;
}

/* The fixed codes of the outbound status messages (TS 1-E Figs 6.5-5..6.5-8, Tables 6.6-5/6.6-6): site ID (2041), free
   (2044), halt (2045) and idle (2046) repeater. */
static inline int
nxdn_scch_status_id(unsigned id) {
    return id == 2041U || (id >= 2044U && id <= 2046U);
}

/* An outbound status message's own fields: the site ID message's Site Type (bits 3-4) and Site Code (bits 5-12, Fig
   6.5-8); the free repeater message's Free Repeater 1 and 2 (bits 3-7, 8-12, Fig 6.5-6), 31 being reserved (§6.6.10
   Table 6.6-8). */
static inline int
nxdn_scch_status_fields_valid(const uint8_t* bits, unsigned id) {
    if (id == 2041U) {
        return nxdn_scch_site_valid(nxdn_scch_field(bits, 3U, 2U), nxdn_scch_field(bits, 5U, 8U));
    }
    if (id == 2044U) {
        return nxdn_scch_field(bits, 3U, 5U) != 31U && nxdn_scch_field(bits, 8U, 5U) != 31U;
    }
    return 1;
}

/* An INFO1 (Figs 6.5-3, 6.5-10): its Spare bit clear (D2: the one Spare bit taken into account; every other Spare bit
   is ignored on reception, Table 9.1-1); inbound, Pass Character 31 at bits 8-12 (TS 2-E Table 5.1-12); outbound, Free
   Repeater 1 and 2 at bits 3-7 and 8-12, neither the reserved 31 (Table 6.6-8). */
static inline int
nxdn_scch_info1_valid(const uint8_t* bits, uint8_t direction) {
    if (bits[24] != 0U) {
        return 0;
    }
    if (direction != 1U) {
        return nxdn_scch_field(bits, 8U, 5U) == 31U;
    }
    return nxdn_scch_field(bits, 3U, 5U) != 31U && nxdn_scch_field(bits, 8U, 5U) != 31U;
}

/*
 * Whether an SCCH's 25 bits carry content NXDN TS 1-E defines (§6.5; Table 9.1-1, a reserved value is not valid).
 * Not:
 * - an INFO1 that nxdn_scch_info1_valid() refuses;
 * - an ID the Unit and Group ID tables reserve, 2001-2040 (Tables 6.6-5, 6.6-6), in an INFO2, INFO3 or INFO4;
 * - an INFO3 source naming a status code (2041, 2044-2046) or All Units (2047): a sender is one unit (§6.6.7.2), and
 *   the status codes belong to the outbound status messages only; nor an outbound INFO3's reserved Free Repeater 1 (31,
 *   bits 3-7, Fig 6.5-9);
 * - an INFO4 or INFO2 naming a status code inbound (only the outbound layouts carry those messages, Figs 6.5-5..6.5-8),
 *   or an outbound status message whose own fields are reserved (nxdn_scch_status_fields_valid());
 * - an INFO2, INFO3 or busy INFO4 with the reserved Home Repeater / Prefix 31 (Table 6.6-4; the status messages have no
 *   prefix at bits 8-12).
 * Such an SCCH is printed but is no evidence of a transmission and changes nothing past its frame.
 */
static inline int
nxdn_scch_content_valid(const uint8_t* bits, uint8_t direction) {
    const unsigned sf = nxdn_scch_field(bits, 0U, 2U);
    const unsigned id = nxdn_scch_field(bits, 13U, 11U);
    if (sf == 3U) {
        return nxdn_scch_info1_valid(bits, direction);
    }
    if (id >= 2001U && id <= 2040U) {
        return 0;
    }
    if (sf == 1U) {
        if (nxdn_scch_status_id(id) || id == 2047U || (direction == 1U && nxdn_scch_field(bits, 3U, 5U) == 31U)) {
            return 0;
        }
    } else if (nxdn_scch_status_id(id)) {
        return direction == 1U && nxdn_scch_status_fields_valid(bits, id);
    }
    return nxdn_scch_field(bits, 8U, 5U) != 31U;
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

/* The receiver lets go of the voice-channel call it followed: the follow, the held call and its policy row. */
static inline void
nxdn_follow_release(dsd_state* state) {
    DSD_MEMSET(&state->nxdn_follow, 0, sizeof(state->nxdn_follow));
    DSD_MEMSET(&state->nxdn_call, 0, sizeof(state->nxdn_call));
    (void)dsd_tg_policy_clear_active_call(state, -1);
}

/* The DISC / end-of-transmission return to the control channel (nxdn_cc_hunt.c): asked, tracked and settled with the
   control-channel hunt's request pattern (nxdn_cc_hunt_note_return(), nxdn_cc_hunt_tick_locked()). */
void nxdn_return_to_cc_request(dsd_opts* opts, dsd_state* state);
void nxdn_return_to_cc_landed(dsd_opts* opts, dsd_state* state);

nxdn_channel_result nxdn_handle_sacch(dsd_opts* opts, dsd_state* state, const uint8_t* trellis_buf,
                                      const uint8_t* m_data, uint8_t crc, uint8_t check);
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
