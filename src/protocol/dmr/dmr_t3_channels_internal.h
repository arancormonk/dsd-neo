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

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

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
    /* The last system code a checked C_ALOHA named, the baseline of a network change (its MODEL and NET). */
    uint16_t syscode;
    uint8_t syscode_valid;
} dmr_ext_t;

/* The extension, or NULL when nothing allocated it yet. */
dmr_ext_t* dmr_ext_get(const dsd_state* state);

/* The extension, allocated zeroed on first use; NULL when out of memory. */
dmr_ext_t* dmr_ext_get_or_create(dsd_state* state);

/* Whether @p origin (a dmr_chan_origin_e) is one the air taught: SITE, UNVERIFIED, NEIGHBOUR or ESTIMATE, the only ones
 * the writer takes and a network change drops. */
int dmr_t3_chan_origin_is_ota(unsigned origin);

/* Empty the LPCN 1..4094 rows whose recorded origin @p drop accepts, with their trust bytes and origins. Returns whether
 * any row went. */
int dmr_t3_chan_drop_rows(dsd_state* state, dmr_ext_t* ext, int (*drop)(unsigned origin));

/* A checked C_ALOHA named @p syscode for the control channel the receiver is on: a code whose MODEL or NET differs from
 * the last checked one's is a network change (dmr_t3_chan_network_changed()); another SITE of the same network is
 * none. */
void dmr_t3_chan_note_checked_syscode(dsd_state* state, uint16_t syscode);

#endif /* DSD_NEO_SRC_PROTOCOL_DMR_DMR_T3_CHANNELS_INTERNAL_H_ */
