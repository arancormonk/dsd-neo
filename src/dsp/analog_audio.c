// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The analog monitor's audio chain (issue #518): voice band-pass, the legacy 960 Hz filters, then a fixed gain or the
 * AGC, with per-state filter and AGC state in DSD_STATE_EXT_DSP_ANALOG_AUDIO.
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/dsp/analog_audio.h>
#include <dsd-neo/dsp/analog_voice.h>
#include <dsd-neo/runtime/analog_channel.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

/* RTL monitor audio: 1 kHz at 3 kHz deviation is 2 pi 3000 / 48000 = 0.3927 rad, 0.125 after the monitor's 1/pi output
   scale, and the AM detector puts 50% at 0.25 x 0.5 = 0.125; the default volume trim doubles it to 0.25. */
#define ANALOG_AUDIO_RTL_MONITOR_REFERENCE 0.25
/* The FSK discriminator output's normalised peak (fsk_modem.c). */
#define ANALOG_AUDIO_RTL_FSK_REFERENCE     30000.0
/* `-n` at which the fixed gain is the source gain. */
#define ANALOG_AUDIO_UNITY_SETTING         50.0

#define ANALOG_AUDIO_CHAINS                2

/* Which reception samples belong to: the RTL stream generation (RTL input), the trunk-tuning generation, and the
   chains' reception epoch, which every boundary dsd_analog_rx_reset() announces moves (dsd_analog_audio_note_reception():
   a scan row commit, the legacy -Y rigctl retune, a reconnect). */
typedef struct {
    uint32_t rtl_generation;
    uint64_t tune_generation;
    uint32_t epoch;
} analog_audio_reception;

typedef struct {
    int configured;
    int band;
    int source;
    int rate_hz;
    /* The reception the chain runs on, and the one the block being collected began in. */
    analog_audio_reception reception;
    int block_noted;
    analog_audio_reception block_reception;
    dsd_voice_bandpass bandpass;
    dsd_voice_agc agc;
    /* The legacy -v 0x2 / 0x4 filters, kept per chain at the chain's rate so they start over with it instead of carrying
       another source's or channel's history. */
    dsd_voice_onepole legacy;
} analog_audio_chain_state;

typedef struct {
    uint32_t epoch;
    analog_audio_chain_state chain[ANALOG_AUDIO_CHAINS];
} analog_audio_ext;

double
dsd_analog_audio_source_gain(dsd_analog_audio_source source) {
    switch (source) {
        case DSD_ANALOG_AUDIO_SOURCE_RTL_MONITOR:
            return DSD_ANALOG_AUDIO_REFERENCE_PEAK / ANALOG_AUDIO_RTL_MONITOR_REFERENCE;
        case DSD_ANALOG_AUDIO_SOURCE_RTL_FSK: return DSD_ANALOG_AUDIO_REFERENCE_PEAK / ANALOG_AUDIO_RTL_FSK_REFERENCE;
        /* PCM: the reference signal is one already at the reference peak. */
        case DSD_ANALOG_AUDIO_SOURCE_PCM16:
        default: return 1.0;
    }
}

static analog_audio_ext*
analog_audio_ext_get(dsd_state* state) {
    analog_audio_ext* ext = DSD_STATE_EXT_GET_AS(analog_audio_ext, state, DSD_STATE_EXT_DSP_ANALOG_AUDIO);
    if (ext) {
        return ext;
    }
    ext = (analog_audio_ext*)calloc(1, sizeof(*ext));
    if (!ext) {
        return NULL;
    }
    if (dsd_state_ext_set(state, DSD_STATE_EXT_DSP_ANALOG_AUDIO, ext, free) != 0) {
        free(ext);
        return NULL;
    }
    return ext;
}

/* The fixed gain for the `-n` setting @p setting (1..100) on @p source. */
static double
analog_audio_fixed_gain(dsd_analog_audio_source source, float setting) {
    return dsd_analog_audio_source_gain(source) * ((double)setting / ANALOG_AUDIO_UNITY_SETTING);
}

static void
analog_audio_apply_gain(float* buf, size_t n, double gain) {
    for (size_t i = 0; i < n; i++) {
        buf[i] = (float)((double)buf[i] * gain);
    }
}

/* The AM band for the monitor under AM, whether the RTL detector or a receiver ahead of PCM input demodulated it; the
   FM band (which also rejects CTCSS and DCS) for everything else, EDACS and the FSK discriminator output included. */
static dsd_voice_band_kind
analog_audio_band(const dsd_opts* opts, dsd_analog_audio_chain chain, dsd_analog_audio_source source) {
    return (chain == DSD_ANALOG_AUDIO_CHAIN_MONITOR && source != DSD_ANALOG_AUDIO_SOURCE_RTL_FSK
            && opts->analog_demod == DSD_ANALOG_DEMOD_AM)
               ? DSD_VOICE_BAND_AM
               : DSD_VOICE_BAND_FM;
}

static int
analog_audio_reception_equal(const analog_audio_reception* a, const analog_audio_reception* b) {
    return a->rtl_generation == b->rtl_generation && a->tune_generation == b->tune_generation && a->epoch == b->epoch;
}

static analog_audio_reception
analog_audio_reception_now(const dsd_opts* opts, const analog_audio_ext* ext) {
    analog_audio_reception r;
    r.rtl_generation = opts->audio_in_type == AUDIO_IN_RTL ? dsd_rtl_stream_metrics_hook_stream_generation() : 0U;
    r.tune_generation = dsd_trunk_tuning_generation();
    r.epoch = ext->epoch;
    return r;
}

static void
analog_audio_chain_configure(analog_audio_chain_state* cs, dsd_voice_band_kind band, dsd_analog_audio_source source,
                             int rate_hz, const analog_audio_reception* reception) {
    if (cs->configured && cs->band == (int)band && cs->source == (int)source && cs->rate_hz == rate_hz
        && analog_audio_reception_equal(&cs->reception, reception)) {
        return;
    }
    /* A rate of 0 or less designs a pass-through band-pass; the AGC and the legacy filters count at 48 kHz, as the
       monitor's fallback does. */
    const int run_rate_hz = rate_hz > 0 ? rate_hz : 48000;
    (void)dsd_voice_bandpass_design(&cs->bandpass, band, rate_hz);
    (void)dsd_voice_agc_init(&cs->agc, run_rate_hz, dsd_analog_audio_source_gain(source));
    (void)dsd_voice_onepole_design(&cs->legacy, run_rate_hz);
    cs->configured = 1;
    cs->band = (int)band;
    cs->source = (int)source;
    cs->rate_hz = rate_hz;
    cs->reception = *reception;
}

/* The voice band-pass and the legacy 960 Hz filters, each as its flag asks. */
static void
analog_audio_run_filters(const dsd_opts* opts, analog_audio_chain_state* cs, float* buf, size_t n) {
    if (opts->use_pbf == 1) {
        dsd_voice_bandpass_process(&cs->bandpass, buf, n);
    }
    if (opts->use_lpf == 1) {
        dsd_voice_onepole_lowpass(&cs->legacy, buf, n);
    }
    if (opts->use_hpf == 1) {
        dsd_voice_onepole_highpass(&cs->legacy, buf, n);
    }
}

/* Drop @p buf: silence out, and the chain starts over with the samples after it. */
static void
analog_audio_drop(analog_audio_chain_state* cs, float* buf, size_t n) {
    for (size_t i = 0; i < n; i++) {
        buf[i] = 0.0f;
    }
    if (cs) {
        cs->configured = 0;
    }
}

void
dsd_analog_audio_note_reception(const dsd_state* state) {
    analog_audio_ext* ext =
        state ? DSD_STATE_EXT_GET_AS(analog_audio_ext, state, DSD_STATE_EXT_DSP_ANALOG_AUDIO) : NULL;
    if (ext) {
        ext->epoch++;
    }
}

void
dsd_analog_audio_block_begin(const dsd_opts* opts, dsd_state* state, dsd_analog_audio_chain chain) {
    if (!opts || !state || (int)chain < 0 || (int)chain >= ANALOG_AUDIO_CHAINS) {
        return;
    }
    analog_audio_ext* ext = analog_audio_ext_get(state);
    if (!ext) {
        return;
    }
    analog_audio_chain_state* cs = &ext->chain[chain];
    cs->block_reception = analog_audio_reception_now(opts, ext);
    cs->block_noted = 1;
}

/* The gain stage: the AGC on auto, else the fixed gain; publishes what it applied over the -n 50 gain. */
static void
analog_audio_run_gain(const dsd_opts* opts, dsd_state* state, analog_audio_chain_state* cs, float* buf, size_t n,
                      dsd_analog_audio_source source, unsigned int flags) {
    if (dsd_analog_gain_is_auto(opts->audio_gainA)) {
        dsd_voice_agc_process(&cs->agc, buf, n, (flags & DSD_ANALOG_AUDIO_PLAYING) ? 1 : 0);
        state->aout_gainA = (float)(20.0 * log10(dsd_voice_agc_gain(&cs->agc) / dsd_analog_audio_source_gain(source)));
    } else {
        analog_audio_apply_gain(buf, n, analog_audio_fixed_gain(source, opts->audio_gainA));
        state->aout_gainA = (float)(20.0 * log10((double)opts->audio_gainA / ANALOG_AUDIO_UNITY_SETTING));
    }
}

int
dsd_analog_audio_process_f(const dsd_opts* opts, dsd_state* state, dsd_analog_audio_chain chain, float* buf, size_t n,
                           dsd_analog_audio_source source, int rate_hz, unsigned int flags) {
    if (!opts || !state || !buf || (int)chain < 0 || (int)chain >= ANALOG_AUDIO_CHAINS) {
        return -1;
    }
    if (n == 0U) {
        return 0;
    }
    analog_audio_ext* ext = analog_audio_ext_get(state);
    if ((flags & DSD_ANALOG_AUDIO_DISCARD) != 0U) {
        analog_audio_drop(ext ? &ext->chain[chain] : NULL, buf, n);
        return 0;
    }
    if (!ext) {
        /* No state for the filters or the AGC: the fixed gain, at the reference setting when the gain is on auto. */
        const float setting =
            dsd_analog_gain_is_auto(opts->audio_gainA) ? (float)ANALOG_AUDIO_UNITY_SETTING : opts->audio_gainA;
        analog_audio_apply_gain(buf, n, analog_audio_fixed_gain(source, setting));
        return 0;
    }
    analog_audio_chain_state* cs = &ext->chain[chain];
    const analog_audio_reception now = analog_audio_reception_now(opts, ext);
    /* Samples collected across a boundary are partly the reception before it: dropped. Samples collected wholly after
       one start the chain over on the new reception (analog_audio_chain_configure()). */
    if (cs->block_noted && !analog_audio_reception_equal(&cs->block_reception, &now)) {
        analog_audio_drop(cs, buf, n);
        return 0;
    }
    if ((flags & DSD_ANALOG_AUDIO_RESET) != 0U) {
        cs->configured = 0;
    }
    analog_audio_chain_configure(cs, analog_audio_band(opts, chain, source), source, rate_hz, &now);
    analog_audio_run_filters(opts, cs, buf, n);
    analog_audio_run_gain(opts, state, cs, buf, n, source, flags);
    return 0;
}

int
dsd_analog_audio_process_s(const dsd_opts* opts, dsd_state* state, dsd_analog_audio_chain chain, short* buf, size_t n,
                           dsd_analog_audio_source source, int rate_hz, unsigned int flags) {
    if (!opts || !state || !buf) {
        return -1;
    }
    float tmp[1024];
    size_t done = 0;
    while (done < n) {
        const size_t len = (n - done) < (sizeof tmp / sizeof tmp[0]) ? (n - done) : (sizeof tmp / sizeof tmp[0]);
        for (size_t i = 0; i < len; i++) {
            tmp[i] = (float)buf[done + i];
        }
        /* RESET applies once, to the first piece. */
        const unsigned int piece_flags = done == 0U ? flags : (flags & ~DSD_ANALOG_AUDIO_RESET);
        if (dsd_analog_audio_process_f(opts, state, chain, tmp, len, source, rate_hz, piece_flags) != 0) {
            return -1;
        }
        for (size_t i = 0; i < len; i++) {
            const float v = tmp[i];
            if (isnan(v)) {
                buf[done + i] = 0;
            } else if (v >= 32767.0f) {
                buf[done + i] = 32767;
            } else if (v <= -32768.0f) {
                buf[done + i] = -32768;
            } else {
                buf[done + i] = (short)lrintf(v);
            }
        }
        done += len;
    }
    return 0;
}
