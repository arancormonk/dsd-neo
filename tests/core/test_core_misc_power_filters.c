// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <dsd-neo/core/audio_filters.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/fec/trellis.h>
#include <dsd-neo/fec/viterbi.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>

#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

static void
test_trellis_and_viterbi_helpers(void) {
    uint8_t decoded_bits[16];
    uint8_t encoded_bits[40];
    DSD_MEMSET(decoded_bits, 0xA5, sizeof(decoded_bits));
    DSD_MEMSET(encoded_bits, 0, sizeof(encoded_bits));

    trellis_decode(decoded_bits, encoded_bits, 8);
    for (size_t i = 0; i < 8U; i++) {
        assert(decoded_bits[i] == 0U);
    }

    assert(q_abs_diff(10U, 4U) == 6U);
    assert(q_abs_diff(4U, 10U) == 6U);

    uint16_t soft_zero[32];
    uint8_t out[8];
    DSD_MEMSET(soft_zero, 0, sizeof(soft_zero));
    DSD_MEMSET(out, 0xFF, sizeof(out));
    assert(viterbi_decode(out, soft_zero, 16U) == 0U);
    assert(out[0] == 0U);

    const uint8_t puncture[] = {1U, 0U, 1U, 1U};
    DSD_MEMSET(out, 0xFF, sizeof(out));
    assert(viterbi_decode_punctured(out, soft_zero, puncture, 12U, (uint16_t)(sizeof(puncture))) == 0U);
    assert(out[0] == 0U);

    viterbi_reset();
    viterbi_decode_bit(0U, 0U, 0U);
    DSD_MEMSET(out, 0xFF, sizeof(out));
    (void)viterbi_chainback(out, 1U, 1U);
    assert((out[0] & 0x80U) == 0U);
}

static void
test_power_helpers(void) {
    const short constant_samples[] = {1000, 1000, 1000, 1000};
    assert(raw_pwr(constant_samples, 4, 1) == 0.0);
    assert(raw_rms(constant_samples, 4, 1) == 0.0);
    assert(raw_pwr(constant_samples, 0, 1) == 0.0);

    const short alternating_samples[] = {32767, -32768, 32767, -32768};
    double short_power = raw_pwr(alternating_samples, 4, 1);
    assert(short_power > 0.99 && short_power <= 1.0);

    const float float_samples[] = {32767.0f, -32768.0f, 32767.0f, -32768.0f};
    double float_power = raw_pwr_f(float_samples, 4, 1);
    assert(float_power > 0.99 && float_power <= 1.0);
    assert(raw_pwr_f(float_samples, 0, 1) == 0.0);

    assert(pwr_to_dB(0.0) == -120.0);
    assert(pwr_to_dB(2.0) == 0.0);
    assert(pwr_to_dB(1.0e-20) == -120.0);

    assert(dB_to_pwr(0.0) == 1.0);
    assert(dB_to_pwr(12.0) == 1.0);
    assert(dB_to_pwr(-250.0) >= 0.0);

    double p = dB_to_pwr(-30.0);
    assert(fabs(pwr_to_dB(p) + 30.0) < 0.001);
}

static void
test_audio_filter_helpers(void) {
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));

    init_audio_filters(&state, 0);
    short lpf_samples[] = {12000, 12000, 12000, 12000};
    lpf(&state, lpf_samples, 4);
    assert(lpf_samples[0] > 0);
    assert(lpf_samples[3] >= lpf_samples[0]);

    float lpf_float[] = {12000.0f, 12000.0f, 12000.0f, 12000.0f};
    lpf_f(&state, lpf_float, 4);
    assert(lpf_float[0] > 0.0f);
    assert(lpf_float[3] >= lpf_float[0]);

    short hpf_samples[] = {0, 12000, 12000, 12000};
    hpf(&state, hpf_samples, 4);
    assert(hpf_samples[1] > 0);

    float hpf_float[] = {0.0f, 12000.0f, 12000.0f, 12000.0f};
    hpf_f(&state, hpf_float, 4);
    assert(hpf_float[1] > 0.0f);

    short pbf_samples[] = {0, 12000, 0, -12000};
    pbf(&state, pbf_samples, 4);
    assert(pbf_samples[1] != 0);

    float pbf_float[] = {0.0f, 12000.0f, 0.0f, -12000.0f};
    pbf_f(&state, pbf_float, 4);
    assert(fabsf(pbf_float[1]) > 0.0f);

    init_audio_filters(&state, 48000);
    state.HRCFilterL.coef = 1.0f;
    state.HRCFilterL.v_in[0] = -32768.0f;
    state.HRCFilterL.v_out[0] = 40000.0f;
    short left[] = {32767};
    hpf_dL(&state, left, 1);
    assert(left[0] == 32767);

    state.HRCFilterR.coef = 1.0f;
    state.HRCFilterR.v_in[0] = 32767.0f;
    state.HRCFilterR.v_out[0] = -40000.0f;
    short right[] = {-32768};
    hpf_dR(&state, right, 1);
    assert(right[0] == -32768);

    short empty[] = {1234};
    lpf(&state, empty, 0);
    hpf(&state, empty, 0);
    pbf(&state, empty, 0);
    assert(empty[0] == 1234);
}

int
main(void) {
    test_trellis_and_viterbi_helpers();
    test_power_helpers();
    test_audio_filter_helpers();
    return 0;
}
