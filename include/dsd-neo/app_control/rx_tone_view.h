// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Frontend-neutral text for the received sub-audible tone or code (issues #522, #523).
 *
 * The decoder publishes what the analog FM monitor hears below the voice band in
 * dsd_state::analog_rx. This view turns that into the one phrase every surface shows -- the
 * terminal's Call Info line, the Qt/Android monitor row -- so they cannot drift on what "no
 * carrier" or "none" means. It also carries the configured tone policy as separate text, so a
 * surface never mistakes the policy it was given for the tone it received.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_RX_TONE_VIEW_H_
#define DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_RX_TONE_VIEW_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief What the received-tone row says. Values are stable for QML consumers. */
enum {
    DSD_APP_RX_TONE_HIDDEN = 0,     /**< Detection is not running, or cannot at this input rate: render nothing. */
    DSD_APP_RX_TONE_NO_CARRIER = 1, /**< Detection runs but there is no carrier: an em dash. */
    DSD_APP_RX_TONE_DETECTING = 2,  /**< Carrier present, no verdict yet: "detecting". */
    DSD_APP_RX_TONE_LOCKED = 3,     /**< A supported tone or code is confirmed: its value. */
    DSD_APP_RX_TONE_NONE = 4,       /**< Carrier present and no supported tone or code: "none". */
};

/** @brief Room for any text this view writes, terminator included. */
enum { DSD_APP_RX_TONE_TEXT_SIZE = 32 };

/**
 * @brief Display-ready received tone.
 *
 * @c text is what the received row shows: "CTCSS 100.0 Hz" for a tone, "DCS D023N / D047I" for
 * a code. A code is shown by both standard spellings of its signal, since a receiver cannot
 * tell which one a transmitter was set to, the canonical member of its alias class first
 * (three octal digits with leading zeros and N or I for the polarity each,
 * runtime/analog_tones.h); @c dcs_code / @c dcs_inverted and @c dcs_alias_code /
 * @c dcs_alias_inverted carry the same two. @c configured_text is the configured receive
 * policy, which reads "off" until tone filtering exists (#527); it is never derived from the
 * received tone. Both are UTF-8 and always terminated.
 */
typedef struct {
    uint8_t visible;                      /**< 1 = detection runs, so the row belongs on screen. */
    uint8_t status;                       /**< One of the DSD_APP_RX_TONE_* values. */
    uint8_t kind;                         /**< dsd_analog_tone_kind of a locked tone; 0 otherwise. */
    uint8_t carrier_open;                 /**< 1 while a carrier is open (held through the short hangover). */
    int ctcss_tenths_hz;                  /**< Locked CTCSS tone in tenths of a hertz; 0 otherwise. */
    int dcs_code;                         /**< Locked DCS code as its value (023 octal = 19); 0 otherwise. */
    int dcs_alias_code;                   /**< The signal's other standard spelling (047 for D023N); 0 otherwise. */
    uint8_t dcs_inverted;                 /**< 1 = inverted polarity; never for a standard code (analog_tones.h). */
    uint8_t dcs_alias_inverted;           /**< 1 = the other spelling is inverted: always, for a standard code. */
    uint32_t generation;                  /**< The publication's reset counter, to tell one reception from the next. */
    char text[DSD_APP_RX_TONE_TEXT_SIZE]; /**< "CTCSS 100.0 Hz", "DCS D023N / D047I", "detecting", "none", "—", "". */
    char configured_text[DSD_APP_RX_TONE_TEXT_SIZE]; /**< The configured tone policy: "off" for now. */
} dsd_app_rx_tone;

/**
 * @brief Fill @p out from the published received tone.
 *
 * Zeroes @p out first, then always fills @c configured_text. @p now_m is monotonic seconds,
 * the clock dsd_time_now_monotonic_s() reads: a publication from an input that has gone quiet
 * past its stale_after_ms deadline (a stdin, UDP or TCP producer that stopped sending, a live
 * radio stream whose source stopped) reads as no carrier, because the decoder, waiting for the
 * next sample, cannot say so itself. Pass 0 to skip that check.
 * Returns 1 when the row should be shown (@c visible), 0 when it should be left out, and -1
 * for invalid arguments.
 */
int dsd_app_rx_tone_view(const dsd_opts* opts, const dsd_state* state, double now_m, dsd_app_rx_tone* out);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_RX_TONE_VIEW_H_ */
