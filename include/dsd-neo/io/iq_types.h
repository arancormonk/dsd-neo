// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Shared IQ capture/replay public types.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_IO_IQ_TYPES_H_
#define DSD_NEO_INCLUDE_DSD_NEO_IO_IQ_TYPES_H_

#include <dsd-neo/platform/platform.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum DSD_ATTR_PACKED {
    DSD_IQ_OK = 0,
    DSD_IQ_ERR_IO = -1,
    DSD_IQ_ERR_INVALID_META = -2,
    DSD_IQ_ERR_UNSUPPORTED_VER = -3,
    DSD_IQ_ERR_UNSUPPORTED_FMT = -4,
    DSD_IQ_ERR_ALIGNMENT = -5,
    DSD_IQ_ERR_RATE_CHAIN = -6,
    DSD_IQ_ERR_RETUNE_REJECT = -7,
    DSD_IQ_ERR_ALLOC = -8,
    DSD_IQ_ERR_QUEUE_INIT = -9,
    DSD_IQ_ERR_INVALID_ARG = -10,
    /** The capture writer is sealed (dsd_iq_capture_record_event()): the submission or event was not taken. */
    DSD_IQ_ERR_SEALED = -11,
} dsd_iq_error;

typedef enum DSD_ATTR_PACKED {
    DSD_IQ_FORMAT_UNKNOWN = 0,
    DSD_IQ_FORMAT_CU8 = 1,
    DSD_IQ_FORMAT_CF32 = 2,
    DSD_IQ_FORMAT_CS16 = 3,
} dsd_iq_sample_format;

typedef enum DSD_ATTR_PACKED {
    DSD_IQ_EVENT_RETUNE = 1,
    DSD_IQ_EVENT_MUTE = 2,
    DSD_IQ_EVENT_RESET = 3,
} dsd_iq_event_kind;

typedef struct {
    dsd_iq_event_kind kind;
    uint64_t byte_offset;
    uint64_t duration_bytes;
    uint64_t center_frequency_hz;
    uint64_t capture_center_frequency_hz;
    uint32_t sample_rate_hz;
    char reason[64];
} dsd_iq_event;

/**
 * The live rate chain a capture recorded (issue #806): metadata v2's optional rate scalars, each set only with its has_
 * flag. A capture written before them records none: its nominal rate is its sample rate, and a replay works out
 * whether the device forced it from the sample rate and the DSP bandwidth (dsd_iq_rate_chain_derived_forced()).
 */
typedef struct {
    int has_nominal;
    /** nominal_rate_hz: the rate the chain was planned from (<dsd-neo/io/rate_plan.h>); sample_rate_hz stays the rate
        the device read back, which paces the replay. */
    uint32_t nominal_rate_hz;
    int has_forced;
    int device_forced; /**< capture_rate_device_forced: the device chose the rate, not dsd-neo */
    /** dsp_rate_hz, iq_step_l, iq_step_m and iq_step_approx_ppm, which come together. */
    int has_step;
    uint32_t dsp_rate_hz; /**< the complex rate after the half-band chain and the I/Q rate step */
    uint32_t step_l;      /**< iq_step_l / iq_step_m: the step's ratio, 0/0 when the chain does not step */
    uint32_t step_m;
    double step_approx_ppm; /**< iq_step_approx_ppm: how far an approximated ratio leaves dsp_rate_hz; 0 when exact */
} dsd_iq_rate_chain;

/**
 * @brief Check that @p chain agrees with itself and with the capture's @p sample_rate_hz and @p base_decimation.
 *
 * nominal_rate_hz must be within DSD_RATE_PLAN_READBACK_TOLERANCE_PPM of the sample rate. With no step, dsp_rate_hz must
 * be exactly the nominal rate over base_decimation. With one, the ratio must be the one the rate planner gives for that
 * nominal rate, those passes and dsp_rate_hz (dsd_rate_plan_compute()), its iq_step_approx_ppm included, on a chain the
 * device forced. Fields the chain does not have are not checked.
 *
 * @return DSD_IQ_OK; DSD_IQ_ERR_RATE_CHAIN with the reason in @p err_buf; DSD_IQ_ERR_INVALID_ARG for a NULL chain.
 */
int dsd_iq_rate_chain_check(const dsd_iq_rate_chain* chain, uint32_t sample_rate_hz, uint32_t base_decimation,
                            char* err_buf, size_t err_buf_size);

/**
 * @brief Whether a capture with no recorded forced flag ran a rate its device chose (issue #806 A-B4).
 *
 * A heuristic, not the device's decision: 1 unless @p sample_rate_hz is within DSD_RATE_PLAN_READBACK_TOLERANCE_PPM of
 * the rate an RTL-SDR asks for the recorded chain, the DSP bandwidth times @p base_decimation, or that times
 * @p post_downsample for a chain whose bandwidth names the rate after the post-demod decimator. So post decimation
 * alone never makes a chain forced. 0 for a capture that recorded no bandwidth.
 */
int dsd_iq_rate_chain_derived_forced(uint32_t sample_rate_hz, int dsp_bw_khz, uint32_t base_decimation,
                                     uint32_t post_downsample);

/**
 * @brief Return the required sample alignment in bytes for the sample format.
 *
 * @return 2 for cu8, 8 for cf32, 4 for cs16, or 0 for unknown format.
 */
size_t dsd_iq_sample_format_alignment_bytes(dsd_iq_sample_format format);

/**
 * @brief Return a stable sample-format name.
 *
 * @return "cu8", "cf32", "cs16", or "unknown".
 */
const char* dsd_iq_sample_format_name(dsd_iq_sample_format format);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_IO_IQ_TYPES_H_ */
