// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * A session that changes receive family at runtime needs the sink the new family
 * writes to. dsd_audio_ensure_analog_output() and dsd_audio_ensure_digital_output()
 * open what openAudioOutput() would have opened for the current options, with the
 * same parameters, and are idempotent: a sink that is already open is left alone,
 * and nothing is opened for a muted session or an output with no such sink.
 * With UDP output the analog monitor's sink is the socket on port + 2, which a
 * session started digital (without -8 or ProVoice) never opened.
 *
 * The device layer is replaced at link time with a recording null backend, and
 * the UDP analog socket is opened through a recording UDP audio hook.
 */

#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/platform/audio.h>
#include <dsd-neo/platform/sockets.h>
#include <dsd-neo/runtime/log.h>
#include <dsd-neo/runtime/udp_audio_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    int sample_rate;
    int channels;
    int bits_per_sample;
    int async_output;
} opened_params;

static uintptr_t g_streams[8];
static opened_params g_opened[8];
static int g_open_count;
static int g_close_count;
static int g_fail_opens;

// GNU ld --wrap requires these exact external symbol names.
// NOLINTBEGIN(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,misc-use-internal-linkage)
dsd_audio_stream* __wrap_dsd_audio_open_output(const dsd_audio_params* params);
void __wrap_dsd_audio_close(dsd_audio_stream* stream);

dsd_audio_stream*
__wrap_dsd_audio_open_output(const dsd_audio_params* params) {
    if (g_fail_opens) {
        return NULL;
    }
    const int index = g_open_count < 8 ? g_open_count : 7;
    g_opened[index].sample_rate = params ? params->sample_rate : 0;
    g_opened[index].channels = params ? params->channels : 0;
    g_opened[index].bits_per_sample = params ? params->bits_per_sample : 0;
    g_opened[index].async_output = params ? params->async_output : 0;
    g_open_count++;
    return (dsd_audio_stream*)&g_streams[index];
}

void
__wrap_dsd_audio_close(dsd_audio_stream* stream) {
    (void)stream;
    g_close_count++;
}

// NOLINTEND(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,misc-use-internal-linkage)

static int g_failures;

/* Error lines the ensure helpers logged, by sink. */
static int g_raw_error_logs;
static int g_digital_error_logs;
static int g_udp_error_logs;

/* Recording UDP analog connect: what the engine installs opens udp_sockfdA on port + 2. */
static int g_udp_connects;
static int g_udp_connect_fails;

static int
fake_udp_connect_analog(dsd_opts* opts) {
    g_udp_connects++;
    if (g_udp_connect_fails) {
        return -1;
    }
    opts->udp_sockfdA = (dsd_socket_t)42;
    return 0;
}

static void
count_error_logs(dsd_neo_log_level_t level, const char* text, void* ctx) {
    (void)ctx;
    if (level != LOG_LEVEL_ERROR || !text) {
        return;
    }
    if (strstr(text, "Failed to open raw audio output") != NULL) {
        g_raw_error_logs++;
    } else if (strstr(text, "Failed to open the UDP analog audio output on 127.0.0.1:23458") != NULL) {
        g_udp_error_logs++;
    } else if (strstr(text, "Failed to open audio output") != NULL) {
        g_digital_error_logs++;
    }
}

static void
expect_int(const char* label, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "FAIL: %s: got %d want %d\n", label, got, want);
        g_failures++;
    }
}

static void
reset_backend(void) {
    DSD_MEMSET(g_opened, 0, sizeof g_opened);
    g_open_count = 0;
    g_close_count = 0;
    g_fail_opens = 0;
}

static void
device_session(dsd_opts* opts) {
    DSD_MEMSET(opts, 0, sizeof(*opts));
    opts->audio_out = 1;
    opts->audio_out_type = 0;
    opts->pulse_raw_rate_out = 48000;
    opts->pulse_raw_out_channels = 1;
    opts->pulse_digi_rate_out = 8000;
    opts->pulse_digi_out_channels = 2;
    opts->udp_sockfd = DSD_INVALID_SOCKET;
    opts->udp_sockfdA = DSD_INVALID_SOCKET;
}

/* `-o udp:127.0.0.1:23456`, started digital: the digital socket is open, the analog one on port + 2 is not. */
static void
udp_session(dsd_opts* opts) {
    device_session(opts);
    opts->audio_out_type = 8;
    DSD_SNPRINTF(opts->udp_hostname, sizeof opts->udp_hostname, "%s", "127.0.0.1");
    opts->udp_portno = 23456;
    opts->udp_sockfd = (dsd_socket_t)41;
}

static void
test_analog_sink(void) {
    static dsd_opts opts;
    device_session(&opts);
    reset_backend();
    expect_int("analog ensure opens the raw sink", dsd_audio_ensure_analog_output(&opts), 0);
    expect_int("one open", g_open_count, 1);
    expect_int("raw sink kept", opts.audio_raw_out != NULL, 1);
    expect_int("raw rate", g_opened[0].sample_rate, 48000);
    expect_int("raw channels", g_opened[0].channels, 1);
    expect_int("raw sample width", g_opened[0].bits_per_sample, 16);
    expect_int("digital sink left closed", opts.audio_out_stream == NULL, 1);

    expect_int("second analog ensure", dsd_audio_ensure_analog_output(&opts), 0);
    expect_int("idempotent", g_open_count, 1);
    expect_int("nothing closed", g_close_count, 0);
}

static void
test_digital_sink(void) {
    static dsd_opts opts;
    device_session(&opts);
    reset_backend();
    expect_int("digital ensure", dsd_audio_ensure_digital_output(&opts), 0);
    expect_int("digital sink opened once", g_open_count, 1);
    expect_int("digital rate", g_opened[0].sample_rate, 8000);
    expect_int("digital channels", g_opened[0].channels, 2);
    expect_int("raw sink not needed", opts.audio_raw_out == NULL, 1);
    expect_int("second digital ensure", dsd_audio_ensure_digital_output(&opts), 0);
    expect_int("digital idempotent", g_open_count, 1);

    /* ProVoice and the -8 source monitor also write the raw sink, as openAudioOutput() opens it for them. */
    opts.frame_provoice = 1;
    expect_int("provoice ensure", dsd_audio_ensure_digital_output(&opts), 0);
    expect_int("provoice adds the raw sink", g_open_count, 2);
    expect_int("provoice raw rate", g_opened[1].sample_rate, 48000);
    expect_int("provoice idempotent", dsd_audio_ensure_digital_output(&opts), 0);
    expect_int("provoice opens once", g_open_count, 2);
}

static void
test_nothing_to_open(void) {
    static dsd_opts opts;
    device_session(&opts);
    reset_backend();
    opts.audio_out = 0; /* muted: unmuting reopens every sink the mode needs */
    expect_int("muted analog", dsd_audio_ensure_analog_output(&opts), 0);
    expect_int("muted digital", dsd_audio_ensure_digital_output(&opts), 0);
    opts.audio_out = 1;
    opts.audio_out_type = 9; /* -o null */
    expect_int("null analog", dsd_audio_ensure_analog_output(&opts), 0);
    opts.audio_out_type = 8; /* UDP */
    expect_int("udp digital", dsd_audio_ensure_digital_output(&opts), 0);
    expect_int("no device opened", g_open_count, 0);
    expect_int("null opts", dsd_audio_ensure_analog_output(NULL), -1);
    expect_int("null opts digital", dsd_audio_ensure_digital_output(NULL), -1);
}

static void
install_udp_hooks(void) {
    dsd_udp_audio_hooks hooks;
    DSD_MEMSET(&hooks, 0, sizeof hooks);
    hooks.connect_analog = fake_udp_connect_analog;
    dsd_udp_audio_hooks_set(hooks);
    g_udp_connects = 0;
    g_udp_connect_fails = 0;
    g_udp_error_logs = 0;
}

static void
test_udp_analog_sink(void) {
    static dsd_opts opts;
    install_udp_hooks();
    reset_backend();

    /* A digital session switched to Analog: the monitor audio goes to port + 2, so that socket opens. */
    udp_session(&opts);
    expect_int("udp digital ensure", dsd_audio_ensure_digital_output(&opts), 0);
    expect_int("digital udp needs no analog socket", g_udp_connects, 0);
    expect_int("udp analog ensure", dsd_audio_ensure_analog_output(&opts), 0);
    expect_int("analog socket opened once", g_udp_connects, 1);
    expect_int("analog socket kept", opts.udp_sockfdA == (dsd_socket_t)42, 1);
    expect_int("second udp analog ensure", dsd_audio_ensure_analog_output(&opts), 0);
    expect_int("udp analog idempotent", g_udp_connects, 1);
    expect_int("udp opens no audio device", g_open_count, 0);

    /* ProVoice and the -8 source monitor write the same socket on a digital udp session. */
    udp_session(&opts);
    opts.monitor_input_audio = 1;
    g_udp_connects = 0;
    expect_int("udp monitor digital ensure", dsd_audio_ensure_digital_output(&opts), 0);
    expect_int("monitor opens the analog socket", g_udp_connects, 1);
    expect_int("still no audio device", g_open_count, 0);

    /* Muted: unmuting reopens what the mode needs. */
    udp_session(&opts);
    opts.audio_out = 0;
    g_udp_connects = 0;
    expect_int("muted udp analog", dsd_audio_ensure_analog_output(&opts), 0);
    expect_int("muted udp opens nothing", g_udp_connects, 0);

    /* A socket that cannot open is reported once, stays invalid, and a success re-arms the message. */
    udp_session(&opts);
    g_udp_connect_fails = 1;
    expect_int("failed udp analog ensure", dsd_audio_ensure_analog_output(&opts), -1);
    expect_int("failed udp analog ensure again", dsd_audio_ensure_analog_output(&opts), -1);
    expect_int("failed socket stays invalid", opts.udp_sockfdA == DSD_INVALID_SOCKET, 1);
    expect_int("a failing udp socket is logged once", g_udp_error_logs, 1);
    g_udp_connect_fails = 0;
    expect_int("recovered udp analog ensure", dsd_audio_ensure_analog_output(&opts), 0);

    /* Without a UDP backend (no engine hooks) there is no socket to open. */
    dsd_udp_audio_hooks_set((dsd_udp_audio_hooks){0});
    udp_session(&opts);
    expect_int("no udp backend", dsd_audio_ensure_analog_output(&opts), -1);
    expect_int("no udp backend is reported", g_udp_error_logs, 2);
}

static void
test_open_failure(void) {
    static dsd_opts opts;
    device_session(&opts);
    reset_backend();
    g_raw_error_logs = 0;
    g_digital_error_logs = 0;
    g_fail_opens = 1;
    expect_int("failed analog ensure reports", dsd_audio_ensure_analog_output(&opts), -1);
    expect_int("failed ensure leaves no sink", opts.audio_raw_out == NULL, 1);
    expect_int("failed analog ensure again", dsd_audio_ensure_analog_output(&opts), -1);
    expect_int("failed analog ensure a third time", dsd_audio_ensure_analog_output(&opts), -1);
    expect_int("a failing raw sink is logged once", g_raw_error_logs, 1);
    expect_int("failed digital ensure reports", dsd_audio_ensure_digital_output(&opts), -1);
    expect_int("failed digital ensure again", dsd_audio_ensure_digital_output(&opts), -1);
    expect_int("a failing digital sink is logged once", g_digital_error_logs, 1);
    expect_int("the digital failure does not re-log the raw sink", g_raw_error_logs, 1);
    g_fail_opens = 0;
    expect_int("recovered analog ensure", dsd_audio_ensure_analog_output(&opts), 0);
    expect_int("recovered sink", opts.audio_raw_out != NULL, 1);

    /* A success re-arms the message: the next failure after it is news again. */
    opts.audio_raw_out = NULL; /* the sink went away (the recording backend has nothing to close) */
    g_fail_opens = 1;
    expect_int("failure after recovery reports", dsd_audio_ensure_analog_output(&opts), -1);
    expect_int("failure after recovery is logged afresh", g_raw_error_logs, 2);
    expect_int("and only once", dsd_audio_ensure_analog_output(&opts), -1);
    expect_int("still two raw messages", g_raw_error_logs, 2);
    g_fail_opens = 0;
}

int
main(void) {
    dsd_neo_log_set_tap(count_error_logs, NULL);
    test_analog_sink();
    test_digital_sink();
    test_nothing_to_open();
    test_udp_analog_sink();
    test_open_failure();
    if (g_failures) {
        DSD_FPRINTF(stderr, "CORE_AUDIO_ENSURE_OUTPUT: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("CORE_AUDIO_ENSURE_OUTPUT: OK\n");
    return 0;
}
