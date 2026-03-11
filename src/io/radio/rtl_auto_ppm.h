// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#pragma once

#include <stdint.h>

namespace dsd {
namespace io {
namespace radio {

enum class RtlAutoPpmSource : int {
    None = 0,
    CarrierTotal = 1,
    PhaseResidual = 2,
    SpectrumResidual = 3,
};

struct RtlAutoPpmSignalMetrics {
    int cqpsk_enable = 0;
    int tracking_enable = 0;
    int carrier_lock = 0;
    int spectrum_valid = 0;
    double nco_cfo_hz = 0.0;
    double phase_cfo_hz = 0.0;
    double spectrum_cfo_hz = 0.0;
};

struct RtlAutoPpmEstimate {
    RtlAutoPpmSource source = RtlAutoPpmSource::None;
    double error_hz = 0.0;

    bool
    valid() const {
        return source != RtlAutoPpmSource::None;
    }
};

struct RtlAutoPpmConfig {
    double min_snr_db = 6.0;
    double min_power_db = -80.0;
    double min_correction_ppm = 0.5;
    double zero_lock_ppm = 0.6;
    double zero_lock_hz = 60.0;
    double max_abs_ppm = 200.0;
    int carrier_observation_ms = 4000;
    int phase_observation_ms = 5000;
    int spectrum_observation_ms = 8000;
    int settle_ms = 2500;
    int lock_hold_ms = 3000;
    int max_step_carrier_ppm = 25;
    int max_step_phase_ppm = 12;
    int max_step_spectrum_ppm = 4;
};

struct RtlAutoPpmInputs {
    uint64_t now_ms = 0;
    int enabled = 0;
    int current_ppm = 0;
    uint32_t tuned_freq_hz = 0;
    double signal_power_db = -100.0;
    double gate_snr_db = -100.0;
    double spec_snr_db = -100.0;
    RtlAutoPpmEstimate estimate;
};

struct RtlAutoPpmUpdate {
    int apply_ppm = 0;
    int new_ppm = 0;
    int locked = 0;
    int training = 0;
    int last_dir = 0;
    int cooldown_ticks = 0;
    double snr_db = -100.0;
    double df_hz = 0.0;
    double est_ppm = 0.0;
    int lock_ppm = 0;
    double lock_snr_db = -100.0;
    double lock_df_hz = 0.0;
};

RtlAutoPpmEstimate rtl_auto_ppm_select_estimate(const RtlAutoPpmSignalMetrics& metrics);

class RtlAutoPpmController {
  public:
    void reset(int current_ppm, uint32_t tuned_freq_hz);
    RtlAutoPpmUpdate update(const RtlAutoPpmConfig& config, const RtlAutoPpmInputs& inputs);

  private:
    void clear_observation_state();
    void clear_runtime_state();
    int max_step_ppm(RtlAutoPpmSource source, const RtlAutoPpmConfig& config) const;
    int observation_ms(RtlAutoPpmSource source, const RtlAutoPpmConfig& config) const;

    int last_enabled_ = 0;
    int locked_ = 0;
    int last_dir_ = 0;
    int last_input_ppm_ = 0;
    uint32_t last_tuned_freq_hz_ = 0;
    uint64_t settle_until_ms_ = 0;
    uint64_t stable_since_ms_ = 0;
    uint64_t observation_since_ms_ = 0;
    double ema_ppm_ = 0.0;
    int ema_valid_ = 0;
    int observation_sign_ = 0;
    RtlAutoPpmSource last_source_ = RtlAutoPpmSource::None;
    int lock_ppm_ = 0;
    double lock_snr_db_ = -100.0;
    double lock_df_hz_ = 0.0;
    uint32_t lock_tuned_freq_hz_ = 0;
};

} // namespace radio
} // namespace io
} // namespace dsd
