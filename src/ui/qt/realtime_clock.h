// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The Qt frontend's reads of Qt's real-time clock.
 *
 * Real time, never the decode clock. It is for toast lifetimes, refresh and midnight timers, and
 * moments that belong to the person using the app rather than to anything decoded: when a system
 * or scan list was last listened to, when a file was imported, how old a location fix or a
 * diagnostics tail is, and the calendar day the history's day sections count from. Anything
 * compared against a decoded stamp (a call's age) reads the decode clock instead, and QML gets
 * that through MetricsModel::decodeNowMs.
 *
 * These are Qt's own reads, unchanged, because the stamps they are compared with come from Qt too:
 * a location fix is stamped with a JavaScript time value, which Qt rounds to the nearest
 * millisecond, so a truncating read would put a fresh fix a millisecond in the future. This is the
 * one frontend file that calls Qt's clock directly; every other file in src/ui/qt that wants Qt's
 * real time goes through it. Monotonic real-time reads (the metrics model's sync hold and
 * received-tone age) use dsd_realtime_mono_s() instead, like the rest of the tree.
 */

#ifndef DSD_NEO_SRC_UI_QT_REALTIME_CLOCK_H_
#define DSD_NEO_SRC_UI_QT_REALTIME_CLOCK_H_

#include <QDate>
#include <QDateTime>
#include <QtGlobal>

namespace dsd_qt {

/** @brief Real wall-clock seconds since the Unix epoch. */
inline qint64
realtimeSecsSinceEpoch() {
    return QDateTime::currentSecsSinceEpoch();
}

/** @brief Real wall-clock milliseconds since the Unix epoch, on the scale of a JavaScript time value. */
inline qint64
realtimeMSecsSinceEpoch() {
    return QDateTime::currentMSecsSinceEpoch();
}

/** @brief The real local date and time. */
inline QDateTime
realtimeCurrentDateTime() {
    return QDateTime::currentDateTime();
}

/** @brief The real local calendar date. */
inline QDate
realtimeCurrentDate() {
    return QDate::currentDate();
}

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_REALTIME_CLOCK_H_ */
