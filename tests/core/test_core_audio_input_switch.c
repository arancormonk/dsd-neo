// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The runtime input switch (issue #634): the new input opens before the old one closes, a switch that cannot open its
 * input changes nothing, and one that does names the input, resets the PCM stream and follows its rate. Files are real,
 * written into a temporary working directory; UDP and TCP run through fake network-audio hooks that record what the
 * switch asks of them.
 */

#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/audio_input_switch.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/platform.h>
#include <dsd-neo/platform/posix_compat.h>
#include <dsd-neo/platform/sockets.h>
#include <dsd-neo/runtime/input_failure.h>
#include <dsd-neo/runtime/net_audio_input_hooks.h>
#include <sndfile.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#if !DSD_PLATFORM_WIN_NATIVE
#include <sys/socket.h>
#include <sys/stat.h>
#endif

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "test_support.h"

static int
expect_true(const char* label, int cond) {
    if (!cond) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", label);
        return 1;
    }
    return 0;
}

static int
expect_int_eq(const char* label, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "FAIL: %s (got %d want %d)\n", label, got, want);
        return 1;
    }
    return 0;
}

static int
expect_str_eq(const char* label, const char* got, const char* want) {
    if (strcmp(got, want) != 0) {
        DSD_FPRINTF(stderr, "FAIL: %s (got \"%s\" want \"%s\")\n", label, got, want);
        return 1;
    }
    return 0;
}

/* --- Fake network-audio hooks: what the switch asks of the UDP and TCP backends. --- */

static int g_udp_ctx_token;
static int g_tcp_ctx_tokens[4];
static char g_udp_log[256];
static int g_udp_fail_port = -1;
static int g_udp_fail_all;
static int g_udp_last_rate;
static int g_tcp_open_fail;
static int g_tcp_opens;
static int g_tcp_closes;

static void
udp_log(const char* entry) {
    const size_t used = strlen(g_udp_log);
    DSD_SNPRINTF(g_udp_log + used, sizeof g_udp_log - used, "%s%s", used ? " " : "", entry);
}

static int
fake_udp_start(dsd_opts* opts, const char* bindaddr, int port, int samplerate) {
    char entry[64];
    DSD_SNPRINTF(entry, sizeof entry, "start:%s:%d", bindaddr, port);
    udp_log(entry);
    g_udp_last_rate = samplerate;
    if (opts->udp_in_ctx || g_udp_fail_all || port == g_udp_fail_port) {
        return -1;
    }
    opts->udp_in_ctx = &g_udp_ctx_token;
    /* As udp_input_start() does: a new stream. */
    dsd_opts_note_pcm_stream(opts);
    return 0;
}

static void
fake_udp_stop(dsd_opts* opts) {
    udp_log("stop");
    opts->udp_in_ctx = NULL;
}

static tcp_input_ctx*
fake_tcp_open(dsd_socket_t sockfd, int samplerate) {
    (void)sockfd;
    (void)samplerate;
    if (g_tcp_open_fail) {
        return NULL;
    }
    g_tcp_opens++;
    return (tcp_input_ctx*)&g_tcp_ctx_tokens[g_tcp_opens % 4];
}

static void
fake_tcp_close(tcp_input_ctx* ctx) {
    (void)ctx;
    g_tcp_closes++;
}

static void
install_fake_hooks(void) {
    dsd_net_audio_input_hooks hooks;
    DSD_MEMSET(&hooks, 0, sizeof hooks);
    hooks.udp_start = fake_udp_start;
    hooks.udp_stop = fake_udp_stop;
    hooks.tcp_open = fake_tcp_open;
    hooks.tcp_close = fake_tcp_close;
    dsd_net_audio_input_hooks_set(hooks);
    g_udp_log[0] = '\0';
    g_udp_fail_port = -1;
    g_udp_fail_all = 0;
    g_udp_last_rate = 0;
    g_tcp_open_fail = 0;
    g_tcp_opens = 0;
    g_tcp_closes = 0;
}

/* --- Files and session. --- */

static int
write_wav(const char* path, int rate_hz, int samples) {
    SF_INFO info;
    DSD_MEMSET(&info, 0, sizeof info);
    info.samplerate = rate_hz;
    info.channels = 1;
    info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
    SNDFILE* f = sf_open(path, SFM_WRITE, &info);
    if (!f) {
        return 1;
    }
    short buf[64] = {0};
    const int ok = samples <= 64 && sf_write_short(f, buf, samples) == samples;
    sf_close(f);
    return ok ? 0 : 1;
}

static int
write_bytes(const char* path, size_t n) {
    FILE* f = dsd_fopen_private(path, "wb");
    if (!f) {
        return 1;
    }
    unsigned char zero[64] = {0};
    const int ok = n <= sizeof zero && fwrite(zero, 1U, n, f) == n;
    return (fclose(f) == 0 && ok) ? 0 : 1;
}

/* A session on Pulse input at 48 kHz, 10 samples a symbol, with no stream open. */
static void
session_init(dsd_opts* opts, dsd_state* state) {
    DSD_MEMSET(opts, 0, sizeof *opts);
    DSD_MEMSET(state, 0, sizeof *state);
    opts->audio_in_type = AUDIO_IN_PULSE;
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "pulse");
    opts->wav_sample_rate = 48000;
    opts->wav_decimator = 48000;
    opts->wav_interpolator = 1;
    opts->pulse_digi_rate_in = 48000;
    opts->tcp_sockfd = DSD_INVALID_SOCKET;
    opts->udp_in_sockfd = DSD_INVALID_SOCKET;
    state->samplesPerSymbol = 10;
    state->symbolCenter = 4;
    state->sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
}

static int
switch_to(dsd_opts* opts, dsd_state* state, dsd_audio_input_kind kind, const char* path) {
    dsd_audio_input_request req;
    DSD_MEMSET(&req, 0, sizeof req);
    req.kind = kind;
    req.path = path;
    req.tcp_sockfd = DSD_INVALID_SOCKET;
    return dsd_audio_switch_input(opts, state, &req);
}

static int
switch_to_udp(dsd_opts* opts, dsd_state* state, const char* bindaddr, int port) {
    dsd_audio_input_request req;
    DSD_MEMSET(&req, 0, sizeof req);
    req.kind = DSD_AUDIO_INPUT_UDP;
    req.host = bindaddr;
    req.port = port;
    req.tcp_sockfd = DSD_INVALID_SOCKET;
    return dsd_audio_switch_input(opts, state, &req);
}

static int
switch_to_tcp(dsd_opts* opts, dsd_state* state, const char* host, int port, dsd_socket_t sockfd) {
    dsd_audio_input_request req;
    DSD_MEMSET(&req, 0, sizeof req);
    req.kind = DSD_AUDIO_INPUT_TCP;
    req.host = host;
    req.port = port;
    req.tcp_sockfd = sockfd;
    return dsd_audio_switch_input(opts, state, &req);
}

static dsd_opts g_opts;
static dsd_state g_state;

/* The issue's switch: to a WAV, then to another at a higher rate, which its header sets. */
static int
test_wav_switch_replaces_the_file_and_rescales_timing(void) {
    int rc = 0;
    session_init(&g_opts, &g_state);
    rc |= expect_int_eq("to a wav", switch_to(&g_opts, &g_state, DSD_AUDIO_INPUT_PCM_FILE, "a.wav"),
                        DSD_AUDIO_INPUT_SWITCHED);
    rc |= expect_int_eq("wav input type", g_opts.audio_in_type, AUDIO_IN_WAV);
    rc |= expect_true("wav file open", g_opts.audio_in_file != NULL && g_opts.audio_in_file_info != NULL);
    rc |= expect_str_eq("wav named", g_opts.audio_in_dev, "a.wav");
    rc |= expect_int_eq("48 kHz wav keeps the rate", g_opts.wav_sample_rate, 48000);
    rc |= expect_int_eq("48 kHz wav keeps the timing", g_state.samplesPerSymbol, 10);
    rc |= expect_int_eq("no header rate replaced", g_opts.wav_header_replaced_rate, 0);

    SNDFILE* const first = g_opts.audio_in_file;
    const uint32_t stream = g_opts.pcm_input_generation;
    rc |= expect_int_eq("to a 96 kHz wav", switch_to(&g_opts, &g_state, DSD_AUDIO_INPUT_PCM_FILE, "b.wav"),
                        DSD_AUDIO_INPUT_SWITCHED);
    rc |= expect_true("another file", g_opts.audio_in_file != NULL && g_opts.audio_in_file != first);
    rc |= expect_str_eq("second wav named", g_opts.audio_in_dev, "b.wav");
    rc |= expect_int_eq("header rate", g_opts.wav_sample_rate, 96000);
    rc |= expect_int_eq("raw rate kept aside", g_opts.wav_header_replaced_rate, 48000);
    rc |= expect_int_eq("timing follows the header rate", g_state.samplesPerSymbol, 20);
    rc |= expect_true("a new stream", g_opts.pcm_input_generation != stream);
    return rc;
}

/* A switch that cannot open its input leaves everything as it was, the failure latch included. */
static int
test_failed_open_changes_nothing(void) {
    int rc = 0;
    SNDFILE* const file = g_opts.audio_in_file;
    SF_INFO* const info = g_opts.audio_in_file_info;
    const uint32_t stream = g_opts.pcm_input_generation;
    const unsigned int latch = dsd_input_failure_generation();
    static const char* const missing[] = {"missing.wav", "missing.bin", "missing.sym", "subdir"};
    static const dsd_audio_input_kind kinds[] = {DSD_AUDIO_INPUT_PCM_FILE, DSD_AUDIO_INPUT_SYMBOL_BIN,
                                                 DSD_AUDIO_INPUT_SYMBOL_FLT, DSD_AUDIO_INPUT_PCM_FILE};
    for (size_t i = 0; i < sizeof kinds / sizeof kinds[0]; i++) {
        rc |= expect_int_eq(missing[i], switch_to(&g_opts, &g_state, kinds[i], missing[i]), DSD_AUDIO_INPUT_KEPT);
        rc |= expect_true("the file stays", g_opts.audio_in_file == file && g_opts.audio_in_file_info == info);
        rc |= expect_true("no symbol file", g_opts.symbolfile == NULL);
        rc |= expect_int_eq("the type stays", g_opts.audio_in_type, AUDIO_IN_WAV);
        rc |= expect_str_eq("the name stays", g_opts.audio_in_dev, "b.wav");
        rc |= expect_int_eq("the rate stays", g_opts.wav_sample_rate, 96000);
        rc |= expect_int_eq("the raw rate stays aside", g_opts.wav_header_replaced_rate, 48000);
        rc |= expect_int_eq("the timing stays", g_state.samplesPerSymbol, 20);
        rc |= expect_true("no new stream", g_opts.pcm_input_generation == stream);
        rc |= expect_true("the failure latch is not written", dsd_input_failure_generation() == latch);
    }
#if !DSD_PLATFORM_WIN_NATIVE
    /* A named pipe would block the decoder thread in its open until a writer turns up: refused at runtime. */
    if (mkfifo("pipe.wav", 0600) == 0) {
        rc |= expect_int_eq("a named pipe is refused",
                            switch_to(&g_opts, &g_state, DSD_AUDIO_INPUT_PCM_FILE, "pipe.wav"), DSD_AUDIO_INPUT_KEPT);
        rc |= expect_true("the file stays after a pipe", g_opts.audio_in_file == file);
        (void)remove("pipe.wav");
    } else {
        rc |= expect_true("fifo created", 0);
    }
#endif
    return rc;
}

/* A header's rate holds for its file only: the next PCM input opens at the raw rate again. */
static int
test_leaving_a_header_rate_returns_to_the_raw_rate(void) {
    int rc = 0;
    rc |= expect_int_eq("to a headerless file", switch_to(&g_opts, &g_state, DSD_AUDIO_INPUT_PCM_FILE, "c.pcm"),
                        DSD_AUDIO_INPUT_SWITCHED);
    rc |= expect_int_eq("raw rate again", g_opts.wav_sample_rate, 48000);
    rc |= expect_int_eq("nothing kept aside", g_opts.wav_header_replaced_rate, 0);
    rc |= expect_int_eq("timing back at the raw rate", g_state.samplesPerSymbol, 10);

    rc |= expect_int_eq("back to the 96 kHz wav", switch_to(&g_opts, &g_state, DSD_AUDIO_INPUT_PCM_FILE, "b.wav"),
                        DSD_AUDIO_INPUT_SWITCHED);
    install_fake_hooks();
    rc |= expect_int_eq("to udp", switch_to_udp(&g_opts, &g_state, "127.0.0.1", 7000), DSD_AUDIO_INPUT_SWITCHED);
    rc |= expect_int_eq("udp starts at the raw rate", g_udp_last_rate, 48000);
    rc |= expect_int_eq("udp runs at the raw rate", g_opts.wav_sample_rate, 48000);
    rc |= expect_int_eq("udp timing at the raw rate", g_state.samplesPerSymbol, 10);
    rc |= expect_true("the wav closed", g_opts.audio_in_file == NULL && g_opts.audio_in_file_info == NULL);
    fake_udp_stop(&g_opts);

    /* An M17 .rrc file is 48 kHz whatever the raw rate, as -i opens it. */
    session_init(&g_opts, &g_state);
    g_opts.wav_sample_rate = 96000;
    g_opts.wav_interpolator = 2;
    rc |= expect_int_eq("a headerless file at a 96 kHz raw rate",
                        switch_to(&g_opts, &g_state, DSD_AUDIO_INPUT_PCM_FILE, "c.pcm"), DSD_AUDIO_INPUT_SWITCHED);
    rc |= expect_int_eq("timing from pulse's 48 kHz to the raw rate", g_state.samplesPerSymbol, 20);
    rc |= expect_int_eq("to an rrc file", switch_to(&g_opts, &g_state, DSD_AUDIO_INPUT_PCM_FILE, "d.rrc"),
                        DSD_AUDIO_INPUT_SWITCHED);
    rc |= expect_int_eq("rrc runs at 48 kHz", g_opts.wav_sample_rate, 48000);
    rc |= expect_int_eq("raw rate kept aside for the rrc", g_opts.wav_header_replaced_rate, 96000);
    rc |= expect_int_eq("rrc timing", g_state.samplesPerSymbol, 10);
    closeAudioInDevice(&g_opts);
    return rc;
}

/* A file rate a config staged while another input ran (staged_file_sample_rate) is the rate every later file opens at,
   as at startup: a WAV whose header overrides it does not use it up. */
static int
test_a_staged_file_rate_outlives_a_wav_header(void) {
    int rc = 0;
    session_init(&g_opts, &g_state);
    g_opts.staged_file_sample_rate = 96000;
    rc |= expect_int_eq("to a 48 kHz wav", switch_to(&g_opts, &g_state, DSD_AUDIO_INPUT_PCM_FILE, "a.wav"),
                        DSD_AUDIO_INPUT_SWITCHED);
    rc |= expect_int_eq("the header's rate runs", g_opts.wav_sample_rate, 48000);
    rc |= expect_int_eq("the staged file rate stays", g_opts.staged_file_sample_rate, 96000);
    rc |= expect_int_eq("to a headerless file", switch_to(&g_opts, &g_state, DSD_AUDIO_INPUT_PCM_FILE, "c.pcm"),
                        DSD_AUDIO_INPUT_SWITCHED);
    rc |= expect_int_eq("it opens at the staged file rate", g_opts.wav_sample_rate, 96000);
    rc |= expect_int_eq("timing at that rate", g_state.samplesPerSymbol, 20);
    closeAudioInDevice(&g_opts);
    return rc;
}

static int
test_symbol_inputs(void) {
    int rc = 0;
    session_init(&g_opts, &g_state);
    rc |= expect_int_eq("a wav first", switch_to(&g_opts, &g_state, DSD_AUDIO_INPUT_PCM_FILE, "a.wav"),
                        DSD_AUDIO_INPUT_SWITCHED);
    rc |= expect_int_eq("to a capture", switch_to(&g_opts, &g_state, DSD_AUDIO_INPUT_SYMBOL_BIN, "e.bin"),
                        DSD_AUDIO_INPUT_SWITCHED);
    rc |= expect_int_eq("capture type", g_opts.audio_in_type, AUDIO_IN_SYMBOL_BIN);
    rc |= expect_true("capture open, wav closed", g_opts.symbolfile != NULL && g_opts.audio_in_file == NULL);
    rc |= expect_int_eq("a capture replays at symbol pace", g_state.use_throttle, 1);
    rc |= expect_str_eq("capture named", g_opts.audio_in_dev, "e.bin");
    FILE* const capture = g_opts.symbolfile;
    rc |= expect_int_eq("to a symbol stream", switch_to(&g_opts, &g_state, DSD_AUDIO_INPUT_SYMBOL_FLT, "f.sym"),
                        DSD_AUDIO_INPUT_SWITCHED);
    rc |= expect_int_eq("symbol stream type", g_opts.audio_in_type, AUDIO_IN_SYMBOL_FLT);
    rc |= expect_true("another symbol file", g_opts.symbolfile != NULL && g_opts.symbolfile != capture);
    rc |= expect_int_eq("a symbol stream is not paced", g_state.use_throttle, 0);
    closeAudioInDevice(&g_opts);

    /* -i's rules, read from the last '.' of the whole path. */
    rc |= expect_int_eq("raw is a symbol stream", (int)dsd_audio_path_input_kind("x.raw"), DSD_AUDIO_INPUT_SYMBOL_FLT);
    rc |= expect_int_eq("sym is a symbol stream", (int)dsd_audio_path_input_kind("x.sym"), DSD_AUDIO_INPUT_SYMBOL_FLT);
    rc |= expect_int_eq("bin is a capture", (int)dsd_audio_path_input_kind("x.bin"), DSD_AUDIO_INPUT_SYMBOL_BIN);
    rc |= expect_int_eq("wav is pcm", (int)dsd_audio_path_input_kind("x.wav"), DSD_AUDIO_INPUT_PCM_FILE);
    rc |= expect_int_eq("no extension is pcm", (int)dsd_audio_path_input_kind("x"), DSD_AUDIO_INPUT_PCM_FILE);
    rc |= expect_int_eq("rrc is pcm", (int)dsd_audio_path_input_kind("x.rrc"), DSD_AUDIO_INPUT_PCM_FILE);
    rc |= expect_int_eq("as startup reads a dotted directory", (int)dsd_audio_path_input_kind("d.raw/x"),
                        DSD_AUDIO_INPUT_SYMBOL_FLT);
    return rc;
}

/* A switch to UDP stops the running UDP input first, since it may hold the port, and starts it again on its own
   endpoint when the new one does not bind: a new stream on the old input. */
static int
test_udp_switch_stops_old_first_and_restarts_it_on_failure(void) {
    int rc = 0;
    session_init(&g_opts, &g_state);
    install_fake_hooks();
    rc |= expect_int_eq("to udp", switch_to_udp(&g_opts, &g_state, NULL, 7000), DSD_AUDIO_INPUT_SWITCHED);
    rc |= expect_str_eq("udp named", g_opts.audio_in_dev, "udp:127.0.0.1:7000");
    rc |= expect_str_eq("udp bind", g_opts.udp_in_bindaddr, "127.0.0.1");
    rc |= expect_int_eq("udp port", g_opts.udp_in_portno, 7000);

    g_udp_log[0] = '\0';
    g_udp_fail_port = 7001;
    const uint32_t stream = g_opts.pcm_input_generation;
    rc |= expect_int_eq("a refused bind restarts the old udp", switch_to_udp(&g_opts, &g_state, "0.0.0.0", 7001),
                        DSD_AUDIO_INPUT_RESTARTED);
    rc |= expect_str_eq("stop first, then the new bind, then the old one again", g_udp_log,
                        "stop start:0.0.0.0:7001 start:127.0.0.1:7000");
    rc |= expect_true("the old udp runs", g_opts.udp_in_ctx != NULL && g_opts.audio_in_type == AUDIO_IN_UDP);
    rc |= expect_str_eq("the old udp keeps its name", g_opts.audio_in_dev, "udp:127.0.0.1:7000");
    rc |= expect_int_eq("the old udp keeps its port", g_opts.udp_in_portno, 7000);
    rc |= expect_true("the restart is a new stream", g_opts.pcm_input_generation != stream);

    g_udp_log[0] = '\0';
    g_udp_fail_all = 1;
    rc |= expect_int_eq("neither binds: lost", switch_to_udp(&g_opts, &g_state, "0.0.0.0", 7001), DSD_AUDIO_INPUT_LOST);
    rc |= expect_true("no udp input runs", g_opts.udp_in_ctx == NULL);
    g_udp_fail_all = 0;
    return rc;
}

/* A switch away from UDP stops it only once the new input opened. */
static int
test_switch_away_from_udp_stops_it_only_on_success(void) {
    int rc = 0;
    session_init(&g_opts, &g_state);
    install_fake_hooks();
    rc |= expect_int_eq("to udp", switch_to_udp(&g_opts, &g_state, "127.0.0.1", 7002), DSD_AUDIO_INPUT_SWITCHED);
    g_udp_log[0] = '\0';
    rc |= expect_int_eq("a missing file", switch_to(&g_opts, &g_state, DSD_AUDIO_INPUT_PCM_FILE, "missing.wav"),
                        DSD_AUDIO_INPUT_KEPT);
    rc |= expect_str_eq("udp untouched", g_udp_log, "");
    rc |= expect_true("udp runs on", g_opts.udp_in_ctx != NULL && g_opts.audio_in_type == AUDIO_IN_UDP);
    rc |= expect_int_eq("to a wav", switch_to(&g_opts, &g_state, DSD_AUDIO_INPUT_PCM_FILE, "a.wav"),
                        DSD_AUDIO_INPUT_SWITCHED);
    rc |= expect_str_eq("udp stopped once", g_udp_log, "stop");
    rc |= expect_true("no udp input", g_opts.udp_in_ctx == NULL);
    closeAudioInDevice(&g_opts);
    return rc;
}

/* A TCP switch takes the socket the caller connected; one whose input does not open leaves it to the caller. */
static int
test_tcp_switch_takes_the_socket(void) {
    int rc = 0;
    session_init(&g_opts, &g_state);
    install_fake_hooks();
    rc |= expect_int_eq("a wav first", switch_to(&g_opts, &g_state, DSD_AUDIO_INPUT_PCM_FILE, "a.wav"),
                        DSD_AUDIO_INPUT_SWITCHED);
    const dsd_socket_t first = dsd_socket_create(AF_INET, SOCK_STREAM, 0);
    rc |= expect_true("socket", first != DSD_INVALID_SOCKET);
    rc |= expect_int_eq("to tcp", switch_to_tcp(&g_opts, &g_state, "audio.example", 7355, first),
                        DSD_AUDIO_INPUT_SWITCHED);
    rc |= expect_int_eq("tcp type", g_opts.audio_in_type, AUDIO_IN_TCP);
    rc |= expect_true("tcp takes the socket", g_opts.tcp_sockfd == first && g_opts.tcp_in_ctx != NULL);
    rc |= expect_true("the wav closed", g_opts.audio_in_file == NULL);
    rc |= expect_str_eq("tcp named", g_opts.audio_in_dev, "tcp:audio.example:7355");
    rc |= expect_str_eq("tcp host", g_opts.tcp_hostname, "audio.example");
    rc |= expect_int_eq("tcp port", g_opts.tcp_portno, 7355);

    const dsd_socket_t second = dsd_socket_create(AF_INET, SOCK_STREAM, 0);
    g_tcp_open_fail = 1;
    rc |= expect_int_eq("a tcp input that does not open", switch_to_tcp(&g_opts, &g_state, "other.example", 1, second),
                        DSD_AUDIO_INPUT_KEPT);
    rc |= expect_true("the running tcp stays", g_opts.tcp_sockfd == first && g_opts.tcp_in_ctx != NULL);
    rc |= expect_int_eq("nothing closed", g_tcp_closes, 0);
    dsd_socket_close(second);

    g_tcp_open_fail = 0;
    const dsd_socket_t third = dsd_socket_create(AF_INET, SOCK_STREAM, 0);
    rc |= expect_int_eq("tcp to tcp", switch_to_tcp(&g_opts, &g_state, "third.example", 2, third),
                        DSD_AUDIO_INPUT_SWITCHED);
    rc |= expect_true("the new socket", g_opts.tcp_sockfd == third);
    rc |= expect_int_eq("the old context closed", g_tcp_closes, 1);
    closeAudioInDevice(&g_opts);
    return rc;
}

/* Leaving a radio input: its timing is in no PCM rate's units, so it is set afresh from the profile the hunt is on, and
   the radio spec is kept for the RTL-SDR row. */
static int
test_leaving_rtl_rederives_timing_and_keeps_the_spec(void) {
    int rc = 0;
    session_init(&g_opts, &g_state);
    g_opts.audio_in_type = AUDIO_IN_RTL;
    DSD_SNPRINTF(g_opts.audio_in_dev, sizeof g_opts.audio_in_dev, "%s", "rtl:0:851.0125M:22:0:24");
    g_state.samplesPerSymbol = 1; /* symbol-rate output */
    g_state.symbolCenter = 0;
    rc |= expect_int_eq("rtl to a wav", switch_to(&g_opts, &g_state, DSD_AUDIO_INPUT_PCM_FILE, "a.wav"),
                        DSD_AUDIO_INPUT_SWITCHED);
    rc |= expect_int_eq("timing set from the 4800 profile", g_state.samplesPerSymbol, 10);
    rc |= expect_str_eq("the radio spec kept", g_opts.radio_in_dev, "rtl:0:851.0125M:22:0:24");
    rc |= expect_int_eq("a pcm switch keeps the kept spec",
                        switch_to(&g_opts, &g_state, DSD_AUDIO_INPUT_PCM_FILE, "b.wav"), DSD_AUDIO_INPUT_SWITCHED);
    rc |= expect_str_eq("the radio spec still kept", g_opts.radio_in_dev, "rtl:0:851.0125M:22:0:24");
    closeAudioInDevice(&g_opts);
    return rc;
}

static int
test_invalid_requests_change_nothing(void) {
    int rc = 0;
    session_init(&g_opts, &g_state);
    install_fake_hooks();
    rc |= expect_int_eq("no path", switch_to(&g_opts, &g_state, DSD_AUDIO_INPUT_PCM_FILE, NULL), DSD_AUDIO_INPUT_KEPT);
    rc |= expect_int_eq("empty path", switch_to(&g_opts, &g_state, DSD_AUDIO_INPUT_PCM_FILE, ""), DSD_AUDIO_INPUT_KEPT);
    rc |= expect_int_eq("udp port 0", switch_to_udp(&g_opts, &g_state, NULL, 0), DSD_AUDIO_INPUT_KEPT);
    rc |= expect_int_eq("udp port 65536", switch_to_udp(&g_opts, &g_state, NULL, 65536), DSD_AUDIO_INPUT_KEPT);
    rc |= expect_int_eq("tcp without a socket", switch_to_tcp(&g_opts, &g_state, "h", 1, DSD_INVALID_SOCKET),
                        DSD_AUDIO_INPUT_KEPT);
    rc |= expect_int_eq("tcp without a host", switch_to_tcp(&g_opts, &g_state, "", 1, (dsd_socket_t)5),
                        DSD_AUDIO_INPUT_KEPT);
    static char longest[sizeof(((dsd_opts*)0)->audio_in_dev) + 8];
    DSD_MEMSET(longest, 'p', sizeof longest - 1U);
    longest[sizeof longest - 1U] = '\0';
    rc |= expect_int_eq("a path longer than the name holds",
                        switch_to(&g_opts, &g_state, DSD_AUDIO_INPUT_PCM_FILE, longest), DSD_AUDIO_INPUT_KEPT);
    rc |= expect_int_eq("no options", switch_to(NULL, &g_state, DSD_AUDIO_INPUT_PULSE, NULL), DSD_AUDIO_INPUT_KEPT);
    rc |= expect_int_eq("no state", switch_to(&g_opts, NULL, DSD_AUDIO_INPUT_PULSE, NULL), DSD_AUDIO_INPUT_KEPT);
    rc |= expect_int_eq("nothing asked of udp", (int)strlen(g_udp_log), 0);
    rc |= expect_int_eq("still on pulse", g_opts.audio_in_type, AUDIO_IN_PULSE);
    rc |= expect_str_eq("still named pulse", g_opts.audio_in_dev, "pulse");
    return rc;
}

int
main(void) {
    if (dsd_socket_init() != 0) {
        DSD_FPRINTF(stderr, "FAIL: dsd_socket_init\n");
        return 1;
    }
    dsd_test_temp_cwd cwd;
    if (dsd_test_temp_cwd_enter(&cwd, "dsdneo_input_switch") != 0) {
        DSD_FPRINTF(stderr, "FAIL: temp working directory\n");
        return 1;
    }
    int rc = 0;
    rc |= write_wav("a.wav", 48000, 32);
    rc |= write_wav("b.wav", 96000, 32);
    rc |= write_bytes("c.pcm", 64U);
    rc |= write_bytes("d.rrc", 64U);
    rc |= write_bytes("e.bin", 16U);
    rc |= write_bytes("f.sym", 16U);
    rc |= expect_int_eq("subdir", dsd_mkdir("subdir", 0700), 0);

    rc |= test_wav_switch_replaces_the_file_and_rescales_timing();
    rc |= test_failed_open_changes_nothing();
    rc |= test_leaving_a_header_rate_returns_to_the_raw_rate();
    rc |= test_a_staged_file_rate_outlives_a_wav_header();
    rc |= test_symbol_inputs();
    rc |= test_udp_switch_stops_old_first_and_restarts_it_on_failure();
    rc |= test_switch_away_from_udp_stops_it_only_on_success();
    rc |= test_tcp_switch_takes_the_socket();
    rc |= test_leaving_rtl_rederives_timing_and_keeps_the_spec();
    rc |= test_invalid_requests_change_nothing();
    closeAudioInDevice(&g_opts);

    static const char* const files[] = {"a.wav", "b.wav", "c.pcm", "d.rrc", "e.bin", "f.sym"};
    for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) {
        (void)remove(files[i]);
    }
    (void)dsd_test_rmdir("subdir");
    rc |= expect_int_eq("temp working directory removed", dsd_test_temp_cwd_leave(&cwd), 0);
    dsd_socket_cleanup();
    if (rc == 0) {
        printf("CORE_AUDIO_INPUT_SWITCH: OK\n");
    }
    return rc;
}
