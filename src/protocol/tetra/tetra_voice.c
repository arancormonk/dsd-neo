// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * TETRA speech: type-1 bits to speech frames (EN 300 395-2 clause 5), mbelib-neo's TETRA speech decoder and the
 * digital voice output.
 */

#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/voice_chain.h>
#include <dsd-neo/protocol/tetra/tetra_air.h>
#include <dsd-neo/protocol/tetra/tetra_fec.h>
#include <dsd-neo/protocol/tetra/tetra_mac.h>
#include <dsd-neo/protocol/tetra/tetra_voice.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/log.h>
#include <mbelib-neo/mbelib.h>
#include <stddef.h>
#include <stdint.h>

#include "tetra_air_sig.h"
#include "tetra_calls.h"
#include "tetra_state.h"
#include "tetra_voice_gate.h"
#include "tetra_voice_state.h"

/* Speech frame bit numbers (B1 = 1) of each sensitivity class, in the order the classes occur in the type-1 bits
   (EN 300 395-2 table 4 assigns the classes; tables 5 and 6 order them). */
static const uint8_t k_class0_bits[] = {
    35, 36,  37,  38,  39,  40,  41,  42,  43,  47,  48,  56,  61,  62,  63,  64,  65,
    66, 67,  68,  69,  70,  74,  75,  83,  88,  89,  90,  91,  92,  93,  94,  95,  96,
    97, 101, 102, 110, 115, 116, 117, 118, 119, 120, 121, 122, 123, 124, 128, 129, 137,
};
static const uint8_t k_class1_bits[] = {
    58, 85,  112, 54, 81, 108, 135, 50, 77, 104, 131, 45,  72, 99, 126, 55, 82, 109, 136,
    5,  13,  34,  8,  16, 17,  22,  23, 24, 25,  26,  6,   14, 7,  15,  60, 87, 114, 46,
    73, 100, 127, 44, 71, 98,  125, 33, 49, 76,  103, 130, 59, 86, 113, 57, 84, 111,
};
static const uint8_t k_class2_bits[] = {
    18, 19, 20, 21, 31, 32, 53, 80, 107, 134, 1,   2,  3,  4,   9,
    10, 11, 12, 27, 28, 29, 30, 52, 79,  106, 133, 51, 78, 105, 132,
};

enum {
    CLASS0_BITS = sizeof(k_class0_bits),
    CLASS1_BITS = sizeof(k_class1_bits),
    CLASS2_BITS = sizeof(k_class2_bits),
};

_Static_assert(CLASS0_BITS + CLASS1_BITS + CLASS2_BITS == TETRA_VOICE_FRAME_BITS, "a speech frame is 137 bits");
_Static_assert(2 * TETRA_VOICE_FRAME_BITS == TETRA_SPEECH_FULL_BITS, "a full slot carries two frames");

/* Scatter one class into the speech frames: @p nframes frames alternate bit by bit within the class (table 5's A,B
   pairs), one frame takes them in turn (table 6). Returns the type-1 bits consumed. */
static int
voice_scatter_class(const uint8_t* type1, const uint8_t* positions, int count, int nframes, uint8_t* frames) {
    for (int bit = 0; bit < count; bit++) {
        for (int frame = 0; frame < nframes; frame++) {
            frames[(frame * TETRA_VOICE_FRAME_BITS) + positions[bit] - 1] = type1[(bit * nframes) + frame] & 1u;
        }
    }
    return count * nframes;
}

static void
voice_reorder(const uint8_t* type1, int nframes, uint8_t* frames) {
    const uint8_t* in = type1;
    in += voice_scatter_class(in, k_class0_bits, CLASS0_BITS, nframes, frames);
    in += voice_scatter_class(in, k_class1_bits, CLASS1_BITS, nframes, frames);
    (void)voice_scatter_class(in, k_class2_bits, CLASS2_BITS, nframes, frames);
}

void
tetra_voice_reorder_full(const uint8_t* type1, uint8_t* frames) {
    if (type1 && frames) {
        voice_reorder(type1, 2, frames);
    }
}

void
tetra_voice_reorder_half(const uint8_t* type1, uint8_t* frame) {
    if (type1 && frame) {
        voice_reorder(type1, 1, frame);
    }
}

_Static_assert(TETRA_VOICE_FRAME_BITS == MBE_TETRA_FRAME_BITS, "mbelib-neo's TETRA frame is 137 bits");
_Static_assert(TETRA_VOICE_FRAME_SAMPLES == MBE_TETRA_FRAME_SAMPLES, "mbelib-neo's TETRA frame is 240 samples");

/* ---------------------------------------------------------------------------------------------------------------
 * The speech decoder.
 * ------------------------------------------------------------------------------------------------------------- */

static void*
voice_mbe_alloc(void) {
    return mbe_tetraDecoderAlloc();
}

static void
voice_mbe_reset(void* decoder) {
    mbe_tetraDecoderReset((mbe_tetra_decoder*)decoder);
}

static void
voice_mbe_release(void* decoder) {
    mbe_tetraDecoderFree((mbe_tetra_decoder*)decoder);
}

static int
voice_mbe_decode(void* decoder, const uint8_t* bits, int bad_frame, int16_t* pcm) {
    return mbe_tetraDecodeFrame((mbe_tetra_decoder*)decoder, bits, bad_frame, pcm);
}

static const tetra_voice_codec k_mbe_codec = {voice_mbe_alloc, voice_mbe_reset, voice_mbe_release, voice_mbe_decode};

/* The codec new decoders come from; a test may install another (tetra_voice_set_codec_for_test()). */
static const tetra_voice_codec* g_voice_codec = &k_mbe_codec;

void
tetra_voice_set_codec_for_test(const tetra_voice_codec* codec) {
    g_voice_codec = codec ? codec : &k_mbe_codec;
}

static void
voice_codec_release(tetra_voice_t* v) {
    if (v->codec && v->codec_ops) {
        v->codec_ops->release(v->codec);
    }
    v->codec = NULL;
    v->codec_ops = NULL;
}

void
tetra_voice_release(tetra_voice_t* voice) {
    if (!voice) {
        return;
    }
    voice_codec_release(voice);
    DSD_MEMSET(voice, 0, sizeof(*voice));
}

/* 1 when a decoder is ready for the next frame: it is made at the first frame that needs it, and made again after a
   frame for which it could not be. */
static int
voice_codec_ready(tetra_voice_t* v) {
    if (v->codec) {
        return 1;
    }
    v->codec = g_voice_codec->alloc();
    if (!v->codec) {
        return 0;
    }
    v->codec_ops = g_voice_codec;
    return 1;
}

/* One frame the speech decoder could not take: no decoder, or a failed decode. Logged once per TETRA session. */
static void
voice_codec_failed(tetra_voice_t* v, const char* what) {
    if (v->codec_errors++ == 0U) {
        LOG_ERROR("TETRA: %s; such speech frames are dropped.\n", what);
    }
    v->last_decode_failed = 1;
    v->carry_len = 0;
}

/* ---------------------------------------------------------------------------------------------------------------
 * Frames to the output.
 * ------------------------------------------------------------------------------------------------------------- */

/* The burst's verdict for every output, settled once per burst. */
typedef struct {
    int audible; /* the encryption class and the talkgroup policy let it play */
    int record;  /* the record policy lets the recordings take it */
} voice_route;

/* The burst plays as tetra_voice_audible() judged it; the record policy then judges the canonical call the speech
   belongs to, published before it is asked, which gives a private call its two parties. */
static voice_route
voice_route_for(const dsd_opts* opts, const dsd_state* state, const tetra_speech* sp) {
    voice_route route = {sp->audible != 0u, 0};
    if (route.audible) {
        int allow = 0;
        route.record = dsd_audio_record_policy_gate_slot(opts, state, 0, &allow) == 0 && allow;
    }
    return route;
}

/* Play @p pcm after the samples carried from the last frame; whole output frames go out, the rest waits. */
static void
voice_play(const dsd_opts* opts, dsd_state* state, tetra_voice_t* v, const short* pcm, int record) {
    short buf[DSD_VOICE_FRAME_SAMPLES + TETRA_VOICE_FRAME_SAMPLES];
    size_t count = v->carry_len;
    DSD_MEMCPY(buf, v->carry, count * sizeof(short));
    DSD_MEMCPY(buf + count, pcm, (size_t)TETRA_VOICE_FRAME_SAMPLES * sizeof(short));
    count += TETRA_VOICE_FRAME_SAMPLES;
    const size_t whole = count - (count % DSD_VOICE_FRAME_SAMPLES);
    dsd_audio_play_decoded_pcm16_record(opts, state, buf, whole, record);
    v->carry_len = (uint16_t)(count - whole);
    DSD_MEMCPY(v->carry, buf + whole, (size_t)v->carry_len * sizeof(short));
}

/* Integrity bookkeeping for one frame: 1 when it goes to the codec. A good frame starts or continues the speech; a
   bad one continues it, concealed, until TETRA_VOICE_CONCEAL_FRAMES of them in a row end it. */
static int
voice_frame_admitted(tetra_voice_t* v, int bad) {
    if (!bad) {
        v->confirmed = 1;
        v->bad_run = 0;
        return 1;
    }
    if (v->confirmed && v->bad_run < TETRA_VOICE_CONCEAL_FRAMES) {
        v->bad_run++;
        return 1;
    }
    v->confirmed = 0;
    v->carry_len = 0;
    return 0;
}

/* Decode and play one admitted frame under the burst's @p route. */
static void
voice_frame_play(const dsd_opts* opts, dsd_state* state, tetra_voice_t* v, const uint8_t* bits, int bad,
                 const voice_route* route) {
    if (!route->audible) {
        v->carry_len = 0;
        return;
    }
    if (!voice_codec_ready(v)) {
        voice_codec_failed(v, "no TETRA speech decoder could be made");
        return;
    }
    int16_t pcm[TETRA_VOICE_FRAME_SAMPLES];
    DSD_MEMSET(pcm, 0, sizeof(pcm));
    if (v->codec_ops->decode(v->codec, bits, bad ? 1 : 0, pcm) != 0) {
        voice_codec_failed(v, "the TETRA speech decoder failed to decode a speech frame");
        return;
    }
    v->last_decode_failed = 0;
    v->frames++;
    if (bad) {
        v->concealed++;
    }
    voice_play(opts, state, v, pcm, route->record);
}

/* Forget the speech in progress: the decoder's state, its integrity and its unplayed samples. */
static void
voice_speech_reset(tetra_voice_t* v) {
    if (v->codec) {
        v->codec_ops->reset(v->codec);
    }
    v->confirmed = 0;
    v->bad_run = 0;
    v->carry_len = 0;
    v->last_good_m = 0.0;
    v->bound = 0;
}

/* The speech in progress belongs to one transmission. Speech of another call (another kind, call identifier or
   target), on another timeslot (each timeslot of a carrier is a traffic channel of its own, EN 300 392-2 clause
   9.4.2.2: the speech no call record names included, which has no other identity), of another transmitting party (D-TX
   GRANTED or D-TX INTERRUPT naming another talker, another direct mode source; a party not known on either side is no
   change, as for the canonical call), or after a TETRA call ended (a release, D-TX CEASED, the state machine leaving
   it, another call published), starts over: no samples, codec state or confirmation of one transmission reach the next
   or its record policy. So does speech that comes TETRA_CALL_SPEECH_END_S or more after the last burst of it with a
   good frame (@p now_m), in every mode: a pause that long ends the transmission, as it ends the heard speech, so the
   next one, whose D-TX CEASED, D-TX GRANTED or DMAC-SYNC may have gone unheard, conceals nothing with the last one's
   decoder. The call identifier and the timeslot tell two transmissions apart at the first burst of the
   second, before anything of it was published: a burst whose CRC failed publishes nothing. The kind tells direct mode
   from trunked mode, and whether trunked mode is the downlink or the uplink is the session's (--tetra-uplink). */
static void
voice_bind(tetra_state_t* ts, const tetra_speech* sp, double now_m) {
    tetra_voice_t* const v = &ts->voice;
    const uint16_t call_id = sp->kind == TETRA_SPEECH_CALL ? sp->call_id : 0U;
    const int other_party = v->owner_source != 0u && sp->source != 0u && v->owner_source != sp->source;
    const int paused = v->last_good_m > 0.0 && now_m - v->last_good_m >= TETRA_CALL_SPEECH_END_S;
    if (v->bound && v->owner_kind == sp->kind && v->owner_call_id == call_id && v->owner_tn == sp->tn
        && v->owner_target == sp->target && v->owner_ends == ts->calls.ends && !other_party && !paused) {
        if (sp->source != 0u) {
            v->owner_source = sp->source;
        }
        return;
    }
    voice_speech_reset(v);
    v->bound = 1;
    v->owner_kind = sp->kind;
    v->owner_call_id = call_id;
    v->owner_tn = sp->tn;
    v->owner_target = sp->target;
    v->owner_source = sp->source;
    v->owner_ends = ts->calls.ends;
}

void
tetra_voice_call_renamed(tetra_voice_t* voice, uint16_t old_id, uint16_t new_id) {
    if (voice && voice->bound && voice->owner_kind == TETRA_SPEECH_CALL && voice->owner_call_id == old_id) {
        voice->owner_call_id = new_id;
    }
}

void
tetra_voice_transmission_started(tetra_voice_t* voice, uint16_t call_id) {
    if (voice && voice->bound && voice->owner_kind == TETRA_SPEECH_CALL && voice->owner_call_id == call_id) {
        voice_speech_reset(voice);
    }
}

/* The frames of one burst of @p sp's call: @p bad[i] for frame i of @p frames. Whether the burst plays is judged first
   (tetra_voice_audible()), so that only speech that plays holds the receiver on its timeslot (tetra_voice_hold());
   speech whose CRC passed then reaches the call (tetra_calls_on_speech()), and the record policy is asked once per
   burst, only for a burst with a frame to play. */
static void
voice_burst(dsd_opts* opts, dsd_state* state, tetra_speech* sp, const uint8_t* frames, const int* bad, int nframes,
            int keystream_applied) {
    tetra_state_t* const ts = tetra_st_existing(state);
    if (!ts) {
        return;
    }
    sp->audible = (uint8_t)tetra_voice_audible(opts, state, sp, keystream_applied);
    if (!tetra_voice_hold(opts, state, sp)) {
        return;
    }
    int good = 0;
    for (int i = 0; i < nframes; i++) {
        good |= !bad[i];
    }
    if (good) {
        tetra_calls_on_speech(opts, state, sp);
    }
    /* After the call heard the speech: publishing this call may have ended the last one. */
    const double now_m = dsd_decode_now_mono_s();
    voice_bind(ts, sp, now_m);
    if (good) {
        ts->voice.last_good_m = now_m;
    }
    voice_route route = {0, 0};
    int routed = 0;
    for (int i = 0; i < nframes; i++) {
        if (!voice_frame_admitted(&ts->voice, bad[i])) {
            continue;
        }
        if (!routed) {
            route = voice_route_for(opts, state, sp);
            routed = 1;
        }
        voice_frame_play(opts, state, &ts->voice, frames + ((size_t)i * TETRA_VOICE_FRAME_BITS), bad[i], &route);
    }
}

/* Undo the air interface encryption of an air-encrypted trunked call's speech; 1 when a key stream applied. The
   encryption sits above the channel coding (EN 300 392-7 clause 6.1), whose first step orders the speech frame bits
   into sensitivity classes (EN 300 395-2 clause 5.2), so @p frame_bits are speech frame bits B1..B137 in the order the
   codec made them, after voice_reorder(). */
static int
voice_decrypt(const dsd_opts* opts, const dsd_state* state, const tetra_speech* sp, uint8_t* frame_bits, int nbits,
              int kss) {
    if (sp->sealed || sp->air_enc == TETRA_ENC_MODE_NONE) {
        return 0;
    }
    /* A call whose allocation came in decrypted signalling takes the key that decrypted it, while it applies. */
    const tetra_call_t* const c = sp->kind == TETRA_SPEECH_CALL ? tetra_calls_peek(state, sp->call_id) : NULL;
    if (c && c->air_key.valid) {
        return tetra_enc_mode_encrypted(sp->air_enc)
               && tetra_air_sig_decrypt_voice(opts, state, &c->air_key, frame_bits, nbits, kss);
    }
    return tetra_air_decrypt_voice(opts, state, sp->air_enc, frame_bits, nbits, kss);
}

void
tetra_voice_full_slot(dsd_opts* opts, dsd_state* state, const uint8_t* type1, int good) {
    uint8_t frames[2 * TETRA_VOICE_FRAME_BITS];
    tetra_speech sp;
    if (!state || !type1 || !tetra_st(state) || !tetra_voice_gate(opts, state, &sp)) {
        return;
    }
    /* Frame A takes KSS(0..136), frame B KSS(137..273). */
    tetra_voice_reorder_full(type1, frames);
    const int keystream = voice_decrypt(opts, state, &sp, frames, (int)sizeof(frames), TETRA_AIR_KSS_TCH_S);
    const int bad[2] = {!good, !good};
    voice_burst(opts, state, &sp, frames, bad, 2, keystream);
    DSD_SECURE_ZERO(frames, sizeof(frames));
}

void
tetra_voice_half_slot(dsd_opts* opts, dsd_state* state, const uint8_t* type1, int good) {
    uint8_t frames[2 * TETRA_VOICE_FRAME_BITS];
    tetra_speech sp;
    if (!state || !type1 || !tetra_st(state) || !tetra_voice_gate(opts, state, &sp)) {
        return;
    }
    /* Frame A of the slot was stolen (EN 300 395-2 clause 5.2): the codec conceals it, frame B is the speech. */
    uint8_t* const speech = frames + TETRA_VOICE_FRAME_BITS;
    DSD_MEMSET(frames, 0, TETRA_VOICE_FRAME_BITS);
    tetra_voice_reorder_half(type1, speech);
    const int keystream = voice_decrypt(opts, state, &sp, speech, TETRA_VOICE_FRAME_BITS, TETRA_AIR_KSS_TCH_S_HALF);
    const int bad[2] = {1, !good};
    voice_burst(opts, state, &sp, frames, bad, 2, keystream);
    DSD_SECURE_ZERO(frames, sizeof(frames));
}

void
tetra_voice_reset(dsd_state* state) {
    /* A session that never decoded TETRA has nothing to reset, and gets no TETRA state for it. */
    if (!state || !dsd_state_ext_get(state, DSD_STATE_EXT_PROTO_TETRA)) {
        return;
    }
    tetra_state_t* const ts = tetra_st(state);
    if (!ts) {
        return;
    }
    voice_speech_reset(&ts->voice);
}
