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
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* What the published copy was filled from: the source's key when its body was last copied, and the trunk-scan target
 * it was parked on (target_id moves whenever that id changes). Zeroed, it holds nothing, so the first copy runs. */
typedef struct {
    uint8_t valid;
    uint8_t present;
    uint64_t instance;
    uint64_t map_seq;
    uint32_t target_id;      /* the parked target's id now */
    uint32_t body_target_id; /* the target the body was copied for */
    uint32_t revision;
    uint32_t missing_revision;
    char target[DSD_CHANNEL_LABEL_SIZE];
} dsd_app_dmr_t3_view_mark;

/* Fill @p dst from the decoder's @p src (decoder thread): the scalars every time, the body only when @p mark's key
 * moved. Returns 1 when it copied the body. */
int dsd_app_dmr_t3_view_publish(dsd_ui_dmr_t3_view* dst, dsd_app_dmr_t3_view_mark* mark, const dsd_state* src);

/* Copy the published @p src into the consumer's @p dst: the scalars every time, the body only when its content_seq
 * moved. Returns 1 when it copied the body. */
int dsd_app_dmr_t3_view_consume(dsd_ui_dmr_t3_view* dst, const dsd_ui_dmr_t3_view* src);

/* Point @p snapshot's slot at @p view (storage the snapshot module owns; no cleanup). */
void dsd_app_dmr_t3_view_attach(dsd_state* snapshot, dsd_ui_dmr_t3_view* view);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_SRC_APP_CONTROL_DMR_T3_VIEW_INTERNAL_H_ */
