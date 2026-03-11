// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <cmath>
#include <cstdio>
#include <stdint.h>

#include "rtl_auto_ppm.h"

using dsd::io::radio::rtl_auto_ppm_select_estimate;
using dsd::io::radio::RtlAutoPpmConfig;
using dsd::io::radio::RtlAutoPpmController;
using dsd::io::radio::RtlAutoPpmEstimate;
using dsd::io::radio::RtlAutoPpmInputs;
using dsd::io::radio::RtlAutoPpmSignalMetrics;
using dsd::io::radio::RtlAutoPpmSource;
using dsd::io::radio::RtlAutoPpmUpdate;

static int
expect_true(const char* label, bool cond) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", label);
        return 1;
    }
    return 0;
}

static int
expect_int_eq(const char* label, int got, int want) {
    if (got != want) {
        std::fprintf(stderr, "FAIL: %s got=%d want=%d\n", label, got, want);
        return 1;
    }
    return 0;
}

static int
expect_double_close(const char* label, double got, double want, double tol) {
    if (std::fabs(got - want) > tol) {
        std::fprintf(stderr, "FAIL: %s got=%.6f want=%.6f tol=%.6f\n", label, got, want, tol);
        return 1;
    }
    return 0;
}

static RtlAutoPpmInputs
make_inputs(uint64_t now_ms, int current_ppm, uint32_t tuned_freq_hz, double est_ppm, RtlAutoPpmSource source) {
    RtlAutoPpmInputs inputs = {};
    inputs.now_ms = now_ms;
    inputs.enabled = 1;
    inputs.current_ppm = current_ppm;
    inputs.tuned_freq_hz = tuned_freq_hz;
    inputs.signal_power_db = -40.0;
    inputs.gate_snr_db = 12.0;
    inputs.spec_snr_db = 10.0;
    inputs.estimate.source = source;
    inputs.estimate.error_hz = est_ppm * static_cast<double>(tuned_freq_hz) / 1.0e6;
    return inputs;
}

static int
test_select_estimate_accepts_large_finite_cqpsk_nco(void) {
    int rc = 0;
    RtlAutoPpmSignalMetrics metrics = {};
    metrics.cqpsk_enable = 1;
    metrics.carrier_lock = 1;
    metrics.spectrum_valid = 1;
    metrics.nco_cfo_hz = -68000.0;
    metrics.spectrum_cfo_hz = 1800.0;

    RtlAutoPpmEstimate estimate = rtl_auto_ppm_select_estimate(metrics);
    rc |= expect_int_eq("large cqpsk nco source", static_cast<int>(estimate.source),
                        static_cast<int>(RtlAutoPpmSource::CarrierTotal));
    rc |= expect_double_close("large cqpsk nco error", estimate.error_hz, -68000.0, 1e-9);
    return rc;
}

static int
test_select_estimate_prefers_cqpsk_nco_after_lock(void) {
    int rc = 0;
    RtlAutoPpmSignalMetrics metrics = {};
    metrics.cqpsk_enable = 1;
    metrics.carrier_lock = 1;
    metrics.spectrum_valid = 1;
    metrics.nco_cfo_hz = 1200.0;
    metrics.spectrum_cfo_hz = 75.0;

    RtlAutoPpmEstimate estimate = rtl_auto_ppm_select_estimate(metrics);
    rc |= expect_int_eq("cqpsk carrier source", static_cast<int>(estimate.source),
                        static_cast<int>(RtlAutoPpmSource::CarrierTotal));
    rc |= expect_double_close("cqpsk carrier error", estimate.error_hz, 1200.0, 1e-9);
    return rc;
}

static int
test_select_estimate_uses_cqpsk_spectrum_before_lock(void) {
    int rc = 0;
    RtlAutoPpmSignalMetrics metrics = {};
    metrics.cqpsk_enable = 1;
    metrics.carrier_lock = 0;
    metrics.spectrum_valid = 1;
    metrics.nco_cfo_hz = 950.0;
    metrics.spectrum_cfo_hz = 240.0;

    RtlAutoPpmEstimate estimate = rtl_auto_ppm_select_estimate(metrics);
    rc |= expect_int_eq("cqpsk bootstrap source", static_cast<int>(estimate.source),
                        static_cast<int>(RtlAutoPpmSource::SpectrumResidual));
    rc |= expect_double_close("cqpsk bootstrap error", estimate.error_hz, 240.0, 1e-9);
    return rc;
}

static int
test_select_estimate_rejects_cqpsk_without_lock_or_spectrum(void) {
    int rc = 0;
    RtlAutoPpmSignalMetrics metrics = {};
    metrics.cqpsk_enable = 1;
    metrics.carrier_lock = 0;
    metrics.nco_cfo_hz = 950.0;
    metrics.spectrum_cfo_hz = 240.0;

    RtlAutoPpmEstimate estimate = rtl_auto_ppm_select_estimate(metrics);
    rc |= expect_int_eq("cqpsk no-lock source", static_cast<int>(estimate.source),
                        static_cast<int>(RtlAutoPpmSource::None));
    rc |= expect_double_close("cqpsk no-lock error", estimate.error_hz, 0.0, 1e-9);
    return rc;
}

static int
test_select_estimate_prefers_spectrum_over_phase_for_non_cqpsk(void) {
    int rc = 0;
    RtlAutoPpmSignalMetrics metrics = {};
    metrics.spectrum_valid = 1;
    metrics.phase_cfo_hz = -320.0;
    metrics.spectrum_cfo_hz = 90.0;

    RtlAutoPpmEstimate estimate = rtl_auto_ppm_select_estimate(metrics);
    rc |= expect_int_eq("spectrum source", static_cast<int>(estimate.source),
                        static_cast<int>(RtlAutoPpmSource::SpectrumResidual));
    rc |= expect_double_close("spectrum error", estimate.error_hz, 90.0, 1e-9);
    return rc;
}

static int
test_select_estimate_rejects_invalid_non_cqpsk_spectrum(void) {
    int rc = 0;
    RtlAutoPpmSignalMetrics metrics = {};
    metrics.phase_cfo_hz = -320.0;
    metrics.spectrum_cfo_hz = 90.0;

    RtlAutoPpmEstimate estimate = rtl_auto_ppm_select_estimate(metrics);
    rc |= expect_int_eq("invalid spectrum source", static_cast<int>(estimate.source),
                        static_cast<int>(RtlAutoPpmSource::None));
    rc |= expect_double_close("invalid spectrum error", estimate.error_hz, 0.0, 1e-9);
    return rc;
}

static int
test_select_estimate_rejects_phase_only_non_cqpsk(void) {
    int rc = 0;
    RtlAutoPpmSignalMetrics metrics = {};
    metrics.phase_cfo_hz = -320.0;
    metrics.spectrum_cfo_hz = std::nan("");

    RtlAutoPpmEstimate estimate = rtl_auto_ppm_select_estimate(metrics);
    rc |=
        expect_int_eq("phase-only source", static_cast<int>(estimate.source), static_cast<int>(RtlAutoPpmSource::None));
    rc |= expect_double_close("phase-only error", estimate.error_hz, 0.0, 1e-9);
    return rc;
}

static int
test_spectrum_estimate_requires_valid_spectral_snr(void) {
    int rc = 0;
    RtlAutoPpmController controller;
    RtlAutoPpmConfig config = {};
    const uint32_t freq_hz = 460000000U;

    controller.reset(0, freq_hz);

    RtlAutoPpmInputs inputs = make_inputs(0, 0, freq_hz, -6.0, RtlAutoPpmSource::SpectrumResidual);
    inputs.gate_snr_db = 18.0;
    inputs.spec_snr_db = -100.0;

    (void)controller.update(config, inputs);
    inputs.now_ms = 9000;
    RtlAutoPpmUpdate update = controller.update(config, inputs);
    rc |= expect_int_eq("invalid spectrum does not apply", update.apply_ppm, 0);
    rc |= expect_int_eq("invalid spectrum stays unlocked", update.locked, 0);
    rc |= expect_int_eq("invalid spectrum is not training", update.training, 0);
    rc |= expect_double_close("invalid spectrum uses spectral snr", update.snr_db, -100.0, 1e-9);
    return rc;
}

static int
test_carrier_estimate_applies_direct_correction_and_locks(void) {
    int rc = 0;
    RtlAutoPpmController controller;
    RtlAutoPpmConfig config = {};
    const uint32_t freq_hz = 851000000U;

    controller.reset(0, freq_hz);

    RtlAutoPpmUpdate update = {};
    update = controller.update(config, make_inputs(0, 0, freq_hz, -8.0, RtlAutoPpmSource::CarrierTotal));
    rc |= expect_int_eq("initial carrier step not immediate", update.apply_ppm, 0);
    update = controller.update(config, make_inputs(4000, 0, freq_hz, -8.0, RtlAutoPpmSource::CarrierTotal));
    rc |= expect_int_eq("carrier step applies after observation", update.apply_ppm, 1);
    rc |= expect_int_eq("carrier step follows residual sign", update.new_ppm, -8);
    rc |= expect_int_eq("carrier step enters training cooldown", update.training, 1);

    update = controller.update(config, make_inputs(5000, -8, freq_hz, 0.02, RtlAutoPpmSource::CarrierTotal));
    rc |= expect_int_eq("settle keeps training active", update.training, 1);
    rc |= expect_true("settle exposes cooldown", update.cooldown_ticks > 0);

    update = controller.update(config, make_inputs(7600, -8, freq_hz, 0.02, RtlAutoPpmSource::CarrierTotal));
    rc |= expect_int_eq("post-settle zero error keeps training", update.training, 1);
    update = controller.update(config, make_inputs(10650, -8, freq_hz, 0.02, RtlAutoPpmSource::CarrierTotal));
    rc |= expect_int_eq("stable zero locks", update.locked, 1);
    rc |= expect_int_eq("lock stores corrected ppm", update.lock_ppm, -8);
    return rc;
}

static int
test_spectrum_fallback_clamps_large_steps(void) {
    int rc = 0;
    RtlAutoPpmController controller;
    RtlAutoPpmConfig config = {};
    const uint32_t freq_hz = 935000000U;

    controller.reset(0, freq_hz);
    (void)controller.update(config, make_inputs(0, 0, freq_hz, -20.0, RtlAutoPpmSource::SpectrumResidual));
    RtlAutoPpmUpdate update =
        controller.update(config, make_inputs(8000, 0, freq_hz, -20.0, RtlAutoPpmSource::SpectrumResidual));
    rc |= expect_int_eq("spectrum fallback applies", update.apply_ppm, 1);
    rc |= expect_int_eq("spectrum fallback clamps step", update.new_ppm, -4);
    return rc;
}

static int
test_external_ppm_change_resets_observation_window(void) {
    int rc = 0;
    RtlAutoPpmController controller;
    RtlAutoPpmConfig config = {};
    const uint32_t freq_hz = 460000000U;

    controller.reset(0, freq_hz);
    (void)controller.update(config, make_inputs(0, 0, freq_hz, -6.0, RtlAutoPpmSource::PhaseResidual));
    RtlAutoPpmUpdate update =
        controller.update(config, make_inputs(3000, 2, freq_hz, -6.0, RtlAutoPpmSource::PhaseResidual));
    rc |= expect_int_eq("external ppm change clears immediate apply", update.apply_ppm, 0);
    update = controller.update(config, make_inputs(8001, 2, freq_hz, -6.0, RtlAutoPpmSource::PhaseResidual));
    rc |= expect_int_eq("new observation can apply after reset", update.apply_ppm, 1);
    rc |= expect_int_eq("new observation builds on external ppm", update.new_ppm, -4);
    return rc;
}

static int
test_positive_residual_increases_applied_ppm(void) {
    int rc = 0;
    RtlAutoPpmController controller;
    RtlAutoPpmConfig config = {};
    const uint32_t freq_hz = 771000000U;

    controller.reset(12, freq_hz);
    (void)controller.update(config, make_inputs(0, 12, freq_hz, 5.0, RtlAutoPpmSource::PhaseResidual));
    RtlAutoPpmUpdate update =
        controller.update(config, make_inputs(5000, 12, freq_hz, 5.0, RtlAutoPpmSource::PhaseResidual));
    rc |= expect_int_eq("positive residual applies", update.apply_ppm, 1);
    rc |= expect_int_eq("positive residual adds to ppm", update.new_ppm, 17);
    return rc;
}

static int
test_deadband_reentry_restarts_observation_window(void) {
    int rc = 0;
    RtlAutoPpmController controller;
    RtlAutoPpmConfig config = {};
    const uint32_t freq_hz = 771000000U;

    controller.reset(0, freq_hz);
    (void)controller.update(config, make_inputs(0, 0, freq_hz, -0.55, RtlAutoPpmSource::PhaseResidual));
    (void)controller.update(config, make_inputs(3000, 0, freq_hz, -0.10, RtlAutoPpmSource::PhaseResidual));

    RtlAutoPpmUpdate update =
        controller.update(config, make_inputs(7000, 0, freq_hz, -1.0, RtlAutoPpmSource::PhaseResidual));
    rc |= expect_int_eq("deadband reentry does not apply immediately", update.apply_ppm, 0);

    update = controller.update(config, make_inputs(12001, 0, freq_hz, -1.0, RtlAutoPpmSource::PhaseResidual));
    rc |= expect_int_eq("deadband reentry applies after fresh observation", update.apply_ppm, 1);
    rc |= expect_int_eq("deadband reentry correction direction", update.new_ppm, -1);
    return rc;
}

static int
test_unlock_preserves_last_lock_snapshot(void) {
    int rc = 0;
    RtlAutoPpmController controller;
    RtlAutoPpmConfig config = {};
    const uint32_t freq_hz = 851000000U;

    controller.reset(0, freq_hz);
    (void)controller.update(config, make_inputs(0, 0, freq_hz, -8.0, RtlAutoPpmSource::CarrierTotal));
    (void)controller.update(config, make_inputs(4000, 0, freq_hz, -8.0, RtlAutoPpmSource::CarrierTotal));
    (void)controller.update(config, make_inputs(7600, -8, freq_hz, 0.02, RtlAutoPpmSource::CarrierTotal));
    RtlAutoPpmUpdate locked =
        controller.update(config, make_inputs(10650, -8, freq_hz, 0.02, RtlAutoPpmSource::CarrierTotal));
    rc |= expect_int_eq("lock snapshot setup reaches lock", locked.locked, 1);

    RtlAutoPpmUpdate update =
        controller.update(config, make_inputs(11000, -8, freq_hz, -2.0, RtlAutoPpmSource::CarrierTotal));
    rc |= expect_int_eq("unlock clears active locked flag", update.locked, 0);
    rc |= expect_int_eq("unlock preserves lock ppm", update.lock_ppm, locked.lock_ppm);
    rc |= expect_double_close("unlock preserves lock snr", update.lock_snr_db, locked.lock_snr_db, 1e-9);
    rc |= expect_double_close("unlock preserves lock df", update.lock_df_hz, locked.lock_df_hz, 1e-9);
    return rc;
}

static int
test_frequency_change_clears_cooldown(void) {
    int rc = 0;
    RtlAutoPpmController controller;
    RtlAutoPpmConfig config = {};
    const uint32_t freq_a_hz = 851000000U;
    const uint32_t freq_b_hz = 935000000U;

    controller.reset(0, freq_a_hz);
    (void)controller.update(config, make_inputs(0, 0, freq_a_hz, -8.0, RtlAutoPpmSource::CarrierTotal));
    RtlAutoPpmUpdate update =
        controller.update(config, make_inputs(4000, 0, freq_a_hz, -8.0, RtlAutoPpmSource::CarrierTotal));
    rc |= expect_int_eq("cooldown setup applies initial correction", update.apply_ppm, 1);

    update = controller.update(config, make_inputs(4500, -8, freq_b_hz, -8.0, RtlAutoPpmSource::CarrierTotal));
    rc |= expect_int_eq("retune clears old cooldown ticks", update.cooldown_ticks, 0);
    rc |= expect_int_eq("retune starts fresh observation", update.apply_ppm, 0);

    update = controller.update(config, make_inputs(8500, -8, freq_b_hz, -8.0, RtlAutoPpmSource::CarrierTotal));
    rc |= expect_int_eq("retune observation can apply on new channel", update.apply_ppm, 1);
    rc |= expect_int_eq("retune observation applies from current ppm", update.new_ppm, -16);
    return rc;
}

static int
test_frequency_change_preserves_lock_state(void) {
    int rc = 0;
    RtlAutoPpmController controller;
    RtlAutoPpmConfig config = {};
    const uint32_t freq_a_hz = 851000000U;
    const uint32_t freq_b_hz = 935000000U;

    controller.reset(0, freq_a_hz);
    (void)controller.update(config, make_inputs(0, 0, freq_a_hz, -8.0, RtlAutoPpmSource::CarrierTotal));
    (void)controller.update(config, make_inputs(4000, 0, freq_a_hz, -8.0, RtlAutoPpmSource::CarrierTotal));
    (void)controller.update(config, make_inputs(7600, -8, freq_a_hz, 0.02, RtlAutoPpmSource::CarrierTotal));
    RtlAutoPpmUpdate locked =
        controller.update(config, make_inputs(10650, -8, freq_a_hz, 0.02, RtlAutoPpmSource::CarrierTotal));
    rc |= expect_int_eq("lock setup reaches zero lock", locked.locked, 1);
    rc |= expect_int_eq("lock setup stores corrected ppm", locked.lock_ppm, -8);

    RtlAutoPpmInputs invalid_inputs = {};
    invalid_inputs.now_ms = 10660;
    invalid_inputs.enabled = 1;
    invalid_inputs.current_ppm = -8;
    invalid_inputs.tuned_freq_hz = freq_b_hz;
    invalid_inputs.signal_power_db = -40.0;
    invalid_inputs.gate_snr_db = 12.0;
    invalid_inputs.spec_snr_db = 10.0;

    RtlAutoPpmUpdate update = controller.update(config, invalid_inputs);
    rc |= expect_int_eq("retune preserves locked flag", update.locked, 1);
    rc |= expect_int_eq("retune stays out of training", update.training, 0);
    rc |= expect_int_eq("retune preserves stored lock ppm", update.lock_ppm, -8);
    rc |= expect_double_close("retune preserves stored lock snr", update.lock_snr_db, locked.lock_snr_db, 1e-9);
    rc |= expect_double_close("retune preserves stored lock df", update.lock_df_hz, locked.lock_df_hz, 1e-9);

    update = controller.update(config, make_inputs(10700, -8, freq_b_hz, 0.02, RtlAutoPpmSource::CarrierTotal));
    rc |= expect_int_eq("valid post-retune zero error stays locked", update.locked, 1);
    rc |= expect_int_eq("valid post-retune zero error stays out of training", update.training, 0);
    return rc;
}

static int
test_zero_error_retraining_clears_internal_lock(void) {
    int rc = 0;
    RtlAutoPpmController controller;
    RtlAutoPpmConfig config = {};
    const uint32_t freq_hz = 851000000U;

    controller.reset(0, freq_hz);
    (void)controller.update(config, make_inputs(0, 0, freq_hz, -8.0, RtlAutoPpmSource::CarrierTotal));
    (void)controller.update(config, make_inputs(4000, 0, freq_hz, -8.0, RtlAutoPpmSource::CarrierTotal));
    (void)controller.update(config, make_inputs(7600, -8, freq_hz, 0.02, RtlAutoPpmSource::CarrierTotal));
    RtlAutoPpmUpdate locked =
        controller.update(config, make_inputs(10650, -8, freq_hz, 0.02, RtlAutoPpmSource::CarrierTotal));
    rc |= expect_int_eq("retraining setup reaches lock", locked.locked, 1);

    RtlAutoPpmUpdate update =
        controller.update(config, make_inputs(10700, -8, freq_hz, 0.02, RtlAutoPpmSource::SpectrumResidual));
    rc |= expect_int_eq("source change starts retraining", update.training, 1);
    rc |= expect_int_eq("source change clears active lock", update.locked, 0);
    rc |= expect_int_eq("source change preserves last lock snapshot", update.lock_ppm, locked.lock_ppm);

    RtlAutoPpmInputs invalid_inputs = {};
    invalid_inputs.now_ms = 10710;
    invalid_inputs.enabled = 1;
    invalid_inputs.current_ppm = -8;
    invalid_inputs.tuned_freq_hz = freq_hz;
    invalid_inputs.signal_power_db = -40.0;
    invalid_inputs.gate_snr_db = 12.0;
    invalid_inputs.spec_snr_db = 10.0;

    update = controller.update(config, invalid_inputs);
    rc |= expect_int_eq("missing estimate does not resurrect lock", update.locked, 0);
    rc |= expect_int_eq("missing estimate preserves last lock snapshot", update.lock_ppm, locked.lock_ppm);
    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_select_estimate_accepts_large_finite_cqpsk_nco();
    rc |= test_select_estimate_prefers_cqpsk_nco_after_lock();
    rc |= test_select_estimate_uses_cqpsk_spectrum_before_lock();
    rc |= test_select_estimate_rejects_cqpsk_without_lock_or_spectrum();
    rc |= test_select_estimate_prefers_spectrum_over_phase_for_non_cqpsk();
    rc |= test_select_estimate_rejects_invalid_non_cqpsk_spectrum();
    rc |= test_select_estimate_rejects_phase_only_non_cqpsk();
    rc |= test_spectrum_estimate_requires_valid_spectral_snr();
    rc |= test_carrier_estimate_applies_direct_correction_and_locks();
    rc |= test_spectrum_fallback_clamps_large_steps();
    rc |= test_external_ppm_change_resets_observation_window();
    rc |= test_positive_residual_increases_applied_ppm();
    rc |= test_deadband_reentry_restarts_observation_window();
    rc |= test_unlock_preserves_last_lock_snapshot();
    rc |= test_frequency_change_clears_cooldown();
    rc |= test_frequency_change_preserves_lock_state();
    rc |= test_zero_error_retraining_clears_internal_lock();
    return rc ? 1 : 0;
}
