// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* The next-start rule on any options and state (next_start_check.h). Pure: no stream, engine or I/O. */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/runtime/config.h>
#include <stddef.h>
#include <stdlib.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "next_start_check.h"

int
app_check_next_start(const dsd_opts* opts, const dsd_state* state, app_next_start_change_fn change, const void* ctx,
                     char* why, size_t why_size) {
    return app_check_next_start_input(opts, state, change, ctx, 0, why, why_size);
}

int
app_check_next_start_input(const dsd_opts* opts, const dsd_state* state, app_next_start_change_fn change,
                           const void* ctx, int replaces_input, char* why, size_t why_size) {
    if (why && why_size > 0U) {
        why[0] = '\0';
    }
    if (!opts || !state || !change) {
        return -1;
    }
    /* Two saves (a few tens of KB each) on the heap, once per change that can touch the trunk-scan rules. */
    dsdneoUserConfig* before = (dsdneoUserConfig*)calloc(2, sizeof *before);
    if (!before) {
        if (why && why_size > 0U) {
            DSD_SNPRINTF(why, why_size, "%s", "out of memory");
        }
        return -1;
    }
    dsdneoUserConfig* after = before + 1;
    dsd_snapshot_opts_to_user_config(opts, state, before);
    /* A replay session saves the [input] it loaded (issue #743), which may be a raw I/Q stream, beside which every save
       turns trunking and the scanners off while the replay runs them. A change that leaves the replay running leaves
       that save, so it is judged with them off too; one that replaces the replay runs them over the new input, so it
       is judged on them as the session runs them. */
    const int replay = dsd_opts_input_is_iq_replay(opts);
    if (replay && replaces_input) {
        dsd_snapshot_opts_to_user_config_as_run(opts, state, after);
    } else {
        DSD_MEMCPY(after, before, sizeof *after);
    }
    change(after, ctx);
    if (replay && !replaces_input) {
        dsd_user_config_turn_off_beside_iq_stream(after);
    }
    const int rc = dsd_user_config_check_trunk_scan_startable(after, before, why, why_size);
    free(before);
    return rc;
}

void
app_next_start_load_channel_map(dsdneoUserConfig* save, const void* ctx) {
    DSD_SNPRINTF(save->trunk_chan_csv, sizeof save->trunk_chan_csv, "%s", (const char*)ctx);
}
