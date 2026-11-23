// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* DSD_APP_CMD_DMR_CHANNEL_SET (discussion #900 F3): the user's Tier III channel edits, checked against the row the
   editor read and applied by dmr_t3_overlay_apply(), and the results a frontend matches its requests by. */

#include <dsd-neo/app_control/commands.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/protocol/dmr/dmr_t3_channels.h>
#include <dsd-neo/runtime/log.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>

#include "command_dispatch.h"
#include "dmr_channel_commands.h"
#include "dmr_t3_view_internal.h"
#include "dsd-neo/core/opts_fwd.h"

/* The protocol's answers pass through unchanged (each side + 0: an int, so no enum types are compared). */
_Static_assert(DSD_APP_DMR_CHANNEL_SET_OK + 0 == DMR_T3_SET_OK + 0, "OK is the overlay's");
_Static_assert(DSD_APP_DMR_CHANNEL_SET_STALE + 0 == DMR_T3_SET_STALE + 0, "STALE is the overlay's");
_Static_assert(DSD_APP_DMR_CHANNEL_SET_TRUNK_SCAN + 0 == DMR_T3_SET_TRUNK_SCAN + 0, "TRUNK_SCAN is the overlay's");
_Static_assert(DSD_APP_DMR_CHANNEL_SET_INVALID + 0 == DMR_T3_SET_INVALID + 0, "INVALID is the overlay's");
_Static_assert(DSD_APP_DMR_CHANNEL_SET_FULL + 0 == DMR_T3_SET_FULL + 0, "FULL is the overlay's");

static dsd_mutex_t result_mutex;
static atomic_int mutex_state = 0;
static dsd_app_dmr_channel_set_result newest;
/* The last DSD_APP_RESULT_HISTORY results, at sequence % DSD_APP_RESULT_HISTORY. */
static dsd_app_dmr_channel_set_result history[DSD_APP_RESULT_HISTORY];
static uint32_t last_request;

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
dsd_app_dmr_channel_request_next(void) {
    lock_results();
    last_request = last_request == UINT32_MAX ? 1U : last_request + 1U;
    const uint32_t request = last_request;
    dsd_mutex_unlock(&result_mutex);
    return request;
}

int
dsd_app_dmr_channel_set_result_get(dsd_app_dmr_channel_set_result* out) {
    if (!out) {
        return 0;
    }
    lock_results();
    *out = newest;
    dsd_mutex_unlock(&result_mutex);
    return out->sequence != 0U;
}

int
dsd_app_dmr_channel_set_results_since(uint64_t after_sequence, dsd_app_dmr_channel_set_result* out, int cap) {
    if (!out || cap <= 0) {
        return 0;
    }
    lock_results();
    const uint64_t last = newest.sequence;
    int n = 0;
    /* A cursor at or past the newest has nothing after it; checked first, so one at UINT64_MAX cannot wrap. */
    if (after_sequence < last) {
        uint64_t seq = after_sequence + 1U;
        if (last >= (uint64_t)DSD_APP_RESULT_HISTORY && seq < last - (uint64_t)DSD_APP_RESULT_HISTORY + 1U) {
            seq = last - (uint64_t)DSD_APP_RESULT_HISTORY + 1U;
        }
        for (; seq != 0U && seq <= last && n < cap; seq++) {
            const dsd_app_dmr_channel_set_result* r = &history[seq % (uint64_t)DSD_APP_RESULT_HISTORY];
            if (r->sequence == seq) {
                out[n++] = *r;
            }
        }
    }
    dsd_mutex_unlock(&result_mutex);
    return n;
}

/* One field of the submitted payload, when the submission reached it. */
static void
payload_field(const struct dsd_app_command* command, size_t offset, void* out, size_t size) {
    if (command->n >= offset + size) {
        DSD_MEMCPY(out, command->data + offset, size);
    }
}

void
dsd_app_dmr_channel_publish_result(const struct dsd_app_command* command, int32_t code, const dsd_state* state) {
    if (!command || command->id != DSD_APP_CMD_DMR_CHANNEL_SET) {
        return;
    }
    dsd_app_dmr_channel_set_result result = {0};
    payload_field(command, offsetof(dsd_app_dmr_channel_set_payload, session_seq), &result.session_seq,
                  sizeof result.session_seq);
    payload_field(command, offsetof(dsd_app_dmr_channel_set_payload, request), &result.request, sizeof result.request);
    payload_field(command, offsetof(dsd_app_dmr_channel_set_payload, lpcn), &result.lpcn, sizeof result.lpcn);
    payload_field(command, offsetof(dsd_app_dmr_channel_set_payload, op), &result.op, sizeof result.op);
    result.code = code;
    result.revision = state ? dmr_t3_chan_revision(state) : 0U;
    lock_results();
    result.sequence = newest.sequence == UINT64_MAX ? 1U : newest.sequence + 1U;
    newest = result;
    history[result.sequence % (uint64_t)DSD_APP_RESULT_HISTORY] = result;
    dsd_mutex_unlock(&result_mutex);
}

static void
dmr_channel_set_log(const dsd_app_dmr_channel_set_payload* p) {
    switch (p->op) {
        case DMR_T3_OVERLAY_SET:
            LOG_INFO("NOTICE: DMR Tier III: channel %u set to %.6f MHz (yours)\n", (unsigned)p->lpcn,
                     (double)p->hz / 1000000.0);
            break;
        case DMR_T3_OVERLAY_HIDE: LOG_INFO("NOTICE: DMR Tier III: channel %u hidden\n", (unsigned)p->lpcn); break;
        default: LOG_INFO("NOTICE: DMR Tier III: channel %u restored\n", (unsigned)p->lpcn); break;
    }
}

int32_t
dsd_app_apply_dmr_channel_set(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* command) {
    if (!state || !command || command->n != sizeof(dsd_app_dmr_channel_set_payload)) {
        return DSD_APP_DMR_CHANNEL_SET_INVALID;
    }
    dsd_app_dmr_channel_set_payload p;
    DSD_MEMCPY(&p, command->data, sizeof p);
    /* The row the editor read holds a long; a value no long holds names no row. Only a SET carries a frequency. */
    const int set = p.op == (uint8_t)DMR_T3_OVERLAY_SET;
    if (p.expected_hz < (int64_t)LONG_MIN || p.expected_hz > (int64_t)LONG_MAX
        || (set && (p.hz < 0 || p.hz > (int64_t)DMR_T3_OVERLAY_MAX_HZ))) {
        return DSD_APP_DMR_CHANNEL_SET_INVALID;
    }
    const long hz = set ? (long)p.hz : 0;
    if (p.session_seq == 0U || p.session_seq != dsd_app_command_session_generation()) {
        return DSD_APP_DMR_CHANNEL_SET_STALE;
    }
    /* The session no longer decodes DMR (a live mode change to P25 only, say), or the carrier names another system (a
       Connect Plus or P25 site): no Tier III channel is in use, so no edit of one applies. Judged here, on the decoder
       thread, so an edit the editor sent before the mode change or the identity cannot slip through. */
    if (!dsd_app_dmr_t3_dmr_decoding(opts)) {
        return DSD_APP_DMR_CHANNEL_SET_NO_DMR;
    }
    if (dsd_app_dmr_t3_other_system_here(state)) {
        return DSD_APP_DMR_CHANNEL_SET_OTHER_SYSTEM;
    }
    const dmr_t3_set_result r = dmr_t3_overlay_apply(opts, state, p.lpcn, (dmr_t3_overlay_op)p.op, hz,
                                                     (long)p.expected_hz, p.expected_origin, p.generation, 1);
    if (r == DMR_T3_SET_OK) {
        dmr_channel_set_log(&p);
    }
    return (int32_t)r;
}
