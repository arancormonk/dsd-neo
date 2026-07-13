// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stddef.h>
#include <stdint.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/platform/timing.h"

static dsd_trunk_tuning_hooks g_trunk_tuning_hooks = {0};
static dsd_atomic_u64 g_trunk_tuning_generation = {1U};
static dsd_atomic_u64 g_trunk_tuning_next_request = {0U};
static atomic_int g_trunk_tuning_request_lock = 0;
static atomic_int g_trunk_tuning_unresolved_count = 0;

#define DSD_TRUNK_TUNING_REQUEST_HISTORY 64

typedef enum {
    DSD_TRUNK_TUNING_REQUEST_FREE = 0,
    DSD_TRUNK_TUNING_REQUEST_PENDING,
    DSD_TRUNK_TUNING_REQUEST_FAILED_GATED,
    DSD_TRUNK_TUNING_REQUEST_FINISHED,
} dsd_trunk_tuning_request_state;

typedef struct {
    uint64_t request_id;
    uint64_t completed_m_ns;
    dsd_trunk_tune_result result;
    dsd_trunk_tuning_request_state state;
    int inherited_failure_gate;
    int backend_complete;
    int owner_ready;
} dsd_trunk_tuning_request_record;

static dsd_trunk_tuning_request_record g_trunk_tuning_requests[DSD_TRUNK_TUNING_REQUEST_HISTORY] = {{0}};

static void
dsd_trunk_tuning_requests_lock(void) {
    int expected = 0;
    while (!atomic_compare_exchange_strong(&g_trunk_tuning_request_lock, &expected, 1)) {
        expected = 0;
    }
}

static void
dsd_trunk_tuning_requests_unlock(void) {
    atomic_store(&g_trunk_tuning_request_lock, 0);
}

static dsd_trunk_tuning_request_record*
dsd_trunk_tuning_request_find_locked(uint64_t request_id) {
    for (int i = 0; i < DSD_TRUNK_TUNING_REQUEST_HISTORY; i++) {
        if (g_trunk_tuning_requests[i].request_id == request_id
            && g_trunk_tuning_requests[i].state != DSD_TRUNK_TUNING_REQUEST_FREE) {
            return &g_trunk_tuning_requests[i];
        }
    }
    return NULL;
}

static dsd_trunk_tuning_request_record*
dsd_trunk_tuning_request_slot_locked(int* out_inherited_failure_gate) {
    dsd_trunk_tuning_request_record* oldest_finished = NULL;
    dsd_trunk_tuning_request_record* oldest_failed = NULL;
    if (out_inherited_failure_gate) {
        *out_inherited_failure_gate = 0;
    }
    for (int i = 0; i < DSD_TRUNK_TUNING_REQUEST_HISTORY; i++) {
        dsd_trunk_tuning_request_record* record = &g_trunk_tuning_requests[i];
        if (record->state == DSD_TRUNK_TUNING_REQUEST_FREE) {
            return record;
        }
        if (record->state == DSD_TRUNK_TUNING_REQUEST_FINISHED
            && (!oldest_finished || record->request_id < oldest_finished->request_id)) {
            oldest_finished = record;
        }
        if (record->state == DSD_TRUNK_TUNING_REQUEST_FAILED_GATED
            && (!oldest_failed || record->request_id < oldest_failed->request_id)) {
            oldest_failed = record;
        }
    }
    if (oldest_finished) {
        return oldest_finished;
    }
    if (oldest_failed && out_inherited_failure_gate) {
        *out_inherited_failure_gate = 1;
    }
    return oldest_failed;
}

static void
dsd_trunk_tuning_retire_failed_requests_locked(uint64_t successful_request_id) {
    for (int i = 0; i < DSD_TRUNK_TUNING_REQUEST_HISTORY; i++) {
        dsd_trunk_tuning_request_record* record = &g_trunk_tuning_requests[i];
        if (record->state == DSD_TRUNK_TUNING_REQUEST_FAILED_GATED && record->request_id < successful_request_id) {
            record->state = DSD_TRUNK_TUNING_REQUEST_FINISHED;
            (void)atomic_fetch_add(&g_trunk_tuning_unresolved_count, -1);
        }
    }
}

static void
dsd_trunk_tuning_finish_success_locked(dsd_trunk_tuning_request_record* record) {
    if (!record || record->state != DSD_TRUNK_TUNING_REQUEST_PENDING) {
        return;
    }
    const uint64_t request_id = record->request_id;
    dsd_trunk_tuning_generation_advance();
    record->state = DSD_TRUNK_TUNING_REQUEST_FINISHED;
    (void)atomic_fetch_add(&g_trunk_tuning_unresolved_count, -1);
    dsd_trunk_tuning_retire_failed_requests_locked(request_id);
}

static void
dsd_trunk_tuning_finish_rolled_back_locked(dsd_trunk_tuning_request_record* record) {
    if (!record || record->state == DSD_TRUNK_TUNING_REQUEST_FINISHED) {
        return;
    }
    if (record->inherited_failure_gate) {
        record->state = DSD_TRUNK_TUNING_REQUEST_FAILED_GATED;
        return;
    }
    record->state = DSD_TRUNK_TUNING_REQUEST_FINISHED;
    (void)atomic_fetch_add(&g_trunk_tuning_unresolved_count, -1);
}

uint64_t
dsd_trunk_tuning_generation(void) {
    return dsd_atomic_u64_load_acquire(&g_trunk_tuning_generation);
}

void
dsd_trunk_tuning_generation_advance(void) {
    (void)dsd_atomic_u64_fetch_add_release(&g_trunk_tuning_generation, 1U);
}

uint64_t
dsd_trunk_tuning_request_begin(void) {
    uint64_t request_id = 0U;
    do {
        request_id = dsd_atomic_u64_fetch_add_release(&g_trunk_tuning_next_request, 1U) + 1U;
    } while (request_id == 0U);

    dsd_trunk_tuning_requests_lock();
    int inherited_failure_gate = 0;
    dsd_trunk_tuning_request_record* record = dsd_trunk_tuning_request_slot_locked(&inherited_failure_gate);
    if (!record) {
        dsd_trunk_tuning_requests_unlock();
        return 0U;
    }
    record->request_id = request_id;
    record->completed_m_ns = 0U;
    record->result = DSD_TRUNK_TUNE_RESULT_PENDING;
    record->state = DSD_TRUNK_TUNING_REQUEST_PENDING;
    record->inherited_failure_gate = inherited_failure_gate;
    record->backend_complete = 0;
    record->owner_ready = 0;
    if (!inherited_failure_gate) {
        (void)atomic_fetch_add(&g_trunk_tuning_unresolved_count, 1);
    }
    dsd_trunk_tuning_requests_unlock();
    return request_id;
}

void
dsd_trunk_tuning_requests_reset(void) {
    dsd_trunk_tuning_requests_lock();
    for (int i = 0; i < DSD_TRUNK_TUNING_REQUEST_HISTORY; i++) {
        g_trunk_tuning_requests[i] = (dsd_trunk_tuning_request_record){0};
    }
    atomic_store(&g_trunk_tuning_unresolved_count, 0);
    dsd_trunk_tuning_requests_unlock();
}

void
dsd_trunk_tuning_retire_failed_requests(void) {
    if (atomic_load(&g_trunk_tuning_unresolved_count) == 0) {
        return;
    }

    dsd_trunk_tuning_requests_lock();
    for (int i = 0; i < DSD_TRUNK_TUNING_REQUEST_HISTORY; i++) {
        dsd_trunk_tuning_request_record* record = &g_trunk_tuning_requests[i];
        if (record->state == DSD_TRUNK_TUNING_REQUEST_FAILED_GATED) {
            record->state = DSD_TRUNK_TUNING_REQUEST_FINISHED;
            (void)atomic_fetch_add(&g_trunk_tuning_unresolved_count, -1);
        }
    }
    dsd_trunk_tuning_requests_unlock();
}

void
dsd_trunk_tuning_request_publish(uint64_t request_id, dsd_trunk_tune_result result) {
    if (request_id == 0U || result == DSD_TRUNK_TUNE_RESULT_PENDING) {
        return;
    }

    dsd_trunk_tuning_requests_lock();
    dsd_trunk_tuning_request_record* record = dsd_trunk_tuning_request_find_locked(request_id);
    if (!record || record->state != DSD_TRUNK_TUNING_REQUEST_PENDING) {
        dsd_trunk_tuning_requests_unlock();
        return;
    }
    record->result = result;
    record->completed_m_ns = dsd_time_monotonic_ns();
    record->backend_complete = 1;
    if (dsd_trunk_tune_result_is_complete(result)) {
        if (record->owner_ready) {
            dsd_trunk_tuning_finish_success_locked(record);
        }
    } else {
        record->state = DSD_TRUNK_TUNING_REQUEST_FAILED_GATED;
    }
    dsd_trunk_tuning_requests_unlock();
}

void
dsd_trunk_tuning_request_mark_ready(uint64_t request_id) {
    if (request_id == 0U) {
        return;
    }

    dsd_trunk_tuning_requests_lock();
    dsd_trunk_tuning_request_record* record = dsd_trunk_tuning_request_find_locked(request_id);
    if (record && record->state == DSD_TRUNK_TUNING_REQUEST_PENDING) {
        record->owner_ready = 1;
        if (record->backend_complete && dsd_trunk_tune_result_is_complete(record->result)) {
            dsd_trunk_tuning_finish_success_locked(record);
        }
    }
    dsd_trunk_tuning_requests_unlock();
}

void
dsd_trunk_tuning_request_complete(uint64_t request_id, dsd_trunk_tune_result result) {
    if (request_id == 0U || result == DSD_TRUNK_TUNE_RESULT_PENDING) {
        return;
    }

    dsd_trunk_tuning_requests_lock();
    dsd_trunk_tuning_request_record* record = dsd_trunk_tuning_request_find_locked(request_id);
    if (!record || record->state == DSD_TRUNK_TUNING_REQUEST_FINISHED) {
        dsd_trunk_tuning_requests_unlock();
        return;
    }
    record->owner_ready = 1;
    if (record->state == DSD_TRUNK_TUNING_REQUEST_FAILED_GATED) {
        /* A backend failure raced the owner return. Only an owner rollback can
         * retire it; a committed owner state must remain gated for recovery. */
        if (!dsd_trunk_tune_result_is_complete(result)) {
            dsd_trunk_tuning_finish_rolled_back_locked(record);
        }
        dsd_trunk_tuning_requests_unlock();
        return;
    }

    if (!record->backend_complete) {
        record->result = result;
        record->completed_m_ns = dsd_time_monotonic_ns();
        record->backend_complete = 1;
        if (dsd_trunk_tune_result_is_complete(result)) {
            dsd_trunk_tuning_finish_success_locked(record);
        } else {
            dsd_trunk_tuning_finish_rolled_back_locked(record);
        }
        dsd_trunk_tuning_requests_unlock();
        return;
    }

    const int backend_committed = dsd_trunk_tune_result_is_complete(record->result);
    const int owner_committed = dsd_trunk_tune_result_is_complete(result);
    if (backend_committed && owner_committed) {
        dsd_trunk_tuning_finish_success_locked(record);
    } else if (!backend_committed && !owner_committed) {
        dsd_trunk_tuning_finish_rolled_back_locked(record);
    } else {
        /* Hardware and decoder ownership disagree. Preserve the frame gate
         * until a newer successful request establishes a coherent boundary. */
        if (!owner_committed) {
            record->result = result;
        }
        record->state = DSD_TRUNK_TUNING_REQUEST_FAILED_GATED;
    }
    dsd_trunk_tuning_requests_unlock();
}

uint64_t
dsd_trunk_tuning_pending_request(void) {
    uint64_t newest_request_id = 0U;
    dsd_trunk_tuning_requests_lock();
    for (int i = 0; i < DSD_TRUNK_TUNING_REQUEST_HISTORY; i++) {
        const dsd_trunk_tuning_request_record* record = &g_trunk_tuning_requests[i];
        if ((record->state == DSD_TRUNK_TUNING_REQUEST_PENDING
             || record->state == DSD_TRUNK_TUNING_REQUEST_FAILED_GATED)
            && record->request_id > newest_request_id) {
            newest_request_id = record->request_id;
        }
    }
    dsd_trunk_tuning_requests_unlock();
    return newest_request_id;
}

dsd_trunk_tune_result
dsd_trunk_tuning_request_status(uint64_t request_id, double* out_completed_m) {
    if (out_completed_m) {
        *out_completed_m = 0.0;
    }
    if (request_id == 0U) {
        return DSD_TRUNK_TUNE_RESULT_FAILED;
    }

    dsd_trunk_tuning_requests_lock();
    const dsd_trunk_tuning_request_record* record = dsd_trunk_tuning_request_find_locked(request_id);
    dsd_trunk_tune_result result = DSD_TRUNK_TUNE_RESULT_FAILED;
    if (record) {
        result = record->state == DSD_TRUNK_TUNING_REQUEST_PENDING ? DSD_TRUNK_TUNE_RESULT_PENDING : record->result;
        if (out_completed_m && record->completed_m_ns != 0U) {
            *out_completed_m = (double)record->completed_m_ns / 1e9;
        }
    }
    dsd_trunk_tuning_requests_unlock();
    return result;
}

int
dsd_trunk_tuning_frame_is_current(uint64_t frame_generation) {
    if (atomic_load(&g_trunk_tuning_unresolved_count) != 0) {
        return 0;
    }
    if (frame_generation != dsd_trunk_tuning_generation()) {
        return 0;
    }
    if (atomic_load(&g_trunk_tuning_unresolved_count) != 0) {
        return 0;
    }
    return frame_generation == dsd_trunk_tuning_generation();
}

int
dsd_trunk_tuning_frame_is_dispatchable(uint64_t frame_generation, int tune_owner_active) {
    if (!tune_owner_active) {
        dsd_trunk_tuning_retire_failed_requests();
    }
    return dsd_trunk_tuning_frame_is_current(frame_generation);
}

static dsd_trunk_tune_result
dsd_trunk_tuning_note_result(uint64_t request_id, dsd_trunk_tune_result result) {
    if (result == DSD_TRUNK_TUNE_RESULT_PENDING) {
        dsd_trunk_tuning_request_mark_ready(request_id);
        dsd_trunk_tune_result status = dsd_trunk_tuning_request_status(request_id, NULL);
        return status == DSD_TRUNK_TUNE_RESULT_OK ? status : result;
    }
    dsd_trunk_tuning_request_complete(request_id, result);
    return dsd_trunk_tuning_request_status(request_id, NULL);
}

void
dsd_trunk_tuning_hooks_set(dsd_trunk_tuning_hooks hooks) {
    g_trunk_tuning_hooks = hooks;
}

dsd_trunk_tune_result
dsd_trunk_tuning_hook_tune_to_freq(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps,
                                   uint64_t* out_request_id) {
    if (out_request_id) {
        *out_request_id = 0U;
    }
    const uint64_t request_id = dsd_trunk_tuning_request_begin();
    if (out_request_id) {
        *out_request_id = request_id;
    }
    if (request_id == 0U) {
        return DSD_TRUNK_TUNE_RESULT_FAILED;
    }
    if (g_trunk_tuning_hooks.tune_to_freq_request) {
        return dsd_trunk_tuning_note_result(
            request_id, g_trunk_tuning_hooks.tune_to_freq_request(opts, state, freq, ted_sps, request_id));
    }
    return dsd_trunk_tuning_note_result(request_id, DSD_TRUNK_TUNE_RESULT_FAILED);
}

dsd_trunk_tune_result
dsd_trunk_tuning_hook_tune_to_cc(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps,
                                 uint64_t* out_request_id) {
    if (out_request_id) {
        *out_request_id = 0U;
    }
    const uint64_t request_id = dsd_trunk_tuning_request_begin();
    if (out_request_id) {
        *out_request_id = request_id;
    }
    if (request_id == 0U) {
        return DSD_TRUNK_TUNE_RESULT_FAILED;
    }
    if (g_trunk_tuning_hooks.tune_to_cc_request) {
        return dsd_trunk_tuning_note_result(
            request_id, g_trunk_tuning_hooks.tune_to_cc_request(opts, state, freq, ted_sps, request_id));
    }
    return dsd_trunk_tuning_note_result(request_id, DSD_TRUNK_TUNE_RESULT_FAILED);
}

dsd_trunk_tune_result
dsd_trunk_tuning_hook_return_to_cc(dsd_opts* opts, dsd_state* state, uint64_t* out_request_id) {
    if (out_request_id) {
        *out_request_id = 0U;
    }
    const uint64_t request_id = dsd_trunk_tuning_request_begin();
    if (out_request_id) {
        *out_request_id = request_id;
    }
    if (request_id == 0U) {
        return DSD_TRUNK_TUNE_RESULT_FAILED;
    }
    if (g_trunk_tuning_hooks.return_to_cc_request) {
        return dsd_trunk_tuning_note_result(request_id,
                                            g_trunk_tuning_hooks.return_to_cc_request(opts, state, request_id));
    }
    return dsd_trunk_tuning_note_result(request_id, DSD_TRUNK_TUNE_RESULT_FAILED);
}
