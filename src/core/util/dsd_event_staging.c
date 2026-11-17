// SPDX-License-Identifier: ISC
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/string_utils.h>
#include <stddef.h>
#include <stdint.h>
#include "dsd_event_staging.h"

static int
staging_available(const dsd_state* state, uint8_t slot) {
    return state != NULL && state->event_history_s != NULL && slot < DSD_CALL_STATE_SLOT_COUNT;
}

void
dsd_event_stage_text(dsd_state* state, uint8_t slot, const char* text) {
    if (!staging_available(state, slot)) {
        return;
    }
    dsd_event_history_transaction transaction;
    dsd_event_history_transaction_begin(state, &transaction);
    Event_History_Staged* staged = &state->event_history_s[slot].staged;
    DSD_SNPRINTF(staged->text_message, sizeof staged->text_message, "%s", text != NULL ? text : "");
    dsd_event_history_transaction_end(&transaction);
}

void
dsd_event_stage_text_append(dsd_state* state, uint8_t slot, const char* text) {
    if (!staging_available(state, slot)) {
        return;
    }
    dsd_event_history_transaction transaction;
    dsd_event_history_transaction_begin(state, &transaction);
    Event_History_Staged* staged = &state->event_history_s[slot].staged;
    dsd_strncat_s(staged->text_message, sizeof staged->text_message, text, sizeof staged->text_message - 1U);
    dsd_event_history_transaction_end(&transaction);
}

void
dsd_event_stage_gps(dsd_state* state, uint8_t slot, const char* gps) {
    if (!staging_available(state, slot)) {
        return;
    }
    dsd_event_history_transaction transaction;
    dsd_event_history_transaction_begin(state, &transaction);
    Event_History_Staged* staged = &state->event_history_s[slot].staged;
    DSD_SNPRINTF(staged->gps_s, sizeof staged->gps_s, "%s", gps != NULL ? gps : "");
    /* Text without a position: a position staged before it no longer describes the staged text. */
    DSD_MEMSET(&staged->pos_fix, 0, sizeof staged->pos_fix);
    dsd_event_history_transaction_end(&transaction);
}

int
dsd_event_stage_position(dsd_state* state, uint8_t slot, const dsd_position_fix* fix, const char* gps_text) {
    if (!staging_available(state, slot) || !dsd_event_pos_fix_admitted(fix) || gps_text == NULL) {
        return -1;
    }
    dsd_event_history_transaction transaction;
    dsd_event_history_transaction_begin(state, &transaction);
    Event_History_Staged* staged = &state->event_history_s[slot].staged;
    DSD_SNPRINTF(staged->gps_s, sizeof staged->gps_s, "%s", gps_text);
    staged->pos_fix = *fix;
    dsd_event_history_transaction_end(&transaction);
    return 1;
}

const dsd_position_fix*
dsd_event_staged_position(const dsd_state* state, uint8_t slot) {
    static const dsd_position_fix none; /* zero: rev 0 */
    return staging_available(state, slot) ? &state->event_history_s[slot].staged.pos_fix : &none;
}

int
dsd_event_pos_fix_admitted(const dsd_position_fix* fix) {
    return fix != NULL && fix->rev != 0U && fix->proc_instance != 0U;
}

void
dsd_event_pos_set_from_fix(dsd_event_pos* pos, const dsd_position_fix* fix) {
    DSD_MEMSET(pos, 0, sizeof(*pos));
    pos->proc_instance = fix->proc_instance;
    pos->rev = fix->rev;
    pos->rx_time_s = fix->rx_time_s;
    pos->identity_freq_hz = fix->identity_freq_hz;
    pos->lat_e7 = fix->lat_e7;
    pos->lon_e7 = fix->lon_e7;
    pos->err_m = fix->err_m;
    pos->flags = fix->flags;
    pos->valid = 1U;
    pos->rx_clock = fix->rx_clock;
    pos->kind = fix->kind;
    pos->scope.kind = fix->sender_scope.kind;
    pos->scope.a = fix->sender_scope.a;
    pos->scope.b = fix->sender_scope.b;
}

void
dsd_event_pos_drop_position(dsd_event_pos* pos) {
    const dsd_position_scope scope = pos->scope;
    const int64_t identity_freq_hz = pos->identity_freq_hz;
    DSD_MEMSET(pos, 0, sizeof(*pos));
    pos->scope = scope;
    pos->identity_freq_hz = identity_freq_hz;
}

const char*
dsd_event_staged_text(const dsd_state* state, uint8_t slot) {
    return staging_available(state, slot) ? state->event_history_s[slot].staged.text_message : "";
}

const char*
dsd_event_staged_gps(const dsd_state* state, uint8_t slot) {
    return staging_available(state, slot) ? state->event_history_s[slot].staged.gps_s : "";
}

void
dsd_event_staging_clear_locked(Event_History_I* history) {
    DSD_MEMSET(&history->staged, 0, sizeof history->staged);
}

void
dsd_event_stage_clear(dsd_state* state, uint8_t slot) {
    if (!staging_available(state, slot)) {
        return;
    }
    dsd_event_history_transaction transaction;
    dsd_event_history_transaction_begin(state, &transaction);
    dsd_event_staging_clear_locked(&state->event_history_s[slot]);
    dsd_event_history_transaction_end(&transaction);
}
