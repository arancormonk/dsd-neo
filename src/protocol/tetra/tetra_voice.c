// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * TETRA speech: type-1 bits to speech frames (EN 300 395-2 clause 5), the codec library (tetra_codec_abi.h) and the
 * digital voice output.
 */

#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/voice_chain.h>
#include <dsd-neo/platform/dynlib.h>
#include <dsd-neo/protocol/tetra/tetra_air.h>
#include <dsd-neo/protocol/tetra/tetra_codec_abi.h>
#include <dsd-neo/protocol/tetra/tetra_fec.h>
#include <dsd-neo/protocol/tetra/tetra_mac.h>
#include <dsd-neo/protocol/tetra/tetra_voice.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/log.h>
#include <stddef.h>
#include <stdint.h>

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

_Static_assert(CLASS0_BITS + CLASS1_BITS + CLASS2_BITS == DSD_TETRA_CODEC_FRAME_BITS, "a speech frame is 137 bits");
_Static_assert(2 * DSD_TETRA_CODEC_FRAME_BITS == TETRA_SPEECH_FULL_BITS, "a full slot carries two frames");

/* Scatter one class into the speech frames: @p nframes frames alternate bit by bit within the class (table 5's A,B
   pairs), one frame takes them in turn (table 6). Returns the type-1 bits consumed. */
static int
voice_scatter_class(const uint8_t* type1, const uint8_t* positions, int count, int nframes, uint8_t* frames) {
    for (int bit = 0; bit < count; bit++) {
        for (int frame = 0; frame < nframes; frame++) {
            frames[(frame * DSD_TETRA_CODEC_FRAME_BITS) + positions[bit] - 1] = type1[(bit * nframes) + frame] & 1u;
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

/* ---------------------------------------------------------------------------------------------------------------
 * The codec library.
 * ------------------------------------------------------------------------------------------------------------- */

static void
voice_codec_destroy(tetra_voice_t* v) {
    if (v->codec && v->destroy) {
        v->destroy(v->codec);
    }
    v->codec = NULL;
}

void
tetra_voice_release(tetra_voice_t* voice) {
    if (!voice) {
        return;
    }
    voice_codec_destroy(voice);
    dsd_dynlib_close(voice->lib);
    DSD_MEMSET(voice, 0, sizeof(*voice));
}

static void
voice_codec_refuse(tetra_voice_t* v, const char* path, const char* reason) {
    LOG_ERROR("TETRA: codec library %s refused: %s. TETRA speech will not be played.\n", path, reason);
    dsd_dynlib_close(v->lib);
    v->lib = NULL;
    v->create = NULL;
    v->decode = NULL;
    v->destroy = NULL;
    v->codec_status = TETRA_CODEC_STATUS_FAILED;
}

/* Load and check the library @p path names and make its first decoder: TETRA_CODEC_STATUS_READY with v->codec set, or
   TETRA_CODEC_STATUS_FAILED. */
static void
voice_codec_load(tetra_voice_t* v, const char* path) {
    char err[256];
    v->lib = dsd_dynlib_open(path, err, sizeof(err));
    if (!v->lib) {
        voice_codec_refuse(v, path, err);
        return;
    }
    const dsd_tetra_codec_abi_version_fn version =
        (dsd_tetra_codec_abi_version_fn)dsd_dynlib_symbol(v->lib, DSD_TETRA_CODEC_SYM_ABI_VERSION);
    v->create = (dsd_tetra_codec_create_fn)dsd_dynlib_symbol(v->lib, DSD_TETRA_CODEC_SYM_CREATE);
    v->decode = (dsd_tetra_codec_decode_fn)dsd_dynlib_symbol(v->lib, DSD_TETRA_CODEC_SYM_DECODE);
    v->destroy = (dsd_tetra_codec_destroy_fn)dsd_dynlib_symbol(v->lib, DSD_TETRA_CODEC_SYM_DESTROY);
    if (!version || !v->create || !v->decode || !v->destroy) {
        DSD_SNPRINTF(err, sizeof(err), "it does not export all of %s, %s, %s and %s", DSD_TETRA_CODEC_SYM_ABI_VERSION,
                     DSD_TETRA_CODEC_SYM_CREATE, DSD_TETRA_CODEC_SYM_DECODE, DSD_TETRA_CODEC_SYM_DESTROY);
        voice_codec_refuse(v, path, err);
        return;
    }
    const uint32_t abi = version();
    if (abi != DSD_TETRA_CODEC_ABI_VERSION) {
        DSD_SNPRINTF(err, sizeof(err), "it implements codec interface version %u, this dsd-neo needs version %u",
                     (unsigned)abi, (unsigned)DSD_TETRA_CODEC_ABI_VERSION);
        voice_codec_refuse(v, path, err);
        return;
    }
    v->codec = v->create();
    if (!v->codec) {
        voice_codec_refuse(v, path, "it could not create a decoder");
        return;
    }
    v->codec_status = TETRA_CODEC_STATUS_READY;
    LOG_INFO("TETRA: speech codec library %s loaded.\n", path);
}

/* 1 when a decoder instance is ready for the next frame: the library loads at the first frame that needs it, and an
   instance is made after each reset. */
static int
voice_codec_ready(const dsd_opts* opts, tetra_voice_t* v) {
    if (v->codec) {
        return 1;
    }
    if (v->codec_status == TETRA_CODEC_STATUS_UNLOADED) {
        if (!opts || opts->tetra_codec_path[0] == '\0') {
            v->codec_status = TETRA_CODEC_STATUS_NONE;
            LOG_INFO("TETRA: speech received, but no codec library is loaded (--tetra-codec); it is not played.\n");
            return 0;
        }
        voice_codec_load(v, opts->tetra_codec_path);
        return v->codec != NULL;
    }
    if (v->codec_status != TETRA_CODEC_STATUS_READY) {
        return 0;
    }
    v->codec = v->create();
    if (!v->codec) {
        voice_codec_refuse(v, opts ? opts->tetra_codec_path : "", "it could not create a decoder");
        return 0;
    }
    return 1;
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
    short buf[DSD_VOICE_FRAME_SAMPLES + DSD_TETRA_CODEC_FRAME_SAMPLES];
    size_t count = v->carry_len;
    DSD_MEMCPY(buf, v->carry, count * sizeof(short));
    DSD_MEMCPY(buf + count, pcm, (size_t)DSD_TETRA_CODEC_FRAME_SAMPLES * sizeof(short));
    count += DSD_TETRA_CODEC_FRAME_SAMPLES;
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
    if (!voice_codec_ready(opts, v)) {
        return;
    }
    int16_t pcm[DSD_TETRA_CODEC_FRAME_SAMPLES];
    DSD_MEMSET(pcm, 0, sizeof(pcm));
    if (v->decode(v->codec, bits, bad ? 1 : 0, pcm) != 0) {
        if (v->codec_errors++ == 0U) {
            LOG_ERROR("TETRA: the codec library failed to decode a speech frame; failed frames are dropped.\n");
        }
        v->last_decode_failed = 1;
        v->carry_len = 0;
        return;
    }
    v->last_decode_failed = 0;
    v->frames++;
    if (bad) {
        v->concealed++;
    }
    voice_play(opts, state, v, pcm, route->record);
}

/* Forget the speech in progress: the decoder instance (made again at the next frame), its integrity and its unplayed
   samples. */
static void
voice_speech_reset(tetra_voice_t* v) {
    voice_codec_destroy(v);
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
        voice_frame_play(opts, state, &ts->voice, frames + ((size_t)i * DSD_TETRA_CODEC_FRAME_BITS), bad[i], &route);
    }
}

/* Undo the air interface encryption of an air-encrypted trunked call's speech; 1 when a key stream applied. The
   encryption sits above the channel coding (EN 300 392-7 clause 6.1), whose first step orders the speech frame bits
   into sensitivity classes (EN 300 395-2 clause 5.2), so @p frame_bits are speech frame bits B1..B137 in the order the
   codec made them, after voice_reorder(). */
static int
voice_decrypt(const dsd_opts* opts, const dsd_state* state, const tetra_speech* sp, uint8_t* frame_bits, int nbits,
              int kss) {
    return !sp->sealed && sp->air_enc != TETRA_ENC_MODE_NONE
           && tetra_air_decrypt_voice(opts, state, sp->air_enc, frame_bits, nbits, kss);
}

void
tetra_voice_full_slot(dsd_opts* opts, dsd_state* state, const uint8_t* type1, int good) {
    uint8_t frames[2 * DSD_TETRA_CODEC_FRAME_BITS];
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
    uint8_t frames[2 * DSD_TETRA_CODEC_FRAME_BITS];
    tetra_speech sp;
    if (!state || !type1 || !tetra_st(state) || !tetra_voice_gate(opts, state, &sp)) {
        return;
    }
    /* Frame A of the slot was stolen (EN 300 395-2 clause 5.2): the codec conceals it, frame B is the speech. */
    uint8_t* const speech = frames + DSD_TETRA_CODEC_FRAME_BITS;
    DSD_MEMSET(frames, 0, DSD_TETRA_CODEC_FRAME_BITS);
    tetra_voice_reorder_half(type1, speech);
    const int keystream = voice_decrypt(opts, state, &sp, speech, DSD_TETRA_CODEC_FRAME_BITS, TETRA_AIR_KSS_TCH_S_HALF);
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
