// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Frontend-neutral decisions about what a per-slot call line should say.
 *
 * The canonical call state keeps an ended epoch on the slot indefinitely -- the
 * reacquisition window, the terminator heal and the history layer all read it after
 * the transmission is over, and nothing ages it out. A status surface wants the
 * opposite: it is about what is on the air now, so it has to decide for itself when a
 * retained epoch stops being news. This is that decision, shared so the terminal, the
 * Qt panel and the Android notification cannot drift apart.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_CALL_VIEW_H_
#define DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_CALL_VIEW_H_

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/state_fwd.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief What a slot line is reporting. */
enum {
    DSD_APP_CALL_LINE_NONE = 0, /**< Nothing has ever been observed on this slot. */
    DSD_APP_CALL_LINE_IDLE,     /**< Quiet: never active, or ended long enough ago. */
    DSD_APP_CALL_LINE_ACTIVE,   /**< A call epoch is open. */
    DSD_APP_CALL_LINE_ENDED,    /**< A call ended within the hold window. */
};

/**
 * @brief Seconds an ended epoch stays on the slot line before it reads as idle.
 *
 * Long enough to catch the end of a call glanced at a moment late, short enough that a
 * quiet channel does not keep advertising a transmission that finished minutes ago.
 * Matches DSD_RECENT_ACTIVITY_TTL_MS so the decay windows in the UI agree.
 */
#define DSD_APP_CALL_LINE_ENDED_HOLD_S 3.0

enum {
    /**
     * @brief Size of @ref dsd_app_slot_call::name.
     *
     * Its own constant rather than DSD_CALL_IDENTITY_TEXT_SIZE, because the two fields
     * are copied from differently sized sources: @c name comes from a CSV import, held
     * in @c Event_History::t_name as a @c char[200], while @c tg_text and @c src_text
     * come from the canonical call state's @c char[DSD_CALL_IDENTITY_TEXT_SIZE] fields.
     * Sizing @c name like the other two silently cut a long talkgroup alias down to 63
     * characters on its way to the hero panel.
     */
    DSD_APP_CALL_NAME_SIZE = 200,
};

/**
 * @brief Display-ready identity for one slot.
 *
 * Crypto is carried as @c algid / @c kid rather than as formatted text: each frontend
 * words it differently, and a formatted string here would force one wording on all.
 */
typedef struct {
    int state; /**< One of the DSD_APP_CALL_LINE_* values. */
    /** CSV-imported group name when one is staged for this talkgroup, else @c tg_text. */
    char name[DSD_APP_CALL_NAME_SIZE];
    char tg_text[DSD_CALL_IDENTITY_TEXT_SIZE];
    char src_text[DSD_CALL_IDENTITY_TEXT_SIZE];
    uint64_t tg_id;      /**< OTA target when one decoded, else the policy-resolved id. */
    uint32_t elapsed_ms; /**< Since the epoch started; frozen at the end for ENDED. */
    uint16_t kid;
    uint8_t algid;
    uint8_t enc; /**< Non-zero when the call reads as encrypted over the air. */
} dsd_app_slot_call;

/**
 * @brief Fold a call-state lookup into what the line should show.
 *
 * @param lookup Result of dsd_call_state_get(); <= 0 means no epoch on the slot.
 * @param call   The snapshot it filled in. Only read when @p lookup is positive.
 * @param now_m  Monotonic seconds, the clock call_state stamps @c ended_m from.
 * @param hold_s How long an ended epoch is held; see @ref DSD_APP_CALL_LINE_ENDED_HOLD_S.
 *
 * A negative age -- an end stamped a hair ahead of the poll -- counts as fresh rather
 * than as an expiry, which is what the comparison would say for age zero too.
 */
int dsd_app_call_line_state(int lookup, const dsd_call_snapshot* call, double now_m, double hold_s);

/**
 * @brief Fill @p out with the display-ready identity for @p slot.
 *
 * Zeroes @p out first, so a slot with nothing on it yields DSD_APP_CALL_LINE_NONE and
 * empty text. Safe with a NULL @p state; a NULL @p out is a no-op.
 */
void dsd_app_slot_call_view(const dsd_state* state, uint8_t slot, double now_m, dsd_app_slot_call* out);

/**
 * @brief The voice-channel frequency in Hz, or 0 when none resolves.
 *
 * Four sources in precedence order: an active P25 call epoch carrying its own frequency,
 * then @c trunk_vc_freq[0], then @c p25_vc_freq[0], then the newest live recent-activity
 * entry -- either its own frequency or a "Ch:" token resolved through @c trunk_chan_map.
 *
 * The tuner centre is deliberately not in this chain. On a trunked system a retune moves
 * the centre out from under the call, so it is not a proxy for the voice channel.
 */
long int dsd_app_vc_freq(const dsd_state* state);

/**
 * @brief The control-channel frequency in Hz, or 0 when none is known.
 *
 * A different value from @ref dsd_app_vc_freq: on a tuned trunked system the decoder is
 * sitting on the voice channel while the control channel is where it will return.
 */
long int dsd_app_cc_freq(const dsd_state* state);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_CALL_VIEW_H_ */
