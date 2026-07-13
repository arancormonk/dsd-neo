// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief RAII orchestrator for RTL-SDR stream lifecycle and control.
 *
 * Declares the C++ wrapper class that manages start/stop lifecycle, tuning,
 * and buffered reads over the RTL stream C API.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_IO_RTL_STREAM_H_
#define DSD_NEO_INCLUDE_DSD_NEO_IO_RTL_STREAM_H_

#include <dsd-neo/core/opts_fwd.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @brief RAII-class orchestrator for RTL-SDR streaming pipeline.
 *
 * Owns a stream-options snapshot, initializes and launches the backend from
 * start(), tears it down from stop(), and auto-stops on destruction.
 */
class RtlSdrOrchestrator {
  public:
    /**
     * @brief Construct a stream with an internal snapshot mirrored to caller-owned options.
     * @param opts Mutable @ref dsd_opts used to configure the stream and receive live PPM updates.
     */
    explicit RtlSdrOrchestrator(dsd_opts& opts);

    /**
     * @brief Destructor. Ensures stop() is called.
     */
    ~RtlSdrOrchestrator();

    /**
     * @brief Initialize and start the stream threads and device async I/O.
     * @return 0 on success, <0 on error.
     */
    int start();

    /**
     * @brief Stop threads and cleanup resources. Safe to call multiple times.
     * @return 0 on success.
     */
    int stop();

    /**
     * @brief Tune to a new center frequency in Hz.
     * @param center_freq_hz Frequency in Hz.
     * @return 0 on success, <0 on error.
     */
    int tune(uint32_t center_freq_hz);

    /**
     * @brief Tune with a caller-owned request ID for asynchronous completion.
     * @param center_freq_hz Frequency in Hz.
     * @param request_id Non-zero ID forwarded to the registered completion callback.
     * @return 0 when completed, 1 when deferred, or a negative error/timeout code.
     */
    int tune_tagged(uint32_t center_freq_hz, uint64_t request_id);

    /**
     * @brief Read up to count audio samples.
     * @param out Destination buffer.
     * @param count Max samples to read.
     * @param out_got [out] Number of samples read.
     * @return 0 on success, <0 on error (e.g., shutdown).
     */
    int read(float* out, size_t count, int& out_got);

  private:
    // Non-copyable to avoid accidental shared lifecycle
    RtlSdrOrchestrator(const RtlSdrOrchestrator&) = delete;
    RtlSdrOrchestrator& operator=(const RtlSdrOrchestrator&) = delete;

    // Mutable snapshot of options passed into C API
    dsd_opts* opts_;
    dsd_opts* caller_opts_;
    bool started_;
};

#endif /* DSD_NEO_INCLUDE_DSD_NEO_IO_RTL_STREAM_H_ */
