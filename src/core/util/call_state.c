// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/platform/timing.h>

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "call_state_internal.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

static dsd_mutex_t g_call_state_alloc_mutex;
static atomic_int g_call_state_alloc_mutex_state = 0; /* 0=uninit, 1=initing, 2=init */

#ifdef DSD_NEO_TEST_HOOKS
static long g_call_state_alloc_fail_after = -1;
static long g_call_state_alloc_calls = 0;
#endif

static void
call_state_alloc_mutex_init(void) {
    if (atomic_load(&g_call_state_alloc_mutex_state) == 2) {
        return;
    }
    int expected = 0;
    if (atomic_compare_exchange_strong(&g_call_state_alloc_mutex_state, &expected, 1)) {
        (void)dsd_mutex_init(&g_call_state_alloc_mutex);
        atomic_store(&g_call_state_alloc_mutex_state, 2);
        return;
    }
    while (atomic_load(&g_call_state_alloc_mutex_state) != 2) {}
}

static void*
call_state_calloc(size_t count, size_t size) {
#ifdef DSD_NEO_TEST_HOOKS
    if (g_call_state_alloc_fail_after >= 0 && g_call_state_alloc_calls >= g_call_state_alloc_fail_after) {
        return NULL;
    }
    g_call_state_alloc_calls++;
#endif
    return calloc(count, size);
}

static void
call_state_ext_cleanup(void* opaque) {
    dsd_call_state_ext* ext = (dsd_call_state_ext*)opaque;
    if (!ext) {
        return;
    }
    (void)dsd_mutex_destroy(&ext->mutex);
    free(ext);
}

static dsd_call_state_ext*
call_state_ext_allocate(void) {
    dsd_call_state_ext* ext = (dsd_call_state_ext*)call_state_calloc(1U, sizeof(*ext));
    if (!ext) {
        return NULL;
    }
    if (dsd_mutex_init(&ext->mutex) != 0) {
        free(ext);
        return NULL;
    }
    for (int slot = 0; slot < DSD_CALL_STATE_SLOT_COUNT; slot++) {
        ext->calls.slots[slot].slot = (uint8_t)slot;
        ext->calls.slots[slot].phase = DSD_CALL_PHASE_IDLE;
        ext->calls.slots[slot].protocol = DSD_SYNC_NONE;
    }
    return ext;
}

dsd_call_state_ext*
dsd_call_state_ext_get(dsd_state* state, int create) {
    if (!state) {
        return NULL;
    }
    dsd_call_state_ext* ext = DSD_STATE_EXT_GET_AS(dsd_call_state_ext, state, DSD_STATE_EXT_CORE_CALL_STATE);
    if (ext || !create) {
        return ext;
    }

    call_state_alloc_mutex_init();
    (void)dsd_mutex_lock(&g_call_state_alloc_mutex);
    ext = DSD_STATE_EXT_GET_AS(dsd_call_state_ext, state, DSD_STATE_EXT_CORE_CALL_STATE);
    if (!ext) {
        ext = call_state_ext_allocate();
        if (ext && dsd_state_ext_set(state, DSD_STATE_EXT_CORE_CALL_STATE, ext, call_state_ext_cleanup) != 0) {
            call_state_ext_cleanup(ext);
            ext = NULL;
        }
    }
    (void)dsd_mutex_unlock(&g_call_state_alloc_mutex);
    return ext;
}

const dsd_call_state_ext*
dsd_call_state_ext_peek(const dsd_state* state) {
    return state ? (const dsd_call_state_ext*)dsd_state_ext_get_const(state, DSD_STATE_EXT_CORE_CALL_STATE) : NULL;
}

void
dsd_call_state_ext_lock(const dsd_call_state_ext* ext) {
    if (ext) {
        (void)dsd_mutex_lock((dsd_mutex_t*)&ext->mutex);
    }
}

void
dsd_call_state_ext_unlock(const dsd_call_state_ext* ext) {
    if (ext) {
        (void)dsd_mutex_unlock((dsd_mutex_t*)&ext->mutex);
    }
}

static double
call_state_observed_m(double observed_m) {
    return observed_m > 0.0 ? observed_m : (double)dsd_time_monotonic_ms() / 1000.0;
}

static uint64_t
call_state_next_nonzero(uint64_t value) {
    value++;
    return value == 0U ? 1U : value;
}

static uint64_t
call_state_effective_target_observation(const dsd_call_observation* observation) {
    return observation->ota_target_id != 0U ? observation->ota_target_id : observation->policy_target_id;
}

static uint64_t
call_state_effective_target_snapshot(const dsd_call_snapshot* snapshot) {
    return snapshot->ota_target_id != 0U ? snapshot->ota_target_id : snapshot->policy_target_id;
}

static int
call_state_protocol_family(int protocol) {
    if (DSD_SYNC_IS_P25P1(protocol)) {
        return 1;
    }
    if (DSD_SYNC_IS_P25P2(protocol)) {
        return 2;
    }
    if (DSD_SYNC_IS_X2TDMA(protocol)) {
        return 3;
    }
    if (DSD_SYNC_IS_DSTAR(protocol)) {
        return 4;
    }
    if (DSD_SYNC_IS_M17(protocol)) {
        return 5;
    }
    if (DSD_SYNC_IS_DMR(protocol)) {
        return 6;
    }
    if (DSD_SYNC_IS_EDACS(protocol)) {
        return 7;
    }
    if (DSD_SYNC_IS_DPMR(protocol)) {
        return 8;
    }
    if (DSD_SYNC_IS_NXDN(protocol)) {
        return 9;
    }
    if (DSD_SYNC_IS_YSF(protocol)) {
        return 10;
    }
    return protocol == DSD_SYNC_NONE ? 0 : 1000 + protocol;
}

static void
call_state_normalize_text(char dst[DSD_CALL_IDENTITY_TEXT_SIZE], const char* src) {
    size_t out = 0U;
    int pending_space = 0;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    while (*src != '\0' && isspace((unsigned char)*src)) {
        src++;
    }
    for (; *src != '\0'; src++) {
        unsigned char ch = (unsigned char)*src;
        if (isspace(ch)) {
            pending_space = out != 0U;
            continue;
        }
        if (pending_space && out + 1U < DSD_CALL_IDENTITY_TEXT_SIZE) {
            dst[out++] = ' ';
        }
        pending_space = 0;
        if (out + 1U < DSD_CALL_IDENTITY_TEXT_SIZE) {
            dst[out++] = (ch < 0x20U || ch == 0x7FU) ? '_' : (char)ch;
        }
    }
    dst[out] = '\0';
}

static int
call_state_known_text_changed(const char* current, const char* incoming) {
    char normalized[DSD_CALL_IDENTITY_TEXT_SIZE];
    call_state_normalize_text(normalized, incoming);
    return current[0] != '\0' && normalized[0] != '\0' && strcmp(current, normalized) != 0;
}

static int
call_state_kind_changed(dsd_call_kind old_kind, dsd_call_kind new_kind) {
    if (old_kind == DSD_CALL_KIND_UNKNOWN || new_kind == DSD_CALL_KIND_UNKNOWN) {
        return 0;
    }
    if ((old_kind == DSD_CALL_KIND_VOICE || new_kind == DSD_CALL_KIND_VOICE)
        && (old_kind == DSD_CALL_KIND_VOICE || old_kind == DSD_CALL_KIND_GROUP_VOICE
            || old_kind == DSD_CALL_KIND_PRIVATE_VOICE)
        && (new_kind == DSD_CALL_KIND_VOICE || new_kind == DSD_CALL_KIND_GROUP_VOICE
            || new_kind == DSD_CALL_KIND_PRIVATE_VOICE)) {
        return 0;
    }
    return old_kind != new_kind;
}

static int
call_state_observation_begins_epoch(const dsd_call_snapshot* current, const dsd_call_observation* observation,
                                    dsd_call_boundary boundary) {
    if (boundary == DSD_CALL_BOUNDARY_BEGIN || current->phase != DSD_CALL_PHASE_ACTIVE) {
        return 1;
    }
    if (current->protocol != DSD_SYNC_NONE && observation->protocol != DSD_SYNC_NONE
        && call_state_protocol_family(current->protocol) != call_state_protocol_family(observation->protocol)) {
        return 1;
    }
    const uint64_t old_target = call_state_effective_target_snapshot(current);
    const uint64_t new_target = call_state_effective_target_observation(observation);
    if (old_target != 0U && new_target != 0U && old_target != new_target) {
        return 1;
    }
    if (call_state_kind_changed(current->kind, observation->kind)) {
        return 1;
    }
    if (current->ota_source_id != 0U && observation->ota_source_id != 0U
        && current->ota_source_id != observation->ota_source_id) {
        return 1;
    }
    return call_state_known_text_changed(current->source_text, observation->source_text)
           || call_state_known_text_changed(current->target_text, observation->target_text);
}

static void
call_state_apply_text(char dst[DSD_CALL_IDENTITY_TEXT_SIZE], const char* src) {
    char normalized[DSD_CALL_IDENTITY_TEXT_SIZE];
    call_state_normalize_text(normalized, src);
    if (normalized[0] != '\0') {
        DSD_MEMCPY(dst, normalized, sizeof(normalized));
    }
}

static void
call_state_apply_observation(dsd_call_snapshot* snapshot, const dsd_call_observation* observation) {
    if (observation->protocol != DSD_SYNC_NONE) {
        snapshot->protocol = observation->protocol;
    }
    if (observation->kind != DSD_CALL_KIND_UNKNOWN
        && !(observation->kind == DSD_CALL_KIND_VOICE
             && (snapshot->kind == DSD_CALL_KIND_GROUP_VOICE || snapshot->kind == DSD_CALL_KIND_PRIVATE_VOICE))) {
        snapshot->kind = observation->kind;
    }
    if (observation->ota_target_id != 0U) {
        snapshot->ota_target_id = observation->ota_target_id;
    }
    if (observation->policy_target_id != 0U) {
        snapshot->policy_target_id = observation->policy_target_id;
    }
    if (observation->ota_source_id != 0U) {
        snapshot->ota_source_id = observation->ota_source_id;
    }
    call_state_apply_text(snapshot->source_text, observation->source_text);
    call_state_apply_text(snapshot->target_text, observation->target_text);
    call_state_apply_text(snapshot->route_text[0], observation->route_text[0]);
    call_state_apply_text(snapshot->route_text[1], observation->route_text[1]);
    if (observation->channel != 0U) {
        snapshot->channel = observation->channel;
    }
    if (observation->frequency_hz != 0) {
        snapshot->frequency_hz = observation->frequency_hz;
    }
    if (observation->has_service_metadata != 0U) {
        snapshot->service_options = observation->service_options;
        snapshot->emergency = observation->emergency;
        snapshot->priority = observation->priority;
        snapshot->has_service_metadata = 1U;
    }
}

int
dsd_call_state_observe(dsd_state* state, const dsd_call_observation* observation, dsd_call_boundary boundary) {
    if (!state || !observation || observation->slot >= DSD_CALL_STATE_SLOT_COUNT) {
        return -1;
    }
    dsd_call_state_ext* ext = dsd_call_state_ext_get(state, 1);
    if (!ext) {
        return -1;
    }
    dsd_call_state_ext_lock(ext);
    dsd_call_snapshot* snapshot = &ext->calls.slots[observation->slot];
    const int begins_epoch = call_state_observation_begins_epoch(snapshot, observation, boundary);
    const double now_m = call_state_observed_m(observation->observed_m);
    if (begins_epoch) {
        DSD_MEMSET(snapshot, 0, sizeof(*snapshot));
        ext->epoch_sequence[observation->slot] = call_state_next_nonzero(ext->epoch_sequence[observation->slot]);
        snapshot->epoch = ext->epoch_sequence[observation->slot];
        snapshot->slot = observation->slot;
        snapshot->protocol = DSD_SYNC_NONE;
        snapshot->crypto = DSD_CALL_CRYPTO_UNKNOWN;
        snapshot->started_m = now_m;
        ext->events[observation->slot].ended_committed = 0U;
        ext->events[observation->slot].notice_epoch = 0U;
        ext->events[observation->slot].notice_target_id = 0U;
        ext->events[observation->slot].notice_kind = DSD_CALL_KIND_UNKNOWN;
        ext->events[observation->slot].notice_handled = 0U;
    }
    snapshot->phase = DSD_CALL_PHASE_ACTIVE;
    call_state_apply_observation(snapshot, observation);
    snapshot->updated_m = now_m;
    snapshot->ended_m = 0.0;
    snapshot->revision = call_state_next_nonzero(snapshot->revision);
    ext->calls.revision = call_state_next_nonzero(ext->calls.revision);
    dsd_call_state_ext_unlock(ext);
    return begins_epoch;
}

int
dsd_call_state_update_crypto(dsd_state* state, uint8_t slot, const dsd_call_crypto_update* update) {
    if (!state || !update || slot >= DSD_CALL_STATE_SLOT_COUNT) {
        return -1;
    }
    dsd_call_state_ext* ext = dsd_call_state_ext_get(state, 0);
    if (!ext) {
        return -1;
    }
    dsd_call_state_ext_lock(ext);
    dsd_call_snapshot* snapshot = &ext->calls.slots[slot];
    if (snapshot->epoch == 0U || snapshot->phase != DSD_CALL_PHASE_ACTIVE) {
        dsd_call_state_ext_unlock(ext);
        return 0;
    }
    snapshot->crypto = update->classification;
    snapshot->algid = update->algid;
    snapshot->kid = update->kid;
    snapshot->mi = update->mi;
    snapshot->audio_permitted = update->audio_permitted ? 1U : 0U;
    snapshot->updated_m = call_state_observed_m(update->observed_m);
    snapshot->revision = call_state_next_nonzero(snapshot->revision);
    ext->calls.revision = call_state_next_nonzero(ext->calls.revision);
    dsd_call_state_ext_unlock(ext);
    return 1;
}

int
dsd_call_state_update_media(dsd_state* state, uint8_t slot, int media_active, double observed_m) {
    if (!state || slot >= DSD_CALL_STATE_SLOT_COUNT) {
        return -1;
    }
    dsd_call_state_ext* ext = dsd_call_state_ext_get(state, 0);
    if (!ext) {
        return -1;
    }
    dsd_call_state_ext_lock(ext);
    dsd_call_snapshot* snapshot = &ext->calls.slots[slot];
    if (snapshot->epoch == 0U || snapshot->phase != DSD_CALL_PHASE_ACTIVE) {
        dsd_call_state_ext_unlock(ext);
        return 0;
    }
    snapshot->media_active = media_active ? 1U : 0U;
    snapshot->updated_m = call_state_observed_m(observed_m);
    snapshot->revision = call_state_next_nonzero(snapshot->revision);
    ext->calls.revision = call_state_next_nonzero(ext->calls.revision);
    dsd_call_state_ext_unlock(ext);
    return 1;
}

int
dsd_call_state_end(dsd_state* state, uint8_t slot, double observed_m) {
    if (!state || slot >= DSD_CALL_STATE_SLOT_COUNT) {
        return -1;
    }
    dsd_call_state_ext* ext = dsd_call_state_ext_get(state, 0);
    if (!ext) {
        return -1;
    }
    dsd_call_state_ext_lock(ext);
    dsd_call_snapshot* snapshot = &ext->calls.slots[slot];
    if (snapshot->epoch == 0U || snapshot->phase != DSD_CALL_PHASE_ACTIVE) {
        dsd_call_state_ext_unlock(ext);
        return 0;
    }
    snapshot->phase = DSD_CALL_PHASE_ENDED;
    snapshot->media_active = 0U;
    snapshot->audio_permitted = 0U;
    snapshot->ended_m = call_state_observed_m(observed_m);
    snapshot->updated_m = snapshot->ended_m;
    snapshot->revision = call_state_next_nonzero(snapshot->revision);
    ext->calls.revision = call_state_next_nonzero(ext->calls.revision);
    dsd_call_state_ext_unlock(ext);
    return 1;
}

int
dsd_call_state_get(const dsd_state* state, uint8_t slot, dsd_call_snapshot* out) {
    if (!state || !out || slot >= DSD_CALL_STATE_SLOT_COUNT) {
        return -1;
    }
    const dsd_call_state_ext* ext = dsd_call_state_ext_peek(state);
    if (!ext) {
        return 0;
    }
    dsd_call_state_ext_lock(ext);
    *out = ext->calls.slots[slot];
    dsd_call_state_ext_unlock(ext);
    return out->epoch != 0U ? 1 : 0;
}

int
dsd_call_state_copy_snapshot(const dsd_state* state, dsd_call_state_snapshot* out) {
    if (!state || !out) {
        return -1;
    }
    const dsd_call_state_ext* ext = dsd_call_state_ext_peek(state);
    if (!ext) {
        DSD_MEMSET(out, 0, sizeof(*out));
        return 0;
    }
    dsd_call_state_ext_lock(ext);
    *out = ext->calls;
    dsd_call_state_ext_unlock(ext);
    return 1;
}

int
dsd_call_state_restore_snapshot(dsd_state* state, const dsd_call_state_snapshot* snapshot) {
    if (!state || !snapshot) {
        return -1;
    }
    dsd_call_state_ext* ext = dsd_call_state_ext_get(state, 1);
    if (!ext) {
        return 0;
    }
    dsd_call_state_ext_lock(ext);
    ext->calls = *snapshot;
    for (int slot = 0; slot < DSD_CALL_STATE_SLOT_COUNT; slot++) {
        if (ext->epoch_sequence[slot] < snapshot->slots[slot].epoch) {
            ext->epoch_sequence[slot] = snapshot->slots[slot].epoch;
        }
    }
    dsd_call_state_ext_unlock(ext);
    return 1;
}

int
dsd_call_state_enrich_text(dsd_state* state, uint8_t slot, uint64_t epoch, const char* source_text,
                           const char* target_text, const char* route0_text, const char* route1_text,
                           double observed_m) {
    if (!state || slot >= DSD_CALL_STATE_SLOT_COUNT || epoch == 0U) {
        return -1;
    }
    dsd_call_state_ext* ext = dsd_call_state_ext_get(state, 0);
    if (!ext) {
        return 0;
    }
    dsd_call_state_ext_lock(ext);
    dsd_call_snapshot* snapshot = &ext->calls.slots[slot];
    if (snapshot->phase != DSD_CALL_PHASE_ACTIVE || snapshot->epoch != epoch) {
        dsd_call_state_ext_unlock(ext);
        return 0;
    }
    call_state_apply_text(snapshot->source_text, source_text);
    call_state_apply_text(snapshot->target_text, target_text);
    call_state_apply_text(snapshot->route_text[0], route0_text);
    call_state_apply_text(snapshot->route_text[1], route1_text);
    snapshot->updated_m = call_state_observed_m(observed_m);
    snapshot->revision = call_state_next_nonzero(snapshot->revision);
    ext->calls.revision = call_state_next_nonzero(ext->calls.revision);
    dsd_call_state_ext_unlock(ext);
    return 1;
}

static int
recent_activity_index_valid(uint8_t index) {
    return index < DSD_RECENT_ACTIVITY_COUNT;
}

static void
recent_activity_copy_text(char* dst, size_t dst_size, const char* text) {
    if (!text) {
        dst[0] = '\0';
        return;
    }
    DSD_SNPRINTF(dst, dst_size, "%s", text);
}

int
dsd_recent_activity_publish(dsd_state* state, uint8_t index, const dsd_call_observation* observation,
                            const char* notice, uint64_t observed_m_ms) {
    if (!state || !recent_activity_index_valid(index) || (!observation && (!notice || notice[0] == '\0'))) {
        return -1;
    }
    const uint64_t now_ms = observed_m_ms != 0U ? observed_m_ms : dsd_time_monotonic_ms();
    dsd_call_state_ext* ext = dsd_call_state_ext_get(state, 1);
    if (!ext) {
        return -1;
    }
    dsd_call_state_ext_lock(ext);
    dsd_recent_activity_entry* entry = &ext->recent.entries[index];
    DSD_MEMSET(entry, 0, sizeof(*entry));
    if (observation) {
        entry->observation = *observation;
        call_state_normalize_text(entry->observation.source_text, observation->source_text);
        call_state_normalize_text(entry->observation.target_text, observation->target_text);
        call_state_normalize_text(entry->observation.route_text[0], observation->route_text[0]);
        call_state_normalize_text(entry->observation.route_text[1], observation->route_text[1]);
    }
    recent_activity_copy_text(entry->notice, sizeof(entry->notice), notice);
    entry->updated_m_ms = now_ms;
    ext->recent.revision = call_state_next_nonzero(ext->recent.revision);
    dsd_call_state_ext_unlock(ext);
    return 1;
}

int
dsd_recent_activity_clear(dsd_state* state, uint8_t index) {
    if (!state || !recent_activity_index_valid(index)) {
        return -1;
    }
    dsd_call_state_ext* ext = dsd_call_state_ext_get(state, 0);
    if (!ext) {
        return 0;
    }
    dsd_call_state_ext_lock(ext);
    DSD_MEMSET(&ext->recent.entries[index], 0, sizeof(ext->recent.entries[index]));
    ext->recent.revision = call_state_next_nonzero(ext->recent.revision);
    dsd_call_state_ext_unlock(ext);
    return 1;
}

int
dsd_recent_activity_clear_all(dsd_state* state) {
    if (!state) {
        return -1;
    }
    dsd_call_state_ext* ext = dsd_call_state_ext_get(state, 1);
    if (ext) {
        dsd_call_state_ext_lock(ext);
        DSD_MEMSET(&ext->recent.entries, 0, sizeof(ext->recent.entries));
        ext->recent.revision = call_state_next_nonzero(ext->recent.revision);
        dsd_call_state_ext_unlock(ext);
        return 1;
    }
    return 0;
}

int
dsd_recent_activity_copy_snapshot(const dsd_state* state, dsd_recent_activity_snapshot* out) {
    if (!state || !out) {
        return -1;
    }
    const dsd_call_state_ext* ext = dsd_call_state_ext_peek(state);
    if (!ext) {
        DSD_MEMSET(out, 0, sizeof(*out));
        return 0;
    }
    dsd_call_state_ext_lock(ext);
    *out = ext->recent;
    dsd_call_state_ext_unlock(ext);
    return 1;
}

int
dsd_recent_activity_restore_snapshot(dsd_state* state, const dsd_recent_activity_snapshot* snapshot) {
    if (!state || !snapshot) {
        return -1;
    }
    dsd_call_state_ext* ext = dsd_call_state_ext_get(state, 1);
    if (!ext) {
        return 0;
    }
    dsd_call_state_ext_lock(ext);
    ext->recent = *snapshot;
    dsd_call_state_ext_unlock(ext);
    return 1;
}

int
dsd_recent_activity_expire(dsd_state* state, uint64_t now_m_ms, uint64_t ttl_ms) {
    if (!state) {
        return -1;
    }
    const uint64_t now_ms = now_m_ms != 0U ? now_m_ms : dsd_time_monotonic_ms();
    const uint64_t max_age_ms = ttl_ms != 0U ? ttl_ms : DSD_RECENT_ACTIVITY_TTL_MS;
    dsd_call_state_ext* ext = dsd_call_state_ext_get(state, 0);
    if (!ext) {
        return 0;
    }
    int expired = 0;
    dsd_call_state_ext_lock(ext);
    for (int i = 0; i < DSD_RECENT_ACTIVITY_COUNT; i++) {
        dsd_recent_activity_entry* entry = &ext->recent.entries[i];
        if (entry->updated_m_ms == 0U || now_ms < entry->updated_m_ms || now_ms - entry->updated_m_ms <= max_age_ms) {
            continue;
        }
        DSD_MEMSET(entry, 0, sizeof(*entry));
        expired++;
    }
    if (expired > 0) {
        ext->recent.revision = call_state_next_nonzero(ext->recent.revision);
    }
    dsd_call_state_ext_unlock(ext);
    return expired;
}

int
dsd_recent_activity_save(const dsd_state* state, uint8_t index, dsd_recent_activity_transaction* transaction) {
    if (!state || !transaction || !recent_activity_index_valid(index)) {
        return -1;
    }
    DSD_MEMSET(transaction, 0, sizeof(*transaction));
    transaction->valid = 1U;
    transaction->index = index;
    const dsd_call_state_ext* ext = dsd_call_state_ext_peek(state);
    if (!ext) {
        return 0;
    }
    dsd_call_state_ext_lock(ext);
    transaction->entry = ext->recent.entries[index];
    dsd_call_state_ext_unlock(ext);
    return 1;
}

int
dsd_recent_activity_restore(dsd_state* state, const dsd_recent_activity_transaction* transaction) {
    if (!state || !transaction || !transaction->valid || !recent_activity_index_valid(transaction->index)) {
        return -1;
    }
    dsd_call_state_ext* ext = dsd_call_state_ext_get(state, 0);
    if (ext) {
        dsd_call_state_ext_lock(ext);
        ext->recent.entries[transaction->index] = transaction->entry;
        ext->recent.revision = call_state_next_nonzero(ext->recent.revision);
        dsd_call_state_ext_unlock(ext);
        return 1;
    }
    return 0;
}

int
dsd_call_state_copy_to_state(dsd_state* dst, const dsd_state* src) {
    if (!dst || !src) {
        return -1;
    }
    const dsd_call_state_ext* src_ext = dsd_call_state_ext_peek(src);
    if (!src_ext) {
        (void)dsd_state_ext_set(dst, DSD_STATE_EXT_CORE_CALL_STATE, NULL, NULL);
        return 0;
    }
    dsd_call_state_snapshot calls;
    dsd_recent_activity_snapshot recent;
    dsd_call_event_lifecycle events[DSD_CALL_STATE_SLOT_COUNT];
    uint64_t epoch_sequence[DSD_CALL_STATE_SLOT_COUNT];
    dsd_call_state_ext_lock(src_ext);
    calls = src_ext->calls;
    recent = src_ext->recent;
    DSD_MEMCPY(events, src_ext->events, sizeof(events));
    DSD_MEMCPY(epoch_sequence, src_ext->epoch_sequence, sizeof(epoch_sequence));
    dsd_call_state_ext_unlock(src_ext);

    dsd_call_state_ext* dst_ext = dsd_call_state_ext_get(dst, 1);
    if (!dst_ext) {
        (void)dsd_state_ext_set(dst, DSD_STATE_EXT_CORE_CALL_STATE, NULL, NULL);
        return -1;
    }
    dsd_call_state_ext_lock(dst_ext);
    dst_ext->calls = calls;
    dst_ext->recent = recent;
    DSD_MEMCPY(dst_ext->events, events, sizeof(events));
    DSD_MEMCPY(dst_ext->epoch_sequence, epoch_sequence, sizeof(epoch_sequence));
    dsd_call_state_ext_unlock(dst_ext);
    return 1;
}

#ifdef DSD_NEO_TEST_HOOKS
void
dsd_call_state_test_alloc_reset(void) {
    g_call_state_alloc_calls = 0;
    g_call_state_alloc_fail_after = -1;
}

void
dsd_call_state_test_alloc_fail_after(long fail_after) {
    g_call_state_alloc_calls = 0;
    g_call_state_alloc_fail_after = fail_after;
}

int
dsd_call_state_test_set_epoch(dsd_state* state, uint8_t slot, uint64_t epoch) {
    if (!state || slot >= DSD_CALL_STATE_SLOT_COUNT) {
        return -1;
    }
    dsd_call_state_ext* ext = dsd_call_state_ext_get(state, 0);
    if (!ext) {
        return 0;
    }
    dsd_call_state_ext_lock(ext);
    ext->calls.slots[slot].epoch = epoch;
    ext->epoch_sequence[slot] = epoch;
    dsd_call_state_ext_unlock(ext);
    return 1;
}
#endif
