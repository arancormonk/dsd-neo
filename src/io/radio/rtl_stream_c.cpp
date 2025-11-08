// SPDX-License-Identifier: GPL-2.0-or-later
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

#include <new>
#include <stdlib.h>
#include <string.h>

extern "C" {
#include <dsd-neo/core/dsd.h>
#include <dsd-neo/io/rtl_stream_c.h>
// Local forward declarations for legacy helpers used under the hood
void dsd_rtl_stream_clear_output(void);
long dsd_rtl_stream_return_pwr(void);
int dsd_rtl_stream_ted_bias(void);
void dsd_rtl_stream_set_resampler_target(int target_hz);
void dsd_rtl_stream_set_ted_sps(int sps);
int dsd_rtl_stream_get_ted_sps(void);
void dsd_rtl_stream_set_ted_gain(int g);
int dsd_rtl_stream_get_ted_gain(void);
void dsd_rtl_stream_set_ted_force(int onoff);
int dsd_rtl_stream_get_ted_force(void);
/* Bias tee control implemented in rtl_sdr_fm.cpp */
int dsd_rtl_stream_set_bias_tee(int on);
void dsd_rtl_stream_p25p2_err_update(int slot, int facch_ok_delta, int facch_err_delta, int sacch_ok_delta,
                                     int sacch_err_delta, int voice_err_delta);
void dsd_rtl_stream_cqpsk_set_rrc(int enable, int alpha_percent, int span_syms);
void dsd_rtl_stream_cqpsk_set_dqpsk(int onoff);
int dsd_rtl_stream_cqpsk_get_rrc(int* enable, int* alpha_percent, int* span_syms);
int dsd_rtl_stream_cqpsk_get_dqpsk(int* onoff);
int dsd_rtl_stream_eye_get(int16_t* out, int max_samples, int* out_sps);
int dsd_rtl_stream_constellation_get(int16_t* out_xy, int max_points);
int dsd_rtl_stream_spectrum_get(float* out_db, int max_bins, int* out_rate);
int dsd_rtl_stream_spectrum_set_size(int n);
int dsd_rtl_stream_spectrum_get_size(void);
/* P25p2 RRC auto-probe (implemented in rtl_sdr_fm.cpp) */
void dsd_rtl_stream_set_p25p2_rrc_autoprobe(int onoff);
int dsd_rtl_stream_get_p25p2_rrc_autoprobe(void);
void dsd_rtl_stream_get_p25p2_rrc_auto(int* decided, int* state, int* choice);
/* Auto-DSP config accessors (implemented in rtl_sdr_fm.cpp) */
void dsd_rtl_stream_auto_dsp_get_config(rtl_auto_dsp_config* out);
void dsd_rtl_stream_auto_dsp_set_config(const rtl_auto_dsp_config* in);
void dsd_rtl_stream_auto_dsp_get_status(rtl_auto_dsp_status* out);
/* Gain query helper implemented in rtl_sdr_fm.cpp */
int dsd_rtl_stream_get_gain(int* out_tenth_db, int* out_is_auto);
/* Auto-PPM runtime toggle */
void dsd_rtl_stream_set_auto_ppm(int onoff);
int dsd_rtl_stream_get_auto_ppm(void);
/* RTL-TCP autotune control */
int dsd_rtl_stream_set_rtltcp_autotune(int onoff);
int dsd_rtl_stream_get_rtltcp_autotune(void);
/* Eye-based SNR fallback */
double dsd_rtl_stream_estimate_snr_c4fm_eye(void);
double dsd_rtl_stream_estimate_snr_qpsk_const(void);
double dsd_rtl_stream_estimate_snr_gfsk_eye(void);
/* Blanker + tuner autogain */
int dsd_rtl_stream_get_blanker(int* out_thr, int* out_win);
void dsd_rtl_stream_set_blanker(int enable, int thr, int win);
int dsd_rtl_stream_get_tuner_autogain(void);
void dsd_rtl_stream_set_tuner_autogain(int onoff);
}

#include <dsd-neo/io/rtl_stream.h>

struct RtlSdrContext {
    RtlSdrOrchestrator* stream;
};

/**
 * @brief Create a new RTL-SDR stream context from options.
 *
 * @param opts Decoder options snapshot used to configure the stream. Must not be NULL.
 * @param out_ctx [out] On success, receives an opaque context pointer.
 * @return 0 on success; otherwise <0 on error.
 */
extern "C" int
rtl_stream_create(const dsd_opts* opts, RtlSdrContext** out_ctx) {
    if (!out_ctx || !opts) {
        return -1;
    }
    *out_ctx = (RtlSdrContext*)calloc(1, sizeof(RtlSdrContext));
    if (!*out_ctx) {
        return -1;
    }
    (*out_ctx)->stream = new (std::nothrow) RtlSdrOrchestrator(*opts);
    if (!(*out_ctx)->stream) {
        free(*out_ctx);
        *out_ctx = NULL;
        return -1;
    }
    return 0;
}

/**
 * @brief Start the stream threads and device I/O.
 *
 * @param ctx Stream context created by rtl_stream_create().
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
 * @param ctx Stream context created by rtl_stream_create().
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
 * @return 0 on success; otherwise <0 on error.
 */
extern "C" int
rtl_stream_tune(RtlSdrContext* ctx, uint32_t center_freq_hz) {
    if (!ctx || !ctx->stream) {
        return -1;
    }
    // simple process-level cache; safe since single tuner is typical
    static uint32_t s_last_freq = 0U;
    if (center_freq_hz == s_last_freq) {
        return 0; // no-op
    }
    int rc = ctx->stream->tune(center_freq_hz);
    if (rc == 0) {
        s_last_freq = center_freq_hz;
    }
    return rc;
}

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
rtl_stream_read(RtlSdrContext* ctx, int16_t* out, size_t count, int* out_got) {
    if (!ctx || !ctx->stream || !out || !out_got) {
        return -1;
    }
    return ctx->stream->read(out, count, *out_got);
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

/**
 * @brief Clear the output ring buffer and wake any waiting producer.
 *
 * Mirrors the legacy behavior. The `ctx` parameter is currently ignored.
 *
 * @param ctx Stream context (unused).
 */
extern "C" void
rtl_stream_clear_output(RtlSdrContext* /*ctx*/) {
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
 * @return Mean power value (approximate RMS squared).
 */
extern "C" long
rtl_stream_return_pwr(const RtlSdrContext* /*ctx*/) {
    return dsd_rtl_stream_return_pwr();
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
rtl_stream_ted_bias(const RtlSdrContext* /*ctx*/) {
    return dsd_rtl_stream_ted_bias();
}

extern "C" void
rtl_stream_set_resampler_target(int target_hz) {
    dsd_rtl_stream_set_resampler_target(target_hz);
}

extern "C" void
rtl_stream_set_ted_sps(int sps) {
    static int s_last_sps = 0;
    if (sps == s_last_sps) {
        return; // no change
    }
    dsd_rtl_stream_set_ted_sps(sps);
    s_last_sps = sps;
}

extern "C" int
rtl_stream_get_ted_sps(void) {
    return dsd_rtl_stream_get_ted_sps();
}

extern "C" void
rtl_stream_set_ted_gain(int gain_q20) {
    dsd_rtl_stream_set_ted_gain(gain_q20);
}

extern "C" int
rtl_stream_get_ted_gain(void) {
    return dsd_rtl_stream_get_ted_gain();
}

extern "C" void
rtl_stream_set_ted_force(int onoff) {
    dsd_rtl_stream_set_ted_force(onoff);
}

extern "C" int
rtl_stream_get_ted_force(void) {
    return dsd_rtl_stream_get_ted_force();
}

extern "C" void
rtl_stream_p25p2_err_update(int slot, int facch_ok_delta, int facch_err_delta, int sacch_ok_delta, int sacch_err_delta,
                            int voice_err_delta) {
    dsd_rtl_stream_p25p2_err_update(slot, facch_ok_delta, facch_err_delta, sacch_ok_delta, sacch_err_delta,
                                    voice_err_delta);
}

extern "C" void
rtl_stream_cqpsk_set_rrc(int enable, int alpha_percent, int span_syms) {
    dsd_rtl_stream_cqpsk_set_rrc(enable, alpha_percent, span_syms);
}

extern "C" void
rtl_stream_cqpsk_set_dqpsk(int onoff) {
    dsd_rtl_stream_cqpsk_set_dqpsk(onoff);
}

extern "C" int
rtl_stream_cqpsk_get_rrc(int* enable, int* alpha_percent, int* span_syms) {
    return dsd_rtl_stream_cqpsk_get_rrc(enable, alpha_percent, span_syms);
}

extern "C" int
rtl_stream_cqpsk_get_dqpsk(int* onoff) {
    return dsd_rtl_stream_cqpsk_get_dqpsk(onoff);
}

extern "C" void
rtl_stream_set_p25p2_rrc_autoprobe(int onoff) {
    dsd_rtl_stream_set_p25p2_rrc_autoprobe(onoff);
}

extern "C" int
rtl_stream_get_p25p2_rrc_autoprobe(void) {
    return dsd_rtl_stream_get_p25p2_rrc_autoprobe();
}

extern "C" int
rtl_stream_get_p25p2_rrc_auto_status(int* decided, int* state, int* choice) {
    dsd_rtl_stream_get_p25p2_rrc_auto(decided, state, choice);
    return 0;
}

/* CQPSK acquisition-only FLL (pre-Costas) */
extern "C" int dsd_rtl_stream_get_cqpsk_acq_fll(void);
extern "C" void dsd_rtl_stream_set_cqpsk_acq_fll(int onoff);

extern "C" int
rtl_stream_get_cqpsk_acq_fll(void) {
    return dsd_rtl_stream_get_cqpsk_acq_fll();
}

extern "C" void
rtl_stream_set_cqpsk_acq_fll(int onoff) {
    dsd_rtl_stream_set_cqpsk_acq_fll(onoff);
}

extern "C" int
rtl_stream_constellation_get(int16_t* out_xy, int max_points) {
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
rtl_stream_eye_get(int16_t* out, int max_samples, int* out_sps) {
    return dsd_rtl_stream_eye_get(out, max_samples, out_sps);
}

/* -------- FM/C4FM amplitude stabilization + DC blocker (runtime) -------- */
extern "C" int dsd_rtl_stream_get_fm_agc(void);
extern "C" void dsd_rtl_stream_set_fm_agc(int onoff);
extern "C" void dsd_rtl_stream_get_fm_agc_params(int* target_rms, int* min_rms, int* alpha_up_q15, int* alpha_down_q15);
extern "C" void dsd_rtl_stream_set_fm_agc_params(int target_rms, int min_rms, int alpha_up_q15, int alpha_down_q15);
extern "C" int dsd_rtl_stream_get_fm_limiter(void);
extern "C" void dsd_rtl_stream_set_fm_limiter(int onoff);
extern "C" int dsd_rtl_stream_get_iq_dc(int* out_shift_k);
extern "C" void dsd_rtl_stream_set_iq_dc(int enable, int shift_k);
extern "C" int dsd_rtl_stream_get_fm_agc_auto(void);
extern "C" void dsd_rtl_stream_set_fm_agc_auto(int onoff);
extern "C" int dsd_rtl_stream_get_fm_cma(void);
extern "C" void dsd_rtl_stream_set_fm_cma(int onoff);
extern "C" void dsd_rtl_stream_get_fm_cma_params(int* taps, int* mu_q15, int* warmup);
extern "C" void dsd_rtl_stream_set_fm_cma_params(int taps, int mu_q15, int warmup);
extern "C" int dsd_rtl_stream_get_fm_cma_strength(void);
extern "C" void dsd_rtl_stream_set_fm_cma_strength(int strength);
extern "C" void dsd_rtl_stream_get_fm_cma_guard(int* freeze_blocks, int* accepts, int* rejects);

extern "C" int
rtl_stream_get_fm_agc(void) {
    return dsd_rtl_stream_get_fm_agc();
}

extern "C" void
rtl_stream_set_fm_agc(int onoff) {
    dsd_rtl_stream_set_fm_agc(onoff);
}

extern "C" void
rtl_stream_get_fm_agc_params(int* target_rms, int* min_rms, int* alpha_up_q15, int* alpha_down_q15) {
    dsd_rtl_stream_get_fm_agc_params(target_rms, min_rms, alpha_up_q15, alpha_down_q15);
}

extern "C" void
rtl_stream_set_fm_agc_params(int target_rms, int min_rms, int alpha_up_q15, int alpha_down_q15) {
    dsd_rtl_stream_set_fm_agc_params(target_rms, min_rms, alpha_up_q15, alpha_down_q15);
}

extern "C" int
rtl_stream_get_fm_limiter(void) {
    return dsd_rtl_stream_get_fm_limiter();
}

extern "C" void
rtl_stream_set_fm_limiter(int onoff) {
    dsd_rtl_stream_set_fm_limiter(onoff);
}

extern "C" int
rtl_stream_get_iq_dc(int* out_shift_k) {
    return dsd_rtl_stream_get_iq_dc(out_shift_k);
}

extern "C" void
rtl_stream_set_iq_dc(int enable, int shift_k) {
    dsd_rtl_stream_set_iq_dc(enable, shift_k);
}

extern "C" int
rtl_stream_get_fm_agc_auto(void) {
    return dsd_rtl_stream_get_fm_agc_auto();
}

extern "C" void
rtl_stream_set_fm_agc_auto(int onoff) {
    dsd_rtl_stream_set_fm_agc_auto(onoff);
}

/* FM CMA wrappers */
extern "C" int
rtl_stream_get_fm_cma(void) {
    return dsd_rtl_stream_get_fm_cma();
}

extern "C" void
rtl_stream_set_fm_cma(int onoff) {
    dsd_rtl_stream_set_fm_cma(onoff);
}

extern "C" void
rtl_stream_get_fm_cma_params(int* taps, int* mu_q15, int* warmup_samples) {
    dsd_rtl_stream_get_fm_cma_params(taps, mu_q15, warmup_samples);
}

extern "C" void
rtl_stream_set_fm_cma_params(int taps, int mu_q15, int warmup_samples) {
    dsd_rtl_stream_set_fm_cma_params(taps, mu_q15, warmup_samples);
}

extern "C" int
rtl_stream_get_fm_cma_strength(void) {
    return dsd_rtl_stream_get_fm_cma_strength();
}

extern "C" void
rtl_stream_set_fm_cma_strength(int strength) {
    dsd_rtl_stream_set_fm_cma_strength(strength);
}

extern "C" void
rtl_stream_get_fm_cma_guard(int* freeze_blocks, int* accepts, int* rejects) {
    dsd_rtl_stream_get_fm_cma_guard(freeze_blocks, accepts, rejects);
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
rtl_stream_get_blanker(int* out_thr, int* out_win) {
    return dsd_rtl_stream_get_blanker(out_thr, out_win);
}

extern "C" void
rtl_stream_set_blanker(int enable, int thr, int win) {
    dsd_rtl_stream_set_blanker(enable, thr, win);
}

extern "C" int
rtl_stream_get_tuner_autogain(void) {
    return dsd_rtl_stream_get_tuner_autogain();
}

extern "C" void
rtl_stream_set_tuner_autogain(int onoff) {
    dsd_rtl_stream_set_tuner_autogain(onoff);
}

/* C4FM DD equalizer runtime config wrappers (update global runtime config) */
extern "C" void dsd_neo_set_c4fm_dd_eq(int enable, int taps, int mu_q15);
extern "C" void dsd_neo_get_c4fm_dd_eq(int* enable, int* taps, int* mu_q15);
extern "C" void dsd_neo_set_c4fm_clk(int mode);
extern "C" int dsd_neo_get_c4fm_clk(void);
extern "C" void dsd_neo_set_c4fm_clk_sync(int enable);
extern "C" int dsd_neo_get_c4fm_clk_sync(void);

extern "C" void
rtl_stream_set_c4fm_dd_eq(int onoff) {
    dsd_neo_set_c4fm_dd_eq(onoff ? 1 : 0, -1, -1);
}

extern "C" int
rtl_stream_get_c4fm_dd_eq(void) {
    int en = 0;
    dsd_neo_get_c4fm_dd_eq(&en, NULL, NULL);
    return en ? 1 : 0;
}

extern "C" void
rtl_stream_set_c4fm_dd_eq_params(int taps, int mu_q15) {
    dsd_neo_set_c4fm_dd_eq(-1, taps, mu_q15);
}

extern "C" void
rtl_stream_get_c4fm_dd_eq_params(int* taps, int* mu_q15) {
    dsd_neo_get_c4fm_dd_eq(NULL, taps, mu_q15);
}

/* C4FM clock assist mode (0=off, 1=EL, 2=MM) */
extern "C" void
rtl_stream_set_c4fm_clk(int mode) {
    if (mode < 0) {
        mode = 0;
    }
    if (mode > 2) {
        mode = 0;
    }
    dsd_neo_set_c4fm_clk(mode);
}

extern "C" int
rtl_stream_get_c4fm_clk(void) {
    return dsd_neo_get_c4fm_clk();
}

extern "C" void
rtl_stream_set_c4fm_clk_sync(int enable) {
    dsd_neo_set_c4fm_clk_sync(enable ? 1 : 0);
}

extern "C" int
rtl_stream_get_c4fm_clk_sync(void) {
    return dsd_neo_get_c4fm_clk_sync();
}

extern "C" void
rtl_stream_auto_dsp_get_config(rtl_auto_dsp_config* out) {
    dsd_rtl_stream_auto_dsp_get_config(out);
}

extern "C" void
rtl_stream_auto_dsp_set_config(const rtl_auto_dsp_config* in) {
    dsd_rtl_stream_auto_dsp_set_config(in);
}

extern "C" void
rtl_stream_auto_dsp_get_status(rtl_auto_dsp_status* out) {
    dsd_rtl_stream_auto_dsp_get_status(out);
}

/* IQ balance prefilter toggle/get */
extern "C" void
rtl_stream_toggle_iq_balance(int onoff) {
    /* implemented in rtl_sdr_fm.cpp */
    extern void dsd_rtl_stream_toggle_iq_balance(int onoff);
    dsd_rtl_stream_toggle_iq_balance(onoff);
}

extern "C" int
rtl_stream_get_iq_balance(void) {
    /* implemented in rtl_sdr_fm.cpp as dsd_rtl_stream_get_iq_balance */
    extern int dsd_rtl_stream_get_iq_balance(void);
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
