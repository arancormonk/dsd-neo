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
 * The origins, a network generation and a revision live in the DMR module's state extension
 * (DSD_STATE_EXT_PROTO_DMR_RC) and are saved per trunk-scan target beside dsd_state::dmr_lcn_trust. Decoder thread only.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_DMR_DMR_T3_CHANNELS_H_
#define DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_DMR_DMR_T3_CHANNELS_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

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

/** The origin of @p lpcn's row: NONE for an empty row, FILE for a nonzero row with none recorded. */
dmr_chan_origin_e dmr_t3_chan_origin(const dsd_state* state, uint16_t lpcn);

/** After a channel map import or reload replaced the map: nonzero rows become FILE, the others NONE. */
void dmr_t3_chan_note_map_loaded(dsd_state* state);

/** After the channel map was emptied: every origin is NONE. */
void dmr_t3_chan_note_map_cleared(dsd_state* state);

/**
 * The receiver is on another network (an operator move, or a checked system code whose MODEL or NET differs from the
 * last checked one's; another site of the same network is none): the over-the-air rows (SITE, UNVERIFIED, NEIGHBOUR, ESTIMATE) and their trust bytes are dropped, FILE and USER
 * rows stay, and the network generation bumps. A trip from the control channel to a voice channel and back is none.
 */
void dmr_t3_chan_network_changed(dsd_opts* opts, dsd_state* state);

/** The network generation: bumps on every network change. */
uint32_t dmr_t3_chan_generation(const dsd_state* state);

/** The revision: bumps on any row or origin change. */
uint32_t dmr_t3_chan_revision(const dsd_state* state);

/**
 * One trunk-scan target's channel provenance. syscode is the last checked system code (valid when syscode_valid), the
 * baseline a later checked one is compared against.
 */
typedef struct {
    uint8_t origin[0x1000];
    uint32_t generation;
    uint32_t revision;
    uint16_t syscode;
    uint8_t syscode_valid;
} dmr_t3_chan_target_snapshot;

/** Save the parked target's provenance. */
void dmr_t3_chan_save(const dsd_state* state, dmr_t3_chan_target_snapshot* out);

/**
 * Restore a target's provenance. The revision goes past both the current one and the saved one, so a reader comparing
 * revisions sees the switch.
 */
void dmr_t3_chan_restore(dsd_state* state, const dmr_t3_chan_target_snapshot* in);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_DMR_DMR_T3_CHANNELS_H_ */
