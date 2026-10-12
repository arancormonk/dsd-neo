// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <dsd-neo/app_control/commands.h>
#include <dsd-neo/app_control/rx_tone_view.h>
#include <dsd-neo/app_control/signal_search.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/engine/signal_search.h>
#include <dsd-neo/runtime/signal_search.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "command_dispatch.h"

static dsd_signal_search_publish_fn observer;
static void* observer_user;
static dsd_signal_search_status published;
static dsd_call_snapshot call;

int
dsd_engine_signal_search_start(dsd_opts* opts, dsd_state* state, const dsd_signal_search_config* config,
                               dsd_signal_search_publish_fn callback, void* user, char* error, size_t capacity) {
    (void)opts;
    (void)state;
    (void)config;
    (void)error;
    (void)capacity;
    observer = callback;
    observer_user = user;
    return 0;
}

int
dsd_engine_signal_search_control(dsd_opts* opts, dsd_state* state, const dsd_signal_search_control* control) {
    (void)opts;
    (void)state;
    (void)control;
    return 0;
}

int
dsd_app_command_submit(int id, const void* data, size_t size) {
    (void)id;
    (void)data;
    (void)size;
    return 0;
}

void
dsd_app_signal_search_publish(const dsd_signal_search_status* status, const dsd_signal_search_hit* hit, void* user) {
    (void)hit;
    (void)user;
    published = *status;
}

void
dsd_app_signal_search_status_get(dsd_signal_search_status* status, char* error, size_t capacity) {
    (void)error;
    (void)capacity;
    *status = published;
}

void
dsd_app_signal_search_store_flush(void) {}

void
dsd_app_signal_search_permanent_avoid(uint32_t frequency) {
    (void)frequency;
}

int
dsd_call_state_get(const dsd_state* state, uint8_t slot, dsd_call_snapshot* out) {
    (void)state;
    if (slot) {
        return 0;
    }
    *out = call;
    return 1;
}

int
dsd_app_rx_tone_view(const dsd_opts* opts, const dsd_state* state, double now, dsd_app_rx_tone* out) {
    (void)opts;
    (void)state;
    (void)now;
    (void)out;
    return 0;
}

int
main(void) {
    dsd_opts* opts = calloc(1, sizeof(*opts));
    dsd_state* state = calloc(1, sizeof(*state));
    struct dsd_app_command* command = calloc(1, sizeof(*command));
    assert(opts && state && command);
    command->n = sizeof(dsd_signal_search_config);
    assert(dsd_app_actions_signal_search[0].fn(opts, state, command) == 1);
    assert(observer);

    dsd_signal_search_status status = {0};
    status.session_id = status.hit.session_id = 1;
    status.hit.hit_id = 1;
    status.phase = DSD_SEARCH_LISTENING;
    state->synctype = DSD_SYNC_DMR_BS_VOICE_POS;
    state->dmr_color_code = 7;
    call.protocol = state->synctype;
    call.phase = DSD_CALL_PHASE_ACTIVE;
    DSD_SNPRINTF(call.source_text, sizeof(call.source_text), "%s", "valid talker");
    observer(&status, &status.hit, observer_user);
    assert(published.hit.access[0] == '\0' && published.hit.source_text[0] == '\0');

    // A protocol confirmation permits the current decoded details.
    status.hit.confirmed = 1;
    DSD_SNPRINTF(status.hit.protocol, sizeof(status.hit.protocol), "%s", "DMR");
    observer(&status, &status.hit, observer_user);
    assert(strcmp(published.hit.access, "CC 7") == 0);
    assert(strcmp(published.hit.source_text, "valid talker") == 0);

    // A later false sync and even a malformed call snapshot cannot enrich a periodic refresh.
    state->synctype = DSD_SYNC_P25P1_POS;
    state->nac = 0x123;
    call.protocol = state->synctype;
    DSD_SNPRINTF(call.source_text, sizeof(call.source_text), "%s", "unverified talker");
    observer(&status, NULL, observer_user);
    assert(strcmp(published.hit.access, "CC 7") == 0);
    assert(strcmp(published.hit.source_text, "valid talker") == 0);
    state->synctype = DSD_SYNC_DMR_BS_VOICE_POS;
    state->dmr_color_code = 3;
    observer(&status, NULL, observer_user);
    assert(strcmp(published.hit.access, "CC 7") == 0);

    // A newly confirmed protocol on the same RF frequency cannot inherit old identities.
    DSD_SNPRINTF(status.hit.protocol, sizeof(status.hit.protocol), "%s", "NXDN48");
    state->synctype = DSD_SYNC_NXDN_POS;
    state->nxdn_last_ran = 3;
    call.phase = DSD_CALL_PHASE_IDLE;
    observer(&status, &status.hit, observer_user);
    assert(strcmp(published.hit.access, "RAN 3") == 0 && published.hit.source_text[0] == '\0');

    status.hit.ended = 1;
    observer(&status, &status.hit, observer_user);
    assert(strcmp(published.hit.access, "RAN 3") == 0);
    status.hit.hit_id = 2;
    status.hit.ended = status.hit.confirmed = 0;
    status.hit.protocol[0] = '\0';
    observer(&status, &status.hit, observer_user);
    assert(published.hit.access[0] == '\0' && published.hit.source_text[0] == '\0');

    free(command);
    free(state);
    free(opts);
    return 0;
}
