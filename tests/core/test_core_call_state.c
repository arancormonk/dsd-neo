// SPDX-License-Identifier: GPL-3.0-or-later

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "dsd-neo/core/state_fwd.h"

static dsd_call_observation
group_call(uint8_t slot, uint32_t target, uint32_t source, double observed_m) {
    dsd_call_observation observation = {0};
    observation.protocol = 35;
    observation.slot = slot;
    observation.kind = DSD_CALL_KIND_GROUP_VOICE;
    observation.ota_target_id = target;
    observation.policy_target_id = target;
    observation.group_id = target;
    observation.source_id = source;
    observation.channel = 0x1234U + slot;
    observation.frequency_hz = 851000000 + slot * 12500;
    observation.service_options = 3U;
    observation.priority = 3U;
    observation.observed_m = observed_m;
    return observation;
}

static void
test_epochs_and_late_identity(dsd_state* state) {
    dsd_call_observation observation = group_call(0, 0, 0, 1.0);
    assert(dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN) == 1);

    dsd_call_snapshot snapshot;
    assert(dsd_call_state_get(state, 0, &snapshot) == 1);
    assert(snapshot.epoch == 1U);
    assert(snapshot.phase == DSD_CALL_PHASE_ACTIVE);

    observation.ota_target_id = observation.group_id = observation.policy_target_id = 100U;
    observation.source_id = 200U;
    observation.observed_m = 2.0;
    assert(dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_CONTINUE) == 0);
    assert(dsd_call_state_get(state, 0, &snapshot) == 1);
    assert(snapshot.epoch == 1U);
    assert(snapshot.ota_target_id == 100U);
    assert(snapshot.source_id == 200U);

    observation.ota_target_id = observation.group_id = observation.policy_target_id = 101U;
    observation.observed_m = 3.0;
    assert(dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_CONTINUE) == 1);
    assert(dsd_call_state_get(state, 0, &snapshot) == 1);
    assert(snapshot.epoch == 2U);

    assert(dsd_call_state_end(state, 0, 4.0) == 1);
    assert(dsd_call_state_end(state, 0, 4.1) == 0);
    assert(dsd_call_state_get(state, 0, &snapshot) == 1);
    assert(snapshot.phase == DSD_CALL_PHASE_ENDED);

    assert(dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN) == 1);
    assert(dsd_call_state_get(state, 0, &snapshot) == 1);
    assert(snapshot.epoch == 3U);
}

static void
test_crypto_and_slot_isolation(dsd_state* state) {
    dsd_call_observation right = group_call(1, 500U, 600U, 5.0);
    assert(dsd_call_state_observe(state, &right, DSD_CALL_BOUNDARY_BEGIN) == 1);

    dsd_call_crypto_update crypto = {
        .classification = DSD_CALL_CRYPTO_DECRYPTABLE,
        .algid = 0x84U,
        .kid = 0x1234U,
        .mi = UINT64_C(0x1122334455667788),
        .audio_permitted = 1U,
        .observed_m = 6.0,
    };
    assert(dsd_call_state_update_crypto(state, 1, &crypto) == 1);
    assert(dsd_call_state_update_media(state, 1, 1, 6.5) == 1);

    dsd_call_state_snapshot all;
    assert(dsd_call_state_copy_snapshot(state, &all) == 1);
    assert(all.slots[0].phase == DSD_CALL_PHASE_ACTIVE);
    assert(all.slots[1].phase == DSD_CALL_PHASE_ACTIVE);
    assert(all.slots[1].crypto == DSD_CALL_CRYPTO_DECRYPTABLE);
    assert(all.slots[1].media_active == 1U);

    assert(dsd_call_state_end(state, 1, 7.0) == 1);
    crypto.classification = DSD_CALL_CRYPTO_CLEAR;
    crypto.observed_m = 7.5;
    assert(dsd_call_state_update_crypto(state, 1, &crypto) == 0);
    assert(dsd_call_state_get(state, 1, &all.slots[1]) == 1);
    assert(all.slots[1].crypto == DSD_CALL_CRYPTO_DECRYPTABLE);
    assert(dsd_call_state_get(state, 0, &all.slots[0]) == 1);
    assert(all.slots[0].phase == DSD_CALL_PHASE_ACTIVE);
}

static void
test_recent_activity(dsd_state* state) {
    assert(dsd_recent_activity_set_at(state, 0, "Active Ch: 1234 TG: 100; ", 1000U) == 1);
    assert(dsd_recent_activity_set_at(state, 1, "Active Ch: 5678 TG: 200; ", 2000U) == 1);
    assert(dsd_recent_activity_append_at(state, 0, "SRC: 9; ", 2500U) == 1);

    dsd_recent_activity_snapshot recent;
    assert(dsd_recent_activity_copy_snapshot(state, &recent) == 1);
    assert(recent.entries[0].updated_m_ms == 2500U);
    assert(recent.entries[1].updated_m_ms == 2000U);
    assert(strstr(recent.entries[0].text, "SRC: 9") != NULL);
    assert(strcmp(state->active_channel[0], recent.entries[0].text) == 0);

    dsd_recent_activity_transaction transaction;
    assert(dsd_recent_activity_save(state, 1, &transaction) == 1);
    assert(dsd_recent_activity_set_at(state, 1, "replacement", 3000U) == 1);
    assert(dsd_recent_activity_restore(state, &transaction) == 1);
    assert(strcmp(state->active_channel[1], "Active Ch: 5678 TG: 200; ") == 0);

    assert(dsd_recent_activity_set_at(state, 1, "Active Ch: 5678 TG: 200; ", 4000U) == 1);
    assert(dsd_recent_activity_expire(state, 5501U, 3000U) == 1);
    assert(state->active_channel[0][0] == '\0');
    assert(strcmp(state->active_channel[1], "Active Ch: 5678 TG: 200; ") == 0);

    assert(dsd_recent_activity_copy_snapshot(state, &recent) == 1);
    const uint64_t entry1_before_refresh = recent.entries[1].updated_m_ms;
    const uint64_t entry0_before_refresh = recent.entries[0].updated_m_ms;
    assert(dsd_recent_activity_sync_legacy_entry(state, 1U) == 1);
    assert(dsd_recent_activity_copy_snapshot(state, &recent) == 1);
    assert(recent.entries[1].updated_m_ms > entry1_before_refresh);
    assert(recent.entries[0].updated_m_ms == entry0_before_refresh);

    assert(dsd_recent_activity_clear(state, 1) == 1);
    assert(state->active_channel[1][0] == '\0');
    assert(dsd_recent_activity_clear_all(state) == 1);
    assert(state->active_channel[0][0] == '\0');
}

static void
test_snapshot_clone(dsd_state* state) {
    dsd_state* clone = (dsd_state*)calloc(1U, sizeof(*clone));
    assert(clone != NULL);
    assert(dsd_call_state_copy_to_state(clone, state) == 1);
    dsd_call_state_snapshot snapshot;
    assert(dsd_call_state_copy_snapshot(clone, &snapshot) == 1);
    assert(snapshot.slots[0].epoch == 3U);
    assert(snapshot.slots[1].crypto == DSD_CALL_CRYPTO_DECRYPTABLE);
    dsd_state_ext_free_all(clone);
    free(clone);
}

int
main(void) {
    dsd_state* state = (dsd_state*)calloc(1U, sizeof(*state));
    assert(state != NULL);
    test_epochs_and_late_identity(state);
    test_crypto_and_slot_isolation(state);
    test_recent_activity(state);
    test_snapshot_clone(state);
    dsd_state_ext_free_all(state);
    free(state);
    return 0;
}
