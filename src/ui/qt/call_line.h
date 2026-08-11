// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Qt-facing names for the shared per-slot call line decision.
 *
 * The canonical call state keeps an ended epoch on the slot indefinitely -- the
 * reacquisition window, the terminator heal and the history layer all read it after the
 * transmission is over, and nothing ages it out (see dsd_call_state_end_ex()). A status
 * panel wants the opposite: it is about what is on the air now, so it has to decide for
 * itself when a retained epoch stops being news. That decision now lives in
 * <dsd-neo/app_control/call_view.h>, shared with the terminal and the Android
 * notification; this header only binds it to the CallLineState names and values this
 * panel (and its tests) already depend on.
 */

#ifndef DSD_NEO_SRC_UI_QT_CALL_LINE_H_
#define DSD_NEO_SRC_UI_QT_CALL_LINE_H_

#include <dsd-neo/app_control/call_view.h>
#include <dsd-neo/core/call_state.h>

namespace dsd_qt {

/** @brief Seconds an ended epoch stays on the slot line before it reads as idle. */
constexpr double kCallLineEndedHoldS = DSD_APP_CALL_LINE_ENDED_HOLD_S;

/** @brief What a slot line is reporting. Values match the app-control enum. */
enum CallLineState {
    kCallLineNone = DSD_APP_CALL_LINE_NONE,
    kCallLineIdle = DSD_APP_CALL_LINE_IDLE,
    kCallLineActive = DSD_APP_CALL_LINE_ACTIVE,
    kCallLineEnded = DSD_APP_CALL_LINE_ENDED,
};

/**
 * @brief Fold a call-state lookup into what the line should show.
 *
 * Delegates to app-control so the terminal, this panel and the Android notification
 * cannot drift apart on when a retained epoch stops being news.
 */
inline CallLineState
call_line_state(int lookup, const dsd_call_snapshot& call, double now_m, double hold_s = kCallLineEndedHoldS) {
    return static_cast<CallLineState>(dsd_app_call_line_state(lookup, &call, now_m, hold_s));
}

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_CALL_LINE_H_ */
