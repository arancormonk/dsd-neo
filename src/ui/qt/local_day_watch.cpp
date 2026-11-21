// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "local_day_watch.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QGuiApplication>
#include <QPointer>
#include <QTimeZone>
#include <Qt>

#include "realtime_clock.h"

namespace dsd_qt {

namespace {

#ifdef DSD_NEO_TEST_HOOKS
LocalDayWatch::Clock g_test_clock = nullptr;

QTimeZone&
test_zone() {
    static QTimeZone zone;
    return zone;
}
#endif

qint64
now_ms() {
#ifdef DSD_NEO_TEST_HOOKS
    if (g_test_clock != nullptr) {
        return g_test_clock();
    }
#endif
    return realtimeMSecsSinceEpoch();
}

/* The zone days are counted in: the process's local zone, the one JavaScript Date and Qt.formatDateTime() use. */
QTimeZone
day_zone() {
#ifdef DSD_NEO_TEST_HOOKS
    if (test_zone().isValid()) {
        return test_zone();
    }
#endif
    return QTimeZone(QTimeZone::LocalTime);
}

/* The local date of @p nowMs and the instant it began. The sample lies in its own date, so that date has a start; the
 * sample stands in should Qt ever say otherwise. */
LocalDayWatch::Day
sample_day(qint64 nowMs) {
    const QTimeZone zone = day_zone();
    LocalDayWatch::Day day;
    day.date = QDateTime::fromMSecsSinceEpoch(nowMs, zone).date();
    const QDateTime start = day.date.startOfDay(zone);
    day.startMs = start.isValid() ? start.toMSecsSinceEpoch() : nowMs;
    return day;
}

QPointer<LocalDayWatch>&
shared_watch() {
    static QPointer<LocalDayWatch> watch;
    return watch;
}

#ifdef DSD_NEO_TEST_HOOKS
void
check_shared() {
    if (LocalDayWatch* watch = shared_watch().data()) {
        watch->check();
    }
}
#endif

} // namespace

LocalDayWatch::LocalDayWatch(QObject* parent) : QObject(parent), m_timer(this) {
    m_timer.setSingleShot(true);
    m_timer.setTimerType(Qt::VeryCoarseTimer);
    connect(&m_timer, &QTimer::timeout, this, &LocalDayWatch::check);
    /* Back on screen after a sleep the timer has not seen. A QCoreApplication (a headless test) has no such state. */
    if (QGuiApplication* app = qobject_cast<QGuiApplication*>(QCoreApplication::instance())) {
        connect(app, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
            if (state == Qt::ApplicationActive) {
                check();
            }
        });
    }
}

LocalDayWatch*
LocalDayWatch::shared() {
    QPointer<LocalDayWatch>& watch = shared_watch();
    /* Not before the application: a watch made then could arm no timer, follow no application state and would never
     * be deleted. current() answers meanwhile. */
    if (watch.isNull() && QCoreApplication::instance() != nullptr) {
        watch = new LocalDayWatch(QCoreApplication::instance());
        watch->check();
    }
    return watch.data();
}

LocalDayWatch::Day
LocalDayWatch::current() {
    if (const LocalDayWatch* watch = shared()) {
        return {watch->m_today, watch->m_dayStartMs, watch->m_revision};
    }
    return sample_day(now_ms());
}

void
LocalDayWatch::check() {
    const qint64 nowMs = now_ms();
    const Day day = sample_day(nowMs);
    const bool changed = day.date != m_today || day.startMs != m_dayStartMs;
    if (changed) {
        m_today = day.date;
        m_dayStartMs = day.startMs;
        m_revision++;
    }
    m_timer.start(checkDelayMs(nowMs));
    if (changed) {
        Q_EMIT dayChanged();
    }
}

int
LocalDayWatch::checkDelayMs(qint64 nowMs) {
    const QTimeZone zone = day_zone();
    const QDateTime next = QDateTime::fromMSecsSinceEpoch(nowMs, zone).date().addDays(1).startOfDay(zone);
    /* A date with no start (one a zone skipped whole), or a midnight already behind the sample (the clock went back
     * across it): no midnight to aim at, so the minute check finds the day. */
    if (!next.isValid()) {
        return kMaxCheckDelayMs;
    }
    const qint64 untilMs = next.toMSecsSinceEpoch() - nowMs;
    if (untilMs <= 0) {
        return kMaxCheckDelayMs;
    }
    return static_cast<int>(qMin<qint64>(untilMs + kPastMidnightMs, kMaxCheckDelayMs));
}

#ifdef DSD_NEO_TEST_HOOKS
void
LocalDayWatch::setClockForTest(Clock clock) {
    g_test_clock = clock;
    check_shared();
}

void
LocalDayWatch::resetClockForTest() {
    g_test_clock = nullptr;
    test_zone() = QTimeZone();
    check_shared();
}

void
LocalDayWatch::setZoneForTest(const QTimeZone& zone) {
    test_zone() = zone;
    check_shared();
}
#endif

} // namespace dsd_qt
