// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/dsp/fsk_modem.h>
#include <math.h>
#include "dsd-neo/core/safe_api.h"

#define DSD_FSK_DISCRIMINATOR_TARGET   30000.0f
#define DSD_FSK_DISCRIMINATOR_MIN_PEAK 1.0e-7f

static int
clamp_levels(int levels) {
    return (levels == 2) ? 2 : 4;
}

static int
valid_rate(int rate_hz, int fallback_hz) {
    return (rate_hz > 0) ? rate_hz : fallback_hz;
}

static inline float
phase_delta_small_angle_or_atan2(float im, float re) {
    float abs_im = fabsf(im);
    if (re > 1.0e-7f && abs_im <= (0.35f * re)) {
        float x = im / re;
        float x2 = x * x;
        /* atan(x) Maclaurin through x^5. In the gated region the next term is
         * below discriminator noise for the RTL FSK symbol path; outside it we
         * keep atan2f's quadrant and large-angle behavior. */
        return x * (1.0f + x2 * (-0.3333333333333333f + x2 * 0.2f));
    }
    return atan2f(im, re);
}

static dsd_fsk_modem_config
normalized_config(const dsd_fsk_modem_config* cfg) {
    dsd_fsk_modem_config out;
    if (cfg) {
        out = *cfg;
    } else {
        DSD_MEMSET(&out, 0, sizeof(out));
    }
    out.sample_rate_hz = valid_rate(out.sample_rate_hz, 48000);
    out.symbol_rate_hz = valid_rate(out.symbol_rate_hz, 4800);
    out.levels = clamp_levels(out.levels);
    return out;
}

void
dsd_fsk_modem_reset(dsd_fsk_modem_state* st) {
    if (!st) {
        return;
    }
    dsd_fsk_modem_config cfg = st->cfg;
    DSD_MEMSET(st, 0, sizeof(*st));
    st->cfg = normalized_config(&cfg);
}

void
dsd_fsk_modem_configure(dsd_fsk_modem_state* st, const dsd_fsk_modem_config* cfg) {
    if (!st) {
        return;
    }
    dsd_fsk_modem_config next = normalized_config(cfg);
    int changed = (st->cfg.sample_rate_hz != next.sample_rate_hz || st->cfg.symbol_rate_hz != next.symbol_rate_hz
                   || st->cfg.levels != next.levels || st->cfg.channel_profile != next.channel_profile);
    st->cfg = next;
    if (changed) {
        dsd_fsk_modem_reset(st);
    }
}

void
dsd_fsk_modem_init(dsd_fsk_modem_state* st, const dsd_fsk_modem_config* cfg) {
    if (!st) {
        return;
    }
    DSD_MEMSET(st, 0, sizeof(*st));
    st->cfg = normalized_config(cfg);
}

void
dsd_fsk_modem_release(dsd_fsk_modem_state* st) {
    (void)st;
}

static float
discriminator_frequency(float cur_i, float cur_q, float prev_i, float prev_q) {
    float re = cur_i * prev_i + cur_q * prev_q;
    float im = cur_q * prev_i - cur_i * prev_q;
    return phase_delta_small_angle_or_atan2(im, re);
}

static float
update_centered_frequency(float* dc_io, float freq) {
    /* Very slow carrier centering. The symbol stream is expected to be
     * roughly balanced over frame-sync windows; keeping this slow prevents
     * long same-symbol runs from being mistaken for CFO. */
    *dc_io += 0.00025f * (freq - *dc_io);
    return freq - *dc_io;
}

static float
clip_pcm_float(float sample) {
    if (sample > 32767.0f) {
        return 32767.0f;
    }
    if (sample < -32768.0f) {
        return -32768.0f;
    }
    return sample;
}

static float
scale_discriminator_sample(dsd_fsk_modem_state* st, float centered) {
    float mag = fabsf(centered);
    if (mag > DSD_FSK_DISCRIMINATOR_MIN_PEAK) {
        if (st->discriminator_peak_est <= DSD_FSK_DISCRIMINATOR_MIN_PEAK) {
            st->discriminator_peak_est = mag;
        } else if (mag > st->discriminator_peak_est) {
            st->discriminator_peak_est += 0.125f * (mag - st->discriminator_peak_est);
        } else {
            st->discriminator_peak_est += 0.00005f * (mag - st->discriminator_peak_est);
        }
    }
    float peak = st->discriminator_peak_est;
    if (peak <= DSD_FSK_DISCRIMINATOR_MIN_PEAK) {
        peak = 1.0f;
    }
    return clip_pcm_float(centered * (DSD_FSK_DISCRIMINATOR_TARGET / peak));
}

int
dsd_fsk_modem_discriminator_process(dsd_fsk_modem_state* st, const float* iq_interleaved, int len_interleaved,
                                    float* out_samples, int max_samples) {
    if (!st || !out_samples || max_samples <= 0 || !iq_interleaved || len_interleaved < 2) {
        return 0;
    }

    int pairs = len_interleaved >> 1;
    int out_count = 0;
    for (int n = 0; n < pairs && out_count < max_samples; n++) {
        float cur_i = iq_interleaved[(n << 1) + 0];
        float cur_q = iq_interleaved[(n << 1) + 1];

        if (!st->have_prev) {
            st->prev_i = cur_i;
            st->prev_q = cur_q;
            st->have_prev = 1;
            out_samples[out_count++] = 0.0f;
            continue;
        }

        float freq = discriminator_frequency(cur_i, cur_q, st->prev_i, st->prev_q);
        float centered = update_centered_frequency(&st->dc_est, freq);
        out_samples[out_count++] = scale_discriminator_sample(st, centered);
        st->prev_i = cur_i;
        st->prev_q = cur_q;
    }

    return out_count;
}
