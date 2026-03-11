// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "rtl_auto_ppm.h"

#include <algorithm>
#include <cmath>

namespace dsd {
namespace io {
namespace radio {
namespace {

static inline bool
finite_hz(double value) {
    return std::isfinite(value);
}

static inline int
sign_with_deadband(double value, double deadband) {
    if (value > deadband) {
        return 1;
    }
    if (value < -deadband) {
        return -1;
    }
    return 0;
}

static inline int
clamp_ppm_step(int step, int limit) {
    if (step > limit) {
        return limit;
    }
    if (step < -limit) {
        return -limit;
    }
    return step;
}

static inline double
estimate_snr_db(RtlAutoPpmSource source, const RtlAutoPpmInputs& inputs) {
    if (source == RtlAutoPpmSource::SpectrumResidual) {
        return inputs.spec_snr_db;
    }
    return std::max(inputs.gate_snr_db, inputs.spec_snr_db);
}

static bool
tracking_estimate_ready(const RtlAutoPpmSignalMetrics& metrics) {
    return metrics.cqpsk_enable && metrics.tracking_enable && metrics.carrier_lock && finite_hz(metrics.nco_cfo_hz);
}

} // namespace

RtlAutoPpmEstimate
rtl_auto_ppm_select_estimate(const RtlAutoPpmSignalMetrics& metrics) {
    /* Tuner PPM is a device-wide calibration. Only CQPSK carrier recovery can
     * export a stable total CFO; non-CQPSK FLL totals can include transmitter
     * or channel offset and must not be folded into the dongle calibration. */
    if (tracking_estimate_ready(metrics)) {
        return {RtlAutoPpmSource::CarrierTotal, metrics.nco_cfo_hz};
    }

    if (metrics.spectrum_valid && finite_hz(metrics.spectrum_cfo_hz)) {
        return {RtlAutoPpmSource::SpectrumResidual, metrics.spectrum_cfo_hz};
    }

    return {};
}

void
RtlAutoPpmController::clear_observation_state() {
    last_dir_ = 0;
    settle_until_ms_ = 0;
    stable_since_ms_ = 0;
    observation_since_ms_ = 0;
    ema_ppm_ = 0.0;
    ema_valid_ = 0;
    observation_sign_ = 0;
    last_source_ = RtlAutoPpmSource::None;
}

void
RtlAutoPpmController::clear_runtime_state() {
    locked_ = 0;
    clear_observation_state();
    lock_ppm_ = 0;
    lock_snr_db_ = -100.0;
    lock_df_hz_ = 0.0;
    lock_tuned_freq_hz_ = 0;
}

void
RtlAutoPpmController::reset(int current_ppm, uint32_t tuned_freq_hz) {
    clear_runtime_state();
    last_enabled_ = 0;
    last_input_ppm_ = current_ppm;
    last_tuned_freq_hz_ = tuned_freq_hz;
}

int
RtlAutoPpmController::max_step_ppm(RtlAutoPpmSource source, const RtlAutoPpmConfig& config) const {
    switch (source) {
        case RtlAutoPpmSource::CarrierTotal: return config.max_step_carrier_ppm;
        case RtlAutoPpmSource::PhaseResidual: return config.max_step_phase_ppm;
        case RtlAutoPpmSource::SpectrumResidual: return config.max_step_spectrum_ppm;
        case RtlAutoPpmSource::None:
        default: return 0;
    }
}

int
RtlAutoPpmController::observation_ms(RtlAutoPpmSource source, const RtlAutoPpmConfig& config) const {
    switch (source) {
        case RtlAutoPpmSource::CarrierTotal: return config.carrier_observation_ms;
        case RtlAutoPpmSource::PhaseResidual: return config.phase_observation_ms;
        case RtlAutoPpmSource::SpectrumResidual: return config.spectrum_observation_ms;
        case RtlAutoPpmSource::None:
        default: return config.phase_observation_ms;
    }
}

RtlAutoPpmUpdate
RtlAutoPpmController::update(const RtlAutoPpmConfig& config, const RtlAutoPpmInputs& inputs) {
    RtlAutoPpmUpdate out = {};
    auto sync_status = [&]() {
        out.new_ppm = inputs.current_ppm;
        out.locked = locked_;
        out.last_dir = last_dir_;
        out.lock_ppm = lock_ppm_;
        out.lock_snr_db = lock_snr_db_;
        out.lock_df_hz = lock_df_hz_;
    };

    if (!inputs.enabled) {
        if (last_enabled_) {
            reset(inputs.current_ppm, inputs.tuned_freq_hz);
        }
        last_enabled_ = 0;
        sync_status();
        return out;
    }

    if (!last_enabled_) {
        reset(inputs.current_ppm, inputs.tuned_freq_hz);
    }
    last_enabled_ = 1;

    if (inputs.current_ppm != last_input_ppm_) {
        locked_ = 0;
        clear_observation_state();
        last_input_ppm_ = inputs.current_ppm;
    }

    if (inputs.tuned_freq_hz != 0 && inputs.tuned_freq_hz != last_tuned_freq_hz_) {
        /* Tuning changes invalidate per-channel drift history. Keep the last
         * lock snapshot across channel hops, but re-base lock validation to the
         * new channel so later residuals can re-enter training if needed. */
        clear_observation_state();
        if (locked_) {
            lock_tuned_freq_hz_ = inputs.tuned_freq_hz;
        }
        last_tuned_freq_hz_ = inputs.tuned_freq_hz;
    } else if (last_tuned_freq_hz_ == 0) {
        last_tuned_freq_hz_ = inputs.tuned_freq_hz;
    }

    sync_status();

    if (!inputs.estimate.valid() || inputs.tuned_freq_hz == 0) {
        if (locked_) {
            out.training = 0;
            out.locked = 1;
        } else {
            out.locked = locked_;
        }
        return out;
    }

    out.df_hz = inputs.estimate.error_hz;
    out.est_ppm = (inputs.estimate.error_hz * 1.0e6) / static_cast<double>(inputs.tuned_freq_hz);
    out.snr_db = estimate_snr_db(inputs.estimate.source, inputs);

    bool snr_ok = (out.snr_db >= config.min_snr_db);
    bool power_ok = (inputs.signal_power_db >= config.min_power_db);
    bool ppm_ok = std::isfinite(out.est_ppm) && (std::fabs(out.est_ppm) <= config.max_abs_ppm);
    bool signal_ok = snr_ok && power_ok && ppm_ok;

    if (locked_) {
        bool keep_lock = true;
        if (signal_ok && inputs.tuned_freq_hz == lock_tuned_freq_hz_) {
            double filtered_ppm = out.est_ppm;
            if (!ema_valid_ || inputs.estimate.source != last_source_) {
                ema_ppm_ = filtered_ppm;
                ema_valid_ = 1;
            } else {
                ema_ppm_ = (0.85 * ema_ppm_) + (0.15 * filtered_ppm);
            }
            last_source_ = inputs.estimate.source;
            filtered_ppm = ema_ppm_;
            double filtered_df_hz = (filtered_ppm * static_cast<double>(inputs.tuned_freq_hz)) / 1.0e6;
            int filtered_dir = sign_with_deadband(filtered_ppm, config.min_correction_ppm);
            /* Residuals inside the correction deadband cannot generate a tuner
             * step, so dropping lock here would strand the controller in an
             * unrecoverable training state. Keep the existing lock until the
             * drift grows large enough to produce a real correction. */
            keep_lock = (filtered_dir == 0)
                        || ((std::fabs(filtered_ppm) <= config.zero_lock_ppm)
                            && (std::fabs(filtered_df_hz) <= config.zero_lock_hz));
            if (keep_lock) {
                out.est_ppm = filtered_ppm;
                out.df_hz = filtered_df_hz;
            }
        }
        if (keep_lock) {
            out.training = 0;
            out.locked = 1;
            return out;
        }

        /* Preserve the last lock snapshot for UI/status, but restart the
         * observation window from the current estimate so drift can retrain. */
        locked_ = 0;
        clear_observation_state();
        sync_status();
    }

    if (settle_until_ms_ > inputs.now_ms) {
        out.training = 1;
        uint64_t remain_ms = settle_until_ms_ - inputs.now_ms;
        out.cooldown_ticks = static_cast<int>((remain_ms + 9U) / 10U);
        out.locked = 0;
        out.last_dir = last_dir_;
        return out;
    }

    if (!signal_ok) {
        observation_since_ms_ = 0;
        stable_since_ms_ = 0;
        observation_sign_ = 0;
        ema_valid_ = 0;
        last_dir_ = 0;
        out.last_dir = 0;
        out.locked = locked_;
        return out;
    }

    if (!ema_valid_ || inputs.estimate.source != last_source_) {
        ema_ppm_ = out.est_ppm;
        ema_valid_ = 1;
        observation_since_ms_ = inputs.now_ms;
        observation_sign_ = sign_with_deadband(ema_ppm_, config.min_correction_ppm);
        stable_since_ms_ = 0;
    } else {
        ema_ppm_ = (0.85 * ema_ppm_) + (0.15 * out.est_ppm);
    }
    last_source_ = inputs.estimate.source;
    out.est_ppm = ema_ppm_;
    out.df_hz = (ema_ppm_ * static_cast<double>(inputs.tuned_freq_hz)) / 1.0e6;

    int dir = sign_with_deadband(ema_ppm_, config.min_correction_ppm);
    out.last_dir = dir;

    if (dir == 0) {
        /* Deadband breaks the continuous same-sign run, so require a fresh
         * observation window if the error later grows again. */
        observation_since_ms_ = 0;
        observation_sign_ = 0;
        if (std::fabs(ema_ppm_) <= config.zero_lock_ppm && std::fabs(out.df_hz) <= config.zero_lock_hz) {
            if (stable_since_ms_ == 0) {
                stable_since_ms_ = inputs.now_ms;
            }
            if ((inputs.now_ms - stable_since_ms_) >= static_cast<uint64_t>(config.lock_hold_ms)) {
                locked_ = 1;
                lock_ppm_ = inputs.current_ppm;
                lock_snr_db_ = out.snr_db;
                lock_df_hz_ = out.df_hz;
                lock_tuned_freq_hz_ = inputs.tuned_freq_hz;
                /* Re-seed post-lock drift validation from the first new sample
                 * after lock instead of blending against the near-zero EMA that
                 * satisfied the lock window. */
                ema_ppm_ = 0.0;
                ema_valid_ = 0;
                last_source_ = RtlAutoPpmSource::None;
                out.locked = 1;
                out.training = 0;
                out.lock_ppm = lock_ppm_;
                out.lock_snr_db = lock_snr_db_;
                out.lock_df_hz = lock_df_hz_;
                return out;
            }
        } else {
            stable_since_ms_ = 0;
        }
        locked_ = 0;
        out.training = 1;
        out.locked = 0;
        return out;
    }

    locked_ = 0;
    stable_since_ms_ = 0;
    if (observation_sign_ != dir) {
        observation_sign_ = dir;
        observation_since_ms_ = inputs.now_ms;
    }

    out.training = 1;
    out.locked = 0;

    int need_ms = observation_ms(inputs.estimate.source, config);
    if ((inputs.now_ms - observation_since_ms_) < static_cast<uint64_t>(need_ms)) {
        return out;
    }

    /* The residual estimate and the RTL tuner correction use the same sign:
     * a positive residual means the signal sits above the tuned center, so the
     * applied hardware PPM correction must also move positive. */
    int step_ppm = static_cast<int>(std::llround(ema_ppm_));
    if (step_ppm == 0) {
        step_ppm = dir;
    }
    step_ppm = clamp_ppm_step(step_ppm, max_step_ppm(inputs.estimate.source, config));
    if (step_ppm == 0) {
        return out;
    }

    int new_ppm = inputs.current_ppm + step_ppm;
    new_ppm = clamp_ppm_step(new_ppm, static_cast<int>(config.max_abs_ppm));
    if (new_ppm == inputs.current_ppm) {
        return out;
    }

    out.apply_ppm = 1;
    out.new_ppm = new_ppm;
    out.last_dir = (step_ppm > 0) ? 1 : -1;
    last_dir_ = out.last_dir;
    last_input_ppm_ = new_ppm;
    /* A single hardware step does not imply convergence. Large residuals may
     * need multiple bounded corrections, and even a full correction needs time
     * for the tuner/DSP path to settle before we can trust a zero-residual
     * lock. Keep training active and restart the observation window. */
    locked_ = 0;
    settle_until_ms_ = inputs.now_ms + static_cast<uint64_t>(config.settle_ms);
    stable_since_ms_ = 0;
    observation_since_ms_ = 0;
    observation_sign_ = 0;
    ema_ppm_ = 0.0;
    ema_valid_ = 0;
    last_source_ = RtlAutoPpmSource::None;
    out.cooldown_ticks = static_cast<int>((static_cast<uint64_t>(config.settle_ms) + 9U) / 10U);
    out.locked = 0;
    out.training = 1;
    return out;
}

} // namespace radio
} // namespace io
} // namespace dsd
