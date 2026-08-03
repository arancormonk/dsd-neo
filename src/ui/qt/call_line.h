// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Decides what a per-slot call line should say, independent of Qt.
 *
 * The canonical call state keeps an ended epoch on the slot indefinitely -- the
 * reacquisition window, the terminator heal and the history layer all read it after the
 * transmission is over, and nothing ages it out (see dsd_call_state_end_ex()). A status
 * panel wants the opposite: it is about what is on the air now, so it has to decide for
 * itself when a retained epoch stops being news. This is that decision, kept free of Qt
 * so the host test can drive it directly.
 */

#ifndef DSD_NEO_SRC_UI_QT_CALL_LINE_H_
#define DSD_NEO_SRC_UI_QT_CALL_LINE_H_

#include <dsd-neo/core/call_state.h>

namespace dsd_qt {

/**
 * @brief Seconds an ended epoch stays on the slot line before it reads as idle.
 *
 * Long enough to catch the end of a call glanced at a moment late, short enough that a
 * quiet channel does not keep advertising a transmission that finished minutes ago.
 * Matches DSD_RECENT_ACTIVITY_TTL_MS so the two decay windows in the UI agree.
 */
constexpr double kCallLineEndedHoldS = 3.0;

/** @brief What a slot line is reporting. */
enum CallLineState {
    kCallLineNone = 0, /**< Nothing has ever been observed on this slot. */
    kCallLineIdle,     /**< The slot is quiet: never active, or ended long enough ago. */
    kCallLineActive,   /**< A call epoch is open. */
    kCallLineEnded,    /**< A call ended within the hold window. */
};

/**
 * @brief Fold a call-state lookup into what the line should show.
 *
 * @param lookup       Result of dsd_call_state_get(); <= 0 means no epoch on the slot.
 * @param call         The snapshot it filled in. Only read when @p lookup is positive.
 * @param now_m        Monotonic seconds, the same clock call_state_observed_m() stamps
 *                     @c ended_m from.
 * @param hold_s       How long an ended epoch is held; see @ref kCallLineEndedHoldS.
 *
 * A negative age -- an end stamped a hair ahead of the poll -- counts as fresh rather
 * than as an expiry, which is the same thing the comparison would say for age zero.
 */
inline CallLineState
call_line_state(int lookup, const dsd_call_snapshot& call, double now_m, double hold_s = kCallLineEndedHoldS) {
    if (lookup <= 0) {
        return kCallLineNone;
    }
    if (call.phase == DSD_CALL_PHASE_ACTIVE) {
        return kCallLineActive;
    }
    if (call.phase != DSD_CALL_PHASE_ENDED) {
        return kCallLineIdle;
    }
    return ((now_m - call.ended_m) < hold_s) ? kCallLineEnded : kCallLineIdle;
}

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_CALL_LINE_H_ */
