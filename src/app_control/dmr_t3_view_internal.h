// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The two snapshot hops of the DMR Tier III channel record (<dsd-neo/app_control/dmr_t3_view.h>), called by
 * ui_snapshot.c under its lock: the decoder's state into the published copy, and the published copy into the
 * consumer's. Each copies the record's body only when its content moved.
 */

#ifndef DSD_NEO_SRC_APP_CONTROL_DMR_T3_VIEW_INTERNAL_H_
#define DSD_NEO_SRC_APP_CONTROL_DMR_T3_VIEW_INTERNAL_H_

#include <dsd-neo/app_control/dmr_t3_view.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The source a part of the body was last copied from: the state instance, the parked trunk-scan target, whether the
 * record was there, the part's revision and (the channel rows only) the channel map's sequence. Zeroed, it holds
 * nothing, so the first copy runs. */
typedef struct {
    uint8_t valid;
    uint8_t present;
    uint64_t instance;
    uint32_t target_id;
    uint32_t revision;
    uint64_t map_seq;
} dsd_app_dmr_t3_view_key;

/* What the published copy was filled from: each part's key (the ledger, and the channel rows: origins, frequencies,
 * overlay rows), and the trunk-scan target the receiver is parked on (target_id moves whenever that id changes). */
typedef struct {
    dsd_app_dmr_t3_view_key missing;
    dsd_app_dmr_t3_view_key rows;
    uint32_t target_id; /* the parked target's id now */
    char target[DSD_CHANNEL_LABEL_SIZE];
} dsd_app_dmr_t3_view_mark;

/* What a hop copied: the ledger, the channel rows, both or neither. */
enum {
    DSD_APP_DMR_T3_VIEW_COPIED_MISSING = 1,
    DSD_APP_DMR_T3_VIEW_COPIED_ROWS = 2,
};

/* Fill @p dst from the decoder's @p src (decoder thread): the scalars every time, the ledger and the channel rows
 * each only when its key in @p mark moved, so a grant heard again (the ledger's last-heard time) copies the ledger
 * alone. Returns the parts it copied (DSD_APP_DMR_T3_VIEW_COPIED_*). */
int dsd_app_dmr_t3_view_publish(dsd_ui_dmr_t3_view* dst, dsd_app_dmr_t3_view_mark* mark, const dsd_state* src);

/* Copy the published @p src into the consumer's @p dst: the scalars every time, the ledger only when its missing_seq
 * moved and the channel rows only when their content_seq moved. Returns the parts it copied. */
int dsd_app_dmr_t3_view_consume(dsd_ui_dmr_t3_view* dst, const dsd_ui_dmr_t3_view* src);

/* Point @p snapshot's slot at @p view (storage the snapshot module owns; no cleanup). */
void dsd_app_dmr_t3_view_attach(dsd_state* snapshot, dsd_ui_dmr_t3_view* view);

/* 1 while @p opts decode DMR (dsd_opts::frame_dmr): what refuses a channel edit (DSD_APP_DMR_CHANNEL_SET_NO_DMR) on
 * the decoder thread, and the view's dmr_decoding. 0 for NULL. */
uint8_t dsd_app_dmr_t3_dmr_decoding(const dsd_opts* opts);

/* The options were published (ui_opts_snapshot.c, on the thread that publishes them, the decoder's): keep their
 * dsd_app_dmr_t3_dmr_decoding() for the record's next publish, which has the decoder state only. */
void dsd_app_dmr_t3_note_opts(const dsd_opts* opts);

/* dsd_app_dmr_t3_dmr_decoding() of the options dsd_app_dmr_t3_note_opts() last saw; 0 before any. */
uint8_t dsd_app_dmr_t3_dmr_decoding_noted(void);

/* 1 while the carrier's received identity (dsd_state::rx_id of the live decoder state @p state) names a system that is
 * not Tier III: another protocol's, or another DMR trunking system (Capacity Plus, Connect Plus, XPT;
 * dsd_rxid_dmr_trunk_named()); or while it names none but the last frame sync was another protocol's than DMR
 * (dsd_state::rx_sync_protocol: a YSF or D-STAR carrier sends no identity). 0 while nothing was heard on the carrier
 * yet (or only DMR that named no trunking system), and for NULL. The view's other_system_here, and
 * what refuses a channel edit (DSD_APP_DMR_CHANNEL_SET_OTHER_SYSTEM) on the decoder thread. */
uint8_t dsd_app_dmr_t3_other_system_here(const dsd_state* state);

/* The frequency DMR Tier III uses for channel @p channel of @p state, a snapshot or the live decoder state: the user's
 * entry over the shared map row (dmr_t3_chan_effective_hz(); a snapshot's view carries it as channel_hz), so a hidden
 * channel has none, whatever another protocol left in the row. While the carrier names another system
 * (dsd_app_dmr_t3_other_system_here(), a Connect Plus LCN, say), or for a channel past the LPCNs, the shared map row.
 * 0 for channel 0 and NULL. */
long dsd_app_dmr_t3_channel_hz(const dsd_state* state, uint32_t channel);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_SRC_APP_CONTROL_DMR_T3_VIEW_INTERNAL_H_ */
