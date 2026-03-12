// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <cmath>
#include <cstdio>
#include <stdint.h>

#include "rtl_auto_ppm.h"
#include "rtl_ppm_request.h"

using dsd::io::radio::rtl_auto_ppm_select_estimate;
using dsd::io::radio::RtlAutoPpmConfig;
using dsd::io::radio::RtlAutoPpmController;
using dsd::io::radio::RtlAutoPpmEstimate;
using dsd::io::radio::RtlAutoPpmInputs;
using dsd::io::radio::RtlAutoPpmSignalMetrics;
using dsd::io::radio::RtlAutoPpmSource;
using dsd::io::radio::RtlAutoPpmUpdate;

static RtlAutoPpmEstimate
select_estimate(const RtlAutoPpmSignalMetrics& metrics) {
    return rtl_auto_ppm_select_estimate(metrics);
}

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
    inputs.requested_ppm = current_ppm;
    inputs.tuned_freq_hz = tuned_freq_hz;
    inputs.signal_power_db = -40.0;
    inputs.gate_snr_db = 12.0;
    inputs.spec_snr_db = 10.0;
    inputs.estimate.source = source;
    inputs.estimate.error_hz = est_ppm * static_cast<double>(tuned_freq_hz) / 1.0e6;
    return inputs;
}

static dsd::io::radio::RtlPpmControllerRequestState
make_request_state(int pending, int ppm, uint32_t request_id) {
    dsd::io::radio::RtlPpmControllerRequestState state = {};
    state.pending = pending;
    state.ppm = ppm;
    state.request_id = request_id;
    return state;
}

static int
test_select_estimate_accepts_large_finite_cqpsk_nco(void) {
    int rc = 0;
    RtlAutoPpmSignalMetrics metrics = {};
    metrics.cqpsk_enable = 1;
    metrics.tracking_enable = 1;
    metrics.carrier_lock = 1;
    metrics.spectrum_valid = 1;
    metrics.nco_cfo_hz = -68000.0;
    metrics.spectrum_cfo_hz = 1800.0;

    RtlAutoPpmEstimate estimate = select_estimate(metrics);
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
    metrics.tracking_enable = 1;
    metrics.carrier_lock = 1;
    metrics.spectrum_valid = 1;
    metrics.nco_cfo_hz = 1200.0;
    metrics.spectrum_cfo_hz = 75.0;

    RtlAutoPpmEstimate estimate = select_estimate(metrics);
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
    metrics.tracking_enable = 1;
    metrics.spectrum_valid = 1;
    metrics.nco_cfo_hz = 950.0;
    metrics.spectrum_cfo_hz = 240.0;

    RtlAutoPpmEstimate estimate = select_estimate(metrics);
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
    metrics.tracking_enable = 1;
    metrics.nco_cfo_hz = 950.0;
    metrics.spectrum_cfo_hz = 240.0;

    RtlAutoPpmEstimate estimate = select_estimate(metrics);
    rc |= expect_int_eq("cqpsk no-lock source", static_cast<int>(estimate.source),
                        static_cast<int>(RtlAutoPpmSource::None));
    rc |= expect_double_close("cqpsk no-lock error", estimate.error_hz, 0.0, 1e-9);
    return rc;
}

static int
test_select_estimate_keeps_non_cqpsk_spectrum_with_tracking_enabled(void) {
    int rc = 0;
    RtlAutoPpmSignalMetrics metrics = {};
    metrics.tracking_enable = 1;
    metrics.spectrum_valid = 1;
    metrics.nco_cfo_hz = -320.0;
    metrics.phase_cfo_hz = -320.0;
    metrics.spectrum_cfo_hz = 40.0;

    RtlAutoPpmEstimate estimate = select_estimate(metrics);
    rc |= expect_int_eq("non-cqpsk spectrum source", static_cast<int>(estimate.source),
                        static_cast<int>(RtlAutoPpmSource::SpectrumResidual));
    rc |= expect_double_close("non-cqpsk spectrum error", estimate.error_hz, 40.0, 1e-9);
    return rc;
}

static int
test_select_estimate_keeps_non_cqpsk_spectrum_for_large_offsets(void) {
    int rc = 0;
    RtlAutoPpmSignalMetrics metrics = {};
    metrics.tracking_enable = 1;
    metrics.spectrum_valid = 1;
    metrics.nco_cfo_hz = 0.0;
    metrics.phase_cfo_hz = -320.0;
    metrics.spectrum_cfo_hz = 250.0;

    RtlAutoPpmEstimate estimate = select_estimate(metrics);
    rc |= expect_int_eq("fallback source", static_cast<int>(estimate.source),
                        static_cast<int>(RtlAutoPpmSource::SpectrumResidual));
    rc |= expect_double_close("fallback error", estimate.error_hz, 250.0, 1e-9);
    return rc;
}

static int
test_select_estimate_rejects_invalid_non_cqpsk_spectrum(void) {
    int rc = 0;
    RtlAutoPpmSignalMetrics metrics = {};
    metrics.tracking_enable = 1;
    metrics.nco_cfo_hz = -320.0;
    metrics.phase_cfo_hz = -320.0;
    metrics.spectrum_cfo_hz = 90.0;

    RtlAutoPpmEstimate estimate = select_estimate(metrics);
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

    RtlAutoPpmEstimate estimate = select_estimate(metrics);
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
test_carrier_estimate_applies_direct_correction_then_locks(void) {
    int rc = 0;
    RtlAutoPpmController controller;
    RtlAutoPpmConfig config = {};
    const uint32_t freq_hz = 851000000U;

    controller.reset(0, freq_hz);

    RtlAutoPpmUpdate update =
        controller.update(config, make_inputs(0, 0, freq_hz, -8.0, RtlAutoPpmSource::CarrierTotal));
    rc |= expect_int_eq("initial carrier step not immediate", update.apply_ppm, 0);

    update = controller.update(config, make_inputs(4000, 0, freq_hz, -8.0, RtlAutoPpmSource::CarrierTotal));
    rc |= expect_int_eq("carrier step applies after observation", update.apply_ppm, 1);
    rc |= expect_int_eq("carrier step follows residual sign", update.new_ppm, -8);
    rc |= expect_int_eq("carrier step keeps training active", update.training, 1);
    rc |= expect_int_eq("carrier step stays unlocked", update.locked, 0);
    rc |= expect_true("carrier step enters settle cooldown", update.cooldown_ticks > 0);

    update = controller.update(config, make_inputs(5000, -8, freq_hz, 0.02, RtlAutoPpmSource::CarrierTotal));
    rc |= expect_int_eq("settle keeps training active", update.training, 1);
    rc |= expect_true("settle still exposes cooldown", update.cooldown_ticks > 0);

    update = controller.update(config, make_inputs(6500, -8, freq_hz, 0.02, RtlAutoPpmSource::CarrierTotal));
    rc |= expect_int_eq("post-settle zero error keeps training", update.training, 1);
    rc |= expect_int_eq("post-settle zero error stays unlocked", update.locked, 0);

    update = controller.update(config, make_inputs(9500, -8, freq_hz, 0.02, RtlAutoPpmSource::CarrierTotal));
    rc |= expect_int_eq("stable zero locks", update.locked, 1);
    rc |= expect_int_eq("stable zero exits training", update.training, 0);
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
    rc |= expect_int_eq("spectrum fallback remains in training", update.training, 1);
    rc |= expect_int_eq("spectrum fallback stays unlocked", update.locked, 0);
    return rc;
}

static int
test_large_spectrum_residual_stays_bounded_across_windows(void) {
    int rc = 0;
    RtlAutoPpmController controller;
    RtlAutoPpmConfig config = {};
    const uint32_t freq_hz = 935000000U;

    controller.reset(0, freq_hz);
    (void)controller.update(config, make_inputs(0, 0, freq_hz, -60.0, RtlAutoPpmSource::SpectrumResidual));

    RtlAutoPpmUpdate update =
        controller.update(config, make_inputs(8000, 0, freq_hz, -60.0, RtlAutoPpmSource::SpectrumResidual));
    rc |= expect_int_eq("bounded step applies", update.apply_ppm, 1);
    rc |= expect_int_eq("bounded step clamps to spectrum max", update.new_ppm, -4);
    rc |= expect_int_eq("bounded step keeps controller unlocked", update.locked, 0);
    rc |= expect_int_eq("bounded step keeps training active", update.training, 1);

    update = controller.update(config, make_inputs(10501, -4, freq_hz, -56.0, RtlAutoPpmSource::SpectrumResidual));
    rc |= expect_int_eq("post-bounded settle does not reapply immediately", update.apply_ppm, 0);
    rc |= expect_int_eq("post-bounded settle remains in training", update.training, 1);

    update = controller.update(config, make_inputs(18502, -4, freq_hz, -56.0, RtlAutoPpmSource::SpectrumResidual));
    rc |= expect_int_eq("residual can trigger another bounded step", update.apply_ppm, 1);
    rc |= expect_int_eq("second bounded step continues converging", update.new_ppm, -8);
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
    rc |= expect_int_eq("new observation remains in training", update.training, 1);
    rc |= expect_int_eq("new observation stays unlocked", update.locked, 0);
    return rc;
}

static int
test_async_ppm_waits_for_applied_snapshot_before_retraining(void) {
    int rc = 0;
    RtlAutoPpmController controller;
    RtlAutoPpmConfig config = {};
    const uint32_t freq_hz = 851000000U;

    controller.reset(0, freq_hz);
    (void)controller.update(config, make_inputs(0, 0, freq_hz, -8.0, RtlAutoPpmSource::CarrierTotal));

    RtlAutoPpmUpdate update =
        controller.update(config, make_inputs(4000, 0, freq_hz, -8.0, RtlAutoPpmSource::CarrierTotal));
    rc |= expect_int_eq("async wait queues correction", update.apply_ppm, 1);
    rc |= expect_int_eq("async wait targets current applied ppm", update.new_ppm, -8);

    update = controller.update(config, make_inputs(9000, 0, freq_hz, -8.0, RtlAutoPpmSource::CarrierTotal));
    rc |= expect_int_eq("async wait does not reapply before hardware ack", update.apply_ppm, 0);
    rc |= expect_int_eq("async wait stays in training", update.training, 1);
    rc |= expect_int_eq("async wait stays unlocked", update.locked, 0);

    update = controller.update(config, make_inputs(9500, -8, freq_hz, 0.02, RtlAutoPpmSource::CarrierTotal));
    rc |= expect_int_eq("async wait resumes from applied ppm", update.apply_ppm, 0);
    rc |= expect_int_eq("async wait starts fresh post-apply observation", update.training, 1);
    rc |= expect_int_eq("async wait remains unlocked until hold time", update.locked, 0);

    update = controller.update(config, make_inputs(12501, -8, freq_hz, 0.02, RtlAutoPpmSource::CarrierTotal));
    rc |= expect_int_eq("async wait can lock after applied ppm settles", update.locked, 1);
    rc |= expect_int_eq("async wait lock stores applied ppm", update.lock_ppm, -8);
    return rc;
}

static int
test_requested_ppm_change_blocks_retraining_until_applied(void) {
    int rc = 0;
    RtlAutoPpmController controller;
    RtlAutoPpmConfig config = {};
    const uint32_t freq_hz = 851000000U;

    controller.reset(0, freq_hz);
    (void)controller.update(config, make_inputs(0, 0, freq_hz, -8.0, RtlAutoPpmSource::CarrierTotal));

    RtlAutoPpmUpdate update =
        controller.update(config, make_inputs(4000, 0, freq_hz, -8.0, RtlAutoPpmSource::CarrierTotal));
    rc |= expect_int_eq("auto step queues before manual override", update.apply_ppm, 1);
    rc |= expect_int_eq("auto step baseline before manual override", update.new_ppm, -8);

    RtlAutoPpmInputs manual_pending = make_inputs(4100, 0, freq_hz, -8.0, RtlAutoPpmSource::CarrierTotal);
    manual_pending.requested_ppm = 5;
    update = controller.update(config, manual_pending);
    rc |= expect_int_eq("manual pending suppresses auto reapply", update.apply_ppm, 0);
    rc |= expect_int_eq("manual pending keeps training active", update.training, 1);
    rc |= expect_int_eq("manual pending keeps controller unlocked", update.locked, 0);

    manual_pending.now_ms = 8000;
    update = controller.update(config, manual_pending);
    rc |= expect_int_eq("manual pending continues blocking retrain", update.apply_ppm, 0);

    update = controller.update(config, make_inputs(9000, 5, freq_hz, -8.0, RtlAutoPpmSource::CarrierTotal));
    rc |= expect_int_eq("manual apply ack restarts observation", update.apply_ppm, 0);
    rc |= expect_int_eq("manual apply ack stays in training", update.training, 1);

    update = controller.update(config, make_inputs(12999, 5, freq_hz, -8.0, RtlAutoPpmSource::CarrierTotal));
    rc |= expect_int_eq("manual apply requires fresh observation window", update.apply_ppm, 0);

    update = controller.update(config, make_inputs(13000, 5, freq_hz, -8.0, RtlAutoPpmSource::CarrierTotal));
    rc |= expect_int_eq("manual apply can retrain after new window", update.apply_ppm, 1);
    rc |= expect_int_eq("manual apply builds on manual baseline", update.new_ppm, -3);
    return rc;
}

static int
test_canceled_requested_ppm_waits_for_controller_request_to_clear(void) {
    int rc = 0;
    RtlAutoPpmController controller;
    RtlAutoPpmConfig config = {};
    const uint32_t freq_hz = 851000000U;

    controller.reset(0, freq_hz);
    (void)controller.update(config, make_inputs(0, 0, freq_hz, -8.0, RtlAutoPpmSource::CarrierTotal));

    RtlAutoPpmUpdate update =
        controller.update(config, make_inputs(4000, 0, freq_hz, -8.0, RtlAutoPpmSource::CarrierTotal));
    rc |= expect_int_eq("cancel setup queues correction", update.apply_ppm, 1);
    rc |= expect_int_eq("cancel setup targets requested correction", update.new_ppm, -8);

    RtlAutoPpmInputs canceled = make_inputs(4500, 0, freq_hz, -8.0, RtlAutoPpmSource::CarrierTotal);
    canceled.requested_ppm = 0;
    canceled.controller_queued_request = make_request_state(1, -8, 1);
    update = controller.update(config, canceled);
    rc |= expect_int_eq("cancel does not clear while request still live", update.apply_ppm, 0);
    rc |= expect_int_eq("cancel keeps controller training while live", update.training, 1);
    rc |= expect_true("cancel keeps cooldown while live request exists", update.cooldown_ticks > 0);

    canceled.now_ms = 7000;
    canceled.controller_queued_request = make_request_state(0, 0, 0);
    update = controller.update(config, canceled);
    rc |= expect_int_eq("cancel clears once request is gone", update.apply_ppm, 0);
    rc |= expect_int_eq("cancel restarts training after request clears", update.training, 1);
    rc |= expect_int_eq("cancel clears cooldown after request clears", update.cooldown_ticks, 0);

    update = controller.update(config, make_inputs(10999, 0, freq_hz, -8.0, RtlAutoPpmSource::CarrierTotal));
    rc |= expect_int_eq("cancel needs full fresh observation", update.apply_ppm, 0);

    update = controller.update(config, make_inputs(11000, 0, freq_hz, -8.0, RtlAutoPpmSource::CarrierTotal));
    rc |= expect_int_eq("cancel allows retraining after fresh window", update.apply_ppm, 1);
    rc |= expect_int_eq("cancel reuses current applied ppm baseline", update.new_ppm, -8);
    return rc;
}

static int
test_overlapped_active_and_queued_requests_keep_training_hold(void) {
    int rc = 0;
    RtlAutoPpmController controller;
    RtlAutoPpmConfig config = {};
    const uint32_t freq_hz = 851000000U;

    controller.reset(0, freq_hz);
    (void)controller.update(config, make_inputs(0, 0, freq_hz, -8.0, RtlAutoPpmSource::CarrierTotal));

    RtlAutoPpmUpdate update =
        controller.update(config, make_inputs(4000, 0, freq_hz, -8.0, RtlAutoPpmSource::CarrierTotal));
    rc |= expect_int_eq("overlap setup queues correction", update.apply_ppm, 1);
    rc |= expect_int_eq("overlap setup targets requested correction", update.new_ppm, -8);

    RtlAutoPpmInputs overlapped = make_inputs(4500, 0, freq_hz, -8.0, RtlAutoPpmSource::CarrierTotal);
    overlapped.requested_ppm = 0;
    overlapped.controller_active_request = make_request_state(1, 5, 10);
    overlapped.controller_queued_request = make_request_state(1, -8, 11);
    update = controller.update(config, overlapped);
    rc |= expect_int_eq("overlap keeps queued correction from disappearing", update.apply_ppm, 0);
    rc |= expect_int_eq("overlap keeps training active while queued request exists", update.training, 1);
    rc |= expect_int_eq("overlap keeps controller unlocked", update.locked, 0);
    rc |= expect_true("overlap keeps cooldown while queued request exists", update.cooldown_ticks > 0);

    update = controller.update(config, make_inputs(7000, -8, freq_hz, 0.02, RtlAutoPpmSource::CarrierTotal));
    rc |= expect_int_eq("overlap apply ack restarts observation", update.apply_ppm, 0);
    rc |= expect_int_eq("overlap apply ack stays in training", update.training, 1);
    rc |= expect_int_eq("overlap apply ack remains unlocked", update.locked, 0);
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
    rc |= expect_int_eq("positive residual remains in training", update.training, 1);
    rc |= expect_int_eq("positive residual stays unlocked", update.locked, 0);
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
test_locked_session_reenters_training_on_same_channel_drift(void) {
    int rc = 0;
    RtlAutoPpmController controller;
    RtlAutoPpmConfig config = {};
    const uint32_t freq_hz = 851000000U;

    controller.reset(0, freq_hz);
    (void)controller.update(config, make_inputs(0, 0, freq_hz, -8.0, RtlAutoPpmSource::CarrierTotal));
    (void)controller.update(config, make_inputs(4000, 0, freq_hz, -8.0, RtlAutoPpmSource::CarrierTotal));
    (void)controller.update(config, make_inputs(6500, -8, freq_hz, 0.02, RtlAutoPpmSource::CarrierTotal));

    RtlAutoPpmUpdate locked =
        controller.update(config, make_inputs(9500, -8, freq_hz, 0.02, RtlAutoPpmSource::CarrierTotal));
    rc |= expect_int_eq("same-channel drift setup reaches lock", locked.locked, 1);

    RtlAutoPpmUpdate update =
        controller.update(config, make_inputs(10000, -8, freq_hz, -2.0, RtlAutoPpmSource::CarrierTotal));
    rc |= expect_int_eq("same-channel drift clears active lock", update.locked, 0);
    rc |= expect_int_eq("same-channel drift resumes training", update.training, 1);
    rc |= expect_int_eq("same-channel drift does not apply immediately", update.apply_ppm, 0);
    rc |= expect_int_eq("same-channel drift preserves last lock ppm", update.lock_ppm, locked.lock_ppm);
    rc |=
        expect_double_close("same-channel drift preserves last lock snr", update.lock_snr_db, locked.lock_snr_db, 1e-9);
    rc |= expect_double_close("same-channel drift preserves last lock df", update.lock_df_hz, locked.lock_df_hz, 1e-9);

    update = controller.update(config, make_inputs(14001, -8, freq_hz, -2.0, RtlAutoPpmSource::CarrierTotal));
    rc |= expect_int_eq("same-channel drift can apply again", update.apply_ppm, 1);
    rc |= expect_int_eq("same-channel drift correction uses current ppm", update.new_ppm, -10);
    rc |= expect_int_eq("same-channel drift correction keeps training active", update.training, 1);
    rc |= expect_int_eq("same-channel drift correction stays unlocked", update.locked, 0);
    return rc;
}

static int
test_locked_session_keeps_sub_deadband_same_channel_drift_locked(void) {
    int rc = 0;
    RtlAutoPpmController controller;
    RtlAutoPpmConfig config = {};
    const uint32_t freq_hz = 851000000U;

    controller.reset(0, freq_hz);
    (void)controller.update(config, make_inputs(0, 0, freq_hz, -8.0, RtlAutoPpmSource::CarrierTotal));
    (void)controller.update(config, make_inputs(4000, 0, freq_hz, -8.0, RtlAutoPpmSource::CarrierTotal));
    (void)controller.update(config, make_inputs(6500, -8, freq_hz, 0.02, RtlAutoPpmSource::CarrierTotal));

    RtlAutoPpmUpdate locked =
        controller.update(config, make_inputs(9500, -8, freq_hz, 0.02, RtlAutoPpmSource::CarrierTotal));
    rc |= expect_int_eq("moderate drift setup reaches lock", locked.locked, 1);

    RtlAutoPpmUpdate update =
        controller.update(config, make_inputs(10000, -8, freq_hz, 0.3, RtlAutoPpmSource::CarrierTotal));
    rc |= expect_int_eq("moderate drift first sample keeps lock", update.locked, 1);
    rc |= expect_int_eq("moderate drift first sample keeps training idle", update.training, 0);
    rc |= expect_double_close("moderate drift first sample exports filtered ppm", update.est_ppm, 0.3, 1e-9);
    rc |= expect_double_close("moderate drift first sample exports filtered df", update.df_hz, 255.3, 1e-6);

    update = controller.update(config, make_inputs(10500, -8, freq_hz, 0.3, RtlAutoPpmSource::CarrierTotal));
    rc |= expect_int_eq("sub-deadband drift above zero-lock hz keeps lock", update.locked, 1);
    rc |= expect_int_eq("sub-deadband drift above zero-lock hz stays out of training", update.training, 0);
    rc |= expect_int_eq("sub-deadband drift above zero-lock hz does not apply", update.apply_ppm, 0);
    rc |= expect_int_eq("sub-deadband drift above zero-lock hz preserves lock ppm", update.lock_ppm, locked.lock_ppm);
    rc |= expect_double_close("sub-deadband drift above zero-lock hz exports filtered ppm", update.est_ppm, 0.3, 1e-9);
    rc |= expect_double_close("sub-deadband drift above zero-lock hz exports filtered df", update.df_hz, 255.3, 1e-6);
    return rc;
}

static int
test_frequency_change_during_training_restarts_observation(void) {
    int rc = 0;
    RtlAutoPpmController controller;
    RtlAutoPpmConfig config = {};
    const uint32_t freq_a_hz = 851000000U;
    const uint32_t freq_b_hz = 935000000U;

    controller.reset(0, freq_a_hz);
    (void)controller.update(config, make_inputs(0, 0, freq_a_hz, -8.0, RtlAutoPpmSource::SpectrumResidual));

    RtlAutoPpmUpdate update =
        controller.update(config, make_inputs(3000, 0, freq_b_hz, -8.0, RtlAutoPpmSource::SpectrumResidual));
    rc |= expect_int_eq("retune during training does not apply immediately", update.apply_ppm, 0);
    rc |= expect_int_eq("retune during training keeps cooldown clear", update.cooldown_ticks, 0);

    update = controller.update(config, make_inputs(11001, 0, freq_b_hz, -8.0, RtlAutoPpmSource::SpectrumResidual));
    rc |= expect_int_eq("retune observation can apply on new channel", update.apply_ppm, 1);
    rc |= expect_int_eq("retune observation applies from current ppm", update.new_ppm, -4);
    rc |= expect_int_eq("retune observation stays unlocked", update.locked, 0);
    rc |= expect_int_eq("retune observation keeps training active", update.training, 1);
    return rc;
}

static int
test_frequency_change_rearms_drift_check_after_lock_carry(void) {
    int rc = 0;
    RtlAutoPpmController controller;
    RtlAutoPpmConfig config = {};
    const uint32_t freq_a_hz = 851000000U;
    const uint32_t freq_b_hz = 935000000U;

    controller.reset(0, freq_a_hz);
    (void)controller.update(config, make_inputs(0, 0, freq_a_hz, -8.0, RtlAutoPpmSource::CarrierTotal));
    (void)controller.update(config, make_inputs(4000, 0, freq_a_hz, -8.0, RtlAutoPpmSource::CarrierTotal));
    (void)controller.update(config, make_inputs(6500, -8, freq_a_hz, 0.02, RtlAutoPpmSource::CarrierTotal));

    RtlAutoPpmUpdate locked =
        controller.update(config, make_inputs(9500, -8, freq_a_hz, 0.02, RtlAutoPpmSource::CarrierTotal));
    rc |= expect_int_eq("lock setup reaches zero lock", locked.locked, 1);
    rc |= expect_int_eq("lock setup stores corrected ppm", locked.lock_ppm, -8);

    RtlAutoPpmInputs invalid_inputs = {};
    invalid_inputs.now_ms = 9560;
    invalid_inputs.enabled = 1;
    invalid_inputs.current_ppm = -8;
    invalid_inputs.requested_ppm = -8;
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

    update = controller.update(config, make_inputs(9700, -8, freq_b_hz, 3.5, RtlAutoPpmSource::CarrierTotal));
    rc |= expect_int_eq("new-channel drift clears carried lock", update.locked, 0);
    rc |= expect_int_eq("new-channel drift restarts training", update.training, 1);
    rc |= expect_int_eq("new-channel drift does not apply immediately", update.apply_ppm, 0);
    rc |= expect_int_eq("new-channel drift preserves last lock ppm", update.lock_ppm, locked.lock_ppm);
    rc |=
        expect_double_close("new-channel drift preserves last lock snr", update.lock_snr_db, locked.lock_snr_db, 1e-9);
    rc |= expect_double_close("new-channel drift preserves last lock df", update.lock_df_hz, locked.lock_df_hz, 1e-9);

    update = controller.update(config, make_inputs(13701, -8, freq_b_hz, 3.5, RtlAutoPpmSource::CarrierTotal));
    rc |= expect_int_eq("new-channel drift can retrain after carry", update.apply_ppm, 1);
    rc |= expect_int_eq("new-channel drift correction uses current ppm", update.new_ppm, -4);
    rc |= expect_int_eq("new-channel drift correction stays unlocked", update.locked, 0);
    rc |= expect_int_eq("new-channel drift correction keeps training active", update.training, 1);
    return rc;
}

static int
test_locked_session_resets_after_external_ppm_change(void) {
    int rc = 0;
    RtlAutoPpmController controller;
    RtlAutoPpmConfig config = {};
    const uint32_t freq_hz = 851000000U;

    controller.reset(0, freq_hz);
    (void)controller.update(config, make_inputs(0, 0, freq_hz, -8.0, RtlAutoPpmSource::CarrierTotal));
    (void)controller.update(config, make_inputs(4000, 0, freq_hz, -8.0, RtlAutoPpmSource::CarrierTotal));
    (void)controller.update(config, make_inputs(6500, -8, freq_hz, 0.02, RtlAutoPpmSource::CarrierTotal));

    RtlAutoPpmUpdate locked =
        controller.update(config, make_inputs(9500, -8, freq_hz, 0.02, RtlAutoPpmSource::CarrierTotal));
    rc |= expect_int_eq("manual-reset setup reaches lock", locked.locked, 1);

    RtlAutoPpmUpdate update =
        controller.update(config, make_inputs(10000, -4, freq_hz, -8.0, RtlAutoPpmSource::CarrierTotal));
    rc |= expect_int_eq("external ppm change clears active lock", update.locked, 0);
    rc |= expect_int_eq("external ppm change does not apply immediately", update.apply_ppm, 0);
    rc |= expect_int_eq("external ppm change preserves last lock snapshot", update.lock_ppm, locked.lock_ppm);
    rc |= expect_true("external ppm change resumes training", update.training != 0);

    update = controller.update(config, make_inputs(14001, -4, freq_hz, -8.0, RtlAutoPpmSource::CarrierTotal));
    rc |= expect_int_eq("rearmed training can apply again", update.apply_ppm, 1);
    rc |= expect_int_eq("rearmed training applies from external ppm", update.new_ppm, -12);
    rc |= expect_int_eq("rearmed correction stays unlocked", update.locked, 0);
    rc |= expect_int_eq("rearmed correction keeps training active", update.training, 1);
    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_select_estimate_accepts_large_finite_cqpsk_nco();
    rc |= test_select_estimate_prefers_cqpsk_nco_after_lock();
    rc |= test_select_estimate_uses_cqpsk_spectrum_before_lock();
    rc |= test_select_estimate_rejects_cqpsk_without_lock_or_spectrum();
    rc |= test_select_estimate_keeps_non_cqpsk_spectrum_with_tracking_enabled();
    rc |= test_select_estimate_keeps_non_cqpsk_spectrum_for_large_offsets();
    rc |= test_select_estimate_rejects_invalid_non_cqpsk_spectrum();
    rc |= test_select_estimate_rejects_phase_only_non_cqpsk();
    rc |= test_spectrum_estimate_requires_valid_spectral_snr();
    rc |= test_carrier_estimate_applies_direct_correction_then_locks();
    rc |= test_spectrum_fallback_clamps_large_steps();
    rc |= test_large_spectrum_residual_stays_bounded_across_windows();
    rc |= test_external_ppm_change_resets_observation_window();
    rc |= test_async_ppm_waits_for_applied_snapshot_before_retraining();
    rc |= test_requested_ppm_change_blocks_retraining_until_applied();
    rc |= test_canceled_requested_ppm_waits_for_controller_request_to_clear();
    rc |= test_overlapped_active_and_queued_requests_keep_training_hold();
    rc |= test_positive_residual_increases_applied_ppm();
    rc |= test_deadband_reentry_restarts_observation_window();
    rc |= test_locked_session_reenters_training_on_same_channel_drift();
    rc |= test_locked_session_keeps_sub_deadband_same_channel_drift_locked();
    rc |= test_frequency_change_during_training_restarts_observation();
    rc |= test_frequency_change_rearms_drift_check_after_lock_carry();
    rc |= test_locked_session_resets_after_external_ppm_change();
    return rc ? 1 : 0;
}
