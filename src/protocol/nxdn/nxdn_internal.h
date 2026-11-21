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

/* How long a checked data call header stays bound to the assembly it opens, on the decode clock: a whole data call
   (a header and at most 16 blocks, one 80 ms frame each) fits with room to spare (issue #800 B6). */
enum { NXDN_DATA_HEADER_HOLD_MS = 2000 };

/* Bind the checked data call header just accepted to the assembly it opens: the reception, carrier and time. */
void nxdn_data_call_bind_header(const dsd_opts* opts, dsd_state* state);
/* 1 while a header is valid and still bound: same reception and carrier, within NXDN_DATA_HEADER_HOLD_MS. */
int nxdn_data_call_header_bound(const dsd_state* state);
/* Drop the header and its binding (a failed channel CRC, a rejected header): no payload assembles under it. */
void nxdn_data_call_drop_header(dsd_state* state);
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
   Table 6.6-8). Returns the field at fault, or NULL. */
static inline const char*
nxdn_scch_status_fields_problem(const uint8_t* bits, unsigned id) {
    if (id == 2041U) {
        if (nxdn_scch_field(bits, 3U, 2U) == 0U) {
            return "Site Type reserved";
        }
        const unsigned site_code = nxdn_scch_field(bits, 5U, 8U);
        return site_code != 0U && site_code <= 250U ? NULL : "Site Code reserved";
    }
    if (id == 2044U) {
        if (nxdn_scch_field(bits, 3U, 5U) == 31U) {
            return "Free Repeater 1 reserved";
        }
        return nxdn_scch_field(bits, 8U, 5U) == 31U ? "Free Repeater 2 reserved" : NULL;
    }
    return NULL;
}

/* An INFO1 (Figs 6.5-3, 6.5-10): inbound, Repeater in Use 1-30 at bits 3-7 (Table 6.6-9: 0 is the outbound
   registration command, 31 an INFO4 or INFO2 end of transmission, never a repeater the sender is on) and Pass Character
   31 at bits 8-12 (TS 2-E Table 5.1-12); outbound, Free Repeater 1 and 2 at bits 3-7 and 8-12, neither the reserved 31
   (Table 6.6-8). Its Spare bit is ignored here as every Spare bit is (Table 9.1-1): the frame's structure and position
   stand, and only the Call Option, Cipher Type and Key ID of an INFO1 with it set are not taken (D2,
   nxdn_scch_handle_info1()). Returns the field at fault, or NULL. */
static inline const char*
nxdn_scch_info1_problem(const uint8_t* bits, uint8_t direction) {
    if (direction != 1U) {
        const unsigned repeater = nxdn_scch_field(bits, 3U, 5U);
        if (repeater == 0U || repeater == 31U) {
            return "Repeater in Use not a repeater";
        }
        return nxdn_scch_field(bits, 8U, 5U) == 31U ? NULL : "Pass Character not 31";
    }
    if (nxdn_scch_field(bits, 3U, 5U) == 31U) {
        return "Free Repeater 1 reserved";
    }
    return nxdn_scch_field(bits, 8U, 5U) == 31U ? "Free Repeater 2 reserved" : NULL;
}

/* The ID of an INFO2, INFO3 or busy INFO4, which carries a Home Repeater / Prefix (bits 8-12): not one the Unit and
   Group ID tables reserve, 2001-2040 (Tables 6.6-5, 6.6-6); the special IDs 2042 and 2043 (interconnect, a session with
   the TC) under prefix 0 only (§6.6.7, §6.6.8: "The Prefix of the Unit which has a Special ID is 0"); and for an INFO3,
   a sender, one unit (§6.6.7.2): never a status code (2041, 2044-2046) or All Units (2047). Returns the fault, or
   NULL. */
static inline const char*
nxdn_scch_id_problem(unsigned sf, unsigned id, unsigned prefix) {
    if (id >= 2001U && id <= 2040U) {
        return "ID reserved";
    }
    if ((id == 2042U || id == 2043U) && prefix != 0U) {
        return "special ID under a nonzero prefix";
    }
    if (sf == 1U && (nxdn_scch_status_id(id) || id == 2047U)) {
        return "ID not a sender";
    }
    return NULL;
}

/*
 * What content of an SCCH's 25 bits NXDN TS 1-E does not define (§6.5; Table 9.1-1: a reserved value is not valid), the
 * field at fault named, or NULL when it is all defined:
 * - an INFO1 that nxdn_scch_info1_problem() refuses;
 * - an inbound INFO4, INFO2 or INFO3 whose Repeater in Use (bits 3-7) is 0, or an inbound INFO3's 31: no repeater
 *   (Table 6.6-9; 31 in an INFO4 or INFO2 is the end of transmission);
 * - an INFO4 or INFO2 naming a status code inbound (only the outbound layouts carry those messages, Figs 6.5-5..6.5-8),
 *   or an outbound status message whose own fields are reserved (nxdn_scch_status_fields_problem());
 * - the ID of an INFO2, INFO3 or busy INFO4 that nxdn_scch_id_problem() refuses;
 * - an outbound INFO3's reserved Free Repeater 1 (31, bits 3-7, Fig 6.5-9);
 * - an INFO2, INFO3 or busy INFO4 with the reserved Home Repeater / Prefix 31 (Table 6.6-4; the status messages have no
 *   prefix at bits 8-12).
 * Such an SCCH is printed but is no evidence of a transmission and changes nothing past its frame.
 */
static inline const char*
nxdn_scch_content_problem(const uint8_t* bits, uint8_t direction) {
    const unsigned sf = nxdn_scch_field(bits, 0U, 2U);
    const unsigned id = nxdn_scch_field(bits, 13U, 11U);
    const unsigned prefix = nxdn_scch_field(bits, 8U, 5U);
    if (sf == 3U) {
        return nxdn_scch_info1_problem(bits, direction);
    }
    if (sf != 1U && nxdn_scch_status_id(id)) {
        return direction == 1U ? nxdn_scch_status_fields_problem(bits, id) : "status ID inbound";
    }
    const char* id_problem = nxdn_scch_id_problem(sf, id, prefix);
    if (id_problem != NULL) {
        return id_problem;
    }
    if (sf == 1U && direction == 1U && nxdn_scch_field(bits, 3U, 5U) == 31U) {
        return "Free Repeater 1 reserved";
    }
    if (direction != 1U) {
        /* Repeater in Use (bits 3-7, TS 1-E §6.6.11 Table 6.6-9): 0 is the outbound registration command, no repeater
           an inbound sender is on; 31 is the end of transmission, which only an INFO4 or INFO2 carries (TS 2-E Table
           5.1-5), so an inbound INFO3's 31 is no repeater either. */
        const unsigned repeater = nxdn_scch_field(bits, 3U, 5U);
        if (repeater == 0U || (sf == 1U && repeater == 31U)) {
            return "Repeater in Use not a repeater";
        }
    }
    return prefix == 31U ? "Prefix 31 reserved" : NULL;
}

/* Whether an SCCH's 25 bits carry content NXDN TS 1-E defines (nxdn_scch_content_problem()). */
static inline int
nxdn_scch_content_valid(const uint8_t* bits, uint8_t direction) {
    return nxdn_scch_content_problem(bits, direction) == NULL;
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

nxdn_channel_result nxdn_handle_sacch(dsd_opts* opts, dsd_state* state, const uint8_t* trellis_buf,
                                      const uint8_t* m_data, uint8_t crc, uint8_t check);
void nxdn_handle_sacch2(const dsd_opts* opts, dsd_state* state, const uint8_t* trellis_buf, const uint8_t* m_data,
                        uint8_t crc, uint8_t check);
void nxdn_handle_cac(dsd_opts* opts, dsd_state* state, const uint8_t* trellis_buf, const uint8_t* m_data, uint16_t crc);
void nxdn_handle_pich_tch(const dsd_opts* opts, dsd_state* state, const uint8_t* trellis_buf, const uint8_t* m_data,
                          uint16_t crc, uint16_t check, uint8_t lich);
void nxdn_handle_facch1(dsd_opts* opts, dsd_state* state, const uint8_t* trellis_buf, const uint8_t* m_data,
                        uint16_t crc, uint16_t check, uint8_t frame);
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

/* The call kind a Call Type (NXDN TS 1-A §6.5.12 Table 6.5-8; Type-D TS 1-E §6.6.21 Table 6.6-15) gives a call: 000
   Broadcast and 001 Conference are group calls; 100 Individual, 110 Interconnect and 111 Speed Dial are private (their
   destination is a radio or a PSTN ID, TS 1-A Table 6.4-6); 010 Unspecified, 011 (reserved on Type-C, a Session Call
   with the trunk controller on Type-D) and 101 (reserved) are generic voice, of neither class. The result does not
   depend on @p type_d (a Type-D call): the two tables classify alike, and only the label of 011 differs by variant.
   VCALL, VCALL_ASSGN, TX_REL, the encryption lockout, the held-call reconcile, the conventional scan hold and the
   data call headers all classify by it. */
dsd_call_kind nxdn_call_type_kind(uint8_t call_type, int type_d);
/* Whether a message of @p call_type may open a call: every type but 010, which only a TX_REL carries. */
int nxdn_call_type_opens_call(uint8_t call_type);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_SRC_PROTOCOL_NXDN_NXDN_INTERNAL_H_ */
