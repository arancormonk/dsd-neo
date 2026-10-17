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
static int reject_start, coordinator_active;
static uint32_t session_avoid_added, session_avoid_removed, permanent_avoided;
static uint64_t session_avoid_session;
/* The coordinator as the control handler sees it: the hit it validates, and what it does with each action. */
static uint32_t live_hit_frequency = 463552100;
static int target_stale, control_refused, permanent_result, controls, permanent_calls;
static dsd_signal_search_control last_control;
static char notice[192];

int
dsd_engine_signal_search_start(dsd_opts* opts, dsd_state* state, const dsd_signal_search_config* config,
                               dsd_signal_search_publish_fn callback, void* user, char* error, size_t capacity) {
    (void)opts;
    (void)state;
    (void)config;
    if (reject_start) {
        DSD_SNPRINTF(error, capacity, "%s", "A search supports up to 100,000 enabled channels");
        return -1;
    }
    observer = callback;
    observer_user = user;
    return 0;
}

int
dsd_engine_signal_search_active(const dsd_state* state) {
    (void)state;
    return coordinator_active;
}

int
dsd_engine_signal_search_control(dsd_opts* opts, dsd_state* state, const dsd_signal_search_control* control) {
    (void)opts;
    (void)state;
    ++controls;
    last_control = *control;
    return control_refused ? -1 : 0;
}

int
dsd_engine_signal_search_target(const dsd_state* state, const dsd_signal_search_control* action, uint32_t* frequency) {
    (void)state;
    if (target_stale) {
        return -1;
    }
    const int by_frequency =
        action->action == DSD_SEARCH_ACTION_UNAVOID || action->action == DSD_SEARCH_ACTION_AVOID_FREQUENCY;
    *frequency = by_frequency ? action->frequency_hz : live_hit_frequency;
    return 0;
}

void
dsd_engine_signal_search_notice(dsd_state* state, uint64_t session_id, const char* message) {
    (void)state;
    (void)session_id;
    DSD_SNPRINTF(notice, sizeof(notice), "%s", message);
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

int
dsd_app_signal_search_permanent_avoid(uint32_t frequency) {
    ++permanent_calls;
    if (permanent_result == DSD_APP_SEARCH_AVOID_ADDED) {
        permanent_avoided = frequency;
    }
    return permanent_result;
}

void
dsd_app_signal_search_session_avoid_add(uint64_t session, uint32_t frequency) {
    session_avoid_session = session;
    session_avoid_added = frequency;
}

void
dsd_app_signal_search_session_avoid_remove(uint32_t frequency) {
    session_avoid_removed = frequency;
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

    // A protocol confirmation permits the current access code. The call tuple is the coordinator's: the observer reads
    // no call text of its own, even from an active, CRC-clean record.
    status.hit.confirmed = 1;
    DSD_SNPRINTF(status.hit.protocol, sizeof(status.hit.protocol), "%s", "DMR");
    observer(&status, &status.hit, observer_user);
    assert(strcmp(published.hit.access, "CC 7") == 0);
    assert(published.hit.source_text[0] == '\0' && published.hit.crypt[0] == '\0' && published.hit.slot == 0);
    // The coordinator's tuple -- IDs, texts, slot and crypt from one checked record -- reaches the published status as
    // it gave it.
    status.hit.target = 100;
    status.hit.source = 200;
    status.hit.slot = 1;
    DSD_SNPRINTF(status.hit.source_text, sizeof(status.hit.source_text), "%s", "valid talker");
    DSD_SNPRINTF(status.hit.crypt, sizeof(status.hit.crypt), "%s", "ALG 21 \xC2\xB7 KID 0005");
    observer(&status, &status.hit, observer_user);
    assert(strcmp(published.hit.access, "CC 7") == 0);
    assert(published.hit.target == 100 && published.hit.source == 200 && published.hit.slot == 1);
    assert(strcmp(published.hit.source_text, "valid talker") == 0);
    assert(strcmp(published.hit.crypt, "ALG 21 \xC2\xB7 KID 0005") == 0);

    // A later false sync and even a malformed call snapshot cannot enrich a periodic refresh.
    state->synctype = DSD_SYNC_P25P1_POS;
    state->nac = 0x123;
    call.protocol = state->synctype;
    DSD_SNPRINTF(call.source_text, sizeof(call.source_text), "%s", "unverified talker");
    observer(&status, NULL, observer_user);
    assert(strcmp(published.hit.access, "CC 7") == 0);
    assert(strcmp(published.hit.source_text, "valid talker") == 0);
    assert(strcmp(published.hit.crypt, "ALG 21 \xC2\xB7 KID 0005") == 0 && published.hit.slot == 1);

    // A later call on the same hit replaces the tuple whole: nothing of the call before it is carried forward.
    state->synctype = DSD_SYNC_DMR_BS_VOICE_POS;
    call.protocol = state->synctype;
    status.hit.target = 500;
    status.hit.source = 600;
    status.hit.source_text[0] = '\0';
    DSD_SNPRINTF(status.hit.crypt, sizeof(status.hit.crypt), "%s", "Encrypted");
    observer(&status, &status.hit, observer_user);
    assert(published.hit.target == 500 && published.hit.source == 600 && published.hit.source_text[0] == '\0');
    assert(strcmp(published.hit.crypt, "Encrypted") == 0 && strcmp(published.hit.access, "CC 7") == 0);
    observer(&status, NULL, observer_user);
    assert(published.hit.source_text[0] == '\0' && strcmp(published.hit.crypt, "Encrypted") == 0);
    state->synctype = DSD_SYNC_DMR_BS_VOICE_POS;
    state->dmr_color_code = 3;
    observer(&status, NULL, observer_user);
    assert(strcmp(published.hit.access, "CC 7") == 0);

    // A newly confirmed protocol on the same RF frequency cannot inherit old identities: the coordinator cleared the
    // tuple, and the observer restores none of it.
    status.hit.target = status.hit.source = 0;
    status.hit.slot = 0;
    status.hit.crypt[0] = '\0';
    DSD_SNPRINTF(status.hit.protocol, sizeof(status.hit.protocol), "%s", "NXDN48");
    state->synctype = DSD_SYNC_NXDN_POS;
    state->nxdn_last_ran = 3;
    call.phase = DSD_CALL_PHASE_IDLE;
    observer(&status, &status.hit, observer_user);
    assert(strcmp(published.hit.access, "RAN 3") == 0 && published.hit.source_text[0] == '\0');
    assert(published.hit.target == 0 && published.hit.slot == 0 && published.hit.crypt[0] == '\0');

    // The coordinator's checked site reaches the published status as it gave it, on a hit change and on a periodic
    // refresh alike; the observer neither invents nor carries one.
    DSD_SNPRINTF(status.hit.site, sizeof(status.hit.site), "%s", "Site 12 \xC2\xB7 Sys 3");
    observer(&status, &status.hit, observer_user);
    assert(strcmp(published.hit.site, "Site 12 \xC2\xB7 Sys 3") == 0);
    observer(&status, NULL, observer_user);
    assert(strcmp(published.hit.site, "Site 12 \xC2\xB7 Sys 3") == 0);

    // A final broadcast followed by silence: the ended hit keeps the site it last carried.
    status.hit.ended = 1;
    observer(&status, &status.hit, observer_user);
    assert(strcmp(published.hit.access, "RAN 3") == 0);
    assert(strcmp(published.hit.site, "Site 12 \xC2\xB7 Sys 3") == 0);
    status.hit.site[0] = '\0';
    status.hit.hit_id = 2;
    status.hit.ended = status.hit.confirmed = 0;
    status.hit.protocol[0] = '\0';
    observer(&status, &status.hit, observer_user);
    assert(published.hit.access[0] == '\0' && published.hit.source_text[0] == '\0');

    // Controls mirror the session's own avoid list for the frontends; a permanent avoid goes to the store as well.
    struct dsd_app_command* control = calloc(1, sizeof(*control));
    assert(control);
    control->n = sizeof(dsd_signal_search_control);
    dsd_signal_search_control action = {1, 2, DSD_SEARCH_ACTION_AVOID, 0};
    // The published copy can be a tick old; the mirror takes the hit the coordinator validates now.
    published.hit.frequency_hz = 451000000;
    DSD_MEMCPY(control->data, &action, sizeof(action));
    assert(dsd_app_actions_signal_search[1].fn(opts, state, control) == 1);
    assert(session_avoid_session == 1 && session_avoid_added == 463552100 && !permanent_avoided);
    action.action = DSD_SEARCH_ACTION_UNAVOID;
    action.frequency_hz = 463552100;
    DSD_MEMCPY(control->data, &action, sizeof(action));
    assert(dsd_app_actions_signal_search[1].fn(opts, state, control) == 1);
    assert(session_avoid_removed == 463552100);
    action.action = DSD_SEARCH_ACTION_AVOID_PERMANENT;
    action.frequency_hz = 0;
    session_avoid_added = 0;
    DSD_MEMCPY(control->data, &action, sizeof(action));
    assert(dsd_app_actions_signal_search[1].fn(opts, state, control) == 1);
    assert(permanent_avoided == 463552100 && session_avoid_added == 463552100);
    assert(last_control.action == DSD_SEARCH_ACTION_AVOID_PERMANENT && last_control.hit_id == 2);

    // A history row avoids by frequency, for this session only, and is mirrored once the coordinator takes it.
    action = (dsd_signal_search_control){1, 0, DSD_SEARCH_ACTION_AVOID_FREQUENCY, 462575000};
    DSD_MEMCPY(control->data, &action, sizeof(action));
    assert(dsd_app_actions_signal_search[1].fn(opts, state, control) == 1);
    assert(session_avoid_added == 462575000 && last_control.frequency_hz == 462575000);
    control_refused = 1; // the session's list is full: nothing is mirrored
    action.frequency_hz = 462600000;
    DSD_MEMCPY(control->data, &action, sizeof(action));
    assert(dsd_app_actions_signal_search[1].fn(opts, state, control) == 2);
    assert(session_avoid_added == 462575000);
    control_refused = 0;

    // A history row's permanent avoid: saved first, then avoided this session by frequency.
    permanent_avoided = 0;
    action = (dsd_signal_search_control){1, 0, DSD_SEARCH_ACTION_AVOID_PERMANENT, 462625000};
    DSD_MEMCPY(control->data, &action, sizeof(action));
    assert(dsd_app_actions_signal_search[1].fn(opts, state, control) == 1);
    assert(permanent_avoided == 462625000 && session_avoid_added == 462625000);
    assert(last_control.action == DSD_SEARCH_ACTION_AVOID_FREQUENCY && last_control.frequency_hz == 462625000);

    // Stale: refused before either avoid store or the coordinator is touched, from both entry points.
    for (int history = 0; history < 2; ++history) {
        target_stale = 1;
        const int before_controls = controls, before_permanent = permanent_calls;
        session_avoid_added = 0;
        action =
            (dsd_signal_search_control){1, history ? 0 : 2, DSD_SEARCH_ACTION_AVOID_PERMANENT, history ? 462650000 : 0};
        DSD_MEMCPY(control->data, &action, sizeof(action));
        assert(dsd_app_actions_signal_search[1].fn(opts, state, control) == 2);
        action.action = history ? DSD_SEARCH_ACTION_AVOID_FREQUENCY : DSD_SEARCH_ACTION_AVOID;
        DSD_MEMCPY(control->data, &action, sizeof(action));
        assert(dsd_app_actions_signal_search[1].fn(opts, state, control) == 2);
        assert(controls == before_controls && permanent_calls == before_permanent && !session_avoid_added);
        target_stale = 0;
    }

    // A full permanent list refuses visibly and changes nothing else: no skip, no session avoid.
    for (int history = 0; history < 2; ++history) {
        permanent_result = DSD_APP_SEARCH_AVOID_FULL;
        notice[0] = '\0';
        const int before_controls = controls;
        session_avoid_added = 0;
        action =
            (dsd_signal_search_control){1, history ? 0 : 2, DSD_SEARCH_ACTION_AVOID_PERMANENT, history ? 462675000 : 0};
        DSD_MEMCPY(control->data, &action, sizeof(action));
        assert(dsd_app_actions_signal_search[1].fn(opts, state, control) == 2);
        assert(strcmp(notice, "The permanent avoid list is full") == 0);
        assert(controls == before_controls && !session_avoid_added);
        // Already a permanent avoid: no refusal, and this session avoids it too.
        permanent_result = DSD_APP_SEARCH_AVOID_DUPLICATE;
        assert(dsd_app_actions_signal_search[1].fn(opts, state, control) == 1);
        assert(session_avoid_added == (history ? 462675000U : 463552100U));
        // Saved, but the session's own list is full: said so, and nothing is mirrored.
        permanent_result = DSD_APP_SEARCH_AVOID_ADDED;
        control_refused = 1;
        session_avoid_added = 0;
        notice[0] = '\0';
        assert(dsd_app_actions_signal_search[1].fn(opts, state, control) == 1);
        assert(strcmp(notice, "Saved as a permanent avoid; this session's avoid list is full") == 0);
        assert(!session_avoid_added);
        control_refused = 0;
    }
    free(control);

    // A start the engine rejects is shown, but owns nothing: no coordinator will ever publish STOPPED for it.
    reject_start = 1;
    published.revision = 9;
    assert(dsd_app_actions_signal_search[0].fn(opts, state, command) == 2);
    assert(published.phase == DSD_SEARCH_ERROR && !published.owned && published.revision == 10);
    assert(strstr(published.message, "100,000") != NULL);

    // A rejected restart of a paused session keeps that coordinator's phase, session and ownership.
    published.session_id = 5;
    published.phase = DSD_SEARCH_PAUSED;
    published.owned = 1;
    published.message[0] = '\0';
    coordinator_active = 1;
    assert(dsd_app_actions_signal_search[0].fn(opts, state, command) == 2);
    assert(published.phase == DSD_SEARCH_PAUSED && published.owned && published.session_id == 5);
    assert(published.revision == 11 && strstr(published.message, "100,000") != NULL);

    free(command);
    free(state);
    free(opts);
    return 0;
}
