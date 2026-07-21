// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/string_utils.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/platform/timing.h>

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

static uint32_t
call_state_effective_target_observation(const dsd_call_observation* observation) {
    if (observation->ota_target_id != 0U) {
        return observation->ota_target_id;
    }
    if (observation->kind == DSD_CALL_KIND_GROUP_VOICE) {
        return observation->group_id;
    }
    if (observation->kind == DSD_CALL_KIND_PRIVATE_VOICE) {
        return observation->private_id;
    }
    return observation->policy_target_id;
}

static uint32_t
call_state_effective_target_snapshot(const dsd_call_snapshot* snapshot) {
    if (snapshot->ota_target_id != 0U) {
        return snapshot->ota_target_id;
    }
    if (snapshot->kind == DSD_CALL_KIND_GROUP_VOICE) {
        return snapshot->group_id;
    }
    if (snapshot->kind == DSD_CALL_KIND_PRIVATE_VOICE) {
        return snapshot->private_id;
    }
    return snapshot->policy_target_id;
}

static int
call_state_kind_changed(dsd_call_kind old_kind, dsd_call_kind new_kind) {
    if (old_kind == DSD_CALL_KIND_UNKNOWN || new_kind == DSD_CALL_KIND_UNKNOWN) {
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
    if (current->protocol != 0 && observation->protocol != 0 && current->protocol != observation->protocol) {
        return 1;
    }
    const uint32_t old_target = call_state_effective_target_snapshot(current);
    const uint32_t new_target = call_state_effective_target_observation(observation);
    if (old_target != 0U && new_target != 0U && old_target != new_target) {
        return 1;
    }
    if (call_state_kind_changed(current->kind, observation->kind)) {
        return 1;
    }
    return current->source_id != 0U && observation->source_id != 0U && current->source_id != observation->source_id;
}

static void
call_state_apply_nonzero_observation(dsd_call_snapshot* snapshot, const dsd_call_observation* observation) {
    if (observation->protocol != 0) {
        snapshot->protocol = observation->protocol;
    }
    if (observation->kind != DSD_CALL_KIND_UNKNOWN) {
        snapshot->kind = observation->kind;
    }
    if (observation->ota_target_id != 0U) {
        snapshot->ota_target_id = observation->ota_target_id;
    }
    if (observation->policy_target_id != 0U) {
        snapshot->policy_target_id = observation->policy_target_id;
    }
    if (observation->source_id != 0U) {
        snapshot->source_id = observation->source_id;
    }
    if (observation->group_id != 0U) {
        snapshot->group_id = observation->group_id;
    }
    if (observation->private_id != 0U) {
        snapshot->private_id = observation->private_id;
    }
    if (observation->channel != 0U) {
        snapshot->channel = observation->channel;
    }
    if (observation->frequency_hz != 0) {
        snapshot->frequency_hz = observation->frequency_hz;
    }
    snapshot->service_options = observation->service_options;
    snapshot->emergency = observation->emergency;
    snapshot->priority = observation->priority;
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
    const uint64_t prior_epoch = snapshot->epoch;
    const double now_m = call_state_observed_m(observation->observed_m);
    if (begins_epoch) {
        DSD_MEMSET(snapshot, 0, sizeof(*snapshot));
        snapshot->epoch = call_state_next_nonzero(prior_epoch);
        snapshot->slot = observation->slot;
        snapshot->crypto = DSD_CALL_CRYPTO_UNKNOWN;
        snapshot->started_m = now_m;
        ext->events[observation->slot].ended_committed = 0U;
        ext->events[observation->slot].notice_handled = 0U;
    }
    snapshot->phase = DSD_CALL_PHASE_ACTIVE;
    call_state_apply_nonzero_observation(snapshot, observation);
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

static void
recent_activity_append_text(char* dst, size_t dst_size, const char* text) {
    const size_t length = strnlen(dst, dst_size);
    if (length >= dst_size) {
        return;
    }
    (void)dsd_strncat_s(dst, dst_size, text, dst_size - length - 1U);
}

int
dsd_recent_activity_set_at(dsd_state* state, uint8_t index, const char* text, uint64_t observed_m_ms) {
    if (!state || !recent_activity_index_valid(index)) {
        return -1;
    }
    const uint64_t now_ms = observed_m_ms != 0U ? observed_m_ms : dsd_time_monotonic_ms();
    dsd_call_state_ext* ext = dsd_call_state_ext_get(state, 1);
    if (ext) {
        dsd_call_state_ext_lock(ext);
        recent_activity_copy_text(ext->recent.entries[index].text, sizeof(ext->recent.entries[index].text), text);
        ext->recent.entries[index].updated_m_ms = ext->recent.entries[index].text[0] != '\0' ? now_ms : 0U;
        ext->recent.revision = call_state_next_nonzero(ext->recent.revision);
        recent_activity_copy_text(state->active_channel[index], sizeof(state->active_channel[index]), text);
        state->last_active_time = time(NULL);
        dsd_call_state_ext_unlock(ext);
        return 1;
    }
    recent_activity_copy_text(state->active_channel[index], sizeof(state->active_channel[index]), text);
    state->last_active_time = time(NULL);
    return 0;
}

int
dsd_recent_activity_set(dsd_state* state, uint8_t index, const char* text) {
    return dsd_recent_activity_set_at(state, index, text, 0U);
}

int
dsd_recent_activity_append_at(dsd_state* state, uint8_t index, const char* text, uint64_t observed_m_ms) {
    if (!state || !text || !recent_activity_index_valid(index)) {
        return -1;
    }
    const uint64_t now_ms = observed_m_ms != 0U ? observed_m_ms : dsd_time_monotonic_ms();
    dsd_call_state_ext* ext = dsd_call_state_ext_get(state, 1);
    if (ext) {
        dsd_call_state_ext_lock(ext);
        recent_activity_append_text(ext->recent.entries[index].text, sizeof(ext->recent.entries[index].text), text);
        ext->recent.entries[index].updated_m_ms = now_ms;
        ext->recent.revision = call_state_next_nonzero(ext->recent.revision);
        DSD_SNPRINTF(state->active_channel[index], sizeof(state->active_channel[index]), "%s",
                     ext->recent.entries[index].text);
        state->last_active_time = time(NULL);
        dsd_call_state_ext_unlock(ext);
        return 1;
    }
    recent_activity_append_text(state->active_channel[index], sizeof(state->active_channel[index]), text);
    state->last_active_time = time(NULL);
    return 0;
}

int
dsd_recent_activity_append(dsd_state* state, uint8_t index, const char* text) {
    return dsd_recent_activity_append_at(state, index, text, 0U);
}

int
dsd_recent_activity_clear(dsd_state* state, uint8_t index) {
    return dsd_recent_activity_set_at(state, index, "", 0U);
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
        DSD_MEMSET(state->active_channel, 0, sizeof(state->active_channel));
        state->last_active_time = time(NULL);
        dsd_call_state_ext_unlock(ext);
        return 1;
    }
    DSD_MEMSET(state->active_channel, 0, sizeof(state->active_channel));
    state->last_active_time = time(NULL);
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
dsd_recent_activity_sync_legacy(dsd_state* state) {
    if (!state) {
        return -1;
    }
    dsd_call_state_ext* ext = dsd_call_state_ext_get(state, 1);
    state->last_active_time = time(NULL);
    if (!ext) {
        return 0;
    }
    const uint64_t now_ms = dsd_time_monotonic_ms();
    int changed = 0;
    dsd_call_state_ext_lock(ext);
    for (int i = 0; i < DSD_RECENT_ACTIVITY_COUNT; i++) {
        if (strncmp(ext->recent.entries[i].text, state->active_channel[i], DSD_RECENT_ACTIVITY_TEXT_SIZE) == 0) {
            continue;
        }
        DSD_SNPRINTF(ext->recent.entries[i].text, sizeof(ext->recent.entries[i].text), "%s", state->active_channel[i]);
        ext->recent.entries[i].updated_m_ms = state->active_channel[i][0] != '\0' ? now_ms : 0U;
        changed = 1;
    }
    if (changed) {
        ext->recent.revision = call_state_next_nonzero(ext->recent.revision);
    }
    dsd_call_state_ext_unlock(ext);
    return changed;
}

int
dsd_recent_activity_sync_legacy_entry(dsd_state* state, uint8_t index) {
    if (!state || !recent_activity_index_valid(index)) {
        return -1;
    }
    const uint64_t now_ms = dsd_time_monotonic_ms();
    dsd_call_state_ext* ext = dsd_call_state_ext_get(state, 1);
    state->last_active_time = time(NULL);
    if (!ext) {
        return 0;
    }
    dsd_call_state_ext_lock(ext);
    DSD_SNPRINTF(ext->recent.entries[index].text, sizeof(ext->recent.entries[index].text), "%s",
                 state->active_channel[index]);
    ext->recent.entries[index].updated_m_ms = state->active_channel[index][0] != '\0' ? now_ms : 0U;
    ext->recent.revision = call_state_next_nonzero(ext->recent.revision);
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
        if ((time(NULL) - state->last_active_time) > (time_t)(max_age_ms / 1000U)) {
            DSD_MEMSET(state->active_channel, 0, sizeof(state->active_channel));
            return 1;
        }
        return 0;
    }
    int expired = 0;
    dsd_call_state_ext_lock(ext);
    for (int i = 0; i < DSD_RECENT_ACTIVITY_COUNT; i++) {
        dsd_recent_activity_entry* entry = &ext->recent.entries[i];
        if (entry->text[0] == '\0' || entry->updated_m_ms == 0U || now_ms < entry->updated_m_ms
            || now_ms - entry->updated_m_ms <= max_age_ms) {
            continue;
        }
        DSD_MEMSET(entry, 0, sizeof(*entry));
        state->active_channel[i][0] = '\0';
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
    transaction->last_active_time = state->last_active_time;
    DSD_SNPRINTF(transaction->legacy_text, sizeof(transaction->legacy_text), "%s", state->active_channel[index]);
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
        DSD_SNPRINTF(state->active_channel[transaction->index], sizeof(state->active_channel[transaction->index]), "%s",
                     transaction->legacy_text);
        state->last_active_time = transaction->last_active_time;
        dsd_call_state_ext_unlock(ext);
        return 1;
    }
    DSD_SNPRINTF(state->active_channel[transaction->index], sizeof(state->active_channel[transaction->index]), "%s",
                 transaction->legacy_text);
    state->last_active_time = transaction->last_active_time;
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
    dsd_call_state_ext_lock(src_ext);
    calls = src_ext->calls;
    recent = src_ext->recent;
    DSD_MEMCPY(events, src_ext->events, sizeof(events));
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
    dsd_call_state_ext_unlock(ext);
    return 1;
}
#endif
