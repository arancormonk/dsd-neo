// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Issue #902: LocalDayWatch, the frontend's one source for the viewer's local day. Before an application exists there
 * is no shared watch, and current() samples the clock itself; the first one made with the application is parented to
 * it, armed and checked. On a fake real clock installed once: a check on the same day publishes nothing; crossing
 * midnight, a jump over several days and a jump back each publish once; an early timer delivery re-arms without
 * publishing; the timer's own expiry, with no other check, finds a midnight slept through and arms again; the app
 * coming back on screen (the real applicationStateChanged signal) checks; the wait before the next check aims past
 * midnight by real instants on days a zone shortens or lengthens at midnight, and falls back to the minute cap for a
 * date a zone skipped (Pacific/Apia 2011-12-30) and after a clock went back across midnight (America/St_Johns
 * 2010-11-07); a zone change that moves today's midnight publishes once; with no test zone the process's local zone
 * counts; resetting gives the real day back.
 *
 * Each case names its zone through LocalDayWatch::setZoneForTest(), so none depends on the process taking a TZ. A case
 * whose named zone this host's time-zone data cannot give (no such zone, or not the historical transition the case is
 * about) is skipped, saying so; the fixed-offset cases run everywhere. */

#include <QByteArray>
#include <QDate>
#include <QGuiApplication>
#include <QObject>
#include <QTimeZone>
#include <QTimer>
#include <Qt>
#include <QtGlobal>
#include <dsd-neo/core/safe_api.h>
#include <stdio.h>
#include "local_day_watch.h"
#include "realtime_clock.h"

using dsd_qt::LocalDayWatch;

namespace {

int g_failures = 0;
int g_skips = 0;
qint64 g_nowMs = 0;
QTimeZone g_zone;

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

/** Count days in @p zone, for the watch and for this test's instants alike. */
void
use_zone(const QTimeZone& zone) {
    g_zone = zone;
    LocalDayWatch::setZoneForTest(zone);
}

/** A fixed offset from UTC: present on every host. */
void
use_offset(int hours) {
    use_zone(QTimeZone::fromSecondsAheadOfUtc(hours * 3600));
}

/** The named zone @p id, when this host's data has it and @p shaped holds for it (the transition a case is about);
 * otherwise the case is skipped, saying why. */
bool
use_named_zone(const char* id, bool (*shaped)(const QTimeZone&), const char* shape) {
    const QTimeZone zone{QByteArray(id)};
    if (!zone.isValid()) {
        DSD_FPRINTF(stdout, "SKIP: this host has no time-zone data for %s\n", id);
        g_skips++;
        return false;
    }
    if (shaped != nullptr && !shaped(zone)) {
        DSD_FPRINTF(stdout, "SKIP: this host's data for %s does not have %s\n", id, shape);
        g_skips++;
        return false;
    }
    use_zone(zone);
    return true;
}

/** The instant of @p date at @p time in the case's zone, the first such instant when the zone repeats it. */
qint64
local_ms(QDate date, QTime time) {
    return QDateTime(date, time, g_zone).toMSecsSinceEpoch();
}

/** When @p date began in the case's zone. */
qint64
start_ms(QDate date) {
    return date.startOfDay(g_zone).toMSecsSinceEpoch();
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

/** Deliver the watch's timer expiring, as the event loop does: a single-shot timer is no longer running when it emits
 * timeout(), so the watch has to arm it again to wake once more. */
void
fire_timer(LocalDayWatch* watch) {
    QTimer* timer = watch->timerForTest();
    expect("the watch's timer is armed", timer->isActive() && timer->isSingleShot());
    timer->stop();
    expect("the timeout is delivered", QMetaObject::invokeMethod(timer, "timeout"));
    expect("the timer is armed again for the next wakeup", timer->isActive());
}

int
armed_ms(LocalDayWatch* watch) {
    return watch->timerForTest()->interval();
}

void
set_now(QDate date, QTime time) {
    g_nowMs = local_ms(date, time);
}

/* Before the application: no watch is cached (it could arm nothing and would never be deleted), and the day is a
 * fresh sample at revision 0. */
void
test_before_the_application() {
    use_offset(-8);
    const QDate day(2026, 3, 10);
    set_now(day, QTime(12, 0));
    expect("no shared watch before the application", LocalDayWatch::shared() == nullptr);
    const LocalDayWatch::Day current = LocalDayWatch::current();
    expect("before the application: the sample's day", current.date == day && current.startMs == start_ms(day));
    expect("before the application: no revision", current.revision == 0);
    expect("and still no watch", LocalDayWatch::shared() == nullptr);
}

void
test_same_day_and_crossings(LocalDayWatch* watch) {
    use_offset(-8);
    const QDate day(2026, 3, 10);
    set_now(day, QTime(12, 0));
    watch->check();
    expect("noon: today is the sample's local date", watch->today() == day);
    expect("noon: the day starts at local midnight", watch->dayStartMs() == start_ms(day));
    const LocalDayWatch::Day current = LocalDayWatch::current();
    expect("current() is the watch's day",
           current.date == day && current.startMs == start_ms(day) && current.revision == watch->revision());
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
               watch->today() == day.addDays(1) && watch->dayStartMs() == start_ms(day.addDays(1)));
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
    use_offset(-8);
    const QDate day(2026, 6, 1);
    set_now(day, QTime(12, 0));
    watch->check();
    expect("midday: the timer waits the minute cap", armed_ms(watch) == LocalDayWatch::kMaxCheckDelayMs);
    set_now(day, QTime(23, 59, 30));
    watch->check();
    expect("30 s before midnight: armed 1.5 s past it", armed_ms(watch) == 31500);
    DaySpy spy(watch);
    set_now(day, QTime(23, 59, 58));
    fire_timer(watch);
    expect("an early timeout publishes nothing", spy.count == 0 && watch->today() == day);
    expect("an early timeout re-arms for midnight", armed_ms(watch) == 3500);
    /* Slept through midnight: nothing but the timer's own expiry checks. */
    set_now(day.addDays(1), QTime(0, 7, 12));
    fire_timer(watch);
    expect("a late timeout finds the new day", spy.count == 1 && watch->today() == day.addDays(1));
    expect("a late timeout re-arms", armed_ms(watch) == LocalDayWatch::kMaxCheckDelayMs);
    /* The next wakeup too. */
    set_now(day.addDays(2), QTime(0, 0, 3));
    fire_timer(watch);
    expect("the second wakeup finds the next day", spy.count == 2 && watch->today() == day.addDays(2));
}

void
test_application_active(LocalDayWatch* watch) {
    use_offset(-8);
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
bool
sao_paulo_shape(const QTimeZone& zone) {
    return QDate(2018, 11, 4).startOfDay(zone).time() == QTime(1, 0)
           && QDate(2019, 2, 17).startOfDay(zone).toMSecsSinceEpoch()
                      - QDateTime(QDate(2019, 2, 16), QTime(23, 0), zone).toMSecsSinceEpoch()
                  == qint64{2} * 3600000;
}

void
test_days_a_zone_reshapes_at_midnight(LocalDayWatch* watch) {
    if (!use_named_zone("America/Sao_Paulo", &sao_paulo_shape, "the 2018-11-04 and 2019-02-17 midnight changes")) {
        return;
    }
    const QDate shortNext(2018, 11, 4);
    set_now(shortNext.addDays(-1), QTime(23, 59, 50));
    watch->check();
    expect("before a skipped midnight: 10 s to the next day's start", armed_ms(watch) == 11500);
    g_nowMs += 11500;
    watch->check();
    expect("the 23 h day begins at its own start",
           watch->today() == shortNext && watch->dayStartMs() == start_ms(shortNext));

    const QDate longDay(2019, 2, 16);
    const qint64 midnight = start_ms(QDate(2019, 2, 17));
    g_nowMs = midnight - 3600000 - 10000; /* 23:59:50 the first time round */
    watch->check();
    expect("the 25 h day's first 23:59:50 is on that day", watch->today() == longDay);
    expect("the first 23:59:50: midnight is over an hour away", armed_ms(watch) == LocalDayWatch::kMaxCheckDelayMs);
    DaySpy spy(watch);
    g_nowMs = midnight - 10000; /* 23:59:50 the second time round */
    watch->check();
    expect("the repeated hour is the same day", spy.count == 0 && watch->today() == longDay);
    expect("the second 23:59:50: 10 s to midnight", armed_ms(watch) == 11500);
}

/* Pacific/Apia skipped 2011-12-30 whole: 29 December ended at the instant 31 December began. */
bool
apia_shape(const QTimeZone& zone) {
    return !QDate(2011, 12, 30).startOfDay(zone).isValid();
}

void
test_skipped_date(LocalDayWatch* watch) {
    if (!use_named_zone("Pacific/Apia", &apia_shape, "2011-12-30 skipped")) {
        return;
    }
    set_now(QDate(2011, 12, 29), QTime(23, 59, 50));
    watch->check();
    expect("before a skipped date: the minute cap", armed_ms(watch) == LocalDayWatch::kMaxCheckDelayMs);
    DaySpy spy(watch);
    g_nowMs += LocalDayWatch::kMaxCheckDelayMs;
    fire_timer(watch);
    expect("the minute check lands on 31 December", spy.count == 1 && watch->today() == QDate(2011, 12, 31));
    expect("31 December starts where 29 December ended",
           watch->dayStartMs() == local_ms(QDate(2011, 12, 29), QTime(23, 59, 50)) + 10000);
}

/* America/St_Johns ended DST at 00:01 on 2010-11-07, back to 23:01 on the 6th: the clock went back across midnight. */
bool
st_johns_shape(const QTimeZone& zone) {
    const qint64 seventh = QDateTime(QDate(2010, 11, 7), QTime(0, 0, 30), zone).toMSecsSinceEpoch();
    return QDateTime::fromMSecsSinceEpoch(seventh + 60000, zone).date() == QDate(2010, 11, 6);
}

void
test_clock_back_across_midnight(LocalDayWatch* watch) {
    if (!use_named_zone("America/St_Johns", &st_johns_shape, "the 2010-11-07 change back across midnight")) {
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
           armed_ms(watch) == LocalDayWatch::kMaxCheckDelayMs);
    g_nowMs += qint64{59} * 60000; /* 00:00:30 standard time on the 7th */
    watch->check();
    expect("the second midnight: the 7th again", spy.count == 2 && watch->today() == seventh);
}

/* The decided limit's one moving part: a zone change that moves today's midnight is picked up by the next check, as the
 * date is the same but the instant it began is not. */
void
test_zone_change_moves_midnight(LocalDayWatch* watch) {
    use_offset(-8);
    const QDate day(2026, 9, 15);
    set_now(day, QTime(12, 0));
    watch->check();
    const qint64 westStart = watch->dayStartMs();
    DaySpy spy(watch);
    use_offset(-5);
    expect("a zone change on the same date publishes the new start once",
           spy.count == 1 && watch->today() == day && watch->dayStartMs() == westStart - qint64{3} * 3600000);
    watch->check();
    expect("and nothing more", spy.count == 1);
}

/* With no test zone, the process's local zone counts, as JavaScript Date does. */
void
test_process_zone(LocalDayWatch* watch) {
    use_zone(QTimeZone());
    const QDate day(2026, 5, 4);
    g_nowMs = QDateTime(day, QTime(12, 0)).toMSecsSinceEpoch();
    watch->check();
    expect("the process's zone: its local date and midnight",
           watch->today() == day && watch->dayStartMs() == day.startOfDay().toMSecsSinceEpoch());
}

void
test_reset(const LocalDayWatch* watch) {
    const QDate before = dsd_qt::realtimeCurrentDate();
    LocalDayWatch::resetClockForTest();
    const QDate after = dsd_qt::realtimeCurrentDate();
    expect("reset: today is the real date", watch->today() == before || watch->today() == after);
}

} // namespace

int
main(int argc, char** argv) {
    LocalDayWatch::setClockForTest(&fake_clock);
    test_before_the_application();
    QGuiApplication app(argc, argv);
    LocalDayWatch* watch = LocalDayWatch::shared();
    expect("one shared watch, the application's",
           watch != nullptr && LocalDayWatch::shared() == watch && watch->parent() == &app);
    expect("the shared watch has checked and armed",
           watch != nullptr && watch->today().isValid() && watch->revision() >= 1 && watch->timerForTest()->isActive());
    if (watch == nullptr) {
        return 1;
    }

    test_same_day_and_crossings(watch);
    test_timer(watch);
    test_application_active(watch);
    test_days_a_zone_reshapes_at_midnight(watch);
    test_skipped_date(watch);
    test_clock_back_across_midnight(watch);
    test_zone_change_moves_midnight(watch);
    test_process_zone(watch);
    test_reset(watch);

    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    DSD_FPRINTF(stdout, "UI_QT_LOCAL_DAY_WATCH: all passed (%d case(s) skipped)\n", g_skips);
    return 0;
}
