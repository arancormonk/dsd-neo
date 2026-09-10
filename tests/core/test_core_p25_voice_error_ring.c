// SPDX-License-Identifier: GPL-3.0-or-later
#include <assert.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/vocoder.h>
#include <stddef.h>
#include <stdint.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

static void
begin(dsd_state* state, int protocol, unsigned slot, unsigned target) {
    dsd_call_observation call = {0};
    call.protocol = protocol;
    call.slot = (uint8_t)slot;
    call.kind = DSD_CALL_KIND_GROUP_VOICE;
    call.ota_target_id = target;
    assert(dsd_call_state_observe(state, &call, DSD_CALL_BOUNDARY_BEGIN) == 1);
}

int
main(void) {
    static dsd_opts opts;
    static dsd_state state;
    initOpts(&opts);
    initState(&state);
    opts.audio_out = 0;
    opts.floating_point = 1;
    state.synctype = DSD_SYNC_P25P1_POS;
    state.p25_crypto_state[0] = DSD_P25_CRYPTO_CLEAR;
    state.p25_p1_voice_err_hist_len = 3;
    begin(&state, state.synctype, 0, 101);
    unsigned expected[3] = {0};
    for (int i = 0; i < 8; ++i) {
        char frame[8][23] = {{0}};
        frame[0][i % 23] = 1;
        processMbeFrame(&opts, &state, frame, NULL, NULL);
        expected[i % 3] = (unsigned)state.errs2;
        assert(state.p25_p1_voice_err_hist_count == (i < 3 ? i + 1 : 3));
        assert(state.p25_p1_voice_err_hist_sum == expected[0] + expected[1] + expected[2]);
        assert(state.p25_p1_voice_err_hist_pos == (i + 1) % 3);
    }
    begin(&state, state.synctype, 0, 102);
    assert(state.p25_p1_voice_err_hist_count == 0);
    assert(state.p25_p1_voice_err_hist_sum == 0);
    assert(state.p25_p1_voice_err_hist_pos == 0);
    assert(state.p25_p1_voice_err_hist_len == 3);
    for (unsigned i = 0; i < sizeof(state.p25_p1_voice_err_hist); ++i) {
        assert(state.p25_p1_voice_err_hist[i] == 0);
    }
    char frame[8][23] = {{0}};
    processMbeFrame(&opts, &state, frame, NULL, NULL);
    assert(state.p25_p1_voice_err_hist_count == 1);
    assert(state.p25_p1_voice_err_hist_sum == (unsigned)state.errs2);

    state.synctype = DSD_SYNC_P25P2_POS;
    state.p2_wacn = 1;
    state.p2_sysid = 2;
    state.p2_cc = 3;
    state.p25_p2_voice_err_hist_len = 3;
    opts.dmr_mono = 1;
    opts.dmr_stereo = 1;
    for (int slot = 0; slot < 2; ++slot) {
        state.currentslot = slot;
        state.p25_p2_audio_allowed[slot] = 1;
        begin(&state, state.synctype, (unsigned)slot, 201 + (unsigned)slot);
        unsigned sums[3] = {0};
        for (int i = 0; i < 8; ++i) {
            char ambe[4][24] = {{0}};
            ambe[0][i % 24] = 1;
            processMbeFrame(&opts, &state, NULL, ambe, NULL);
            sums[i % 3] = (unsigned)(slot ? state.errs2R : state.errs2);
            assert(state.p25_p2_voice_err_hist_count[slot] == (i < 3 ? i + 1 : 3));
            assert(state.p25_p2_voice_err_hist_sum[slot] == sums[0] + sums[1] + sums[2]);
        }
    }
    unsigned left_sum = state.p25_p2_voice_err_hist_sum[0];
    begin(&state, state.synctype, 1, 203);
    assert(state.p25_p2_voice_err_hist_count[0] == 3);
    assert(state.p25_p2_voice_err_hist_sum[0] == left_sum);
    assert(state.p25_p2_voice_err_hist_count[1] == 0);
    assert(state.p25_p2_voice_err_hist_sum[1] == 0);
    assert(state.p25_p2_voice_err_hist_pos[1] == 0);
    // Late entry creates a provisional epoch. Learning identity later must not
    // discard its already-produced samples, while a real re-key must do so.
    state.synctype = DSD_SYNC_P25P1_POS;
    dsd_call_state_end(&state, 0, 0);
    state.p25_p1_voice_err_hist_len = 0;
    char late_frame[8][23] = {{0}};
    processMbeFrame(&opts, &state, late_frame, NULL, NULL);
    assert(state.p25_p1_voice_err_hist_len == 50);
    assert(state.p25_p1_voice_err_hist_count == 1);
    dsd_call_observation identity = {0};
    identity.protocol = state.synctype;
    identity.kind = DSD_CALL_KIND_GROUP_VOICE;
    identity.ota_target_id = 301;
    assert(dsd_call_state_observe(&state, &identity, DSD_CALL_BOUNDARY_BEGIN) == 0);
    assert(state.p25_p1_voice_err_hist_count == 1);
    state.p25_p1_voice_err_hist_len = 100;
    assert(dsd_call_state_observe(&state, &identity, DSD_CALL_BOUNDARY_BEGIN) == 1);
    processMbeFrame(&opts, &state, late_frame, NULL, NULL);
    assert(state.p25_p1_voice_err_hist_len == 64);
    assert(state.p25_p1_voice_err_hist_count == 1);
    freeState(&state);
    return 0;
}
