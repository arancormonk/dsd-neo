// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Issue #902: LocalDayWatch, the frontend's one source for the viewer's local day. On a fake real clock installed
 * once: a check on the same day publishes nothing; crossing midnight, a jump over several days and a jump back each
 * publish once; an early timer delivery re-arms without publishing; the timer's own timeout, with no other check,
 * finds a midnight slept through; the app coming back on screen (the real applicationStateChanged signal) checks; the
 * wait before the next check aims past midnight by real instants on days a zone shortens or lengthens at midnight, and
 * falls back to the minute cap for a date a zone skipped (Pacific/Apia 2011-12-30) and after a clock went back across
 * midnight (America/St_Johns 2010-11-07); a zone change that moves today's midnight publishes once; resetting the clock gives the real day back. Each case sets the process time
 * zone first, as the app reads it. */

#include <QByteArray>
#include <QDate>
#include <QDateTime>
#include <QGuiApplication>
#include <QMetaObject>
#include <QObject>
#include <QTime>
#include <QTimeZone>
#include <QTimer>
#include <Qt>
#include <QtGlobal>
#include <ctime>
#include <dsd-neo/core/safe_api.h>
#include "local_day_watch.h"
#include "realtime_clock.h"

using dsd_qt::LocalDayWatch;

namespace {

int g_failures = 0;
qint64 g_nowMs = 0;

qint64
fake_clock() {
    return g_nowMs;
}

void
expect(const char* what, bool ok) {
    if (!ok) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", what);
        g_failures++;
    }
}

/** Make @p id the process's local zone, as TZ does at launch. */
bool
use_zone(const char* id) {
    if (!QTimeZone::isTimeZoneIdAvailable(QByteArray(id))) {
        DSD_FPRINTF(stderr, "FAIL: time zone %s is not installed\n", id);
        g_failures++;
        return false;
    }
    qputenv("TZ", QByteArray(id));
#ifdef _WIN32
    _tzset();
#else
    tzset();
#endif
    return true;
}

/** The instant of local @p date at @p time, the first such instant when the zone repeats it. */
qint64
local_ms(QDate date, QTime time) {
    return QDateTime(date, time).toMSecsSinceEpoch();
}

/** Counts dayChanged() for as long as it lives. */
struct DaySpy {
    explicit DaySpy(LocalDayWatch* watch) {
        connection = QObject::connect(watch, &LocalDayWatch::dayChanged, [this]() { count++; });
    }

    ~DaySpy() { QObject::disconnect(connection); }

    DaySpy(const DaySpy&) = delete;
    DaySpy& operator=(const DaySpy&) = delete;

    int count = 0;
    QMetaObject::Connection connection;
};

/** Deliver the watch's own timer timeout, as the event loop does when it runs out. */
void
fire_timer(LocalDayWatch* watch) {
    QTimer* timer = watch->findChild<QTimer*>(QStringLiteral("localDayTimer"));
    expect("the watch's timer is reachable", timer != nullptr && timer->isActive());
    if (timer != nullptr) {
        expect("the timeout is delivered", QMetaObject::invokeMethod(timer, "timeout"));
    }
}

void
set_now(QDate date, QTime time) {
    g_nowMs = local_ms(date, time);
}

void
test_same_day_and_crossings(LocalDayWatch* watch) {
    if (!use_zone("America/Los_Angeles")) {
        return;
    }
    const QDate day(2026, 3, 10);
    set_now(day, QTime(12, 0));
    watch->check();
    expect("noon: today is the sample's local date", watch->today() == day);
    expect("noon: the day starts at local midnight", watch->dayStartMs() == local_ms(day, QTime(0, 0)));
    const quint64 rev = watch->revision();
    {
        DaySpy spy(watch);
        set_now(day, QTime(23, 59, 59, 500));
        watch->check();
        expect("a check on the same day publishes nothing", spy.count == 0 && watch->revision() == rev);
    }
    {
        DaySpy spy(watch);
        set_now(day.addDays(1), QTime(0, 0, 1));
        watch->check();
        expect("crossing midnight publishes once", spy.count == 1 && watch->revision() == rev + 1);
        expect("crossing midnight: the new day",
               watch->today() == day.addDays(1) && watch->dayStartMs() == local_ms(day.addDays(1), QTime(0, 0)));
        watch->check();
        expect("a second check on the new day publishes nothing", spy.count == 1);
    }
    {
        DaySpy spy(watch);
        set_now(day.addDays(5), QTime(9, 30));
        watch->check();
        expect("a jump over several days publishes once", spy.count == 1 && watch->today() == day.addDays(5));
    }
    {
        DaySpy spy(watch);
        set_now(day.addDays(-2), QTime(18, 0));
        watch->check();
        expect("a jump back publishes once", spy.count == 1 && watch->today() == day.addDays(-2));
    }
}

void
test_timer(LocalDayWatch* watch) {
    if (!use_zone("America/Los_Angeles")) {
        return;
    }
    const QDate day(2026, 6, 1);
    set_now(day, QTime(12, 0));
    watch->check();
    expect("midday: the timer waits the minute cap", watch->armedDelayMsForTest() == LocalDayWatch::kMaxCheckDelayMs);
    set_now(day, QTime(23, 59, 30));
    watch->check();
    expect("30 s before midnight: armed 1.5 s past it", watch->armedDelayMsForTest() == 31500);
    DaySpy spy(watch);
    set_now(day, QTime(23, 59, 58));
    fire_timer(watch);
    expect("an early timeout publishes nothing", spy.count == 0 && watch->today() == day);
    expect("an early timeout re-arms for midnight", watch->armedDelayMsForTest() == 3500);
    /* Slept through midnight: nothing but the timer's own timeout checks. */
    set_now(day.addDays(1), QTime(0, 7, 12));
    fire_timer(watch);
    expect("a late timeout finds the new day", spy.count == 1 && watch->today() == day.addDays(1));
    expect("a late timeout re-arms", watch->armedDelayMsForTest() == LocalDayWatch::kMaxCheckDelayMs);
}

void
test_application_active(LocalDayWatch* watch) {
    if (!use_zone("America/Los_Angeles")) {
        return;
    }
    const QDate day(2026, 8, 20);
    set_now(day, QTime(22, 0));
    watch->check();
    DaySpy spy(watch);
    set_now(day.addDays(1), QTime(7, 45));
    Q_EMIT qGuiApp->applicationStateChanged(Qt::ApplicationSuspended);
    Q_EMIT qGuiApp->applicationStateChanged(Qt::ApplicationInactive);
    expect("leaving the screen checks nothing", spy.count == 0 && watch->today() == day);
    Q_EMIT qGuiApp->applicationStateChanged(Qt::ApplicationActive);
    expect("back on screen finds the new day", spy.count == 1 && watch->today() == day.addDays(1));
}

/* America/Sao_Paulo began DST at midnight on 2018-11-04 (00:00 became 01:00: a 23 h day) and ended it at midnight on
 * 2019-02-17 (00:00 went back to 23:00 on the 16th: a 25 h day). The wait is real time to the next date's start. */
void
test_days_a_zone_reshapes_at_midnight(LocalDayWatch* watch) {
    if (!use_zone("America/Sao_Paulo")) {
        return;
    }
    const QDate shortNext(2018, 11, 4);
    expect("the 23 h day starts at 01:00", shortNext.startOfDay().time() == QTime(1, 0));
    set_now(shortNext.addDays(-1), QTime(23, 59, 50));
    watch->check();
    expect("before a skipped midnight: 10 s to the next day's start", watch->armedDelayMsForTest() == 11500);
    g_nowMs += 11500;
    watch->check();
    expect("the 23 h day begins at its own start",
           watch->today() == shortNext && watch->dayStartMs() == shortNext.startOfDay().toMSecsSinceEpoch());

    const QDate longDay(2019, 2, 16);
    const qint64 midnight = QDate(2019, 2, 17).startOfDay().toMSecsSinceEpoch();
    g_nowMs = midnight - 3600000 - 10000; /* 23:59:50 the first time round */
    watch->check();
    expect("the 25 h day's first 23:59:50 is on that day", watch->today() == longDay);
    expect("the first 23:59:50: midnight is over an hour away",
           watch->armedDelayMsForTest() == LocalDayWatch::kMaxCheckDelayMs);
    DaySpy spy(watch);
    g_nowMs = midnight - 10000; /* 23:59:50 the second time round */
    watch->check();
    expect("the repeated hour is the same day", spy.count == 0 && watch->today() == longDay);
    expect("the second 23:59:50: 10 s to midnight", watch->armedDelayMsForTest() == 11500);
}

/* Pacific/Apia skipped 2011-12-30 whole: 29 December ended at the instant 31 December began. */
void
test_skipped_date(LocalDayWatch* watch) {
    if (!use_zone("Pacific/Apia")) {
        return;
    }
    expect("2011-12-30 has no start in Apia", !QDate(2011, 12, 30).startOfDay().isValid());
    set_now(QDate(2011, 12, 29), QTime(23, 59, 50));
    watch->check();
    expect("before a skipped date: the minute cap", watch->armedDelayMsForTest() == LocalDayWatch::kMaxCheckDelayMs);
    DaySpy spy(watch);
    g_nowMs += LocalDayWatch::kMaxCheckDelayMs;
    fire_timer(watch);
    expect("the minute check lands on 31 December", spy.count == 1 && watch->today() == QDate(2011, 12, 31));
    expect("31 December starts where 29 December ended",
           watch->dayStartMs() == local_ms(QDate(2011, 12, 29), QTime(23, 59, 50)) + 10000);
}

/* America/St_Johns ended DST at 00:01 on 2010-11-07, back to 23:01 on the 6th: the clock went back across midnight. */
void
test_clock_back_across_midnight(LocalDayWatch* watch) {
    if (!use_zone("America/St_Johns")) {
        return;
    }
    const QDate seventh(2010, 11, 7);
    set_now(seventh, QTime(0, 0, 30));
    watch->check();
    expect("just after the first midnight: the 7th", watch->today() == seventh);
    DaySpy spy(watch);
    g_nowMs += 60000; /* 00:01:30 daylight time is 23:01:30 standard time on the 6th */
    watch->check();
    expect("the clock went back across midnight: the 6th again",
           spy.count == 1 && watch->today() == seventh.addDays(-1));
    expect("the 6th's next midnight is behind the sample: the minute cap",
           watch->armedDelayMsForTest() == LocalDayWatch::kMaxCheckDelayMs);
    g_nowMs += 59 * 60000; /* 00:00:30 standard time on the 7th */
    watch->check();
    expect("the second midnight: the 7th again", spy.count == 2 && watch->today() == seventh);
}

/* The decided limit's one moving part: a zone change that moves today's midnight is picked up by the next check, as the
 * date is the same but the instant it began is not. */
void
test_zone_change_moves_midnight(LocalDayWatch* watch) {
    if (!use_zone("America/Los_Angeles")) {
        return;
    }
    const QDate day(2026, 9, 15);
    set_now(day, QTime(12, 0));
    watch->check();
    const qint64 westStart = watch->dayStartMs();
    if (!use_zone("America/New_York")) {
        return;
    }
    DaySpy spy(watch);
    watch->check();
    expect("a zone change on the same date publishes the new start once",
           spy.count == 1 && watch->today() == day && watch->dayStartMs() == westStart - 3 * 3600000);
    watch->check();
    expect("and nothing more", spy.count == 1);
}

void
test_reset(LocalDayWatch* watch) {
    const QDate before = dsd_qt::realtimeCurrentDate();
    LocalDayWatch::resetClockForTest();
    const QDate after = dsd_qt::realtimeCurrentDate();
    expect("reset: today is the real date", watch->today() == before || watch->today() == after);
}

} // namespace

int
main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    LocalDayWatch* watch = LocalDayWatch::shared();
    expect("one shared watch", watch != nullptr && LocalDayWatch::shared() == watch && watch->parent() == &app);
    expect("the shared watch has checked", watch->today().isValid() && watch->revision() >= 1);
    LocalDayWatch::setClockForTest(&fake_clock);

    test_same_day_and_crossings(watch);
    test_timer(watch);
    test_application_active(watch);
    test_days_a_zone_reshapes_at_midnight(watch);
    test_skipped_date(watch);
    test_clock_back_across_midnight(watch);
    test_zone_change_moves_midnight(watch);
    test_reset(watch);

    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    DSD_FPRINTF(stdout, "UI_QT_LOCAL_DAY_WATCH: all passed\n");
    return 0;
}
