// SPDX-License-Identifier: GPL-3.0-or-later
/* Minimal canonical-call hooks for decoder source-isolation tests. */

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/platform/threading.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include "dsd-neo/core/position.h"

static const dsd_state* g_stub_state;

#ifdef DSD_NEO_EVENT_HISTORY_TRANSACTIONS
static struct {
    dsd_mutex_t mutex;
    dsd_call_snapshot calls[DSD_CALL_STATE_SLOT_COUNT];
} g_stub_ext;

#define g_stub_calls (g_stub_ext.calls)
static int g_stub_mutex_initialized;
#else
static dsd_call_snapshot g_stub_calls[DSD_CALL_STATE_SLOT_COUNT];
#endif
static uint64_t g_stub_epoch;
static unsigned int g_stub_event_sync_counts[DSD_CALL_STATE_SLOT_COUNT];

// NOLINTNEXTLINE(misc-use-internal-linkage)
unsigned int dsd_call_state_stub_event_sync_count(uint8_t slot);

static void
stub_select_state(dsd_state* state) {
#ifdef DSD_NEO_EVENT_HISTORY_TRANSACTIONS
    if (!g_stub_mutex_initialized) {
        (void)dsd_mutex_init(&g_stub_ext.mutex);
        g_stub_mutex_initialized = 1;
    }
    void* stub_ext = &g_stub_ext;
#else
    void* stub_ext = g_stub_calls;
#endif
    if (g_stub_state != state || state->state_ext[DSD_STATE_EXT_CORE_CALL_STATE] != stub_ext) {
        g_stub_state = state;
        DSD_MEMSET(g_stub_calls, 0, sizeof(g_stub_calls));
        DSD_MEMSET(g_stub_event_sync_counts, 0, sizeof(g_stub_event_sync_counts));
        state->state_ext[DSD_STATE_EXT_CORE_CALL_STATE] = stub_ext;
        state->state_ext_cleanup[DSD_STATE_EXT_CORE_CALL_STATE] = NULL;
    }
}

int
dsd_call_state_ensure(dsd_state* state) {
    if (state == NULL) {
        return 0;
    }
    stub_select_state(state);
    return 1;
}

int
dsd_call_state_observe(dsd_state* state, const dsd_call_observation* observation, dsd_call_boundary boundary) {
    if (state == NULL || observation == NULL || observation->slot >= DSD_CALL_STATE_SLOT_COUNT) {
        return -1;
    }
    stub_select_state(state);
    dsd_call_snapshot* call = &g_stub_calls[observation->slot];
    int begins_epoch = boundary == DSD_CALL_BOUNDARY_BEGIN || call->phase != DSD_CALL_PHASE_ACTIVE;
    const uint64_t old_target = call->ota_target_id != 0U ? call->ota_target_id : call->policy_target_id;
    const uint64_t new_target =
        observation->ota_target_id != 0U ? observation->ota_target_id : observation->policy_target_id;
    if (!begins_epoch && old_target != 0U && new_target != 0U && old_target != new_target) {
        begins_epoch = 1;
    }
    if (!begins_epoch && call->ota_source_id != 0U && observation->ota_source_id != 0U
        && call->ota_source_id != observation->ota_source_id) {
        begins_epoch = 1;
    }
    if (!begins_epoch && call->source_text[0] != '\0' && observation->source_text[0] != '\0'
        && strcmp(call->source_text, observation->source_text) != 0) {
        begins_epoch = 1;
    }
    if (!begins_epoch && call->target_text[0] != '\0' && observation->target_text[0] != '\0'
        && strcmp(call->target_text, observation->target_text) != 0) {
        begins_epoch = 1;
    }
    if (begins_epoch) {
        DSD_MEMSET(call, 0, sizeof(*call));
        call->epoch = ++g_stub_epoch;
        call->transmission_epoch = call->epoch;
        call->phase = DSD_CALL_PHASE_ACTIVE;
        call->slot = observation->slot;
        call->started_m = observation->observed_m;
    }
    call->protocol = observation->protocol;
    call->kind = observation->kind;
    if (observation->ota_source_id != 0U) {
        call->ota_source_id = observation->ota_source_id;
    }
    if (observation->ota_target_id != 0U) {
        call->ota_target_id = observation->ota_target_id;
    }
    if (observation->policy_target_id != 0U) {
        call->policy_target_id = observation->policy_target_id;
    }
    if (observation->source_text[0] != '\0') {
        DSD_SNPRINTF(call->source_text, sizeof(call->source_text), "%s", observation->source_text);
    }
    if (observation->target_text[0] != '\0') {
        DSD_SNPRINTF(call->target_text, sizeof(call->target_text), "%s", observation->target_text);
    }
    if (observation->route_text[0][0] != '\0') {
        DSD_SNPRINTF(call->route_text[0], sizeof(call->route_text[0]), "%s", observation->route_text[0]);
    }
    if (observation->route_text[1][0] != '\0') {
        DSD_SNPRINTF(call->route_text[1], sizeof(call->route_text[1]), "%s", observation->route_text[1]);
    }
    call->channel = observation->channel;
    call->frequency_hz = observation->frequency_hz;
    if (observation->has_service_metadata != 0U) {
        call->service_options = observation->service_options;
        call->emergency = observation->emergency;
        call->priority = observation->priority;
        call->has_service_metadata = 1U;
    }
    return begins_epoch;
}

int
dsd_call_state_get(const dsd_state* state, uint8_t slot, dsd_call_snapshot* out) {
    if (state == NULL || slot >= DSD_CALL_STATE_SLOT_COUNT || out == NULL) {
        return -1;
    }
    stub_select_state((dsd_state*)state);
    *out = g_stub_calls[slot];
    return out->epoch != 0U ? 1 : 0;
}

int
dsd_call_state_note_source_home(dsd_state* state, uint8_t slot, int protocol, const dsd_call_source_home* home,
                                int* replaced) {
    if (replaced) {
        *replaced = 0;
    }
    if (state == NULL || home == NULL || home->valid == 0U || slot >= DSD_CALL_STATE_SLOT_COUNT) {
        return -1;
    }
    (void)protocol;
    stub_select_state(state);
    dsd_call_snapshot* call = &g_stub_calls[slot];
    if (call->epoch == 0U || call->phase != DSD_CALL_PHASE_ACTIVE) {
        return 0;
    }
    if (replaced) {
        *replaced = call->source_home.valid != 0U
                    && (call->source_home.unit != home->unit || call->source_home.wacn != home->wacn
                        || call->source_home.sysid != home->sysid);
    }
    const int same_radio = call->source_home.valid != 0U && call->source_home.unit == home->unit
                           && call->source_home.wacn == home->wacn && call->source_home.sysid == home->sysid;
    const uint32_t kept_source = (same_radio && home->source == 0U) ? call->source_home.source : home->source;
    call->source_home = *home;
    call->source_home.source = kept_source;
    call->source_home.withdrawn = 0U;
    return 1;
}

int
dsd_call_state_note_source_address(dsd_state* state, uint8_t slot, int protocol, uint32_t source, int* dropped) {
    if (dropped) {
        *dropped = 0;
    }
    if (state == NULL || source == 0U || slot >= DSD_CALL_STATE_SLOT_COUNT) {
        return -1;
    }
    (void)protocol;
    stub_select_state(state);
    dsd_call_snapshot* call = &g_stub_calls[slot];
    if (call->epoch == 0U || call->phase != DSD_CALL_PHASE_ACTIVE) {
        return 0;
    }
    if (call->source_home.valid != 0U && call->source_home.source != source) {
        DSD_MEMSET(&call->source_home, 0, sizeof(call->source_home));
        call->source_home.withdrawn = 1U;
        if (dropped) {
            *dropped = 1;
        }
    }
    return 1;
}

int
dsd_call_state_note_source_verdict(dsd_state* state, uint8_t slot, uint64_t epoch, uint32_t source, int checked,
                                   uint64_t rx_gen) {
    if (state == NULL || slot >= DSD_CALL_STATE_SLOT_COUNT || epoch == 0U) {
        return -1;
    }
    stub_select_state(state);
    dsd_call_snapshot* call = &g_stub_calls[slot];
    if (call->epoch != epoch) {
        return 0;
    }
    call->source_checked = (uint8_t)(checked != 0 && source != 0U);
    call->source_checked_id = source;
    call->source_checked_rx_gen = rx_gen;
    return 1;
}

int
dsd_call_state_source_learnable(const dsd_state* state, const dsd_call_snapshot* call) {
    if (state == NULL || call == NULL || call->ota_source_id == 0U || call->ota_source_id > UINT32_MAX) {
        return 0;
    }
    if (!DSD_SYNC_IS_P25(call->protocol)) {
        return 1;
    }
    return call->source_checked != 0U && call->source_checked_id == (uint32_t)call->ota_source_id
           && call->source_checked_rx_gen == dsd_state_reception_generation(state);
}

int
dsd_call_state_protocol_family(int protocol) {
    // Mirrors the real store closely enough for the boundary decisions these
    // stubs exercise: P25 Phase 1 and Phase 2 must land in different families.
    if (DSD_SYNC_IS_P25P1(protocol)) {
        return 1;
    }
    if (DSD_SYNC_IS_P25P2(protocol)) {
        return 2;
    }
    return protocol == DSD_SYNC_NONE ? 0 : 1000 + protocol;
}

int
dsd_call_state_update_crypto(dsd_state* state, uint8_t slot, int protocol, const dsd_call_crypto_update* update) {
    (void)protocol;
    if (state == NULL || slot >= DSD_CALL_STATE_SLOT_COUNT || update == NULL) {
        return -1;
    }
    stub_select_state(state);
    g_stub_calls[slot].crypto = update->classification;
    g_stub_calls[slot].algid = update->algid;
    g_stub_calls[slot].kid = update->kid;
    g_stub_calls[slot].mi = update->mi;
    g_stub_calls[slot].audio_permitted = update->audio_permitted;
    // The checked encryption indication, as the real store keeps it: set or cleared by the update's own, else kept.
    if (update->enc_observed == DSD_CALL_ENC_OBSERVED_ENCRYPTED) {
        g_stub_calls[slot].enc_received = 1U;
    } else if (update->enc_observed == DSD_CALL_ENC_OBSERVED_CLEAR) {
        g_stub_calls[slot].enc_received = 0U;
    }
    return 0;
}

int
dsd_call_state_update_retained_crypto(dsd_state* state, uint8_t slot, int protocol,
                                      const dsd_call_crypto_update* update) {
    return dsd_call_state_update_crypto(state, slot, protocol, update);
}

int
dsd_call_state_update_media(dsd_state* state, uint8_t slot, int media_active, double observed_m) {
    if (state == NULL || slot >= DSD_CALL_STATE_SLOT_COUNT) {
        return -1;
    }
    stub_select_state(state);
    dsd_call_snapshot* call = &g_stub_calls[slot];
    if (call->epoch == 0U || call->phase != DSD_CALL_PHASE_ACTIVE) {
        return 0;
    }
    /* Production substitutes its monotonic clock when callers pass zero. These source-isolation
     * tests deliberately link no timing backend, so wall time supplies the same positive-stamp
     * contract; tests that exercise a specific timeline pass it explicitly. */
    const double now_m = observed_m > 0.0 ? observed_m : (double)time(NULL);
    call->media_active = media_active != 0;
    call->updated_m = now_m;
    if (media_active) {
        if (call->media_started_m <= 0.0) {
            call->media_started_m = now_m;
        }
        call->media_updated_m = now_m;
    }
    return 1;
}

int
dsd_call_state_note_checked_media(dsd_state* state, uint8_t slot, int protocol, double observed_m) {
    if (state == NULL || slot >= DSD_CALL_STATE_SLOT_COUNT || protocol == DSD_SYNC_NONE) {
        return -1;
    }
    stub_select_state(state);
    dsd_call_snapshot* call = &g_stub_calls[slot];
    if (call->epoch == 0U || call->phase != DSD_CALL_PHASE_ACTIVE) {
        return 0;
    }
    const double now_m = observed_m > 0.0 ? observed_m : (double)time(NULL);
    if (call->checked_media_started_m <= 0.0) {
        call->checked_media_started_m = now_m;
    }
    call->checked_media_updated_m = now_m;
    return 1;
}

int
dsd_call_state_end_ex(dsd_state* state, uint8_t slot, double observed_m, dsd_call_end_reason reason) {
    if (state == NULL || slot >= DSD_CALL_STATE_SLOT_COUNT) {
        return -1;
    }
    stub_select_state(state);
    // Mirrors the production early-return: ending an already-ended epoch is a no-op, so the
    // repeated sync-loss ends that fire while unsynced neither downgrade an explicit terminator's
    // reason nor re-stamp ended_m and slide the reacquisition window. A stub that overwrote them
    // would model the exact bug that guard exists to prevent.
    if (g_stub_calls[slot].epoch == 0U || g_stub_calls[slot].phase != DSD_CALL_PHASE_ACTIVE) {
        // And production's one exception, mirrored for the same reason: final evidence decoded
        // after a recoverable end retracts the reacquisition permission that end granted, and a
        // second unverified terminator corroborates an unverified end into TERMINATOR.
        // Stub-linked protocol tests drive exactly this path from their terminator handlers, so a
        // stub that returned 0 here would leave every one of them modelling the pre-retraction
        // behaviour and silently skipping the event flush that follows.
        const uint8_t prior = g_stub_calls[slot].end_reason;
        const int prior_recoverable =
            prior == (uint8_t)DSD_CALL_END_SYNC_LOSS || prior == (uint8_t)DSD_CALL_END_UNVERIFIED_TERMINATOR;
        const int retracts =
            (reason == DSD_CALL_END_EXPLICIT || reason == DSD_CALL_END_TERMINATOR) && prior_recoverable;
        const int corroborates =
            reason == DSD_CALL_END_UNVERIFIED_TERMINATOR && prior == (uint8_t)DSD_CALL_END_UNVERIFIED_TERMINATOR;
        if (g_stub_calls[slot].epoch != 0U && g_stub_calls[slot].phase == DSD_CALL_PHASE_ENDED
            && (retracts || corroborates)) {
            // ended_m is deliberately left alone: the reason changes, the moment does not.
            g_stub_calls[slot].end_reason = (reason == DSD_CALL_END_TERMINATOR || corroborates)
                                                ? (uint8_t)DSD_CALL_END_TERMINATOR
                                                : (uint8_t)DSD_CALL_END_EXPLICIT;
            return 1;
        }
        return 0;
    }
    g_stub_calls[slot].phase = DSD_CALL_PHASE_ENDED;
    g_stub_calls[slot].end_reason = (uint8_t)reason;
    // Stamped from the caller's timeline so a stub-linked test can drive the reacquisition
    // window explicitly. No clock is read here: these targets deliberately link no timing
    // backend, and a test that wants a window must supply both endpoints itself.
    g_stub_calls[slot].ended_m = observed_m > 0.0 ? observed_m : 0.0;
    return 1;
}

/* This stub never reacquires an ended epoch (every begin after an end opens a transmission of its own), so it answers
   as its own observe behaves: nothing may be reacquired. */
int
dsd_call_snapshot_may_reacquire(const dsd_call_snapshot* call, double now_m) {
    (void)call;
    (void)now_m;
    return 0;
}

int
dsd_call_state_end(dsd_state* state, uint8_t slot, double observed_m) {
    return dsd_call_state_end_ex(state, slot, observed_m, DSD_CALL_END_EXPLICIT);
}

int
dsd_recent_activity_publish(dsd_state* state, uint8_t index, const dsd_call_observation* observation,
                            const char* notice, uint64_t observed_m_ms) {
    (void)state;
    (void)index;
    (void)observation;
    (void)notice;
    (void)observed_m_ms;
    return 0;
}

int
dsd_recent_activity_clear(dsd_state* state, uint8_t index) {
    (void)state;
    (void)index;
    return 0;
}

int
dsd_recent_activity_clear_all(dsd_state* state) {
    (void)state;
    return 0;
}

int
dsd_recent_activity_expire(dsd_state* state, uint64_t now_m_ms, uint64_t ttl_ms) {
    (void)state;
    (void)now_m_ms;
    (void)ttl_ms;
    return 0;
}

void
dsd_event_sync_slot(dsd_opts* opts, dsd_state* state, uint8_t slot) {
    (void)opts;
    if (state == NULL || slot >= DSD_CALL_STATE_SLOT_COUNT) {
        return;
    }
    stub_select_state(state);
    g_stub_event_sync_counts[slot]++;
}

unsigned int
dsd_call_state_stub_event_sync_count(uint8_t slot) {
    return slot < DSD_CALL_STATE_SLOT_COUNT ? g_stub_event_sync_counts[slot] : 0U;
}

int
dsd_event_enrich_alias(dsd_state* state, uint8_t slot, int protocol, uint64_t epoch, const char* alias) {
    (void)protocol;
    (void)state;
    (void)slot;
    (void)epoch;
    (void)alias;
    return 0;
}

int
dsd_event_enrich_alias_ex(const dsd_opts* opts, dsd_state* state, uint8_t slot, int protocol, uint64_t epoch,
                          const char* alias) {
    (void)opts;
    return dsd_event_enrich_alias(state, slot, protocol, epoch, alias);
}

int
dsd_event_enrich_gps(dsd_state* state, uint8_t slot, int protocol, uint64_t epoch, const char* gps) {
    (void)protocol;
    dsd_call_snapshot call;
    if (state == NULL || state->event_history_s == NULL || slot >= DSD_CALL_STATE_SLOT_COUNT || epoch == 0U
        || gps == NULL || dsd_call_state_get(state, slot, &call) <= 0 || call.epoch != epoch) {
        return 0;
    }
    DSD_SNPRINTF(state->event_history_s[slot].Event_History_Items[0].gps_s,
                 sizeof(state->event_history_s[slot].Event_History_Items[0].gps_s), "%s", gps);
    state->event_history_s[slot].revision++;
    return 1;
}

// The position writers (issue #800), against the stub call: an admitted report (rev set) goes into row 0 with its text
// while the slot's call is the epoch (ACTIVE unless the call was captured closing) and names the report's source. No
// sender-scope or rev ordering: tests that need those link the real event code.
static int
stub_write_position(dsd_state* state, uint8_t slot, uint64_t epoch, int allow_ended, const dsd_position_fix* fix,
                    const char* gps_text) {
    dsd_call_snapshot call;
    if (state == NULL || state->event_history_s == NULL || slot >= DSD_CALL_STATE_SLOT_COUNT || epoch == 0U
        || fix == NULL || fix->rev == 0U || gps_text == NULL) {
        return -1;
    }
    if (dsd_call_state_get(state, slot, &call) <= 0 || call.epoch != epoch
        || (!allow_ended && call.phase != DSD_CALL_PHASE_ACTIVE)
        || (fix->source_id != 0U && call.ota_source_id != fix->source_id)) {
        return 0;
    }
    Event_History* item = &state->event_history_s[slot].Event_History_Items[0];
    DSD_SNPRINTF(item->gps_s, sizeof(item->gps_s), "%s", gps_text);
    DSD_MEMSET(&item->pos, 0, sizeof(item->pos));
    item->pos.valid = 1U;
    item->pos.rev = fix->rev;
    item->pos.proc_instance = fix->proc_instance;
    item->pos.rx_time_s = fix->rx_time_s;
    item->pos.lat_e7 = fix->lat_e7;
    item->pos.lon_e7 = fix->lon_e7;
    item->pos.err_m = fix->err_m;
    item->pos.flags = fix->flags;
    item->pos.kind = fix->kind;
    item->pos.rx_clock = fix->rx_clock;
    item->pos.scope = fix->sender_scope;
    item->pos.identity_freq_hz = fix->identity_freq_hz;
    state->event_history_s[slot].revision++;
    return 1;
}

int
dsd_event_enrich_position(const dsd_opts* opts, dsd_state* state, uint8_t slot, uint64_t epoch,
                          const dsd_position_fix* fix, const char* gps_text) {
    (void)opts;
    return stub_write_position(state, slot, epoch, 0, fix, gps_text);
}

int
dsd_event_capture_closing_call(const dsd_state* state, uint8_t slot, dsd_event_closing_call* out) {
    dsd_call_snapshot call;
    if (state == NULL || out == NULL || slot >= DSD_CALL_STATE_SLOT_COUNT) {
        return -1;
    }
    DSD_MEMSET(out, 0, sizeof(*out));
    if (dsd_call_state_get(state, slot, &call) <= 0 || call.epoch == 0U || call.phase != DSD_CALL_PHASE_ACTIVE) {
        return 0;
    }
    out->epoch = call.epoch;
    out->source_id = call.ota_source_id;
    out->protocol = call.protocol;
    out->slot = slot;
    out->valid = 1U;
    return 1;
}

int
dsd_event_enrich_position_closing(const dsd_opts* opts, dsd_state* state, const dsd_event_closing_call* closing,
                                  const dsd_position_fix* fix, const char* gps_text) {
    (void)opts;
    if (closing == NULL) {
        return -1;
    }
    return closing->valid ? stub_write_position(state, closing->slot, closing->epoch, 1, fix, gps_text) : 0;
}

int
dsd_event_set_open_call_position(dsd_state* state, uint8_t slot, const dsd_position_fix* fix, const char* gps_text) {
    dsd_call_snapshot call;
    if (state == NULL || slot >= DSD_CALL_STATE_SLOT_COUNT) {
        return -1;
    }
    const uint64_t epoch = dsd_call_state_get(state, slot, &call) > 0 ? call.epoch : 0U;
    return epoch != 0U ? stub_write_position(state, slot, epoch, 0, fix, gps_text) : 0;
}

// The open-call detail write, against the stub call: written while the slot's call is active and of the detail's
// protocol family, declined otherwise.
int
dsd_event_set_open_call_detail(dsd_state* state, uint8_t slot, int protocol, dsd_event_detail_field field,
                               const char* value) {
    dsd_call_snapshot call;
    if (state == NULL || state->event_history_s == NULL || slot >= DSD_CALL_STATE_SLOT_COUNT || value == NULL) {
        return -1;
    }
    if (dsd_call_state_get(state, slot, &call) <= 0 || call.phase != DSD_CALL_PHASE_ACTIVE
        || dsd_sync_protocol_family(call.protocol) != dsd_sync_protocol_family(protocol)) {
        return 0;
    }
    Event_History* item = &state->event_history_s[slot].Event_History_Items[0];
    if (field == DSD_EVENT_DETAIL_ALIAS) {
        DSD_SNPRINTF(item->alias, sizeof(item->alias), "%s", value);
    } else if (field == DSD_EVENT_DETAIL_GPS) {
        DSD_SNPRINTF(item->gps_s, sizeof(item->gps_s), "%s", value);
    } else {
        DSD_SNPRINTF(item->text_message, sizeof(item->text_message), "%s", value);
    }
    state->event_history_s[slot].revision++;
    return 1;
}

int
dsd_call_state_note_key_selection(dsd_state* state, uint8_t slot, int protocol, uint64_t epoch,
                                  dsd_call_key_source source, int signaled_id, int effective_id, int available,
                                  int fallback) {
    (void)state;
    (void)slot;
    (void)protocol;
    (void)epoch;
    (void)source;
    (void)signaled_id;
    (void)effective_id;
    (void)available;
    (void)fallback;
    return 0;
}
