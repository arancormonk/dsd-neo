// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The highest DSP rate the digital channel filters are designed at.
 *
 * Every digital channel profile (P25 C4FM/CQPSK, ProVoice, 12.5 and 6.25 kHz, WIDE, and TETRA's profile while its own
 * chain does not run) is a Blackman low-pass with the 1200 Hz transition the analog channel uses
 * (DSD_ANALOG_CHANNEL_TRANSITION_HZ), designed within the channel filter's 288 taps (DSD_CHANNEL_LPF_MAX_TAPS). The
 * tap count grows with the rate, 74 x rate / (22 x 1200) made odd, so each of those designs fits up to the same rate.
 * Above it the demodulator falls back to a fixed 63-tap prototype built for 24 kHz, whose passband scales with the
 * rate (a third of it for WIDE): a neighbouring channel then passes unfiltered.
 *
 * A plain constant with no includes, so the I/Q replay planner, which sits below the DSP and runtime libraries, can
 * keep a replay of a non-TETRA digital mode at or below it. DSP_CHANNEL_FILTERS checks it against the designs: every
 * profile designs at this rate and none does 1 Hz above it.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_DIGITAL_CHANNEL_H_
#define DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_DIGITAL_CHANNEL_H_

/** @brief The highest DSP rate in Hz at which every digital channel profile designs within 288 taps:
 *  floor((288 x 22 x 1200 - 1) / 74), the last rate whose design is 287 taps. */
#define DSD_DIGITAL_CHANNEL_MAX_RATE_HZ 102745

#endif /* DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_DIGITAL_CHANNEL_H_ */
