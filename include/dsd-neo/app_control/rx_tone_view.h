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
 * carrier" or "none" means. It also carries the tone policy in force (issue #527) as separate
 * text, with what it does to the carrier on air, so a surface never mistakes the policy it was
 * given for the tone it received. The live tone-filter editor (terminal and Qt) opens on the
 * configured policy this header also reads, and says what an edit did with its notice.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_RX_TONE_VIEW_H_
#define DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_RX_TONE_VIEW_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <stddef.h>
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

/** @brief Room for any received-tone or verdict text this view writes, terminator included. */
enum { DSD_APP_RX_TONE_TEXT_SIZE = 32 };

/** @brief Room for the tone policy text ("allow 67.0 Hz/.../…+12 (row; default block 100.0 Hz)"), terminator
 * included. */
enum { DSD_APP_RX_TONE_POLICY_TEXT_SIZE = 128 };

/**
 * @brief Display-ready received tone.
 *
 * @c text is what the received row shows: "CTCSS 100.0 Hz" for a tone, "DCS D023N / D047I" for
 * a code. A code is shown by both standard spellings of its signal, since a receiver cannot
 * tell which one a transmitter was set to, the canonical member of its alias class first
 * (three octal digits with leading zeros and N or I for the polarity each,
 * runtime/analog_tones.h); @c dcs_code / @c dcs_inverted and @c dcs_alias_code /
 * @c dcs_alias_inverted carry the same two. @c configured_text is the CTCSS/DCS receive policy
 * in force (issue #527): "off", or the mode and its list ("allow 100.0 Hz/D023N"), and while a
 * scan row's own policy runs over the configured one, " (row; default X)" naming the
 * configured policy X it shadows, as every row override reads; it is never derived from the
 * received tone. @c gate_text says what the policy does with the carrier on air: "passing",
 * "muted: checking tone", "muted: not allowed" or "muted: no tone", and "" with no carrier or no
 * policy. All three are UTF-8 and always terminated.
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
    char configured_text[DSD_APP_RX_TONE_POLICY_TEXT_SIZE]; /**< The tone policy in force: "off", "allow 100.0 Hz". */
    /** 1 = detection runs and a tone policy is in force, or a scan row set its own: the Tone filter row belongs on
        screen. Independent of @c visible: at an input rate detection cannot use, the policy still mutes. */
    uint8_t policy_visible;
    uint8_t policy_row; /**< 1 = a scan row's own policy is in force ("(row; default X)"). */
    uint8_t gate;       /**< dsd_analog_tone_gate on the carrier on air; OFF without one. */
    /** 1 = @c gate was decided for want of a tone or code within the check window (REJECTED: "muted: no tone"; an
        ALLOWED block-list pass on no tone); 0 when a confirmed value decided it, or nothing is decided. */
    uint8_t gate_no_tone;
    char gate_text[DSD_APP_RX_TONE_TEXT_SIZE]; /**< "passing", "muted: checking tone", ..., or "". */
    /** 1 = detection runs, so a tone policy acts on what the monitor plays: the Tone filter row and its live editor
        belong on screen whatever the policy, off included. Implied by @c policy_visible. */
    uint8_t policy_editable;
} dsd_app_rx_tone;

/**
 * @brief Fill @p out from the published received tone.
 *
 * Zeroes @p out first, then always fills @c configured_text ("off" without @p opts), and the
 * policy fields whenever the arguments are valid. A NULL @p state still gets @c configured_text, read
 * from @p opts alone (no scan row), and returns -1 with nothing else filled: a terminal menu row
 * before the first snapshot reads the policy its own options hold, as its editor opens on it.
 * @p now_m is monotonic seconds, the clock dsd_time_now_monotonic_s() reads: a publication from an
 * input that has gone quiet past its stale_after_ms deadline (a stdin, UDP or TCP producer that
 * stopped sending, a live radio stream whose source stopped) reads as no carrier, because the
 * decoder, waiting for the next sample, cannot say so itself; the verdict goes with it. Pass 0 to
 * skip that check. Returns 1 when the received row should be shown (@c visible), 0 when it should
 * be left out, and -1 for invalid arguments.
 */
int dsd_app_rx_tone_view(const dsd_opts* opts, const dsd_state* state, double now_m, dsd_app_rx_tone* out);

/** @brief Room for a whole tone list as the parser reads it back, terminator included (DSD_TONE_LIST_TEXT_MAX + 1):
 * what the live editor opens on and what DSD_APP_CMD_TONE_FILTER_SET carries. */
enum { DSD_APP_TONE_FILTER_LIST_SIZE = 1024 };

/** @brief Room for the notice after a tone-filter edit, terminator included (a toast, dsd_state::ui_msg). */
enum { DSD_APP_TONE_FILTER_NOTICE_SIZE = 128 };

/**
 * @brief The configured CTCSS/DCS receive policy as the live editors open on it (issue #527).
 *
 * The configured policy is the one DSD_APP_CMD_TONE_FILTER_SET edits and a save writes
 * (dsd_scan_mode_configured_tone_policy()): never a scan row's own, which runs over dsd_opts while the row is on air.
 * @c list is its list as the parser reads it back, spelled as written ("100.0/D023I"), kept with off; "" for none.
 */
typedef struct {
    int mode;             /**< dsd_tone_filter_mode of the configured policy. */
    uint8_t row_override; /**< 1 = the scan row on air sets its own policy, which shadows an edit. */
    char list[DSD_APP_TONE_FILTER_LIST_SIZE]; /**< The configured list, "" for none. */
} dsd_app_tone_filter_setting;

/**
 * @brief Fill @p out with the configured tone policy from decoder state or a frontend snapshot pair.
 *
 * Zeroes @p out first (off, no list). Returns 0, or -1 for NULL @p opts or @p out.
 */
int dsd_app_tone_filter_setting_get(const dsd_opts* opts, const dsd_state* state, dsd_app_tone_filter_setting* out);

/**
 * @brief Write the notice after an edit of the configured tone policy: "Applied: Tone filter -> allow 100.0 Hz/D023N",
 * or while a scan row's own policy is on air "Default tone filter -> allow 100.0 Hz; this channel overrides it
 * (block 67.0 Hz)", as the squelch and width edits say it. Long lists are summarised so the whole notice fits
 * DSD_APP_TONE_FILTER_NOTICE_SIZE, and the notice is ASCII, their overflow mark written "...+N" (it is a toast, which
 * the terminal's status line prints as is). Reads the configured policy and the installed row options, so it is right
 * on the decoder thread right after the edit. Returns 0, or -1 for NULL @p opts or @p out or a zero @p out_size (@p out
 * then holds "" when it can).
 */
int dsd_app_tone_filter_edit_notice(const dsd_opts* opts, const dsd_state* state, char* out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_RX_TONE_VIEW_H_ */
