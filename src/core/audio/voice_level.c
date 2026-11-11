// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The digital voice chain's speech level (issue #518): one speech AGC (<dsd-neo/dsp/speech_agc.h>) per slot at the
 * voice rate, with the reception it runs for, in DSD_STATE_EXT_CORE_VOICE_LEVEL. Decoder thread only, like the chain.
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

/* Digital voice starts at the decoder's own level: the `-g 1` reference, which DVSI's decoder plays. */
#define VOICE_LEVEL_START_GAIN_DB 0.0f

typedef struct {
    dsd_speech_agc agc;
    /* The reception the AGC runs for: the transmission, the protocol family and the producer's talkspurt, each 0 until
       a frame names one. */
    uint64_t transmission;
    uint32_t talkspurt;
    int family;
} voice_level_slot;

typedef struct {
    voice_level_slot slot[2];
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
    for (int s = 0; s < 2; s++) {
        if (dsd_speech_agc_init(&ext->slot[s].agc, (int)DSD_VOICE_HPF_RATE_HZ, VOICE_LEVEL_START_GAIN_DB,
                                DSD_SPEECH_AGC_TARGET_DBFS, DSD_SPEECH_AGC_CEILING_DBFS)
            != 0) {
            free(ext);
            return NULL;
        }
    }
    if (dsd_state_ext_set(state, DSD_STATE_EXT_CORE_VOICE_LEVEL, ext, free) != 0) {
        free(ext);
        return NULL;
    }
    return ext;
}

static void
voice_level_slot_reset(voice_level_slot* s) {
    dsd_speech_agc_reset(&s->agc);
    s->transmission = 0U;
    s->talkspurt = 0U;
    s->family = 0;
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
            voice_level_slot_reset(&ext->slot[s]);
            /* The readout shows the start gain until the slot's next frame. */
            *(s == 1 ? &state->aout_gainR : &state->aout_gain) = VOICE_LEVEL_START_GAIN_DB;
        }
    }
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
voice_level_follow(voice_level_slot* s, const dsd_voice_reception* reception, uint32_t talkspurt) {
    const int new_transmission =
        reception->transmission != 0U && s->transmission != 0U && reception->transmission != s->transmission;
    const int new_family = reception->family != 0 && s->family != 0 && reception->family != s->family;
    const int new_talkspurt = talkspurt != 0U && s->talkspurt != 0U && talkspurt != s->talkspurt;
    if (new_transmission || new_family || new_talkspurt) {
        voice_level_slot_reset(s);
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

void
dsd_voice_level_run(const dsd_opts* opts, dsd_state* state, int slot, float* frame, const dsd_voice_frame_info* info,
                    const dsd_voice_reception* reception) {
    float* shown = slot == 1 ? &state->aout_gainR : &state->aout_gain;
    const float setting = slot == 1 ? opts->audio_gainR : opts->audio_gain;
    voice_level_ext* ext = setting < 0.0f ? NULL : voice_level_ext_get(state);
    if (!ext) {
        /* Unity: the pass-through below 0, or no memory for the AGC. */
        *shown = 0.0f;
        return;
    }
    voice_level_slot* s = &ext->slot[slot];
    voice_level_follow(s, reception, info ? info->talkspurt : 0U);
    dsd_speech_agc_set_fixed(&s->agc, setting > 0.0f ? setting : 0.0f);
    const int learn = dsd_voice_frame_may_learn(info);
    const int decodable = info ? info->decodable != 0U : 1;
    const int tone = info && info->media == (uint8_t)DSD_VOICE_MEDIA_TONE;
    dsd_speech_agc_process(&s->agc, frame, DSD_VOICE_FRAME_SAMPLES, learn, decodable, tone);
    *shown = dsd_speech_agc_gain_db(&s->agc);
}
