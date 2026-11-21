// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * DMR Tier III channel frequencies: where each row of the channel map (dsd_state::trunk_chan_map, LPCN 1..4094) came
 * from, and the one writer every over-the-air frequency goes through.
 *
 * A row's origin decides what may replace it. A row from a channel map file (FILE) or typed by the user (USER) is never
 * overwritten over the air. A frequency a checked CSBK or MBC names (SITE) replaces a RAS-accepted one (UNVERIFIED), a
 * neighbour site's (NEIGHBOUR) and a heuristic estimate (ESTIMATE) always, and another SITE one when the frequency
 * differs. UNVERIFIED, NEIGHBOUR and ESTIMATE only fill empty rows. A nonzero row with no recorded origin (a map loaded
 * before any of this was recorded) counts as FILE.
 *
 * A row another writer (an import, another protocol) sets afterwards is no longer the air's: dsd_state_set_trunk_chan_freq()
 * clears its bit in dsd_state::dmr_lcn_ota, and the row then reads as FILE, which DMR neither replaces nor drops.
 *
 * The origins, a network generation and a revision live in the DMR module's state extension
 * (DSD_STATE_EXT_PROTO_DMR_RC); with dsd_state::dmr_lcn_trust and dsd_state::dmr_lcn_ota they are saved per trunk-scan
 * target and per Signal search session. Decoder thread only.
 *
 * Beside them sits the missing-channel ledger: the channels a grant named that have no frequency, so the receiver
 * cannot follow their calls. Only a standard outbound grant that passed its CRC, or that -F accepted as RAS on a
 * RAS-confirmed carrier (then marked unchecked), feeds it, never a synthesized PDU; a grant whose channel has a
 * frequency never reaches it. A map change clears the channels it gave a frequency, a network change empties it, and
 * it is saved with the provenance.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_DMR_DMR_T3_CHANNELS_H_
#define DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_DMR_DMR_T3_CHANNELS_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/protocol/dmr/dmr_t3_grant.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Where a channel map row came from. The values are stored per row and saved per trunk-scan target. */
typedef enum {
    DMR_CHAN_ORIGIN_NONE = 0,
    DMR_CHAN_ORIGIN_FILE = 1,
    DMR_CHAN_ORIGIN_USER = 2,
    DMR_CHAN_ORIGIN_SITE = 3,
    DMR_CHAN_ORIGIN_UNVERIFIED = 4,
    DMR_CHAN_ORIGIN_NEIGHBOUR = 5,
    DMR_CHAN_ORIGIN_ESTIMATE = 6
} dmr_chan_origin_e;

/**
 * The only over-the-air writer of the channel map: record @p freq_hz for @p lpcn (1..4094) as @p origin (SITE,
 * UNVERIFIED, NEIGHBOUR or ESTIMATE; FILE, USER and NONE are refused) under the precedence above. A written row's trust
 * byte (dsd_state::dmr_lcn_trust) is 2 when learned on the control channel and 1 otherwise, NEIGHBOUR always 1 and
 * ESTIMATE 0. A new row or a SITE change posts a notice on the receiving burst's slot with that burst's CRC flags; with
 * the heuristic fill on, a changed anchor drops every estimate and fills again. Returns 1 when the row or its origin
 * changed.
 */
int dmr_t3_chan_learn(dsd_opts* opts, dsd_state* state, uint16_t lpcn, long freq_hz, dmr_chan_origin_e origin);

/** The origin of @p lpcn's row: NONE for an empty row, FILE for a nonzero row with none recorded or one another writer
 * set after the air. */
dmr_chan_origin_e dmr_t3_chan_origin(const dsd_state* state, uint16_t lpcn);

/** After a channel map import or reload replaced the map: nonzero rows become FILE, the others NONE. */
void dmr_t3_chan_note_map_loaded(dsd_state* state);

/** After the channel map was emptied: every origin is NONE. */
void dmr_t3_chan_note_map_cleared(dsd_state* state);

/**
 * The receiver is on another network (an operator move, or a checked system code whose MODEL or NET differs from the
 * last checked one's; another site of the same network is none): the over-the-air rows (SITE, UNVERIFIED, NEIGHBOUR,
 * ESTIMATE) and their trust bytes are dropped, FILE and USER rows stay, the missing-channel ledger empties, and the
 * network generation bumps. A trip from the control channel to a voice channel and back is none.
 */
void dmr_t3_chan_network_changed(dsd_opts* opts, dsd_state* state);

/** The network generation: bumps on every network change. */
uint32_t dmr_t3_chan_generation(const dsd_state* state);

/** The revision: bumps on any row or origin change, another writer taking a row at its frequency included. */
uint32_t dmr_t3_chan_revision(const dsd_state* state);

/**
 * One channel of the ledger with the latest grant that named it: its destination slot (0 or 1), the grant's classes and
 * parties, when it was heard (dsd_decode_now_mono_ms()), and whether no checked grant named the channel yet.
 */
typedef struct {
    uint16_t lpcn;
    uint8_t slot;
    uint8_t group;
    uint8_t data;
    uint8_t unverified;
    uint32_t caller;
    uint32_t recipient;
    uint64_t last_heard_ms;
} dmr_t3_missing_detail;

/**
 * The missing-channel ledger. Bit LPCN & 7 of byte LPCN >> 3 of each set: missing (granted, still no frequency),
 * checked (a checked grant named it), reported (its notice was logged; only a network change clears it). The revision
 * bumps on every change. details holds the detail_count most recent channels, newest first; a channel past them is in
 * the sets only.
 */
typedef struct {
    uint8_t missing[512];
    uint8_t checked[512];
    uint8_t reported[512];
    uint32_t revision;
    uint8_t detail_count;
    dmr_t3_missing_detail details[16];
} dmr_t3_missing_ledger;

/**
 * A grant of class @p cls for @p lpcn (1..4094) on destination slot @p slot named no frequency: record it, and log its
 * notice the first time this network names the channel. @p unverified is nonzero for a grant -F accepted as RAS; the
 * caller judges the provenance and records nothing else. A channel with a frequency is ignored.
 */
void dmr_t3_missing_note_grant(dsd_opts* opts, dsd_state* state, uint16_t lpcn, const dmr_t3_grant_class* cls,
                               uint8_t slot, int unverified);

/** After the channel map changed: the channels that now have a frequency leave the ledger; none join it. */
void dmr_t3_missing_reconcile(const dsd_state* state);

/** How many channels the ledger holds, the ones past its details included. */
size_t dmr_t3_missing_count(const dsd_state* state);

/** Copy the ledger to @p out (all zero when nothing was recorded). */
void dmr_t3_missing_copy(const dsd_state* state, dmr_t3_missing_ledger* out);

/** Log the exit summary of the ledger's channels, if any. */
void dmr_t3_missing_log_summary(const dsd_state* state);

/** Log the exit summary of @p ledger, a trunk-scan target's, naming that target @p target (NULL or empty: none). */
void dmr_t3_missing_log_summary_for(const dmr_t3_missing_ledger* ledger, const char* target);

/**
 * One trunk-scan target's (or Signal search session's) channel provenance: the origins, the trust bytes
 * (dsd_state::dmr_lcn_trust), the rows the air holds (dsd_state::dmr_lcn_ota), the generation and revision, and the missing-channel ledger. syscode is the last
 * checked system code (valid when syscode_valid), the baseline a later checked one is compared against.
 */
typedef struct {
    uint8_t origin[0x1000];
    uint8_t trust[0x1000];
    uint8_t ota[0x1000 / 8];
    uint32_t generation;
    uint32_t revision;
    uint16_t syscode;
    uint8_t syscode_valid;
    dmr_t3_missing_ledger missing;
} dmr_t3_chan_target_snapshot;

/** Save the parked target's provenance and ledger. */
void dmr_t3_chan_save(const dsd_state* state, dmr_t3_chan_target_snapshot* out);

/**
 * Restore a target's provenance and ledger, the map rows being restored beside it. Each revision goes past both the
 * current one and the saved one, so a reader comparing revisions sees the switch. The hunt's candidate list is not part
 * of it.
 */
void dmr_t3_chan_restore(dsd_state* state, const dmr_t3_chan_target_snapshot* in);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_DMR_DMR_T3_CHANNELS_H_ */
