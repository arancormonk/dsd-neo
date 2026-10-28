// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/app_control/commands.h>
#include <dsd-neo/app_control/recording_view.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/wav_journal.h>
#include <dsd-neo/core/wav_recording.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/threading.h>
#include <stdint.h>
#include "recording_view_internal.h"

/* Recording commands are counted when the queue admits them and when they leave it (applied, refused, dropped), in
   queue order, so a command is pending while fewer have left than were admitted. The published view carries how many
   had left when it was built: a command reads as applied only in a view built after it, the one that shows its mode.
   Process-wide, as the queue's export results are; a session's stop drops its pending commands, which leave too. */
static dsd_mutex_t g_mu;
static atomic_int g_mu_init = 0;
static uint64_t g_submitted;
static int32_t g_submitted_id;
static uint64_t g_resolved;
static int g_resolved_outcome;
static int32_t g_resolved_id;
static dsd_app_recording_view g_pub;
static uint64_t g_pub_resolved;

/* 0 = uninitialized, 1 = initialization in flight, 2 = ready. */
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

int
dsd_app_recording_command_tracked(int cmd_id) {
    return cmd_id == DSD_APP_CMD_WAV_START || cmd_id == DSD_APP_CMD_WAV_STOP || cmd_id == DSD_APP_CMD_WAV_TOGGLE
           || cmd_id == DSD_APP_CMD_WAV_STATIC_OPEN;
}

void
dsd_app_recording_command_submitted(int cmd_id) {
    if (!dsd_app_recording_command_tracked(cmd_id)) {
        return;
    }
    ensure_mu_init();
    dsd_mutex_lock(&g_mu);
    g_submitted++;
    g_submitted_id = cmd_id;
    dsd_mutex_unlock(&g_mu);
}

void
dsd_app_recording_command_resolved(int cmd_id, int outcome) {
    if (!dsd_app_recording_command_tracked(cmd_id)) {
        return;
    }
    ensure_mu_init();
    dsd_mutex_lock(&g_mu);
    if (g_resolved < g_submitted) {
        g_resolved++;
    }
    g_resolved_outcome = outcome;
    g_resolved_id = cmd_id;
    if (outcome == DSD_APP_RECORDING_COMMAND_DROPPED) {
        /* A dropped command changed nothing a later view would show, and none may follow (a session that stopped). */
        g_pub.command_state = (uint8_t)outcome;
        g_pub.command_id = cmd_id;
        g_pub_resolved = g_resolved;
    }
    dsd_mutex_unlock(&g_mu);
}

static uint8_t
recording_mode(const dsd_opts* opts) {
    if (opts->dmr_stereo_wav == 1) {
        return DSD_APP_RECORDING_MODE_PER_CALL;
    }
    if (opts->static_wav_file == 1) {
        return DSD_APP_RECORDING_MODE_STATIC;
    }
    return DSD_APP_RECORDING_MODE_OFF;
}

void
dsd_app_recording_view_publish(const dsd_opts* opts) {
    if (!opts) {
        return;
    }
    dsd_app_recording_view view;
    DSD_MEMSET(&view, 0, sizeof view);
    view.published = 1;
    view.mode = recording_mode(opts);
    view.per_call = view.mode == DSD_APP_RECORDING_MODE_PER_CALL;
    if (view.per_call) {
        view.slot_files = (uint8_t)((opts->wav_out_f != NULL ? 1U : 0U) | (opts->wav_out_fR != NULL ? 2U : 0U));
    }
    view.unknown_only = 0;
    switch (dsd_wav_journal_state_get()) {
        case DSD_WAV_JOURNAL_ON: view.journal_state = DSD_APP_RECORDING_JOURNAL_ON; break;
        case DSD_WAV_JOURNAL_FAILED: view.journal_state = DSD_APP_RECORDING_JOURNAL_FAILED; break;
        default: view.journal_state = DSD_APP_RECORDING_JOURNAL_NONE; break;
    }
    dsd_wav_recording_status status;
    dsd_wav_recording_status_get(&status);
    for (int kind = 0; kind < DSD_WAV_RECORDING_ERROR_KINDS; kind++) {
        view.failures[kind] = status.failures[kind];
    }
    view.last_error = status.last_error;

    ensure_mu_init();
    dsd_mutex_lock(&g_mu);
    view.command_state = (uint8_t)(g_resolved == 0 ? DSD_APP_RECORDING_COMMAND_NONE : g_resolved_outcome);
    view.command_id = g_resolved == 0 ? 0 : g_resolved_id;
    g_pub = view;
    g_pub_resolved = g_resolved;
    dsd_mutex_unlock(&g_mu);
}

int
dsd_app_recording_view_get(dsd_app_recording_view* out) {
    if (!out) {
        return -1;
    }
    ensure_mu_init();
    dsd_mutex_lock(&g_mu);
    *out = g_pub;
    if (g_submitted > g_pub_resolved) {
        out->command_state = DSD_APP_RECORDING_COMMAND_PENDING;
        out->command_id = g_submitted_id;
    }
    dsd_mutex_unlock(&g_mu);
    return out->published ? 1 : 0;
}
