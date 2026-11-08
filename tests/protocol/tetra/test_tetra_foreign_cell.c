// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * A followed TETRA call returns to the control channel it left (discussion #748): the traffic carrier F that cell A
 * assigns can be a carrier of another cell B too, and what B sends there (its BSCH, its SYSINFO naming its own main
 * carrier) rewrites the session's control channel (state->trunk_cc_freq) and cell. After an assigned channel an MS
 * returns to the MCCH it used before the assignment (EN 300 392-2 clause 23.3.1.2.1.2), so the state machine keeps the
 * control channel the follow started from, its home, and every way back goes there (tetra_sm_return_cc_hz()).
 *
 * Signalling goes through the real BSCH parser (tetra_bsch_parse()) and MAC (tetra_mac_parse_schd()) the way tetra.c
 * decodes a burst, into the real call records and state machine; the tuning backend is a pair of stand-in hooks that
 * do to the shared tuning state what the engine's do. Time is the decode clock's TEST source.
 */

#include <assert.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/platform/sockets.h>
#include <dsd-neo/protocol/tetra/tetra_bsch.h>
#include <dsd-neo/protocol/tetra/tetra_mac.h>
#include <dsd-neo/protocol/tetra/tetra_mm.h>
#include <dsd-neo/protocol/tetra/tetra_trunk_sm.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "tetra_confirm.h"
#include "tetra_state.h"
#include "tetra_test_pdu.h"

#define SECOND_NS 1000000000ULL

/* Cell A (home) and cell B share network 262/1001; B has another colour code unless a case says otherwise. Carrier
 * numbers are band 4, offset 2 (tetra_carrier_to_dl_hz()). */
enum { MCC = 262, MNC = 1001, COL_A = 10, COL_B = 20, LA_A = 100, LA_B = 200 };

enum { CARR_A_CC = 50, CARR_F = 60, CARR_G = 70, CARR_H = 80 };

enum { CALL_A = 0x0BEE, CALL_B = 0x0CAF, GSSI_A = 0x00BEEF, GSSI_B = 0x00CAFE };

static int g_tunes;
static int g_returns;
static long g_tuned_hz;
static long g_returned_hz;
static uint64_t g_request;
static dsd_trunk_tune_result g_tune_result;
static dsd_trunk_tune_result g_return_result;

static long
hz(uint32_t carrier) {
    return tetra_carrier_to_dl_hz(carrier, 4U, 2U);
}

/* What the engine's tune hook does when it accepts a voice-channel tune (dsd_engine_tune_rtl()), in flight or done. */
static dsd_trunk_tune_result
tune_hook(dsd_opts* opts, dsd_state* state, long freq, int ted_sps, uint64_t request_id) {
    (void)ted_sps;
    g_tunes++;
    g_tuned_hz = freq;
    g_request = request_id;
    if (dsd_trunk_tune_result_is_ok(g_tune_result)) {
        opts->rtlsdr_center_freq = (uint32_t)freq;
        opts->trunk_is_tuned = 1;
        state->trunk_vc_freq[0] = state->trunk_vc_freq[1] = freq;
        state->p25_vc_freq[0] = state->p25_vc_freq[1] = freq;
        state->last_vc_sync_time = dsd_decode_time();
        state->last_vc_sync_time_m = dsd_decode_now_mono_s();
    }
    return g_tune_result;
}

/* And when it accepts a return (dsd_engine_return_to_cc_request()), which takes no target: the P25 alias when one is
 * set, else state->trunk_cc_freq (dsd_engine_resolve_cc_freq()). */
static dsd_trunk_tune_result
return_hook(dsd_opts* opts, dsd_state* state, uint64_t request_id) {
    g_returns++;
    g_request = request_id;
    g_returned_hz = state->p25_cc_freq != 0 ? state->p25_cc_freq : state->trunk_cc_freq;
    if (dsd_trunk_tune_result_is_ok(g_return_result)) {
        opts->rtlsdr_center_freq = (uint32_t)g_returned_hz;
        opts->trunk_is_tuned = 0;
        state->trunk_vc_freq[0] = state->trunk_vc_freq[1] = 0;
        state->p25_vc_freq[0] = state->p25_vc_freq[1] = 0;
    }
    return g_return_result;
}

static int
tuner_free_hook(const dsd_opts* opts, const dsd_state* state) {
    (void)opts;
    (void)state;
    return 1;
}

static int
same_time(double a, double b) {
    return fabs(a - b) < 1e-9;
}

static void
at(double seconds) {
    dsd_decode_clock_test_set_ns((uint64_t)(seconds * (double)SECOND_NS));
}

static void
reset_backend(void) {
    dsd_trunk_tuning_hooks hooks = {0};
    hooks.tune_to_freq_request = tune_hook;
    hooks.return_to_cc_request = return_hook;
    hooks.tuner_free_for_protocol = tuner_free_hook;
    dsd_trunk_tuning_hooks_set(hooks);
    dsd_trunk_tuning_requests_reset();
    g_tunes = 0;
    g_returns = 0;
    g_tuned_hz = 0;
    g_returned_hz = 0;
    g_request = 0U;
    g_tune_result = DSD_TRUNK_TUNE_RESULT_OK;
    g_return_result = DSD_TRUNK_TUNE_RESULT_OK;
    dsd_decode_clock_use_test(SECOND_NS * 1000ULL);
}

typedef struct {
    dsd_opts* opts;
    dsd_state* state;
    double t; /* the decode clock, seconds */
} session;

static void
advance(session* s, double seconds) {
    s->t += seconds;
    at(s->t);
}

/* One burst whose block is a MAC block, as tetra.c decodes it. */
static void
mac_burst(session* s, const ttb_block* b) {
    tetra_confirm_burst_begin(s->state);
    tetra_mac_parse_schd(b->bits, b->n, 0, s->opts, s->state);
    tetra_confirm_note_signalling(s->state);
    (void)tetra_confirm_burst_end(s->state);
    tetra_sm_after_burst(s->opts, s->state);
}

static void
bsch_burst(session* s, uint32_t colour) {
    ttb_block b = {0};
    ttb_sync_pdu(&b, MCC, MNC, colour);
    tetra_confirm_burst_begin(s->state);
    assert(tetra_bsch_parse(b.bits, b.n, s->opts, s->state) == 1);
    tetra_confirm_note_signalling(s->state);
    (void)tetra_confirm_burst_end(s->state);
    tetra_sm_after_burst(s->opts, s->state);
}

static void
sysinfo_burst(session* s, uint32_t main_carrier, uint32_t la) {
    ttb_block b = {0};
    ttb_sysinfo(&b, main_carrier, 4U, 2U, la);
    mac_burst(s, &b);
}

/* A D-SETUP to group @p gssi for @p call_id with a downlink allocation of timeslot bitmap @p timeslots on @p carrier. */
static void
setup_burst(session* s, uint32_t gssi, uint32_t call_id, uint32_t timeslots, uint32_t carrier) {
    ttb_block b = {0};
    const int start = ttb_resource(&b, 0, 1 /* SSI */, gssi);
    ttb_resource_flags(&b, 1);
    ttb_allocation(&b, timeslots, 1, carrier);
    ttb_put(&b, 2, 4); /* BL-UDATA */
    ttb_d_setup(&b, call_id, TTB_SPEECH_GROUP);
    ttb_close_resource(&b, start);
    ttb_finish(&b, TTB_SCH_HD_BITS, -1);
    mac_burst(s, &b);
}

/* A live RTL session with a 2 s hangtime, on A's control channel by A's BSCH and SYSINFO. */
static session
open_on_a(void) {
    session s;
    s.opts = (dsd_opts*)calloc(1U, sizeof(*s.opts));
    s.state = (dsd_state*)calloc(1U, sizeof(*s.state));
    assert(s.opts != NULL && s.state != NULL);
    s.t = 1000.0;
    at(s.t);
    s.opts->trunk_enable = 1;
    s.opts->trunk_hangtime = 2.0F;
    s.opts->frame_tetra = 1;
    s.opts->audio_in_type = AUDIO_IN_RTL;
    s.opts->rtlsdr_center_freq = (uint32_t)hz(CARR_A_CC);
    s.opts->rigctl_sockfd = DSD_INVALID_SOCKET;
    s.opts->trunk_tune_group_calls = 1;
    s.opts->trunk_tune_private_calls = 1;
    s.opts->trunk_tune_data_calls = 1;
    s.opts->trunk_tune_enc_calls = 1;
    s.state->samplesPerSymbol = 10;
    assert(tetra_st(s.state) != NULL);
    tetra_sm_init(s.opts, s.state);
    bsch_burst(&s, COL_A);
    sysinfo_burst(&s, CARR_A_CC, LA_A);
    assert(tetra_sm_get_state(s.state) == TETRA_SM_ON_CC && s.state->trunk_cc_freq == hz(CARR_A_CC));
    assert(tetra_sm_return_cc_hz(s.state) == 0 && !tetra_sm_guarding(s.opts, s.state));
    return s;
}

static void
close_session(session* s) {
    dsd_state_ext_free_all(s->state);
    free(s->state);
    free(s->opts);
}

/* A's speech on the followed timeslot 2. */
static void
speech(session* s) {
    tetra_confirm_note_signalling(s->state);
    tetra_sm_note_voice(s->state, 2U);
}

/* A's group call granted to timeslot 2 of @p carrier and followed there. */
static void
follow_a(session* s, uint32_t carrier) {
    setup_burst(s, GSSI_A, CALL_A, 0x4U, carrier);
    tetra_sm_grant g = {0};
    assert(tetra_sm_followed(s->state, &g) && g.freq_hz == hz(carrier) && g.call_id == CALL_A);
    advance(s, 0.1);
    speech(s);
    tetra_sm_tick(s->opts, s->state);
    assert(tetra_sm_get_state(s->state) == TETRA_SM_TUNED);
}

/* The follow holds A's control channel as its home, however the session's control channel reads now. */
static void
assert_home_is_a(const session* s) {
    const tetra_state_t* const ts = tetra_st_peek(s->state);
    assert(ts->sm.home.valid && ts->sm.home.cc_hz == hz(CARR_A_CC));
    assert(ts->sm.home.mcc == MCC && ts->sm.home.mnc == MNC && ts->sm.home.colour == COL_A && ts->sm.home.net_known);
    assert(ts->sm.home.la == LA_A && ts->sm.home.la_known);
    assert(tetra_sm_return_cc_hz(s->state) == hz(CARR_A_CC));
    assert(tetra_sm_guarding(s->opts, s->state) == (s->opts->trunk_enable == 1));
}

/* Back on A's control channel after one return there, resting ON_CC on it, the follow and its home gone. */
static void
assert_back_home(const session* s, int returns) {
    assert(g_returns == returns && g_returned_hz == hz(CARR_A_CC));
    assert((long)s->opts->rtlsdr_center_freq == hz(CARR_A_CC) && s->opts->trunk_is_tuned == 0);
    assert(tetra_sm_get_state(s->state) == TETRA_SM_ON_CC && !tetra_sm_followed(s->state, NULL));
    assert(s->state->trunk_cc_freq == hz(CARR_A_CC));
    assert(tetra_sm_return_cc_hz(s->state) == 0 && !tetra_sm_guarding(s->opts, s->state));
    assert(!tetra_st_peek(s->state)->sm.home.valid && !tetra_st_peek(s->state)->sm.return_required);
}

/* What is decoded on F after A's call landed there. */
typedef struct {
    const char* name;
    uint32_t colour; /* B's colour code */
    uint32_t main_carrier;
    uint32_t la;
    int sysinfo;     /* B's SYSINFO decoded, not only its BSCH */
    int a_bsch_then; /* A's BSCH decoded again after B */
} foreign_case;

static const foreign_case k_cases[] = {
    {"(a) other colour, other LA, B's SYSINFO names F", COL_B, CARR_F, LA_B, 1, 0},
    {"(a2) other colour, same LA", COL_B, CARR_F, LA_A, 1, 0},
    {"(a3) other colour, BSCH only", COL_B, CARR_F, LA_B, 0, 0},
    {"(b) same colour, other LA", COL_A, CARR_F, LA_B, 1, 0},
    {"(b2) same colour, same LA", COL_A, CARR_F, LA_A, 1, 0},
    {"(e) other colour, F is B's secondary carrier (SYSINFO names H)", COL_B, CARR_H, LA_B, 1, 0},
    {"(f) B heard, then A's BSCH again", COL_B, CARR_F, LA_B, 1, 1},
    {"(d, control) A's own BSCH and SYSINFO on F", COL_A, CARR_A_CC, LA_A, 1, 0},
};

static void
hear_on_f(session* s, const foreign_case* c) {
    advance(s, 0.2);
    bsch_burst(s, c->colour);
    if (c->sysinfo) {
        sysinfo_burst(s, c->main_carrier, c->la);
    }
    if (c->a_bsch_then) {
        bsch_burst(s, COL_A);
    }
}

enum { END_HANGTIME, END_RELEASE, END_GRANT_TIMEOUT };

/* How the follow ends: the hangtime after speech, A's release of the call, or the grant timeout of a call that never
 * spoke. */
static void
end_follow(session* s, int how) {
    if (how == END_HANGTIME) {
        advance(s, 3.0);
        tetra_sm_tick(s->opts, s->state);
    } else if (how == END_RELEASE) {
        tetra_sm_on_release(s->opts, s->state, CALL_A);
    } else {
        advance(s, TETRA_SM_GRANT_TIMEOUT_S + 0.5);
        tetra_sm_tick(s->opts, s->state);
    }
}

/* Whatever cell B decoded on A's traffic carrier F did to the session's control channel -- cleared it (B's BSCH),
 * named F (B's SYSINFO there), named B's own main carrier H -- the hangtime, a release and the grant timeout each
 * return once to A's control channel and rest ON_CC there (regression: main stayed on F as B's control channel,
 * stranded IDLE with none, or returned to H). Back there, A's BSCH and SYSINFO are taken on and A's next grant is
 * followed with A's control channel as its home again. */
static void
test_every_return_goes_home(void) {
    for (size_t k = 0; k < sizeof k_cases / sizeof k_cases[0]; k++) {
        for (int how = END_HANGTIME; how <= END_GRANT_TIMEOUT; how++) {
            printf("foreign cell %s, end %d\n", k_cases[k].name, how);
            reset_backend();
            session s = open_on_a();
            if (how == END_GRANT_TIMEOUT) {
                setup_burst(&s, GSSI_A, CALL_A, 0x4U, CARR_F);
            } else {
                follow_a(&s, CARR_F);
            }
            assert_home_is_a(&s);
            hear_on_f(&s, &k_cases[k]);
            /* What the session reads as its control channel now may be B's; the return target is not. */
            assert(tetra_sm_get_state(s.state) == TETRA_SM_TUNED);
            assert(tetra_sm_return_cc_hz(s.state) == hz(CARR_A_CC) && tetra_sm_guarding(s.opts, s.state));
            end_follow(&s, how);
            assert_back_home(&s, 1);

            bsch_burst(&s, COL_A);
            sysinfo_burst(&s, CARR_A_CC, LA_A);
            assert(tetra_sm_get_state(s.state) == TETRA_SM_ON_CC && s.state->trunk_cc_freq == hz(CARR_A_CC));
            advance(&s, 1.0);
            setup_burst(&s, GSSI_A, CALL_A + 1U, 0x4U, CARR_G);
            assert(g_tuned_hz == hz(CARR_G) && tetra_sm_get_state(s.state) == TETRA_SM_TUNED);
            assert(tetra_sm_return_cc_hz(s.state) == hz(CARR_A_CC));
            close_session(&s);
        }
    }
}

/* A reallocation of the followed call (TUNED to TUNED) keeps the home it started with, even once B's SYSINFO named
 * another control channel; the return from the reallocated channel still goes to A. */
static void
test_reallocation_keeps_the_home(void) {
    reset_backend();
    session s = open_on_a();
    follow_a(&s, CARR_F);
    sysinfo_burst(&s, CARR_F, LA_A); /* B, same colour and LA: the control channel now reads F */
    assert(s.state->trunk_cc_freq == hz(CARR_F));
    const tetra_sm_grant moved = {hz(CARR_G), CARR_G, 0x4U, CALL_A};
    assert(tetra_sm_on_grant(s.opts, s.state, &moved) == 1 && g_tuned_hz == hz(CARR_G));
    assert_home_is_a(&s);
    /* And one on the carrier the receiver is on now. */
    tetra_confirm_note_signalling(s.state);
    const tetra_sm_grant here = {hz(CARR_G), CARR_G, 0x2U, CALL_A + 1U};
    assert(tetra_sm_on_grant(s.opts, s.state, &here) == 1 && g_tunes == 2);
    assert_home_is_a(&s);
    advance(&s, TETRA_SM_GRANT_TIMEOUT_S + 0.5);
    tetra_sm_tick(s.opts, s.state);
    assert_back_home(&s, 1);
    close_session(&s);
}

/* A grant from the control channel the backend takes in flight: the home is the follow's from the request on. A
 * completion keeps it; a failure puts the state machine back on the control channel with no home, as a refused or
 * deferred tune leaves it, having taken none. A reallocation in flight that fails keeps the home of the follow it
 * leaves in place. */
static void
test_pending_grant(void) {
    for (int fails = 0; fails <= 1; fails++) {
        reset_backend();
        session s = open_on_a();
        g_tune_result = DSD_TRUNK_TUNE_RESULT_PENDING;
        setup_burst(&s, GSSI_A, CALL_A, 0x4U, CARR_F);
        assert(g_tunes == 1 && g_request != 0U);
        assert(tetra_sm_return_cc_hz(s.state) == hz(CARR_A_CC) && tetra_sm_guarding(s.opts, s.state));
        dsd_trunk_tuning_request_publish(g_request, fails ? DSD_TRUNK_TUNE_RESULT_FAILED : DSD_TRUNK_TUNE_RESULT_OK);
        tetra_sm_tick(s.opts, s.state);
        if (fails) {
            assert(tetra_sm_get_state(s.state) == TETRA_SM_ON_CC && !tetra_st_peek(s.state)->sm.home.valid);
            assert(tetra_sm_return_cc_hz(s.state) == 0 && !tetra_sm_guarding(s.opts, s.state));
        } else {
            assert_home_is_a(&s);
        }
        close_session(&s);
    }

    const dsd_trunk_tune_result unaccepted[] = {DSD_TRUNK_TUNE_RESULT_FAILED, DSD_TRUNK_TUNE_RESULT_DEFERRED};
    for (size_t k = 0; k < 2U; k++) {
        reset_backend();
        session s = open_on_a();
        g_tune_result = unaccepted[k];
        setup_burst(&s, GSSI_A, CALL_A, 0x4U, CARR_F);
        assert(g_tunes == 1 && tetra_sm_get_state(s.state) == TETRA_SM_ON_CC);
        assert(!tetra_st_peek(s.state)->sm.home.valid && tetra_sm_return_cc_hz(s.state) == 0);
        close_session(&s);
    }

    reset_backend();
    session s = open_on_a();
    follow_a(&s, CARR_F);
    sysinfo_burst(&s, CARR_F, LA_A);
    g_tune_result = DSD_TRUNK_TUNE_RESULT_PENDING;
    const tetra_sm_grant moved = {hz(CARR_G), CARR_G, 0x4U, CALL_A};
    assert(tetra_sm_on_grant(s.opts, s.state, &moved) == 1);
    assert_home_is_a(&s);
    dsd_trunk_tuning_request_publish(g_request, DSD_TRUNK_TUNE_RESULT_FAILED);
    tetra_sm_tick(s.opts, s.state);
    tetra_sm_grant g = {0};
    assert(tetra_sm_followed(s.state, &g) && g.freq_hz == hz(CARR_F));
    assert_home_is_a(&s);
    close_session(&s);
}

/* Following A on F with B's SYSINFO having named F as the control channel, the hangtime passed. */
static session
open_released_on_f(void) {
    reset_backend();
    session s = open_on_a();
    follow_a(&s, CARR_F);
    advance(&s, 0.2);
    bsch_burst(&s, COL_B);
    sysinfo_burst(&s, CARR_F, LA_B);
    assert(s.state->trunk_cc_freq == hz(CARR_F));
    advance(&s, 3.0);
    return s;
}

/* The return asked for at the hangtime goes home and the backend takes it in flight: the state machine still holds the
 * follow (its home and the session's control channel are A's) until the backend answers. A completion rests ON_CC on
 * A. A failure puts the follow back, its home and the session's control channel as they were (B's F) -- unless
 * something changed that since, which stays -- and the next tick returns home again. */
static void
test_pending_return(void) {
    for (int answer = 0; answer < 3; answer++) {
        session s = open_released_on_f();
        g_return_result = DSD_TRUNK_TUNE_RESULT_PENDING;
        tetra_sm_tick(s.opts, s.state);
        assert(g_returns == 1 && g_returned_hz == hz(CARR_A_CC) && s.state->trunk_cc_freq == hz(CARR_A_CC));
        assert(tetra_sm_owns_return(s.state) && !tetra_sm_followed(s.state, NULL));
        assert(tetra_sm_return_cc_hz(s.state) == hz(CARR_A_CC) && tetra_sm_guarding(s.opts, s.state));
        g_return_result = DSD_TRUNK_TUNE_RESULT_OK;
        if (answer == 0) {
            dsd_trunk_tuning_request_publish(g_request, DSD_TRUNK_TUNE_RESULT_OK);
            tetra_sm_tick(s.opts, s.state);
            assert_back_home(&s, 1);
            close_session(&s);
            continue;
        }
        const long newer_hz = hz(CARR_H);
        if (answer == 2) {
            s.state->trunk_cc_freq = newer_hz; /* another owner's since the request */
        }
        dsd_trunk_tuning_request_publish(g_request, DSD_TRUNK_TUNE_RESULT_FAILED);
        assert(tetra_sm_settle(s.opts, s.state) == 1);
        assert(tetra_sm_get_state(s.state) == TETRA_SM_TUNED && tetra_sm_followed(s.state, NULL));
        assert(s.state->trunk_cc_freq == (answer == 2 ? newer_hz : hz(CARR_F)));
        assert_home_is_a(&s);
        tetra_sm_tick(s.opts, s.state);
        assert_back_home(&s, 2);
        close_session(&s);
    }
}

/* A return the backend refuses or defers keeps the call and the home, and puts the session's control channel back at
 * once; the next tick returns home. */
static void
test_refused_and_deferred_return(void) {
    const dsd_trunk_tune_result unaccepted[] = {DSD_TRUNK_TUNE_RESULT_FAILED, DSD_TRUNK_TUNE_RESULT_DEFERRED};
    for (size_t k = 0; k < 2U; k++) {
        session s = open_released_on_f();
        g_return_result = unaccepted[k];
        tetra_sm_tick(s.opts, s.state);
        assert(g_returns == 1 && g_returned_hz == hz(CARR_A_CC));
        assert(tetra_sm_get_state(s.state) == TETRA_SM_TUNED && s.state->trunk_cc_freq == hz(CARR_F));
        assert_home_is_a(&s);
        g_return_result = DSD_TRUNK_TUNE_RESULT_OK;
        tetra_sm_tick(s.opts, s.state);
        assert_back_home(&s, 2);
        close_session(&s);
    }
}

/* The engine or a frontend took the receiver (an external return, a carrier boundary) or the state machine started
 * over (engine start, a decode-mode change that drops the TETRA state): the home goes with the follow. A grant or
 * return of its own in flight is let go of, and its late answer puts nothing back: no follow, no control channel. */
static void
test_letting_go_drops_the_home(void) {
    enum { BY_EXTERNAL, BY_ABANDON, BY_INIT, BY_DROP };

    for (int by = BY_EXTERNAL; by <= BY_DROP; by++) {
        for (int pending = 0; pending < 3; pending++) {
            reset_backend();
            session s = open_on_a();
            if (pending == 1) {
                g_tune_result = DSD_TRUNK_TUNE_RESULT_PENDING;
                setup_burst(&s, GSSI_A, CALL_A, 0x4U, CARR_F);
            } else {
                follow_a(&s, CARR_F);
                sysinfo_burst(&s, CARR_F, LA_A);
                if (pending == 2) {
                    g_return_result = DSD_TRUNK_TUNE_RESULT_PENDING;
                    advance(&s, 3.0);
                    tetra_sm_tick(s.opts, s.state);
                    assert(g_returns == 1);
                }
            }
            const uint64_t request = g_request;
            assert(tetra_sm_return_cc_hz(s.state) == hz(CARR_A_CC));
            if (by == BY_EXTERNAL) {
                tetra_sm_on_external_cc_return(s.opts, s.state);
            } else if (by == BY_ABANDON) {
                tetra_sm_abandon_carrier(s.opts, s.state);
            } else if (by == BY_INIT) {
                tetra_sm_init(s.opts, s.state);
            } else {
                tetra_state_reset(s.state);
            }
            assert(tetra_sm_return_cc_hz(s.state) == 0 && !tetra_sm_guarding(s.opts, s.state));
            assert(!tetra_st_peek(s.state)->sm.home.valid);
            if (pending != 0) {
                const long operator_hz = hz(CARR_H);
                s.state->trunk_cc_freq = operator_hz;
                dsd_trunk_tuning_request_publish(request, DSD_TRUNK_TUNE_RESULT_FAILED);
                tetra_sm_tick(s.opts, s.state);
                assert(s.state->trunk_cc_freq == operator_hz && !tetra_sm_followed(s.state, NULL));
                assert(tetra_sm_return_cc_hz(s.state) == 0);
            }
            close_session(&s);
        }
    }
}

/* What is taken on while trunking is off and the receiver stays on the followed channel. */
enum { OFF_HEARS_B, OFF_HEARS_OTHER_LA };

static void
hear_while_untrunked(session* s, int what) {
    advance(s, 0.2);
    if (what == OFF_HEARS_B) {
        bsch_burst(s, COL_B);
        sysinfo_burst(s, CARR_F, LA_B);
        assert(s->state->trunk_cc_freq == hz(CARR_F));
    } else {
        sysinfo_burst(s, CARR_A_CC, LA_B);
    }
}

/* Following A on F, trunking turned off (the machine keeps the channel and lets go of the call), something taken on
 * there, A's speech still going on. */
static session
open_untrunked_on_f(int what) {
    reset_backend();
    session s = open_on_a();
    follow_a(&s, CARR_F);
    s.opts->trunk_enable = 0;
    tetra_sm_tick(s.opts, s.state);
    assert(tetra_sm_get_state(s.state) == TETRA_SM_TUNED && !tetra_sm_guarding(s.opts, s.state));
    assert(tetra_sm_return_cc_hz(s.state) == hz(CARR_A_CC));
    hear_while_untrunked(&s, what);
    advance(&s, 0.1);
    speech(&s);
    return s;
}

/* Trunking on again while TUNED ends the follow at once, speech or not: the call was let go when trunking went off and
 * what the carrier delivered since (another cell, another location area) cannot be trusted. The session's control
 * channel is A's again as soon as trunking is seen on -- by the tick, or by the trunk command before it -- and the
 * return goes home (regression: main kept the channel until the hangtime and then stayed on F as B's control
 * channel). */
static void
test_trunking_back_on_returns_home(void) {
    for (int what = OFF_HEARS_B; what <= OFF_HEARS_OTHER_LA; what++) {
        for (int by_command = 0; by_command <= 1; by_command++) {
            session s = open_untrunked_on_f(what);
            if (by_command) {
                tetra_sm_on_trunking_on(s.state);
                assert(s.state->trunk_cc_freq == hz(CARR_A_CC) && tetra_st_peek(s.state)->sm.return_required);
            }
            s.opts->trunk_enable = 1;
            assert(tetra_sm_guarding(s.opts, s.state));
            tetra_sm_tick(s.opts, s.state);
            assert_back_home(&s, 1);
            close_session(&s);
        }
    }
}

/* Until that return completes nothing on the carrier keeps the follow: a grant is ignored, speech and channel
 * maintenance start no timer. A return refused or deferred is asked for again at the next tick, and so is one that
 * failed in flight, whose rollback puts the follow back still owing its return. */
static void
test_return_required_until_home(void) {
    const dsd_trunk_tune_result answers[] = {
        DSD_TRUNK_TUNE_RESULT_FAILED,
        DSD_TRUNK_TUNE_RESULT_DEFERRED,
        DSD_TRUNK_TUNE_RESULT_PENDING,
    };
    for (size_t k = 0; k < 3U; k++) {
        session s = open_untrunked_on_f(OFF_HEARS_B);
        s.opts->trunk_enable = 1;
        g_return_result = answers[k];
        tetra_sm_tick(s.opts, s.state);
        assert(g_returns == 1 && g_returned_hz == hz(CARR_A_CC));
        if (answers[k] == DSD_TRUNK_TUNE_RESULT_PENDING) {
            dsd_trunk_tuning_request_publish(g_request, DSD_TRUNK_TUNE_RESULT_FAILED);
            assert(tetra_sm_settle(s.opts, s.state) == 1);
        }
        const tetra_state_t* const ts = tetra_st_peek(s.state);
        assert(tetra_sm_get_state(s.state) == TETRA_SM_TUNED && ts->sm.return_required);
        assert(tetra_sm_return_cc_hz(s.state) == hz(CARR_A_CC));

        /* Nothing on the carrier counts for the follow meanwhile. */
        const double voice_m = ts->sm.t_voice_m;
        const double maint_m = ts->sm.t_maint_m;
        advance(&s, 0.1);
        speech(&s);
        tetra_sm_note_maintenance(s.state);
        assert(same_time(ts->sm.t_voice_m, voice_m) && same_time(ts->sm.t_maint_m, maint_m));
        tetra_confirm_note_signalling(s.state);
        const int tunes = g_tunes;
        const tetra_sm_grant other = {hz(CARR_G), CARR_G, 0x4U, CALL_B};
        assert(tetra_sm_on_grant(s.opts, s.state, &other) == 0 && g_tunes == tunes);

        g_return_result = DSD_TRUNK_TUNE_RESULT_OK;
        tetra_sm_tick(s.opts, s.state);
        assert_back_home(&s, 2);
        close_session(&s);
    }
}

/* A follow on a traffic timeslot of A's control channel's own carrier stops there without a tune once trunking is on
 * again, and the carrier's cell is taken on again from scratch: what was decoded there while untrunked (B's SYSINFO,
 * its timebase and the carrier's evidence) goes, the session's control channel is A's, and A's next SYNC and SYSINFO
 * are taken on as on any control channel. */
static void
test_trunking_back_on_on_the_home_carrier(void) {
    reset_backend();
    session s = open_on_a();
    follow_a(&s, CARR_A_CC);
    assert(g_tunes == 0 && tetra_sm_return_cc_hz(s.state) == hz(CARR_A_CC));
    s.opts->trunk_enable = 0;
    tetra_sm_tick(s.opts, s.state);
    advance(&s, 0.2);
    bsch_burst(&s, COL_B);
    sysinfo_burst(&s, CARR_F, LA_B);
    assert(s.state->trunk_cc_freq == hz(CARR_F) && tetra_st_peek(s.state)->dl_carrier_hz == hz(CARR_F));

    s.opts->trunk_enable = 1;
    tetra_sm_tick(s.opts, s.state);
    const tetra_state_t* const ts = tetra_st_peek(s.state);
    assert(g_returns == 0 && g_tunes == 0 && s.opts->trunk_is_tuned == 0);
    assert(tetra_sm_get_state(s.state) == TETRA_SM_ON_CC && !tetra_sm_followed(s.state, NULL));
    assert(s.state->trunk_cc_freq == hz(CARR_A_CC) && tetra_sm_return_cc_hz(s.state) == 0);
    assert(!ts->sm.home.valid && !ts->sm.return_required);
    assert(ts->dl_carrier_hz == 0 && !ts->sysinfo_known && ts->tdma_valid == 0);
    assert(!tetra_confirm_carrier_proven(s.state));

    bsch_burst(&s, COL_A);
    sysinfo_burst(&s, CARR_A_CC, LA_A);
    assert(tetra_sm_get_state(s.state) == TETRA_SM_ON_CC && ts->dl_carrier_hz == hz(CARR_A_CC) && ts->la == LA_A);
    close_session(&s);
}

int
main(void) {
    test_every_return_goes_home();
    test_reallocation_keeps_the_home();
    test_pending_grant();
    test_pending_return();
    test_refused_and_deferred_return();
    test_letting_go_drops_the_home();
    test_trunking_back_on_returns_home();
    test_return_required_until_home();
    test_trunking_back_on_on_the_home_carrier();
    printf("TETRA foreign cell: OK\n");
    return 0;
}
