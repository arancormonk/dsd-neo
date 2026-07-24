// SPDX-License-Identifier: GPL-3.0-or-later

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include "dsd-neo/core/state_fwd.h"

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
    assert(dsd_call_state_test_set_epoch(state, 0U, UINT64_MAX) == 1);
    assert(dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN) == 1);
    assert(dsd_call_state_get(state, 0U, &snapshot) == 1);
    assert(snapshot.epoch == 1U);

    dsd_state* clone = (dsd_state*)calloc(1U, sizeof(*clone));
    assert(clone != NULL);
    dsd_call_state_test_alloc_fail_after(0);
    assert(dsd_call_state_copy_to_state(clone, state) == -1);
    assert(dsd_state_ext_get(clone, DSD_STATE_EXT_CORE_CALL_STATE) == NULL);
    dsd_call_state_test_alloc_reset();

    dsd_state_ext_free_all(state);
    dsd_state_ext_free_all(clone);
    free(clone);
    free(state);
    return 0;
}
