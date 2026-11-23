// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * A rigctl peer's confirmed retune starts a new symbol timeline (issue #834), as an RTL retune's stream flush does: the
 * real rigctl client talks to a peer on a loopback socket, which takes, refuses or drops each frequency request.
 * Every rigctl leg is covered: the backend tune of a trunk grant, a voice-channel hop, a control-channel tune and a
 * return (on and off the decoder thread), the legacy control-channel return of the no-carrier pass, and app-control's
 * io_control leg of a trunked manual return. A tune to the frequency the peer is on, a refused request and a lost
 * reply move nothing. The spans the timeline guards are checked through their own readers: a DMR slot's MI chain after
 * a return, and a P25 Phase 1 terminator's place. The confirmation also asks the audio input to drop what it holds from
 * before the retune (the UDP ring, the TCP reader and socket), which the decoder thread's next input read carries out,
 * keeping what arrives after it; a WAV file is read on as it was.
 */

#include <dsd-neo/app_control/commands.h>
#include <dsd-neo/app_control/frontend_runtime.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/dsp/resampler.h>
#include <dsd-neo/dsp/symbol.h>
#include <dsd-neo/engine/frame_processing.h>
#include <dsd-neo/engine/trunk_tuning.h>
#include <dsd-neo/io/rigctl_client.h>
#include <dsd-neo/io/tcp_input.h>
#include <dsd-neo/io/udp_input.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/platform.h>
#include <dsd-neo/platform/sockets.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/protocol/dmr/dmr_trunk_sm.h>
#include <dsd-neo/protocol/p25/p25_sm_watchdog.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/exitflag.h>
#include <dsd-neo/runtime/frame_sync_hooks.h>
#include <dsd-neo/runtime/net_audio_input_hooks.h>
#include <dsd-neo/runtime/trunk_scan_hooks.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <errno.h>
#include <sndfile.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "commands_internal.h"
#include "dmr_vcrypt.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "edacs_internal.h"
#include "engine_hooks_install.h"
#include "p25p1_terminator_end.h"
#include "pcm_input_staging.h"
#include "services.h"
#include "test_env.h"
#include "test_support.h"

#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/input_backlog.h>
#include <dsd-neo/platform/pcm_stream.h>
#if DSD_PLATFORM_WIN_NATIVE
#include <fcntl.h>
#include <io.h>
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#define CC_HZ  451012500L
#define VC1_HZ 452012500L
#define VC2_HZ 452037500L
#define VC3_HZ 452062500L

static const uint64_t k_t0_ns = 1000000ULL * 1000000000ULL;

/* Set while a case is run to fail on purpose (test_a_producer_that_cannot_start_ends_its_case()): its failures are
   reported as expected, not as FAIL lines. */
static int g_failure_expected = 0;

static int
expect_true(const char* what, int cond) {
    if (!cond) {
        DSD_FPRINTF(stderr, "%s: %s\n", g_failure_expected ? "expected failure" : "FAIL", what);
        return 1;
    }
    return 0;
}

/* The next this many producer threads a case starts (start_producer()) fail to start, as on a host out of threads. */
static int g_producers_to_fail = 0;

/* Start a case's producer thread: 0, or nonzero when it did not start (nothing to join). */
static int
start_producer(dsd_thread_t* th,
               DSD_THREAD_RETURN_TYPE(
#if DSD_PLATFORM_WIN_NATIVE
                   __stdcall
#endif
                       * fn)(void*),
               void* arg) {
    if (g_producers_to_fail > 0) {
        g_producers_to_fail--;
        return -1;
    }
    return dsd_thread_create(th, fn, arg);
}

/* ---- A rigctl peer on a loopback socket ---- */

/* What the peer does with a frequency request (F). The RIG_MOVES_* modes apply the frequency, then answer once with a
   reply the client cannot take as a complete RPRT (a garbled line, a malformed status); the next request is taken. */
enum {
    RIG_TAKES = 0,
    RIG_REFUSES = 1,
    RIG_DROPS = 2,
    RIG_MOVES_GARBLED = 3,
    RIG_MOVES_MALFORMED = 4,
};

typedef struct {
    dsd_socket_t listener;
    dsd_socket_t conn;
    dsd_thread_t thread;
    int started;
    atomic_int f_mode;
    atomic_int f_taken; /* F requests answered RPRT 0 */
    /* The audio input's write end (-1: none). A peer that applies a frequency before it answers sends the new channel's
       audio there first: two samples from f_audio_value (0: none), which then moves on by two. */
    atomic_int audio_fd;
    atomic_int f_audio_value;
    /* A passband request (M) is answered while the peer is still on the old frequency: with m_audio_value non-zero the
       old channel's next two samples, from m_audio_value, which then moves on by two, reach the audio input before the
       reply does. m_taken counts the M requests. */
    atomic_int m_audio_value;
    atomic_int m_taken;
} fake_rig;

/* Two samples reach the audio input before the peer's reply does, numbered from *@p next_value (0: none): the new
   channel's first two once it applied a frequency, the old channel's while it sets a passband. */
static void
fake_rig_send_audio(fake_rig* rig, atomic_int* next_value) {
    const int fd = atomic_load(&rig->audio_fd);
    if (fd < 0 || atomic_load(next_value) == 0) {
        return;
    }
    const int value = atomic_fetch_add(next_value, 2);
    const uint8_t b[4] = {(uint8_t)((unsigned)value & 0xFFU), (uint8_t)(((unsigned)value >> 8) & 0xFFU),
                          (uint8_t)((unsigned)(value + 1) & 0xFFU), (uint8_t)(((unsigned)(value + 1) >> 8) & 0xFFU)};
    (void)dsd_write(fd, b, sizeof b);
}

/* One line of the peer's protocol: the reply to @p line, or NULL to drop the connection. */
static const char*
fake_rig_reply(fake_rig* rig, const char* line, long* freq, char* buf, size_t cap) {
    if (line[0] == 'F') {
        const int mode = atomic_load(&rig->f_mode);
        if (mode == RIG_DROPS) {
            return NULL;
        }
        if (mode == RIG_REFUSES) {
            return "RPRT -1\n";
        }
        char* end = NULL;
        errno = 0;
        const long hz = strtol(line + 1, &end, 10);
        if (end != line + 1 && errno == 0) {
            *freq = hz;
        }
        (void)atomic_fetch_add(&rig->f_taken, 1);
        fake_rig_send_audio(rig, &rig->f_audio_value);
        if (mode >= RIG_MOVES_GARBLED) {
            atomic_store(&rig->f_mode, RIG_TAKES);
        }
        if (mode == RIG_MOVES_GARBLED) {
            return "?\n";
        }
        if (mode == RIG_MOVES_MALFORMED) {
            return "RPRT x\n";
        }
        return "RPRT 0\n";
    }
    if (line[0] == 'M') {
        (void)atomic_fetch_add(&rig->m_taken, 1);
        fake_rig_send_audio(rig, &rig->m_audio_value);
        return "RPRT 0\n";
    }
    if (line[0] == 'f') {
        DSD_SNPRINTF(buf, cap, "%ld\n", *freq);
        return buf;
    }
    if (line[0] == 'm') {
        return "FM\n12500\n";
    }
    return "RPRT 0\n";
}

static DSD_THREAD_RETURN_TYPE
#if DSD_PLATFORM_WIN_NATIVE
    __stdcall
#endif
    fake_rig_main(void* arg) {
    fake_rig* rig = (fake_rig*)arg;
    rig->conn = dsd_socket_accept(rig->listener, NULL, NULL);
    char line[256];
    size_t len = 0U;
    long freq = 0;
    char buf[64];
    while (rig->conn != DSD_INVALID_SOCKET) {
        char c = 0;
        if (dsd_socket_recv(rig->conn, &c, 1U, 0) <= 0) {
            break;
        }
        if (c != '\n') {
            if (len < sizeof line - 1U) {
                line[len++] = c;
            }
            continue;
        }
        line[len] = '\0';
        len = 0U;
        const char* reply = fake_rig_reply(rig, line, &freq, buf, sizeof buf);
        if (reply == NULL || dsd_socket_send(rig->conn, reply, strlen(reply), 0) < 0) {
            break;
        }
    }
    if (rig->conn != DSD_INVALID_SOCKET) {
        (void)dsd_socket_close(rig->conn);
        rig->conn = DSD_INVALID_SOCKET;
    }
    DSD_THREAD_RETURN;
}

/* Start the peer and connect @p opts to it as the session's rigctl peer. */
static int
fake_rig_open(fake_rig* rig, dsd_opts* opts) {
    DSD_MEMSET(rig, 0, sizeof(*rig));
    rig->conn = DSD_INVALID_SOCKET;
    atomic_store(&rig->f_mode, RIG_TAKES);
    atomic_store(&rig->f_taken, 0);
    atomic_store(&rig->audio_fd, -1);
    atomic_store(&rig->f_audio_value, 0);
    atomic_store(&rig->m_audio_value, 0);
    atomic_store(&rig->m_taken, 0);
    rig->listener = dsd_socket_create(AF_INET, SOCK_STREAM, 0);
    if (rig->listener == DSD_INVALID_SOCKET) {
        return -1;
    }
    struct sockaddr_in addr;
    DSD_MEMSET(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    struct sockaddr_in bound;
    DSD_MEMSET(&bound, 0, sizeof bound);
#if DSD_PLATFORM_WIN_NATIVE
    int bound_len = (int)sizeof bound;
#else
    socklen_t bound_len = (socklen_t)sizeof bound;
#endif
    if (dsd_socket_bind(rig->listener, (struct sockaddr*)&addr, sizeof addr) != 0
        || getsockname(rig->listener, (struct sockaddr*)&bound, &bound_len) != 0
        || dsd_socket_listen(rig->listener, 1) != 0) {
        return -1;
    }
    if (dsd_thread_create(&rig->thread, fake_rig_main, rig) != 0) {
        return -1;
    }
    rig->started = 1;
    char host[] = "127.0.0.1";
    opts->rigctl_sockfd = Connect(host, (int)ntohs(bound.sin_port));
    if (opts->rigctl_sockfd == DSD_INVALID_SOCKET) {
        return -1;
    }
    RigctlRebindPeer(DSD_INVALID_SOCKET, opts->rigctl_sockfd, 0);
    /* A new connection, as rigctl_connect.c makes one: the engine's legacy legs forget what they last sent. */
    dsd_engine_legacy_tune_caches_forget();
    opts->use_rigctl = 1;
    opts->rigctl_sent_freq_hz = 0U;
    return 0;
}

static void
fake_rig_close(fake_rig* rig, dsd_opts* opts) {
    if (opts->rigctl_sockfd != DSD_INVALID_SOCKET) {
        (void)dsd_socket_close(opts->rigctl_sockfd);
        opts->rigctl_sockfd = DSD_INVALID_SOCKET;
    }
    if (rig->started) {
        (void)dsd_thread_join(rig->thread);
    }
    (void)dsd_socket_close(rig->listener);
    opts->use_rigctl = 0;
}

/* The P25 watchdog's retune, confirmed the moment it was asked for: its mark, posted at once. */
static void
watchdog_retune_now(void) {
    dsd_input_cut cut;
    dsd_input_backlog_mark(&cut);
    dsd_frame_sync_note_off_thread_unflushed_retune(&cut);
}

/* ---- Fixture ---- */

typedef struct {
    dsd_opts* opts;
    dsd_state* state;
    fake_rig rig;
} rig_case;

static int
rig_case_open(rig_case* c, int audio_in_type) {
    c->opts = (dsd_opts*)calloc(1, sizeof(*c->opts));
    c->state = (dsd_state*)calloc(1, sizeof(*c->state));
    if (c->opts == NULL || c->state == NULL) {
        free(c->opts);
        free(c->state);
        return -1;
    }
    initOpts(c->opts);
    initState(c->state);
    c->opts->audio_in_type = audio_in_type;
    c->opts->trunk_enable = 1;
    c->opts->setmod_bw = 0;
    c->state->trunk_cc_freq = CC_HZ;
    dsd_trunk_tuning_requests_reset();
    return fake_rig_open(&c->rig, c->opts);
}

static void
rig_case_close(rig_case* c) {
    fake_rig_close(&c->rig, c->opts);
    freeState(c->state);
    free(c->state);
    free(c->opts);
}

/* Whether a tune moved the symbol clock epoch from @p before, which it must when @p moves. */
static int
expect_epoch(const char* what, const dsd_state* state, uint32_t before, int moves) {
    return expect_true(what, moves ? state->symbol_clock_epoch != before : state->symbol_clock_epoch == before);
}

/* ---- Cases ---- */

/* Every trunk-hook tune the peer confirmed moves the epoch: a grant, a hop with no release, a control-channel tune and
   a return. A tune to the frequency the peer is on, a refusal and a lost reply move nothing. */
static int
test_trunk_tunes(void) {
    rig_case c;
    if (rig_case_open(&c, AUDIO_IN_PULSE) != 0) {
        return expect_true("trunk tunes: the peer", 0);
    }
    dsd_opts* opts = c.opts;
    dsd_state* state = c.state;
    int rc = 0;
    uint32_t e = state->symbol_clock_epoch;
    rc |= expect_true("grant",
                      dsd_engine_trunk_tune_to_freq_request(opts, state, VC1_HZ, 0, 0U) == DSD_TRUNK_TUNE_RESULT_OK);
    rc |= expect_epoch("a grant's confirmed tune moves the epoch", state, e, 1);
    e = state->symbol_clock_epoch;
    rc |= expect_true("hop",
                      dsd_engine_trunk_tune_to_freq_request(opts, state, VC2_HZ, 0, 0U) == DSD_TRUNK_TUNE_RESULT_OK);
    rc |= expect_epoch("a voice-channel hop with no release moves the epoch", state, e, 1);
    e = state->symbol_clock_epoch;
    const int taken = atomic_load(&c.rig.f_taken);
    rc |= expect_true("same carrier",
                      dsd_engine_trunk_tune_to_freq_request(opts, state, VC2_HZ, 0, 0U) == DSD_TRUNK_TUNE_RESULT_OK);
    rc |= expect_true("a tune to the carrier the peer is on sends nothing", atomic_load(&c.rig.f_taken) == taken);
    rc |= expect_epoch("and moves nothing", state, e, 0);
    rc |= expect_true("cc tune",
                      dsd_engine_trunk_tune_to_cc_request(opts, state, CC_HZ, 0, 0U) == DSD_TRUNK_TUNE_RESULT_OK);
    rc |= expect_epoch("a control-channel tune moves the epoch", state, e, 1);
    rc |= expect_true("grant again",
                      dsd_engine_trunk_tune_to_freq_request(opts, state, VC1_HZ, 0, 0U) == DSD_TRUNK_TUNE_RESULT_OK);
    e = state->symbol_clock_epoch;
    rc |= expect_true("return", dsd_engine_return_to_cc_request(opts, state, 0U) == DSD_TRUNK_TUNE_RESULT_OK);
    rc |= expect_epoch("a return to the control channel moves the epoch", state, e, 1);

    e = state->symbol_clock_epoch;
    atomic_store(&c.rig.f_mode, RIG_REFUSES);
    rc |= expect_true("refused", dsd_engine_trunk_tune_to_freq_request(opts, state, VC3_HZ, 0, 0U)
                                     == DSD_TRUNK_TUNE_RESULT_FAILED);
    rc |= expect_epoch("a refused request moves nothing", state, e, 0);
    atomic_store(&c.rig.f_mode, RIG_DROPS);
    rc |= expect_true("lost reply", dsd_engine_trunk_tune_to_freq_request(opts, state, VC3_HZ, 0, 0U)
                                        == DSD_TRUNK_TUNE_RESULT_FAILED);
    rc |= expect_epoch("a lost reply moves nothing", state, e, 0);
    rig_case_close(&c);
    return rc;
}

/* The P25 watchdog's tune runs off the decoder thread: the epoch moves when the decoder thread reconciles it. */
static int
test_off_thread_tune(void) {
    rig_case c;
    if (rig_case_open(&c, AUDIO_IN_PULSE) != 0) {
        return expect_true("off thread: the peer", 0);
    }
    dsd_frame_sync_off_thread_tunes_reset(c.state);
    const uint32_t e = c.state->symbol_clock_epoch;
    dsd_engine_set_decoder_thread(0);
    int rc = expect_true("watchdog grant", dsd_engine_trunk_tune_to_freq_request(c.opts, c.state, VC1_HZ, 0, 0U)
                                               == DSD_TRUNK_TUNE_RESULT_OK);
    dsd_engine_set_decoder_thread(1);
    rc |= expect_epoch("off the decoder thread the tune leaves the epoch alone", c.state, e, 0);
    rc |= expect_true("the decoder thread reconciles the tune", dsd_frame_sync_apply_off_thread_tunes(c.opts, c.state));
    rc |= expect_epoch("and the epoch moves then", c.state, e, 1);
    rc |= expect_true("with the input drop asked for", c.state->input_drop_pending == 1U);
    rig_case_close(&c);
    return rc;
}

/* The no-carrier pass's own control-channel return (the legacy rigctl leg) moves the epoch: on audio input, and on an
   RTL input rigctl alone retunes, since no RTL leg follows that return and nothing flushes the stream. */
static int
test_legacy_cc_return_on(int audio_in_type) {
    rig_case c;
    if (rig_case_open(&c, audio_in_type) != 0) {
        return expect_true("legacy return: the peer", 0);
    }
    dsd_opts* opts = c.opts;
    dsd_state* state = c.state;
    opts->trunk_is_tuned = 1;
    state->lastsynctype = DSD_SYNC_DMR_BS_DATA_POS;
    state->last_vc_sync_time = 0;
    const uint32_t e = state->symbol_clock_epoch;
    noCarrier(opts, state);
    int rc = expect_true("the pass returned to the control channel by rigctl",
                         atomic_load(&c.rig.f_taken) == 1 && opts->rigctl_sent_freq_hz == (uint32_t)CC_HZ);
    rc |=
        expect_epoch(audio_in_type == AUDIO_IN_RTL ? "the legacy return on an RTL input rigctl retunes moves the epoch"
                                                   : "the legacy return moves the epoch",
                     state, e, 1);
    rig_case_close(&c);
    return rc;
}

static int
test_legacy_cc_return(void) {
    return test_legacy_cc_return_on(AUDIO_IN_WAV) | test_legacy_cc_return_on(AUDIO_IN_RTL);
}

/* App-control's io_control leg: a trunked manual return (no P25 control channel, so not the trunk hook) moves the
   epoch, though it runs no carrier boundary. */
static int
test_manual_return_io_leg(void) {
    rig_case c;
    if (rig_case_open(&c, AUDIO_IN_PULSE) != 0) {
        return expect_true("manual return: the peer", 0);
    }
    dsd_opts* opts = c.opts;
    dsd_state* state = c.state;
    opts->frame_p25p1 = 0;
    opts->frame_p25p2 = 0;
    dsd_app_frontend_runtime_start(opts, state);
    const uint32_t e = state->symbol_clock_epoch;
    int rc = expect_true("posted", dsd_app_command_action(DSD_APP_CMD_RETURN_CC) > 0);
    (void)dsd_app_drain_cmds(opts, state);
    rc |= expect_true("the return tuned the peer", atomic_load(&c.rig.f_taken) == 1);
    rc |= expect_epoch("a manual return on the io_control leg moves the epoch", state, e, 1);
    dsd_app_frontend_runtime_stop();
    rig_case_close(&c);
    return rc;
}

/* A DMR slot's MI chain does not reach across a return to the control channel (a rest channel carries voice): the
   first voice sync after it finds the chain lost, so a call joined there late is muted until it is keyed again, as an
   RTL stream flush leaves it. */
static int
test_dmr_chain_after_return(void) {
    rig_case c;
    if (rig_case_open(&c, AUDIO_IN_PULSE) != 0) {
        return expect_true("dmr chain: the peer", 0);
    }
    dsd_opts* opts = c.opts;
    dsd_state* state = c.state;
    dsd_engine_trunk_tuning_hooks_install();
    int rc = expect_true("on the voice channel",
                         dsd_engine_trunk_tune_to_freq_request(opts, state, VC1_HZ, 0, 0U) == DSD_TRUNK_TUNE_RESULT_OK);
    state->payload_algid = 0x21;
    state->symbol_clock = 100000U;
    dmr_vcrypt_note_first(state, 0U);
    state->symbol_clock += 20U;
    dmr_vcrypt_superframe_start(state, 0U);
    rc |= expect_true("the chain runs",
                      state->dmr_vc_ref_state[0] == DMR_VC_REF_VALID && state->dmr_vc_untrusted[0] == 0U);
    dmr_sm_ctx_t ctx;
    dmr_sm_init_ctx(&ctx, opts, state);
    rc |= expect_true("the return", dmr_sm_return_to_cc(&ctx, opts, state, CC_HZ) == DSD_TRUNK_TUNE_RESULT_OK);
    state->symbol_clock += (uint64_t)5U * DMR_VC_SUPERFRAME_SYMBOLS;
    dmr_vcrypt_superframe_start(state, 0U);
    rc |= expect_true("the first voice sync after the return finds the chain lost", state->dmr_vc_untrusted[0] == 1U);
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
    rig_case_close(&c);
    return rc;
}

/* A P25 Phase 1 terminator read where the last end predicts, after a rigctl tune, is no repeat of that end. */
static int
test_p25p1_terminator_after_tune(void) {
    rig_case c;
    if (rig_case_open(&c, AUDIO_IN_PULSE) != 0) {
        return expect_true("p25 terminator: the peer", 0);
    }
    dsd_opts* opts = c.opts;
    dsd_state* state = c.state;
    const dsd_call_observation call = {.protocol = DSD_SYNC_P25P1_POS,
                                       .slot = 0U,
                                       .kind = DSD_CALL_KIND_GROUP_VOICE,
                                       .ota_target_id = 300U,
                                       .policy_target_id = 300U,
                                       .ota_source_id = 30U};
    (void)dsd_call_state_observe(state, &call, DSD_CALL_BOUNDARY_BEGIN);
    dsd_call_snapshot snap;
    int rc = expect_true("the call", dsd_call_state_get(state, 0U, &snap) > 0);
    (void)dsd_call_state_end_ex(state, 0U, 0.0, DSD_CALL_END_EXPLICIT);
    const uint64_t clock = 50000U;
    const uint64_t epoch = p25p1_terminator_note_end(state, snap.epoch, clock, 72U);
    rc |= expect_true("the TDU ended the call", epoch == snap.epoch && epoch != 0U);
    rc |= expect_true("a repeat back to back follows it", p25p1_terminator_follows_the_last_end(state, clock + 72U));
    rc |= expect_true("the tune",
                      dsd_engine_trunk_tune_to_freq_request(opts, state, VC1_HZ, 0, 0U) == DSD_TRUNK_TUNE_RESULT_OK);
    rc |= expect_true("after a rigctl tune, a terminator at that place is the next carrier's",
                      !p25p1_terminator_follows_the_last_end(state, clock + 72U));
    rig_case_close(&c);
    return rc;
}

/* End to end through the DMR state machine and the engine's trunk hooks: a grant followed from the control channel,
   then a voice channel followed from that one, each moves the epoch at the peer's confirmation and holds the slots'
   voice until their calls say whether they are encrypted. */
static int
test_dmr_follow_end_to_end(void) {
    rig_case c;
    if (rig_case_open(&c, AUDIO_IN_PULSE) != 0) {
        return expect_true("dmr follow: the peer", 0);
    }
    dsd_opts* opts = c.opts;
    dsd_state* state = c.state;
    dsd_engine_trunk_tuning_hooks_install();
    opts->trunk_tune_group_calls = 1;
    opts->trunk_hangtime = 2.0f;
    state->trunk_chan_map[0x10] = VC1_HZ;
    state->trunk_chan_map[0x11] = VC2_HZ;
    state->synctype = DSD_SYNC_DMR_BS_DATA_POS;
    dsd_decode_clock_use_test(k_t0_ns);
    dmr_sm_init(opts, state);
    uint32_t e = state->symbol_clock_epoch;
    dmr_sm_emit_group_grant(opts, state, VC1_HZ, 0x10, 100, 1);
    int rc =
        expect_true("the grant is followed", state->trunk_vc_freq[0] == VC1_HZ && atomic_load(&c.rig.f_taken) == 1);
    rc |= expect_epoch("the follow moves the epoch", state, e, 1);
    rc |= expect_true("and holds the slots' voice",
                      state->dmr_enc_class_held[0] == 1U && state->dmr_enc_class_held[1] == 1U);
    state->dmr_enc_class_held[0] = state->dmr_enc_class_held[1] = 0U;
    /* The call's voice, then its hangtime runs out: the next grant is followed from this voice channel. */
    dsd_decode_clock_test_set_ns(k_t0_ns + 500000000ULL);
    state->dmr_confidence_voice_open[0] = 1;
    state->dmr_confidence_voice_m[0] = dsd_decode_now_mono_s();
    dmr_sm_emit_voice_sync(opts, state, 0);
    e = state->symbol_clock_epoch;
    dsd_decode_clock_test_set_ns(k_t0_ns + 3000000000ULL);
    dmr_sm_emit_group_grant(opts, state, VC2_HZ, 0x11, 200, 2);
    rc |= expect_true("the next voice channel is followed", state->trunk_vc_freq[0] == VC2_HZ);
    rc |= expect_epoch("that follow moves the epoch too", state, e, 1);
    rc |= expect_true("and holds the slots' voice again", state->dmr_enc_class_held[0] == 1U);
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
    dsd_decode_clock_use_test(k_t0_ns);
    rig_case_close(&c);
    return rc;
}

/* ---- The input's queued samples (issue #834) ---- */

static int
bound_port(dsd_socket_t sock) {
    struct sockaddr_in sa;
    DSD_MEMSET(&sa, 0, sizeof sa);
#if DSD_PLATFORM_WIN_NATIVE
    int len = (int)sizeof sa;
#else
    socklen_t len = (socklen_t)sizeof sa;
#endif
    return getsockname(sock, (struct sockaddr*)&sa, &len) == 0 ? (int)ntohs(sa.sin_port) : -1;
}

static int
udp_send(dsd_socket_t tx, int port, const int16_t* samples, size_t count) {
    uint8_t buf[64];
    if (count * 2U > sizeof buf) {
        return -1;
    }
    for (size_t i = 0; i < count; i++) {
        buf[i * 2U] = (uint8_t)((uint16_t)samples[i] & 0xFFU);
        buf[i * 2U + 1U] = (uint8_t)((uint16_t)samples[i] >> 8);
    }
    struct sockaddr_in dst;
    DSD_MEMSET(&dst, 0, sizeof dst);
    if (dsd_socket_resolve("127.0.0.1", port, &dst) != 0) {
        return -1;
    }
    return dsd_socket_sendto(tx, buf, count * 2U, 0, (struct sockaddr*)&dst, (int)sizeof dst) == (int)(count * 2U) ? 0
                                                                                                                   : -1;
}

/* The next UDP sample, waiting for it (-32768 for none). Every caller sent it already: the bound (10 s) only turns a
   lost datagram into a failure, never a slow receive thread. */
static int16_t
udp_next(dsd_opts* opts) {
    int16_t s = 0;
    for (int tries = 0; tries < 500; tries++) {
        if (udp_input_read_sample_wait(opts, &s, 20U) == 1) {
            return s;
        }
    }
    return INT16_MIN;
}

/* A datagram's first sample read, the rest still queued: @p first must come first. */
static int
udp_queue(dsd_socket_t tx, int port, dsd_opts* opts, const int16_t* samples, size_t count) {
    return udp_send(tx, port, samples, count) == 0 && udp_next(opts) == samples[0];
}

/* UDP: the samples the ring holds at the peer's confirmation go at the decoder's next read, and those sent after it are
   read. A tune to the frequency the peer is on and a refused request drop nothing. A tune off the decoder thread (the
   P25 watchdog's) asks for nothing itself: the decoder thread takes its post and drops at its next read. */
static int
test_udp_backlog(void) {
    rig_case c;
    if (rig_case_open(&c, AUDIO_IN_UDP) != 0) {
        return expect_true("udp: the peer", 0);
    }
    dsd_opts* opts = c.opts;
    dsd_state* state = c.state;
    dsd_engine_net_audio_input_hooks_install();
    int rc = expect_true("udp input", udp_input_start(opts, "127.0.0.1", 0, 48000) == 0);
    const int port = bound_port(opts->udp_in_sockfd);
    dsd_socket_t tx = dsd_socket_create(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    rc |= expect_true("sender", tx != DSD_INVALID_SOCKET && port > 0);
    rc |= expect_true("on a voice channel",
                      dsd_engine_trunk_tune_to_freq_request(opts, state, VC1_HZ, 0, 0U) == DSD_TRUNK_TUNE_RESULT_OK);

    const int16_t old_audio[] = {1, 2, 3, 4};
    const int16_t new_audio[] = {9, 10};
    rc |= expect_true("the old channel's audio is queued", udp_queue(tx, port, opts, old_audio, 4U));
    rc |= expect_true("hop",
                      dsd_engine_trunk_tune_to_freq_request(opts, state, VC2_HZ, 0, 0U) == DSD_TRUNK_TUNE_RESULT_OK);
    rc |= expect_true("the tune asks for the drop, and does not carry it out", state->input_drop_pending == 1U);
    dsd_symbol_apply_input_drop(opts, state); /* the decoder thread's next input read */
    rc |= expect_true("which carries it out once", state->input_drop_pending == 0U);
    rc |= expect_true("send after the drop", udp_send(tx, port, new_audio, 2U) == 0);
    rc |= expect_true("the samples queued before the confirmation are not read after it", udp_next(opts) == 9);
    rc |= expect_true("the samples after it are read", udp_next(opts) == 10);

    /* What arrives between the confirmation and the next read is the new channel's: kept. */
    const int16_t old_again[] = {60, 61, 62};
    rc |= expect_true("the old channel's audio again", udp_queue(tx, port, opts, old_again, 3U));
    rc |= expect_true("hop back",
                      dsd_engine_trunk_tune_to_freq_request(opts, state, VC1_HZ, 0, 0U) == DSD_TRUNK_TUNE_RESULT_OK);
    rc |= expect_true("the new channel's audio before the next read",
                      udp_send(tx, port, (const int16_t[]){70, 71}, 2U) == 0);
    dsd_input_cut arrived;
    for (int tries = 0; tries < 100; tries++) {
        dsd_input_backlog_mark(&arrived);
        if (arrived.arrived >= state->input_drop_cut.arrived + 2U) {
            break;
        }
        dsd_sleep_ms(10U);
    }
    dsd_symbol_apply_input_drop(opts, state);
    const int16_t kept_first = udp_next(opts);
    const int16_t kept_second = udp_next(opts);
    rc |= expect_true("audio that arrived after the confirmation is kept", kept_first == 70 && kept_second == 71);
    rc |= expect_true("hop",
                      dsd_engine_trunk_tune_to_freq_request(opts, state, VC2_HZ, 0, 0U) == DSD_TRUNK_TUNE_RESULT_OK);
    dsd_symbol_apply_input_drop(opts, state);

    const int16_t same[] = {5, 6};
    rc |= expect_true("queued again", udp_queue(tx, port, opts, same, 2U));
    rc |= expect_true("same carrier",
                      dsd_engine_trunk_tune_to_freq_request(opts, state, VC2_HZ, 0, 0U) == DSD_TRUNK_TUNE_RESULT_OK);
    dsd_symbol_apply_input_drop(opts, state);
    rc |= expect_true("a tune to the carrier the peer is on drops nothing", udp_next(opts) == 6);

    const int16_t refused[] = {7, 8};
    rc |= expect_true("queued for a refusal", udp_queue(tx, port, opts, refused, 2U));
    atomic_store(&c.rig.f_mode, RIG_REFUSES);
    rc |= expect_true("refused", dsd_engine_trunk_tune_to_freq_request(opts, state, VC3_HZ, 0, 0U)
                                     == DSD_TRUNK_TUNE_RESULT_FAILED);
    dsd_symbol_apply_input_drop(opts, state);
    rc |= expect_true("a refused request drops nothing", udp_next(opts) == 8);
    atomic_store(&c.rig.f_mode, RIG_TAKES);

    const int16_t watchdog[] = {30, 31};
    const int16_t after_watchdog[] = {40};
    rc |= expect_true("queued for the watchdog", udp_queue(tx, port, opts, watchdog, 2U));
    dsd_frame_sync_off_thread_tunes_reset(state);
    dsd_engine_set_decoder_thread(0);
    rc |= expect_true("watchdog tune",
                      dsd_engine_trunk_tune_to_freq_request(opts, state, VC3_HZ, 0, 0U) == DSD_TRUNK_TUNE_RESULT_OK);
    dsd_engine_set_decoder_thread(1);
    rc |= expect_true("the watchdog's tune leaves the input to the decoder thread", state->input_drop_pending == 0U);
    (void)dsd_frame_sync_apply_off_thread_tunes(opts, state);
    dsd_symbol_apply_input_drop(opts, state);
    rc |= expect_true("send after it", udp_send(tx, port, after_watchdog, 1U) == 0);
    rc |= expect_true("a tune off the decoder thread drops at the decoder's next read", udp_next(opts) == 40);

    if (tx != DSD_INVALID_SOCKET) {
        (void)dsd_socket_close(tx);
    }
    udp_input_stop(opts);
    rig_case_close(&c);
    return rc;
}

/* A sender that waits, then sends a datagram, and sends it again every few ms until the reader says its read returned:
   what a source delivers after the decoder already waits on it. A reader that is descheduled past the first burst
   still gets one; one that never returns is ended through the exit flag after a bound, so the test fails, never
   hangs. */
typedef struct {
    dsd_socket_t tx;
    int port;
    int16_t first;
    size_t count;
    unsigned int delay_ms;
    atomic_int read_returned;
    atomic_int gave_up;
} late_sender;

static int
udp_send_ramp(dsd_socket_t tx, int port, int16_t first, size_t count) {
    uint8_t buf[1024];
    if (count * 2U > sizeof buf) {
        return -1;
    }
    for (size_t i = 0; i < count; i++) {
        const uint16_t v = (uint16_t)(first + (int16_t)i);
        buf[i * 2U] = (uint8_t)(v & 0xFFU);
        buf[i * 2U + 1U] = (uint8_t)(v >> 8);
    }
    struct sockaddr_in dst;
    DSD_MEMSET(&dst, 0, sizeof dst);
    if (dsd_socket_resolve("127.0.0.1", port, &dst) != 0) {
        return -1;
    }
    return dsd_socket_sendto(tx, buf, count * 2U, 0, (struct sockaddr*)&dst, (int)sizeof dst) == (int)(count * 2U) ? 0
                                                                                                                   : -1;
}

static DSD_THREAD_RETURN_TYPE
#if DSD_PLATFORM_WIN_NATIVE
    __stdcall
#endif
    late_sender_main(void* arg) {
    late_sender* ls = (late_sender*)arg;
    dsd_sleep_ms(ls->delay_ms);
    for (unsigned int waited_ms = 0U; !atomic_load(&ls->read_returned); waited_ms += 20U) {
        (void)udp_send_ramp(ls->tx, ls->port, ls->first, ls->count);
        if (waited_ms >= 5000U) {
            atomic_store(&ls->gave_up, 1);
            dsd_exitflag_store(1);
            break;
        }
        dsd_sleep_ms(20U);
    }
    DSD_THREAD_RETURN;
}

/* The P25 watchdog's tune: the decoder's next read, before any reconcile of the tune's notice, moves the epoch and
   drops what the input holds of the old channel. */
static int
test_watchdog_tune_drops_at_the_next_read(void) {
    rig_case c;
    if (rig_case_open(&c, AUDIO_IN_UDP) != 0) {
        return expect_true("watchdog read: the peer", 0);
    }
    dsd_opts* opts = c.opts;
    dsd_state* state = c.state;
    dsd_engine_net_audio_input_hooks_install();
    int rc = expect_true("udp input", udp_input_start(opts, "127.0.0.1", 0, 48000) == 0);
    const int port = bound_port(opts->udp_in_sockfd);
    dsd_socket_t tx = dsd_socket_create(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    rc |= expect_true("sender", tx != DSD_INVALID_SOCKET && port > 0);
    rc |= expect_true("on a voice channel",
                      dsd_engine_trunk_tune_to_freq_request(opts, state, VC1_HZ, 0, 0U) == DSD_TRUNK_TUNE_RESULT_OK);
    dsd_symbol_apply_input_drop(opts, state);
    dsd_frame_sync_off_thread_tunes_reset(state);
    /* The old channel's audio: more than a symbol of it queued once its first sample was read. */
    rc |= expect_true("the old channel's audio", udp_send_ramp(tx, port, 30, 300U) == 0 && udp_next(opts) == 30);
    /* The symbol reader has met this input already: its first symbol moves no epoch for an input switch. */
    state->symbol_clock_input = opts->pcm_input_generation;
    const uint32_t e = state->symbol_clock_epoch;
    dsd_engine_set_decoder_thread(0);
    rc |= expect_true("watchdog tune",
                      dsd_engine_trunk_tune_to_freq_request(opts, state, VC2_HZ, 0, 0U) == DSD_TRUNK_TUNE_RESULT_OK);
    dsd_engine_set_decoder_thread(1);
    rc |= expect_epoch("the watchdog's tune leaves the epoch to the decoder thread", state, e, 0);
    /* The new channel's audio comes once the decoder waits on the input. No reconcile runs before the read. */
    late_sender ls = {tx, port, 1000, 300U, 50U, 0, 0};
    dsd_thread_t th;
    if (start_producer(&th, late_sender_main, &ls) != 0) {
        /* No sender: the read would wait on the empty input for good, and there is no thread to join. */
        rc |= expect_true("the late sender started", 0);
    } else {
        (void)getSymbol(opts, state, 0);
        atomic_store(&ls.read_returned, 1);
        (void)dsd_thread_join(th);
        rc |= expect_true("the read returned on the new channel's audio", atomic_load(&ls.gave_up) == 0);
        dsd_exitflag_store(0);
        rc |= expect_true("the next read moves the epoch once", state->symbol_clock_epoch == e + 1U);
        rc |= expect_true("and the read drops the old channel's queued audio: what follows is the new channel's",
                          udp_next(opts) >= 1000);
        /* The tune's notice is reconciled later: the count the read took is not taken again. */
        (void)dsd_frame_sync_apply_off_thread_tunes(opts, state);
        rc |= expect_true("the reconcile takes nothing twice",
                          state->symbol_clock_epoch == e + 1U && state->input_drop_pending == 0U);
    }
    if (tx != DSD_INVALID_SOCKET) {
        (void)dsd_socket_close(tx);
    }
    udp_input_stop(opts);
    rig_case_close(&c);
    return rc;
}

/* The P25 watchdog's beat, serviced on the decoder thread (p25_sm_watchdog_service()): its tick releases a followed
   voice channel to the control channel over rigctl, an ordinary decoder-thread tune, so it marks the input and asks
   for its drop (dsd_engine_note_rigctl_retune()). The decoder's next read drops the old channel's queued audio, so no
   frame on the control channel is built from it. */
static int
test_watchdog_beat_tick_drops_at_the_next_read(void) {
    rig_case c;
    if (rig_case_open(&c, AUDIO_IN_UDP) != 0) {
        return expect_true("watchdog beat: the peer", 0);
    }
    dsd_opts* opts = c.opts;
    dsd_state* state = c.state;
    dsd_engine_net_audio_input_hooks_install();
    dsd_engine_trunk_tuning_hooks_install();
    dsd_engine_set_decoder_thread(1);
    int rc = expect_true("udp input", udp_input_start(opts, "127.0.0.1", 0, 48000) == 0);
    const int port = bound_port(opts->udp_in_sockfd);
    dsd_socket_t tx = dsd_socket_create(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    rc |= expect_true("sender", tx != DSD_INVALID_SOCKET && port > 0);
    opts->trunk_tune_group_calls = 1;
    opts->frame_p25p1 = 1;
    state->p25_cc_freq = CC_HZ;
    state->trunk_chan_map[0x1234] = VC1_HZ;
    dsd_trunk_recovery_note_protocol(state, DSD_TRUNK_RECOVERY_P25);
    p25_sm_ctx_t* ctx = p25_sm_get_ctx();
    p25_sm_init_ctx(ctx, opts, state);
    p25_sm_event_t grant = p25_sm_ev_group_grant(0x1234, VC1_HZ, 1234, 42, 0);
    p25_sm_tick_guard_enter();
    p25_sm_event(ctx, opts, state, &grant);
    state->p25_sm_force_release = 1;
    p25_sm_tick_guard_leave();
    rc |= expect_true("on the voice channel", ctx->state == P25_SM_TUNED);
    dsd_symbol_apply_input_drop(opts, state);
    dsd_frame_sync_off_thread_tunes_reset(state);
    /* The voice channel's audio, queued: more than a symbol of it once its first sample was read. */
    rc |= expect_true("the voice channel's audio", udp_send_ramp(tx, port, 30, 300U) == 0 && udp_next(opts) == 30);
    state->symbol_clock_input = opts->pcm_input_generation;
    const uint32_t e = state->symbol_clock_epoch;
    p25_sm_watchdog_request_tick();
    rc |= expect_true("the beat's tick ran", p25_sm_watchdog_service(opts, state) == 1);
    rc |= expect_true("and returned to the control channel", ctx->state != P25_SM_TUNED && opts->trunk_is_tuned == 0);
    rc |= expect_epoch("the decoder-thread tune moves the epoch at once", state, e, 1);
    /* The control channel's audio comes once the decoder waits on the input. */
    late_sender ls = {tx, port, 1000, 300U, 50U, 0, 0};
    dsd_thread_t th;
    if (start_producer(&th, late_sender_main, &ls) != 0) {
        rc |= expect_true("the late sender started", 0);
    } else {
        (void)getSymbol(opts, state, 0);
        atomic_store(&ls.read_returned, 1);
        (void)dsd_thread_join(th);
        rc |= expect_true("the read returned on the control channel's audio", atomic_load(&ls.gave_up) == 0);
        dsd_exitflag_store(0);
        rc |= expect_true("the next read drops the voice channel's queued audio: what follows is the control channel's",
                          udp_next(opts) >= 1000);
    }
    p25_sm_init_ctx(ctx, opts, state);
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
    if (tx != DSD_INVALID_SOCKET) {
        (void)dsd_socket_close(tx);
    }
    udp_input_stop(opts);
    rig_case_close(&c);
    return rc;
}

/* ---- A watchdog retune and the input's queue at the moment it was posted (issue #834) ---- */

static int make_pipe(int fds[2]);

/* What feeds the input under test: a UDP sender, a TCP server's end of the connection, or a pipe's write end. */
typedef struct {
    int kind; /* AUDIO_IN_UDP, AUDIO_IN_TCP or AUDIO_IN_STDIN */
    dsd_socket_t sock;
    int port;
    int fd;
} wait_src;

static int
wait_src_send(const wait_src* src, int16_t first, size_t count) {
    if (src->kind == AUDIO_IN_UDP) {
        return udp_send_ramp(src->sock, src->port, first, count);
    }
    uint8_t buf[1024];
    if (count * 2U > sizeof buf) {
        return -1;
    }
    for (size_t i = 0; i < count; i++) {
        const uint16_t v = (uint16_t)(first + (int16_t)i);
        buf[i * 2U] = (uint8_t)(v & 0xFFU);
        buf[i * 2U + 1U] = (uint8_t)(v >> 8);
    }
    if (src->kind == AUDIO_IN_TCP) {
        return dsd_socket_send(src->sock, buf, count * 2U, 0) == (int)(count * 2U) ? 0 : -1;
    }
    return dsd_write(src->fd, buf, count * 2U) == (ssize_t)(count * 2U) ? 0 : -1;
}

/* The input's next sample, waiting for it (INT16_MIN for none). Every caller sent it already: the bound (10 s) only
   turns lost audio into a failure, never a slow host. */
static int16_t
wait_input_next(dsd_opts* opts) {
    int16_t s = 0;
    for (int tries = 0; tries < 500; tries++) {
        int got = 0;
        if (opts->audio_in_type == AUDIO_IN_UDP) {
            got = udp_input_read_sample_wait(opts, &s, 20U);
        } else if (opts->audio_in_type == AUDIO_IN_TCP) {
            got = tcp_input_read_sample_wait(opts->tcp_in_ctx, &s, 20U);
        } else {
            got = dsd_pcm_stream_read_wait(opts->audio_in_stdin, &s, 20U);
        }
        if (got == 1) {
            return s;
        }
    }
    return INT16_MIN;
}

/* The P25 watchdog, once the decoder waits on the empty input: it confirms a retune, and the new channel's first
   audio arrives after it. The 150 ms is when that is likely, not a condition: a post made before the decoder's read
   began is taken at its start, before the new audio is sent, and the case ends the same. */
typedef struct {
    const wait_src* src;
    int sent;
} wait_poster;

static DSD_THREAD_RETURN_TYPE
#if DSD_PLATFORM_WIN_NATIVE
    __stdcall
#endif
    wait_poster_main(void* arg) {
    wait_poster* wp = (wait_poster*)arg;
    dsd_sleep_ms(150U);
    watchdog_retune_now();
    wp->sent = wait_src_send(wp->src, 1000, 300U) == 0;
    DSD_THREAD_RETURN;
}

/* Wait until a mark of the input counts @p arrived (samples on UDP, bytes on TCP and stdin): all the audio sent so far
   reached it. The bound (10 s) only turns lost audio into a failure, never a slow host. */
static int
input_counts(uint64_t arrived) {
    for (int tries = 0; tries < 1000; tries++) {
        dsd_input_cut cut;
        dsd_input_backlog_mark(&cut);
        if (cut.measured && cut.arrived >= arrived) {
            return cut.arrived == arrived;
        }
        dsd_sleep_ms(10U);
    }
    return 0;
}

/* The decoder thread's next read after a post: what symbol_read() does first. */
static void
take_at_the_next_read(dsd_opts* opts, dsd_state* state) {
    dsd_frame_sync_take_off_thread_unflushed_retunes(opts, state);
    dsd_symbol_apply_input_drop(opts, state);
}

/* The drop is of what the input held when the watchdog posted its retune. Audio queued then goes at the decoder's
   next read; audio that arrived after it is the new channel's and is read, even when it is what ended the read that
   was waiting when the post came. */
static int
watchdog_queue_case(dsd_opts* opts, dsd_state* state, const wait_src* src, const char* what) {
    char label[160];
    dsd_frame_sync_off_thread_tunes_reset(state);
    state->symbol_clock_input = opts->pcm_input_generation;
    DSD_SNPRINTF(label, sizeof label, "%s: the old channel's audio, its first sample read", what);
    int rc = expect_true(label, wait_src_send(src, 30, 300U) == 0 && wait_input_next(opts) == 30);
    DSD_SNPRINTF(label, sizeof label, "%s: all of it reached the input", what);
    rc |= expect_true(label, input_counts(src->kind == AUDIO_IN_UDP ? 300U : 600U));
    watchdog_retune_now();
    take_at_the_next_read(opts, state);
    rc |= expect_true("send after the drop", wait_src_send(src, 500, 2U) == 0);
    DSD_SNPRINTF(label, sizeof label, "%s: what was queued at the post goes at the next read", what);
    const int16_t after_first = wait_input_next(opts);
    const int16_t after_second = wait_input_next(opts);
    rc |= expect_true(label, after_first == 500 && after_second == 501);

    const uint32_t e = state->symbol_clock_epoch;
    wait_poster wp = {src, 0};
    dsd_thread_t th;
    if (start_producer(&th, wait_poster_main, &wp) != 0) {
        /* No watchdog: the read would wait on the empty input for good, and there is no thread to join. */
        return rc | expect_true("the watchdog started", 0);
    }
    (void)getSymbol(opts, state, 0); /* waits on the empty input; the new channel's audio ends the wait */
    (void)dsd_thread_join(th);
    rc |= expect_true("the new channel's audio was sent", wp.sent);
    take_at_the_next_read(opts, state);
    rc |= expect_true("the post is taken once", state->symbol_clock_epoch == e + 1U);
    const int16_t next = wait_input_next(opts);
    DSD_SNPRINTF(label, sizeof label, "%s: the new channel's audio after the read it ended is kept (next %d)", what,
                 (int)next);
    rc |= expect_true(label, next > 1000 && next < 1300);
    return rc;
}

static int
test_watchdog_post_drops_only_what_was_queued(void) {
    rig_case c;
    if (rig_case_open(&c, AUDIO_IN_UDP) != 0) {
        return expect_true("queue at the post: the peer", 0);
    }
    dsd_opts* opts = c.opts;
    dsd_state* state = c.state;
    dsd_engine_net_audio_input_hooks_install();

    /* UDP: the ring. */
    int rc = expect_true("udp input", udp_input_start(opts, "127.0.0.1", 0, 48000) == 0);
    wait_src udp = {AUDIO_IN_UDP, dsd_socket_create(AF_INET, SOCK_DGRAM, IPPROTO_UDP), bound_port(opts->udp_in_sockfd),
                    -1};
    rc |= expect_true("udp sender", udp.sock != DSD_INVALID_SOCKET && udp.port > 0);
    if (rc == 0) {
        rc |= watchdog_queue_case(opts, state, &udp, "udp");
    }
    if (udp.sock != DSD_INVALID_SOCKET) {
        (void)dsd_socket_close(udp.sock);
    }
    udp_input_stop(opts);

    /* TCP: the reader's buffer and the socket. */
    opts->audio_in_type = AUDIO_IN_TCP;
    dsd_socket_t listener = dsd_socket_create(AF_INET, SOCK_STREAM, 0);
    dsd_socket_t client = dsd_socket_create(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    DSD_MEMSET(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const int tcp_ok = listener != DSD_INVALID_SOCKET && client != DSD_INVALID_SOCKET
                       && dsd_socket_bind(listener, (struct sockaddr*)&addr, sizeof addr) == 0
                       && dsd_socket_listen(listener, 1) == 0;
    rc |= expect_true("tcp sockets", tcp_ok);
    if (tcp_ok) {
        struct sockaddr_in bound;
        DSD_MEMSET(&bound, 0, sizeof bound);
        bound.sin_family = AF_INET;
        bound.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bound.sin_port = htons((uint16_t)bound_port(listener));
        rc |= expect_true("tcp connect", dsd_socket_connect(client, (struct sockaddr*)&bound, sizeof bound) == 0);
        wait_src tcp = {AUDIO_IN_TCP, dsd_socket_accept(listener, NULL, NULL), 0, -1};
        opts->tcp_in_ctx = tcp_input_open(client, 48000);
        rc |= expect_true("tcp input", tcp.sock != DSD_INVALID_SOCKET && opts->tcp_in_ctx != NULL);
        if (tcp.sock != DSD_INVALID_SOCKET && opts->tcp_in_ctx != NULL) {
            rc |= watchdog_queue_case(opts, state, &tcp, "tcp");
        }
        tcp_input_close(opts->tcp_in_ctx);
        opts->tcp_in_ctx = NULL;
        if (tcp.sock != DSD_INVALID_SOCKET) {
            (void)dsd_socket_close(tcp.sock);
        }
    }
    if (client != DSD_INVALID_SOCKET) {
        (void)dsd_socket_close(client);
    }
    if (listener != DSD_INVALID_SOCKET) {
        (void)dsd_socket_close(listener);
    }

    /* stdin from another program: the reader's buffer and the pipe. */
    opts->audio_in_type = AUDIO_IN_STDIN;
    int fds[2] = {-1, -1};
    const int pipe_ok = make_pipe(fds) == 0;
    rc |= expect_true("pipe", pipe_ok);
    if (pipe_ok) {
        opts->audio_in_stdin = dsd_pcm_stream_open_fd(fds[0]);
        wait_src pipe_src = {AUDIO_IN_STDIN, DSD_INVALID_SOCKET, 0, fds[1]};
        rc |= expect_true("stdin reader", opts->audio_in_stdin != NULL);
        if (opts->audio_in_stdin != NULL) {
            rc |= watchdog_queue_case(opts, state, &pipe_src, "stdin");
        }
        dsd_pcm_stream_close(opts->audio_in_stdin);
        opts->audio_in_stdin = NULL;
        (void)dsd_close(fds[0]);
        (void)dsd_close(fds[1]);
    }
    rig_case_close(&c);
    return rc;
}

/* ---- A watchdog retune posted while the TCP input reconnects (issue #834) ---- */

/* When the watchdog posts its retune against the reconnect. */
enum {
    RECONNECT_POST_IN_BACKOFF = 0,   /* before the connection is made */
    RECONNECT_POST_IN_OPEN = 1,      /* once it is made, before its reader attaches */
    RECONNECT_POST_IN_OPEN_LATE = 2, /* the same, and new audio reaches the connection before its reader attaches */
    /* Marked once the connection is made, before its reader attaches, and confirmed (posted) after it attached. */
    RECONNECT_MARK_IN_OPEN_POST_ATTACHED = 3,
    /* Marked during the backoff, before the connection is made, and confirmed while it opens. */
    RECONNECT_MARK_IN_BACKOFF_POST_IN_OPEN = 4,
    /* A watchdog beat's tick on the decoder thread, in the connect's cancel check once the server has accepted and
       queued the old channel's audio, returns to the control channel over rigctl before the reconnect knows the
       socket. */
    RECONNECT_TICK_IN_CONNECT = 5
};

#define RECONNECT_OLD_SAMPLES   300
#define RECONNECT_LATE_SAMPLES  50
#define RECONNECT_AFTER_SAMPLES 200
#define RECONNECT_OLD_VALUE     1
#define RECONNECT_NEW_VALUE     1000

/* A server the reconnect reaches on a loopback socket: the new connection holds RECONNECT_OLD_SAMPLES as soon as it is
   made. The watchdog confirms a retune during the backoff before it (those samples then are the new channel's), or
   while the decoder opens it (they are the old channel's; with RECONNECT_POST_IN_OPEN_LATE the new channel's first
   samples reach it too before its reader attaches). The new channel's audio follows once the reader attaches. The real
   TCP reader reads it. */
static struct {
    int ctx;  /* the old connection, which the first read ends */
    int when; /* RECONNECT_POST_* */
    int ended;
    dsd_socket_t listener;
    dsd_socket_t server; /* the peer's end of the new connection */
    int first_value;     /* the first sample read on the new connection (-1: none yet) */
    int old_read;        /* samples read that the connection held when it was made */
    int new_read;        /* samples read that were sent after the post */
    long dropped;        /* samples a drop took */
    dsd_input_cut mark;  /* a retune's mark, taken before it is confirmed (RECONNECT_MARK_*) */
} g_reconnect;

/* Send @p count samples of @p value to the new connection and wait until @p client holds @p held bytes ready (the
   bound, 10 s, only turns a lost send into a failure). */
static int
reconnect_send(dsd_socket_t client, int16_t value, int count, long held) {
    uint8_t bytes[2U * RECONNECT_OLD_SAMPLES];
    if (count < 0 || count > RECONNECT_OLD_SAMPLES) {
        return -1;
    }
    for (size_t i = 0; i < (size_t)count; i++) {
        bytes[2U * i] = (uint8_t)((uint16_t)value & 0xFFU);
        bytes[2U * i + 1U] = (uint8_t)((uint16_t)value >> 8);
    }
    if (dsd_socket_send(g_reconnect.server, bytes, (size_t)count * 2U, 0) != count * 2) {
        return -1;
    }
    for (int tries = 0; tries < 1000; tries++) {
        if (dsd_socket_readable_bytes(client) >= held) {
            return 0;
        }
        dsd_sleep_ms(10U);
    }
    return -1;
}

static dsd_socket_t
reconnect_connect(const char* host, int port, int resolve, dsd_socket_cancel_fn cancelled, void* context) {
    (void)host;
    (void)port;
    (void)resolve;
    if (g_reconnect.when == RECONNECT_POST_IN_BACKOFF) {
        watchdog_retune_now();
    } else if (g_reconnect.when == RECONNECT_MARK_IN_BACKOFF_POST_IN_OPEN) {
        dsd_input_backlog_mark(&g_reconnect.mark);
    }
    g_reconnect.listener = dsd_socket_create(AF_INET, SOCK_STREAM, 0);
    dsd_socket_t client = dsd_socket_create(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    DSD_MEMSET(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (g_reconnect.listener == DSD_INVALID_SOCKET || client == DSD_INVALID_SOCKET
        || dsd_socket_bind(g_reconnect.listener, (struct sockaddr*)&addr, sizeof addr) != 0
        || dsd_socket_listen(g_reconnect.listener, 1) != 0) {
        if (client != DSD_INVALID_SOCKET) {
            (void)dsd_socket_close(client);
        }
        return DSD_INVALID_SOCKET;
    }
    addr.sin_port = htons((uint16_t)bound_port(g_reconnect.listener));
    if (dsd_socket_connect(client, (struct sockaddr*)&addr, sizeof addr) != 0) {
        (void)dsd_socket_close(client);
        return DSD_INVALID_SOCKET;
    }
    g_reconnect.server = dsd_socket_accept(g_reconnect.listener, NULL, NULL);
    if (g_reconnect.server == DSD_INVALID_SOCKET
        || reconnect_send(client, RECONNECT_OLD_VALUE, RECONNECT_OLD_SAMPLES, 2L * RECONNECT_OLD_SAMPLES) != 0) {
        (void)dsd_socket_close(client);
        return DSD_INVALID_SOCKET;
    }
    if (g_reconnect.when == RECONNECT_TICK_IN_CONNECT) {
        /* The connect's next cancel check, as dsd_socket_connect_bounded() makes before it sees the socket writable. */
        p25_sm_watchdog_request_tick();
        if (cancelled != NULL && cancelled(context)) {
            (void)dsd_socket_close(client);
            return DSD_INVALID_SOCKET;
        }
    }
    return client;
}

static tcp_input_ctx*
reconnect_open(dsd_socket_t sockfd, int samplerate) {
    long held = 2L * RECONNECT_OLD_SAMPLES;
    if (g_reconnect.when == RECONNECT_POST_IN_OPEN || g_reconnect.when == RECONNECT_POST_IN_OPEN_LATE) {
        watchdog_retune_now();
    } else if (g_reconnect.when == RECONNECT_MARK_IN_OPEN_POST_ATTACHED) {
        dsd_input_backlog_mark(&g_reconnect.mark);
    } else if (g_reconnect.when == RECONNECT_MARK_IN_BACKOFF_POST_IN_OPEN) {
        dsd_frame_sync_note_off_thread_unflushed_retune(&g_reconnect.mark);
    }
    if (g_reconnect.when == RECONNECT_POST_IN_OPEN_LATE) {
        held += 2L * RECONNECT_LATE_SAMPLES;
        if (reconnect_send(sockfd, RECONNECT_NEW_VALUE, RECONNECT_LATE_SAMPLES, held) != 0) {
            return NULL;
        }
    }
    tcp_input_ctx* reader = tcp_input_open(sockfd, samplerate);
    if (reader != NULL && g_reconnect.when == RECONNECT_MARK_IN_OPEN_POST_ATTACHED) {
        dsd_frame_sync_note_off_thread_unflushed_retune(&g_reconnect.mark);
    }
    if (reader != NULL
        && reconnect_send(sockfd, RECONNECT_NEW_VALUE, RECONNECT_AFTER_SAMPLES, held + 2L * RECONNECT_AFTER_SAMPLES)
               != 0) {
        tcp_input_close(reader);
        return NULL;
    }
    return reader;
}

static void
reconnect_close(tcp_input_ctx* ctx) {
    if (ctx != NULL && ctx != (tcp_input_ctx*)&g_reconnect.ctx) {
        tcp_input_close(ctx);
    }
}

static int
reconnect_read(tcp_input_ctx* ctx, int16_t* out, unsigned int timeout_ms) {
    if (ctx == (tcp_input_ctx*)&g_reconnect.ctx) {
        g_reconnect.ended = 1;
        return 0;
    }
    const int got = tcp_input_read_sample_wait(ctx, out, timeout_ms);
    if (got == 1) {
        if (g_reconnect.first_value < 0) {
            g_reconnect.first_value = *out;
        }
        g_reconnect.old_read += *out == RECONNECT_OLD_VALUE;
        g_reconnect.new_read += *out == RECONNECT_NEW_VALUE;
    }
    return got;
}

static long
reconnect_discard(tcp_input_ctx* ctx, const dsd_input_cut* cut) {
    const long n = tcp_input_discard_through(ctx, cut);
    g_reconnect.dropped += n > 0 ? n : 0;
    return n;
}

/* A followed voice channel the next watchdog tick releases over rigctl, on the decoder thread (as in
   test_watchdog_beat_tick_drops_at_the_next_read()). */
static int
reconnect_follow_voice_channel(dsd_opts* opts, dsd_state* state, p25_sm_ctx_t* p25) {
    dsd_engine_trunk_tuning_hooks_install();
    /* The hunt's reads service a beat through the engine's hook, as in a run. */
    dsd_frame_sync_hooks_set(&(dsd_frame_sync_hooks){.p25_sm_watchdog_service = p25_sm_watchdog_service});
    dsd_engine_set_decoder_thread(1);
    opts->trunk_tune_group_calls = 1;
    opts->frame_p25p1 = 1;
    state->p25_cc_freq = CC_HZ;
    state->trunk_chan_map[0x1234] = VC1_HZ;
    dsd_trunk_recovery_note_protocol(state, DSD_TRUNK_RECOVERY_P25);
    p25_sm_init_ctx(p25, opts, state);
    p25_sm_event_t grant = p25_sm_ev_group_grant(0x1234, VC1_HZ, 1234, 42, 0);
    p25_sm_tick_guard_enter();
    p25_sm_event(p25, opts, state, &grant);
    state->p25_sm_force_release = 1;
    p25_sm_tick_guard_leave();
    return p25->state == P25_SM_TUNED ? 0 : -1;
}

/* The replacement stream's own drop. A retune the watchdog posts once the new connection exists, while the decoder
   opens it, drops what that connection held when it was posted: none of it is read, and audio that reached the
   connection after the post (before its reader attached, or after) is read, none of it dropped. One posted during the
   backoff, before the connection was made, moved the peer before it began: what the connection holds is the new
   channel's, and none of it is dropped. A decoder-thread tick that retunes inside the connect, once the server has
   queued the old channel's audio, drops that audio the same way. */
static int
watchdog_post_while_tcp_reconnects(int when) {
    rig_case c;
    if (rig_case_open(&c, AUDIO_IN_TCP) != 0) {
        return expect_true("reconnect: the peer", 0);
    }
    dsd_opts* opts = c.opts;
    dsd_state* state = c.state;
    DSD_MEMSET(&g_reconnect, 0, sizeof g_reconnect);
    g_reconnect.when = when;
    g_reconnect.listener = DSD_INVALID_SOCKET;
    g_reconnect.server = DSD_INVALID_SOCKET;
    g_reconnect.first_value = -1;
    dsd_net_audio_input_hooks_set(&(dsd_net_audio_input_hooks){.tcp_connect = reconnect_connect,
                                                               .tcp_open = reconnect_open,
                                                               .tcp_close = reconnect_close,
                                                               .tcp_read_sample_wait = reconnect_read,
                                                               .tcp_discard_through = reconnect_discard});
    opts->tcp_in_ctx = (tcp_input_ctx*)&g_reconnect.ctx;
    opts->tcp_sockfd = DSD_INVALID_SOCKET;
    p25_sm_ctx_t* p25 = p25_sm_get_ctx();
    int rc = 0;
    if (when == RECONNECT_TICK_IN_CONNECT) {
        rc |= expect_true("reconnect: on the voice channel", reconnect_follow_voice_channel(opts, state, p25) == 0);
    }
    dsd_frame_sync_off_thread_tunes_reset(state);
    state->symbol_clock_input = opts->pcm_input_generation;
    /* The old connection's last cutoff, carried out, given an order past any mark: the newest of that input. The new
       connection is another input, so its post is its cutoff all the same. */
    dsd_input_cut old_cut;
    dsd_input_backlog_mark(&old_cut);
    old_cut.order = UINT64_MAX;
    (void)dsd_frame_sync_request_input_drop(opts, state, &old_cut);
    state->input_drop_pending = 0U;
    (void)getSymbol(opts, state, 0); /* the connection ends, the reconnect opens the new one */
    (void)getSymbol(opts, state, 0);
    char label[256];
    rc |= expect_true("reconnect: the old connection ended and the new one was read",
                      g_reconnect.ended && g_reconnect.first_value >= 0);
    if (when == RECONNECT_TICK_IN_CONNECT) {
        rc |= expect_true("reconnect: the tick in the connect returned to the control channel",
                          p25->state != P25_SM_TUNED && opts->trunk_is_tuned == 0);
    }
    if (when == RECONNECT_POST_IN_BACKOFF || when == RECONNECT_MARK_IN_BACKOFF_POST_IN_OPEN) {
        DSD_SNPRINTF(label, sizeof label,
                     "a retune %s drops nothing of the new connection: it is read from its first sample (first %d, "
                     "dropped %ld)",
                     when == RECONNECT_POST_IN_BACKOFF ? "posted during the backoff"
                                                       : "asked for during the backoff and confirmed while it opens",
                     g_reconnect.first_value, g_reconnect.dropped);
        rc |= expect_true(label, g_reconnect.dropped == 0 && g_reconnect.first_value == RECONNECT_OLD_VALUE);
        rc |= expect_true("and the post is not taken later", dsd_input_backlog_take(NULL) == 0);
    } else {
        DSD_SNPRINTF(label, sizeof label,
                     "a retune marked while the new connection opens drops what it held at the mark, and nothing "
                     "that came after (%s; read %d old, %d new, first %d, dropped %ld of %d)",
                     when == RECONNECT_POST_IN_OPEN_LATE            ? "new audio before its reader attached"
                     : when == RECONNECT_MARK_IN_OPEN_POST_ATTACHED ? "confirmed after its reader attached"
                     : when == RECONNECT_TICK_IN_CONNECT            ? "a decoder-thread tick in the connect"
                                                                    : "no new audio before",
                     g_reconnect.old_read, g_reconnect.new_read, g_reconnect.first_value, g_reconnect.dropped,
                     RECONNECT_OLD_SAMPLES);
        rc |= expect_true(label, g_reconnect.old_read == 0 && g_reconnect.dropped == RECONNECT_OLD_SAMPLES
                                     && g_reconnect.first_value == RECONNECT_NEW_VALUE);
    }
    if (when == RECONNECT_TICK_IN_CONNECT) {
        p25_sm_init_ctx(p25, opts, state);
        dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
        dsd_frame_sync_hooks_set(NULL);
    }
    reconnect_close(opts->tcp_in_ctx);
    dsd_net_audio_input_hooks_set(NULL);
    opts->tcp_in_ctx = NULL;
    if (opts->tcp_sockfd != DSD_INVALID_SOCKET) {
        (void)dsd_socket_close(opts->tcp_sockfd);
        opts->tcp_sockfd = DSD_INVALID_SOCKET;
    }
    if (g_reconnect.server != DSD_INVALID_SOCKET) {
        (void)dsd_socket_close(g_reconnect.server);
    }
    if (g_reconnect.listener != DSD_INVALID_SOCKET) {
        (void)dsd_socket_close(g_reconnect.listener);
    }
    rig_case_close(&c);
    return rc;
}

static int
test_watchdog_post_while_tcp_reconnects(void) {
    return watchdog_post_while_tcp_reconnects(RECONNECT_POST_IN_OPEN)
           | watchdog_post_while_tcp_reconnects(RECONNECT_POST_IN_OPEN_LATE)
           | watchdog_post_while_tcp_reconnects(RECONNECT_POST_IN_BACKOFF)
           | watchdog_post_while_tcp_reconnects(RECONNECT_MARK_IN_OPEN_POST_ATTACHED)
           | watchdog_post_while_tcp_reconnects(RECONNECT_MARK_IN_BACKOFF_POST_IN_OPEN)
           | watchdog_post_while_tcp_reconnects(RECONNECT_TICK_IN_CONNECT);
}

/* A watchdog retune posted before the input is replaced moved the peer before the new stream began: the new stream
   holds nothing of the channel it left, so the post is never taken. One posted after the replacement is. */
static int
test_watchdog_post_and_an_input_switch(void) {
    rig_case c;
    if (rig_case_open(&c, AUDIO_IN_PULSE) != 0) {
        return expect_true("watchdog switch: the peer", 0);
    }
    dsd_opts* opts = c.opts;
    dsd_state* state = c.state;
    dsd_frame_sync_off_thread_tunes_reset(state);
    uint32_t e = state->symbol_clock_epoch;
    dsd_engine_set_decoder_thread(0);
    int rc = expect_true("watchdog tune before the switch",
                         dsd_engine_trunk_tune_to_freq_request(opts, state, VC1_HZ, 0, 0U) == DSD_TRUNK_TUNE_RESULT_OK);
    dsd_engine_set_decoder_thread(1);
    dsd_opts_reset_pcm_input_state(opts); /* the input is replaced before the decoder's next read */
    dsd_frame_sync_take_off_thread_unflushed_retunes(opts, state);
    rc |= expect_true("a post made before the switch is not taken on the new input",
                      state->symbol_clock_epoch == e && state->input_drop_pending == 0U);
    dsd_engine_set_decoder_thread(0);
    rc |= expect_true("watchdog tune after the switch",
                      dsd_engine_trunk_tune_to_freq_request(opts, state, VC2_HZ, 0, 0U) == DSD_TRUNK_TUNE_RESULT_OK);
    dsd_engine_set_decoder_thread(1);
    dsd_frame_sync_take_off_thread_unflushed_retunes(opts, state);
    rc |= expect_true("a post made after the switch is taken",
                      state->symbol_clock_epoch == e + 1U && state->input_drop_pending == 1U
                          && state->input_drop_generation == opts->pcm_input_generation);
    state->input_drop_pending = 0U;
    (void)dsd_frame_sync_apply_off_thread_tunes(opts, state);
    rig_case_close(&c);
    return rc;
}

/* A drop belongs to the input and the run it was asked for: one asked for before an input switch, or before a run
   boundary, drops nothing of the input that follows. */
static int
test_drop_belongs_to_its_input(void) {
    rig_case c;
    if (rig_case_open(&c, AUDIO_IN_UDP) != 0) {
        return expect_true("drop input: the peer", 0);
    }
    dsd_opts* opts = c.opts;
    dsd_state* state = c.state;
    dsd_engine_net_audio_input_hooks_install();
    int rc = expect_true("udp input", udp_input_start(opts, "127.0.0.1", 0, 48000) == 0);
    const int port = bound_port(opts->udp_in_sockfd);
    dsd_socket_t tx = dsd_socket_create(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    rc |= expect_true("on a voice channel",
                      dsd_engine_trunk_tune_to_freq_request(opts, state, VC1_HZ, 0, 0U) == DSD_TRUNK_TUNE_RESULT_OK);
    rc |= expect_true("a drop is asked for", state->input_drop_pending == 1U);
    dsd_opts_note_pcm_stream(opts); /* the input is replaced before the next read */
    rc |= expect_true("the new input's audio", udp_queue(tx, port, opts, (const int16_t[]){50, 51}, 2U));
    dsd_symbol_apply_input_drop(opts, state);
    rc |= expect_true("a drop asked for on the input replaced drops nothing of the new one",
                      state->input_drop_pending == 0U && udp_next(opts) == 51);
    rc |= expect_true("hop",
                      dsd_engine_trunk_tune_to_freq_request(opts, state, VC2_HZ, 0, 0U) == DSD_TRUNK_TUNE_RESULT_OK);
    rc |= expect_true("asked again", state->input_drop_pending == 1U);
    dsd_frame_sync_off_thread_tunes_reset(state); /* a run boundary: Stop, then Start */
    rc |= expect_true("a run boundary clears it", state->input_drop_pending == 0U);
    if (tx != DSD_INVALID_SOCKET) {
        (void)dsd_socket_close(tx);
    }
    udp_input_stop(opts);
    rig_case_close(&c);
    return rc;
}

/* TCP: what the reader buffered and what the socket has ready at the confirmation go at the decoder's next read; what
   is sent after it is read. */
static int
test_tcp_backlog(void) {
    rig_case c;
    if (rig_case_open(&c, AUDIO_IN_TCP) != 0) {
        return expect_true("tcp: the peer", 0);
    }
    dsd_opts* opts = c.opts;
    dsd_state* state = c.state;
    dsd_engine_net_audio_input_hooks_install();
    dsd_socket_t listener = dsd_socket_create(AF_INET, SOCK_STREAM, 0);
    dsd_socket_t client = dsd_socket_create(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    DSD_MEMSET(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int rc = expect_true("tcp sockets", listener != DSD_INVALID_SOCKET && client != DSD_INVALID_SOCKET
                                            && dsd_socket_bind(listener, (struct sockaddr*)&addr, sizeof addr) == 0
                                            && dsd_socket_listen(listener, 1) == 0);
    if (rc != 0) {
        if (client != DSD_INVALID_SOCKET) {
            (void)dsd_socket_close(client);
        }
        if (listener != DSD_INVALID_SOCKET) {
            (void)dsd_socket_close(listener);
        }
        rig_case_close(&c);
        return rc;
    }
    struct sockaddr_in bound;
    DSD_MEMSET(&bound, 0, sizeof bound);
    bound.sin_family = AF_INET;
    bound.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bound.sin_port = htons((uint16_t)bound_port(listener));
    rc |= expect_true("tcp connect", dsd_socket_connect(client, (struct sockaddr*)&bound, sizeof bound) == 0);
    dsd_socket_t server = dsd_socket_accept(listener, NULL, NULL);
    opts->tcp_in_ctx = tcp_input_open(client, 48000);
    rc |= expect_true("tcp input", server != DSD_INVALID_SOCKET && opts->tcp_in_ctx != NULL);
    rc |= expect_true("on a voice channel",
                      dsd_engine_trunk_tune_to_freq_request(opts, state, VC1_HZ, 0, 0U) == DSD_TRUNK_TUNE_RESULT_OK);
    const uint8_t old_audio[] = {0x01U, 0x00U, 0x02U, 0x00U, 0x03U, 0x00U};
    rc |= expect_true("send the old channel's audio",
                      dsd_socket_send(server, old_audio, sizeof old_audio, 0) == (int)sizeof old_audio);
    int16_t s = 0;
    rc |= expect_true("its first sample", tcp_input_read_sample_wait(opts->tcp_in_ctx, &s, 1000U) == 1 && s == 1);
    rc |= expect_true("hop",
                      dsd_engine_trunk_tune_to_freq_request(opts, state, VC2_HZ, 0, 0U) == DSD_TRUNK_TUNE_RESULT_OK);
    dsd_symbol_apply_input_drop(opts, state); /* the decoder thread's next input read */
    const uint8_t new_audio[] = {0x09U, 0x00U};
    rc |= expect_true("send after the confirmation",
                      dsd_socket_send(server, new_audio, sizeof new_audio, 0) == (int)sizeof new_audio);
    rc |= expect_true("the samples held before the confirmation are not read after it",
                      tcp_input_read_sample_wait(opts->tcp_in_ctx, &s, 1000U) == 1 && s == 9);
    tcp_input_close(opts->tcp_in_ctx);
    opts->tcp_in_ctx = NULL;
    (void)dsd_socket_close(server);
    (void)dsd_socket_close(client);
    (void)dsd_socket_close(listener);
    rig_case_close(&c);
    return rc;
}

static int
make_pipe(int fds[2]) {
#if DSD_PLATFORM_WIN_NATIVE
    return _pipe(fds, 65536U, _O_BINARY);
#else
    return pipe(fds);
#endif
}

/* stdin from another program (a pipe): what the pipe and the reader hold at the confirmation go at the next read. */
static int
test_stdin_backlog(void) {
    rig_case c;
    if (rig_case_open(&c, AUDIO_IN_STDIN) != 0) {
        return expect_true("stdin: the peer", 0);
    }
    dsd_opts* opts = c.opts;
    dsd_state* state = c.state;
    int fds[2] = {-1, -1};
    int rc = expect_true("pipe", make_pipe(fds) == 0);
    opts->audio_in_stdin = dsd_pcm_stream_open_fd(fds[0]);
    rc |= expect_true("stdin reader", opts->audio_in_stdin != NULL);
    rc |= expect_true("on a voice channel",
                      dsd_engine_trunk_tune_to_freq_request(opts, state, VC1_HZ, 0, 0U) == DSD_TRUNK_TUNE_RESULT_OK);
    dsd_symbol_apply_input_drop(opts, state);
    const uint8_t old_audio[] = {0x01U, 0x00U, 0x02U, 0x00U, 0x03U, 0x00U};
    rc |= expect_true("the old channel's audio",
                      dsd_write(fds[1], old_audio, sizeof old_audio) == (ssize_t)sizeof old_audio);
    int16_t s = 0;
    rc |= expect_true("its first sample", dsd_pcm_stream_read_wait(opts->audio_in_stdin, &s, 1000U) == 1 && s == 1);
    rc |= expect_true("more of it, still in the pipe",
                      dsd_write(fds[1], old_audio, sizeof old_audio) == (ssize_t)sizeof old_audio);
    rc |= expect_true("hop",
                      dsd_engine_trunk_tune_to_freq_request(opts, state, VC2_HZ, 0, 0U) == DSD_TRUNK_TUNE_RESULT_OK);
    dsd_symbol_apply_input_drop(opts, state);
    const uint8_t new_audio[] = {0x09U, 0x00U};
    rc |=
        expect_true("send after the drop", dsd_write(fds[1], new_audio, sizeof new_audio) == (ssize_t)sizeof new_audio);
    rc |= expect_true("what the reader and the pipe held is not read after it",
                      dsd_pcm_stream_read_wait(opts->audio_in_stdin, &s, 1000U) == 1 && s == 9);
    dsd_pcm_stream_close(opts->audio_in_stdin);
    opts->audio_in_stdin = NULL;
    (void)dsd_close(fds[0]);
    (void)dsd_close(fds[1]);
    rig_case_close(&c);
    return rc;
}

/* ---- The newest cutoff wins (issue #834) ---- */

/* One sample of @p value into the pipe the stdin reader reads. */
static int
pipe_put(int fd, int16_t value) {
    const uint8_t b[2] = {(uint8_t)((uint16_t)value & 0xFFU), (uint8_t)((uint16_t)value >> 8)};
    return dsd_write(fd, b, sizeof b) == (ssize_t)sizeof b;
}

/* A tune the peer confirms: on the decoder thread, the io_control tune a command makes (the frequency entry, a manual
   tune), which reconciles no tune made off the thread first; off it, the P25 watchdog's trunk tune, which posts its
   mark for the decoder thread. */
static int
tune_on(dsd_opts* opts, dsd_state* state, long hz, int decoder_thread) {
    if (decoder_thread) {
        uint64_t pending_id = 0U;
        return svc_io_control_tune(opts, state, (uint32_t)hz, &pending_id) == 0;
    }
    dsd_engine_set_decoder_thread(0);
    const int ok = dsd_engine_trunk_tune_to_freq_request(opts, state, hz, 0, 0U) == DSD_TRUNK_TUNE_RESULT_OK;
    dsd_engine_set_decoder_thread(1);
    return ok;
}

/* The next sample the stdin reader reads, or INT16_MIN for none within a second. */
static int16_t
pipe_next(dsd_opts* opts) {
    int16_t s = 0;
    return dsd_pcm_stream_read_wait(opts->audio_in_stdin, &s, 1000U) == DSD_PCM_STREAM_SAMPLE ? s : INT16_MIN;
}

/* Whether two marks are the same mark. */
static int
same_cut(const dsd_input_cut* a, const dsd_input_cut* b) {
    return a->seq == b->seq && a->arrived == b->arrived && a->marked_ns == b->marked_ns && a->measured == b->measured;
}

/* What the first read after the case's tunes returns, and the epoch moves it made, against what the case expects. */
static int
expect_newest(const char* what, dsd_opts* opts, const dsd_state* state, int16_t want, uint32_t e, uint32_t moves) {
    char label[200];
    const int16_t got = pipe_next(opts);
    DSD_SNPRINTF(label, sizeof label, "%s: the first sample read is %d (read %d), the epoch moved %u (moved %u)", what,
                 (int)want, (int)got, moves, state->symbol_clock_epoch - e);
    return expect_true(label, got == want && state->symbol_clock_epoch - e == moves && state->input_drop_pending == 0U);
}

/* For one input the pending cutoff is the newest mark, whichever path installs it and in whatever order they are
   taken: a watchdog post taken after the decoder thread's own later retune never brings back the audio that retune
   dropped, and moves no epoch (that retune moved it, and no symbol came between them). */
static int
test_newest_cutoff_wins(void) {
    rig_case c;
    if (rig_case_open(&c, AUDIO_IN_STDIN) != 0) {
        return expect_true("newest cutoff: the peer", 0);
    }
    dsd_opts* opts = c.opts;
    dsd_state* state = c.state;
    int fds[2] = {-1, -1};
    int rc = expect_true("pipe", make_pipe(fds) == 0);
    opts->audio_in_stdin = rc == 0 ? dsd_pcm_stream_open_fd(fds[0]) : NULL;
    if (opts->audio_in_stdin == NULL) {
        rig_case_close(&c);
        return expect_true("stdin reader", 0);
    }
    rc |= expect_true("on a voice channel", tune_on(opts, state, VC1_HZ, 1));
    dsd_symbol_apply_input_drop(opts, state);
    dsd_frame_sync_off_thread_tunes_reset(state);
    state->symbol_clock_input = opts->pcm_input_generation;

    /* The watchdog's A->B post, then a decoder-thread B->C retune before the next read: the post is older. */
    uint32_t e = state->symbol_clock_epoch;
    rc |= expect_true("A's audio", pipe_put(fds[1], 10));
    rc |= expect_true("watchdog A->B", tune_on(opts, state, VC2_HZ, 0));
    rc |= expect_true("B's audio", pipe_put(fds[1], 20));
    rc |= expect_true("io_control B->C", tune_on(opts, state, VC3_HZ, 1));
    take_at_the_next_read(opts, state);
    rc |= expect_true("C's audio", pipe_put(fds[1], 30));
    rc |= expect_newest("an older watchdog post taken after a newer direct retune", opts, state, 30, e, 1U);
    (void)dsd_frame_sync_apply_off_thread_tunes(opts, state);

    /* The decoder thread's retune, then a watchdog post before the next read: the post is newer, and is taken. */
    e = state->symbol_clock_epoch;
    rc |= expect_true("C's audio again", pipe_put(fds[1], 40));
    rc |= expect_true("io_control C->A", tune_on(opts, state, VC1_HZ, 1));
    rc |= expect_true("A's audio again", pipe_put(fds[1], 50));
    rc |= expect_true("watchdog A->B again", tune_on(opts, state, VC2_HZ, 0));
    take_at_the_next_read(opts, state);
    rc |= expect_true("B's audio again", pipe_put(fds[1], 60));
    rc |= expect_newest("a newer watchdog post taken after an older direct retune", opts, state, 60, e, 2U);
    (void)dsd_frame_sync_apply_off_thread_tunes(opts, state);

    /* Two posts before one take: the later one is the cutoff. */
    e = state->symbol_clock_epoch;
    rc |= expect_true("B's audio, third", pipe_put(fds[1], 70));
    rc |= expect_true("watchdog B->C", tune_on(opts, state, VC3_HZ, 0));
    rc |= expect_true("C's audio, third", pipe_put(fds[1], 80));
    rc |= expect_true("watchdog C->A", tune_on(opts, state, VC1_HZ, 0));
    take_at_the_next_read(opts, state);
    rc |= expect_true("A's audio, third", pipe_put(fds[1], 90));
    rc |= expect_newest("two posts before one take", opts, state, 90, e, 1U);
    (void)dsd_frame_sync_apply_off_thread_tunes(opts, state);

    /* The newer direct retune's drop already carried out when the older post is taken: the post asks for nothing. */
    e = state->symbol_clock_epoch;
    rc |= expect_true("A's audio, fourth", pipe_put(fds[1], 100));
    rc |= expect_true("watchdog A->B, fourth", tune_on(opts, state, VC2_HZ, 0));
    rc |= expect_true("B's audio, fourth", pipe_put(fds[1], 110));
    /* The decoder thread's retune runs between the watchdog's post and its count (frame_sync_take_unflushed_retunes()
       has not seen the post yet): its drop is carried out first. */
    rc |= expect_true("io_control B->C, fourth", tune_on(opts, state, VC3_HZ, 1));
    dsd_symbol_apply_input_drop(opts, state);
    const dsd_input_cut held = state->input_drop_cut;
    rc |= expect_true("C's audio, fourth", pipe_put(fds[1], 120));
    take_at_the_next_read(opts, state);
    rc |= expect_true("the older post leaves the newer cutoff in place", same_cut(&held, &state->input_drop_cut));
    rc |= expect_newest("an older post taken after the newer drop was carried out", opts, state, 120, e, 1U);
    (void)dsd_frame_sync_apply_off_thread_tunes(opts, state);

    /* A take racing a post: the watchdog's second post has marked the input, its count not yet raised, when the decoder
       takes the first one's count. The take carries the latest mark (the later post's), and the second count, when it
       comes, finds nothing left to take. */
    e = state->symbol_clock_epoch;
    rc |= expect_true("C's audio, fifth", pipe_put(fds[1], 150));
    rc |= expect_true("watchdog C->A, fifth", tune_on(opts, state, VC1_HZ, 0));
    rc |= expect_true("A's audio, fifth", pipe_put(fds[1], 160));
    dsd_input_cut second;
    dsd_input_backlog_mark(&second);
    dsd_input_backlog_post(&second); /* the second post's mark; its count comes after the take */
    take_at_the_next_read(opts, state);
    rc |= expect_true("audio after the second post", pipe_put(fds[1], 170));
    rc |= expect_newest("a take racing a post", opts, state, 170, e, 1U);
    (void)dsd_frame_sync_apply_off_thread_tunes(opts, state);

    /* The request itself, whatever asks: of two marks of the input, the later one stands in either order. */
    dsd_input_cut early;
    dsd_input_cut late;
    dsd_input_backlog_mark(&early);
    rc |= expect_true("audio between the marks", pipe_put(fds[1], 130));
    dsd_input_backlog_mark(&late);
    dsd_frame_sync_request_input_drop(opts, state, &late);
    dsd_frame_sync_request_input_drop(opts, state, &early);
    rc |= expect_true("an earlier mark asked for after a later one leaves the later one",
                      state->input_drop_pending == 1U && same_cut(&late, &state->input_drop_cut));
    dsd_symbol_apply_input_drop(opts, state);
    rc |= expect_true("audio after both marks", pipe_put(fds[1], 140));
    rc |= expect_true("the later mark's drop takes what came between them", pipe_next(opts) == 140);

    dsd_pcm_stream_close(opts->audio_in_stdin);
    opts->audio_in_stdin = NULL;
    (void)dsd_close(fds[0]);
    (void)dsd_close(fds[1]);
    rig_case_close(&c);
    return rc;
}

/* ---- The cutoff is marked immediately before the frequency is sent (issue #834) ---- */

/* A rigctl leg that asks the peer for @p hz: 1 when the peer took it. */
typedef int (*rig_leg_fn)(dsd_opts* opts, dsd_state* state, long hz);

static int
leg_trunk(dsd_opts* opts, dsd_state* state, long hz) {
    return dsd_engine_trunk_tune_to_freq_request(opts, state, hz, 0, 0U) == DSD_TRUNK_TUNE_RESULT_OK;
}

static int
leg_watchdog(dsd_opts* opts, dsd_state* state, long hz) {
    return tune_on(opts, state, hz, 0);
}

static int
leg_io_control(dsd_opts* opts, dsd_state* state, long hz) {
    return tune_on(opts, state, hz, 1);
}

static int
leg_scan(dsd_opts* opts, dsd_state* state, long hz) {
    uint64_t request_id = 0U;
    return dsd_engine_scan_tune_to_freq(opts, state, hz, 0, 0U, &request_id) == DSD_TRUNK_TUNE_RESULT_OK;
}

static int
leg_cc_return(dsd_opts* opts, dsd_state* state, long hz) {
    state->trunk_cc_freq = hz;
    return dsd_engine_return_to_cc_request(opts, state, 0U) == DSD_TRUNK_TUNE_RESULT_OK;
}

/* The no-carrier pass's own control-channel return: the legacy leg. */
static int
leg_no_carrier_return(dsd_opts* opts, dsd_state* state, long hz) {
    state->trunk_cc_freq = hz;
    opts->trunk_is_tuned = 1;
    state->lastsynctype = DSD_SYNC_DMR_BS_DATA_POS;
    state->last_vc_sync_time = 0;
    noCarrier(opts, state);
    return opts->rigctl_sent_freq_hz == (uint32_t)hz;
}

/* A peer that applies the frequency and sends the new channel's audio before its reply: the audio queued before the
   request goes at the next read, and the new channel's, which reached the input before the reply, is read. */
static int
new_audio_case(dsd_opts* opts, dsd_state* state, fake_rig* rig, int fd, rig_leg_fn leg, long hz, const char* what) {
    char label[200];
    const int new_value = atomic_load(&rig->f_audio_value);
    int rc = expect_true("the old channel's audio", pipe_put(fd, 7) && pipe_put(fd, 8));
    DSD_SNPRINTF(label, sizeof label, "%s: the peer took the frequency", what);
    rc |= expect_true(label, leg(opts, state, hz));
    take_at_the_next_read(opts, state);
    const int16_t first = pipe_next(opts);
    const int16_t second = first == INT16_MIN ? INT16_MIN : pipe_next(opts);
    DSD_SNPRINTF(label, sizeof label,
                 "%s: the new channel's audio sent before the reply is read (read %d, %d; expected %d, %d)", what,
                 (int)first, (int)second, new_value, new_value + 1);
    rc |= expect_true(label, first == (int16_t)new_value && second == (int16_t)(new_value + 1));
    return rc;
}

/* Every rigctl leg marks the input immediately before it sends the frequency, and installs that mark only once the
   peer took it: a peer that moves and sends new audio before it answers loses none of it. A refused or lost request
   installs nothing. */
static int
test_new_audio_before_the_reply(void) {
    rig_case c;
    if (rig_case_open(&c, AUDIO_IN_STDIN) != 0) {
        return expect_true("before the reply: the peer", 0);
    }
    dsd_opts* opts = c.opts;
    dsd_state* state = c.state;
    int fds[2] = {-1, -1};
    int rc = expect_true("pipe", make_pipe(fds) == 0);
    opts->audio_in_stdin = rc == 0 ? dsd_pcm_stream_open_fd(fds[0]) : NULL;
    if (opts->audio_in_stdin == NULL) {
        rig_case_close(&c);
        return expect_true("stdin reader", 0);
    }
    dsd_frame_sync_off_thread_tunes_reset(state);
    state->symbol_clock_input = opts->pcm_input_generation;
    atomic_store(&c.rig.f_audio_value, 1000);
    atomic_store(&c.rig.audio_fd, fds[1]);
    rc |= new_audio_case(opts, state, &c.rig, fds[1], leg_trunk, VC1_HZ, "a trunk tune");
    rc |= new_audio_case(opts, state, &c.rig, fds[1], leg_watchdog, VC2_HZ, "the watchdog's tune");
    (void)dsd_frame_sync_apply_off_thread_tunes(opts, state);
    rc |= new_audio_case(opts, state, &c.rig, fds[1], leg_io_control, VC3_HZ, "an io_control tune");
    rc |= new_audio_case(opts, state, &c.rig, fds[1], leg_scan, VC1_HZ, "a scan tune");
    rc |= new_audio_case(opts, state, &c.rig, fds[1], leg_cc_return, CC_HZ, "a return");
    rc |= new_audio_case(opts, state, &c.rig, fds[1], leg_trunk, VC2_HZ, "a grant after it");
    rc |= new_audio_case(opts, state, &c.rig, fds[1], leg_no_carrier_return, CC_HZ, "the no-carrier return");

    /* A refused request and a lost reply install nothing: what was queued is read on. */
    atomic_store(&c.rig.f_mode, RIG_REFUSES);
    rc |= expect_true("audio before a refusal", pipe_put(fds[1], 21));
    rc |= expect_true("refused", !leg_trunk(opts, state, VC3_HZ));
    take_at_the_next_read(opts, state);
    rc |= expect_true("a refused request drops nothing", pipe_next(opts) == 21);
    const uint32_t e = state->symbol_clock_epoch;
    rc |= expect_true("audio before a refused watchdog tune", pipe_put(fds[1], 22));
    rc |= expect_true("refused off the thread", !leg_watchdog(opts, state, VC3_HZ));
    take_at_the_next_read(opts, state);
    rc |= expect_true("a refused watchdog tune posts nothing",
                      pipe_next(opts) == 22 && state->symbol_clock_epoch == e && dsd_input_backlog_take(NULL) == 0);
    atomic_store(&c.rig.f_mode, RIG_DROPS);
    rc |= expect_true("audio before a lost reply", pipe_put(fds[1], 23));
    rc |= expect_true("lost", !leg_io_control(opts, state, VC3_HZ));
    take_at_the_next_read(opts, state);
    rc |= expect_true("a lost reply drops nothing", pipe_next(opts) == 23 && state->symbol_clock_epoch == e);
    atomic_store(&c.rig.f_mode, RIG_TAKES);
    atomic_store(&c.rig.audio_fd, -1);

    dsd_pcm_stream_close(opts->audio_in_stdin);
    opts->audio_in_stdin = NULL;
    (void)dsd_close(fds[0]);
    (void)dsd_close(fds[1]);
    rig_case_close(&c);
    return rc;
}

/* Issue #834: a reply that is not a complete numeric RPRT (garbled, malformed, cut short) leaves the peer's frequency
   unknown: it may have moved. The next tune goes out whatever the frequency last confirmed, and counts as a move: what
   the input held is dropped, even for a tune back to that frequency. A complete RPRT refusal leaves the peer where it
   was, as known: a tune back there sends nothing and drops nothing. On the trunk, watchdog, io_control and scan legs. */
static int
unknown_frequency_case(rig_leg_fn leg, const char* what) {
    rig_case c;
    if (rig_case_open(&c, AUDIO_IN_STDIN) != 0) {
        return expect_true("unknown frequency: the peer", 0);
    }
    dsd_opts* opts = c.opts;
    dsd_state* state = c.state;
    int fds[2] = {-1, -1};
    int rc = expect_true("unknown frequency: pipe", make_pipe(fds) == 0);
    opts->audio_in_stdin = rc == 0 ? dsd_pcm_stream_open_fd(fds[0]) : NULL;
    if (opts->audio_in_stdin == NULL) {
        rig_case_close(&c);
        return expect_true("unknown frequency: stdin reader", 0);
    }
    dsd_frame_sync_off_thread_tunes_reset(state);
    state->symbol_clock_input = opts->pcm_input_generation;
    char label[200];
    rc |= expect_true("unknown frequency: on A", leg(opts, state, VC1_HZ));
    take_at_the_next_read(opts, state);
    static const int k_ambiguous[] = {RIG_MOVES_GARBLED, RIG_MOVES_MALFORMED};
    for (size_t i = 0; i < sizeof k_ambiguous / sizeof k_ambiguous[0]; i++) {
        atomic_store(&c.rig.f_mode, k_ambiguous[i]);
        DSD_SNPRINTF(label, sizeof label, "%s, reply %d: the tune to B is not confirmed", what, k_ambiguous[i]);
        rc |= expect_true(label, !leg(opts, state, VC2_HZ));
        take_at_the_next_read(opts, state);
        rc |= expect_true("unknown frequency: queued audio", pipe_put(fds[1], (int16_t)(31 + i)));
        const int taken = atomic_load(&c.rig.f_taken);
        const uint32_t e = state->symbol_clock_epoch;
        DSD_SNPRINTF(label, sizeof label, "%s, reply %d: the tune back to A is taken", what, k_ambiguous[i]);
        rc |= expect_true(label, leg(opts, state, VC1_HZ));
        take_at_the_next_read(opts, state);
        rc |= expect_true("unknown frequency: audio after the tune", pipe_put(fds[1], 41));
        const int16_t next = pipe_next(opts);
        DSD_SNPRINTF(
            label, sizeof label,
            "%s, reply %d: the tune back to A goes out and counts as a move (sent %d, epoch moved %u, read %d)", what,
            k_ambiguous[i], atomic_load(&c.rig.f_taken) - taken, state->symbol_clock_epoch - e, (int)next);
        rc |= expect_true(label,
                          atomic_load(&c.rig.f_taken) == taken + 1 && state->symbol_clock_epoch != e && next == 41);
    }
    /* A complete refusal: the peer stayed on A, as known. */
    atomic_store(&c.rig.f_mode, RIG_REFUSES);
    rc |= expect_true("unknown frequency: B refused", !leg(opts, state, VC2_HZ));
    take_at_the_next_read(opts, state);
    atomic_store(&c.rig.f_mode, RIG_TAKES);
    rc |= expect_true("unknown frequency: queued audio after the refusal", pipe_put(fds[1], 51));
    int taken = atomic_load(&c.rig.f_taken);
    uint32_t e = state->symbol_clock_epoch;
    rc |= expect_true("unknown frequency: A after the refusal", leg(opts, state, VC1_HZ));
    take_at_the_next_read(opts, state);
    int16_t next = pipe_next(opts);
    DSD_SNPRINTF(
        label, sizeof label,
        "%s: after a refusal the peer is on A as known: nothing sent, nothing dropped (sent %d, epoch moved %u, "
        "read %d)",
        what, atomic_load(&c.rig.f_taken) - taken, state->symbol_clock_epoch - e, (int)next);
    rc |= expect_true(label, atomic_load(&c.rig.f_taken) == taken && state->symbol_clock_epoch == e && next == 51);
    (void)dsd_frame_sync_apply_off_thread_tunes(opts, state);
    dsd_pcm_stream_close(opts->audio_in_stdin);
    opts->audio_in_stdin = NULL;
    (void)dsd_close(fds[0]);
    (void)dsd_close(fds[1]);
    rig_case_close(&c);
    return rc;
}

static int
test_an_unreadable_tune_reply_leaves_the_frequency_unknown(void) {
    return unknown_frequency_case(leg_trunk, "trunk") | unknown_frequency_case(leg_watchdog, "watchdog")
           | unknown_frequency_case(leg_io_control, "io_control") | unknown_frequency_case(leg_scan, "scan");
}

/* The legacy rigctl leg (the untyped -Y step, the no-carrier control-channel return) skips a frequency it last sent
   only while the peer still confirms it (RigctlConfirmedFreq()): a manual tune to another frequency whose reply the
   client could not read may have moved the peer, so the next return to the cached frequency goes out again. */
static int
test_the_legacy_leg_resends_after_an_unknown_manual_tune(void) {
    rig_case c;
    if (rig_case_open(&c, AUDIO_IN_STDIN) != 0) {
        return expect_true("legacy cache: the peer", 0);
    }
    dsd_opts* opts = c.opts;
    dsd_state* state = c.state;
    int rc =
        expect_true("legacy cache: the return reaches the control channel", leg_no_carrier_return(opts, state, CC_HZ));
    int taken = atomic_load(&c.rig.f_taken);
    rc |= expect_true("legacy cache: a repeat of the return sends nothing",
                      leg_no_carrier_return(opts, state, CC_HZ) && atomic_load(&c.rig.f_taken) == taken);

    atomic_store(&c.rig.f_mode, RIG_MOVES_GARBLED); /* the peer moves to B, and its reply cannot be read */
    rc |= expect_true("legacy cache: the manual tune to B is not confirmed", !leg_io_control(opts, state, VC1_HZ));
    rc |= expect_true("legacy cache: the peer's frequency is unknown", RigctlConfirmedFreq(opts->rigctl_sockfd) == 0U);
    taken = atomic_load(&c.rig.f_taken);
    rc |= expect_true("legacy cache: the return to the control channel is taken",
                      leg_no_carrier_return(opts, state, CC_HZ));
    char label[160];
    DSD_SNPRINTF(label, sizeof label, "legacy cache: the return goes out again (sent %d), and the peer confirms it",
                 atomic_load(&c.rig.f_taken) - taken);
    rc |= expect_true(label, atomic_load(&c.rig.f_taken) == taken + 1
                                 && RigctlConfirmedFreq(opts->rigctl_sockfd) == (uint32_t)CC_HZ);
    rig_case_close(&c);
    return rc;
}

/* DMR's attribution of the receiver (the control channel it parked on, dmr_sm_ctx_t::cc_rx_freq_hz) holds only while
   the rigctl peer's position is known: a manual tune whose reply could not be read may have moved the peer, so the
   machine forgets where the receiver is and its hunt sends the control channel again, rather than listening on as if
   still there. A complete refusal leaves the peer where it was, and the attribution with it. */
static int
test_an_unknown_manual_tune_forgets_dmr_receiver(void) {
    rig_case c;
    if (rig_case_open(&c, AUDIO_IN_PULSE) != 0) {
        return expect_true("dmr receiver: the peer", 0);
    }
    dsd_opts* opts = c.opts;
    dsd_state* state = c.state;
    dmr_sm_init(opts, state);
    dmr_sm_ctx_t* ctx = dmr_sm_get_ctx();
    int rc = expect_true("dmr receiver: the peer is on the control channel", leg_io_control(opts, state, CC_HZ));
    dmr_sm_begin_cc_acquisition(ctx, opts, state, CC_HZ, 0U);
    rc |= expect_true("dmr receiver: DMR parks there", ctx->cc_rx_freq_hz == CC_HZ);

    atomic_store(&c.rig.f_mode, RIG_REFUSES);
    rc |= expect_true("dmr receiver: a manual tune the peer refuses", !leg_io_control(opts, state, VC1_HZ));
    rc |= expect_true("dmr receiver: a refusal keeps where the receiver is",
                      ctx->cc_rx_freq_hz == CC_HZ && opts->rigctl_sent_freq_hz == (uint32_t)CC_HZ);

    atomic_store(&c.rig.f_mode, RIG_MOVES_GARBLED);
    rc |= expect_true("dmr receiver: a manual tune whose reply is lost", !leg_io_control(opts, state, VC1_HZ));
    char label[160];
    DSD_SNPRINTF(label, sizeof label,
                 "dmr receiver: the receiver is no longer taken to be on the control channel (%ld)",
                 ctx->cc_rx_freq_hz);
    rc |= expect_true(label, ctx->cc_rx_freq_hz == 0 && opts->rigctl_sent_freq_hz == 0U);
    dmr_sm_init(opts, state);
    rig_case_close(&c);
    return rc;
}

/* A peer that is slow to answer a passband request (M) is still on the old frequency while it does: the old channel's
   audio that reaches the input then goes at the next read with what was queued before the tune, and the new channel's,
   which reached it before the frequency's reply, is read. Every rigctl leg asks for a passband here (a -B on audio
   input, a new one for each tune so the client's record of the peer skips none). */
static int
test_old_audio_while_the_passband_is_set(void) {
    rig_case c;
    if (rig_case_open(&c, AUDIO_IN_STDIN) != 0) {
        return expect_true("passband: the peer", 0);
    }
    dsd_opts* opts = c.opts;
    dsd_state* state = c.state;
    int fds[2] = {-1, -1};
    int rc = expect_true("pipe", make_pipe(fds) == 0);
    opts->audio_in_stdin = rc == 0 ? dsd_pcm_stream_open_fd(fds[0]) : NULL;
    if (opts->audio_in_stdin == NULL) {
        rig_case_close(&c);
        return expect_true("stdin reader", 0);
    }
    dsd_frame_sync_off_thread_tunes_reset(state);
    state->symbol_clock_input = opts->pcm_input_generation;
    atomic_store(&c.rig.f_audio_value, 1000);
    atomic_store(&c.rig.m_audio_value, 500);
    atomic_store(&c.rig.audio_fd, fds[1]);

    static const struct {
        rig_leg_fn leg;
        long hz;
        const char* what;
    } k_legs[] = {
        {leg_trunk, VC1_HZ, "passband, a trunk tune"},
        {leg_watchdog, VC2_HZ, "passband, the watchdog's tune"},
        {leg_io_control, VC3_HZ, "passband, an io_control tune"},
        {leg_scan, VC1_HZ, "passband, a scan tune"},
        {leg_cc_return, CC_HZ, "passband, a return"},
        {leg_trunk, VC2_HZ, "passband, a grant after it"},
        {leg_no_carrier_return, CC_HZ, "passband, the no-carrier return"},
    };

    for (size_t i = 0U; i < sizeof k_legs / sizeof k_legs[0]; i++) {
        char label[200];
        opts->setmod_bw = 6000 + (int)i * 500;
        const int m_before = atomic_load(&c.rig.m_taken);
        rc |= new_audio_case(opts, state, &c.rig, fds[1], k_legs[i].leg, k_legs[i].hz, k_legs[i].what);
        DSD_SNPRINTF(label, sizeof label, "%s: the leg asked for a passband", k_legs[i].what);
        rc |= expect_true(label, atomic_load(&c.rig.m_taken) > m_before);
        if (k_legs[i].leg == leg_watchdog) {
            (void)dsd_frame_sync_apply_off_thread_tunes(opts, state);
        }
    }
    atomic_store(&c.rig.audio_fd, -1);
    opts->setmod_bw = 0;

    dsd_pcm_stream_close(opts->audio_in_stdin);
    opts->audio_in_stdin = NULL;
    (void)dsd_close(fds[0]);
    (void)dsd_close(fds[1]);
    rig_case_close(&c);
    return rc;
}

/* An 8 kHz stdin input, upsampled six times through the staged resampler (issue #834): the last sample the decoder read
   left its upsampled outputs staged and its FIR history. Read before the mark, they are the old channel's, and the
   drop takes them with the rest: the next sample re-primes the history. Read after the mark (a watchdog's confirmation
   taken late), they are the new channel's, and are kept. */
static int
staged_after_read(dsd_opts* opts, int16_t want) {
    int16_t s = pipe_next(opts);
    if (s != want) {
        return 0;
    }
    dsd_pcm_input_stage_resample(opts, (float)s);
    opts->input_upsample_pos = 1; /* the decoder took the first of the staged outputs */
    return opts->input_upsample_len == 6 && opts->input_upsample_prev_valid == 1;
}

static int
test_staged_low_rate_pcm(void) {
    rig_case c;
    if (rig_case_open(&c, AUDIO_IN_STDIN) != 0) {
        return expect_true("staged: the peer", 0);
    }
    dsd_opts* opts = c.opts;
    dsd_state* state = c.state;
    opts->wav_sample_rate = 8000;
    int fds[2] = {-1, -1};
    int rc = expect_true("pipe", make_pipe(fds) == 0);
    opts->audio_in_stdin = rc == 0 ? dsd_pcm_stream_open_fd(fds[0]) : NULL;
    if (opts->audio_in_stdin == NULL) {
        rig_case_close(&c);
        return expect_true("stdin reader", 0);
    }
    dsd_frame_sync_off_thread_tunes_reset(state);
    state->symbol_clock_input = opts->pcm_input_generation;
    rc |= expect_true("staged: an upsampled input", dsd_pcm_input_uses_staged_resampler(opts) == 1);
    rc |= expect_true("staged: old samples", pipe_put(fds[1], 300) && pipe_put(fds[1], 301));
    rc |= expect_true("staged: the decoder read an old sample", staged_after_read(opts, 300));
    rc |= expect_true("staged: a hop", leg_trunk(opts, state, VC1_HZ));
    dsd_symbol_apply_input_drop(opts, state);
    char label[200];
    DSD_SNPRINTF(
        label, sizeof label,
        "staged: the old sample's staged outputs and FIR history go with the drop (staged %d of %d, primed %d)",
        opts->input_upsample_len - opts->input_upsample_pos, opts->input_upsample_len, opts->input_upsample_prev_valid);
    rc |= expect_true(label,
                      opts->input_upsample_pos >= opts->input_upsample_len && opts->input_upsample_prev_valid == 0);
    rc |= expect_true("staged: the new channel's sample", pipe_put(fds[1], 400));
    rc |= expect_true("staged: the old sample still queued went too", pipe_next(opts) == 400);

    /* The watchdog's hop, its confirmation taken after the decoder read the new channel's first sample. */
    rc |= expect_true("staged: the watchdog's hop", leg_watchdog(opts, state, VC2_HZ));
    rc |= expect_true("staged: the new channel's samples", pipe_put(fds[1], 500) && pipe_put(fds[1], 501));
    rc |= expect_true("staged: the decoder read a new sample first", staged_after_read(opts, 500));
    take_at_the_next_read(opts, state);
    rc |= expect_true("staged: what was staged after the mark is kept", opts->input_upsample_pos == 1
                                                                            && opts->input_upsample_len == 6
                                                                            && opts->input_upsample_prev_valid == 1);
    rc |= expect_true("staged: and the input reads on", pipe_next(opts) == 501);
    (void)dsd_frame_sync_apply_off_thread_tunes(opts, state);
    dsd_resampler_reset(&opts->input_resampler); /* the staged resampler's FIR, as the input's close frees it */

    dsd_pcm_stream_close(opts->audio_in_stdin);
    opts->audio_in_stdin = NULL;
    (void)dsd_close(fds[0]);
    (void)dsd_close(fds[1]);
    rig_case_close(&c);
    return rc;
}

/* Issue #834: 8 kHz PCM on stdin redirected from a file (no pipe): a retune's cutoff does not apply to it, so the drop
   keeps what the low-rate resampler staged and its FIR history, as it keeps the rest of the file: the staged outputs of
   the sample read are played, then the file reads on. */
/* Issue #834: a watchdog's retune confirmed late, after the decoder read the new channel's first sample: the drop finds
   the input read past its mark and keeps what was staged. That sample's staged outputs and the FIR history still came
   through the old channel's samples (1000, 1000, 1000): they are rebuilt from the new channel alone, as a stream that
   starts on it, so the old channel crosses no timeline boundary. */
static int
test_staged_low_rate_pcm_late_confirmation(void) {
    rig_case c;
    if (rig_case_open(&c, AUDIO_IN_STDIN) != 0) {
        return expect_true("late staged: the peer", 0);
    }
    dsd_opts* opts = c.opts;
    dsd_state* state = c.state;
    opts->wav_sample_rate = 8000;
    int fds[2] = {-1, -1};
    int rc = expect_true("late staged: pipe", make_pipe(fds) == 0);
    opts->audio_in_stdin = rc == 0 ? dsd_pcm_stream_open_fd(fds[0]) : NULL;
    if (opts->audio_in_stdin == NULL) {
        rig_case_close(&c);
        return expect_true("late staged: stdin reader", 0);
    }
    dsd_frame_sync_off_thread_tunes_reset(state);
    state->symbol_clock_input = opts->pcm_input_generation;
    for (int i = 0; i < 3; i++) {
        rc |= expect_true("late staged: an old sample", pipe_put(fds[1], 1000));
    }
    for (int i = 0; i < 3; i++) {
        rc |= expect_true("late staged: an old sample read", staged_after_read(opts, 1000));
    }
    rc |= expect_true("late staged: the watchdog's hop", leg_watchdog(opts, state, VC2_HZ));
    for (int i = 0; i < 2; i++) {
        rc |= expect_true("late staged: a new sample", pipe_put(fds[1], 0));
    }
    const int16_t s = pipe_next(opts);
    dsd_pcm_input_stage_resample(opts, (float)s);
    opts->input_upsample_pos = 1; /* the decoder took the first of the staged outputs */
    take_at_the_next_read(opts, state);
    int flat = s == 0 && opts->input_upsample_len == 6 && opts->input_upsample_pos == 1;
    for (int i = opts->input_upsample_pos; i < opts->input_upsample_len; i++) {
        const float d = opts->input_upsample_buf[i];
        flat &= d > -0.001f && d < 0.001f;
    }
    char label[200];
    DSD_SNPRINTF(label, sizeof label,
                 "late staged: the new sample's staged outputs carry no old-channel sample (next %.3f, want 0)",
                 (double)opts->input_upsample_buf[1]);
    rc |= expect_true(label, flat);
    rc |= expect_true("late staged: and the input reads on", pipe_next(opts) == 0);
    (void)dsd_frame_sync_apply_off_thread_tunes(opts, state);
    dsd_resampler_reset(&opts->input_resampler);
    dsd_pcm_stream_close(opts->audio_in_stdin);
    opts->audio_in_stdin = NULL;
    (void)dsd_close(fds[0]);
    (void)dsd_close(fds[1]);
    rig_case_close(&c);
    return rc;
}

static int
test_staged_low_rate_pcm_from_a_file(void) {
    char path[DSD_TEST_PATH_MAX];
    const int fd = dsd_test_mkstemp(path, sizeof path, "dsd-neo-rigctl-staged-file");
    if (fd < 0) {
        return expect_true("staged file: temp file", 0);
    }
    int rc = expect_true("staged file: samples", pipe_put(fd, 300) && pipe_put(fd, 301));
#if DSD_PLATFORM_WIN_NATIVE
    rc |= expect_true("staged file: rewound", _lseek(fd, 0, SEEK_SET) == 0);
#else
    rc |= expect_true("staged file: rewound", lseek(fd, 0, SEEK_SET) == 0);
#endif
    rig_case c;
    if (rig_case_open(&c, AUDIO_IN_STDIN) != 0) {
        (void)dsd_close(fd);
        (void)remove(path);
        return expect_true("staged file: the peer", 0);
    }
    dsd_opts* opts = c.opts;
    dsd_state* state = c.state;
    opts->wav_sample_rate = 8000;
    opts->audio_in_stdin = dsd_pcm_stream_open_fd(fd);
    rc |= expect_true("staged file: stdin reader", opts->audio_in_stdin != NULL);
    dsd_frame_sync_off_thread_tunes_reset(state);
    state->symbol_clock_input = opts->pcm_input_generation;
    if (opts->audio_in_stdin != NULL) {
        rc |= expect_true("staged file: the decoder read a sample", staged_after_read(opts, 300));
        rc |= expect_true("staged file: a hop", leg_trunk(opts, state, VC1_HZ));
        dsd_symbol_apply_input_drop(opts, state);
        char label[200];
        DSD_SNPRINTF(label, sizeof label,
                     "staged file: the staged outputs and FIR history stay (staged %d of %d, primed %d)",
                     opts->input_upsample_len - opts->input_upsample_pos, opts->input_upsample_len,
                     opts->input_upsample_prev_valid);
        rc |= expect_true(label, opts->input_upsample_pos == 1 && opts->input_upsample_len == 6
                                     && opts->input_upsample_prev_valid == 1);
        rc |= expect_true("staged file: and the file reads on", pipe_next(opts) == 301);
        dsd_pcm_stream_close(opts->audio_in_stdin);
        opts->audio_in_stdin = NULL;
    }
    dsd_resampler_reset(&opts->input_resampler);
    (void)dsd_close(fd);
    (void)remove(path);
    rig_case_close(&c);
    return rc;
}

/* A port nothing listens on: bound, then closed. */
static int
closed_tcp_port(void) {
    dsd_socket_t s = dsd_socket_create(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == DSD_INVALID_SOCKET) {
        return -1;
    }
    struct sockaddr_in sa;
    DSD_MEMSET(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = 0;
    int port = -1;
#if DSD_PLATFORM_WIN_NATIVE
    int len = (int)sizeof sa;
#else
    socklen_t len = (socklen_t)sizeof sa;
#endif
    if (dsd_socket_bind(s, (struct sockaddr*)&sa, (int)sizeof sa) == 0
        && getsockname(s, (struct sockaddr*)&sa, &len) == 0) {
        port = (int)ntohs(sa.sin_port);
    }
    (void)dsd_socket_close(s);
    return port;
}

/* Issue #834: only a rigctl peer's retune is posted off the decoder thread, and only while rigctl is in use, so the
   symbol reader checks for a post only then: a run without rigctl pays no per-symbol load of the shared count (a
   locked operation on the MSVC atomics fallback). A post still outstanding when rigctl stops being in use (a reconnect
   that failed) is taken there, so none is left for the reader to miss. */
static int
test_symbol_reader_checks_posts_only_with_rigctl(void) {
    rig_case c;
    if (rig_case_open(&c, AUDIO_IN_PULSE) != 0) {
        return expect_true("posts: the peer", 0);
    }
    dsd_opts* opts = c.opts;
    dsd_state* state = c.state;
    opts->audio_in_stream = NULL; /* reads return at once */
    dsd_frame_sync_off_thread_tunes_reset(state);
    dsd_input_cut cut;
    dsd_input_backlog_mark(&cut);
    dsd_frame_sync_note_off_thread_unflushed_retune(&cut);
    uint32_t epoch = state->symbol_clock_epoch;
    opts->use_rigctl = 0;
    (void)getSymbol(opts, state, 0);
    int rc = expect_true("posts: a read without rigctl checks no post", state->symbol_clock_epoch == epoch);
    opts->use_rigctl = 1;
    (void)getSymbol(opts, state, 0);
    rc |= expect_true("posts: a read with rigctl takes it", state->symbol_clock_epoch == epoch + 1U);

    /* A post outstanding when a failed reconnect turns rigctl off: taken there. */
    dsd_input_backlog_mark(&cut);
    dsd_frame_sync_note_off_thread_unflushed_retune(&cut);
    epoch = state->symbol_clock_epoch;
    const dsd_socket_t live = opts->rigctl_sockfd;
    opts->rigctl_sockfd = DSD_INVALID_SOCKET; /* the connection was lost: a reconnect that fails turns rigctl off */
    const int port = closed_tcp_port();
    rc |=
        expect_true("posts: the failed reconnect", port > 0 && svc_rigctl_connect(opts, state, "127.0.0.1", port) != 0);
    rc |= expect_true("posts: rigctl is off", opts->use_rigctl == 0);
    rc |= expect_true("posts: the post outstanding is taken as rigctl stops", state->symbol_clock_epoch == epoch + 1U);
    opts->rigctl_sockfd = live;
    opts->use_rigctl = 1;
    rig_case_close(&c);
    return rc;
}

/* A case whose producer thread cannot start (a host out of threads) reports the failure and ends: it never waits on an
   input nothing will feed, nor joins a thread that does not exist. Both producers' cases are run with their thread
   made to fail; each must return its failure. */
static int
test_a_producer_that_cannot_start_ends_its_case(void) {
    g_failure_expected = 1;
    g_producers_to_fail = 1;
    const int late = test_watchdog_tune_drops_at_the_next_read();
    g_producers_to_fail = 1;
    const int queued = test_watchdog_post_drops_only_what_was_queued();
    g_producers_to_fail = 0;
    g_failure_expected = 0;
    int rc = expect_true("a late sender that cannot start fails its case", late != 0);
    rc |= expect_true("a watchdog that cannot start fails its case", queued != 0);
    return rc;
}

/* Every input reader carries out a pending drop before it reads: the symbol reader and EDACS analog voice. */
static int
test_readers_carry_out_the_drop(void) {
    rig_case c;
    if (rig_case_open(&c, AUDIO_IN_PULSE) != 0) {
        return expect_true("readers: the peer", 0);
    }
    dsd_opts* opts = c.opts;
    dsd_state* state = c.state;
    opts->audio_in_stream = NULL; /* reads return at once */
    int rc = expect_true("hop",
                         dsd_engine_trunk_tune_to_freq_request(opts, state, VC1_HZ, 0, 0U) == DSD_TRUNK_TUNE_RESULT_OK);
    rc |= expect_true("a drop is pending", state->input_drop_pending == 1U);
    (void)getSymbol(opts, state, 0);
    rc |= expect_true("the symbol reader carries it out", state->input_drop_pending == 0U);
    rc |= expect_true("hop again",
                      dsd_engine_trunk_tune_to_freq_request(opts, state, VC2_HZ, 0, 0U) == DSD_TRUNK_TUNE_RESULT_OK);
    short a1[960];
    short a2[960];
    short a3[960];
    double pwr = 0.0;
    (void)edacs_collect_analog_triplet(opts, state, NULL, a1, a2, a3, &pwr);
    rc |= expect_true("EDACS analog voice carries it out", state->input_drop_pending == 0U);
    rig_case_close(&c);
    return rc;
}

/* A WAV file holds no other channel: the confirmation asks for the drop, and the next read carries it out without
   moving the file's read position. */
static int
test_wav_input_unchanged(void) {
    char path[DSD_TEST_PATH_MAX];
    const int fd = dsd_test_mkstemp(path, sizeof path, "dsd-neo-rigctl-retune");
    if (fd < 0) {
        return expect_true("wav: temp file", 0);
    }
    (void)dsd_close(fd);
    SF_INFO info;
    DSD_MEMSET(&info, 0, sizeof info);
    info.samplerate = 48000;
    info.channels = 1;
    info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
    short samples[64] = {0};
    SNDFILE* out = sf_open(path, SFM_WRITE, &info);
    int rc = expect_true("wav written", out != NULL && sf_write_short(out, samples, 64) == 64);
    if (out != NULL) {
        sf_close(out);
    }
    rig_case c;
    if (rig_case_open(&c, AUDIO_IN_WAV) != 0) {
        (void)remove(path);
        return expect_true("wav: the peer", 0);
    }
    DSD_MEMSET(&info, 0, sizeof info);
    c.opts->audio_in_file = sf_open(path, SFM_READ, &info);
    rc |= expect_true("wav opened", c.opts->audio_in_file != NULL);
    if (c.opts->audio_in_file != NULL) {
        rc |= expect_true("ten read", sf_read_short(c.opts->audio_in_file, samples, 10) == 10);
        rc |= expect_true("tune", dsd_engine_trunk_tune_to_freq_request(c.opts, c.state, VC1_HZ, 0, 0U)
                                      == DSD_TRUNK_TUNE_RESULT_OK);
        rc |= expect_true("wav: the drop is asked for", c.state->input_drop_pending == 1U);
        dsd_symbol_apply_input_drop(c.opts, c.state); /* the decoder thread's next input read */
        rc |= expect_true("wav: the drop is carried out", c.state->input_drop_pending == 0U);
        rc |= expect_true("the file reads on where it was", sf_seek(c.opts->audio_in_file, 0, SEEK_CUR) == 10);
        sf_close(c.opts->audio_in_file);
        c.opts->audio_in_file = NULL;
    }
    rig_case_close(&c);
    (void)remove(path);
    return rc;
}

int
main(void) {
    /* Every tune waits for the fake peer's reply on the rigctl receive timeout, a real socket timer (1.5 s by default):
       at its longest, a host pause that keeps the peer's thread off the CPU cannot fail a tune. No case waits for it
       to run out: a peer that drops the connection is seen at once. */
    if (dsd_test_setenv("DSD_NEO_RIGCTL_RCVTIMEO", "60000", 1) != 0) {
        DSD_FPRINTF(stderr, "setenv failed\n");
        return 1;
    }
    if (dsd_socket_init() != 0) {
        DSD_FPRINTF(stderr, "socket init failed\n");
        return 1;
    }
    dsd_decode_clock_use_test(k_t0_ns);
    dsd_engine_set_decoder_thread(1);
    int rc = 0;
    rc |= test_trunk_tunes();
    rc |= test_off_thread_tune();
    rc |= test_legacy_cc_return();
    rc |= test_manual_return_io_leg();
    rc |= test_dmr_chain_after_return();
    rc |= test_p25p1_terminator_after_tune();
    rc |= test_dmr_follow_end_to_end();
    rc |= test_udp_backlog();
    rc |= test_watchdog_tune_drops_at_the_next_read();
    rc |= test_watchdog_beat_tick_drops_at_the_next_read();
    rc |= test_watchdog_post_drops_only_what_was_queued();
    rc |= test_watchdog_post_while_tcp_reconnects();
    rc |= test_watchdog_post_and_an_input_switch();
    rc |= test_drop_belongs_to_its_input();
    rc |= test_tcp_backlog();
    rc |= test_stdin_backlog();
    rc |= test_newest_cutoff_wins();
    rc |= test_new_audio_before_the_reply();
    rc |= test_old_audio_while_the_passband_is_set();
    rc |= test_staged_low_rate_pcm();
    rc |= test_staged_low_rate_pcm_from_a_file();
    rc |= test_staged_low_rate_pcm_late_confirmation();
    rc |= test_symbol_reader_checks_posts_only_with_rigctl();
    rc |= test_a_producer_that_cannot_start_ends_its_case();
    rc |= test_readers_carry_out_the_drop();
    rc |= test_wav_input_unchanged();
    rc |= test_an_unreadable_tune_reply_leaves_the_frequency_unknown();
    rc |= test_the_legacy_leg_resends_after_an_unknown_manual_tune();
    rc |= test_an_unknown_manual_tune_forgets_dmr_receiver();
    dsd_socket_cleanup();
    if (rc == 0) {
        DSD_FPRINTF(stderr, "ENGINE_RIGCTL_RETUNE: OK\n");
    }
    return rc;
}
