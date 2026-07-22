// SPDX-License-Identifier: GPL-3.0-or-later

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

static dsd_call_observation
group_call(uint8_t slot, uint32_t target, uint32_t source, double observed_m) {
    dsd_call_observation observation = {0};
    observation.protocol = 35;
    observation.slot = slot;
    observation.kind = DSD_CALL_KIND_GROUP_VOICE;
    observation.ota_target_id = target;
    observation.policy_target_id = target;
    observation.ota_source_id = source;
    observation.channel = 0x1234U + slot;
    observation.frequency_hz = 851000000 + slot * 12500;
    observation.service_options = 3U;
    observation.priority = 3U;
    observation.has_service_metadata = 1U;
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

    observation.ota_target_id = observation.policy_target_id = 100U;
    observation.ota_source_id = 200U;
    observation.observed_m = 2.0;
    assert(dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_CONTINUE) == 0);
    assert(dsd_call_state_get(state, 0, &snapshot) == 1);
    assert(snapshot.epoch == 1U);
    assert(snapshot.ota_target_id == 100U);
    assert(snapshot.ota_source_id == 200U);

    observation.ota_target_id = observation.policy_target_id = 101U;
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
    dsd_call_observation activity0 = group_call(0U, 100U, 9U, 1.0);
    dsd_call_observation activity1 = group_call(1U, 200U, 10U, 2.0);
    assert(dsd_recent_activity_publish(state, 0U, &activity0, "Active Ch: 1234 TG: 100 SRC: 9; ", 2500U) == 1);
    assert(dsd_recent_activity_publish(state, 1U, &activity1, "Active Ch: 5678 TG: 200; ", 2000U) == 1);

    dsd_recent_activity_snapshot recent;
    assert(dsd_recent_activity_copy_snapshot(state, &recent) == 1);
    assert(recent.entries[0].updated_m_ms == 2500U);
    assert(recent.entries[1].updated_m_ms == 2000U);
    assert(recent.entries[0].observation.ota_target_id == 100U);
    assert(recent.entries[0].observation.ota_source_id == 9U);
    assert(strstr(recent.entries[0].notice, "SRC: 9") != NULL);

    dsd_recent_activity_transaction transaction;
    assert(dsd_recent_activity_save(state, 1, &transaction) == 1);
    activity1.ota_target_id = 201U;
    assert(dsd_recent_activity_publish(state, 1U, &activity1, "replacement", 3000U) == 1);
    assert(dsd_recent_activity_restore(state, &transaction) == 1);
    assert(strcmp(transaction.entry.notice, "Active Ch: 5678 TG: 200; ") == 0);
    assert(dsd_recent_activity_copy_snapshot(state, &recent) == 1);
    assert(recent.entries[1].observation.ota_target_id == 200U);

    activity1.ota_target_id = 200U;
    assert(dsd_recent_activity_publish(state, 1U, &activity1, "Active Ch: 5678 TG: 200; ", 4000U) == 1);
    assert(dsd_recent_activity_expire(state, 5501U, 3000U) == 1);
    assert(dsd_recent_activity_copy_snapshot(state, &recent) == 1);
    assert(recent.entries[0].updated_m_ms == 0U);
    assert(recent.entries[1].updated_m_ms == 4000U);

    const uint64_t entry1_before_refresh = recent.entries[1].updated_m_ms;
    const uint64_t entry0_before_refresh = recent.entries[0].updated_m_ms;
    assert(dsd_recent_activity_publish(state, 1U, &activity1, "Active Ch: 5678 TG: 200; ", 5000U) == 1);
    assert(dsd_recent_activity_copy_snapshot(state, &recent) == 1);
    assert(recent.entries[1].updated_m_ms > entry1_before_refresh);
    assert(recent.entries[0].updated_m_ms == entry0_before_refresh);

    const dsd_recent_activity_snapshot saved = recent;
    assert(dsd_recent_activity_clear_all(state) == 1);
    assert(dsd_recent_activity_restore_snapshot(state, &saved) == 1);
    assert(dsd_recent_activity_copy_snapshot(state, &recent) == 1);
    assert(recent.entries[1].observation.ota_target_id == 200U);

    assert(dsd_recent_activity_clear(state, 1) == 1);
    assert(dsd_recent_activity_copy_snapshot(state, &recent) == 1);
    assert(recent.entries[1].updated_m_ms == 0U);
    assert(dsd_recent_activity_clear_all(state) == 1);
    assert(dsd_recent_activity_copy_snapshot(state, &recent) == 1);
    assert(recent.entries[0].updated_m_ms == 0U);
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

static void
test_voice_specialization_and_sparse_metadata(void) {
    dsd_state* state = (dsd_state*)calloc(1U, sizeof(*state));
    assert(state != NULL);

    dsd_call_observation observation = {
        .protocol = DSD_SYNC_DMR_BS_VOICE_POS,
        .slot = 0U,
        .kind = DSD_CALL_KIND_VOICE,
        .observed_m = 1.0,
    };
    assert(dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN) == 1);

    dsd_call_snapshot snapshot;
    assert(dsd_call_state_get(state, 0U, &snapshot) == 1);
    const uint64_t voice_epoch = snapshot.epoch;
    assert(snapshot.has_service_metadata == 0U);

    observation.kind = DSD_CALL_KIND_GROUP_VOICE;
    observation.ota_target_id = 100U;
    observation.policy_target_id = 100U;
    observation.service_options = 0xC5U;
    observation.emergency = 1U;
    observation.priority = 5U;
    observation.has_service_metadata = 1U;
    observation.observed_m = 2.0;
    assert(dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_CONTINUE) == 0);
    assert(dsd_call_state_get(state, 0U, &snapshot) == 1);
    assert(snapshot.epoch == voice_epoch);
    assert(snapshot.kind == DSD_CALL_KIND_GROUP_VOICE);
    assert(snapshot.has_service_metadata == 1U);

    observation = (dsd_call_observation){
        .protocol = DSD_SYNC_DMR_BS_VOICE_POS,
        .slot = 0U,
        .kind = DSD_CALL_KIND_GROUP_VOICE,
        .ota_target_id = 100U,
        .policy_target_id = 100U,
        .ota_source_id = 200U,
        .observed_m = 3.0,
    };
    assert(dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_CONTINUE) == 0);
    assert(dsd_call_state_get(state, 0U, &snapshot) == 1);
    assert(snapshot.epoch == voice_epoch);
    assert(snapshot.ota_source_id == 200U);
    assert(snapshot.service_options == 0xC5U);
    assert(snapshot.emergency == 1U);
    assert(snapshot.priority == 5U);

    observation.kind = DSD_CALL_KIND_VOICE;
    observation.ota_target_id = 0U;
    observation.policy_target_id = 0U;
    observation.ota_source_id = 0U;
    observation.observed_m = 4.0;
    assert(dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_CONTINUE) == 0);
    assert(dsd_call_state_get(state, 0U, &snapshot) == 1);
    assert(snapshot.epoch == voice_epoch);
    assert(snapshot.kind == DSD_CALL_KIND_GROUP_VOICE);
    assert(snapshot.service_options == 0xC5U);

    observation.kind = DSD_CALL_KIND_GROUP_VOICE;
    observation.has_service_metadata = 1U;
    observation.observed_m = 5.0;
    assert(dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_CONTINUE) == 0);
    assert(dsd_call_state_get(state, 0U, &snapshot) == 1);
    assert(snapshot.service_options == 0U);
    assert(snapshot.emergency == 0U);
    assert(snapshot.priority == 0U);

    dsd_state_ext_free_all(state);
    free(state);
}

static void
test_64bit_text_and_protocol_family_continuity(void) {
    dsd_state* state = (dsd_state*)calloc(1U, sizeof(*state));
    assert(state != NULL);

    dsd_call_observation observation = {0};
    observation.protocol = DSD_SYNC_M17_LSF_POS;
    observation.slot = 0U;
    observation.kind = DSD_CALL_KIND_VOICE;
    observation.ota_target_id = UINT64_C(0xFFFFFFFFFFFF);
    observation.policy_target_id = 77U;
    observation.ota_source_id = UINT64_C(0x123456789ABC);
    DSD_MEMCPY(observation.source_text, "  SRC\tCALL  ", 13U);
    DSD_MEMCPY(observation.target_text, " BROADCAST ", 11U);
    DSD_MEMCPY(observation.route_text[0], " RPT1\x01 ", 7U);
    DSD_MEMCPY(observation.route_text[1], "  RPT2  ROUTE ", 14U);
    observation.observed_m = 1.0;
    assert(dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN) == 1);

    dsd_call_snapshot snapshot;
    assert(dsd_call_state_get(state, 0U, &snapshot) == 1);
    assert(snapshot.epoch == 1U);
    assert(snapshot.ota_target_id == UINT64_C(0xFFFFFFFFFFFF));
    assert(snapshot.ota_source_id == UINT64_C(0x123456789ABC));
    assert(strcmp(snapshot.source_text, "SRC CALL") == 0);
    assert(strcmp(snapshot.target_text, "BROADCAST") == 0);
    assert(strcmp(snapshot.route_text[0], "RPT1_") == 0);
    assert(strcmp(snapshot.route_text[1], "RPT2 ROUTE") == 0);

    observation.protocol = DSD_SYNC_M17_STR_NEG;
    observation.policy_target_id = 88U;
    observation.observed_m = 2.0;
    assert(dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_CONTINUE) == 0);
    assert(dsd_call_state_get(state, 0U, &snapshot) == 1);
    assert(snapshot.epoch == 1U);
    assert(snapshot.policy_target_id == 88U);

    assert(dsd_call_state_enrich_text(state, 0U, snapshot.epoch, NULL, NULL, "NEW RPT1", "NEW RPT2", 2.5) == 1);
    assert(dsd_call_state_get(state, 0U, &snapshot) == 1);
    assert(snapshot.epoch == 1U);
    assert(strcmp(snapshot.route_text[0], "NEW RPT1") == 0);
    assert(dsd_call_state_enrich_text(state, 0U, snapshot.epoch + 1U, "STALE", NULL, NULL, NULL, 2.6) == 0);
    assert(dsd_call_state_get(state, 0U, &snapshot) == 1);
    assert(strcmp(snapshot.source_text, "SRC CALL") == 0);

    observation = group_call(0U, 100U, 200U, 3.0);
    observation.protocol = DSD_SYNC_DMR_BS_VOICE_POS;
    assert(dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN) == 1);
    assert(dsd_call_state_get(state, 0U, &snapshot) == 1);
    const uint64_t dmr_epoch = snapshot.epoch;
    observation.protocol = DSD_SYNC_DMR_BS_VOICE_NEG;
    observation.observed_m = 3.1;
    assert(dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_CONTINUE) == 0);
    assert(dsd_call_state_get(state, 0U, &snapshot) == 1);
    assert(snapshot.epoch == dmr_epoch);

    observation.protocol = DSD_SYNC_EDACS_POS;
    observation.observed_m = 4.0;
    assert(dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN) == 1);
    assert(dsd_call_state_get(state, 0U, &snapshot) == 1);
    const uint64_t edacs_epoch = snapshot.epoch;
    observation.protocol = DSD_SYNC_PROVOICE_NEG;
    observation.observed_m = 4.1;
    assert(dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_CONTINUE) == 0);
    assert(dsd_call_state_get(state, 0U, &snapshot) == 1);
    assert(snapshot.epoch == edacs_epoch);

    dsd_call_state_snapshot saved;
    assert(dsd_call_state_copy_snapshot(state, &saved) == 1);
    assert(dsd_call_state_end(state, 0U, 5.0) == 1);
    assert(dsd_call_state_restore_snapshot(state, &saved) == 1);
    assert(dsd_call_state_get(state, 0U, &snapshot) == 1);
    assert(snapshot.phase == DSD_CALL_PHASE_ACTIVE);
    assert(snapshot.epoch == edacs_epoch);

    dsd_state_ext_free_all(state);
    free(state);
}

static void
test_positive_p25p1_protocol_observation(void) {
    dsd_state* state = (dsd_state*)calloc(1U, sizeof(*state));
    assert(state != NULL);

    dsd_call_observation observation = group_call(0U, 700U, 800U, 1.0);
    observation.protocol = DSD_SYNC_P25P2_POS;
    assert(dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN) == 1);

    dsd_call_snapshot snapshot;
    assert(dsd_call_state_get(state, 0U, &snapshot) == 1);
    assert(snapshot.epoch == 1U);
    assert(snapshot.protocol == DSD_SYNC_P25P2_POS);

    observation.protocol = DSD_SYNC_P25P1_POS;
    observation.observed_m = 2.0;
    assert(dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_CONTINUE) == 1);
    assert(dsd_call_state_get(state, 0U, &snapshot) == 1);
    assert(snapshot.epoch == 2U);
    assert(snapshot.protocol == DSD_SYNC_P25P1_POS);

    observation.protocol = DSD_SYNC_NONE;
    observation.observed_m = 3.0;
    assert(dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_CONTINUE) == 0);
    assert(dsd_call_state_get(state, 0U, &snapshot) == 1);
    assert(snapshot.epoch == 2U);
    assert(snapshot.protocol == DSD_SYNC_P25P1_POS);

    dsd_state_ext_free_all(state);
    free(state);
}

int
main(void) {
    dsd_state* state = (dsd_state*)calloc(1U, sizeof(*state));
    assert(state != NULL);
    test_epochs_and_late_identity(state);
    test_crypto_and_slot_isolation(state);
    test_recent_activity(state);
    test_snapshot_clone(state);
    test_voice_specialization_and_sparse_metadata();
    test_64bit_text_and_protocol_family_continuity();
    test_positive_p25p1_protocol_observation();
    dsd_state_ext_free_all(state);
    free(state);
    return 0;
}
