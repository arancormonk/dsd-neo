// SPDX-License-Identifier: GPL-3.0-or-later

#include <dsd-neo/core/call_channel.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/received_identity.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/runtime/decode_clock.h>

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

static int
double_near(double actual, double expected) {
    return fabs(actual - expected) < 1e-9;
}

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
test_retained_crypto_update(void) {
    dsd_state* state = (dsd_state*)calloc(1U, sizeof(*state));
    assert(state != NULL);
    const dsd_call_observation observation = group_call(0U, 500U, 600U, 1.0);
    assert(dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN) == 1);
    assert(dsd_call_state_end(state, 0U, 2.0) == 1);

    const dsd_call_crypto_update crypto = {
        .classification = DSD_CALL_CRYPTO_ENCRYPTED,
        .algid = 0x84U,
        .kid = 0x1234U,
        .mi = UINT64_C(0x1122334455667788),
        .audio_permitted = 0U,
        .observed_m = 3.0,
    };
    assert(dsd_call_state_update_retained_crypto(state, 0U, &crypto) == 1);

    dsd_call_snapshot snapshot;
    assert(dsd_call_state_get(state, 0U, &snapshot) == 1);
    assert(snapshot.phase == DSD_CALL_PHASE_ENDED);
    assert(snapshot.crypto == DSD_CALL_CRYPTO_ENCRYPTED);
    assert(snapshot.algid == 0x84U);
    assert(snapshot.kid == 0x1234U);
    assert(snapshot.mi == UINT64_C(0x1122334455667788));
    assert(snapshot.audio_permitted == 0U);

    // An ended call keeps the updated_m it had when it ended. Carrier repeats
    // re-describe it for as long as the receiver stays on the frequency, and
    // moving updated_m past ended_m makes a finished call look freshly updated
    // to consumers that order by recency.
    assert(double_near(snapshot.ended_m, 2.0));
    assert(snapshot.updated_m <= snapshot.ended_m);

    // Re-applying the identical crypto must not churn the revision: pollers
    // would otherwise see one revision bump per repeat with nothing to show.
    const uint64_t settled_revision = snapshot.revision;
    assert(dsd_call_state_update_retained_crypto(state, 0U, &crypto) == 1);
    assert(dsd_call_state_get(state, 0U, &snapshot) == 1);
    assert(snapshot.revision == settled_revision);

    // A genuine change still lands and still bumps the revision.
    dsd_call_crypto_update rekey = crypto;
    rekey.kid = 0x4321U;
    rekey.observed_m = 9.0;
    assert(dsd_call_state_update_retained_crypto(state, 0U, &rekey) == 1);
    assert(dsd_call_state_get(state, 0U, &snapshot) == 1);
    assert(snapshot.kid == 0x4321U);
    assert(snapshot.revision != settled_revision);
    assert(snapshot.updated_m <= snapshot.ended_m);

    dsd_state_ext_free_all(state);
    free(state);
}

static void
test_media_timestamps(void) {
    dsd_state* state = (dsd_state*)calloc(1U, sizeof(*state));
    assert(state != NULL);
    dsd_call_observation observation = group_call(0U, 500U, 600U, 1.0);
    assert(dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN) == 1);

    dsd_call_snapshot snapshot;
    assert(dsd_call_state_get(state, 0U, &snapshot) == 1);
    assert(double_near(snapshot.media_started_m, 0.0));
    assert(double_near(snapshot.media_updated_m, 0.0));
    assert(dsd_call_state_update_media(state, 0U, 1, 1.1) == 1);
    assert(dsd_call_state_update_media(state, 0U, 0, 1.2) == 1);
    assert(dsd_call_state_update_media(state, 0U, 1, 1.3) == 1);

    observation.observed_m = 1.4;
    assert(dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_CONTINUE) == 0);
    const dsd_call_crypto_update crypto = {
        .classification = DSD_CALL_CRYPTO_CLEAR,
        .observed_m = 1.5,
    };
    assert(dsd_call_state_update_crypto(state, 0U, &crypto) == 1);
    assert(dsd_call_state_end_ex(state, 0U, 1.6, DSD_CALL_END_TERMINATOR) == 1);
    assert(dsd_call_state_get(state, 0U, &snapshot) == 1);
    assert(snapshot.media_active == 0U);
    assert(double_near(snapshot.media_started_m, 1.1));
    assert(double_near(snapshot.media_updated_m, 1.3));

    dsd_call_state_snapshot saved;
    assert(dsd_call_state_copy_snapshot(state, &saved) == 1);
    observation.observed_m = 2.0;
    assert(dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN) == 1);
    assert(dsd_call_state_get(state, 0U, &snapshot) == 1);
    assert(double_near(snapshot.media_started_m, 0.0));
    assert(double_near(snapshot.media_updated_m, 0.0));
    assert(dsd_call_state_restore_snapshot(state, &saved) == 1);
    assert(dsd_call_state_get(state, 0U, &snapshot) == 1);
    assert(double_near(snapshot.media_started_m, 1.1));
    assert(double_near(snapshot.media_updated_m, 1.3));

    dsd_state_ext_free_all(state);
    free(state);
}

static void
test_reacquisition_carries_media_timestamps(void) {
    static const dsd_call_end_reason reasons[] = {
        DSD_CALL_END_SYNC_LOSS,
        DSD_CALL_END_UNVERIFIED_TERMINATOR,
    };
    for (size_t i = 0U; i < sizeof(reasons) / sizeof(reasons[0]); i++) {
        dsd_state* state = (dsd_state*)calloc(1U, sizeof(*state));
        assert(state != NULL);
        dsd_call_observation observation = group_call(0U, 500U, 600U, 10.0);
        assert(dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN) == 1);
        assert(dsd_call_state_update_media(state, 0U, 1, 10.05) == 1);
        assert(dsd_call_state_update_media(state, 0U, 1, 10.15) == 1);
        assert(dsd_call_state_end_ex(state, 0U, 10.2, reasons[i]) == 1);

        observation.observed_m = 10.3;
        if (reasons[i] == DSD_CALL_END_UNVERIFIED_TERMINATOR) {
            observation.ota_target_id = 0U;
            observation.policy_target_id = 0U;
            observation.ota_source_id = 0U;
        }
        assert(dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN) == 1);
        dsd_call_snapshot snapshot;
        assert(dsd_call_state_get(state, 0U, &snapshot) == 1);
        assert(snapshot.phase == DSD_CALL_PHASE_ACTIVE);
        assert(double_near(snapshot.started_m, 10.3));
        assert(double_near(snapshot.media_started_m, 10.05));
        assert(double_near(snapshot.media_updated_m, 10.15));
        assert(snapshot.media_active == 0U);
        assert(dsd_call_state_update_media(state, 0U, 1, 10.35) == 1);
        assert(dsd_call_state_get(state, 0U, &snapshot) == 1);
        assert(double_near(snapshot.media_started_m, 10.05));
        assert(double_near(snapshot.media_updated_m, 10.35));

        dsd_state_ext_free_all(state);
        free(state);
    }
}

// A DMR call's channel is named once, by the grant or move that tuned its follow: a later observation naming another
// channel or kind never replaces it, one naming none never clears it, and a kind applies only with a channel. A
// reacquired epoch keeps both. Another protocol's channel (kind NONE) is replaced as it always was.
static void
test_channel_fill_only(void) {
    dsd_state* state = (dsd_state*)calloc(1U, sizeof(*state));
    assert(state != NULL);
    dsd_call_snapshot snapshot;
    dsd_call_observation dmr = group_call(0U, 500U, 600U, 1.0);
    dmr.channel = 0U;
    dmr.channel_kind = DSD_CALL_CHANNEL_LPCN;
    assert(dsd_call_state_observe(state, &dmr, DSD_CALL_BOUNDARY_BEGIN) == 1);
    assert(dsd_call_state_get(state, 0U, &snapshot) == 1);
    assert(snapshot.channel == 0U && snapshot.channel_kind == DSD_CALL_CHANNEL_NONE);

    dmr.channel = 12U;
    dmr.observed_m = 1.1;
    assert(dsd_call_state_observe(state, &dmr, DSD_CALL_BOUNDARY_CONTINUE) == 0);
    assert(dsd_call_state_get(state, 0U, &snapshot) == 1);
    assert(snapshot.channel == 12U && snapshot.channel_kind == DSD_CALL_CHANNEL_LPCN);

    dmr.channel = 37U;
    dmr.channel_kind = DSD_CALL_CHANNEL_LSN;
    dmr.observed_m = 1.2;
    assert(dsd_call_state_observe(state, &dmr, DSD_CALL_BOUNDARY_CONTINUE) == 0);
    dmr.channel = 0U;
    dmr.channel_kind = DSD_CALL_CHANNEL_NONE;
    dmr.observed_m = 1.3;
    assert(dsd_call_state_observe(state, &dmr, DSD_CALL_BOUNDARY_CONTINUE) == 0);
    assert(dsd_call_state_get(state, 0U, &snapshot) == 1);
    assert(snapshot.channel == 12U && snapshot.channel_kind == DSD_CALL_CHANNEL_LPCN);

    // A sync loss and the same call's re-entry inside the window: the reopened epoch keeps the channel and its kind,
    // whatever channel the re-entry names.
    assert(dsd_call_state_end_ex(state, 0U, 1.4, DSD_CALL_END_SYNC_LOSS) == 1);
    dsd_call_observation reentry = group_call(0U, 500U, 600U, 1.6);
    assert(dsd_call_state_observe(state, &reentry, DSD_CALL_BOUNDARY_BEGIN) == 1);
    assert(dsd_call_state_get(state, 0U, &snapshot) == 1);
    assert(snapshot.phase == DSD_CALL_PHASE_ACTIVE && snapshot.ota_target_id == 500U);
    assert(snapshot.channel == 12U && snapshot.channel_kind == DSD_CALL_CHANNEL_LPCN);

    dsd_call_observation other = group_call(1U, 700U, 800U, 2.0);
    assert(dsd_call_state_observe(state, &other, DSD_CALL_BOUNDARY_BEGIN) == 1);
    other.channel = 0x2000U;
    other.observed_m = 2.1;
    assert(dsd_call_state_observe(state, &other, DSD_CALL_BOUNDARY_CONTINUE) == 0);
    assert(dsd_call_state_get(state, 1U, &snapshot) == 1);
    assert(snapshot.channel == 0x2000U && snapshot.channel_kind == DSD_CALL_CHANNEL_NONE);

    dsd_state_ext_free_all(state);
    free(state);
}

// A decode instant far from any platform clock reading (1e9 s), so a decision that read the
// platform clock instead of the decode clock could not land on the expected side of its window.
#define CALL_STATE_TEST_DECODE_T0_NS 1000000000000000000ULL
#define CALL_STATE_TEST_DECODE_T0_S  1000000000.0

static void
decode_clock_at_ms(uint64_t offset_ms) {
    dsd_decode_clock_test_set_ns(CALL_STATE_TEST_DECODE_T0_NS + (offset_ms * 1000000ULL));
}

static dsd_call_observation
identityless_voice(uint8_t slot) {
    dsd_call_observation observation = {0};
    observation.protocol = 35;
    observation.slot = slot;
    observation.kind = DSD_CALL_KIND_VOICE;
    return observation;
}

// The reacquisition window is judged on the decode clock, with the real clock left alone. The
// engine's sync-loss end and the vocoder's identity-less media mark both pass no observation time,
// so the store's own fallback stamps them; a protocol end that does pass its time stamps it from
// the same decode clock, and the fallback must agree with it.
static void
test_reacquire_runs_on_the_decode_clock(void) {
    dsd_state* state = (dsd_state*)calloc(1U, sizeof(*state));
    assert(state != NULL);
    dsd_decode_clock_use_test(CALL_STATE_TEST_DECODE_T0_NS);

    // Both stamps from the fallback: 0.25 s after the fade is inside the 0.5 s window.
    dsd_call_observation call = group_call(0U, 500U, 600U, 0.0);
    assert(dsd_call_state_observe(state, &call, DSD_CALL_BOUNDARY_BEGIN) == 1);
    dsd_call_snapshot snapshot;
    assert(dsd_call_state_get(state, 0U, &snapshot) == 1);
    const uint64_t first_epoch = snapshot.epoch;
    assert(snapshot.transmission_epoch == first_epoch && dsd_call_snapshot_transmission(&snapshot) == first_epoch);
    decode_clock_at_ms(1000U);
    assert(dsd_call_state_end_ex(state, 0U, 0.0, DSD_CALL_END_SYNC_LOSS) == 1);
    decode_clock_at_ms(1250U);
    dsd_call_observation reentry = identityless_voice(0U);
    assert(dsd_call_state_observe(state, &reentry, DSD_CALL_BOUNDARY_BEGIN) == 1);
    assert(dsd_call_state_get(state, 0U, &snapshot) == 1);
    assert(snapshot.phase == DSD_CALL_PHASE_ACTIVE);
    assert(snapshot.ota_target_id == 500U);
    assert(snapshot.ota_source_id == 600U);
    // The reopened epoch is a new one, yet the same transmission: state bound to the call stays bound to it.
    assert(snapshot.epoch != first_epoch && dsd_call_snapshot_transmission(&snapshot) == first_epoch);

    // One decode second after the fade is outside it, however little real time passed.
    decode_clock_at_ms(10000U);
    assert(dsd_call_state_end_ex(state, 0U, 0.0, DSD_CALL_END_SYNC_LOSS) == 1);
    decode_clock_at_ms(11000U);
    assert(dsd_call_state_observe(state, &reentry, DSD_CALL_BOUNDARY_BEGIN) == 1);
    assert(dsd_call_state_get(state, 0U, &snapshot) == 1);
    assert(snapshot.phase == DSD_CALL_PHASE_ACTIVE);
    assert(snapshot.ota_target_id == 0U);
    assert(snapshot.ota_source_id == 0U);
    // A new transmission is its own.
    assert(dsd_call_snapshot_transmission(&snapshot) == snapshot.epoch && snapshot.epoch != first_epoch);

    // An end its caller stamps from the decode clock, and a re-entry the fallback stamps.
    assert(dsd_call_state_end_ex(state, 0U, 0.0, DSD_CALL_END_EXPLICIT) == 1);
    decode_clock_at_ms(20000U);
    assert(dsd_call_state_observe(state, &call, DSD_CALL_BOUNDARY_BEGIN) == 1);
    assert(dsd_call_state_end_ex(state, 0U, dsd_decode_now_mono_s(), DSD_CALL_END_SYNC_LOSS) == 1);
    decode_clock_at_ms(20250U);
    assert(dsd_call_state_observe(state, &reentry, DSD_CALL_BOUNDARY_BEGIN) == 1);
    assert(dsd_call_state_get(state, 0U, &snapshot) == 1);
    assert(snapshot.phase == DSD_CALL_PHASE_ACTIVE);
    assert(snapshot.ota_target_id == 500U);
    // The fallback stamps at the decode clock's full resolution.
    assert(double_near(snapshot.started_m, CALL_STATE_TEST_DECODE_T0_S + 20.25));

    dsd_decode_clock_use_system();
    dsd_state_ext_free_all(state);
    free(state);
}

// dsd_call_snapshot_may_reacquire() answers with the windows the store heals by: each recoverable reason's own (0.5 s
// after a sync loss, 0.25 s after an unverified terminator), never for a final end, an active call or an ended epoch that
// names no call; and where it says no, an identity-less re-entry begins a new transmission.
static void
test_may_reacquire_follows_the_heal_windows(void) {
    static const struct {
        dsd_call_end_reason reason;
        double inside_s;
        double outside_s;
    } k_cases[] = {
        {DSD_CALL_END_SYNC_LOSS, 0.45, 0.6},
        {DSD_CALL_END_UNVERIFIED_TERMINATOR, 0.2, 0.3},
        {DSD_CALL_END_EXPLICIT, -1.0, 0.1},
        {DSD_CALL_END_TERMINATOR, -1.0, 0.1},
    };

    assert(dsd_call_snapshot_may_reacquire(NULL, 1.0) == 0);
    for (size_t i = 0U; i < sizeof(k_cases) / sizeof(k_cases[0]); i++) {
        dsd_state* state = (dsd_state*)calloc(1U, sizeof(*state));
        assert(state != NULL);
        dsd_call_observation observation = group_call(0U, 500U, 600U, 10.0);
        assert(dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN) == 1);
        dsd_call_snapshot snapshot;
        assert(dsd_call_state_get(state, 0U, &snapshot) == 1);
        assert(dsd_call_snapshot_may_reacquire(&snapshot, 10.1) == 0); // still active
        const uint64_t first = dsd_call_snapshot_transmission(&snapshot);
        assert(dsd_call_state_end_ex(state, 0U, 10.2, k_cases[i].reason) == 1);
        assert(dsd_call_state_get(state, 0U, &snapshot) == 1);
        if (k_cases[i].inside_s >= 0.0) {
            assert(dsd_call_snapshot_may_reacquire(&snapshot, 10.2 + k_cases[i].inside_s) == 1);
        }
        assert(dsd_call_snapshot_may_reacquire(&snapshot, 10.2 + k_cases[i].outside_s) == 0);
        assert(dsd_call_snapshot_may_reacquire(&snapshot, 10.1) == 0); // before the end

        // Where it says no, the store does not heal either: an identity-less re-entry is a transmission of its own.
        dsd_call_observation reentry = identityless_voice(0U);
        reentry.observed_m = 10.2 + k_cases[i].outside_s;
        assert(dsd_call_state_observe(state, &reentry, DSD_CALL_BOUNDARY_BEGIN) == 1);
        assert(dsd_call_state_get(state, 0U, &snapshot) == 1);
        assert(dsd_call_snapshot_transmission(&snapshot) != first);

        // An ended epoch that names no call is never reacquired, inside any window.
        assert(dsd_call_state_end_ex(state, 0U, 11.0, DSD_CALL_END_SYNC_LOSS) == 1);
        assert(dsd_call_state_get(state, 0U, &snapshot) == 1);
        assert(dsd_call_snapshot_may_reacquire(&snapshot, 11.1) == 0);
        dsd_state_ext_free_all(state);
        free(state);
    }
}

// Recent-activity rows publish and age on the decode clock when the caller passes no time, which
// every protocol publisher and expiry sweep does.
static void
test_recent_activity_runs_on_the_decode_clock(void) {
    dsd_state* state = (dsd_state*)calloc(1U, sizeof(*state));
    assert(state != NULL);
    dsd_decode_clock_use_test(CALL_STATE_TEST_DECODE_T0_NS);

    const dsd_call_observation activity = group_call(0U, 100U, 9U, 0.0);
    assert(dsd_recent_activity_publish(state, 0U, &activity, "Active Ch: 1234 TG: 100; ", 0U) == 1);
    dsd_recent_activity_snapshot recent;
    assert(dsd_recent_activity_copy_snapshot(state, &recent) == 1);
    assert(recent.entries[0].updated_m_ms == CALL_STATE_TEST_DECODE_T0_NS / 1000000ULL);

    decode_clock_at_ms(2900U);
    assert(dsd_recent_activity_expire(state, 0U, 3000U) == 0);
    assert(dsd_recent_activity_copy_snapshot(state, &recent) == 1);
    assert(recent.entries[0].updated_m_ms != 0U);

    decode_clock_at_ms(3100U);
    assert(dsd_recent_activity_expire(state, 0U, 3000U) == 1);
    assert(dsd_recent_activity_copy_snapshot(state, &recent) == 1);
    assert(recent.entries[0].updated_m_ms == 0U);

    dsd_decode_clock_use_system();
    dsd_state_ext_free_all(state);
    free(state);
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
test_explicit_identity_specializes_provisional_voice(void) {
    static const struct {
        int provisional_protocol;
        int identified_protocol;
    } cases[] = {
        {DSD_SYNC_P25P1_POS, DSD_SYNC_P25P1_NEG},
        {DSD_SYNC_P25P2_POS, DSD_SYNC_P25P2_NEG},
        {DSD_SYNC_X2TDMA_VOICE_POS, DSD_SYNC_X2TDMA_VOICE_NEG},
        {DSD_SYNC_DSTAR_VOICE_POS, DSD_SYNC_DSTAR_HD_NEG},
        {DSD_SYNC_M17_STR_POS, DSD_SYNC_M17_LSF_NEG},
        {DSD_SYNC_DMR_BS_VOICE_POS, DSD_SYNC_DMR_BS_VOICE_NEG},
        {DSD_SYNC_PROVOICE_POS, DSD_SYNC_EDACS_NEG},
        {DSD_SYNC_DPMR_FS1_POS, DSD_SYNC_DPMR_FS4_NEG},
        {DSD_SYNC_NXDN_POS, DSD_SYNC_NXDN_NEG},
        {DSD_SYNC_YSF_POS, DSD_SYNC_YSF_NEG},
    };

    for (size_t i = 0U; i < sizeof(cases) / sizeof(cases[0]); i++) {
        dsd_state* state = (dsd_state*)calloc(1U, sizeof(*state));
        assert(state != NULL);
        dsd_call_observation observation = {
            .protocol = cases[i].provisional_protocol,
            .slot = 0U,
            .kind = DSD_CALL_KIND_VOICE,
            .observed_m = 1.0,
        };
        assert(dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN) == 1);

        dsd_call_snapshot snapshot;
        assert(dsd_call_state_get(state, 0U, &snapshot) == 1);
        const uint64_t provisional_epoch = snapshot.epoch;

        observation.protocol = cases[i].identified_protocol;
        observation.kind = DSD_CALL_KIND_GROUP_VOICE;
        observation.ota_target_id = 1000U + i;
        observation.policy_target_id = 1000U + i;
        observation.ota_source_id = 2000U + i;
        observation.observed_m = 2.0;
        assert(dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN) == 0);
        assert(dsd_call_state_get(state, 0U, &snapshot) == 1);
        assert(snapshot.epoch == provisional_epoch);
        assert(snapshot.kind == DSD_CALL_KIND_GROUP_VOICE);
        assert(snapshot.ota_target_id == 1000U + i);
        assert(snapshot.ota_source_id == 2000U + i);

        observation.observed_m = 3.0;
        assert(dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN) == 1);
        assert(dsd_call_state_get(state, 0U, &snapshot) == 1);
        assert(snapshot.epoch != provisional_epoch);
        dsd_state_ext_free_all(state);
        free(state);
    }
}

static void
test_explicit_begin_keeps_real_provisional_boundaries(void) {
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
    const uint64_t first_epoch = snapshot.epoch;

    observation.observed_m = 2.0;
    assert(dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN) == 1);
    assert(dsd_call_state_get(state, 0U, &snapshot) == 1);
    assert(snapshot.epoch != first_epoch);
    const uint64_t second_epoch = snapshot.epoch;

    observation.protocol = DSD_SYNC_P25P1_POS;
    observation.kind = DSD_CALL_KIND_GROUP_VOICE;
    observation.ota_target_id = 3000U;
    observation.policy_target_id = 3000U;
    observation.ota_source_id = 4000U;
    observation.observed_m = 3.0;
    assert(dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN) == 1);
    assert(dsd_call_state_get(state, 0U, &snapshot) == 1);
    assert(snapshot.epoch != second_epoch);
    assert(snapshot.protocol == DSD_SYNC_P25P1_POS);

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
    for (size_t i = strlen(snapshot.source_text) + 1U; i < sizeof(snapshot.source_text); i++) {
        assert(snapshot.source_text[i] == '\0');
    }
    for (size_t i = strlen(snapshot.target_text) + 1U; i < sizeof(snapshot.target_text); i++) {
        assert(snapshot.target_text[i] == '\0');
    }
    for (size_t route = 0U; route < DSD_CALL_ROUTE_COUNT; route++) {
        for (size_t i = strlen(snapshot.route_text[route]) + 1U; i < sizeof(snapshot.route_text[route]); i++) {
            assert(snapshot.route_text[route][i] == '\0');
        }
    }

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

static dsd_call_crypto_update
crypto_update(dsd_call_crypto_state classification, uint8_t algid, uint16_t kid, uint8_t received) {
    dsd_call_crypto_update update = {0};
    update.classification = classification;
    update.algid = algid;
    update.kid = kid;
    update.received = received;
    return update;
}

static void
assert_received(const dsd_state* state, uint8_t slot, uint8_t bits, uint8_t algid, uint16_t kid) {
    dsd_call_snapshot snapshot;
    assert(dsd_call_state_get(state, slot, &snapshot) == 1);
    assert(snapshot.crypto_received == bits);
    assert((bits & DSD_CALL_CRYPTO_RX_ALG) == 0U || snapshot.received_algid == algid);
    assert((bits & DSD_CALL_CRYPTO_RX_KID) == 0U || snapshot.received_kid == kid);
}

// The received ALG ID and key id are the epoch's, separate from the live values every republish rewrites: only an
// update that received a field sets it, a forced, inferred or cleared value published later leaves it, a new
// transmission starts without, and a reacquired one carries the ending epoch's.
static void
test_received_crypto_provenance(void) {
    dsd_state* state = (dsd_state*)calloc(1U, sizeof(*state));
    assert(state != NULL);
    dsd_call_observation call = group_call(0U, 500U, 600U, 10.0);
    assert(dsd_call_state_observe(state, &call, DSD_CALL_BOUNDARY_BEGIN) == 1);
    // A privacy indication with nothing received, then a forced ALG ID with the "no key id" sentinel: none received.
    dsd_call_crypto_update update = crypto_update(DSD_CALL_CRYPTO_ENCRYPTED_PENDING, 0U, 0U, 0U);
    assert(dsd_call_state_update_crypto(state, 0U, &update) == 1);
    update = crypto_update(DSD_CALL_CRYPTO_ENCRYPTED_PENDING, 0x24U, 0xFFU, 0U);
    assert(dsd_call_state_update_crypto(state, 0U, &update) == 1);
    assert_received(state, 0U, 0U, 0U, 0U);
    // A checked header receives both; only the received fields are marked.
    update = crypto_update(DSD_CALL_CRYPTO_ENCRYPTED, 0x21U, 0x05U, DSD_CALL_CRYPTO_RX_ALG);
    assert(dsd_call_state_update_crypto(state, 0U, &update) == 1);
    assert_received(state, 0U, DSD_CALL_CRYPTO_RX_ALG, 0x21U, 0U);
    // A repeat that receives nothing new changes nothing, revision included.
    dsd_call_snapshot before;
    assert(dsd_call_state_get(state, 0U, &before) == 1);
    assert(dsd_call_state_update_crypto(state, 0U, &update) == 1);
    dsd_call_snapshot after;
    assert(dsd_call_state_get(state, 0U, &after) == 1 && after.revision == before.revision);
    // The same live values now receiving the key id too is a change.
    update.received = DSD_CALL_CRYPTO_RX_ALG | DSD_CALL_CRYPTO_RX_KID;
    assert(dsd_call_state_update_crypto(state, 0U, &update) == 1);
    assert(dsd_call_state_get(state, 0U, &after) == 1 && after.revision != before.revision);
    assert_received(state, 0U, DSD_CALL_CRYPTO_RX_ALG | DSD_CALL_CRYPTO_RX_KID, 0x21U, 0x05U);
    // A later republish of other live values (a sync loss cleared them, an inference rewrote them) keeps them.
    update = crypto_update(DSD_CALL_CRYPTO_ENCRYPTED_PENDING, 0U, 0U, 0U);
    assert(dsd_call_state_update_crypto(state, 0U, &update) == 1);
    assert(dsd_call_state_get(state, 0U, &after) == 1 && after.algid == 0U && after.kid == 0U);
    assert_received(state, 0U, DSD_CALL_CRYPTO_RX_ALG | DSD_CALL_CRYPTO_RX_KID, 0x21U, 0x05U);
    // A newly received value replaces the old one.
    update = crypto_update(DSD_CALL_CRYPTO_ENCRYPTED, 0x25U, 0x07U, DSD_CALL_CRYPTO_RX_KID);
    assert(dsd_call_state_update_crypto(state, 0U, &update) == 1);
    assert_received(state, 0U, DSD_CALL_CRYPTO_RX_ALG | DSD_CALL_CRYPTO_RX_KID, 0x21U, 0x07U);
    // The other slot is its own.
    dsd_call_observation other = group_call(1U, 700U, 800U, 10.0);
    assert(dsd_call_state_observe(state, &other, DSD_CALL_BOUNDARY_BEGIN) == 1);
    assert_received(state, 1U, 0U, 0U, 0U);

    // A sync loss and the same call reacquired: the received values come back with its identity.
    assert(dsd_call_state_end_ex(state, 0U, 10.2, DSD_CALL_END_SYNC_LOSS) == 1);
    call.observed_m = 10.3;
    assert(dsd_call_state_observe(state, &call, DSD_CALL_BOUNDARY_BEGIN) == 1);
    assert(dsd_call_state_get(state, 0U, &after) == 1 && after.epoch != before.epoch);
    assert_received(state, 0U, DSD_CALL_CRYPTO_RX_ALG | DSD_CALL_CRYPTO_RX_KID, 0x21U, 0x07U);
    // A retained ended call may still receive (a late checked header describing it).
    assert(dsd_call_state_end_ex(state, 1U, 10.4, DSD_CALL_END_TERMINATOR) == 1);
    update = crypto_update(DSD_CALL_CRYPTO_ENCRYPTED, 0x84U, 0x0001U, DSD_CALL_CRYPTO_RX_ALG);
    assert(dsd_call_state_update_crypto(state, 1U, &update) == 0); // an ended call takes no live update
    assert_received(state, 1U, 0U, 0U, 0U);
    assert(dsd_call_state_update_retained_crypto(state, 1U, &update) == 1);
    assert_received(state, 1U, DSD_CALL_CRYPTO_RX_ALG, 0x84U, 0U);

    // A new transmission on the slot starts with nothing received, even with the same identity.
    assert(dsd_call_state_end_ex(state, 0U, 11.0, DSD_CALL_END_TERMINATOR) == 1);
    call.observed_m = 11.1;
    assert(dsd_call_state_observe(state, &call, DSD_CALL_BOUNDARY_BEGIN) == 1);
    assert_received(state, 0U, 0U, 0U, 0U);
    // So does a different call replacing an active one.
    update = crypto_update(DSD_CALL_CRYPTO_ENCRYPTED, 0x21U, 0x05U, DSD_CALL_CRYPTO_RX_ALG | DSD_CALL_CRYPTO_RX_KID);
    assert(dsd_call_state_update_crypto(state, 0U, &update) == 1);
    dsd_call_observation replacement = group_call(0U, 501U, 601U, 11.2);
    assert(dsd_call_state_observe(state, &replacement, DSD_CALL_BOUNDARY_CONTINUE) == 1);
    assert_received(state, 0U, 0U, 0U, 0U);

    // An identity-bearing BEGIN that specializes the identity-less voice epoch it opened in is the same transmission:
    // what that epoch received stays.
    assert(dsd_call_state_end_ex(state, 0U, 12.0, DSD_CALL_END_TERMINATOR) == 1);
    dsd_call_observation provisional = identityless_voice(0U);
    provisional.observed_m = 13.0;
    assert(dsd_call_state_observe(state, &provisional, DSD_CALL_BOUNDARY_BEGIN) == 1);
    assert(dsd_call_state_update_crypto(state, 0U, &update) == 1);
    dsd_call_observation named = group_call(0U, 900U, 901U, 13.1);
    named.protocol = provisional.protocol;
    assert(dsd_call_state_observe(state, &named, DSD_CALL_BOUNDARY_BEGIN) == 0);
    assert_received(state, 0U, DSD_CALL_CRYPTO_RX_ALG | DSD_CALL_CRYPTO_RX_KID, 0x21U, 0x05U);

    dsd_state_ext_free_all(state);
    free(state);
}

/* Reacquisition and the system and site the call was heard on. Each case opens a call on slot 0 with received crypto,
   ends it on sync loss at 10.2 s and hears the same TG/SRC again at 10.3 s, inside the window, naming no channel or
   frequency of its own: a reopen is seeded with the ending call's channel, frequency, crypto and transmission; a fresh
   call starts with none of them. */
static uint64_t
open_crypto_call(dsd_state* state, const dsd_call_observation* call) {
    assert(dsd_call_state_observe(state, call, DSD_CALL_BOUNDARY_BEGIN) == 1);
    const dsd_call_crypto_update update =
        crypto_update(DSD_CALL_CRYPTO_ENCRYPTED, 0x84U, 0x0101U, DSD_CALL_CRYPTO_RX_ALG | DSD_CALL_CRYPTO_RX_KID);
    assert(dsd_call_state_update_crypto(state, 0U, &update) == 1);
    dsd_call_snapshot snapshot;
    assert(dsd_call_state_get(state, 0U, &snapshot) == 1);
    return dsd_call_snapshot_transmission(&snapshot);
}

static dsd_call_observation
reentry_of(const dsd_call_observation* call) {
    dsd_call_observation reentry = *call;
    reentry.channel = 0U;
    reentry.channel_kind = DSD_CALL_CHANNEL_NONE;
    reentry.frequency_hz = 0;
    reentry.observed_m = 10.3;
    return reentry;
}

/* Hears @p reentry and returns whether it reopened transmission @p first with the ending call's channel @p channel (of
   @p kind), frequency @p freq_hz and crypto, or else began a fresh call holding none of them; any mix fails. */
static int
reentry_reacquires(dsd_state* state, const dsd_call_observation* reentry, uint64_t first, uint32_t channel,
                   uint8_t kind, int64_t freq_hz) {
    assert(dsd_call_state_observe(state, reentry, DSD_CALL_BOUNDARY_BEGIN) == 1);
    dsd_call_snapshot s;
    assert(dsd_call_state_get(state, 0U, &s) == 1);
    assert(s.phase == DSD_CALL_PHASE_ACTIVE && s.epoch != first);
    assert(s.ota_target_id == reentry->ota_target_id && s.ota_source_id == reentry->ota_source_id);
    if (dsd_call_snapshot_transmission(&s) == first) {
        assert(s.channel == channel && s.channel_kind == kind && s.frequency_hz == freq_hz);
        assert(s.crypto == DSD_CALL_CRYPTO_ENCRYPTED && s.algid == 0x84U && s.kid == 0x0101U);
        assert(s.crypto_received == (DSD_CALL_CRYPTO_RX_ALG | DSD_CALL_CRYPTO_RX_KID));
        return 1;
    }
    assert(dsd_call_snapshot_transmission(&s) == s.epoch);
    assert(s.channel == 0U && s.channel_kind == DSD_CALL_CHANNEL_NONE && s.frequency_hz == 0);
    assert(s.crypto == DSD_CALL_CRYPTO_UNKNOWN && s.algid == 0U && s.kid == 0U && s.crypto_received == 0U);
    return 0;
}

static dsd_call_observation
dmr_call(uint32_t channel, uint8_t kind) {
    dsd_call_observation call = group_call(0U, 500U, 600U, 10.0);
    call.protocol = DSD_SYNC_DMR_BS_VOICE_POS;
    call.channel = channel;
    call.channel_kind = kind;
    return call;
}

// A carrier naming another system by the time the same TG/SRC is heard again: another system's call, not the faded one.
static void
test_reacquire_refused_on_conflicting_site(void) {
    dsd_state* state = (dsd_state*)calloc(1U, sizeof(*state));
    assert(state != NULL);
    dsd_rxid_note_p25_network(state, 0xBEE00U, 0x1A3U);
    const dsd_call_observation call = group_call(0U, 500U, 600U, 10.0);
    const uint64_t first = open_crypto_call(state, &call);
    assert(dsd_call_state_end_ex(state, 0U, 10.2, DSD_CALL_END_SYNC_LOSS) == 1);
    dsd_rxid_note_p25_network(state, 0xBEE01U, 0x1A3U);
    const dsd_call_observation reentry = reentry_of(&call);
    assert(reentry_reacquires(state, &reentry, first, 0U, 0U, 0) == 0);
    dsd_state_ext_free_all(state);
    free(state);
}

// The same system, more of it learned after the end, or the same parts named again after a carrier reset: the same call.
static void
test_reacquire_kept_on_compatible_site(void) {
    dsd_state* state = (dsd_state*)calloc(1U, sizeof(*state));
    assert(state != NULL);
    dsd_rxid_note_p25_network(state, 0xBEE00U, 0x1A3U);
    const dsd_call_observation call = group_call(0U, 500U, 600U, 10.0);
    const uint64_t first = open_crypto_call(state, &call);
    assert(dsd_call_state_end_ex(state, 0U, 10.2, DSD_CALL_END_SYNC_LOSS) == 1);
    dsd_rxid_note_p25_site(state, 0x1A3U, 1U, 2U);
    dsd_call_observation reentry = reentry_of(&call);
    assert(reentry_reacquires(state, &reentry, first, call.channel, DSD_CALL_CHANNEL_NONE, call.frequency_hz) == 1);

    assert(dsd_call_state_end_ex(state, 0U, 10.4, DSD_CALL_END_SYNC_LOSS) == 1);
    dsd_rxid_reset(state);
    dsd_rxid_note_p25_network(state, 0xBEE00U, 0x1A3U);
    reentry.observed_m = 10.5;
    assert(reentry_reacquires(state, &reentry, first, call.channel, DSD_CALL_CHANNEL_NONE, call.frequency_hz) == 1);
    dsd_state_ext_free_all(state);
    free(state);
}

// The record named another system while the call was on the air, so its end captured none for history; the call still
// keeps the record it was heard under (dsd_call_snapshot::reacquire_rx_id), so the end may be reacquired, but not by a
// reentry under the other system. Nothing received at all compares as compatible.
static void
test_reacquire_refused_when_site_changed_before_end(void) {
    dsd_state* state = (dsd_state*)calloc(1U, sizeof(*state));
    assert(state != NULL);
    dsd_rxid_note_p25_network(state, 0xBEE00U, 0x1A3U);
    const dsd_call_observation call = group_call(0U, 500U, 600U, 10.0);
    const uint64_t first = open_crypto_call(state, &call);
    dsd_rxid_note_p25_network(state, 0xBEE01U, 0x2B4U);
    assert(dsd_call_state_end_ex(state, 0U, 10.2, DSD_CALL_END_SYNC_LOSS) == 1);
    dsd_call_snapshot s;
    assert(dsd_call_state_get(state, 0U, &s) == 1);
    assert(s.ended_rx_id.family == DSD_RXID_FAMILY_NONE);
    assert(s.reacquire_rx_id.family == DSD_RXID_FAMILY_P25 && s.reacquire_rx_id.p25_wacn == 0xBEE00U);
    assert(dsd_call_snapshot_may_reacquire(&s, 10.3) == 1);
    const dsd_call_observation reentry = reentry_of(&call);
    assert(reentry_reacquires(state, &reentry, first, 0U, 0U, 0) == 0);
    dsd_state_ext_free_all(state);
    free(state);

    dsd_state* silent = (dsd_state*)calloc(1U, sizeof(*silent));
    assert(silent != NULL);
    const uint64_t quiet = open_crypto_call(silent, &call);
    assert(dsd_call_state_end_ex(silent, 0U, 10.2, DSD_CALL_END_SYNC_LOSS) == 1);
    assert(dsd_call_state_get(silent, 0U, &s) == 1);
    assert(s.ended_rx_id.family == DSD_RXID_FAMILY_NONE && s.reacquire_rx_id.family == DSD_RXID_FAMILY_NONE);
    assert(dsd_call_snapshot_may_reacquire(&s, 10.3) == 1);
    assert(reentry_reacquires(silent, &reentry, quiet, call.channel, DSD_CALL_CHANNEL_NONE, call.frequency_hz) == 1);
    dsd_state_ext_free_all(silent);
    free(silent);
}

// A sync of another protocol family (noise passing its sync word during the fade) wipes the record mid-call. The call
// keeps the record it was heard under: a reentry under the empty record, or under the same network named again,
// reopens it as before.
static void
test_reacquire_kept_after_cross_family_wipe(void) {
    for (int renote = 0; renote <= 1; renote++) {
        dsd_state* state = (dsd_state*)calloc(1U, sizeof(*state));
        assert(state != NULL);
        dsd_rxid_note_p25_network(state, 0xBEE00U, 0x1A3U);
        const dsd_call_observation call = group_call(0U, 500U, 600U, 10.0);
        const uint64_t first = open_crypto_call(state, &call);
        const uint32_t heard_epoch = state->rx_id.epoch;
        dsd_rxid_note_sync(state, DSD_SYNC_DMR_BS_VOICE_POS);
        assert(state->rx_id.epoch != heard_epoch && state->rx_id.family == DSD_RXID_FAMILY_NONE);
        assert(dsd_call_state_end_ex(state, 0U, 10.2, DSD_CALL_END_SYNC_LOSS) == 1);
        if (renote) {
            dsd_rxid_note_p25_network(state, 0xBEE00U, 0x1A3U);
        }
        const dsd_call_observation reentry = reentry_of(&call);
        assert(reentry_reacquires(state, &reentry, first, call.channel, DSD_CALL_CHANNEL_NONE, call.frequency_hz) == 1);
        dsd_state_ext_free_all(state);
        free(state);
    }
}

// The same wipe, then a DMR site noted before the end: the reentry is heard under another family's system, so it is
// another system's call (P25 against DMR), not the faded one.
static void
test_reacquire_refused_after_cross_family_wipe_and_dmr_site(void) {
    dsd_state* state = (dsd_state*)calloc(1U, sizeof(*state));
    assert(state != NULL);
    dsd_rxid_note_p25_network(state, 0xBEE00U, 0x1A3U);
    const dsd_call_observation call = group_call(0U, 500U, 600U, 10.0);
    const uint64_t first = open_crypto_call(state, &call);
    dsd_rxid_note_sync(state, DSD_SYNC_DMR_BS_VOICE_POS);
    assert(state->rx_id.family == DSD_RXID_FAMILY_NONE);
    dsd_rxid_note_dmr_site(state, DSD_RXID_DMR_TRUNK_TIER3, 1, 11U, 300U, 1U, "TIII Small · Net 12 · Site 301");
    assert(state->rx_id.family == DSD_RXID_FAMILY_DMR);
    assert(dsd_call_state_end_ex(state, 0U, 10.2, DSD_CALL_END_SYNC_LOSS) == 1);
    dsd_call_snapshot s;
    assert(dsd_call_state_get(state, 0U, &s) == 1);
    assert(s.ended_rx_id.family == DSD_RXID_FAMILY_NONE);
    assert(s.reacquire_rx_id.family == DSD_RXID_FAMILY_P25 && s.reacquire_rx_id.p25_wacn == 0xBEE00U);
    assert(dsd_call_snapshot_may_reacquire(&s, 10.3) == 1);
    const dsd_call_observation reentry = reentry_of(&call);
    assert(reentry_reacquires(state, &reentry, first, 0U, 0U, 0) == 0);
    dsd_state_ext_free_all(state);
    free(state);
}

// Wiped, healed under the empty record, faded again: the reopened epoch kept the site the call was heard on, so a
// reentry under another identity is still another system's call.
static void
test_reacquire_two_fades_keep_the_heard_site(void) {
    dsd_state* state = (dsd_state*)calloc(1U, sizeof(*state));
    assert(state != NULL);
    dsd_rxid_note_p25_network(state, 0xBEE00U, 0x1A3U);
    const dsd_call_observation call = group_call(0U, 500U, 600U, 10.0);
    const uint64_t first = open_crypto_call(state, &call);
    dsd_rxid_reset(state);
    assert(dsd_call_state_end_ex(state, 0U, 10.2, DSD_CALL_END_SYNC_LOSS) == 1);
    dsd_call_observation reentry = reentry_of(&call);
    assert(reentry_reacquires(state, &reentry, first, call.channel, DSD_CALL_CHANNEL_NONE, call.frequency_hz) == 1);
    dsd_call_snapshot s;
    assert(dsd_call_state_get(state, 0U, &s) == 1);
    assert(s.reacquire_rx_id.family == DSD_RXID_FAMILY_P25 && s.reacquire_rx_id.p25_wacn == 0xBEE00U);

    assert(dsd_call_state_end_ex(state, 0U, 10.4, DSD_CALL_END_SYNC_LOSS) == 1);
    assert(dsd_call_state_get(state, 0U, &s) == 1);
    assert(s.reacquire_rx_id.family == DSD_RXID_FAMILY_P25 && s.reacquire_rx_id.p25_wacn == 0xBEE00U);
    dsd_rxid_note_p25_network(state, 0xBEE01U, 0x1A3U);
    reentry.observed_m = 10.5;
    assert(reentry_reacquires(state, &reentry, first, 0U, 0U, 0) == 0);
    dsd_state_ext_free_all(state);
    free(state);
}

// No identity on either side, or a site learned after the end that fits the call's channel: the same call.
static void
test_reacquire_identity_less_kept(void) {
    dsd_state* state = (dsd_state*)calloc(1U, sizeof(*state));
    assert(state != NULL);
    const dsd_call_observation call = dmr_call(12U, DSD_CALL_CHANNEL_LPCN);
    const uint64_t first = open_crypto_call(state, &call);
    assert(dsd_call_state_end_ex(state, 0U, 10.2, DSD_CALL_END_SYNC_LOSS) == 1);
    dsd_call_observation reentry = reentry_of(&call);
    assert(reentry_reacquires(state, &reentry, first, 12U, DSD_CALL_CHANNEL_LPCN, call.frequency_hz) == 1);

    assert(dsd_call_state_end_ex(state, 0U, 10.4, DSD_CALL_END_SYNC_LOSS) == 1);
    dsd_rxid_note_dmr_site(state, DSD_RXID_DMR_TRUNK_TIER3, 1, 11U, 300U, 1U, "TIII Small · Net 12 · Site 301");
    reentry.observed_m = 10.5;
    assert(reentry_reacquires(state, &reentry, first, 12U, DSD_CALL_CHANNEL_LPCN, call.frequency_hz) == 1);
    dsd_state_ext_free_all(state);
    free(state);
}

typedef enum {
    LEARN_NOTHING,
    LEARN_CONNECT_PLUS_SITE,
    LEARN_CAPACITY_PLUS_REST_LSN,
    LEARN_XPT_FREE_LCN,
} learned_after_end;

static void
learn_after_end(dsd_state* state, learned_after_end learned) {
    switch (learned) {
        case LEARN_CONNECT_PLUS_SITE:
            dsd_rxid_note_dmr_site(state, DSD_RXID_DMR_TRUNK_CONNECT_PLUS, 1, 1234U, 5U, 0U,
                                   "Con+ · Net 1234 · Site 5");
            break;
        case LEARN_CAPACITY_PLUS_REST_LSN: dsd_rxid_note_dmr_rest_lsn(state, 3U); break;
        case LEARN_XPT_FREE_LCN: dsd_rxid_note_dmr_xpt_free_lcn(state, 4U); break;
        default: break;
    }
}

// A channel-only Tier III call (its LPCN, no site) faded, and the TG/SRC is heard again under a Connect Plus identity
// learned after the end. The Connect Plus voice LC names no channel, so an inherited LPCN would never be replaced: a
// fresh transmission, with no channel or crypto inherited. The trunking system a rest LSN or a free LCN names alone
// counts too, and so does a reopening observation naming its own channel of another kind.
static void
test_reacquire_channel_only_t3_then_conplus_refused(void) {
    static const struct {
        uint8_t ended_kind;
        learned_after_end learned;
        uint32_t reentry_channel;
        uint8_t reentry_kind;
        int reacquires;
    } k_cases[] = {
        {DSD_CALL_CHANNEL_LPCN, LEARN_CONNECT_PLUS_SITE, 0U, DSD_CALL_CHANNEL_NONE, 0},
        {DSD_CALL_CHANNEL_LPCN, LEARN_CAPACITY_PLUS_REST_LSN, 0U, DSD_CALL_CHANNEL_NONE, 0},
        {DSD_CALL_CHANNEL_LPCN, LEARN_XPT_FREE_LCN, 0U, DSD_CALL_CHANNEL_NONE, 0},
        {DSD_CALL_CHANNEL_LPCN, LEARN_NOTHING, 3U, DSD_CALL_CHANNEL_LCN, 0},
        {DSD_CALL_CHANNEL_LSN, LEARN_CAPACITY_PLUS_REST_LSN, 0U, DSD_CALL_CHANNEL_NONE, 1},
        {DSD_CALL_CHANNEL_LCN, LEARN_CONNECT_PLUS_SITE, 0U, DSD_CALL_CHANNEL_NONE, 1},
    };

    for (size_t i = 0U; i < sizeof(k_cases) / sizeof(k_cases[0]); i++) {
        dsd_state* state = (dsd_state*)calloc(1U, sizeof(*state));
        assert(state != NULL);
        const dsd_call_observation call = dmr_call(12U, k_cases[i].ended_kind);
        const uint64_t first = open_crypto_call(state, &call);
        assert(dsd_call_state_end_ex(state, 0U, 10.2, DSD_CALL_END_SYNC_LOSS) == 1);
        learn_after_end(state, k_cases[i].learned);
        dsd_call_observation reentry = reentry_of(&call);
        reentry.channel = k_cases[i].reentry_channel;
        reentry.channel_kind = k_cases[i].reentry_kind;
        if (k_cases[i].reacquires) {
            assert(reentry_reacquires(state, &reentry, first, 12U, k_cases[i].ended_kind, call.frequency_hz) == 1);
        } else {
            assert(dsd_call_state_observe(state, &reentry, DSD_CALL_BOUNDARY_BEGIN) == 1);
            dsd_call_snapshot s;
            assert(dsd_call_state_get(state, 0U, &s) == 1);
            assert(dsd_call_snapshot_transmission(&s) == s.epoch && s.epoch != first);
            assert(s.channel == reentry.channel && s.channel_kind == reentry.channel_kind && s.frequency_hz == 0);
            assert(s.crypto == DSD_CALL_CRYPTO_UNKNOWN && s.algid == 0U && s.kid == 0U && s.crypto_received == 0U);
        }
        dsd_state_ext_free_all(state);
        free(state);
    }
}

int
main(void) {
    dsd_state* state = (dsd_state*)calloc(1U, sizeof(*state));
    assert(state != NULL);
    test_epochs_and_late_identity(state);
    test_crypto_and_slot_isolation(state);
    test_retained_crypto_update();
    test_media_timestamps();
    test_reacquisition_carries_media_timestamps();
    test_channel_fill_only();
    test_reacquire_runs_on_the_decode_clock();
    test_may_reacquire_follows_the_heal_windows();
    test_recent_activity(state);
    test_recent_activity_runs_on_the_decode_clock();
    test_snapshot_clone(state);
    test_voice_specialization_and_sparse_metadata();
    test_explicit_identity_specializes_provisional_voice();
    test_explicit_begin_keeps_real_provisional_boundaries();
    test_64bit_text_and_protocol_family_continuity();
    test_positive_p25p1_protocol_observation();
    test_received_crypto_provenance();
    test_reacquire_refused_on_conflicting_site();
    test_reacquire_kept_on_compatible_site();
    test_reacquire_refused_when_site_changed_before_end();
    test_reacquire_kept_after_cross_family_wipe();
    test_reacquire_refused_after_cross_family_wipe_and_dmr_site();
    test_reacquire_two_fades_keep_the_heard_site();
    test_reacquire_identity_less_kept();
    test_reacquire_channel_only_t3_then_conplus_refused();
    dsd_state_ext_free_all(state);
    free(state);
    return 0;
}
