// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief C lifecycle API over the C++ RTL-SDR orchestrator.
 *
 * Owns the opaque context boundary for lifecycle, tuning, and I/O operations
 * that require an RtlSdrOrchestrator instance.
 */

#include <new>
#include <stdint.h>
#include <stdlib.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/io/rtl_stream_fwd.h"

extern "C" {
#include <dsd-neo/io/rtl_stream_c.h>

/* Backend operations whose signatures intentionally differ from the public
 * context API. */
double dsd_rtl_stream_return_pwr(void);
int dsd_rtl_stream_cqpsk_timing_bias(void);
int dsd_rtl_stream_set_rtltcp_autotune(int onoff);
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

extern "C" int
rtl_stream_tune_tagged(RtlSdrContext* ctx, uint32_t center_freq_hz, uint64_t token) {
    if (!ctx || !ctx->stream || token == 0U) {
        return RTL_STREAM_TUNE_FAILED;
    }
    return ctx->stream->tune_tagged(center_freq_hz, token);
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
rtl_stream_read(RtlSdrContext* ctx, float* out, size_t count, int* out_got) {
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
 * @brief Return mean power approximation (RMS^2 proxy) for soft squelch.
 *
 * The computation uses a small fixed sample window for efficiency.
 *
 * @param ctx Stream context (unused).
 * @return Mean power value (approximate RMS squared, normalized).
 */
extern "C" double
rtl_stream_return_pwr(const RtlSdrContext* ctx) {
    (void)ctx;
    return dsd_rtl_stream_return_pwr();
}

extern "C" int
rtl_stream_cqpsk_timing_bias(const RtlSdrContext* ctx) {
    (void)ctx;
    return dsd_rtl_stream_cqpsk_timing_bias();
}

extern "C" void
rtl_stream_set_rtltcp_autotune(int onoff) {
    (void)dsd_rtl_stream_set_rtltcp_autotune(onoff);
}
