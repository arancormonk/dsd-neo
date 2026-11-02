// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief TCP PCM16LE audio input backend implementation.
 *
 * The socket is read through one buffered PCM reader (<dsd-neo/platform/pcm_stream.h>), shared by the blocking read
 * (EDACS, the M17 encoder) and the bounded read (the symbol path), so neither loses bytes the other buffered.
 */

#include <dsd-neo/io/tcp_input.h>
#include <dsd-neo/platform/pcm_stream.h>
#include <dsd-neo/platform/sockets.h>
#include <stdint.h>
#include <stdlib.h>
#include "dsd-neo/core/opts_fwd.h"

/**
 * @brief Internal TCP input context structure.
 */
struct tcp_input_ctx {
    dsd_pcm_stream* stream;
    int samplerate;
    int valid;
};

tcp_input_ctx*
tcp_input_open(dsd_socket_t sockfd, int samplerate) {
    if (sockfd == DSD_INVALID_SOCKET) {
        return NULL;
    }

    tcp_input_ctx* ctx = (tcp_input_ctx*)calloc(1, sizeof(tcp_input_ctx));
    if (!ctx) {
        return NULL;
    }
    ctx->stream = dsd_pcm_stream_open_socket(sockfd);
    if (!ctx->stream) {
        free(ctx);
        return NULL;
    }
    ctx->samplerate = samplerate;
    ctx->valid = 1;
    return ctx;
}

void
tcp_input_close(tcp_input_ctx* ctx) {
    if (!ctx) {
        return;
    }
    dsd_pcm_stream_close(ctx->stream);
    ctx->stream = NULL;
    ctx->valid = 0;
    free(ctx);
}

int
tcp_input_read_sample(tcp_input_ctx* ctx, int16_t* out) {
    if (!ctx || !ctx->valid || !out) {
        return 0;
    }
    if (dsd_pcm_stream_read(ctx->stream, out) != 1) {
        ctx->valid = 0;
        return 0;
    }
    return 1;
}

int
tcp_input_read_sample_wait(tcp_input_ctx* ctx, int16_t* out, unsigned int timeout_ms) {
    if (!ctx || !ctx->valid || !out) {
        return 0;
    }
    const int got = dsd_pcm_stream_read_wait(ctx->stream, out, timeout_ms);
    if (got == DSD_PCM_STREAM_END) {
        ctx->valid = 0;
        return 0;
    }
    return got == DSD_PCM_STREAM_SAMPLE ? 1 : -1;
}

int
tcp_input_is_valid(const tcp_input_ctx* ctx) {
    if (!ctx) {
        return 0;
    }
    return ctx->valid;
}
