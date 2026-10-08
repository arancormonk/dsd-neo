// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Frontend → decoder app-command queue (bounded). */

#include <dsd-neo/app_control/analog_width_view.h>
#include <dsd-neo/app_control/call_view.h>
#include <dsd-neo/app_control/commands.h>
#include <dsd-neo/app_control/history.h>
#include <dsd-neo/app_control/rr_import_apply.h>
#include <dsd-neo/app_control/rtl_gain_view.h>
#include <dsd-neo/app_control/rx_tone_view.h>
#include <dsd-neo/app_control/scan_row_view.h>
#include <dsd-neo/app_control/squelch_view.h>
#include <dsd-neo/core/airspy_config.h>
#include <dsd-neo/core/analog_tone.h>
#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/audio_input_switch.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/channel_mode.h>
#include <dsd-neo/core/constants.h>
#include <dsd-neo/core/csv_validate.h>
#include <dsd-neo/core/enc_lockout.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/frontend_types.h>
#include <dsd-neo/core/key_set.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/scan_profile.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/core/time_format.h>
#include <dsd-neo/crypto/dmr_keystream.h>
#include <dsd-neo/dsp/analog_rx.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/dsp/symbol.h>
#include <dsd-neo/engine/channel_scan.h>
#include <dsd-neo/engine/engine.h>
#include <dsd-neo/engine/frame_processing.h>
#include <dsd-neo/engine/scan_voice_gate.h>
#include <dsd-neo/engine/trunk_scan.h>
#include <dsd-neo/engine/trunk_tuning.h>
#include <dsd-neo/io/control.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/platform.h>
#include <dsd-neo/platform/posix_compat.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/protocol/dmr/dmr.h>
#include <dsd-neo/protocol/dmr/dmr_trunk_sm.h>
#include <dsd-neo/protocol/p25/p25_cc_candidates.h>
#include <dsd-neo/protocol/p25/p25_sm_watchdog.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/runtime/airspy_config.h>
#include <dsd-neo/runtime/analog_channel.h>
#include <dsd-neo/runtime/analog_tones.h>
#include <dsd-neo/runtime/call_alert.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/decode_mode.h>
#include <dsd-neo/runtime/exitflag.h>
#include <dsd-neo/runtime/freq_parse.h>
#include <dsd-neo/runtime/input_failure.h>
#include <dsd-neo/runtime/log.h>
#include <dsd-neo/runtime/rigctl_passband.h>
#include <dsd-neo/runtime/scan_mode.h>
#include <dsd-neo/runtime/scan_options.h>
#include <dsd-neo/runtime/squelch.h>
#include <dsd-neo/runtime/telemetry.h>
#include <dsd-neo/runtime/trunk_cc_candidates.h>
#include <dsd-neo/runtime/trunk_scan_hooks.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <errno.h>
#include <limits.h>
#include <sndfile.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "command_dispatch.h"
#include "commands_internal.h"
#include "key_commands.h"
#include "scan_row_edit.h"
#include "services.h"

#define DSD_APP_CMD_Q_CAP 128

_Static_assert(sizeof(dsdneoUserConfig) <= sizeof(((struct dsd_app_command*)0)->data),
               "dsd_app_command payload too small for dsdneoUserConfig");
_Static_assert(sizeof(dsd_app_rr_apply_payload) <= sizeof(((struct dsd_app_command*)0)->data),
               "dsd_app_command payload too small for dsd_app_rr_apply_payload");

enum {
    UI_CMD_APPLY_UNHANDLED = 0,
    UI_CMD_APPLY_COMPLETED = 1,
    UI_CMD_APPLY_FAILED = 2,
    UI_CMD_APPLY_UNSUPPORTED = 3,
    UI_CMD_APPLY_INVALID_PAYLOAD = 4,
    UI_CMD_APPLY_RESTART_REQUIRED = 5,
};

static struct dsd_app_command g_q[DSD_APP_CMD_Q_CAP];
static size_t g_head = 0; // pop index
static int g_session_open = 0;
static uint64_t g_session_generation = 0;
static size_t g_tail = 0; // push index
static dsd_mutex_t g_mu;
static atomic_int g_mu_init = 0;
static atomic_int g_overflow = 0;
static atomic_int g_overflow_warn_gate = 0;
#ifdef DSD_NEO_TEST_HOOKS
/* Relaxed on purpose: observing this count must not publish the drain's writes
 * (a synchronizing counter would hide an unguarded store swap from TSan). */
static dsd_atomic_u64 g_test_policy_guard_waits;
/* The result of the last command the drain applied (UI_CMD_APPLY_*). Decoder thread only, as the drain is. */
static int g_test_last_result;
#endif
/* WP-D1: the last export completion is independent of transient decoder toasts.
 * Protected by g_mu; retained without a reader first having to arm publication. */
static dsd_app_tg_export_result g_tg_export_result;
/* The last DSD_APP_RESULT_HISTORY results, at sequence % DSD_APP_RESULT_HISTORY, for dsd_app_tg_export_results_since(). */
static dsd_app_tg_export_result g_tg_export_history[DSD_APP_RESULT_HISTORY];
_Static_assert(sizeof(g_tg_export_result.path) == sizeof(((dsd_opts*)0)->group_in_file),
               "retained export path must fit every accepted group-file path");

/* 0 = uninitialized, 1 = initialization in flight, 2 = ready. The loser of the
 * first-call race must wait: taking a mutex another thread has not finished
 * initializing is undefined behavior. */
static void
ensure_mu_init(void) {
    if (atomic_load(&g_mu_init) == 2) {
        return;
    }
    int expected = 0;
    if (atomic_compare_exchange_strong(&g_mu_init, &expected, 1)) {
        (void)dsd_mutex_init(&g_mu);
        atomic_store(&g_mu_init, 2);
        return;
    }
    while (atomic_load(&g_mu_init) != 2) {
        dsd_thread_yield();
    }
}

uint64_t
dsd_app_command_session_generation(void) {
    ensure_mu_init();
    dsd_mutex_lock(&g_mu);
    const uint64_t generation = g_session_open ? g_session_generation : 0;
    dsd_mutex_unlock(&g_mu);
    return generation;
}

int
dsd_app_tg_export_result_get(dsd_app_tg_export_result* out) {
    if (!out) {
        return 0;
    }
    ensure_mu_init();
    dsd_mutex_lock(&g_mu);
    DSD_MEMCPY(out, &g_tg_export_result, sizeof(*out));
    dsd_mutex_unlock(&g_mu);
    return out->sequence != 0;
}

int
dsd_app_tg_export_results_since(uint64_t after_sequence, dsd_app_tg_export_result* out, int cap) {
    if (!out || cap <= 0) {
        return 0;
    }
    ensure_mu_init();
    dsd_mutex_lock(&g_mu);
    const uint64_t newest = g_tg_export_result.sequence;
    uint64_t seq = after_sequence + 1U;
    if (newest >= (uint64_t)DSD_APP_RESULT_HISTORY && seq < newest - (uint64_t)DSD_APP_RESULT_HISTORY + 1U) {
        seq = newest - (uint64_t)DSD_APP_RESULT_HISTORY + 1U;
    }
    int n = 0;
    for (; seq != 0U && seq <= newest && n < cap; seq++) {
        const dsd_app_tg_export_result* r = &g_tg_export_history[seq % (uint64_t)DSD_APP_RESULT_HISTORY];
        if (r->sequence == seq) {
            DSD_MEMCPY(&out[n++], r, sizeof(*r));
        }
    }
    dsd_mutex_unlock(&g_mu);
    return n;
}

/* Called after dispatch, including envelope rejection, before erasing the queued
 * request. Publish only export fields; other command payloads may carry secrets. */
static void
tg_export_publish_result_unlocked(const struct dsd_app_command* cmd, int status) {
    if (cmd->id != DSD_APP_CMD_TG_LIST_EXPORT) {
        return;
    }
    dsd_app_tg_export_result result = {0};
    const size_t context_offset = offsetof(dsd_app_tg_export_payload, policy_context);
    const size_t generation_offset = offsetof(dsd_app_tg_export_payload, policy_generation);
    const size_t path_offset = offsetof(dsd_app_tg_export_payload, path);
    if (cmd->n >= context_offset + sizeof result.policy_context) {
        DSD_MEMCPY(&result.policy_context, cmd->data + context_offset, sizeof result.policy_context);
    }
    if (cmd->n >= generation_offset + sizeof result.policy_generation) {
        DSD_MEMCPY(&result.policy_generation, cmd->data + generation_offset, sizeof result.policy_generation);
    }
    if (cmd->n > path_offset) {
        const char* path = (const char*)cmd->data + path_offset;
        const char* end = memchr(path, 0, cmd->n - path_offset);
        if (end && (size_t)(end - path) < sizeof result.path) {
            DSD_MEMCPY(result.path, path, (size_t)(end - path));
        }
    }
    result.success = status == UI_CMD_APPLY_COMPLETED;
    result.sequence = g_tg_export_result.sequence + 1U;
    // cppcheck-suppress knownConditionTrueFalse -- unsigned sequence wraps to zero at UINT64_MAX.
    if (result.sequence == 0) {
        result.sequence = 1;
    }
    g_tg_export_result = result;
    g_tg_export_history[result.sequence % (uint64_t)DSD_APP_RESULT_HISTORY] = result;
}

static void
tg_export_publish_result(const struct dsd_app_command* cmd, int status) {
    ensure_mu_init();
    dsd_mutex_lock(&g_mu);
    tg_export_publish_result_unlocked(cmd, status);
    dsd_mutex_unlock(&g_mu);
}

void
dsd_app_command_session_set_open(int open) {
    ensure_mu_init();
    dsd_mutex_lock(&g_mu);
    /* Lifecycle edges run on the decoder owner, serialized with command drain.
     * Admission and disposal share the producer lock: a racing late submission
     * is either erased here or rejected after the session closes. */
    while (g_head != g_tail) {
        tg_export_publish_result_unlocked(&g_q[g_head], UI_CMD_APPLY_FAILED);
        dsd_app_publish_decryption_result(&g_q[g_head], DSD_APP_KEY_CANCELLED);
        DSD_SECURE_ZERO(&g_q[g_head], sizeof g_q[g_head]);
        g_head = (g_head + 1) % DSD_APP_CMD_Q_CAP;
    }
    if (open) {
        g_session_generation = g_session_generation == UINT64_MAX ? 1 : g_session_generation + 1;
    }
    g_session_open = open != 0;
    atomic_store(&g_overflow_warn_gate, 0);
    dsd_mutex_unlock(&g_mu);
}

// Dispatch commands via per-domain registries
static int
ui_cmd_dispatch(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    const struct dsd_app_command_reg* regs[] = {dsd_app_actions_audio, dsd_app_actions_radio, dsd_app_actions_trunk,
                                                dsd_app_actions_logging, NULL};
    for (int i = 0; regs[i] != NULL; ++i) {
        for (const struct dsd_app_command_reg* r = regs[i]; r && r->fn != NULL; ++r) {
            if (r->id == c->id) {
                return r->fn(opts, state, c);
            }
        }
    }
    return 0; // not handled
}

static inline int
q_is_full_unlocked(void) {
    return ((g_tail + 1) % DSD_APP_CMD_Q_CAP) == g_head;
}

static inline int
q_is_empty_unlocked(void) {
    return g_head == g_tail;
}

static const int k_ui_cmd_string_ids[] = {
    DSD_APP_CMD_EVENT_LOG_SET,       DSD_APP_CMD_WAV_STATIC_OPEN,      DSD_APP_CMD_WAV_RAW_OPEN,
    DSD_APP_CMD_DSP_OUT_SET,         DSD_APP_CMD_SYMCAP_OPEN,          DSD_APP_CMD_SYMBOL_IN_OPEN,
    DSD_APP_CMD_INPUT_WAV_SET,       DSD_APP_CMD_INPUT_SYM_STREAM_SET, DSD_APP_CMD_PULSE_OUT_SET,
    DSD_APP_CMD_PULSE_IN_SET,        DSD_APP_CMD_LRRP_SET_CUSTOM,      DSD_APP_CMD_IMPORT_CHANNEL_MAP,
    DSD_APP_CMD_IMPORT_SRC_LIST,     DSD_APP_CMD_IMPORT_GROUP_LIST,    DSD_APP_CMD_IMPORT_KEYS_DEC,
    DSD_APP_CMD_IMPORT_KEYS_HEX,     DSD_APP_CMD_KEY_TYT_AP_SET,       DSD_APP_CMD_KEY_RETEVIS_RC2_SET,
    DSD_APP_CMD_KEY_TYT_EP_SET,      DSD_APP_CMD_KEY_KEN_SCR_SET,      DSD_APP_CMD_KEY_ANYTONE_BP_SET,
    DSD_APP_CMD_KEY_XOR_SET,         DSD_APP_CMD_M17_USER_DATA_SET,    DSD_APP_CMD_IMPORT_P25_BANDPLAN,
    DSD_APP_CMD_EXPORT_P25_BANDPLAN,
};

/* Setters where only the newest value matters, so a queued one may be overwritten
   in place rather than walked through. A list rather than a switch for the same
   reason k_ui_cmd_string_ids above is one: each entry costs a branch in a switch,
   and this set only grows.

   The last four are discrete choices rather than swept values, and are here on a
   narrower argument: a segmented control taken twice in a second should land on
   the second answer without the decoder rebuilding timing for the first one on
   the way. */
// Direct keys are deliberately absent: BASIC and RC4 update independent state,
// and even HEX can select different destinations by width. An ID-only overwrite
// would silently discard one requested key before its handler ever sees it.
static const int k_ui_cmd_coalescible_setter_ids[] = {
    DSD_APP_CMD_GAIN_SET,
    DSD_APP_CMD_AGAIN_SET,
    DSD_APP_CMD_INPUT_VOL_SET,
    DSD_APP_CMD_RTL_SET_FREQ,
    DSD_APP_CMD_MANUAL_TUNE,
    DSD_APP_CMD_RTL_SET_GAIN,
    DSD_APP_CMD_RTL_SET_PPM,
    DSD_APP_CMD_RTL_SET_BW,
    DSD_APP_CMD_RTL_SET_SQL_DB,
    DSD_APP_CMD_RTL_SET_SQL_SETTING,
    DSD_APP_CMD_RTL_SET_VOL_MULT,
    DSD_APP_CMD_HANGTIME_SET,
    DSD_APP_CMD_MOD_SET,
    DSD_APP_CMD_DECODE_MODE_SET,
    DSD_APP_CMD_TRUNK_SET,
    DSD_APP_CMD_SLOT_PREF_SET,
    DSD_APP_CMD_SCAN_VOICE_QUALIFY_MS_SET,
    DSD_APP_CMD_SCAN_VOICE_HOLD_MS_SET,
    DSD_APP_CMD_NFM_BANDWIDTH_SET,
    DSD_APP_CMD_AM_BANDWIDTH_SET,
};

static int
ui_cmd_is_coalescible_setter(int cmd_id) {
    for (size_t i = 0; i < sizeof k_ui_cmd_coalescible_setter_ids / sizeof k_ui_cmd_coalescible_setter_ids[0]; i++) {
        if (k_ui_cmd_coalescible_setter_ids[i] == cmd_id) {
            return 1;
        }
    }
    return 0;
}

static int
ui_cmd_is_string_payload_id(int cmd_id) {
    for (size_t i = 0; i < sizeof k_ui_cmd_string_ids / sizeof k_ui_cmd_string_ids[0]; i++) {
        if (k_ui_cmd_string_ids[i] == cmd_id) {
            return 1;
        }
    }
    return 0;
}

static struct dsd_app_command*
ui_cmd_find_pending_tail_unlocked(int cmd_id) {
    if (q_is_empty_unlocked()) {
        return NULL;
    }
    const size_t tail_idx = (g_tail + DSD_APP_CMD_Q_CAP - 1U) % DSD_APP_CMD_Q_CAP;
    return g_q[tail_idx].id == cmd_id ? &g_q[tail_idx] : NULL;
}

/* Whether a setter may overwrite the command queued at the tail (the same command). The squelch setting coalesces only
   with a request of the same mode: an AUTO request sets a margin and a LEVEL one a level that the other leaves alone,
   so replacing one with the other would drop what the first kept. */
static int
ui_cmd_coalesces_with(const struct dsd_app_command* pending, int cmd_id, const void* payload, size_t payload_sz) {
    if (!pending) {
        return 0;
    }
    if (cmd_id != DSD_APP_CMD_RTL_SET_SQL_SETTING) {
        return 1;
    }
    dsd_app_squelch_setting_payload queued;
    dsd_app_squelch_setting_payload next;
    if (!payload || pending->n != sizeof queued || payload_sz != sizeof next) {
        return 0;
    }
    DSD_MEMCPY(&queued, pending->data, sizeof queued);
    DSD_MEMCPY(&next, payload, sizeof next);
    return queued.mode == next.mode;
}

static void
ui_cmd_store_payload(struct dsd_app_command* c, int cmd_id, const void* payload, size_t payload_sz) {
    // Erase before overwrite, including coalescing to a shorter rejected payload.
    // Wiping all commands also covers legacy key setters and account credentials.
    DSD_SECURE_ZERO(c, sizeof(*c));
    size_t copy_sz = payload ? payload_sz : 0;
    if (copy_sz > sizeof c->data) {
        copy_sz = sizeof c->data;
    }
    c->id = cmd_id;
    c->n = copy_sz;
    c->payload_truncated = (payload_sz > copy_sz) ? 1U : 0U;
    if (copy_sz && payload) {
        DSD_MEMCPY(c->data, payload, copy_sz);
        if (c->payload_truncated && ui_cmd_is_string_payload_id(cmd_id)) {
            c->data[copy_sz - 1U] = '\0';
        }
    }
}

static void ui_set_toast(dsd_state* state, int ttl_s, const char* fmt, ...) DSD_ATTR_FORMAT(printf, 3, 4);

static void
ui_set_toast(dsd_state* state, int ttl_s, const char* fmt, ...) {
    if (!state || !fmt) {
        return;
    }
    if (ttl_s < 1) {
        ttl_s = 1;
    }
    va_list ap;
    va_start(ap, fmt);
    (void)DSD_VSNPRINTF(state->ui_msg, sizeof state->ui_msg, fmt, ap);
    va_end(ap);
    state->ui_msg_expire = dsd_realtime_time() + ttl_s;
}

/* An I/Q replay plays the tuning its capture recorded and defers every other retune unseen (issue #575), so a tune or a
   tuner release asked of it would be reported as done and never land. Returns 1, with the reason toasted, while the
   input in force is a replay (dsd_opts_input_is_iq_replay()); the caller fails the command. */
static int
ui_cmd_refuse_replay_tune(const dsd_opts* opts, dsd_state* state) {
    if (!dsd_opts_input_is_iq_replay(opts)) {
        return 0;
    }
    ui_set_toast(state, 3, "An I/Q replay cannot retune.");
    return 1;
}

static int
ui_reconfigure_output_for_input_policy(dsd_opts* opts, dsd_state* state) {
    if (dsd_audio_reconfigure_output_for_input_policy(opts) != 0) {
        ui_set_toast(state, 4, "Failed: audio output reconfigure");
        return -1;
    }
    return 0;
}

/* The decoder left its input. A tone heard on it does not describe the next one, so it goes now
   rather than when the detector next loses it (issue #522), and an I/Q replay's capture clock no
   longer times what the decoder reads, so decode time is the system's again (issue #572). */
static void
ui_input_left(dsd_opts* opts, dsd_state* state) {
    dsd_analog_rx_reset(state);
    dsd_engine_decode_clock_leave_replay(opts, state);
}

/* The audio input changed: the old one is left (ui_input_left()), then the output follows the
   new input's policy. */
static int
ui_input_switched(dsd_opts* opts, dsd_state* state) {
    ui_input_left(opts, state);
    return ui_reconfigure_output_for_input_policy(opts, state);
}

static int ui_scan_goes_on(const dsd_opts* opts, const dsd_state* state);

/* Whether a menu input switch must not run (issue #634), with a toast saying why: the engine mode reads no input a
   switch would open (svc_input_switch_unsupported()), or a scan is in force, whose timing snapshots belong to the input
   that runs (a scan scope or -Y with rows, ui_scan_goes_on(), or --trunk-scan with targets). */
static int
ui_input_mode_refused(const dsd_opts* opts, dsd_state* state) {
    const char* why = svc_input_switch_unsupported(opts);
    if (why) {
        ui_set_toast(state, 4, "Unsupported: %s", why);
        return 1;
    }
    return 0;
}

static int
ui_input_switch_refused(const dsd_opts* opts, dsd_state* state) {
    if (ui_input_mode_refused(opts, state)) {
        return 1;
    }
    if (ui_scan_goes_on(opts, state) || (opts->trunk_scan_enabled == 1 && state->trunk_scan_target_count > 0U)) {
        ui_set_toast(state, 4, "Unsupported: stop the scan first");
        return 1;
    }
    return 0;
}

/* What an input switch did (a dsd_audio_input_switch_result), as the command's toast and status (issue #634). @p what
   names the kind of input, @p name the input. A switch that ran replaced the stream the decoder read, so the engine
   ends the reception before it decodes the new one (state->input_boundary). When the output then cannot follow the new
   input's policy, the new input stays and the output reconfigure's own toast says what failed. The caller holds the
   P25 SM tick guard, from the switch through the output reconfigure: the watchdog may flush audio into the stream a
   reconfigure closes. */
static int
ui_cmd_finish_input_switch(dsd_opts* opts, dsd_state* state, int rc, const char* what, const char* name) {
    switch (rc) {
        case DSD_AUDIO_INPUT_SWITCHED:
            state->input_boundary = 1;
            if (ui_input_switched(opts, state) != 0) {
                return UI_CMD_APPLY_FAILED;
            }
            ui_set_toast(state, 3, "Applied: %s -> %s", what, name);
            return UI_CMD_APPLY_COMPLETED;
        case DSD_AUDIO_INPUT_RESTARTED:
            state->input_boundary = 1;
            ui_input_left(opts, state);
            ui_set_toast(state, 5, "Failed: %s -> %s; the UDP input before it was restarted", what, name);
            return UI_CMD_APPLY_FAILED;
        case DSD_AUDIO_INPUT_LOST:
            state->input_boundary = 1;
            ui_input_left(opts, state);
            ui_set_toast(state, 5, "Failed: %s -> %s; the UDP input before it did not bind again", what, name);
            return UI_CMD_APPLY_FAILED;
        default: ui_set_toast(state, 4, "Failed: %s -> %s (see log)", what, name); return UI_CMD_APPLY_FAILED;
    }
}

static inline int
ui_rc_is_not_supported(int rc) {
    return rc == DSD_ERR_NOT_SUPPORTED;
}

static int
ui_cmd_apply_status_from_service_rc(int rc) {
    if (rc == 0) {
        return UI_CMD_APPLY_COMPLETED;
    }
    if (rc == DSD_ERR_NOT_SUPPORTED) {
        return UI_CMD_APPLY_UNSUPPORTED;
    }
    return UI_CMD_APPLY_FAILED;
}

#ifdef USE_RADIO
static int
ui_cmd_apply_status_from_tune_rc(int rc) {
    if (rc == RTL_STREAM_TUNE_TIMEOUT) {
        /* The controller accepted the tune and will publish its terminal
         * result after the synchronous wait expires. Command completion here
         * means the request was accepted, not that hardware already moved. */
        return UI_CMD_APPLY_COMPLETED;
    }
    return ui_cmd_apply_status_from_service_rc(rc);
}
#endif

struct UiVisibilityToggleSpec {
    int cmd_id;
    size_t opts_offset;
};

static const struct UiVisibilityToggleSpec k_ui_visibility_toggle_specs[] = {
    {DSD_APP_CMD_UI_SHOW_DSP_PANEL_TOGGLE, offsetof(dsd_opts, frontend_display.show_dsp_panel)},
    {DSD_APP_CMD_UI_SHOW_P25_METRICS_TOGGLE, offsetof(dsd_opts, frontend_display.show_p25_metrics)},
    {DSD_APP_CMD_UI_SHOW_P25_AFFIL_TOGGLE, offsetof(dsd_opts, frontend_display.show_p25_affiliations)},
    {DSD_APP_CMD_UI_SHOW_P25_NEIGHBORS_TOGGLE, offsetof(dsd_opts, frontend_display.show_p25_neighbors)},
    {DSD_APP_CMD_UI_SHOW_P25_IDEN_TOGGLE, offsetof(dsd_opts, frontend_display.show_p25_iden_plan)},
    {DSD_APP_CMD_UI_SHOW_P25_CCC_TOGGLE, offsetof(dsd_opts, frontend_display.show_p25_cc_candidates)},
    {DSD_APP_CMD_UI_SHOW_CHANNELS_TOGGLE, offsetof(dsd_opts, frontend_display.show_channels)},
    {DSD_APP_CMD_UI_SHOW_P25_CALLSIGN_TOGGLE, offsetof(dsd_opts, frontend_display.show_p25_callsign_decode)},
};

static int
ui_apply_visibility_toggle(dsd_opts* opts, int cmd_id) {
    for (size_t i = 0; i < (sizeof k_ui_visibility_toggle_specs / sizeof k_ui_visibility_toggle_specs[0]); ++i) {
        if (k_ui_visibility_toggle_specs[i].cmd_id == cmd_id) {
            uint8_t* field = (uint8_t*)((char*)opts + k_ui_visibility_toggle_specs[i].opts_offset);
            *field = *field ? 0 : 1;
            return 1;
        }
    }
    return 0;
}

static int
apply_cmd_ui_visibility(dsd_opts* opts, const struct dsd_app_command* c) {
    if (!opts || !c) {
        return 0;
    }
    return ui_apply_visibility_toggle(opts, c->id);
}

static int
ui_cmd_copy_payload_string(const struct dsd_app_command* c, char* out, size_t out_sz) {
    if (!c || !out || out_sz == 0 || c->n == 0) {
        return 0;
    }
    size_t n = (c->n < out_sz - 1) ? c->n : out_sz - 1;
    DSD_MEMCPY(out, c->data, n);
    out[n] = '\0';
    return 1;
}

/* A path payload, whole (issue #634): one that does not fit @p out, or that the queue cut short, would name another
   file than the one asked for, which an input switch would open, so it is refused. */
static int
ui_cmd_copy_payload_path(const struct dsd_app_command* c, char* out, size_t out_sz) {
    if (!c || !out || out_sz == 0 || c->n == 0 || c->payload_truncated) {
        return 0;
    }
    const char* end = (const char*)memchr(c->data, '\0', c->n);
    const size_t len = end ? (size_t)(end - (const char*)c->data) : c->n;
    if (len == 0 || len >= out_sz) {
        return 0;
    }
    DSD_MEMCPY(out, c->data, len);
    out[len] = '\0';
    return 1;
}

typedef int (*dsd_app_command_handler_fn)(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c);

struct dsd_app_command_handler_entry {
    int cmd_id;
    dsd_app_command_handler_fn fn;
};

static int
ui_cmd_apply_handler_table(const struct dsd_app_command_handler_entry* entries, size_t count, dsd_opts* opts,
                           dsd_state* state, const struct dsd_app_command* c) {
    if (!entries || !c) {
        return 0;
    }
    for (size_t i = 0; i < count; ++i) {
        if (entries[i].cmd_id == c->id) {
            return entries[i].fn(opts, state, c);
        }
    }
    return 0;
}

static void
ui_cmd_reset_key_mute_state(dsd_opts* opts, dsd_state* state) {
    if (!opts || !state) {
        return;
    }
    state->key_profile_ref[0] = '\0';
    /* Key ownership stays live; only the mute decision updates configured defaults. */
    const int scoped = dsd_scan_mode_suspend(opts, state);
    dsd_key_apply_mute_policy(opts, state);
    if (scoped) {
        (void)dsd_scan_mode_resume(opts, state);
    }
    // Every direct key mutation funnels through here: invalidate the
    // encrypted-target lockout ledger so each locked target re-verifies once
    // against the new key material.
    dsd_enc_lockout_bump_key_epoch(state);
}

static int
apply_cmd_key_management_basic(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    if (!opts || !state || !c) {
        return 0;
    }
    switch (c->id) {
        case DSD_APP_CMD_KEY_BASIC_SET: {
            if (c->n >= (int)sizeof(uint32_t)) {
                uint32_t v = 0;
                DSD_MEMCPY(&v, c->data, sizeof v);
                state->K = v;
                state->basic_key_present = 1;
                DSD_SECURE_ZERO(&v, sizeof v);
                ui_cmd_reset_key_mute_state(opts, state);
            }
            return 1;
        }
        case DSD_APP_CMD_KEY_SCRAMBLER_SET: {
            if (c->n >= (int)sizeof(uint32_t)) {
                uint32_t v = 0;
                DSD_MEMCPY(&v, c->data, sizeof v);
                state->R = v;
                state->scalar_key_present[0] = 1;
                DSD_SECURE_ZERO(&v, sizeof v);
                ui_cmd_reset_key_mute_state(opts, state);
            }
            return 1;
        }
        case DSD_APP_CMD_KEY_RC4DES_SET: {
            if (c->n >= (int)sizeof(uint64_t)) {
                uint64_t v = 0;
                DSD_MEMCPY(&v, c->data, sizeof v);
                state->R = v;
                state->RR = v;
                state->scalar_key_present[0] = state->scalar_key_present[1] = 1;
                DSD_SECURE_ZERO(&v, sizeof v);
                ui_cmd_reset_key_mute_state(opts, state);
            }
            return 1;
        }
        default: return 0;
    }
}

static int
apply_cmd_key_hytera_set(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    if (!opts || !state || !c || c->n < (int)(sizeof(uint64_t) * 5)) {
        return 0;
    }

    struct {
        uint64_t H, K1, K2, K3, K4;
    } p;

    DSD_MEMCPY(&p, c->data, sizeof p);
    state->H = p.H;
    state->K1 = p.K1;
    state->K2 = p.K2;
    state->K3 = p.K3;
    state->K4 = p.K4;
    if (state->K3 != 0ULL || state->K4 != 0ULL) {
        state->hytera_key_segments = 4U;
    } else if (state->K2 != 0ULL) {
        state->hytera_key_segments = 2U;
    } else {
        state->hytera_key_segments = 1U;
    }
    ui_cmd_reset_key_mute_state(opts, state);
    DSD_SNPRINTF(state->ui_msg, sizeof state->ui_msg, "Hytera key loaded (%s)",
                 (state->M == 1) ? "forced" : "not forced");
    state->ui_msg_expire = dsd_realtime_time() + 5;
    DSD_SECURE_ZERO(&p, sizeof p);
    return 1;
}

static int
apply_cmd_key_aes_set(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    if (!opts || !state || !c || c->n < (int)(sizeof(uint64_t) * 4)) {
        return 0;
    }

    struct {
        uint64_t K1, K2, K3, K4;
    } p;

    DSD_MEMCPY(&p, c->data, sizeof p);
    state->A1[0] = state->A1[1] = p.K1;
    state->A2[0] = state->A2[1] = p.K2;
    state->A3[0] = state->A3[1] = p.K3;
    state->A4[0] = state->A4[1] = p.K4;
    state->aes_key_loaded[0] = state->aes_key_loaded[1] = 1;
    state->aes_key_segments[0] = state->aes_key_segments[1] = 4U;
    for (int i = 0; i < 8; i++) {
        state->aes_key[i + 0] = (uint8_t)((p.K1 >> (56 - (i * 8))) & 0xFFU);
        state->aes_key[i + 8] = (uint8_t)((p.K2 >> (56 - (i * 8))) & 0xFFU);
        state->aes_key[i + 16] = (uint8_t)((p.K3 >> (56 - (i * 8))) & 0xFFU);
        state->aes_key[i + 24] = (uint8_t)((p.K4 >> (56 - (i * 8))) & 0xFFU);
    }
    state->H = 0ULL;
    state->K1 = 0ULL;
    state->K2 = 0ULL;
    state->K3 = 0ULL;
    state->K4 = 0ULL;
    state->hytera_key_segments = 0U;
    ui_cmd_reset_key_mute_state(opts, state);
    DSD_SECURE_ZERO(&p, sizeof p);
    return 1;
}

static int
apply_cmd_key_management_block_keys(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    static const struct dsd_app_command_handler_entry entries[] = {
        {DSD_APP_CMD_KEY_HYTERA_SET, apply_cmd_key_hytera_set},
        {DSD_APP_CMD_KEY_AES_SET, apply_cmd_key_aes_set},
    };
    return ui_cmd_apply_handler_table(entries, sizeof entries / sizeof entries[0], opts, state, c);
}

typedef void (*UiStreamKeyLoaderFn)(dsd_state* state, const char* input, int show_keys);

struct UiStreamKeyLoaderEntry {
    int cmd_id;
    size_t payload_cap;
    UiStreamKeyLoaderFn fn;
};

static void
ui_load_ken_scrambler_key(dsd_state* state, const char* input, int show_keys) {
    char local[128];
    DSD_SNPRINTF(local, sizeof local, "%s", input ? input : "");
    ken_dmr_scrambler_keystream_creation(state, local, show_keys);
    DSD_SECURE_ZERO(local, sizeof local);
}

static void
ui_load_anytone_bp_key(dsd_state* state, const char* input, int show_keys) {
    char local[128];
    DSD_SNPRINTF(local, sizeof local, "%s", input ? input : "");
    anytone_bp_keystream_creation(state, local, show_keys);
    DSD_SECURE_ZERO(local, sizeof local);
}

static int
apply_cmd_key_management_stream_keys(const dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    static const struct UiStreamKeyLoaderEntry entries[] = {
        {DSD_APP_CMD_KEY_TYT_AP_SET, 256U, tyt_ap_pc4_keystream_creation},
        {DSD_APP_CMD_KEY_RETEVIS_RC2_SET, 256U, retevis_rc2_keystream_creation},
        {DSD_APP_CMD_KEY_TYT_EP_SET, 256U, tyt_ep_aes_keystream_creation},
        {DSD_APP_CMD_KEY_KEN_SCR_SET, 128U, ui_load_ken_scrambler_key},
        {DSD_APP_CMD_KEY_ANYTONE_BP_SET, 128U, ui_load_anytone_bp_key},
        {DSD_APP_CMD_KEY_XOR_SET, 256U, straight_mod_xor_keystream_creation},
    };
    if (!opts || !state || !c) {
        return 0;
    }

    for (size_t i = 0U; i < sizeof entries / sizeof entries[0]; i++) {
        if (entries[i].cmd_id == c->id) {
            /* Zeroed up front: an empty payload copies nothing, and the wipe below covers the whole buffer either
             * way, so it should never be handed bytes nothing wrote. */
            char s[256] = {0};
            if (ui_cmd_copy_payload_string(c, s, entries[i].payload_cap)) {
                entries[i].fn(state, s, opts->show_keys);
                dsd_enc_lockout_bump_key_epoch(state);
            }
            DSD_SECURE_ZERO(s, sizeof s);
            return 1;
        }
    }
    return 0;
}

static int
apply_cmd_key_management_m17(dsd_state* state, const struct dsd_app_command* c) {
    if (!state || !c || c->id != DSD_APP_CMD_M17_USER_DATA_SET) {
        return 0;
    }
    if (c->n > 0) {
        size_t n = (c->n < sizeof(state->m17dat) - 1) ? c->n : sizeof(state->m17dat) - 1;
        DSD_MEMCPY(state->m17dat, c->data, n);
        state->m17dat[n] = '\0';
    }
    return 1;
}

static int
apply_cmd_key_management(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    if (!c) {
        return 0;
    }
    if (apply_cmd_key_management_basic(opts, state, c)) {
        return 1;
    }
    if (apply_cmd_key_management_block_keys(opts, state, c)) {
        return 1;
    }
    if (apply_cmd_key_management_stream_keys(opts, state, c)) {
        return 1;
    }
    return apply_cmd_key_management_m17(state, c);
}

#ifdef USE_RADIO
static int
apply_dsp_op_cqpsk_toggle(const dsd_app_dsp_payload* p, const dsd_opts* opts) {
    if (!p || p->op != DSD_APP_DSP_OP_TOGGLE_CQ) {
        return 0;
    }
    svc_toggle_rtl_cqpsk(opts);
    return 1;
}

static int
apply_dsp_op_iq_and_ted(const dsd_app_dsp_payload* p) {
    if (!p) {
        return 0;
    }
    switch (p->op) {
        case DSD_APP_DSP_OP_TOGGLE_IQBAL: {
            int on = rtl_stream_get_iq_balance();
            int new_on = on ? 0 : 1;
            rtl_stream_toggle_iq_balance(new_on);
            dsd_setenv("DSD_NEO_IQ_BALANCE", new_on ? "1" : "0", 1);
            return 1;
        }
        case DSD_APP_DSP_OP_IQ_DC_TOGGLE: {
            int k = 0;
            int on = rtl_stream_get_iq_dc(&k);
            int new_on = on ? 0 : 1;
            rtl_stream_set_iq_dc(new_on, -1);
            dsd_setenv("DSD_NEO_IQ_DC_BLOCK", new_on ? "1" : "0", 1);
            return 1;
        }
        case DSD_APP_DSP_OP_IQ_DC_K_DELTA: {
            int k = 0;
            (void)rtl_stream_get_iq_dc(&k);
            /* In 64 bits, saturated: a delta can be any int32 (the control API sends what its client asked for). Not
               below 0, which the setter reads as "keep the shift"; it clamps the rest to its own range. */
            int64_t next = (int64_t)k + p->a;
            if (next > INT32_MAX) {
                next = INT32_MAX;
            } else if (next < 0) {
                next = 0;
            }
            rtl_stream_set_iq_dc(-1, (int)next);
            return 1;
        }
        case DSD_APP_DSP_OP_TED_GAIN_SET: {
            int g_milli = p->a;
            if (g_milli < 10) {
                g_milli = 10;
            }
            if (g_milli > 500) {
                g_milli = 500;
            }
            rtl_stream_set_ted_gain((float)g_milli * 0.001f);
            return 1;
        }
        default: return 0;
    }
}

/* The tuner autogain toggle. Under --trunk-scan it flips the configured setting the scan puts back at every retune,
   not the stream's: each retune forces the supervisor off under a manual gain, so reading the stream there would always
   see Off and the toggle could never turn it off again. The stream changes only while the setting is in force. */
static int
apply_dsp_op_frontend_gain(const dsd_app_dsp_payload* p, const dsd_opts* opts, dsd_state* state) {
    if (!p) {
        return 0;
    }
    if (p->op == DSD_APP_DSP_OP_TUNER_AUTOGAIN_TOGGLE) {
        int configured = 0;
        /* The P25 watchdog's control-channel returns read the configured setting under the P25 SM tick guard, so the
           read, the change and the stream write it implies are one step under it. */
        p25_sm_tick_guard_enter();
        const int scan = opts && state && dsd_engine_trunk_scan_saved_tuner_autogain(state, &configured);
        if (scan) {
            const int on = configured ? 0 : 1;
            if (dsd_engine_trunk_scan_set_configured_autogain(opts, state, on) == 1) {
                /* The scan's setting, which lasts while the scan runs: not the operator's for later opens. */
                rtl_stream_enforce_tuner_autogain(on);
            }
        }
        p25_sm_tick_guard_leave();
        if (scan) {
            dsd_app_rtl_gain_view view;
            char notice[96];
            (void)dsd_app_rtl_gain_view_get(opts, state, &view);
            (void)dsd_app_rtl_autogain_view_edit_notice(&view, notice, sizeof notice);
            ui_set_toast(state, 3, "%s", notice);
            return 1;
        }
        int on = rtl_stream_get_tuner_autogain();
        rtl_stream_set_tuner_autogain(on ? 0 : 1);
        return 1;
    }
    return 0;
}

static void
apply_dsp_op(const dsd_app_dsp_payload* p, const dsd_opts* opts, dsd_state* state) {
    if (!p) {
        return;
    }
    if (apply_dsp_op_cqpsk_toggle(p, opts)) {
        return;
    }
    if (apply_dsp_op_iq_and_ted(p)) {
        return;
    }
    (void)apply_dsp_op_frontend_gain(p, opts, state);
}
#endif

static int
apply_cmd_runtime_toggles(dsd_opts* opts, const struct dsd_app_command* c) {
    if (!opts || !c) {
        return 0;
    }
    switch (c->id) {
        case DSD_APP_CMD_DMR_LE_TOGGLE: svc_toggle_dmr_le(opts); return 1;
        case DSD_APP_CMD_ALL_MUTES_TOGGLE: svc_toggle_all_mutes(opts); return 1;
        case DSD_APP_CMD_INV_X2_TOGGLE: svc_toggle_inv_x2(opts); return 1;
        case DSD_APP_CMD_INV_DMR_TOGGLE: svc_toggle_inv_dmr(opts); return 1;
        case DSD_APP_CMD_INV_DPMR_TOGGLE: svc_toggle_inv_dpmr(opts); return 1;
        case DSD_APP_CMD_INV_M17_TOGGLE: svc_toggle_inv_m17(opts); return 1;
        default: return 0;
    }
}

static int
ui_cmd_handle_wav_static_open(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    int result = UI_CMD_APPLY_COMPLETED;
    if (state && c->n > 0) {
        char path[1024] = {0};
        if (ui_cmd_copy_payload_string(c, path, sizeof path)) {
            int rc = svc_open_static_wav(opts, state, path);
            result = ui_cmd_apply_status_from_service_rc(rc);
            if (rc == 0) {
                ui_set_toast(state, 3, "Applied: Static WAV output -> %s", path);
            } else {
                ui_set_toast(state, 4, "Failed: Static WAV open -> %s", path);
            }
        }
    }
    return result;
}

static int
ui_cmd_handle_wav_raw_open(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    int result = UI_CMD_APPLY_COMPLETED;
    if (state && c->n > 0) {
        char path[1024] = {0};
        if (ui_cmd_copy_payload_string(c, path, sizeof path)) {
            int rc = svc_open_raw_wav(opts, state, path);
            result = ui_cmd_apply_status_from_service_rc(rc);
            if (rc == 0) {
                ui_set_toast(state, 3, "Applied: Raw WAV output -> %s", path);
            } else {
                ui_set_toast(state, 4, "Failed: Raw WAV open -> %s", path);
            }
        }
    }
    return result;
}

static int
ui_cmd_handle_dsp_out_set(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    int result = UI_CMD_APPLY_COMPLETED;
    if (c->n > 0) {
        char name[256] = {0};
        if (ui_cmd_copy_payload_string(c, name, sizeof name)) {
            int rc = svc_set_dsp_output_file(opts, name);
            result = ui_cmd_apply_status_from_service_rc(rc);
            if (rc == 0) {
                ui_set_toast(state, 3, "Applied: DSP output -> %s", opts->dsp_out_file);
            } else {
                ui_set_toast(state, 4, "Failed: DSP output path invalid");
            }
        }
    }
    return result;
}

static int
apply_cmd_io_and_import_file_outputs_a(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    static const struct dsd_app_command_handler_entry k_handlers[] = {
        {DSD_APP_CMD_WAV_STATIC_OPEN, ui_cmd_handle_wav_static_open},
        {DSD_APP_CMD_WAV_RAW_OPEN, ui_cmd_handle_wav_raw_open},
        {DSD_APP_CMD_DSP_OUT_SET, ui_cmd_handle_dsp_out_set},
    };
    if (!opts || !c) {
        return 0;
    }
    return ui_cmd_apply_handler_table(k_handlers, sizeof k_handlers / sizeof k_handlers[0], opts, state, c);
}

static int
ui_cmd_handle_symcap_open(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    int result = UI_CMD_APPLY_COMPLETED;
    if (state && c->n > 0) {
        char path[1024] = {0};
        if (ui_cmd_copy_payload_string(c, path, sizeof path)) {
            int rc = svc_open_symbol_out(opts, state, path);
            result = ui_cmd_apply_status_from_service_rc(rc);
            if (rc == 0) {
                ui_set_toast(state, 3, "Applied: Symbol capture -> %s", path);
            } else {
                ui_set_toast(state, 4, "Failed: Symbol capture open -> %s", path);
            }
        }
    }
    return result;
}

/* The kind of input a path opens as (dsd_audio_path_input_kind()), for a toast. */
static const char*
ui_file_input_what(const char* path) {
    switch (dsd_audio_path_input_kind(path)) {
        case DSD_AUDIO_INPUT_SYMBOL_FLT: return "Symbol stream input";
        case DSD_AUDIO_INPUT_SYMBOL_BIN: return "Symbol input";
        default: return "WAV input";
    }
}

/* Input > Switch source > Symbol file, for a capture (.bin): replayed at symbol pace (issue #634). */
static int
ui_cmd_handle_symbol_in_open(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    char path[sizeof opts->audio_in_dev];
    if (!state || c->n == 0) {
        return UI_CMD_APPLY_COMPLETED;
    }
    if (!ui_cmd_copy_payload_path(c, path, sizeof path)) {
        ui_set_toast(state, 4, "Failed: the path is longer than %zu characters", sizeof path - 1U);
        return UI_CMD_APPLY_FAILED;
    }
    if (ui_input_switch_refused(opts, state)) {
        return UI_CMD_APPLY_UNSUPPORTED;
    }
    p25_sm_tick_guard_enter();
    const int rc = svc_open_symbol_in(opts, state, path);
    const int result = ui_cmd_finish_input_switch(opts, state, rc, "Symbol input", path);
    p25_sm_tick_guard_leave();
    return result;
}

/* Input > Switch source > WAV / raw file: the file opens as `-i` opens it, so a saved config reopens the same kind
   (issue #634). */
static int
ui_cmd_handle_input_wav_set(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    char path[sizeof opts->audio_in_dev];
    if (!state || c->n == 0) {
        return UI_CMD_APPLY_COMPLETED;
    }
    if (!ui_cmd_copy_payload_path(c, path, sizeof path)) {
        ui_set_toast(state, 4, "Failed: the path is longer than %zu characters", sizeof path - 1U);
        return UI_CMD_APPLY_FAILED;
    }
    if (ui_input_switch_refused(opts, state)) {
        return UI_CMD_APPLY_UNSUPPORTED;
    }
    p25_sm_tick_guard_enter();
    const int rc = svc_open_audio_file_in(opts, state, path);
    const int result = ui_cmd_finish_input_switch(opts, state, rc, ui_file_input_what(path), path);
    p25_sm_tick_guard_leave();
    return result;
}

/* Input > Switch source > Symbol file, for a float symbol stream (.raw, .sym). */
static int
ui_cmd_handle_input_sym_stream_set(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    char path[sizeof opts->audio_in_dev];
    if (!state || c->n == 0) {
        return UI_CMD_APPLY_COMPLETED;
    }
    if (!ui_cmd_copy_payload_path(c, path, sizeof path)) {
        ui_set_toast(state, 4, "Failed: the path is longer than %zu characters", sizeof path - 1U);
        return UI_CMD_APPLY_FAILED;
    }
    if (ui_input_switch_refused(opts, state)) {
        return UI_CMD_APPLY_UNSUPPORTED;
    }
    p25_sm_tick_guard_enter();
    const int rc = svc_open_symbol_stream_in(opts, state, path);
    const int result = ui_cmd_finish_input_switch(opts, state, rc, "Symbol stream input", path);
    p25_sm_tick_guard_leave();
    return result;
}

/* Input > Switch source > Pulse: the default capture device. */
static int
ui_cmd_handle_input_set_pulse(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    (void)c;
    if (!state) {
        return UI_CMD_APPLY_COMPLETED;
    }
    if (ui_input_switch_refused(opts, state)) {
        return UI_CMD_APPLY_UNSUPPORTED;
    }
    p25_sm_tick_guard_enter();
    const int rc = svc_set_pulse_input(opts, state, "");
    const int result = ui_cmd_finish_input_switch(opts, state, rc, "Pulse input", "default device");
    p25_sm_tick_guard_leave();
    return result;
}

static int
apply_cmd_io_and_import_file_outputs_b(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    static const struct dsd_app_command_handler_entry k_handlers[] = {
        {DSD_APP_CMD_SYMCAP_OPEN, ui_cmd_handle_symcap_open},
        {DSD_APP_CMD_SYMBOL_IN_OPEN, ui_cmd_handle_symbol_in_open},
        {DSD_APP_CMD_INPUT_WAV_SET, ui_cmd_handle_input_wav_set},
        {DSD_APP_CMD_INPUT_SYM_STREAM_SET, ui_cmd_handle_input_sym_stream_set},
        {DSD_APP_CMD_INPUT_SET_PULSE, ui_cmd_handle_input_set_pulse},
    };
    if (!opts || !c) {
        return 0;
    }
    return ui_cmd_apply_handler_table(k_handlers, sizeof k_handlers / sizeof k_handlers[0], opts, state, c);
}

static int
ui_cmd_parse_host_port_payload(const struct dsd_app_command* c, char* host, size_t host_sz, int32_t* port) {
    if (!c || !host || host_sz == 0 || !port || c->n < (int)(256 + sizeof(int32_t))) {
        return 0;
    }
    DSD_MEMSET(host, 0, host_sz);
    DSD_MEMCPY(host, c->data, (host_sz > 255) ? 255 : (host_sz - 1));
    DSD_MEMCPY(port, c->data + 256, sizeof *port);
    return 1;
}

static int
ui_cmd_handle_udp_out_cfg(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    char host[256] = {0};
    int32_t port = 0;
    int result = UI_CMD_APPLY_COMPLETED;
    if (state && ui_cmd_parse_host_port_payload(c, host, sizeof host, &port)) {
        int rc = svc_udp_output_config(opts, state, host, port);
        result = ui_cmd_apply_status_from_service_rc(rc);
        if (rc == 0) {
            ui_set_toast(state, 3, "UDP output configured: %s:%d", host, (int)port);
        } else {
            ui_set_toast(state, 4, "UDP output failed: %s:%d", host, (int)port);
        }
    }
    return result;
}

/* A TCP audio connect to @p host:@p port, from the menu's host and port or the '8' key (issue #634). */
static int
ui_cmd_tcp_connect_audio(dsd_opts* opts, dsd_state* state, const char* host, int port) {
    if (ui_input_switch_refused(opts, state)) {
        return UI_CMD_APPLY_UNSUPPORTED;
    }
    char endpoint[300];
    DSD_SNPRINTF(endpoint, sizeof endpoint, "%s:%d", host, port);
    p25_sm_tick_guard_enter();
    const int rc = svc_tcp_connect_audio(opts, state, host, port);
    const int result = ui_cmd_finish_input_switch(opts, state, rc, "TCP audio", endpoint);
    p25_sm_tick_guard_leave();
    return result;
}

static int
ui_cmd_handle_tcp_connect_audio_cfg(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    char host[256] = {0};
    int32_t port = 0;
    if (!state || !ui_cmd_parse_host_port_payload(c, host, sizeof host, &port)) {
        return UI_CMD_APPLY_COMPLETED;
    }
    return ui_cmd_tcp_connect_audio(opts, state, host, (int)port);
}

static int
ui_cmd_handle_rigctl_connect_cfg(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    char host[256] = {0};
    int32_t port = 0;
    int result = UI_CMD_APPLY_COMPLETED;
    if (state && ui_cmd_parse_host_port_payload(c, host, sizeof host, &port)) {
        int rc = svc_rigctl_connect(opts, state, host, port);
        result = ui_cmd_apply_status_from_service_rc(rc);
        if (rc == 0) {
            ui_set_toast(state, 3, "Rigctl connected: %s:%d", host, (int)port);
        } else {
            ui_set_toast(state, 4, "Rigctl connect failed: %s:%d", host, (int)port);
        }
    }
    return result;
}

/* Input > Switch source > UDP audio: binds now (issue #634). */
static int
ui_cmd_handle_udp_input_cfg(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    char bind[256] = {0};
    int32_t port = 0;
    if (!state || !ui_cmd_parse_host_port_payload(c, bind, sizeof bind, &port)) {
        return UI_CMD_APPLY_COMPLETED;
    }
    char endpoint[300];
    DSD_SNPRINTF(endpoint, sizeof endpoint, "%s:%d", bind[0] ? bind : "127.0.0.1", (int)port);
    if (port < 1 || port > 65535) {
        ui_set_toast(state, 4, "Failed: UDP input port %d is out of range (1-65535)", (int)port);
        return UI_CMD_APPLY_FAILED;
    }
    if (ui_input_switch_refused(opts, state)) {
        return UI_CMD_APPLY_UNSUPPORTED;
    }
    p25_sm_tick_guard_enter();
    const int rc = svc_udp_input_config(opts, state, bind, (int)port);
    const int result = ui_cmd_finish_input_switch(opts, state, rc, "UDP input", endpoint);
    p25_sm_tick_guard_leave();
    return result;
}

static int
apply_cmd_io_and_import_network(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    static const struct dsd_app_command_handler_entry k_handlers[] = {
        {DSD_APP_CMD_UDP_OUT_CFG, ui_cmd_handle_udp_out_cfg},
        {DSD_APP_CMD_TCP_CONNECT_AUDIO_CFG, ui_cmd_handle_tcp_connect_audio_cfg},
        {DSD_APP_CMD_RIGCTL_CONNECT_CFG, ui_cmd_handle_rigctl_connect_cfg},
        {DSD_APP_CMD_UDP_INPUT_CFG, ui_cmd_handle_udp_input_cfg},
    };
    if (!opts || !c) {
        return 0;
    }
    return ui_cmd_apply_handler_table(k_handlers, sizeof k_handlers / sizeof k_handlers[0], opts, state, c);
}

static int
ui_cmd_parse_i32_payload(const struct dsd_app_command* c, int32_t* out) {
    if (!c || !out || c->n < (int)sizeof(*out)) {
        return 0;
    }
    DSD_MEMCPY(out, c->data, sizeof *out);
    return 1;
}

static int
ui_cmd_parse_u32_payload(const struct dsd_app_command* c, uint32_t* out) {
    if (!c || !out || c->n < (int)sizeof(*out)) {
        return 0;
    }
    DSD_MEMCPY(out, c->data, sizeof *out);
    return 1;
}

static int
ui_cmd_parse_double_payload(const struct dsd_app_command* c, double* out) {
    if (!c || !out || c->n < (int)sizeof(*out)) {
        return 0;
    }
    DSD_MEMCPY(out, c->data, sizeof *out);
    return 1;
}

/* Put back the configured decoder settings @p back holds, except the row-scoped options that lead the snapshot
   (dsd_scan_settings: squelch, the tone policy, forcing, the voice gate and the rest up to the decoder set), which stay
   as @p now, the settings in force, has them. A command that goes back on a change it made (ui_revert_analog_entry(),
   ui_restore_receive_settings()) puts back what it switched, not policy it did not. @p back is overwritten. */
static void
ui_restore_settings_keeping_row_policy(dsd_opts* opts, dsd_state* state, dsd_scan_settings* back,
                                       const dsd_scan_settings* now) {
    DSD_MEMCPY(back, now, offsetof(dsd_scan_settings, frame_dstar));
    dsd_scan_settings_restore(back, opts, state);
}

/* Whether a scoped command that found the conventional scanner on (@p was_scanner) leaves the scan scope rather than
   resume it (apply_cmd_leave_scanner_scope()): the scanner is off once the command has run, and no trunk scan owns the
   tuner. The row the scope holds then never runs again, and the session goes back to its configured settings. */
static int
ui_cmd_leaves_scanner_scope(const dsd_opts* opts, int was_scanner) {
    return was_scanner && opts->scanner_mode != 1 && opts->trunk_scan_enabled != 1;
}

/* End every slot's call now and commit it (dsd_event_sync_slot()), its last render reading the carrier it was heard
   on. */
static void
ui_end_calls(dsd_opts* opts, dsd_state* state) {
    const double ended_m = dsd_decode_now_mono_s();
    for (int slot = 0; slot < DSD_CALL_STATE_SLOT_COUNT; slot++) {
        if (dsd_call_state_end(state, (uint8_t)slot, ended_m) > 0) {
            dsd_event_sync_slot(opts, state, (uint8_t)slot);
        }
    }
}

/* Under trunking, a retune to another carrier leaves behind the voice channel a grant sent the receiver to, if any
   (issue #575). The state machine that followed it comes to rest on its control channel without tuning -- the user's
   tune is the move -- as trunk scan hands a carrier back (trunk_scan_release_active_carrier()), and the shared release
   drops trunk_is_tuned and the voice channel frequencies (dsd_engine_release_tuned_call_state()). Left set,
   dsd_opts_trunk_vc_followed() let the next call's LC or VCALL stamp the old voice channel's frequency. NXDN trunking
   keeps no state machine of its own: the shared release is all it owes. @p guard_held says whether the caller holds the
   P25 SM tick guard, which the P25 state machine's release needs. */
static void
ui_leave_followed_assignment(dsd_opts* opts, dsd_state* state, int guard_held) {
    if (opts->trunk_enable != 1) {
        return;
    }
    if (!guard_held) {
        p25_sm_tick_guard_enter();
    }
    p25_sm_ctx_t* p25 = p25_sm_get_ctx();
    if (p25_sm_get_state(p25) == P25_SM_TUNED) {
        p25_sm_abandon_carrier(p25, opts, state, "user-retune");
    }
    dmr_sm_ctx_t* dmr = dmr_sm_get_ctx();
    if (dmr->state == DMR_SM_TUNED) {
        dmr_sm_abandon_carrier(dmr, opts, state, "user-retune");
    }
    dsd_engine_release_tuned_call_state(opts, state);
    if (!guard_held) {
        p25_sm_tick_guard_leave();
    }
}

/* An accepted retune the user asked for to another carrier, which can sync before any no-carrier pass ends the
   reception (issue #575). Under trunking the assignment it followed is left first (ui_leave_followed_assignment()),
   while that voice channel's calls are still active: the release flushes the partial superframe buffered for them,
   which the mixer plays only for an active call on a talkgroup the hold or policy allows. Then every call heard on the
   carrier it leaves ends and commits (ui_end_calls()), with the codes that carrier decoded and the frequencies it named,
   and only then do those go: the codes (dsd_engine_forget_carrier_codes()), and with trunking off the voice
   frequencies its grants named (dsd_engine_forget_untrunked_carrier_state()). A retune within a system under trunking
   -- the return to the control channel, the skip and lockout returns, the P25 control channel picks and candidate
   cycles, a trunked channel cycle -- does not run it: the system's codes stay valid there, as under automatic trunk
   following, and the DMR decode gate they carry keeps dispatching the control channel's bursts. @p guard_held says
   whether the caller holds the P25 SM tick guard. */
static void
ui_leave_carrier(dsd_opts* opts, dsd_state* state, int guard_held) {
    ui_leave_followed_assignment(opts, state, guard_held);
    ui_end_calls(opts, state);
    dsd_engine_forget_carrier_codes(state);
    dsd_engine_forget_untrunked_carrier_state(opts, state);
}

#ifdef USE_RADIO
/* A rollback's toast: @p prefix and the reason @p why, and, when the restart of the input that ran turned the I/Q
   capture off to keep the recording (svc_rtl_restart_recovery_locked(), @p capture_stopped), a note of it if the whole
   fits the toast; the log says it either way. */
static void
ui_set_rollback_toast(dsd_state* state, const char* prefix, const char* why, int capture_stopped) {
    static const char k_capture_stopped[] = "; I/Q capture stopped";
    if (capture_stopped && strlen(prefix) + strlen(why) + sizeof k_capture_stopped <= sizeof state->ui_msg) {
        ui_set_toast(state, 5, "%s%s%s", prefix, why, k_capture_stopped);
    } else {
        ui_set_toast(state, 5, "%s%s", prefix, why);
    }
}

/* The receive input a stream start reads, as a change that rewrites it finds it (issue #578): the device string and
   input type, the rtl_tcp endpoint, the RTL device index, the tuning and front-end settings a start copies, the Airspy
   and SoapySDR settings, and the digital resample policy. These are what Input > Switch source, a config's [input] and
   its hot restart write. @c running says whether that input worked: any input but an RTL-family one (PCM, which no
   switch closes), or an RTL-family one with a stream. A start that fails on the input that replaced it puts this one
   back (ui_restore_radio_input()), so the session never ends up without an input it had. @c stream_running says
   whether a radio stream ran then (state->rtl_ctx): the RTL-family input's own, or one running behind a PCM input,
   which a switch to PCM leaves running, recording. The start stops that stream either way, closing its I/Q capture,
   which the rollback accounts for even when it only puts PCM back. @c failure is the input failure the session had
   latched then, which the failed start can have replaced with its own and which comes back once this input runs again
   (svc_rtl_restart_recovery_locked()). */
typedef struct {
    int running;
    int stream_running;
    dsd_audio_in_type audio_in_type;
    char audio_in_dev[sizeof(((dsd_opts*)0)->audio_in_dev)];
    int rtltcp_enabled;
    char rtltcp_hostname[sizeof(((dsd_opts*)0)->rtltcp_hostname)];
    int rtltcp_portno;
    int rtl_dev_index;
    uint32_t rtlsdr_center_freq;
    int rtl_dsp_bw_khz;
    int rtl_gain_value;
    int rtlsdr_ppm_error;
    int rtl_auto_ppm;
    double rtl_squelch_level;
    int rtl_squelch_mode;
    int rtl_squelch_margin_db;
    int rtl_volume_multiplier;
    int digital_resample_mode;
    dsd_airspy_config airspy;
    int airspy_config_error;
    char soapy_profile[sizeof(((dsd_opts*)0)->soapy_profile)];
    char soapy_stream_format[sizeof(((dsd_opts*)0)->soapy_stream_format)];
    char soapy_antenna[sizeof(((dsd_opts*)0)->soapy_antenna)];
    char soapy_clock[sizeof(((dsd_opts*)0)->soapy_clock)];
    char soapy_settings[sizeof(((dsd_opts*)0)->soapy_settings)];
    char soapy_gains[sizeof(((dsd_opts*)0)->soapy_gains)];
    int soapy_bandwidth_hz;
    dsd_input_failure failure;
} ui_radio_input;

static void
ui_capture_radio_input(const dsd_opts* opts, const dsd_state* state, ui_radio_input* out) {
    DSD_MEMSET(out, 0, sizeof *out);
    if (!opts) {
        return;
    }
    out->stream_running = state && state->rtl_ctx != NULL;
    out->running = opts->audio_in_type != AUDIO_IN_RTL || out->stream_running;
    out->audio_in_type = opts->audio_in_type;
    DSD_MEMCPY(out->audio_in_dev, opts->audio_in_dev, sizeof out->audio_in_dev);
    out->rtltcp_enabled = opts->rtltcp_enabled;
    DSD_MEMCPY(out->rtltcp_hostname, opts->rtltcp_hostname, sizeof out->rtltcp_hostname);
    out->rtltcp_portno = opts->rtltcp_portno;
    out->rtl_dev_index = opts->rtl_dev_index;
    out->rtlsdr_center_freq = opts->rtlsdr_center_freq;
    out->rtl_dsp_bw_khz = opts->rtl_dsp_bw_khz;
    out->rtl_gain_value = opts->rtl_gain_value;
    out->rtlsdr_ppm_error = opts->rtlsdr_ppm_error;
    out->rtl_auto_ppm = opts->rtl_auto_ppm;
    out->rtl_squelch_level = opts->rtl_squelch_level;
    out->rtl_squelch_mode = opts->rtl_squelch_mode;
    out->rtl_squelch_margin_db = opts->rtl_squelch_margin_db;
    out->rtl_volume_multiplier = opts->rtl_volume_multiplier;
    out->digital_resample_mode = opts->digital_resample_mode;
    out->airspy = opts->airspy;
    out->airspy_config_error = opts->airspy_config_error;
    DSD_MEMCPY(out->soapy_profile, opts->soapy_profile, sizeof out->soapy_profile);
    DSD_MEMCPY(out->soapy_stream_format, opts->soapy_stream_format, sizeof out->soapy_stream_format);
    DSD_MEMCPY(out->soapy_antenna, opts->soapy_antenna, sizeof out->soapy_antenna);
    DSD_MEMCPY(out->soapy_clock, opts->soapy_clock, sizeof out->soapy_clock);
    DSD_MEMCPY(out->soapy_settings, opts->soapy_settings, sizeof out->soapy_settings);
    DSD_MEMCPY(out->soapy_gains, opts->soapy_gains, sizeof out->soapy_gains);
    out->soapy_bandwidth_hz = opts->soapy_bandwidth_hz;
    dsd_input_failure_get(&out->failure);
}

/* Put back the input @p in describes. The caller has it running again (svc_rtl_restart_recovery_locked()). */
static void
ui_restore_radio_input(dsd_opts* opts, const ui_radio_input* in) {
    opts->audio_in_type = in->audio_in_type;
    DSD_MEMCPY(opts->audio_in_dev, in->audio_in_dev, sizeof opts->audio_in_dev);
    opts->rtltcp_enabled = in->rtltcp_enabled;
    DSD_MEMCPY(opts->rtltcp_hostname, in->rtltcp_hostname, sizeof opts->rtltcp_hostname);
    opts->rtltcp_portno = in->rtltcp_portno;
    opts->rtl_dev_index = in->rtl_dev_index;
    opts->rtlsdr_center_freq = in->rtlsdr_center_freq;
    opts->rtl_dsp_bw_khz = in->rtl_dsp_bw_khz;
    opts->rtl_gain_value = in->rtl_gain_value;
    opts->rtlsdr_ppm_error = in->rtlsdr_ppm_error;
    opts->rtl_auto_ppm = in->rtl_auto_ppm;
    opts->rtl_squelch_level = in->rtl_squelch_level;
    opts->rtl_squelch_mode = in->rtl_squelch_mode;
    opts->rtl_squelch_margin_db = in->rtl_squelch_margin_db;
    opts->rtl_volume_multiplier = in->rtl_volume_multiplier;
    opts->digital_resample_mode = in->digital_resample_mode;
    opts->airspy = in->airspy;
    opts->airspy_config_error = in->airspy_config_error;
    DSD_MEMCPY(opts->soapy_profile, in->soapy_profile, sizeof opts->soapy_profile);
    DSD_MEMCPY(opts->soapy_stream_format, in->soapy_stream_format, sizeof opts->soapy_stream_format);
    DSD_MEMCPY(opts->soapy_antenna, in->soapy_antenna, sizeof opts->soapy_antenna);
    DSD_MEMCPY(opts->soapy_clock, in->soapy_clock, sizeof opts->soapy_clock);
    DSD_MEMCPY(opts->soapy_settings, in->soapy_settings, sizeof opts->soapy_settings);
    DSD_MEMCPY(opts->soapy_gains, in->soapy_gains, sizeof opts->soapy_gains);
    opts->soapy_bandwidth_hz = in->soapy_bandwidth_hz;
}

/* Put back the configured receive settings @p before holds, the ones a config's [mode], [demod] and [analog] set: the
   decoders, modulation, inversion and slot policy, the analog monitor and its kind, both configured analog widths,
   the output name and the symbol timing, and [analog]'s tone policy. The other row-scoped options that lead the
   snapshot stay as they are now: the sections that set them stay applied. The analog entry cannot do this for a
   config apply: it arms only on a running stream (ui_arm_analog_entry()) and keeps the widths in force
   (ui_revert_analog_entry()). */
static void
ui_restore_receive_settings(dsd_opts* opts, dsd_state* state, const dsd_scan_settings* before) {
    dsd_scan_settings now;
    dsd_scan_settings_capture(opts, state, &now);
    dsd_scan_settings back = *before;
    ui_restore_settings_keeping_row_policy(opts, state, &back, &now);
    opts->analog_tone_set = before->analog_tone_set;
    opts->analog_tone_filter = before->analog_tone_filter;
}

/* What a config's [mode] sets that the decoder reads and the configured receive settings (dsd_scan_settings) do not
   hold (issue #578): the EDACS variant (edacs_ea and edacs_esk, which its EDACS/ProVoice preset clears) and the extra
   LRRP ports dmr_lrrp_ports lists. */
typedef struct {
    int ea_mode;
    unsigned short esk_mask;
    int lrrp_extra_port_count;
    uint16_t lrrp_extra_ports[DSD_LRRP_EXTRA_PORT_MAX];
} ui_cfg_mode_extras;

static void
ui_cfg_capture_mode_extras(const dsd_opts* opts, const dsd_state* state, ui_cfg_mode_extras* out) {
    out->ea_mode = state->ea_mode;
    out->esk_mask = state->esk_mask;
    out->lrrp_extra_port_count = opts->lrrp_extra_port_count;
    DSD_MEMCPY(out->lrrp_extra_ports, opts->lrrp_extra_ports, sizeof out->lrrp_extra_ports);
}

static void
ui_cfg_restore_mode_extras(dsd_opts* opts, dsd_state* state, const ui_cfg_mode_extras* in) {
    state->ea_mode = in->ea_mode;
    state->esk_mask = in->esk_mask;
    opts->lrrp_extra_port_count = in->lrrp_extra_port_count;
    DSD_MEMCPY(opts->lrrp_extra_ports, in->lrrp_extra_ports, sizeof opts->lrrp_extra_ports);
}

/* What a config apply that reopens the running input puts back when the reopened stream does not start (issue #578):
   the input a start reads, the configured receive settings, and the rest of what [mode] sets that the decoder reads.
   Taken once the config has passed its checks, before it is applied: under a scan row, while the scope is suspended,
   so the receive settings are the configured ones. @c was_scanner says whether the conventional scanner ran then,
   which, with what the config leaves, decides whether the scope resumes (ui_cmd_leaves_scanner_scope()). */
typedef struct {
    ui_radio_input input;
    dsd_scan_settings receive;
    ui_cfg_mode_extras mode;
    int was_scanner;
} ui_cfg_rollback;

static void
ui_cfg_capture_rollback(const dsd_opts* opts, const dsd_state* state, ui_cfg_rollback* out) {
    DSD_MEMSET(out, 0, sizeof *out);
    ui_capture_radio_input(opts, state, &out->input);
    dsd_scan_settings_capture(opts, state, &out->receive);
    ui_cfg_capture_mode_extras(opts, state, &out->mode);
    out->was_scanner = opts->scanner_mode == 1;
}

/* The analog monitor the scan row on air runs once its suspended scope resumes (issue #578): the kind an nfm or am row
   runs, or the configured one a row that declares no mode inherits on an analog session, with the width of that kind
   in force then, the row's own (--nfm-bandwidth-hz, --am-bandwidth-hz) or else the configured one, which @p opts holds
   while the scope is suspended. Returns 0, with no kind or width, when no row's scope is suspended, when the command
   leaves that scope rather than resume it (a config that turns off the conventional scanner @p was_scanner says ran:
   ui_cmd_leaves_scanner_scope()), or when the row runs no monitor (a digital row, or one on a digital session: the
   resume asks the front end for a symbol profile, which takes no width). */
static int
ui_scan_row_resumed_monitor(const dsd_opts* opts, const dsd_state* state, int was_scanner, int* kind, int* width_hz) {
    if (!dsd_scan_mode_updating(state) || ui_cmd_leaves_scanner_scope(opts, was_scanner)) {
        return 0;
    }
    const dsd_scan_mode mode = dsd_scan_mode_row(state);
    if (dsd_scan_mode_is_analog(mode)) {
        *kind = dsd_scan_mode_analog_kind(mode);
    } else if (mode == DSD_SCAN_MODE_INHERIT && dsd_opts_is_analog_family(opts)) {
        *kind = opts->analog_demod;
    } else {
        return 0;
    }
    const int am = *kind == DSD_ANALOG_DEMOD_AM;
    const dsd_scan_option_values* row = dsd_scan_mode_row_options(state);
    if (row && (row->present & DSD_SCAN_OPT_BANDWIDTH) != 0U && (row->channel_bw_kind == DSD_ANALOG_DEMOD_AM) == am) {
        *width_hz = row->channel_bw_hz;
    } else {
        *width_hz = am ? opts->analog_am_bandwidth_hz : opts->analog_nfm_bandwidth_hz;
    }
    return 1;
}

/*
 * The end of a config apply's reopen of the running input (@p rc: what the reopened stream's start returned), issue
 * #578. A config apply never loses the input the session ran. When the start failed, refusing the analog width at the
 * rate the device delivered or failing for the device, the reason is taken while the options still describe the input
 * that failed (svc_describe_start_failure()). The reopened stream opens on the configured settings, under a scan row's
 * suspended scope too, so its start holds the configured analog width to the rate it delivers even while a digital row
 * runs, as every reopen holds it (the scan leaves back to that monitor). A start that worked there was therefore never
 * asked for the analog monitor the row puts back when the scope resumes, at the rate the new stream delivers (a
 * SoapySDR or Airspy device sets one no check could know up front): the stream is asked for it now
 * (svc_check_started_stream_analog()), and a stream that refuses it fails the reopen the same way, rather than leave
 * the decoder on the row's analog monitor over a front end that stays on the configured settings. Either way the
 * receive settings and the input @p before holds then go back, with the input's device settings and tuning, the input
 * that ran is running again, on the receive profile its stream ran (under a scan row, the row's, not the configured
 * settings the suspended scope's options hold, which the old rate need not run), without the I/Q capture and with the
 * input failure the session had latched before the apply (svc_rtl_restart_recovery_locked()), and the toast says why
 * ("Config not applied: ..."). Returns -1 then, and the caller fails the apply. The rest of the config stays applied
 * (its output, trunking, logging, alerts, recording and DSP sections, the group list it imported, the environment
 * defaults it set). With no stream running before the apply there is no input to put back: the config stays applied,
 * and the toast says why the input it opened did not start ("Config applied; no input running: ..."), returning -1 all
 * the same, failing the apply as a failed start fails Input > Switch source, or, when that input runs but refuses the
 * scan row, says so ("Config applied; the scan row cannot run: ...") and returns 1, which fails the apply with the new
 * input kept. Returns 0 when the reopened input runs everything asked of it. Caller holds the P25 SM tick guard.
 */
static int
ui_cfg_settle_reopen(dsd_opts* opts, dsd_state* state, int rc, const ui_cfg_rollback* before) {
    char why[128];
    if (rc == 0) {
        int kind = DSD_ANALOG_DEMOD_FM;
        int width_hz = 0;
        if (!ui_scan_row_resumed_monitor(opts, state, before->was_scanner, &kind, &width_hz)
            || svc_check_started_stream_analog(opts, kind, width_hz, why, sizeof why) == 0) {
            return 0;
        }
        if (!before->input.running) {
            ui_set_toast(state, 5, "Config applied; the scan row cannot run: %s", why);
            return 1;
        }
    } else {
        (void)svc_describe_start_failure(opts, why, sizeof why);
        if (!before->input.running) {
            ui_set_toast(state, 5, "Config applied; no input running: %s", why);
            return -1;
        }
    }
    ui_restore_receive_settings(opts, state, &before->receive);
    ui_cfg_restore_mode_extras(opts, state, &before->mode);
    ui_restore_radio_input(opts, &before->input);
    int capture_stopped = 0;
    if (svc_rtl_restart_recovery_locked(opts, state, before->input.stream_running, &before->input.failure,
                                        &capture_stopped)
        != 0) {
        LOG_ERROR("Config: the input it replaced did not restart either; no radio input is running.\n");
    }
    ui_set_rollback_toast(state, "Config not applied: ", why, capture_stopped);
    return -1;
}

/* Input > Switch source > RTL-SDR opens a device at the RTL DSP bandwidth, so an explicit analog width that bandwidth
   cannot filter is refused before the running input is torn down: the new stream's start would refuse it and leave
   none. Input > Switch source > Airspy opens a device that sets its own rate, which its start holds the width in force
   to (none under a scan row that runs a digital protocol), so only the rule every rate shares is asked first (issue
   #578): DSD_NEO_CHANNEL_LPF=0 turns off the filter an explicit width, or the AM default, needs. */
static int
ui_cmd_rtl_enable_input_refused(const dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    if (!opts || !state) {
        return 0;
    }
    char why[128];
    const int check = c->id == DSD_APP_CMD_AIRSPY_ENABLE_INPUT
                          ? svc_check_airspy_input_analog_width(opts, state, why, sizeof why)
                          : svc_check_rtl_input_analog_width(opts, state, why, sizeof why);
    if (check == 0) {
        return 0;
    }
    ui_set_toast(state, 5, "Refused: %s", why);
    return 1;
}

/* Input > Switch source, or a DSP bandwidth change, whose start failed on the new input or settings (issue #578): a
   width the rate the device delivered cannot filter, or a device that did not open. The command never leaves the
   session without the input it had: the reason is taken while the options still describe the input that failed
   (svc_describe_start_failure(), into @p why), then the input that ran before (@p before) is put back and running
   again (svc_rtl_restart_recovery_locked(), which sets @p out_capture_stopped): a PCM input as it was (a switch never
   closed it; a radio stream that ran behind it, recording, stays stopped, and the capture with it), an RTL-family one
   started again without the I/Q capture that would write over its recording, and either with the input failure the
   session had latched before the change. It is the input that ran, so nothing is reset as for a new one
   (ui_input_switched()); under a scan row, the row's profile is asked of the restarted stream once the command has run
   (ui_publish_row_to_started_stream()). Caller holds the P25 SM tick guard. Returns 1 when the start refused its width,
   0 when it failed for another reason. */
static int
ui_radio_start_failed_locked(dsd_opts* opts, dsd_state* state, const ui_radio_input* before, char* why, size_t why_size,
                             int* out_capture_stopped) {
    *out_capture_stopped = 0;
    const int refused = svc_describe_start_failure(opts, why, why_size);
    if (before->running) {
        ui_restore_radio_input(opts, before);
        if (svc_rtl_restart_recovery_locked(opts, state, before->stream_running, &before->failure, out_capture_stopped)
            != 0) {
            LOG_ERROR("The input the change replaced did not restart either; no radio input is running.\n");
        }
    }
    return refused;
}

/* The rate the symbol timing is in under a PCM input, for the radio output's rescale to start from (issue #634); 0
   under the radio. */
static int
ui_pcm_timing_rate_hz(const dsd_opts* opts) {
    return opts->audio_in_type != AUDIO_IN_RTL ? dsd_opts_current_input_timing_rate(opts) : 0;
}

/* The device a Switch source radio row opens: the Airspy row's own spec, or for the RTL-SDR row the radio a switch to
   PCM left (issue #634), else an RTL-SDR (svc_rtl_row_destination()). */
static void
ui_radio_row_set_device(dsd_opts* opts, int cmd_id) {
    if (cmd_id == DSD_APP_CMD_AIRSPY_ENABLE_INPUT) {
        DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "airspy%s%s",
                     opts->airspy.serial[0] ? ":serial=" : "", opts->airspy.serial);
        opts->rtltcp_enabled = 0;
        return;
    }
    char destination[sizeof opts->audio_in_dev];
    svc_rtl_row_destination(opts, destination, sizeof destination);
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", destination);
    /* The backend flag follows the device, as a config's input does: an Airspy run since cleared it, and the retune
       settling reads it (an rtl_tcp stream is buffered). */
    opts->rtltcp_enabled = dsd_opts_audio_in_dev_is_rtltcp_spec(destination) ? 1 : 0;
}

/* A radio row's stream replaced the input: the PCM input it replaced is closed, as an input switch closes the input it
   replaces (its file, or its UDP socket and receive thread with the port); the radio output's rescale starts from the
   rate the timing is in; and the engine ends the reception before it decodes the new stream. */
static void
ui_radio_row_took_over(dsd_opts* opts, dsd_state* state, int pcm_timing_rate_hz) {
    closeAudioInDevice(opts);
    if (pcm_timing_rate_hz > 0) {
        dsd_symbol_note_timing_rate(pcm_timing_rate_hz);
    }
    state->input_boundary = 1;
}

/* Input > Switch source > RTL-SDR or Airspy. The watchdog reads the input, and may retune it, while it holds the P25 SM
   tick guard, so the switch holds that guard from the copy of the input it would put back, through its rewrite of the
   input and its start, and, when the start fails, on through putting back the input that ran and starting it again
   (issue #578): nothing the watchdog reads is read or written outside it, as a config apply's reopen holds it
   throughout. A copy taken before the guard could miss a retune the watchdog completed in between, which putting it
   back would undo while the trunking state follows the retune. */
static int
ui_cmd_handle_rtl_enable_input(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    if (opts && state && ui_input_mode_refused(opts, state)) {
        return UI_CMD_APPLY_UNSUPPORTED;
    }
    if (ui_cmd_rtl_enable_input_refused(opts, state, c)) {
        return UI_CMD_APPLY_FAILED;
    }
    if (!opts || !state) {
        return UI_CMD_APPLY_COMPLETED;
    }
    char why[128];
    why[0] = '\0';
    int refused = 0;
    int capture_stopped = 0;
    int output_failed = 0;
    p25_sm_tick_guard_enter();
    ui_radio_input before;
    ui_capture_radio_input(opts, state, &before);
    const int pcm_timing_rate_hz = ui_pcm_timing_rate_hz(opts);
    ui_radio_row_set_device(opts, c->id);
    const int rc = svc_rtl_enable_input_locked(opts, state);
    const int failed = rc != 0 && !ui_rc_is_not_supported(rc);
    if (failed) {
        refused = ui_radio_start_failed_locked(opts, state, &before, why, sizeof why, &capture_stopped);
    } else if (rc == 0) {
        ui_radio_row_took_over(opts, state, pcm_timing_rate_hz);
        /* The output follows the new input under the guard too: the watchdog may flush audio into the stream a
           reconfigure closes (issue #634). */
        output_failed = ui_input_switched(opts, state) != 0;
    }
    p25_sm_tick_guard_leave();

    int result = ui_cmd_apply_status_from_service_rc(rc);
    if (rc == 0) {
        if (!output_failed) {
            ui_set_toast(state, 3, "Applied: %s input enabled",
                         c->id == DSD_APP_CMD_AIRSPY_ENABLE_INPUT ? "Airspy" : "RTL");
        } else {
            result = UI_CMD_APPLY_FAILED;
        }
    } else if (!failed) {
        ui_set_toast(state, 3, "Unsupported: active radio backend cannot enable RTL input");
    } else {
        ui_set_rollback_toast(state, refused ? "Refused: " : "Failed: ", why, capture_stopped);
    }
    return result;
}

static int
ui_cmd_handle_rtl_restart(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    (void)c;
    int result = UI_CMD_APPLY_COMPLETED;
    if (state) {
        int rc = svc_rtl_restart(opts, state);
        result = ui_cmd_apply_status_from_service_rc(rc);
        if (rc == 0) {
            ui_set_toast(state, 3, "Applied: RTL stream restarted");
        } else if (ui_rc_is_not_supported(rc)) {
            ui_set_toast(state, 3, "Unsupported: active radio backend cannot restart stream");
        } else {
            ui_set_toast(state, 4, "Failed: RTL stream restart");
        }
    }
    return result;
}

static int
ui_cmd_handle_rtl_set_dev(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    int32_t v = 0;
    int result = UI_CMD_APPLY_COMPLETED;
    if (state && ui_cmd_parse_i32_payload(c, &v)) {
        int rc = svc_rtl_set_dev_index(opts, state, v);
        result = ui_cmd_apply_status_from_service_rc(rc);
        if (rc == 0) {
            ui_set_toast(state, 3, "Applied: RTL device index -> %d", (int)v);
        } else if (ui_rc_is_not_supported(rc)) {
            ui_set_toast(state, 3, "Unsupported: device index is not available for current radio source");
        } else {
            ui_set_toast(state, 4, "Failed: RTL device index -> %d", (int)v);
        }
    }
    return result;
}

static int
apply_cmd_io_and_import_rtl_a(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    static const struct dsd_app_command_handler_entry k_handlers[] = {
        {DSD_APP_CMD_AIRSPY_ENABLE_INPUT, ui_cmd_handle_rtl_enable_input},
        {DSD_APP_CMD_RTL_ENABLE_INPUT, ui_cmd_handle_rtl_enable_input},
        {DSD_APP_CMD_RTL_RESTART, ui_cmd_handle_rtl_restart},
        {DSD_APP_CMD_RTL_SET_DEV, ui_cmd_handle_rtl_set_dev},
    };
    if (!opts || !c) {
        return 0;
    }
    return ui_cmd_apply_handler_table(k_handlers, sizeof k_handlers / sizeof k_handlers[0], opts, state, c);
}

static int cc_has_active_p25_context(const dsd_opts* opts, const dsd_state* state);

static int
manual_frequency_selects_p25_cc(const dsd_opts* opts, const dsd_state* state) {
    if (opts->trunk_enable != 1 || (opts->frame_p25p1 != 1 && opts->frame_p25p2 != 1)) {
        return 0;
    }
    if (cc_has_active_p25_context(opts, state)) {
        return 1;
    }
    if (state->synctype != DSD_SYNC_NONE || state->lastsynctype != DSD_SYNC_NONE) {
        return 0;
    }
    // A learned CC type survives P25 sync loss; noCarrier retires it after
    // another trunking protocol takes over. The shared frequency alone is not
    // P25 evidence, since DMR/NXDN/EDACS also use that anchor.
    if (state->p25_cc_freq > 0 && (state->p25_cc_is_tdma == 0 || state->p25_cc_is_tdma == 1)) {
        return 1;
    }
    const dsdneoUserDecodeMode mode = dsd_infer_decode_mode_preset_exact(opts);
    return mode == DSDCFG_MODE_P25P1 || mode == DSDCFG_MODE_P25P2;
}

static int
ui_cmd_handle_p25_cc_selection(dsd_opts* opts, dsd_state* state, uint32_t hz) {
    const dsd_trunk_tune_result result = p25_sm_select_control_channel(p25_sm_get_ctx(), opts, state, (long)hz);
    if (dsd_trunk_tune_result_is_ok(result)) {
        ui_set_toast(state, 3, "%s: P25 control channel -> %u Hz",
                     result == DSD_TRUNK_TUNE_RESULT_PENDING ? "Accepted (pending)" : "Applied", hz);
        return UI_CMD_APPLY_COMPLETED;
    }
    ui_set_toast(state, 4, "%s: P25 control channel -> %u Hz",
                 result == DSD_TRUNK_TUNE_RESULT_DEFERRED ? "Deferred; retry" : "Failed", hz);
    return UI_CMD_APPLY_FAILED;
}

static int
ui_cmd_leave_typed_scan_after_tune(dsd_opts* opts, dsd_state* state, int result) {
    if ((result != 0 && result != RTL_STREAM_TUNE_TIMEOUT) || opts->scanner_mode != 1
        || !dsd_channel_modes_present(state)) {
        return 0;
    }
    p25_sm_tick_guard_enter();
    (void)svc_leave_channel_scan(opts, state);
    dsd_scan_keys_leave(state);
    opts->scanner_mode = 0;
    state->lcn_freq_roll = 0;
    p25_sm_tick_guard_leave();
    return 1;
}

/* The frequency entry's refusals, each toasted: a replay, a trunk scan owning the tuner, a frequency out of range.
   Returns 1 with the command status in *status when the entry is refused. */
static int
ui_cmd_rtl_set_freq_refused(const dsd_opts* opts, dsd_state* state, uint32_t v, int* status) {
    if (ui_cmd_refuse_replay_tune(opts, state)) {
        *status = UI_CMD_APPLY_FAILED;
        return 1;
    }
    if (opts->trunk_scan_enabled) {
        ui_set_toast(state, 3, "Trunk scan active: frequency control disabled");
        *status = UI_CMD_APPLY_FAILED;
        return 1;
    }
    if (v == 0
#if LONG_MAX < UINT32_MAX
        || v > (uint32_t)LONG_MAX
#endif
    ) {
        ui_set_toast(state, 3, "Invalid frequency");
        *status = UI_CMD_APPLY_INVALID_PAYLOAD;
        return 1;
    }
    return 0;
}

/* The toast a frequency entry's tune leaves, from svc_rtl_set_freq()'s result. */
static void
ui_cmd_rtl_set_freq_toast(dsd_state* state, int rc, uint32_t v, int stop_scanner) {
    if (rc == 0) {
        ui_set_toast(state, 3, "Applied: RTL frequency -> %u Hz%s", v, stop_scanner ? " (scanner stopped)" : "");
    } else if (rc == RTL_STREAM_TUNE_TIMEOUT) {
        ui_set_toast(state, 3, "Accepted: RTL frequency -> %u Hz (pending)%s", v,
                     stop_scanner ? " (scanner stopped)" : "");
    } else if (ui_rc_is_not_supported(rc)) {
        ui_set_toast(state, 3, "Unsupported: frequency control not available on active backend");
    } else {
        ui_set_toast(state, 4, "Failed: RTL frequency -> %u Hz", v);
    }
}

static int
ui_cmd_handle_rtl_set_freq(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    uint32_t v = 0;
    int result = UI_CMD_APPLY_COMPLETED;
    if (state && ui_cmd_parse_u32_payload(c, &v)) {
        int refused_status = UI_CMD_APPLY_FAILED;
        if (ui_cmd_rtl_set_freq_refused(opts, state, v, &refused_status)) {
            return refused_status;
        }
        if (manual_frequency_selects_p25_cc(opts, state)) {
            return ui_cmd_handle_p25_cc_selection(opts, state, v);
        }
        int rc = svc_rtl_set_freq(opts, state, v);
        result = ui_cmd_apply_status_from_tune_rc(rc);
        if (rc == 0 || rc == RTL_STREAM_TUNE_TIMEOUT) {
            /* A new channel: the tone heard on the old one goes now, not when the analog tap
               next notices the stream moved (issue #522), and so do its calls and the codes it
               decoded (issue #575). */
            dsd_analog_rx_reset(state);
            ui_leave_carrier(opts, state, 0);
        }
        const int stop_scanner = ui_cmd_leave_typed_scan_after_tune(opts, state, rc);
        ui_cmd_rtl_set_freq_toast(state, rc, v, stop_scanner);
    }
    return result;
}

/* Defined further down with the manual-tuning helpers; tap-to-tune needs the
 * same call-state teardown the manual return-to-CC path uses. */
static void reset_call_tracking(dsd_opts* opts, dsd_state* state, int clear_trunk_vc);

/*
 * Live retune from a spectrum tap.
 *
 * Kept separate from ui_cmd_handle_rtl_set_freq() on purpose. That one is the
 * settings-menu tune, which selects a CC during P25 trunk following; here the
 * tune is a navigation gesture, so it (a) evaluates the tuner-ownership gate at drain
 * time on the authoritative live opts rather than trusting the frontend's
 * affordance, and (b) drops the stale auto-modulation votes and per-slot call
 * state that would otherwise slow or corrupt re-acquisition in decode mode
 * "auto".
 */
static int
ui_cmd_handle_manual_tune(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    uint32_t v = 0;
    int result = UI_CMD_APPLY_COMPLETED;
    if (!state || !ui_cmd_parse_u32_payload(c, &v)) {
        return result;
    }
    /* Every refusal below is a failed command with its reason toasted, as the frequency entry's are
     * (ui_cmd_handle_rtl_set_freq()). The replay's comes ahead of the owner gates: their toasts point at a release,
     * which a replay refuses too. */
    if (ui_cmd_refuse_replay_tune(opts, state)) {
        return UI_CMD_APPLY_FAILED;
    }
    /* Either automatic controller owns the tuner. Trunking parks on a control
     * channel; conventional scanner mode steps the channel map on its own once
     * trunk_hangtime expires (no_carrier_step_scanner_mode_if_needed()), so a
     * tap accepted under it would be silently undone seconds later — worse than
     * a refusal, because the toast would have claimed it worked. */
    if (opts->trunk_enable) {
        ui_set_toast(state, 3, "Trunking active: tap-to-tune disabled");
        return UI_CMD_APPLY_FAILED;
    }
    if (opts->scanner_mode) {
        ui_set_toast(state, 3, "Scanner active: tap-to-tune disabled");
        return UI_CMD_APPLY_FAILED;
    }
    /* The third owner, and the one a release cannot clear (see apply_tuner_release):
     * dsd_trunk_scan_hook_tick() runs on every engine iteration and steps targets on
     * its own, so a tap accepted under it is undone within a dwell -- and it is the
     * owner engine_trunk_tuning_owner_active() actually gates dispatch on. */
    if (opts->trunk_scan_enabled) {
        ui_set_toast(state, 3, "Trunk scan active: tap-to-tune disabled");
        return UI_CMD_APPLY_FAILED;
    }
    int rc = svc_rtl_set_freq(opts, state, v);
    result = ui_cmd_apply_status_from_tune_rc(rc);
    if (rc == 0 || rc == RTL_STREAM_TUNE_TIMEOUT) {
        /* Only after the tune is accepted, matching how trunk_tuning.c and the
         * manual return-to-CC path order this — never on the failure path. */
        dsd_frame_sync_reset_mod_state();
        dsd_analog_rx_reset(state);
        ui_leave_carrier(opts, state, 0); // the old carrier's calls commit, then its codes go (issue #575)
        reset_call_tracking(opts, state, 1);
        if (rc == 0) {
            ui_set_toast(state, 3, "Applied: tuned -> %u Hz", v);
        } else {
            ui_set_toast(state, 3, "Accepted: tuned -> %u Hz (pending)", v);
        }
    } else if (ui_rc_is_not_supported(rc)) {
        ui_set_toast(state, 3, "Unsupported: frequency control not available on active backend");
    } else {
        ui_set_toast(state, 4, "Failed: tune -> %u Hz", v);
    }
    return result;
}

static int
ui_cmd_handle_rtl_set_gain(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    int32_t v = 0;
    int result = UI_CMD_APPLY_COMPLETED;
    if (state && ui_cmd_parse_i32_payload(c, &v)) {
        int rc = svc_rtl_set_gain(opts, state, v);
        result = ui_cmd_apply_status_from_service_rc(rc);
        if (rc == 0) {
            dsd_app_rtl_gain_view view;
            char notice[96];
            (void)dsd_app_rtl_gain_view_get(opts, state, &view);
            (void)dsd_app_rtl_gain_view_edit_notice(&view, notice, sizeof notice);
            ui_set_toast(state, 3, "%s", notice);
        } else if (ui_rc_is_not_supported(rc)) {
            ui_set_toast(state, 3, "Unsupported: gain control not available on active backend");
        } else {
            ui_set_toast(state, 4, "Failed: RTL gain -> %d", (int)v);
        }
    }
    return result;
}

static int
ui_cmd_handle_rtl_set_ppm(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    int32_t v = 0;
    int result = UI_CMD_APPLY_COMPLETED;
    if (state && ui_cmd_parse_i32_payload(c, &v)) {
        int rc = rtl_stream_request_ppm(opts, v);
        result = ui_cmd_apply_status_from_service_rc(rc);
        if (rc == 0) {
            ui_set_toast(state, 3, "Applied: RTL PPM -> %d", rtl_stream_get_requested_ppm(opts));
        } else if (ui_rc_is_not_supported(rc)) {
            ui_set_toast(state, 3, "Unsupported: PPM correction not available on active backend");
        } else {
            ui_set_toast(state, 4, "Failed: RTL PPM update");
        }
    }
    return result;
}

static int
ui_cmd_handle_airspy_set(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    if (!opts || !state || c->n != sizeof(dsd_app_airspy_setting_payload)) {
        return UI_CMD_APPLY_INVALID_PAYLOAD;
    }
    dsd_app_airspy_setting_payload edit;
    DSD_MEMCPY(&edit, c->data, sizeof edit);
    if (!memchr(edit.key, '\0', sizeof edit.key) || !memchr(edit.value, '\0', sizeof edit.value)) {
        return UI_CMD_APPLY_INVALID_PAYLOAD;
    }
    dsd_airspy_config config = opts->airspy;
    if (dsd_airspy_config_set(&config, edit.key, edit.value) != 0) {
        return UI_CMD_APPLY_INVALID_PAYLOAD;
    }
    int capture_stopped = 0;
    const int rc = svc_airspy_apply(opts, state, &config, &capture_stopped);
    if (rc == 0) {
        ui_set_toast(state, 3, "Applied: Airspy setting");
    } else {
        /* A reopen whose start failed started the settings it replaced again without the I/Q capture (issue #578). */
        ui_set_rollback_toast(state, "Failed: ", "Airspy setting", capture_stopped);
    }
    return ui_cmd_apply_status_from_service_rc(rc);
}

static int
apply_cmd_io_and_import_rtl_b(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    static const struct dsd_app_command_handler_entry k_handlers[] = {
        {DSD_APP_CMD_RTL_SET_FREQ, ui_cmd_handle_rtl_set_freq}, {DSD_APP_CMD_MANUAL_TUNE, ui_cmd_handle_manual_tune},
        {DSD_APP_CMD_AIRSPY_SET, ui_cmd_handle_airspy_set},     {DSD_APP_CMD_RTL_SET_GAIN, ui_cmd_handle_rtl_set_gain},
        {DSD_APP_CMD_RTL_SET_PPM, ui_cmd_handle_rtl_set_ppm},
    };
    if (!opts || !c) {
        return 0;
    }
    return ui_cmd_apply_handler_table(k_handlers, sizeof k_handlers / sizeof k_handlers[0], opts, state, c);
}

/* RTL_SET_BW. The bandwidth is held to the widths in use first (svc_rtl_set_bandwidth_locked()), refused with nothing
   changed; one it takes reopens the device, whose start can still fail (issue #578): a width the rate an Airspy or
   SoapySDR device delivers at the new bandwidth cannot filter, which nothing before the reopen knows, or a device
   that does not open. That start never leaves the session without the input it had: the bandwidth, and the input it
   ran, go back and start again (ui_radio_start_failed_locked()), and the toast says why ("Refused: " for a width,
   "Failed: " otherwise). The watchdog reads the input under the P25 SM tick guard, so the command holds it from the
   copy of the input it would put back through the reopen and any rollback, as Input > Switch source does. */
static int
ui_cmd_handle_rtl_set_bw(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    int32_t v = 0;
    if (!opts || !state || !ui_cmd_parse_i32_payload(c, &v)) {
        return UI_CMD_APPLY_COMPLETED;
    }
    char why[128];
    int refused = 0;
    int capture_stopped = 0;
    p25_sm_tick_guard_enter();
    ui_radio_input before;
    ui_capture_radio_input(opts, state, &before);
    const int rc = svc_rtl_set_bandwidth_locked(opts, state, v, why, sizeof why);
    /* A bandwidth refused before anything changed says why; a start that failed has said nothing yet. */
    const int start_failed = rc != 0 && !ui_rc_is_not_supported(rc) && why[0] == '\0';
    if (start_failed) {
        refused = ui_radio_start_failed_locked(opts, state, &before, why, sizeof why, &capture_stopped);
    }
    p25_sm_tick_guard_leave();

    if (rc == 0) {
        ui_set_toast(state, 3, "Applied: RTL DSP BW -> %d kHz", (int)opts->rtl_dsp_bw_khz);
    } else if (ui_rc_is_not_supported(rc)) {
        ui_set_toast(state, 3, "Unsupported: bandwidth control not available on active backend");
    } else if (start_failed) {
        ui_set_rollback_toast(state, refused ? "Refused: " : "Failed: ", why, capture_stopped);
    } else {
        ui_set_toast(state, 5, "Refused: %s", why);
    }
    return ui_cmd_apply_status_from_service_rc(rc);
}

static int
ui_cmd_handle_rtl_set_vol_mult(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    int32_t v = 0;
    int result = UI_CMD_APPLY_COMPLETED;
    if (state && ui_cmd_parse_i32_payload(c, &v)) {
        int rc = svc_rtl_set_volume_mult(opts, v);
        result = ui_cmd_apply_status_from_service_rc(rc);
        if (rc == 0) {
            ui_set_toast(state, 3, "Applied: RTL monitor gain -> %dX", (int)opts->rtl_volume_multiplier);
        } else if (ui_rc_is_not_supported(rc)) {
            ui_set_toast(state, 3, "Unsupported: monitor gain not available on active backend");
        } else {
            ui_set_toast(state, 4, "Failed: RTL monitor gain update");
        }
    }
    return result;
}

static int
apply_cmd_io_and_import_rtl_c(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    static const struct dsd_app_command_handler_entry k_handlers[] = {
        {DSD_APP_CMD_RTL_SET_BW, ui_cmd_handle_rtl_set_bw},
        {DSD_APP_CMD_RTL_SET_VOL_MULT, ui_cmd_handle_rtl_set_vol_mult},
    };
    if (!opts || !c) {
        return 0;
    }
    return ui_cmd_apply_handler_table(k_handlers, sizeof k_handlers / sizeof k_handlers[0], opts, state, c);
}

static int
ui_cmd_handle_rtl_set_bias_tee(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    int32_t on = 0;
    int result = UI_CMD_APPLY_COMPLETED;
    if (state && ui_cmd_parse_i32_payload(c, &on)) {
        int rc = svc_rtl_set_bias_tee(opts, state, on);
        result = ui_cmd_apply_status_from_service_rc(rc);
        if (rc == 0) {
            ui_set_toast(state, 3, "Applied: RTL bias tee -> %s", on ? "On" : "Off");
        } else if (ui_rc_is_not_supported(rc)) {
            ui_set_toast(state, 3, "Unsupported: bias tee control not available on active backend");
        } else {
            ui_set_toast(state, 4, "Failed: RTL bias tee update");
        }
    }
    return result;
}

static int
ui_cmd_handle_rtltcp_set_autotune(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    int32_t on = 0;
    int result = UI_CMD_APPLY_COMPLETED;
    if (state && ui_cmd_parse_i32_payload(c, &on)) {
        int rc = svc_rtltcp_set_autotune(opts, state, on);
        result = ui_cmd_apply_status_from_service_rc(rc);
        if (rc == 0) {
            ui_set_toast(state, 3, "Applied: RTL-TCP adaptive networking -> %s", on ? "On" : "Off");
        } else if (ui_rc_is_not_supported(rc)) {
            ui_set_toast(state, 3, "Unsupported: RTL-TCP autotune not available on active backend");
        } else {
            ui_set_toast(state, 4, "Failed: RTL-TCP adaptive networking update");
        }
    }
    return result;
}

static int
ui_cmd_handle_rtl_set_auto_ppm(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    int32_t on = 0;
    int result = UI_CMD_APPLY_COMPLETED;
    if (state && ui_cmd_parse_i32_payload(c, &on)) {
        int rc = svc_rtl_set_auto_ppm(opts, state, on);
        result = ui_cmd_apply_status_from_service_rc(rc);
        if (rc == 0) {
            ui_set_toast(state, 3, "Applied: Auto PPM -> %s", on ? "On" : "Off");
        } else if (ui_rc_is_not_supported(rc)) {
            ui_set_toast(state, 3, "Unsupported: Auto PPM not available on active backend");
        } else {
            ui_set_toast(state, 4, "Failed: Auto PPM update");
        }
    }
    return result;
}

static int
apply_cmd_io_and_import_rtl_d(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    static const struct dsd_app_command_handler_entry k_handlers[] = {
        {DSD_APP_CMD_RTL_SET_BIAS_TEE, ui_cmd_handle_rtl_set_bias_tee},
        {DSD_APP_CMD_RTLTCP_SET_AUTOTUNE, ui_cmd_handle_rtltcp_set_autotune},
        {DSD_APP_CMD_RTL_SET_AUTO_PPM, ui_cmd_handle_rtl_set_auto_ppm},
    };
    if (!opts || !c) {
        return 0;
    }
    return ui_cmd_apply_handler_table(k_handlers, sizeof k_handlers / sizeof k_handlers[0], opts, state, c);
}
#endif

/* The squelch commands run on every input, so they sit outside the radio guard: audio input takes a level or the
   noise squelch (issue #628). */
/* The squelch edit's toast, for either command: the setting stored, and whether a scan row still wins. */
static void
ui_cmd_toast_squelch_edit(const dsd_opts* opts, dsd_state* state, int rc) {
    if (rc == 0) {
        dsd_app_squelch_view view;
        char notice[96];
        (void)dsd_app_squelch_view_get(opts, state, &view);
        (void)dsd_app_squelch_view_edit_notice(&view, notice, sizeof notice);
        ui_set_toast(state, 3, "%s", notice);
    } else if (rc == SVC_SQL_NOISE_NEEDS_FM && dsd_squelch_input_kind(opts) == DSD_SQUELCH_INPUT_AUDIO) {
        /* Auto needs a radio input too: AM audio takes a level. */
        ui_set_toast(state, 4, "Refused: the noise squelch needs an FM channel; AM audio takes a level");
    } else if (rc == SVC_SQL_NOISE_NEEDS_FM) {
        ui_set_toast(state, 4, "Refused: the noise squelch needs an FM channel; AM takes auto");
    } else if (rc == SVC_SQL_AUTO_NEEDS_RADIO) {
        ui_set_toast(state, 4, "Refused: the auto squelch needs a radio input; audio input takes a level or noise");
    } else if (ui_rc_is_not_supported(rc)) {
        ui_set_toast(state, 3, "Unsupported: squelch control not available on active backend");
    } else {
        ui_set_toast(state, 4, "Failed: squelch update");
    }
}

static int
ui_cmd_handle_rtl_set_sql_db(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    double d = 0.0;
    int result = UI_CMD_APPLY_COMPLETED;
    if (state && ui_cmd_parse_double_payload(c, &d)) {
        int rc = svc_rtl_set_sql_db(opts, state, d);
        result = ui_cmd_apply_status_from_service_rc(rc);
        /* Report the threshold that was stored rather than the number that was asked for: a request of 0 dB switches
           the squelch off, and echoing "0.0 dB" would describe a gate at full scale instead. The command edits the
           configured default, so when a scan row overrides the squelch the notice says the row still wins. */
        ui_cmd_toast_squelch_edit(opts, state, rc);
    }
    return result;
}

static int
ui_cmd_handle_rtl_set_sql_setting(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    dsd_app_squelch_setting_payload payload;
    if (!state || c->n != sizeof payload) {
        return UI_CMD_APPLY_COMPLETED;
    }
    DSD_MEMCPY(&payload, c->data, sizeof payload);
    dsd_squelch_setting setting;
    setting.mode = dsd_squelch_mode_or_level(payload.mode);
    setting.margin_db = payload.margin_db;
    setting.level = payload.level;
    const int rc = svc_rtl_set_sql_setting(opts, state, &setting);
    ui_cmd_toast_squelch_edit(opts, state, rc);
    return ui_cmd_apply_status_from_service_rc(rc);
}

static int
apply_cmd_io_and_import_squelch(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    static const struct dsd_app_command_handler_entry k_handlers[] = {
        {DSD_APP_CMD_RTL_SET_SQL_DB, ui_cmd_handle_rtl_set_sql_db},
        {DSD_APP_CMD_RTL_SET_SQL_SETTING, ui_cmd_handle_rtl_set_sql_setting},
    };
    if (!opts || !c) {
        return 0;
    }
    return ui_cmd_apply_handler_table(k_handlers, sizeof k_handlers / sizeof k_handlers[0], opts, state, c);
}

/* Whether the command runs under the P25 SM tick guard (apply_cmd()), which the services that ask a rigctl peer for a
   passband take otherwise (svc_rigctl_follow_passband(), issue #621). */
static int command_writes_policy_store(const struct dsd_app_command* c);

/* DSD_APP_CMD_RIGCTL_SET_MOD_BW: -B, asked of a rigctl peer at once while it is the passband the FM monitor on audio
   input asks for (svc_apply_rigctl_setmod_bw(), issue #621); a refusal puts the previous -B back. */
static int
ui_cmd_handle_rigctl_set_mod_bw(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    int32_t hz = 0;
    if (!state || !ui_cmd_parse_i32_payload(c, &hz)) {
        return UI_CMD_APPLY_COMPLETED;
    }
    char why[96];
    if (svc_apply_rigctl_setmod_bw(opts, state, (int)hz, command_writes_policy_store(c), why, sizeof why) != 0) {
        ui_set_toast(state, 5, "Refused: %s", why);
        return UI_CMD_APPLY_FAILED;
    }
    ui_set_toast(state, 3, "Applied: Rigctl setmod BW -> %d Hz", opts->setmod_bw);
    return UI_CMD_APPLY_COMPLETED;
}

static int
ui_cmd_handle_tg_hold_set(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    uint32_t tg = 0;
    (void)opts;
    if (state && ui_cmd_parse_u32_payload(c, &tg)) {
        svc_set_tg_hold(state, tg);
        ui_set_toast(state, 3, "Applied: TG Hold -> %u", tg);
    }
    return 1;
}

static int
ui_cmd_handle_hangtime_set(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    double s = 0.0;
    if (state && ui_cmd_parse_double_payload(c, &s)) {
        svc_set_hangtime(opts, s);
        ui_set_toast(state, 3, "Applied: Hangtime -> %.3f s", opts->trunk_hangtime);
    }
    return 1;
}

static int
ui_cmd_handle_slot_pref_set(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    int32_t p = 0;
    if (state && ui_cmd_parse_i32_payload(c, &p)) {
        svc_set_slot_pref(opts, p);
        const char* label = (opts->slot_preference == 0) ? "Slot 1" : (opts->slot_preference == 1) ? "Slot 2" : "Auto";
        ui_set_toast(state, 3, "Applied: Slot preference -> %s", label);
    }
    return 1;
}

static int
ui_cmd_handle_slots_onoff_set(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    int32_t m = 0;
    if (state && ui_cmd_parse_i32_payload(c, &m)) {
        svc_set_slots_onoff(opts, m);
        ui_set_toast(state, 3, "Applied: Slot mask -> %d", (opts->slot1_on ? 1 : 0) | (opts->slot2_on ? 2 : 0));
    }
    return 1;
}

static int
ui_cmd_handle_scan_voice_only_set(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    int32_t on = 0;
    if (state && ui_cmd_parse_i32_payload(c, &on)) {
        svc_set_scan_voice_only(opts, on);
        ui_set_toast(state, 3, "Applied: Voice-only scan -> %s", opts->scan_voice_only ? "On" : "Off");
    }
    return 1;
}

static int
ui_cmd_handle_scan_voice_qualify_ms_set(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    int32_t ms = 0;
    if (state && ui_cmd_parse_i32_payload(c, &ms)) {
        svc_set_scan_voice_qualify_ms(opts, ms);
        ui_set_toast(state, 3, "Applied: Voice qualify -> %d ms", opts->scan_voice_qualify_ms);
    }
    return 1;
}

static int
ui_cmd_handle_scan_voice_hold_ms_set(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    int32_t ms = 0;
    if (state && ui_cmd_parse_i32_payload(c, &ms)) {
        svc_set_scan_voice_hold_ms(opts, ms);
        ui_set_toast(state, 3, "Applied: Voice hold -> %d ms", opts->scan_voice_hold_ms);
    }
    return 1;
}

/* DSD_APP_CMD_NFM_BANDWIDTH_SET (issue #525) and DSD_APP_CMD_AM_BANDWIDTH_SET (issue #524): the configured width of
   analog @p kind, held to the front end while the configured preset runs that kind and handed to a running monitor of
   it, or asked of a rigctl peer that demodulates audio input while the monitor of that kind runs (issue #621,
   svc_set_analog_bandwidth()); refused with the reason, changing nothing, otherwise. */
static int
ui_cmd_handle_analog_bandwidth_set(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c, int kind) {
    int32_t hz = 0;
    if (!state || !ui_cmd_parse_i32_payload(c, &hz)) {
        return UI_CMD_APPLY_INVALID_PAYLOAD;
    }
    char why[128];
    if (svc_set_analog_bandwidth(opts, state, kind, (int)hz, command_writes_policy_store(c), why, sizeof why) != 0) {
        ui_set_toast(state, 5, "Refused: %s", why);
        return UI_CMD_APPLY_FAILED;
    }
    /* The command edits the configured width of its kind: while a scan row sets its own, the notice says the row still
       wins. */
    char notice[96];
    (void)dsd_app_analog_width_edit_notice(opts, state, kind, notice, sizeof notice);
    ui_set_toast(state, 3, "%s", notice);
    return UI_CMD_APPLY_COMPLETED;
}

static int
ui_cmd_handle_nfm_bandwidth_set(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    return ui_cmd_handle_analog_bandwidth_set(opts, state, c, DSD_ANALOG_DEMOD_FM);
}

static int
ui_cmd_handle_am_bandwidth_set(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    return ui_cmd_handle_analog_bandwidth_set(opts, state, c, DSD_ANALOG_DEMOD_AM);
}

/* The prefix of a refused tone-filter edit's toast, before the parser's reason. */
#define UI_TONE_FILTER_REFUSED "Refused: tone filter: "

_Static_assert(sizeof(((dsd_app_tone_filter_payload*)0)->list) == (size_t)DSD_TONE_LIST_TEXT_MAX + 1U,
               "the tone-filter payload holds exactly the longest list the parser reads");
/* The tone-filter toasts land in dsd_state::ui_msg whole: an edit's notice, and a refusal with the longest reason. */
_Static_assert(sizeof(((dsd_state*)0)->ui_msg) >= (size_t)DSD_APP_TONE_FILTER_NOTICE_SIZE,
               "a tone-filter edit's notice fits the toast");
_Static_assert(sizeof(((dsd_state*)0)->ui_msg)
                   >= sizeof(UI_TONE_FILTER_REFUSED) - 1U + (size_t)DSD_TONE_LIST_ERROR_SIZE,
               "a tone-filter refusal fits the toast");

/* The policy a tone-filter edit sets, checked as dsd_tone_filter_check() checks it. An edit that keeps the list
   (keep_list, the terminal's Off) takes the configured list as it stands now, on the decoder thread, so an edit or a
   loaded config queued before it is what it keeps; with no list configured it is judged as an edit without one, which
   only off takes. Returns 0 with the policy's list in @p out, -1 with the reason in @p why. */
static int
ui_tone_filter_edit_policy(const dsd_opts* opts, const dsd_state* state, const dsd_app_tone_filter_payload* edit,
                           dsd_tone_set* out, char* why, size_t why_size) {
    if (!edit->keep_list) {
        return dsd_tone_filter_check((int)edit->mode, edit->list, out, why, why_size);
    }
    dsd_tone_set kept;
    DSD_MEMSET(&kept, 0, sizeof kept);
    dsd_scan_mode_configured_tone_policy(opts, state, NULL, &kept);
    if (dsd_tone_set_count(&kept) > 0 && dsd_tone_filter_mode_name((int)edit->mode) != NULL) {
        *out = kept;
        return 0;
    }
    return dsd_tone_filter_check((int)edit->mode, "", out, why, why_size);
}

/* DSD_APP_CMD_TONE_FILTER_SET (issue #527, the live tone-filter editor): the configured CTCSS/DCS receive policy, mode
   and list, checked by the list parser and refused with the reason (by entry number, never the text), changing nothing,
   when it fails. Like the squelch and width setters it edits the configured policy without suspending a scan row
   (dsd_scan_mode_set_configured_tone_policy()): a row's own tone options stay in force until it leaves, and the notice
   says so. The monitor's tap judges by the new policy from its next read of audio. */
static int
ui_cmd_handle_tone_filter_set(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    if (!opts || !state || c->n != sizeof(dsd_app_tone_filter_payload)) {
        return UI_CMD_APPLY_INVALID_PAYLOAD;
    }
    dsd_app_tone_filter_payload edit;
    DSD_MEMCPY(&edit, c->data, sizeof edit);
    if (!memchr(edit.list, '\0', sizeof edit.list) || (edit.keep_list != 0 && edit.keep_list != 1)
        || (edit.keep_list && edit.list[0] != '\0')) {
        return UI_CMD_APPLY_INVALID_PAYLOAD;
    }
    dsd_tone_set set;
    char why[DSD_TONE_LIST_ERROR_SIZE];
    if (ui_tone_filter_edit_policy(opts, state, &edit, &set, why, sizeof why) != 0) {
        ui_set_toast(state, 5, UI_TONE_FILTER_REFUSED "%s", why);
        return UI_CMD_APPLY_FAILED;
    }
    (void)dsd_scan_mode_set_configured_tone_policy(opts, state, (int)edit.mode, &set);
    char notice[DSD_APP_TONE_FILTER_NOTICE_SIZE];
    (void)dsd_app_tone_filter_edit_notice(opts, state, notice, sizeof notice);
    ui_set_toast(state, 3, "%s", notice);
    return UI_CMD_APPLY_COMPLETED;
}

/* A session edit of the scan row on air ("this channel", issue #518): scan_row_edit.c applies it and words the toast. */
static int
ui_cmd_handle_scan_row_edit(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    char notice[DSD_APP_SCAN_ROW_NOTICE_SIZE];
    int ttl_s = 3;
    const int rc = dsd_app_apply_scan_row_edit(opts, state, c, notice, sizeof notice, &ttl_s);
    if (notice[0]) {
        ui_set_toast(state, ttl_s, "%s", notice);
    }
    if (rc == DSD_APP_SCAN_ROW_EDIT_BAD_PAYLOAD) {
        return UI_CMD_APPLY_INVALID_PAYLOAD;
    }
    return rc == DSD_APP_SCAN_ROW_EDIT_DONE ? UI_CMD_APPLY_COMPLETED : UI_CMD_APPLY_FAILED;
}

static int
apply_cmd_io_and_import_runtime_a(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    static const struct dsd_app_command_handler_entry k_handlers[] = {
        {DSD_APP_CMD_NFM_BANDWIDTH_SET, ui_cmd_handle_nfm_bandwidth_set},
        {DSD_APP_CMD_AM_BANDWIDTH_SET, ui_cmd_handle_am_bandwidth_set},
        {DSD_APP_CMD_TONE_FILTER_SET, ui_cmd_handle_tone_filter_set},
        {DSD_APP_CMD_SCAN_ROW_EDIT, ui_cmd_handle_scan_row_edit},
        {DSD_APP_CMD_RIGCTL_SET_MOD_BW, ui_cmd_handle_rigctl_set_mod_bw},
        {DSD_APP_CMD_TG_HOLD_SET, ui_cmd_handle_tg_hold_set},
        {DSD_APP_CMD_HANGTIME_SET, ui_cmd_handle_hangtime_set},
        {DSD_APP_CMD_SLOT_PREF_SET, ui_cmd_handle_slot_pref_set},
        {DSD_APP_CMD_SLOTS_ONOFF_SET, ui_cmd_handle_slots_onoff_set},
        {DSD_APP_CMD_SCAN_VOICE_ONLY_SET, ui_cmd_handle_scan_voice_only_set},
        {DSD_APP_CMD_SCAN_VOICE_QUALIFY_MS_SET, ui_cmd_handle_scan_voice_qualify_ms_set},
        {DSD_APP_CMD_SCAN_VOICE_HOLD_MS_SET, ui_cmd_handle_scan_voice_hold_ms_set},
    };
    if (!opts || !c) {
        return 0;
    }
    return ui_cmd_apply_handler_table(k_handlers, sizeof k_handlers / sizeof k_handlers[0], opts, state, c);
}

static int
apply_cmd_io_and_import_pulse_io(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    if (!opts || !c) {
        return 0;
    }
    switch (c->id) {
        case DSD_APP_CMD_PULSE_OUT_SET: {
            int result = UI_CMD_APPLY_COMPLETED;
            if (state && c->n > 0) {
                char name[256] = {0};
                size_t n = c->n < sizeof(name) ? c->n : sizeof(name) - 1;
                DSD_MEMCPY(name, c->data, n);
                name[n] = '\0';
                int rc = svc_set_pulse_output(opts, name);
                result = ui_cmd_apply_status_from_service_rc(rc);
                if (rc == 0) {
                    ui_set_toast(state, 3, "Applied: Pulse output -> %s", name);
                } else {
                    ui_set_toast(state, 4, "Failed: Pulse output -> %s", name);
                }
            }
            return result;
        }
        case DSD_APP_CMD_PULSE_IN_SET: {
            if (!state || c->n == 0) {
                return UI_CMD_APPLY_COMPLETED;
            }
            char name[256] = {0};
            size_t n = c->n < sizeof(name) ? c->n : sizeof(name) - 1;
            DSD_MEMCPY(name, c->data, n);
            name[n] = '\0';
            if (ui_input_switch_refused(opts, state)) {
                return UI_CMD_APPLY_UNSUPPORTED;
            }
            p25_sm_tick_guard_enter();
            const int rc = svc_set_pulse_input(opts, state, name);
            const int result =
                ui_cmd_finish_input_switch(opts, state, rc, "Pulse input", name[0] ? name : "default device");
            p25_sm_tick_guard_leave();
            return result;
        }
        default: return 0;
    }
}

static int
ui_cmd_handle_lrrp_set_home(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    (void)c;
    int result = UI_CMD_APPLY_COMPLETED;
    if (state) {
        int rc = svc_lrrp_set_home(opts);
        result = ui_cmd_apply_status_from_service_rc(rc);
        if (rc == 0) {
            ui_set_toast(state, 3, "Applied: LRRP output -> %s", opts->lrrp_out_file);
        } else {
            ui_set_toast(state, 4, "Failed: LRRP output (home)");
        }
    }
    return result;
}

static int
ui_cmd_handle_lrrp_set_dsdp(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    (void)c;
    int result = UI_CMD_APPLY_COMPLETED;
    if (state) {
        int rc = svc_lrrp_set_dsdp(opts);
        result = ui_cmd_apply_status_from_service_rc(rc);
        if (rc == 0) {
            ui_set_toast(state, 3, "Applied: LRRP output -> %s", opts->lrrp_out_file);
        } else {
            ui_set_toast(state, 4, "Failed: LRRP output (DSDPlus)");
        }
    }
    return result;
}

static int
ui_cmd_handle_lrrp_set_custom(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    int result = UI_CMD_APPLY_COMPLETED;
    if (state && c->n > 0) {
        char path[1024] = {0};
        if (ui_cmd_copy_payload_string(c, path, sizeof path)) {
            int rc = svc_lrrp_set_custom(opts, path);
            result = ui_cmd_apply_status_from_service_rc(rc);
            if (rc == 0) {
                ui_set_toast(state, 3, "Applied: LRRP output -> %s", path);
            } else {
                ui_set_toast(state, 4, "Failed: LRRP output -> %s", path);
            }
        }
    }
    return result;
}

static int
ui_cmd_handle_lrrp_disable(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    (void)c;
    if (state) {
        svc_lrrp_disable(opts);
        ui_set_toast(state, 3, "Applied: LRRP output disabled");
    }
    return 1;
}

static int
ui_cmd_handle_p25_p2_params_set(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    struct {
        uint64_t w;
        uint64_t s;
        uint64_t n;
    } p = {0};

    (void)opts;
    if (state && c->n >= (int)sizeof p) {
        DSD_MEMCPY(&p, c->data, sizeof p);
        svc_set_p2_params(state, p.w, p.s, p.n);
        ui_set_toast(state, 3, "Applied: P25 P2 params W:%llX S:%llX N:%llX", (unsigned long long)state->p2_wacn,
                     (unsigned long long)state->p2_sysid, (unsigned long long)state->p2_cc);
    }
    return 1;
}

static int
apply_cmd_io_and_import_lrrp_and_p2(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    static const struct dsd_app_command_handler_entry k_handlers[] = {
        {DSD_APP_CMD_LRRP_SET_HOME, ui_cmd_handle_lrrp_set_home},
        {DSD_APP_CMD_LRRP_SET_DSDP, ui_cmd_handle_lrrp_set_dsdp},
        {DSD_APP_CMD_LRRP_SET_CUSTOM, ui_cmd_handle_lrrp_set_custom},
        {DSD_APP_CMD_LRRP_DISABLE, ui_cmd_handle_lrrp_disable},
        {DSD_APP_CMD_P25_P2_PARAMS_SET, ui_cmd_handle_p25_p2_params_set},
    };
    if (!opts || !c) {
        return 0;
    }
    return ui_cmd_apply_handler_table(k_handlers, sizeof k_handlers / sizeof k_handlers[0], opts, state, c);
}

static int
ui_cmd_handle_import_channel_map(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    int result = UI_CMD_APPLY_COMPLETED;
    if (state && c->n > 0) {
        char path[1024] = {0};
        if (ui_cmd_copy_payload_string(c, path, sizeof path)) {
            int rc = svc_import_channel_map(opts, state, path);
            result = ui_cmd_apply_status_from_service_rc(rc);
            char skipped[160];
            if (rc == 0 && svc_channel_map_refused_rows(opts, state, skipped, sizeof skipped) > 0) {
                /* The rows load; the ones the DSP rate cannot run are named with the import (issue #526). */
                ui_set_toast(state, 5, "Imported; %s", skipped);
            } else if (rc == 0) {
                ui_set_toast(state, 3, "Applied: Channel map imported -> %s", path);
            } else {
                ui_set_toast(state, 4, "Failed: Channel map import -> %s", path);
            }
        }
    }
    return result;
}

static int
ui_cmd_handle_import_p25_bandplan(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    int result = UI_CMD_APPLY_COMPLETED;
    if (state && c->n > 0) {
        char path[1024] = {0};
        if (ui_cmd_copy_payload_string(c, path, sizeof path)) {
            int rc = svc_import_p25_bandplan(opts, state, path);
            result = ui_cmd_apply_status_from_service_rc(rc);
            if (rc == 0) {
                ui_set_toast(state, 3, "Applied: P25 band plan imported -> %s", path);
            } else {
                ui_set_toast(state, 4, "Failed: P25 band plan import -> %s", path);
            }
        }
    }
    return result;
}

static int
// cppcheck-suppress constParameterCallback -- signature is fixed by the command handler table.
ui_cmd_handle_export_p25_bandplan(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    int result = UI_CMD_APPLY_COMPLETED;
    if (state && c->n > 0) {
        char path[1024] = {0};
        if (ui_cmd_copy_payload_string(c, path, sizeof path)) {
            int rows = svc_export_p25_bandplan(opts, state, path);
            result = ui_cmd_apply_status_from_service_rc(rows > 0 ? 0 : -1);
            if (rows > 0) {
                ui_set_toast(state, 3, "Applied: %d P25 band plan row(s) exported -> %s", rows, path);
            } else {
                ui_set_toast(state, 4, "Failed: P25 band plan export -> %s", path);
            }
        }
    }
    return result;
}

static int
ui_cmd_handle_import_group_list(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    int result = UI_CMD_APPLY_COMPLETED;
    if (state && c->n > 0) {
        char path[1024] = {0};
        if (ui_cmd_copy_payload_string(c, path, sizeof path)) {
            int rc = svc_import_group_list(opts, state, path);
            result = ui_cmd_apply_status_from_service_rc(rc);
            if (rc == 0) {
                ui_set_toast(state, 3, "Applied: Group list reloaded -> %s", path);
            } else {
                ui_set_toast(state, 4, "Failed: Group list reload -> %s", path);
            }
        }
    }
    return result;
}

static int
ui_cmd_handle_import_src_list(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    int result = UI_CMD_APPLY_COMPLETED;
    if (state && c->n > 0) {
        char path[1024] = {0};
        if (ui_cmd_copy_payload_string(c, path, sizeof path)) {
            int rc = svc_import_src_list(opts, state, path);
            result = ui_cmd_apply_status_from_service_rc(rc);
            if (rc == 0) {
                ui_set_toast(state, 3, "Applied: Source ID list imported -> %s", path);
            } else {
                ui_set_toast(state, 4, "Failed: Source ID list import -> %s", path);
            }
        }
    }
    return result;
}

static int
ui_cmd_handle_import_keys_dec(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    int result = UI_CMD_APPLY_COMPLETED;
    if (state && c->n > 0) {
        char path[1024] = {0};
        if (ui_cmd_copy_payload_string(c, path, sizeof path)) {
            // A parked keyed row holds the foreground keyring: edit the globals
            // underneath it so the import survives the next hop.
            dsd_scan_keys_suspend(state);
            int rc = svc_import_keys_dec(opts, state, path);
            dsd_scan_keys_resume(state);
            result = ui_cmd_apply_status_from_service_rc(rc);
            if (rc == 0) {
                dsd_enc_lockout_bump_key_epoch(state);
                ui_set_toast(state, 3, "Applied: Keys (DEC) imported -> %s", path);
            } else {
                ui_set_toast(state, 4, "Failed: Keys (DEC) import -> %s", path);
            }
        }
    }
    return result;
}

static int
ui_cmd_handle_import_keys_hex(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    int result = UI_CMD_APPLY_COMPLETED;
    if (state && c->n > 0) {
        char path[1024] = {0};
        if (ui_cmd_copy_payload_string(c, path, sizeof path)) {
            dsd_scan_keys_suspend(state);
            int rc = svc_import_keys_hex(opts, state, path);
            dsd_scan_keys_resume(state);
            result = ui_cmd_apply_status_from_service_rc(rc);
            if (rc == 0) {
                dsd_enc_lockout_bump_key_epoch(state);
                ui_set_toast(state, 3, "Applied: Keys (HEX) imported -> %s", path);
            } else {
                ui_set_toast(state, 4, "Failed: Keys (HEX) import -> %s", path);
            }
        }
    }
    return result;
}

static int
ui_cmd_handle_import_channel_map_clear(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    (void)c;
    if (!state) {
        return UI_CMD_APPLY_COMPLETED;
    }
    const int rc = svc_clear_channel_map(opts, state);
    if (rc == 0) {
        ui_set_toast(state, 3, "Applied: Channel map cleared");
    } else {
        ui_set_toast(state, 4, "Failed: Channel map clear");
    }
    return ui_cmd_apply_status_from_service_rc(rc);
}

static int
ui_cmd_handle_import_group_list_clear(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    (void)c;
    if (!state) {
        return UI_CMD_APPLY_COMPLETED;
    }
    const int rc = svc_clear_group_list(opts, state);
    if (rc == 0) {
        ui_set_toast(state, 3, "Applied: Group list cleared");
    } else {
        ui_set_toast(state, 4, "Failed: Group list clear");
    }
    return ui_cmd_apply_status_from_service_rc(rc);
}

static int
ui_cmd_handle_import_src_list_clear(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    (void)c;
    if (!state) {
        return UI_CMD_APPLY_COMPLETED;
    }
    const int rc = svc_clear_src_list(opts, state);
    if (rc == 0) {
        ui_set_toast(state, 3, "Applied: Source ID list cleared");
    } else {
        ui_set_toast(state, 4, "Failed: Source ID list clear");
    }
    return ui_cmd_apply_status_from_service_rc(rc);
}

static int
ui_cmd_handle_import_keys_clear(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    (void)c;
    if (!state) {
        return UI_CMD_APPLY_COMPLETED;
    }
    dsd_scan_keys_suspend(state);
    const int rc = svc_clear_keys(opts, state);
    dsd_scan_keys_resume(state);
    if (rc == 0) {
        // The lockout ledger is keyed on the key epoch, so targets skipped for
        // want of a key have to be reconsidered against the empty keyring the
        // same way they are against a newly imported one.
        dsd_enc_lockout_bump_key_epoch(state);
        ui_set_toast(state, 3, "Applied: Keys cleared");
    } else {
        ui_set_toast(state, 4, "Failed: Keys clear");
    }
    return ui_cmd_apply_status_from_service_rc(rc);
}

static int
apply_cmd_io_and_import_imports(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    static const struct dsd_app_command_handler_entry k_handlers[] = {
        {DSD_APP_CMD_IMPORT_CHANNEL_MAP, ui_cmd_handle_import_channel_map},
        {DSD_APP_CMD_IMPORT_GROUP_LIST, ui_cmd_handle_import_group_list},
        {DSD_APP_CMD_IMPORT_SRC_LIST, ui_cmd_handle_import_src_list},
        {DSD_APP_CMD_IMPORT_SRC_LIST_CLEAR, ui_cmd_handle_import_src_list_clear},
        {DSD_APP_CMD_IMPORT_KEYS_DEC, ui_cmd_handle_import_keys_dec},
        {DSD_APP_CMD_IMPORT_KEYS_HEX, ui_cmd_handle_import_keys_hex},
        {DSD_APP_CMD_IMPORT_CHANNEL_MAP_CLEAR, ui_cmd_handle_import_channel_map_clear},
        {DSD_APP_CMD_IMPORT_GROUP_LIST_CLEAR, ui_cmd_handle_import_group_list_clear},
        {DSD_APP_CMD_IMPORT_KEYS_CLEAR, ui_cmd_handle_import_keys_clear},
        {DSD_APP_CMD_IMPORT_P25_BANDPLAN, ui_cmd_handle_import_p25_bandplan},
        {DSD_APP_CMD_EXPORT_P25_BANDPLAN, ui_cmd_handle_export_p25_bandplan},
    };
    if (!opts || !c) {
        return 0;
    }
    return ui_cmd_apply_handler_table(k_handlers, sizeof k_handlers / sizeof k_handlers[0], opts, state, c);
}

static int
apply_cmd_io_and_import(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    /* Each group returns nonzero once it handled the command. */
    static const dsd_app_command_handler_fn k_groups[] = {
        apply_cmd_io_and_import_file_outputs_a, apply_cmd_io_and_import_file_outputs_b,
        apply_cmd_io_and_import_network,
#ifdef USE_RADIO
        apply_cmd_io_and_import_rtl_a,          apply_cmd_io_and_import_rtl_b,
        apply_cmd_io_and_import_rtl_c,          apply_cmd_io_and_import_rtl_d,
#endif
        apply_cmd_io_and_import_squelch,        apply_cmd_io_and_import_runtime_a,
        apply_cmd_io_and_import_pulse_io,       apply_cmd_io_and_import_lrrp_and_p2,
        apply_cmd_io_and_import_imports,
    };
    if (!opts || !c) {
        return 0;
    }
    for (size_t i = 0; i < sizeof k_groups / sizeof k_groups[0]; i++) {
        const int r = k_groups[i](opts, state, c);
        if (r) {
            return r;
        }
    }
    return 0;
}

#ifdef USE_RADIO
static int
apply_cmd_dsp(const struct dsd_app_command* c, const dsd_opts* opts, dsd_state* state) {
    if (!c || c->id != DSD_APP_CMD_DSP_OP) {
        return 0;
    }
    dsd_app_dsp_payload p = {0};
    if (c->n >= (int)sizeof(dsd_app_dsp_payload)) {
        DSD_MEMCPY(&p, c->data, sizeof p);
    }
    if (opts && dsd_opts_audio_in_dev_is_airspy_spec(opts->audio_in_dev)
        && p.op == DSD_APP_DSP_OP_TUNER_AUTOGAIN_TOGGLE) {
        return UI_CMD_APPLY_UNSUPPORTED;
    }
    apply_dsp_op(&p, opts, state);
    return 1;
}
#endif

/* Through the shared chain rather than a fourth copy of the same ternary: the retune and
   tune commands here have to name the control channel the status surfaces name. */
static long
current_cc_freq(const dsd_state* state) {
    return dsd_app_cc_freq(state);
}

static void
reset_call_tracking(dsd_opts* opts, dsd_state* state, int clear_trunk_vc) {
    ui_end_calls(opts, state);
    DSD_MEMSET(state->nxdn_sacch_frame_segment, 1, sizeof(state->nxdn_sacch_frame_segment));
    DSD_MEMSET(state->nxdn_sacch_frame_segcrc, 1, sizeof(state->nxdn_sacch_frame_segcrc));
    (void)dsd_recent_activity_clear_all(state);
    dmr_reset_blocks(opts, state);
    state->payload_algid = state->payload_algidR = 0;
    state->payload_keyid = state->payload_keyidR = 0;
    state->payload_mi = state->payload_miR = state->payload_miP = state->payload_miN = 0;
    opts->trunk_is_tuned = 0;
    state->p25_vc_freq[0] = state->p25_vc_freq[1] = 0;
    if (clear_trunk_vc) {
        state->trunk_vc_freq[0] = state->trunk_vc_freq[1] = 0;
    }
}

static int
current_demod_rate(const dsd_opts* opts, const dsd_state* state) {
#ifdef USE_RADIO
    if (opts->audio_in_type == AUDIO_IN_RTL && state->rtl_ctx) {
        int demod_rate = (int)rtl_stream_output_rate(state->rtl_ctx);
        if (demod_rate > 0) {
            return demod_rate;
        }
    }
#else
    (void)state;
#endif
    return dsd_opts_current_input_timing_rate(opts);
}

static int
cc_has_active_p25_context(const dsd_opts* opts, const dsd_state* state) {
    if (!opts || !state || (opts->frame_p25p1 != 1 && opts->frame_p25p2 != 1)) {
        return 0;
    }
    if (state->synctype != DSD_SYNC_NONE) {
        return DSD_SYNC_IS_P25(state->synctype) ? 1 : 0;
    }
    if (state->lastsynctype != DSD_SYNC_NONE) {
        return DSD_SYNC_IS_P25(state->lastsynctype) ? 1 : 0;
    }
    /* A P25 voice tune remains authoritative while frame sync is temporarily absent. */
    return opts->trunk_is_tuned == 1 && (state->p25_vc_freq[0] != 0 || state->p25_vc_freq[1] != 0);
}

static int
cc_symbol_rate(const dsd_opts* opts, const dsd_state* state, int fdma_only) {
    if (!cc_has_active_p25_context(opts, state)) {
        return 0;
    }
    if (state->p25_cc_is_tdma == 0) {
        return 4800;
    }
    if (!fdma_only && state->p25_cc_is_tdma == 1) {
        return 6000;
    }
    // Keep the retune profile-neutral unless the P25 CC type is known and allowed by the action.
    return 0;
}

static void
set_cc_symbol_timing(const dsd_opts* opts, dsd_state* state, int sym_rate) {
    if (sym_rate == 0) {
        return;
    }
    state->samplesPerSymbol = dsd_opts_compute_sps_rate(opts, sym_rate, current_demod_rate(opts, state));
    state->symbolCenter = dsd_opts_symbol_center(state->samplesPerSymbol);
    state->sps_hunt_idx = sym_rate == 6000 ? DSD_FRAME_SYNC_SPS_PROFILE_6000_4 : DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    state->sps_hunt_counter = 0;
}

static int
request_manual_tune(dsd_opts* opts, dsd_state* state, long int freq, int p25_cc_symbol_rate, const char* action,
                    dsd_trunk_tune_result* out_result, uint64_t* out_request_id) {
    int result = 0;
    int accepted = 0;
    dsd_trunk_tune_result tune_result = DSD_TRUNK_TUNE_RESULT_FAILED;
    uint64_t request_id = 0U;
    if (p25_cc_symbol_rate != 0) {
        const int ted_sps = dsd_opts_compute_sps_rate(opts, p25_cc_symbol_rate, current_demod_rate(opts, state));
        tune_result = dsd_trunk_tuning_hook_tune_to_cc(opts, state, freq, ted_sps, &request_id);
        result = (int)tune_result;
        accepted = dsd_trunk_tune_result_is_ok(tune_result);
    } else {
        /* Generic LCN actions and unknown CC types must not stage a P25 RTL profile. */
        result = io_control_set_freq(opts, state, freq);
        accepted = result == RTL_STREAM_TUNE_OK || result == RTL_STREAM_TUNE_TIMEOUT;
        if (accepted) {
            /* This leg has no correlated tune request to await, so an accepted
             * timeout (issued, not yet confirmed) is reported as complete. */
            tune_result = DSD_TRUNK_TUNE_RESULT_OK;
        }
    }
    if (out_result) {
        *out_result = tune_result;
    }
    if (out_request_id) {
        *out_request_id = request_id;
    }
    if (accepted) {
        /* The radio is moving (issue #522). With rigctl on PCM input nothing else reports this
           hop: io_control does not advance the trunk-tuning generation, and there is no RTL
           stream generation to move, so the tone heard on the old channel goes here. */
        dsd_analog_rx_reset(state);
        return 1;
    }
    LOG_WARN("WARNING: %s tune to %ld Hz was not accepted (result=%d); preserving decoder state\n",
             action ? action : "Manual", freq, result);
    return 0;
}

static void
mark_cc_sync(dsd_state* state, int include_monotonic) {
    state->last_cc_sync_time = dsd_decode_time();
    if (include_monotonic) {
        state->last_cc_sync_time_m = dsd_decode_now_mono_s();
    }
}

/* #506: the manual CC tunes below go straight to the tuner, bypassing p25_sm_release().
 * Hand the P25 SM its parked state, or it keeps judging every later grant as a
 * preemption of the assignment it was following. */
static void
hand_p25_sm_to_cc(dsd_opts* opts, dsd_state* state, dsd_trunk_tune_result tune_result, uint64_t request_id,
                  const char* source) {
    if (p25_sm_on_external_cc_tune(p25_sm_get_ctx(), opts, state, tune_result, request_id, source)) {
        return;
    }
    /* A decline hands the SM back to its own recovery: the tune was refused, or the
     * context is idle or uninitialized. It mutates nothing, so without a line here
     * "handed off" and "left alone" are indistinguishable in a field log. */
    LOG_DEBUG("P25 SM CC handoff declined (source=%s result=%d)\n", source ? source : "unknown", (int)tune_result);
}

/* Whether the P25 SM owns this trunk. When it does, the tune, the decoder resets and
 * the SM handoff run under the watchdog guard as one step, like every other CC
 * retune (p25_sm_select_control_channel(), the no-carrier return): a watchdog tick
 * in between would see a TUNED context with no tuned decoder and issue a second CC
 * return of its own. DMR and NXDN trunks keep their unguarded path. */
static int
p25_sm_owns_trunk(const dsd_opts* opts, const dsd_state* state) {
    return dsd_trunk_p25_recovery_allowed(opts, state);
}

/* Each manual retune runs its tune, its decoder resets and its SM handoff as one
 * guarded step, so a watchdog tick never observes the half-moved state in between. */
typedef int (*manual_retune_step_fn)(dsd_opts* opts, dsd_state* state, int p25_live);

static int
run_manual_retune_guarded(dsd_opts* opts, dsd_state* state, manual_retune_step_fn step) {
    const int p25_live = p25_sm_owns_trunk(opts, state);
    if (p25_live) {
        p25_sm_tick_guard_enter();
    }
    const int status = step(opts, state, p25_live);
    if (p25_live) {
        p25_sm_tick_guard_leave();
    }
    return status;
}

static int
apply_manual_return_to_cc_locked(dsd_opts* opts, dsd_state* state, int p25_live) {
    const long freq = current_cc_freq(state);
    const int sym_rate = cc_symbol_rate(opts, state, 0);
    dsd_trunk_tune_result tune_result = DSD_TRUNK_TUNE_RESULT_FAILED;
    uint64_t request_id = 0U;
    if (!request_manual_tune(opts, state, freq, sym_rate, "Return-to-CC", &tune_result, &request_id)) {
        return UI_CMD_APPLY_FAILED;
    }
    reset_call_tracking(opts, state, 1);
    mark_cc_sync(state, 1);
    set_cc_symbol_timing(opts, state, sym_rate);
    if (p25_live) {
        hand_p25_sm_to_cc(opts, state, tune_result, request_id, "return-to-cc");
    }
    LOG_INFO("User Activated Return to CC\n");
    return UI_CMD_APPLY_COMPLETED;
}

static int
apply_manual_return_to_cc(dsd_opts* opts, dsd_state* state) {
    if (!state) {
        return UI_CMD_APPLY_COMPLETED;
    }
    if (opts->trunk_enable != 1 || (state->trunk_cc_freq == 0 && state->p25_cc_freq == 0)) {
        return UI_CMD_APPLY_COMPLETED;
    }
    /* A retune the user asked for, refused as a tap is (issue #575). */
    if (ui_cmd_refuse_replay_tune(opts, state)) {
        return UI_CMD_APPLY_FAILED;
    }
    return run_manual_retune_guarded(opts, state, apply_manual_return_to_cc_locked);
}

static int
apply_user_block_transition_locked(dsd_opts* opts, dsd_state* state, int p25_live, const char* tune_reason,
                                   const char* sm_source) {
    long cc_freq = 0;
    dsd_trunk_tune_result tune_result = DSD_TRUNK_TUNE_RESULT_FAILED;
    uint64_t request_id = 0U;
    const int sym_rate = cc_symbol_rate(opts, state, 1);
    if (opts->trunk_enable == 1) {
        cc_freq = current_cc_freq(state);
        if (cc_freq != 0
            && !request_manual_tune(opts, state, cc_freq, sym_rate, tune_reason, &tune_result, &request_id)) {
            return UI_CMD_APPLY_FAILED;
        }
    }

    reset_call_tracking(opts, state, 1);
    if (opts->trunk_enable == 1) {
        dsd_engine_no_carrier_locked(opts, state);
        state->trunk_cc_freq = cc_freq;
    }
    /* Wall clock only: the P25 handoff below stamps the monotonic CC sync from the
     * tune boundary itself, which is what its acquisition window is measured from. */
    mark_cc_sync(state, 0);
    set_cc_symbol_timing(opts, state, sym_rate);
    if (p25_live) {
        /* With no CC known nothing was tuned; tune_result is still FAILED and the
         * handoff declines, leaving the SM to its own stale-context recovery. */
        hand_p25_sm_to_cc(opts, state, tune_result, request_id, sm_source);
    }
    return UI_CMD_APPLY_COMPLETED;
}

static int
apply_lockout_decoder_transition_locked(dsd_opts* opts, dsd_state* state, int p25_live) {
    return apply_user_block_transition_locked(opts, state, p25_live, "Lockout return-to-CC", "user-lockout");
}

static int
apply_skip_decoder_transition_locked(dsd_opts* opts, dsd_state* state, int p25_live) {
    return apply_user_block_transition_locked(opts, state, p25_live, "Skip return-to-CC", "user-skip");
}

static int
try_manual_candidate_cycle_locked(dsd_opts* opts, dsd_state* state, int p25_live) {
    long cand = 0;
    if (opts->p25_prefer_candidates != 1 || !p25_cc_next_candidate(state, &cand)) {
        return UI_CMD_APPLY_UNHANDLED;
    }
    const int sym_rate = cc_symbol_rate(opts, state, 0);
    dsd_trunk_tune_result tune_result = DSD_TRUNK_TUNE_RESULT_FAILED;
    uint64_t request_id = 0U;
    if (!request_manual_tune(opts, state, cand, sym_rate, "Candidate cycle", &tune_result, &request_id)) {
        return UI_CMD_APPLY_FAILED;
    }

    reset_call_tracking(opts, state, 0);
    if (opts->trunk_enable != 1) {
        /* With trunking off the candidate list is no system the receiver follows: the next entry is another carrier
           (issue #575). */
        ui_leave_carrier(opts, state, p25_live);
    }
    LOG_INFO("Candidate Cycle: tuning to %.06lf MHz\n", (double)cand / 1000000);
    mark_cc_sync(state, 1);
    set_cc_symbol_timing(opts, state, sym_rate);
    if (p25_live) {
        hand_p25_sm_to_cc(opts, state, tune_result, request_id, "candidate-cycle");
    }
    return UI_CMD_APPLY_COMPLETED;
}

static int
apply_manual_lcn_cycle_untyped_locked(dsd_opts* opts, dsd_state* state, int p25_live) {
    int count = state->lcn_freq_count;
    if (count <= 0) {
        return UI_CMD_APPLY_COMPLETED;
    }

    int next = state->lcn_freq_roll;
    if (next < 0 || next >= count) {
        next = 0;
    }
    const int start = next;
    long freq = 0;
    for (int examined = 0; examined < count; examined++) {
        freq = *dsd_state_trunk_lcn_slot(state, next);
        if (freq != 0 && !dsd_state_trunk_lcn_avoid_get(state, (size_t)next)
            && (next == 0 || *dsd_state_trunk_lcn_slot(state, next - 1) != freq)) {
            break;
        }
        next++;
        if (next >= count) {
            next = 0;
        }
        freq = 0;
    }

    if (freq == 0) {
        state->lcn_freq_roll = start + 1;
        if (state->lcn_freq_roll >= count) {
            state->lcn_freq_roll = 0;
        }
        return UI_CMD_APPLY_COMPLETED;
    }
    dsd_trunk_tune_result tune_result = DSD_TRUNK_TUNE_RESULT_FAILED;
    uint64_t request_id = 0U;
    if (!request_manual_tune(opts, state, freq, 0, "Channel cycle", &tune_result, &request_id)) {
        return UI_CMD_APPLY_FAILED;
    }

    reset_call_tracking(opts, state, 0);
    if (opts->trunk_enable != 1) {
        /* A conventional list's next channel is another carrier; a trunked system's channel list is the system's own
           (issue #575). */
        ui_leave_carrier(opts, state, p25_live);
    }
    LOG_INFO("Channel Cycle: tuning to %.06lf MHz\n", (double)freq / 1000000);
    state->lcn_freq_roll = next + 1;
    dsd_scan_row_keys_apply(state, next);
    mark_cc_sync(state, 1);
    if (p25_live) {
        hand_p25_sm_to_cc(opts, state, tune_result, request_id, "channel-cycle");
    }
    return UI_CMD_APPLY_COMPLETED;
}

static int
apply_manual_lcn_cycle_locked(dsd_opts* opts, dsd_state* state, int p25_live) {
    if (opts->scanner_mode == 1 && dsd_channel_modes_present(state)) {
        return dsd_engine_channel_scan_step_manual(opts, state) < 0 ? UI_CMD_APPLY_FAILED : UI_CMD_APPLY_COMPLETED;
    }
    return apply_manual_lcn_cycle_untyped_locked(opts, state, p25_live);
}

static int
apply_manual_lcn_cycle(dsd_opts* opts, dsd_state* state) {
    return run_manual_retune_guarded(opts, state, apply_manual_lcn_cycle_locked);
}

/* #506: both legs retune through the tuning hooks, past p25_sm_release(), exactly
 * like the lockout and the return-to-CC do. Without the handoff the SM stays TUNED on
 * the assignment it was following and refuses every later grant as a preemption of it. */
static int
apply_manual_channel_cycle_locked(dsd_opts* opts, dsd_state* state, int p25_live) {
    if (opts->scanner_mode == 1 && dsd_channel_modes_present(state)) {
        return apply_manual_lcn_cycle_locked(opts, state, p25_live);
    }
    const int candidate_status = try_manual_candidate_cycle_locked(opts, state, p25_live);
    if (candidate_status != UI_CMD_APPLY_UNHANDLED) {
        return candidate_status;
    }
    return apply_manual_lcn_cycle_locked(opts, state, p25_live);
}

static int
apply_manual_channel_cycle(dsd_opts* opts, dsd_state* state) {
    return run_manual_retune_guarded(opts, state, apply_manual_channel_cycle_locked);
}

#ifdef USE_RADIO
static int
cfg_uses_rtl_runtime(const dsdneoUserConfig* cfg) {
    if (!cfg || !cfg->has_input) {
        return 0;
    }
    return cfg->input_source == DSDCFG_INPUT_RTL || cfg->input_source == DSDCFG_INPUT_RTLTCP
           || cfg->input_source == DSDCFG_INPUT_SOAPY || cfg->input_source == DSDCFG_INPUT_AIRSPY;
}

static void
apply_cfg_live_rtl_ppm_request(dsd_opts* opts, const dsdneoUserConfig* cfg, int old_audio_in_type) {
    if (!opts || old_audio_in_type != AUDIO_IN_RTL || opts->audio_in_type != AUDIO_IN_RTL || !cfg_uses_rtl_runtime(cfg)
        || !cfg->rtl_ppm_is_set) {
        return;
    }
    /* Config apply must mint a fresh request generation even when the input
     * device string is unchanged, otherwise same-value retries after a failed
     * apply are mistaken for stale state and never reach the controller.
     * Omitted rtl_ppm must preserve the live correction instead of clearing it
     * back to the config struct's zero-initialized default. */
    (void)rtl_stream_request_ppm(opts, cfg->rtl_ppm);
}

static void
apply_cfg_rtl_common(dsd_opts* opts, const dsdneoUserConfig* cfg) {
    if (cfg->rtl_freq[0]) {
        uint32_t hz = dsd_parse_freq_hz(cfg->rtl_freq);
        if (hz > 0) {
            opts->rtlsdr_center_freq = hz;
        }
    }
    if (cfg->rtl_bw_khz) {
        opts->rtl_dsp_bw_khz = cfg->rtl_bw_khz;
    }
    /* rtl_sql is the same setting the CLI `sql` field carries, so it converts the
     * same way: negative is decibels, 0 is off. Storing the raw integer put a
     * -50 dB request in as a power of -50, which switches the squelch off.
     * Unlike the sibling keys below, 0 is a real value here rather than "key
     * omitted", so this applies unconditionally — as the startup loader does. */
    opts->rtl_squelch_level = dsd_squelch_level_from_sql((double)cfg->rtl_sql);
    /* The mode beside it: rtl_sql_mode = auto or noise puts rtl_sql_margin_db (over the learned floor, or of FM
       quieting) in force (issue #518 follow-up), the level kept beneath for a switch back. */
    opts->rtl_squelch_mode = DSD_SQUELCH_MODE_LEVEL;
    if (dsd_squelch_mode_is_dynamic(cfg->rtl_sql_mode)) {
        const dsd_squelch_setting dynamic = dsd_squelch_setting_dynamic(
            cfg->rtl_sql_mode, cfg->rtl_sql_margin_db > 0 ? cfg->rtl_sql_margin_db : DSD_SQUELCH_MARGIN_DEFAULT_DB);
        dsd_squelch_setting_store(opts, &dynamic);
    }
    svc_rtl_push_squelch(opts);
    if (dsd_user_config_rtl_gain_is_set(cfg)) {
        opts->rtl_gain_value = cfg->rtl_gain;
    }
    if (cfg->rtl_volume) {
        opts->rtl_volume_multiplier = cfg->rtl_volume;
    }
}

/* A config's gain under --trunk-scan is the configured gain the scan puts back at each switch, like a live edit: taken
   before the reopen, with the gain in force left at the parked target's own rtl_gain (@p before's) when it overrides,
   so the device reopens on the gain the target runs. Returns 1 with the configured gain it replaced in @p prev when
   it took the config's gain, for apply_cfg_settle_scan_gain(). Caller holds the P25 SM tick guard. */
static int
apply_cfg_stage_scan_gain(dsd_opts* opts, dsd_state* state, const dsdneoUserConfig* cfg, const ui_cfg_rollback* before,
                          int* prev) {
    if (!dsd_user_config_rtl_gain_is_set(cfg) || opts->trunk_scan_enabled != 1
        || state->trunk_scan_target_count == 0U) {
        return 0;
    }
    *prev = state->trunk_scan_configured_gain;
    if (dsd_engine_trunk_scan_set_configured_gain(opts, state, cfg->rtl_gain) == 0) {
        opts->rtl_gain_value = before->input.rtl_gain_value;
    }
    return 1;
}

/* An apply that ui_cfg_settle_reopen() rolled back (@p settled -1 with an input running before, which it put back) puts
   back the configured gain apply_cfg_stage_scan_gain() replaced. An apply it kept -- the reopened input runs, or it
   started with none running before, refused the scan row (1) or failed (-1), and the config stays applied -- keeps
   the config's. Returns @p settled. */
static int
apply_cfg_settle_scan_gain(dsd_opts* opts, dsd_state* state, const ui_cfg_rollback* before, int staged, int prev,
                           int settled) {
    if (staged && settled < 0 && before->input.running) {
        (void)dsd_engine_trunk_scan_set_configured_gain(opts, state, prev);
    }
    return settled;
}

/* A config whose [input] spec is the running input's reopens nothing for a new spec, but the gain it names still has to
   be the gain the input runs: a live gain edit does not rewrite the spec, and a SoapySDR spec carries no gain at all,
   so the spec alone cannot tell the gain changed. The gain is taken as a reopen takes it (under --trunk-scan as the
   configured gain, a parked target's own gain left in force), and the input restarts when the gain in force changed,
   with a reopen's rollback. An Airspy's gains are its own settings. Caller holds the P25 SM tick guard. */
static int
apply_cfg_rtl_live_gain(dsd_opts* opts, dsd_state* state, const dsdneoUserConfig* cfg, const ui_cfg_rollback* before) {
    /* The input keeps the frequency and DSP bandwidth it runs: a scanner may own the frequency (a config saved on
       another channel must not detune it), and a new DSP rate would want the width checks a reopen gets
       (cfg_radio_reopen() calls this restart one at the running rate). A SoapySDR config's own frequency and
       bandwidth, which its spec cannot carry, wait for an [input] that reopens the device. */
    opts->rtlsdr_center_freq = before->input.rtlsdr_center_freq;
    opts->rtl_dsp_bw_khz = before->input.rtl_dsp_bw_khz;
    if (!dsd_user_config_rtl_gain_is_set(cfg) || cfg->input_source == DSDCFG_INPUT_AIRSPY) {
        return 0;
    }
    opts->rtl_gain_value = cfg->rtl_gain;
    int prev_gain = 0;
    const int staged = apply_cfg_stage_scan_gain(opts, state, cfg, before, &prev_gain);
    if (opts->rtl_gain_value == before->input.rtl_gain_value) {
        return 0;
    }
    return apply_cfg_settle_scan_gain(opts, state, before, staged, prev_gain,
                                      ui_cfg_settle_reopen(opts, state, svc_rtl_restart_locked(opts, state), before));
}

/* A config's RTL-family [input] that builds a spec other than the RTL-family input's (@p before, the input the apply
   found) reopens the device with the config's tuning, or opens it when no stream ran. Returns what
   ui_cfg_settle_reopen() makes of the stream's start: -1 when it failed, or ran but refused the scan row on air (the
   running input put back, or none to put back), 1 when the input it opened with none running before refused that row,
   0 otherwise. */
static int
apply_cfg_rtl_hot_restart(dsd_opts* opts, dsd_state* state, const dsdneoUserConfig* cfg,
                          const ui_cfg_rollback* before) {
    const char* old_audio_in_dev = before->input.audio_in_dev;
    if (!cfg->has_input
        || (cfg->input_source != DSDCFG_INPUT_RTL && cfg->input_source != DSDCFG_INPUT_RTLTCP
            && cfg->input_source != DSDCFG_INPUT_SOAPY && cfg->input_source != DSDCFG_INPUT_AIRSPY)
        || before->input.audio_in_type != AUDIO_IN_RTL || opts->audio_in_type != AUDIO_IN_RTL) {
        return 0;
    }
    if (strncmp(old_audio_in_dev, opts->audio_in_dev, sizeof opts->audio_in_dev) == 0) {
        return apply_cfg_rtl_live_gain(opts, state, cfg, before);
    }

    if (cfg->input_source == DSDCFG_INPUT_RTL) {
        if (cfg->rtl_device >= 0) {
            opts->rtl_dev_index = cfg->rtl_device;
        }
        apply_cfg_rtl_common(opts, cfg);
        opts->rtltcp_enabled = 0;
    } else if (cfg->input_source == DSDCFG_INPUT_RTLTCP) {
        if (cfg->rtltcp_host[0]) {
            DSD_SNPRINTF(opts->rtltcp_hostname, sizeof opts->rtltcp_hostname, "%s", cfg->rtltcp_host);
        }
        if (cfg->rtltcp_port) {
            opts->rtltcp_portno = cfg->rtltcp_port;
        }
        apply_cfg_rtl_common(opts, cfg);
        opts->rtltcp_enabled = 1;
    } else { // DSDCFG_INPUT_SOAPY
        apply_cfg_rtl_common(opts, cfg);
        opts->rtltcp_enabled = 0;
    }
    int prev_gain = 0;
    const int staged = apply_cfg_stage_scan_gain(opts, state, cfg, before, &prev_gain);
    return apply_cfg_settle_scan_gain(opts, state, before, staged, prev_gain,
                                      ui_cfg_settle_reopen(opts, state, svc_rtl_restart_locked(opts, state), before));
}
#endif

/* What a config's reopen of the running input did (issue #634): CFG_REOPEN_NOTHING when it reopened nothing, else the
   dsd_audio_input_switch_result of the reopen. A reopen goes through the input switch, so one that fails keeps the
   running input, and the input name the config wrote is put back. */
enum { CFG_REOPEN_NOTHING = -1 };

static int
apply_cfg_tcp_hot_restart(dsd_opts* opts, dsd_state* state, const dsdneoUserConfig* cfg, const char* old_audio_in_dev,
                          int old_audio_in_type) {
    if (!cfg->has_input || cfg->input_source != DSDCFG_INPUT_TCP || old_audio_in_type != AUDIO_IN_TCP
        || strncmp(old_audio_in_dev, "tcp", 3) != 0 || strncmp(opts->audio_in_dev, "tcp", 3) != 0
        || strncmp(old_audio_in_dev, opts->audio_in_dev, sizeof opts->audio_in_dev) == 0) {
        return CFG_REOPEN_NOTHING;
    }

    char host[sizeof opts->tcp_hostname];
    DSD_SNPRINTF(host, sizeof host, "%s", cfg->tcp_host[0] ? cfg->tcp_host : opts->tcp_hostname);
    const int port = cfg->tcp_port ? cfg->tcp_port : opts->tcp_portno;
    const int rc = svc_tcp_connect_audio(opts, state, host, port);
    if (rc != DSD_AUDIO_INPUT_SWITCHED) {
        DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", old_audio_in_dev);
        LOG_ERROR("Config: failed to reconnect TCP audio %s:%d\n", host, port);
    }
    return rc;
}

static int
apply_cfg_udp_hot_restart(dsd_opts* opts, dsd_state* state, const dsdneoUserConfig* cfg, const char* old_audio_in_dev,
                          int old_audio_in_type) {
    if (!cfg->has_input || cfg->input_source != DSDCFG_INPUT_UDP || old_audio_in_type != AUDIO_IN_UDP
        || strncmp(old_audio_in_dev, "udp", 3) != 0 || strncmp(opts->audio_in_dev, "udp", 3) != 0
        || strncmp(old_audio_in_dev, opts->audio_in_dev, sizeof opts->audio_in_dev) == 0) {
        return CFG_REOPEN_NOTHING;
    }

    /* The endpoint the config names, over the running one's; the running one stays in the options, for the switch to
       bind again should the new one not bind. */
    char bindaddr[sizeof opts->udp_in_bindaddr];
    DSD_SNPRINTF(bindaddr, sizeof bindaddr, "%s",
                 cfg->udp_addr[0] ? cfg->udp_addr : (opts->udp_in_bindaddr[0] ? opts->udp_in_bindaddr : "127.0.0.1"));
    const int port = cfg->udp_port ? cfg->udp_port : (opts->udp_in_portno ? opts->udp_in_portno : 7355);
    const int rc = svc_udp_input_config(opts, state, bindaddr, port);
    if (rc != DSD_AUDIO_INPUT_SWITCHED) {
        DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", old_audio_in_dev);
        LOG_ERROR("Config: failed to restart UDP input %s:%d\n", bindaddr, port);
    }
    return rc;
}

static void
rollback_cfg_file_hot_restart(dsd_opts* opts, dsd_state* state, const char* old_audio_in_dev, int old_audio_in_type,
                              int old_wav_sample_rate, int old_effective_input_rate, int failed_effective_input_rate) {
    if (!opts) {
        return;
    }

    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", old_audio_in_dev);
    opts->audio_in_type = old_audio_in_type;
    dsd_opts_apply_input_sample_rate(opts, old_wav_sample_rate);

    if (state && failed_effective_input_rate != old_effective_input_rate) {
        dsd_state_rescale_symbol_timing(state, failed_effective_input_rate, old_effective_input_rate);
    }
}

static void
restore_symbol_timing_snapshot(dsd_state* state, int old_samples_per_symbol, int old_symbol_center, int old_jitter) {
    if (!state) {
        return;
    }

    int restored_sps = old_samples_per_symbol > 0 ? old_samples_per_symbol : 10;
    int restored_center = old_symbol_center;
    if (restored_center < 0 || restored_center >= restored_sps) {
        restored_center = dsd_opts_symbol_center(restored_sps);
    }

    state->samplesPerSymbol = restored_sps;
    state->symbolCenter = restored_center;
    state->jitter = old_jitter;
}

static void
restore_live_pcm_rate_after_staged_file_apply(dsd_opts* opts, const dsdneoUserConfig* cfg, int old_wav_sample_rate) {
    if (!opts || !cfg || !cfg->has_input || cfg->input_source != DSDCFG_INPUT_FILE || !cfg->file_path[0]) {
        return;
    }
    if (opts->audio_in_type == AUDIO_IN_WAV) {
        return;
    }

    opts->staged_file_sample_rate = (cfg->file_sample_rate > 0) ? cfg->file_sample_rate : 48000;
    if (opts->wav_sample_rate != old_wav_sample_rate) {
        dsd_opts_apply_input_sample_rate(opts, old_wav_sample_rate);
    }
}

static int
audio_file_info_uses_container_metadata(const SF_INFO* info) {
    if (!info) {
        return 0;
    }

    int major_format = info->format & SF_FORMAT_TYPEMASK;
    return major_format != 0 && major_format != SF_FORMAT_RAW;
}

static int
cfg_file_runtime_should_apply(const dsd_opts* opts, const dsd_state* state, const dsdneoUserConfig* cfg) {
    return opts && state && cfg && cfg->has_input && cfg->input_source == DSDCFG_INPUT_FILE && cfg->file_path[0];
}

static int
cfg_file_runtime_configured_rate(const dsd_opts* opts) {
    int configured_effective_input_rate = dsd_opts_effective_input_rate(opts);
    if (configured_effective_input_rate <= 0) {
        configured_effective_input_rate = 48000;
    }
    return configured_effective_input_rate;
}

static void
cfg_file_runtime_apply_wav_input(dsd_opts* opts, dsd_state* state, const dsdneoUserConfig* cfg,
                                 int old_samples_per_symbol, int old_symbol_center, int old_jitter,
                                 int configured_effective_input_rate) {
    if (strncmp(opts->audio_in_dev, cfg->file_path, sizeof opts->audio_in_dev) != 0) {
        restore_symbol_timing_snapshot(state, old_samples_per_symbol, old_symbol_center, old_jitter);
        return;
    }

    const int raw_sample_rate = opts->wav_sample_rate;
    int active_sample_rate = opts->wav_sample_rate;
    if (audio_file_info_uses_container_metadata(opts->audio_in_file_info) && opts->audio_in_file_info->samplerate > 0) {
        active_sample_rate = opts->audio_in_file_info->samplerate;
    }
    if (active_sample_rate <= 0) {
        active_sample_rate = 48000;
    }
    if (opts->wav_sample_rate != active_sample_rate) {
        dsd_opts_apply_input_sample_rate(opts, active_sample_rate);
    }
    /* A header rate holds for this file only (issue #634). */
    opts->wav_header_replaced_rate =
        (raw_sample_rate > 0 && active_sample_rate != raw_sample_rate) ? raw_sample_rate : 0;

    int active_effective_input_rate = dsd_opts_effective_input_rate(opts);
    if (cfg->has_mode) {
        dsd_apply_decode_mode_symbol_timing(cfg->decode_mode, active_effective_input_rate, state);
        dsd_audio_rescale_symbol_timing(state, active_effective_input_rate, active_effective_input_rate);
        return;
    }
    dsd_audio_rescale_symbol_timing(state, configured_effective_input_rate, active_effective_input_rate);
}

static void
cfg_file_runtime_apply_live_input(const dsd_opts* opts, dsd_state* state, const dsdneoUserConfig* cfg,
                                  int old_runtime_input_rate, int old_samples_per_symbol, int old_symbol_center,
                                  int old_jitter) {
    if (old_runtime_input_rate <= 0) {
        old_runtime_input_rate = current_demod_rate(opts, state);
    }
    if (old_runtime_input_rate <= 0) {
        old_runtime_input_rate = 48000;
    }

    if (cfg->has_mode) {
        dsd_apply_decode_mode_symbol_timing(cfg->decode_mode, old_runtime_input_rate, state);
        return;
    }
    restore_symbol_timing_snapshot(state, old_samples_per_symbol, old_symbol_center, old_jitter);
}

static void
apply_cfg_file_runtime_rate(dsd_opts* opts, dsd_state* state, const dsdneoUserConfig* cfg, int old_runtime_input_rate,
                            int old_samples_per_symbol, int old_symbol_center, int old_jitter) {
    if (!cfg_file_runtime_should_apply(opts, state, cfg)) {
        return;
    }

    const int configured_effective_input_rate = cfg_file_runtime_configured_rate(opts);
    if (opts->audio_in_type == AUDIO_IN_WAV) {
        cfg_file_runtime_apply_wav_input(opts, state, cfg, old_samples_per_symbol, old_symbol_center, old_jitter,
                                         configured_effective_input_rate);
        return;
    }

    cfg_file_runtime_apply_live_input(opts, state, cfg, old_runtime_input_rate, old_samples_per_symbol,
                                      old_symbol_center, old_jitter);
}

static int
apply_cfg_file_hot_restart(dsd_opts* opts, dsd_state* state, const dsdneoUserConfig* cfg, const char* old_audio_in_dev,
                           int old_audio_in_type, int old_wav_sample_rate, int old_effective_input_rate) {
    if (!cfg->has_input || cfg->input_source != DSDCFG_INPUT_FILE || old_audio_in_type != AUDIO_IN_WAV
        || strncmp(old_audio_in_dev, opts->audio_in_dev, sizeof opts->audio_in_dev) == 0) {
        return CFG_REOPEN_NOTHING;
    }

    SNDFILE* new_audio_in_file = NULL;
    SF_INFO* new_audio_in_file_info = NULL;
    int configured_effective_input_rate = dsd_opts_effective_input_rate(opts);

    /* A pipe or device would block the decoder thread in its open until a writer turns up: startup (-i) only, as for
       a menu switch (issue #634). */
    dsd_stat_t st;
    if (dsd_stat_path(opts->audio_in_dev, &st) != 0 || !dsd_stat_is_regular(&st)) {
        LOG_ERROR("Config: file input %s is not a regular file (a pipe or device can be given at startup with -i)\n",
                  opts->audio_in_dev);
        rollback_cfg_file_hot_restart(opts, state, old_audio_in_dev, old_audio_in_type, old_wav_sample_rate,
                                      old_effective_input_rate, configured_effective_input_rate);
        return DSD_AUDIO_INPUT_KEPT;
    }
    if (dsd_audio_open_mono_file_input(opts->audio_in_dev, opts->wav_sample_rate, &new_audio_in_file,
                                       &new_audio_in_file_info, NULL, NULL)
        != 0) {
        LOG_ERROR("Config: failed to open file input %s: %s\n", opts->audio_in_dev, sf_strerror(NULL));
        rollback_cfg_file_hot_restart(opts, state, old_audio_in_dev, old_audio_in_type, old_wav_sample_rate,
                                      old_effective_input_rate, configured_effective_input_rate);
        return DSD_AUDIO_INPUT_KEPT;
    }

    if (opts->audio_in_file) {
        sf_close(opts->audio_in_file);
    }
    free(opts->audio_in_file_info);

    opts->audio_in_file = new_audio_in_file;
    opts->audio_in_file_info = new_audio_in_file_info;
    opts->audio_in_type = AUDIO_IN_WAV;
    dsd_opts_reset_pcm_input_state(opts);
    return DSD_AUDIO_INPUT_SWITCHED;
}

static int
apply_cfg_pulse_in_hot_restart(dsd_opts* opts, dsd_state* state, const dsdneoUserConfig* cfg,
                               const char* old_audio_in_dev, int old_audio_in_type) {
    if (!cfg->has_input || cfg->input_source != DSDCFG_INPUT_PULSE || old_audio_in_type != AUDIO_IN_PULSE
        || opts->audio_in_type != AUDIO_IN_PULSE) {
        return CFG_REOPEN_NOTHING;
    }
    if (strncmp(old_audio_in_dev, opts->audio_in_dev, sizeof opts->audio_in_dev) == 0
        && strncmp(old_audio_in_dev, "pulse", 5) == 0) {
        return CFG_REOPEN_NOTHING;
    }

    /* The device the spec names (its first ':' field, as parse_audio_input_string() reads it), or the default. */
    char device[128] = {0};
    if (strncmp(opts->audio_in_dev, "pulse", 5) == 0 && opts->audio_in_dev[5] == ':' && opts->audio_in_dev[6] != '\0') {
        DSD_SNPRINTF(device, sizeof device, "%s", opts->audio_in_dev + 6);
        char* colon = strchr(device, ':');
        if (colon) {
            *colon = '\0';
        }
    }
    const int rc = svc_set_pulse_input(opts, state, device);
    if (rc != DSD_AUDIO_INPUT_SWITCHED) {
        DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", old_audio_in_dev);
        LOG_ERROR("Config: failed to open PulseAudio input\n");
    }
    return rc;
}

static void
apply_cfg_pulse_out_hot_restart(dsd_opts* opts, const dsdneoUserConfig* cfg, const char* old_audio_out_dev,
                                int old_audio_out_type) {
    if (!cfg->has_output || cfg->output_backend != DSDCFG_OUTPUT_PULSE || old_audio_out_type != 0
        || opts->audio_out_type != 0) {
        return;
    }
    if (strncmp(old_audio_out_dev, opts->audio_out_dev, sizeof opts->audio_out_dev) == 0
        && strncmp(old_audio_out_dev, "pulse", 5) == 0) {
        return;
    }

    closeAudioOutput(opts);
    if (strncmp(opts->audio_out_dev, "pulse", 5) == 0 && opts->audio_out_dev[5] == ':'
        && opts->audio_out_dev[6] != '\0') {
        char tmp[128] = {0};
        DSD_SNPRINTF(tmp, sizeof tmp, "%s", opts->audio_out_dev + 6);
        parse_audio_output_string(opts, tmp);
    } else {
        opts->pa_output_idx[0] = '\0';
    }
    if (openAudioOutput(opts) != 0) {
        LOG_ERROR("Config: failed to open PulseAudio output\n");
    }
}

/* An Airspy [input] over a running Airspy applies live rather than reopening it. Pure option logic, so the config
   check (cfg_radio_reopen()) reads it in every build. */
static int
cfg_is_live_airspy(const dsdneoUserConfig* cfg, const char* old_device, int old_type) {
    return cfg && cfg->has_input && cfg->input_source == DSDCFG_INPUT_AIRSPY && old_type == AUDIO_IN_RTL
           && dsd_opts_audio_in_dev_is_airspy_spec(old_device);
}

#ifdef USE_RADIO
/*
 * The RTL-family input a config's [input] reopens or retunes, @p before being the input the apply found. An Airspy
 * source over a running Airspy applies what it can live (svc_airspy_apply_config_locked()). A change the stream only
 * takes when it opens (a new sample rate or serial, DSP bandwidth or monitor volume, svc_airspy_settings_reopen())
 * reopens the running Airspy here instead, where a start that fails puts back everything the config applied to the
 * receive side (ui_cfg_settle_reopen()), the widths included: the Airspy path's own rollback keeps the width it
 * refused, so its restart is refused too. An Airspy with no stream running opens here the same way, where a start that
 * fails has nothing to put back and says why. Any other RTL-family source reopens the input for a new spec
 * (apply_cfg_rtl_hot_restart(), with or without a stream running).
 * The configured PPM never outlives a rollback: an RTL-SDR or rtl_tcp reopen opens with the request made before it
 * (the spec carries the PPM, the options it opens with do not), which the rollback puts back with the input, and the
 * Airspy path asks for it only once its input runs. Returns nonzero when a start failed or its stream refused the scan
 * row on air (ui_cfg_settle_reopen(): -1 with the input put back, or none to put back; 1 with the input it opened kept,
 * none having run before), or the live Airspy path failed, which fails the apply.
 */
static int
apply_cfg_radio_input_tuned(dsd_opts* opts, dsd_state* state, const dsdneoUserConfig* cfg,
                            const ui_cfg_rollback* before) {
    const ui_radio_input* in = &before->input;
    if (!cfg_is_live_airspy(cfg, in->audio_in_dev, in->audio_in_type)) {
        apply_cfg_live_rtl_ppm_request(opts, cfg, in->audio_in_type);
        return apply_cfg_rtl_hot_restart(opts, state, cfg, before);
    }
    /* The Airspy path compares the settings the config asks for with the ones the Airspy runs. */
    opts->airspy = in->airspy;
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", in->audio_in_dev);
    const svc_airspy_tuning tuning = {in->rtlsdr_center_freq,    in->rtl_dsp_bw_khz,   in->rtl_squelch_level,
                                      in->rtl_volume_multiplier, in->rtl_squelch_mode, in->rtl_squelch_margin_db};
    int rc = 0;
    if (!state->rtl_ctx
        || svc_airspy_settings_reopen(&in->airspy, &cfg->airspy, tuning.bandwidth, opts->rtl_dsp_bw_khz, tuning.volume,
                                      opts->rtl_volume_multiplier)) {
        rc = ui_cfg_settle_reopen(opts, state, svc_airspy_reopen_locked(opts, state, &cfg->airspy), before);
        if (rc < 0) {
            return rc;
        }
    } else {
        rc = svc_airspy_apply_config_locked(opts, state, &cfg->airspy, &tuning);
    }
    apply_cfg_live_rtl_ppm_request(opts, cfg, in->audio_in_type);
    return rc;
}

/* apply_cfg_radio_input_tuned(), then the radio input on another centre leaves the old carrier (issue #575): a reopen
   on the config's frequency, or the live Airspy retune, keeps the input a radio input, so no input boundary or
   no-carrier pass ends the reception before the next carrier syncs. A start that failed put the old centre back, and
   its calls and codes stay. */
static int
apply_cfg_radio_input(dsd_opts* opts, dsd_state* state, const dsdneoUserConfig* cfg, const ui_cfg_rollback* before) {
    const int rc = apply_cfg_radio_input_tuned(opts, state, cfg, before);
    if (before->input.audio_in_type == AUDIO_IN_RTL && opts->audio_in_type == AUDIO_IN_RTL
        && opts->rtlsdr_center_freq != before->input.rtlsdr_center_freq) {
        ui_leave_carrier(opts, state, 1); // a config apply runs under the P25 SM tick guard
    }
    return rc;
}
#endif

/* A reopen that replaced the stream the decoder read (issue #634): the engine ends the reception before it decodes the
   new one. A reopen that restarted the running UDP input, or lost it, puts back the options the config wrote, so the
   received-tone check that compares them (cfg_forget_rx_tone_on_boundary()) would see no change: the boundary is
   taken here. A reopen that failed applied none of the config's input, so the file rate it staged for later file opens
   goes back to @p old_staged_file_sample_rate as well. */
static void
apply_cfg_note_reopen(dsd_opts* opts, dsd_state* state, int rc, int old_staged_file_sample_rate) {
    if (rc == CFG_REOPEN_NOTHING) {
        return;
    }
    if (rc != DSD_AUDIO_INPUT_SWITCHED) {
        opts->staged_file_sample_rate = old_staged_file_sample_rate;
    }
    if (rc == DSD_AUDIO_INPUT_KEPT) {
        return;
    }
    state->input_boundary = 1;
    if (rc == DSD_AUDIO_INPUT_RESTARTED || rc == DSD_AUDIO_INPUT_LOST) {
        ui_input_left(opts, state);
    }
}

/* The input options a config apply found, before it wrote its own. */
typedef struct {
    const char* audio_in_dev;
    int audio_in_type;
    int wav_sample_rate;
    int effective_input_rate;
    int staged_file_sample_rate;
} cfg_input_before;

static void
apply_cfg_runtime_hot_switches(dsd_opts* opts, dsd_state* state, const dsdneoUserConfig* cfg,
                               const cfg_input_before* in, const char* old_audio_out_dev, int old_audio_out_type) {
    /* Tighten runtime behavior when applying configs mid-run by restarting
     * active backends whose configuration changed, while avoiding cross-backend
     * hot-switches. The running RTL-family input is reopened before these, by apply_cfg_radio_input(). */
    const int staged = in->staged_file_sample_rate;
    apply_cfg_note_reopen(opts, state, apply_cfg_tcp_hot_restart(opts, state, cfg, in->audio_in_dev, in->audio_in_type),
                          staged);
    apply_cfg_note_reopen(opts, state, apply_cfg_udp_hot_restart(opts, state, cfg, in->audio_in_dev, in->audio_in_type),
                          staged);
    apply_cfg_note_reopen(opts, state,
                          apply_cfg_file_hot_restart(opts, state, cfg, in->audio_in_dev, in->audio_in_type,
                                                     in->wav_sample_rate, in->effective_input_rate),
                          staged);
    apply_cfg_note_reopen(
        opts, state, apply_cfg_pulse_in_hot_restart(opts, state, cfg, in->audio_in_dev, in->audio_in_type), staged);
    apply_cfg_pulse_out_hot_restart(opts, cfg, old_audio_out_dev, old_audio_out_type);
}

int
dsd_app_command_submit(int cmd_id, const void* payload, size_t payload_sz) {
    ensure_mu_init();
    dsd_mutex_lock(&g_mu);
    if (!g_session_open) {
        dsd_mutex_unlock(&g_mu);
        return DSD_APP_COMMAND_SUBMIT_REJECTED;
    }
    if (ui_cmd_is_coalescible_setter(cmd_id)) {
        struct dsd_app_command* pending = ui_cmd_find_pending_tail_unlocked(cmd_id);
        if (ui_cmd_coalesces_with(pending, cmd_id, payload, payload_sz)) {
            ui_cmd_store_payload(pending, cmd_id, payload, payload_sz);
            dsd_mutex_unlock(&g_mu);
            return DSD_APP_COMMAND_SUBMIT_COALESCED;
        }
    }
    if (q_is_full_unlocked()) {
        tg_export_publish_result_unlocked(&g_q[g_head], UI_CMD_APPLY_FAILED);
        dsd_app_publish_decryption_result(&g_q[g_head], DSD_APP_KEY_CANCELLED);
        // Erase before advancing: the evicted slot is not the slot about to be
        // written, so clearing only the insertion slot leaves a discarded key alive.
        DSD_SECURE_ZERO(&g_q[g_head], sizeof(g_q[g_head]));
        // Drop the oldest command (advance head) and warn once per burst
        g_head = (g_head + 1) % DSD_APP_CMD_Q_CAP;
        atomic_fetch_add(&g_overflow, 1);
        if (atomic_exchange(&g_overflow_warn_gate, 1) == 0) {
            LOG_WARN("WARNING: app_command_queue: overflow; dropping oldest command(s).\n");
        }
    }
    struct dsd_app_command* c = &g_q[g_tail];
    ui_cmd_store_payload(c, cmd_id, payload, payload_sz);
    g_tail = (g_tail + 1) % DSD_APP_CMD_Q_CAP;
    dsd_mutex_unlock(&g_mu);
    return DSD_APP_COMMAND_SUBMIT_QUEUED;
}

static const int k_ui_cmd_action_ids[] = {
    DSD_APP_CMD_TOGGLE_MUTE,
    DSD_APP_CMD_TOGGLE_COMPACT,
    DSD_APP_CMD_HISTORY_CYCLE,
    DSD_APP_CMD_SLOT1_TOGGLE,
    DSD_APP_CMD_SLOT2_TOGGLE,
    DSD_APP_CMD_SLOT_PREF_CYCLE,
    DSD_APP_CMD_TRUNK_TOGGLE,
    DSD_APP_CMD_SCANNER_TOGGLE,
    DSD_APP_CMD_TUNER_RELEASE,
    DSD_APP_CMD_SCAN_HOLD_TOGGLE,
    DSD_APP_CMD_SCAN_AVOID,
    DSD_APP_CMD_SCAN_AVOID_CLEAR,
    DSD_APP_CMD_PAYLOAD_TOGGLE,
    DSD_APP_CMD_P25_GA_TOGGLE,
    DSD_APP_CMD_LPF_TOGGLE,
    DSD_APP_CMD_HPF_TOGGLE,
    DSD_APP_CMD_PBF_TOGGLE,
    DSD_APP_CMD_HPF_D_TOGGLE,
    DSD_APP_CMD_AGGR_SYNC_TOGGLE,
    DSD_APP_CMD_CALL_ALERT_TOGGLE,
    DSD_APP_CMD_CONST_TOGGLE,
    DSD_APP_CMD_CONST_NORM_TOGGLE,
    DSD_APP_CMD_EYE_TOGGLE,
    DSD_APP_CMD_EYE_UNICODE_TOGGLE,
    DSD_APP_CMD_EYE_COLOR_TOGGLE,
    DSD_APP_CMD_FSK_HIST_TOGGLE,
    DSD_APP_CMD_SPECTRUM_TOGGLE,
    DSD_APP_CMD_INPUT_VOL_CYCLE,
    DSD_APP_CMD_EH_NEXT,
    DSD_APP_CMD_EH_PREV,
    DSD_APP_CMD_EH_TOGGLE_SLOT,
    DSD_APP_CMD_INVERT_TOGGLE,
    DSD_APP_CMD_MOD_TOGGLE,
    DSD_APP_CMD_DMR_RESET,
    DSD_APP_CMD_INPUT_MONITOR_TOGGLE,
    DSD_APP_CMD_COSINE_FILTER_TOGGLE,
    DSD_APP_CMD_TCP_CONNECT_AUDIO,
    DSD_APP_CMD_RIGCTL_CONNECT,
    DSD_APP_CMD_RETURN_CC,
    DSD_APP_CMD_CHANNEL_CYCLE,
    DSD_APP_CMD_SYMCAP_SAVE,
    DSD_APP_CMD_SYMCAP_STOP,
    DSD_APP_CMD_REPLAY_LAST,
    DSD_APP_CMD_WAV_START,
    DSD_APP_CMD_WAV_STOP,
    DSD_APP_CMD_WAV_TOGGLE,
    DSD_APP_CMD_STOP_PLAYBACK,
    DSD_APP_CMD_TRUNK_WLIST_TOGGLE,
    DSD_APP_CMD_TRUNK_PRIV_TOGGLE,
    DSD_APP_CMD_TRUNK_DATA_TOGGLE,
    DSD_APP_CMD_TRUNK_ENC_TOGGLE,
    DSD_APP_CMD_ENC_LOCKOUT_CLEAR,
    /* The frontend "None" selection unloads an imported CSV through these; a bridge
       submits them with dsd_app_command_action(), which admits only this list. */
    DSD_APP_CMD_IMPORT_CHANNEL_MAP_CLEAR,
    DSD_APP_CMD_IMPORT_GROUP_LIST_CLEAR,
    DSD_APP_CMD_IMPORT_KEYS_CLEAR,
    DSD_APP_CMD_IMPORT_SRC_LIST_CLEAR,
    DSD_APP_CMD_QUIT,
    DSD_APP_CMD_FORCE_PRIV_TOGGLE,
    DSD_APP_CMD_FORCE_RC4_TOGGLE,
    DSD_APP_CMD_TRUNK_GROUP_TOGGLE,
    DSD_APP_CMD_SIM_NOCAR,
    DSD_APP_CMD_MOD_P2_TOGGLE,
    DSD_APP_CMD_M17_TX_TOGGLE,
    DSD_APP_CMD_PROVOICE_ESK_TOGGLE,
    DSD_APP_CMD_PROVOICE_MODE_TOGGLE,
    DSD_APP_CMD_UI_MSG_CLEAR,
    DSD_APP_CMD_EH_RESET,
    DSD_APP_CMD_EVENT_LOG_DISABLE,
    DSD_APP_CMD_LCW_RETUNE_TOGGLE,
    DSD_APP_CMD_P25_CC_CAND_TOGGLE,
    DSD_APP_CMD_REVERSE_MUTE_TOGGLE,
    DSD_APP_CMD_DMR_LE_TOGGLE,
    DSD_APP_CMD_ALL_MUTES_TOGGLE,
    DSD_APP_CMD_INV_X2_TOGGLE,
    DSD_APP_CMD_INV_DMR_TOGGLE,
    DSD_APP_CMD_INV_DPMR_TOGGLE,
    DSD_APP_CMD_INV_M17_TOGGLE,
    DSD_APP_CMD_INPUT_SET_PULSE,
    DSD_APP_CMD_AIRSPY_ENABLE_INPUT,
    DSD_APP_CMD_RTL_ENABLE_INPUT,
    DSD_APP_CMD_RTL_RESTART,
    DSD_APP_CMD_LRRP_SET_HOME,
    DSD_APP_CMD_LRRP_SET_DSDP,
    DSD_APP_CMD_LRRP_DISABLE,
    DSD_APP_CMD_UI_SHOW_DSP_PANEL_TOGGLE,
    DSD_APP_CMD_UI_SHOW_P25_METRICS_TOGGLE,
    DSD_APP_CMD_UI_SHOW_P25_AFFIL_TOGGLE,
    DSD_APP_CMD_UI_SHOW_P25_NEIGHBORS_TOGGLE,
    DSD_APP_CMD_UI_SHOW_P25_IDEN_TOGGLE,
    DSD_APP_CMD_UI_SHOW_P25_CCC_TOGGLE,
    DSD_APP_CMD_UI_SHOW_CHANNELS_TOGGLE,
    DSD_APP_CMD_UI_SHOW_P25_CALLSIGN_TOGGLE,
};

static const int k_ui_cmd_i32_ids[] = {
    DSD_APP_CMD_TG_LOCKOUT_PERSIST_SET,
    DSD_APP_CMD_FORCE_KEY_SET,
    DSD_APP_CMD_GAIN_DELTA,
    DSD_APP_CMD_AGAIN_DELTA,
    DSD_APP_CMD_SPEC_SIZE_DELTA,
    DSD_APP_CMD_PPM_DELTA,
    DSD_APP_CMD_GAIN_SET,
    DSD_APP_CMD_AGAIN_SET,
    DSD_APP_CMD_RTL_SET_DEV,
    DSD_APP_CMD_RTL_SET_GAIN,
    DSD_APP_CMD_RTL_SET_PPM,
    DSD_APP_CMD_RTL_SET_BW,
    DSD_APP_CMD_RTL_SET_VOL_MULT,
    DSD_APP_CMD_RTL_SET_BIAS_TEE,
    DSD_APP_CMD_RTLTCP_SET_AUTOTUNE,
    DSD_APP_CMD_RTL_SET_AUTO_PPM,
    DSD_APP_CMD_RIGCTL_SET_MOD_BW,
    DSD_APP_CMD_SLOT_PREF_SET,
    DSD_APP_CMD_SLOTS_ONOFF_SET,
    DSD_APP_CMD_SCAN_VOICE_ONLY_SET,
    DSD_APP_CMD_SCAN_VOICE_QUALIFY_MS_SET,
    DSD_APP_CMD_SCAN_VOICE_HOLD_MS_SET,
    DSD_APP_CMD_NFM_BANDWIDTH_SET,
    DSD_APP_CMD_AM_BANDWIDTH_SET,
    DSD_APP_CMD_INPUT_VOL_SET,
    DSD_APP_CMD_MOD_SET,
    DSD_APP_CMD_DECODE_MODE_SET,
    DSD_APP_CMD_TRUNK_SET,
};

static int
ui_cmd_id_in_list(int cmd_id, const int* ids, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (ids[i] == cmd_id) {
            return 1;
        }
    }
    return 0;
}

static int
ui_cmd_is_action_id(int cmd_id) {
    return ui_cmd_id_in_list(cmd_id, k_ui_cmd_action_ids, sizeof k_ui_cmd_action_ids / sizeof k_ui_cmd_action_ids[0]);
}

int
dsd_app_command_action(int cmd_id) {
    return ui_cmd_is_action_id(cmd_id) ? dsd_app_command_submit(cmd_id, NULL, 0U) : DSD_APP_COMMAND_SUBMIT_REJECTED;
}

int
dsd_app_command_set_i32(int cmd_id, int32_t value) {
    return ui_cmd_id_in_list(cmd_id, k_ui_cmd_i32_ids, sizeof k_ui_cmd_i32_ids / sizeof k_ui_cmd_i32_ids[0])
               ? dsd_app_command_submit(cmd_id, &value, sizeof value)
               : DSD_APP_COMMAND_SUBMIT_REJECTED;
}

int
dsd_app_command_set_u8(int cmd_id, uint8_t value) {
    switch (cmd_id) {
        case DSD_APP_CMD_TG_HOLD_TOGGLE:
        case DSD_APP_CMD_CALL_ALERT_EVENTS_SET:
        case DSD_APP_CMD_SKIP_SLOT:
        case DSD_APP_CMD_LOCKOUT_SLOT: return dsd_app_command_submit(cmd_id, &value, sizeof value);
        default: return DSD_APP_COMMAND_SUBMIT_REJECTED;
    }
}

int
dsd_app_command_set_u32(int cmd_id, uint32_t value) {
    switch (cmd_id) {
        case DSD_APP_CMD_RTL_SET_FREQ:
        case DSD_APP_CMD_MANUAL_TUNE:
        case DSD_APP_CMD_TG_HOLD_SET:
        case DSD_APP_CMD_KEY_BASIC_SET:
        case DSD_APP_CMD_KEY_SCRAMBLER_SET: return dsd_app_command_submit(cmd_id, &value, sizeof value);
        default: return DSD_APP_COMMAND_SUBMIT_REJECTED;
    }
}

int
dsd_app_command_set_u64(int cmd_id, uint64_t value) {
    if (cmd_id != DSD_APP_CMD_KEY_RC4DES_SET) {
        return DSD_APP_COMMAND_SUBMIT_REJECTED;
    }
    return dsd_app_command_submit(cmd_id, &value, sizeof value);
}

int
dsd_app_command_set_double(int cmd_id, double value) {
    switch (cmd_id) {
        case DSD_APP_CMD_INPUT_WARN_DB_SET:
        case DSD_APP_CMD_RTL_SET_SQL_DB:
        case DSD_APP_CMD_HANGTIME_SET: return dsd_app_command_submit(cmd_id, &value, sizeof value);
        default: return DSD_APP_COMMAND_SUBMIT_REJECTED;
    }
}

int
dsd_app_command_set_float(int cmd_id, float value) {
    if (cmd_id != DSD_APP_CMD_CONST_GATE_DELTA) {
        return DSD_APP_COMMAND_SUBMIT_REJECTED;
    }
    return dsd_app_command_submit(cmd_id, &value, sizeof value);
}

int
dsd_app_command_set_string(int cmd_id, const char* value) {
    if (!value) {
        return DSD_APP_COMMAND_SUBMIT_REJECTED;
    }
    return ui_cmd_id_in_list(cmd_id, k_ui_cmd_string_ids, sizeof k_ui_cmd_string_ids / sizeof k_ui_cmd_string_ids[0])
               ? dsd_app_command_submit(cmd_id, value, strlen(value) + 1U)
               : DSD_APP_COMMAND_SUBMIT_REJECTED;
}

int
dsd_app_command_set_endpoint(int cmd_id, const char* host, int32_t port) {
    if (!host) {
        return DSD_APP_COMMAND_SUBMIT_REJECTED;
    }
    switch (cmd_id) {
        case DSD_APP_CMD_UDP_OUT_CFG:
        case DSD_APP_CMD_TCP_CONNECT_AUDIO_CFG:
        case DSD_APP_CMD_RIGCTL_CONNECT_CFG: {
            dsd_app_endpoint_payload payload = {0};
            DSD_SNPRINTF(payload.host, sizeof payload.host, "%s", host);
            payload.port = port;
            return dsd_app_command_submit(cmd_id, &payload, sizeof payload);
        }
        case DSD_APP_CMD_UDP_INPUT_CFG: {
            dsd_app_udp_input_payload payload = {0};
            DSD_SNPRINTF(payload.bind, sizeof payload.bind, "%s", host);
            payload.port = port;
            return dsd_app_command_submit(cmd_id, &payload, sizeof payload);
        }
        default: return DSD_APP_COMMAND_SUBMIT_REJECTED;
    }
}

int
dsd_app_command_set_p25_p2_params(const dsd_app_p25_p2_params_payload* payload) {
    return payload ? dsd_app_command_submit(DSD_APP_CMD_P25_P2_PARAMS_SET, payload, sizeof *payload)
                   : DSD_APP_COMMAND_SUBMIT_REJECTED;
}

int
dsd_app_command_set_tg_listen(const dsd_app_tg_listen_payload* payload) {
    return payload ? dsd_app_command_submit(DSD_APP_CMD_TG_LISTEN_SET, payload, sizeof *payload)
                   : DSD_APP_COMMAND_SUBMIT_REJECTED;
}

int
dsd_app_command_set_tg_listen_all(const dsd_app_tg_listen_all_payload* payload) {
    return payload ? dsd_app_command_submit(DSD_APP_CMD_TG_LISTEN_SET_ALL, payload, sizeof *payload)
                   : DSD_APP_COMMAND_SUBMIT_REJECTED;
}

int
dsd_app_command_set_hytera_key(const dsd_app_hytera_key_payload* payload) {
    return payload ? dsd_app_command_submit(DSD_APP_CMD_KEY_HYTERA_SET, payload, sizeof *payload)
                   : DSD_APP_COMMAND_SUBMIT_REJECTED;
}

int
dsd_app_command_set_aes_key(const dsd_app_aes_key_payload* payload) {
    return payload ? dsd_app_command_submit(DSD_APP_CMD_KEY_AES_SET, payload, sizeof *payload)
                   : DSD_APP_COMMAND_SUBMIT_REJECTED;
}

int
dsd_app_command_dsp_op(const dsd_app_dsp_payload* payload) {
    return payload ? dsd_app_command_submit(DSD_APP_CMD_DSP_OP, payload, sizeof *payload)
                   : DSD_APP_COMMAND_SUBMIT_REJECTED;
}

int
dsd_app_command_apply_config(const dsdneoUserConfig* config) {
    return config ? dsd_app_command_submit(DSD_APP_CMD_CONFIG_APPLY, config, sizeof *config)
                  : DSD_APP_COMMAND_SUBMIT_REJECTED;
}

int
dsd_app_command_set_config_metadata(const dsd_app_config_metadata_payload* payload) {
    return payload ? dsd_app_command_submit(DSD_APP_CMD_CONFIG_METADATA_SET, payload, sizeof *payload)
                   : DSD_APP_COMMAND_SUBMIT_REJECTED;
}

int
dsd_app_command_set_tone_filter(int32_t mode, const char* list) {
    dsd_app_tone_filter_payload payload;
    DSD_MEMSET(&payload, 0, sizeof payload);
    payload.mode = mode;
    const char* text = list ? list : "";
    const size_t len = strlen(text);
    if (len >= sizeof payload.list) {
        return DSD_APP_COMMAND_SUBMIT_REJECTED;
    }
    DSD_MEMCPY(payload.list, text, len);
    return dsd_app_command_submit(DSD_APP_CMD_TONE_FILTER_SET, &payload, sizeof payload);
}

int
dsd_app_command_scan_row_edit(const dsd_app_scan_row_edit_payload* payload) {
    if (!payload || memchr(payload->target_id, '\0', sizeof payload->target_id) == NULL
        || memchr(payload->tone_list, '\0', sizeof payload->tone_list) == NULL) {
        return DSD_APP_COMMAND_SUBMIT_REJECTED;
    }
    return dsd_app_command_submit(DSD_APP_CMD_SCAN_ROW_EDIT, payload, sizeof(*payload));
}

int
dsd_app_command_set_tone_filter_mode(int32_t mode) {
    dsd_app_tone_filter_payload payload;
    DSD_MEMSET(&payload, 0, sizeof payload);
    payload.mode = mode;
    payload.keep_list = 1;
    return dsd_app_command_submit(DSD_APP_CMD_TONE_FILTER_SET, &payload, sizeof payload);
}

int
dsd_app_command_set_rr_apply(const dsd_app_rr_apply_payload* payload) {
    return payload ? dsd_app_command_submit(DSD_APP_CMD_RR_APPLY_IMPORT, payload, sizeof *payload)
                   : DSD_APP_COMMAND_SUBMIT_REJECTED;
}

int
dsd_app_command_set_rr_account(const dsd_app_rr_account_payload* payload) {
    return payload ? dsd_app_command_submit(DSD_APP_CMD_RR_ACCOUNT_SET, payload, sizeof *payload)
                   : DSD_APP_COMMAND_SUBMIT_REJECTED;
}

static int
ui_cmd_payload_has_min_size(const struct dsd_app_command* c, size_t want) {
    return c != NULL && c->n >= want;
}

struct ui_cmd_payload_min_size_rule {
    int id;
    size_t min_size;
};

static const struct ui_cmd_payload_min_size_rule k_ui_cmd_payload_min_size_rules[] = {
    {DSD_APP_CMD_TG_LOCKOUT_PERSIST_SET, sizeof(int32_t)},
    {DSD_APP_CMD_TG_SESSION_AVOID_CLEAR, sizeof(uint64_t)},
    {DSD_APP_CMD_TG_ROW_SET, sizeof(dsd_app_tg_row_payload)},
    {DSD_APP_CMD_TG_ROW_REMOVE, sizeof(dsd_app_tg_range_payload)},
    {DSD_APP_CMD_TG_LIST_EXPORT, offsetof(dsd_app_tg_export_payload, path) + 1U},
    {DSD_APP_CMD_TG_SELECTION_SET, sizeof(dsd_app_tg_selection_payload)},
    {DSD_APP_CMD_KEY_DIRECT_SET, sizeof(dsd_app_key_direct_payload)},
    {DSD_APP_CMD_DECRYPTION_APPLY, sizeof(dsd_app_decryption_payload)},
    {DSD_APP_CMD_SCAN_ROW_EDIT, sizeof(dsd_app_scan_row_edit_payload)},
    {DSD_APP_CMD_FORCE_KEY_SET, sizeof(int32_t)},
    {DSD_APP_CMD_TG_HOLD_TOGGLE, sizeof(uint8_t)},
    {DSD_APP_CMD_CALL_ALERT_EVENTS_SET, sizeof(uint8_t)},
    {DSD_APP_CMD_LOCKOUT_SLOT, sizeof(uint8_t)},
    {DSD_APP_CMD_SKIP_SLOT, sizeof(uint8_t)},
    {DSD_APP_CMD_RTL_SET_FREQ, sizeof(uint32_t)},
    {DSD_APP_CMD_MOD_SET, sizeof(int32_t)},
    {DSD_APP_CMD_DECODE_MODE_SET, sizeof(int32_t)},
    {DSD_APP_CMD_TRUNK_SET, sizeof(int32_t)},
    {DSD_APP_CMD_SCAN_VOICE_ONLY_SET, sizeof(int32_t)},
    {DSD_APP_CMD_SCAN_VOICE_QUALIFY_MS_SET, sizeof(int32_t)},
    {DSD_APP_CMD_SCAN_VOICE_HOLD_MS_SET, sizeof(int32_t)},
    {DSD_APP_CMD_MANUAL_TUNE, sizeof(uint32_t)},
    {DSD_APP_CMD_TG_HOLD_SET, sizeof(uint32_t)},
    {DSD_APP_CMD_KEY_BASIC_SET, sizeof(uint32_t)},
    {DSD_APP_CMD_KEY_SCRAMBLER_SET, sizeof(uint32_t)},
    {DSD_APP_CMD_KEY_RC4DES_SET, sizeof(uint64_t)},
    {DSD_APP_CMD_INPUT_WARN_DB_SET, sizeof(double)},
    {DSD_APP_CMD_RTL_SET_SQL_DB, sizeof(double)},
    {DSD_APP_CMD_RTL_SET_SQL_SETTING, sizeof(dsd_app_squelch_setting_payload)},
    {DSD_APP_CMD_HANGTIME_SET, sizeof(double)},
    {DSD_APP_CMD_CONST_GATE_DELTA, sizeof(float)},
    {DSD_APP_CMD_UDP_OUT_CFG, sizeof(dsd_app_endpoint_payload)},
    {DSD_APP_CMD_TCP_CONNECT_AUDIO_CFG, sizeof(dsd_app_endpoint_payload)},
    {DSD_APP_CMD_RIGCTL_CONNECT_CFG, sizeof(dsd_app_endpoint_payload)},
    {DSD_APP_CMD_UDP_INPUT_CFG, sizeof(dsd_app_udp_input_payload)},
    {DSD_APP_CMD_P25_P2_PARAMS_SET, sizeof(dsd_app_p25_p2_params_payload)},
    {DSD_APP_CMD_TG_LISTEN_SET, sizeof(dsd_app_tg_listen_payload)},
    {DSD_APP_CMD_TG_LISTEN_SET_ALL, sizeof(dsd_app_tg_listen_all_payload)},
    {DSD_APP_CMD_KEY_HYTERA_SET, sizeof(dsd_app_hytera_key_payload)},
    {DSD_APP_CMD_KEY_AES_SET, sizeof(dsd_app_aes_key_payload)},
    {DSD_APP_CMD_DSP_OP, sizeof(dsd_app_dsp_payload)},
    {DSD_APP_CMD_CONFIG_APPLY, sizeof(dsdneoUserConfig)},
    {DSD_APP_CMD_CONFIG_METADATA_SET, sizeof(dsd_app_config_metadata_payload)},
    {DSD_APP_CMD_RR_APPLY_IMPORT, sizeof(dsd_app_rr_apply_payload)},
    {DSD_APP_CMD_RR_ACCOUNT_SET, sizeof(dsd_app_rr_account_payload)},
};

static size_t
ui_cmd_payload_min_size_for_id(int cmd_id) {
    for (size_t i = 0; i < sizeof k_ui_cmd_payload_min_size_rules / sizeof k_ui_cmd_payload_min_size_rules[0]; i++) {
        if (k_ui_cmd_payload_min_size_rules[i].id == cmd_id) {
            return k_ui_cmd_payload_min_size_rules[i].min_size;
        }
    }
    return 0U;
}

static int
ui_cmd_payload_is_valid(const struct dsd_app_command* c) {
    if (!c) {
        return 0;
    }
    if (c->payload_truncated && !ui_cmd_is_string_payload_id(c->id)) {
        return 0;
    }
    if (ui_cmd_is_action_id(c->id)) {
        return c->n == 0U;
    }
    if (ui_cmd_id_in_list(c->id, k_ui_cmd_i32_ids, sizeof k_ui_cmd_i32_ids / sizeof k_ui_cmd_i32_ids[0])) {
        return ui_cmd_payload_has_min_size(c, sizeof(int32_t));
    }
    if (ui_cmd_id_in_list(c->id, k_ui_cmd_string_ids, sizeof k_ui_cmd_string_ids / sizeof k_ui_cmd_string_ids[0])) {
        return c->n > 0U;
    }
    const size_t min_size = ui_cmd_payload_min_size_for_id(c->id);
    return min_size == 0U || ui_cmd_payload_has_min_size(c, min_size);
}

static int
apply_cmd_basic_a(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    switch (c->id) {
        case DSD_APP_CMD_QUIT: dsd_exitflag_store(1); return 1;
        case DSD_APP_CMD_FORCE_PRIV_TOGGLE:
            if (!state) {
                return 1;
            }
            if (state->M == 1 || state->M == 0x21) {
                state->M = 0;
            } else {
                state->M = 1;
            }
            dsd_enc_lockout_bump_key_epoch(state);
            return 1;
        case DSD_APP_CMD_FORCE_RC4_TOGGLE:
            if (!state) {
                return 1;
            }
            if (state->M == 1 || state->M == 0x21) {
                state->M = 0;
            } else {
                state->M = 0x21;
            }
            dsd_enc_lockout_bump_key_epoch(state);
            return 1;
        case DSD_APP_CMD_TOGGLE_COMPACT:
            opts->frontend_terminal_display.terminal_compact = opts->frontend_terminal_display.terminal_compact ? 0 : 1;
            dsd_telemetry_request_redraw();
            return 1;
        case DSD_APP_CMD_HISTORY_CYCLE:
            (void)dsd_app_frontend_history_cycle_mode();
            dsd_telemetry_request_redraw();
            return 1;
        default: return 0;
    }
}

static int
apply_cmd_slot_controls(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    switch (c->id) {
        case DSD_APP_CMD_SLOT1_TOGGLE:
            if (!state) {
                return 1;
            }
            if (opts->slot1_on == 1) {
                opts->slot1_on = 0;
                if (opts->slot_preference == 0) {
                    opts->slot_preference = 2;
                }
                state->audio_out_float_buf_p = state->audio_out_float_buf + 100;
                state->audio_out_buf_p = state->audio_out_buf + 100;
                DSD_MEMSET(state->audio_out_float_buf, 0, 100 * sizeof(float));
                DSD_MEMSET(state->audio_out_buf, 0, 100 * sizeof(short));
                state->audio_out_idx2 = 0;
                state->audio_out_idx = 0;
            } else {
                opts->slot1_on = 1;
            }
            return 1;
        case DSD_APP_CMD_SLOT2_TOGGLE:
            if (!state) {
                return 1;
            }
            if (opts->slot2_on == 1) {
                opts->slot2_on = 0;
                opts->slot_preference = 0;
                state->audio_out_float_buf_pR = state->audio_out_float_bufR + 100;
                state->audio_out_buf_pR = state->audio_out_bufR + 100;
                DSD_MEMSET(state->audio_out_float_bufR, 0, 100 * sizeof(float));
                DSD_MEMSET(state->audio_out_bufR, 0, 100 * sizeof(short));
                state->audio_out_idx2R = 0;
                state->audio_out_idxR = 0;
            } else {
                opts->slot2_on = 1;
            }
            return 1;
        case DSD_APP_CMD_SLOT_PREF_CYCLE:
            if (opts->slot_preference == 0 || opts->slot_preference == 1) {
                opts->slot_preference++;
            } else {
                opts->slot_preference = 0;
            }
            return 1;
        default: return 0;
    }
}

static int
ui_cmd_handle_payload_toggle(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    (void)state;
    (void)c;
    opts->payload = opts->payload ? 0 : 1;
    return 1;
}

static int
ui_cmd_handle_p25_ga_toggle(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    (void)c;
    opts->frontend_display.show_p25_group_affiliations = opts->frontend_display.show_p25_group_affiliations ? 0 : 1;
    if (state) {
        DSD_SNPRINTF(state->ui_msg, sizeof state->ui_msg, "P25 Group Affiliation: %s",
                     opts->frontend_display.show_p25_group_affiliations ? "On" : "Off");
        state->ui_msg_expire = dsd_realtime_time() + 3;
    }
    return 1;
}

static int
ui_cmd_handle_lpf_toggle(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    (void)state;
    (void)c;
    opts->use_lpf = opts->use_lpf ? 0 : 1;
    return 1;
}

static int
ui_cmd_handle_hpf_toggle(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    (void)state;
    (void)c;
    opts->use_hpf = opts->use_hpf ? 0 : 1;
    return 1;
}

static int
ui_cmd_handle_pbf_toggle(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    (void)state;
    (void)c;
    opts->use_pbf = opts->use_pbf ? 0 : 1;
    return 1;
}

static int
ui_cmd_handle_hpf_d_toggle(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    (void)state;
    (void)c;
    opts->use_hpf_d = opts->use_hpf_d ? 0 : 1;
    return 1;
}

static int
ui_cmd_handle_aggr_sync_toggle(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    (void)state;
    (void)c;
    opts->aggressive_framesync = opts->aggressive_framesync ? 0 : 1;
    return 1;
}

static int
ui_cmd_handle_call_alert_toggle(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    (void)state;
    (void)c;
    uint8_t events = dsd_call_alert_mask_events(opts->call_alert_events);
    opts->call_alert_events = events;
    opts->call_alert = opts->call_alert ? 0 : (events ? 1 : 0);
    return 1;
}

static int
ui_cmd_handle_call_alert_events_set(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    (void)state;
    uint8_t events = 0;
    if (c->n >= sizeof events) {
        DSD_MEMCPY(&events, c->data, sizeof events);
        events = dsd_call_alert_mask_events(events);
        opts->call_alert = events ? 1 : 0;
        opts->call_alert_events = events;
    }
    return 1;
}

static int
apply_cmd_payload_filters(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    static const struct dsd_app_command_handler_entry k_handlers[] = {
        {DSD_APP_CMD_PAYLOAD_TOGGLE, ui_cmd_handle_payload_toggle},
        {DSD_APP_CMD_P25_GA_TOGGLE, ui_cmd_handle_p25_ga_toggle},
        {DSD_APP_CMD_LPF_TOGGLE, ui_cmd_handle_lpf_toggle},
        {DSD_APP_CMD_HPF_TOGGLE, ui_cmd_handle_hpf_toggle},
        {DSD_APP_CMD_PBF_TOGGLE, ui_cmd_handle_pbf_toggle},
        {DSD_APP_CMD_HPF_D_TOGGLE, ui_cmd_handle_hpf_d_toggle},
        {DSD_APP_CMD_AGGR_SYNC_TOGGLE, ui_cmd_handle_aggr_sync_toggle},
        {DSD_APP_CMD_CALL_ALERT_TOGGLE, ui_cmd_handle_call_alert_toggle},
        {DSD_APP_CMD_CALL_ALERT_EVENTS_SET, ui_cmd_handle_call_alert_events_set},
    };
    return ui_cmd_apply_handler_table(k_handlers, sizeof k_handlers / sizeof k_handlers[0], opts, state, c);
}

/* Visual aids are suppressed while compact view is active; surface a hint when
   a visualizer is switched on there so the key does not look dead. */
static void
ui_toast_visualizer_hidden_if_compact(const dsd_opts* opts, dsd_state* state, uint8_t view_on) {
    if (view_on && opts->frontend_terminal_display.terminal_compact == 1) {
        ui_set_toast(state, 3, "Visualizer hidden in compact view (c to exit)");
    }
}

static int
apply_cmd_constellation(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    switch (c->id) {
        case DSD_APP_CMD_CONST_TOGGLE:
            if (opts->audio_in_type == AUDIO_IN_RTL) {
                opts->frontend_display.constellation = opts->frontend_display.constellation ? 0 : 1;
                ui_toast_visualizer_hidden_if_compact(opts, state, opts->frontend_display.constellation);
            }
            return 1;
        case DSD_APP_CMD_CONST_NORM_TOGGLE:
            if (opts->audio_in_type == AUDIO_IN_RTL && opts->frontend_display.constellation == 1) {
                opts->frontend_display.const_norm_mode = (opts->frontend_display.const_norm_mode == 0) ? 1 : 0;
            }
            return 1;
        case DSD_APP_CMD_CONST_GATE_DELTA:
            if (opts->audio_in_type == AUDIO_IN_RTL && opts->frontend_display.constellation == 1) {
                float d = 0.0f;
                if (c->n >= (int)sizeof(float)) {
                    DSD_MEMCPY(&d, c->data, sizeof(float));
                }
                float* g = (opts->mod_qpsk == 1) ? &opts->frontend_display.const_gate_qpsk
                                                 : &opts->frontend_display.const_gate_other;
                *g += d;
                if (*g < 0.0f) {
                    *g = 0.0f;
                }
                if (*g > 0.90f) {
                    *g = 0.90f;
                }
            }
            return 1;
        default: return 0;
    }
}

static int
ui_cmd_handle_eye_toggle(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    (void)c;
    if (opts->audio_in_type == AUDIO_IN_RTL) {
        opts->frontend_display.eye_view = opts->frontend_display.eye_view ? 0 : 1;
        ui_toast_visualizer_hidden_if_compact(opts, state, opts->frontend_display.eye_view);
    }
    return 1;
}

static int
ui_cmd_handle_eye_unicode_toggle(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    (void)state;
    (void)c;
    if (opts->audio_in_type == AUDIO_IN_RTL && opts->frontend_display.eye_view == 1) {
        opts->frontend_terminal_display.eye_unicode = opts->frontend_terminal_display.eye_unicode ? 0 : 1;
    }
    return 1;
}

static int
ui_cmd_handle_eye_color_toggle(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    (void)state;
    (void)c;
    if (opts->audio_in_type == AUDIO_IN_RTL && opts->frontend_display.eye_view == 1) {
        opts->frontend_terminal_display.eye_color = opts->frontend_terminal_display.eye_color ? 0 : 1;
    }
    return 1;
}

static int
ui_cmd_handle_fsk_hist_toggle(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    (void)c;
    if (opts->audio_in_type == AUDIO_IN_RTL) {
        opts->frontend_display.fsk_hist_view = opts->frontend_display.fsk_hist_view ? 0 : 1;
        ui_toast_visualizer_hidden_if_compact(opts, state, opts->frontend_display.fsk_hist_view);
    }
    return 1;
}

static int
ui_cmd_handle_spectrum_toggle(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    (void)c;
    if (opts->audio_in_type == AUDIO_IN_RTL) {
        opts->frontend_display.spectrum_view = opts->frontend_display.spectrum_view ? 0 : 1;
        ui_toast_visualizer_hidden_if_compact(opts, state, opts->frontend_display.spectrum_view);
    }
    return 1;
}

static int
ui_cmd_handle_spec_size_delta(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    int32_t d = 0;
    (void)state;
    if (c->n >= (int)sizeof(int32_t)) {
        DSD_MEMCPY(&d, c->data, sizeof(int32_t));
    }
    if (opts->audio_in_type == AUDIO_IN_RTL && opts->frontend_display.spectrum_view == 1) {
        /* Keep toggle state normalized while applying size changes. */
        opts->frontend_display.spectrum_view = 1;
#ifdef USE_RADIO
        int n = rtl_stream_spectrum_get_size();
        /* In 64 bits: a delta can be any int32 (the control API sends what its client asked for). */
        int64_t want = (int64_t)n + d;
        if (want < 64) {
            want = 64;
        }
        if (want > 1024) {
            want = 1024;
        }
        if (want != n) {
            (void)rtl_stream_spectrum_set_size((int)want);
        }
#else
        (void)d;
#endif
    }
    return 1;
}

static int
apply_cmd_eye_spectrum(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    static const struct dsd_app_command_handler_entry k_handlers[] = {
        {DSD_APP_CMD_EYE_TOGGLE, ui_cmd_handle_eye_toggle},
        {DSD_APP_CMD_EYE_UNICODE_TOGGLE, ui_cmd_handle_eye_unicode_toggle},
        {DSD_APP_CMD_EYE_COLOR_TOGGLE, ui_cmd_handle_eye_color_toggle},
        {DSD_APP_CMD_FSK_HIST_TOGGLE, ui_cmd_handle_fsk_hist_toggle},
        {DSD_APP_CMD_SPECTRUM_TOGGLE, ui_cmd_handle_spectrum_toggle},
        {DSD_APP_CMD_SPEC_SIZE_DELTA, ui_cmd_handle_spec_size_delta},
    };
    return ui_cmd_apply_handler_table(k_handlers, sizeof k_handlers / sizeof k_handlers[0], opts, state, c);
}

/**
 * @brief Hand the tuner back to the operator: trunking and scanner mode both off.
 *
 * Idempotent. A frontend only learns that *something* owns the tuner, not which,
 * so TRUNK_TOGGLE/SCANNER_TOGGLE cannot express "off" from there without guessing.
 *
 * The call-state teardown is not optional and is not left to a following
 * MANUAL_TUNE: a release is frequently the whole request -- look around from
 * where we already are -- and then no tune ever runs it. Left set,
 * opts->trunk_is_tuned keeps update_dmr_bs_sync_times_if_tuned() stamping sync
 * times, which in turn suppresses the engine's stale-follow-state cleanup, and it
 * is also the flag whose zero state lets the decoder learn a control channel.
 *
 * No dsd_frame_sync_reset_mod_state() here: nothing retuned, so the modulation
 * votes still describe the signal actually being received.
 *
 * opts->trunk_scan_enabled is a third tuner owner (see
 * engine_trunk_tuning_owner_active()) and is deliberately untouched: it owns live
 * scan hooks that clearing a flag would not tear down, and it is only reachable
 * from a frontend that passes --trunk-scan.
 *
 * Refused during an I/Q replay (issue #575): it hands the tuner over to manual
 * tunes, which a replay refuses, so it would only stop the decoder following the
 * capture's trunking while the capture's recorded retunes went on.
 */
static int
apply_tuner_release(dsd_opts* opts, dsd_state* state) {
    if (!state) {
        return UI_CMD_APPLY_COMPLETED;
    }
    if (ui_cmd_refuse_replay_tune(opts, state)) {
        return UI_CMD_APPLY_FAILED;
    }
    opts->trunk_enable = 0;
    opts->scanner_mode = 0;
    // Leaving -Y hands the foreground keyring back to the globals.
    if (opts->trunk_scan_enabled != 1) {
        (void)svc_leave_channel_scan(opts, state);
        dsd_scan_keys_leave(state);
    }
    reset_call_tracking(opts, state, 1);
    ui_set_toast(state, 3, "Automatic tuning stopped");
    return UI_CMD_APPLY_COMPLETED;
}

/**
 * @brief Answer a decode-mode request that needs no preset run, or decline to.
 *
 * Range-checked before the cast, not after. dsdneoUserDecodeMode is a packed enum
 * -- one byte -- so a value outside it does not stay outside it: 260 casts to 4,
 * which is DSDCFG_MODE_DMR, and the whole DMR preset would then run for a command
 * nobody could have meant. The payload is a plain int32 on the wire and
 * CommandBridge::setDecodeMode() passes it through unvalidated, so this is the only
 * place the two widths are reconciled.
 *
 * Idempotent for the reason ui_handle_mod_set() is: a chip re-asserts its own state
 * after a frame it did not cause, and DecodeChip taps whether or not it is already
 * selected. Applying a preset ends both call epochs, drops the auto-modulation
 * votes and releases the modulation locks -- so re-applying the mode already in
 * effect would cut a live call and revert an operator's QPSK pick on a tap that
 * asked for nothing to change. Compared through dsd_infer_decode_mode_preset(),
 * which is the same reading the control binds to, so the skip and the selection
 * cannot answer differently.
 *
 * AUTO is excluded because there the reading is not an identification: AUTO is that
 * helper's catch-all for any frame set matching no single preset, so a session with
 * one protocol switched off answers AUTO without being it, and "listen for
 * everything" has to stay able to widen such a set back out.
 *
 * @param c        Command whose payload is already known to be wide enough.
 * @param out_mode Receives the requested mode whenever one could be read.
 * @return The verdict to answer with, its toast already posted, or
 *         UI_CMD_APPLY_UNHANDLED when @p out_mode is a mode still to apply.
 */
static int
decode_mode_set_early_verdict(const dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c,
                              dsdneoUserDecodeMode* out_mode) {
    int32_t requested = 0;
    DSD_MEMCPY(&requested, c->data, sizeof requested);
    if (requested < 0 || requested > (int32_t)DSDCFG_MODE_AM) {
        ui_set_toast(state, 4, "Decode mode not available");
        return UI_CMD_APPLY_UNSUPPORTED;
    }
    *out_mode = (dsdneoUserDecodeMode)requested;
    if (!dsd_decode_mode_runs_on_input(*out_mode, opts)) {
        /* AM on a PCM input (issue #524): the audio arrives demodulated. */
        ui_set_toast(state, 5, "Refused: %s", DSD_DECODE_MODE_AM_NEEDS_IQ_TEXT);
        return UI_CMD_APPLY_UNSUPPORTED;
    }
    if (*out_mode != DSDCFG_MODE_AUTO && dsd_infer_decode_mode_preset(opts) == *out_mode) {
        ui_set_toast(state, 3, "Decoding %s", dsd_decode_mode_display_name(*out_mode));
        return UI_CMD_APPLY_COMPLETED;
    }
    return UI_CMD_APPLY_UNHANDLED;
}

/*
 * A switch onto the analog monitor, or between its FM and AM kinds, the RTL front end has not taken yet
 * (DSD_APP_CMD_DECODE_MODE_SET, a config's [mode]): the configured decoder settings from before it and the ones it
 * left, and the kind it switched to. The explicit NFM width, or the AM width (its default included), it runs was held
 * to the demod rate before the switch committed, but a retune that moves the rate in between gets the front end to
 * refuse the analog profile, at once or where it lands, and stay on the digital family or the kind it ran. The decoder
 * then goes back to the settings it had rather than decode from a front end running something else, provided its
 * configured settings are still the ones the switch left; the row-scoped options (squelch, forcing, the voice gate) and
 * the channel widths (a width command made after the switch) stay as they are, since the switch did not set them.
 * Staged before such a command changes anything, armed once it has switched a running RTL session, and dropped once
 * the front end takes the switch (or a request after it). A switch made straight over one still armed (from the
 * settings that one left, before any command found it taken) keeps that one's settings from before: the front end's
 * requests are last-writer-wins, so while the earlier request was still queued it never ran the settings in between,
 * and going back to them would ask it for a profile of their own that the moved rate can refuse as well. The demod
 * thread can still have taken the earlier request after the last command asked, before the later one was queued, so
 * the settings each superseded switch left are kept too, by the analog kind they run (between): a refusal the front
 * end records as keeping that kind goes back to them (ui_analog_entry_rollback()).
 */
static dsd_scan_settings g_analog_entry_staged;

static struct {
    int armed;
    int kind; /* the dsd_analog_demod the switch is onto */
    dsd_scan_settings before;
    dsd_scan_settings after;
    int has_between[2];           /* by dsd_analog_demod */
    dsd_scan_settings between[2]; /* the settings the last superseded switch onto that kind left */
} g_analog_entry;

static void
ui_stage_analog_entry(const dsd_opts* opts, const dsd_state* state) {
    dsd_scan_settings_capture(opts, state, &g_analog_entry_staged);
}

/* The configured channel widths of @p now over @p settings. */
static void
ui_analog_entry_keep_widths(dsd_scan_settings* settings, const dsd_scan_settings* now) {
    settings->analog_nfm_bandwidth_hz = now->analog_nfm_bandwidth_hz;
    settings->analog_am_bandwidth_hz = now->analog_am_bandwidth_hz;
}

/* The settings a command changes without asking the front end for a receive profile of its own (issue #582): the
   cosine filter (DSD_APP_CMD_COSINE_FILTER_TOGGLE), the inversions (DSD_APP_CMD_INVERT_TOGGLE, DSD_APP_CMD_INV_*_TOGGLE)
   and the input monitor (DSD_APP_CMD_INPUT_MONITOR_TOGGLE). Of @p now over @p settings, each one @p now holds other
   than @p since (every one, with @p since NULL). */
static void
ui_analog_entry_keep_unpublished(dsd_scan_settings* settings, const dsd_scan_settings* now,
                                 const dsd_scan_settings* since) {
#define UI_KEEP_UNPUBLISHED(field)                                                                                     \
    do {                                                                                                               \
        if (!since || now->field != since->field) {                                                                    \
            settings->field = now->field;                                                                              \
        }                                                                                                              \
    } while (0)
    UI_KEEP_UNPUBLISHED(use_cosine_filter);
    UI_KEEP_UNPUBLISHED(inverted_p2);
    UI_KEEP_UNPUBLISHED(inverted_x2tdma);
    UI_KEEP_UNPUBLISHED(inverted_dmr);
    UI_KEEP_UNPUBLISHED(inverted_dpmr);
    UI_KEEP_UNPUBLISHED(inverted_ysf);
    UI_KEEP_UNPUBLISHED(inverted_m17);
    UI_KEEP_UNPUBLISHED(monitor_input_audio);
#undef UI_KEEP_UNPUBLISHED
}

/* The configured options now run the analog family where they did not (@p was_analog_family), or another analog kind
   than @p was_kind: note the switch while a running RTL front end has it to make. One that supersedes a switch still
   armed keeps that switch's settings from before it (g_analog_entry). The channel widths are configuration a width
   command sets on its own, not part of a switch, as ui_revert_analog_entry() compares them: a width set between the
   two switches (dsd_scan_settings_equal() compares the width of the analog kind in force, issue #526) does not mean the
   settings moved on from the earlier one. */
static void
ui_arm_analog_entry(const dsd_opts* opts, const dsd_state* state, int was_analog_family, int was_kind) {
    if ((was_analog_family && was_kind == opts->analog_demod) || !dsd_opts_is_analog_family(opts)
        || opts->audio_in_type != AUDIO_IN_RTL || !state->rtl_ctx) {
        return;
    }
    dsd_scan_settings armed_after = g_analog_entry.after;
    ui_analog_entry_keep_widths(&armed_after, &g_analog_entry_staged);
    const int supersedes = g_analog_entry.armed && dsd_scan_settings_equal(&g_analog_entry_staged, &armed_after, 0);
    if (supersedes && dsd_analog_demod_is_valid(g_analog_entry.kind)) {
        g_analog_entry.between[g_analog_entry.kind] = g_analog_entry.after;
        g_analog_entry.has_between[g_analog_entry.kind] = 1;
    } else if (!supersedes) {
        g_analog_entry.before = g_analog_entry_staged;
        g_analog_entry.has_between[DSD_ANALOG_DEMOD_FM] = 0;
        g_analog_entry.has_between[DSD_ANALOG_DEMOD_AM] = 0;
    }
    dsd_scan_settings_capture(opts, state, &g_analog_entry.after);
    g_analog_entry.kind = opts->analog_demod;
    g_analog_entry.armed = 1;
}

/* The width of each analog kind in force before a scoped command ran, taken before its suspend puts the configured
   ones in dsd_opts. */
typedef struct {
    int nfm_hz;
    int am_hz;
} ui_analog_widths;

/* The width of analog @p kind in @p before; -1 without one. */
static int
ui_analog_width_before(const ui_analog_widths* before, int kind) {
    if (!before) {
        return -1;
    }
    return (kind == DSD_ANALOG_DEMOD_AM) ? before->am_hz : before->nfm_hz;
}

/* Time the decoder, and the symbol profile it publishes next, for @p profile at @p rate_hz. */
static void
ui_time_profile(const dsd_opts* opts, dsd_state* state, dsd_decode_mode_profile profile, int rate_hz) {
    state->samplesPerSymbol = dsd_opts_compute_sps_rate(opts, profile.symbol_rate_hz, rate_hz);
    state->symbolCenter = dsd_opts_symbol_center(state->samplesPerSymbol);
}

/* Time the decoder for @p profile at @p rate_hz and ask the front end for that profile (svc_publish_symbol_profile(),
   which times a switch out of the analog family for the rate it lands on instead). Returns the publish's result. */
static int
ui_publish_profile_timed(const dsd_opts* opts, dsd_state* state, dsd_decode_mode_profile profile, int rate_hz) {
    ui_time_profile(opts, state, profile, rate_hz);
    return svc_publish_symbol_profile(opts, state, profile);
}

/*
 * The rate a stream a command started runs the row's @p profile at once the publish that asks it for that profile lands
 * (issue #578): the resume's, for a command that suspended the scope, or the one right after a command that left it in
 * force (ui_publish_row_to_started_stream()). A reopen under a suspended scope opened on the configured settings, so
 * the rate it delivers now can be another family's or modulation's; a rollback's restart, and a stream a command that
 * left the scope in force started, opened on the row's settings, with the CQPSK state an open picks for them. The
 * request applies the row's own CQPSK state, whatever DSD_NEO_CQPSK says, over the output chain the stream opened on,
 * whether that is the digital family or the analog one a typed row runs its profile under on an analog session. A
 * CQPSK row's timing loop then runs at the demod rate the stream published at its start
 * (rtl_stream_get_request_rate_hz()), not at the resampled output of a stream that opened on the FSK discriminator or
 * the analog monitor. Anything else (an FSK row, whose samples leave at the rate the stream delivers now, which only an
 * open, a family switch, a retune or a landing resamples anew; an analog row; the M17 encoder's analog front end) keeps
 * the rate the stream delivers.
 *
 * The row's own CQPSK state is the one the publish asks for (dsd_scan_mode_cqpsk_explicit(), issue #583): the
 * decoder's, unless the row is a trunk-scan DMR or NXDN target, which runs CQPSK off whatever rf_mod a -mq lock left
 * it on. Such a target is asked for the FSK discriminator, and reads the rate the stream delivers, not the demod rate
 * its decoder's inherited CQPSK would pick: under DSD_NEO_CQPSK=0 a stream that opened on FSK, resampled to 48 kHz,
 * would otherwise run it at 48 kHz timed for the demod rate.
 *
 * A row the publish lands on the digital family's landing is timed there by the publish itself instead
 * (svc_publish_symbol_profile(), issue #583): a front end still on the analog family under a digital configured mode,
 * a row timed for that landing, or work outstanding that lands a family. The landing is where an open of the mode
 * would put it (the CQPSK family DSD_NEO_CQPSK names when set, unless the row is a trunk-scan target that makes its own
 * choice, dsd_scan_mode_cqpsk_explicit()), with an output chain designed for it, and the publish decides it once, times
 * the decoder for it and asks for that landing, so the timing here is only for a publish that lands none.
 */
static int
ui_started_stream_rate(const dsd_opts* opts, const dsd_state* state, dsd_decode_mode_profile profile) {
#ifdef USE_RADIO
    int row_cqpsk = 0;
    (void)dsd_scan_mode_cqpsk_explicit(opts, state, &row_cqpsk);
    if (opts->audio_in_type == AUDIO_IN_RTL && state->rtl_ctx && opts->analog_only != 1 && profile.symbol_rate_hz > 0
        && row_cqpsk) {
        const int rate_hz = rtl_stream_get_request_rate_hz();
        if (rate_hz > 0) {
            return rate_hz;
        }
    }
#else
    (void)profile;
#endif
    return current_demod_rate(opts, state);
}

/*
 * A stream a command started while a scan row's scope was in force, not suspended (issue #578): Input > Switch source,
 * a DSP bandwidth, an Airspy setting, a gain, a device index or an explicit restart that worked, or the restart of the
 * input that ran when one of those did not start. It opened on the options in force, the row's, but an open picks its
 * CQPSK state by its own rule, DSD_NEO_CQPSK's when set, else the QPSK flag of the options, not the CQPSK state the
 * decoder runs the row with (state->rf_mod), which the row's tune applied whatever DSD_NEO_CQPSK says: under
 * DSD_NEO_CQPSK=1 the stream would run a C4FM row on CQPSK, and under DSD_NEO_CQPSK=0 a CQPSK row on the FSK
 * discriminator, until another profile request. The row's effective profile is asked of it, as the resume asks it of a
 * stream a command that suspended the scope started (ui_resume_scope_and_publish()), and timed for the rate the stream
 * runs the row at (ui_started_stream_rate()), without ending the decoder's acquisition, and with nothing reset as for a
 * new input (ui_input_switched()). An analog row's profile is the monitor the stream opened on, whose width its start
 * held to the rate it runs, so the front end runs that monitor whatever the request's outcome.
 */
static void
ui_publish_row_to_started_stream(const dsd_opts* opts, dsd_state* state) {
    if (!dsd_scan_mode_configured_view(state)) {
        return;
    }
    const dsd_decode_mode_profile profile = dsd_scan_mode_effective_profile(opts, state);
    (void)ui_publish_profile_timed(opts, state, profile, ui_started_stream_rate(opts, state, profile));
}

/* The row's constraint back over the configured options a scoped update edited. When that changes the decoder, the
   acquisition it made ends and the front end is told the effective profile: @p out_changed says whether it did, and
   the result is that publish's (-1: the front end refused the analog profile at once). @p before holds the widths in
   force before the update (NULL: the caller changes no width). Only a row that takes the configured width has that
   width moved by the update, so the width before it was the configured one too, which the publish keeps for a refusal
   where it lands (svc_publish_symbol_profile_changing_width()). @p restarted says the update started a stream while
   the scope was suspended (a config apply's reopen, or the restart of the input a failed one replaced): that stream
   opened on the configured settings (the reopen) or on the row as it ran before the update (the restart), not on the
   row the resume puts back, so the row's profile is published to it even when the row compares unchanged and the
   decoder keeps its acquisition (issue #578). Changed or not, the row's profile is then timed for the rate that stream
   runs the row at (ui_started_stream_rate()), not with the timing the decoder kept from the old stream, nor the one the
   resume took from the rate the new stream delivers on the settings it opened on: a CQPSK row read at the stream's
   symbol-rate output has one sample per symbol, which is no timing for its symbol loop. */
static int
ui_resume_scope_and_publish(dsd_opts* opts, dsd_state* state, int* out_changed, const ui_analog_widths* before,
                            int restarted) {
    const int resuming = dsd_scan_mode_updating(state);
    *out_changed = dsd_scan_mode_resume(opts, state) ? 1 : 0;
    const int started_stream = restarted && resuming;
    if (!*out_changed && !started_stream) {
        return 0;
    }
    const dsd_decode_mode_profile profile = dsd_scan_mode_effective_profile(opts, state);
    int configured_before_hz = -1;
    if (*out_changed) {
        reset_call_tracking(opts, state, 1);
        dsd_frame_sync_reset_acquisition(opts, state, opts->trunk_scan_enabled != 1);
        const int width_before = ui_analog_width_before(before, opts->analog_demod);
        if (width_before >= 0 && dsd_opts_analog_width_hz(opts) != width_before) {
            configured_before_hz = width_before;
        }
    }
    if (started_stream) {
        ui_time_profile(opts, state, profile, ui_started_stream_rate(opts, state, profile));
    }
    return svc_publish_symbol_profile_changing_width(opts, state, profile, configured_before_hz);
}

/*
 * The settings a refused switch goes back to: the ones from before it, unless the front end refused it where it landed
 * and recorded keeping the analog family on a kind those settings do not run (@p kept), having taken a switch this one
 * superseded before this one's request was queued: then the settings that switch left, for the kind it kept. NULL (a
 * refusal by the request, which recorded nothing): the ones from before.
 */
static const dsd_scan_settings*
ui_analog_entry_rollback(const svc_monitor_refusal* kept) {
    const dsd_scan_settings* before = &g_analog_entry.before;
    if (!kept || !kept->kept_analog || !dsd_analog_demod_is_valid(kept->kept_kind)
        || (before->analog_only == 1 && before->analog_demod == kept->kept_kind)
        || !g_analog_entry.has_between[kept->kept_kind]) {
        return before;
    }
    return &g_analog_entry.between[kept->kept_kind];
}

/*
 * The decoder is back on the analog kind the front end kept (@p kept): a width of that kind changed while the refused
 * switch was pending never reached it (the switch's request replaced the width's), so one the rate it runs at now
 * cannot filter goes back to the width it runs, and the republish asks it for what it runs; one the rate filters stays
 * and is asked for again, as a width change would be.
 */
static void
ui_hold_reverted_analog_width(dsd_opts* opts, const dsd_state* state, const svc_monitor_refusal* kept) {
    if (!kept || !kept->kept_analog || opts->analog_only != 1 || opts->analog_demod != kept->kept_kind) {
        return;
    }
    const int width_hz = dsd_app_analog_width_setting_hz(opts, kept->kept_kind);
    char why[128];
    if (width_hz != kept->kept_width_hz
        && svc_check_analog_bandwidth(opts, state, kept->kept_kind, width_hz, why, sizeof why) != 0) {
        (void)svc_store_analog_width_setting(opts, state, kept->kept_kind, kept->kept_width_hz);
    }
}

static int ui_settle_refused_scan_leave(dsd_opts* opts, dsd_state* state, const svc_monitor_refusal* refusal);

/*
 * A switch a refused scan leave put back (@p leave: what the front end kept when it refused the leave's return, issue
 * #578), whose own monitor request for the kind restored the front end refused at once, at the rate it publishes: that
 * request queued nothing, so the front end still runs what it kept, and the kind restored is reconciled as the leave's
 * own kind would be (ui_settle_refused_scan_leave()), its toast replacing the revert's to say what the monitor runs.
 */
static void
ui_settle_leave_revert_refused_at_once(dsd_opts* opts, dsd_state* state, const svc_monitor_refusal* leave) {
    svc_monitor_refusal refusal = *leave;
    refusal.kind = opts->analog_demod;
    refusal.width_hz = dsd_opts_analog_width_hz(opts);
    refusal.configured_before_hz = -1; /* the revert changed the kind, not the configured width of the one restored */
    refusal.superseded = 0;
    (void)ui_settle_refused_scan_leave(opts, state, &refusal);
}

/*
 * The front end refused the switch onto the analog monitor or onto its other kind (@p width_hz: the width of that kind
 * it refused, 0 for its default) and stayed digital, or on the kind it ran (@p kept: what it recorded keeping, for a
 * refusal where the request landed; NULL for one by the request): put the decoder back on the configured settings it
 * had before the switch, or on the ones the front end runs (ui_analog_entry_rollback(), ui_hold_reverted_analog_width()),
 * timed for the demod rate the front end runs now, under a scan row's scope as well, and say why. Returns 1 when it
 * did; 0 when no switch is armed, or the configured settings have moved on since (a later command published its own
 * profile). A later command that asks the front end for no receive profile (the cosine filter, an inversion or the input
 * monitor toggled, issue #582) has not moved them on: the decoder goes back all the same, keeping what it set, rather
 * than stay on a kind the front end does not run.
 *
 * A switch a refused scan leave puts back (@p kept carries the leave; the scan has stopped, so no scope is in force) is
 * part of that leave (issue #578): the rate a row's retune left can filter the kind restored no better than the one
 * the leave asked for, and the front end can still run the row's digital profile. The request for the kind restored
 * carries the leave on (svc_publish_symbol_profile_after_scan_leave()), so a refusal where it lands is reconciled as a
 * refused leave, and one at once is reconciled here the same way (ui_settle_leave_revert_refused_at_once()): the width
 * a monitor of that kind kept, else the kind's default, the toast saying what the monitor runs. The hold runs before
 * that request is made (a width of the kind restored changed while the switch was pending, which the rate cannot filter,
 * goes back to the one the front end runs), so the leave's rule only settles what the request, held or not, asked for.
 */
static int
ui_revert_analog_entry(dsd_opts* opts, dsd_state* state, int width_hz, const svc_monitor_refusal* kept) {
    if (!g_analog_entry.armed) {
        return 0;
    }
    g_analog_entry.armed = 0;
    const int scoped = dsd_scan_mode_suspend(opts, state);
    dsd_scan_settings now;
    dsd_scan_settings_capture(opts, state, &now);
    /* The channel widths are configuration a width command sets on its own (DSD_APP_CMD_NFM_BANDWIDTH_SET), not part of
       the switch: one set after the switch, even in the same drain, does not mean the settings moved on, and the
       revert keeps it (dsd_scan_settings_equal() compares the width of the analog kind the switch left). */
    dsd_scan_settings after = g_analog_entry.after;
    ui_analog_entry_keep_widths(&after, &now);
    ui_analog_entry_keep_unpublished(&after, &now, NULL);
    const int reverted = dsd_scan_settings_equal(&now, &after, 0);
    if (reverted) {
        dsd_scan_settings back = *ui_analog_entry_rollback(kept);
        ui_analog_entry_keep_widths(&back, &now);
        /* What such a command set since the switch stands; what the switch itself set goes back with it. */
        ui_analog_entry_keep_unpublished(&back, &now, &g_analog_entry.after);
        ui_restore_settings_keeping_row_policy(opts, state, &back, &now);
        ui_hold_reverted_analog_width(opts, state, kept);
        if (!opts->analog_only) {
            /* The snapshot timed the mode for the demod rate of its day, and the retune that got the switch refused
               can have moved that rate: timed again for the rate the front end runs now, with the profile the
               restored hunt index describes, as a change onto the mode would time it (decode_mode_republish()). The
               publish below hands the front end this timing, and a modulation lock keeps the SPS hunt from
               correcting a stale one. */
            const dsd_decode_mode_profile profile = dsd_scan_mode_effective_profile(opts, state);
            state->samplesPerSymbol =
                dsd_opts_compute_sps_rate(opts, profile.symbol_rate_hz, current_demod_rate(opts, state));
            state->symbolCenter = dsd_opts_symbol_center(state->samplesPerSymbol);
        }
        /* The sink the restored mode writes to, as decode_mode_apply_value() opens it: a refused switch between FM and
           AM goes back to the analog monitor, which has no digital voice stream to open. */
        if (opts->analog_only) {
            (void)dsd_audio_ensure_analog_output(opts);
        } else {
            (void)dsd_audio_ensure_digital_output(opts);
        }
        dsd_symbol_analog_block_reset(state);
    }
    int changed = 0;
    if (scoped) {
        (void)ui_resume_scope_and_publish(opts, state, &changed, NULL, 0);
    }
    const int leave = kept && kept->scan_leave;
    int refused_at_once = 0;
    if (reverted && !scoped) {
        const dsd_decode_mode_profile profile = dsd_scan_mode_effective_profile(opts, state);
        if (leave) {
            refused_at_once = svc_publish_symbol_profile_after_scan_leave(opts, state, profile, -1) != 0;
        } else {
            (void)svc_publish_symbol_profile(opts, state, profile);
        }
        dsd_frame_sync_reset_acquisition(opts, state, opts->trunk_scan_enabled != 1);
        reset_call_tracking(opts, state, 1);
    }
    if (reverted) {
        const int kind = g_analog_entry.kind;
        char why[128];
        svc_describe_analog_refusal(opts, kind, width_hz, why, sizeof why);
        ui_set_toast(state, 5, "Failed: %s -> %s",
                     dsd_decode_mode_display_name(kind == DSD_ANALOG_DEMOD_AM ? DSDCFG_MODE_AM : DSDCFG_MODE_ANALOG),
                     why);
    }
    if (refused_at_once) {
        ui_settle_leave_revert_refused_at_once(opts, state, kept);
    }
    return reverted;
}

/**
 * @brief Recompute symbol timing for @p mode at the live demod rate and publish it.
 *
 * Split out of apply_decode_mode_set() so the RadioReference apply can run it
 * again after it overrides the modulation: svc_publish_symbol_profile() reads
 * state->rf_mod, so a QPSK override applied after the preset needs a second
 * publish or the front end keeps the preset's demodulator and channel filter.
 */
static int
decode_mode_republish(const dsd_opts* opts, dsd_state* state, dsdneoUserDecodeMode mode) {
    /* The SPS hunt resumes from wherever the previous mode left it, and its next
       pass overwrites the timing computed here; the RTL front end likewise keeps
       the old mode's demodulator family and channel filter until told otherwise. */
    return ui_publish_profile_timed(opts, state, dsd_decode_mode_profile_for(mode), current_demod_rate(opts, state));
}

/*
 * Whether the front end would take the receive profile a change to @p mode publishes. An explicit NFM width, and the AM
 * width (its default included, since AM always runs its channel filter), is held to the DSP rate it will run at
 * wherever the session is (svc_check_analog_bandwidth()), under a scan row as well: the row's resume or leave requests
 * the analog profile with it, and a width the front end refused there would switch the decoder to Analog or AM only to
 * put it back (ui_revert_analog_entry()), or, for a typed row's leave, leave an analog decoder on a front end running
 * something else with nothing but a log line to say so. The unset NFM default, which no rate refuses, is asked about as
 * before (svc_check_mode_receive_profile()). @p why receives a reason for the toast when there is one beyond the front
 * end's log.
 */
static int
ui_check_mode_receive_profile(const dsd_opts* opts, const dsd_state* state, dsdneoUserDecodeMode mode, char* why,
                              size_t why_size) {
    if (why && why_size > 0U) {
        why[0] = '\0';
    }
    /* The Analog preset selects NFM and AM the AM detector, each keeping the configured width of its kind
       (dsd_apply_decode_mode_preset()). */
    if (mode == DSDCFG_MODE_AM && opts->m17encoder != 1) {
        return svc_check_analog_bandwidth(opts, state, DSD_ANALOG_DEMOD_AM, opts->analog_am_bandwidth_hz, why,
                                          why_size);
    }
    if (mode == DSDCFG_MODE_ANALOG && opts->m17encoder != 1 && opts->analog_nfm_bandwidth_hz > 0) {
        return svc_check_analog_bandwidth(opts, state, DSD_ANALOG_DEMOD_FM, opts->analog_nfm_bandwidth_hz, why,
                                          why_size);
    }
    return svc_check_mode_receive_profile(opts, state, mode);
}

/* The analog monitor block the decoder has part-collected holds the old family's samples (a digital session collects
   its unsynced input there too, monitored or not), or on the monitor the old kind's (FM discriminator audio of an AM
   carrier, or the reverse): dropped when a mode change moved the decoder off them (@p was_analog, @p was_kind: the
   family and kind before it), so the first block the new family or kind plays does not start with them. */
static void
decode_mode_drop_old_analog_block(const dsd_opts* opts, dsd_state* state, int was_analog, int was_kind) {
    const int analog = opts->analog_only != 0;
    if (analog != was_analog || (analog && opts->analog_demod != was_kind)) {
        dsd_symbol_analog_block_reset(state);
    }
}

/**
 * @brief Switch which protocols are decoded, mid-session.
 *
 * Goes through the same preset helper the CLI uses, so a mode chosen here means
 * exactly what the same mode means at startup rather than a second opinion about
 * which frame_* flags it implies.
 *
 * The CLI profile specifically, not the interactive one: only its AUTO re-enables
 * the whole frame set. The other profiles leave AUTO as a label because they run
 * against freshly defaulted options where everything is already on -- but this
 * command runs against options a previous single-protocol choice has already
 * narrowed, so "listen for everything" has to actually widen them again.
 *
 * Every preset also settles the modulation, which is why a panel offering both
 * reads modulation back from the engine rather than remembering what it asked for.
 *
 * Callers that already hold a dsdneoUserDecodeMode use this directly; it does no
 * payload decoding, so nothing has to synthesize a struct dsd_app_command to
 * reach it.
 */
static int
decode_mode_apply_value(dsd_opts* opts, dsd_state* state, dsdneoUserDecodeMode mode) {
    /* The presets also carry an audio layout, but the output stream was opened
       once at session start with the layout in force then (openAudioOutput()
       reads pulse_digi_out_channels/pulse_digi_rate_out and the backend fixes
       them for the life of the stream). Letting a preset change them here would
       have dsd_play_synthesized_voice() dispatch mono writes into a stereo
       stream for the rest of the session, so the session's own layout is kept.

       dmr_stereo is deliberately not in this pair, though the presets set it
       next to them. It selects two-slot decoding, not a stream shape: the DMR
       playback paths take the channel count separately and mix both slots down
       when it is 1 (playSynthesizedVoiceSS3/FS3), so a mono session that switches
       to DMR still hears both slots. Putting it back with the channel count would
       instead leave a DMR session on dmr_stereo == 0 with dmr_mono == 0, which no
       preset produces and which dmr_handle_voice() answers by running the MS
       bootstrap against BS voice and nothing at all against MS voice. */
    const int audio_channels = opts->pulse_digi_out_channels;
    const int audio_rate = opts->pulse_digi_rate_out;
    const int was_analog = opts->analog_only != 0;
    const int was_analog_family = dsd_opts_is_analog_family(opts);
    const int was_kind = opts->analog_demod;
    /* Asked before anything changes: a front end that would refuse the analog receive profile the new mode publishes
       leaves the session in its mode, instead of an analog decoder on a front end running something else. */
    char why[128];
    if (ui_check_mode_receive_profile(opts, state, mode, why, sizeof why) != 0) {
        if (why[0] != '\0') {
            ui_set_toast(state, 5, "Failed: %s -> %s", dsd_decode_mode_display_name(mode), why);
        } else {
            ui_set_toast(state, 4, "Failed: %s -> RTL front end refused its channel (see log)",
                         dsd_decode_mode_display_name(mode));
        }
        return UI_CMD_APPLY_FAILED;
    }
    /* Released before the preset runs, not after. The presets skip their whole
       modulation block under mod_cli_lock (decode_mode_apply_dmr() and siblings),
       so on a session started with `-mq`/`-mg` the new protocol would keep the old
       modulation -- and svc_publish_symbol_profile() below reads state->rf_mod, so
       it would then ask the front end for that modulation's demodulator and channel
       filter at the new protocol's symbol rate, with the lock still on to stop the
       SPS hunt correcting it. Choosing a protocol here is a fresh answer to what is
       on this channel and outranks a `-m` flag from session start; this is the same
       release ui_apply_modulation() performs for the modulation control.

       Restored if the mode turns out to be unsupported, so a refused command
       changes nothing -- which is the contract the preset helper itself keeps. */
    const int mod_locks[3] = {opts->mod_cli_lock, opts->mod_p25p2_c4fm, opts->mod_p25p2_profile_lock};
    ui_stage_analog_entry(opts, state);
    opts->mod_p25p2_c4fm = 0;
    opts->mod_p25p2_profile_lock = 0;
    opts->mod_cli_lock = 0;
    if (dsd_apply_decode_mode_preset(mode, DSD_DECODE_PRESET_PROFILE_CLI, opts, state) != 0) {
        opts->mod_cli_lock = mod_locks[0];
        opts->mod_p25p2_c4fm = mod_locks[1];
        opts->mod_p25p2_profile_lock = mod_locks[2];
        ui_set_toast(state, 4, "Decode mode not available");
        return UI_CMD_APPLY_UNSUPPORTED;
    }
    opts->pulse_digi_out_channels = audio_channels;
    opts->pulse_digi_rate_out = audio_rate;
    /* The session's sinks were opened for the mode it started in: an analog start has no digital voice stream and
       a digital one no raw monitor stream. Open whichever the new mode writes to, if it is missing. */
    if (opts->analog_only) {
        (void)dsd_audio_ensure_analog_output(opts);
    } else {
        (void)dsd_audio_ensure_digital_output(opts);
    }
    decode_mode_drop_old_analog_block(opts, state, was_analog, was_kind);
    /* The presets write symbol timing for a 48 kHz input, and on an RTL front end
       the demod output rate is whatever the capture rate decimates to, so the
       timing has to be recomputed at the live rate or the decoder is put on the
       wrong symbol clock for the protocol it was just told to look for.

       Taken from the mode's steady-state profile rather than from the preset's
       starting timing, because the same profile decides the SPS hunt index and
       the channel filter published just below, and a mode running on one symbol
       clock with a hunt profile and a filter built for another is exactly what
       that costs. A switch onto the monitor, or between FM and AM, the front
       end refuses at once (a retune moved the demod rate since the check
       above) puts the decoder back as it was; one refused where it lands is
       put back by the next drain. */
    ui_arm_analog_entry(opts, state, was_analog_family, was_kind);
    if (decode_mode_republish(opts, state, mode) != 0
        && ui_revert_analog_entry(opts, state, dsd_opts_analog_width_hz(opts), NULL)) {
        return UI_CMD_APPLY_FAILED;
    }
    /* The decoder was hunting for a different protocol a moment ago: its
       modulation votes describe frames of the old kind, and any call open on the
       old protocol will never be closed by the new one. */
    if (!dsd_scan_mode_updating(state)) {
        dsd_frame_sync_reset_acquisition(opts, state, opts->trunk_scan_enabled != 1);
        reset_call_tracking(opts, state, 1);
    }
    ui_set_toast(state, 3, "Decoding %s", dsd_decode_mode_display_name(mode));
    return UI_CMD_APPLY_COMPLETED;
}

static int
apply_decode_mode_set(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    if (!state || c->n < (int)sizeof(int32_t)) {
        return UI_CMD_APPLY_INVALID_PAYLOAD;
    }
    dsdneoUserDecodeMode mode = DSDCFG_MODE_AUTO;
    const int early = decode_mode_set_early_verdict(opts, state, c, &mode);
    if (early != UI_CMD_APPLY_UNHANDLED) {
        return early;
    }
    return decode_mode_apply_value(opts, state, mode);
}

/**
 * @brief Everything that can refuse a RadioReference apply before it mutates anything.
 *
 * After this returns COMPLETED the sequence is best-effort and cannot roll back;
 * Each channel-map import validates before adoption and preserves its previous path on failure.
 */
static int
rr_apply_preflight(const dsd_opts* opts, dsd_state* state, const dsd_app_rr_apply_payload* p) {
    // Same invariant svc_import_channel_map() enforces for -C: a trunk-scan run
    // gets its channel maps per target. Refusing the whole apply (rather than
    // just the channel-map half, which is all svc_import_group_list would leave)
    // is what keeps a half-applied import off a live session.
    if (opts->trunk_scan_enabled == 1) {
        ui_set_toast(state, 4, "Failed: RR import -> trunk scan owns the channel map");
        return UI_CMD_APPLY_FAILED;
    }
    /* <=, not <: DSDCFG_MODE_UNSET is 0 and is not a preset, so letting it through
       would run dsd_apply_decode_mode_preset() and republish a symbol profile for
       "no mode". Neither producer can emit it today; the guard is what keeps that
       true. */
    if (p->decode_mode <= (int32_t)DSDCFG_MODE_UNSET || p->decode_mode > (int32_t)DSDCFG_MODE_AM
        || !dsd_decode_mode_runs_on_input((dsdneoUserDecodeMode)p->decode_mode, opts)) {
        ui_set_toast(state, 4, "Failed: RR import -> decode mode");
        return UI_CMD_APPLY_FAILED;
    }
    dsd_csv_validation counts;
    DSD_MEMSET(&counts, 0, sizeof counts);
    if (p->has_chan && (dsd_csv_validate_chan_file(p->chan_path, &counts) != 0 || counts.accepted == 0U)) {
        ui_set_toast(state, 4, "Failed: RR import -> channel map unreadable");
        return UI_CMD_APPLY_FAILED;
    }
    DSD_MEMSET(&counts, 0, sizeof counts);
    if (p->has_group && (dsd_csv_validate_group_file(p->group_path, &counts) != 0 || counts.accepted == 0U)) {
        ui_set_toast(state, 4, "Failed: RR import -> group list unreadable");
        return UI_CMD_APPLY_FAILED;
    }
    return UI_CMD_APPLY_COMPLETED;
}

/* decode_mode_apply_edacs_pv() resets both fields to 0, so the variant has to be
   restated after the preset. Values verified against the optarg[0] == 'h'/'H'/
   'e'/'E' branches of case 'f': in the args macro (grep state->esk_mask in
   src/runtime/cli/args.c): -fh/-fe leave 0, -fH/-fE set 0xA0. */
static void
rr_apply_edacs_variants(dsd_state* state, const dsd_app_rr_apply_payload* p, dsdneoUserDecodeMode mode) {
    if (mode != DSDCFG_MODE_EDACS_PV) {
        return;
    }
    state->ea_mode = p->edacs_ea ? 1 : 0;
    state->esk_mask = p->edacs_esk ? (unsigned short)0xA0 : (unsigned short)0;
}

/* The `-mq` block, including the two case 'm': prologue lines. mod_cli_lock is
   load-bearing: snapshot_demod_config() in src/runtime/config_user.cpp returns
   early without it, so Config->Save would emit no [demod] section at all. */
static void
rr_apply_simulcast(dsd_opts* opts, dsd_state* state, const dsd_app_rr_apply_payload* p) {
    if (!p->simulcast_qpsk) {
        return;
    }
    opts->mod_p25p2_c4fm = 0;
    opts->mod_p25p2_profile_lock = 0;
    opts->mod_c4fm = 0;
    opts->mod_qpsk = 1;
    opts->mod_gfsk = 0;
    state->rf_mod = 1;
    opts->mod_cli_lock = 1;
}

/* One automatic tuner owner at a time, mirroring ui_handle_trunk_set and
   ui_handle_scanner_toggle in src/app_control/actions/actions_trunk.c.
   opts->trunk_cli_seen is CLI provenance and is deliberately not touched -- the
   in-session action handlers do not touch it either. */
static void
rr_apply_tuner_owner(dsd_opts* opts, dsd_state* state, const dsd_app_rr_apply_payload* p) {
    if (!p) {
        return;
    }
    if (p->trunking) {
        opts->trunk_enable = 1;
        opts->scanner_mode = 0;
    } else if (p->scanner) {
        opts->scanner_mode = 1;
        opts->trunk_enable = 0;
    } else {
        opts->trunk_enable = 0;
        opts->scanner_mode = 0;
    }
    if (!p->scanner) {
        (void)svc_leave_channel_scan(opts, state);
        dsd_scan_keys_leave(state);
    }
}

/* A half the import did not write CLEARS the live one rather than leaving it:
   this command re-points the session at a different system, and a stale channel
   map is not merely out of date, it resolves the new system's voice grants
   against the old system's frequencies and tunes off-system. chan_need == 2
   protocols (DMR Cap+/Con+/TIII) legitimately produce no channel map at all, so
   this is an ordinary outcome, not an edge case. services.h says the same thing
   about why the clear counterparts exist: "the previous file would otherwise
   stay live for the session". */
static int
rr_apply_files(dsd_opts* opts, dsd_state* state, const dsd_app_rr_apply_payload* p) {
    int rc = UI_CMD_APPLY_COMPLETED;
    if (p->has_chan) {
        if (svc_import_channel_map(opts, state, p->chan_path) != 0) {
            rc = UI_CMD_APPLY_FAILED;
        }
    } else {
        (void)svc_clear_channel_map(opts, state);
    }
    if (p->has_group) {
        if (svc_import_group_list(opts, state, p->group_path) != 0) {
            rc = UI_CMD_APPLY_FAILED;
        }
    } else {
        (void)svc_clear_group_list(opts, state);
    }
    return rc;
}

/* io_control_set_freq() is compiled unconditionally and dispatches rigctl
   (opts->use_rigctl == 1) and RTL (opts->audio_in_type == AUDIO_IN_RTL) itself,
   returning -1 when neither owns the tuner. 0 is applied and
   RTL_STREAM_TUNE_TIMEOUT is accepted-pending; anything else is a session that
   cannot retune (WAV, stdin, UDP, symbol file), which the preview already warned
   about and which must not fail the whole apply. Returns 1 when the tune was
   accepted (applied or pending), 0 otherwise. */
static int
rr_apply_tune(dsd_opts* opts, const dsd_state* state, const dsd_app_rr_apply_payload* p) {
    if (p->tune_hz == 0U) {
        return 0;
    }
    /* io_control_set_freq() takes a long, which is 32-bit signed under the
       win-msvc-* presets while the payload carries a full uint32_t (and
       rr_generate.c accepts sites up to 6 GHz). Casting a value above LONG_MAX
       there would hand the tuner a NEGATIVE frequency, so the retune is skipped
       instead - the same outcome as a session that cannot retune at all, which
       the import preview already warns about. Guarded by the preprocessor because
       where long is 64-bit the test is provably false and -Werror=type-limits
       rejects it. */
#if LONG_MAX < UINT32_MAX
    if (p->tune_hz > (uint32_t)LONG_MAX) {
        return 0;
    }
#endif
    const int rc = io_control_set_freq(opts, state, (long int)p->tune_hz);
    return rc == RTL_STREAM_TUNE_OK || rc == RTL_STREAM_TUNE_TIMEOUT;
}

/* The session was just re-pointed at a different system, so the old system's CC
   candidates and sync timestamps are wrong rather than merely stale. This is the
   DSD_APP_CMD_SIM_NOCAR field set WITHOUT noCarrier(): the call state is already
   ended by reset_call_tracking below, and noCarrier() would additionally unwind
   the audio path for a tune that was requested, not lost. */
static void
rr_apply_reacquire(dsd_opts* opts, dsd_state* state) {
    dsd_trunk_cc_candidates_reset(state);
    state->last_cc_sync_time = 0;
    state->last_vc_sync_time = 0;
    state->last_vc_sync_time_m = 0.0;
    if (!dsd_scan_mode_updating(state)) {
        dsd_frame_sync_reset_acquisition(opts, state, opts->trunk_scan_enabled != 1);
        reset_call_tracking(opts, state, 1);
    }
}

/**
 * @brief Apply a finished RadioReference import to the live session.
 *
 * Runs decode_mode_apply_value() rather than apply_decode_mode_set(): the latter
 * short-circuits through decode_mode_set_early_verdict() whenever the session is
 * already in the target mode, which skips the preset, the SPS recompute and the
 * symbol-profile publish -- exactly what a refresh re-applying the same mode
 * needs to happen. It also avoids putting a second ~16 KB struct dsd_app_command
 * on the decoder thread's frame; the drain already holds one.
 */
static int
apply_rr_import(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    if (!opts || !state || c->n < sizeof(dsd_app_rr_apply_payload)) {
        return UI_CMD_APPLY_INVALID_PAYLOAD;
    }
    dsd_app_rr_apply_payload p;
    DSD_MEMCPY(&p, c->data, sizeof p);
    p.chan_path[sizeof p.chan_path - 1] = '\0';
    p.group_path[sizeof p.group_path - 1] = '\0';

    const int pre = rr_apply_preflight(opts, state, &p);
    if (pre != UI_CMD_APPLY_COMPLETED) {
        return pre;
    }
    const dsdneoUserDecodeMode mode = (dsdneoUserDecodeMode)p.decode_mode;
    (void)svc_leave_channel_scan(opts, state);
    if (decode_mode_apply_value(opts, state, mode) != UI_CMD_APPLY_COMPLETED) {
        ui_set_toast(state, 4, "Failed: RR import -> decode mode");
        return UI_CMD_APPLY_FAILED;
    }
    rr_apply_edacs_variants(state, &p, mode);
    rr_apply_simulcast(opts, state, &p);
    opts->p25_prefer_candidates = p.p25_prefer_candidates ? (uint8_t)1 : (uint8_t)0;
    rr_apply_tuner_owner(opts, state, &p);
    const int files_rc = rr_apply_files(opts, state, &p);
    /* Unconditional, and after every write to state->rf_mod: svc_publish_symbol_profile()
       reads it, so the simulcast override needs a second publish, and a re-apply
       onto an already-matching mode needs the first one. */
    (void)decode_mode_republish(opts, state, mode);
    if (rr_apply_tune(opts, state, &p)) {
        /* The receiver moved to the imported system's channel, which can sync before any no-carrier pass ends the
           reception on the old one (issue #575). The import runs under the P25 SM tick guard. */
        ui_leave_carrier(opts, state, 1);
    }
    rr_apply_reacquire(opts, state);

    if (files_rc != UI_CMD_APPLY_COMPLETED) {
        ui_set_toast(state, 4, "Failed: RR import -> CSV import");
        return UI_CMD_APPLY_FAILED;
    }
    ui_set_toast(state, 4, "Applied: RR import (%s)%s; save via Config menu", dsd_decode_mode_display_name(mode),
                 p.trunking ? " +trunking" : (p.scanner ? " +scanner" : ""));
    return UI_CMD_APPLY_COMPLETED;
}

/* The UI thread never writes dsd_opts: the opts pointer a menu action receives
   alternates between the live struct and the read-only published snapshot, and
   the decoder thread republishes opts by whole-struct copy after every drained
   command. */
static int
apply_rr_account_set(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    if (!opts || !state || c->n < sizeof(dsd_app_rr_account_payload)) {
        return UI_CMD_APPLY_INVALID_PAYLOAD;
    }
    dsd_app_rr_account_payload account;
    DSD_MEMCPY(&account, c->data, sizeof account);
    account.username[sizeof account.username - 1] = '\0';
    account.app_key[sizeof account.app_key - 1] = '\0';
    DSD_SNPRINTF(opts->rr_username, sizeof opts->rr_username, "%s", account.username);
    DSD_SNPRINTF(opts->rr_app_key, sizeof opts->rr_app_key, "%s", account.app_key);
    /* Never echo either field: tools/check_secret_redaction.sh runs on every push
       and the credential policy in radioreference.h forbids it outright. */
    ui_set_toast(state, 3, "Applied: RadioReference account");
    return UI_CMD_APPLY_COMPLETED;
}

static int
apply_cmd_trunk_controls(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    switch (c->id) {
        case DSD_APP_CMD_DMR_RESET:
            if (!state) {
                return 1;
            }
            state->dmr_rest_channel = -1;
            state->dmr_mfid = -1;
            DSD_SNPRINTF(state->dmr_branding_sub, sizeof state->dmr_branding_sub, "%s", "");
            DSD_SNPRINTF(state->dmr_branding, sizeof state->dmr_branding, "%s", "");
            DSD_SNPRINTF(state->dmr_site_parms, sizeof state->dmr_site_parms, "%s", "");
            opts->dmr_dmrla_is_set = 0;
            opts->dmr_dmrla_n = 0;
            state->nxdn_location_site_code = 0;
            state->nxdn_location_sys_code = 0;
            DSD_SNPRINTF(state->nxdn_location_category, sizeof state->nxdn_location_category, "%s", " ");
            state->nxdn_last_ran = -1;
            state->nxdn_ran = 0;
            state->nxdn_rcn = 0;
            state->nxdn_base_freq = 0;
            state->nxdn_step = 0;
            state->nxdn_bw = 0;
            return 1;
        case DSD_APP_CMD_TCP_CONNECT_AUDIO: {
            /* A copy: the switch rewrites tcp_hostname once it connects. */
            char host[sizeof opts->tcp_hostname];
            DSD_SNPRINTF(host, sizeof host, "%s", opts->tcp_hostname);
            return ui_cmd_tcp_connect_audio(opts, state, host, opts->tcp_portno);
        }
        case DSD_APP_CMD_RIGCTL_CONNECT: {
            /* The TCP input's host at the rigctl port, through the service the menu's connect takes too (issue #589).
               The toasts name the host asked for: a failed reconnect keeps the old one in rigctlhostname. */
            const int rc = svc_rigctl_connect(opts, state, opts->tcp_hostname, opts->rigctlportno);
            if (rc == 0) {
                ui_set_toast(state, 3, "Rigctl connected: %s:%d", opts->tcp_hostname, opts->rigctlportno);
            } else {
                ui_set_toast(state, 4, "Rigctl connect failed: %s:%d", opts->tcp_hostname, opts->rigctlportno);
            }
            return ui_cmd_apply_status_from_service_rc(rc);
        }
        case DSD_APP_CMD_RETURN_CC: return apply_manual_return_to_cc(opts, state);
        case DSD_APP_CMD_TUNER_RELEASE: return apply_tuner_release(opts, state);
        case DSD_APP_CMD_DECODE_MODE_SET: return apply_decode_mode_set(opts, state, c);
        case DSD_APP_CMD_RR_APPLY_IMPORT: return apply_rr_import(opts, state, c);
        case DSD_APP_CMD_RR_ACCOUNT_SET: return apply_rr_account_set(opts, state, c);
        case DSD_APP_CMD_SIM_NOCAR:
            if (!state) {
                return 1;
            }
            state->last_cc_sync_time = 0;
            state->last_vc_sync_time = 0;
            state->last_vc_sync_time_m = 0.0;
            noCarrier(opts, state);
            return 1;
        default: return 0;
    }
}

static int
apply_cmd_lockout_resolve_target(const dsd_state* state, const struct dsd_app_command* c, uint8_t* slot_out,
                                 uint32_t* target_out) {
    uint8_t slot = 0U;
    if (c->n >= 1) {
        DSD_MEMCPY(&slot, c->data, 1U);
    }
    dsd_call_snapshot call;
    if (dsd_call_state_get(state, slot & 1U, &call) <= 0 || call.phase != DSD_CALL_PHASE_ACTIVE) {
        return 0;
    }
    // The over-the-air target, as Skip uses: it is the talkgroup every frontend
    // shows. On a patched P25 call the policy target is only the member WG the
    // grant matched, and locking that member leaves the supergroup free to tune.
    const uint64_t target = call.ota_target_id != 0U ? call.ota_target_id : call.policy_target_id;
    if (target == 0U || target > UINT32_MAX) {
        return 0;
    }
    *slot_out = slot;
    *target_out = (uint32_t)target;
    return 1;
}

static int
tg_listen_apply(dsd_state* state, uint32_t id_start, uint32_t id_end, int listen) {
    return dsd_tg_policy_set_mode(state, id_start, id_end, listen ? "A" : "B");
}

static void
tg_listen_persist(dsd_opts* opts, dsd_state* state) {
    if (dsd_scan_groups_row_active(state)) {
        return;
    }
    if (dsd_tg_policy_write_group_file(opts, state) != 0) {
        LOG_WARN("WARNING: Talkgroup list changes could not be written to '%s'.\n", opts->group_in_file);
        ui_set_toast(state, 4, "Talkgroup change kept for this session only");
    }
}

static int
tg_listen_row_is_editable(const dsd_tg_policy_entry* entry) {
    return entry->source != DSD_TG_POLICY_SOURCE_RUNTIME_ALIAS && strcmp(entry->mode, "D") != 0;
}

/* Only mode/allow-list denial can change as a result of a row edit. A patched
 * P25 call is judged on the member WG its grant matched, plus the supergroup's
 * own final blocks (a mode row on the supergroup releases it; an allow-list miss
 * there does not, since a listed member is how patch-aware following admits it). */
static unsigned int
tg_listen_blocked_slots(const dsd_opts* opts, const dsd_state* state) {
    unsigned int blocked = 0;
    for (unsigned int slot = 0; slot < 2U; ++slot) {
        dsd_call_snapshot call;
        if (dsd_call_state_get(state, slot, &call) <= 0 || call.phase != DSD_CALL_PHASE_ACTIVE
            || call.kind == DSD_CALL_KIND_PRIVATE_VOICE) {
            continue;
        }
        const uint64_t target = call.policy_target_id ? call.policy_target_id : call.ota_target_id;
        if (target == 0U || target > UINT32_MAX) {
            continue;
        }
        dsd_tg_policy_decision decision;
        if (dsd_tg_policy_evaluate_group_call(opts, state, (uint32_t)target, 0, 0, 0, &decision) != 0) {
            continue;
        }
        if (call.ota_target_id <= UINT32_MAX) {
            (void)dsd_tg_policy_apply_ota_final_blocks(opts, state, (uint32_t)call.ota_target_id, (uint32_t)target, 0,
                                                       &decision);
        }
        if (decision.block_reasons & (DSD_TG_POLICY_BLOCK_MODE | DSD_TG_POLICY_BLOCK_ALLOWLIST)) {
            blocked |= 1U << slot;
        }
    }
    return blocked;
}

static void
tg_listen_release_blocked_calls(dsd_opts* opts, dsd_state* state, unsigned int eligible_slots) {
    if (!(tg_listen_blocked_slots(opts, state) & eligible_slots)) {
        return;
    }
    if (apply_lockout_decoder_transition_locked(opts, state, p25_sm_owns_trunk(opts, state)) == UI_CMD_APPLY_FAILED) {
        ui_set_toast(state, 4, "Talkgroup not tuned; return-to-CC tune failed");
    }
}

/* WP-D1: Every talkgroup edit checks the same pair: comparing only the generation
 * would allow a row from the previous scan target to address a different list. */
static int
tg_edit_check_version(dsd_state* state, uint64_t context, unsigned int generation) {
    uint64_t current_context = 0;
    unsigned int current_generation = 0;
    dsd_tg_policy_table_version(state, &current_context, &current_generation);
    if (context != current_context || generation != current_generation) {
        ui_set_toast(state, 3, "Talkgroup list changed: stale edit rejected");
        return UI_CMD_APPLY_FAILED;
    }
    return UI_CMD_APPLY_COMPLETED;
}

static int
tg_row_fields_valid(const dsd_app_tg_row_payload* p) {
    const uint32_t all = DSD_APP_TG_FIELD_LISTEN | DSD_APP_TG_FIELD_PRIORITY | DSD_APP_TG_FIELD_PREEMPT
                         | DSD_APP_TG_FIELD_NAME | DSD_APP_TG_FIELD_TAGS;
    return !(!p->fields || (p->fields & ~all)
             || ((p->fields & DSD_APP_TG_FIELD_LISTEN) && p->listen != 0 && p->listen != 1)
             || ((p->fields & DSD_APP_TG_FIELD_PREEMPT) && p->preempt != 0 && p->preempt != 1));
}

static int
apply_cmd_tg_row_set(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    dsd_app_tg_row_payload p;
    DSD_MEMCPY(&p, c->data, sizeof p);
    if (tg_edit_check_version(state, p.policy_context, p.policy_generation) != UI_CMD_APPLY_COMPLETED) {
        return UI_CMD_APPLY_FAILED;
    }
    if (!tg_row_fields_valid(&p)) {
        ui_set_toast(state, 3, "Invalid talkgroup fields");
        return UI_CMD_APPLY_INVALID_PAYLOAD;
    }
    dsd_tg_policy_entry values = {0};
    uint32_t mask = 0;
    if (p.fields & DSD_APP_TG_FIELD_LISTEN) {
        mask |= DSD_TG_POLICY_FIELD_LISTEN;
    }
    if (p.fields & DSD_APP_TG_FIELD_PRIORITY) {
        mask |= DSD_TG_POLICY_FIELD_PRIORITY;
    }
    if (p.fields & DSD_APP_TG_FIELD_PREEMPT) {
        mask |= DSD_TG_POLICY_FIELD_PREEMPT;
    }
    if (p.fields & DSD_APP_TG_FIELD_NAME) {
        mask |= DSD_TG_POLICY_FIELD_NAME;
    }
    if (p.fields & DSD_APP_TG_FIELD_TAGS) {
        mask |= DSD_TG_POLICY_FIELD_TAGS;
    }
    DSD_SNPRINTF(values.mode, sizeof values.mode, "%s", p.listen ? "A" : "B");
    values.priority = p.priority;
    values.preempt = (uint8_t)p.preempt;
    DSD_MEMCPY(values.name, p.name, sizeof values.name);
    DSD_MEMCPY(values.tags, p.tags, sizeof values.tags);
    const unsigned int eligible_slots = ~tg_listen_blocked_slots(opts, state);
    if (dsd_tg_policy_set_fields(state, p.id_start, p.id_end, &values, mask) != 0) {
        ui_set_toast(state, 3, "Talkgroup edit refused: invalid fields or alias row");
        return UI_CMD_APPLY_FAILED;
    }
    ui_set_toast(state, 3, "Applied: Talkgroup updated");
    tg_listen_persist(opts, state);
    tg_listen_release_blocked_calls(opts, state, eligible_slots);
    return UI_CMD_APPLY_COMPLETED;
}

static int
apply_cmd_tg_row_remove(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    dsd_app_tg_range_payload p;
    DSD_MEMCPY(&p, c->data, sizeof p);
    if (tg_edit_check_version(state, p.policy_context, p.policy_generation) != UI_CMD_APPLY_COMPLETED) {
        return UI_CMD_APPLY_FAILED;
    }
    const unsigned int eligible_slots = ~tg_listen_blocked_slots(opts, state);
    if (dsd_tg_policy_remove_bounds(state, p.id_start, p.id_end) != 0) {
        ui_set_toast(state, 3, "Talkgroup remove refused: missing or alias row");
        return UI_CMD_APPLY_FAILED;
    }
    ui_set_toast(state, 3, "Applied: Talkgroup removed");
    tg_listen_persist(opts, state);
    tg_listen_release_blocked_calls(opts, state, eligible_slots);
    return UI_CMD_APPLY_COMPLETED;
}

static int
apply_cmd_tg_list_export(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    uint64_t context = 0;
    unsigned int generation = 0;
    /* Flexible member can precede tail padding; read fields without unaligned casts. */
    DSD_MEMCPY(&context, c->data + offsetof(dsd_app_tg_export_payload, policy_context), sizeof context);
    DSD_MEMCPY(&generation, c->data + offsetof(dsd_app_tg_export_payload, policy_generation), sizeof generation);
    if (tg_edit_check_version(state, context, generation) != UI_CMD_APPLY_COMPLETED) {
        return UI_CMD_APPLY_FAILED;
    }
    const char* path = (const char*)c->data + offsetof(dsd_app_tg_export_payload, path);
    const char* end = memchr(path, 0, c->n - offsetof(dsd_app_tg_export_payload, path));
    if (!opts || !end || end == path || (size_t)(end - path) >= sizeof opts->group_in_file) {
        ui_set_toast(state, 3, "Invalid export path");
        return UI_CMD_APPLY_INVALID_PAYLOAD;
    }
    if (dsd_scan_groups_row_active(state)) {
        ui_set_toast(state, 3, "Talkgroup export refused in scan-row context");
        return UI_CMD_APPLY_FAILED;
    }
    /* Only the output path is needed; do not copy unrelated options or secrets. */
    dsd_opts* output = calloc(1, sizeof(*output));
    if (!output) {
        ui_set_toast(state, 3, "Talkgroup export failed: out of memory");
        return UI_CMD_APPLY_FAILED;
    }
    DSD_SNPRINTF(output->group_in_file, sizeof output->group_in_file, "%s", path);
    int rc = dsd_tg_policy_write_group_file(output, state);
    free(output);
    if (rc != 0) {
        ui_set_toast(state, 3, "Talkgroup export failed");
        return UI_CMD_APPLY_FAILED;
    }
    DSD_SNPRINTF(opts->group_in_file, sizeof opts->group_in_file, "%s", path);
    ui_set_toast(state, 3, "Applied: Talkgroup list exported");
    return UI_CMD_APPLY_COMPLETED;
}

/* End WP-D1 talkgroup edit/export handlers. */

/* WP-D2: typed direct keys share CLI parsing and scan baseline ownership. */
static int
apply_cmd_key_direct(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    dsd_app_key_direct_payload p;
    DSD_MEMCPY(&p, c->data, sizeof p);
    dsd_key_type type;
    const char* shape;
    switch (p.key_type) {
        case DSD_APP_KEY_TYPE_BASIC:
            type = DSD_KEY_TYPE_BASIC;
            shape = "Expected decimal 0..255";
            break;
        case DSD_APP_KEY_TYPE_HEX:
            type = DSD_KEY_TYPE_HEX;
            shape = "Expected 10, 32, or 64 hex digits";
            break;
        case DSD_APP_KEY_TYPE_RC4:
            type = DSD_KEY_TYPE_RC4;
            shape = "Expected 1..16 hex digits";
            break;
        case DSD_APP_KEY_TYPE_SCRAMBLER:
            type = DSD_KEY_TYPE_SCRAMBLER;
            shape = "Expected decimal 0..32767";
            break;
        case DSD_APP_KEY_TYPE_M17_SCRAMBLER:
            type = DSD_KEY_TYPE_M17_SCRAMBLER;
            shape = "Expected a nonzero M17 seed with 2, 4, or 6 hex digits";
            break;
        case DSD_APP_KEY_TYPE_M17_AES:
            type = DSD_KEY_TYPE_M17_AES;
            shape = "Expected a nonzero M17 AES key with 32, 48, or 64 hex digits";
            break;
        default:
            DSD_SECURE_ZERO(&p, sizeof p);
            ui_set_toast(state, 3, "Invalid key type");
            return UI_CMD_APPLY_INVALID_PAYLOAD;
    }
    dsd_key_direct_result result = DSD_KEY_DIRECT_INVALID_ARGUMENT;
    if (memchr(p.value, 0, sizeof p.value)) {
        result = dsd_scan_keys_apply_direct(state, type, p.value);
        if (result == DSD_KEY_DIRECT_OK) {
            // A global edit must not change an active row's signalled KIDs or
            // loader. The common mute reset also serves unscoped direct edits.
            const int row_keyloader = state->keyloader;
            const int row_kid = state->payload_keyid;
            const int row_kid_right = state->payload_keyidR;
            char row_profile_ref[64];
            DSD_MEMCPY(row_profile_ref, state->key_profile_ref, sizeof(row_profile_ref));
            ui_cmd_reset_key_mute_state(opts, state);
            if (state->scan_keys_active_set) {
                state->keyloader = row_keyloader;
                state->payload_keyid = row_kid;
                state->payload_keyidR = row_kid_right;
                DSD_MEMCPY(state->key_profile_ref, row_profile_ref, sizeof(row_profile_ref));
            }
        }
    }
    DSD_SECURE_ZERO(&p, sizeof p);
    if (result != DSD_KEY_DIRECT_OK) {
        ui_set_toast(state, 3, "%s", shape);
        return UI_CMD_APPLY_INVALID_PAYLOAD;
    }
    ui_set_toast(state, 3, "Key applied");
    return UI_CMD_APPLY_COMPLETED;
}

static int
apply_cmd_force_key(dsd_state* state, const struct dsd_app_command* c) {
    int32_t mode;
    DSD_MEMCPY(&mode, c->data, sizeof mode);
    const int previous = state->M;
    if (dsd_key_apply_force(state, mode) != DSD_KEY_DIRECT_OK) {
        ui_set_toast(state, 3, "Expected force key mode 0, 1, or 2");
        return UI_CMD_APPLY_INVALID_PAYLOAD;
    }
    if (state->M != previous) {
        dsd_enc_lockout_bump_key_epoch(state);
    }
    return UI_CMD_APPLY_COMPLETED;
}

static int
parse_selection_uint(char** cursor, uint32_t* value, int final) {
    const char* at = *cursor;
    char* end = NULL;
    if (*at < '0' || *at > '9') {
        return 0;
    }
    errno = 0;
    const unsigned long long parsed = strtoull(at, &end, 10);
    if (errno || end == at || parsed > UINT32_MAX) {
        return 0;
    }
    if (final) {
        if (*end && strcmp(end, "\n") != 0 && strcmp(end, "\r\n") != 0) {
            return 0;
        }
    } else if (*end != ',') {
        return 0;
    }
    *value = (uint32_t)parsed;
    *cursor = end + (final ? 0 : 1);
    return 1;
}

static int
parse_selection_line(const char* line, dsd_tg_policy_selection* entry) {
    char* at = NULL;
    errno = 0;
    const long long index = strtoll(line, &at, 10);
    if (errno || at == line || *at != ',' || index < -1 || index > INT32_MAX) {
        return 0;
    }
    ++at;
    entry->policy_index = (int32_t)index;
    return parse_selection_uint(&at, &entry->id_start, 0) && parse_selection_uint(&at, &entry->id_end, 1);
}

static int
read_tg_selection(FILE* file, dsd_tg_policy_selection* entries, size_t expected) {
    char line[128];
    size_t count = 0;
    while (fgets(line, sizeof(line), file)) {
        if (count >= expected || !parse_selection_line(line, &entries[count])) {
            return 0;
        }
        ++count;
    }
    return !ferror(file) && count == expected;
}

static int
apply_cmd_tg_selection(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* command) {
    dsd_app_tg_selection_payload p;
    DSD_MEMCPY(&p, command->data, sizeof(p));
    if (!p.count || p.count > 1000000U || !memchr(p.selection_path, 0, sizeof(p.selection_path))
        || (p.listening != 0 && p.listening != 1)) {
        return UI_CMD_APPLY_INVALID_PAYLOAD;
    }
    FILE* file = dsd_fopen_existing_regular_file(p.selection_path, "rb");
    if (!file) {
        ui_set_toast(state, 4, "Could not read the captured talkgroup selection");
        return UI_CMD_APPLY_FAILED;
    }
    dsd_tg_policy_selection* entries = calloc(p.count, sizeof(*entries));
    if (!entries) {
        fclose(file);
        return UI_CMD_APPLY_FAILED;
    }
    const int valid = read_tg_selection(file, entries, p.count) && !dsd_exitflag_load();
    fclose(file);
    const int rc = valid ? dsd_tg_policy_set_listening_selection(state, p.policy_context, p.policy_generation, entries,
                                                                 p.count, p.listening)
                         : 1;
    free(entries);
    if (rc) {
        ui_set_toast(state, 4, "The talkgroup selection changed or could not be applied. Review it again.");
        return UI_CMD_APPLY_FAILED;
    }
    tg_listen_persist(opts, state);
    if (!p.listening) {
        tg_listen_release_blocked_calls(opts, state, 3U);
    }
    ui_set_toast(state, 4, "%u selected talkgroups %s", p.count, p.listening ? "listening" : "not tuned");
    return UI_CMD_APPLY_COMPLETED;
}

static int
apply_cmd_foundation(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    (void)opts;
    switch (c->id) {
        case DSD_APP_CMD_TG_LOCKOUT_PERSIST_SET: {
            int32_t persist = 0;
            DSD_MEMCPY(&persist, c->data, sizeof persist);
            if (persist != 0 && persist != 1) {
                return UI_CMD_APPLY_INVALID_PAYLOAD;
            }
            opts->persist_tg_lockouts = (uint8_t)persist;
            ui_set_toast(state, 3, "User TG lockouts: %s", persist ? "save to group list" : "session only");
            return UI_CMD_APPLY_COMPLETED;
        }
        case DSD_APP_CMD_TG_SESSION_AVOID_CLEAR: {
            uint64_t expected = 0;
            uint64_t current = 0;
            DSD_MEMCPY(&expected, c->data, sizeof expected);
            dsd_tg_policy_table_version(state, &current, NULL);
            if (current != expected) {
                ui_set_toast(state, 3, "Talkgroup list changed: temporary avoids were not cleared");
                return UI_CMD_APPLY_FAILED;
            }
            dsd_tg_policy_session_avoid_clear(state);
            dsd_tg_policy_call_skip_clear(state);
            ui_set_toast(state, 3, "Cleared temporary TG avoids in current list");
            return UI_CMD_APPLY_COMPLETED;
        }
        case DSD_APP_CMD_TG_ROW_SET: return apply_cmd_tg_row_set(opts, state, c);
        case DSD_APP_CMD_TG_SELECTION_SET: return apply_cmd_tg_selection(opts, state, c);
        case DSD_APP_CMD_TG_ROW_REMOVE: return apply_cmd_tg_row_remove(opts, state, c);
        case DSD_APP_CMD_TG_LIST_EXPORT: return apply_cmd_tg_list_export(opts, state, c);
        case DSD_APP_CMD_KEY_DIRECT_SET: return apply_cmd_key_direct(opts, state, c);
        case DSD_APP_CMD_DECRYPTION_APPLY: return dsd_app_apply_decryption(opts, state, c);
        case DSD_APP_CMD_FORCE_KEY_SET: return apply_cmd_force_key(state, c);
        default: return UI_CMD_APPLY_UNHANDLED;
    }
}

static int
apply_cmd_tg_listen_one(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    dsd_app_tg_listen_payload payload;
    DSD_MEMCPY(&payload, c->data, sizeof payload);
    if (payload.id_start > payload.id_end) {
        ui_set_toast(state, 3, "Invalid talkgroup range");
        return UI_CMD_APPLY_FAILED;
    }
    const int rc = tg_listen_apply(state, payload.id_start, payload.id_end, payload.listen);
    if (rc == 1) {
        ui_set_toast(state, 3, "Range %u-%u is not on the list", (unsigned)payload.id_start, (unsigned)payload.id_end);
        return UI_CMD_APPLY_FAILED;
    }
    if (rc != 0) {
        LOG_WARN("WARNING: Could not update TG %u (rc=%d).\n", (unsigned)payload.id_start, rc);
        ui_set_toast(state, 4, "Could not update TG %u", (unsigned)payload.id_start);
        return UI_CMD_APPLY_FAILED;
    }
    tg_listen_persist(opts, state);
    if (!payload.listen) {
        tg_listen_release_blocked_calls(opts, state, 3U);
    }
    return UI_CMD_APPLY_COMPLETED;
}

static int
apply_cmd_tg_listen_all(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    dsd_app_tg_listen_all_payload payload;
    DSD_MEMCPY(&payload, c->data, sizeof payload);
    payload.tags[sizeof payload.tags - 1U] = '\0';
    size_t applied = 0;
    for (size_t i = 0; i < dsd_tg_policy_entry_count(state); ++i) {
        dsd_tg_policy_entry entry;
        if (!dsd_tg_policy_entry_at(state, i, &entry) || !tg_listen_row_is_editable(&entry)
            || (payload.tags[0] && strcmp(entry.tags, payload.tags) != 0)) {
            continue;
        }
        if (dsd_tg_policy_set_mode_at(state, i, payload.listen ? "A" : "B") == 0) {
            ++applied;
        }
    }
    ui_set_toast(state, 3, "%zu talkgroups %s", applied, payload.listen ? "listening" : "not tuned");
    tg_listen_persist(opts, state);
    if (!payload.listen) {
        tg_listen_release_blocked_calls(opts, state, 3U);
    }
    return UI_CMD_APPLY_COMPLETED;
}

static int
apply_cmd_tg_listen(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    if (c->id != DSD_APP_CMD_TG_LISTEN_SET && c->id != DSD_APP_CMD_TG_LISTEN_SET_ALL) {
        return 0;
    }
    if (!state) {
        return 1;
    }
    if (c->id == DSD_APP_CMD_TG_LISTEN_SET) {
        return apply_cmd_tg_listen_one(opts, state, c);
    }
    return apply_cmd_tg_listen_all(opts, state, c);
}

static void
tg_lockout_report(dsd_opts* opts, dsd_state* state, unsigned int tg) {
    const char* suffix = !opts->persist_tg_lockouts                                     ? " for this session"
                         : !opts->group_in_file[0] || dsd_scan_groups_row_active(state) ? " in current list (not saved)"
                                                                                        : "";
    ui_set_toast(state, 3, "TG %u locked out%s", tg, suffix);
    if (opts->persist_tg_lockouts) {
        tg_listen_persist(opts, state);
    }
}

static void
slot_block_note_event(dsd_opts* opts, dsd_state* state, uint8_t slot, const char* text) {
    dsd_event_note_current_call(opts, state, slot == 0 ? 0U : 1U, text);
}

static int
apply_cmd_lockout_slot(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    if (!state) {
        return (c && c->id == DSD_APP_CMD_LOCKOUT_SLOT) ? 1 : 0;
    }
    if (c->id != DSD_APP_CMD_LOCKOUT_SLOT) {
        return 0;
    }
    uint8_t slot = 0U;
    uint32_t target = 0U;
    int upsert_rc = 0;
    if (opts->frame_provoice == 1) {
        return 1;
    }
    if (!apply_cmd_lockout_resolve_target(state, c, &slot, &target)) {
        return 1;
    }
    const unsigned int tg = target;
    const int temporary = !opts->persist_tg_lockouts;
    upsert_rc = temporary ? dsd_tg_policy_session_avoid_add(state, target) : tg_listen_apply(state, target, target, 0);
    if (upsert_rc != 0) {
        LOG_WARN("WARNING: User lockout for TG %u could not be applied (rc=%d).\n", tg, upsert_rc);
        ui_set_toast(state, 4, "Could not lock out TG %u", tg);
        return UI_CMD_APPLY_FAILED;
    }

    char text[128];
    DSD_SNPRINTF(text, sizeof text, "Target: %u; has been locked out; %s.", tg,
                 temporary ? "Session Only" : "User Lock Out");
    slot_block_note_event(opts, state, slot, text);

    tg_lockout_report(opts, state, tg);

    const int transition_status = apply_lockout_decoder_transition_locked(opts, state, p25_sm_owns_trunk(opts, state));
    if (transition_status == UI_CMD_APPLY_FAILED) {
        LOG_WARN("WARNING: User lockout for TG %u was applied, but the return-to-CC cleanup tune was not accepted.\n",
                 tg);
        ui_set_toast(state, 4, "TG %u locked out%s; return-to-CC tune failed", tg,
                     temporary ? " for this session" : "");
    }
    return UI_CMD_APPLY_COMPLETED;
}

static int
apply_cmd_skip_slot(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    if (c->id != DSD_APP_CMD_SKIP_SLOT) {
        return UI_CMD_APPLY_UNHANDLED;
    }
    if (!state || opts->frame_provoice == 1) {
        return UI_CMD_APPLY_COMPLETED;
    }
    uint8_t slot = 0;
    DSD_MEMCPY(&slot, c->data, sizeof slot);
    slot &= 1U;
    dsd_call_snapshot call;
    if (dsd_call_state_get(state, slot, &call) <= 0 || call.phase != DSD_CALL_PHASE_ACTIVE) {
        return UI_CMD_APPLY_COMPLETED;
    }
    const uint64_t target = call.ota_target_id ? call.ota_target_id : call.policy_target_id;
    if (!target || target > UINT32_MAX || call.ota_source_id > UINT32_MAX) {
        return UI_CMD_APPLY_COMPLETED;
    }
    const unsigned int tg = (uint32_t)target;
    const int fallback = call.kind == DSD_CALL_KIND_PRIVATE_VOICE || !DSD_SYNC_IS_P25(call.protocol);
    const int arm_rc =
        dsd_tg_policy_call_skip_arm(state, tg, (uint32_t)call.ota_source_id, fallback, dsd_decode_now_mono_s());
    if (arm_rc != 0) {
        ui_set_toast(state, 4, "Could not skip TG %u", tg);
        return UI_CMD_APPLY_FAILED;
    }
    char text[128];
    DSD_SNPRINTF(text, sizeof text, "Target: %u; call skipped.", tg);
    slot_block_note_event(opts, state, slot, text);
    const int transition_status = apply_skip_decoder_transition_locked(opts, state, p25_sm_owns_trunk(opts, state));
    ui_set_toast(state, 3, "TG %u skipped", tg);
    if (transition_status == UI_CMD_APPLY_FAILED) {
        LOG_WARN("WARNING: TG %u was skipped, but the return-to-CC cleanup tune was not accepted.\n", tg);
        ui_set_toast(state, 4, "TG %u skipped; return-to-CC tune failed", tg);
    }
    return UI_CMD_APPLY_COMPLETED;
}

static void
apply_cmd_m17_tx_toggle(const dsd_opts* opts, dsd_state* state) {
    if (opts->m17encoder != 1) {
        return;
    }
    state->m17encoder_tx = (state->m17encoder_tx == 0) ? 1 : 0;
    if (state->m17encoder_tx == 0) {
        state->m17encoder_eot = 1;
    }
}

static void
apply_cmd_provoice_mode_toggle(dsd_opts* opts, dsd_state* state) {
    if (opts->frame_provoice != 1) {
        return;
    }
    state->ea_mode = (state->ea_mode == 0) ? 1 : 0;
    state->edacs_site_id = 0;
    state->edacs_lcn_count = 0;
    state->edacs_cc_lcn = 0;
    state->edacs_tuned_lcn = -1;
    state->p25_cc_freq = 0;
    state->trunk_cc_freq = 0;
    opts->trunk_is_tuned = 0;
    const double ended_m = dsd_decode_now_mono_s();
    for (int slot = 0; slot < DSD_CALL_STATE_SLOT_COUNT; slot++) {
        if (dsd_call_state_end(state, (uint8_t)slot, ended_m) > 0) {
            dsd_event_sync_slot(opts, state, (uint8_t)slot);
        }
    }
}

static int
apply_cmd_provoice_m17(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    switch (c->id) {
        case DSD_APP_CMD_M17_TX_TOGGLE:
            if (!state) {
                return 1;
            }
            apply_cmd_m17_tx_toggle(opts, state);
            return 1;
        case DSD_APP_CMD_PROVOICE_ESK_TOGGLE:
            if (!state) {
                return 1;
            }
            if (opts->frame_provoice == 1) {
                state->esk_mask = (state->esk_mask == 0) ? 0xA0 : 0;
            }
            return 1;
        case DSD_APP_CMD_PROVOICE_MODE_TOGGLE:
            if (!state) {
                return 1;
            }
            apply_cmd_provoice_mode_toggle(opts, state);
            return 1;
        default: return 0;
    }
}

/*
 * On-the-fly scan controls (#380). Whichever scanner owns the tuner gets the command:
 * --trunk-scan hands it to the coordinator through the control hook, -Y acts on the
 * scan list here, and with neither running the command is accepted and declined with
 * a status message rather than left unhandled.
 */
static int
scan_control_tuner_present(const dsd_opts* opts) {
    return opts->use_rigctl == 1 || opts->audio_in_type == AUDIO_IN_RTL;
}

static void
apply_manual_scan_hold_toggle(dsd_state* state) {
    state->lcn_scan_hold = state->lcn_scan_hold ? 0U : 1U;
    if (!state->lcn_scan_hold) {
        // The rotation left the dwell timer alone while held; give the row a full hangtime
        // now rather than hopping on the very next no-carrier pass.
        mark_cc_sync(state, 1);
        state->scan_visit_since_m = state->last_cc_sync_time_m;
        state->scan_visit_roll_seen = state->lcn_freq_roll;
        state->scan_visit_rearm_pending = 0U;
    }
    ui_set_toast(state, 3, "Scan hold %s", state->lcn_scan_hold ? "on" : "off");
}

static void
apply_manual_scan_avoid(dsd_opts* opts, dsd_state* state) {
    const int count = state->lcn_freq_count;
    const int roll = state->lcn_freq_roll;
    if (count <= 0 || roll < 1 || roll > count) {
        ui_set_toast(state, 3, "No scan channel on air to avoid");
        return;
    }
    if (dsd_state_trunk_lcn_usable_count(state) <= 1) {
        ui_set_toast(state, 3, "Cannot avoid the last usable scan channel");
        return;
    }
    const int row = roll - 1;
    const long freq = *dsd_state_trunk_lcn_slot(state, row);
    if (dsd_state_trunk_lcn_avoid_set(state, (size_t)row, 1) != 0) {
        ui_set_toast(state, 3, "Failed: scan avoid out of memory");
        return;
    }
    ui_set_toast(state, 3, "Avoiding %.4f MHz (%u avoided)", (double)freq / 1000000.0,
                 (unsigned)state->lcn_avoid_count);
    if (scan_control_tuner_present(opts)) {
        (void)apply_manual_lcn_cycle(opts, state);
    }
}

static void
apply_manual_scan_avoid_clear(dsd_state* state) {
    const int cleared = dsd_state_trunk_lcn_avoid_clear(state);
    ui_set_toast(state, 3, "Cleared %d scan avoid%s", cleared, cleared == 1 ? "" : "s");
}

static void
apply_trunk_scan_control(dsd_opts* opts, dsd_state* state, int op) {
    char target_id[sizeof(state->trunk_scan_active_id)];
    DSD_SNPRINTF(target_id, sizeof(target_id), "%s", state->trunk_scan_active_id);
    const int rc = dsd_trunk_scan_hook_control(opts, state, op);
    if (rc == DSD_TRUNK_SCAN_CONTROL_BUSY) {
        ui_set_toast(state, 3, "Trunk scan busy; try again");
        return;
    }
    if (rc == DSD_TRUNK_SCAN_CONTROL_UNAVAILABLE) {
        ui_set_toast(state, 3, "Trunk scan is not running");
        return;
    }
    switch (op) {
        case DSD_TRUNK_SCAN_CONTROL_HOLD_TOGGLE:
            if (rc >= 0) {
                ui_set_toast(state, 3, "Trunk scan hold %s", rc ? "on" : "off");
            }
            break;
        case DSD_TRUNK_SCAN_CONTROL_AVOID_ACTIVE:
            if (rc == DSD_TRUNK_SCAN_CONTROL_REFUSED) {
                ui_set_toast(state, 3, "Cannot avoid the last usable trunk scan target");
            } else {
                ui_set_toast(state, 3, "Avoiding target %s (%u avoided)", target_id,
                             (unsigned)state->trunk_scan_avoided_count);
            }
            break;
        case DSD_TRUNK_SCAN_CONTROL_AVOID_CLEAR:
            if (rc >= 0) {
                ui_set_toast(state, 3, "Cleared %d trunk scan avoid%s", rc, rc == 1 ? "" : "s");
            }
            break;
        case DSD_TRUNK_SCAN_CONTROL_ADVANCE:
            if (rc == DSD_TRUNK_SCAN_CONTROL_REFUSED) {
                ui_set_toast(state, 3, "Trunk scan has only one target");
            }
            break;
        default: break;
    }
}

static int
apply_cmd_scan_controls(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    int op;
    switch (c->id) {
        case DSD_APP_CMD_SCAN_HOLD_TOGGLE: op = DSD_TRUNK_SCAN_CONTROL_HOLD_TOGGLE; break;
        case DSD_APP_CMD_SCAN_AVOID: op = DSD_TRUNK_SCAN_CONTROL_AVOID_ACTIVE; break;
        case DSD_APP_CMD_SCAN_AVOID_CLEAR: op = DSD_TRUNK_SCAN_CONTROL_AVOID_CLEAR; break;
        default: return 0;
    }
    if (!state) {
        return 1;
    }
    if (opts->trunk_scan_enabled == 1) {
        apply_trunk_scan_control(opts, state, op);
        return 1;
    }
    if (opts->scanner_mode != 1) {
        ui_set_toast(state, 3, "Not scanning: hold/avoid need -Y or --trunk-scan");
        return 1;
    }
    switch (op) {
        case DSD_TRUNK_SCAN_CONTROL_HOLD_TOGGLE: apply_manual_scan_hold_toggle(state); break;
        case DSD_TRUNK_SCAN_CONTROL_AVOID_ACTIVE: apply_manual_scan_avoid(opts, state); break;
        default: apply_manual_scan_avoid_clear(state); break;
    }
    return 1;
}

static int
apply_cmd_channel_cycle(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    if (c->id != DSD_APP_CMD_CHANNEL_CYCLE) {
        return 0;
    }
    if (!state) {
        return 1;
    }
    // Every leg below is a retune the user asked for -- the next target, -Y row, P25 candidate or LCN -- refused during
    // an I/Q replay as a tap is (issue #575).
    if (ui_cmd_refuse_replay_tune(opts, state)) {
        return UI_CMD_APPLY_FAILED;
    }
    // Under --trunk-scan "next" means the next target. Walking the parked target's own
    // LCN list here would retune under the coordinator's feet and be snapshotted as if
    // the target had moved itself.
    if (opts->trunk_scan_enabled == 1) {
        apply_trunk_scan_control(opts, state, DSD_TRUNK_SCAN_CONTROL_ADVANCE);
        return 1;
    }
    if (scan_control_tuner_present(opts)) {
        return apply_manual_channel_cycle(opts, state);
    }
    return 1;
}

static int
ui_cmd_handle_symcap_save(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    (void)c;
    char timestr[7];
    char datestr[9];
    (void)dsd_format_local_datetime(dsd_decode_time(), DSD_LOCAL_DATETIME_TIME_COMPACT, timestr, sizeof timestr);
    (void)dsd_format_local_datetime(dsd_decode_time(), DSD_LOCAL_DATETIME_DATE_COMPACT, datestr, sizeof datestr);
    DSD_SNPRINTF(opts->symbol_out_file, sizeof opts->symbol_out_file, "%s_%s_dibit_capture.bin", datestr, timestr);
    openSymbolOutFile(opts, state);
    if (state && state->event_history_s) {
        char event_str[2000] = {0};
        DSD_SNPRINTF(event_str, sizeof event_str, "DSD-neo Dibit Capture File Started: %s;", opts->symbol_out_file);
        (void)dsd_event_emit_system_notice(opts, state, 0U, event_str);
        dsd_event_sync_slot(opts, state, 0);
    }
    opts->symbol_out_file_creation_time = dsd_decode_time();
    opts->symbol_out_file_is_auto = 1;
    return 1;
}

static int
ui_cmd_handle_symcap_stop(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    (void)c;
    if (opts->symbol_out_f) {
        /* The capture it closes is what Replay last replays (symbol_capture_last); the running input keeps its name
           (issue #634). */
        closeSymbolOutFile(opts, state);
        if (state && state->event_history_s) {
            char event_str[2000] = {0};
            DSD_SNPRINTF(event_str, sizeof event_str, "DSD-neo Dibit Capture File  Closed: %s;", opts->symbol_out_file);
            (void)dsd_event_emit_system_notice(opts, state, 0U, event_str);
            dsd_event_sync_slot(opts, state, 0);
        }
    }
    opts->symbol_out_file_is_auto = 0;
    return 1;
}

/* Replay last capture: the capture closed most recently this session (symbol_capture_last), replayed at symbol pace
   as Input > Switch source > Symbol file replays one (issue #634). */
static int
ui_cmd_handle_replay_last(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    (void)c;
    if (!state) {
        return UI_CMD_APPLY_COMPLETED;
    }
    if (opts->symbol_capture_last[0] == '\0') {
        ui_set_toast(state, 4, "Failed: no capture to replay");
        return UI_CMD_APPLY_FAILED;
    }
    if (ui_input_switch_refused(opts, state)) {
        return UI_CMD_APPLY_UNSUPPORTED;
    }
    char path[sizeof opts->symbol_capture_last];
    DSD_SNPRINTF(path, sizeof path, "%s", opts->symbol_capture_last);
    p25_sm_tick_guard_enter();
    const int rc = svc_open_symbol_in(opts, state, path);
    const int result = ui_cmd_finish_input_switch(opts, state, rc, "Replay", path);
    p25_sm_tick_guard_leave();
    return result;
}

static int
ui_cmd_handle_wav_start(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    (void)c;
    if (opts->dmr_stereo_wav == 1 && (opts->wav_out_f != NULL || opts->wav_out_fR != NULL)) {
        return UI_CMD_APPLY_COMPLETED;
    }

    int rc = svc_enable_per_call_wav(opts, state);
    return ui_cmd_apply_status_from_service_rc(rc);
}

static int
ui_cmd_handle_wav_stop(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    (void)c;
    opts->wav_out_f = close_and_rename_wav_file(opts->wav_out_f, opts, opts->wav_out_file, opts->wav_out_dir,
                                                state ? &state->event_history_s[0] : NULL);
    opts->wav_out_fR = close_and_rename_wav_file(opts->wav_out_fR, opts, opts->wav_out_fileR, opts->wav_out_dir,
                                                 state ? &state->event_history_s[1] : NULL);
    opts->wav_out_file[0] = 0;
    opts->wav_out_fileR[0] = 0;
    opts->dmr_stereo_wav = 0;
    return 1;
}

static int
ui_cmd_handle_wav_toggle(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    if (opts->dmr_stereo_wav == 1 && (opts->wav_out_f != NULL || opts->wav_out_fR != NULL)) {
        return ui_cmd_handle_wav_stop(opts, state, c);
    }
    return ui_cmd_handle_wav_start(opts, state, c);
}

/* Stop replay: a playback (a WAV or raw file, a symbol file) ends with a switch to the configured Pulse input (issue
   #634). The terminal reads its keys from stdin, so there is no switch to stdin. A Pulse input that does not open
   leaves the playback running, as any failed switch leaves the input it had. */
static int
ui_cmd_handle_stop_playback(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    (void)c;
    if (!state) {
        return UI_CMD_APPLY_COMPLETED;
    }
    if (!svc_playback_running(opts)) {
        ui_set_toast(state, 3, "No playback to stop");
        return UI_CMD_APPLY_COMPLETED;
    }
    if (ui_input_switch_refused(opts, state)) {
        return UI_CMD_APPLY_UNSUPPORTED;
    }
    p25_sm_tick_guard_enter();
    const int rc = svc_stop_playback(opts, state);
    const int result = ui_cmd_finish_input_switch(opts, state, rc, "Stop replay", "Pulse input");
    p25_sm_tick_guard_leave();
    return result;
}

static int
apply_cmd_capture_playback(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    static const struct dsd_app_command_handler_entry k_handlers[] = {
        {DSD_APP_CMD_SYMCAP_SAVE, ui_cmd_handle_symcap_save},     {DSD_APP_CMD_SYMCAP_STOP, ui_cmd_handle_symcap_stop},
        {DSD_APP_CMD_REPLAY_LAST, ui_cmd_handle_replay_last},     {DSD_APP_CMD_WAV_START, ui_cmd_handle_wav_start},
        {DSD_APP_CMD_WAV_STOP, ui_cmd_handle_wav_stop},           {DSD_APP_CMD_WAV_TOGGLE, ui_cmd_handle_wav_toggle},
        {DSD_APP_CMD_STOP_PLAYBACK, ui_cmd_handle_stop_playback},
    };
    return ui_cmd_apply_handler_table(k_handlers, sizeof k_handlers / sizeof k_handlers[0], opts, state, c);
}

static int
ui_cmd_handle_lcw_retune_toggle(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    (void)state;
    (void)c;
    svc_toggle_lcw_retune(opts);
    return 1;
}

static int
ui_cmd_handle_p25_cc_cand_toggle(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    (void)state;
    (void)c;
    opts->p25_prefer_candidates = opts->p25_prefer_candidates ? 0 : 1;
    return 1;
}

static int
ui_cmd_handle_reverse_mute_toggle(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    (void)state;
    (void)c;
    svc_toggle_reverse_mute(opts);
    return 1;
}

static int
ui_cmd_handle_config_metadata_set(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    (void)opts;
    if (!state || !c || c->n < sizeof(dsd_app_config_metadata_payload)) {
        return UI_CMD_APPLY_INVALID_PAYLOAD;
    }
    dsd_app_config_metadata_payload payload;
    DSD_MEMCPY(&payload, c->data, sizeof payload);
    state->config_autosave_enabled = payload.autosave_enabled ? 1 : 0;
    DSD_SNPRINTF(state->config_autosave_path, sizeof state->config_autosave_path, "%s", payload.path);
    state->config_autosave_path[sizeof state->config_autosave_path - 1] = '\0';
    return UI_CMD_APPLY_COMPLETED;
}

static int
cfg_airspy_settings_valid(const dsdneoUserConfig* cfg) {
    if (!cfg->has_input || cfg->input_source != DSDCFG_INPUT_AIRSPY) {
        return 1;
    }
    return !cfg->airspy_invalid && dsd_airspy_config_valid(&cfg->airspy);
}

/* A width of analog @p kind the front end refused after the change was committed (@p width_hz) is put back to match
   @p kept_hz, the width the front end kept, with the configured width from before the change @p configured_before_hz
   (-1: none) (svc_restore_analog_width()), and the toast says why. */
static void
ui_restore_refused_analog_width(dsd_opts* opts, dsd_state* state, int kind, int width_hz, int kept_hz,
                                int configured_before_hz) {
    svc_restore_analog_width(opts, state, kind, kept_hz, configured_before_hz);
    char why[128];
    svc_describe_analog_refusal(opts, kind, width_hz, why, sizeof why);
    ui_set_toast(state, 5, "Refused: %s", why);
}

/* Whether a scan still decides the front end after a scan leave: a scan row's scope in force or being updated, or the
   conventional scanner still on with rows to visit (a channel-map adopt or a RadioReference import that keeps it on),
   whose next row's tune decides what the front end runs. A refused leave is left to that tune (issue #578). */
static int
ui_scan_goes_on(const dsd_opts* opts, const dsd_state* state) {
    return dsd_scan_mode_configured_view(state) || dsd_scan_mode_updating(state)
           || (opts->scanner_mode == 1 && state->lcn_freq_count > 0);
}

/* Whether a scan leave's refused return to the monitor (@p refusal) is left to what came after it: a receive request
   queued after it, which superseded it and decides the front end (svc_monitor_refusal::superseded), or a scan that
   goes on, whose next row's tune decides it (ui_scan_goes_on()). Nothing is put back for such a refusal: neither the
   configured width, nor the mode a switch armed before the leave replaced, which stays armed. The drain asks this
   once, before anything else it does with a refusal (ui_settle_receive_requests()). */
static int
ui_scan_leave_left_to_later(const dsd_opts* opts, const dsd_state* state, const svc_monitor_refusal* refusal) {
    return refusal->scan_leave && (refusal->superseded || ui_scan_goes_on(opts, state));
}

/* Whether the options are still the ones a refused scan leave put back (@p refusal): the analog family, and the kind
   and configured width it asked for. */
static int
ui_scan_leave_options_stand(const dsd_opts* opts, const svc_monitor_refusal* refusal) {
    return dsd_opts_is_analog_family(opts) && opts->analog_demod == refusal->kind
           && dsd_app_analog_width_setting_hz(opts, refusal->kind) == refusal->width_hz;
}

/* A refused scan leave's front end off the monitor of the kind it asked for (@p refusal): the kind's default is asked
   for and stored as the configured width, with the request carrying the leave on, and 0 returned. When the default was
   what the front end refused (@p refusal's own width: the fallback of an earlier refusal, landing on a rate a retune
   moved again, or a change to the default made after the leave), or the rate refuses it now, nothing is asked, the
   configured width from before the default goes back where there is one, and -1 is returned. */
static int
ui_scan_leave_fall_back_to_default(dsd_opts* opts, dsd_state* state, const svc_monitor_refusal* refusal) {
    const int kind = refusal->kind;
    if (refusal->width_hz == 0) {
        if (refusal->configured_before_hz >= 0) {
            (void)svc_store_analog_width_setting(opts, state, kind, refusal->configured_before_hz);
        }
        return -1;
    }
    if (svc_check_analog_bandwidth(opts, state, kind, 0, NULL, 0U) != 0) {
        return -1;
    }
    /* Straight to the monitor request, as ui_revert_analog_entry() republishes: svc_publish_analog_bandwidth() would
       wait for a CQPSK a kept P25 row requested, which is what the default has to replace. */
    (void)svc_store_analog_width_setting(opts, state, kind, 0);
    if (svc_publish_symbol_profile_after_scan_leave(opts, state, dsd_scan_mode_effective_profile(opts, state),
                                                    refusal->width_hz)
        != 0) {
        (void)svc_store_analog_width_setting(opts, state, kind, refusal->width_hz);
        return -1;
    }
    return 0;
}

/*
 * A scan leave's return to the analog monitor the front end refused (issue #578), at once or where it landed
 * (@p refusal): a row's retune had moved the demod rate off the one the configured width was held to. The front end
 * kept its receive profile, and the decoder follows what it kept, "kept width, else default". Still on the monitor
 * output of the kind the leave asked for, the configured width takes the width that monitor runs, as after any refused
 * width change (svc_restore_analog_width()). Off it (a typed digital row's channel profile, which never touches the
 * width setting, another kind's monitor, or the digital family), the kind's default is requested and stored as the
 * configured width: the NFM default is never refused, and a default the rate refuses as well (AM's) changes nothing.
 * That request carries the leave on (svc_publish_symbol_profile_after_scan_leave()), so a default refused where it
 * lands (a retune in flight moved the rate again) comes back here as a refused default: nothing more is asked of the
 * front end, and the configured width goes back to the one from before the fallback. The toast says why either way,
 * naming the width the leave asked for. A switch between FM and AM armed under the row, which the leave's refusal put
 * back, is part of the leave: the request for the kind put back carries the leave on too, and one the front end refuses
 * at once or where it lands is settled here for that kind (ui_revert_analog_entry()), since the rate the row left can
 * filter its width no better. Only while the options are still the ones the leave put back (the analog family, that
 * kind, that width). A refusal left to what came after the leave never gets here
 * (ui_scan_leave_left_to_later(), which the drain asks first): a channel-map adopt or a RadioReference import that keeps
 * the scanner on gets its front end from the next row's tune, and is deliberately left to it, and a receive request
 * queued after the leave's, from anywhere (a CQPSK toggle made before the demod thread took the return), decides what
 * the front end runs, which asking for the default would undo (CQPSK off again); either way there is no request, the
 * configured width stays as it is and there is no toast, the stream having logged the refusal. Returns 1 when it acted.
 */
static int
ui_settle_refused_scan_leave(dsd_opts* opts, dsd_state* state, const svc_monitor_refusal* refusal) {
    const int kind = refusal->kind;
    if (!ui_scan_leave_options_stand(opts, refusal)) {
        return 0;
    }
    /* A refused default (the NFM default never is) was the fallback of a width refused before it, or a change to the
       default made after the leave: the width from before it is the one to name. */
    const int asked_hz = (refusal->width_hz == 0 && refusal->configured_before_hz > 0) ? refusal->configured_before_hz
                                                                                       : refusal->width_hz;
    int runs_hz = 0;
    if (refusal->kept_analog && refusal->kept_monitor && refusal->kept_kind == kind) {
        if (refusal->kept_width_hz == refusal->width_hz) {
            return 0; /* the monitor runs the configured width: nothing to reconcile */
        }
        runs_hz = refusal->kept_width_hz;
        svc_restore_analog_width(opts, state, kind, runs_hz, refusal->configured_before_hz);
    } else {
        runs_hz = ui_scan_leave_fall_back_to_default(opts, state, refusal);
    }
    char why[128];
    svc_describe_monitor_return_refusal(opts, kind, asked_hz, runs_hz, why, sizeof why);
    ui_set_toast(state, 5, "Refused: %s", why);
    return 1;
}

/* Hand a changed width of the analog kind in force to the running front end. One it refuses now (a retune moved the
   rate since the width was checked) is put back to @p previous_hz. Returns -1 then. */
static int
ui_publish_analog_bandwidth(dsd_opts* opts, dsd_state* state, int previous_hz) {
    const int kind = opts->analog_demod;
    if (svc_publish_analog_bandwidth(opts, state, kind, previous_hz) == 0) {
        return 0;
    }
    ui_restore_refused_analog_width(opts, state, kind, dsd_app_analog_width_setting_hz(opts, kind), previous_hz,
                                    previous_hz);
    return -1;
}

/* The analog kind the session runs once the config's [mode] is applied (dsd_analog_demod), or -1 when it does not run
   the analog family: decode = "analog" is NFM and decode = "am" AM. A [mode] without a decode key (a demod or LRRP
   setting alone) applies no preset (dsd_apply_decode_mode_preset() refuses DSDCFG_MODE_UNSET), so the session keeps its
   family and kind, as it does without a [mode]. The M17 encoder never runs it. */
static int
cfg_analog_kind_after(const dsd_opts* opts, const dsdneoUserConfig* cfg) {
    if (opts->m17encoder == 1) {
        return -1;
    }
    if (cfg->has_mode && cfg->decode_mode != DSDCFG_MODE_UNSET) {
        if (cfg->decode_mode == DSDCFG_MODE_AM) {
            return DSD_ANALOG_DEMOD_AM;
        }
        return cfg->decode_mode == DSDCFG_MODE_ANALOG ? DSD_ANALOG_DEMOD_FM : -1;
    }
    return opts->analog_only == 1 ? opts->analog_demod : -1;
}

/* The width of analog @p kind the session has once the config is applied: [analog] sets it (its loader keeps only
   in-range widths, and the apply treats anything else as the default), otherwise it stays. */
static int
cfg_analog_width_after(const dsd_opts* opts, const dsdneoUserConfig* cfg, int kind) {
    if (!cfg->has_analog) {
        return dsd_app_analog_width_setting_hz(opts, kind);
    }
    const int width_hz = (kind == DSD_ANALOG_DEMOD_AM) ? cfg->analog_am_bandwidth_hz : cfg->analog_nfm_bandwidth_hz;
    return dsd_analog_width_in_range(kind, width_hz) ? width_hz : 0;
}

/* The DSP bandwidth, in kHz, an RTL-SDR or rtl_tcp reopen runs at: rtl_bw_khz as the config gives it, the session's
   when it gives none (apply_cfg_rtl_common()). */
static int
cfg_reopen_rtl_bw_khz(const dsd_opts* opts, const dsdneoUserConfig* cfg) {
    return cfg->rtl_bw_khz ? cfg->rtl_bw_khz : opts->rtl_dsp_bw_khz;
}

/* How a config's [input] reopens the running input's device (apply_cfg_radio_input()). */
typedef enum {
    CFG_REOPEN_NONE = 0,        /* nothing reopens: the running device and its rate stay */
    CFG_REOPEN_AT_RTL_BW,       /* an RTL-SDR or rtl_tcp device, at its DSP bandwidth */
    CFG_REOPEN_AT_DEVICE_RATE,  /* a SoapySDR or Airspy device, at the rate the device delivers */
    CFG_REOPEN_AT_RUNNING_RATE, /* the running device at the rate it runs now: an Airspy for its monitor volume
                                   alone, or any device for a new gain under the same spec */
} cfg_reopen_kind;

/*
 * A running RTL-family input (an RTL-SDR, rtl_tcp, SoapySDR or Airspy one) is reopened when the config's [input]
 * builds a radio input spec other than the one it runs (dsd_user_config_radio_input_spec()): an RTL-SDR or rtl_tcp
 * one at its DSP bandwidth, a SoapySDR or Airspy one at whatever rate that device delivers. An Airspy source over a
 * running Airspy applies live instead (cfg_is_live_airspy()), reopening only for settings the stream takes when it
 * opens, and the same spec (an rtl_tcp source without rtl_freq for the host and port already in use, say) reopens
 * nothing. A reopen whose start fails anyway puts the running input back (ui_cfg_settle_reopen()). A config apply never
 * switches the input type, so the spec given to any other input waits for the next start, which checks it
 * (dsd_engine_setup_check_analog_width() for an RTL-SDR or rtl_tcp spec, the stream start's own check for every radio
 * input).
 */
/* Whether a config whose [input] spec is the running input's restarts it for its gain (apply_cfg_rtl_live_gain()): it
   names a gain, the input is not an Airspy (whose gains are its own settings), and the gain it puts in force differs
   from the running one. Under --trunk-scan a parked target's own gain stays in force, so nothing restarts then. */
static int
cfg_gain_restarts_running_input(const dsd_opts* opts, const dsd_state* state, const dsdneoUserConfig* cfg) {
    if (!cfg->has_input || !dsd_user_config_rtl_gain_is_set(cfg)
        || (cfg->input_source != DSDCFG_INPUT_RTL && cfg->input_source != DSDCFG_INPUT_RTLTCP
            && cfg->input_source != DSDCFG_INPUT_SOAPY)) {
        return 0;
    }
    if (state && opts->trunk_scan_enabled == 1 && state->trunk_scan_target_count > 0U
        && state->trunk_scan_gain_override) {
        return 0;
    }
    return cfg->rtl_gain != opts->rtl_gain_value;
}

static cfg_reopen_kind
cfg_radio_reopen(const dsd_opts* opts, const dsd_state* state, const dsdneoUserConfig* cfg) {
    char spec[sizeof opts->audio_in_dev];
    if (opts->audio_in_type != AUDIO_IN_RTL) {
        return CFG_REOPEN_NONE;
    }
    if (cfg_is_live_airspy(cfg, opts->audio_in_dev, opts->audio_in_type)) {
        /* The Airspy path applies what it can live, but reopens the device for a new sample rate or serial, or a DSP
           bandwidth or monitor volume other than the stream opened with (svc_airspy_settings_reopen()), with the
           shared tuning the config applies (apply_shared_radio_tuning_from_config()). A reopen runs at the rate the
           device delivers for the new settings: a wider DSP bandwidth decimates its capture less. The monitor volume
           is only copied when the stream opens, so a reopen for it alone runs at the rate the Airspy runs now, which
           the running front end holds the configured width the stream opens on to, changed or not (issue #578). */
        const int bw_khz = cfg_reopen_rtl_bw_khz(opts, cfg);
        const int next_bw_khz = dsd_analog_rtl_dsp_bw_is_selectable(bw_khz) ? bw_khz : DSD_ANALOG_RTL_DSP_BW_MAX_KHZ;
        const int next_volume = cfg->rtl_volume ? cfg->rtl_volume : opts->rtl_volume_multiplier;
        if (!svc_airspy_settings_reopen(&opts->airspy, &cfg->airspy, opts->rtl_dsp_bw_khz, next_bw_khz,
                                        opts->rtl_volume_multiplier, next_volume)) {
            return CFG_REOPEN_NONE;
        }
        return svc_airspy_settings_reopen(&opts->airspy, &cfg->airspy, opts->rtl_dsp_bw_khz, next_bw_khz, next_volume,
                                          next_volume)
                   ? CFG_REOPEN_AT_DEVICE_RATE
                   : CFG_REOPEN_AT_RUNNING_RATE;
    }
    /* A config that builds no spec (an [input] without rtl_freq, say) leaves the running one, as one that builds the
       same spec does: either way the input restarts for a new gain alone, at the rate it runs
       (apply_cfg_rtl_live_gain()). */
    if (dsd_user_config_radio_input_spec(cfg, opts, spec, sizeof spec) != 0
        || strncmp(spec, opts->audio_in_dev, sizeof opts->audio_in_dev) == 0) {
        return cfg_gain_restarts_running_input(opts, state, cfg) ? CFG_REOPEN_AT_RUNNING_RATE : CFG_REOPEN_NONE;
    }
    return (cfg->input_source == DSDCFG_INPUT_RTL || cfg->input_source == DSDCFG_INPUT_RTLTCP)
               ? CFG_REOPEN_AT_RTL_BW
               : CFG_REOPEN_AT_DEVICE_RATE;
}

/*
 * A width of analog @p kind the config leaves the analog monitor on (@p width_hz, 0 for the AM default), held to the
 * DSP rate it will run at. When the config reopens an RTL-SDR or rtl_tcp device, that is the reopened device's DSP
 * bandwidth (@p reopen_bw_khz), whatever runs now (another bandwidth, or a SoapySDR or Airspy device whose rate the
 * device forced); otherwise the running front end holds it, as DSD_APP_CMD_NFM_BANDWIDTH_SET and
 * DSD_APP_CMD_AM_BANDWIDTH_SET do. A refusal leaves the whole config unapplied, with a toast naming the width, the rate
 * and the fix.
 */
static int
cfg_check_analog_width(const dsd_opts* opts, dsd_state* state, int kind, int reopen_bw_khz, int width_hz) {
    char why[128];
    const int rc = (reopen_bw_khz > 0)
                       ? svc_check_analog_bandwidth_for_rtl_bw(kind, width_hz, reopen_bw_khz, why, sizeof why)
                       : svc_check_analog_bandwidth(opts, state, kind, width_hz, why, sizeof why);
    if (rc == 0) {
        return UI_CMD_APPLY_COMPLETED;
    }
    ui_set_toast(state, 5, "Config not applied: %s", why);
    return UI_CMD_APPLY_FAILED;
}

/*
 * A config apply is scoped: while it runs, dsd_opts holds the configured options, but the stream a reopen starts runs
 * the scan row on air again once the scope resumes. An nfm or am row's own width (--nfm-bandwidth-hz or
 * --am-bandwidth-hz, issue #526) is therefore held to the rate a reopened device runs at, as RTL_SET_BW and Input >
 * Switch source hold it: a reopen that cannot filter it leaves the whole config unapplied, rather than the row refused
 * where it lands. A config that reopens nothing, or reopens the Airspy at the rate it runs, moves no rate.
 */
static int
cfg_check_scan_row_width(const dsd_opts* opts, dsd_state* state, const dsdneoUserConfig* cfg) {
    const dsd_scan_option_values* row = dsd_scan_mode_row_options(state);
    if (!row || !(row->present & DSD_SCAN_OPT_BANDWIDTH) || row->channel_bw_hz <= 0) {
        return UI_CMD_APPLY_COMPLETED;
    }
    const int kind = row->channel_bw_kind == DSD_ANALOG_DEMOD_AM ? DSD_ANALOG_DEMOD_AM : DSD_ANALOG_DEMOD_FM;
    const cfg_reopen_kind reopen = cfg_radio_reopen(opts, state, cfg);
    char why[128];
    int rc = 0;
    if (reopen == CFG_REOPEN_AT_RTL_BW) {
        rc = svc_check_analog_bandwidth_for_rtl_bw(kind, row->channel_bw_hz, cfg_reopen_rtl_bw_khz(opts, cfg), why,
                                                   sizeof why);
    } else if (reopen == CFG_REOPEN_AT_DEVICE_RATE) {
        rc = svc_check_analog_bandwidth_at_device_rate(kind, row->channel_bw_hz, why, sizeof why);
    }
    if (rc == 0) {
        return UI_CMD_APPLY_COMPLETED;
    }
    ui_set_toast(state, 5, "Config not applied: %s", why);
    return UI_CMD_APPLY_FAILED;
}

/*
 * A width of analog @p kind that a DSP rate holds (an explicit width, or the AM default) and that the session runs once
 * the config is applied, held to the rate it will run at whenever the config changes the width, reopens a device under
 * it (cfg_radio_reopen()) or, with @p opens_anew, has the front end open on it anew, changed or not: at the DSP
 * bandwidth an RTL-SDR or rtl_tcp reopen runs at, only to the rules every rate shares for a SoapySDR or Airspy reopen
 * (its start checks the rate it delivers, and a start that refuses it puts the running input back), otherwise at the
 * running front end's rate, which an Airspy reopened for its monitor volume alone keeps (issue #578).
 */
static int
cfg_check_held_analog_width(const dsd_opts* opts, dsd_state* state, const dsdneoUserConfig* cfg, int kind, int width_hz,
                            int opens_anew) {
    const cfg_reopen_kind reopen = cfg_radio_reopen(opts, state, cfg);
    if (reopen == CFG_REOPEN_AT_DEVICE_RATE) {
        /* No rate to hold it to before the device opens, but the rules every rate shares still apply. */
        char why[128];
        if (svc_check_analog_bandwidth_at_device_rate(kind, width_hz, why, sizeof why) == 0) {
            return UI_CMD_APPLY_COMPLETED;
        }
        ui_set_toast(state, 5, "Config not applied: %s", why);
        return UI_CMD_APPLY_FAILED;
    }
    const int reopen_bw_khz = (reopen == CFG_REOPEN_AT_RTL_BW) ? cfg_reopen_rtl_bw_khz(opts, cfg) : 0;
    const int holds = width_hz != dsd_app_analog_width_setting_hz(opts, kind) || reopen_bw_khz > 0 || opens_anew;
    return holds ? cfg_check_analog_width(opts, state, kind, reopen_bw_khz, width_hz) : UI_CMD_APPLY_COMPLETED;
}

/*
 * A session the config leaves on a preset that runs no FM -- a digital one, or AM (issue #524) -- still runs the
 * configured NFM width on the scan's nfm rows or targets without a width of their own (issue #526,
 * dsd_engine_scan_runs_configured_nfm_width()), and one it leaves on a preset that runs no AM the configured AM width
 * on the scan's am rows or targets without one (dsd_engine_scan_runs_configured_am_width(); the AM default is held as
 * its 6 kHz): each held to the rate as cfg_check_held_analog_width() holds a width. @p preset_kind is the analog kind
 * the config leaves the session on (cfg_analog_kind_after(), -1 for none), which holds its own width.
 */
static int
cfg_check_scan_widths(const dsd_opts* opts, dsd_state* state, const dsdneoUserConfig* cfg, int preset_kind) {
    for (int kind = DSD_ANALOG_DEMOD_FM; kind <= DSD_ANALOG_DEMOD_AM; kind++) {
        const int width_hz = cfg_analog_width_after(opts, cfg, kind);
        const int runs = kind == DSD_ANALOG_DEMOD_AM ? dsd_engine_scan_runs_configured_am_width(opts, state)
                                                     : dsd_engine_scan_runs_configured_nfm_width(opts, state);
        if (kind == preset_kind || !runs || (width_hz <= 0 && kind != DSD_ANALOG_DEMOD_AM)) {
            continue;
        }
        const int rc = cfg_check_held_analog_width(opts, state, cfg, kind, width_hz, 0);
        if (rc != UI_CMD_APPLY_COMPLETED) {
            return rc;
        }
    }
    return UI_CMD_APPLY_COMPLETED;
}

/*
 * Asked before anything changes, as DSD_APP_CMD_DECODE_MODE_SET asks. A config that leaves the analog monitor on an
 * explicit width, or on AM (whose default the rate holds too, since AM always runs its channel filter), is held to the
 * rate that width will run at (cfg_check_held_analog_width()), under a scan row too, as the width commands and
 * RTL_SET_BW hold it. So is one that leaves the session on another kind or none while the scan has an nfm or am row or
 * target that runs the configured width of its kind (issue #526, cfg_check_scan_widths()), and an nfm or am row's own
 * width on air (cfg_check_scan_row_width()). A config that
 * reopens a SoapySDR or Airspy device instead runs the width at the rate that device delivers, which neither the
 * running stream's rate nor rtl_bw_khz says: the reopened stream's start checks it there
 * (rtl_demod_finalize_analog_channel()), as it does for Input > Switch source > RTL-SDR over a SoapySDR input, and one
 * that refuses it puts the running input back and fails the apply (ui_cfg_settle_reopen(), issue #578). With the
 * unset NFM default, which no rate refuses, a [mode] that moves a running RTL session onto the monitor publishes the
 * analog receive profile (apply_cfg_receive_family_change()), and a front end that would refuse it (logged with the
 * reason) leaves the whole config unapplied, instead of an Analog decoder on a digital front end. [mode] decode = am on
 * a PCM input is not refused: no channel filter runs there, the config applies, and the session then runs the Analog
 * monitor, says why and turns autosave off (apply_cmd_fall_back_from_am_on_pcm()), as a start with that config does,
 * since a shared config must not break a PCM session.
 */
static int
cfg_check_receive_family(const dsd_opts* opts, dsd_state* state, const dsdneoUserConfig* cfg) {
    const int row_rc = cfg_check_scan_row_width(opts, state, cfg);
    if (row_rc != UI_CMD_APPLY_COMPLETED) {
        return row_rc;
    }
    const int kind = cfg_analog_kind_after(opts, cfg);
    const int scan_rc = cfg_check_scan_widths(opts, state, cfg, kind);
    if (kind < 0 || scan_rc != UI_CMD_APPLY_COMPLETED) {
        return scan_rc;
    }
    const int width_hz = cfg_analog_width_after(opts, cfg, kind);
    if (width_hz > 0 || kind == DSD_ANALOG_DEMOD_AM) {
        /* The front end opens on this width anew when the config moves the session onto the monitor or onto the kind,
           and when it reopens the running Airspy for its volume alone: that stream opens on the configured settings
           (the apply runs inside a scan row's suspended scope), at the rate it runs now, even where the config leaves
           the width as it was and a typed digital row on air runs none (issue #578). */
        const int opens_anew = !dsd_opts_is_analog_family(opts) || opts->analog_demod != kind
                               || cfg_radio_reopen(opts, state, cfg) == CFG_REOPEN_AT_RUNNING_RATE;
        return cfg_check_held_analog_width(opts, state, cfg, kind, width_hz, opens_anew);
    }
    if (!cfg->has_mode || opts->analog_only || opts->analog_nfm_bandwidth_hz != 0
        || svc_check_mode_receive_profile(opts, state, cfg->decode_mode) == 0) {
        return UI_CMD_APPLY_COMPLETED;
    }
    ui_set_toast(state, 4, "Config not applied: RTL front end refused the %s channel (see log)",
                 dsd_decode_mode_display_name(cfg->decode_mode));
    return UI_CMD_APPLY_FAILED;
}

static int
cfg_prepare_runtime_apply(dsd_opts* opts, dsd_state* state, const dsdneoUserConfig* cfg) {
    if (!cfg_airspy_settings_valid(cfg)) {
        return UI_CMD_APPLY_INVALID_PAYLOAD;
    }
    const int family_rc = cfg_check_receive_family(opts, state, cfg);
    if (family_rc != UI_CMD_APPLY_COMPLETED) {
        return family_rc;
    }
    /* Refused before any mutation: the conventional scanner and a trunk-scan
     * coordinator are exclusive tuner owners (same rule as the scanner toggle). */
    if (cfg->has_trunking && cfg->trunk_scanner && !cfg->trunk_enabled && opts->trunk_scan_enabled == 1) {
        ui_set_toast(state, 3, "Trunk scan active: conventional scanner unavailable");
        return UI_CMD_APPLY_FAILED;
    }
    if (!cfg->has_trunking || !cfg->trunk_group_csv[0]) {
        return UI_CMD_APPLY_COMPLETED;
    }
    if (!memchr(cfg->trunk_group_csv, 0, sizeof cfg->trunk_group_csv)) {
        return UI_CMD_APPLY_INVALID_PAYLOAD;
    }
    if (strcmp(opts->group_in_file, cfg->trunk_group_csv) == 0) {
        return UI_CMD_APPLY_COMPLETED;
    }
    // Config loads already suspend scan-row groups, so this replaces only the global list.
    if (svc_import_group_list(opts, state, cfg->trunk_group_csv) != 0) {
        ui_set_toast(state, 4, "Config not applied: group list could not be loaded");
        return UI_CMD_APPLY_FAILED;
    }
    return UI_CMD_APPLY_COMPLETED;
}

/*
 * Settings whose lifecycle startup owns. The frontend kind belongs to startup and
 * ui_start/ui_stop: runtime config/profile loads may carry persisted frontend
 * defaults, but flipping it live can strand an active frontend session before the
 * UI thread has a chance to shut it down. The trunk-scan coordinator is likewise
 * only installed at startup, and its flag must keep saying whether one is
 * installed: the scanner-exclusivity refusals and P25 recovery admission read it
 * (#554). Both are put back after the config is applied; nonzero means the config
 * asked to change one of them, which the caller reports as restart-required.
 */
static int
cfg_restore_lifecycle_owned(dsd_opts* opts, const dsdneoUserConfig* cfg, dsd_frontend_kind old_frontend_kind,
                            int old_trunk_scan_enabled) {
    opts->frontend_kind = old_frontend_kind;
    opts->trunk_scan_enabled = old_trunk_scan_enabled;
    const int frontend_changed = cfg->frontend_kind_is_set && cfg->frontend_kind != old_frontend_kind;
    const int trunk_scan_changed = cfg->has_trunk_scan && (cfg->trunk_scan_enabled ? 1 : 0) != old_trunk_scan_enabled;
    return frontend_changed || trunk_scan_changed;
}

/*
 * A [mode] that moves a running session between the analog monitor and a digital decoder needs what
 * DSD_APP_CMD_DECODE_MODE_SET does for the same move: the sink the new family writes to, the part-collected analog
 * monitor block of the old family's samples dropped, and on an RTL front end the receive-family switch with symbol
 * timing at the live demod rate (decode_mode_republish()). Without it the front end stays on the old family's
 * demodulator. A [mode] that stays inside its family keeps the config-apply behaviour it
 * had, except that a digital mode writing raw audio (ProVoice, or the -8 source monitor) gets the raw sink a session
 * started in another digital mode never opened, as DECODE_MODE_SET gives it (dsd_audio_ensure_digital_output() is
 * idempotent), and that the digital modes it configures are noted with an RTL front end, as DECODE_MODE_SET notes them
 * (svc_note_digital_decode_modes()): after a live switch onto the digital family they pick the FSK channel profile the
 * front end's CQPSK toggle returns to.
 */
/* The receive profile the configured options ask a front end for: whether it is the analog monitor, its kind, and the
   configured width of that kind (issue #524). */
typedef struct {
    int analog_only;
    int analog_kind;
    int width_hz;
} cfg_rx_profile;

static cfg_rx_profile
cfg_rx_profile_of(const dsd_opts* opts) {
    const cfg_rx_profile p = {opts->analog_only ? 1 : 0, opts->analog_demod, dsd_opts_analog_width_hz(opts)};
    return p;
}

/* Still on the analog monitor: a switch between FM and AM moves the front end onto the other kind between blocks, and
   one it refuses at once goes back to the mode the session had, as DECODE_MODE_SET's does, failing the apply (one
   refused where it lands is put back by the next drain); a new width of the kind in force is a width-only change, put
   back when the front end refuses it after all (a retune moved the rate since cfg_check_receive_family()). */
static int
apply_cfg_analog_profile_change(dsd_opts* opts, dsd_state* state, const cfg_rx_profile* before) {
    if (opts->m17encoder == 1) {
        return 0;
    }
    if (opts->analog_demod != before->analog_kind) {
        if (opts->audio_in_type != AUDIO_IN_RTL) {
            return 0;
        }
        /* The part-collected monitor block holds the old kind's audio, as decode_mode_apply_value() drops it. */
        dsd_symbol_analog_block_reset(state);
        ui_arm_analog_entry(opts, state, 1, before->analog_kind);
        return (decode_mode_republish(opts, state, dsd_infer_decode_mode_preset(opts)) != 0
                && ui_revert_analog_entry(opts, state, dsd_opts_analog_width_hz(opts), NULL))
                   ? -1
                   : 0;
    }
    return dsd_opts_analog_width_hz(opts) != before->width_hz
               ? ui_publish_analog_bandwidth(opts, state, before->width_hz)
               : 0;
}

static int
apply_cfg_receive_family_change(dsd_opts* opts, dsd_state* state, const dsdneoUserConfig* cfg,
                                const cfg_rx_profile* before) {
    const int analog_only = opts->analog_only ? 1 : 0;
    const int old_analog_only = before->analog_only;
    int rc = 0;
    if (analog_only && old_analog_only) {
        rc = apply_cfg_analog_profile_change(opts, state, before);
    }
    if (!cfg->has_mode) {
        return rc;
    }
    if (analog_only == old_analog_only) {
        if (!analog_only && (opts->frame_provoice == 1 || opts->monitor_input_audio == 1)) {
            (void)dsd_audio_ensure_digital_output(opts);
        }
        svc_note_digital_decode_modes(opts, state);
        return rc;
    }
    if (analog_only) {
        (void)dsd_audio_ensure_analog_output(opts);
    } else {
        (void)dsd_audio_ensure_digital_output(opts);
    }
    dsd_symbol_analog_block_reset(state);
    if (opts->audio_in_type == AUDIO_IN_RTL) {
        /* A switch onto the monitor the front end refuses at once goes back to the mode the session had, as
           DECODE_MODE_SET's does, and fails the apply; one refused where it lands is put back by the next drain. */
        ui_arm_analog_entry(opts, state, old_analog_only && opts->m17encoder != 1, before->analog_kind);
        if (decode_mode_republish(opts, state, cfg->decode_mode) != 0
            && ui_revert_analog_entry(opts, state, dsd_opts_analog_width_hz(opts), NULL)) {
            rc = -1;
        }
    }
    return rc;
}

/* Whether a config apply's new input, opened with no stream running before it, runs but refuses the scan row on air,
   which the apply reported ("Config applied; the scan row cannot run: ...": ui_cfg_settle_reopen() returned 1, issue
   #578). Set by the apply (ui_cfg_note_refused_row()) and taken, once the command has run, for the resume of the scope
   it ran under (ui_scope_stream_of()). */
static int g_cfg_row_refusal_reported;

#ifdef USE_RADIO
/* A config apply whose new input, with no stream running before it, runs but refuses the scan row on air
   (ui_cfg_settle_reopen() returned @p radio_rc 1): the input and the whole config stay, its [mode] and [analog] widths
   included, and the toast says the row cannot run. That stream opened on the config's receive side, so a switch onto
   the analog monitor, or between FM and AM, the config made (apply_cfg_receive_family_change()) has nothing left for
   the front end to refuse: it is dropped. The row's own refusal when the scope resumes is the one the toast reports, so
   it is noted for the resume (g_cfg_row_refusal_reported), which then puts back neither the mode the session had
   (ui_revert_analog_entry()) nor the width from before a config that set the one the row runs (issue #578). */
static void
ui_cfg_note_refused_row(int radio_rc) {
    if (radio_rc > 0) {
        g_analog_entry.armed = 0;
        g_cfg_row_refusal_reported = 1;
    }
}
#endif

/* A config apply that moved the input is an input switch like the commands that make one
   (ui_input_left(): the tone heard before it goes, issue #522, and so does an I/Q replay's
   capture clock, issue #572), and one that changed the decode mode a decode-mode change, which
   forgets the tone too. Out of the analog monitor nothing else forgets it before the row can
   come back, since no monitor block need arrive in between. An unrelated settings change keeps
   it, including the mode every runtime apply restates. */
static void
cfg_forget_rx_tone_on_boundary(dsd_opts* opts, dsd_state* state, int old_audio_in_type, const char* old_audio_in_dev,
                               dsdneoUserDecodeMode old_decode_mode) {
    if (opts->audio_in_type != old_audio_in_type
        || strncmp(old_audio_in_dev, opts->audio_in_dev, sizeof opts->audio_in_dev) != 0) {
        ui_input_left(opts, state);
    } else if (dsd_infer_decode_mode_preset_exact(opts) != old_decode_mode) {
        dsd_analog_rx_reset(state);
    }
}

static int
ui_cmd_handle_config_apply(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    if (!state || c->n < sizeof(dsdneoUserConfig)) {
        return UI_CMD_APPLY_INVALID_PAYLOAD;
    }

    dsdneoUserConfig cfg;
    char old_audio_in_dev[sizeof opts->audio_in_dev];
    char old_audio_out_dev[sizeof opts->audio_out_dev];
    int old_audio_in_type = opts->audio_in_type;
    int old_audio_out_type = opts->audio_out_type;
    int old_wav_sample_rate = opts->wav_sample_rate;
    int old_effective_input_rate = dsd_opts_effective_input_rate(opts);
    const int old_staged_file_sample_rate = opts->staged_file_sample_rate;
    int old_runtime_input_rate = current_demod_rate(opts, state);
    int old_samples_per_symbol = state->samplesPerSymbol;
    int old_symbol_center = state->symbolCenter;
    int old_jitter = state->jitter;
    dsd_frontend_kind old_frontend_kind = opts->frontend_kind;
    const int old_trunk_scan_enabled = opts->trunk_scan_enabled;
    const cfg_rx_profile old_rx = cfg_rx_profile_of(opts);
    const int old_audio_channels = opts->pulse_digi_out_channels;
    const int old_audio_rate = opts->pulse_digi_rate_out;
    const dsdneoUserDecodeMode old_decode_mode = dsd_infer_decode_mode_preset_exact(opts);

    DSD_SNPRINTF(old_audio_in_dev, sizeof old_audio_in_dev, "%s", opts->audio_in_dev);
    DSD_SNPRINTF(old_audio_out_dev, sizeof old_audio_out_dev, "%s", opts->audio_out_dev);

    DSD_MEMCPY(&cfg, c->data, sizeof cfg);
    const int prepare_rc = cfg_prepare_runtime_apply(opts, state, &cfg);
    if (prepare_rc != UI_CMD_APPLY_COMPLETED) {
        return prepare_rc;
    }
    ui_stage_analog_entry(opts, state);
#ifdef USE_RADIO
    ui_cfg_rollback rollback;
    ui_cfg_capture_rollback(opts, state, &rollback);
#endif
    dsd_apply_user_config_to_opts(&cfg, opts, state);
    /* A [mode] preset carries an audio layout too, but the session's output streams were opened with the layout in
       force then, which the backend fixes for their life: the session's layout is kept, as decode_mode_apply_value()
       keeps it for DSD_APP_CMD_DECODE_MODE_SET. Put back before anything below opens or reopens an output (a changed
       output device, the input-policy reconfigure, the sink a family change opens), so every stream is opened with
       the layout the decoder writes. */
    opts->pulse_digi_out_channels = old_audio_channels;
    opts->pulse_digi_rate_out = old_audio_rate;
    const int restart_required = cfg_restore_lifecycle_owned(opts, &cfg, old_frontend_kind, old_trunk_scan_enabled);
#ifdef USE_RADIO
    /* A reopen whose start fails puts the receive side back (ui_cfg_settle_reopen()), so everything below sees the
       input, mode and widths the session ran, and publishes nothing for the settings the start refused; with no stream
       running before it there is nothing to put back, and no stream to publish to. */
    const int radio_rc = apply_cfg_radio_input(opts, state, &cfg, &rollback);
#endif
    const cfg_input_before input_before = {old_audio_in_dev, old_audio_in_type, old_wav_sample_rate,
                                           old_effective_input_rate, old_staged_file_sample_rate};
    apply_cfg_runtime_hot_switches(opts, state, &cfg, &input_before, old_audio_out_dev, old_audio_out_type);
    restore_live_pcm_rate_after_staged_file_apply(opts, &cfg, old_wav_sample_rate);
    apply_cfg_file_runtime_rate(opts, state, &cfg, old_runtime_input_rate, old_samples_per_symbol, old_symbol_center,
                                old_jitter);
    cfg_forget_rx_tone_on_boundary(opts, state, old_audio_in_type, old_audio_in_dev, old_decode_mode);
    int reconfigure_rc = ui_reconfigure_output_for_input_policy(opts, state);
    /* A width the front end refused after the check (put back, with a toast) fails the apply like a reconfigure. */
    reconfigure_rc |= apply_cfg_receive_family_change(opts, state, &cfg, &old_rx);
#ifdef USE_RADIO
    ui_cfg_note_refused_row(radio_rc);
    if (radio_rc != 0) {
        return UI_CMD_APPLY_FAILED;
    }
#endif
    if (cfg.has_input && cfg.input_source == DSDCFG_INPUT_AIRSPY && old_audio_in_type != AUDIO_IN_RTL) {
        return UI_CMD_APPLY_RESTART_REQUIRED;
    }
    if (restart_required) {
        return UI_CMD_APPLY_RESTART_REQUIRED;
    }
    return (reconfigure_rc == 0) ? UI_CMD_APPLY_COMPLETED : UI_CMD_APPLY_FAILED;
}

static int
apply_cmd_misc_config(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    static const struct dsd_app_command_handler_entry k_handlers[] = {
        {DSD_APP_CMD_LCW_RETUNE_TOGGLE, ui_cmd_handle_lcw_retune_toggle},
        {DSD_APP_CMD_P25_CC_CAND_TOGGLE, ui_cmd_handle_p25_cc_cand_toggle},
        {DSD_APP_CMD_REVERSE_MUTE_TOGGLE, ui_cmd_handle_reverse_mute_toggle},
        {DSD_APP_CMD_CONFIG_APPLY, ui_cmd_handle_config_apply},
        {DSD_APP_CMD_CONFIG_METADATA_SET, ui_cmd_handle_config_metadata_set},
    };
    return ui_cmd_apply_handler_table(k_handlers, sizeof k_handlers / sizeof k_handlers[0], opts, state, c);
}

static int
apply_cmd_unscoped(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    static const dsd_app_command_handler_fn k_command_groups[] = {
        apply_cmd_basic_a,          apply_cmd_slot_controls,  apply_cmd_payload_filters, apply_cmd_constellation,
        apply_cmd_eye_spectrum,     apply_cmd_trunk_controls, apply_cmd_lockout_slot,    apply_cmd_skip_slot,
        apply_cmd_tg_listen,        apply_cmd_provoice_m17,   apply_cmd_scan_controls,   apply_cmd_channel_cycle,
        apply_cmd_capture_playback, apply_cmd_misc_config,    apply_cmd_foundation,
    };
    if (!c) {
        return UI_CMD_APPLY_INVALID_PAYLOAD;
    }
    if (!opts) {
        if (c->id == DSD_APP_CMD_QUIT) {
            dsd_exitflag_store(1);
            return UI_CMD_APPLY_COMPLETED;
        }
        return UI_CMD_APPLY_UNSUPPORTED;
    }
    int r = ui_cmd_dispatch(opts, state, c);
    if (r) {
        return r;
    }
    r = apply_cmd_ui_visibility(opts, c);
    if (r) {
        return r;
    }
    r = apply_cmd_key_management(opts, state, c);
    if (r) {
        return r;
    }
    r = apply_cmd_runtime_toggles(opts, c);
    if (r) {
        return r;
    }
    r = apply_cmd_io_and_import(opts, state, c);
    if (r) {
        return r;
    }
#ifdef USE_RADIO
    r = apply_cmd_dsp(c, opts, state);
    if (r) {
        return r;
    }
#endif
    for (size_t i = 0; i < (sizeof k_command_groups / sizeof k_command_groups[0]); ++i) {
        r = k_command_groups[i](opts, state, c);
        if (r) {
            return r;
        }
    }
    return UI_CMD_APPLY_UNSUPPORTED;
}

static int
command_updates_scan_mode(const struct dsd_app_command* c) {
    if (c && c->id == DSD_APP_CMD_DECRYPTION_APPLY && c->n == sizeof(dsd_app_decryption_payload)) {
        int32_t scope;
        uint32_t fields;
        DSD_MEMCPY(&scope, c->data + offsetof(dsd_app_decryption_payload, scope), sizeof(scope));
        DSD_MEMCPY(&fields, c->data + offsetof(dsd_app_decryption_payload, fields), sizeof(fields));
        return scope == DSD_APP_KEY_SCOPE_DEFAULTS
               && (fields & (DSD_APP_DECRYPTION_FORCE | DSD_APP_DECRYPTION_MATERIAL));
    }
    /* Commands that edit configuration in place run against the saved baseline.
     * Ownership changes (including RR import) release the scope before editing.
     * Live controls must continue to inspect the effective row, so suspending
     * every command and diffing afterward would change their behavior. */
    static const int commands[] = {
        DSD_APP_CMD_TG_LIST_EXPORT,
        DSD_APP_CMD_FORCE_KEY_SET,
        DSD_APP_CMD_ALL_MUTES_TOGGLE,
        DSD_APP_CMD_FORCE_PRIV_TOGGLE,
        DSD_APP_CMD_FORCE_RC4_TOGGLE,
        DSD_APP_CMD_AGGR_SYNC_TOGGLE,
        DSD_APP_CMD_TRUNK_DATA_TOGGLE,
        DSD_APP_CMD_TRUNK_ENC_TOGGLE,
        DSD_APP_CMD_P25_CC_CAND_TOGGLE,
        DSD_APP_CMD_SCAN_VOICE_ONLY_SET,
        DSD_APP_CMD_SCAN_VOICE_QUALIFY_MS_SET,
        DSD_APP_CMD_SCAN_VOICE_HOLD_MS_SET,
        /* DSD_APP_CMD_NFM_BANDWIDTH_SET and DSD_APP_CMD_AM_BANDWIDTH_SET are deliberately not here: like squelch, the
         * command edits the configured width (svc_set_analog_bandwidth(), through
         * dsd_scan_mode_set_configured_nfm_bandwidth() for NFM) instead of suspending and re-applying the row, which
         * would read the live acquisition the row has made as a change and end a followed call. An analog row's own
         * width of the edited kind (--nfm-bandwidth-hz or --am-bandwidth-hz, issue #526) stays in force over it.
         * DSD_APP_CMD_TONE_FILTER_SET (issue #527) is not here for the same reason: it edits the configured tone
         * policy through dsd_scan_mode_set_configured_tone_policy(), and an nfm row's own tone options stay in force
         * over it. */
        DSD_APP_CMD_IMPORT_GROUP_LIST,
        DSD_APP_CMD_IMPORT_GROUP_LIST_CLEAR,
        DSD_APP_CMD_DECODE_MODE_SET,
        DSD_APP_CMD_MOD_SET,
        DSD_APP_CMD_MOD_TOGGLE,
        DSD_APP_CMD_MOD_P2_TOGGLE,
        DSD_APP_CMD_INVERT_TOGGLE,
        DSD_APP_CMD_COSINE_FILTER_TOGGLE,
        DSD_APP_CMD_INV_X2_TOGGLE,
        DSD_APP_CMD_INV_DMR_TOGGLE,
        DSD_APP_CMD_INV_DPMR_TOGGLE,
        DSD_APP_CMD_INV_M17_TOGGLE,
        DSD_APP_CMD_INPUT_MONITOR_TOGGLE,
        DSD_APP_CMD_CONFIG_APPLY,
        /* Squelch is a row option (--squelch-db), but its commands stay unscoped. The setter
         * edits the configured default through dsd_scan_mode_set_configured_squelch(), which
         * touches no acquisition setting, so a squelch nudge can never read as a decoder change
         * that ends the call. AIRSPY_SET and the input enables rewrite no squelch, and a stream
         * they reopen has to start on the row's acquisition and threshold, the ones in force (its
         * CQPSK state, which an open picks by its own rule, follows: ui_publish_row_to_started_stream()). */
    };
    if (!c) {
        return 0;
    }
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
        if (commands[i] == c->id) {
            return 1;
        }
    }
    return 0;
}

/* Every free, publish or realloc of the live talkgroup policy store runs under
 * the P25 SM tick guard: the watchdog's release-time audio flush evaluates policy
 * under it (#554). The guard is not re-entrant. Nothing reached from these commands
 * may acquire it with p25_sm_tick_guard_enter(), run_manual_retune_guarded(),
 * svc_rtl_restart(), svc_airspy_apply_config() or noCarrier(); use the _locked forms.
 * TRUNK_SET and RTL_SET_FREQ deliberately use narrow brackets instead. */
static int
command_writes_policy_store(const struct dsd_app_command* c) {
    static const int commands[] = {
        DSD_APP_CMD_IMPORT_GROUP_LIST,
        DSD_APP_CMD_IMPORT_GROUP_LIST_CLEAR,
        DSD_APP_CMD_CONFIG_APPLY,
        DSD_APP_CMD_RR_APPLY_IMPORT,
        DSD_APP_CMD_IMPORT_CHANNEL_MAP,
        DSD_APP_CMD_IMPORT_CHANNEL_MAP_CLEAR,
        DSD_APP_CMD_TUNER_RELEASE,
        DSD_APP_CMD_SCANNER_TOGGLE,
        DSD_APP_CMD_TG_LISTEN_SET,
        DSD_APP_CMD_TG_LISTEN_SET_ALL,
        DSD_APP_CMD_TG_ROW_SET,
        DSD_APP_CMD_TG_ROW_REMOVE,
        DSD_APP_CMD_TG_SELECTION_SET,
        DSD_APP_CMD_LOCKOUT_SLOT,
        DSD_APP_CMD_TG_SESSION_AVOID_CLEAR,
        DSD_APP_CMD_SKIP_SLOT,
    };
    if (!c) {
        return 0;
    }
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
        if (commands[i] == c->id) {
            return 1;
        }
    }
    return 0;
}

/* A command that turned the conventional scanner off (ui_cmd_leaves_scanner_scope()) ends its scan. */
static void
apply_cmd_leave_scanner_scope(dsd_opts* opts, dsd_state* state, int guarded) {
    if (!guarded) {
        p25_sm_tick_guard_enter();
    }
    /* A suspended scope leaves the newly applied configuration in place. */
    (void)svc_leave_channel_scan(opts, state);
    dsd_scan_keys_leave(state);
    if (!guarded) {
        p25_sm_tick_guard_leave();
    }
}

/* What a scoped command did to the stream while the scan scope was suspended (issue #578). */
typedef enum {
    UI_SCOPE_STREAM_KEPT = 0,    /* started none */
    UI_SCOPE_STREAM_STARTED,     /* started one, which opened on other settings than the row's the resume puts back */
    UI_SCOPE_STREAM_REFUSES_ROW, /* started one that refuses the row on air, which the command reported */
} ui_scope_stream;

/* What the command that ran since svc_rtl_start_count() read @p starts_before did to the stream. Takes the note of a
   refused row a config apply left (g_cfg_row_refusal_reported), so that it goes with that command alone. */
static ui_scope_stream
ui_scope_stream_of(unsigned int starts_before) {
    const int row_refused = g_cfg_row_refusal_reported;
    g_cfg_row_refusal_reported = 0;
    if (svc_rtl_start_count() == starts_before) {
        return UI_SCOPE_STREAM_KEPT;
    }
    return row_refused ? UI_SCOPE_STREAM_REFUSES_ROW : UI_SCOPE_STREAM_STARTED;
}

/* The row's constraint back over the configured options a scoped command edited, and the front end told of what
   changed. A width the row runs on the analog family is an acquisition setting there (dsd_scan_settings_equal()), so a
   configured width it runs reaches the front end with the profile the resume publishes, which keeps the width from
   before the command for a refusal where it lands (ui_settle_receive_requests()); a digital row's front end
   takes no width, and a row that sets its own (issue #526) keeps it over the edit. Returns -1 when that was a switch
   onto the analog monitor, or between FM and AM, the front end refused at once, which puts the decoder back as it was
   and fails the command. A stream the command started meanwhile (@p stream) gets the row's profile either way
   (ui_resume_scope_and_publish()); one that refuses the row on air, as the command reported
   (UI_SCOPE_STREAM_REFUSES_ROW: a config apply with no stream before it, which keeps the input it opened and the whole
   config), refuses that profile for the reason the command's toast gives, not for a width or a switch the command
   made, so nothing is put back and the toast stands (issue #578). */
static int
apply_cmd_resume_scope(dsd_opts* opts, dsd_state* state, const ui_analog_widths* before, ui_scope_stream stream) {
    int changed = 0;
    if (ui_resume_scope_and_publish(opts, state, &changed, before, stream != UI_SCOPE_STREAM_KEPT) == 0
        || stream == UI_SCOPE_STREAM_REFUSES_ROW) {
        return 0;
    }
    /* Refused at once (a retune moved the demod rate since the command held the width to it): the front end kept its
       receive profile, so the decoder goes back to it, the mode it had before a switch onto the monitor or between FM
       and AM, or else the width the monitor kept. */
    if (ui_revert_analog_entry(opts, state, dsd_opts_analog_width_hz(opts), NULL)) {
        return -1;
    }
    const int kind = opts->analog_demod;
    const int before_hz = ui_analog_width_before(before, kind);
    const int width_hz = dsd_app_analog_width_setting_hz(opts, kind);
    if (width_hz != before_hz) {
        /* Only a row that takes the configured width has its width in force changed by a scoped command, so the width
           in force before it was the configured one as well. */
        ui_restore_refused_analog_width(opts, state, kind, width_hz, before_hz, before_hz);
    }
    return 0;
}

/* The decode mode a config apply's [mode] sets (@p c), or DSDCFG_MODE_UNSET for another command or a config without
   one. Read from the command's copy of the config, field by field, since its payload bytes carry no alignment. */
static dsdneoUserDecodeMode
apply_cmd_config_decode_mode(const struct dsd_app_command* c) {
    if (!c || c->id != DSD_APP_CMD_CONFIG_APPLY || c->n < sizeof(dsdneoUserConfig)) {
        return DSDCFG_MODE_UNSET;
    }
    int has_mode = 0;
    dsdneoUserDecodeMode mode = DSDCFG_MODE_UNSET;
    DSD_MEMCPY(&has_mode, c->data + offsetof(dsdneoUserConfig, has_mode), sizeof has_mode);
    DSD_MEMCPY(&mode, c->data + offsetof(dsdneoUserConfig, decode_mode), sizeof mode);
    return has_mode ? mode : DSDCFG_MODE_UNSET;
}

/*
 * AM runs only on an I/Q input (issue #524). A command that left the configured AM preset on a PCM input -- a live
 * input switch (Pulse, WAV, symbol, TCP or UDP audio), or a config that applied decode = am on one -- falls the session
 * back to the Analog monitor, as a start with a config's decode = am on PCM input does, and says why. Checked after
 * every command, so no input path can leave AM running on demodulated audio. The fallback is a decode-mode change of
 * the configured mode, made through the scope as DSD_APP_CMD_DECODE_MODE_SET makes one.
 *
 * After a config apply whose [mode] is decode = am (@p c), as after a start with one, autosave is turned off
 * (dsd_decode_mode_keep_saved_am()): that decode = am holds for the sessions that can run it, and autosave would write
 * the fallback over it in the file a Config > Load has just made the autosave target. Otherwise the fallback follows
 * the input: a live input switch, or a config that moves a running AM session's input onto PCM without a [mode] of
 * its own, is the operator moving the session there, and autosave keeps that input with the Analog monitor it runs.
 */
static void
apply_cmd_fall_back_from_am_on_pcm(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    if (!opts || !state || dsd_scan_mode_configured_preset_exact(opts, state) != DSDCFG_MODE_AM
        || dsd_decode_mode_input_is_iq(opts)) {
        return;
    }
    const int scoped = dsd_scan_mode_suspend(opts, state);
    const int rc = decode_mode_apply_value(opts, state, DSDCFG_MODE_ANALOG);
    if (scoped) {
        int changed = 0;
        (void)ui_resume_scope_and_publish(opts, state, &changed, NULL, 0);
    }
    if (rc != UI_CMD_APPLY_COMPLETED) {
        return;
    }
    LOG_WARN("WARNING: %s. Decoding Analog on this input.\n", DSD_DECODE_MODE_AM_NEEDS_IQ_TEXT);
    if (apply_cmd_config_decode_mode(c) == DSDCFG_MODE_AM && dsd_decode_mode_keep_saved_am(state)) {
        LOG_INFO("NOTICE: %s.\n", DSD_DECODE_MODE_KEEP_SAVED_AM_NOTICE);
        return;
    }
    ui_set_toast(state, 6, "Decoding Analog: %s", DSD_DECODE_MODE_AM_NEEDS_IQ_TEXT);
}

/* The configured width of each analog kind (dsd_scan_mode_configured_analog_width()). */
static ui_analog_widths
ui_configured_analog_widths(const dsd_opts* opts, const dsd_state* state) {
    const ui_analog_widths widths = {dsd_scan_mode_configured_analog_width(opts, state, DSD_ANALOG_DEMOD_FM),
                                     dsd_scan_mode_configured_analog_width(opts, state, DSD_ANALOG_DEMOD_AM)};
    return widths;
}

/* A config apply that changed the configured width of an analog kind from @p configured_before, once it has made its
   requests (the scope's resume included): the change counts for a refusal where a later request of that kind lands, as
   a width command's change does, whether the apply asked the front end for that width or not (the kind not in force,
   the kind a switch in the same config leaves, one a scan row's own width shadows) (svc_note_analog_width_change()). */
static void
ui_note_config_width_changes(const dsd_opts* opts, const dsd_state* state, const ui_analog_widths* configured_before) {
    const ui_analog_widths now = ui_configured_analog_widths(opts, state);
    for (int kind = DSD_ANALOG_DEMOD_FM; kind <= DSD_ANALOG_DEMOD_AM; kind++) {
        const int before_hz = ui_analog_width_before(configured_before, kind);
        if (ui_analog_width_before(&now, kind) != before_hz) {
            svc_note_analog_width_change(opts, state, kind, before_hz);
        }
    }
}

/* The analog widths a command starts from (apply_cmd_scoped()). */
typedef struct {
    ui_analog_widths in_force;   /* taken before a suspend puts the configured ones in dsd_opts */
    int config_apply;            /* 1: a config apply, whose configured-width changes are noted */
    ui_analog_widths configured; /* the configured widths before a config apply */
} ui_cmd_widths_before;

static ui_cmd_widths_before
ui_cmd_widths_before_of(const dsd_opts* opts, const dsd_state* state, const struct dsd_app_command* c) {
    ui_cmd_widths_before before = {{0, 0}, 0, {0, 0}};
    if (!opts) {
        return before;
    }
    before.in_force.nfm_hz = opts->analog_nfm_bandwidth_hz;
    before.in_force.am_hz = opts->analog_am_bandwidth_hz;
    before.config_apply = c && c->id == DSD_APP_CMD_CONFIG_APPLY && state;
    if (before.config_apply) {
        before.configured = ui_configured_analog_widths(opts, state);
    }
    return before;
}

/* The row's constraint back after a command that suspended it (@p scoped) and maybe started a stream meanwhile
   (@p stream) (apply_cmd_resume_scope()), then, for a config apply, the configured widths it changed noted once every
   request it made is made (ui_note_config_width_changes()). Returns -1 when the resume failed. */
static int
apply_cmd_resume_and_note_widths(dsd_opts* opts, dsd_state* state, int scoped, ui_scope_stream stream,
                                 const ui_cmd_widths_before* before) {
    const int resume_failed = scoped && apply_cmd_resume_scope(opts, state, &before->in_force, stream) != 0;
    if (before->config_apply) {
        ui_note_config_width_changes(opts, state, &before->configured);
    }
    return resume_failed ? -1 : 0;
}

/* The configured CTCSS/DCS receive policy, and whether nothing in the session runs detection for it (issue #527). */
typedef struct {
    int mode;
    dsd_tone_set set;
    int unused; /* a list policy (allow or block) that nothing in the session can apply */
} ui_tone_filter_reach;

static ui_tone_filter_reach
ui_tone_filter_reach_of(const dsd_opts* opts, const dsd_state* state) {
    ui_tone_filter_reach reach;
    DSD_MEMSET(&reach, 0, sizeof reach);
    if (!opts || !state) {
        return reach;
    }
    dsd_scan_mode_configured_tone_policy(opts, state, &reach.mode, &reach.set);
    reach.unused = (reach.mode == DSD_TONE_FILTER_ALLOW || reach.mode == DSD_TONE_FILTER_BLOCK)
                   && !dsd_engine_scan_hears_tones(opts, state);
    return reach;
}

/*
 * A tone filter set while AM or a digital mode is active warns once (issue #527). The command line and a scan's start
 * say so for the session they begin; a command says so when it is what left the configured list policy where nothing
 * runs detection (dsd_engine_scan_hears_tones()): a loaded config that sets a policy there, or a decode-mode change, a
 * channel map or a scanner toggle that takes the FM monitor away from the policy in force. A command after which the
 * same policy is still unheard says nothing again.
 */
static void
ui_warn_tone_filter_unreached(const dsd_opts* opts, const dsd_state* state, const ui_tone_filter_reach* before) {
    const ui_tone_filter_reach after = ui_tone_filter_reach_of(opts, state);
    if (!after.unused
        || (before->unused && before->mode == after.mode && dsd_tone_set_same_signals(&before->set, &after.set))) {
        return;
    }
    (void)dsd_scan_mode_warn_tone_filter_unused(opts, state, 0);
}

/* A rigctl request as a toast names it: "NFM 12.5 kHz", "NFM at its own passband". */
static void
ui_rigctl_passband_text(const dsd_rigctl_passband* passband, char* out, size_t out_size) {
    char width[DSD_ANALOG_WIDTH_TEXT_MAX];
    const char* label = dsd_analog_demod_label(passband->kind);
    if (passband->bandwidth_hz > 0 && dsd_analog_width_format(passband->bandwidth_hz, width, sizeof width) == 0) {
        DSD_SNPRINTF(out, out_size, "%s %s", label, width);
    } else {
        DSD_SNPRINTF(out, out_size, "%s at its own passband", label);
    }
}

/*
 * After any command, a rigctl peer that demodulates audio input is asked for the passband the command left in force
 * when it changed it (issue #621, svc_rigctl_follow_passband(), with @p guarded the hold apply_cmd() took): a
 * decode-mode or input switch onto, off or between monitors, a map import, a config; a peer the command brought (a
 * first rigctl connect, a switch onto audio input) only for a width, as the start asks it. The width, -B and "this
 * channel" commands have asked already, and the peer's record skips a request it is known to run. A refusal, or a lost
 * reply, of a config apply's request puts back the configured width of the refused kind the config changed (from
 * @p widths_before; a config sets no -B), asks again for the passband the session then runs, since the peer may run
 * either, and fails the command with the rest of the config applied. Any other command's change stands, with a
 * warning, and so does a config apply's that leaves the monitor (a refused -B, which a tune sends best-effort). Returns
 * -1 when the command fails.
 */
static int
apply_cmd_follow_rigctl_passband(dsd_opts* opts, dsd_state* state, const svc_rigctl_session* before,
                                 const ui_cmd_widths_before* widths_before, int guarded) {
    if (svc_rigctl_follow_passband(opts, state, before, guarded) >= 0) {
        return 0;
    }
    const svc_rigctl_session refused = svc_rigctl_session_now(opts, state);
    char refused_text[48];
    ui_rigctl_passband_text(&refused.request, refused_text, sizeof refused_text);
    /* Undoing a monitor's passband with a digital mode's -B (FOLLOW) is best-effort, as a tune's -B is: the change
       stands, a config apply's included. */
    if (!widths_before->config_apply || refused.request.source == DSD_RIGCTL_PASSBAND_FOLLOW) {
        LOG_WARN("WARNING: The rigctl peer refused %s; the setting stands.\n", refused_text);
        ui_set_toast(state, 5, "Rigctl peer refused %s (see log)", refused_text);
        return 0;
    }
    const int kind = refused.request.kind;
    const int old_hz = ui_analog_width_before(&widths_before->configured, kind);
    if (old_hz == dsd_scan_mode_configured_analog_width(opts, state, kind)) {
        /* The config changed what runs (a mode or an input), not the width: nothing to put back. */
        LOG_WARN("WARNING: The rigctl peer refused %s after a config apply; the config stays applied.\n", refused_text);
        ui_set_toast(state, 5, "Config applied; the rigctl peer refused %s (see log)", refused_text);
        return -1;
    }
    (void)svc_store_analog_width_setting(opts, state, kind, old_hz);
    (void)svc_rigctl_follow_passband(opts, state, &refused, guarded);
    dsd_app_analog_width_view view;
    (void)dsd_app_analog_width_view_get(opts, state, NULL, &view);
    char setting[DSD_APP_ANALOG_WIDTH_TEXT_MAX];
    (void)dsd_app_analog_width_setting_text(&view, kind, old_hz, setting, sizeof setting);
    const char* label = dsd_analog_demod_label(kind);
    LOG_WARN("WARNING: The rigctl peer refused %s after a config apply; the %s width stays %s.\n", refused_text, label,
             setting);
    ui_set_toast(state, 5, "Config applied; the rigctl peer refused %s: the %s width stays %s", refused_text, label,
                 setting);
    return -1;
}

static int
apply_cmd_scoped(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c, int guarded) {
    /* Before the suspend, so it is the request the row in force made. */
    const svc_rigctl_session rigctl_before = svc_rigctl_session_now(opts, state);
    const int mode_update = command_updates_scan_mode(c);
    const int was_scanner = opts && opts->scanner_mode == 1;
    const ui_cmd_widths_before widths_before = ui_cmd_widths_before_of(opts, state, c);
    const ui_tone_filter_reach tone_before = ui_tone_filter_reach_of(opts, state);
    const int scoped = mode_update && opts && state && dsd_scan_mode_suspend(opts, state);
    const int group_update = c
                             && (c->id == DSD_APP_CMD_IMPORT_GROUP_LIST || c->id == DSD_APP_CMD_IMPORT_GROUP_LIST_CLEAR
                                 || c->id == DSD_APP_CMD_CONFIG_APPLY);
    const int groups_suspended = group_update && dsd_scan_groups_suspend(state);
    /* A stream the command starts while the scope is suspended opens on other settings than the row the resume puts
       back: a reopen on the configured ones, a rollback's restart on the row as it ran before (issue #578). One a
       command that leaves the scope in force starts opens on the row's settings with the CQPSK state an open picks,
       and is asked for the row's profile at once (ui_publish_row_to_started_stream()). */
    const unsigned int starts_before = svc_rtl_start_count();
    const int result = apply_cmd_unscoped(opts, state, c);
    const ui_scope_stream stream = ui_scope_stream_of(starts_before);
    if (groups_suspended) {
        dsd_scan_groups_resume(state);
    }
    if (ui_cmd_leaves_scanner_scope(opts, was_scanner)) {
        apply_cmd_leave_scanner_scope(opts, state, guarded);
    }
    if (!scoped && stream != UI_SCOPE_STREAM_KEPT) {
        ui_publish_row_to_started_stream(opts, state);
    }
    if (apply_cmd_resume_and_note_widths(opts, state, scoped, stream, &widths_before) != 0) {
        return UI_CMD_APPLY_FAILED;
    }
    apply_cmd_fall_back_from_am_on_pcm(opts, state, c);
    const int rigctl_refused = apply_cmd_follow_rigctl_passband(opts, state, &rigctl_before, &widths_before, guarded);
    ui_warn_tone_filter_unreached(opts, state, &tone_before);
    return rigctl_refused ? UI_CMD_APPLY_FAILED : result;
}

static int
apply_cmd(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    const int guarded = command_writes_policy_store(c);
    if (guarded) {
        if (!p25_sm_tick_guard_try_enter()) {
#ifdef DSD_NEO_TEST_HOOKS
            (void)dsd_atomic_u64_fetch_add_relaxed(&g_test_policy_guard_waits, 1U);
#endif
            p25_sm_tick_guard_enter();
        }
    }
    const int result = apply_cmd_scoped(opts, state, c, guarded);
    if (guarded) {
        p25_sm_tick_guard_leave();
    }
    return result;
}

/* A "this channel" width edit's request (not a scan leave's): its edit goes back, never the configured width (issue
 * #518). Returns 1 when the refusal was the edit's and is settled. */
static int
ui_settle_refused_row_edit(dsd_opts* opts, dsd_state* state, const svc_monitor_refusal* refusal) {
    char row_notice[DSD_APP_SCAN_ROW_NOTICE_SIZE];
    if (refusal->scan_leave
        || !dsd_app_scan_row_edit_settle_refusal(opts, state, refusal, row_notice, sizeof row_notice)) {
        return 0;
    }
    ui_set_toast(state, 5, "%s", row_notice);
    dsd_telemetry_publish_opts_snapshot(opts);
    dsd_telemetry_publish_snapshot(state);
    return 1;
}

/*
 * An analog monitor request the demod thread refused where it landed (a retune moved the demod rate after its width was
 * checked) is not in force, so the decoder follows what the front end kept, the toast says why, and frontends see it at
 * once: a front end still on the digital family never made the switch onto the monitor, and one still running the
 * other analog kind never made the switch between FM and AM, so the decoder goes back to the mode it had
 * (ui_revert_analog_entry()); one on the analog family of the kind asked for kept the width it ran, and the configured
 * width goes back to that one, as the stream recorded the setting (so of two requests queued back to back, the first
 * taken and the second refused, the first one's width stands, and an explicit 6000 Hz AM width stays explicit rather
 * than turning into the default it filters the same as). Under a scan row it goes back to the configured width the
 * front end ran, never to a row's own width (svc_restore_analog_width()): from before the refused change, or from
 * before the first of two the demod thread took together, the first replaced by the second in its queue.
 *
 * A scan leave's return to the monitor, refused at once or where it landed, or a request that carried it on
 * (svc_leave_channel_scan()), is reconciled as ui_settle_refused_scan_leave() says (issue #578), since a typed row may
 * have left the front end off the monitor with the width setting untouched, unless a request queued after it decides
 * the front end instead (svc_monitor_refusal::superseded), or the scan goes on (ui_scan_goes_on(): a channel-map adopt
 * or a RadioReference import that keeps the scanner on), whose next row's tune decides it. A switch onto the monitor or
 * between FM and AM armed before the leave goes back first (ui_revert_analog_entry()) when the front end kept the
 * digital family or the other kind, and is dropped when it kept the analog family of the kind asked for, but in neither
 * of those two cases, which are checked first, before anything else is done with the refusal
 * (ui_scan_leave_left_to_later()), whatever the front end kept: the old mode's profile would replace the later
 * request's (a CQPSK toggle made before the demod thread took the return), or go back on a return the scan has moved on
 * from, and dropping the switch would leave no mode to go back to. The switch then stays armed, since the later request
 * or the row's tune, which is no monitor request, did not make it either, and the next monitor request's outcome
 * settles it (dropped once one is taken, gone back when one is refused off the kind asked for), or a revert finds the
 * configured settings moved on and drops it. A switch the leave's refusal put back asks the front end for the kind it
 * restores with the leave carried on, so a refusal of that request, at once or where it lands, is reconciled for that
 * kind as the leave's own would be.
 *
 * A request the demod thread never took (SVC_MONITOR_REQUEST_SUPERSEDED: a later request replaced it in the stream's
 * queue, or a retune's receive family retired it, as a scan that goes on to an analog row does with a leave's return)
 * is neither taken nor refused: what replaced it decides the front end, nothing is reconciled, and a switch armed
 * before it stays armed for the next monitor request to settle, as for a scan leave left to later (issue #578).
 */
static void
ui_settle_receive_requests(dsd_opts* opts, dsd_state* state) {
    if (!opts || !state) {
        return;
    }
    svc_monitor_refusal refusal = {0};
    const int outcome = svc_take_monitor_request_outcome(opts, state, &refusal);
    if (outcome == SVC_MONITOR_REQUEST_TAKEN) {
        g_analog_entry.armed = 0; /* the front end took the switch, or a request made after it */
        return;
    }
    /* Nothing outstanding, still pending, or superseded before the front end took it: an armed switch stays armed. */
    if (outcome != SVC_MONITOR_REQUEST_REFUSED) {
        return;
    }
    /* A scan leave's refusal that a later request superseded, or whose scan goes on, is left to that request or to the
       next row's tune before any branch below touches an armed switch, whatever the front end kept: going back would
       publish the old mode's profile over it, and dropping the switch would lose the mode to go back to. Neither is a
       monitor request (one would have taken the record over), so the front end has made the switch no more than
       before, and it stays armed. */
    if (ui_scan_leave_left_to_later(opts, state, &refusal)) {
        return;
    }
    if (ui_settle_refused_row_edit(opts, state, &refusal)) {
        return;
    }
    int changed = 0;
    if (!refusal.kept_analog || refusal.kept_kind != refusal.kind) {
        changed = ui_revert_analog_entry(opts, state, refusal.width_hz, &refusal);
        if (!changed && refusal.scan_leave) {
            changed = ui_settle_refused_scan_leave(opts, state, &refusal);
        }
    } else if (refusal.scan_leave) {
        g_analog_entry.armed = 0; /* the front end runs the analog family of the kind the switch was onto */
        changed = ui_settle_refused_scan_leave(opts, state, &refusal);
    } else {
        g_analog_entry.armed = 0;
        const int kept_hz = refusal.kept_width_hz;
        if (dsd_app_analog_width_setting_hz(opts, refusal.kind) == refusal.width_hz && kept_hz != refusal.width_hz) {
            /* The width has not changed again since. */
            ui_restore_refused_analog_width(opts, state, refusal.kind, refusal.width_hz, kept_hz,
                                            refusal.configured_before_hz);
            changed = 1;
        }
    }
    if (changed) {
        dsd_telemetry_publish_opts_snapshot(opts);
        dsd_telemetry_publish_snapshot(state);
    }
}

int
dsd_app_commands_pending(void) {
    ensure_mu_init();
    dsd_mutex_lock(&g_mu);
    const int pending = !q_is_empty_unlocked();
    dsd_mutex_unlock(&g_mu);
    return pending;
}

int
dsd_app_drain_cmds(dsd_opts* opts, dsd_state* state) {
    int n_applied = 0;
    ensure_mu_init();
    for (;;) {
        /* Before each command, not only once per drain: the demod thread can take or refuse a receive request between
           two commands, and a switch between analog kinds or onto the monitor keeps the baseline of one still armed
           only while that one is unsettled (ui_arm_analog_entry()). */
        ui_settle_receive_requests(opts, state);
        struct dsd_app_command cmd;
        int have = 0;
        dsd_mutex_lock(&g_mu);
        if (!q_is_empty_unlocked()) {
            cmd = g_q[g_head];
            DSD_SECURE_ZERO(&g_q[g_head], sizeof(g_q[g_head]));
            g_head = (g_head + 1) % DSD_APP_CMD_Q_CAP;
            have = 1;
        }
        // Reset overflow warning gate when queue has space again
        if (((g_tail + 1) % DSD_APP_CMD_Q_CAP) != g_head) {
            atomic_store(&g_overflow_warn_gate, 0);
        }
        dsd_mutex_unlock(&g_mu);
        if (!have) {
            break;
        }
        if (!ui_cmd_payload_is_valid(&cmd)) {
            tg_export_publish_result(&cmd, UI_CMD_APPLY_INVALID_PAYLOAD);
            dsd_app_publish_decryption_result(&cmd, DSD_APP_KEY_INVALID);
            DSD_SECURE_ZERO(&cmd, sizeof cmd);
            n_applied++;
            continue;
        }
        const int result = apply_cmd(opts, state, &cmd);
#ifdef DSD_NEO_TEST_HOOKS
        g_test_last_result = result;
#endif
        tg_export_publish_result(&cmd, result);
        dsd_app_publish_decryption_result(&cmd, result);
        DSD_SECURE_ZERO(&cmd, sizeof cmd);
        // After applying a command, publish updated snapshots so the UI can
        // render consistent opts/state without racing live structures.
        if (opts && state && opts->scanner_mode == 1 && opts->trunk_scan_enabled != 1) {
            const double now_m = dsd_decode_now_mono_s();
            // Controls can change visits or hold state inside a long input wait.
            // Refresh the gate and its publication before exposing that command.
            dsd_scan_voice_gate_tick(opts, state, 0, now_m);
            dsd_engine_scan_visit_tick(opts, state, now_m);
            dsd_engine_scan_y_timing_tick(opts, state, now_m, dsd_decode_now_realtime_s());
        }
        dsd_telemetry_publish_opts_snapshot(opts);
        if (state) {
            dsd_telemetry_publish_snapshot(state);
        }
        n_applied++;
    }
    return n_applied;
}

#ifdef DSD_NEO_TEST_HOOKS
int
dsd_app_command_test_policy_guard_waits(void) {
    return (int)dsd_atomic_u64_load_relaxed(&g_test_policy_guard_waits);
}

int
dsd_app_command_test_last_failed(void) {
    return g_test_last_result == UI_CMD_APPLY_FAILED;
}

int
dsd_app_command_test_last_invalid_payload(void) {
    return g_test_last_result == UI_CMD_APPLY_INVALID_PAYLOAD;
}

static int
command_bytes_zero(const void* storage, size_t size) {
    const unsigned char* bytes = storage;
    for (size_t i = 0; i < size; ++i) {
        if (bytes[i]) {
            return 0;
        }
    }
    return 1;
}

int
dsd_app_command_test_storage_cleared(void) {
    ensure_mu_init();
    dsd_mutex_lock(&g_mu);
    int clear = 1;
    for (size_t i = g_tail; i != g_head; i = (i + 1) % DSD_APP_CMD_Q_CAP) {
        clear &= command_bytes_zero(&g_q[i], sizeof g_q[i]);
    }
    if (g_head == g_tail) {
        clear = command_bytes_zero(g_q, sizeof g_q);
    }
    dsd_mutex_unlock(&g_mu);
    return clear;
}

int
dsd_app_command_test_tail_padding_cleared(void) {
    ensure_mu_init();
    dsd_mutex_lock(&g_mu);
    const struct dsd_app_command* c = &g_q[(g_tail + DSD_APP_CMD_Q_CAP - 1) % DSD_APP_CMD_Q_CAP];
    int clear = command_bytes_zero(c->data + c->n, sizeof c->data - c->n);
    dsd_mutex_unlock(&g_mu);
    return clear;
}
#endif
