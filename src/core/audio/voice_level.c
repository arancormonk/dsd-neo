// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The digital voice chain's speech level (issue #518): one speech AGC (<dsd-neo/dsp/speech_agc.h>) per slot at the
 * voice rate, with the reception it runs for, the talkers both slots remember, the M17 encoder monitor's fixed-unity
 * one and the DMR superframe mixer's one-channel limiter, in DSD_STATE_EXT_CORE_VOICE_LEVEL. Decoder thread only, like
 * the chain.
 */

#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/voice_chain.h>
#include <dsd-neo/dsp/speech_agc.h>
#include <stdint.h>
#include <stdlib.h>

#include "dsd-neo/core/opts.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "voice_chain_internal.h"

/* The talkers the slots remember: the gain a radio's speech settled at predicts its next transmission about twice as
   well as any fixed opening gain. */
#define VOICE_LEVEL_TALKERS 64

/* A remembered talker: a radio ID within a protocol family, the gain its speech settled at, and when the entry was last
   stored or applied (a count; 0 for an unused entry). */
typedef struct {
    uint64_t source;
    uint64_t used;
    float gain_db;
    int family;
} voice_level_talker;

typedef struct {
    dsd_speech_agc agc;
    /* The reception the AGC runs for: the transmission, the radio it comes from, the protocol family and the producer's
       talkspurt, each 0 until a frame names one. */
    uint64_t transmission;
    uint64_t source;
    uint32_t talkspurt;
    int family;
    /* The AGC was initialised or reset: the slot's next frame opens a reception. */
    int opening;
} voice_level_slot;

typedef struct {
    voice_level_slot slot[2];
    /* The M17 encoder's monitor: unity gain through the limiter, nothing learned (DSD_VOICE_MEDIA_TX_MONITOR). */
    dsd_speech_agc tx_monitor;
    /* The DMR superframe mixer's one-channel limiter: two slots' talkers summed stay under the ceiling. */
    dsd_peak_limiter dmr_mix;
    /* Both slots' talkers. A carrier boundary resets the slots, which stores what they learned, and keeps these. */
    voice_level_talker talkers[VOICE_LEVEL_TALKERS];
    uint64_t talker_clock;
} voice_level_ext;

static voice_level_ext*
voice_level_ext_get(dsd_state* state) {
    voice_level_ext* ext = DSD_STATE_EXT_GET_AS(voice_level_ext, state, DSD_STATE_EXT_CORE_VOICE_LEVEL);
    if (ext) {
        return ext;
    }
    ext = (voice_level_ext*)calloc(1, sizeof(*ext));
    if (!ext) {
        return NULL;
    }
    for (int s = 0; s < 3; s++) {
        dsd_speech_agc* agc = s < 2 ? &ext->slot[s].agc : &ext->tx_monitor;
        if (dsd_speech_agc_init(agc, (int)DSD_VOICE_HPF_RATE_HZ, DSD_VOICE_LEVEL_START_GAIN_DB,
                                DSD_SPEECH_AGC_TARGET_DBFS, DSD_SPEECH_AGC_CEILING_DBFS)
            != 0) {
            free(ext);
            return NULL;
        }
    }
    /* Every transmission is a new reception, many of them over before the AGC's slow startup arrives. */
    for (int s = 0; s < 2; s++) {
        dsd_speech_agc_set_acquisition(&ext->slot[s].agc, 1);
        ext->slot[s].opening = 1;
    }
    if (dsd_peak_limiter_init(&ext->dmr_mix, (int)DSD_VOICE_HPF_RATE_HZ, DSD_SPEECH_AGC_CEILING_DBFS) != 0) {
        free(ext);
        return NULL;
    }
    dsd_speech_agc_set_fixed(&ext->tx_monitor, 1.0f);
    if (dsd_state_ext_set(state, DSD_STATE_EXT_CORE_VOICE_LEVEL, ext, free) != 0) {
        free(ext);
        return NULL;
    }
    return ext;
}

/* The remembered talker @p source of protocol family @p family; NULL when not remembered. */
static voice_level_talker*
voice_level_talker_find(voice_level_ext* ext, int family, uint64_t source) {
    for (int i = 0; i < VOICE_LEVEL_TALKERS; i++) {
        voice_level_talker* t = &ext->talkers[i];
        if (t->used != 0U && t->source == source && t->family == family) {
            return t;
        }
    }
    return NULL;
}

/* The entry a new talker takes: an unused one, else the one least recently stored or applied. */
static voice_level_talker*
voice_level_talker_new_entry(voice_level_ext* ext) {
    voice_level_talker* oldest = &ext->talkers[0];
    for (int i = 0; i < VOICE_LEVEL_TALKERS; i++) {
        voice_level_talker* t = &ext->talkers[i];
        if (t->used == 0U) {
            return t;
        }
        if (t->used < oldest->used) {
            oldest = t;
        }
    }
    return oldest;
}

/* A remembered gain @p gain_db moved half way to a newly settled one. */
static float
voice_level_half_way(float gain_db, float settled_db) {
    return gain_db + 0.5f * (settled_db - gain_db);
}

/* The slot's reception is over: remember the gain its talker's speech settled at, when it named its source and its
   estimate was established. A talker heard before moves half way to it. */
static void
voice_level_remember(voice_level_ext* ext, const voice_level_slot* s) {
    float settled = 0.0f;
    if (s->source == 0U || !dsd_speech_agc_settled_gain_db(&s->agc, &settled)) {
        return;
    }
    voice_level_talker* t = voice_level_talker_find(ext, s->family, s->source);
    if (t) {
        t->gain_db = voice_level_half_way(t->gain_db, settled);
    } else {
        t = voice_level_talker_new_entry(ext);
        t->source = s->source;
        t->family = s->family;
        t->gain_db = settled;
    }
    t->used = ++ext->talker_clock;
}

/* A new reception: remember what the slot learned, then start its AGC over. */
static void
voice_level_slot_reset(voice_level_ext* ext, voice_level_slot* s) {
    voice_level_remember(ext, s);
    dsd_speech_agc_reset(&s->agc);
    s->transmission = 0U;
    s->source = 0U;
    s->talkspurt = 0U;
    s->family = 0;
    s->opening = 1;
}

void
dsd_voice_level_reset(dsd_state* state, int slot) {
    if (!state || slot > 1) {
        return;
    }
    /* A slot that never ran a frame has nothing to forget. */
    voice_level_ext* ext = DSD_STATE_EXT_GET_AS(voice_level_ext, state, DSD_STATE_EXT_CORE_VOICE_LEVEL);
    if (!ext) {
        return;
    }
    for (int s = 0; s < 2; s++) {
        if (slot < 0 || slot == s) {
            voice_level_slot_reset(ext, &ext->slot[s]);
            /* The readout shows the gain the slot's next frame opens at. */
            *(s == 1 ? &state->aout_gainR : &state->aout_gain) = DSD_VOICE_LEVEL_OPEN_GAIN_DB;
        }
    }
    /* The DMR mixer's limiter goes with both slots: the next talkers are another reception's. */
    if (slot < 0) {
        dsd_peak_limiter_reset(&ext->dmr_mix);
    }
}

dsd_peak_limiter*
dsd_voice_level_dmr_mix_limiter(dsd_state* state) {
    voice_level_ext* ext = state ? voice_level_ext_get(state) : NULL;
    return ext ? &ext->dmr_mix : NULL;
}

const dsd_speech_agc*
dsd_voice_level_agc(const dsd_state* state, int slot) {
    if (!state || slot < 0 || slot > 1) {
        return NULL;
    }
    const voice_level_ext* ext = (const voice_level_ext*)dsd_state_ext_get_const(state, DSD_STATE_EXT_CORE_VOICE_LEVEL);
    return ext ? &ext->slot[slot].agc : NULL;
}

/* A new reception starts the AGC over: another transmission, protocol family or talkspurt, each judged only between
   two values a frame named, so a frame that names none (a call not known yet, a producer with no talkspurt) never
   cuts one. */
static void
voice_level_follow(voice_level_ext* ext, voice_level_slot* s, const dsd_voice_reception* reception,
                   uint32_t talkspurt) {
    const int new_transmission =
        reception->transmission != 0U && s->transmission != 0U && reception->transmission != s->transmission;
    const int new_family = reception->family != 0 && s->family != 0 && reception->family != s->family;
    const int new_talkspurt = talkspurt != 0U && s->talkspurt != 0U && talkspurt != s->talkspurt;
    if (new_transmission || new_family || new_talkspurt) {
        voice_level_slot_reset(ext, s);
    }
    if (reception->transmission != 0U) {
        s->transmission = reception->transmission;
    }
    if (reception->family != 0) {
        s->family = reception->family;
    }
    if (talkspurt != 0U) {
        s->talkspurt = talkspurt;
    }
}

/* The gain the memory will hold for slot @p s's talker (@p t, NULL when not remembered) once the other slot's
   reception in progress is stored, when that reception names the same source in the same protocol family and its
   estimate is established: a radio whose transmission on one slot ended and whose next one starts on the other, with
   no carrier boundary between (two-slot TDMA on one carrier), still has its settled gain unstored there. Nothing is
   stored here: the other slot stores it once, at its own reset. Returns 0 when that reception does not count. */
static int
voice_level_other_slot_gain(const voice_level_ext* ext, const voice_level_slot* s, const voice_level_talker* t,
                            float* gain_db) {
    const voice_level_slot* o = s == &ext->slot[0] ? &ext->slot[1] : &ext->slot[0];
    float settled = 0.0f;
    if (o->source != s->source || o->family != s->family || !dsd_speech_agc_settled_gain_db(&o->agc, &settled)) {
        return 0;
    }
    *gain_db = t ? voice_level_half_way(t->gain_db, settled) : settled;
    return 1;
}

/* The reception's source: the first frame that names one opens the reception, if nothing has committed yet, at the
   gain that radio's speech settled at: as the memory holds it, or as it will hold it once the other slot's reception
   from the same radio is stored. On the reception's first frame that is at once; later (a P25 Phase 1 source arrives
   with the first link control, about 180 ms in) it eases in over one frame. The slot's own reception was stored at its
   reset, so only the other slot's is read. */
static void
voice_level_recall(voice_level_ext* ext, voice_level_slot* s, uint64_t source, int first_frame) {
    if (source == 0U) {
        return;
    }
    const int first_naming = s->source == 0U;
    s->source = source;
    if (!first_naming || s->agc.learned.commits != 0) {
        return;
    }
    voice_level_talker* t = voice_level_talker_find(ext, s->family, source);
    float gain_db = t ? t->gain_db : 0.0f;
    if (!voice_level_other_slot_gain(ext, s, t, &gain_db) && !t) {
        return;
    }
    dsd_speech_agc_open_at(&s->agc, gain_db, first_frame ? 0 : 1);
    if (t) {
        t->used = ++ext->talker_clock;
    }
}

/* The encoder's own transmission plays at the level it went out at, whatever -g says, and the slot's AGC neither learns
   from it nor starts over for it. */
static void
voice_level_run_tx_monitor(dsd_state* state, float* frame, float* shown) {
    voice_level_ext* ext = voice_level_ext_get(state);
    if (!ext) {
        /* Unity, without the limiter: no memory for it. */
        *shown = 0.0f;
        return;
    }
    dsd_speech_agc_process(&ext->tx_monitor, frame, DSD_VOICE_FRAME_SAMPLES, 0, 1, 0);
    *shown = dsd_speech_agc_gain_db(&ext->tx_monitor);
}

void
dsd_voice_level_run(const dsd_opts* opts, dsd_state* state, int slot, float* frame, const dsd_voice_frame_info* info,
                    const dsd_voice_reception* reception) {
    float* shown = slot == 1 ? &state->aout_gainR : &state->aout_gain;
    if (info && info->media == (uint8_t)DSD_VOICE_MEDIA_TX_MONITOR) {
        voice_level_run_tx_monitor(state, frame, shown);
        return;
    }
    const float setting = slot == 1 ? opts->audio_gainR : opts->audio_gain;
    voice_level_ext* ext = setting < 0.0f ? NULL : voice_level_ext_get(state);
    if (!ext) {
        /* Unity: the pass-through below 0, or no memory for the AGC. */
        *shown = 0.0f;
        return;
    }
    voice_level_slot* s = &ext->slot[slot];
    voice_level_follow(ext, s, reception, info ? info->talkspurt : 0U);
    const int first_frame = s->opening;
    if (first_frame) {
        /* The reception's first frame: it opens at the opening gain from its first sample. */
        dsd_speech_agc_open_at(&s->agc, DSD_VOICE_LEVEL_OPEN_GAIN_DB, 0);
        s->opening = 0;
    }
    voice_level_recall(ext, s, reception->source, first_frame);
    dsd_speech_agc_set_fixed(&s->agc, setting > 0.0f ? setting : 0.0f);
    const int learn = dsd_voice_frame_may_learn(info);
    const int decodable = info ? info->decodable != 0U : 1;
    const int tone = info && info->media == (uint8_t)DSD_VOICE_MEDIA_TONE;
    dsd_speech_agc_process(&s->agc, frame, DSD_VOICE_FRAME_SAMPLES, learn, decodable, tone);
    *shown = dsd_speech_agc_gain_db(&s->agc);
}
