// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * The gates a TETRA speech burst passes before it plays (tetra_voice_gate.h): the followed call or the heard timeslot,
 * and the call whose talkgroup and encryption the burst is judged by.
 */

#include "tetra_voice_gate.h"

#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/protocol/tetra/tetra_dmo.h>
#include <dsd-neo/protocol/tetra/tetra_mac.h>
#include <dsd-neo/protocol/tetra/tetra_trunk_sm.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <stdint.h>

#include "tetra_calls.h"
#include "tetra_confirm.h"
#include "tetra_state.h"

/* The burst's timeslot from the TDMA clock (a BSCH sets it, tetra_tdma_advance_ndb() runs it), 0 when it is stopped. */
static uint8_t
voice_timeslot(const tetra_state_t* ts) {
    return (ts->tdma_valid && ts->tn >= 1u && ts->tn <= 4u) ? ts->tn : 0u;
}

/* The speech of @p c, or of no call record: its air encryption then comes from the timeslot's last addressed
 * MAC-RESOURCE, or the carrier's when the timeslot had none. */
static void
voice_of_call(const tetra_state_t* ts, const tetra_call_t* c, tetra_speech* sp) {
    if (c) {
        sp->kind = TETRA_SPEECH_CALL;
        sp->call_id = c->call_id;
        /* The canonical call's target, which the talkgroup policy finds the call by (tetra_calls_target()). */
        sp->target = tetra_calls_target(c);
        sp->source = c->talker_ssi;
        sp->air_enc = c->air_enc;
        sp->sealed = c->e2ee;
        return;
    }
    sp->kind = TETRA_SPEECH_ANON;
    uint8_t evidence = ts->calls.slot_enc[sp->tn];
    if (evidence == 0u) {
        evidence = ts->calls.slot_enc[0];
    }
    sp->air_enc = evidence != 0u ? (uint8_t)(evidence - 1u) : (uint8_t)TETRA_ENC_MODE_NONE;
}

/* The followed call's record does not place it elsewhere than the followed channel, timeslot @p tn of the followed
 * carrier. An allocation that moved it away without moving the receiver (one no call PDU took, which never tunes, or a
 * reallocation whose tune failed) leaves the followed channel to whatever call the SwMI gives it next; a record that
 * holds no allocation says nothing. A call whose channel was allocated to another call is no followed call's: the state
 * machine let go of it there (calls_take_channel() in tetra_calls.c). */
static int
voice_record_on(const tetra_call_t* c, const tetra_sm_grant* g, uint8_t tn) {
    if (c->alloc.timeslots == 0u) {
        return 1;
    }
    return c->alloc.carrier == g->carrier && (c->alloc.freq_hz == 0L || c->alloc.freq_hz == g->freq_hz)
           && (c->alloc.timeslots & tetra_slot_bit(tn)) != 0u;
}

/* Trunking: the followed call's timeslots on its carrier, once the tune settled and its signalling proved it there, and
 * while its record still has it there. A burst the TDMA clock cannot place (tn 0: a retune stopped the clock, and the
 * carrier's next BSCH has not set it yet, though addressed signalling may already have proved the call) can be any
 * timeslot's, another call's included, so it is not the followed call's. */
static int
voice_followed(const dsd_state* state, const tetra_state_t* ts, tetra_speech* sp) {
    tetra_sm_grant g;
    if (!tetra_sm_followed(state, &g) || ts->sm.pending_request_id != 0U) {
        return 0;
    }
    if ((g.timeslots & tetra_slot_bit(sp->tn)) == 0u) {
        return 0;
    }
    if (!tetra_confirm_followed_call_proven(state)) {
        return 0;
    }
    const tetra_call_t* const c = tetra_calls_peek(state, g.call_id);
    if (c && !voice_record_on(c, &g, sp->tn)) {
        return 0;
    }
    sp->followed = 1;
    voice_of_call(ts, c, sp);
    return 1;
}

/* Not trunking: any timeslot, the one the receiver hears decided once the burst's audibility is known
 * (tetra_voice_hold()). On the downlink the TDMA clock places each burst once the carrier's BSCH set it, and one it
 * cannot place (tn 0) may be any timeslot's, so it is not heard, as a timeslot holding the receiver would not be told
 * from another. The uplink has no BSCH, so nothing places its bursts (all tn 0) and its speech is heard as it comes. */
static int
voice_heard(const dsd_opts* opts, const dsd_state* state, const tetra_state_t* ts, tetra_speech* sp) {
    if (sp->tn == 0u && !ts->uplink) {
        return 0;
    }
    voice_of_call(ts, tetra_calls_on_timeslot(opts, state, sp->tn), sp);
    return 1;
}

/* Direct mode: the call on the channel, by the destination its DM-SETUP or DM-OCCUPIED gave (none while DM-2-B hides
 * it) and its security class; there is no direct mode key stream, so anything not known clear is sealed. */
static void
voice_direct(const dsd_state* state, const tetra_state_t* ts, tetra_speech* sp) {
    sp->kind = TETRA_SPEECH_DMO;
    sp->target = ts->calls.dmo.active ? ts->calls.dmo.dst : 0u;
    sp->source = ts->calls.dmo.active ? ts->calls.dmo.src : 0u;
    sp->sealed = (uint8_t)(tetra_dmo_call_crypt(state) != TETRA_DMO_CRYPT_CLEAR);
}

int
tetra_voice_gate(const dsd_opts* opts, const dsd_state* state, tetra_speech* sp) {
    if (!sp) {
        return 0;
    }
    DSD_MEMSET(sp, 0, sizeof(*sp));
    const tetra_state_t* const ts = tetra_st_peek(state);
    if (!ts || tetra_tdma_control_frame(ts)) {
        /* Frame 18 carries no speech: nothing of it is heard, played or concealed. */
        return 0;
    }
    sp->tn = voice_timeslot(ts);
    int heard = 1;
    if (ts->dmo) {
        voice_direct(state, ts, sp);
    } else if (opts && opts->trunk_enable == 1) {
        heard = voice_followed(state, ts, sp);
    } else {
        heard = voice_heard(opts, state, ts, sp);
    }
    if (!heard) {
        DSD_MEMSET(sp, 0, sizeof(*sp));
    }
    return heard;
}

int
tetra_voice_hold(const dsd_opts* opts, const dsd_state* state, const tetra_speech* sp) {
    const tetra_state_t* const ts = tetra_st_peek(state);
    if (!ts || !sp) {
        return 0;
    }
    if ((opts && opts->trunk_enable == 1) || sp->kind == TETRA_SPEECH_DMO) {
        return 1;
    }
    const tetra_calls_t* const calls = &ts->calls;
    const double now_m = dsd_decode_now_mono_s();
    if (!calls->heard || sp->tn == calls->heard_tn || now_m - calls->heard_m >= TETRA_CALL_SPEECH_END_S) {
        return 1;
    }
    const int held = calls->held && now_m - calls->held_m < TETRA_CALL_SPEECH_END_S;
    return !held && sp->audible;
}

int
tetra_voice_audible(const dsd_opts* opts, const dsd_state* state, const tetra_speech* sp, int keystream_applied) {
    return sp && dsd_voice_crypto_class_audible(opts, 0, tetra_voice_crypto_class(sp, keystream_applied))
           && !tetra_calls_speech_muted(opts, state, sp);
}

dsd_voice_crypto_class
tetra_voice_crypto_class(const tetra_speech* sp, int keystream_applied) {
    if (!sp) {
        return DSD_VOICE_CRYPTO_CLEAR;
    }
    if (sp->sealed) {
        return DSD_VOICE_CRYPTO_UNDECRYPTABLE;
    }
    if (sp->air_enc == TETRA_ENC_MODE_NONE) {
        return DSD_VOICE_CRYPTO_CLEAR;
    }
    return keystream_applied ? DSD_VOICE_CRYPTO_DECRYPTABLE : DSD_VOICE_CRYPTO_UNDECRYPTABLE;
}
