// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Unit tests: closing DSD-neo so it can restart (#830): refused while a copy runs, the session stopped and awaited
 * before the quit, and a stop that never settles, a quit the platform refuses or a process that outlives its quit
 * leaving the app open. */

#include <QCoreApplication>
#include <QEventLoop>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <cstdint>
#include <cstdio>
#include <functional>

#include "decoder_host.h"
#include "restart_closer.h"

using dsd_qt::DecoderHost;
using dsd_qt::RestartCloser;

namespace {

int failures = 0;

void
check_at(bool ok, int line, const char* why) {
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "test_ui_qt_restart_closer.cpp:%d: %s\n", line, why);
    }
}

#define CHECK(cond, why) check_at((cond), __LINE__, (why))

/* A host whose session, engine and quit answer as a case says, counting the stops and quits asked for. */
class CloseHost : public DecoderHost {
  public:
    bool active = false;
    bool busy = false;
    bool quits = true;
    int stops = 0;
    int quitCalls = 0;

    bool
    isRunning() const override {
        return active;
    }

    QString
    statusText() const override {
        return QString();
    }

    bool
    start(const QStringList& argv) override {
        (void)argv;
        return false;
    }

    void
    stop() override {
        ++stops;
    }

    SessionState
    sessionState() const override {
        return active ? Running : Idle;
    }

    bool
    engineBusy() const override {
        return busy;
    }

    bool
    quitApp() override {
        ++quitCalls;
        return quits;
    }
};

/* One closer over @p host with a clock the case moves, and what it said. */
struct Rig {
    CloseHost host;
    bool importsBusy = false;
    bool backupBusy = false;
    uint64_t now = 1000;
    QStringList outcomes;
    RestartCloser closer;

    Rig()
        : closer(
              &host, [this] { return importsBusy; }, [this] { return backupBusy; }, nullptr, [this] { return now; }) {
        QObject::connect(&closer, &RestartCloser::finished,
                         [this](const QString& outcome) { outcomes.append(outcome); });
    }
};

/* Let the closer's poll run a few times at the clock as it is. */
void
run_polls() {
    QEventLoop loop;
    QTimer::singleShot(RestartCloser::kPollMs * 3, &loop, &QEventLoop::quit);
    loop.exec();
}

void
test_refused_while_a_copy_runs() {
    for (int which = 0; which < 2; ++which) {
        Rig rig;
        rig.host.active = true;
        rig.importsBusy = which == 0;
        rig.backupBusy = which == 1;
        CHECK(rig.closer.refused(), "a running import or backup refuses the close");
        CHECK(!rig.closer.begin(), "and begin() says so");
        run_polls();
        CHECK(!rig.closer.active(), "a refused close is not active");
        CHECK(rig.host.stops == 0, "a refused close stops nothing");
        CHECK(rig.host.quitCalls == 0, "and never quits");
        CHECK(rig.outcomes.isEmpty(), "and finishes nothing");
    }
}

void
test_idle_quits_on_the_first_poll() {
    Rig rig;
    CHECK(!rig.closer.refused(), "nothing copying, nothing refuses");
    CHECK(rig.closer.begin(), "an idle app begins closing");
    CHECK(rig.closer.active(), "and is closing");
    CHECK(rig.host.stops == 0, "with no session, nothing to stop");
    CHECK(rig.host.quitCalls == 0, "the quit waits for the first poll, never inside begin()");
    run_polls();
    CHECK(rig.host.quitCalls == 1, "the first poll quits, once");
    CHECK(rig.outcomes == QStringList{QStringLiteral("closing")}, "and says the app is closing");
    CHECK(rig.closer.active(), "a closing app stays active: no start may slip in before it ends");
    CHECK(!rig.closer.begin(), "a second begin while active is refused");
}

void
test_session_stopped_and_awaited() {
    Rig rig;
    rig.host.active = true;
    rig.host.busy = true;
    CHECK(rig.closer.begin(), "a listening app begins closing");
    CHECK(rig.host.stops == 1, "its session is stopped, once");
    run_polls();
    CHECK(rig.host.quitCalls == 0, "no quit while the session still reads active");
    rig.host.active = false;
    rig.now += 5000;
    run_polls();
    CHECK(rig.host.quitCalls == 0, "no quit while the engine still reads busy");
    CHECK(rig.outcomes.isEmpty(), "still waiting at five seconds");
    CHECK(rig.host.stops == 1, "the stop is not asked again");
    rig.host.busy = false;
    run_polls();
    CHECK(rig.host.quitCalls == 1, "both settled: the app quits");
    CHECK(rig.outcomes == QStringList{QStringLiteral("closing")}, "and says so once");
}

void
test_stop_that_never_settles() {
    Rig rig;
    rig.host.active = true;
    CHECK(rig.closer.begin(), "a listening app begins closing");
    rig.now += RestartCloser::kStopWaitMs - 1;
    run_polls();
    CHECK(rig.outcomes.isEmpty(), "just short of the wait, still waiting");
    rig.now += 1;
    run_polls();
    CHECK(rig.outcomes == QStringList{QStringLiteral("stopFailed")}, "a stop that never settles fails the close");
    CHECK(!rig.closer.active(), "and leaves the app open");
    CHECK(rig.host.quitCalls == 0, "never quitting under a live session");
    rig.host.active = false;
    run_polls();
    CHECK(rig.host.quitCalls == 0, "a failed close stays failed when the session ends later");
    CHECK(rig.closer.begin(), "and a new close can begin");
}

void
test_quit_refused() {
    Rig rig;
    rig.host.quits = false;
    CHECK(rig.closer.begin(), "an idle app begins closing");
    run_polls();
    CHECK(rig.host.quitCalls == 1, "the quit is asked once");
    CHECK(rig.outcomes == QStringList{QStringLiteral("quitFailed")}, "a refused quit fails the close");
    CHECK(!rig.closer.active(), "and leaves the app open");
}

/* A quit the host took, but the process goes on (an OEM launcher or multi-window mode keeps it): the close ends as
 * failed once the quit wait is over, so the starts it held are free and the screen can say what to do. */
void
test_process_that_outlives_its_quit() {
    Rig rig;
    CHECK(rig.closer.begin(), "an idle app begins closing");
    run_polls();
    CHECK(rig.host.quitCalls == 1, "the quit is asked once");
    CHECK(rig.outcomes == QStringList{QStringLiteral("closing")}, "and the app says it is closing");
    rig.now += RestartCloser::kQuitWaitMs - 1;
    run_polls();
    CHECK(rig.closer.active(), "just short of the quit wait, the close still holds the starts");
    CHECK(rig.outcomes.size() == 1, "and has not failed");
    rig.now += 1;
    run_polls();
    CHECK(rig.outcomes == (QStringList{QStringLiteral("closing"), QStringLiteral("quitFailed")}),
          "a process still running after the quit wait fails the close");
    CHECK(!rig.closer.active(), "which releases the starts");
    CHECK(rig.host.quitCalls == 1, "the quit is not asked again");
    rig.now += RestartCloser::kQuitWaitMs;
    run_polls();
    CHECK(rig.outcomes.size() == 2, "a failed close says nothing more");
    CHECK(rig.closer.begin(), "and a new close can begin");
    run_polls();
    CHECK(rig.host.quitCalls == 2 && rig.outcomes.size() == 3, "which quits again and says closing");
    CHECK(rig.closer.active(), "and holds the starts again for its own quit wait");
}

} // namespace

int
main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    test_refused_while_a_copy_runs();
    test_idle_quits_on_the_first_poll();
    test_session_stopped_and_awaited();
    test_stop_that_never_settles();
    test_quit_refused();
    test_process_that_outlives_its_quit();
    return failures ? 1 : 0;
}
