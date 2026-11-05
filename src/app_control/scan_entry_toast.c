// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The status-line text for a frequency typed by hand that is a row of the scan in force (issue #644), in one place:
 * what the scan's go-to of that row came to, what became of an entry whose tune was still outstanding, `y` continuing
 * an entry hold, and why an entry on a row's frequency that the scan cannot take stayed a plain tune.
 *
 * A -Y row is named by its label, else "row N"; a trunk-scan target as every target toast names it, "#<n> <label>",
 * its id in place of a label it lacks (svc_trunk_scan_toast_name()). Every text fits dsd_state::ui_msg. The name is
 * never cut: a part that would not fit is dropped whole, the "(1 of 2)" count first, then a trunked target's
 * control-channel frequency.
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/engine/channel_scan.h>
#include <dsd-neo/engine/scan_entry.h>
#include <dsd-neo/engine/trunk_scan.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/freq_parse.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "services.h"

/* The entry whose tune is outstanding, in the state's own slot (DSD_STATE_EXT_APP_SCAN_ENTRY): the row a go-to that
   answered PENDING went to, named in the toast its landing leaves (svc_scan_entry_settle()) and in `y`'s while it
   waits. A failure is named by the scan's own record instead, which every entry has. Decoder thread only. */
typedef struct {
    int waiting;
    dsd_scan_entry_match_t match;
    long park_hz;
    char text[sizeof(((dsd_state*)0)->ui_msg)]; /* the toast it left, which only its own landing replaces */
} scan_entry_waiting;

/* The state's slot; made on first use when @p create, else NULL until then (and when it cannot be made). */
static scan_entry_waiting*
entry_waiting_slot(dsd_state* state, int create) {
    scan_entry_waiting* slot = DSD_STATE_EXT_GET_AS(scan_entry_waiting, state, DSD_STATE_EXT_APP_SCAN_ENTRY);
    if (slot || !create) {
        return slot;
    }
    slot = (scan_entry_waiting*)calloc(1, sizeof(*slot));
    if (slot && dsd_state_ext_set(state, DSD_STATE_EXT_APP_SCAN_ENTRY, slot, free) != 0) {
        free(slot);
        slot = NULL;
    }
    return slot;
}

const char*
svc_trunk_scan_toast_name(uint32_t ordinal, const char* label, const char* id, char* out, size_t out_size) {
    const char* shown = (label && label[0]) ? label : (id ? id : "");
    if (ordinal > 0U) {
        DSD_SNPRINTF(out, out_size, "#%u %s", (unsigned)ordinal, shown);
    } else {
        DSD_SNPRINTF(out, out_size, "%s", shown);
    }
    return out;
}

/* @p m's row as a toast names it: a target's place in the list and its label (or id), a -Y row's label. */
static const char*
entry_name(const dsd_scan_entry_match_t* m, char* out, size_t size) {
    if (m->scanner == DSD_SCAN_ENTRY_TRUNK) {
        return svc_trunk_scan_toast_name(m->row >= 0 ? (uint32_t)m->row + 1U : 0U, m->label, NULL, out, size);
    }
    DSD_SNPRINTF(out, size, "%s", m->label);
    return out;
}

static void
entry_toast_set(dsd_state* state, int seconds, const char* text) {
    DSD_SNPRINTF(state->ui_msg, sizeof state->ui_msg, "%s", text);
    state->ui_msg_expire = dsd_realtime_time() + seconds;
}

/* "<lead><name>[ (n of N)]<body>[<extra>]" into @p out: the name and the mandatory parts whole, and of the optional
   ones only what fits, the count dropped before the extra. */
static void
entry_compose(char* out, size_t size, const char* lead, const dsd_scan_entry_match_t* m, const char* body,
              const char* extra) {
    char name[DSD_TRUNK_SCAN_TOAST_NAME_SIZE];
    (void)entry_name(m, name, sizeof name);
    char count[32] = "";
    if (m->count > 1U) {
        DSD_SNPRINTF(count, sizeof count, " (%u of %u)", m->ordinal, m->count);
    }
    const char* counts[] = {count, "", count, ""};
    const char* extras[] = {extra, extra, "", ""};
    for (size_t i = 0; i < sizeof counts / sizeof counts[0]; i++) {
        const int n = DSD_SNPRINTF(out, size, "%s%s%s%s%s", lead, name, counts[i], body, extras[i]);
        if (n >= 0 && (size_t)n < size) {
            return;
        }
    }
}

/* A trunked target's control channel the hold parks or follows, for @p phrase ("..., trunked: following its control
   channel 851.012500 MHz"); "" for a conventional row, or with no frequency known. */
static const char*
entry_park_text(const dsd_scan_entry_match_t* m, long park_hz, const char* phrase, char* out, size_t size) {
    out[0] = '\0';
    if (!m->trunked || park_hz <= 0 || (unsigned long)park_hz > UINT32_MAX) {
        return out;
    }
    char mhz[16];
    (void)dsd_freq_hz_mhz_text((uint32_t)park_hz, mhz, sizeof mhz);
    DSD_SNPRINTF(out, size, "%s%s MHz", phrase, mhz);
    return out;
}

static void
entry_holding_text(char* out, size_t size, const dsd_scan_entry_match_t* m, long park_hz) {
    char park[64];
    entry_compose(out, size, "Holding: ", m, "",
                  entry_park_text(m, park_hz, ", trunked: following its control channel ", park, sizeof park));
}

static void
entry_waiting_text(char* out, size_t size, const dsd_scan_entry_match_t* m, long park_hz, int resuming) {
    char park[64];
    entry_compose(out, size, resuming ? "Resuming scan at " : "Going to ", m, ": waiting for the tune",
                  entry_park_text(m, park_hz, " to its control channel ", park, sizeof park));
}

/* "Failed: tune to fire-dispatch; scan stays paused", for a go-to that came to @p result. */
static void
entry_failure_text(char* out, size_t size, const dsd_scan_entry_match_t* m, dsd_scan_goto_result result, int paused) {
    const char* tail = paused ? "; scan stays paused" : "";
    dsd_scan_entry_match_t named = *m;
    named.count = 0U; /* a failure names the row alone */
    switch (result) {
        case DSD_SCAN_GOTO_DEFERRED: entry_compose(out, size, "Deferred; retry: tune to ", &named, tail, ""); break;
        case DSD_SCAN_GOTO_SETUP_FAILED:
            entry_compose(out, size, "Failed: ", &named, " could not be set up", tail);
            break;
        default: entry_compose(out, size, "Failed: tune to ", &named, tail, ""); break;
    }
}

/* Whether the scan @p scanner names is paused now. */
static int
entry_scan_paused(const dsd_state* state, dsd_scan_entry_scanner scanner) {
    return scanner == DSD_SCAN_ENTRY_TRUNK ? state->trunk_scan_paused != 0U : state->lcn_scan_paused != 0U;
}

/* Remember the entry @p match left waiting, with the toast @p text it left; or forget one (@p match NULL). */
static void
entry_remember(dsd_state* state, const dsd_scan_entry_match_t* match, long park_hz, const char* text) {
    scan_entry_waiting* slot = entry_waiting_slot(state, match != NULL);
    if (!slot) {
        return;
    }
    slot->waiting = match != NULL;
    if (match) {
        slot->match = *match;
        slot->park_hz = park_hz;
        DSD_SNPRINTF(slot->text, sizeof slot->text, "%s", text);
    }
}

void
svc_scan_entry_toast_outcome(dsd_state* state, const dsd_scan_entry_match_t* match, dsd_scan_goto_result result,
                             long park_hz, int engine_toasted) {
    if (!state || !match) {
        return;
    }
    entry_remember(state, NULL, 0L, NULL);
    char text[sizeof state->ui_msg];
    switch (result) {
        case DSD_SCAN_GOTO_HOLDING:
            /* What the go-to's own call put on the status line (an earlier resume's failure) stays. */
            if (engine_toasted) {
                return;
            }
            entry_holding_text(text, sizeof text, match, park_hz);
            break;
        case DSD_SCAN_GOTO_PENDING:
            entry_waiting_text(text, sizeof text, match, park_hz,
                               match->scanner == DSD_SCAN_ENTRY_CHANNEL && state->lcn_scan_resume_pending);
            entry_remember(state, match, park_hz, text);
            break;
        default:
            entry_failure_text(text, sizeof text, match, result, entry_scan_paused(state, match->scanner));
            entry_toast_set(state, 4, text);
            return;
    }
    entry_toast_set(state, 3, text);
}

/* Whether the entry @p slot remembers still waits on its tune. */
static int
entry_still_waiting(const dsd_state* state, const scan_entry_waiting* slot) {
    return slot->match.scanner == DSD_SCAN_ENTRY_TRUNK ? dsd_engine_trunk_scan_entry_pending(state)
                                                       : dsd_engine_channel_scan_entry_pending(state);
}

/* Whether the scan now holds the row @p slot's entry went to, marked an entry hold: its tune landed. */
static int
entry_landed(const dsd_state* state, const scan_entry_waiting* slot) {
    return slot->match.scanner == DSD_SCAN_ENTRY_TRUNK ? state->trunk_scan_entry_hold != 0U
                                                       : state->lcn_scan_entry_hold != 0U;
}

/* Name the late failure the scan kept, as the scan named what the entry went to. */
static void
entry_failure_toast(dsd_state* state, const dsd_scan_entry_failure_t* failed) {
    dsd_scan_entry_match_t named;
    DSD_MEMSET(&named, 0, sizeof named);
    named.scanner = failed->scanner;
    named.row = failed->row;
    DSD_SNPRINTF(named.label, sizeof named.label, "%s", failed->label);
    char text[sizeof state->ui_msg];
    entry_failure_text(text, sizeof text, &named, failed->result, entry_scan_paused(state, failed->scanner));
    entry_toast_set(state, 4, text);
}

void
svc_scan_entry_settle(dsd_state* state) {
    if (!state) {
        return;
    }
    dsd_scan_entry_failure_t failed;
    if (dsd_engine_scan_entry_take_failure(state, &failed)) {
        entry_failure_toast(state, &failed);
        entry_remember(state, NULL, 0L, NULL);
        return;
    }
    scan_entry_waiting* slot = entry_waiting_slot(state, 0);
    if (!slot || !slot->waiting || entry_still_waiting(state, slot)) {
        return;
    }
    slot->waiting = 0;
    /* Landed: the hold replaces the entry's own waiting text, and nothing newer (a failure, another command's). A
       cancel (`Y`, `L`, `J`, `b`, another entry, a pause, a stop) leaves what it said. */
    if (entry_landed(state, slot) && strcmp(state->ui_msg, slot->text) == 0) {
        char text[sizeof state->ui_msg];
        entry_holding_text(text, sizeof text, &slot->match, slot->park_hz);
        entry_toast_set(state, 3, text);
    }
}

void
svc_scan_entry_toast_continues(dsd_state* state) {
    if (state) {
        entry_toast_set(state, 3, "Scan continues");
    }
}

void
svc_scan_entry_toast_waiting(dsd_state* state) {
    if (!state) {
        return;
    }
    scan_entry_waiting* slot = entry_waiting_slot(state, 0);
    if (!slot || !slot->waiting) {
        entry_toast_set(state, 3, "Scan: waiting for the tune");
        return;
    }
    char text[sizeof state->ui_msg];
    entry_waiting_text(text, sizeof text, &slot->match, slot->park_hz, 0);
    DSD_SNPRINTF(slot->text, sizeof slot->text, "%s", text);
    entry_toast_set(state, 3, text);
}

/* Why an entry on @p match's row stayed a plain tune: with the row's name (@p name), else (no room) as "that row" or
   "that target" (@p trunk). */
static void
entry_reason_text(const dsd_scan_entry_match_t* match, const char* name, int trunk, char* out, size_t size) {
    const char* noun = trunk ? "that target" : "that row";
    const char* phrase = NULL; /* after the name, or after the noun */
    switch (match->reason) {
        case DSD_SCAN_ENTRY_AVOIDED: phrase = " is avoided"; break;
        case DSD_SCAN_ENTRY_LIST_OFF: phrase = name ? " is in a list that is off" : "'s list is off"; break;
        case DSD_SCAN_ENTRY_WIDTH: phrase = "'s width does not fit"; break;
        default: out[0] = '\0'; return;
    }
    DSD_SNPRINTF(out, size, "; %s%s", name ? name : noun, phrase);
}

void
svc_scan_entry_toast_reason(const dsd_opts* opts, dsd_state* state, const dsd_scan_entry_match_t* match) {
    if (!opts || !state || !match || match->scanner != DSD_SCAN_ENTRY_NONE) {
        return;
    }
    const int trunk = opts->trunk_scan_enabled == 1;
    dsd_scan_entry_match_t named = *match;
    named.scanner = trunk ? DSD_SCAN_ENTRY_TRUNK : DSD_SCAN_ENTRY_CHANNEL;
    char name[DSD_TRUNK_SCAN_TOAST_NAME_SIZE];
    (void)entry_name(&named, name, sizeof name);
    const size_t used = strnlen(state->ui_msg, sizeof state->ui_msg);
    for (int with_name = 1; with_name >= 0; with_name--) {
        char reason[sizeof state->ui_msg];
        entry_reason_text(match, with_name ? name : NULL, trunk, reason, sizeof reason);
        if (reason[0] == '\0') {
            return;
        }
        if (used + strlen(reason) < sizeof state->ui_msg) {
            DSD_SNPRINTF(state->ui_msg + used, sizeof state->ui_msg - used, "%s", reason);
            return;
        }
    }
}
