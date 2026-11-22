// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The one over-the-air writer of the DMR Tier III channel map (dmr_t3_chan_learn(), <dsd-neo/protocol/dmr/
 * dmr_t3_channels.h>): its precedence over the row's origin, the trust byte, the learned and changed notices, and the
 * opt-in heuristic gap fill, which writes its estimates through the same writer.
 */

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/dmr/dmr_t3_channels.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include "dmr_call_channel.h"
#include "dmr_t3_channels_internal.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

/* Whether @p origin at @p hz may replace a row of origin @p cur holding @p cur_hz (spec F2): any origin fills an empty
   row; only SITE replaces a filled one, a weaker origin always and another SITE one when the frequency differs. */
static int
dmr_t3_chan_may_write(dmr_chan_origin_e cur, long cur_hz, dmr_chan_origin_e origin, long hz) {
    if (cur == DMR_CHAN_ORIGIN_NONE) {
        return 1;
    }
    if (origin != DMR_CHAN_ORIGIN_SITE || cur == DMR_CHAN_ORIGIN_FILE || cur == DMR_CHAN_ORIGIN_USER) {
        return 0;
    }
    return cur != DMR_CHAN_ORIGIN_SITE || cur_hz != hz;
}

/* The trust byte a row learned as @p origin takes: 2 learned on the control channel, else 1; a neighbour site's never
   more than 1 and an estimate 0. */
static uint8_t
dmr_t3_chan_trust(const dsd_opts* opts, const dsd_state* state, dmr_chan_origin_e origin) {
    if (origin == DMR_CHAN_ORIGIN_ESTIMATE) {
        return 0U;
    }
    const int on_cc = state->trunk_cc_freq != 0 && opts != NULL && opts->trunk_is_tuned == 0;
    return (origin != DMR_CHAN_ORIGIN_NEIGHBOUR && on_cc) ? 2U : 1U;
}

/* A channel notice on the receiving burst's slot, which the emitter marks with that slot's CRC flags. Signalling, not a
   call: the data-notice beeper stays quiet. */
static void
dmr_t3_chan_emit_notice(dsd_opts* opts, dsd_state* state, const char* msg) {
    if (opts == NULL) {
        return;
    }
    const uint8_t slot = (uint8_t)(state->currentslot & 1);
    const int prev_alert = opts->call_alert;
    opts->call_alert = 0;
    const dsd_call_observation observation = dsd_call_observation_data(state->lastsynctype, slot, 0xFFFFFFU, 0xFFFFFFU);
    (void)dsd_event_emit_data_notice(opts, state, slot, &observation, msg);
    opts->call_alert = prev_alert;
    dsd_event_sync_slot(opts, state, slot);
}

static void
dmr_t3_chan_notice_write(dsd_opts* opts, dsd_state* state, uint16_t lpcn, long old_hz, long hz) {
    char msg[160];
    if (old_hz == 0) {
        DSD_SNPRINTF(msg, sizeof(msg), "DMR TIII: Learned channel %u -> %.6f MHz;", (unsigned)lpcn,
                     (double)hz / 1000000.0);
    } else if (old_hz != hz) {
        DSD_SNPRINTF(msg, sizeof(msg), "DMR TIII: Channel %u changed %.6f -> %.6f MHz;", (unsigned)lpcn,
                     (double)old_hz / 1000000.0, (double)hz / 1000000.0);
    } else {
        return; /* an origin change at the same frequency shows nothing new */
    }
    dmr_t3_chan_emit_notice(opts, state, msg);
}

/* Heuristic gap fill (opt-in, dsd_opts::dmr_t3_heuristic_fill): a 125 Hz-step linear model through the anchor rows,
   every row that is no estimate, accepted when every anchor sits within 500 Hz of it, fills the empty rows between the
   first and the last anchor as estimates. */
typedef struct {
    int first_lcn;
    int last_lcn;
    long first_freq;
    long long sum_df;
    long long sum_dl;
    int anchors;
} dmr_heuristic_anchor_stats;

/* A row with a live frequency that is no estimate: a hidden channel (USER at no frequency) anchors nothing. */
static int
dmr_heuristic_is_anchor(const dsd_state* state, int l) {
    if (state->trunk_chan_map[l] <= 0) {
        return 0;
    }
    const dmr_chan_origin_e origin = dmr_t3_chan_origin(state, (uint16_t)l);
    return origin != DMR_CHAN_ORIGIN_NONE && origin != DMR_CHAN_ORIGIN_ESTIMATE;
}

static void
dmr_heuristic_collect_anchor_stats(const dsd_state* state, dmr_heuristic_anchor_stats* stats) {
    int prev_lcn = 0;
    long prev_freq = 0;

    DSD_MEMSET(stats, 0, sizeof(*stats));
    for (int l = 1; l <= DMR_T3_MAX_LCN; l++) {
        if (!dmr_heuristic_is_anchor(state, l)) {
            continue;
        }
        const long f = state->trunk_chan_map[l];
        if (stats->first_lcn == 0) {
            stats->first_lcn = l;
            stats->first_freq = f;
        } else {
            stats->sum_dl += l - prev_lcn;
            stats->sum_df += f - prev_freq;
        }
        stats->last_lcn = l;
        stats->anchors++;
        prev_lcn = l;
        prev_freq = f;
    }
}

static long
dmr_heuristic_estimate_step(const dmr_heuristic_anchor_stats* stats) {
    if (stats->anchors < 2 || stats->sum_dl <= 0) {
        return 0;
    }
    const double slope = (double)stats->sum_df / (double)stats->sum_dl;
    const long step = (long)llround(slope / 125.0) * 125L;
    return (step > 0) ? step : 0;
}

static int
dmr_heuristic_validate_model(const dsd_state* state, const dmr_heuristic_anchor_stats* stats, long step) {
    for (int l = 1; l <= DMR_T3_MAX_LCN; l++) {
        if (!dmr_heuristic_is_anchor(state, l)) {
            continue;
        }
        const long model = stats->first_freq + (long)(l - stats->first_lcn) * step;
        if (labs(state->trunk_chan_map[l] - model) > 500) {
            return 0;
        }
    }
    return 1;
}

static int
dmr_heuristic_fill_gaps(dsd_opts* opts, dsd_state* state, const dmr_heuristic_anchor_stats* stats, long step) {
    int filled = 0;
    for (int l = stats->first_lcn; l <= stats->last_lcn; l++) {
        const long f = stats->first_freq + (long)(l - stats->first_lcn) * step;
        if (state->trunk_chan_map[l] == 0 && f > 0
            && dmr_t3_chan_learn(opts, state, (uint16_t)l, f, DMR_CHAN_ORIGIN_ESTIMATE) != 0) {
            filled++;
        }
    }
    return filled;
}

static void
dmr_heuristic_report_fill(dsd_opts* opts, dsd_state* state, const dmr_heuristic_anchor_stats* stats, long step,
                          int filled) {
    char msg[160];
    DSD_SNPRINTF(msg, sizeof(msg), "DMR TIII: Heuristic filled %d LCNs (%0.3f kHz step) from %04d@%0.6f MHz;", filled,
                 (double)step / 1000.0, stats->first_lcn, (double)stats->first_freq / 1000000.0);
    dmr_t3_chan_emit_notice(opts, state, msg);
}

static void
dmr_heuristic_fill(dsd_opts* opts, dsd_state* state) {
    if (opts == NULL || !opts->dmr_t3_heuristic_fill) {
        return;
    }
    dmr_heuristic_anchor_stats stats;
    dmr_heuristic_collect_anchor_stats(state, &stats);
    const long step = dmr_heuristic_estimate_step(&stats);
    if (step <= 0 || !dmr_heuristic_validate_model(state, &stats, step)) {
        return;
    }
    const int filled = dmr_heuristic_fill_gaps(opts, state, &stats, step);
    if (filled > 0) {
        dmr_heuristic_report_fill(opts, state, &stats, step, filled);
    }
}

static int
dmr_t3_chan_origin_is_estimate(unsigned origin) {
    return origin == (unsigned)DMR_CHAN_ORIGIN_ESTIMATE;
}

/* An anchor appeared, moved or went: every estimate goes, and the fill runs again from the anchors left. */
void
dmr_t3_chan_anchors_changed(dsd_opts* opts, dsd_state* state, dmr_ext_t* ext) {
    if (dmr_t3_chan_drop_rows(state, ext, dmr_t3_chan_origin_is_estimate)) {
        ext->revision++;
    }
    dmr_heuristic_fill(opts, state);
}

int
dmr_t3_chan_learn(dsd_opts* opts, dsd_state* state, uint16_t lpcn, long freq_hz, dmr_chan_origin_e origin) {
    if (state == NULL || !dmr_t3_lpcn_is_channel(lpcn) || freq_hz <= 0 || !dmr_t3_chan_origin_is_ota(origin)) {
        return 0;
    }
    const dmr_chan_origin_e cur = dmr_t3_chan_origin(state, lpcn);
    const long cur_hz = state->trunk_chan_map[lpcn];
    if (!dmr_t3_chan_may_write(cur, cur_hz, origin, freq_hz)) {
        return 0;
    }
    /* Without the origin record a learned row would read as a file's, which nothing over the air may replace. */
    dmr_ext_t* ext = dmr_ext_get_or_create(state);
    if (ext == NULL) {
        return 0;
    }
    /* The writer's own row is no takeover (dsd_state_set_trunk_chan_freq() would count one): its bit goes first. */
    const uint64_t seq_before = state->trunk_chan_map_seq;
    dsd_state_dmr_lcn_ota_set(state, lpcn, 0);
    dsd_state_set_trunk_chan_freq(state, lpcn, freq_hz);
    dsd_state_dmr_lcn_ota_set(state, lpcn, 1);
    dmr_t3_missing_note_written(state, ext, lpcn, seq_before);
    ext->origin[lpcn] = (uint8_t)origin;
    state->dmr_lcn_trust[lpcn] = dmr_t3_chan_trust(opts, state, origin);
    ext->revision++;
    if (origin == DMR_CHAN_ORIGIN_ESTIMATE) {
        return 1; /* the fill's own row: its summary is the notice */
    }
    dmr_t3_chan_notice_write(opts, state, lpcn, cur_hz, freq_hz);
    /* An estimate confirmed at its own frequency is promoted in place: it sits between the first and the last anchor
       on the model line, so the model through the anchors (its step is the first-to-last slope) and every estimate
       stay as they are. */
    if (cur == DMR_CHAN_ORIGIN_NONE || cur_hz != freq_hz) {
        dmr_t3_chan_anchors_changed(opts, state, ext);
    }
    return 1;
}
