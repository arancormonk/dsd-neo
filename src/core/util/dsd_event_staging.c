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
    dsd_event_history_transaction_end(&transaction);
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
