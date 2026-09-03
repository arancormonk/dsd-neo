// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Resolver for the label of the channel currently being listened to.
 */

#include <dsd-neo/core/channel_label.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>

#include <stddef.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

/*
 * Both candidates are DSD_CHANNEL_LABEL_SIZE char arrays owned by dsd_state, so
 * the returned pointer stays valid for the caller's copy and is bounded by that
 * size even if a writer ever fills one to the brim.
 */
static const char*
channel_label_pick(const dsd_opts* opts, const dsd_state* state, dsd_channel_label_source* source) {
    *source = DSD_CHANNEL_LABEL_SOURCE_NONE;
    if (opts->trunk_scan_enabled == 1 && state->trunk_scan_active_id[0] != '\0') {
        *source = DSD_CHANNEL_LABEL_SOURCE_TRUNK_SCAN;
        return state->trunk_scan_active_id;
    }
    /* lcn_freq_roll is advanced past the row just tuned, so the row on air is roll - 1.
     * Bound it by lcn_freq_count the way every other scan-list consumer does: a protocol
     * writer that shrinks the count leaves roll pointing past the end. A row whose
     * frequency is 0 -- the placeholder an importer writes to keep the file's numbering --
     * is stepped over without a retune, so the receiver is still on the previous row for
     * that whole hangtime and the placeholder's name would credit the wrong channel. */
    if (opts->scanner_mode == 1 && state->lcn_freq_roll > 0 && state->lcn_freq_roll <= state->lcn_freq_count
        && *dsd_state_trunk_lcn_slot_const(state, state->lcn_freq_roll - 1) != 0) {
        const char* name = dsd_state_trunk_lcn_name_get(state, (size_t)(state->lcn_freq_roll - 1));
        if (name && name[0] != '\0') {
            *source = DSD_CHANNEL_LABEL_SOURCE_SCAN_LIST;
        }
        return name;
    }
    return NULL;
}

dsd_channel_label_source
dsd_channel_label_current_source(const dsd_opts* opts, const dsd_state* state) {
    dsd_channel_label_source source = DSD_CHANNEL_LABEL_SOURCE_NONE;
    if (opts && state) {
        (void)channel_label_pick(opts, state, &source);
    }
    return source;
}

int
dsd_channel_label_current(const dsd_opts* opts, const dsd_state* state, char* out, size_t out_sz) {
    if (out && out_sz > 0) {
        out[0] = '\0';
    }
    if (!opts || !state) {
        return 0;
    }
    dsd_channel_label_source source = DSD_CHANNEL_LABEL_SOURCE_NONE;
    const char* label = channel_label_pick(opts, state, &source);
    if (source == DSD_CHANNEL_LABEL_SOURCE_NONE || !label || label[0] == '\0') {
        return 0;
    }
    if (out && out_sz > 0) {
        DSD_SNPRINTF(out, out_sz, "%.*s", (int)(DSD_CHANNEL_LABEL_SIZE - 1), label);
    }
    return 1;
}
