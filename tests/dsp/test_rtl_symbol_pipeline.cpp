// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <dsd-neo/dsp/demod_pipeline.h>
#include <dsd-neo/dsp/demod_state.h>
#include <dsd-neo/dsp/fsk_modem.h>

namespace {

constexpr float kPi = 3.14159265358979323846f;

static void
reset_demod(demod_state* s) {
    std::memset(s, 0, sizeof(*s));
    s->rate_in = 48000;
    s->rate_out = 48000;
    s->rate_out2 = 12000;
    s->post_downsample = 4;
    s->deemph = 1;
    s->dc_block = 1;
    s->audio_lpf_enable = 1;
    s->audio_lpf_alpha = 0.2f;
    s->mode_demod = &dsd_fm_demod;
    s->lowpassed = s->input_cb_buf;
    s->channel_lpf_enable = 0;
}

static int
classify4(float sym) {
    if (sym < -2.0f) {
        return -3;
    }
    if (sym < 0.0f) {
        return -1;
    }
    if (sym < 2.0f) {
        return 1;
    }
    return 3;
}

static void
synthesize_fsk_iq(float* out_iq, const float* symbols, int symbol_count, int sps, float deviation_per_level) {
    float phase = 0.19f;
    int out = 0;
    for (int sym = 0; sym < symbol_count; sym++) {
        for (int k = 0; k < sps; k++) {
            phase += deviation_per_level * symbols[sym];
            if (phase > kPi) {
                phase -= 2.0f * kPi;
            } else if (phase < -kPi) {
                phase += 2.0f * kPi;
            }
            out_iq[out++] = 0.85f * std::cos(phase);
            out_iq[out++] = 0.85f * std::sin(phase);
        }
    }
}

static int
check_fsk_symbol_output_contract(demod_state* s) {
    enum { SYMBOLS = 512, SPS = 10 };

    static const float levels[] = {-3.0f, -1.0f, 1.0f, 3.0f, 1.0f, -1.0f};
    float expected[SYMBOLS];

    reset_demod(s);
    s->output_kind = DSD_DEMOD_OUTPUT_SYMBOL_FSK;
    s->symbol_rate_hz = 4800;
    s->symbol_levels = 4;
    s->lp_len = SYMBOLS * SPS * 2;

    for (int i = 0; i < SYMBOLS; i++) {
        expected[i] = levels[i % (int)(sizeof(levels) / sizeof(levels[0]))];
    }
    synthesize_fsk_iq(s->input_cb_buf, expected, SYMBOLS, SPS, 0.028f);

    dsd_fsk_modem_config cfg = {};
    cfg.sample_rate_hz = s->rate_out;
    cfg.symbol_rate_hz = s->symbol_rate_hz;
    cfg.levels = s->symbol_levels;
    cfg.channel_profile = DSD_CH_LPF_PROFILE_P25_C4FM;
    dsd_fsk_modem_init(&s->fsk_modem_state, &cfg);

    full_demod(s);

    if (s->result_len < SYMBOLS - 2 || s->result_len > SYMBOLS + 2) {
        std::fprintf(stderr, "FSK symbol path result_len=%d, expected about %d symbols\n", s->result_len, SYMBOLS);
        return 1;
    }

    int checked = 0;
    int ok = 0;
    const int limit = (s->result_len < SYMBOLS) ? s->result_len : SYMBOLS;
    for (int i = 80; i < limit; i++) {
        checked++;
        if (classify4(s->result[i]) == (int)expected[i]) {
            ok++;
        }
    }
    if (checked < 200 || ((double)ok / (double)checked) < 0.95) {
        std::fprintf(stderr, "FSK symbol path accuracy %.3f (%d/%d)\n", checked ? (double)ok / (double)checked : 0.0,
                     ok, checked);
        return 1;
    }
    return 0;
}

static void
synthesize_dqpsk_iq(float* out_iq, const float* differential_symbols, int symbol_count) {
    float phase = 0.0f;
    for (int i = 0; i < symbol_count; i++) {
        phase += differential_symbols[i] * (kPi / 4.0f);
        while (phase > kPi) {
            phase -= 2.0f * kPi;
        }
        while (phase < -kPi) {
            phase += 2.0f * kPi;
        }
        out_iq[(i << 1) + 0] = std::cos(phase);
        out_iq[(i << 1) + 1] = std::sin(phase);
    }
}

static int
check_cqpsk_symbol_output_contract(demod_state* s) {
    enum { SYMBOLS = 256 };

    static const float expected_cycle[] = {-3.0f, -1.0f, 1.0f, 3.0f};
    float expected[SYMBOLS];

    reset_demod(s);
    s->output_kind = DSD_DEMOD_OUTPUT_SYMBOL_CQPSK;
    s->cqpsk_enable = 1;
    s->symbol_rate_hz = 4800;
    s->symbol_levels = 4;
    s->lp_len = SYMBOLS * 2;
    s->mode_demod = &raw_demod;
    s->cqpsk_diff_prev_r = 1.0f;
    s->cqpsk_diff_prev_j = 0.0f;

    for (int i = 0; i < SYMBOLS; i++) {
        expected[i] = expected_cycle[i & 3];
    }
    synthesize_dqpsk_iq(s->input_cb_buf, expected, SYMBOLS);

    full_demod(s);

    if (s->result_len != SYMBOLS) {
        std::fprintf(stderr, "CQPSK symbol path result_len=%d, expected %d\n", s->result_len, SYMBOLS);
        return 1;
    }
    for (int i = 0; i < SYMBOLS; i++) {
        if (std::fabs(s->result[i] - expected[i]) > 0.02f) {
            std::fprintf(stderr, "CQPSK symbol[%d]=%.4f, expected %.1f\n", i, s->result[i], expected[i]);
            return 1;
        }
    }
    return 0;
}

} // namespace

int
main(void) {
    demod_state* s = static_cast<demod_state*>(std::malloc(sizeof(*s)));
    if (!s) {
        return 1;
    }

    int rc = 0;
    rc |= check_fsk_symbol_output_contract(s);
    rc |= check_cqpsk_symbol_output_contract(s);

    std::free(s);
    return rc;
}
