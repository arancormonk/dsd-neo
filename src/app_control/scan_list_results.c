// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/app_control/commands.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/runtime/trunk_scan_hooks.h>
#include <stddef.h>
#include <stdint.h>

#include "command_dispatch.h"
#include "scan_list_results.h"

/* The coordinator's answers pass through unchanged (each side + 0: an int, so no enum types are compared). */
_Static_assert(DSD_APP_SCAN_LIST_SET_OK + 0 == DSD_TRUNK_SCAN_LIST_SET_OK + 0, "OK is the coordinator's");
_Static_assert(DSD_APP_SCAN_LIST_SET_UNCHANGED + 0 == DSD_TRUNK_SCAN_LIST_SET_UNCHANGED + 0,
               "UNCHANGED is the coordinator's");
_Static_assert(DSD_APP_SCAN_LIST_SET_REFUSED_LAST + 0 == DSD_TRUNK_SCAN_LIST_SET_REFUSED_LAST + 0,
               "REFUSED_LAST is the coordinator's");
_Static_assert(DSD_APP_SCAN_LIST_SET_BUSY + 0 == DSD_TRUNK_SCAN_LIST_SET_BUSY + 0, "BUSY is the coordinator's");
_Static_assert(DSD_APP_SCAN_LIST_SET_UNAVAILABLE + 0 == DSD_TRUNK_SCAN_LIST_SET_UNAVAILABLE + 0,
               "UNAVAILABLE is the coordinator's");
_Static_assert(DSD_APP_SCAN_LIST_SET_PAUSED + 0 == DSD_TRUNK_SCAN_LIST_SET_PAUSED + 0, "PAUSED is the coordinator's");
_Static_assert(DSD_APP_SCAN_LIST_SET_STALE + 0 == DSD_TRUNK_SCAN_LIST_SET_STALE + 0, "STALE is the coordinator's");
_Static_assert(DSD_APP_SCAN_LIST_ALL + 0 == DSD_TRUNK_SCAN_LIST_ALL + 0, "every list is the coordinator's");

static dsd_mutex_t result_mutex;
static atomic_int mutex_state = 0;
static dsd_app_scan_list_result newest;
/* The last DSD_APP_RESULT_HISTORY results, at sequence % DSD_APP_RESULT_HISTORY, for dsd_app_scan_list_results_since(). */
static dsd_app_scan_list_result history[DSD_APP_RESULT_HISTORY];
static uint32_t last_request;
/* The decoder's trunk_scan_lists_seq as the last drain left it, for a request dropped before the decoder saw it. */
static uint32_t known_lists_seq;
/* The newest result's sequence when the command session last opened: every result at or below it answered a request
   of an earlier decoder run, for dsd_app_scan_list_session_results_since(). */
static uint64_t session_floor;

/* 0 = uninitialized, 1 = initialization in flight, 2 = ready: the loser of the first-call race waits, since taking a
   mutex another thread has not finished initializing is undefined behavior. */
static void
lock_results(void) {
    int expected = 0;
    if (atomic_compare_exchange_strong(&mutex_state, &expected, 1)) {
        (void)dsd_mutex_init(&result_mutex);
        atomic_store(&mutex_state, 2);
    } else {
        while (atomic_load(&mutex_state) != 2) {
            dsd_thread_yield();
        }
    }
    dsd_mutex_lock(&result_mutex);
}

uint32_t
dsd_app_scan_list_request_next(void) {
    lock_results();
    last_request = last_request == UINT32_MAX ? 1U : last_request + 1U;
    const uint32_t request = last_request;
    dsd_mutex_unlock(&result_mutex);
    return request;
}

int
dsd_app_scan_list_result_get(dsd_app_scan_list_result* out) {
    if (!out) {
        return 0;
    }
    lock_results();
    *out = newest;
    dsd_mutex_unlock(&result_mutex);
    return out->sequence != 0U;
}

/* Copy up to @p cap retained results above @p after_sequence, oldest first. Caller holds the results' lock. */
static int
results_after_locked(uint64_t after_sequence, dsd_app_scan_list_result* out, int cap) {
    const uint64_t last = newest.sequence;
    /* A cursor at or past the newest has nothing after it; checked first, so one at UINT64_MAX cannot wrap to the
       start of the history. */
    if (after_sequence >= last) {
        return 0;
    }
    uint64_t seq = after_sequence + 1U;
    if (last >= (uint64_t)DSD_APP_RESULT_HISTORY && seq < last - (uint64_t)DSD_APP_RESULT_HISTORY + 1U) {
        seq = last - (uint64_t)DSD_APP_RESULT_HISTORY + 1U;
    }
    int n = 0;
    for (; seq != 0U && seq <= last && n < cap; seq++) {
        const dsd_app_scan_list_result* r = &history[seq % (uint64_t)DSD_APP_RESULT_HISTORY];
        if (r->sequence == seq) {
            out[n++] = *r;
        }
    }
    return n;
}

int
dsd_app_scan_list_results_since(uint64_t after_sequence, dsd_app_scan_list_result* out, int cap) {
    if (!out || cap <= 0) {
        return 0;
    }
    lock_results();
    const int n = results_after_locked(after_sequence, out, cap);
    dsd_mutex_unlock(&result_mutex);
    return n;
}

int
dsd_app_scan_list_session_results_since(uint64_t after_sequence, dsd_app_scan_list_result* out, int cap) {
    if (!out || cap <= 0) {
        return 0;
    }
    lock_results();
    const int n = results_after_locked(after_sequence > session_floor ? after_sequence : session_floor, out, cap);
    dsd_mutex_unlock(&result_mutex);
    return n;
}

void
dsd_app_scan_list_note_session_open(void) {
    lock_results();
    session_floor = newest.sequence;
    dsd_mutex_unlock(&result_mutex);
}

/* One field of the submitted payload, when the submission reached it. */
static void
payload_field(const struct dsd_app_command* command, size_t offset, void* out, size_t size) {
    if (command->n >= offset + size) {
        DSD_MEMCPY(out, command->data + offset, size);
    }
}

void
dsd_app_scan_list_note_lists_seq(uint32_t lists_seq) {
    lock_results();
    known_lists_seq = lists_seq;
    dsd_mutex_unlock(&result_mutex);
}

void
dsd_app_scan_list_publish_result(const struct dsd_app_command* command, int32_t code, const dsd_state* state) {
    if (!command || command->id != DSD_APP_CMD_SCAN_LIST_SET) {
        return;
    }
    dsd_app_scan_list_result result = {0};
    payload_field(command, offsetof(dsd_app_scan_list_set_payload, session), &result.session, sizeof result.session);
    payload_field(command, offsetof(dsd_app_scan_list_set_payload, request), &result.request, sizeof result.request);
    payload_field(command, offsetof(dsd_app_scan_list_set_payload, list), &result.list, sizeof result.list);
    payload_field(command, offsetof(dsd_app_scan_list_set_payload, enabled), &result.enabled, sizeof result.enabled);
    result.code = code;
    lock_results();
    if (state) {
        known_lists_seq = state->trunk_scan_lists_seq;
    }
    result.lists_seq = known_lists_seq;
    result.sequence = newest.sequence == UINT64_MAX ? 1U : newest.sequence + 1U;
    newest = result;
    history[result.sequence % (uint64_t)DSD_APP_RESULT_HISTORY] = result;
    dsd_mutex_unlock(&result_mutex);
}
