// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The DMR module's state extension (DSD_STATE_EXT_PROTO_DMR_RC): one allocation that holds the Reverse Channel notice
 * dedupe (dmr_rc_notify.c) and the Tier III channel provenance (dmr_t3_channels.c, <dsd-neo/protocol/dmr/
 * dmr_t3_channels.h>). Protocol-internal; decoder thread only.
 */

#ifndef DSD_NEO_SRC_PROTOCOL_DMR_DMR_T3_CHANNELS_INTERNAL_H_
#define DSD_NEO_SRC_PROTOCOL_DMR_DMR_T3_CHANNELS_INTERNAL_H_

#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/protocol/dmr/dmr_t3_channels.h>

#include <stdint.h>
#include <time.h>

enum { DMR_RC_NOTIFY_KEYS = 3 };

/* The highest Tier III channel number (LPCN 1..4094). */
enum { DMR_T3_MAX_LCN = 4094 };

/* Per-key repeat suppression for RC event rows. Keys 0/1 are the embedded SB/RC path per slot index, key 2 the
 * standalone burst; keeping them apart stops an embedded repeat train from masking a standalone burst and vice
 * versa. */
typedef struct {
    uint8_t last_cmd[DMR_RC_NOTIFY_KEYS];
    uint8_t valid[DMR_RC_NOTIFY_KEYS];
    time_t last_time[DMR_RC_NOTIFY_KEYS];
} dmr_rc_dedup_t;

typedef struct {
    dmr_rc_dedup_t rc;
    /* dmr_chan_origin_e per LPCN; a nonzero row with NONE reads as FILE. */
    uint8_t origin[0x1000];
    uint32_t generation;
    uint32_t revision;
    /* The last system code a checked C_ALOHA, standard C_BCAST or SYS_Parms short LC named, the baseline of a network
       change (its MODEL and NET). */
    uint16_t syscode;
    uint8_t syscode_valid;
    /* The channels granted without a frequency (dmr_t3_missing_note_grant()). */
    dmr_t3_missing_ledger missing;
    /* The user's channels over the map (dmr_t3_overlay.c). */
    dmr_t3_overlay overlay;
    /* A message named the site Tier III this session (dmr_t3_note_tier3()): a checked one (tier3), or one -F accepted
       as RAS (tier3_unchecked, display only); saved per trunk-scan target. */
    uint8_t tier3;
    uint8_t tier3_unchecked;
    /* The last checked TSCC evidence on the carrier the receiver was on (dmr_t3_cc_note_evidence()): its frequency and
       dsd_state::carrier_seq then. Valid only while that count holds; never saved per target. */
    uint8_t cc_evidence;
    uint32_t cc_evidence_carrier;
    long cc_evidence_hz;
} dmr_ext_t;

/* The extension, or NULL when nothing allocated it yet. */
dmr_ext_t* dmr_ext_get(const dsd_state* state);

/* The extension, allocated zeroed on first use; NULL when out of memory. */
dmr_ext_t* dmr_ext_get_or_create(dsd_state* state);

/* Whether @p origin (a dmr_chan_origin_e) is one the air taught: SITE, UNVERIFIED, NEIGHBOUR or ESTIMATE, the only ones
 * the writer takes and a network change drops. */
int dmr_t3_chan_origin_is_ota(unsigned origin);

/* Empty the LPCN 1..4094 rows whose recorded origin @p drop accepts, with their trust bytes and origins, and the
 * overlay bases of that origin (RESTORE then returns no frequency). Returns whether any row went. */
int dmr_t3_chan_drop_rows(dsd_state* state, dmr_ext_t* ext, int (*drop)(unsigned origin));

/* An anchor of the heuristic fill appeared, moved or went: every estimate goes and the fill runs again (with
 * dsd_opts::dmr_t3_heuristic_fill on; @p opts may be NULL). */
void dmr_t3_chan_anchors_changed(dsd_opts* opts, dsd_state* state, dmr_ext_t* ext);

/* Whether @p lpcn is hidden by the user (an overlay HIDE entry). */
int dmr_t3_overlay_hidden(const dmr_ext_t* ext, uint16_t lpcn);

/* Keep the live row as overlay entry @p l's base (its frequency, origin and trust); called with the row not
 * overlaid. */
void dmr_t3_overlay_take_base(const dsd_state* state, dmr_ext_t* ext, uint16_t l);

/* Lay overlay entry @p l over the live row: the user's frequency, or none for a hidden channel, as USER, trust 0. */
void dmr_t3_overlay_write_live(dsd_state* state, dmr_ext_t* ext, uint16_t l);

/* Row @p lpcn gained a frequency: it leaves the missing-channel ledger (the one-row dmr_t3_missing_reconcile()). */
void dmr_t3_missing_forget(dmr_ext_t* ext, uint16_t lpcn);

/* A checked C_ALOHA or standard C_BCAST, or a C_SYS_Parms or P_SYS_Parms short LC, named @p syscode for the carrier
 * the receiver is on: a code whose MODEL or NET differs from the last checked one's is a network change
 * (dmr_t3_chan_network_changed()); another SITE of the same network is none. */
void dmr_t3_chan_note_checked_syscode(dsd_state* state, uint16_t syscode);

#endif /* DSD_NEO_SRC_PROTOCOL_DMR_DMR_T3_CHANNELS_INTERNAL_H_ */
