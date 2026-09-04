// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_IO_RADIO_RTL_AUTO_PPM_H
#define DSD_NEO_IO_RADIO_RTL_AUTO_PPM_H

#include <stdint.h>

#include "rtl_ppm_request.h"

namespace dsd {
namespace io {
namespace radio {

enum class RtlAutoPpmSource : uint8_t {
    None = 0,
    CarrierTotal = 1,
    PhaseResidual = 2,
};

struct RtlAutoPpmSignalMetrics {
    int cqpsk_enable = 0;
    int tracking_enable = 0;
    int carrier_lock = 0;
    double nco_cfo_hz = 0.0;
    double phase_cfo_hz = 0.0;
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
    int settle_ms = 2500;
    int lock_hold_ms = 3000;
    int max_step_carrier_ppm = 25;
    int max_step_phase_ppm = 12;
};

struct RtlAutoPpmInputs {
    uint64_t now_ms = 0;
    int enabled = 0;
    int current_ppm = 0;
    int requested_ppm = 0;
    RtlPpmControllerRequestState controller_queued_request = {};
    RtlPpmControllerRequestState controller_active_request = {};
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

/* Convert the FSK modem's discriminator centering estimate into the RTL/Soapy
 * frequency-correction sign used by auto-PPM. The result is intentionally
 * opposite the raw dc_est phase-delta sign. */
double rtl_auto_ppm_fsk_dc_est_to_cfo_hz(double dc_rad_per_sample, int sample_rate_hz);

class RtlAutoPpmController {
  public:
    void reset(int current_ppm, uint32_t tuned_freq_hz);
    RtlAutoPpmUpdate update(const RtlAutoPpmConfig& config, const RtlAutoPpmInputs& inputs);

  private:
    void clear_observation_state();
    void clear_runtime_state();

    int last_enabled_ = 0;
    int locked_ = 0;
    int last_dir_ = 0;
    int last_input_ppm_ = 0;
    int pending_apply_ = 0;
    int pending_ppm_ = 0;
    uint32_t last_tuned_freq_hz_ = 0;
    uint64_t settle_until_ms_ = 0;
    uint64_t stable_since_ms_ = 0;
    uint64_t observation_since_ms_ = 0;
    double ema_ppm_ = 0.0;
    int ema_valid_ = 0;
    int observation_sign_ = 0;
    RtlAutoPpmSource last_source_ = RtlAutoPpmSource::None;
    RtlAutoPpmSource lock_source_ = RtlAutoPpmSource::None;
    int lock_ppm_ = 0;
    double lock_snr_db_ = -100.0;
    double lock_df_hz_ = 0.0;
    uint32_t lock_tuned_freq_hz_ = 0;
};

} // namespace radio
} // namespace io
} // namespace dsd

#endif /* DSD_NEO_IO_RADIO_RTL_AUTO_PPM_H */
