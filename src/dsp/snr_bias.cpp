// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/dsp/snr_bias.h>

#include <dsd-neo/dsp/demod_state.h>
#include <dsd-neo/runtime/digital_channel.h>

#include <math.h>

/* SNR estimator bias constants: these are the "pure" estimator biases for the
 * variance-ratio method, independent of noise bandwidth. They correct for the
 * statistical bias of the quartile/median-based clustering approach.
 *
 * The total SNR correction is:
 *   bias_total = bias_estimator + 10*log10(Bn / Rs)
 * where Bn = noise equivalent bandwidth of channel LPF, Rs = symbol rate.
 *
 * These estimator biases were derived by subtracting the nominal bandwidth
 * term from the original empirical calibration values:
 *   C4FM: 7.95 dB - 10*log10(8000/4800) = 7.95 - 2.22 = 5.73 dB
 *   GFSK/QPSK: 2.43 dB - 10*log10(5400/4800) = 2.43 - 0.51 = 1.92 dB
 */
static const double kC4fmEstimatorBiasDb = 5.73;
static const double kEvmEstimatorBiasDb = 1.92;

/* Noise equivalent bandwidth (Hz) for each channel LPF profile.
 * Computed as Bn = (Fs/2) * Σh² / (Σh)² for the 24 kHz reference designs and
 * used as fixed approximations because channel_lpf_* now holds absolute
 * transition-center frequencies even when Fs changes, at every rate the
 * profile designs at (up to DSD_DIGITAL_CHANNEL_MAX_RATE_HZ). The 6.25/12.5 kHz
 * profiles protect nominal channel edges by placing those edges inside the
 * passband rather than at the -6 dB cutoff center. */
static const double kNoiseBwWideHz = 8415.0;     /* Wide/analog profile, 8 kHz protected edge */
static const double kNoiseBw6K25Hz = 3540.0;     /* 6.25 kHz modes, 3125 Hz protected edge */
static const double kNoiseBw12K5Hz = 6665.0;     /* 12.5 kHz modes, 6250 Hz protected edge */
static const double kNoiseBwProvoiceHz = 6665.0; /* ProVoice, 6250 Hz protected edge */
static const double kNoiseBwP25C4fmHz = 6665.0;  /* P25 C4FM, 6250 Hz protected edge */
static const double kNoiseBwP25CqpskHz = 7065.0; /* P25 CQPSK/LSM, 12.5 kHz edge plus guard */
/* The TETRA chain: its RRC matched filter, not the wider channel filter ahead of it, bounds the noise. Its equivalent
 * noise bandwidth is the 18000 sym/s symbol rate two-sided, so 9000 Hz one-sided, the convention of the figures above
 * (Bn = (Fs/2) * sum(h^2) / sum(h)^2, which gives 9000 Hz for the RRC: sum(h^2) = 1/sps, sum(h) ~ 1). */
static const double kNoiseBwTetraHz = 9000.0;
/* The TETRA channel filter alone (dsd_tetra_channel_design(), 12.5 kHz cutoff, 2.5 kHz Blackman transition), which an
 * uplink session runs without the chain's matched filter: the same Bn formula over its taps gives 12114 Hz at every
 * rate of the TETRA domain (36, 54, 72 and 144 kHz checked; the design keeps its shape in Hz). */
static const double kNoiseBwTetraChannelHz = 12114.0;
/* Above DSD_DIGITAL_CHANNEL_MAX_RATE_HZ no profile design fits the channel filter, which runs a fixed 63-tap prototype
 * instead (demod_pipeline.cpp) whose response scales with the rate. Its Bn, by the same formula over its taps, is a
 * fixed fraction of the rate: 0.32505 for the WIDE prototype (cut at a third of the rate) that the WIDE, ProVoice and
 * P25 profiles fall back to, 0.20003 for the one the 6.25/12.5 kHz profiles fall back to. */
static const double kNoiseBwWideFallbackPerRate = 0.32505;
static const double kNoiseBwDigitalFallbackPerRate = 0.20003;

/* The profile design's bandwidth, held in Hz at every rate it designs at. */
static double
snr_noise_bandwidth_designed_hz(int lpf_profile, int tetra_stage) {
    switch (lpf_profile) {
        case DSD_CH_LPF_PROFILE_6K25: return kNoiseBw6K25Hz;
        case DSD_CH_LPF_PROFILE_12K5: return kNoiseBw12K5Hz;
        case DSD_CH_LPF_PROFILE_PROVOICE: return kNoiseBwProvoiceHz;
        case DSD_CH_LPF_PROFILE_P25_C4FM: return kNoiseBwP25C4fmHz;
        case DSD_CH_LPF_PROFILE_P25_CQPSK: return kNoiseBwP25CqpskHz;
        case DSD_CH_LPF_PROFILE_TETRA:
            switch (tetra_stage) {
                case DSD_SNR_TETRA_STAGE_CHAIN: return kNoiseBwTetraHz;
                case DSD_SNR_TETRA_STAGE_CHANNEL: return kNoiseBwTetraChannelHz;
                /* Neither stage runs: the WIDE channel plan. */
                default: return kNoiseBwWideHz;
            }
        default: return kNoiseBwWideHz;
    }
}

/* Whether the channel plan at this rate is the 63-tap fallback prototype: a generic profile design (any profile but
 * TETRA's own stages) at a rate above DSD_DIGITAL_CHANNEL_MAX_RATE_HZ. */
static int
snr_noise_bandwidth_falls_back(int complex_rate_hz, int lpf_profile, int tetra_stage) {
    if (complex_rate_hz <= DSD_DIGITAL_CHANNEL_MAX_RATE_HZ) {
        return 0;
    }
    return lpf_profile != DSD_CH_LPF_PROFILE_TETRA
           || (tetra_stage != DSD_SNR_TETRA_STAGE_CHAIN && tetra_stage != DSD_SNR_TETRA_STAGE_CHANNEL);
}

double
dsd_snr_noise_bandwidth_hz(int complex_rate_hz, int lpf_profile, int tetra_stage) {
    if (!snr_noise_bandwidth_falls_back(complex_rate_hz, lpf_profile, tetra_stage)) {
        return snr_noise_bandwidth_designed_hz(lpf_profile, tetra_stage);
    }
    const int digital = lpf_profile == DSD_CH_LPF_PROFILE_6K25 || lpf_profile == DSD_CH_LPF_PROFILE_12K5;
    return (double)complex_rate_hz * (digital ? kNoiseBwDigitalFallbackPerRate : kNoiseBwWideFallbackPerRate);
}

double
dsd_snr_bias_c4fm_db(int complex_rate_hz, int ted_sps, int lpf_profile, int tetra_stage) {
    if (complex_rate_hz <= 0 || ted_sps <= 0) {
        return kC4fmEstimatorBiasDb + 2.2; /* fallback to original ~7.95 dB */
    }
    double symbol_rate = (double)complex_rate_hz / (double)ted_sps;
    double noise_bw = dsd_snr_noise_bandwidth_hz(complex_rate_hz, lpf_profile, tetra_stage);
    /* Total bias = estimator bias + bandwidth penalty */
    return kC4fmEstimatorBiasDb + 10.0 * log10(noise_bw / symbol_rate);
}

double
dsd_snr_bias_evm_db(int complex_rate_hz, int ted_sps, int lpf_profile, int tetra_stage) {
    if (complex_rate_hz <= 0 || ted_sps <= 0) {
        return kEvmEstimatorBiasDb + 0.5; /* fallback to original ~2.43 dB */
    }
    double symbol_rate = (double)complex_rate_hz / (double)ted_sps;
    double noise_bw = dsd_snr_noise_bandwidth_hz(complex_rate_hz, lpf_profile, tetra_stage);
    /* Total bias = estimator bias + bandwidth penalty */
    return kEvmEstimatorBiasDb + 10.0 * log10(noise_bw / symbol_rate);
}
