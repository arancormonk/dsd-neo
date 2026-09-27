// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The configured CTCSS/DCS receive policy's types (issue #527), with no behaviour.
 *
 * A tone set is a bounded bitmask, not a list: it copies by value into dsd_opts, the scan scopes and frontend
 * snapshots, holds no pointer and no secret, and a set cannot hold one value twice. Parsing, formatting and matching
 * live in runtime/analog_tones.h; the decoder's verdict lives in dsd_state::analog_rx.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_CORE_ANALOG_TONE_H_
#define DSD_NEO_INCLUDE_DSD_NEO_CORE_ANALOG_TONE_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief What the configured tone list does: dsd_opts::analog_tone_filter. Stable values (the INI spells them). */
typedef enum {
    DSD_TONE_FILTER_OFF = 0,   /**< No policy: ordinary carrier squelch; the list, if any, is kept. */
    DSD_TONE_FILTER_ALLOW = 1, /**< Only traffic carrying a listed tone or code is heard. */
    DSD_TONE_FILTER_BLOCK = 2, /**< Traffic carrying a listed tone or code is muted; everything else is heard. */
} dsd_tone_filter_mode;

/**
 * @brief A set of standard CTCSS tones and DCS codes.
 *
 * Bit i of @c ctcss is the i-th tone of the standard table (dsd_ctcss_tone_tenths(i)). DCS bit (2 * i + p) of the
 * @c dcs words, word (bit / 64), is the i-th standard code (dsd_dcs_code(i)) spelled in polarity p (0 = N, 1 = I),
 * as it was written: D023I and D047N are one signal (runtime/analog_tones.h), a set holds at most one spelling of it,
 * and matching goes by the signal. All zero is the empty set.
 */
typedef struct dsd_tone_set {
    uint64_t ctcss;
    uint64_t dcs[4];
} dsd_tone_set;

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_CORE_ANALOG_TONE_H_ */
