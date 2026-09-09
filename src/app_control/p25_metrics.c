// SPDX-License-Identifier: GPL-3.0-or-later
#include <dsd-neo/app_control/p25_metrics.h>

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>

dsd_app_fec_ratio
dsd_app_fec_ratio_make(uint64_t ok, uint64_t err) {
    dsd_app_fec_ratio out = {0};
    out.ok = ok;
    out.err = err;
    out.valid = ok != 0 || err != 0;
    if (out.valid) {
        /* Convert before adding: both inputs can span the whole counter range. */
        out.ok_pct = 100.0 * (double)ok / ((double)ok + (double)err);
    }
    return out;
}

static dsd_app_voice_errs
voice_average(unsigned sum, int count, int capacity, int maximum) {
    dsd_app_voice_errs out = {0};
    if (count > 0 && count <= capacity && capacity <= maximum) {
        out.valid = 1;
        out.samples = (unsigned)count;
        out.errs_per_frame = (double)sum / (double)count;
    }
    return out;
}

dsd_app_voice_errs
dsd_app_p25p1_voice_avg_errs(const dsd_state* state) {
    if (!state) {
        return (dsd_app_voice_errs){0};
    }
    return voice_average(state->p25_p1_voice_err_hist_sum, state->p25_p1_voice_err_hist_count,
                         state->p25_p1_voice_err_hist_len, (int)sizeof(state->p25_p1_voice_err_hist));
}

dsd_app_voice_errs
dsd_app_p25p2_voice_avg_errs(const dsd_state* state, int slot) {
    if (!state || slot < 0 || slot > 1) {
        return (dsd_app_voice_errs){0};
    }
    return voice_average(state->p25_p2_voice_err_hist_sum[slot], state->p25_p2_voice_err_hist_count[slot],
                         state->p25_p2_voice_err_hist_len, (int)sizeof(state->p25_p2_voice_err_hist[slot]));
}

void
dsd_app_p25_quality_from_state(const dsd_state* state, dsd_app_p25_quality* out) {
    if (!out) {
        return;
    }
    DSD_MEMSET(out, 0, sizeof(*out));
    if (!state) {
        return;
    }
    out->cc_fec = dsd_app_fec_ratio_make(state->p25_p1_fec_ok, state->p25_p1_fec_err);
    out->voice_fec = dsd_app_fec_ratio_make(state->p25_p1_voice_fec_ok, state->p25_p1_voice_fec_err);
    out->rs = dsd_app_fec_ratio_make(
        (uint64_t)state->p25_p2_rs_facch_ok + state->p25_p2_rs_sacch_ok + state->p25_p2_rs_ess_ok,
        (uint64_t)state->p25_p2_rs_facch_err + state->p25_p2_rs_sacch_err + state->p25_p2_rs_ess_err);
    out->valid = out->cc_fec.valid || out->voice_fec.valid || out->rs.valid;
    for (int slot = 0; slot < 2; ++slot) {
        dsd_call_snapshot call;
        if (dsd_call_state_get(state, (uint8_t)slot, &call) <= 0 || call.phase != DSD_CALL_PHASE_ACTIVE) {
            continue;
        }
        if (DSD_SYNC_IS_P25P1(call.protocol) && slot == 0) {
            out->p1_voice = dsd_app_p25p1_voice_avg_errs(state);
        } else if (DSD_SYNC_IS_P25P2(call.protocol)) {
            out->p2_voice[slot] = dsd_app_p25p2_voice_avg_errs(state, slot);
        } else if (!DSD_SYNC_IS_P25(call.protocol) && call.protocol != DSD_SYNC_NONE && call.media_active) {
            out->last_frame[slot].valid = 1;
            out->last_frame[slot].errs = slot == 0 ? state->errs : state->errsR;
            out->last_frame[slot].errs2 = slot == 0 ? state->errs2 : state->errs2R;
        }
        out->valid |= out->p1_voice.valid || out->p2_voice[slot].valid || out->last_frame[slot].valid;
    }
}
