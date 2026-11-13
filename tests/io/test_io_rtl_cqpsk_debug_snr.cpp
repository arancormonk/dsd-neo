// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The DSD_NEO_DEBUG_CQPSK "[CQPSK] EVM ... SNR" line against the CQPSK SNR meter, on the real chain.
 *
 * A raised-cosine pi/4-DQPSK stream (as DSP_CQPSK_EVM synthesizes it) with AWGN at known SNRs runs through full_demod()
 * at 48 kHz, at the P25 Phase 1 (10 samples/symbol) and Phase 2 (8) rates. The meter (the I/Q estimator every
 * frontend shows for CQPSK) updates every other block, as the demod thread runs it on a CQPSK stream; the debug line is
 * read from stderr. Both subtract the same estimator bias, so the two must agree: the debug line once read its
 * differential-phase error against the levels' mean power (5), about 4.9 dB above the meter on any channel.
 *
 * The line's own fields must agree too: EVM is measured against the reference the SNR uses, so on every line
 * SNR = -20*log10(EVM/100) - bias, with the bias the line names being the one the chain reports. EVM once stayed
 * relative to the levels' RMS (about 2.24) after the SNR moved to the meter's scale, 4.9 dB apart.
 */

#include <cmath>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/dsp/demod_pipeline.h>
#include <dsd-neo/dsp/demod_state.h>
#include <dsd-neo/dsp/snr_bias.h>
#include <dsd-neo/dsp/ted.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/mem.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

#include "rtl_stream_test_support.h"
#include "test_env.h"
#include "test_support.h"

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kAlpha = 0.2;
constexpr int kRateHz = 48000;
constexpr int kBlockPairs = kRateHz / 25;
constexpr int kTxHalfSpanSymbols = 8;
constexpr int kNumSymbols = 36000;
constexpr int kWarmupSymbols = 4000;
constexpr double kAmplitude = 0.35;
constexpr double kToleranceDb = 0.6;
/* The fields are printed to 0.01% (EVM) and 0.1 dB (SNR, bias): rounding alone stays well inside these. */
constexpr double kFieldToleranceDb = 0.15;
constexpr double kBiasTolerance = 0.06;
constexpr size_t kCaptureBytes = (size_t)256U * 1024U;

int g_bias_sps = 10;

double
rc_impulse(double t, double sps) {
    const double x = t / sps;
    const double sinc = (std::fabs(x) < 1e-9) ? 1.0 : std::sin(kPi * x) / (kPi * x);
    const double den = 1.0 - (2.0 * kAlpha * x) * (2.0 * kAlpha * x);
    if (std::fabs(den) < 1e-7) {
        return sinc * kPi / 4.0;
    }
    return sinc * std::cos(kPi * kAlpha * x) / den;
}

uint32_t
lcg_next(uint32_t* state) {
    *state = (*state * 1103515245u) + 12345u;
    return *state;
}

double
lcg_uniform(uint32_t* state) {
    return ((double)(lcg_next(state) >> 8) + 0.5) / 16777216.0;
}

/* Interleaved I/Q of a pi/4-DQPSK stream at @p sps samples per symbol. */
std::vector<float>
synthesize(int sps) {
    static const double kStep[4] = {kPi / 4.0, 3.0 * kPi / 4.0, -kPi / 4.0, -3.0 * kPi / 4.0};
    const int pairs = kNumSymbols * sps;
    const int half_span = kTxHalfSpanSymbols * sps;
    std::vector<float> iq((size_t)pairs * 2U, 0.0f);
    uint32_t seed = 0x13572468u;
    double phase = 0.0;
    for (int k = 0; k < kNumSymbols; k++) {
        phase += kStep[(lcg_next(&seed) >> 16) & 3U];
        const double si = std::cos(phase) * kAmplitude;
        const double sq = std::sin(phase) * kAmplitude;
        const int center = k * sps;
        const int n0 = center - half_span < 0 ? 0 : center - half_span;
        const int n1 = center + half_span >= pairs ? pairs - 1 : center + half_span;
        for (int n = n0; n <= n1; n++) {
            const double p = rc_impulse((double)(n - center), (double)sps);
            iq[(size_t)n * 2U] += (float)(si * p);
            iq[(size_t)n * 2U + 1U] += (float)(sq * p);
        }
    }
    return iq;
}

/* Complex AWGN @p snr_db below the signal's mean power (over the whole 48 kHz). */
void
add_noise(std::vector<float>* iq, double snr_db) {
    double power = 0.0;
    for (float v : *iq) {
        power += (double)v * (double)v;
    }
    power /= 0.5 * (double)iq->size();
    const double sigma = std::sqrt(power / std::pow(10.0, snr_db / 10.0) / 2.0);
    uint32_t seed = 0x2468ACE1u;
    for (size_t n = 0; n + 1U < iq->size(); n += 2U) {
        const double mag = sigma * std::sqrt(-2.0 * std::log(lcg_uniform(&seed)));
        const double ang = 2.0 * kPi * lcg_uniform(&seed);
        (*iq)[n] += (float)(mag * std::cos(ang));
        (*iq)[n + 1U] += (float)(mag * std::sin(ang));
    }
}

demod_state*
make_state(int sps) {
    demod_state* s = static_cast<demod_state*>(dsd_neo_aligned_malloc(sizeof(demod_state)));
    if (!s) {
        return nullptr;
    }
    DSD_MEMSET(s, 0, sizeof(*s));
    ted_init_state(&s->ted_state);
    s->rate_in = kRateHz;
    s->rate_out = kRateHz;
    s->complex_rate = kRateHz;
    s->cqpsk_enable = 1;
    s->ted_sps = sps;
    s->sps_is_integer = 1;
    s->output_kind = DSD_DEMOD_OUTPUT_SYMBOL_CQPSK;
    s->symbol_rate_hz = kRateHz / sps;
    s->symbol_levels = 4;
    s->channel_lpf_enable = 1;
    s->channel_lpf_profile = DSD_CH_LPF_PROFILE_P25_CQPSK;
    s->mode_demod = &dsd_fm_demod;
    s->cqpsk_diff_prev_r = 1.0f;
    s->cqpsk_diff_prev_j = 0.0f;
    s->cqpsk_agc_avg = 1.0f;
    s->squelch_gate_open = 1;
    return s;
}

/* The bias the stream would report for this chain (the engine installs rtl_stream_get_snr_bias_evm()). */
double
chain_bias_evm(void) {
    return dsd_snr_bias_evm_db(kRateHz, g_bias_sps, DSD_CH_LPF_PROFILE_P25_CQPSK, 0);
}

/* Run @p iq through the chain in 40 ms blocks; the mean meter reading after warm-up goes to @p out_meter_db. */
int
run_chain(const std::vector<float>& iq, int sps, double* out_meter_db) {
    demod_state* s = make_state(sps);
    if (!s) {
        return 1;
    }
    const int pairs = (int)(iq.size() / 2U);
    double sum = 0.0;
    int n = 0;
    int symbols = 0;
    int rc = 0;
    for (int off = 0, block = 0; off + kBlockPairs <= pairs; off += kBlockPairs, block++) {
        DSD_MEMCPY(s->input_cb_buf, iq.data() + (size_t)off * 2U, (size_t)kBlockPairs * 2U * sizeof(float));
        s->lowpassed = s->input_cb_buf;
        s->lp_len = kBlockPairs * 2;
        full_demod(s);
        symbols += s->result_len;
        if ((block & 1) == 0) {
            continue;
        }
        double meter_db = -100.0;
        if (rtl_stream_test_snr_update_block(s, block == 1, &meter_db) != 0) {
            rc = 1;
            break;
        }
        if (symbols > kWarmupSymbols) {
            sum += meter_db;
            n++;
        }
    }
    dsd_neo_aligned_free(s);
    if (rc != 0 || n == 0) {
        return 1;
    }
    *out_meter_db = sum / (double)n;
    return 0;
}

/* The mean SNR of the "[CQPSK] EVM" lines in @p text, the first left out: it closes the previous run's tail and this
   run's warm-up. */
int
mean_debug_snr(const char* text, double* out_db) {
    static const char kTag[] = "[CQPSK] EVM:";
    double sum = 0.0;
    int n = 0;
    int seen = 0;
    for (const char* p = strstr(text, kTag); p; p = strstr(p + 1, kTag)) {
        const char* snr = strstr(p, "SNR:");
        if (!snr) {
            break;
        }
        char* end = nullptr;
        const double v = strtod(snr + 4, &end);
        if (end == snr + 4) {
            return 1;
        }
        if (seen++ == 0) {
            continue;
        }
        sum += v;
        n++;
    }
    if (n < 2) {
        return 1;
    }
    *out_db = sum / (double)n;
    return 0;
}

/* Every "[CQPSK] EVM" line in @p text names the chain's bias (@p bias) and has SNR = -20*log10(EVM/100) - bias. */
int
check_line_fields(const char* text, double bias, int sps, double snr_in_db) {
    static const char kTag[] = "[CQPSK] EVM:";
    int lines = 0;
    int rc = 0;
    for (const char* p = strstr(text, kTag); p; p = strstr(p + 1, kTag)) {
        const char* eol = strchr(p, '\n');
        char* end = nullptr;
        const double evm_pct = strtod(p + sizeof(kTag) - 1U, &end);
        const char* snr = strstr(p, "SNR:");
        if (end == p + sizeof(kTag) - 1U || !snr || (eol && snr > eol)) {
            DSD_FPRINTF(stderr, "sps=%d in=%.0f dB: unreadable line\n", sps, snr_in_db);
            return 1;
        }
        const double snr_db = strtod(snr + 4, &end);
        if (end == snr + 4 || !(evm_pct > 0.0)) {
            DSD_FPRINTF(stderr, "sps=%d in=%.0f dB: unreadable line\n", sps, snr_in_db);
            return 1;
        }
        lines++;
        const double from_evm = -20.0 * std::log10(evm_pct / 100.0) - bias;
        if (!(std::fabs(from_evm - snr_db) <= kFieldToleranceDb)) {
            DSD_FPRINTF(stderr,
                        "sps=%d in=%.0f dB: EVM %.2f%% gives SNR %.2f dB less the bias, the line says %.1f dB\n", sps,
                        snr_in_db, evm_pct, from_evm, snr_db);
            rc = 1;
        }
        const char* named = strstr(p, "bias:");
        const double named_bias = (named && (!eol || named < eol)) ? strtod(named + 5, &end) : -100.0;
        if (!(std::fabs(named_bias - bias) <= kBiasTolerance)) {
            DSD_FPRINTF(stderr, "sps=%d in=%.0f dB: the line does not name the bias it took off (%.2f dB)\n", sps,
                        snr_in_db, bias);
            rc = 1;
        }
        if (rc != 0) {
            return rc;
        }
    }
    if (lines < 2) {
        DSD_FPRINTF(stderr, "sps=%d in=%.0f dB: too few [CQPSK] EVM lines\n", sps, snr_in_db);
        return 1;
    }
    return 0;
}

int
check_case(int sps, double snr_in_db, char* buf) {
    std::vector<float> iq = synthesize(sps);
    add_noise(&iq, snr_in_db);
    g_bias_sps = sps;
    dsd_test_capture_stderr cap;
    if (dsd_test_capture_stderr_begin(&cap, "dsdneo_cqpsk_debug_snr") != 0) {
        DSD_FPRINTF(stderr, "stderr capture failed\n");
        return 1;
    }
    double meter_db = -100.0;
    const int run_rc = run_chain(iq, sps, &meter_db);
    (void)dsd_test_capture_stderr_end(&cap);
    if (dsd_test_capture_stderr_read(&cap, buf, kCaptureBytes) != 0 || run_rc != 0) {
        DSD_FPRINTF(stderr, "sps=%d in=%.0f dB: chain or capture failed\n", sps, snr_in_db);
        return 1;
    }
    double debug_db = -100.0;
    if (mean_debug_snr(buf, &debug_db) != 0) {
        DSD_FPRINTF(stderr, "sps=%d in=%.0f dB: too few [CQPSK] EVM lines\n", sps, snr_in_db);
        return 1;
    }
    const double diff = debug_db - meter_db;
    DSD_FPRINTF(stderr, "sps=%d in=%.0f dB: meter %.2f dB, debug line %.2f dB, diff %+.2f dB\n", sps, snr_in_db,
                meter_db, debug_db, diff);
    if (!(std::fabs(diff) <= kToleranceDb)) {
        DSD_FPRINTF(stderr, "  FAIL: the debug line is not on the meter's scale (tolerance %.1f dB)\n", kToleranceDb);
        return 1;
    }
    return check_line_fields(buf, chain_bias_evm(), sps, snr_in_db);
}

} // namespace

int
main(void) {
    if (dsd_test_setenv("DSD_NEO_DEBUG_CQPSK", "1", 1) != 0) {
        return 1;
    }
    dsd_neo_config_init();
    dsd_rtl_stream_metrics_hooks hooks = {};
    hooks.snr_bias_evm = chain_bias_evm;
    dsd_rtl_stream_metrics_hooks_set(&hooks);

    std::vector<char> buf(kCaptureBytes);
    static const int kSps[2] = {10, 8};
    static const double kSnrDb[4] = {6.0, 12.0, 18.0, 24.0};
    int rc = 0;
    for (int sps : kSps) {
        for (double snr : kSnrDb) {
            rc |= check_case(sps, snr, buf.data());
        }
    }
    dsd_rtl_stream_metrics_hooks_set(nullptr);
    (void)dsd_test_unsetenv("DSD_NEO_DEBUG_CQPSK");
    return rc;
}
