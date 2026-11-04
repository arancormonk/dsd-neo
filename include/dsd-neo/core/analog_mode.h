// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The demodulator a conventional analog call was heard through (NFM or AM), or unknown on audio input.
 *
 * A call observation, the canonical call and its history row carry it as a uint8_t beside the protocol: a row is analog
 * by its protocol (DSD_SYNC_ANALOG), and the mode means nothing on any other row. On a radio input (RTL-SDR, rtl_tcp,
 * SoapySDR, Airspy, an I/Q replay) dsd-neo runs the demodulator itself and knows it; on audio input (stdin, files,
 * UDP/TCP PCM, a rigctl peer) another receiver demodulated the audio, so the mode is unknown.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_CORE_ANALOG_MODE_H_
#define DSD_NEO_INCLUDE_DSD_NEO_CORE_ANALOG_MODE_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** An analog call's mode. The values are stable: history rows, the control API and the Qt history store keep them. */
typedef enum {
    DSD_ANALOG_MODE_UNKNOWN = 0, /**< Not known: audio input, or not an analog call. */
    DSD_ANALOG_MODE_NFM = 1,     /**< Narrowband FM. */
    DSD_ANALOG_MODE_AM = 2,      /**< AM. */
} dsd_analog_mode;

/** Non-zero when @p mode (dsd_analog_mode) names a demodulator, NFM or AM; zero for an unknown mode or any other value. */
static inline int
dsd_analog_mode_known(uint8_t mode) {
    return mode == (uint8_t)DSD_ANALOG_MODE_NFM || mode == (uint8_t)DSD_ANALOG_MODE_AM;
}

/**
 * The name of @p mode (dsd_analog_mode) that identifies an analog call's system, as its row's system identity string
 * and its recording's name do: "NFM", "AM", or "ANALOG" for an unknown mode or any other value.
 */
static inline const char*
dsd_analog_mode_name(uint8_t mode) {
    switch (mode) {
        case DSD_ANALOG_MODE_NFM: return "NFM";
        case DSD_ANALOG_MODE_AM: return "AM";
        default: return "ANALOG";
    }
}

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_CORE_ANALOG_MODE_H_ */
