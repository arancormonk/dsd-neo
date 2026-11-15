// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

/** Test-only host for issue #834 item 3: a real call, replayed through the real engine, on a -Y row that has landed.
 *
 * -Y -C parses a real two-row map and row zero's own options are installed as the conventional scanner installs them
 * at its commit. I/Q replay cannot retune (a tune is deferred), so no step ever lands; instead this host notes row
 * zero's landing itself at the first hunt, on the replay's decode clock, which arms the -Y step's frame-boundary rule
 * exactly as a commit would. It then prints, on stderr, the timeline the check (tests/y_call_hold_check.py) judges:
 *
 *   YHOLD landed t=<s>
 *   YHOLD sync t=<s> proven=<n> partial=<n> anchor=<s>  every sync the hunt returned, with the PROVEN and PARTIAL
 *                                       notes so far and the -Y hangtime anchor (last_cc_sync_time)
 *   YHOLD step t=<s> via=<frame|nocarrier> every -Y step attempt, and whether a frame-boundary yield led to it
 *
 * Times are decode-mono seconds (the replay's clock). */
#include <assert.h>
#include <dsd-neo/core/channel_mode.h>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/scan_profile.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/engine/engine.h>
#include <dsd-neo/engine/scan_voice_gate.h>
#include <dsd-neo/runtime/bootstrap.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/scan_mode.h>
#include <dsd-neo/runtime/scan_options.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "scan_landing.h"
#include "scan_owner.h"

static int g_landed = 0;
static uint64_t g_yield_epoch_seen = 0U;
static int64_t g_yield_mark_seen = 0;

/* Row zero's own options, as the conventional scanner installs them on commit (issue #521). An untyped row (a list
   with no mode column, mode INHERIT) keeps the session's decoder set, as the untyped step does. */
static void
enter_row_zero(dsd_opts* opts, dsd_state* state) {
    const dsd_scan_mode mode = dsd_channel_mode_get(state, 0);
    if (mode != DSD_SCAN_MODE_INHERIT) {
        const dsd_scan_row_profile* profile = dsd_channel_profile_get(state, 0);
        dsd_scan_settings prepared;
        const int prepared_rc = dsd_scan_mode_prepare(opts, state, mode, profile ? &profile->values : NULL, &prepared);
        assert(prepared_rc == 0);
        const int entered_rc = dsd_scan_mode_enter(opts, state, mode);
        assert(entered_rc == 0);
        const int options_rc = dsd_scan_mode_options(opts, state, profile ? &profile->values : NULL);
        assert(options_rc == 0);
    }
    /* The cursor past the row, the row heard, the tuner the scan's: what the commit leaves. */
    state->lcn_freq_roll = 1;
    dsd_state_lcn_on_air_note(state, 0);
    state->operator_tuned = 0U;
    /* No step before the landing: the hangtime anchor lies ahead of the replay's clock until the landing sets it. */
    state->last_cc_sync_time = dsd_decode_time() + 86400 * 365;
    DSD_FPRINTF(stderr, "YHOLD row0 mode=%s\n", dsd_scan_mode_name(mode));
}

/* Row zero lands, on the replay's clock: what channel_scan_commit() notes, the hangtime and visit clocks with it. */
static void
land_row_zero(dsd_state* state) {
    const long freq = *dsd_state_trunk_lcn_slot(state, 0);
    dsd_engine_scan_landing_note_tune(state);
    dsd_engine_scan_landing_note_row(state, 0, freq, 0U);
    state->last_cc_sync_time = dsd_decode_time();
    state->last_cc_sync_time_m = dsd_decode_now_mono_s();
    dsd_scan_voice_gate_note_retune(state, state->last_cc_sync_time_m);
    g_landed = 1;
    DSD_FPRINTF(stderr, "YHOLD landed t=%.3f\n", state->last_cc_sync_time_m);
}

// NOLINTBEGIN(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,misc-use-internal-linkage)
int __real_getFrameSync(dsd_opts* opts, dsd_state* state);
int __wrap_getFrameSync(dsd_opts* opts, dsd_state* state);
int __real_dsd_engine_channel_scan_step(dsd_opts* opts, dsd_state* state);
int __wrap_dsd_engine_channel_scan_step(dsd_opts* opts, dsd_state* state);
int __real_dsd_engine_scan_untyped_find_row(const dsd_state* state, int from_roll, int skip_placeholders);
int __wrap_dsd_engine_scan_untyped_find_row(const dsd_state* state, int from_roll, int skip_placeholders);

int
__wrap_getFrameSync(dsd_opts* opts, dsd_state* state) {
    if (!g_landed) {
        land_row_zero(state);
    }
    const int synctype = __real_getFrameSync(opts, state);
    if (synctype > DSD_SYNC_NONE) {
        DSD_FPRINTF(stderr, "YHOLD sync t=%.3f proven=%u partial=%u anchor=%lld\n", dsd_decode_now_mono_s(),
                    state->scan_y_content_seq, state->scan_y_partial_seq, (long long)state->last_cc_sync_time);
    }
    return synctype;
}

/* A -Y step attempt, and whether a frame-boundary yield led to it: every yield writes a new (epoch, mark) pair. */
static void
note_step(const dsd_state* state) {
    const dsd_engine_scan_owner* owner =
        (const dsd_engine_scan_owner*)dsd_state_ext_get_const(state, DSD_STATE_EXT_ENGINE_TRUNK_SCAN);
    int via_frame = 0;
    if (owner
        && (owner->y_frame_yield_epoch != g_yield_epoch_seen || owner->y_frame_yield_mark_ms != g_yield_mark_seen)) {
        via_frame = 1;
        g_yield_epoch_seen = owner->y_frame_yield_epoch;
        g_yield_mark_seen = owner->y_frame_yield_mark_ms;
    }
    DSD_FPRINTF(stderr, "YHOLD step t=%.3f via=%s\n", dsd_decode_now_mono_s(), via_frame ? "frame" : "nocarrier");
}

/* A typed list's step. */
int
__wrap_dsd_engine_channel_scan_step(dsd_opts* opts, dsd_state* state) {
    note_step(state);
    return __real_dsd_engine_channel_scan_step(opts, state);
}

/* An untyped list's step looks for its next row first (no_carrier_step_scanner_mode_if_needed(), placeholders kept);
   the resume that skips them is no step. */
int
__wrap_dsd_engine_scan_untyped_find_row(const dsd_state* state, int from_roll, int skip_placeholders) {
    if (!skip_placeholders && g_landed) {
        note_step(state);
    }
    return __real_dsd_engine_scan_untyped_find_row(state, from_roll, skip_placeholders);
}

// NOLINTEND(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,misc-use-internal-linkage)

int
main(int argc, char** argv) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    assert(opts && state);
    initOpts(opts);
    initState(state);
    int rc = 1;
    if (dsd_runtime_bootstrap(argc, argv, opts, state, NULL, &rc) == DSD_BOOTSTRAP_CONTINUE) {
        assert(opts->scanner_mode == 1 && opts->trunk_scan_enabled == 0);
        assert(state->lcn_freq_count > 1);
        enter_row_zero(opts, state);
        rc = dsd_engine_run_with_lifecycle(opts, state, NULL);
    }
    dsd_scan_mode_leave(opts, state);
    freeState(state);
    free(state);
    free(opts);
    return rc;
}
