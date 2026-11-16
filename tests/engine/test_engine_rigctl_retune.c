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
 * a return, and a P25 Phase 1 terminator's place.
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
#include <dsd-neo/engine/frame_processing.h>
#include <dsd-neo/engine/trunk_tuning.h>
#include <dsd-neo/io/rigctl_client.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/platform.h>
#include <dsd-neo/platform/sockets.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/protocol/dmr/dmr.h>
#include <dsd-neo/protocol/dmr/dmr_trunk_sm.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "commands_internal.h"
#include "dmr_vcrypt.h"
#include "engine_hooks_install.h"
#include "p25p1_terminator_end.h"

#if DSD_PLATFORM_WIN_NATIVE
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
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

/* Whether the tune @p tune moves the symbol clock epoch, which it must when @p moves. */
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
    rig_case_close(&c);
    return rc;
}

/* The no-carrier pass's own control-channel return (the legacy rigctl leg) moves the epoch. */
static int
test_legacy_cc_return(void) {
    rig_case c;
    if (rig_case_open(&c, AUDIO_IN_WAV) != 0) {
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
    rc |= expect_epoch("the legacy return moves the epoch", state, e, 1);
    rig_case_close(&c);
    return rc;
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
    dsd_socket_cleanup();
    if (rc == 0) {
        DSD_FPRINTF(stderr, "ENGINE_RIGCTL_RETUNE: OK\n");
    }
    return rc;
}
