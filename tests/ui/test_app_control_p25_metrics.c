// SPDX-License-Identifier: GPL-3.0-or-later
#include <dsd-neo/app_control/p25_metrics.h>

#include <assert.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <limits.h>
#include <math.h>

static void
begin(dsd_state* state, int protocol, unsigned slot) {
    dsd_call_observation call = {0};
    call.protocol = protocol;
    call.slot = (uint8_t)slot;
    call.kind = DSD_CALL_KIND_GROUP_VOICE;
    call.ota_target_id = slot + 1;
    assert(dsd_call_state_observe(state, &call, DSD_CALL_BOUNDARY_BEGIN) == 1);
}

int
main(void) {
    dsd_app_fec_ratio r = dsd_app_fec_ratio_make(0, 0);
    assert(!r.valid && r.ok_pct == 0.0 && r.ok == 0 && r.err == 0);
    r = dsd_app_fec_ratio_make(3, 1);
    assert(r.valid && r.ok_pct == 75.0 && r.ok == 3 && r.err == 1);
    assert(dsd_app_fec_ratio_make(0, 2).ok_pct == 0.0);
    assert(dsd_app_fec_ratio_make(0, 2).valid);
    assert(dsd_app_fec_ratio_make(2, 0).ok_pct == 100.0);
    assert(dsd_app_fec_ratio_make(UINT64_MAX, UINT64_MAX).ok_pct == 50.0);

    static dsd_state state;
    dsd_app_p25_quality q;
    dsd_app_p25_quality_from_state(NULL, &q);
    assert(!q.valid);
    dsd_app_p25_quality_from_state(&state, NULL);
    assert(!dsd_app_p25p1_voice_avg_errs(NULL).valid);
    assert(!dsd_app_p25p2_voice_avg_errs(NULL, 0).valid);
    assert(!dsd_app_p25p2_voice_avg_errs(&state, -1).valid);
    assert(!dsd_app_p25p2_voice_avg_errs(&state, 2).valid);
    state.p25_p1_voice_err_hist_len = 50;
    state.p25_p1_voice_err_hist_count = 51;
    assert(!dsd_app_p25p1_voice_avg_errs(&state).valid);
    state.p25_p1_voice_err_hist_count = -1;
    assert(!dsd_app_p25p1_voice_avg_errs(&state).valid);
    state.p25_p1_voice_err_hist_count = 0;
    state.synctype = DSD_SYNC_P25P1_POS;
    state.p25_p1_voice_err_hist_len = 50;
    dsd_app_p25_quality_from_state(&state, &q);
    assert(!q.valid); // Sync and allocated capacity are not voice samples.
    state.p25_p1_fec_ok = 3;
    state.p25_p1_fec_err = 1;
    dsd_app_p25_quality_from_state(&state, &q);
    assert(q.valid && q.cc_fec.valid && q.cc_fec.ok_pct == 75.0 && !q.p1_voice.valid);
    state.synctype = DSD_SYNC_NONE;
    dsd_app_p25_quality_from_state(&state, &q);
    assert(q.cc_fec.valid); // CC counters follow noCarrier, not transient sync gaps.
    state.p25_p1_fec_ok = state.p25_p1_fec_err = 0;
    dsd_app_p25_quality_from_state(&state, &q);
    assert(!q.cc_fec.valid);

    begin(&state, DSD_SYNC_P25P1_POS, 0);
    state.p25_p1_voice_err_hist_count = 2;
    state.p25_p1_voice_err_hist_sum = 9;
    dsd_app_voice_errs v = dsd_app_p25p1_voice_avg_errs(&state);
    assert(v.valid && v.samples == 2 && v.errs_per_frame == 4.5);
    state.p25_p1_voice_fec_ok = 7;
    state.p25_p1_voice_fec_err = 1;
    dsd_app_p25_quality_from_state(&state, &q);
    assert(q.p1_voice.valid && q.p1_voice.errs_per_frame == 4.5);
    assert(q.voice_fec.valid && q.voice_fec.ok_pct == 87.5);
    assert(dsd_call_state_end(&state, 0, 0) == 1);
    dsd_app_p25_quality_from_state(&state, &q);
    assert(!q.p1_voice.valid); // Retained ended call cannot advertise fresh voice.
    begin(&state, DSD_SYNC_P25P1_POS, 0);
    dsd_app_p25_quality_from_state(&state, &q);
    assert(!q.p1_voice.valid && q.p1_voice.samples == 0);

    state.synctype = DSD_SYNC_P25P2_POS;
    begin(&state, state.synctype, 0);
    begin(&state, state.synctype, 1);
    state.p25_p2_voice_err_hist_len = 50;
    state.p25_p2_voice_err_hist_count[1] = 3;
    state.p25_p2_voice_err_hist_sum[1] = 6;
    state.p25_p2_rs_facch_ok = UINT_MAX;
    state.p25_p2_rs_sacch_ok = UINT_MAX;
    state.p25_p2_rs_ess_ok = UINT_MAX;
    state.p25_p2_rs_facch_err = UINT_MAX;
    dsd_app_p25_quality_from_state(&state, &q);
    assert(!q.p2_voice[0].valid && q.p2_voice[1].valid);
    assert(q.p2_voice[1].samples == 3 && q.p2_voice[1].errs_per_frame == 2.0);
    assert(q.rs.valid && q.rs.ok == 3ULL * UINT_MAX && q.rs.ok_pct == 75.0);
    begin(&state, state.synctype, 1);
    dsd_app_p25_quality_from_state(&state, &q);
    assert(!q.p2_voice[1].valid);

    state.synctype = DSD_SYNC_DMR_BS_VOICE_POS;
    begin(&state, state.synctype, 1);
    state.errsR = 2;
    state.errs2R = 7;
    dsd_app_p25_quality_from_state(&state, &q);
    assert(!q.last_frame[1].valid);
    assert(dsd_call_state_update_media(&state, 1, 1, 0) == 1);
    dsd_app_p25_quality_from_state(&state, &q);
    assert(q.last_frame[1].valid && q.last_frame[1].errs == 2 && q.last_frame[1].errs2 == 7);
    assert(!q.last_frame[0].valid && !q.p1_voice.valid && !q.p2_voice[1].valid);
    state.errsR = state.errs2R = 0;
    dsd_app_p25_quality_from_state(&state, &q);
    assert(q.last_frame[1].valid && q.last_frame[1].errs2 == 0);
    assert(dsd_call_state_end(&state, 1, 0) == 1);
    dsd_app_p25_quality_from_state(&state, &q);
    assert(!q.last_frame[1].valid);
    dsd_state_ext_free_all(&state);
    return 0;
}
