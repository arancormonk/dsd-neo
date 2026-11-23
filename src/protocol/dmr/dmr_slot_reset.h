// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* One DMR slot's call state, reset slot by slot: the pieces a terminator clears, the slot's data header and packet
   assembly, and the retirement that runs all of them for a slot whose assignment ended while its companion slot keeps
   the receiver (dmr_slot_retire()). The other slot is never touched. */
#ifndef DSD_NEO_PROTOCOL_DMR_SLOT_RESET_H
#define DSD_NEO_PROTOCOL_DMR_SLOT_RESET_H

#include <dsd-neo/core/state.h>
#include <stdint.h>

#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

/* The per-slot live crypto fields the vocoder decrypts and gates with, mapped once so the terminator reset, the
   retirement and the heal stash/restore (dmr_flco.c) all walk the same list. A field added to the stash but missed by a
   reset (or the reverse) would be stashed and healed but never cleared, leaking one call's crypto into the next. */
typedef struct {
    unsigned int* fid;
    unsigned int* so;
    int* algid;
    int* keyid;
    unsigned long long int* mi;
} dmr_slot_crypto;

/* The live crypto fields of slot @p slot (0: TS1, else TS2). */
dmr_slot_crypto dmr_slot_crypto_fields(dsd_state* state, uint8_t slot);

/* The crypto half of a terminator's reset of slot @p slot: its FID, service options, ALG ID, key ID and MI, its
   encryption class, vendor late-entry mode, held pre-call PI and its superframe MI chain (issue #729). */
void dmr_slot_reset_crypto(dsd_state* state, uint8_t slot);

/* The metadata half of a terminator's reset of slot @p slot: its partially assembled talker alias, its dmr_pdu_sf
   superframe scratch and its embedded and LRRP GPS strings. */
void dmr_slot_reset_metadata(dsd_state* state, uint8_t slot);

/* Slot @p slot's share of dmr_reset_blocks(): its data header (disarmed, format 7), confirmed-data and proprietary-header
   flags, block counters, padding and keystream offsets, collected-block CRC verdicts, sequence numbers and the packet's
   addressing. The carrier's CACH fragments and the Capacity Plus CSBK assembly are not the slot's call's and stay.
   Inline, so the fixtures that compile dmr_block.c on its own need nothing more. */
static inline void
dmr_slot_reset_blocks(dsd_state* state, uint8_t slot) {
    const uint8_t idx = slot != 0U ? 1U : 0U;
    state->data_p_head[idx] = 0;
    state->data_conf_data[idx] = 0;
    DSD_MEMSET(state->dmr_pdu_sf[idx], 0, sizeof(state->dmr_pdu_sf[idx]));
    state->data_block_counter[idx] = 1;
    state->data_block_poc[idx] = 0;
    state->data_header_dd_format[idx] = 0;
    state->data_header_bit_padding[idx] = 0;
    state->data_byte_ctr[idx] = 0;
    state->udt_uab_reserved[idx] = 0;
    state->data_ks_start[idx] = 0;
    state->data_header_blocks[idx] = 1;
    DSD_MEMSET(state->data_block_crc_valid[idx], 0, sizeof(state->data_block_crc_valid[idx]));
    state->dmr_lrrp_source[idx] = 0;
    state->dmr_lrrp_target[idx] = 0;
    // Qualifies dmr_lrrp_target, so it clears with it -- same reason as dsd_init.c and
    // no_carrier_reset_dmr_data_blocks(). A stale group flag left behind a cleared target would
    // qualify whatever target is written next, including one a non-DMR protocol wrote.
    state->dmr_data_target_is_group[idx] = 0;
    state->dmr_data_checked_group[idx] = 0;
    state->data_header_valid[idx] = 0;
    state->data_header_crc_invalid[idx] = 0;
    state->data_header_format[idx] = 7;
    state->data_header_sap[idx] = 0;
    state->data_dbsn_expected[idx] = 0;
    state->data_dbsn_have[idx] = 0;
    DSD_SNPRINTF(state->dmr_lrrp_gps[idx], sizeof(state->dmr_lrrp_gps[idx]), "%s", "");
}

/* Slot @p slot's assignment ended (a grant there named another call), and the receiver may stay on the carrier for the
   companion slot's call, which is not touched: the slot is left as a fresh call there finds it. Its crypto and metadata as a verified
   terminator leaves them (no heal can restore an assignment the system ended, so the heal stash goes too), the
   talkgroup its key autoload keyed from, its derived DES IV, its data header and packet assembly, its pending embedded
   LC with its voice gate (closed until a fresh superframe's voice sync, so no LC is dispatched from the retired call's
   fragments), and its voice held until its next call says how it is encrypted (dmr_enc_class_hold_slot()). Call after the slot's call has ended, so its
   final row keeps the FID its vendor annotations render by. */
void dmr_slot_retire(dsd_state* state, uint8_t slot);

#endif
