// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * The gates a TETRA speech burst passes (tetra_voice_gate.h): trunking on, the followed call's timeslots (by the tracked
 * TDMA timeslot, TN1 in bit 3 of the allocation's bitmap) on its settled carrier once its signalling proved it there;
 * not trunking, one heard timeslot at a time; direct mode always. Then the call each burst belongs to, and its
 * encryption class. Runs on the decode clock's TEST source.
 */
#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/protocol/tetra/tetra_dmo.h>
#include <dsd-neo/protocol/tetra/tetra_mac.h>
#include <dsd-neo/protocol/tetra/tetra_trunk_sm.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "tetra_calls.h"
#include "tetra_confirm.h"
#include "tetra_state.h"
#include "tetra_voice_gate.h"

#define SECOND_NS 1000000000ULL

static int g_fail = 0;

static void
expect(int ok, const char* what) {
    if (!ok) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", what);
        g_fail++;
    }
}

static void
clock_at(double seconds) {
    dsd_decode_clock_test_set_ns((uint64_t)(seconds * (double)SECOND_NS));
}

typedef struct {
    dsd_opts* opts;
    dsd_state* state;
} fixture;

static void
fixture_open(fixture* f) {
    f->opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    f->state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!f->opts || !f->state || !tetra_st(f->state)) {
        DSD_FPRINTF(stderr, "out of memory\n");
        exit(1);
    }
    clock_at(1000.0);
}

static void
fixture_close(fixture* f) {
    dsd_state_ext_free_all(f->state);
    free(f->state);
    free(f->opts);
}

/* A record of call @p call_id: a D-SETUP of a group call (table 14.35 communication type 01) to @p gssi, on timeslots
 * @p timeslots of the carrier at @p freq_hz. */
static void
group_call(fixture* f, uint16_t call_id, uint32_t gssi, long freq_hz, uint8_t timeslots) {
    tetra_call_event setup = {.call_id = call_id, .kind = TETRA_CALL_EV_SETUP, .basic_service = 0x04};
    tetra_calls_on_event(NULL, f->state, &setup, NULL);
    tetra_call_t* c = tetra_calls_find(f->state, call_id);
    c->called_ssi = gssi;
    c->alloc.freq_hz = freq_hz;
    c->alloc.timeslots = timeslots;
    c->alloc.up_down = 3;
    c->alloc_m = dsd_decode_now_mono_s();
}

/* The followed grant and the record of the call it came from, both on @p timeslots, as a grant is made from the
 * record's allocation (tetra_calls.c). */
static void
followed_timeslots(fixture* f, uint8_t timeslots) {
    tetra_state_t* ts = tetra_st(f->state);
    ts->sm.followed.timeslots = timeslots;
    tetra_call_t* c = tetra_calls_find(f->state, ts->sm.followed.call_id);
    if (c) {
        c->alloc.timeslots = timeslots;
    }
}

/* The trunking state machine follows call @p call_id on @p timeslots of its record's carrier, its tune settled. */
static void
follow(fixture* f, uint16_t call_id, uint8_t timeslots) {
    tetra_state_t* ts = tetra_st(f->state);
    f->opts->trunk_enable = 1;
    ts->sm.state = (uint8_t)TETRA_SM_TUNED;
    ts->sm.pending_request_id = 0;
    ts->sm.followed.call_id = call_id;
    const tetra_call_t* c = tetra_calls_peek(f->state, call_id);
    ts->sm.followed.freq_hz = c ? c->alloc.freq_hz : 0L;
    ts->sm.followed.carrier = c ? c->alloc.carrier : 0U;
    followed_timeslots(f, timeslots);
}

static void
at_timeslot(fixture* f, uint8_t tn) {
    tetra_state_t* ts = tetra_st(f->state);
    ts->tdma_valid = 1;
    ts->tn = tn;
}

static void
test_trunked_gate(void) {
    fixture f;
    fixture_open(&f);
    tetra_state_t* ts = tetra_st(f.state);
    tetra_speech sp;
    group_call(&f, 9, 0x123456u, 392012500L, 0x08);
    at_timeslot(&f, 1);

    f.opts->trunk_enable = 1;
    expect(!tetra_voice_gate(f.opts, f.state, &sp), "trunked: nothing is heard before the state machine follows");
    ts->sm.state = (uint8_t)TETRA_SM_ON_CC;
    expect(!tetra_voice_gate(f.opts, f.state, &sp), "trunked: nothing is heard on the control channel");

    follow(&f, 9, 0x08);
    expect(!tetra_voice_gate(f.opts, f.state, &sp), "trunked: nothing before signalling proved the call here");
    tetra_confirm_note_followed_call(f.state);
    expect(tetra_voice_gate(f.opts, f.state, &sp) && sp.followed && sp.kind == TETRA_SPEECH_CALL && sp.call_id == 9
               && sp.target == 0x123456u && sp.tn == 1,
           "trunked: the proven followed call is heard, judged by its GSSI");
    ts->sm.pending_request_id = 7;
    expect(!tetra_voice_gate(f.opts, f.state, &sp) && sp.kind == 0 && sp.target == 0u,
           "trunked: nothing while a retune is in flight, and nothing is described");
    ts->sm.pending_request_id = 0;

    for (unsigned tn = 1; tn <= 4; ++tn) {
        at_timeslot(&f, (uint8_t)tn);
        for (unsigned assigned = 1; assigned <= 4; ++assigned) {
            followed_timeslots(&f, (uint8_t)(1u << (4u - assigned)));
            char label[64];
            DSD_SNPRINTF(label, sizeof(label), "trunked: TN%u under an allocation of TN%u", tn, assigned);
            expect(tetra_voice_gate(f.opts, f.state, &sp) == (tn == assigned), label);
        }
    }
    /* After a retune the TDMA clock waits for the carrier's next BSCH, while addressed signalling can prove the call
       there sooner: until the clock places the burst it may be any timeslot's, another call's on another timeslot
       included, so nothing plays, whatever the allocation. */
    ts->tdma_valid = 0;
    for (unsigned slots = 0x01; slots <= 0x0F; slots++) {
        followed_timeslots(&f, (uint8_t)slots);
        char label[80];
        DSD_SNPRINTF(label, sizeof(label), "trunked: a burst the clock cannot place is not heard (timeslots 0x%X)",
                     slots);
        expect(!tetra_voice_gate(f.opts, f.state, &sp) && sp.tn == 0 && sp.kind == 0, label);
    }
    at_timeslot(&f, 4);
    expect(tetra_voice_gate(f.opts, f.state, &sp) && sp.followed && sp.tn == 4,
           "trunked: once the clock places the burst on a followed timeslot it is heard");
    ts->tn = 0;
    expect(!tetra_voice_gate(f.opts, f.state, &sp), "trunked: a clock outside TN1..TN4 places nothing");
    ts->sm.followed.timeslots = 0;
    at_timeslot(&f, 1);
    expect(!tetra_voice_gate(f.opts, f.state, &sp), "trunked: a followed call with no timeslot is not heard");

    /* An allocation that moved the call away without moving the receiver (one no call PDU took, which never tunes, or
       a reallocation whose tune failed) leaves the followed channel to whatever call comes there next; a record that
       holds no allocation says nothing. */
    followed_timeslots(&f, 0x08);
    expect(tetra_voice_gate(f.opts, f.state, &sp) && sp.followed, "trunked: the followed channel is heard");
    tetra_call_t* const c = tetra_calls_find(f.state, 9);
    c->alloc.carrier = 130;
    c->alloc.freq_hz = 392037500L;
    expect(!tetra_voice_gate(f.opts, f.state, &sp), "trunked: not once its record moved to another carrier");
    c->alloc.carrier = 0;
    c->alloc.freq_hz = 392012500L;
    c->alloc.timeslots = 0x04;
    expect(!tetra_voice_gate(f.opts, f.state, &sp), "trunked: nor once it moved to another timeslot");
    DSD_MEMSET(&c->alloc, 0, sizeof c->alloc);
    expect(tetra_voice_gate(f.opts, f.state, &sp) && sp.followed, "trunked: a record without allocation says nothing");

    /* A followed call no record names is heard without a talkgroup. */
    ts->sm.followed.timeslots = 0x08;
    ts->sm.followed.call_id = 77;
    expect(tetra_voice_gate(f.opts, f.state, &sp) && sp.followed && sp.kind == TETRA_SPEECH_ANON && sp.target == 0u,
           "trunked: followed speech without a record is anonymous");
    expect(!tetra_voice_gate(f.opts, NULL, &sp) && !tetra_voice_gate(f.opts, f.state, NULL), "trunked: no state");
    fixture_close(&f);
}

/* Direct mode speech plays while trunking is enabled: there is no trunked call to follow on a direct mode channel. */
static void
test_direct_mode(void) {
    fixture f;
    fixture_open(&f);
    tetra_state_t* ts = tetra_st(f.state);
    tetra_speech sp;
    f.opts->trunk_enable = 1;
    ts->dmo = 1;
    ts->dmo_crypt = TETRA_DMO_CRYPT_CLEAR;
    at_timeslot(&f, 1);
    expect(tetra_voice_gate(f.opts, f.state, &sp) && sp.kind == TETRA_SPEECH_DMO && !sp.followed && sp.target == 0u
               && !sp.sealed,
           "direct: heard with trunking on, no call yet");
    ts->calls.dmo.active = 1;
    ts->calls.dmo.dst = 0x800001u;
    expect(tetra_voice_gate(f.opts, f.state, &sp) && sp.target == 0x800001u,
           "direct: judged by the destination of the call on the channel");
    ts->calls.dmo.active = 0;
    expect(tetra_voice_gate(f.opts, f.state, &sp) && sp.target == 0u, "direct: an ended call is no destination");
    ts->dmo_crypt = TETRA_DMO_CRYPT_AIR;
    expect(tetra_voice_gate(f.opts, f.state, &sp) && sp.sealed, "direct: air encryption is sealed");
    ts->dmo_crypt = TETRA_DMO_CRYPT_E2E;
    expect(tetra_voice_gate(f.opts, f.state, &sp) && sp.sealed, "direct: end-to-end encryption is sealed");
    ts->dmo_crypt = TETRA_DMO_CRYPT_UNKNOWN;
    expect(tetra_voice_gate(f.opts, f.state, &sp) && sp.sealed, "direct: an unknown security class is sealed");
    fixture_close(&f);
}

/* A burst through the gates: tetra_voice_gate(), then, @p audible saying whether it plays, the timeslot the receiver
 * hears (tetra_voice_hold()). 1 when it is heard; then, when @p speech, its CRC-passed speech reaches the call
 * (tetra_calls_on_speech()). */
static int
heard_burst(fixture* f, tetra_speech* sp, int audible, int speech) {
    if (!tetra_voice_gate(f->opts, f->state, sp)) {
        return 0;
    }
    sp->audible = (uint8_t)audible;
    if (!tetra_voice_hold(f->opts, f->state, sp)) {
        return 0;
    }
    if (speech) {
        tetra_calls_on_speech(f->opts, f->state, sp);
    }
    return 1;
}

/* Not trunking, the first timeslot with speech that plays is heard until its speech stops for
 * TETRA_CALL_SPEECH_END_S. */
static void
test_heard_timeslot(void) {
    fixture f;
    fixture_open(&f);
    tetra_speech sp;
    at_timeslot(&f, 2);
    expect(heard_burst(&f, &sp, 1, 1) && !sp.followed && sp.tn == 2, "heard: any timeslot to start with");
    at_timeslot(&f, 3);
    expect(!heard_burst(&f, &sp, 1, 0), "heard: another timeslot waits while TN2 speaks");
    clock_at(1000.0 + TETRA_CALL_SPEECH_END_S - 0.1);
    expect(!heard_burst(&f, &sp, 1, 0), "heard: and waits until the speech has stopped long enough");
    at_timeslot(&f, 2);
    expect(heard_burst(&f, &sp, 1, 0), "heard: TN2 goes on");
    clock_at(1000.0 + TETRA_CALL_SPEECH_END_S);
    at_timeslot(&f, 3);
    expect(heard_burst(&f, &sp, 1, 1) && sp.tn == 3, "heard: once TN2 stopped, TN3 is heard");
    at_timeslot(&f, 2);
    expect(!heard_burst(&f, &sp, 1, 0), "heard: and holds the receiver in turn");
    tetra_calls_forget_carrier(f.state);
    expect(heard_burst(&f, &sp, 1, 0), "heard: a new carrier hears any timeslot");

    /* A downlink carrier's BSCH sets the TDMA clock, which then places every burst: one it cannot place yet may be any
       timeslot's, so it is not heard, with no timeslot heard as with one holding the receiver. */
    tetra_state_t* const ts = tetra_st(f.state);
    ts->tdma_valid = 0;
    expect(!tetra_voice_gate(f.opts, f.state, &sp) && sp.tn == 0 && sp.kind == 0,
           "heard: a downlink burst the clock cannot place is not heard");
    at_timeslot(&f, 4);
    expect(heard_burst(&f, &sp, 1, 1) && sp.tn == 4, "heard: TN4 once the clock places it");
    ts->tdma_valid = 0;
    expect(!tetra_voice_gate(f.opts, f.state, &sp), "heard: nor while TN4 holds the receiver");
    at_timeslot(&f, 4);
    expect(heard_burst(&f, &sp, 1, 0), "heard: TN4 goes on");

    /* The uplink has no BSCH: nothing places its bursts, and its speech is heard as it comes. */
    tetra_calls_forget_carrier(f.state);
    ts->tdma_valid = 0;
    ts->uplink = 1;
    expect(heard_burst(&f, &sp, 1, 1) && sp.tn == 0, "heard: uplink speech, which nothing places, is heard");
    expect(heard_burst(&f, &sp, 1, 0), "heard: and goes on");
    fixture_close(&f);
}

/* Not trunking, speech that does not play (its talkgroup policy or encryption mutes it) holds nothing: the heard
 * timeslot shows its call, and speech that plays on another timeslot takes the receiver, which then holds it against
 * muted and playing speech alike; muted speech of another timeslot waits while the shown call speaks, so two muted
 * calls do not take turns. A hold runs out TETRA_CALL_SPEECH_END_S after the held timeslot's last speech that played,
 * whatever muted speech of it came since. Trunking on and in direct mode, the hold does not apply. */
static void
test_muted_speech_holds_nothing(void) {
    fixture f;
    fixture_open(&f);
    tetra_speech sp;
    at_timeslot(&f, 2);
    expect(heard_burst(&f, &sp, 0, 1), "muted: TN2's muted speech is heard");
    at_timeslot(&f, 4);
    expect(!heard_burst(&f, &sp, 0, 0), "muted: TN4's muted speech waits while TN2's is shown");
    at_timeslot(&f, 3);
    expect(heard_burst(&f, &sp, 1, 1) && sp.tn == 3, "muted: TN3's speech that plays takes the receiver");
    at_timeslot(&f, 2);
    expect(!heard_burst(&f, &sp, 0, 0) && !heard_burst(&f, &sp, 1, 0), "muted: TN3 then holds it against TN2");

    /* TN3's speech goes on muted: its hold runs out a speech end after the last burst that played. */
    clock_at(1000.0 + 0.5);
    at_timeslot(&f, 3);
    expect(heard_burst(&f, &sp, 0, 1), "muted: TN3 goes on muted");
    clock_at(1000.0 + TETRA_CALL_SPEECH_END_S - 0.1);
    at_timeslot(&f, 2);
    expect(!heard_burst(&f, &sp, 1, 0), "muted: TN3 still holds the receiver");
    at_timeslot(&f, 3);
    expect(heard_burst(&f, &sp, 0, 1), "muted: TN3 muted again");
    clock_at(1000.0 + TETRA_CALL_SPEECH_END_S + 0.1);
    at_timeslot(&f, 4);
    expect(!heard_burst(&f, &sp, 0, 0), "muted: TN4's muted speech still waits for TN3's shown call");
    at_timeslot(&f, 2);
    expect(heard_burst(&f, &sp, 1, 1) && sp.tn == 2, "muted: TN2's speech that plays takes the receiver");

    /* Trunking on and direct mode have gates of their own. */
    f.opts->trunk_enable = 1;
    sp.tn = 3;
    sp.audible = 0;
    sp.kind = TETRA_SPEECH_CALL;
    expect(tetra_voice_hold(f.opts, f.state, &sp), "muted: no hold while trunking");
    f.opts->trunk_enable = 0;
    sp.kind = TETRA_SPEECH_DMO;
    expect(tetra_voice_hold(f.opts, f.state, &sp), "muted: nor in direct mode");
    expect(!tetra_voice_hold(f.opts, f.state, NULL) && !tetra_voice_hold(f.opts, NULL, &sp), "muted: no state");
    fixture_close(&f);
}

/* Heard speech belongs to the record whose traffic channel is its timeslot of the tuned frequency, else to none. */
static void
test_heard_call(void) {
    fixture f;
    fixture_open(&f);
    const tetra_state_t* ts = tetra_st(f.state);
    tetra_speech sp;
    group_call(&f, 9, 0x123456u, 392012500L, 0x04); /* TN2 */
    group_call(&f, 10, 0x222222u, 392037500L, 0x04);
    at_timeslot(&f, 2);
    expect(tetra_voice_gate(f.opts, f.state, &sp) && sp.kind == TETRA_SPEECH_ANON,
           "heard call: no tuned frequency, no record");
    f.opts->audio_in_type = AUDIO_IN_RTL;
    f.opts->rtlsdr_center_freq = 392012500U;
    expect(tetra_voice_gate(f.opts, f.state, &sp) && sp.kind == TETRA_SPEECH_CALL && sp.call_id == 9
               && sp.target == 0x123456u,
           "heard call: the record on the tuned carrier's timeslot");
    f.opts->rtlsdr_center_freq = 392037500U;
    expect(tetra_voice_gate(f.opts, f.state, &sp) && sp.call_id == 10, "heard call: each carrier its own record");
    at_timeslot(&f, 3);
    expect(tetra_voice_gate(f.opts, f.state, &sp) && sp.kind == TETRA_SPEECH_ANON,
           "heard call: another timeslot is not the call's");

    /* The call's own evidence decides its air encryption; speech of no record takes its timeslot's, else the
       carrier's. */
    at_timeslot(&f, 2);
    tetra_calls_find(f.state, 10)->air_enc = TETRA_ENC_MODE_EVEN;
    tetra_calls_find(f.state, 10)->e2ee = 1;
    expect(tetra_voice_gate(f.opts, f.state, &sp) && sp.air_enc == TETRA_ENC_MODE_EVEN && sp.sealed,
           "heard call: the record's air and end-to-end encryption");
    at_timeslot(&f, 3);
    expect(tetra_voice_gate(f.opts, f.state, &sp) && sp.air_enc == TETRA_ENC_MODE_NONE && !sp.sealed,
           "heard call: no evidence is clear");
    tetra_mac_address addr;
    DSD_MEMSET(&addr, 0, sizeof(addr));
    addr.type = TETRA_MAC_ADDR_SSI;
    addr.ssi = 0x777777u;
    tetra_calls_on_addressed(f.opts, f.state, &addr, TETRA_ENC_MODE_ODD, 0, 2);
    expect(tetra_voice_gate(f.opts, f.state, &sp) && sp.air_enc == TETRA_ENC_MODE_ODD,
           "heard call: the carrier's evidence when the timeslot had none");
    tetra_calls_on_addressed(f.opts, f.state, &addr, TETRA_ENC_MODE_NONE, 0, 3);
    tetra_calls_on_addressed(f.opts, f.state, &addr, TETRA_ENC_MODE_EVEN, 0, 1);
    expect(tetra_voice_gate(f.opts, f.state, &sp) && sp.air_enc == TETRA_ENC_MODE_NONE,
           "heard call: the timeslot's own evidence first");
    tetra_calls_forget_carrier(f.state);
    expect(tetra_voice_gate(f.opts, f.state, &sp) && sp.air_enc == TETRA_ENC_MODE_NONE && ts->calls.slot_enc[0] == 0u,
           "heard call: a new carrier has no evidence");
    fixture_close(&f);
}

static void
test_crypto_class(void) {
    tetra_speech sp;
    DSD_MEMSET(&sp, 0, sizeof(sp));
    expect(tetra_voice_crypto_class(&sp, 0) == DSD_VOICE_CRYPTO_CLEAR, "crypto: not encrypted is clear");
    expect(tetra_voice_crypto_class(&sp, 1) == DSD_VOICE_CRYPTO_CLEAR, "crypto: a stray key stream changes nothing");
    for (uint8_t mode = TETRA_ENC_MODE_RESERVED; mode <= TETRA_ENC_MODE_ODD; mode++) {
        sp.air_enc = mode;
        expect(tetra_voice_crypto_class(&sp, 1) == DSD_VOICE_CRYPTO_DECRYPTABLE, "crypto: air encrypted, decrypted");
        expect(tetra_voice_crypto_class(&sp, 0) == DSD_VOICE_CRYPTO_UNDECRYPTABLE, "crypto: air encrypted, no key");
    }
    sp.air_enc = TETRA_ENC_MODE_NONE;
    sp.sealed = 1;
    expect(tetra_voice_crypto_class(&sp, 0) == DSD_VOICE_CRYPTO_UNDECRYPTABLE, "crypto: end-to-end is undecryptable");
    sp.air_enc = TETRA_ENC_MODE_EVEN;
    expect(tetra_voice_crypto_class(&sp, 1) == DSD_VOICE_CRYPTO_UNDECRYPTABLE,
           "crypto: an air key stream does not undo end-to-end encryption");
    expect(tetra_voice_crypto_class(NULL, 0) == DSD_VOICE_CRYPTO_CLEAR, "crypto: no speech is clear");
}

int
main(void) {
    dsd_decode_clock_use_test(SECOND_NS * 1000ULL);
    test_trunked_gate();
    test_direct_mode();
    test_heard_timeslot();
    test_muted_speech_holds_nothing();
    test_heard_call();
    test_crypto_class();
    if (g_fail) {
        DSD_FPRINTF(stderr, "%d check(s) failed\n", g_fail);
        return 1;
    }
    puts("PASS test_tetra_voice_gate");
    return 0;
}
