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
void dsd_rtl_stream_p25p2_err_update(int slot, int facch_ok_delta, int facch_err_delta, int sacch_ok_delta,
                                     int sacch_err_delta, int voice_err_delta);
void dsd_rtl_stream_cqpsk_set_rrc(int enable, int alpha_percent, int span_syms);
void dsd_rtl_stream_cqpsk_set_dqpsk(int onoff);
int dsd_rtl_stream_cqpsk_get_rrc(int* enable, int* alpha_percent, int* span_syms);
int dsd_rtl_stream_cqpsk_get_dqpsk(int* onoff);
int dsd_rtl_stream_eye_get(int16_t* out, int max_samples, int* out_sps);
int dsd_rtl_stream_constellation_get(int16_t* out_xy, int max_points);
/* Auto-DSP config accessors (implemented in rtl_sdr_fm.cpp) */
void dsd_rtl_stream_auto_dsp_get_config(rtl_auto_dsp_config* out);
void dsd_rtl_stream_auto_dsp_set_config(const rtl_auto_dsp_config* in);
void dsd_rtl_stream_auto_dsp_get_status(rtl_auto_dsp_status* out);
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
    return ctx->stream->tune(center_freq_hz);
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
rtl_stream_ted_bias(const RtlSdrContext* /*ctx*/) {
    return dsd_rtl_stream_ted_bias();
}

extern "C" void
rtl_stream_set_resampler_target(int target_hz) {
    dsd_rtl_stream_set_resampler_target(target_hz);
}

extern "C" void
rtl_stream_set_ted_sps(int sps) {
    dsd_rtl_stream_set_ted_sps(sps);
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

extern "C" int
rtl_stream_constellation_get(int16_t* out_xy, int max_points) {
    return dsd_rtl_stream_constellation_get(out_xy, max_points);
}

extern "C" int
rtl_stream_eye_get(int16_t* out, int max_samples, int* out_sps) {
    return dsd_rtl_stream_eye_get(out, max_samples, out_sps);
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
