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
        sp->target = c->called_ssi;
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

/* Trunking: the followed call's timeslots on its carrier, once the tune settled and its signalling proved it there. */
static int
voice_followed(const dsd_state* state, const tetra_state_t* ts, tetra_speech* sp) {
    tetra_sm_grant g;
    if (!tetra_sm_followed(state, &g) || ts->sm.pending_request_id != 0U) {
        return 0;
    }
    if ((g.timeslots & 0x0Fu) == 0u || (sp->tn != 0u && (g.timeslots & tetra_slot_bit(sp->tn)) == 0u)) {
        return 0;
    }
    if (!tetra_confirm_followed_call_proven(state)) {
        return 0;
    }
    sp->followed = 1;
    voice_of_call(ts, tetra_calls_peek(state, g.call_id), sp);
    return 1;
}

/* Not trunking: the heard timeslot, or any while none has been heard for TETRA_CALL_SPEECH_END_S. */
static int
voice_heard(const dsd_opts* opts, const dsd_state* state, const tetra_state_t* ts, tetra_speech* sp) {
    const tetra_calls_t* const calls = &ts->calls;
    if (calls->heard && sp->tn != 0u && calls->heard_tn != 0u && sp->tn != calls->heard_tn
        && dsd_decode_now_mono_s() - calls->heard_m < TETRA_CALL_SPEECH_END_S) {
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
    if (!ts) {
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
