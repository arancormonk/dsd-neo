// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The analog monitor's audio chain (issue #518): voice band-pass, the legacy 960 Hz filters, then the source gain and
 * the speech AGC, fixed or automatic, with per-state filter and AGC state in DSD_STATE_EXT_DSP_ANALOG_AUDIO.
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/dsp/analog_audio.h>
#include <dsd-neo/dsp/analog_voice.h>
#include <dsd-neo/dsp/fsk_modem.h>
#include <dsd-neo/dsp/speech_agc.h>
#include <dsd-neo/runtime/analog_channel.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/log.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

/* The reference signal's peak in the FSK discriminator output: 3 kHz deviation on its fixed scale (fsk_modem.h), the
   deviation DSD_ANALOG_AUDIO_RTL_MONITOR_REFERENCE takes for the monitor audio (which the -6 raw WAV's scale uses
   too). */
#define ANALOG_AUDIO_RTL_FSK_REFERENCE (3000.0 * (double)DSD_FSK_DISCRIMINATOR_COUNTS_PER_HZ)
/* `-n` at which the fixed gain is the source gain. */
#define ANALOG_AUDIO_UNITY_SETTING     50.0

#define ANALOG_AUDIO_CHAINS            2

/* Which reception samples belong to: the RTL stream generation and the de-emphasis it runs (RTL input), the
   trunk-tuning generation, and the chains' reception epoch, which every boundary dsd_analog_rx_reset() announces moves
   (dsd_analog_audio_note_reception(): a scan row commit, the legacy -Y rigctl retune, a reconnect). */
typedef struct {
    uint32_t rtl_generation;
    /* The gain that makes up for the RTL monitor audio's de-emphasis at 1 kHz; 1.0 off RTL input. */
    double deemph_gain;
    uint64_t tune_generation;
    uint32_t epoch;
} analog_audio_reception;

typedef struct {
    int configured;
    int band;
    int source;
    int rate_hz;
    /* The gain that takes the source's reference signal to the reference level on this reception
       (analog_audio_chain_gain()): the gain at -n 50, which the AGC's gains are over. */
    double source_gain;
    /* The reception the chain runs on, and the one the block being collected began in. */
    analog_audio_reception reception;
    int block_noted;
    analog_audio_reception block_reception;
    dsd_voice_bandpass bandpass;
    dsd_speech_agc agc;
    /* The AGC runs at this chain's rate; at a rate it does not support, the fixed gain plays through the limiter. */
    int agc_ok;
    int agc_rate_hz;
    dsd_peak_limiter limiter;
    /* The last unsupported rate auto was asked for at (analog_audio_log_unsupported_rate()). */
    int unsupported_rate_logged;
    /* The new-reception rule (dsd_analog_audio_process_runs_f()): whether a sample learned since the AGC last started
       over, the decode-clock time of the last one, the samples processed after it, and the last sample's flag. */
    int learned;
    uint64_t last_learn_ns;
    uint64_t since_learn;
    int learn_prev;
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
            return DSD_ANALOG_AUDIO_REFERENCE_PEAK / DSD_ANALOG_AUDIO_RTL_MONITOR_REFERENCE;
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

/* The gain that takes @p source's reference signal to the reference level: the source gain, and on RTL monitor audio
   the de-emphasis made up at 1 kHz, so the reference tone plays at the reference level whatever DSD_NEO_DEEMPH selects
   (issue #518). The -6 raw WAV and the M17 encoder's input keep the source gain alone. */
static double
analog_audio_chain_gain(dsd_analog_audio_source source, double deemph_gain) {
    const double gain = dsd_analog_audio_source_gain(source);
    return source == DSD_ANALOG_AUDIO_SOURCE_RTL_MONITOR ? gain * deemph_gain : gain;
}

/* The fixed gain for the `-n` setting @p setting (1..100) over @p source_gain, the chain's gain at -n 50. */
static double
analog_audio_fixed_gain(double source_gain, float setting) {
    return source_gain * ((double)setting / ANALOG_AUDIO_UNITY_SETTING);
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

/* The AGC's start gain over -n 50 (issue #518, D6/D7): the band's, FM narrowband up to the 16 kHz channel width and
   wideband above it; on RTL monitor audio less the `vol` trim over its default, which scales the samples ahead of the
   chain (a trim of 0 plays silence and moves nothing). */
static float
analog_audio_start_gain_db(const dsd_opts* opts, dsd_voice_band_kind band, dsd_analog_audio_source source) {
    float db = DSD_ANALOG_AUDIO_START_GAIN_AM_DB;
    if (band != DSD_VOICE_BAND_AM) {
        const int width_hz = dsd_analog_width_effective_hz(DSD_ANALOG_DEMOD_FM, opts->analog_nfm_bandwidth_hz);
        db = width_hz <= DSD_ANALOG_AUDIO_FM_NARROW_MAX_WIDTH_HZ ? DSD_ANALOG_AUDIO_START_GAIN_FM_NARROW_DB
                                                                 : DSD_ANALOG_AUDIO_START_GAIN_FM_WIDE_DB;
    }
    if (source == DSD_ANALOG_AUDIO_SOURCE_RTL_MONITOR) {
        /* 20 log10(vol / 2) for the trims 1 and 3; 2, the default, and 0 move nothing. */
        static const float k_trim_db[] = {0.0f, -6.0206f, 0.0f, 3.5218f};
        const int vol = opts->rtl_volume_multiplier;
        if (vol >= 0 && vol < (int)(sizeof k_trim_db / sizeof k_trim_db[0])) {
            db -= k_trim_db[vol];
        }
    }
    return db;
}

/* Two de-emphasis gains this close are the same one: the stream publishes it as a float. */
static const double k_deemph_gain_tolerance = 1e-6;

static int
analog_audio_reception_equal(const analog_audio_reception* a, const analog_audio_reception* b) {
    return a->rtl_generation == b->rtl_generation && fabs(a->deemph_gain - b->deemph_gain) <= k_deemph_gain_tolerance
           && a->tune_generation == b->tune_generation && a->epoch == b->epoch;
}

/* The RTL monitor audio's de-emphasis gain on an RTL input, 1.0 on any other. */
static double
analog_audio_deemph_gain(const dsd_opts* opts) {
    return opts->audio_in_type == AUDIO_IN_RTL ? dsd_rtl_stream_metrics_hook_monitor_deemph_gain() : 1.0;
}

static analog_audio_reception
analog_audio_reception_now(const dsd_opts* opts, const analog_audio_ext* ext) {
    analog_audio_reception r;
    /* The gain before the generation: the stream moves the generation before it publishes a new gain, so a new gain is
       never read with the old generation. A block noted with the old gain and the new generation is dropped. */
    r.deemph_gain = analog_audio_deemph_gain(opts);
    r.rtl_generation = opts->audio_in_type == AUDIO_IN_RTL ? dsd_rtl_stream_metrics_hook_stream_generation() : 0U;
    r.tune_generation = dsd_trunk_tuning_generation();
    r.epoch = ext->epoch;
    return r;
}

/* The AGC at the chain's rate, or, at a rate it does not support, the limiter alone. The AGC runs with the coloured-noise
   veto: open-squelch FM hiss after heavy de-emphasis (DSD_NEO_DEEMPH=nfm) would otherwise pass its pitch cue often
   enough to keep the hiss from easing down. */
static void
analog_audio_agc_configure(analog_audio_chain_state* cs, int run_rate_hz, float start_gain_db) {
    cs->agc_ok = dsd_speech_agc_init(&cs->agc, run_rate_hz, start_gain_db, DSD_SPEECH_AGC_TARGET_DBFS,
                                     DSD_SPEECH_AGC_CEILING_DBFS)
                 == 0;
    if (cs->agc_ok) {
        dsd_speech_agc_set_noise_veto(&cs->agc, 1);
    }
    cs->agc_rate_hz = run_rate_hz;
    cs->learned = 0;
    cs->since_learn = 0U;
    cs->last_learn_ns = 0U;
    cs->learn_prev = -1;
    if (!cs->agc_ok) {
        (void)dsd_peak_limiter_init(&cs->limiter, run_rate_hz, DSD_SPEECH_AGC_CEILING_DBFS);
    }
}

/* Auto asked for at a rate the AGC does not support: say so, once per rate. A fixed -n plays as asked and says
   nothing. */
static void
analog_audio_log_unsupported_rate(analog_audio_chain_state* cs) {
    if (cs->unsupported_rate_logged == cs->agc_rate_hz) {
        return;
    }
    cs->unsupported_rate_logged = cs->agc_rate_hz;
    if (cs->agc_rate_hz < DSD_SPEECH_AGC_MIN_RATE_HZ) {
        LOG_WARN("Automatic analog gain inactive: %d Hz audio is below %d Hz; playing at the -n 50 level\n",
                 cs->agc_rate_hz, DSD_SPEECH_AGC_MIN_RATE_HZ);
    } else {
        LOG_WARN("Automatic analog gain inactive: %d Hz audio is above %d Hz; playing at the -n 50 level\n",
                 cs->agc_rate_hz, DSD_SPEECH_AGC_MAX_RATE_HZ);
    }
}

static void
analog_audio_chain_configure(analog_audio_chain_state* cs, dsd_voice_band_kind band, dsd_analog_audio_source source,
                             int rate_hz, const analog_audio_reception* reception, float start_gain_db) {
    if (cs->configured && cs->band == (int)band && cs->source == (int)source && cs->rate_hz == rate_hz
        && analog_audio_reception_equal(&cs->reception, reception)) {
        return;
    }
    /* A rate of 0 or less designs a pass-through band-pass; the AGC and the legacy filters count at 48 kHz, as the
       monitor's fallback does. */
    const int run_rate_hz = rate_hz > 0 ? rate_hz : 48000;
    cs->source_gain = analog_audio_chain_gain(source, reception->deemph_gain);
    (void)dsd_voice_bandpass_design(&cs->bandpass, band, rate_hz);
    analog_audio_agc_configure(cs, run_rate_hz, start_gain_db);
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

/* The decode-clock time of sample @p i of @p n whose last sample is at @p end_ns: its distance in samples before it. */
static uint64_t
analog_audio_sample_ns(uint64_t end_ns, size_t n, size_t i, int rate_hz) {
    const uint64_t back = ((uint64_t)(n - 1U - i) * 1000000000ULL) / (uint64_t)rate_hz;
    return end_ns > back ? end_ns - back : 0U;
}

/* Whether learning sample @p i of the @p n in a call whose last sample is at @p end_ns starts a new reception: a second
   or more since the last sample that learned (sample @p last of this call when @p have, else one before it), by the
   samples processed in between or by the clock. */
static int
analog_audio_new_reception(const analog_audio_chain_state* cs, int have, size_t last, size_t i, size_t n,
                           uint64_t end_ns) {
    const uint64_t rate_hz = (uint64_t)cs->agc.rate_hz;
    if (have) {
        return (uint64_t)(i - last) >= rate_hz;
    }
    if (!cs->learned) {
        return 0;
    }
    const uint64_t t_ns = analog_audio_sample_ns(end_ns, n, i, cs->agc.rate_hz);
    return cs->since_learn + i + 1U >= rate_hz
           || (t_ns > cs->last_learn_ns && t_ns - cs->last_learn_ns >= DSD_ANALOG_AUDIO_NEW_RECEPTION_GAP_NS);
}

/* The AGC over @p n samples, each learning as @p learn says, or, with no @p learn, every one as @p all says. */
static void
analog_audio_agc_runs(dsd_speech_agc* agc, float* buf, size_t n, const uint8_t* learn, int all) {
    if (learn || !all) {
        dsd_speech_agc_process_runs(agc, buf, n, learn);
        return;
    }
    uint8_t ones[256];
    DSD_MEMSET(ones, 1, sizeof ones);
    for (size_t done = 0; done < n;) {
        const size_t len = (n - done) < sizeof ones ? (n - done) : sizeof ones;
        dsd_speech_agc_process_runs(agc, buf + done, len, ones);
        done += len;
    }
}

/* The AGC over the call, starting it over before each learning sample that comes a second or more after the last one
   (dsd_analog_audio_process_runs_f()). @p end_ns is the decode-clock time of the call's last sample. */
static void
analog_audio_run_agc(analog_audio_chain_state* cs, float* buf, size_t n, const uint8_t* learn, int all,
                     uint64_t end_ns) {
    size_t from = 0;
    int prev = cs->learn_prev;
    int have = 0;
    size_t last = 0;
    for (size_t i = 0; i < n; i++) {
        const int l = learn ? learn[i] != 0U : all;
        /* Inside a run the samples are one apart: only a run's first sample, or the call's first to learn, can. */
        if (l && (!have || prev != 1) && analog_audio_new_reception(cs, have, last, i, n, end_ns)) {
            analog_audio_agc_runs(&cs->agc, buf + from, i - from, learn ? learn + from : NULL, all);
            dsd_speech_agc_reset(&cs->agc);
            from = i;
        }
        if (l) {
            have = 1;
            last = i;
        }
        prev = l;
    }
    analog_audio_agc_runs(&cs->agc, buf + from, n - from, learn ? learn + from : NULL, all);
    cs->learn_prev = prev;
    if (have) {
        cs->learned = 1;
        cs->last_learn_ns = analog_audio_sample_ns(end_ns, n, last, cs->agc.rate_hz);
        cs->since_learn = (uint64_t)(n - 1U - last);
    } else {
        cs->since_learn += (uint64_t)n;
    }
}

/* The gain stage: the source gain, then the AGC, fixed at N / 50 or automatic; publishes what it applied over the
   -n 50 gain. At a rate the AGC does not support, the fixed gain (-n 50 on auto, logged) through the limiter. */
static void
analog_audio_run_gain(const dsd_opts* opts, dsd_state* state, analog_audio_chain_state* cs, float* buf, size_t n,
                      unsigned int flags, const uint8_t* learn, uint64_t end_ns, float start_gain_db) {
    const int is_auto = dsd_analog_gain_is_auto(opts->audio_gainA);
    const double setting = is_auto ? 1.0 : (double)opts->audio_gainA / ANALOG_AUDIO_UNITY_SETTING;
    if (!cs->agc_ok) {
        if (is_auto) {
            analog_audio_log_unsupported_rate(cs);
        }
        analog_audio_apply_gain(buf, n, cs->source_gain * setting);
        dsd_peak_limiter_process(&cs->limiter, buf, n);
        state->aout_gainA = (float)(20.0 * log10(setting * (double)cs->limiter.gain));
        return;
    }
    analog_audio_apply_gain(buf, n, cs->source_gain);
    dsd_speech_agc_set_start_gain_db(&cs->agc, start_gain_db);
    dsd_speech_agc_set_fixed(&cs->agc, is_auto ? 0.0f : (float)setting);
    analog_audio_run_agc(cs, buf, n, learn, (flags & DSD_ANALOG_AUDIO_LEARN) != 0U, end_ns);
    state->aout_gainA = dsd_speech_agc_gain_db(&cs->agc);
}

/* dsd_analog_audio_process_runs_f() with the decode-clock time of the last sample given: @p end_ns. */
static int
analog_audio_process_at(const dsd_opts* opts, dsd_state* state, dsd_analog_audio_chain chain, float* buf, size_t n,
                        dsd_analog_audio_source source, int rate_hz, unsigned int flags, const uint8_t* learn,
                        uint64_t end_ns) {
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
        analog_audio_apply_gain(
            buf, n, analog_audio_fixed_gain(analog_audio_chain_gain(source, analog_audio_deemph_gain(opts)), setting));
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
    const dsd_voice_band_kind band = analog_audio_band(opts, chain, source);
    const float start_gain_db = analog_audio_start_gain_db(opts, band, source);
    analog_audio_chain_configure(cs, band, source, rate_hz, &now, start_gain_db);
    analog_audio_run_filters(opts, cs, buf, n);
    analog_audio_run_gain(opts, state, cs, buf, n, flags, learn, end_ns, start_gain_db);
    return 0;
}

int
dsd_analog_audio_process_runs_f(const dsd_opts* opts, dsd_state* state, dsd_analog_audio_chain chain, float* buf,
                                size_t n, dsd_analog_audio_source source, int rate_hz, unsigned int flags,
                                const uint8_t* learn) {
    return analog_audio_process_at(opts, state, chain, buf, n, source, rate_hz, flags, learn, dsd_decode_now_mono_ns());
}

int
dsd_analog_audio_process_f(const dsd_opts* opts, dsd_state* state, dsd_analog_audio_chain chain, float* buf, size_t n,
                           dsd_analog_audio_source source, int rate_hz, unsigned int flags) {
    return dsd_analog_audio_process_runs_f(opts, state, chain, buf, n, source, rate_hz, flags, NULL);
}

int
dsd_analog_audio_process_s(const dsd_opts* opts, dsd_state* state, dsd_analog_audio_chain chain, short* buf, size_t n,
                           dsd_analog_audio_source source, int rate_hz, unsigned int flags) {
    if (!opts || !state || !buf) {
        return -1;
    }
    float tmp[1024];
    /* One clock reading for the call, each piece's last sample placed before it by the samples after that piece. */
    const uint64_t end_ns = dsd_decode_now_mono_ns();
    const uint64_t piece_rate_hz = (uint64_t)(rate_hz > 0 ? rate_hz : 48000);
    size_t done = 0;
    while (done < n) {
        const size_t len = (n - done) < (sizeof tmp / sizeof tmp[0]) ? (n - done) : (sizeof tmp / sizeof tmp[0]);
        for (size_t i = 0; i < len; i++) {
            tmp[i] = (float)buf[done + i];
        }
        /* RESET applies once, to the first piece. */
        const unsigned int piece_flags = done == 0U ? flags : (flags & ~DSD_ANALOG_AUDIO_RESET);
        const uint64_t back = ((uint64_t)(n - done - len) * 1000000000ULL) / piece_rate_hz;
        if (analog_audio_process_at(opts, state, chain, tmp, len, source, rate_hz, piece_flags, NULL,
                                    end_ns > back ? end_ns - back : 0U)
            != 0) {
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
