// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/analog_tone.h>
#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/wav_journal.h>
#include <dsd-neo/core/wav_recording.h>
#include <dsd-neo/engine/engine.h>
#include <dsd-neo/io/iq_capture.h>
#include <dsd-neo/io/iq_types.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/platform.h>
#include <dsd-neo/platform/sockets.h>
#include <dsd-neo/runtime/analog_channel.h>
#include <dsd-neo/runtime/analog_tones.h>
#include <dsd-neo/runtime/cli.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/decode_mode.h>
#include <dsd-neo/runtime/exitflag.h>
#include <dsd-neo/runtime/input_failure.h>
#include <dsd-neo/runtime/squelch.h>
#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/io/rtl_stream_fwd.h"
#include "test_env.h"
#include "test_support.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

#if defined(USE_RADIO) && defined(DSD_NEO_TEST_RTL_START_WRAP)
static int g_rtl_create_calls = 0;
static int g_rtl_start_calls = 0;
static int g_rtl_destroy_calls = 0;
static int g_rtl_create_result = 0;
static int g_rtl_start_result = 0;
static int g_fake_rtl_context = 0;
/* The options the stream was created with, and the replay centre they held then (issue #575). */
static dsd_opts* g_rtl_create_opts = NULL;
static uint32_t g_rtl_create_replay_center = 0U;
/* A replay centre the start fake writes into those options, as a replay read while the stream ran would; 0: none. */
static uint32_t g_rtl_start_replay_center = 0U;

// GNU ld --wrap entry points must keep the reserved __wrap_* symbol names.
// NOLINTBEGIN(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
int
__wrap_rtl_stream_create(dsd_opts* opts, RtlSdrContext** out_ctx) {
    g_rtl_create_opts = opts;
    g_rtl_create_replay_center = opts->iq_replay_center_freq;
    g_rtl_create_calls++;
    *out_ctx = g_rtl_create_result == 0 ? (RtlSdrContext*)&g_fake_rtl_context : NULL;
    return g_rtl_create_result;
}

int
__wrap_rtl_stream_start(RtlSdrContext* ctx) {
    (void)ctx;
    g_rtl_start_calls++;
    if (g_rtl_start_replay_center != 0U && g_rtl_create_opts != NULL) {
        g_rtl_create_opts->iq_replay_center_freq = g_rtl_start_replay_center;
    }
    return g_rtl_start_result;
}

int
__wrap_rtl_stream_destroy(RtlSdrContext* ctx) {
    (void)ctx;
    g_rtl_destroy_calls++;
    return 0;
}

// NOLINTEND(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)

static void
reset_rtl_start_fakes(int create_result, int start_result) {
    g_rtl_create_calls = 0;
    g_rtl_start_calls = 0;
    g_rtl_destroy_calls = 0;
    g_rtl_create_result = create_result;
    g_rtl_start_result = start_result;
    g_rtl_create_opts = NULL;
    g_rtl_create_replay_center = 0U;
    g_rtl_start_replay_center = 0U;
}
#endif

#ifdef DSD_NEO_TEST_RIGCTL_WRAP
/* Issue #589: the rigctl connection the engine opens at start (a fake socket for the one port a case names) and the
   peer record handed to it. */
static int g_rigctl_fake_port = 0;
/* A TCP audio server that refuses every connect, counted; the third ends the session, so a retry loop stops. */
static int g_tcp_refusing_port = 0;
static int g_tcp_refused_connects = 0;
static int g_rigctl_rebind_calls = 0;
static dsd_socket_t g_rigctl_rebind_old = 0;
static dsd_socket_t g_rigctl_rebind_new = 0;

// GNU ld --wrap entry points must keep the reserved __wrap_* symbol names.
// NOLINTBEGIN(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
dsd_socket_t __real_Connect(char* hostname, int portno);

dsd_socket_t
__wrap_Connect(char* hostname, int portno) {
    if (g_rigctl_fake_port != 0 && portno == g_rigctl_fake_port) {
        return (dsd_socket_t)777;
    }
    if (g_tcp_refusing_port != 0 && portno == g_tcp_refusing_port) {
        if (++g_tcp_refused_connects >= 3) {
            dsd_exitflag_store(1);
        }
        return DSD_INVALID_SOCKET;
    }
    return __real_Connect(hostname, portno);
}

void
__wrap_RigctlRebindPeer(dsd_socket_t old_fd, dsd_socket_t new_fd, int same_endpoint) {
    (void)same_endpoint;
    g_rigctl_rebind_calls++;
    g_rigctl_rebind_old = old_fd;
    g_rigctl_rebind_new = new_fd;
}

/* Issue #621: the session passband the engine asks the peer for at start, and when -- each call takes the next number
   in one sequence, the P25 watchdog's start too -- with whether the run's input was open by then. */
static const dsd_opts* g_run_opts = NULL;
static int g_order_seq = 0;
static int g_ask_calls = 0;
static int g_ask_seq = 0;
static int g_ask_kind = -1;
static int g_ask_bw = 0;
static int g_ask_input_open = 0;
static int g_watchdog_seq = 0;
static int g_lifecycle_seq = 0;
static int g_session_marks = 0;
static int g_session_mark_kind = -1;

bool
__wrap_SetScanRowModulation(dsd_socket_t sockfd, int kind, int bandwidth) {
    (void)sockfd;
    g_ask_calls++;
    g_ask_seq = ++g_order_seq;
    g_ask_kind = kind;
    g_ask_bw = bandwidth;
    g_ask_input_open = g_run_opts && g_run_opts->audio_in_type == AUDIO_IN_WAV && g_run_opts->audio_in_file != NULL;
    return true;
}

void
__wrap_RigctlMarkSessionPassband(dsd_socket_t sockfd, int kind) {
    (void)sockfd;
    g_session_marks++;
    g_session_mark_kind = kind;
}

void __real_p25_sm_watchdog_start(dsd_opts* opts, dsd_state* state);

void
__wrap_p25_sm_watchdog_start(dsd_opts* opts, dsd_state* state) {
    g_watchdog_seq = ++g_order_seq;
    __real_p25_sm_watchdog_start(opts, state);
}

// NOLINTEND(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
#endif

static int
expect_true(const char* tag, int cond) {
    if (!cond) {
        DSD_FPRINTF(stderr, "%s failed\n", tag);
        return 1;
    }
    return 0;
}

static int
expect_double_near(const char* tag, double got, double want, double tol) {
    if (fabs(got - want) > tol) {
        DSD_FPRINTF(stderr, "%s failed got=%f want=%f\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
init_test_runtime(dsd_opts** opts_out, dsd_state** state_out) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (opts == NULL || state == NULL) {
        DSD_FPRINTF(stderr, "alloc-failed: runtime\n");
        free(opts);
        free(state);
        return 1;
    }

    initOpts(opts);
    initState(state);
    opts->playfiles = 1;
    opts->audio_in_type = AUDIO_IN_NULL;
    opts->audio_out_type = 9;
    DSD_SNPRINTF(opts->audio_out_dev, sizeof opts->audio_out_dev, "%s", "null");

    *opts_out = opts;
    *state_out = state;
    return 0;
}

static void
free_test_runtime(dsd_opts* opts, dsd_state* state) {
    if (state != NULL) {
        freeState(state);
    }
    free(state);
    free(opts);
}

typedef struct {
    int start_calls;
    int stop_calls;
    int failures;
} engine_lifecycle_test_ctx;

static const unsigned char kStdoutAudioBytes[] = {0x00, 0x0A, 0x0D, 0x1A, 0x7F, 0x80, 0xFF, 0x0A};

static int
write_stdout_audio_probe(dsd_opts* opts, dsd_state* state, void* context) {
    (void)state;
    int* calls = context;
    ++*calls;
    if (opts->audio_out_type != 1 || opts->audio_out_fd != dsd_fileno(stdout)) {
        return -1;
    }
    return dsd_write(opts->audio_out_fd, kStdoutAudioBytes, sizeof(kStdoutAudioBytes))
                   == (ssize_t)sizeof(kStdoutAudioBytes)
               ? 0
               : -1;
}

/* The inherited stdout descriptor is text-mode on Windows. Audio samples containing LF must not grow a CR byte. */
static int
test_stdout_audio_preserves_bytes(void) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }
    const char* path = "engine_stdout_audio_test.tmp";
    FILE* sink = dsd_fopen_private(path, "w");
    const int saved_stdout = dsd_dup(dsd_fileno(stdout));
    int rc = 0;
    if (!sink || saved_stdout < 0 || fflush(stdout) != 0 || dsd_dup2(dsd_fileno(sink), dsd_fileno(stdout)) < 0) {
        rc = 1;
    } else {
        DSD_SNPRINTF(opts->audio_out_dev, sizeof(opts->audio_out_dev), "%s", "-");
        int calls = 0;
        const dsd_engine_lifecycle_hooks hooks = {.start = write_stdout_audio_probe, .context = &calls};
        rc |= expect_true("stdout audio setup succeeds", dsd_engine_run_with_lifecycle(opts, state, &hooks) == 0);
        rc |= expect_true("stdout audio probe runs once", calls == 1);
    }
    if (saved_stdout >= 0) {
        rc |= expect_true("restore stdout", dsd_dup2(saved_stdout, dsd_fileno(stdout)) >= 0);
        rc |= expect_true("close saved stdout", dsd_close(saved_stdout) == 0);
    }
    if (sink) {
        rc |= expect_true("close stdout capture", fclose(sink) == 0);
    }
    FILE* capture = dsd_fopen_existing_regular_file(path, "rb");
    if (!capture) {
        rc = 1;
    } else {
        unsigned char got[sizeof(kStdoutAudioBytes) + 1] = {0};
        const size_t count = fread(got, 1, sizeof(got), capture);
        rc |= expect_true("stdout audio retains exact bytes",
                          count == sizeof(kStdoutAudioBytes) && memcmp(got, kStdoutAudioBytes, count) == 0);
        rc |= expect_true("close stdout capture reader", fclose(capture) == 0);
    }
    (void)remove(path);
    free_test_runtime(opts, state);
    return rc;
}

static int
test_lifecycle_start(dsd_opts* opts, dsd_state* state, void* context) {
    engine_lifecycle_test_ctx* ctx = (engine_lifecycle_test_ctx*)context;
    ctx->start_calls++;
    if (opts->udp_in_portno != 7355) {
        DSD_FPRINTF(stderr, "lifecycle start ran before UDP setup\n");
        ctx->failures++;
    }
    if (dsd_state_ext_get(state, DSD_STATE_EXT_ENGINE_START_MS) == NULL) {
        DSD_FPRINTF(stderr, "lifecycle start missing live state extension\n");
        ctx->failures++;
    }
    return 0;
}

static void
test_lifecycle_stop(dsd_opts* opts, dsd_state* state, void* context) {
    engine_lifecycle_test_ctx* ctx = (engine_lifecycle_test_ctx*)context;
    ctx->stop_calls++;
    if (opts->udp_in_portno != 7355) {
        DSD_FPRINTF(stderr, "lifecycle stop saw state before setup completed\n");
        ctx->failures++;
    }
    if (dsd_state_ext_get(state, DSD_STATE_EXT_ENGINE_START_MS) == NULL) {
        DSD_FPRINTF(stderr, "lifecycle stop ran after engine cleanup\n");
        ctx->failures++;
    }
}

static int
test_lifecycle_hooks_start_after_setup_and_stop_before_cleanup(void) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }

    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "udp");
    state->debug_mode = 1;

    engine_lifecycle_test_ctx ctx = {0};
    dsd_engine_lifecycle_hooks hooks = {
        .start = test_lifecycle_start,
        .stop = test_lifecycle_stop,
        .context = &ctx,
    };
    int rc = dsd_engine_run_with_lifecycle(opts, state, &hooks);

    int test_rc = 0;
    test_rc |= expect_true("lifecycle run ok", rc == 0);
    test_rc |= expect_true("lifecycle start called once", ctx.start_calls == 1);
    test_rc |= expect_true("lifecycle stop called once", ctx.stop_calls == 1);
    test_rc |= expect_true("lifecycle ordering checks", ctx.failures == 0);
    test_rc |= expect_true("cleanup ran after lifecycle stop",
                           dsd_state_ext_get(state, DSD_STATE_EXT_ENGINE_START_MS) == NULL);

    free_test_runtime(opts, state);
    return test_rc;
}

#ifdef SIGHUP
/* Issue #637: a hangup (the terminal closed, an SSH session dropped) ends the session as SIGINT does, through the normal
   shutdown, unless the session was started ignoring it (nohup) or the host embedding the decoder owns its signals. */
typedef struct {
    int stop_calls;
    int exit_after_hangup;
} engine_hangup_test_ctx;

static int g_host_hangups;

static void
test_host_hangup_handler(int sgnl) {
    (void)sgnl;
    g_host_hangups++;
}

static int
raise_hangup_at_start(dsd_opts* opts, dsd_state* state, void* context) {
    (void)opts;
    (void)state;
    engine_hangup_test_ctx* ctx = (engine_hangup_test_ctx*)context;
    (void)raise(SIGHUP);
    ctx->exit_after_hangup = dsd_exitflag_load();
    dsd_exitflag_store(1);
    return 0;
}

static void
count_hangup_stop(dsd_opts* opts, dsd_state* state, void* context) {
    (void)opts;
    (void)state;
    ((engine_hangup_test_ctx*)context)->stop_calls++;
}

/* @p disposition is what the session starts with for SIGHUP; @p embedded sets DSD_NEO_NO_SIGNAL_HANDLERS. Returns the
   context the run leaves, with -1 in stop_calls when it could not run. */
static engine_hangup_test_ctx
run_with_hangup(void (*disposition)(int), int embedded) {
    engine_hangup_test_ctx ctx = {-1, 0};
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (embedded && dsd_test_setenv("DSD_NEO_NO_SIGNAL_HANDLERS", "1", 1) != 0) {
        return ctx;
    }
    dsd_neo_config_init();
    if (init_test_runtime(&opts, &state) != 0) {
        (void)dsd_test_unsetenv("DSD_NEO_NO_SIGNAL_HANDLERS");
        dsd_neo_config_init();
        return ctx;
    }
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "udp");
    ctx.stop_calls = 0;
    dsd_engine_lifecycle_hooks hooks = {
        .start = raise_hangup_at_start,
        .stop = count_hangup_stop,
        .context = &ctx,
    };
    void (*saved_int)(int) = signal(SIGINT, SIG_DFL);
    void (*saved_term)(int) = signal(SIGTERM, SIG_DFL);
    void (*saved_hup)(int) = signal(SIGHUP, disposition);
    dsd_exitflag_store(0);
    (void)dsd_engine_run_with_lifecycle(opts, state, &hooks);
    if (disposition == SIG_IGN && signal(SIGHUP, SIG_IGN) != SIG_IGN) {
        ctx.stop_calls = -2; /* the ignored hangup did not stay ignored */
    }
    (void)signal(SIGHUP, saved_hup);
    (void)signal(SIGTERM, saved_term);
    (void)signal(SIGINT, saved_int);
    dsd_exitflag_store(0);
    free_test_runtime(opts, state);
    (void)dsd_test_unsetenv("DSD_NEO_NO_SIGNAL_HANDLERS");
    dsd_neo_config_init();
    return ctx;
}

static int
test_hangup_shuts_down_like_an_interrupt(void) {
    int test_rc = 0;
    /* The default disposition would end the process at once: the session's handler turns it into a shutdown. */
    const engine_hangup_test_ctx hangup = run_with_hangup(SIG_DFL, 0);
    test_rc |= expect_true("hangup requests the shutdown", hangup.exit_after_hangup == 1);
    test_rc |= expect_true("hangup shuts down through the lifecycle stop", hangup.stop_calls == 1);
    /* Started under nohup: the hangup stays ignored. */
    const engine_hangup_test_ctx ignored = run_with_hangup(SIG_IGN, 0);
    test_rc |=
        expect_true("an ignored hangup stays ignored", ignored.exit_after_hangup == 0 && ignored.stop_calls == 1);
    /* An embedding host keeps its own handler. */
    g_host_hangups = 0;
    const engine_hangup_test_ctx embedded = run_with_hangup(test_host_hangup_handler, 1);
    test_rc |= expect_true("an embedding host keeps its hangup handler",
                           g_host_hangups == 1 && embedded.exit_after_hangup == 0 && embedded.stop_calls == 1);
    return test_rc;
}
#endif

static int
inject_receiver_failure(dsd_opts* opts, dsd_state* state, void* context) {
    (void)opts;
    (void)state;
    int* calls = (int*)context;
    ++*calls;
    dsd_input_failure_report(DSD_INPUT_FAILURE_DEVICE, -77);
    dsd_exitflag_store(1);
    return 0;
}

static int
test_receiver_failure_is_not_successful_completion(void) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }
    int calls = 0;
    const dsd_engine_lifecycle_hooks hooks = {.start = inject_receiver_failure, .context = &calls};
    int result = dsd_engine_run_with_lifecycle(opts, state, &hooks);
    dsd_input_failure failure;
    dsd_input_failure_get(&failure);
    int rc = expect_true("receiver failure after initialization is a failed run", calls == 1 && result != 0);
    rc |= expect_true("receiver error retained after cleanup",
                      failure.kind == DSD_INPUT_FAILURE_DEVICE && failure.native_code == -77);
    result = dsd_engine_run_with_lifecycle(opts, state, NULL);
    dsd_input_failure_get(&failure);
    rc |= expect_true("next run clears receiver error", result == 0 && failure.kind == DSD_INPUT_FAILURE_NONE);
    free_test_runtime(opts, state);
    return rc;
}

static int
test_conflicting_scan_modes_fail_before_live_setup(void) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }

    opts->trunk_scan_enabled = 1;
    opts->scanner_mode = 1;
    int rc = dsd_engine_run_with_lifecycle(opts, state, NULL);

    int test_rc = expect_true("conflicting scan modes rejected", rc != 0);
    free_test_runtime(opts, state);
    return test_rc;
}

static int
test_m17_udp_input_and_output_specs(void) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }

    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "m17udp:rx.example:17000");
    DSD_SNPRINTF(opts->audio_out_dev, sizeof opts->audio_out_dev, "%s", "m17udp:tx.example:17001");
    int rc = dsd_engine_run_with_lifecycle(opts, state, NULL);

    int test_rc = 0;
    test_rc |= expect_true("m17 run ok", rc == 0);
    test_rc |= expect_true("m17 output enables ip", opts->m17_use_ip == 1);
    test_rc |= expect_true("m17 output type", opts->audio_out_type == 9);
    test_rc |= expect_true("m17 host parsed", strcmp(opts->m17_hostname, "tx.example") == 0);
    test_rc |= expect_true("m17 port parsed", opts->m17_portno == 17001);

    free_test_runtime(opts, state);
    return test_rc;
}

static int
test_m17_userdata_is_normalized_during_common_setup(void) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }

    DSD_SNPRINTF(state->m17dat, sizeof state->m17dat, "%s", "M17:31:n0call:all:16000:7");
    int rc = dsd_engine_run_with_lifecycle(opts, state, NULL);

    int test_rc = 0;
    test_rc |= expect_true("m17 userdata run ok", rc == 0);
    test_rc |= expect_true("m17 CAN clamps to maximum", state->m17_can_en == 15);
    test_rc |= expect_true("m17 source uppercased", strcmp(state->str50c, "N0CALL") == 0);
    test_rc |= expect_true("m17 destination uppercased", strcmp(state->str50b, "ALL") == 0);
    test_rc |= expect_true("m17 rate parsed", state->m17_rate == 16000);
    test_rc |= expect_true("m17 vox clamps to enabled", state->m17_vox == 1);

    free_test_runtime(opts, state);
    return test_rc;
}

/* The stream encoder keeps one input sample in INPUT_RATE / 8000: a rate below 8000 kept none and hung reading nothing,
   and one between the multiples fed codec2 audio at the wrong rate, so -M takes only multiples of 8000 up to 48000. */
static int
test_m17_userdata_refuses_unsupported_input_rates(void) {
    static const struct {
        const char* userdata;
        int want_rate;
    } cases[] = {
        {"M17:7:A:B:0", 48000},     {"M17:7:A:B:4000", 48000}, {"M17:7:A:B:12000", 48000}, {"M17:7:A:B:96000", 48000},
        {"M17:7:A:B:-8000", 48000}, {"M17:7:A:B:8000", 8000},  {"M17:7:A:B:24000", 24000}, {"M17:7:A:B:48000", 48000},
    };

    int failures = 0;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        dsd_opts* opts = NULL;
        dsd_state* state = NULL;
        if (init_test_runtime(&opts, &state) != 0) {
            return 1;
        }
        DSD_SNPRINTF(state->m17dat, sizeof state->m17dat, "%s", cases[i].userdata);
        const int rc = dsd_engine_run_with_lifecycle(opts, state, NULL);
        if (rc != 0 || state->m17_rate != cases[i].want_rate) {
            DSD_FPRINTF(stderr, "%s: rc %d, rate %d, want %d\n", cases[i].userdata, rc, state->m17_rate,
                        cases[i].want_rate);
            failures++;
        }
        free_test_runtime(opts, state);
    }
    return expect_true("m17 input rate held to multiples of 8000 up to 48000", failures == 0);
}

static int
test_m17_stream_encoder_rejects_unsupported_input(void) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }

    opts->playfiles = 0;
    opts->m17encoder = 1;
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "m17udp:rx.example:17000");
    int rc = dsd_engine_run_with_lifecycle(opts, state, NULL);

    int test_rc = expect_true("M17 stream encoder rejects non-PCM input", rc != 0);
    free_test_runtime(opts, state);
    return test_rc;
}

#if defined(USE_RADIO) && defined(DSD_NEO_TEST_RTL_START_WRAP)
static int
test_m17_stream_encoder_propagates_rtl_start_failures(void) {
    int test_rc = 0;
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }

    opts->playfiles = 0;
    opts->m17encoder = 1;
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "rtltcp:127.0.0.1:1234");
    reset_rtl_start_fakes(-1, 0);
    int rc = dsd_engine_run_with_lifecycle(opts, state, NULL);

    test_rc |= expect_true("M17 RTL create failure rejects startup", rc != 0);
    test_rc |= expect_true("M17 RTL create failure stops before start",
                           g_rtl_create_calls == 1 && g_rtl_start_calls == 0 && g_rtl_destroy_calls == 0);
    test_rc |=
        expect_true("M17 RTL create failure clears stream state", opts->rtl_started == 0 && state->rtl_ctx == NULL);
    free_test_runtime(opts, state);

    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }
    opts->playfiles = 0;
    opts->m17encoder = 1;
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "rtltcp:127.0.0.1:1234");
    reset_rtl_start_fakes(0, -1);
    rc = dsd_engine_run_with_lifecycle(opts, state, NULL);

    test_rc |= expect_true("M17 RTL start failure rejects startup", rc != 0);
    test_rc |= expect_true("M17 RTL start failure destroys context",
                           g_rtl_create_calls == 1 && g_rtl_start_calls == 1 && g_rtl_destroy_calls == 1);
    test_rc |=
        expect_true("M17 RTL start failure clears stream state", opts->rtl_started == 0 && state->rtl_ctx == NULL);
    free_test_runtime(opts, state);
    return test_rc;
}

/* Issue #575: the I/Q replay centre the tuned-frequency reading follows belongs to the stream that read it. An engine
 * run opens the RTL stream with none, whatever an earlier run on the same options left (the create fake sees what the
 * options held then), and its cleanup leaves none behind once the stream goes, though one was read after the open (the
 * start fake writes one, as a replay read would). */
static int
test_rtl_run_clears_a_stale_replay_centre(void) {
    int test_rc = 0;
    for (int start_fails = 0; start_fails <= 1; start_fails++) {
        dsd_opts* opts = NULL;
        dsd_state* state = NULL;
        if (init_test_runtime(&opts, &state) != 0) {
            return 1;
        }
        opts->playfiles = 0;
        DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "rtltcp:127.0.0.1:1234");
        opts->iq_replay_center_freq = 851500000U;
        /* A create that fails, then a start that fails: the run ends at its stream open either way, and cleans up. */
        reset_rtl_start_fakes(start_fails ? 0 : -1, start_fails ? -1 : 0);
        g_rtl_start_replay_center = 851625000U;
        const int rc = dsd_engine_run_with_lifecycle(opts, state, NULL);
        test_rc |= expect_true("RTL run whose stream fails to open fails", rc != 0 && g_rtl_create_calls == 1);
        test_rc |= expect_true("RTL run opens its stream with no replay centre", g_rtl_create_replay_center == 0U);
        test_rc |= expect_true("RTL run's start ran only after a create", g_rtl_start_calls == start_fails);
        test_rc |= expect_true("RTL run's cleanup leaves no replay centre", opts->iq_replay_center_freq == 0U);
        test_rc |=
            expect_true("RTL run reads the tuned centre", dsd_opts_tuned_freq_hz(opts) == opts->rtlsdr_center_freq);
        free_test_runtime(opts, state);
    }
    reset_rtl_start_fakes(0, 0);
    return test_rc;
}
#endif

static int
test_udp_input_defaults_and_null_output(void) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }

    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "udp");
    int rc = dsd_engine_run_with_lifecycle(opts, state, NULL);

    int test_rc = 0;
    test_rc |= expect_true("udp run ok", rc == 0);
    test_rc |= expect_true("udp default bind address", strcmp(opts->udp_in_bindaddr, "127.0.0.1") == 0);
    test_rc |= expect_true("udp default port", opts->udp_in_portno == 7355);
    test_rc |= expect_true("null output disables audio", opts->audio_out_type == 9 && opts->audio_out == 0);

    free_test_runtime(opts, state);
    return test_rc;
}

static int
test_udp_output_connection_failure_is_fatal(void) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }

    DSD_SNPRINTF(opts->audio_out_dev, sizeof opts->audio_out_dev, "%s", "udp:invalid host:23456");
    int rc = dsd_engine_run_with_lifecycle(opts, state, NULL);

    int test_rc = expect_true("UDP output connection failure rejects setup", rc != 0);
    test_rc |= expect_true("UDP output connection failure preserves requested output",
                           strcmp(opts->audio_out_dev, "udp:invalid host:23456") == 0);
    test_rc |= expect_true("UDP output connection failure does not select another backend", opts->audio_out_type == 9);

    free_test_runtime(opts, state);
    return test_rc;
}

static int
test_tcp_connection_failure_is_fatal(void) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }

    opts->frame_m17 = 0;
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "tcp:127.0.0.1:0");
    int rc = dsd_engine_run_with_lifecycle(opts, state, NULL);

    int test_rc = expect_true("TCP connection failure rejects setup", rc != 0);
    test_rc |= expect_true("TCP connection failure preserves requested input",
                           strcmp(opts->audio_in_dev, "tcp:127.0.0.1:0") == 0);
    test_rc |= expect_true("TCP connection failure does not select Pulse", opts->audio_in_type != AUDIO_IN_PULSE);

    free_test_runtime(opts, state);
    return test_rc;
}

#ifndef USE_RADIO
static int
test_unavailable_radio_inputs_are_rejected(void) {
    static const char* const specs[] = {
        "rtl",
        "rtltcp:127.0.0.1:1234",
        "soapy:driver=test",
    };
    int test_rc = 0;

    for (size_t i = 0; i < sizeof specs / sizeof specs[0]; i++) {
        dsd_opts* opts = NULL;
        dsd_state* state = NULL;
        if (init_test_runtime(&opts, &state) != 0) {
            return 1;
        }

        opts->playfiles = 0;
        DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", specs[i]);
        int rc = dsd_engine_run_with_lifecycle(opts, state, NULL);

        test_rc |= expect_true("unavailable radio input rejects setup", rc != 0);
        test_rc |= expect_true("unavailable radio input preserves requested device",
                               strcmp(opts->audio_in_dev, specs[i]) == 0);

        free_test_runtime(opts, state);
    }
    return test_rc;
}
#endif

static int
expect_contains(const char* tag, const char* haystack, const char* needle) {
    if (strstr(haystack, needle) == NULL) {
        DSD_FPRINTF(stderr, "%s failed: %s not in [%s]\n", tag, needle, haystack);
        return 1;
    }
    return 0;
}

static int
expect_omits(const char* tag, const char* haystack, const char* needle) {
    if (strstr(haystack, needle) != NULL) {
        DSD_FPRINTF(stderr, "%s failed: %s present in [%s]\n", tag, needle, haystack);
        return 1;
    }
    return 0;
}

/* Run the setup with the startup banners captured, so what the user is told about
 * the squelch is assertable and not merely inferred from the stored value. */
static int
run_lifecycle_capturing_banner(dsd_opts* opts, dsd_state* state, char* buf, size_t buf_size, int* rc_out) {
    dsd_test_capture_stderr cap;
    if (dsd_test_capture_stderr_begin(&cap, "engine_banner") != 0) {
        return 1;
    }
    *rc_out = dsd_engine_run_with_lifecycle(opts, state, NULL);
    if (dsd_test_capture_stderr_end(&cap) != 0) {
        return 1;
    }
    return dsd_test_capture_stderr_read(&cap, buf, buf_size) != 0;
}

static int
test_rtltcp_tuning_tokens_and_bias(void) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }

    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s",
                 "rtltcp:radio.local:1234:769.00625M:28:3:24:-47:3:bias=off");
    opts->rtl_bias_tee = 1;
    int rc = dsd_engine_run_with_lifecycle(opts, state, NULL);

    int test_rc = 0;
    test_rc |= expect_true("rtltcp run ok", rc == 0);
    test_rc |= expect_true("rtltcp host", strcmp(opts->rtltcp_hostname, "radio.local") == 0);
    test_rc |= expect_true("rtltcp port", opts->rtltcp_portno == 1234);
    test_rc |=
        expect_true("rtltcp enables rtl input", opts->rtltcp_enabled == 1 && opts->audio_in_type == AUDIO_IN_RTL);
    test_rc |= expect_true("rtltcp frequency", opts->rtlsdr_center_freq == 769006250U);
    test_rc |=
        expect_true("rtltcp gain ppm bw volume", opts->rtl_gain_value == 28 && opts->rtlsdr_ppm_error == 3
                                                     && opts->rtl_dsp_bw_khz == 24 && opts->rtl_volume_multiplier == 3);
    test_rc |= expect_true("rtltcp bias off", opts->rtl_bias_tee == 0);
    test_rc |= expect_double_near("rtltcp squelch", opts->rtl_squelch_level, dB_to_pwr(-47.0), 1e-12);

    free_test_runtime(opts, state);
    return test_rc;
}

/* The bias tee puts DC on the antenna port. Every documented spelling of the trailing token must do what it says
 * (`bias=on` read as off before), and a value or token the parser cannot read must change nothing. Full-length specs
 * so the parser actually reaches the token. */
static int
test_rtltcp_bias_spellings(void) {
    static const struct {
        const char* token;
        int before;
        int after;
    } k_cases[] = {
        {"bias=on", 0, 1},    {"bias=ON", 0, 1},   {"bias", 0, 1},       {"b", 0, 1},       {"bias=", 0, 1},
        {"bias=1", 0, 1},     {"bias=true", 0, 1}, {"bias=yes", 0, 1},   {"b=on", 0, 1},    {"bias=off", 1, 0},
        {"bias=Off", 1, 0},   {"bias=0", 1, 0},    {"bias=false", 1, 0}, {"bias=no", 1, 0}, {"bias=bogus", 0, 0},
        {"bias=bogus", 1, 1}, {"bias=onx", 0, 0},  {"bias=2", 1, 1},     {"bogus", 0, 0},   {"bogus", 1, 1},
        {"biased", 0, 0},
    };

    int test_rc = 0;
    for (size_t i = 0; i < sizeof(k_cases) / sizeof(k_cases[0]); i++) {
        dsd_opts* opts = NULL;
        dsd_state* state = NULL;
        if (init_test_runtime(&opts, &state) != 0) {
            return 1;
        }
        DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev,
                     "rtltcp:radio.local:1234:769.00625M:28:3:24:-47:2:%s", k_cases[i].token);
        opts->rtl_bias_tee = k_cases[i].before;
        char log[8192];
        int rc = -1;
        test_rc |= run_lifecycle_capturing_banner(opts, state, log, sizeof log, &rc);
        char tag[96];
        DSD_SNPRINTF(tag, sizeof tag, "bias token '%s' from %d", k_cases[i].token, k_cases[i].before);
        test_rc |= expect_true(tag, rc == 0 && opts->rtl_bias_tee == k_cases[i].after);
        if (k_cases[i].before == k_cases[i].after) {
            test_rc |= expect_contains(tag, log, "WARNING: Ignoring");
        }
        free_test_runtime(opts, state);
    }
    return test_rc;
}

/* --rtl-udp-control tunes the front end behind the trunk-scan coordinator, which owns the tuner: under --trunk-scan the
 * listener stays closed and the user is told why. Other sessions keep it. */
static int
test_trunk_scan_turns_off_rtl_udp_control(void) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }
    opts->trunk_scan_enabled = 1;
    opts->rtl_udp_port = 9911;
    char log[8192];
    int rc = 0;
    int test_rc = run_lifecycle_capturing_banner(opts, state, log, sizeof log, &rc);
    /* The run itself fails later (no targets CSV); the listener decision comes first. */
    test_rc |= expect_true("trunk scan closes the udp retune port", opts->rtl_udp_port == 0);
    test_rc |= expect_contains("trunk scan udp warning", log, "--rtl-udp-control is off under --trunk-scan");
    free_test_runtime(opts, state);

    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }
    opts->rtl_udp_port = 9911;
    test_rc |= run_lifecycle_capturing_banner(opts, state, log, sizeof log, &rc);
    test_rc |= expect_true("plain session keeps the udp retune port", opts->rtl_udp_port == 9911);
    test_rc |= expect_omits("plain session has no udp warning", log, "--rtl-udp-control is off");
    free_test_runtime(opts, state);
    return test_rc;
}

static int
test_rtltcp_invalid_and_partial_tuning_tokens(void) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }

    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s",
                 "rtltcp:bad-port.example:not-a-port:not-a-freq:bad-gain:bad-ppm:bogus-bw:-35:7:bias=0");
    opts->rtl_gain_value = 14;
    opts->rtlsdr_ppm_error = -2;
    opts->rtl_squelch_level = 0.25;
    opts->rtl_bias_tee = 1;
    int rc = dsd_engine_run_with_lifecycle(opts, state, NULL);

    int test_rc = 0;
    test_rc |= expect_true("rtltcp invalid run ok", rc == 0);
    test_rc |= expect_true("rtltcp invalid host", strcmp(opts->rtltcp_hostname, "bad-port.example") == 0);
    test_rc |= expect_true("rtltcp invalid port defaults", opts->rtltcp_portno == 1234);
    test_rc |= expect_true("rtltcp invalid freq becomes zero", opts->rtlsdr_center_freq == 0U);
    test_rc |= expect_true("rtltcp invalid gain and ppm preserved",
                           opts->rtl_gain_value == 14 && opts->rtlsdr_ppm_error == -2);
    test_rc |= expect_true("rtltcp invalid bandwidth defaults", opts->rtl_dsp_bw_khz == 48);
    test_rc |= expect_true("rtltcp invalid bias off", opts->rtl_bias_tee == 0);
    /* A monitor gain outside 0..3 reads as the default 1, as on every input (issue #669 B10): 7 would multiply the
       audio sevenfold, and the menu cycle could never return to it. */
    test_rc |= expect_true("rtltcp out-of-range volume reads as 1", opts->rtl_volume_multiplier == 1);
    test_rc |= expect_double_near("rtltcp invalid squelch", opts->rtl_squelch_level, dB_to_pwr(-35.0), 1e-12);

    free_test_runtime(opts, state);

    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }
    /* An unparseable squelch token is not a request to switch the squelch off:
     * the setting the session already had has to survive it. */
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "rtltcp:sql.example:1234:450M:11:0:24:loud:3");
    opts->rtl_squelch_level = 0.25;
    rc = dsd_engine_run_with_lifecycle(opts, state, NULL);
    test_rc |= expect_true("rtltcp unparseable sql run ok", rc == 0);
    test_rc |=
        expect_double_near("rtltcp unparseable sql keeps previous threshold", opts->rtl_squelch_level, 0.25, 1e-12);
    test_rc |= expect_true("rtltcp unparseable sql still parses volume", opts->rtl_volume_multiplier == 3);

    free_test_runtime(opts, state);

    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "rtltcp:short.example:7777:450M:11:bias=on");
    opts->rtl_bias_tee = 0;
    rc = dsd_engine_run_with_lifecycle(opts, state, NULL);

    test_rc |= expect_true("rtltcp partial run ok", rc == 0);
    test_rc |= expect_true("rtltcp partial host", strcmp(opts->rtltcp_hostname, "short.example") == 0);
    test_rc |= expect_true("rtltcp partial port", opts->rtltcp_portno == 7777);
    test_rc |= expect_true("rtltcp partial freq and gain",
                           opts->rtlsdr_center_freq == 450000000U && opts->rtl_gain_value == 11);
    test_rc |= expect_true("rtltcp partial stops before bias", opts->rtl_bias_tee == 0);

    free_test_runtime(opts, state);
    return test_rc;
}

static int
test_soapy_setup_normalizes_args_and_tuning(void) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }

    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s",
                 "soapy:driver=test,serial=ABC:450.5M:7:2:12:-33:3");
    char banner[4096] = {0};
    int rc = 0;
    if (run_lifecycle_capturing_banner(opts, state, banner, sizeof banner, &rc) != 0) {
        free_test_runtime(opts, state);
        return 1;
    }

    int test_rc = 0;
    test_rc |= expect_true("soapy run ok", rc == 0);
    test_rc |= expect_true("soapy normalizes args", strcmp(opts->audio_in_dev, "soapy:driver=test,serial=ABC") == 0);
    test_rc |= expect_true("soapy selects rtl input", opts->audio_in_type == AUDIO_IN_RTL && opts->rtltcp_enabled == 0);
    test_rc |= expect_true("soapy rtl-style tuning", opts->rtlsdr_center_freq == 450500000U && opts->rtl_gain_value == 7
                                                         && opts->rtlsdr_ppm_error == 2 && opts->rtl_dsp_bw_khz == 12
                                                         && opts->rtl_volume_multiplier == 3);
    test_rc |= expect_double_near("soapy squelch", opts->rtl_squelch_level, dB_to_pwr(-33.0), 1e-12);
    test_rc |= expect_contains("soapy banner states the threshold", banner, "SQ=-33.0dB");

    free_test_runtime(opts, state);

    /* sql=0 is what every documented example uses, and it switches the squelch
     * off. Reported as -120.0 dB it read as a threshold that had been applied,
     * which is the confusion this case pins down. */
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "soapy:driver=test:450.5M:7:2:12:0:3");
    banner[0] = '\0';
    if (run_lifecycle_capturing_banner(opts, state, banner, sizeof banner, &rc) != 0) {
        free_test_runtime(opts, state);
        return 1;
    }
    test_rc |= expect_true("soapy disabled-squelch run ok", rc == 0);
    test_rc |= expect_true("soapy sql=0 stores no threshold", opts->rtl_squelch_level == 0.0);
    test_rc |= expect_contains("soapy banner names a disabled squelch", banner, "SQ=off");
    test_rc |= expect_omits("soapy banner does not invent a threshold", banner, "SQ=-120");

    free_test_runtime(opts, state);
    return test_rc;
}

/* A small cu8 capture with its sidecar in @p dir; @p out_metadata_path receives the sidecar's path. */
static int
write_replay_capture(const char* dir, char* out_metadata_path, size_t out_metadata_path_size) {
    char data_path[DSD_TEST_PATH_MAX];
    if (dsd_test_path_join(data_path, sizeof(data_path), dir, "capture.iq") != 0
        || dsd_test_path_join(out_metadata_path, out_metadata_path_size, dir, "capture.iq.json") != 0) {
        return -1;
    }
    dsd_iq_capture_config cfg;
    DSD_MEMSET(&cfg, 0, sizeof(cfg));
    DSD_SNPRINTF(cfg.data_path, sizeof(cfg.data_path), "%s", data_path);
    DSD_SNPRINTF(cfg.metadata_path, sizeof(cfg.metadata_path), "%s", out_metadata_path);
    cfg.format = DSD_IQ_FORMAT_CU8;
    DSD_SNPRINTF(cfg.capture_stage, sizeof(cfg.capture_stage), "%s", "post_mute_pre_widen");
    cfg.sample_rate_hz = 1536000U;
    cfg.center_frequency_hz = 851375000ULL;
    cfg.capture_center_frequency_hz = 851759000ULL;
    cfg.rtl_dsp_bw_khz = 48;
    cfg.base_decimation = 32U;
    cfg.post_downsample = 1U;
    cfg.demod_rate_hz = 48000U;
    cfg.fs4_shift_enabled = 1;
    cfg.combine_rotate_enabled = 1;
    cfg.muted_bytes_excluded = 1;
    DSD_SNPRINTF(cfg.source_backend, sizeof(cfg.source_backend), "%s", "rtl");
    DSD_SNPRINTF(cfg.source_args, sizeof(cfg.source_args), "%s", "dev=0");
    dsd_iq_capture_writer* writer = NULL;
    char err[256] = {0};
    if (dsd_iq_capture_open(&cfg, &writer, err, sizeof(err)) != DSD_IQ_OK || !writer) {
        DSD_FPRINTF(stderr, "could not open the capture writer: %s\n", err[0] ? err : "unknown");
        return -1;
    }
    static uint8_t payload[4096];
    DSD_MEMSET(payload, 128, sizeof(payload));
    if (dsd_iq_capture_submit(writer, payload, sizeof(payload)) != DSD_IQ_OK) {
        dsd_iq_capture_abort(writer);
        return -1;
    }
    dsd_iq_capture_final_stats stats;
    DSD_MEMSET(&stats, 0, sizeof(stats));
    dsd_iq_capture_close(writer, &stats);
    return 0;
}

static int
test_iq_replay_guard_and_requested_setup(void) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }

    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "iqreplay:/tmp/capture.iq");
    int rc = dsd_engine_run_with_lifecycle(opts, state, NULL);
    int test_rc = expect_true("direct iqreplay rejected", rc != 0);
    free_test_runtime(opts, state);

    /* Issue #572: the engine reads a requested replay's sidecar first, for the capture clock, so a capture whose
       sidecar does not parse fails the run there. */
    char dir[DSD_TEST_PATH_MAX];
    if (!dsd_test_mkdtemp(dir, sizeof(dir), "dsdneo_run_setup_replay")) {
        return 1;
    }
    const char* const files[] = {"capture.iq", "capture.iq.json", NULL};
    char missing_path[DSD_TEST_PATH_MAX];
    char metadata_path[DSD_TEST_PATH_MAX];
    if (dsd_test_path_join(missing_path, sizeof(missing_path), dir, "missing.iq.json") != 0
        || write_replay_capture(dir, metadata_path, sizeof(metadata_path)) != 0) {
        (void)dsd_test_remove_temp_dir(dir, files);
        return 1;
    }

    if (init_test_runtime(&opts, &state) != 0) {
        (void)dsd_test_remove_temp_dir(dir, files);
        return 1;
    }
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "iqreplay:%s", missing_path);
    opts->iq_replay_requested = 1;
    rc = dsd_engine_run_with_lifecycle(opts, state, NULL);
    test_rc |= expect_true("requested iqreplay without a sidecar fails", rc == 1);
    free_test_runtime(opts, state);

    if (init_test_runtime(&opts, &state) != 0) {
        (void)dsd_test_remove_temp_dir(dir, files);
        return 1;
    }
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "iqreplay:%s", metadata_path);
    opts->iq_replay_requested = 1;
    rc = dsd_engine_run_with_lifecycle(opts, state, NULL);
    test_rc |= expect_true("requested iqreplay accepted", rc == 0);
    test_rc |= expect_true("requested iqreplay state", opts->iq_replay_active == 1 && opts->rtltcp_enabled == 0
                                                           && opts->audio_in_type == AUDIO_IN_RTL);

    free_test_runtime(opts, state);
    if (dsd_test_remove_temp_dir(dir, files) != 0) {
        DSD_FPRINTF(stderr, "could not remove %s\n", dir);
        test_rc = 1;
    }
    return test_rc;
}

static int
test_missing_source_labels_do_not_stop_decode(void) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }
    char path[DSD_TEST_PATH_MAX];
    int fd = dsd_test_mkstemp(path, sizeof(path), "dsd-missing-source-list");
    if (fd < 0) {
        free_test_runtime(opts, state);
        return 1;
    }
    (void)dsd_close(fd);
    (void)remove(path);
    DSD_SNPRINTF(opts->src_in_file, sizeof(opts->src_in_file), "%s", path);
    int rc = expect_true("missing cosmetic source list does not abort startup",
                         dsd_engine_run_with_lifecycle(opts, state, NULL) == 0);
    free_test_runtime(opts, state);
    return rc;
}

#if DSD_PLATFORM_WIN_NATIVE
/* The Windows CRT's narrow file calls read a path in the ANSI code page, so a UTF-8 folder name is not tried there. */
#define PER_CALL_TEST_FOLDER "Call recordings"
#else
#define PER_CALL_TEST_FOLDER "Call recordings \xC3\xA9t\xC3\xA9"
#endif

/* What the per-call WAV outputs were when the run started. */
typedef struct {
    const char* dir;
    int starts;
    int opened_in_dir;
} per_call_start_probe;

static int
path_has_dir(const char* path, const char* dir) {
    const size_t n = strlen(dir);
    return strncmp(path, dir, n) == 0 && path[n] == '/';
}

static int
probe_per_call_outputs_at_start(dsd_opts* opts, dsd_state* state, void* context) {
    (void)state;
    per_call_start_probe* probe = (per_call_start_probe*)context;
    probe->starts++;
    probe->opened_in_dir = opts->wav_out_f != NULL && opts->wav_out_fR != NULL
                           && path_has_dir(opts->wav_out_file, probe->dir)
                           && path_has_dir(opts->wav_out_fileR, probe->dir);
    return 0;
}

static int
remove_listed_file(const char* name, void* user) {
    char path[DSD_TEST_PATH_MAX];
    if (dsd_test_path_join(path, sizeof path, (const char*)user, name) == 0) {
        (void)remove(path);
    }
    return 0;
}

/*
 * Issue #708: with -P on the command line, the engine opens both slots' recordings when it starts, in the folder -7
 * names, creating it, whichever order the two options come in. -P used to open them while it was parsed, in the folder
 * set by then, so `-P -7 <folder>` recorded into ./WAV and left the folder empty. The folder's name has a space, and
 * non-ASCII letters where the platform's file calls take UTF-8.
 */
static int
test_per_call_wav_opens_in_the_cli_folder_at_start(void) {
    dsd_test_temp_cwd cwd;
    if (dsd_test_temp_cwd_enter(&cwd, "dsdneo_engine_per_call") != 0) {
        DSD_FPRINTF(stderr, "per-call start: temp working directory failed\n");
        return 1;
    }
    int rc = 0;
    for (int order = 0; order < 2; order++) {
        const char* tag = order == 0 ? "-P -7" : "-7 -P";
        dsd_opts* opts = NULL;
        dsd_state* state = NULL;
        if (init_test_runtime(&opts, &state) != 0) {
            rc = 1;
            break;
        }
        char dir[DSD_TEST_PATH_MAX];
        if (dsd_test_path_join(dir, sizeof dir, cwd.dir, PER_CALL_TEST_FOLDER) != 0) {
            free_test_runtime(opts, state);
            rc = 1;
            break;
        }
        char arg0[] = "dsd-neo";
        char opt_p[] = "-P";
        char opt_7[] = "-7";
        char* argv_p_first[] = {arg0, opt_p, opt_7, dir, NULL};
        char* argv_dir_first[] = {arg0, opt_7, dir, opt_p, NULL};
        int argc_effective = 0;
        int exit_rc = 0;
        const int parsed =
            dsd_parse_args(4, order == 0 ? argv_p_first : argv_dir_first, opts, state, &argc_effective, &exit_rc);
        if (parsed != DSD_PARSE_CONTINUE) {
            DSD_FPRINTF(stderr, "%s: parse rc=%d\n", tag, parsed);
            rc = 1;
        }

        per_call_start_probe probe = {dir, 0, 0};
        const dsd_engine_lifecycle_hooks hooks = {.start = probe_per_call_outputs_at_start, .context = &probe};
        if (dsd_engine_run_with_lifecycle(opts, state, &hooks) != 0 || probe.starts != 1) {
            DSD_FPRINTF(stderr, "%s: the run failed (starts %d)\n", tag, probe.starts);
            rc = 1;
        }
        if (!probe.opened_in_dir) {
            DSD_FPRINTF(stderr, "%s: the slot recordings were not open in the -7 folder at start (%s, %s)\n", tag,
                        opts->wav_out_file, opts->wav_out_fileR);
            rc = 1;
        }
        dsd_stat_t st;
        if (dsd_stat_path(dir, &st) != 0) {
            DSD_FPRINTF(stderr, "%s: the engine did not create the -7 folder\n", tag);
            rc = 1;
        }
        if (dsd_stat_path("WAV", &st) == 0) {
            DSD_FPRINTF(stderr, "%s: ./WAV was created\n", tag);
            rc = 1;
        }

        /* The run's end discards recordings that hold no audio; whatever is left goes. */
        (void)dsd_dir_list(dir, remove_listed_file, dir);
        (void)dsd_test_rmdir(dir);
        char default_dir[] = "WAV";
        (void)dsd_dir_list(default_dir, remove_listed_file, default_dir);
        (void)dsd_test_rmdir(default_dir);
        free_test_runtime(opts, state);
    }
    if (dsd_test_temp_cwd_leave(&cwd) != 0) {
        DSD_FPRINTF(stderr, "per-call start: the temp working directory was left non-empty\n");
        rc = 1;
    }
    return rc;
}

/* Occurrences of @p needle in @p text. */
static int
count_text(const char* text, const char* needle) {
    int count = 0;
    for (const char* at = strstr(text, needle); at; at = strstr(at + 1, needle)) {
        count++;
    }
    return count;
}

/* What the recording journal was when the run started, and the slot recordings open when live processing ended. */
typedef struct {
    int starts;
    int stops;
    int journal_at_start;
    uint32_t failures_at_start;
    char pending_id[64];
    char open_ids[2][64];
} journal_run_probe;

/* The journal id a per-call temp name stands for: TEMP_<date>_<time>_<token>_<seq>[.wav] gives "<token>-<seq>". */
static void
journal_id_of_temp(const char* temp, char* out, size_t cap) {
    out[0] = '\0';
    const char* seq = temp ? strrchr(temp, '_') : NULL;
    if (seq && seq - temp >= 17) {
        const char* dot = strchr(seq, '.');
        const int seq_len = dot ? (int)(dot - seq - 1) : (int)strlen(seq + 1);
        DSD_SNPRINTF(out, cap, "%.16s-%.*s", seq - 16, seq_len, seq + 1);
    }
}

static void
write_slot_voice(dsd_opts* opts, dsd_state* state, int frames) {
    for (int f = 0; f < frames; f++) {
        for (int i = 0; i < 160; i++) {
            state->audio_out_temp_buf[i] = (float)((i % 32) * 512 - 8192);
        }
        dsd_write_mono_voice_to_slot_wav(opts, state, 0);
    }
}

/* At start: the journal's state and the run's failure counts. Then slot 1 records a call and closes its recording
   before the call's row commits (as the EDACS rotation does), so that recording waits for the row, and goes on
   recording the call in a new file, which is still open when the run ends. */
static int
probe_journal_at_start(dsd_opts* opts, dsd_state* state, void* context) {
    journal_run_probe* probe = (journal_run_probe*)context;
    probe->starts++;
    probe->journal_at_start = (int)dsd_wav_journal_state_get();
    dsd_wav_recording_status status;
    dsd_wav_recording_status_get(&status);
    probe->failures_at_start = 0U;
    for (int kind = 0; kind < DSD_WAV_RECORDING_ERROR_KINDS; kind++) {
        probe->failures_at_start += status.failures[kind];
    }
    if (opts->wav_out_f == NULL) {
        return 0;
    }
    const dsd_call_observation call = {
        .protocol = DSD_SYNC_P25P1_POS,
        .slot = 0U,
        .kind = DSD_CALL_KIND_GROUP_VOICE,
        .ota_target_id = 708U,
        .policy_target_id = 708U,
        .ota_source_id = 7U,
    };
    (void)dsd_call_state_observe(state, &call, DSD_CALL_BOUNDARY_BEGIN);
    dsd_event_sync_slot(opts, state, 0);
    write_slot_voice(opts, state, 2);
    journal_id_of_temp(opts->wav_out_file, probe->pending_id, sizeof probe->pending_id);
    opts->wav_out_f = close_and_publish_call_wav(opts->wav_out_f, opts, state, 0, opts->wav_out_file, opts->wav_out_dir,
                                                 opts->wav_out_open_time);
    opts->wav_out_f = open_wav_file(opts->wav_out_dir, opts->wav_out_file, sizeof opts->wav_out_file, 8000, 0);
    opts->wav_out_open_time = dsd_decode_time();
    write_slot_voice(opts, state, 2);
    return 0;
}

static void
probe_journal_at_stop(dsd_opts* opts, dsd_state* state, void* context) {
    (void)state;
    journal_run_probe* probe = (journal_run_probe*)context;
    probe->stops++;
    journal_id_of_temp(opts->wav_out_f ? opts->wav_out_file : NULL, probe->open_ids[0], sizeof probe->open_ids[0]);
    journal_id_of_temp(opts->wav_out_fR ? opts->wav_out_fileR : NULL, probe->open_ids[1], sizeof probe->open_ids[1]);
}

/* The journal line of @p type naming @p id, or NULL. */
static const char*
journal_record_for(const char* text, const char* type, const char* id) {
    char want[160];
    DSD_SNPRINTF(want, sizeof want, "\"type\":\"%s\",", type);
    char want_id[96];
    DSD_SNPRINTF(want_id, sizeof want_id, "\"id\":\"%s\"", id);
    for (const char* line = text; line && *line;) {
        const char* nl = strchr(line, '\n');
        const size_t len = nl ? (size_t)(nl - line) : strlen(line);
        char one[4096];
        DSD_SNPRINTF(one, sizeof one, "%.*s", (int)len, line);
        if (strstr(one, want) != NULL && strstr(one, want_id) != NULL) {
            return line;
        }
        line = nl ? nl + 1 : NULL;
    }
    return NULL;
}

/*
 * Issue #708: the engine's half of the recording journal. With -P and --wav-journal, the run opens the journal before
 * its first recording, in the -7 folder it creates when that is new (the journal used to open first and fail there),
 * and a failure count left by an earlier run reads 0 at start. The journal closes only after the run's last
 * recordings: each slot recording open at the end gets its record (a publish, or a discard for one with no audio),
 * every recording that waited for its row is resolved by a link or an unlink, and the journal is off afterwards.
 */
static int
test_run_journals_its_recordings_from_start_to_end(void) {
    dsd_test_temp_cwd cwd;
    if (dsd_test_temp_cwd_enter(&cwd, "dsdneo_engine_journal") != 0) {
        DSD_FPRINTF(stderr, "run journal: temp working directory failed\n");
        return 1;
    }
    int rc = 0;
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (init_test_runtime(&opts, &state) != 0) {
        (void)dsd_test_temp_cwd_leave(&cwd);
        return 1;
    }
    char dir[DSD_TEST_PATH_MAX];
    char journal_path[DSD_TEST_PATH_MAX * 2];
    rc |= expect_true("run journal: folder path", dsd_test_path_join(dir, sizeof dir, cwd.dir, "calls") == 0);
    rc |= expect_true("run journal: journal path",
                      dsd_test_path_join(journal_path, sizeof journal_path, dir, "recordings.jsonl") == 0);
    char arg0[] = "dsd-neo";
    char opt_p[] = "-P";
    char opt_7[] = "-7";
    char opt_journal[] = "--wav-journal";
    char* argv[] = {arg0, opt_p, opt_7, dir, opt_journal, journal_path, NULL};
    int argc_effective = 0;
    int exit_rc = 0;
    rc |= expect_true("run journal: parse",
                      dsd_parse_args(6, argv, opts, state, &argc_effective, &exit_rc) == DSD_PARSE_CONTINUE);

    (void)dsd_wav_recording_note_failure(DSD_WAV_RECORDING_ERROR_OPEN);
    journal_run_probe probe;
    DSD_MEMSET(&probe, 0, sizeof probe);
    const dsd_engine_lifecycle_hooks hooks = {
        .start = probe_journal_at_start,
        .stop = probe_journal_at_stop,
        .context = &probe,
    };
    rc |= expect_true("run journal: the run", dsd_engine_run_with_lifecycle(opts, state, &hooks) == 0);
    rc |= expect_true("run journal: started and stopped once", probe.starts == 1 && probe.stops == 1);
    rc |= expect_true("run journal: on at start, in the new -7 folder", probe.journal_at_start == DSD_WAV_JOURNAL_ON);
    rc |= expect_true("run journal: an earlier run's failures read 0 at start", probe.failures_at_start == 0U);
    rc |= expect_true("run journal: off after the run", dsd_wav_journal_state_get() == DSD_WAV_JOURNAL_OFF);

    static char text[64 * 4096];
    text[0] = '\0';
    FILE* f = dsd_fopen_existing_regular_file(journal_path, "rb");
    if (f) {
        const size_t n = fread(text, 1, sizeof text - 1, f);
        text[n] = '\0';
        (void)fclose(f);
    }
    const char* pending = probe.pending_id[0] ? journal_record_for(text, "publish", probe.pending_id) : NULL;
    rc |= expect_true("run journal: the recording closed before its row is published waiting for it",
                      pending != NULL && strncmp(strstr(pending, "\"pending\":"), "\"pending\":true", 14) == 0);
    for (const char* at = strstr(text, "\"pending\":true"); at; at = strstr(at + 1, "\"pending\":true")) {
        const char* line = at;
        while (line > text && line[-1] != '\n') {
            line--;
        }
        const char* id_at = strstr(line, "\"id\":\"");
        char id[64] = {0};
        if (id_at) {
            id_at += 6;
            const char* id_end = strchr(id_at, '"');
            DSD_SNPRINTF(id, sizeof id, "%.*s", id_end ? (int)(id_end - id_at) : 0, id_at);
        }
        if (!journal_record_for(text, "link", id) && !journal_record_for(text, "unlink", id)) {
            DSD_FPRINTF(stderr, "run journal: pending recording %s was never linked or unlinked\n", id);
            rc = 1;
        }
    }
    for (int slot = 0; slot < 2; slot++) {
        const char* id = probe.open_ids[slot];
        if (id[0] == '\0' || (!journal_record_for(text, "publish", id) && !journal_record_for(text, "discard", id))) {
            DSD_FPRINTF(stderr, "run journal: slot %d's recording open at the end (%s) has no record\n", slot + 1, id);
            rc = 1;
        }
    }
    if (rc != 0) {
        DSD_FPRINTF(stderr, "run journal:\n%s\n", text);
    }

    (void)dsd_dir_list(dir, remove_listed_file, dir);
    (void)dsd_test_rmdir(dir);
    free_test_runtime(opts, state);
    if (dsd_test_temp_cwd_leave(&cwd) != 0) {
        DSD_FPRINTF(stderr, "run journal: the temp working directory was left non-empty\n");
        rc = 1;
    }
    return rc;
}

/*
 * Issue #527: a -Y channel map that reaches the engine through a config file, which the command line never imported,
 * is weighed against the tone policy when the engine imports it: said exactly once when no row runs the FM monitor
 * (no nfm row, and under -fA no row without a mode of its own, since a typed row runs its own mode: an am row the AM
 * monitor, which hears no tone), and not at all when one does. A map with no rows leaves the scan on the configured
 * decode mode, which is weighed instead: the FM monitor hears tones, a digital mode does not.
 */
static int
test_config_channel_map_weighs_the_tone_filter(void) {
    static const struct {
        const char* body;
        int analog_only;
        int rows;
        int warnings;
    } cases[] = {
        {"channel,frequency_hz,mode\n1,461000000,dmr\n2,851012500,p25\n", 0, 2, 1},
        {"channel,frequency_hz,mode\n1,461000000,dmr\n2,851012500,p25\n", 1, 2, 1},
        {"channel,frequency_hz,mode\n1,461000000,dmr\n2,154430000,nfm\n", 0, 2, 0},
        {"channel,frequency_hz,mode\n1,461000000,dmr\n2,154430000,\n", 1, 2, 0},
        {"channel,frequency_hz\n1,154430000\n2,155475000\n", 1, 2, 0},
        {"channel,frequency_hz,mode\n1,461000000,dmr\n2,118300000,am\n", 1, 2, 1},
        {"channel,frequency_hz,mode\n", 1, 0, 0},
        {"channel,frequency_hz,mode\n", 0, 0, 1},
    };

    static const char* const expected =
        "the tone filter (--tone-allow/--tone-block, [analog] tone_filter) has no effect";
    int test_rc = 0;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        dsd_opts* opts = NULL;
        dsd_state* state = NULL;
        if (init_test_runtime(&opts, &state) != 0) {
            return 1;
        }
        const int fd = dsd_test_mkstemp(opts->chan_in_file, sizeof opts->chan_in_file, "tone-chan-map-");
        if (fd >= 0) {
            dsd_close(fd);
        }
        FILE* fp = fd >= 0 ? dsd_fopen_private(opts->chan_in_file, "w") : NULL;
        if (!fp || fputs(cases[i].body, fp) < 0 || fclose(fp) != 0) {
            DSD_FPRINTF(stderr, "tone map case %zu: could not write the map\n", i);
            free_test_runtime(opts, state);
            return 1;
        }
        opts->scanner_mode = 1;
        opts->analog_only = cases[i].analog_only;
        opts->analog_demod = DSD_ANALOG_DEMOD_FM;
        opts->analog_tone_filter = DSD_TONE_FILTER_ALLOW;
        test_rc |= expect_true("tone map list", dsd_tone_set_parse("100.0", &opts->analog_tone_set, NULL, 0) == 0);
        dsd_test_capture_stderr cap;
        char log[8192] = {0};
        if (dsd_test_capture_stderr_begin(&cap, "tone-chan-map-log") != 0) {
            (void)remove(opts->chan_in_file);
            free_test_runtime(opts, state);
            return 1;
        }
        const int rc = dsd_engine_run_with_lifecycle(opts, state, NULL);
        const int imported = state->lcn_freq_count;
        (void)dsd_test_capture_stderr_end(&cap);
        (void)dsd_test_capture_stderr_read(&cap, log, sizeof log);
        const int warnings = count_text(log, expected);
        if (rc != 0 || imported != cases[i].rows || warnings != cases[i].warnings) {
            DSD_FPRINTF(stderr, "tone map case %zu: rc=%d rows=%d warnings=%d want %d log:\n%s\n", i, rc, imported,
                        warnings, cases[i].warnings, log);
            test_rc = 1;
        }
        (void)remove(opts->chan_in_file);
        free_test_runtime(opts, state);
    }
    return test_rc;
}

/*
 * --squelch noise needs FM (issue #518 follow-up): on the AM monitor (-fM, no scan) a --squelch noise is refused, a
 * noise setting that came from an input spec or the config runs as auto with the same N and says so, and the FM
 * monitor takes it as set. Without a radio input it runs on audio input (issue #628) without a word at startup: the
 * PCM noise squelch says so itself if the input has no room or no band. --squelch auto there is off and says so.
 */
static int
test_noise_squelch_needs_fm(void) {
    static const struct {
        const char* dev; /* NULL: no radio input */
        const char* says;
        const char* omits;
        int am;
        int cli;
        int mode;
        int ok;
    } cases[] = {
        {"soapy:driver=test:118.1M:7:0:24", "--squelch noise needs an FM channel", NULL, 1, 1, DSD_SQUELCH_MODE_NOISE,
         0},
        {"soapy:driver=test:118.1M:7:0:24:noise+12:2", "runs auto+12dB", "--squelch noise needs an FM", 1, 0,
         DSD_SQUELCH_MODE_NOISE, 1},
        {"soapy:driver=test:162.475M:7:0:24", "SQ=noise+12dB", "--squelch noise needs an FM", 0, 1,
         DSD_SQUELCH_MODE_NOISE, 1},
        {NULL, NULL, "needs a radio input", 0, 1, DSD_SQUELCH_MODE_NOISE, 1},
        {NULL, "--squelch auto needs a radio input", "needs an FM", 0, 1, DSD_SQUELCH_MODE_AUTO, 1},
    };

    int test_rc = 0;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        dsd_opts* opts = NULL;
        dsd_state* state = NULL;
        if (init_test_runtime(&opts, &state) != 0) {
            return 1;
        }
        if (cases[i].dev) {
            DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", cases[i].dev);
        }
        opts->analog_only = 1;
        opts->analog_demod = cases[i].am ? DSD_ANALOG_DEMOD_AM : DSD_ANALOG_DEMOD_FM;
        if (cases[i].cli) {
            const dsd_squelch_setting dynamic = dsd_squelch_setting_dynamic(cases[i].mode, 12);
            dsd_squelch_setting_store(opts, &dynamic);
            opts->rtl_squelch_cli_set = 1;
        }
        char banner[8192] = {0};
        int rc = 0;
        if (run_lifecycle_capturing_banner(opts, state, banner, sizeof banner, &rc) != 0) {
            free_test_runtime(opts, state);
            return 1;
        }
        char tag[64];
        DSD_SNPRINTF(tag, sizeof tag, "noise squelch case %zu", i);
        test_rc |= expect_true(tag, (rc == 0) == (cases[i].ok != 0));
        if (cases[i].says) {
            test_rc |= expect_contains(tag, banner, cases[i].says);
        }
        if (cases[i].omits) {
            test_rc |= expect_omits(tag, banner, cases[i].omits);
        }
        /* The setting stays as written: the demodulator resolves it per channel. */
        test_rc |= expect_true(tag, opts->rtl_squelch_mode == cases[i].mode && opts->rtl_squelch_margin_db == 12);
        free_test_runtime(opts, state);
    }
    return test_rc;
}

/* Issue #625: the M17 encoder's VOX and EDACS analog voice say at startup how a dynamic setting runs for them. The
   encoder has no squelch on audio input (VOX stays keyed: warned, without the audio-input advice to use noise); EDACS
   runs NOISE as AUTO on a radio input and neither setting on audio input. */
static int
test_squelch_notices_for_edacs_and_the_encoder(void) {
    static const struct {
        const char* dev; /* NULL: no radio input */
        int encoder;
        int vox;
        int edacs;
        int mode;
        const char* says;
        const char* omits;
    } cases[] = {
        {NULL, 1, 1, 0, DSD_SQUELCH_MODE_AUTO, "WARNING: M17 VOX: --squelch auto runs on a radio input only",
         "On audio input use --squelch noise"},
        {NULL, 1, 1, 0, DSD_SQUELCH_MODE_NOISE, "M17 VOX: --squelch noise runs on a radio input only", NULL},
        {NULL, 1, 0, 0, DSD_SQUELCH_MODE_AUTO, "NOTICE: M17 encoder: --squelch auto runs on a radio input only",
         "M17 VOX"},
        {"soapy:driver=test:162.475M:7:0:24", 1, 1, 0, DSD_SQUELCH_MODE_AUTO, NULL, "radio input only"},
        {"soapy:driver=test:851.0125M:7:0:24", 0, 0, 1, DSD_SQUELCH_MODE_NOISE,
         "EDACS analog voice runs the noise squelch as auto+12dB", NULL},
        {"soapy:driver=test:851.0125M:7:0:24", 0, 0, 1, DSD_SQUELCH_MODE_AUTO, NULL, "EDACS"},
        {NULL, 0, 0, 1, DSD_SQUELCH_MODE_AUTO, "an EDACS analog call ends on its release marker",
         "--squelch auto needs a radio input"},
        /* Auto trunking with no EDACS addressing selected is no EDACS session: the noise squelch is its own. */
        {"soapy:driver=test:851.0125M:7:0:24", 0, 0, 2, DSD_SQUELCH_MODE_NOISE, NULL, "EDACS"},
        {NULL, 0, 0, 2, DSD_SQUELCH_MODE_AUTO, "--squelch auto needs a radio input", "EDACS"},
    };

    int test_rc = 0;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        dsd_opts* opts = NULL;
        dsd_state* state = NULL;
        if (init_test_runtime(&opts, &state) != 0) {
            return 1;
        }
        if (cases[i].dev) {
            DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", cases[i].dev);
        }
        opts->analog_only = 0;
        opts->m17encoder = cases[i].encoder;
        if (cases[i].encoder) {
            DSD_SNPRINTF(state->m17dat, sizeof state->m17dat, "M17:7:SRC:DST:48000:%d", cases[i].vox);
        }
        if (cases[i].edacs == 1) {
            /* -fh -T: the EDACS/ProVoice preset selects standard addressing. */
            (void)dsd_apply_decode_mode_preset(DSDCFG_MODE_EDACS_PV, DSD_DECODE_PRESET_PROFILE_CLI, opts, state);
            opts->trunk_enable = 1;
        } else if (cases[i].edacs == 2) {
            (void)dsd_apply_decode_mode_preset(DSDCFG_MODE_AUTO, DSD_DECODE_PRESET_PROFILE_CLI, opts, state);
            opts->trunk_enable = 1;
        }
        const dsd_squelch_setting dynamic = dsd_squelch_setting_dynamic(cases[i].mode, 12);
        dsd_squelch_setting_store(opts, &dynamic);
        opts->rtl_squelch_cli_set = 1;
        char banner[8192] = {0};
        int rc = 0;
        if (run_lifecycle_capturing_banner(opts, state, banner, sizeof banner, &rc) != 0) {
            free_test_runtime(opts, state);
            return 1;
        }
        char tag[64];
        DSD_SNPRINTF(tag, sizeof tag, "edacs/encoder squelch notice case %zu", i);
        test_rc |= expect_true(tag, rc == 0);
        if (cases[i].says) {
            test_rc |= expect_contains(tag, banner, cases[i].says);
        }
        if (cases[i].omits) {
            test_rc |= expect_omits(tag, banner, cases[i].omits);
        }
        free_test_runtime(opts, state);
    }
    return test_rc;
}

#ifdef DSD_NEO_TEST_RIGCTL_WRAP
static int
note_rigctl_rebinds_at_start(dsd_opts* opts, dsd_state* state, void* context) {
    (void)opts;
    (void)state;
    *(int*)context = g_rigctl_rebind_calls;
    return 0;
}

/* Issue #589: the rigctl connection the engine opens at start names the peer record before the lifecycle starts the
 * P25 watchdog, whose retunes use it. Otherwise the watchdog's first request would reset the record while a TCP audio
 * reconnect's Connect() reads it on the decoder thread. */
static int
test_startup_rigctl_connection_names_the_peer_record(void) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "udp");
    opts->use_rigctl = 1;
    DSD_SNPRINTF(opts->rigctlhostname, sizeof opts->rigctlhostname, "%s", "127.0.0.1");
    opts->rigctlportno = 45321;
    g_rigctl_fake_port = 45321;
    g_rigctl_rebind_calls = 0;
    int rebinds_at_start = -1;
    const dsd_engine_lifecycle_hooks hooks = {.start = note_rigctl_rebinds_at_start, .context = &rebinds_at_start};
    const int result = dsd_engine_run_with_lifecycle(opts, state, &hooks);
    int rc = expect_true("startup rigctl run ok", result == 0);
    rc |= expect_true("startup rigctl connected", opts->use_rigctl == 1 && opts->rigctl_sockfd == (dsd_socket_t)777);
    rc |= expect_true("startup rigctl record named before the lifecycle starts", rebinds_at_start == 1);
    rc |= expect_true("startup rigctl record names the new socket",
                      g_rigctl_rebind_old == DSD_INVALID_SOCKET && g_rigctl_rebind_new == (dsd_socket_t)777);
    g_rigctl_fake_port = 0;
    opts->use_rigctl = 0;
    opts->rigctl_sockfd = DSD_INVALID_SOCKET;
    free_test_runtime(opts, state);
    return rc;
}

/* A 16-bit mono WAV of @p frames silent samples at @p path. */
static int
write_silent_wav(const char* path, uint32_t frames) {
    FILE* fp = dsd_fopen_private(path, "wb");
    if (!fp) {
        return -1;
    }
    const uint32_t rate = 48000U;
    const uint32_t data_bytes = frames * 2U;
    uint8_t header[44];
    DSD_MEMSET(header, 0, sizeof header);
    DSD_MEMCPY(header, "RIFF", 4);
    const uint32_t riff = 36U + data_bytes;
    const uint32_t fields[] = {16U, rate, rate * 2U};
    for (int i = 0; i < 4; i++) {
        header[4 + i] = (uint8_t)(riff >> (8 * i));
    }
    DSD_MEMCPY(header + 8, "WAVEfmt ", 8);
    for (int i = 0; i < 4; i++) {
        header[16 + i] = (uint8_t)(fields[0] >> (8 * i));
        header[24 + i] = (uint8_t)(fields[1] >> (8 * i));
        header[28 + i] = (uint8_t)(fields[2] >> (8 * i));
        header[40 + i] = (uint8_t)(data_bytes >> (8 * i));
    }
    header[20] = 1U; /* PCM */
    header[22] = 1U; /* mono */
    header[32] = 2U; /* block align */
    header[34] = 16U;
    DSD_MEMCPY(header + 36, "data", 4);
    int rc = fwrite(header, 1, sizeof header, fp) == sizeof header ? 0 : -1;
    for (uint32_t i = 0; rc == 0 && i < data_bytes; i++) {
        rc = fputc(0, fp) == EOF ? -1 : 0;
    }
    if (fclose(fp) != 0) {
        rc = -1;
    }
    return rc;
}

/* The lifecycle start: its place in the sequence, and the run stops at once. */
static int
stop_the_run_at_start(dsd_opts* opts, dsd_state* state, void* context) {
    (void)opts;
    (void)state;
    (void)context;
    g_lifecycle_seq = ++g_order_seq;
    dsd_exitflag_store(1);
    return 0;
}

/* Issue #621: a session that starts on the FM monitor of audio input with a rigctl peer and a configured NFM width asks
 * the peer for that width once the input is open, before the lifecycle starts the frontends and before the P25
 * watchdog starts, whose retunes would otherwise race it. */
/* One run of a -fA WAV session with a configured NFM width and a rigctl peer, stopped at the lifecycle start; with
   @p untyped_list the session scans an all-blank -Y list (two frequencies, no modes), the legacy leg's. @p tag names
   the case in the checks. */
static int
startup_passband_case(const char* tag, int untyped_list) {
    char dir[DSD_TEST_PATH_MAX];
    char wav[DSD_TEST_PATH_MAX];
    if (!dsd_test_mkdtemp(dir, sizeof dir, "dsdneo_run_setup_passband")) {
        return 1;
    }
    static const char* const files[] = {"in.wav", NULL};
    if (dsd_test_path_join(wav, sizeof wav, dir, "in.wav") != 0 || write_silent_wav(wav, 480U) != 0) {
        (void)dsd_test_remove_temp_dir(dir, files);
        return 1;
    }
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (init_test_runtime(&opts, &state) != 0) {
        (void)dsd_test_remove_temp_dir(dir, files);
        return 1;
    }
    opts->playfiles = 0;
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", wav);
    opts->analog_only = 1;
    opts->monitor_input_audio = 1;
    opts->analog_demod = DSD_ANALOG_DEMOD_FM;
    opts->analog_nfm_bandwidth_hz = 12500;
    opts->setmod_bw = 7000;
    opts->use_rigctl = 1;
    DSD_SNPRINTF(opts->rigctlhostname, sizeof opts->rigctlhostname, "%s", "127.0.0.1");
    opts->rigctlportno = 45322;
    if (untyped_list) {
        opts->scanner_mode = 1;
        state->trunk_lcn_freq[0] = 155475000;
        state->trunk_lcn_freq[1] = 155500000;
        state->lcn_freq_count = 2;
    }
    g_rigctl_fake_port = 45322;
    g_run_opts = opts;
    g_order_seq = 0;
    g_ask_calls = 0;
    g_ask_seq = 0;
    g_watchdog_seq = 0;
    g_lifecycle_seq = 0;
    g_session_marks = 0;
    g_session_mark_kind = -1;
    const dsd_engine_lifecycle_hooks hooks = {.start = stop_the_run_at_start};
    const int result = dsd_engine_run_with_lifecycle(opts, state, &hooks);
    dsd_exitflag_store(0);
    char label[96];
    DSD_SNPRINTF(label, sizeof label, "%s run ok", tag);
    int rc = expect_true(label, result == 0);
    DSD_SNPRINTF(label, sizeof label, "%s asked once", tag);
    rc |= expect_true(label, g_ask_calls == 1);
    DSD_SNPRINTF(label, sizeof label, "%s is the configured NFM width", tag);
    rc |= expect_true(label, g_ask_kind == DSD_ANALOG_DEMOD_FM && g_ask_bw == 12500);
    DSD_SNPRINTF(label, sizeof label, "%s asked once the input is open", tag);
    rc |= expect_true(label, g_ask_input_open == 1);
    /* No typed -Y list or trunk scan: the passband is the session's (issue #621), and so is the untyped list's, which
       holds no scan scope and has no leave restore. */
    DSD_SNPRINTF(label, sizeof label, "%s is the session's", tag);
    rc |= expect_true(label, g_session_marks == 1 && g_session_mark_kind == DSD_ANALOG_DEMOD_FM);
    DSD_SNPRINTF(label, sizeof label, "%s asked before the lifecycle starts", tag);
    rc |= expect_true(label, g_lifecycle_seq > 0 && g_ask_seq > 0 && g_ask_seq < g_lifecycle_seq);
    DSD_SNPRINTF(label, sizeof label, "%s asked before the watchdog starts", tag);
    rc |= expect_true(label, g_watchdog_seq > 0 && g_ask_seq > 0 && g_ask_seq < g_watchdog_seq);
    g_run_opts = NULL;
    g_rigctl_fake_port = 0;
    opts->use_rigctl = 0;
    opts->rigctl_sockfd = DSD_INVALID_SOCKET;
    free_test_runtime(opts, state);
    DSD_SNPRINTF(label, sizeof label, "%s temp dir removed", tag);
    rc |= expect_true(label, dsd_test_remove_temp_dir(dir, files) == 0);
    return rc;
}

static int
test_startup_asks_the_session_passband(void) {
    int rc = startup_passband_case("startup passband", 0);
    rc |= startup_passband_case("startup passband on an untyped -Y list", 1);
    return rc;
}
#endif

/* What output setup left open when the decoder started: the UDP analog socket (port + 2) and the -8 source monitor. */
typedef struct {
    int analog_socket;
    int monitor;
} udp_analog_seen;

static int
note_udp_analog_at_start(dsd_opts* opts, dsd_state* state, void* context) {
    (void)state;
    udp_analog_seen* seen = (udp_analog_seen*)context;
    seen->analog_socket = opts->udp_sockfdA != DSD_INVALID_SOCKET;
    seen->monitor = opts->monitor_input_audio;
    return 0;
}

enum { UDP_SESSION_AUTO, UDP_SESSION_AUTO_EDACS, UDP_SESSION_EDACS };

/* `-o udp:127.0.0.1:23456` for @p kind (Auto with no EDACS addressing, -fh -fa, -fh), trunked or not, with -8 or
   not. */
static int
run_udp_output_session(int kind, int trunk, int monitor, udp_analog_seen* seen) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }
    if (kind == UDP_SESSION_AUTO_EDACS || kind == UDP_SESSION_EDACS) {
        (void)dsd_apply_decode_mode_preset(DSDCFG_MODE_EDACS_PV, DSD_DECODE_PRESET_PROFILE_CLI, opts, state);
    }
    if (kind == UDP_SESSION_AUTO || kind == UDP_SESSION_AUTO_EDACS) {
        (void)dsd_apply_decode_mode_preset(DSDCFG_MODE_AUTO, DSD_DECODE_PRESET_PROFILE_CLI, opts, state);
    }
    opts->trunk_enable = trunk;
    opts->monitor_input_audio = monitor;
    DSD_SNPRINTF(opts->audio_out_dev, sizeof opts->audio_out_dev, "%s", "udp:127.0.0.1:23456");
    const dsd_engine_lifecycle_hooks hooks = {.start = note_udp_analog_at_start, .context = seen};
    const int rc = dsd_engine_run_with_lifecycle(opts, state, &hooks);
    free_test_runtime(opts, state);
    return expect_true("udp output session runs", rc == 0);
}

/*
 * Auto enables ProVoice, but a session with no EDACS addressing selected opens no UDP analog socket on port + 2 and,
 * trunked, keeps -8 on; an EDACS session (-fh -fa, -fh) opens the socket and, trunked, turns -8 off as before.
 */
static int
test_udp_analog_socket_follows_the_edacs_session(void) {
    int rc = 0;
    udp_analog_seen seen = {-1, -1};
    rc |= run_udp_output_session(UDP_SESSION_AUTO, 0, 0, &seen);
    rc |= expect_true("auto opens no udp analog socket", seen.analog_socket == 0);
    rc |= run_udp_output_session(UDP_SESSION_AUTO, 1, 1, &seen);
    rc |= expect_true("auto -T -8 opens the socket -8 writes", seen.analog_socket == 1);
    rc |= expect_true("auto -T keeps -8", seen.monitor == 1);
    rc |= run_udp_output_session(UDP_SESSION_AUTO_EDACS, 0, 0, &seen);
    rc |= expect_true("-fh -fa opens the udp analog socket", seen.analog_socket == 1);
    rc |= run_udp_output_session(UDP_SESSION_EDACS, 1, 1, &seen);
    rc |= expect_true("-fh -T opens the udp analog socket", seen.analog_socket == 1);
    rc |= expect_true("-fh -T turns -8 off", seen.monitor == 0);
    return rc;
}

#ifdef DSD_NEO_TEST_RIGCTL_WRAP
/* The connects a `tcp:` input made against a refusing server before setup gave up, for the decoders @p mode sets. */
static int
run_refused_tcp_input(dsdneoUserDecodeMode mode, int* connects, int* rc_out) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }
    (void)dsd_apply_decode_mode_preset(mode, DSD_DECODE_PRESET_PROFILE_CLI, opts, state);
    g_tcp_refusing_port = 45323;
    g_tcp_refused_connects = 0;
    dsd_exitflag_store(0);
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "tcp:127.0.0.1:45323");
    *rc_out = dsd_engine_run_with_lifecycle(opts, state, NULL);
    *connects = g_tcp_refused_connects;
    g_tcp_refusing_port = 0;
    dsd_exitflag_store(0);
    free_test_runtime(opts, state);
    return 0;
}

/* A TCP audio connect that fails is an error at once, under Auto too; only an M17-only session (-fz) waits for its
   server, retrying. */
static int
test_tcp_connect_retry_is_m17_only(void) {
    int connects = 0;
    int run_rc = 0;
    int rc = run_refused_tcp_input(DSDCFG_MODE_AUTO, &connects, &run_rc);
    rc |= expect_true("auto tcp connect failure rejects setup", run_rc != 0);
    rc |= expect_true("auto tries the tcp server once", connects == 1);
    rc |= run_refused_tcp_input(DSDCFG_MODE_M17, &connects, &run_rc);
    rc |= expect_true("-fz retries the tcp server", connects == 3);
    return rc;
}
#endif

int
main(void) {
    int rc = 0;
    rc |= test_missing_source_labels_do_not_stop_decode();
    rc |= test_per_call_wav_opens_in_the_cli_folder_at_start();
    rc |= test_run_journals_its_recordings_from_start_to_end();
    rc |= test_conflicting_scan_modes_fail_before_live_setup();
    rc |= test_m17_udp_input_and_output_specs();
    rc |= test_m17_userdata_is_normalized_during_common_setup();
    rc |= test_m17_userdata_refuses_unsupported_input_rates();
    rc |= test_m17_stream_encoder_rejects_unsupported_input();
#if defined(USE_RADIO) && defined(DSD_NEO_TEST_RTL_START_WRAP)
    rc |= test_m17_stream_encoder_propagates_rtl_start_failures();
    rc |= test_rtl_run_clears_a_stale_replay_centre();
#endif
    rc |= test_udp_input_defaults_and_null_output();
    rc |= test_udp_output_connection_failure_is_fatal();
    rc |= test_tcp_connection_failure_is_fatal();
#ifndef USE_RADIO
    rc |= test_unavailable_radio_inputs_are_rejected();
#endif
    rc |= test_rtltcp_tuning_tokens_and_bias();
    rc |= test_rtltcp_bias_spellings();
    rc |= test_trunk_scan_turns_off_rtl_udp_control();
    rc |= test_rtltcp_invalid_and_partial_tuning_tokens();
    rc |= test_soapy_setup_normalizes_args_and_tuning();
    rc |= test_iq_replay_guard_and_requested_setup();
    rc |= test_lifecycle_hooks_start_after_setup_and_stop_before_cleanup();
#ifdef SIGHUP
    rc |= test_hangup_shuts_down_like_an_interrupt();
#endif
    rc |= test_stdout_audio_preserves_bytes();
#ifdef DSD_NEO_TEST_RIGCTL_WRAP
    rc |= test_startup_rigctl_connection_names_the_peer_record();
    rc |= test_startup_asks_the_session_passband();
#endif
    rc |= test_receiver_failure_is_not_successful_completion();
    rc |= test_config_channel_map_weighs_the_tone_filter();
    rc |= test_noise_squelch_needs_fm();
    rc |= test_squelch_notices_for_edacs_and_the_encoder();
    rc |= test_udp_analog_socket_follows_the_edacs_session();
#ifdef DSD_NEO_TEST_RIGCTL_WRAP
    rc |= test_tcp_connect_retry_is_m17_only();
#endif

    if (rc == 0) {
        printf("ENGINE_RUN_SETUP: OK\n");
    }
    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
