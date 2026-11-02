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
 * no longer counts. An untyped -Y step's record that a newer tune displaces keeps its drop for the poll as well.
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/engine/manual_tune.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/protocol/p25/p25_sm_watchdog.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/freq_parse.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
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
/* An untyped -Y step's record a newer tune displaced, its drop still to report: its row (a resume's) has an owner
   waiting on it, which the newer record's settlement would never name (issue #671). An entry's drop settles nothing,
   so an entry displaced leaves none. One is enough: a newer step displaced comes from a newer scan session, which the
   older one's report would only find stale. */
static dsd_manual_tune_record g_displaced;
static int g_displaced_unreported;
/* The record the last poll that settled one took. */
static dsd_manual_tune_settled g_settled;
static atomic_int g_record_pending = MANUAL_TUNE_NONE_NOTED;
/* A RadioReference import's tune (dsd_engine_manual_tune_note_rr_import(), issue #803): the toast its late failure
   posts, and the import's own toast, which its landing or drop may replace ("" when only a cleared line may be). Kept
   for the import noted last; a record of another kind never reads them. */
static char g_rr_failure_text[sizeof(((dsd_state*)0)->ui_msg)];
static char g_rr_own_toast[sizeof(((dsd_state*)0)->ui_msg)];

/* How long a RadioReference import's tune outcome stays up: its failure as long as the import's partial apply, its
   landing as long as a tune's success. A drop stays as a tune's failure does (DSD_MANUAL_TUNE_FAILED_TOAST_S). */
enum { MANUAL_TUNE_RR_FAILED_TOAST_S = 6, MANUAL_TUNE_RR_LANDED_TOAST_S = 3 };

/* The last few manual-tune completions, each (ID count << 32) | (uint32_t)result, 0 for an empty slot. Not one slot:
   an older tune that finishes late (one in flight before a newer one queued) would overwrite the completion the record
   awaits, which then never comes again (issue #671). A completion can also arrive before its record is noted, so the
   slots are not filtered by the record either. Eight outlast any run of tunes between a note and its poll. */
enum { MANUAL_TUNE_COMPLETION_SLOTS = 8 };

static dsd_atomic_u64 g_completions[MANUAL_TUNE_COMPLETION_SLOTS];
static atomic_uint g_completion_next;

/* The completion for @p id in the slots, in @p packed: 1 when one came, 0 otherwise. */
static int
manual_tune_completion_for(uint64_t id, uint64_t* packed) {
    for (int i = 0; i < MANUAL_TUNE_COMPLETION_SLOTS; i++) {
        const uint64_t slot = dsd_atomic_u64_load_acquire(&g_completions[i]);
        if (slot != 0U && (uint32_t)(slot >> 32) == (uint32_t)id) {
            *packed = slot;
            return 1;
        }
    }
    return 0;
}

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
    const unsigned int slot = atomic_fetch_add(&g_completion_next, 1U) % MANUAL_TUNE_COMPLETION_SLOTS;
    dsd_atomic_u64_store_release(&g_completions[slot], packed);
}

void
dsd_engine_manual_tune_note_pending(dsd_state* state, dsd_manual_tune_kind kind, uint64_t id, uint32_t requested_hz,
                                    uint32_t prior_applied_hz, int row, uint32_t scan_session, uint64_t map_seq) {
    if (atomic_load(&g_record_pending) != MANUAL_TUNE_NONE_NOTED && g_record.kind == DSD_MANUAL_TUNE_UNTYPED_STEP) {
        g_displaced = g_record;
        g_displaced_unreported = 1;
    }
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
dsd_engine_manual_tune_note_rr_import(dsd_state* state, uint64_t id, uint32_t requested_hz, uint32_t prior_applied_hz,
                                      const char* failure_text, int own_toast_replaceable) {
    if (id == 0U) {
        /* Nothing was queued, so nothing decides the receiver: dsd_engine_manual_tune_note_pending() would still drop
           an older tune noted. */
        return;
    }
    DSD_SNPRINTF(g_rr_failure_text, sizeof g_rr_failure_text, "%s", failure_text ? failure_text : "");
    g_rr_own_toast[0] = '\0';
    if (own_toast_replaceable && state) {
        DSD_SNPRINTF(g_rr_own_toast, sizeof g_rr_own_toast, "%s", state->ui_msg);
    }
    dsd_engine_manual_tune_note_pending(state, DSD_MANUAL_TUNE_RR_IMPORT, id, requested_hz, prior_applied_hz, -1, 0U,
                                        0U);
}

void
dsd_engine_manual_tune_supersede(dsd_state* state) {
    (void)state; // the record is process-wide, as the RTL controller is
    if (atomic_load(&g_record_pending) == MANUAL_TUNE_NOTED) {
        atomic_store(&g_record_pending, MANUAL_TUNE_DROPPED_UNREPORTED);
    }
}

/* What @p record was stays for its owner (dsd_engine_manual_tune_last_settled()). */
static void
manual_tune_keep_settled(const dsd_manual_tune_record* record) {
    g_settled.kind = record->kind;
    g_settled.requested_hz = record->requested_hz;
    g_settled.row = record->row;
    g_settled.scan_session = record->scan_session;
    g_settled.map_seq = record->map_seq;
}

/* The record goes, and what it was stays for its owner. */
static void
manual_tune_settle(void) {
    atomic_store(&g_record_pending, MANUAL_TUNE_NONE_NOTED);
    manual_tune_keep_settled(&g_record);
}

/* Whether a RadioReference import's landing or drop may take the line: it shows nothing, or still the import's own
   toast. A newer message says where the receiver went, and a failure the import reported outranks its landing. */
static int
manual_tune_rr_line_free(const dsd_state* state) {
    if (state->ui_msg[0] == '\0' || state->ui_msg_expire <= dsd_realtime_time()) {
        return 1;
    }
    return g_rr_own_toast[0] != '\0' && strcmp(state->ui_msg, g_rr_own_toast) == 0;
}

/* The record settled as @p polled: a RadioReference import's tune says how (issue #803). @p stream_stopped says a drop
   came from the stream's session ending rather than from a newer tune. */
static void
manual_tune_rr_report(dsd_state* state, int polled, int stream_stopped) {
    if (!state || g_record.kind != DSD_MANUAL_TUNE_RR_IMPORT) {
        return;
    }
    char mhz[16];
    (void)dsd_freq_hz_mhz_text(g_record.requested_hz, mhz, sizeof mhz);
    const time_t now = dsd_realtime_time();
    if (polled == DSD_MANUAL_TUNE_FAILED) {
        if (g_rr_failure_text[0] != '\0') {
            DSD_SNPRINTF(state->ui_msg, sizeof state->ui_msg, "%s", g_rr_failure_text);
        } else {
            DSD_SNPRINTF(state->ui_msg, sizeof state->ui_msg, "Failed: RR import -> tune to %s MHz failed", mhz);
        }
        state->ui_msg_expire = now + MANUAL_TUNE_RR_FAILED_TOAST_S;
        return;
    }
    if (!manual_tune_rr_line_free(state)) {
        return;
    }
    if (polled == DSD_MANUAL_TUNE_LANDED) {
        DSD_SNPRINTF(state->ui_msg, sizeof state->ui_msg, "Applied: RR import -> tuned to %s MHz", mhz);
        state->ui_msg_expire = now + MANUAL_TUNE_RR_LANDED_TOAST_S;
        return;
    }
    DSD_SNPRINTF(state->ui_msg, sizeof state->ui_msg, "Interrupted: RR import -> tune to %s MHz; %s", mhz,
                 stream_stopped ? "the radio input stopped" : "a newer tune took over");
    state->ui_msg_expire = now + DSD_MANUAL_TUNE_FAILED_TOAST_S;
}

/* dsd_engine_manual_tune_poll() for a noted record, under the P25 SM tick guard: a watchdog retune (trunking on once
   the scan the tune was for has left) supersedes the record and writes the cached centre under it, so the record's
   state, its completion and the centre a failure puts back are read and written in one hold, never over a newer
   tune's. */
static int
manual_tune_poll_noted(dsd_opts* opts, dsd_state* state) {
    const int noted = atomic_load(&g_record_pending);
    if (noted == MANUAL_TUNE_NONE_NOTED) {
        return DSD_MANUAL_TUNE_NONE;
    }
    if (noted == MANUAL_TUNE_DROPPED_UNREPORTED) {
        manual_tune_settle();
        manual_tune_rr_report(state, DSD_MANUAL_TUNE_DROPPED, 0);
        return DSD_MANUAL_TUNE_DROPPED;
    }
    /* The completion before the session: a stop moves the session before it fails the request it queued, so a failure
       read here that a stop made is always seen with the stop's session, and never toasted as a failed tune. */
    uint64_t packed = 0U;
    const int completed = manual_tune_completion_for(g_record.id, &packed);
    if (g_record.session_generation != manual_tune_session_generation()) {
        /* The stream it was asked of stopped: a stop fails its queued request, and the next stream opens on the
           frequency the decoder holds. */
        manual_tune_settle();
        manual_tune_rr_report(state, DSD_MANUAL_TUNE_DROPPED, 1);
        return DSD_MANUAL_TUNE_DROPPED;
    }
    if (!completed) {
        return DSD_MANUAL_TUNE_NONE;
    }
    manual_tune_settle();
    if ((int32_t)(uint32_t)packed == RTL_STREAM_TUNE_OK) {
        manual_tune_rr_report(state, DSD_MANUAL_TUNE_LANDED, 0);
        return DSD_MANUAL_TUNE_LANDED;
    }
    if ((int32_t)(uint32_t)packed == RTL_STREAM_TUNE_SUPERSEDED) {
        /* Taken over before it ran: what replaced it decides the receiver, as for a tune superseded here. */
        manual_tune_rr_report(state, DSD_MANUAL_TUNE_DROPPED, 0);
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
    manual_tune_rr_report(state, DSD_MANUAL_TUNE_FAILED, 0);
    return DSD_MANUAL_TUNE_FAILED;
}

int
dsd_engine_manual_tune_poll(dsd_opts* opts, dsd_state* state) {
    if (g_displaced_unreported) {
        /* The displaced step first: the record that displaced it is polled next. */
        g_displaced_unreported = 0;
        manual_tune_keep_settled(&g_displaced);
        return DSD_MANUAL_TUNE_DROPPED;
    }
    if (atomic_load(&g_record_pending) == MANUAL_TUNE_NONE_NOTED) {
        return DSD_MANUAL_TUNE_NONE;
    }
    /* The guard is not re-entrant: a caller that holds it (a guarded command's pass) keeps its hold. */
    const int take_guard = !p25_sm_tick_guard_held_by_caller();
    if (take_guard) {
        p25_sm_tick_guard_enter();
    }
    const int polled = manual_tune_poll_noted(opts, state);
    if (take_guard) {
        p25_sm_tick_guard_leave();
    }
    return polled;
}

void
dsd_engine_manual_tune_last_settled(dsd_manual_tune_settled* out) {
    if (out) {
        *out = g_settled;
    }
}

int
dsd_engine_manual_tune_noted(void) {
    return atomic_load(&g_record_pending) == MANUAL_TUNE_NOTED;
}

int
dsd_engine_manual_tune_settled_ready(void) {
    if (g_displaced_unreported) {
        return 1;
    }
    if (atomic_load(&g_record_pending) != MANUAL_TUNE_NOTED) {
        return 0;
    }
    uint64_t packed = 0U;
    if (!manual_tune_completion_for(g_record.id, &packed)) {
        return 0;
    }
    /* A RadioReference import's landing is reported, so the reception it ends hands its toast to the outer pass. */
    return (int32_t)(uint32_t)packed != RTL_STREAM_TUNE_OK || g_record.kind == DSD_MANUAL_TUNE_UNTYPED_STEP
           || g_record.kind == DSD_MANUAL_TUNE_RR_IMPORT;
}
