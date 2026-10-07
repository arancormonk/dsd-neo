// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The passband a rigctl peer that demodulates audio input is asked for (issue #621): whenever the peer demodulates the
 * analog monitor, the row's own width, else the configured width of the kind in force, else for AM its 6 kHz default
 * and for FM -B, else the peer's own passband. Everything else (a digital mode, a radio input, a symbol or null input)
 * follows with FM at -B, as every tune did before. The opts predicates that say whether a peer is there and demodulates
 * what DSD-neo hears.
 */

#include <assert.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/platform/sockets.h>
#include <dsd-neo/runtime/analog_channel.h>
#include <dsd-neo/runtime/rigctl_passband.h>
#include <dsd-neo/runtime/scan_mode.h>
#include <dsd-neo/runtime/scan_options.h>
#include <stdio.h>
#include <stdlib.h>

static void
expect_passband(const char* tag, dsd_rigctl_passband got, int kind, int bandwidth_hz, int source) {
    if (got.kind != kind || got.bandwidth_hz != bandwidth_hz || got.source != source) {
        DSD_FPRINTF(stderr, "%s: got kind=%d bw=%d source=%d, want kind=%d bw=%d source=%d\n", tag, got.kind,
                    got.bandwidth_hz, got.source, kind, bandwidth_hz, source);
        assert(0);
    }
}

/* A live rigctl peer on PCM input under the FM monitor, nothing configured. */
static dsd_opts*
fm_monitor_opts(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    assert(opts);
    opts->audio_in_type = AUDIO_IN_TCP;
    opts->use_rigctl = 1;
    opts->rigctl_sockfd = 7;
    opts->analog_only = 1;
    opts->monitor_input_audio = 1;
    opts->analog_demod = DSD_ANALOG_DEMOD_FM;
    return opts;
}

static dsd_scan_option_values
row_with_width(int kind, int hz) {
    dsd_scan_option_values row;
    DSD_MEMSET(&row, 0, sizeof row);
    row.present = DSD_SCAN_OPT_BANDWIDTH;
    row.channel_bw_kind = kind;
    row.channel_bw_hz = hz;
    return row;
}

static void
test_for_kind(void) {
    static const struct {
        int kind;
        int row_hz;
        int configured_hz;
        int setmod_bw_hz;
        int want_kind;
        int want_hz;
        int want_source;
    } cases[] = {
        /* FM: the row's own, else the configured width, else -B, else the peer's own. */
        {DSD_ANALOG_DEMOD_FM, 12500, 20000, 7000, DSD_ANALOG_DEMOD_FM, 12500, DSD_RIGCTL_PASSBAND_ROW},
        {DSD_ANALOG_DEMOD_FM, 0, 20000, 7000, DSD_ANALOG_DEMOD_FM, 20000, DSD_RIGCTL_PASSBAND_CONFIGURED},
        {DSD_ANALOG_DEMOD_FM, 0, 0, 7000, DSD_ANALOG_DEMOD_FM, 7000, DSD_RIGCTL_PASSBAND_SETMOD_BW},
        {DSD_ANALOG_DEMOD_FM, 0, 0, 0, DSD_ANALOG_DEMOD_FM, 0, DSD_RIGCTL_PASSBAND_PEER_OWN},
        /* AM: the row's own, else the configured width, else its 6 kHz default; -B never stands in for AM. */
        {DSD_ANALOG_DEMOD_AM, 8333, 10000, 7000, DSD_ANALOG_DEMOD_AM, 8333, DSD_RIGCTL_PASSBAND_ROW},
        {DSD_ANALOG_DEMOD_AM, 0, 10000, 7000, DSD_ANALOG_DEMOD_AM, 10000, DSD_RIGCTL_PASSBAND_CONFIGURED},
        {DSD_ANALOG_DEMOD_AM, 0, 0, 7000, DSD_ANALOG_DEMOD_AM, DSD_ANALOG_AM_WIDTH_DEFAULT_HZ,
         DSD_RIGCTL_PASSBAND_AM_DEFAULT},
        {DSD_ANALOG_DEMOD_AM, 0, 0, 0, DSD_ANALOG_DEMOD_AM, DSD_ANALOG_AM_WIDTH_DEFAULT_HZ,
         DSD_RIGCTL_PASSBAND_AM_DEFAULT},
    };

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        char tag[64];
        DSD_SNPRINTF(tag, sizeof tag, "for_kind case %zu", i);
        expect_passband(
            tag,
            dsd_rigctl_passband_for_kind(cases[i].kind, cases[i].row_hz, cases[i].configured_hz, cases[i].setmod_bw_hz),
            cases[i].want_kind, cases[i].want_hz, cases[i].want_source);
    }
}

/* The FM monitor on PCM input, the AM monitor too: the row's own width before the configured one, even where the two
   are the same (the in-force width is the row's while it is on air), and only a width of the kind in force. */
static void
test_of_on_the_analog_monitor(void) {
    dsd_opts* opts = fm_monitor_opts();
    opts->setmod_bw = 12500;

    const dsd_scan_option_values own = row_with_width(DSD_ANALOG_DEMOD_FM, 20000);
    opts->analog_nfm_bandwidth_hz = 20000;
    expect_passband("fm row own", dsd_rigctl_passband_of(opts, &own), DSD_ANALOG_DEMOD_FM, 20000,
                    DSD_RIGCTL_PASSBAND_ROW);
    /* Issue #621: the configured NFM width is the passband asked for where no row width of its own applies. */
    expect_passband("fm configured", dsd_rigctl_passband_of(opts, NULL), DSD_ANALOG_DEMOD_FM, 20000,
                    DSD_RIGCTL_PASSBAND_CONFIGURED);
    const dsd_scan_option_values am_own = row_with_width(DSD_ANALOG_DEMOD_AM, 8333);
    expect_passband("fm with an am width", dsd_rigctl_passband_of(opts, &am_own), DSD_ANALOG_DEMOD_FM, 20000,
                    DSD_RIGCTL_PASSBAND_CONFIGURED);
    dsd_scan_option_values no_width = own;
    no_width.present = 0;
    expect_passband("fm row without a width", dsd_rigctl_passband_of(opts, &no_width), DSD_ANALOG_DEMOD_FM, 20000,
                    DSD_RIGCTL_PASSBAND_CONFIGURED);
    opts->analog_nfm_bandwidth_hz = 0;
    expect_passband("fm -B", dsd_rigctl_passband_of(opts, NULL), DSD_ANALOG_DEMOD_FM, 12500,
                    DSD_RIGCTL_PASSBAND_SETMOD_BW);
    opts->setmod_bw = 0;
    expect_passband("fm peer's own", dsd_rigctl_passband_of(opts, NULL), DSD_ANALOG_DEMOD_FM, 0,
                    DSD_RIGCTL_PASSBAND_PEER_OWN);

    /* Every PCM input. */
    static const int pcm[] = {AUDIO_IN_PULSE, AUDIO_IN_STDIN, AUDIO_IN_WAV, AUDIO_IN_UDP, AUDIO_IN_TCP};
    opts->analog_nfm_bandwidth_hz = 12500;
    for (size_t i = 0; i < sizeof pcm / sizeof pcm[0]; i++) {
        opts->audio_in_type = (dsd_audio_in_type)pcm[i];
        expect_passband("fm configured on each pcm input", dsd_rigctl_passband_of(opts, NULL), DSD_ANALOG_DEMOD_FM,
                        12500, DSD_RIGCTL_PASSBAND_CONFIGURED);
    }

    /* The AM monitor: own, configured, default; -B and the NFM width play no part. */
    opts->analog_demod = DSD_ANALOG_DEMOD_AM;
    opts->setmod_bw = 7000;
    opts->analog_am_bandwidth_hz = 10000;
    expect_passband("am row own", dsd_rigctl_passband_of(opts, &am_own), DSD_ANALOG_DEMOD_AM, 8333,
                    DSD_RIGCTL_PASSBAND_ROW);
    expect_passband("am with an fm width", dsd_rigctl_passband_of(opts, &own), DSD_ANALOG_DEMOD_AM, 10000,
                    DSD_RIGCTL_PASSBAND_CONFIGURED);
    expect_passband("am configured", dsd_rigctl_passband_of(opts, NULL), DSD_ANALOG_DEMOD_AM, 10000,
                    DSD_RIGCTL_PASSBAND_CONFIGURED);
    opts->analog_am_bandwidth_hz = 0;
    expect_passband("am default", dsd_rigctl_passband_of(opts, NULL), DSD_ANALOG_DEMOD_AM,
                    DSD_ANALOG_AM_WIDTH_DEFAULT_HZ, DSD_RIGCTL_PASSBAND_AM_DEFAULT);
    free(opts);
}

/* No analog monitor the peer demodulates: FM at -B, as every tune asked before, whatever the widths say. Liveness is
   the caller's to check, so a session without rigctl reads the same. */
static void
test_of_follows_elsewhere(void) {
    dsd_opts* opts = fm_monitor_opts();
    opts->setmod_bw = 7000;
    opts->analog_nfm_bandwidth_hz = 20000;
    opts->analog_am_bandwidth_hz = 10000;
    const dsd_scan_option_values own = row_with_width(DSD_ANALOG_DEMOD_FM, 12500);

    /* A digital mode. */
    opts->analog_only = 0;
    opts->frame_dmr = 1;
    expect_passband("digital", dsd_rigctl_passband_of(opts, &own), DSD_ANALOG_DEMOD_FM, 7000,
                    DSD_RIGCTL_PASSBAND_FOLLOW);
    opts->frame_dmr = 0;
    opts->analog_only = 1;
    /* The M17 encoder shares the analog front end but is not the monitor. */
    opts->m17encoder = 1;
    expect_passband("m17 encoder", dsd_rigctl_passband_of(opts, NULL), DSD_ANALOG_DEMOD_FM, 7000,
                    DSD_RIGCTL_PASSBAND_FOLLOW);
    opts->m17encoder = 0;

    /* A radio input demodulates its own I/Q; symbol and null inputs carry no audio the peer demodulates. */
    static const int other[] = {AUDIO_IN_RTL, AUDIO_IN_SYMBOL_BIN, AUDIO_IN_SYMBOL_FLT, AUDIO_IN_NULL};
    for (int am = 0; am <= 1; am++) {
        opts->analog_demod = am ? DSD_ANALOG_DEMOD_AM : DSD_ANALOG_DEMOD_FM;
        for (size_t i = 0; i < sizeof other / sizeof other[0]; i++) {
            opts->audio_in_type = (dsd_audio_in_type)other[i];
            expect_passband("non-pcm input", dsd_rigctl_passband_of(opts, &own), DSD_ANALOG_DEMOD_FM, 7000,
                            DSD_RIGCTL_PASSBAND_FOLLOW);
        }
    }
    opts->audio_in_type = AUDIO_IN_WAV;
    opts->analog_demod = DSD_ANALOG_DEMOD_FM;
    opts->use_rigctl = 0;
    expect_passband("no rigctl", dsd_rigctl_passband_of(opts, NULL), DSD_ANALOG_DEMOD_FM, 20000,
                    DSD_RIGCTL_PASSBAND_CONFIGURED);
    expect_passband("null opts", dsd_rigctl_passband_of(NULL, &own), DSD_ANALOG_DEMOD_FM, 0,
                    DSD_RIGCTL_PASSBAND_FOLLOW);
    free(opts);
}

/* A width is a strict request; a width or -B on the monitor is a passband this client sets, sent through the capturing
   path; following and the peer's own are neither. Equality takes the source too. */
static void
test_classes_and_equality(void) {
    static const struct {
        int source;
        int is_width;
        int sets_passband;
    } cases[] = {
        {DSD_RIGCTL_PASSBAND_FOLLOW, 0, 0},     {DSD_RIGCTL_PASSBAND_ROW, 1, 1},
        {DSD_RIGCTL_PASSBAND_CONFIGURED, 1, 1}, {DSD_RIGCTL_PASSBAND_AM_DEFAULT, 1, 1},
        {DSD_RIGCTL_PASSBAND_SETMOD_BW, 0, 1},  {DSD_RIGCTL_PASSBAND_PEER_OWN, 0, 0},
    };

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        const dsd_rigctl_passband p = {DSD_ANALOG_DEMOD_FM, 12500, cases[i].source};
        assert(dsd_rigctl_passband_is_width(&p) == cases[i].is_width);
        assert(dsd_rigctl_passband_sets_passband(&p) == cases[i].sets_passband);
        assert(dsd_rigctl_passband_equal(&p, &p) == 1);
    }
    assert(dsd_rigctl_passband_is_width(NULL) == 0);
    assert(dsd_rigctl_passband_sets_passband(NULL) == 0);

    const dsd_rigctl_passband follow = {DSD_ANALOG_DEMOD_FM, 12500, DSD_RIGCTL_PASSBAND_FOLLOW};
    const dsd_rigctl_passband setmod = {DSD_ANALOG_DEMOD_FM, 12500, DSD_RIGCTL_PASSBAND_SETMOD_BW};
    const dsd_rigctl_passband configured = {DSD_ANALOG_DEMOD_FM, 12500, DSD_RIGCTL_PASSBAND_CONFIGURED};
    const dsd_rigctl_passband wider = {DSD_ANALOG_DEMOD_FM, 20000, DSD_RIGCTL_PASSBAND_CONFIGURED};
    const dsd_rigctl_passband am = {DSD_ANALOG_DEMOD_AM, 12500, DSD_RIGCTL_PASSBAND_CONFIGURED};
    /* The same wire request from another source is a change: entering the monitor from FOLLOW is one. */
    assert(dsd_rigctl_passband_equal(&follow, &setmod) == 0);
    assert(dsd_rigctl_passband_equal(&setmod, &configured) == 0);
    assert(dsd_rigctl_passband_equal(&configured, &wider) == 0);
    assert(dsd_rigctl_passband_equal(&configured, &am) == 0);
    assert(dsd_rigctl_passband_equal(NULL, &configured) == 0);
    assert(dsd_rigctl_passband_equal(&configured, NULL) == 0);
    assert(dsd_rigctl_passband_equal(NULL, NULL) == 1);
}

/* Issue #621: a request is captured (the peer's own passband read before it overwrites that passband, and the reading
   kept for the return) when it sets a passband, and also when it follows at -B on PCM audio input, where a digital
   mode is heard through the peer's FM passband. FOLLOW on any other input only follows the frequency; FOLLOW at 0 and
   the peer's own passband overwrite nothing. */
static void
test_captures(void) {
    dsd_opts* opts = fm_monitor_opts();
    static const int sets_passband[] = {DSD_RIGCTL_PASSBAND_ROW, DSD_RIGCTL_PASSBAND_CONFIGURED,
                                        DSD_RIGCTL_PASSBAND_AM_DEFAULT, DSD_RIGCTL_PASSBAND_SETMOD_BW};
    static const int inputs[] = {AUDIO_IN_PULSE, AUDIO_IN_STDIN, AUDIO_IN_WAV,        AUDIO_IN_UDP,       AUDIO_IN_TCP,
                                 AUDIO_IN_RTL,   AUDIO_IN_NULL,  AUDIO_IN_SYMBOL_BIN, AUDIO_IN_SYMBOL_FLT};
    for (size_t i = 0; i < sizeof inputs / sizeof inputs[0]; i++) {
        opts->audio_in_type = inputs[i];
        const int pcm = dsd_opts_input_is_pcm_audio(opts);
        for (size_t j = 0; j < sizeof sets_passband / sizeof sets_passband[0]; j++) {
            const dsd_rigctl_passband set = {DSD_ANALOG_DEMOD_FM, 12500, sets_passband[j]};
            assert(dsd_rigctl_passband_captures(opts, &set) == 1);
        }
        const dsd_rigctl_passband follow = {DSD_ANALOG_DEMOD_FM, 12500, DSD_RIGCTL_PASSBAND_FOLLOW};
        assert(dsd_rigctl_passband_captures(opts, &follow) == pcm);
        const dsd_rigctl_passband follow_own = {DSD_ANALOG_DEMOD_FM, 0, DSD_RIGCTL_PASSBAND_FOLLOW};
        assert(dsd_rigctl_passband_captures(opts, &follow_own) == 0);
        const dsd_rigctl_passband own = {DSD_ANALOG_DEMOD_FM, 0, DSD_RIGCTL_PASSBAND_PEER_OWN};
        assert(dsd_rigctl_passband_captures(opts, &own) == 0);
    }
    /* The rule's own request on a digital PCM session is -B, captured; with -B off it is the peer's own, not. */
    opts->audio_in_type = AUDIO_IN_TCP;
    opts->analog_only = 0;
    opts->monitor_input_audio = 0;
    opts->frame_dmr = 1;
    opts->setmod_bw = 20000;
    dsd_rigctl_passband digital = dsd_rigctl_passband_of(opts, NULL);
    assert(digital.source == DSD_RIGCTL_PASSBAND_FOLLOW && dsd_rigctl_passband_captures(opts, &digital) == 1);
    opts->setmod_bw = 0;
    digital = dsd_rigctl_passband_of(opts, NULL);
    assert(digital.source == DSD_RIGCTL_PASSBAND_FOLLOW && dsd_rigctl_passband_captures(opts, &digital) == 0);
    const dsd_rigctl_passband follow = {DSD_ANALOG_DEMOD_FM, 12500, DSD_RIGCTL_PASSBAND_FOLLOW};
    assert(dsd_rigctl_passband_captures(NULL, &follow) == 0);
    assert(dsd_rigctl_passband_captures(opts, NULL) == 0);
    free(opts);
}

static void
test_opts_predicates(void) {
    dsd_opts* opts = fm_monitor_opts();
    assert(dsd_opts_rigctl_live(opts) == 1);
    assert(dsd_opts_input_is_pcm_audio(opts) == 1);
    assert(dsd_opts_rigctl_peer_demodulates(opts) == 1);

    /* Socket 0 is a socket. */
    opts->rigctl_sockfd = 0;
    assert(dsd_opts_rigctl_live(opts) == 1 && dsd_opts_rigctl_peer_demodulates(opts) == 1);
    opts->rigctl_sockfd = DSD_INVALID_SOCKET;
    assert(dsd_opts_rigctl_live(opts) == 0 && dsd_opts_rigctl_peer_demodulates(opts) == 0);
    opts->rigctl_sockfd = 7;
    opts->use_rigctl = 0;
    assert(dsd_opts_rigctl_live(opts) == 0 && dsd_opts_rigctl_peer_demodulates(opts) == 0);
    opts->use_rigctl = 1;

    static const struct {
        int type;
        int pcm;
    } inputs[] = {
        {AUDIO_IN_PULSE, 1}, {AUDIO_IN_STDIN, 1},      {AUDIO_IN_WAV, 1},  {AUDIO_IN_UDP, 1},        {AUDIO_IN_TCP, 1},
        {AUDIO_IN_RTL, 0},   {AUDIO_IN_SYMBOL_BIN, 0}, {AUDIO_IN_NULL, 0}, {AUDIO_IN_SYMBOL_FLT, 0},
    };

    for (size_t i = 0; i < sizeof inputs / sizeof inputs[0]; i++) {
        opts->audio_in_type = (dsd_audio_in_type)inputs[i].type;
        assert(dsd_opts_input_is_pcm_audio(opts) == inputs[i].pcm);
        assert(dsd_opts_rigctl_peer_demodulates(opts) == inputs[i].pcm);
        assert(dsd_opts_rigctl_live(opts) == 1);
    }
    assert(dsd_opts_rigctl_live(NULL) == 0);
    assert(dsd_opts_input_is_pcm_audio(NULL) == 0);
    assert(dsd_opts_rigctl_peer_demodulates(NULL) == 0);
    free(opts);
}

/* Issue #621: whose a passband request is. A scan row's reading of the peer's own passband lasts the scan (its leave
   restore resets it); the session's is spent by a return to the peer's own. A trunk scan, or a scan scope held on the
   state (a typed -Y list's row), makes it a scan row's; the legacy untyped -Y list holds no scope and asks for the
   session's passband. Without a state, any configured -Y list counts as a scan. */
static void
test_request_is_session(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    assert(opts && state);
    assert(dsd_scan_mode_rigctl_request_is_session(NULL, state) == 0);
    assert(dsd_scan_mode_rigctl_request_is_session(opts, state) == 1);
    assert(dsd_scan_mode_rigctl_request_is_session(opts, NULL) == 1);
    /* The legacy -Y list: no scope, the session's; without a state it cannot be told from a typed one. */
    opts->scanner_mode = 1;
    assert(dsd_scan_mode_rigctl_request_is_session(opts, state) == 1);
    assert(dsd_scan_mode_rigctl_request_is_session(opts, NULL) == 0);
    /* A typed -Y row's scope. */
    assert(dsd_scan_mode_begin(opts, state) == 0);
    assert(dsd_scan_mode_rigctl_request_is_session(opts, state) == 0);
    dsd_scan_mode_leave(opts, state);
    assert(dsd_scan_mode_rigctl_request_is_session(opts, state) == 1);
    /* A trunk scan, scope or not. */
    opts->scanner_mode = 0;
    opts->trunk_scan_enabled = 1;
    assert(dsd_scan_mode_rigctl_request_is_session(opts, state) == 0);
    assert(dsd_scan_mode_rigctl_request_is_session(opts, NULL) == 0);
    dsd_state_ext_free_all(state);
    free(state);
    free(opts);
}

int
main(void) {
    test_for_kind();
    test_of_on_the_analog_monitor();
    test_of_follows_elsewhere();
    test_classes_and_equality();
    test_captures();
    test_opts_predicates();
    test_request_is_session();
    printf("RUNTIME_RIGCTL_PASSBAND: OK\n");
    return 0;
}
