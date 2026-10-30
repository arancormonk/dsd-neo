// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * A manual tune whose RTL wait timed out, followed to its completion (issue #671, <dsd-neo/engine/manual_tune.h>).
 *
 * The record is process-wide, as the one RTL controller and its one completion callback are, and the decoder thread
 * owns it. The completion arrives on the controller thread into a single atomic slot that packs the ID's count (its
 * low half, which tells manual-tune IDs apart) with the result, so the decoder reads both in one load. One slot is
 * enough: a completion of another manual tune than the noted one comes only from a newer tune, which supersedes the
 * record anyway. A record superseded, or whose stream session ended, is dropped, and the next poll reports the drop
 * once, so the owner of what the tune was for (a -Y resume's row) settles it rather than waiting on a completion that
 * no longer counts.
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/engine/manual_tune.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/freq_parse.h>
#include <stdint.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

#include "manual_tune_pending.h"

typedef struct {
    uint64_t id;
    dsd_manual_tune_kind kind;
    uint32_t requested_hz;
    uint32_t prior_applied_hz;
    int row;
    uint32_t scan_session;
    uint64_t map_seq;
    uint32_t session_generation;
} dsd_manual_tune_record;

/* Where the record stands: nothing noted, a tune noted and its completion awaited, or the noted tune dropped (another
   tune decides where the receiver goes) with the drop still to report to its owner. */
enum { MANUAL_TUNE_NONE_NOTED = 0, MANUAL_TUNE_NOTED = 1, MANUAL_TUNE_DROPPED_UNREPORTED = 2 };

static dsd_manual_tune_record g_record;
/* The record the last poll that settled one took. */
static dsd_manual_tune_settled g_settled;
static atomic_int g_record_pending = MANUAL_TUNE_NONE_NOTED;
/* (ID count << 32) | (uint32_t)result of the last manual-tune completion; 0 when none came. */
static dsd_atomic_u64 g_completion = {0U};

static uint32_t
manual_tune_session_generation(void) {
#ifdef USE_RADIO
    return rtl_stream_session_generation();
#else
    return 0U;
#endif
}

/* The centre the receiver is on after a failure: the one the stream last applied, else the one noted before the
   tune. */
static uint32_t
manual_tune_applied_after_failure(void) {
#ifdef USE_RADIO
    uint32_t applied_hz = 0U;
    if (rtl_stream_get_last_applied_freq(&applied_hz) == 0 && applied_hz != 0U) {
        return applied_hz;
    }
#endif
    return g_record.prior_applied_hz;
}

void
dsd_engine_manual_tune_completion(uint64_t id, int result) {
    const uint64_t packed = ((uint64_t)(uint32_t)id << 32) | (uint64_t)(uint32_t)result;
    dsd_atomic_u64_store_release(&g_completion, packed);
}

void
dsd_engine_manual_tune_note_pending(dsd_state* state, dsd_manual_tune_kind kind, uint64_t id, uint32_t requested_hz,
                                    uint32_t prior_applied_hz, int row, uint32_t scan_session, uint64_t map_seq) {
    dsd_engine_manual_tune_supersede(state);
    if (id == 0U) {
        return;
    }
    g_record.id = id;
    g_record.kind = kind;
    g_record.requested_hz = requested_hz;
    g_record.prior_applied_hz = prior_applied_hz;
    g_record.row = row;
    g_record.scan_session = scan_session;
    g_record.map_seq = map_seq;
    g_record.session_generation = manual_tune_session_generation();
    atomic_store(&g_record_pending, MANUAL_TUNE_NOTED);
}

void
dsd_engine_manual_tune_supersede(dsd_state* state) {
    (void)state; // the record is process-wide, as the RTL controller is
    if (atomic_load(&g_record_pending) == MANUAL_TUNE_NOTED) {
        atomic_store(&g_record_pending, MANUAL_TUNE_DROPPED_UNREPORTED);
    }
}

/* The record goes, and what it was stays for its owner (dsd_engine_manual_tune_last_settled()). */
static void
manual_tune_settle(void) {
    atomic_store(&g_record_pending, MANUAL_TUNE_NONE_NOTED);
    g_settled.kind = g_record.kind;
    g_settled.requested_hz = g_record.requested_hz;
    g_settled.row = g_record.row;
    g_settled.scan_session = g_record.scan_session;
    g_settled.map_seq = g_record.map_seq;
}

int
dsd_engine_manual_tune_poll(dsd_opts* opts, dsd_state* state) {
    const int noted = atomic_load(&g_record_pending);
    if (noted == MANUAL_TUNE_NONE_NOTED) {
        return DSD_MANUAL_TUNE_NONE;
    }
    if (noted == MANUAL_TUNE_DROPPED_UNREPORTED) {
        manual_tune_settle();
        return DSD_MANUAL_TUNE_DROPPED;
    }
    /* The completion before the session: a stop moves the session before it fails the request it queued, so a failure
       read here that a stop made is always seen with the stop's session, and never toasted as a failed tune. */
    const uint64_t packed = dsd_atomic_u64_load_acquire(&g_completion);
    if (g_record.session_generation != manual_tune_session_generation()) {
        /* The stream it was asked of stopped: a stop fails its queued request, and the next stream opens on the
           frequency the decoder holds. */
        manual_tune_settle();
        return DSD_MANUAL_TUNE_DROPPED;
    }
    if ((uint32_t)(packed >> 32) != (uint32_t)g_record.id) {
        return DSD_MANUAL_TUNE_NONE;
    }
    manual_tune_settle();
    if ((int32_t)(uint32_t)packed == RTL_STREAM_TUNE_OK) {
        return DSD_MANUAL_TUNE_LANDED;
    }
    if ((int32_t)(uint32_t)packed == RTL_STREAM_TUNE_SUPERSEDED) {
        /* Taken over before it ran: what replaced it decides the receiver, as for a tune superseded here. */
        return DSD_MANUAL_TUNE_DROPPED;
    }
    if (opts) {
        const uint32_t applied_hz = manual_tune_applied_after_failure();
        if (applied_hz != 0U) {
            opts->rtlsdr_center_freq = applied_hz;
        }
    }
    /* A scan step that failed retries its row, as one that failed in its wait does, unannounced. */
    if (state && g_record.kind == DSD_MANUAL_TUNE_ENTRY) {
        char mhz[16];
        (void)dsd_freq_hz_mhz_text(g_record.requested_hz, mhz, sizeof mhz);
        DSD_SNPRINTF(state->ui_msg, sizeof state->ui_msg, "Failed: frequency -> %s MHz", mhz);
        state->ui_msg_expire = dsd_realtime_time() + DSD_MANUAL_TUNE_FAILED_TOAST_S;
    }
    return DSD_MANUAL_TUNE_FAILED;
}

void
dsd_engine_manual_tune_last_settled(dsd_manual_tune_settled* out) {
    if (out) {
        *out = g_settled;
    }
}

int
dsd_engine_manual_tune_settled_ready(void) {
    if (atomic_load(&g_record_pending) != MANUAL_TUNE_NOTED) {
        return 0;
    }
    const uint64_t packed = dsd_atomic_u64_load_acquire(&g_completion);
    if ((uint32_t)(packed >> 32) != (uint32_t)g_record.id) {
        return 0;
    }
    return (int32_t)(uint32_t)packed != RTL_STREAM_TUNE_OK || g_record.kind == DSD_MANUAL_TUNE_UNTYPED_STEP;
}
