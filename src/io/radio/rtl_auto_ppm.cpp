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
    (void)source;
    return std::max(inputs.gate_snr_db, inputs.spec_snr_db);
}

static inline double
min_correctable_df_hz(uint32_t tuned_freq_hz, const RtlAutoPpmConfig& config) {
    return (config.min_correction_ppm * static_cast<double>(tuned_freq_hz)) / 1.0e6;
}

static inline bool
within_acquisition_lock_window(double ppm, double df_hz, uint32_t tuned_freq_hz, const RtlAutoPpmConfig& config) {
    /* Lock acquisition must never be stricter than the smallest residual the
     * controller can actually correct. Otherwise a sub-deadband residual can
     * neither step nor lock, leaving training stuck forever. */
    double effective_zero_lock_ppm = std::max(config.zero_lock_ppm, config.min_correction_ppm);
    double effective_zero_lock_hz = std::max(config.zero_lock_hz, min_correctable_df_hz(tuned_freq_hz, config));
    return (std::fabs(ppm) <= effective_zero_lock_ppm) && (std::fabs(df_hz) <= effective_zero_lock_hz);
}

static bool
tracking_estimate_ready(const RtlAutoPpmSignalMetrics& metrics) {
    return metrics.cqpsk_enable && metrics.tracking_enable && metrics.carrier_lock && finite_hz(metrics.nco_cfo_hz);
}

struct auto_ppm_state {
    int last_enabled = 0;
    int locked = 0;
    int last_dir = 0;
    int last_input_ppm = 0;
    int pending_apply = 0;
    int pending_ppm = 0;
    uint32_t last_tuned_freq_hz = 0;
    uint64_t settle_until_ms = 0;
    uint64_t stable_since_ms = 0;
    uint64_t observation_since_ms = 0;
    double ema_ppm = 0.0;
    int ema_valid = 0;
    int observation_sign = 0;
    RtlAutoPpmSource last_source = RtlAutoPpmSource::None;
    RtlAutoPpmSource lock_source = RtlAutoPpmSource::None;
    int lock_ppm = 0;
    double lock_snr_db = -100.0;
    double lock_df_hz = 0.0;
    uint32_t lock_tuned_freq_hz = 0;
};

static inline int
rtl_auto_ppm_source_max_step(RtlAutoPpmSource source, const RtlAutoPpmConfig& config) {
    switch (source) {
        case RtlAutoPpmSource::CarrierTotal: return config.max_step_carrier_ppm;
        case RtlAutoPpmSource::PhaseResidual: return config.max_step_phase_ppm;
        case RtlAutoPpmSource::None:
        default: return 0;
    }
}

static inline int
rtl_auto_ppm_source_observation_ms(RtlAutoPpmSource source, const RtlAutoPpmConfig& config) {
    switch (source) {
        case RtlAutoPpmSource::CarrierTotal: return config.carrier_observation_ms;
        case RtlAutoPpmSource::PhaseResidual: return config.phase_observation_ms;
        case RtlAutoPpmSource::None:
        default: return 0;
    }
}

static inline int
rtl_auto_ppm_cooldown_ticks(uint64_t settle_until_ms, uint64_t now_ms) {
    if (settle_until_ms <= now_ms) {
        return 0;
    }
    uint64_t remain_ms = settle_until_ms - now_ms;
    return static_cast<int>((remain_ms + 9U) / 10U);
}

static void
rtl_auto_ppm_fill_estimate_status(const RtlAutoPpmInputs& inputs, RtlAutoPpmUpdate* out) {
    if (!inputs.estimate.valid() || inputs.tuned_freq_hz == 0) {
        return;
    }
    out->df_hz = inputs.estimate.error_hz;
    out->est_ppm = (inputs.estimate.error_hz * 1.0e6) / static_cast<double>(inputs.tuned_freq_hz);
    out->snr_db = estimate_snr_db(inputs.estimate.source, inputs);
}

static void
rtl_auto_ppm_sync_status(const RtlAutoPpmInputs& inputs, const auto_ppm_state& st, RtlAutoPpmUpdate* out) {
    out->new_ppm = inputs.current_ppm;
    out->locked = st.locked;
    out->last_dir = st.last_dir;
    out->lock_ppm = st.lock_ppm;
    out->lock_snr_db = st.lock_snr_db;
    out->lock_df_hz = st.lock_df_hz;
}

static void
rtl_auto_ppm_settle_training(const RtlAutoPpmInputs& inputs, const auto_ppm_state& st, RtlAutoPpmUpdate* out) {
    out->training = 1;
    out->locked = 0;
    out->cooldown_ticks = rtl_auto_ppm_cooldown_ticks(st.settle_until_ms, inputs.now_ms);
}

static int
rtl_auto_ppm_signal_ok(const RtlAutoPpmConfig& config, const RtlAutoPpmInputs& inputs, const RtlAutoPpmUpdate& out) {
    int snr_ok = (out.snr_db >= config.min_snr_db);
    int power_ok = (inputs.signal_power_db >= config.min_power_db);
    int ppm_ok = std::isfinite(out.est_ppm) && (std::fabs(out.est_ppm) <= config.max_abs_ppm);
    return snr_ok && power_ok && ppm_ok;
}

static int
rtl_auto_ppm_handle_locked_state(const RtlAutoPpmConfig& config, const RtlAutoPpmInputs& inputs, auto_ppm_state* st,
                                 RtlAutoPpmUpdate* out) {
    if (!st->locked) {
        return 0;
    }

    st->last_input_ppm = inputs.current_ppm;
    if (inputs.tuned_freq_hz != 0) {
        st->last_tuned_freq_hz = inputs.tuned_freq_hz;
    }
    st->pending_apply = 0;
    st->pending_ppm = inputs.current_ppm;
    st->settle_until_ms = 0;
    rtl_auto_ppm_sync_status(inputs, *st, out);
    rtl_auto_ppm_fill_estimate_status(inputs, out);

    int unlock_for_carrier_upgrade = 0;
    if (st->lock_source != RtlAutoPpmSource::CarrierTotal && inputs.estimate.source == RtlAutoPpmSource::CarrierTotal
        && inputs.tuned_freq_hz != 0 && (st->lock_tuned_freq_hz == 0 || inputs.tuned_freq_hz == st->lock_tuned_freq_hz)
        && inputs.requested_ppm == inputs.current_ppm && inputs.current_ppm == st->lock_ppm
        && rtl_auto_ppm_signal_ok(config, inputs, *out)) {
        int dir = sign_with_deadband(out->est_ppm, config.min_correction_ppm);
        if (dir != 0) {
            unlock_for_carrier_upgrade = 1;
        } else if (within_acquisition_lock_window(out->est_ppm, out->df_hz, inputs.tuned_freq_hz, config)) {
            st->lock_source = RtlAutoPpmSource::CarrierTotal;
            st->lock_snr_db = out->snr_db;
            st->lock_df_hz = out->df_hz;
            rtl_auto_ppm_sync_status(inputs, *st, out);
        }
    }

    if (!unlock_for_carrier_upgrade) {
        out->training = 0;
        out->locked = 1;
        out->cooldown_ticks = 0;
        return 1;
    }

    st->locked = 0;
    st->last_dir = 0;
    st->settle_until_ms = 0;
    st->stable_since_ms = 0;
    st->observation_since_ms = 0;
    st->ema_ppm = 0.0;
    st->ema_valid = 0;
    st->observation_sign = 0;
    st->last_source = RtlAutoPpmSource::None;
    rtl_auto_ppm_sync_status(inputs, *st, out);
    return 0;
}

static int
rtl_auto_ppm_handle_requested_ppm_pending(const RtlAutoPpmConfig& config, const RtlAutoPpmInputs& inputs,
                                          auto_ppm_state* st, RtlAutoPpmUpdate* out) {
    if (inputs.requested_ppm != inputs.current_ppm) {
        if (!st->pending_apply || st->pending_ppm != inputs.requested_ppm) {
            st->locked = 0;
            st->last_dir = 0;
            st->stable_since_ms = 0;
            st->observation_since_ms = 0;
            st->ema_ppm = 0.0;
            st->ema_valid = 0;
            st->observation_sign = 0;
            st->last_source = RtlAutoPpmSource::None;
            st->pending_apply = 1;
            st->pending_ppm = inputs.requested_ppm;
            st->settle_until_ms = inputs.now_ms + static_cast<uint64_t>(config.settle_ms);
        }
        rtl_auto_ppm_sync_status(inputs, *st, out);
        rtl_auto_ppm_fill_estimate_status(inputs, out);
        rtl_auto_ppm_settle_training(inputs, *st, out);
        return 1;
    }

    if (!st->pending_apply) {
        return 0;
    }

    if (inputs.current_ppm == st->pending_ppm) {
        st->pending_apply = 0;
        st->last_input_ppm = inputs.current_ppm;
        return 0;
    }

    int request_pending = rtl_ppm_controller_request_has_ppm(inputs.controller_queued_request, st->pending_ppm)
                          || rtl_ppm_controller_request_has_ppm(inputs.controller_active_request, st->pending_ppm);

    if (inputs.current_ppm == st->last_input_ppm && request_pending) {
        rtl_auto_ppm_sync_status(inputs, *st, out);
        rtl_auto_ppm_fill_estimate_status(inputs, out);
        rtl_auto_ppm_settle_training(inputs, *st, out);
        return 1;
    }

    if (inputs.current_ppm == st->last_input_ppm) {
        st->pending_apply = 0;
        st->pending_ppm = inputs.current_ppm;
        st->settle_until_ms = 0;
    } else {
        st->pending_apply = 0;
    }
    return 0;
}

static void
rtl_auto_ppm_apply_input_changes(const RtlAutoPpmInputs& inputs, auto_ppm_state* st) {
    if (inputs.current_ppm != st->last_input_ppm) {
        st->locked = 0;
        st->last_dir = 0;
        st->settle_until_ms = 0;
        st->stable_since_ms = 0;
        st->observation_since_ms = 0;
        st->ema_ppm = 0.0;
        st->ema_valid = 0;
        st->observation_sign = 0;
        st->last_source = RtlAutoPpmSource::None;
        st->last_input_ppm = inputs.current_ppm;
    }

    if (inputs.tuned_freq_hz != 0 && inputs.tuned_freq_hz != st->last_tuned_freq_hz) {
        st->last_dir = 0;
        st->settle_until_ms = 0;
        st->stable_since_ms = 0;
        st->observation_since_ms = 0;
        st->ema_ppm = 0.0;
        st->ema_valid = 0;
        st->observation_sign = 0;
        st->last_source = RtlAutoPpmSource::None;
        st->last_tuned_freq_hz = inputs.tuned_freq_hz;
    } else if (st->last_tuned_freq_hz == 0) {
        st->last_tuned_freq_hz = inputs.tuned_freq_hz;
    }
}

static int
rtl_auto_ppm_handle_signal_checks(const RtlAutoPpmConfig& config, const RtlAutoPpmInputs& inputs, auto_ppm_state* st,
                                  RtlAutoPpmUpdate* out) {
    if (!inputs.estimate.valid() || inputs.tuned_freq_hz == 0) {
        out->training = st->locked ? 0 : out->training;
        out->locked = st->locked ? 1 : st->locked;
        return 1;
    }

    out->df_hz = inputs.estimate.error_hz;
    out->est_ppm = (inputs.estimate.error_hz * 1.0e6) / static_cast<double>(inputs.tuned_freq_hz);
    out->snr_db = estimate_snr_db(inputs.estimate.source, inputs);

    if (st->settle_until_ms > inputs.now_ms) {
        rtl_auto_ppm_settle_training(inputs, *st, out);
        out->last_dir = st->last_dir;
        return 1;
    }

    if (rtl_auto_ppm_signal_ok(config, inputs, *out)) {
        return 0;
    }

    st->observation_since_ms = 0;
    st->stable_since_ms = 0;
    st->observation_sign = 0;
    st->ema_valid = 0;
    st->last_dir = 0;
    out->last_dir = 0;
    out->locked = st->locked;
    return 1;
}

static int
rtl_auto_ppm_update_tracking(const RtlAutoPpmConfig& config, const RtlAutoPpmInputs& inputs, auto_ppm_state* st,
                             RtlAutoPpmUpdate* out) {
    if (!st->ema_valid || inputs.estimate.source != st->last_source) {
        st->ema_ppm = out->est_ppm;
        st->ema_valid = 1;
        st->observation_since_ms = inputs.now_ms;
        st->observation_sign = sign_with_deadband(st->ema_ppm, config.min_correction_ppm);
        st->stable_since_ms = 0;
    } else {
        st->ema_ppm = (0.85 * st->ema_ppm) + (0.15 * out->est_ppm);
    }

    st->last_source = inputs.estimate.source;
    out->est_ppm = st->ema_ppm;
    out->df_hz = (st->ema_ppm * static_cast<double>(inputs.tuned_freq_hz)) / 1.0e6;
    out->last_dir = sign_with_deadband(st->ema_ppm, config.min_correction_ppm);
    return out->last_dir;
}

static int
rtl_auto_ppm_try_lock_or_wait(const RtlAutoPpmConfig& config, const RtlAutoPpmInputs& inputs, auto_ppm_state* st,
                              RtlAutoPpmUpdate* out) {
    if (out->last_dir != 0) {
        return 0;
    }

    st->observation_since_ms = 0;
    st->observation_sign = 0;
    if (within_acquisition_lock_window(st->ema_ppm, out->df_hz, inputs.tuned_freq_hz, config)) {
        if (st->stable_since_ms == 0) {
            st->stable_since_ms = inputs.now_ms;
        }
        if ((inputs.now_ms - st->stable_since_ms) >= static_cast<uint64_t>(config.lock_hold_ms)) {
            st->locked = 1;
            st->lock_source = inputs.estimate.source;
            st->lock_ppm = inputs.current_ppm;
            st->lock_snr_db = out->snr_db;
            st->lock_df_hz = out->df_hz;
            st->lock_tuned_freq_hz = inputs.tuned_freq_hz;
            st->ema_ppm = 0.0;
            st->ema_valid = 0;
            st->last_source = RtlAutoPpmSource::None;
            out->locked = 1;
            out->training = 0;
            out->lock_ppm = st->lock_ppm;
            out->lock_snr_db = st->lock_snr_db;
            out->lock_df_hz = st->lock_df_hz;
            return 1;
        }
    } else {
        st->stable_since_ms = 0;
    }

    st->locked = 0;
    out->training = 1;
    out->locked = 0;
    return 1;
}

static int
rtl_auto_ppm_try_apply_correction(const RtlAutoPpmConfig& config, const RtlAutoPpmInputs& inputs, auto_ppm_state* st,
                                  RtlAutoPpmUpdate* out) {
    st->locked = 0;
    st->stable_since_ms = 0;
    if (st->observation_sign != out->last_dir) {
        st->observation_sign = out->last_dir;
        st->observation_since_ms = inputs.now_ms;
    }

    out->training = 1;
    out->locked = 0;

    int need_ms = rtl_auto_ppm_source_observation_ms(inputs.estimate.source, config);
    if ((inputs.now_ms - st->observation_since_ms) < static_cast<uint64_t>(need_ms)) {
        return 0;
    }

    int step_ppm = static_cast<int>(std::llround(st->ema_ppm));
    if (step_ppm == 0) {
        step_ppm = out->last_dir;
    }
    step_ppm = clamp_ppm_step(step_ppm, rtl_auto_ppm_source_max_step(inputs.estimate.source, config));
    if (step_ppm == 0) {
        return 0;
    }

    int new_ppm = inputs.current_ppm + step_ppm;
    new_ppm = clamp_ppm_step(new_ppm, static_cast<int>(config.max_abs_ppm));
    if (new_ppm == inputs.current_ppm) {
        return 0;
    }

    out->apply_ppm = 1;
    out->new_ppm = new_ppm;
    out->last_dir = (step_ppm > 0) ? 1 : -1;
    st->last_dir = out->last_dir;
    st->pending_apply = 1;
    st->pending_ppm = new_ppm;
    st->locked = 0;
    st->settle_until_ms = inputs.now_ms + static_cast<uint64_t>(config.settle_ms);
    st->stable_since_ms = 0;
    st->observation_since_ms = 0;
    st->observation_sign = 0;
    st->ema_ppm = 0.0;
    st->ema_valid = 0;
    st->last_source = RtlAutoPpmSource::None;
    out->cooldown_ticks = static_cast<int>((static_cast<uint64_t>(config.settle_ms) + 9U) / 10U);
    out->locked = 0;
    out->training = 1;
    return 1;
}

} // namespace

RtlAutoPpmEstimate
rtl_auto_ppm_select_estimate(const RtlAutoPpmSignalMetrics& metrics) {
    /* Tuner PPM is a device-wide calibration. Only CQPSK carrier recovery can
     * export a stable total CFO; non-CQPSK totals can include transmitter or
     * channel offset and must not be folded into the dongle calibration. */
    if (tracking_estimate_ready(metrics)) {
        return {RtlAutoPpmSource::CarrierTotal, metrics.nco_cfo_hz};
    }

    /* For non-CQPSK, tracking_enable means phase_cfo_hz has already been
     * vetted by a mode-specific tracker such as the FSK modem DC estimator. */
    if (!metrics.cqpsk_enable && metrics.tracking_enable && finite_hz(metrics.phase_cfo_hz)) {
        return {RtlAutoPpmSource::PhaseResidual, metrics.phase_cfo_hz};
    }

    return {};
}

double
rtl_auto_ppm_fsk_dc_est_to_cfo_hz(double dc_rad_per_sample, int sample_rate_hz) {
    /* FSK dc_est is the discriminator-centering term subtracted inside the
     * modem (`centered = freq - dc_est`), not the residual that remains after
     * centering. Hardware tests with RTL-SDR frequency correction show that
     * positive dc_est must request a negative PPM correction to reduce the
     * observed symbol DC bias, so keep this conversion opposite the raw
     * phase-delta sign. */
    return -dc_rad_per_sample * static_cast<double>(sample_rate_hz) / 6.28318530717958647692;
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
    pending_apply_ = 0;
    pending_ppm_ = 0;
    lock_source_ = RtlAutoPpmSource::None;
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

RtlAutoPpmUpdate
RtlAutoPpmController::update(const RtlAutoPpmConfig& config, const RtlAutoPpmInputs& inputs) {
    RtlAutoPpmUpdate out = {};
    auto_ppm_state st = {};
    st.last_enabled = last_enabled_;
    st.locked = locked_;
    st.last_dir = last_dir_;
    st.last_input_ppm = last_input_ppm_;
    st.pending_apply = pending_apply_;
    st.pending_ppm = pending_ppm_;
    st.last_tuned_freq_hz = last_tuned_freq_hz_;
    st.settle_until_ms = settle_until_ms_;
    st.stable_since_ms = stable_since_ms_;
    st.observation_since_ms = observation_since_ms_;
    st.ema_ppm = ema_ppm_;
    st.ema_valid = ema_valid_;
    st.observation_sign = observation_sign_;
    st.last_source = last_source_;
    st.lock_source = lock_source_;
    st.lock_ppm = lock_ppm_;
    st.lock_snr_db = lock_snr_db_;
    st.lock_df_hz = lock_df_hz_;
    st.lock_tuned_freq_hz = lock_tuned_freq_hz_;

    if (!inputs.enabled) {
        if (st.last_enabled) {
            reset(inputs.current_ppm, inputs.tuned_freq_hz);
        }
        st.last_enabled = 0;
        rtl_auto_ppm_sync_status(inputs, st, &out);
        last_enabled_ = st.last_enabled;
        return out;
    }

    if (!st.last_enabled) {
        reset(inputs.current_ppm, inputs.tuned_freq_hz);
        st.last_input_ppm = inputs.current_ppm;
        st.last_tuned_freq_hz = inputs.tuned_freq_hz;
        st.locked = 0;
        st.last_dir = 0;
        st.pending_apply = 0;
        st.pending_ppm = 0;
        st.settle_until_ms = 0;
        st.stable_since_ms = 0;
        st.observation_since_ms = 0;
        st.ema_ppm = 0.0;
        st.ema_valid = 0;
        st.observation_sign = 0;
        st.last_source = RtlAutoPpmSource::None;
        st.lock_source = RtlAutoPpmSource::None;
        st.lock_ppm = 0;
        st.lock_snr_db = -100.0;
        st.lock_df_hz = 0.0;
        st.lock_tuned_freq_hz = 0;
    }
    st.last_enabled = 1;

    if (rtl_auto_ppm_handle_locked_state(config, inputs, &st, &out)) {
        goto commit_and_return;
    }

    if (rtl_auto_ppm_handle_requested_ppm_pending(config, inputs, &st, &out)) {
        goto commit_and_return;
    }

    rtl_auto_ppm_apply_input_changes(inputs, &st);
    rtl_auto_ppm_sync_status(inputs, st, &out);

    if (rtl_auto_ppm_handle_signal_checks(config, inputs, &st, &out)) {
        goto commit_and_return;
    }

    rtl_auto_ppm_update_tracking(config, inputs, &st, &out);
    if (rtl_auto_ppm_try_lock_or_wait(config, inputs, &st, &out)) {
        goto commit_and_return;
    }

    (void)rtl_auto_ppm_try_apply_correction(config, inputs, &st, &out);

commit_and_return:
    last_enabled_ = st.last_enabled;
    locked_ = st.locked;
    last_dir_ = st.last_dir;
    last_input_ppm_ = st.last_input_ppm;
    pending_apply_ = st.pending_apply;
    pending_ppm_ = st.pending_ppm;
    last_tuned_freq_hz_ = st.last_tuned_freq_hz;
    settle_until_ms_ = st.settle_until_ms;
    stable_since_ms_ = st.stable_since_ms;
    observation_since_ms_ = st.observation_since_ms;
    ema_ppm_ = st.ema_ppm;
    ema_valid_ = st.ema_valid;
    observation_sign_ = st.observation_sign;
    last_source_ = st.last_source;
    lock_source_ = st.lock_source;
    lock_ppm_ = st.lock_ppm;
    lock_snr_db_ = st.lock_snr_db;
    lock_df_hz_ = st.lock_df_hz;
    lock_tuned_freq_hz_ = st.lock_tuned_freq_hz;
    return out;
}

} // namespace radio
} // namespace io
} // namespace dsd
