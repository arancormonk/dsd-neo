// SPDX-License-Identifier: GPL-3.0-or-later

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include "dsd-neo/core/state_fwd.h"

/* Every record keyed to a call epoch or transmission (issue #800): the DMR checked-LC source, the P25 Phase 1 checked
 * voice-user identity and L3Harris Part A, the D-STAR D-PRS record and the last admitted M17 GNSS META. */
static void
set_epoch_records(dsd_state* state) {
    state->dmr_lc_checked_transmission[0] = 1U;
    state->dmr_lc_checked_source[0] = 1001U;
    state->p25_p1_lc_checked_id.epoch = 1U;
    state->p25_p1_lc_checked_id.source = 1001U;
    state->p25_p1_lc_checked_id.target = 100U;
    state->p25_p1_harris_part_a.valid = 1U;
    state->p25_p1_harris_part_a.epoch = 1U;
    state->dstar_dprs.collecting = 1U;
    state->dstar_dprs.len = 5U;
    state->dstar_dprs.epoch = 1U;
    state->m17_gnss_tx_kind = 1U;
    state->m17_gnss_tx_id = 1U;
    state->m17_gnss_tx_epoch = 1U;
    state->m17_gnss_meta_admitted[0] = 0x12U;
}

static int
epoch_records_set(const dsd_state* state) {
    return state->dmr_lc_checked_transmission[0] == 1U && state->dmr_lc_checked_source[0] == 1001U
           && state->p25_p1_lc_checked_id.epoch == 1U && state->p25_p1_harris_part_a.valid == 1U
           && state->dstar_dprs.collecting == 1U && state->dstar_dprs.epoch == 1U && state->m17_gnss_tx_kind == 1U
           && state->m17_gnss_tx_id == 1U && state->m17_gnss_tx_epoch == 1U
           && state->m17_gnss_meta_admitted[0] == 0x12U;
}

static int
epoch_records_clear(const dsd_state* state) {
    return state->dmr_lc_checked_transmission[0] == 0U && state->dmr_lc_checked_source[0] == 0U
           && state->p25_p1_lc_checked_id.epoch == 0U && state->p25_p1_lc_checked_id.source == 0U
           && state->p25_p1_harris_part_a.valid == 0U && state->p25_p1_harris_part_a.epoch == 0U
           && state->dstar_dprs.collecting == 0U && state->dstar_dprs.len == 0U && state->dstar_dprs.epoch == 0U
           && state->m17_gnss_tx_kind == 0U && state->m17_gnss_tx_id == 0U && state->m17_gnss_tx_epoch == 0U
           && state->m17_gnss_meta_admitted[0] == 0U;
}

int
main(void) {
    dsd_state* state = (dsd_state*)calloc(1U, sizeof(*state));
    assert(state != NULL);
    // A new call-state extension restarts the epochs: every record keyed to the old transmissions goes with them.
    set_epoch_records(state);
    dsd_call_observation observation = {0};
    observation.protocol = 35;
    observation.slot = 0U;
    observation.kind = DSD_CALL_KIND_GROUP_VOICE;
    observation.ota_target_id = 100U;
    observation.observed_m = 1.0;

    dsd_call_state_test_alloc_fail_after(0);
    assert(dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN) == -1);
    assert(dsd_state_ext_get(state, DSD_STATE_EXT_CORE_CALL_STATE) == NULL);

    // A failed allocation started nothing: the records stand.
    assert(epoch_records_set(state));

    dsd_call_state_test_alloc_reset();
    assert(dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN) == 1);
    assert(epoch_records_clear(state));
    dsd_call_snapshot snapshot;
    assert(dsd_call_state_get(state, 0U, &snapshot) == 1);
    assert(snapshot.epoch == 1U);
    assert(dsd_call_state_test_set_epoch(state, 0U, UINT64_MAX) == 1);
    assert(dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN) == 1);
    assert(dsd_call_state_get(state, 0U, &snapshot) == 1);
    assert(snapshot.epoch == 1U);

    // The next engine run on this state (its cleanup frees every extension) restarts the epochs at 1 again: the
    // records the last run left, keyed to its epoch 1, would name the new run's first call.
    set_epoch_records(state);
    dsd_state_ext_free_all(state);
    assert(dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN) == 1);
    assert(dsd_call_state_get(state, 0U, &snapshot) == 1 && snapshot.epoch == 1U);
    assert(epoch_records_clear(state));

    dsd_state* clone = (dsd_state*)calloc(1U, sizeof(*clone));
    assert(clone != NULL);
    dsd_call_state_test_alloc_fail_after(0);
    assert(dsd_call_state_copy_to_state(clone, state) == -1);
    assert(dsd_state_ext_get(clone, DSD_STATE_EXT_CORE_CALL_STATE) == NULL);
    dsd_call_state_test_alloc_reset();

    // A copy or a restore carries the source's epoch sequence: the destination's record keyed to it stands.
    clone->dmr_lc_checked_transmission[1] = 1U;
    clone->dmr_lc_checked_source[1] = 1001U;
    assert(dsd_call_state_copy_to_state(clone, state) == 1);
    assert(clone->dmr_lc_checked_transmission[1] == 1U && clone->dmr_lc_checked_source[1] == 1001U);
    dsd_call_state_snapshot calls;
    assert(dsd_call_state_copy_snapshot(state, &calls) >= 0);
    dsd_state* restored = (dsd_state*)calloc(1U, sizeof(*restored));
    assert(restored != NULL);
    set_epoch_records(restored);
    assert(dsd_call_state_restore_snapshot(restored, &calls) >= 0);
    assert(dsd_state_ext_get(restored, DSD_STATE_EXT_CORE_CALL_STATE) != NULL);
    assert(epoch_records_set(restored));
    // Every carrier change (a carrier boundary, a trunk-scan target switch, a scan step, a user's move) forgets it.
    dsd_state_note_carrier_change(restored);
    assert(restored->dmr_lc_checked_transmission[0] == 0U && restored->dmr_lc_checked_source[0] == 0U);
    dsd_state_ext_free_all(restored);
    free(restored);

    dsd_state_ext_free_all(state);
    dsd_state_ext_free_all(clone);
    free(clone);
    free(state);
    return 0;
}
