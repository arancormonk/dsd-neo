// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "local_day_watch.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QGuiApplication>
#include <QPointer>
#include <QString>
#include <Qt>

#include "realtime_clock.h"

namespace dsd_qt {

namespace {

LocalDayWatch::Clock g_clock = &realtimeMSecsSinceEpoch;

QPointer<LocalDayWatch>&
shared_watch() {
    static QPointer<LocalDayWatch> watch;
    return watch;
}

} // namespace

LocalDayWatch::LocalDayWatch(QObject* parent) : QObject(parent), m_timer(this) {
    m_timer.setObjectName(QStringLiteral("localDayTimer"));
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
    if (watch.isNull()) {
        watch = new LocalDayWatch(QCoreApplication::instance());
        watch->check();
    }
    return watch.data();
}

void
LocalDayWatch::check() {
    const qint64 nowMs = g_clock();
    const QDateTime now = QDateTime::fromMSecsSinceEpoch(nowMs);
    const QDate day = now.date();
    /* The sample lies in its own date, so that date has a start; the sample stands in should Qt ever say otherwise. */
    const QDateTime start = day.startOfDay();
    const qint64 startMs = start.isValid() ? start.toMSecsSinceEpoch() : nowMs;
    const bool changed = day != m_today || startMs != m_dayStartMs;
    if (changed) {
        m_today = day;
        m_dayStartMs = startMs;
        m_revision++;
    }
    m_timer.start(checkDelayMs(nowMs));
    if (changed) {
        Q_EMIT dayChanged();
    }
}

int
LocalDayWatch::checkDelayMs(qint64 nowMs) {
    const QDateTime next = QDateTime::fromMSecsSinceEpoch(nowMs).date().addDays(1).startOfDay();
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

void
LocalDayWatch::setClockForTest(Clock clock) {
    g_clock = clock != nullptr ? clock : &realtimeMSecsSinceEpoch;
    if (LocalDayWatch* watch = shared_watch().data()) {
        watch->check();
    }
}

void
LocalDayWatch::resetClockForTest() {
    setClockForTest(&realtimeMSecsSinceEpoch);
}

} // namespace dsd_qt
