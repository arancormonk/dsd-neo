// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/protocol/nxdn/nxdn_trunk_diag.h>
#include <dsd-neo/runtime/log.h>
#include <dsd-neo/runtime/trunk_scan_hooks.h>
#include <stdint.h>
#include <stdlib.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

typedef nxdn_trunk_diag_ledger nxdn_trunk_diag_t;

static nxdn_trunk_diag_t*
nxdn_trunk_diag_get(const dsd_state* state) {
    if (!state) {
        return NULL;
    }
    return DSD_STATE_EXT_GET_AS(nxdn_trunk_diag_t, (dsd_state*)state, DSD_STATE_EXT_PROTO_NXDN_TRUNK_DIAG);
}

static nxdn_trunk_diag_t*
nxdn_trunk_diag_get_or_create(dsd_state* state) {
    if (!state) {
        return NULL;
    }

    nxdn_trunk_diag_t* diag = nxdn_trunk_diag_get(state);
    if (diag) {
        return diag;
    }

    diag = (nxdn_trunk_diag_t*)calloc(1, sizeof(*diag));
    if (!diag) {
        return NULL;
    }
    if (dsd_state_ext_set(state, DSD_STATE_EXT_PROTO_NXDN_TRUNK_DIAG, diag, free) != 0) {
        free(diag);
        return NULL;
    }
    return diag;
}

int
nxdn_trunk_diag_note_missing_channel(dsd_state* state, uint16_t channel) {
    if (!state) {
        return 0;
    }
    if (channel == 0 || channel >= 0xFFFFu) {
        return 0;
    }

    nxdn_trunk_diag_t* diag = nxdn_trunk_diag_get_or_create(state);
    if (!diag) {
        return 0;
    }

    const size_t byte_idx = (size_t)channel / 8u;
    const uint8_t bit_mask = (uint8_t)(1u << ((unsigned)channel % 8u));
    if ((diag->missing_seen[byte_idx] & bit_mask) != 0) {
        return 0;
    }

    diag->missing_seen[byte_idx] |= bit_mask;
    diag->missing_unique++;
    return 1;
}

long int
nxdn_trunk_diag_dense_chan_lookup(const void* ctx, uint16_t channel) {
    const long int* chan_map = (const long int*)ctx;
    return chan_map ? chan_map[channel] : 0;
}

static size_t
nxdn_trunk_diag_collect_from(const nxdn_trunk_diag_ledger* ledger, nxdn_trunk_diag_chan_freq_fn lookup, const void* ctx,
                             uint16_t* out, size_t out_cap) {
    if (!ledger || !lookup || ledger->missing_unique == 0) {
        return 0;
    }

    size_t total = 0;
    size_t wrote = 0;

    for (unsigned int ch = 1; ch < 0xFFFFu; ch++) {
        const size_t byte_idx = (size_t)ch / 8u;
        const uint8_t bit_mask = (uint8_t)(1u << (ch % 8u));
        if ((ledger->missing_seen[byte_idx] & bit_mask) == 0) {
            continue;
        }
        if (lookup(ctx, (uint16_t)ch) != 0) {
            continue;
        }

        total++;
        if (out && wrote < out_cap) {
            out[wrote++] = (uint16_t)ch;
        }
    }

    return total;
}

size_t
nxdn_trunk_diag_collect_unmapped_channels(const dsd_state* state, uint16_t* out, size_t out_cap) {
    if (!state) {
        return 0;
    }
    return nxdn_trunk_diag_collect_from(nxdn_trunk_diag_get(state), nxdn_trunk_diag_dense_chan_lookup,
                                        state->trunk_chan_map, out, out_cap);
}

void
nxdn_trunk_diag_ledger_save(const dsd_state* state, nxdn_trunk_diag_ledger* out) {
    if (!out) {
        return;
    }
    DSD_MEMSET(out, 0, sizeof(*out));

    const nxdn_trunk_diag_t* diag = nxdn_trunk_diag_get(state);
    if (!diag) {
        return;
    }
    DSD_MEMCPY(out, diag, sizeof(*out));
}

void
nxdn_trunk_diag_ledger_restore(dsd_state* state, const nxdn_trunk_diag_ledger* ledger) {
    if (!state || !ledger) {
        return;
    }

    nxdn_trunk_diag_t* diag = nxdn_trunk_diag_get(state);
    if (!diag) {
        // Nothing recorded on either side: rotating past targets that never decoded a grant must
        // not allocate a ledger for each of them.
        if (ledger->missing_unique == 0) {
            return;
        }
        diag = nxdn_trunk_diag_get_or_create(state);
        if (!diag) {
            return;
        }
    }
    DSD_MEMCPY(diag, ledger, sizeof(*diag));
}

const char*
nxdn_trunk_diag_chan_map_path(const dsd_opts* opts, const dsd_state* state) {
    if (!opts) {
        return NULL;
    }
    if (opts->chan_in_file[0] != '\0') {
        return opts->chan_in_file;
    }
    if (opts->trunk_scan_enabled != 1) {
        return NULL;
    }

    // Trunk scan rejects a global -C map and imports each target's chan_csv through throwaway
    // options, so the coordinator is the only holder of the path.
    const char* path = dsd_trunk_scan_hook_active_chan_csv(state);
    return (path && path[0] != '\0') ? path : NULL;
}

void
nxdn_trunk_diag_log_missing_channel_once(const dsd_opts* opts, dsd_state* state, uint16_t channel,
                                         const char* context) {
    if (!opts || !state) {
        return;
    }
    const char* chan_csv = nxdn_trunk_diag_chan_map_path(opts, state);
    if (!chan_csv) {
        return;
    }
    if (channel == 0 || channel >= 0xFFFFu) {
        return;
    }
    if (state->trunk_chan_map[channel] != 0) {
        return;
    }
    if (!nxdn_trunk_diag_note_missing_channel(state, channel)) {
        return;
    }

    if (context && context[0] != '\0') {
        LOG_INFO("NOTICE: NXDN trunking: %s: CH %u has no frequency mapping in chan_csv (%s)\n", context, channel,
                 chan_csv);
    } else {
        LOG_INFO("NOTICE: NXDN trunking: CH %u has no frequency mapping in chan_csv (%s)\n", channel, chan_csv);
    }
}

static void
nxdn_trunk_diag_summary_append_channels(char* msg, size_t msg_cap, size_t* used, const uint16_t* missing,
                                        size_t shown) {
    if (!msg || !used || !missing || msg_cap == 0) {
        return;
    }

    for (size_t i = 0; i < shown && *used < msg_cap; i++) {
        const char* sep = (i == 0) ? " CH " : ", CH ";
        int w = DSD_SNPRINTF(msg + *used, msg_cap - *used, "%s%u", sep, missing[i]);
        if (w < 0) {
            break;
        }
        *used += (size_t)w;
    }
}

static void
nxdn_trunk_diag_summary_append_overflow(char* msg, size_t msg_cap, size_t* used, size_t total, size_t shown) {
    if (!msg || !used || msg_cap == 0 || total <= shown || *used >= msg_cap) {
        return;
    }

    int w = DSD_SNPRINTF(msg + *used, msg_cap - *used, " (+%zu more)", total - shown);
    if (w > 0) {
        *used += (size_t)w;
    }
}

static void
nxdn_trunk_diag_summary_finalize(char* msg, size_t msg_cap, size_t used) {
    if (!msg || msg_cap == 0) {
        return;
    }

    if (used < msg_cap) {
        (void)DSD_SNPRINTF(msg + used, msg_cap - used, "\n");
    } else {
        msg[msg_cap - 1] = '\0';
    }
}

// Cppcheck 2.21 loses the final prototype name after a callback typedef parameter.
// cppcheck-suppress-begin funcArgNamesDifferentUnnamed
void
nxdn_trunk_diag_log_summary_for(const char* chan_csv, const nxdn_trunk_diag_ledger* ledger,
                                nxdn_trunk_diag_chan_freq_fn lookup, const void* ctx) {
    if (!chan_csv || chan_csv[0] == '\0') {
        return;
    }

    uint16_t missing[16];
    const size_t cap = sizeof(missing) / sizeof(missing[0]);
    const size_t total = nxdn_trunk_diag_collect_from(ledger, lookup, ctx, missing, cap);
    if (total == 0) {
        return;
    }

    char msg[512];
    int n =
        DSD_SNPRINTF(msg, sizeof msg, "NXDN trunking: %zu channel%s missing frequency mapping in chan_csv (%s):", total,
                     (total == 1) ? " is" : "s are", chan_csv);
    if (n < 0) {
        return;
    }
    size_t used = (size_t)n;

    const size_t shown = (total < cap) ? total : cap;
    nxdn_trunk_diag_summary_append_channels(msg, sizeof msg, &used, missing, shown);
    nxdn_trunk_diag_summary_append_overflow(msg, sizeof msg, &used, total, shown);
    nxdn_trunk_diag_summary_finalize(msg, sizeof msg, used);

    LOG_INFO("NOTICE: %s", msg);
}

// cppcheck-suppress-end funcArgNamesDifferentUnnamed

void
nxdn_trunk_diag_log_summary(const dsd_opts* opts, const dsd_state* state) {
    if (!opts || !state) {
        return;
    }
    nxdn_trunk_diag_log_summary_for(nxdn_trunk_diag_chan_map_path(opts, state), nxdn_trunk_diag_get(state),
                                    nxdn_trunk_diag_dense_chan_lookup, state->trunk_chan_map);
}
