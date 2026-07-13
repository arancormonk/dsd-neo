// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief RAII orchestrator for RTL-SDR stream lifecycle and control.
 *
 * Manages the global radio backend's start/stop, tuning, reads, and error
 * propagation behind the public context API.
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/io/rtl_stream.h>
#include <stdint.h>
#include <stdlib.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"

extern "C" {
// Backend operations hidden from the installed context API.
int dsd_rtl_stream_open(dsd_opts* opts);
int dsd_rtl_stream_read(float* out, size_t count, dsd_opts* opts, const dsd_state* state);
uint32_t rtl_stream_output_generation(void);
int dsd_rtl_stream_tune(dsd_opts* opts, long int frequency);
int dsd_rtl_stream_soft_stop(void);
void dsd_rtl_stream_register_requested_ppm_opts(dsd_opts* active_opts, dsd_opts* caller_opts);
void dsd_rtl_stream_unregister_requested_ppm_opts(dsd_opts* active_opts, dsd_opts* caller_opts);
}

namespace {
/**
 * @brief Allocate and duplicate a snapshot of @ref dsd_opts.
 *
 * @param src Source options pointer (may be NULL).
 * @return Newly allocated duplicate or NULL on allocation failure or NULL input.
 */
static dsd_opts*
copy_opts(const dsd_opts* src) {
    if (!src) {
        return nullptr;
    }
    dsd_opts* dst = static_cast<dsd_opts*>(malloc(sizeof(dsd_opts)));
    if (!dst) {
        return nullptr;
    }
    DSD_MEMCPY(dst, src, sizeof(dsd_opts));
    return dst;
}
} // namespace

RtlSdrOrchestrator::RtlSdrOrchestrator(dsd_opts& opts) : opts_(copy_opts(&opts)), caller_opts_(&opts), started_(false) {
    if (opts_) {
        dsd_rtl_stream_register_requested_ppm_opts(opts_, &opts);
    }
}

/**
 * @brief Destructor. Ensures stop() is called and frees internal options.
 */
RtlSdrOrchestrator::~RtlSdrOrchestrator() {
    stop();
    if (opts_) {
        dsd_rtl_stream_unregister_requested_ppm_opts(opts_, caller_opts_);
        free(opts_);
        opts_ = nullptr;
    }
}

/**
 * @brief Initialize and start the stream threads and device async I/O.
 * @return 0 on success, <0 on error.
 */
int
RtlSdrOrchestrator::start() {
    if (started_) {
        return 0;
    }
    if (!opts_) {
        return -1;
    }
    int r = dsd_rtl_stream_open(opts_);
    if (r < 0) {
        return r;
    }
    started_ = true;
    return 0;
}

/**
 * @brief Stop threads and cleanup resources. Safe to call multiple times.
 * @return 0 on success.
 */
int
RtlSdrOrchestrator::stop() {
    if (!started_) {
        return 0;
    }
    /*
     * Use the soft-stop path to avoid touching the global exitflag.
     * The ncurses menu restarts/destroys RTL streams as part of reconfiguring
     * device parameters (gain/bandwidth/etc). Calling the hard close would set
     * exitflag=1 and terminate the whole application when merely closing the
     * menu. The soft-stop mirrors cleanup (threads, rings, device) without
     * requesting process exit.
    */
    dsd_rtl_stream_soft_stop();
    started_ = false;
    return 0;
}

/**
 * @brief Tune to a new center frequency in Hz.
 * @param center_freq_hz Frequency in Hz.
 * @return 0 on success, <0 on error.
 */
int
RtlSdrOrchestrator::tune(uint32_t center_freq_hz) {
    if (!started_) {
        return -1;
    }
    if (!opts_) {
        return -2;
    }
    int rc = dsd_rtl_stream_tune(opts_, (long int)center_freq_hz);
    if (rc != 0) {
        return rc;
    }
    return 0;
}

/**
 * @brief Read up to count audio samples.
 * @param out Destination buffer.
 * @param count Max samples to read.
 * @param out_got [out] Number of samples read.
 * @return 0 on success, <0 on error (e.g., shutdown).
 */
int
RtlSdrOrchestrator::read(float* out, size_t count, int& out_got) {
    if (!started_) {
        return -1;
    }
    if (!opts_) {
        return -2;
    }
    const bool replay_active = opts_->iq_replay_active != 0;
    for (;;) {
        const uint32_t generation_before = rtl_stream_output_generation();
        int got = dsd_rtl_stream_read(out, count, opts_, nullptr);
        if (got < 0) {
            return got;
        }
        /* Replay RESET/rewind boundaries wait for the submitted demod
         * generation before advancing the timeline. The final pre-boundary
         * output batch is therefore valid even when the replay thread advances
         * the output generation immediately after the read. Retrying that
         * batch can starve forever on a short looping capture. Live retunes do
         * not have this ordering guarantee and must still reject a stale
         * handoff. */
        if (!replay_active && rtl_stream_output_generation() != generation_before) {
            continue;
        }
        out_got = got;
        return 0;
    }
}
