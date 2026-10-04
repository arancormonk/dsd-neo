// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The received-tone text every frontend shows (issues #522, #523): one phrase per detection
 * state, a CTCSS tone, or a DCS code under both standard spellings of its signal, the canonical
 * one first, hidden whenever detection is not running or cannot run at the input rate, and the
 * configured policy carried as separate text that the received tone never feeds.
 */

#include <assert.h>
#include <dsd-neo/app_control/rx_tone_view.h>
#include <dsd-neo/core/analog_tone.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/runtime/analog_tones.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#include <dsd-neo/runtime/scan_mode.h>
#include <dsd-neo/runtime/scan_options.h>
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
publish_dcs(dsd_state* state, int code, int inverted) {
    publish(state, 1, DSD_ANALOG_TONE_STATE_LOCKED, DSD_ANALOG_TONE_KIND_DCS, 0);
    state->analog_rx.dcs_code = code;
    state->analog_rx.dcs_inverted = inverted;
}

static void
assert_view(const dsd_opts* opts, const dsd_state* state, int status, const char* text) {
    dsd_app_rx_tone view;
    const int rc = dsd_app_rx_tone_view(opts, state, 0.0, &view);
    assert(rc == (status == DSD_APP_RX_TONE_HIDDEN ? 0 : 1));
    assert(view.status == (uint8_t)status);
    assert(view.visible == (status == DSD_APP_RX_TONE_HIDDEN ? 0U : 1U));
    assert(strcmp(view.text, text) == 0);
    /* Whatever was received, the configured policy text is its own: off here, where no policy is set. */
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
    assert(dsd_app_rx_tone_view(&opts, state, 0.0, &view) == 1);
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

    /* A publication this build cannot name is never shown as a value: 161.0 Hz is not a
       supported tone, and a DCS code must be a supported one under the canonical name of its
       alias class -- the detector never publishes 000, 340 (a rotation of 023) or D023I (the
       signal of D047N). Each still reads as a carrier under evaluation, not as a value. */
    publish(state, 1, DSD_ANALOG_TONE_STATE_LOCKED, DSD_ANALOG_TONE_KIND_CTCSS, 1610);
    assert_view(&opts, state, DSD_APP_RX_TONE_DETECTING, "detecting");
    publish(state, 1, DSD_ANALOG_TONE_STATE_LOCKED, DSD_ANALOG_TONE_KIND_DCS, 0);
    assert_view(&opts, state, DSD_APP_RX_TONE_DETECTING, "detecting");
    publish_dcs(state, 0340, 0);
    assert_view(&opts, state, DSD_APP_RX_TONE_DETECTING, "detecting");
    publish_dcs(state, 0023, 1);
    assert_view(&opts, state, DSD_APP_RX_TONE_DETECTING, "detecting");
    publish_dcs(state, 01000, 0);
    assert_view(&opts, state, DSD_APP_RX_TONE_DETECTING, "detecting");
    assert(dsd_app_rx_tone_view(&opts, state, 0.0, &view) == 1);
    assert(view.kind == 0U && view.dcs_code == 0 && view.dcs_inverted == 0U);
    assert(view.dcs_alias_code == 0 && view.dcs_alias_inverted == 0U);
    /* A received DCS code: both standard spellings of its signal, three octal digits with
       leading zeros and the polarity each, the published (canonical, normal) one first. A radio
       set to D023N and one set to D047I send the same signal, so neither is named alone. */
    publish_dcs(state, 0023, 0);
    assert_view(&opts, state, DSD_APP_RX_TONE_LOCKED, "DCS D023N / D047I");
    assert(dsd_app_rx_tone_view(&opts, state, 0.0, &view) == 1);
    assert(view.kind == (uint8_t)DSD_ANALOG_TONE_KIND_DCS);
    assert(view.dcs_code == 0023 && view.dcs_inverted == 0U && view.ctcss_tenths_hz == 0);
    assert(view.dcs_alias_code == 0047 && view.dcs_alias_inverted == 1U);
    /* The signal of D023I, which the detector publishes as D047N. */
    publish_dcs(state, 0047, 0);
    assert_view(&opts, state, DSD_APP_RX_TONE_LOCKED, "DCS D047N / D023I");
    assert(dsd_app_rx_tone_view(&opts, state, 0.0, &view) == 1);
    assert(view.dcs_code == 0047 && view.dcs_inverted == 0U);
    assert(view.dcs_alias_code == 0023 && view.dcs_alias_inverted == 1U);
    publish_dcs(state, 0754, 0);
    assert_view(&opts, state, DSD_APP_RX_TONE_LOCKED, "DCS D754N / D116I");
    /* A state from a newer decoder renders as nothing heard. */
    publish(state, 1, 99, 0, 0);
    assert_view(&opts, state, DSD_APP_RX_TONE_NO_CARRIER, EM_DASH);
    free(state);
}

/* An RTL stream whose output is CQPSK symbols rather than monitor audio. */
static int
fake_symbol_output_kind(void) {
    return RTL_STREAM_OUTPUT_SYMBOL_CQPSK;
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

    /* The FM monitor at an input rate the front end cannot use (a 384 kHz or sub-2400 Hz
       input): detection is on but hears nothing, so the row is left out rather than showing
       an em dash that would claim there is no carrier. */
    publish(state, 0, DSD_ANALOG_TONE_STATE_UNAVAILABLE, 0, 0);
    assert_view(&opts, state, DSD_APP_RX_TONE_HIDDEN, "");
    free(state);
}

static void
test_received_is_not_configured(void) {
    static dsd_opts opts;
    dsd_state* state = make_state();
    make_monitor_opts(&opts);
    dsd_app_rx_tone view;

    /* With no policy in force, no gate value and no received tone changes the configured
       text, and the configured text never changes the received one. */
    publish(state, 1, DSD_ANALOG_TONE_STATE_LOCKED, DSD_ANALOG_TONE_KIND_CTCSS, 1318);
    for (int gate = DSD_ANALOG_TONE_GATE_OFF; gate <= DSD_ANALOG_TONE_GATE_REJECTED; gate++) {
        state->analog_rx.gate = gate;
        assert(dsd_app_rx_tone_view(&opts, state, 0.0, &view) == 1);
        assert(strcmp(view.text, "CTCSS 131.8 Hz") == 0);
        assert(strcmp(view.configured_text, "off") == 0);
    }
    /* The same for a received code (issue #523). */
    publish_dcs(state, 0245, 0);
    for (int gate = DSD_ANALOG_TONE_GATE_OFF; gate <= DSD_ANALOG_TONE_GATE_REJECTED; gate++) {
        state->analog_rx.gate = gate;
        assert(dsd_app_rx_tone_view(&opts, state, 0.0, &view) == 1);
        assert(strcmp(view.text, "DCS D245N / D072I") == 0 && view.dcs_code == 0245 && view.dcs_alias_code == 0072);
        assert(strcmp(view.configured_text, "off") == 0);
    }
    free(state);
}

/*
 * A live stream input (stdin, UDP, TCP) whose producer stopped sending: the decoder waits for
 * the next sample and cannot retract what it last published, so the tap stamps a deadline and
 * the view, on the caller's clock, reads the publication past it as no carrier. Up to the
 * deadline, and with no deadline at all (files, Pulse, RTL), the publication stands.
 */
static void
test_paused_stream_reads_no_carrier(void) {
    static dsd_opts opts;
    dsd_state* state = make_state();
    make_monitor_opts(&opts);
    opts.audio_in_type = AUDIO_IN_UDP;
    dsd_app_rx_tone view;

    publish(state, 1, DSD_ANALOG_TONE_STATE_LOCKED, DSD_ANALOG_TONE_KIND_CTCSS, 1000);
    state->analog_rx.stale_after_ms = 5000U;
    /* Monotonic seconds: 4.999 s is before the 5000 ms deadline, 5.001 s after it. */
    assert(dsd_app_rx_tone_view(&opts, state, 4.999, &view) == 1);
    assert(view.status == DSD_APP_RX_TONE_LOCKED && strcmp(view.text, "CTCSS 100.0 Hz") == 0);
    assert(dsd_app_rx_tone_view(&opts, state, 5.001, &view) == 1);
    assert(view.visible == 1U && view.status == DSD_APP_RX_TONE_NO_CARRIER);
    assert(strcmp(view.text, EM_DASH) == 0);
    assert(view.carrier_open == 0U && view.kind == 0U && view.ctcss_tenths_hz == 0);
    assert(view.generation == 7U);
    assert(strcmp(view.configured_text, "off") == 0);

    /* So does a received code. */
    publish_dcs(state, 0023, 0);
    state->analog_rx.stale_after_ms = 5000U;
    assert(dsd_app_rx_tone_view(&opts, state, 4.999, &view) == 1);
    assert(view.status == DSD_APP_RX_TONE_LOCKED && strcmp(view.text, "DCS D023N / D047I") == 0);
    assert(dsd_app_rx_tone_view(&opts, state, 5.001, &view) == 1);
    assert(view.status == DSD_APP_RX_TONE_NO_CARRIER && strcmp(view.text, EM_DASH) == 0);
    assert(view.kind == 0U && view.dcs_code == 0 && view.dcs_inverted == 0U);
    assert(view.dcs_alias_code == 0 && view.dcs_alias_inverted == 0U);

    /* "detecting" and "none" describe a carrier too, and go stale the same way. */
    publish(state, 1, DSD_ANALOG_TONE_STATE_NONE, 0, 0);
    state->analog_rx.stale_after_ms = 5000U;
    assert(dsd_app_rx_tone_view(&opts, state, 6.0, &view) == 1);
    assert(view.status == DSD_APP_RX_TONE_NO_CARRIER);

    /* No deadline: an input that never pauses, however old the frame. */
    publish(state, 1, DSD_ANALOG_TONE_STATE_LOCKED, DSD_ANALOG_TONE_KIND_CTCSS, 1000);
    assert(dsd_app_rx_tone_view(&opts, state, 1.0e6, &view) == 1);
    assert(view.status == DSD_APP_RX_TONE_LOCKED);
    /* No clock (0): the caller asked for no aging. */
    state->analog_rx.stale_after_ms = 5000U;
    assert(dsd_app_rx_tone_view(&opts, state, 0.0, &view) == 1);
    assert(view.status == DSD_APP_RX_TONE_LOCKED);
    /* Hidden stays hidden: a stale publication never brings the row back. */
    opts.analog_only = 0;
    assert(dsd_app_rx_tone_view(&opts, state, 6.0, &view) == 0);
    assert(view.status == DSD_APP_RX_TONE_HIDDEN);
    free(state);
}

/* --- Issue #527: the tone policy beside the received tone --- */

static void
set_policy(dsd_opts* opts, int mode, const char* list) {
    opts->analog_tone_filter = mode;
    assert(dsd_tone_set_parse(list, &opts->analog_tone_set, NULL, 0) == 0);
}

/* The Tone filter row: the policy in force and, while a carrier is heard, what it does with it. The received row goes
   on saying only what was received. */
static void
test_tone_filter_policy_text(void) {
    static dsd_opts opts;
    dsd_state* state = make_state();
    make_monitor_opts(&opts);
    set_policy(&opts, DSD_TONE_FILTER_ALLOW, "D023N/100.0");
    dsd_app_rx_tone view;

    /* No carrier: the policy alone, no verdict. */
    publish(state, 0, DSD_ANALOG_TONE_STATE_IDLE, 0, 0);
    state->analog_rx.gate = DSD_ANALOG_TONE_GATE_PENDING;
    assert(dsd_app_rx_tone_view(&opts, state, 0.0, &view) == 1);
    assert(view.policy_visible == 1U && view.policy_row == 0U);
    assert(strcmp(view.configured_text, "allow 100.0 Hz/D023N") == 0);
    assert(view.gate == DSD_ANALOG_TONE_GATE_OFF && view.gate_text[0] == '\0');

    static const struct {
        int tone_state;
        int tenths;
        int gate;
        int no_tone;
        const char* received;
        const char* verdict;
    } cases[] = {
        {DSD_ANALOG_TONE_STATE_ACQUIRING, 0, DSD_ANALOG_TONE_GATE_PENDING, 0, "detecting", "muted: checking tone"},
        {DSD_ANALOG_TONE_STATE_LOCKED, 1000, DSD_ANALOG_TONE_GATE_ALLOWED, 0, "CTCSS 100.0 Hz", "passing"},
        {DSD_ANALOG_TONE_STATE_LOCKED, 670, DSD_ANALOG_TONE_GATE_REJECTED, 0, "CTCSS 67.0 Hz", "muted: not allowed"},
        {DSD_ANALOG_TONE_STATE_NONE, 0, DSD_ANALOG_TONE_GATE_REJECTED, 1, "none", "muted: no tone"},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        publish(state, 1, cases[i].tone_state, cases[i].tenths ? DSD_ANALOG_TONE_KIND_CTCSS : 0, cases[i].tenths);
        state->analog_rx.gate = cases[i].gate;
        state->analog_rx.gate_no_tone = cases[i].no_tone;
        assert(dsd_app_rx_tone_view(&opts, state, 0.0, &view) == 1);
        assert(strcmp(view.text, cases[i].received) == 0);
        assert(strcmp(view.configured_text, "allow 100.0 Hz/D023N") == 0);
        assert(view.gate == (uint8_t)cases[i].gate && strcmp(view.gate_text, cases[i].verdict) == 0);
        /* Frontends that word the verdict themselves (Qt) read why from the field, never from the English text. */
        assert(view.gate_no_tone == (uint8_t)cases[i].no_tone);
    }
    /* A check still running has decided nothing, for want of a tone or otherwise. */
    publish(state, 1, DSD_ANALOG_TONE_STATE_ACQUIRING, 0, 0);
    state->analog_rx.gate = DSD_ANALOG_TONE_GATE_PENDING;
    state->analog_rx.gate_no_tone = 1;
    assert(dsd_app_rx_tone_view(&opts, state, 0.0, &view) == 1);
    assert(view.gate_no_tone == 0U && strcmp(view.gate_text, "muted: checking tone") == 0);

    /* A block list's pass on no tone reads as passing too. */
    set_policy(&opts, DSD_TONE_FILTER_BLOCK, "67");
    publish(state, 1, DSD_ANALOG_TONE_STATE_NONE, 0, 0);
    state->analog_rx.gate = DSD_ANALOG_TONE_GATE_ALLOWED;
    state->analog_rx.gate_no_tone = 1;
    assert(dsd_app_rx_tone_view(&opts, state, 0.0, &view) == 1);
    assert(strcmp(view.configured_text, "block 67.0 Hz") == 0 && strcmp(view.gate_text, "passing") == 0);
    assert(view.gate_no_tone == 1U);

    /* A stale input reads as no carrier: the verdict goes with it. */
    opts.audio_in_type = AUDIO_IN_UDP;
    state->analog_rx.stale_after_ms = 5000U;
    assert(dsd_app_rx_tone_view(&opts, state, 6.0, &view) == 1);
    assert(view.gate_text[0] == '\0' && view.policy_visible == 1U);
    opts.audio_in_type = 0;

    /* An input rate detection cannot use hides the received row, not the policy that is muting it. */
    publish(state, 1, DSD_ANALOG_TONE_STATE_UNAVAILABLE, 0, 0);
    state->analog_rx.gate = DSD_ANALOG_TONE_GATE_ALLOWED;
    state->analog_rx.gate_no_tone = 1;
    assert(dsd_app_rx_tone_view(&opts, state, 0.0, &view) == 0);
    assert(view.policy_visible == 1U && strcmp(view.gate_text, "passing") == 0);

    /* No detection (a digital mode), no policy row, whatever is configured. */
    opts.analog_only = 0;
    assert(dsd_app_rx_tone_view(&opts, state, 0.0, &view) == 0);
    assert(view.policy_visible == 0U && view.gate_text[0] == '\0');
    assert(strcmp(view.configured_text, "block 67.0 Hz") == 0);
    opts.analog_only = 1;

    /* Policy off: the row stays away, the text says off, and a published verdict is not shown. */
    set_policy(&opts, DSD_TONE_FILTER_OFF, "67");
    publish(state, 1, DSD_ANALOG_TONE_STATE_NONE, 0, 0);
    state->analog_rx.gate = DSD_ANALOG_TONE_GATE_REJECTED;
    assert(dsd_app_rx_tone_view(&opts, state, 0.0, &view) == 1);
    assert(view.policy_visible == 0U && strcmp(view.configured_text, "off") == 0 && view.gate_text[0] == '\0');

    /* A long list keeps to the text and says how much it left out. */
    set_policy(&opts, DSD_TONE_FILTER_ALLOW,
               "67/69.3/71.9/74.4/77/79.7/82.5/85.4/88.5/91.5/94.8/97.4/100/103.5/D023/D025/D026/D031/D032");
    assert(dsd_app_rx_tone_view(&opts, state, 0.0, &view) == 1);
    assert(strncmp(view.configured_text, "allow 67.0 Hz/69.3 Hz/", 22) == 0);
    assert(strstr(view.configured_text, "\xE2\x80\xA6+") != NULL);
    free(state);
}

/* A scan row's own policy is what runs while the row is on air; the text says so and names the configured default it
   shadows, as every row override reads ("(row; default X)"), and a row that turns the policy off shows that too. */
static void
test_tone_filter_row_policy(void) {
    static dsd_opts opts;
    dsd_state* state = make_state();
    make_monitor_opts(&opts);
    opts.wav_sample_rate = 48000;
    opts.audio_in_type = AUDIO_IN_WAV;
    set_policy(&opts, DSD_TONE_FILTER_ALLOW, "100.0");
    assert(dsd_scan_mode_begin(&opts, state) == 0);
    assert(dsd_scan_mode_enter(&opts, state, DSD_SCAN_MODE_NFM) == 0);
    dsd_scan_option_values row = {0};
    row.present = DSD_SCAN_OPT_TONE;
    row.tone_filter = DSD_TONE_FILTER_BLOCK;
    assert(dsd_tone_set_parse("D023I", &row.tone_set, NULL, 0) == 0);
    assert(dsd_scan_mode_options(&opts, state, &row) == 0);
    dsd_app_rx_tone view;
    publish(state, 0, DSD_ANALOG_TONE_STATE_IDLE, 0, 0);
    assert(dsd_app_rx_tone_view(&opts, state, 0.0, &view) == 1);
    assert(view.policy_visible == 1U && view.policy_row == 1U);
    assert(strcmp(view.configured_text, "block D023I (row; default allow 100.0 Hz)") == 0);
    row.tone_filter = DSD_TONE_FILTER_OFF;
    DSD_MEMSET(&row.tone_set, 0, sizeof(row.tone_set));
    assert(dsd_scan_mode_options(&opts, state, &row) == 0);
    assert(dsd_app_rx_tone_view(&opts, state, 0.0, &view) == 1);
    assert(view.policy_visible == 1U && strcmp(view.configured_text, "off (row; default allow 100.0 Hz)") == 0);
    dsd_scan_mode_leave(&opts, state);
    assert(dsd_app_rx_tone_view(&opts, state, 0.0, &view) == 1);
    assert(view.policy_row == 0U && strcmp(view.configured_text, "allow 100.0 Hz") == 0);

    /* Long lists on both sides are each summarised, and the whole still fits: the default keeps its closing mark. */
    static const char k_long[] = "67/69.3/71.9/74.4/77/79.7/82.5/85.4/88.5/91.5/94.8/97.4/100/103.5/D023/D025/D026";
    set_policy(&opts, DSD_TONE_FILTER_BLOCK, k_long);
    assert(dsd_scan_mode_begin(&opts, state) == 0);
    assert(dsd_scan_mode_enter(&opts, state, DSD_SCAN_MODE_NFM) == 0);
    row.tone_filter = DSD_TONE_FILTER_ALLOW;
    assert(dsd_tone_set_parse(k_long, &row.tone_set, NULL, 0) == 0);
    assert(dsd_scan_mode_options(&opts, state, &row) == 0);
    assert(dsd_app_rx_tone_view(&opts, state, 0.0, &view) == 1);
    const char* shadowed = strstr(view.configured_text, " (row; default block 67.0 Hz/");
    assert(strncmp(view.configured_text, "allow 67.0 Hz/69.3 Hz/", 22) == 0 && shadowed != NULL);
    const char* first_mark = strstr(view.configured_text, "\xE2\x80\xA6+");
    assert(first_mark != NULL && first_mark < shadowed && strstr(shadowed, "\xE2\x80\xA6+") != NULL);
    const size_t length = strlen(view.configured_text);
    assert(length < sizeof(view.configured_text) - 1U && view.configured_text[length - 1U] == ')');
    dsd_scan_mode_leave(&opts, state);
    assert(dsd_app_rx_tone_view(&opts, state, 0.0, &view) == 1);
    assert(view.policy_row == 0U && strncmp(view.configured_text, "block 67.0 Hz/69.3 Hz/", 22) == 0);
    assert(strstr(view.configured_text, "(row") == NULL);
    dsd_state_ext_free_all(state);
    free(state);
}

/* The Tone filter row is where the live editor opens (Qt), so it may be on screen with no policy: whenever detection
   runs, since a policy then acts on what the monitor plays. */
static void
test_tone_filter_editable(void) {
    static dsd_opts opts;
    dsd_state* state = make_state();
    make_monitor_opts(&opts);
    dsd_app_rx_tone view;
    publish(state, 0, DSD_ANALOG_TONE_STATE_IDLE, 0, 0);
    assert(dsd_app_rx_tone_view(&opts, state, 0.0, &view) == 1);
    assert(view.policy_editable == 1U && view.policy_visible == 0U && strcmp(view.configured_text, "off") == 0);
    /* At a rate detection cannot use the received row goes, the editor stays. */
    publish(state, 1, DSD_ANALOG_TONE_STATE_UNAVAILABLE, 0, 0);
    assert(dsd_app_rx_tone_view(&opts, state, 0.0, &view) == 0);
    assert(view.policy_editable == 1U);
    /* The AM monitor and a digital mode run no detection: nothing to edit there. */
    opts.analog_demod = 1;
    assert(dsd_app_rx_tone_view(&opts, state, 0.0, &view) == 0);
    assert(view.policy_editable == 0U);
    opts.analog_demod = 0;
    opts.analog_only = 0;
    set_policy(&opts, DSD_TONE_FILTER_ALLOW, "100.0");
    assert(dsd_app_rx_tone_view(&opts, state, 0.0, &view) == 0);
    assert(view.policy_editable == 0U && view.policy_visible == 0U);
    assert(dsd_app_rx_tone_view(NULL, state, 0.0, &view) == -1 && view.policy_editable == 0U);
    free(state);
}

/* What the editors open on: the configured policy's mode and its list as the parser reads it back, spelled as written,
   and whether a scan row's own policy on air shadows an edit. Never the row's. */
static void
test_tone_filter_setting(void) {
    static dsd_opts opts;
    dsd_state* state = make_state();
    make_monitor_opts(&opts);
    opts.wav_sample_rate = 48000;
    opts.audio_in_type = AUDIO_IN_WAV;
    dsd_app_tone_filter_setting setting;
    assert(dsd_app_tone_filter_setting_get(&opts, state, &setting) == 0);
    assert(setting.mode == DSD_TONE_FILTER_OFF && setting.list[0] == '\0' && setting.row_override == 0U);
    set_policy(&opts, DSD_TONE_FILTER_BLOCK, "D023I/100");
    assert(dsd_app_tone_filter_setting_get(&opts, state, &setting) == 0);
    assert(setting.mode == DSD_TONE_FILTER_BLOCK && strcmp(setting.list, "100.0/D023I") == 0);
    /* Off keeps its list, and the editor opens on it. */
    opts.analog_tone_filter = DSD_TONE_FILTER_OFF;
    assert(dsd_app_tone_filter_setting_get(&opts, state, &setting) == 0);
    assert(setting.mode == DSD_TONE_FILTER_OFF && strcmp(setting.list, "100.0/D023I") == 0);
    opts.analog_tone_filter = DSD_TONE_FILTER_BLOCK;

    assert(dsd_scan_mode_begin(&opts, state) == 0);
    assert(dsd_scan_mode_enter(&opts, state, DSD_SCAN_MODE_NFM) == 0);
    dsd_scan_option_values row = {0};
    row.present = DSD_SCAN_OPT_TONE;
    row.tone_filter = DSD_TONE_FILTER_ALLOW;
    assert(dsd_tone_set_parse("67.0", &row.tone_set, NULL, 0) == 0);
    assert(dsd_scan_mode_options(&opts, state, &row) == 0);
    assert(dsd_app_tone_filter_setting_get(&opts, state, &setting) == 0);
    assert(setting.mode == DSD_TONE_FILTER_BLOCK && strcmp(setting.list, "100.0/D023I") == 0);
    assert(setting.row_override == 1U);
    /* A row without a policy of its own shadows nothing. */
    assert(dsd_scan_mode_options(&opts, state, NULL) == 0);
    assert(dsd_app_tone_filter_setting_get(&opts, state, &setting) == 0);
    assert(setting.row_override == 0U && setting.mode == DSD_TONE_FILTER_BLOCK);
    dsd_scan_mode_leave(&opts, state);

    /* Invalid arguments: -1, and a zeroed setting when there is one to zero. */
    DSD_MEMSET(&setting, 0x5A, sizeof(setting));
    assert(dsd_app_tone_filter_setting_get(NULL, state, &setting) == -1);
    assert(setting.mode == DSD_TONE_FILTER_OFF && setting.list[0] == '\0' && setting.row_override == 0U);
    assert(dsd_app_tone_filter_setting_get(&opts, state, NULL) == -1);
    dsd_state_ext_free_all(state);
    free(state);
}

/* @p text is all ASCII, with exactly @p marks "...+N" summaries of a long list. */
static void
assert_ascii_with_marks(const char* text, int marks) {
    for (const char* p = text; *p; p++) {
        assert((unsigned char)*p < 0x80U);
    }
    int found = 0;
    for (const char* p = strstr(text, "...+"); p; p = strstr(p + 4, "...+")) {
        found++;
    }
    assert(found == marks);
}

/* The toast after a tone-filter edit: what the configured policy now is, and while a scan row's own policy is on air,
   that the row overrides it, naming the row's -- as the squelch and width edits say it. */
static void
test_tone_filter_edit_notice(void) {
    static dsd_opts opts;
    dsd_state* state = make_state();
    make_monitor_opts(&opts);
    opts.wav_sample_rate = 48000;
    opts.audio_in_type = AUDIO_IN_WAV;
    char notice[DSD_APP_TONE_FILTER_NOTICE_SIZE];
    set_policy(&opts, DSD_TONE_FILTER_ALLOW, "D023N/100");
    assert(dsd_app_tone_filter_edit_notice(&opts, state, notice, sizeof(notice)) == 0);
    assert(strcmp(notice, "Applied: Tone filter -> allow 100.0 Hz/D023N") == 0);
    opts.analog_tone_filter = DSD_TONE_FILTER_OFF;
    assert(dsd_app_tone_filter_edit_notice(&opts, state, notice, sizeof(notice)) == 0);
    assert(strcmp(notice, "Applied: Tone filter -> off") == 0);
    /* Wherever detection runs or not: the notice is about the setting. */
    opts.analog_only = 0;
    set_policy(&opts, DSD_TONE_FILTER_BLOCK, "67");
    assert(dsd_app_tone_filter_edit_notice(&opts, state, notice, sizeof(notice)) == 0);
    assert(strcmp(notice, "Applied: Tone filter -> block 67.0 Hz") == 0);
    opts.analog_only = 1;

    assert(dsd_scan_mode_begin(&opts, state) == 0);
    assert(dsd_scan_mode_enter(&opts, state, DSD_SCAN_MODE_NFM) == 0);
    dsd_scan_option_values row = {0};
    row.present = DSD_SCAN_OPT_TONE;
    row.tone_filter = DSD_TONE_FILTER_ALLOW;
    assert(dsd_tone_set_parse("D754N", &row.tone_set, NULL, 0) == 0);
    assert(dsd_scan_mode_options(&opts, state, &row) == 0);
    assert(dsd_app_tone_filter_edit_notice(&opts, state, notice, sizeof(notice)) == 0);
    assert(strcmp(notice, "Default tone filter -> block 67.0 Hz; this channel overrides it (allow D754N)") == 0);
    row.tone_filter = DSD_TONE_FILTER_OFF;
    DSD_MEMSET(&row.tone_set, 0, sizeof(row.tone_set));
    assert(dsd_scan_mode_options(&opts, state, &row) == 0);
    assert(dsd_app_tone_filter_edit_notice(&opts, state, notice, sizeof(notice)) == 0);
    assert(strcmp(notice, "Default tone filter -> block 67.0 Hz; this channel overrides it (off)") == 0);
    dsd_scan_mode_leave(&opts, state);

    /* Long lists on both sides still fit the toast whole, each summarised, the closing mark kept. */
    static const char k_long[] = "67/69.3/71.9/74.4/77/79.7/82.5/85.4/88.5/91.5/94.8/97.4/100/103.5/D023/D025/D026";
    set_policy(&opts, DSD_TONE_FILTER_BLOCK, k_long);
    assert(dsd_scan_mode_begin(&opts, state) == 0);
    assert(dsd_scan_mode_enter(&opts, state, DSD_SCAN_MODE_NFM) == 0);
    row.tone_filter = DSD_TONE_FILTER_ALLOW;
    assert(dsd_tone_set_parse(k_long, &row.tone_set, NULL, 0) == 0);
    assert(dsd_scan_mode_options(&opts, state, &row) == 0);
    assert(dsd_app_tone_filter_edit_notice(&opts, state, notice, sizeof(notice)) == 0);
    assert(strncmp(notice, "Default tone filter -> block 67.0 Hz/", 37) == 0);
    const size_t length = strlen(notice);
    assert(length < sizeof(notice) - 1U && notice[length - 1U] == ')');
    assert(strstr(notice, "; this channel overrides it (allow 67.0 Hz/") != NULL);
    /* A toast is ASCII, which the terminal's status line prints as is: each summary's mark is "...". */
    assert_ascii_with_marks(notice, 2);
    dsd_scan_mode_leave(&opts, state);
    /* Without a row, the one list summarised the same way. */
    assert(dsd_app_tone_filter_edit_notice(&opts, state, notice, sizeof(notice)) == 0);
    assert(strncmp(notice, "Applied: Tone filter -> block 67.0 Hz/", 38) == 0);
    assert_ascii_with_marks(notice, 1);

    /* Invalid arguments. */
    assert(dsd_app_tone_filter_edit_notice(NULL, state, notice, sizeof(notice)) == -1 && notice[0] == '\0');
    assert(dsd_app_tone_filter_edit_notice(&opts, state, NULL, sizeof(notice)) == -1);
    assert(dsd_app_tone_filter_edit_notice(&opts, state, notice, 0) == -1);
    dsd_state_ext_free_all(state);
    free(state);
}

static void
test_invalid_arguments(void) {
    static dsd_opts opts;
    dsd_state* state = make_state();
    make_monitor_opts(&opts);
    dsd_app_rx_tone view;
    assert(dsd_app_rx_tone_view(NULL, state, 0.0, &view) == -1);
    assert(view.visible == 0U && view.text[0] == '\0');
    assert(strcmp(view.configured_text, "off") == 0);
    assert(dsd_app_rx_tone_view(&opts, NULL, 0.0, &view) == -1);
    assert(strcmp(view.configured_text, "off") == 0 && view.policy_visible == 0U);
    assert(dsd_app_rx_tone_view(&opts, state, 0.0, NULL) == -1);
    /* No state: still the policy dsd_opts holds (a terminal menu row before the first snapshot), nothing else. */
    opts.analog_tone_filter = DSD_TONE_FILTER_ALLOW;
    assert(dsd_tone_set_parse("100.0/D023N", &opts.analog_tone_set, NULL, 0) == 0);
    assert(dsd_app_rx_tone_view(&opts, NULL, 0.0, &view) == -1);
    assert(strcmp(view.configured_text, "allow 100.0 Hz/D023N") == 0);
    assert(view.visible == 0U && view.policy_visible == 0U && view.policy_editable == 0U && view.policy_row == 0U);
    assert(view.gate_text[0] == '\0' && view.text[0] == '\0');
    free(state);
}

int
main(void) {
    test_states_and_formats();
    test_hidden_outside_the_fm_monitor();
    test_received_is_not_configured();
    test_paused_stream_reads_no_carrier();
    test_invalid_arguments();
    test_tone_filter_policy_text();
    test_tone_filter_row_policy();
    test_tone_filter_editable();
    test_tone_filter_setting();
    test_tone_filter_edit_notice();
    return 0;
}
