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
#include <dsd-neo/protocol/dmr/dmr.h>
#include <dsd-neo/protocol/dmr/dmr_trunk_sm.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/exitflag.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <errno.h>
#include <sndfile.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "commands_internal.h"
#include "dmr_vcrypt.h"
#include "edacs_internal.h"
#include "engine_hooks_install.h"
#include "p25p1_terminator_end.h"
#include "test_support.h"

#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/pcm_stream.h>
#if DSD_PLATFORM_WIN_NATIVE
#include <fcntl.h>
#include <io.h>
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#define CC_HZ  451012500L
#define VC1_HZ 452012500L
#define VC2_HZ 452037500L
#define VC3_HZ 452062500L

static const uint64_t k_t0_ns = 1000000ULL * 1000000000ULL;

static int
expect_true(const char* what, int cond) {
    if (!cond) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", what);
        return 1;
    }
    return 0;
}

/* ---- A rigctl peer on a loopback socket ---- */

/* What the peer does with a frequency request (F). */
enum { RIG_TAKES = 0, RIG_REFUSES = 1, RIG_DROPS = 2 };

typedef struct {
    dsd_socket_t listener;
    dsd_socket_t conn;
    dsd_thread_t thread;
    int started;
    atomic_int f_mode;
    atomic_int f_taken; /* F requests answered RPRT 0 */
} fake_rig;

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
    state->symbol_clock += 5U * DMR_VC_SUPERFRAME_SYMBOLS;
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

/* The next UDP sample, waiting up to a second for it (-32768 for none). */
static int16_t
udp_next(dsd_opts* opts) {
    int16_t s = 0;
    for (int tries = 0; tries < 50; tries++) {
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
    rc |= expect_true("late sender", dsd_thread_create(&th, late_sender_main, &ls) == 0);
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
    if (tx != DSD_INVALID_SOCKET) {
        (void)dsd_socket_close(tx);
    }
    udp_input_stop(opts);
    rig_case_close(&c);
    return rc;
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
    rc |= test_watchdog_post_and_an_input_switch();
    rc |= test_drop_belongs_to_its_input();
    rc |= test_tcp_backlog();
    rc |= test_stdin_backlog();
    rc |= test_readers_carry_out_the_drop();
    rc |= test_wav_input_unchanged();
    dsd_socket_cleanup();
    if (rc == 0) {
        DSD_FPRINTF(stderr, "ENGINE_RIGCTL_RETUNE: OK\n");
    }
    return rc;
}
