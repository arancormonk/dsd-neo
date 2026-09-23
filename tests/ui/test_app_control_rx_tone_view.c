// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The received-tone text every frontend shows (issue #522): one phrase per detection state,
 * hidden whenever the analog FM monitor is not running, and the configured policy carried as
 * separate text that the received tone never feeds.
 */

#include <assert.h>
#include <dsd-neo/app_control/rx_tone_view.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define EM_DASH "\xE2\x80\x94"

static dsd_state*
make_state(void) {
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    assert(state != NULL);
    return state;
}

/* The analog FM monitor: analog-only decoding with the input monitored. */
static void
make_monitor_opts(dsd_opts* opts) {
    DSD_MEMSET(opts, 0, sizeof(*opts));
    opts->analog_only = 1;
    opts->monitor_input_audio = 1;
}

static void
publish(dsd_state* state, int carrier, int tone_state, int kind, int tenths) {
    DSD_MEMSET(&state->analog_rx, 0, sizeof(state->analog_rx));
    state->analog_rx.carrier_open = carrier;
    state->analog_rx.tone_state = tone_state;
    state->analog_rx.tone_kind = kind;
    state->analog_rx.ctcss_tenths_hz = tenths;
    state->analog_rx.generation = 7U;
}

static void
assert_view(const dsd_opts* opts, const dsd_state* state, int status, const char* text) {
    dsd_app_rx_tone view;
    const int rc = dsd_app_rx_tone_view(opts, state, &view);
    assert(rc == (status == DSD_APP_RX_TONE_HIDDEN ? 0 : 1));
    assert(view.status == (uint8_t)status);
    assert(view.visible == (status == DSD_APP_RX_TONE_HIDDEN ? 0U : 1U));
    assert(strcmp(view.text, text) == 0);
    /* Whatever was received, the configured policy text is its own: off until #527. */
    assert(strcmp(view.configured_text, "off") == 0);
}

static void
test_states_and_formats(void) {
    static dsd_opts opts;
    dsd_state* state = make_state();
    make_monitor_opts(&opts);

    /* Reset or not yet running: nothing heard, so the row carries an em dash. */
    publish(state, 0, DSD_ANALOG_TONE_STATE_INACTIVE, 0, 0);
    assert_view(&opts, state, DSD_APP_RX_TONE_NO_CARRIER, EM_DASH);
    publish(state, 0, DSD_ANALOG_TONE_STATE_IDLE, 0, 0);
    assert_view(&opts, state, DSD_APP_RX_TONE_NO_CARRIER, EM_DASH);

    publish(state, 1, DSD_ANALOG_TONE_STATE_ACQUIRING, 0, 0);
    assert_view(&opts, state, DSD_APP_RX_TONE_DETECTING, "detecting");

    publish(state, 1, DSD_ANALOG_TONE_STATE_LOCKED, DSD_ANALOG_TONE_KIND_CTCSS, 1000);
    assert_view(&opts, state, DSD_APP_RX_TONE_LOCKED, "CTCSS 100.0 Hz");
    dsd_app_rx_tone view;
    assert(dsd_app_rx_tone_view(&opts, state, &view) == 1);
    assert(view.kind == (uint8_t)DSD_ANALOG_TONE_KIND_CTCSS);
    assert(view.ctcss_tenths_hz == 1000);
    assert(view.carrier_open == 1U);
    assert(view.generation == 7U);

    /* Both ends of the table, one decimal, no padding. */
    publish(state, 1, DSD_ANALOG_TONE_STATE_LOCKED, DSD_ANALOG_TONE_KIND_CTCSS, 670);
    assert_view(&opts, state, DSD_APP_RX_TONE_LOCKED, "CTCSS 67.0 Hz");
    publish(state, 1, DSD_ANALOG_TONE_STATE_LOCKED, DSD_ANALOG_TONE_KIND_CTCSS, 2541);
    assert_view(&opts, state, DSD_APP_RX_TONE_LOCKED, "CTCSS 254.1 Hz");

    publish(state, 1, DSD_ANALOG_TONE_STATE_NONE, 0, 0);
    assert_view(&opts, state, DSD_APP_RX_TONE_NONE, "none");

    /* A publication this build cannot name is never shown as a value: 150.0 Hz is not a
       supported tone, and a DCS verdict has no text until #523. Both still read as a carrier
       under evaluation, not as a tone. */
    publish(state, 1, DSD_ANALOG_TONE_STATE_LOCKED, DSD_ANALOG_TONE_KIND_CTCSS, 1500);
    assert_view(&opts, state, DSD_APP_RX_TONE_DETECTING, "detecting");
    publish(state, 1, DSD_ANALOG_TONE_STATE_LOCKED, DSD_ANALOG_TONE_KIND_DCS, 0);
    assert_view(&opts, state, DSD_APP_RX_TONE_DETECTING, "detecting");
    /* A state from a newer decoder renders as nothing heard. */
    publish(state, 1, 99, 0, 0);
    assert_view(&opts, state, DSD_APP_RX_TONE_NO_CARRIER, EM_DASH);
    free(state);
}

/* An RTL stream whose output is CQPSK symbols rather than monitor audio. */
static int
fake_symbol_output_kind(void) {
    return 2;
}

static void
test_hidden_outside_the_fm_monitor(void) {
    static dsd_opts opts;
    dsd_state* state = make_state();
    make_monitor_opts(&opts);
    publish(state, 1, DSD_ANALOG_TONE_STATE_LOCKED, DSD_ANALOG_TONE_KIND_CTCSS, 1000);

    /* A digital mode, or analog without the monitor, runs no detection: a stale publication
       must not reach the screen. */
    opts.analog_only = 0;
    assert_view(&opts, state, DSD_APP_RX_TONE_HIDDEN, "");
    opts.analog_only = 1;
    opts.monitor_input_audio = 0;
    assert_view(&opts, state, DSD_APP_RX_TONE_HIDDEN, "");

    /* The FM monitor on input the tap never hears -- a symbol capture, or an RTL stream still
       outputting a digital family's samples -- is hidden too: the row is shown exactly while
       detection runs, not while the options merely ask for it. */
    opts.monitor_input_audio = 1;
    opts.audio_in_type = AUDIO_IN_SYMBOL_BIN;
    assert_view(&opts, state, DSD_APP_RX_TONE_HIDDEN, "");
    opts.audio_in_type = AUDIO_IN_RTL;
    const dsd_rtl_stream_metrics_hooks hooks = {.output_kind = fake_symbol_output_kind};
    dsd_rtl_stream_metrics_hooks_set(&hooks);
    assert_view(&opts, state, DSD_APP_RX_TONE_HIDDEN, "");
    dsd_rtl_stream_metrics_hooks_set(NULL);
    assert_view(&opts, state, DSD_APP_RX_TONE_LOCKED, "CTCSS 100.0 Hz");
    free(state);
}

static void
test_received_is_not_configured(void) {
    static dsd_opts opts;
    dsd_state* state = make_state();
    make_monitor_opts(&opts);
    dsd_app_rx_tone view;

    /* The policy gate field is reserved and always OFF; no value of it, or of the received
       tone, changes the configured text, and the configured text never changes the received
       one. */
    publish(state, 1, DSD_ANALOG_TONE_STATE_LOCKED, DSD_ANALOG_TONE_KIND_CTCSS, 1318);
    for (int gate = DSD_ANALOG_TONE_GATE_OFF; gate <= DSD_ANALOG_TONE_GATE_REJECTED; gate++) {
        state->analog_rx.gate = gate;
        assert(dsd_app_rx_tone_view(&opts, state, &view) == 1);
        assert(strcmp(view.text, "CTCSS 131.8 Hz") == 0);
        assert(strcmp(view.configured_text, "off") == 0);
    }
    free(state);
}

static void
test_invalid_arguments(void) {
    static dsd_opts opts;
    dsd_state* state = make_state();
    make_monitor_opts(&opts);
    dsd_app_rx_tone view;
    assert(dsd_app_rx_tone_view(NULL, state, &view) == -1);
    assert(view.visible == 0U && view.text[0] == '\0');
    assert(strcmp(view.configured_text, "off") == 0);
    assert(dsd_app_rx_tone_view(&opts, NULL, &view) == -1);
    assert(dsd_app_rx_tone_view(&opts, state, NULL) == -1);
    free(state);
}

int
main(void) {
    test_states_and_formats();
    test_hidden_outside_the_fm_monitor();
    test_received_is_not_configured();
    test_invalid_arguments();
    return 0;
}
