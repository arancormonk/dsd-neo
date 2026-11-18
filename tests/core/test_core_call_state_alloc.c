// SPDX-License-Identifier: GPL-3.0-or-later

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include "dsd-neo/core/state_fwd.h"

/* Every record keyed to a call epoch or transmission (issue #800): the DMR checked-LC source, the P25 Phase 1 checked
 * voice-user identity and L3Harris Part A, the call a D-STAR header named and the last admitted M17 GNSS META. The call
 * state knows none of them: it never gives out an epoch twice on one state, so none of them matches a later call. */
static void
set_epoch_records(dsd_state* state, uint64_t epoch) {
    state->dmr_lc_attribution[0].call_epoch = epoch;
    state->dmr_lc_attribution[0].transmission = epoch;
    state->dmr_lc_attribution[0].source = 1001U;
    state->p25_p1_lc_attribution.call_epoch = epoch;
    state->p25_p1_lc_attribution.source = 1001U;
    state->p25_p1_lc_attribution.target = 100U;
    state->p25_p1_harris_part_a.valid = 1U;
    state->p25_p1_harris_part_a.epoch = epoch;
    state->dstar_tx_call_epoch = epoch;
    state->m17_gnss_tx_kind = 1U;
    state->m17_gnss_tx_id = epoch;
    state->m17_gnss_tx_epoch = epoch;
    state->m17_gnss_meta_admitted[0] = 0x12U;
}

static int
epoch_records_set(const dsd_state* state, uint64_t epoch) {
    return state->dmr_lc_attribution[0].transmission == epoch && state->dmr_lc_attribution[0].source == 1001U
           && state->p25_p1_lc_attribution.call_epoch == epoch && state->p25_p1_harris_part_a.valid == 1U
           && state->dstar_tx_call_epoch == epoch && state->m17_gnss_tx_kind == 1U && state->m17_gnss_tx_id == epoch
           && state->m17_gnss_tx_epoch == epoch && state->m17_gnss_meta_admitted[0] == 0x12U;
}

int
main(void) {
    dsd_state* state = (dsd_state*)calloc(1U, sizeof(*state));
    assert(state != NULL);
    dsd_call_observation observation = {0};
    observation.protocol = 35;
    observation.slot = 0U;
    observation.kind = DSD_CALL_KIND_GROUP_VOICE;
    observation.ota_target_id = 100U;
    observation.observed_m = 1.0;

    dsd_call_state_test_alloc_fail_after(0);
    assert(dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN) == -1);
    assert(dsd_state_ext_get(state, DSD_STATE_EXT_CORE_CALL_STATE) == NULL);

    dsd_call_state_test_alloc_reset();
    assert(dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN) == 1);
    dsd_call_snapshot snapshot;
    assert(dsd_call_state_get(state, 0U, &snapshot) == 1);
    assert(snapshot.epoch == 1U);
    assert(dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN) == 1);
    assert(dsd_call_state_get(state, 0U, &snapshot) == 1 && snapshot.epoch == 2U);
    // The run's protocols key their records to its last call.
    set_epoch_records(state, 2U);

    // The next engine run on this state (its cleanup frees every extension) goes on from the last run's epochs: its
    // first call is epoch 3, so no record the last run left names it. The call state touched none of them.
    dsd_state_ext_free_all(state);
    assert(dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN) == 1);
    assert(dsd_call_state_get(state, 0U, &snapshot) == 1 && snapshot.epoch == 3U);
    assert(epoch_records_set(state, 2U));
    assert(dsd_call_snapshot_transmission(&snapshot) != state->dmr_lc_attribution[0].transmission);
    // The other slot's sequence goes on from its own floor (none yet).
    observation.slot = 1U;
    assert(dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN) == 1);
    assert(dsd_call_state_get(state, 1U, &snapshot) == 1 && snapshot.epoch == 1U);
    observation.slot = 0U;

    dsd_state* clone = (dsd_state*)calloc(1U, sizeof(*clone));
    assert(clone != NULL);
    dsd_call_state_test_alloc_fail_after(0);
    assert(dsd_call_state_copy_to_state(clone, state) == -1);
    assert(dsd_state_ext_get(clone, DSD_STATE_EXT_CORE_CALL_STATE) == NULL);
    dsd_call_state_test_alloc_reset();

    // A copy or a restore carries the source's epoch sequence, and raises the destination's floor with it: a later
    // extension of the destination goes on from there.
    clone->dmr_lc_attribution[1].transmission = 1U;
    clone->dmr_lc_attribution[1].source = 1001U;
    assert(dsd_call_state_copy_to_state(clone, state) == 1);
    assert(clone->dmr_lc_attribution[1].transmission == 1U && clone->dmr_lc_attribution[1].source == 1001U);
    assert(clone->call_epoch_floor[0] == 3U && clone->call_epoch_floor[1] == 1U);
    dsd_call_state_snapshot calls;
    assert(dsd_call_state_copy_snapshot(state, &calls) >= 0);
    dsd_state* restored = (dsd_state*)calloc(1U, sizeof(*restored));
    assert(restored != NULL);
    set_epoch_records(restored, 3U);
    assert(dsd_call_state_restore_snapshot(restored, &calls) >= 0);
    assert(dsd_state_ext_get(restored, DSD_STATE_EXT_CORE_CALL_STATE) != NULL);
    assert(epoch_records_set(restored, 3U));
    assert(restored->call_epoch_floor[0] == 3U);
    dsd_state_ext_free_all(restored);
    assert(dsd_call_state_observe(restored, &observation, DSD_CALL_BOUNDARY_BEGIN) == 1);
    assert(dsd_call_state_get(restored, 0U, &snapshot) == 1 && snapshot.epoch == 4U);
    // Every carrier change (a carrier boundary, a trunk-scan target switch, a scan step, a user's move) forgets it.
    dsd_state_note_carrier_change(restored);
    assert(restored->dmr_lc_attribution[0].transmission == 0U && restored->dmr_lc_attribution[0].source == 0U);
    dsd_state_ext_free_all(restored);
    free(restored);

    dsd_state_ext_free_all(state);
    dsd_state_ext_free_all(clone);
    free(clone);
    free(state);
    return 0;
}
