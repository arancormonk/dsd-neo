// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Event-history notices for decoded DMR Reverse Channel commands.
 *
 * Shared by the standalone RC burst handler (dmr_rc.c) and the embedded SB/RC
 * path (dmr_le.c). Lives in its own translation unit so dmr_le.o does not
 * inherit the standalone handler's dibit-reader dependency chain.
 */

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/protocol/dmr/dmr.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

enum { DMR_RC_NOTIFY_KEYS = 3, DMR_RC_NOTIFY_WINDOW_S = 5 };

/* Per-key repeat suppression for RC event rows. Keys 0/1 are the embedded
 * SB/RC path per slot index, key 2 the standalone burst; keeping them apart
 * stops an embedded repeat train from masking a standalone burst and vice
 * versa. */
typedef struct {
    uint8_t last_cmd[DMR_RC_NOTIFY_KEYS];
    uint8_t valid[DMR_RC_NOTIFY_KEYS];
    time_t last_time[DMR_RC_NOTIFY_KEYS];
} dmr_rc_dedup_t;

static dmr_rc_dedup_t*
dmr_rc_dedup_get_or_create(dsd_state* state) {
    dmr_rc_dedup_t* dedup = DSD_STATE_EXT_GET_AS(dmr_rc_dedup_t, state, DSD_STATE_EXT_PROTO_DMR_RC);
    if (dedup != NULL) {
        return dedup;
    }

    dedup = (dmr_rc_dedup_t*)calloc(1, sizeof(*dedup));
    if (dedup == NULL) {
        return NULL;
    }
    if (dsd_state_ext_set(state, DSD_STATE_EXT_PROTO_DMR_RC, dedup, free) != 0) {
        free(dedup);
        return NULL;
    }
    return dedup;
}

void
dmr_rc_notify_command(dsd_opts* opts, dsd_state* state, uint8_t emit_slot, uint8_t dedup_key, uint8_t rc_command,
                      int have_cc, uint8_t cc, time_t now) {
    if (opts == NULL || state == NULL || dedup_key >= DMR_RC_NOTIFY_KEYS) {
        return;
    }

    /* Reserved commands (6..15) stay stderr-only: the CRC-7 covers just four
     * data bits, so a corrupted-but-CRC-passing burst is plausible enough to
     * keep out of the event history. */
    const char* name = dmr_rc_command_name(rc_command);
    if (name == NULL) {
        return;
    }

    /* An identical command inside the sliding window is a repeat train, not a
     * new operator event; refreshing the timestamp keeps a continuous train
     * collapsed to one row. Read-only: suppression never allocates. */
    dmr_rc_dedup_t* dedup = DSD_STATE_EXT_GET_AS(dmr_rc_dedup_t, state, DSD_STATE_EXT_PROTO_DMR_RC);
    if (dedup != NULL && dedup->valid[dedup_key] != 0U && dedup->last_cmd[dedup_key] == rc_command
        && now >= dedup->last_time[dedup_key] && now - dedup->last_time[dedup_key] < DMR_RC_NOTIFY_WINDOW_S) {
        dedup->last_time[dedup_key] = now;
        return;
    }

    char notice[96];
    if (have_cc != 0) {
        DSD_SNPRINTF(notice, sizeof(notice), "DMR RC: %s; CC: %02u;", name, cc & 0x0FU);
    } else {
        DSD_SNPRINTF(notice, sizeof(notice), "DMR RC: %s;", name);
    }

    /* RC is signalling, not a call: keep the data-notice beeper quiet. */
    const int prev_alert = opts->call_alert;
    opts->call_alert = 0;
    const dsd_call_observation observation =
        dsd_call_observation_data(state->lastsynctype, emit_slot, 0xFFFFFFU, 0xFFFFFFU);
    const int emitted = dsd_event_emit_data_notice_classified_with_gps(opts, state, emit_slot, &observation,
                                                                       DSD_EVENT_CATEGORY_CONTROL, notice, "");
    opts->call_alert = prev_alert;

    /* Record only after a successful emit: states without an event history
     * (the emitter rejects them) never allocate the ext slot. */
    if (emitted == 0) {
        dedup = dmr_rc_dedup_get_or_create(state);
        if (dedup != NULL) {
            dedup->valid[dedup_key] = 1U;
            dedup->last_cmd[dedup_key] = rc_command;
            dedup->last_time[dedup_key] = now;
        }
    }
}
