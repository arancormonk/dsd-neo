// SPDX-License-Identifier: GPL-3.0-or-later
/* Minimal canonical-call hooks for decoder source-isolation tests. */

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/platform/threading.h>
#include <stdint.h>
#include <string.h>

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
    const int begins_epoch = boundary == DSD_CALL_BOUNDARY_BEGIN || call->phase != DSD_CALL_PHASE_ACTIVE;
    if (begins_epoch) {
        DSD_MEMSET(call, 0, sizeof(*call));
        call->epoch = ++g_stub_epoch;
        call->phase = DSD_CALL_PHASE_ACTIVE;
        call->slot = observation->slot;
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
dsd_call_state_update_crypto(dsd_state* state, uint8_t slot, const dsd_call_crypto_update* update) {
    if (state == NULL || slot >= DSD_CALL_STATE_SLOT_COUNT || update == NULL) {
        return -1;
    }
    stub_select_state(state);
    g_stub_calls[slot].crypto = update->classification;
    g_stub_calls[slot].algid = update->algid;
    g_stub_calls[slot].kid = update->kid;
    g_stub_calls[slot].mi = update->mi;
    g_stub_calls[slot].audio_permitted = update->audio_permitted;
    return 0;
}

int
dsd_call_state_update_media(dsd_state* state, uint8_t slot, int media_active, double observed_m) {
    if (state == NULL || slot >= DSD_CALL_STATE_SLOT_COUNT) {
        return -1;
    }
    stub_select_state(state);
    g_stub_calls[slot].media_active = media_active != 0;
    (void)observed_m;
    return 0;
}

int
dsd_call_state_end(dsd_state* state, uint8_t slot, double observed_m) {
    if (state == NULL || slot >= DSD_CALL_STATE_SLOT_COUNT) {
        return -1;
    }
    stub_select_state(state);
    const int ended = g_stub_calls[slot].phase == DSD_CALL_PHASE_ACTIVE;
    g_stub_calls[slot].phase = DSD_CALL_PHASE_ENDED;
    (void)observed_m;
    return ended;
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
    (void)state;
    (void)slot;
}

int
dsd_event_enrich_alias(dsd_state* state, uint8_t slot, uint64_t epoch, const char* alias) {
    (void)state;
    (void)slot;
    (void)epoch;
    (void)alias;
    return 0;
}

int
dsd_event_enrich_gps(dsd_state* state, uint8_t slot, uint64_t epoch, const char* gps) {
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
