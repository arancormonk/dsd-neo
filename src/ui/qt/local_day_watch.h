// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The viewer's local calendar day, one source for every "today" the frontend draws (issue #902).
 *
 * History's TODAY / YESTERDAY sections, a Signal search detection's found time, the map's receive times and the
 * File browser's dates all say a time with its date only when it is not today. Each needs to know when today ends,
 * and a midnight timer alone cannot tell it: the timer runs on the monotonic clock, which stops while a phone sleeps
 * deeply, so one armed for midnight fires late by the sleep. The watch re-checks the real clock instead: at the next
 * local midnight, at least once a minute, and whenever the app comes back on screen (a QGuiApplication's
 * Qt::ApplicationActive; Android delivers it on resume). Each check takes one sample of the real clock
 * (realtime_clock.h) and publishes the local date and the instant it began; when either differs from the last ones
 * published, the revision goes up and dayChanged() is emitted once, however many days passed.
 *
 * Time zone (a decided limit): the process's local zone throughout, the one JavaScript Date, Qt.formatDateTime() and
 * QDateTime's local conversions use, so native and QML text agree. The app does not watch for the zone changing while
 * it runs. A change that moves today's midnight is picked up by the next check, because the start instant differs; a
 * change that keeps the same midnight instant, and times already drawn elsewhere (History and Activity clock text,
 * map pins), follow at their next redraw, the next day change or a restart.
 *
 * GUI thread only. The shared watch is created on first use once a QCoreApplication exists, parented to it: before
 * then there is no event loop to arm its timer on and no application state to follow, so shared() is null and
 * current() samples the clock itself.
 */

#ifndef DSD_NEO_SRC_UI_QT_LOCAL_DAY_WATCH_H_
#define DSD_NEO_SRC_UI_QT_LOCAL_DAY_WATCH_H_

#include <QDate> // IWYU pragma: keep -- QDate is the member type; IWYU names it <QDate> in some units and <QDateTime> in others.
#include <QDateTime> // IWYU pragma: keep -- IWYU names QDate as <QDate> in some units and <QDateTime> in others.
#include <QObject>
#include <QTimer>
#include <QtGlobal>
#ifdef DSD_NEO_TEST_HOOKS
#include <QTimeZone>
#endif

namespace dsd_qt {

class LocalDayWatch : public QObject {
    Q_OBJECT

  public:
    /** @brief A local day as published: its date, the instant it began (ms since the Unix epoch) and the revision. */
    struct Day {
        QDate date;
        qint64 startMs = 0;
        quint64 revision = 0;
    };

    /** @brief The longest wait between two checks. */
    static constexpr int kMaxCheckDelayMs = 60000;
    /** @brief How far past midnight the midnight check is armed, so a coarse timer that fires a little early lands on
     * the new day rather than re-reading the old one. */
    static constexpr int kPastMidnightMs = 1500;

    /** @brief The application's watch, created on first use (checked once as it is made); null while no
     * QCoreApplication exists. */
    static LocalDayWatch* shared();

    /** @brief The shared watch's day; with no application yet, a fresh sample of the clock at revision 0. */
    static Day current();

    /** @brief The local date of the last check's sample. */
    QDate
    today() const {
        return m_today;
    }

    /** @brief When today() began, in ms since the Unix epoch. */
    qint64
    dayStartMs() const {
        return m_dayStartMs;
    }

    /** @brief Bumped by every published change of today() or dayStartMs(). */
    quint64
    revision() const {
        return m_revision;
    }

    /** @brief Sample the clock, publish a changed day (dayChanged()), and re-arm the timer. */
    void check();

    /** @brief The wait after a check that sampled @p nowMs: to kPastMidnightMs past the next local midnight, at most
     * kMaxCheckDelayMs, and kMaxCheckDelayMs when the next date has no start or it does not lie after @p nowMs (a
     * clock moved back across midnight). */
    static int checkDelayMs(qint64 nowMs);

#ifdef DSD_NEO_TEST_HOOKS
    /** @brief A real clock in ms since the Unix epoch. */
    using Clock = qint64 (*)();

    /** @brief Read @p clock instead of the real clock, and check the shared watch at once if it exists. */
    static void setClockForTest(Clock clock);
    /** @brief Back to the real clock and the process's zone, and check the shared watch at once if it exists. */
    static void resetClockForTest();
    /** @brief Count days in @p zone instead of the process's local zone (an invalid zone: the process's), so a test
     * does not depend on the process taking a zone; checks the shared watch at once if it exists. */
    static void setZoneForTest(const QTimeZone& zone);

    /** @brief The timer that re-checks. */
    QTimer*
    timerForTest() {
        return &m_timer;
    }
#endif

  Q_SIGNALS:
    /** @brief today() or dayStartMs() changed; revision() has moved on. */
    void dayChanged();

  private:
    explicit LocalDayWatch(QObject* parent);

    QTimer m_timer;
    QDate m_today;
    qint64 m_dayStartMs = 0;
    quint64 m_revision = 0;
};

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_LOCAL_DAY_WATCH_H_ */
