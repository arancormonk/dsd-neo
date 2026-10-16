// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * When a single-system trunked session may hunt its channel map for a control channel it has never decoded
 * (issue #703): trunking on outside -Y and --trunk-scan, off any granted voice channel, no input switch pending, a
 * live tuner the trunking hooks can move, a user channel map with a usable row, and no protocol proven yet.
 */

#include <assert.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/runtime/trunk_scan_hooks.h>
#include <stdlib.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

static dsd_opts* g_opts;
static dsd_state* g_state;

static int
eligible(void) {
    return dsd_state_trunk_cold_start_eligible(g_opts, g_state);
}

/* A session that may hunt: trunking on an RTL tuner with a two-row channel map. */
static void
baseline(void) {
    dsd_state_trunk_lcn_free(g_state);
    DSD_MEMSET(g_opts, 0, sizeof(*g_opts));
    DSD_MEMSET(g_state, 0, sizeof(*g_state));
    g_opts->trunk_enable = 1;
    g_opts->audio_in_type = AUDIO_IN_RTL;
    DSD_SNPRINTF(g_opts->audio_in_dev, sizeof(g_opts->audio_in_dev), "%s", "rtl:0:851.0125M");
    DSD_SNPRINTF(g_opts->chan_in_file, sizeof(g_opts->chan_in_file), "%s", "site.csv");
    g_state->trunk_lcn_freq[0] = 851012500L;
    g_state->trunk_lcn_freq[1] = 851537500L;
    g_state->lcn_freq_count = 2;
}

static void
test_session_modes(void) {
    baseline();
    assert(eligible());
    g_opts->trunk_enable = 0; // conventional decoding has no control channel to hunt
    assert(!eligible());
    baseline();
    g_opts->scanner_mode = 1; // -Y owns the rotation
    assert(!eligible());
    baseline();
    g_opts->trunk_scan_enabled = 1; // trunk scan seeds and hunts its own targets
    assert(!eligible());
    baseline();
    g_opts->trunk_is_tuned = 1; // on a voice channel a grant sent the receiver to
    assert(!eligible());
    baseline();
    g_state->input_boundary = 1; // an input switch is pending its carrier boundary
    assert(!eligible());
}

static void
test_tuner(void) {
    baseline();
    DSD_SNPRINTF(g_opts->audio_in_dev, sizeof(g_opts->audio_in_dev), "%s", "iqreplay:capture.cu8");
    assert(!eligible()); // an I/Q replay defers every retune
    baseline();
    g_opts->iq_replay_requested = 1;
    g_opts->iq_replay_active = 1;
    assert(eligible()); // flags a replay-to-live input switch left behind are not the input in force
    baseline();
    g_opts->audio_in_type = AUDIO_IN_TCP;
    DSD_SNPRINTF(g_opts->audio_in_dev, sizeof(g_opts->audio_in_dev), "%s", "tcp:127.0.0.1:7355");
    assert(!eligible()); // nothing the decoder can retune
    g_opts->use_rigctl = 1;
    assert(eligible()); // ... unless rigctl moves the radio
}

static void
test_owner(void) {
    baseline();
    g_state->trunk_recovery_protocol = DSD_TRUNK_RECOVERY_P25;
    assert(!eligible());
    g_state->trunk_recovery_protocol = DSD_TRUNK_RECOVERY_DMR;
    assert(!eligible());
}

static void
test_channel_map(void) {
    baseline();
    g_opts->chan_in_file[0] = '\0'; // no map, or one cleared live
    assert(!eligible());
    baseline();
    g_state->trunk_lcn_freq[0] = 0;
    g_state->trunk_lcn_freq[1] = 0;
    assert(!eligible()); // rows that all failed to parse
    baseline();
    g_state->lcn_freq_count = 0;
    assert(!eligible());
    baseline();
    g_state->trunk_lcn_freq[0] = 0;
    assert(eligible()); // one usable row is enough
    baseline();
    assert(dsd_state_trunk_lcn_reserve(g_state, 40U) == 0); // rows past the embedded array
    g_state->trunk_lcn_freq[0] = 0;
    g_state->trunk_lcn_freq[1] = 0;
    g_state->lcn_freq_count = 40;
    *dsd_state_trunk_lcn_slot(g_state, 39) = 852062500L;
    assert(eligible());
}

/* The NXDN hunt shares every session condition and also runs under a proven NXDN owner, to recover a lost control
   channel; the startup predicate stays owner-unknown only. */
static void
test_nxdn_hunt(void) {
    baseline();
    assert(dsd_state_trunk_nxdn_hunt_eligible(g_opts, g_state));
    g_state->trunk_recovery_protocol = DSD_TRUNK_RECOVERY_NXDN;
    assert(dsd_state_trunk_nxdn_hunt_eligible(g_opts, g_state));
    assert(!eligible());
    g_state->trunk_recovery_protocol = DSD_TRUNK_RECOVERY_P25;
    assert(!dsd_state_trunk_nxdn_hunt_eligible(g_opts, g_state));
    g_state->trunk_recovery_protocol = DSD_TRUNK_RECOVERY_DMR;
    assert(!dsd_state_trunk_nxdn_hunt_eligible(g_opts, g_state));
    baseline();
    g_state->trunk_recovery_protocol = DSD_TRUNK_RECOVERY_NXDN;
    g_opts->trunk_is_tuned = 1;
    assert(!dsd_state_trunk_nxdn_hunt_eligible(g_opts, g_state));
    g_opts->trunk_is_tuned = 0;
    g_opts->scanner_mode = 1;
    assert(!dsd_state_trunk_nxdn_hunt_eligible(g_opts, g_state));
    g_opts->scanner_mode = 0;
    g_opts->trunk_scan_enabled = 1;
    assert(!dsd_state_trunk_nxdn_hunt_eligible(g_opts, g_state));
    g_opts->trunk_scan_enabled = 0;
    g_state->input_boundary = 1;
    assert(!dsd_state_trunk_nxdn_hunt_eligible(g_opts, g_state));
    g_state->input_boundary = 0;
    g_opts->chan_in_file[0] = '\0';
    assert(!dsd_state_trunk_nxdn_hunt_eligible(g_opts, g_state));
    baseline();
    g_state->trunk_recovery_protocol = DSD_TRUNK_RECOVERY_NXDN;
    DSD_SNPRINTF(g_opts->audio_in_dev, sizeof(g_opts->audio_in_dev), "%s", "iqreplay:capture.cu8");
    assert(!dsd_state_trunk_nxdn_hunt_eligible(g_opts, g_state));
    baseline();
    g_opts->audio_in_type = AUDIO_IN_TCP;
    assert(!dsd_state_trunk_nxdn_hunt_eligible(g_opts, g_state));
    assert(!dsd_state_trunk_nxdn_hunt_eligible(NULL, g_state));
    assert(!dsd_state_trunk_nxdn_hunt_eligible(g_opts, NULL));
}

int
main(void) {
    g_opts = (dsd_opts*)calloc(1, sizeof(*g_opts));
    g_state = (dsd_state*)calloc(1, sizeof(*g_state));
    assert(g_opts && g_state);
    test_session_modes();
    test_tuner();
    test_owner();
    test_channel_map();
    test_nxdn_hunt();
    assert(!dsd_state_trunk_cold_start_eligible(NULL, g_state));
    assert(!dsd_state_trunk_cold_start_eligible(g_opts, NULL));
    dsd_state_trunk_lcn_free(g_state);
    free(g_state);
    free(g_opts);
    return 0;
}
