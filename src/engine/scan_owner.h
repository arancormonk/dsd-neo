// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/** Private ownership below the stable ENGINE_TRUNK_SCAN extension ID. */
#ifndef DSD_NEO_ENGINE_SCAN_OWNER_H
#define DSD_NEO_ENGINE_SCAN_OWNER_H
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/state_fwd.h>
#include <stdint.h>
#include <stdlib.h>

/** The row or target a scan last confirmed it landed on, and its frequency (issue #667 D9, #644; scan_landing.h). */
typedef struct {
    uint32_t hz;       /**< 0 for none, and for a trunked target, whose park is its control channel */
    uint8_t by;        /**< the scan that landed it (DSD_ENGINE_SCAN_LANDING_*); NONE for no landing */
    uint32_t session;  /**< -Y: dsd_state::lcn_scan_session; trunk scan: the coordinator's session */
    uint64_t map_seq;  /**< -Y: dsd_state::trunk_chan_map_seq at the landing */
    uint64_t list_rev; /**< -Y: dsd_state::lcn_list_rev at the landing */
    int index;         /**< the -Y row or trunk-scan target landed */
    /** The scan owner's landing epoch this landing took (dsd_engine_scan_owner::landing_epoch, issue #834): moved by
        every note and every forget, so a later landing on the same row or target is another visit. 0 for none. */
    uint64_t epoch;
    /** The trunk-tuning request whose tune landed (0 for a -Y row's synchronous tune). */
    uint64_t tune_request;
    /** The RTL stream output generation read when that tune was requested, before it was issued (0 with no stream):
        only a capture of a newer generation can be the landed one. */
    uint32_t generation_before_tune;
    /** dsd_realtime_mono_ms() when the landing was noted: where the visit's first quiet check's wait for its capture
        is bounded from (one that starts again later in the visit waits from its restart). */
    uint64_t landed_realtime_ms;
    /** The owner's sync sequence and the decode-mono time when the landing was noted: a quiet check's activity baseline
        for the visit, so a sync or carrier heard on the landed channel before the check is first published counts. */
    uint64_t sync_seq;
    double landed_decode_m;
} dsd_engine_scan_landing;

/** The -Y row or trunk-scan target a frequency typed by hand went to, whose confirmed landing holds the scan there
 * (issue #644; scan_landing.h). */
typedef struct {
    uint8_t active;    /**< 0: no entry waits on a landing */
    uint8_t by;        /**< the scan it waits in (DSD_ENGINE_SCAN_LANDING_ROW or _TARGET) */
    int row;           /**< the -Y row or trunk-scan target gone to */
    uint32_t session;  /**< -Y: dsd_state::lcn_scan_session of its tune; trunk scan: the coordinator's session */
    uint64_t list_rev; /**< -Y: dsd_state::lcn_list_rev of its tune */
    uint64_t request;  /**< its tune: a typed row's first request, kept through its restages; an untyped row's noted
                            manual tune; a target's park, or the P25 recovery tune it adopted; 0 until the tune is
                            begun */
} dsd_engine_scan_entry_intent;

/** An entry whose own tune failed after its go-to returned (issue #644; scan_landing.h): kept until app-control takes
 * it, once, to name the row in a toast. A cancel (`Y`, `L`, `J`, `b`, another entry, a pause, a stop) records none. */
typedef struct {
    uint8_t set;    /**< 0: no failure waits to be taken */
    uint8_t by;     /**< the scan it failed in (DSD_ENGINE_SCAN_LANDING_ROW or _TARGET) */
    uint8_t result; /**< the dsd_scan_goto_result it came to: REFUSED, DEFERRED or SETUP_FAILED */
    int row;        /**< the -Y row or trunk-scan target gone to */
    char label[64]; /**< its name when it failed: a -Y row's label, else "row N"; a target's label, else its id */
} dsd_engine_scan_entry_failure;

typedef struct {
    void* trunk;
    void* search;
    void* analog_call;
    /* The scan quiet check (issue #834, scan_quiet.c), made on first use. */
    void* quiet;
    dsd_state_ext_cleanup_fn trunk_free, search_free, analog_call_free, quiet_free;
    dsd_engine_scan_landing landing;
    dsd_engine_scan_entry_intent entry;
    dsd_engine_scan_entry_failure entry_failed;
    /* Issue #834. landing_epoch: moved by every landing note and forget (scan_landing.h). tune_generation_before: the
       RTL output generation at the last scan tune request, carried to the landing it notes. sync_seq: moved, only while
       skip-silent is on, whenever the frame sync returns a sync and whenever the analog carrier opens, so a quiet check
       sees activity since its visit's landing. quiet_inv_seq: moved where a quiet check's facts change and no counter
       of their own says so (a hold, a pause, a resume, a tune in flight, a stop, the setting, a config apply, a Signal
       search start). */
    uint64_t landing_epoch;
    uint32_t tune_generation_before;
    uint64_t sync_seq;
    uint64_t quiet_inv_seq;
} dsd_engine_scan_owner;

static inline void
dsd_engine_scan_owner_free(void* ptr) {
    dsd_engine_scan_owner* p = (dsd_engine_scan_owner*)ptr;
    if (p->trunk_free) {
        p->trunk_free(p->trunk);
    }
    if (p->search_free) {
        p->search_free(p->search);
    }
    if (p->analog_call_free) {
        p->analog_call_free(p->analog_call);
    }
    if (p->quiet_free) {
        p->quiet_free(p->quiet);
    }
    free(p);
}

static inline void*
dsd_engine_scan_owned(const dsd_state* state, int search) {
    const dsd_engine_scan_owner* p =
        (const dsd_engine_scan_owner*)dsd_state_ext_get_const(state, DSD_STATE_EXT_ENGINE_TRUNK_SCAN);
    return p ? (search == 2 ? p->analog_call : (search ? p->search : p->trunk)) : NULL;
}

/* The owner, made on first use; NULL when it cannot be. */
static inline dsd_engine_scan_owner*
dsd_engine_scan_owner_ensure(dsd_state* state) {
    dsd_engine_scan_owner* p = DSD_STATE_EXT_GET_AS(dsd_engine_scan_owner, state, DSD_STATE_EXT_ENGINE_TRUNK_SCAN);
    if (!p) {
        p = (dsd_engine_scan_owner*)calloc(1, sizeof(*p));
        if (!p) {
            return NULL;
        }
        if (dsd_state_ext_set(state, DSD_STATE_EXT_ENGINE_TRUNK_SCAN, p, dsd_engine_scan_owner_free)) {
            free(p);
            return NULL;
        }
    }
    return p;
}

/* The owner, or NULL when none was made; makes none. */
static inline dsd_engine_scan_owner*
dsd_engine_scan_owner_get(dsd_state* state) {
    return state && dsd_state_ext_get_const(state, DSD_STATE_EXT_ENGINE_TRUNK_SCAN)
               ? dsd_engine_scan_owner_ensure(state)
               : NULL;
}

/** A sync was returned or the analog carrier opened (issue #834), and is noted only while skip-silent is on: a quiet
 * check sees activity since its visit's landing. A state with no scan owner has no check, and gets no owner made for
 * it. */
static inline void
dsd_engine_scan_quiet_note_sync(dsd_state* state) {
    dsd_engine_scan_owner* p = dsd_engine_scan_owner_get(state);
    if (p) {
        p->sync_seq++;
    }
}

/** Something a quiet check depends on changed with no counter of its own (issue #834): the check made is dropped at
 * its next reconcile, and its request cancelled there. A state with no scan owner has no check. */
static inline void
dsd_engine_scan_quiet_note_change(dsd_state* state) {
    dsd_engine_scan_owner* p = dsd_engine_scan_owner_get(state);
    if (p) {
        p->quiet_inv_seq++;
    }
}

static inline int
dsd_engine_scan_own(dsd_state* state, int search, void* value, dsd_state_ext_cleanup_fn cleanup) {
    dsd_engine_scan_owner* p = DSD_STATE_EXT_GET_AS(dsd_engine_scan_owner, state, DSD_STATE_EXT_ENGINE_TRUNK_SCAN);
    if (!p) {
        if (!value) {
            return 0;
        }
        p = dsd_engine_scan_owner_ensure(state);
        if (!p) {
            return -1;
        }
    }
    if (search == 2) {
        if (p->analog_call_free) {
            p->analog_call_free(p->analog_call);
        }
        p->analog_call = value;
        p->analog_call_free = cleanup;
    } else if (search) {
        if (p->search_free) {
            p->search_free(p->search);
        }
        p->search = value;
        p->search_free = cleanup;
    } else {
        if (p->trunk_free) {
            p->trunk_free(p->trunk);
        }
        p->trunk = value;
        p->trunk_free = cleanup;
    }
    return 0;
}
#endif
