// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/app_control/access_code_view.h>
#include <dsd-neo/app_control/commands.h>
#include <dsd-neo/app_control/rx_tone_view.h>
#include <dsd-neo/app_control/signal_search.h>
#include <dsd-neo/core/access_code.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/engine/signal_search.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/signal_search.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "../command_dispatch.h"

/* One tuner/decoder owns the observer. Copy values here; the worker never reads live state. */
static struct {
    const dsd_opts* opts;
    const dsd_state* state;
    dsd_signal_search_hit last;
} observer;

/* The access code of a confirmed hit, formatted here as the frontends show it. The call tuple (IDs, texts, slot,
   crypt) is the coordinator's, from one checked call record; this reads only the service options of the call the
   tuple names (its slot), under the same record checks. */
static void
format_confirmed_access(dsd_signal_search_hit* hit) {
    dsd_call_snapshot call;
    uint16_t service = 0;
    uint8_t metadata = 0;
    const uint8_t slot = hit->slot > 0 ? (uint8_t)(hit->slot - 1) : 0U;
    if (dsd_call_state_get(observer.state, slot, &call) > 0 && call.phase == DSD_CALL_PHASE_ACTIVE
        && call.protocol == observer.state->synctype && !call.crc_invalid) {
        service = call.service_options;
        metadata = call.has_service_metadata;
    }
    uint8_t kind = 0;
    uint16_t value = 0;
    if (dsd_access_code_current(observer.state, observer.state->synctype, service, metadata, &kind, &value)) {
        (void)dsd_app_access_code_format(kind, value, DSD_APP_ACCESS_CODE_SHORT, hit->access, sizeof(hit->access));
    }
}

static void
publish_store(const dsd_signal_search_status* status, const dsd_signal_search_hit* hit) {
    dsd_app_signal_search_publish(status, hit, NULL);
    if (status->phase == DSD_SEARCH_STOPPED) {
        dsd_app_signal_search_store_flush();
    }
}

static void
publish_search(const dsd_signal_search_status* status, const dsd_signal_search_hit* changed, void* unused) {
    (void)unused;
    dsd_signal_search_status copy = *status;
    dsd_signal_search_hit* hit = &copy.hit;
    if (!hit->hit_id) {
        publish_store(&copy, NULL);
        return;
    }
    if (hit->session_id == observer.last.session_id && hit->hit_id == observer.last.hit_id
        && strcmp(hit->protocol, observer.last.protocol) == 0) {
        DSD_MEMCPY(hit->access, observer.last.access, sizeof(hit->access));
        DSD_MEMCPY(hit->tone, observer.last.tone, sizeof(hit->tone));
    }
    /* A periodic refresh can follow an unconfirmed sync or a damaged frame. Read
       decoded fields only in the synchronous, protocol-confirmed publication. */
    if (changed && !hit->ended && hit->confirmed) {
        format_confirmed_access(hit);
    }
    if (!hit->ended) {
        dsd_app_rx_tone tone;
        if (dsd_app_rx_tone_view(observer.opts, observer.state, dsd_realtime_mono_s(), &tone) > 0
            && tone.status == DSD_APP_RX_TONE_LOCKED) {
            DSD_SNPRINTF(hit->tone, sizeof(hit->tone), "%s", tone.text);
        }
    }
    observer.last = *hit;
    /* The store coalesces identical records, including periodic live-detail refreshes. */
    publish_store(&copy, changed || !hit->ended ? hit : NULL);
}

int
dsd_app_signal_search_start(const dsd_signal_search_config* config) {
    return config ? dsd_app_command_submit(DSD_APP_CMD_SIGNAL_SEARCH_START, config, sizeof(*config))
                  : DSD_APP_COMMAND_SUBMIT_REJECTED;
}

int
dsd_app_signal_search_control(const dsd_signal_search_control* control) {
    return control ? dsd_app_command_submit(DSD_APP_CMD_SIGNAL_SEARCH_CONTROL, control, sizeof(*control))
                   : DSD_APP_COMMAND_SUBMIT_REJECTED;
}

static int
start_search(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* cmd) {
    if (cmd->n != sizeof(dsd_signal_search_config)) {
        return 2;
    }
    dsd_signal_search_config config;
    DSD_MEMCPY(&config, cmd->data, sizeof(config));
    observer.opts = opts;
    observer.state = state;
    char error[192] = "";
    if (dsd_engine_signal_search_start(opts, state, &config, publish_search, NULL, error, sizeof(error))) {
        dsd_signal_search_status status = {0};
        dsd_app_signal_search_status_get(&status, NULL, 0);
        const uint64_t revision = status.revision + 1;
        if (!dsd_engine_signal_search_active(state)) {
            /* No coordinator owns the tuner: the error is for display, and session teardown retires it. */
            DSD_MEMSET(&status, 0, sizeof(status));
            status.session_id = config.session_id;
            status.phase = DSD_SEARCH_ERROR;
            status.owned = 0;
        }
        /* A rejected restart of a paused session leaves that coordinator, its phase and its ownership as they were. */
        status.revision = revision;
        DSD_SNPRINTF(status.message, sizeof(status.message), "%s", error);
        dsd_app_signal_search_publish(&status, NULL, NULL);
        return 2;
    }
    return 1;
}

/* A permanent avoid is saved first, and only then is the session's own avoid made, so a full permanent list changes
   nothing at all. A history row (hit_id 0) names its frequency, and the session avoids it by frequency. */
static int
avoid_permanently(dsd_opts* opts, dsd_state* state, const dsd_signal_search_control* control) {
    dsd_signal_search_control session = *control;
    if (!session.hit_id) {
        session.action = DSD_SEARCH_ACTION_AVOID_FREQUENCY;
    }
    uint32_t frequency = 0;
    if (dsd_engine_signal_search_target(state, &session, &frequency)) {
        return 2;
    }
    const int saved = dsd_app_signal_search_permanent_avoid(frequency);
    if (saved != DSD_APP_SEARCH_AVOID_ADDED && saved != DSD_APP_SEARCH_AVOID_DUPLICATE) {
        dsd_engine_signal_search_notice(state, control->session_id,
                                        saved == DSD_APP_SEARCH_AVOID_FULL ? "The permanent avoid list is full"
                                                                           : "This frequency cannot be avoided");
        return 2;
    }
    if (dsd_engine_signal_search_control(opts, state, &session)) {
        /* Validated above on this thread, so only a full session list refuses. */
        dsd_engine_signal_search_notice(state, control->session_id,
                                        "Saved as a permanent avoid; this session's avoid list is full");
        return 1;
    }
    dsd_app_signal_search_session_avoid_add(control->session_id, frequency);
    return 1;
}

static int
control_search(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* cmd) {
    if (cmd->n != sizeof(dsd_signal_search_control)) {
        return 2;
    }
    dsd_signal_search_control control;
    DSD_MEMCPY(&control, cmd->data, sizeof(control));
    if (control.action == DSD_SEARCH_ACTION_AVOID_PERMANENT) {
        return avoid_permanently(opts, state, &control);
    }
    /* The frequency an avoid mirrors is the one the coordinator validates now, never an older status copy's hit. */
    uint32_t frequency = 0;
    if (dsd_engine_signal_search_target(state, &control, &frequency)
        || dsd_engine_signal_search_control(opts, state, &control)) {
        return 2;
    }
    switch (control.action) {
        case DSD_SEARCH_ACTION_AVOID:
        case DSD_SEARCH_ACTION_AVOID_FREQUENCY:
            dsd_app_signal_search_session_avoid_add(control.session_id, frequency);
            break;
        case DSD_SEARCH_ACTION_UNAVOID: dsd_app_signal_search_session_avoid_remove(frequency); break;
        default: break;
    }
    return 1;
}
const struct dsd_app_command_reg dsd_app_actions_signal_search[] = {
    {DSD_APP_CMD_SIGNAL_SEARCH_START, start_search},
    {DSD_APP_CMD_SIGNAL_SEARCH_CONTROL, control_search},
    {0, NULL},
};
