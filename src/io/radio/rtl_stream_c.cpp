// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief C shim over the C++ RTL-SDR orchestrator for use by C code.
 *
 * Exposes a minimal C API that mirrors lifecycle, tuning, and I/O operations
 * of RtlSdrOrchestrator. Intended to allow incremental migration from the
 * legacy C control paths while preserving behavior.
 */

#include "dsd-neo/core/input_level.h"

#include <new>
#include <stdint.h>
#include <stdlib.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/io/rtl_stream_fwd.h"

extern "C" {
#include <dsd-neo/io/rtl_stream_c.h>

// Local forward declarations for legacy helpers used under the hood
void dsd_rtl_stream_clear_output(void);
double dsd_rtl_stream_return_pwr(void);
int dsd_rtl_stream_is_active(void);
int dsd_rtl_stream_cqpsk_timing_bias(void);
int dsd_rtl_stream_get_output_kind(void);
int dsd_rtl_stream_get_symbol_profile(int* out_symbol_rate_hz, int* out_levels);
int dsd_rtl_stream_get_symbol_profile_full(int* out_symbol_rate_hz, int* out_levels, int* out_channel_profile);
int dsd_rtl_stream_set_symbol_profile(int symbol_rate_hz, int levels, int channel_profile);
void dsd_rtl_stream_prepare_retune_profile(int cqpsk_enable, int symbol_rate_hz, int levels, int channel_profile,
                                           int ted_sps, int persist_ted_override);
void dsd_rtl_stream_prepare_retune_profile_for_target(uint32_t target_freq_hz, int cqpsk_enable, int symbol_rate_hz,
                                                      int levels, int channel_profile, int ted_sps,
                                                      int persist_ted_override);
void dsd_rtl_stream_prepare_retune_profile_for_target_with_gain(uint32_t target_freq_hz, int cqpsk_enable,
                                                                int symbol_rate_hz, int levels, int channel_profile,
                                                                int ted_sps, int persist_ted_override,
                                                                const rtl_stream_retune_gain_profile* gain_profile);
void dsd_rtl_stream_apply_pending_retune_profile(void);
void dsd_rtl_stream_apply_pending_retune_profile_for_target(uint32_t target_freq_hz);
void dsd_rtl_stream_clear_pending_retune_profile(void);
int dsd_rtl_stream_request_fsk_reacquire(void);
uint32_t dsd_rtl_stream_output_generation(void);
int dsd_rtl_stream_monitor_read(float* out, size_t count, int* out_got);
unsigned int dsd_rtl_stream_monitor_rate(void);
void dsd_rtl_stream_set_resampler_target(int target_hz);
int dsd_rtl_stream_get_ted_sps(void);
int dsd_rtl_stream_get_ted_sps_override(void);
void dsd_rtl_stream_set_ted_sps(int sps);
void dsd_rtl_stream_clear_ted_sps_override(void);
void dsd_rtl_stream_set_ted_sps_no_override(int sps);
void dsd_rtl_stream_set_ted_gain(float g);
float dsd_rtl_stream_get_ted_gain(void);
/* Bias tee control implemented in rtl_sdr_fm.cpp */
int dsd_rtl_stream_set_bias_tee(int on);
void dsd_rtl_stream_p25p2_err_update(int slot, int facch_ok_delta, int facch_err_delta, int sacch_ok_delta,
                                     int sacch_err_delta, int voice_err_delta);
int dsd_rtl_stream_eye_get(float* out, int max_samples, int* out_sps);
int dsd_rtl_stream_constellation_get(float* out_xy, int max_points);
int dsd_rtl_stream_spectrum_get(float* out_db, int max_bins, int* out_rate);
int dsd_rtl_stream_spectrum_set_size(int n);
int dsd_rtl_stream_spectrum_get_size(void);

/* Gain query helper implemented in rtl_sdr_fm.cpp */
int dsd_rtl_stream_get_gain(int* out_tenth_db, int* out_is_auto);
/* Auto-PPM runtime toggle */
void dsd_rtl_stream_set_auto_ppm(int onoff);
int dsd_rtl_stream_get_auto_ppm(void);
/* RTL-TCP autotune control */
int dsd_rtl_stream_set_rtltcp_autotune(int onoff);
int dsd_rtl_stream_get_rtltcp_autotune(void);
int dsd_rtl_stream_get_last_applied_freq(uint32_t* out_freq_hz);
/* Eye-based SNR fallback */
double dsd_rtl_stream_estimate_snr_c4fm_eye(void);
double dsd_rtl_stream_estimate_snr_qpsk_const(void);
double dsd_rtl_stream_estimate_snr_gfsk_eye(void);
/* SNR bias computation (dynamic, accounts for BW/sample rate) */
double dsd_rtl_stream_get_snr_bias_c4fm(void);
double dsd_rtl_stream_get_snr_bias_evm(void);
/* Tuner autogain */
int dsd_rtl_stream_get_tuner_autogain(void);
void dsd_rtl_stream_set_tuner_autogain(int onoff);
int dsd_rtl_stream_get_decode_health(rtl_stream_decode_health* out);
int dsd_rtl_stream_get_input_level(dsd_input_level_snapshot* out);
void dsd_rtl_stream_set_channel_squelch(float level);
void dsd_rtl_stream_toggle_iq_balance(int onoff);
int dsd_rtl_stream_get_iq_balance(void);
#if defined(DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS)
int dsd_rtl_stream_test_request_retune(long int frequency, int timeout_ms);
int dsd_rtl_stream_test_prepare_reconfigure_input(size_t queued_samples, size_t* out_used_after,
                                                  uint32_t* out_generation_before, uint32_t* out_generation_after);
int dsd_rtl_stream_test_retune_output_pending(size_t queued_samples, int cached_symbols, size_t* out_ring_pending,
                                              int* out_cache_pending, int* out_drained);
int dsd_rtl_stream_test_tune_result_output_drain(int tune_result, size_t queued_samples, int cached_symbols,
                                                 size_t* out_used_after, int* out_cache_pending_after,
                                                 uint32_t* out_generation_before, uint32_t* out_generation_after);
int dsd_rtl_stream_test_clear_output(size_t queued_samples, int cached_symbols, size_t* out_used_after,
                                     int* out_cache_pending_after, uint32_t* out_generation_before,
                                     uint32_t* out_generation_after);
int dsd_rtl_stream_test_clear_output_fsk_reset(size_t queued_samples, int* out_have_prev_after_clear,
                                               int* out_consumed_reset, int* out_have_prev_after_consume);
int dsd_rtl_stream_test_fsk_cfo_snapshot(double dc_rad_per_sample, int rate_out_hz, double* out_cfo_hz,
                                         int* out_after_generation_bump_available, int* out_after_reset_available);
int dsd_rtl_stream_test_fsk_snr_sps(int rate_out_hz, int symbol_rate_hz, int stale_ted_sps);
int dsd_rtl_stream_test_direct_output_rate_after_open_update(int output_kind, int rate_out_hz, int resamp_target_hz,
                                                             unsigned int* out_rate_hz, int* out_resamp_enabled);
int dsd_rtl_stream_test_fsk_reacquire(int output_kind, size_t queued_samples, int cached_symbols,
                                      size_t* out_used_after, int* out_cache_pending_after,
                                      uint32_t* out_generation_before, uint32_t* out_generation_after,
                                      int* out_request_rc, int* out_consumed);
int dsd_rtl_stream_test_retune_profile_request_binding(int* out_first_profile, int* out_second_profile,
                                                       uint32_t* out_first_freq_hz, uint32_t* out_second_freq_hz,
                                                       uint32_t* out_first_request_id, uint32_t* out_second_request_id);
int dsd_rtl_stream_test_retune_profile_coalesced_no_profile(int* out_profile, uint32_t* out_profile_freq_hz,
                                                            uint32_t* out_manual_freq_hz, uint32_t* out_request_id,
                                                            uint32_t* out_coalesced_request_id);
int dsd_rtl_stream_test_retune_profile_gain_binding(int* out_gain_is_set, int* out_gain_tenth_db, int* out_gain_is_auto,
                                                    int* out_autogain_is_set, int* out_autogain_on);
int dsd_rtl_stream_test_get_replay_state(rtl_stream_test_replay_state* out_state);
int dsd_rtl_stream_test_steady_state_watermark_enabled(const char* audio_in_dev);
#endif
}

#include <dsd-neo/io/rtl_stream.h>

struct RtlSdrContext {
    RtlSdrOrchestrator* stream;
};

static int
rtl_stream_create_impl(const dsd_opts* opts, dsd_opts* mirrored_opts, RtlSdrContext** out_ctx) {
    if (!out_ctx || !opts) {
        return -1;
    }
    *out_ctx = static_cast<RtlSdrContext*>(calloc(1, sizeof(RtlSdrContext)));
    if (!*out_ctx) {
        return -1;
    }
    (*out_ctx)->stream = new (std::nothrow) RtlSdrOrchestrator(*opts, mirrored_opts);
    if (!(*out_ctx)->stream) {
        free(*out_ctx);
        *out_ctx = NULL;
        return -1;
    }
    return 0;
}

/**
 * @brief Create a new RTL-SDR stream context from an immutable options snapshot.
 *
 * @param opts Decoder options snapshot used to configure the stream. Must not be NULL.
 * @param out_ctx [out] On success, receives an opaque context pointer.
 * @return 0 on success; otherwise <0 on error.
 */
extern "C" int
rtl_stream_create(const dsd_opts* opts, RtlSdrContext** out_ctx) {
    return rtl_stream_create_impl(opts, NULL, out_ctx);
}

/**
 * @brief Create a new RTL-SDR stream context mirrored to caller-owned options.
 *
 * @param opts Mutable caller-owned decoder options to mirror. Must not be NULL.
 * @param out_ctx [out] On success, receives an opaque context pointer.
 * @return 0 on success; otherwise <0 on error.
 */
extern "C" int
rtl_stream_create_mirrored(dsd_opts* opts, RtlSdrContext** out_ctx) {
    return rtl_stream_create_impl(opts, opts, out_ctx);
}

/**
 * @brief Start the stream threads and device I/O.
 *
 * @param ctx Stream context created by rtl_stream_create() or rtl_stream_create_mirrored().
 * @return 0 on success; otherwise <0 on error.
 */
extern "C" int
rtl_stream_start(RtlSdrContext* ctx) {
    if (!ctx || !ctx->stream) {
        return -1;
    }
    return ctx->stream->start();
}

/**
 * @brief Stop the stream and cleanup resources associated with the run.
 *
 * Safe to call multiple times; subsequent calls are no-ops.
 *
 * @param ctx Stream context created by rtl_stream_create() or rtl_stream_create_mirrored().
 * @return 0 on success; otherwise <0 on error.
 */
extern "C" int
rtl_stream_stop(RtlSdrContext* ctx) {
    if (!ctx || !ctx->stream) {
        return -1;
    }
    return ctx->stream->stop();
}

extern "C" int
rtl_stream_soft_stop(RtlSdrContext* ctx) {
    if (!ctx || !ctx->stream) {
        return -1;
    }
    return ctx->stream->soft_stop();
}

/**
 * @brief Destroy the stream context and free all associated resources.
 *
 * If the stream is running, it is stopped before destruction.
 *
 * @param ctx Stream context to destroy. May be NULL.
 * @return 0 always.
 */
extern "C" int
rtl_stream_destroy(RtlSdrContext* ctx) {
    if (!ctx) {
        return 0;
    }
    if (ctx->stream) {
        ctx->stream->stop();
        delete ctx->stream;
        ctx->stream = NULL;
    }
    free(ctx);
    return 0;
}

/**
 * @brief Tune to a new center frequency.
 *
 * @param ctx Stream context.
 * @param center_freq_hz New center frequency in Hz.
 * @return rtl_stream_tune_result: 0 on success, 1 when deferred, negative on error/timeout.
 */
extern "C" int
rtl_stream_tune(RtlSdrContext* ctx, uint32_t center_freq_hz) {
    if (!ctx || !ctx->stream) {
        return -1;
    }
    return ctx->stream->tune(center_freq_hz);
}

#if defined(DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS)
extern "C" int
rtl_stream_test_request_retune(const RtlSdrContext* ctx, uint32_t freq_hz, int timeout_ms) {
    if (!ctx || !ctx->stream) {
        return -2;
    }
    return dsd_rtl_stream_test_request_retune((long int)freq_hz, timeout_ms);
}

extern "C" int
rtl_stream_test_prepare_reconfigure_input(size_t queued_samples, size_t* out_used_after,
                                          uint32_t* out_generation_before, uint32_t* out_generation_after) {
    return dsd_rtl_stream_test_prepare_reconfigure_input(queued_samples, out_used_after, out_generation_before,
                                                         out_generation_after);
}

extern "C" int
rtl_stream_test_retune_output_pending(size_t queued_samples, int cached_symbols, size_t* out_ring_pending,
                                      int* out_cache_pending, int* out_drained) {
    return dsd_rtl_stream_test_retune_output_pending(queued_samples, cached_symbols, out_ring_pending,
                                                     out_cache_pending, out_drained);
}

extern "C" int
rtl_stream_test_tune_result_output_drain(int tune_result, size_t queued_samples, int cached_symbols,
                                         size_t* out_used_after, int* out_cache_pending_after,
                                         uint32_t* out_generation_before, uint32_t* out_generation_after) {
    return dsd_rtl_stream_test_tune_result_output_drain(tune_result, queued_samples, cached_symbols, out_used_after,
                                                        out_cache_pending_after, out_generation_before,
                                                        out_generation_after);
}

extern "C" int
rtl_stream_test_clear_output(size_t queued_samples, int cached_symbols, size_t* out_used_after,
                             int* out_cache_pending_after, uint32_t* out_generation_before,
                             uint32_t* out_generation_after) {
    return dsd_rtl_stream_test_clear_output(queued_samples, cached_symbols, out_used_after, out_cache_pending_after,
                                            out_generation_before, out_generation_after);
}

extern "C" int
rtl_stream_test_clear_output_fsk_reset(size_t queued_samples, int* out_have_prev_after_clear, int* out_consumed_reset,
                                       int* out_have_prev_after_consume) {
    return dsd_rtl_stream_test_clear_output_fsk_reset(queued_samples, out_have_prev_after_clear, out_consumed_reset,
                                                      out_have_prev_after_consume);
}

extern "C" int
rtl_stream_test_fsk_cfo_snapshot(double dc_rad_per_sample, int rate_out_hz, double* out_cfo_hz,
                                 int* out_after_generation_bump_available, int* out_after_reset_available) {
    return dsd_rtl_stream_test_fsk_cfo_snapshot(dc_rad_per_sample, rate_out_hz, out_cfo_hz,
                                                out_after_generation_bump_available, out_after_reset_available);
}

extern "C" int
rtl_stream_test_fsk_snr_sps(int rate_out_hz, int symbol_rate_hz, int stale_ted_sps) {
    return dsd_rtl_stream_test_fsk_snr_sps(rate_out_hz, symbol_rate_hz, stale_ted_sps);
}

extern "C" int
rtl_stream_test_direct_output_rate_after_open_update(int output_kind, int rate_out_hz, int resamp_target_hz,
                                                     unsigned int* out_rate_hz, int* out_resamp_enabled) {
    return dsd_rtl_stream_test_direct_output_rate_after_open_update(output_kind, rate_out_hz, resamp_target_hz,
                                                                    out_rate_hz, out_resamp_enabled);
}

extern "C" int
rtl_stream_test_fsk_reacquire(int output_kind, size_t queued_samples, int cached_symbols, size_t* out_used_after,
                              int* out_cache_pending_after, uint32_t* out_generation_before,
                              uint32_t* out_generation_after, int* out_request_rc, int* out_consumed) {
    return dsd_rtl_stream_test_fsk_reacquire(output_kind, queued_samples, cached_symbols, out_used_after,
                                             out_cache_pending_after, out_generation_before, out_generation_after,
                                             out_request_rc, out_consumed);
}

extern "C" int
rtl_stream_test_retune_profile_request_binding(int* out_first_profile, int* out_second_profile,
                                               uint32_t* out_first_freq_hz, uint32_t* out_second_freq_hz,
                                               uint32_t* out_first_request_id, uint32_t* out_second_request_id) {
    return dsd_rtl_stream_test_retune_profile_request_binding(out_first_profile, out_second_profile, out_first_freq_hz,
                                                              out_second_freq_hz, out_first_request_id,
                                                              out_second_request_id);
}

extern "C" int
rtl_stream_test_retune_profile_coalesced_no_profile(int* out_profile, uint32_t* out_profile_freq_hz,
                                                    uint32_t* out_manual_freq_hz, uint32_t* out_request_id,
                                                    uint32_t* out_coalesced_request_id) {
    return dsd_rtl_stream_test_retune_profile_coalesced_no_profile(out_profile, out_profile_freq_hz, out_manual_freq_hz,
                                                                   out_request_id, out_coalesced_request_id);
}

extern "C" int
rtl_stream_test_retune_profile_gain_binding(int* out_gain_is_set, int* out_gain_tenth_db, int* out_gain_is_auto,
                                            int* out_autogain_is_set, int* out_autogain_on) {
    return dsd_rtl_stream_test_retune_profile_gain_binding(out_gain_is_set, out_gain_tenth_db, out_gain_is_auto,
                                                           out_autogain_is_set, out_autogain_on);
}

extern "C" int
rtl_stream_test_get_replay_state(const RtlSdrContext* ctx, rtl_stream_test_replay_state* out_state) {
    if (!ctx || !ctx->stream || !out_state) {
        return -2;
    }
    return dsd_rtl_stream_test_get_replay_state(out_state);
}

extern "C" int
rtl_stream_test_steady_state_watermark_enabled(const char* audio_in_dev) {
    return dsd_rtl_stream_test_steady_state_watermark_enabled(audio_in_dev);
}
#endif

/**
 * @brief Read up to `count` interleaved audio samples into `out`.
 *
 * @param ctx Stream context.
 * @param out Destination buffer for samples. Must not be NULL.
 * @param count Maximum number of samples to read.
 * @param out_got [out] Set to the number of samples actually read.
 * @return 0 on success; otherwise <0 on error (e.g., shutdown).
 */
extern "C" int
rtl_stream_read(RtlSdrContext* ctx, float* out, size_t count, int* out_got) {
    if (!ctx || !ctx->stream || !out || !out_got) {
        return -1;
    }
    return ctx->stream->read(out, count, *out_got);
}

extern "C" int
rtl_stream_read_monitor(const RtlSdrContext* ctx, float* out, size_t count, int* out_got) {
    if (!ctx || !ctx->stream || !out || !out_got) {
        return -1;
    }
    return dsd_rtl_stream_monitor_read(out, count, out_got);
}

/**
 * @brief Get the current output sample rate in Hz.
 *
 * @param ctx Stream context.
 * @return Output sample rate in Hz; returns 0 if `ctx` is invalid.
 */
extern "C" uint32_t
rtl_stream_output_rate(const RtlSdrContext* ctx) {
    if (!ctx || !ctx->stream) {
        return 0U;
    }
    return ctx->stream->output_rate();
}

extern "C" uint32_t
rtl_stream_monitor_rate(const RtlSdrContext* ctx) {
    if (!ctx || !ctx->stream) {
        return 0U;
    }
    return dsd_rtl_stream_monitor_rate();
}

extern "C" uint32_t
rtl_stream_output_generation(void) {
    return dsd_rtl_stream_output_generation();
}

extern "C" int
rtl_stream_is_active(void) {
    return dsd_rtl_stream_is_active();
}

extern "C" int
rtl_stream_get_output_kind(void) {
    return dsd_rtl_stream_get_output_kind();
}

extern "C" int
rtl_stream_get_symbol_profile(int* out_symbol_rate_hz, int* out_levels) {
    return dsd_rtl_stream_get_symbol_profile(out_symbol_rate_hz, out_levels);
}

extern "C" int
rtl_stream_get_symbol_profile_full(int* out_symbol_rate_hz, int* out_levels, int* out_channel_profile) {
    return dsd_rtl_stream_get_symbol_profile_full(out_symbol_rate_hz, out_levels, out_channel_profile);
}

extern "C" int
rtl_stream_set_symbol_profile(int symbol_rate_hz, int levels, int channel_profile) {
    return dsd_rtl_stream_set_symbol_profile(symbol_rate_hz, levels, channel_profile);
}

extern "C" void
rtl_stream_prepare_retune_profile(int cqpsk_enable, int symbol_rate_hz, int levels, int channel_profile, int ted_sps,
                                  int persist_ted_override) {
    dsd_rtl_stream_prepare_retune_profile(cqpsk_enable, symbol_rate_hz, levels, channel_profile, ted_sps,
                                          persist_ted_override);
}

extern "C" void
rtl_stream_prepare_retune_profile_for_target(uint32_t target_freq_hz, int cqpsk_enable, int symbol_rate_hz, int levels,
                                             int channel_profile, int ted_sps, int persist_ted_override) {
    dsd_rtl_stream_prepare_retune_profile_for_target(target_freq_hz, cqpsk_enable, symbol_rate_hz, levels,
                                                     channel_profile, ted_sps, persist_ted_override);
}

extern "C" void
rtl_stream_prepare_retune_profile_for_target_with_gain(uint32_t target_freq_hz, int cqpsk_enable, int symbol_rate_hz,
                                                       int levels, int channel_profile, int ted_sps,
                                                       int persist_ted_override,
                                                       const rtl_stream_retune_gain_profile* gain_profile) {
    dsd_rtl_stream_prepare_retune_profile_for_target_with_gain(target_freq_hz, cqpsk_enable, symbol_rate_hz, levels,
                                                               channel_profile, ted_sps, persist_ted_override,
                                                               gain_profile);
}

extern "C" void
rtl_stream_apply_pending_retune_profile(void) {
    dsd_rtl_stream_apply_pending_retune_profile();
}

extern "C" void
rtl_stream_apply_pending_retune_profile_for_target(uint32_t target_freq_hz) {
    dsd_rtl_stream_apply_pending_retune_profile_for_target(target_freq_hz);
}

extern "C" void
rtl_stream_clear_pending_retune_profile(void) {
    dsd_rtl_stream_clear_pending_retune_profile();
}

extern "C" int
rtl_stream_request_fsk_reacquire(void) {
    return dsd_rtl_stream_request_fsk_reacquire();
}

/**
 * @brief Clear the output ring buffer and wake any waiting producer.
 *
 * Mirrors the legacy behavior. The `ctx` parameter is currently ignored.
 *
 * @param ctx Stream context (unused).
 */
extern "C" void
rtl_stream_clear_output(RtlSdrContext* ctx) {
    (void)ctx;
    // Delegate to legacy function to keep behavior identical
    dsd_rtl_stream_clear_output();
}

/**
 * @brief Return mean power approximation (RMS^2 proxy) for soft squelch.
 *
 * The computation uses a small fixed sample window for efficiency and mirrors
 * the legacy implementation.
 *
 * @param ctx Stream context (unused).
 * @return Mean power value (approximate RMS squared, normalized).
 */
extern "C" double
rtl_stream_return_pwr(const RtlSdrContext* ctx) {
    (void)ctx;
    return dsd_rtl_stream_return_pwr();
}

extern "C" void
rtl_stream_set_channel_squelch(float level) {
    dsd_rtl_stream_set_channel_squelch(level);
}

extern "C" int
rtl_stream_set_bias_tee(int on) {
    return dsd_rtl_stream_set_bias_tee(on);
}

extern "C" int
rtl_stream_get_gain(int* out_tenth_db, int* out_is_auto) {
    return dsd_rtl_stream_get_gain(out_tenth_db, out_is_auto);
}

extern "C" int
rtl_stream_cqpsk_timing_bias(const RtlSdrContext* ctx) {
    (void)ctx;
    return dsd_rtl_stream_cqpsk_timing_bias();
}

extern "C" void
rtl_stream_set_resampler_target(int target_hz) {
    dsd_rtl_stream_set_resampler_target(target_hz);
}

extern "C" int
rtl_stream_get_ted_sps(void) {
    return dsd_rtl_stream_get_ted_sps();
}

extern "C" int
rtl_stream_get_ted_sps_override(void) {
    return dsd_rtl_stream_get_ted_sps_override();
}

extern "C" void
rtl_stream_set_ted_sps(int sps) {
    dsd_rtl_stream_set_ted_sps(sps);
}

extern "C" void
rtl_stream_clear_ted_sps_override(void) {
    dsd_rtl_stream_clear_ted_sps_override();
}

extern "C" void
rtl_stream_set_ted_sps_no_override(int sps) {
    dsd_rtl_stream_set_ted_sps_no_override(sps);
}

extern "C" void
rtl_stream_set_ted_gain(float gain) {
    dsd_rtl_stream_set_ted_gain(gain);
}

extern "C" float
rtl_stream_get_ted_gain(void) {
    return dsd_rtl_stream_get_ted_gain();
}

extern "C" void
rtl_stream_p25p2_err_update(int slot, int facch_ok_delta, int facch_err_delta, int sacch_ok_delta, int sacch_err_delta,
                            int voice_err_delta) {
    dsd_rtl_stream_p25p2_err_update(slot, facch_ok_delta, facch_err_delta, sacch_ok_delta, sacch_err_delta,
                                    voice_err_delta);
}

extern "C" int
rtl_stream_constellation_get(float* out_xy, int max_points) {
    return dsd_rtl_stream_constellation_get(out_xy, max_points);
}

extern "C" int
rtl_stream_spectrum_get(float* out_db, int max_bins, int* out_rate) {
    return dsd_rtl_stream_spectrum_get(out_db, max_bins, out_rate);
}

extern "C" int
rtl_stream_spectrum_set_size(int n) {
    return dsd_rtl_stream_spectrum_set_size(n);
}

extern "C" int
rtl_stream_spectrum_get_size(void) {
    return dsd_rtl_stream_spectrum_get_size();
}

/* Carrier/Costas diagnostics and control */
extern "C" void dsd_rtl_stream_reset_costas(void);
extern "C" double dsd_rtl_stream_get_cfo_hz(void);
extern "C" double dsd_rtl_stream_get_residual_cfo_hz(void);
extern "C" int dsd_rtl_stream_get_carrier_lock(void);

extern "C" void
rtl_stream_reset_costas(void) {
    dsd_rtl_stream_reset_costas();
}

extern "C" double
rtl_stream_get_cfo_hz(void) {
    return dsd_rtl_stream_get_cfo_hz();
}

extern "C" double
rtl_stream_get_residual_cfo_hz(void) {
    return dsd_rtl_stream_get_residual_cfo_hz();
}

extern "C" int
rtl_stream_get_carrier_lock(void) {
    return dsd_rtl_stream_get_carrier_lock();
}

extern "C" int dsd_rtl_stream_get_nco_q15(void);
extern "C" int dsd_rtl_stream_get_demod_rate_hz(void);
extern "C" int dsd_rtl_stream_get_costas_err_q14(void);
extern "C" int dsd_rtl_stream_get_costas_metrics(rtl_stream_costas_metrics* out);

extern "C" int
rtl_stream_get_nco_q15(void) {
    return dsd_rtl_stream_get_nco_q15();
}

extern "C" int
rtl_stream_get_demod_rate_hz(void) {
    return dsd_rtl_stream_get_demod_rate_hz();
}

extern "C" int
rtl_stream_get_costas_err_q14(void) {
    return dsd_rtl_stream_get_costas_err_q14();
}

extern "C" int
rtl_stream_get_costas_metrics(rtl_stream_costas_metrics* out) {
    return dsd_rtl_stream_get_costas_metrics(out);
}

extern "C" double dsd_rtl_stream_get_fll_band_edge_freq_hz(void);

extern "C" double
rtl_stream_get_fll_band_edge_freq_hz(void) {
    return dsd_rtl_stream_get_fll_band_edge_freq_hz();
}

extern "C" int
rtl_stream_get_decode_health(rtl_stream_decode_health* out) {
    return dsd_rtl_stream_get_decode_health(out);
}

extern "C" int
rtl_stream_get_input_level(dsd_input_level_snapshot* out) {
    return dsd_rtl_stream_get_input_level(out);
}

/* Auto-PPM status snapshot */
extern "C" int dsd_rtl_stream_auto_ppm_get_status(int* enabled, double* snr_db, double* df_hz, double* est_ppm,
                                                  int* last_dir, int* cooldown, int* locked);
extern "C" int dsd_rtl_stream_auto_ppm_training_active(void);
extern "C" int dsd_rtl_stream_auto_ppm_get_lock(int* ppm, double* snr_db, double* df_hz);

extern "C" int
rtl_stream_auto_ppm_get_status(int* enabled, double* snr_db, double* df_hz, double* est_ppm, int* last_dir,
                               int* cooldown, int* locked) {
    return dsd_rtl_stream_auto_ppm_get_status(enabled, snr_db, df_hz, est_ppm, last_dir, cooldown, locked);
}

extern "C" int
rtl_stream_auto_ppm_training_active(void) {
    return dsd_rtl_stream_auto_ppm_training_active();
}

extern "C" int
rtl_stream_auto_ppm_get_lock(int* ppm, double* snr_db, double* df_hz) {
    return dsd_rtl_stream_auto_ppm_get_lock(ppm, snr_db, df_hz);
}

extern "C" void
rtl_stream_set_auto_ppm(int onoff) {
    dsd_rtl_stream_set_auto_ppm(onoff);
}

extern "C" int
rtl_stream_get_auto_ppm(void) {
    return dsd_rtl_stream_get_auto_ppm();
}

extern "C" int
rtl_stream_eye_get(float* out, int max_samples, int* out_sps) {
    return dsd_rtl_stream_eye_get(out, max_samples, out_sps);
}

/* -------- SNR bias computation (dynamic, accounts for BW/sample rate) -------- */
extern "C" double
rtl_stream_get_snr_bias_c4fm(void) {
    return dsd_rtl_stream_get_snr_bias_c4fm();
}

extern "C" double
rtl_stream_get_snr_bias_evm(void) {
    return dsd_rtl_stream_get_snr_bias_evm();
}

/* -------- IQ DC blocker (runtime) -------- */
extern "C" int dsd_rtl_stream_get_iq_dc(int* out_shift_k);
extern "C" void dsd_rtl_stream_set_iq_dc(int enable, int shift_k);

extern "C" int
rtl_stream_get_iq_dc(int* out_shift_k) {
    return dsd_rtl_stream_get_iq_dc(out_shift_k);
}

extern "C" void
rtl_stream_set_iq_dc(int enable, int shift_k) {
    dsd_rtl_stream_set_iq_dc(enable, shift_k);
}

extern "C" double
rtl_stream_estimate_snr_c4fm_eye(void) {
    return dsd_rtl_stream_estimate_snr_c4fm_eye();
}

extern "C" double
rtl_stream_estimate_snr_qpsk_const(void) {
    return dsd_rtl_stream_estimate_snr_qpsk_const();
}

extern "C" double
rtl_stream_estimate_snr_gfsk_eye(void) {
    return dsd_rtl_stream_estimate_snr_gfsk_eye();
}

extern "C" int
rtl_stream_get_tuner_autogain(void) {
    return dsd_rtl_stream_get_tuner_autogain();
}

extern "C" void
rtl_stream_set_tuner_autogain(int onoff) {
    dsd_rtl_stream_set_tuner_autogain(onoff);
}

/* IQ balance prefilter toggle/get */
extern "C" void
rtl_stream_toggle_iq_balance(int onoff) {
    dsd_rtl_stream_toggle_iq_balance(onoff);
}

extern "C" int
rtl_stream_get_iq_balance(void) {
    return dsd_rtl_stream_get_iq_balance();
}

extern "C" int
rtl_stream_get_rtltcp_autotune(void) {
    return dsd_rtl_stream_get_rtltcp_autotune();
}

extern "C" void
rtl_stream_set_rtltcp_autotune(int onoff) {
    (void)dsd_rtl_stream_set_rtltcp_autotune(onoff);
}

extern "C" int
rtl_stream_get_last_applied_freq(uint32_t* out_freq_hz) {
    return dsd_rtl_stream_get_last_applied_freq(out_freq_hz);
}
