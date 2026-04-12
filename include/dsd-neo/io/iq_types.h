// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Shared IQ capture/replay public types.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
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
} dsd_iq_error;

typedef enum {
    DSD_IQ_FORMAT_CU8 = 1,
    DSD_IQ_FORMAT_CF32 = 2,
    DSD_IQ_FORMAT_CS16 = 3,
} dsd_iq_sample_format;

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
