// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Silent inputs and queued commands (issue #634). A frame-sync hunt read waits on a silent UDP input in slices; once
 * the silence lasts a stream pause (DSD_ANALOG_STREAM_PAUSE_MIN_MS) and a command is queued, it leaves the wait with
 * no sample (state->input_interrupted), so the engine applies the command. A shorter gap, frame decoding, or no queued
 * command keeps it waiting, as before. The M17 TCP reconnect gives up for a queued command in the hunt, between
 * attempts, and a lost TCP input is no socket (DSD_INVALID_SOCKET), never fd 0.
 */

#include <assert.h>
#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/dsp/analog_audio.h>
#include <dsd-neo/dsp/analog_rx.h>
#include <dsd-neo/dsp/symbol.h>
#include <dsd-neo/platform/sockets.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/runtime/control_pump.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/exitflag.h>
#include <dsd-neo/runtime/net_audio_input_hooks.h>
#include <dsd-neo/runtime/shutdown.h>
#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

static int g_cleanup_calls = 0;

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
openAudioInput(dsd_opts* opts) {
    (void)opts;
    return -1;
}

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_audio_reconfigure_output_for_input_policy(dsd_opts* opts) {
    (void)opts;
    return 0;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_request_shutdown(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    g_cleanup_calls++;
    exitflag = 1;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_audio_rescale_symbol_timing(dsd_state* state, int old_rate_hz, int new_rate_hz) {
    (void)state;
    (void)old_rate_hz;
    (void)new_rate_hz;
}

double
// NOLINTNEXTLINE(misc-use-internal-linkage)
pwr_to_dB(double mean_power) {
    (void)mean_power;
    return 0.0;
}

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_analog_audio_process_f(const dsd_opts* opts, dsd_state* state, dsd_analog_audio_chain chain, float* buf, size_t n,
                           dsd_analog_audio_source source, int rate_hz, unsigned int flags) {
    (void)opts;
    (void)state;
    (void)chain;
    (void)buf;
    (void)n;
    (void)source;
    (void)rate_hz;
    (void)flags;
    return 0;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_analog_audio_block_begin(const dsd_opts* opts, dsd_state* state, dsd_analog_audio_chain chain) {
    (void)opts;
    (void)state;
    (void)chain;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_analog_audio_note_reception(const dsd_state* state) {
    (void)state;
}

/* --- The controls-pending query and a scripted UDP input. --- */

static int g_pending = 0;
/* Timed-out waits before the input stops (0), or before a sample arrives (positive sample value set). */
static int g_silent_waits = 0;
static int g_stop_after_silence = 0;
static int16_t g_sample_after_silence = 0;
static int g_waits = 0;
static int g_spurious_first_wait = 0; /* the first wait comes back at once, empty, as a spurious wakeup can */

static int
fake_controls_pending(void) {
    return g_pending;
}

static int
fake_udp_read_sample_wait(dsd_opts* opts, int16_t* out, unsigned int timeout_ms) {
    (void)opts;
    g_waits++;
    if (g_waits <= g_silent_waits) {
        if (g_waits > 1 || !g_spurious_first_wait) {
            dsd_sleep_ms(timeout_ms);
        }
        return -1;
    }
    if (g_stop_after_silence) {
        return 0;
    }
    *out = g_sample_after_silence;
    return 1;
}

static void
init_udp_session(dsd_opts* opts, dsd_state* state, int silent_waits, int stop_after) {
    DSD_MEMSET(opts, 0, sizeof *opts);
    DSD_MEMSET(state, 0, sizeof *state);
    opts->audio_in_type = AUDIO_IN_UDP;
    opts->wav_sample_rate = 48000;
    opts->input_volume_multiplier = 1;
    state->samplesPerSymbol = 1;
    state->symbolCenter = 0;
    exitflag = 0;
    g_cleanup_calls = 0;
    g_waits = 0;
    g_silent_waits = silent_waits;
    g_stop_after_silence = stop_after;
    g_sample_after_silence = 1111;
    dsd_net_audio_input_hooks hooks;
    DSD_MEMSET(&hooks, 0, sizeof hooks);
    hooks.udp_read_sample_wait = fake_udp_read_sample_wait;
    dsd_net_audio_input_hooks_set(hooks);
}

/* Silence for a stream pause, a command queued: the hunt read leaves with no sample, after the pause and not before. */
static void
test_a_silent_hunt_read_leaves_for_a_queued_command(void) {
    static dsd_opts opts;
    static dsd_state state;
    init_udp_session(&opts, &state, 1000, 0);
    g_pending = 1;
    const uint64_t start_ms = dsd_realtime_mono_ms();
    assert(getSymbol(&opts, &state, 0) == 0.0f);
    const uint64_t waited_ms = dsd_realtime_mono_ms() - start_ms;
    assert(state.input_interrupted == 1);
    assert(g_cleanup_calls == 0 && exitflag == 0);
    assert(state.symbolcnt == 0U);
    assert(waited_ms >= DSD_ANALOG_STREAM_PAUSE_MIN_MS && waited_ms < 5000U);

    /* A first wait that came back at once (a spurious wakeup) is no silence: the pause is still whole. */
    init_udp_session(&opts, &state, 1000, 0);
    g_spurious_first_wait = 1;
    const uint64_t spurious_start_ms = dsd_realtime_mono_ms();
    assert(getSymbol(&opts, &state, 0) == 0.0f);
    const uint64_t spurious_waited_ms = dsd_realtime_mono_ms() - spurious_start_ms;
    assert(state.input_interrupted == 1);
    assert(spurious_waited_ms >= DSD_ANALOG_STREAM_PAUSE_MIN_MS && spurious_waited_ms < 5000U);
    g_spurious_first_wait = 0;
    g_pending = 0;
}

/* With no command queued the read waits on, as before: here until the input stops. */
static void
test_a_silent_read_with_nothing_queued_waits(void) {
    static dsd_opts opts;
    static dsd_state state;
    init_udp_session(&opts, &state, 8, 1);
    g_pending = 0;
    (void)getSymbol(&opts, &state, 0);
    assert(state.input_interrupted == 0);
    assert(g_waits == 9);
    assert(g_cleanup_calls == 1);
}

/* Frame decoding never leaves its read for a command: the frame waits for its samples. */
static void
test_frame_decoding_never_leaves_its_read(void) {
    static dsd_opts opts;
    static dsd_state state;
    init_udp_session(&opts, &state, 8, 0);
    g_pending = 1;
    (void)getSymbol(&opts, &state, 1);
    assert(state.input_interrupted == 0);
    assert(g_waits == 9);
    assert(g_cleanup_calls == 0);
    g_pending = 0;
}

/* A gap shorter than a stream pause costs nothing: the sample after it is read, with a command queued or not. */
static void
test_a_short_gap_never_leaves_the_read(void) {
    static dsd_opts opts;
    static dsd_state state;
    init_udp_session(&opts, &state, 2, 0);
    g_pending = 1;
    (void)getSymbol(&opts, &state, 0);
    assert(state.input_interrupted == 0);
    assert(g_waits == 3);
    assert(state.symbolcnt == 1U);
    assert(g_cleanup_calls == 0);
    g_pending = 0;
}

/* --- TCP: a reconnect the hunt gives up for a queued command; a lost input is no socket. --- */

static int g_tcp_connects = 0;
static dsd_socket_t g_tcp_connect_result = DSD_INVALID_SOCKET;
static int g_tcp_closes = 0;
static int g_tcp_ctx_token;
static int g_command_during_connect = 0; /* a command is queued while the connect waits */

static int
fake_tcp_read_sample(tcp_input_ctx* ctx, int16_t* out) {
    (void)ctx;
    (void)out;
    return 0; /* the stream ended */
}

static void
fake_tcp_close(tcp_input_ctx* ctx) {
    (void)ctx;
    g_tcp_closes++;
}

static dsd_socket_t
fake_tcp_connect(const char* host, int port, int resolve, dsd_socket_cancel_fn cancelled, void* context) {
    (void)host;
    (void)port;
    assert(resolve == 0); /* a reconnect looks nothing up */
    g_tcp_connects++;
    if (g_command_during_connect) {
        g_pending = 1;
    }
    if (cancelled && cancelled(context)) {
        return DSD_INVALID_SOCKET;
    }
    if (g_tcp_connects >= 3 && !g_pending) {
        exitflag = 1; /* end an M17 session's endless retry */
    }
    return g_tcp_connect_result;
}

static void
init_tcp_session(dsd_opts* opts, dsd_state* state, int m17) {
    DSD_MEMSET(opts, 0, sizeof *opts);
    DSD_MEMSET(state, 0, sizeof *state);
    opts->audio_in_type = AUDIO_IN_TCP;
    opts->audio_out_type = 0;
    opts->wav_sample_rate = 48000;
    opts->input_volume_multiplier = 1;
    opts->frame_m17 = m17;
    opts->tcp_sockfd = DSD_INVALID_SOCKET;
    opts->tcp_in_ctx = (tcp_input_ctx*)&g_tcp_ctx_token; /* the stream whose read ends */
    DSD_SNPRINTF(opts->tcp_hostname, sizeof opts->tcp_hostname, "%s", "audio.example");
    opts->tcp_portno = 7355;
    state->samplesPerSymbol = 1;
    state->symbolCenter = 0;
    exitflag = 0;
    g_cleanup_calls = 0;
    g_tcp_connects = 0;
    g_tcp_closes = 0;
    g_tcp_connect_result = DSD_INVALID_SOCKET;
    dsd_net_audio_input_hooks hooks;
    DSD_MEMSET(&hooks, 0, sizeof hooks);
    hooks.tcp_read_sample = fake_tcp_read_sample;
    hooks.tcp_close = fake_tcp_close;
    hooks.tcp_connect = fake_tcp_connect;
    dsd_net_audio_input_hooks_set(hooks);
}

/* An M17 session retries a lost TCP server without end; in the hunt, a queued command ends the retry for now, with
   the input still disconnected, and the next read tries again. */
static void
test_the_m17_tcp_retry_leaves_for_a_queued_command(void) {
    static dsd_opts opts;
    static dsd_state state;
    init_tcp_session(&opts, &state, 1);
    g_pending = 1;
    assert(getSymbol(&opts, &state, 0) == 0.0f);
    assert(state.input_interrupted == 1);
    assert(g_cleanup_calls == 0 && exitflag == 0);
    assert(opts.tcp_in_ctx == NULL && opts.tcp_sockfd == DSD_INVALID_SOCKET);
    assert(g_tcp_closes == 1); /* the ended stream, closed once */
    assert(state.input_fallback_pending == 0);
    g_pending = 0;

    /* With nothing queued it retries, here until shutdown. */
    init_tcp_session(&opts, &state, 1);
    (void)getSymbol(&opts, &state, 0);
    assert(g_tcp_connects >= 3);
    assert(state.input_interrupted == 0);
    assert(exitflag == 1);
}

/* A command queued while a hunt's reconnect waits on its connect cancels it: that is no lost server, so the session
   neither falls back to Pulse nor ends; the engine applies the command, and the next read tries again. */
static void
test_a_cancelled_reconnect_is_no_lost_input(void) {
    static dsd_opts opts;
    static dsd_state state;
    init_tcp_session(&opts, &state, 0);
    g_command_during_connect = 1;
    assert(getSymbol(&opts, &state, 0) == 0.0f);
    assert(g_tcp_connects == 1);
    assert(state.input_interrupted == 1);
    assert(state.input_fallback_pending == 0);
    assert(g_cleanup_calls == 0 && exitflag == 0);
    assert(opts.tcp_in_ctx == NULL && opts.tcp_sockfd == DSD_INVALID_SOCKET);
    g_command_during_connect = 0;
    g_pending = 0;
}

/* A TCP input whose reconnect fails ends: no socket left behind (DSD_INVALID_SOCKET, so shutdown never closes fd 0),
   and the switch to Pulse handed to the engine. */
static void
test_a_lost_tcp_input_is_no_socket(void) {
    static dsd_opts opts;
    static dsd_state state;
    init_tcp_session(&opts, &state, 0);
    (void)getSymbol(&opts, &state, 0);
    assert(g_tcp_connects == 1);
    assert(opts.tcp_sockfd == DSD_INVALID_SOCKET);
    assert(opts.tcp_in_ctx == NULL);
    assert(g_tcp_closes == 1);
    assert(state.input_fallback_pending == 1);
    assert(state.input_interrupted == 1);
    assert(g_cleanup_calls == 0);
}

int
main(void) {
    dsd_runtime_set_controls_pending(fake_controls_pending);
    test_a_silent_hunt_read_leaves_for_a_queued_command();
    test_a_silent_read_with_nothing_queued_waits();
    test_frame_decoding_never_leaves_its_read();
    test_a_short_gap_never_leaves_the_read();
    test_the_m17_tcp_retry_leaves_for_a_queued_command();
    test_a_cancelled_reconnect_is_no_lost_input();
    test_a_lost_tcp_input_is_no_socket();
    dsd_runtime_set_controls_pending(NULL);
    dsd_net_audio_input_hooks_set((dsd_net_audio_input_hooks){0});
    printf("DSP_INPUT_INTERRUPT: OK\n");
    return 0;
}
