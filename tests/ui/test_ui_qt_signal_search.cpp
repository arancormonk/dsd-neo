// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

// LLVM 22/GCC 16 misclassifies these runtime test oracles as compile-time assertions.
// NOLINTBEGIN(cert-dcl03-c,misc-static-assert)

#include <QByteArray>
#include <QChar>
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QMap>
#include <QMetaType>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QTimer>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <QtGlobal>
#include <cassert>
#include <cmath>
#include <dsd-neo/app_control/frontend_runtime.h>
#include <dsd-neo/app_control/signal_search.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/runtime/control_pump.h>
#include <dsd-neo/runtime/signal_search.h>
#include <memory>
#include <stdint.h>
#include <stdio.h>
#include <utility>
#include <vector>
#include "../test_support/qt_test_paths.h"
#include "decoder_host.h"
#include "json_store.h"
#include "known_frequency_index.h"
#include "saved_systems_model.h"
#include "scan_lists_model.h"
#include "session_args.h"
#include "signal_search_model.h"

namespace {
/** A host whose session state a case moves, so the model's start on the Running edge can be observed. With
    @p signalsInitialized it also reports initialization, from initialize(), as the Android host does. */
class SessionHost : public dsd_qt::DecoderHost {
  public:
    explicit SessionHost(bool signalsInitialized = false) : m_signalsInitialized(signalsInitialized) {}

    bool
    isRunning() const override {
        return m_state == Running;
    }

    bool
    signalsSessionInitialized() const override {
        return m_signalsInitialized;
    }

    void
    initialize() {
        Q_EMIT sessionInitialized();
    }

    QString
    statusText() const override {
        return {};
    }

    SessionState
    sessionState() const override {
        return m_state;
    }

    bool
    start(const QStringList& /*argv*/) override {
        return false;
    }

    void
    stop() override {
        enter(Idle);
    }

    void
    enter(SessionState state) {
        m_state = state;
        Q_EMIT sessionStateChanged();
    }

  private:
    SessionState m_state = Idle;
    bool m_signalsInitialized;
};

/* Whether a command waits in the decoder's queue; only an open command session answers. */
bool
startQueued() {
    return dsd_runtime_controls_pending() != 0;
}

/* Empties the queue as a decoder session's teardown does, then opens command admission again. */
void
reopenAdmission() {
    dsd_app_frontend_runtime_stop();
    dsd_app_frontend_runtime_start(nullptr, nullptr);
}

/* Runs the event loop for @p ms: long enough for the model's 250 ms status tick to fire. */
void
spin(int ms) {
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

/* Android reports Running when the engine thread raises g_running, a moment before that thread's
   dsd_app_frontend_runtime_start() opens command admission. A start submitted on that edge is refused; the arm
   survives the refusal and goes in once initialization (which follows the admission edge) is reported, once. */
void
testRunningBeforeAdmission(dsd_qt::KnownFrequencyIndex* index) {
    SessionHost android(true);
    dsd_qt::SignalSearchModel search(&android, index);
    assert(search.arm().isEmpty());
    android.enter(dsd_qt::DecoderHost::Starting);
    android.enter(dsd_qt::DecoderHost::Running); // admission still closed: refused
    assert(search.error().isEmpty());
    dsd_app_frontend_runtime_start(nullptr, nullptr);
    assert(!startQueued());
    android.initialize();
    assert(startQueued());
    // Submitted once: no later edge, initialization or tick starts it again.
    reopenAdmission();
    android.initialize();
    android.enter(dsd_qt::DecoderHost::Running);
    spin(300);
    assert(!startQueued());
    android.enter(dsd_qt::DecoderHost::Stopping);
    android.enter(dsd_qt::DecoderHost::Idle);
    dsd_app_frontend_runtime_stop();
}

/* The host reports initialization ahead of the phase that arrives with it, so the session can still read Starting;
   admission is open by then, and the start goes in at once, and only once. */
void
testInitializedWhileStarting(dsd_qt::KnownFrequencyIndex* index) {
    SessionHost android(true);
    dsd_qt::SignalSearchModel search(&android, index);
    assert(search.arm().isEmpty());
    android.enter(dsd_qt::DecoderHost::Starting);
    dsd_app_frontend_runtime_start(nullptr, nullptr);
    android.initialize();
    assert(startQueued());
    reopenAdmission();
    android.enter(dsd_qt::DecoderHost::Running);
    spin(300);
    assert(!startQueued());
    android.enter(dsd_qt::DecoderHost::Idle);
    dsd_app_frontend_runtime_stop();
}

/* An arm made before its start (a USB grant still pending) waits through Idle for the session it asked for. */
void
testArmWaitsThroughIdle(dsd_qt::KnownFrequencyIndex* index) {
    SessionHost android(true);
    dsd_qt::SignalSearchModel search(&android, index);
    assert(search.arm().isEmpty());
    android.enter(dsd_qt::DecoderHost::Idle);
    dsd_app_frontend_runtime_start(nullptr, nullptr);
    android.enter(dsd_qt::DecoderHost::Starting);
    android.enter(dsd_qt::DecoderHost::Running);
    assert(startQueued());
    android.enter(dsd_qt::DecoderHost::Idle);
    dsd_app_frontend_runtime_stop();
}

/* A session that ends before admission opens takes the arm with it, whether it stops, fails or simply ends, and
   whether or not it reported Running first: the next, ordinary session starts no search. */
void
testArmEndsWithSession(dsd_qt::KnownFrequencyIndex* index, bool reachedRunning) {
    SessionHost android(true);
    dsd_qt::SignalSearchModel search(&android, index);
    for (const auto end : {dsd_qt::DecoderHost::Stopping, dsd_qt::DecoderHost::Failed, dsd_qt::DecoderHost::Idle}) {
        assert(search.arm().isEmpty());
        android.enter(dsd_qt::DecoderHost::Starting);
        if (reachedRunning) {
            android.enter(dsd_qt::DecoderHost::Running); // refused: admission is closed
        }
        android.enter(end);
        android.enter(dsd_qt::DecoderHost::Idle);
        dsd_app_frontend_runtime_start(nullptr, nullptr);
        android.enter(dsd_qt::DecoderHost::Starting);
        android.enter(dsd_qt::DecoderHost::Running);
        android.initialize();
        assert(!startQueued());
        android.enter(dsd_qt::DecoderHost::Idle);
        dsd_app_frontend_runtime_stop();
    }
}

/* A host without the initialization signal (desktop) uses Running as its readiness edge: with admission open then,
   the start goes in on that edge, and a Resume over the running session goes in at once. */
void
testRunningEdgeWithoutInitialization(dsd_qt::KnownFrequencyIndex* index) {
    SessionHost desktop;
    dsd_qt::SignalSearchModel search(&desktop, index);
    assert(search.arm().isEmpty());
    dsd_app_frontend_runtime_start(nullptr, nullptr);
    desktop.enter(dsd_qt::DecoderHost::Running);
    assert(startQueued());
    reopenAdmission();
    assert(search.arm().isEmpty());
    assert(startQueued());
    desktop.enter(dsd_qt::DecoderHost::Idle);
    dsd_app_frontend_runtime_stop();
}

/* Such a host opening admission only after Running: the refused start is kept, and the status tick submits it, once. */
void
testTickRetryWithoutInitialization(dsd_qt::KnownFrequencyIndex* index) {
    SessionHost desktop;
    dsd_qt::SignalSearchModel search(&desktop, index);
    assert(search.arm().isEmpty());
    desktop.enter(dsd_qt::DecoderHost::Running);
    assert(search.error().isEmpty());
    dsd_app_frontend_runtime_start(nullptr, nullptr);
    for (int waited = 0; waited < 5000 && !startQueued(); waited += 50) {
        spin(50);
    }
    assert(startQueued());
    reopenAdmission();
    spin(300);
    assert(!startQueued());
    desktop.enter(dsd_qt::DecoderHost::Idle);
    dsd_app_frontend_runtime_stop();
}

/* The session a queued start names: the decoder (here with no receiver, so it rejects the start) publishes it. */
uint64_t
submittedSession() {
    auto opts = std::make_unique<dsd_opts>();
    auto state = std::make_unique<dsd_state>();
    dsd_runtime_pump_controls(opts.get(), state.get());
    dsd_state_ext_free_all(state.get());
    dsd_signal_search_status status{};
    dsd_app_signal_search_status_get(&status, nullptr, 0);
    return status.session_id;
}

/* Publishes a paused search that holds the tuner as session @p session, and lets the model read it. */
void
publishPaused(dsd_qt::SignalSearchModel& search, uint64_t session, uint64_t revision) {
    dsd_signal_search_status paused{};
    paused.session_id = session;
    paused.revision = revision;
    paused.phase = DSD_SEARCH_PAUSED;
    paused.owned = 1;
    dsd_app_signal_search_publish(&paused, nullptr, nullptr);
    search.refresh();
}

/* A Resume restarts a paused search under its own session, so its hits keep numbering on. A start after a Stop is a
   new session even when the model's status, refreshed only every 250 ms, still shows the paused one: reusing that ID
   would number the new session's hits from 1 again and overwrite the old session's history. */
void
testRestartAfterPausedStop(dsd_qt::KnownFrequencyIndex* index) {
    SessionHost desktop;
    dsd_qt::SignalSearchModel search(&desktop, index);
    dsd_app_frontend_runtime_start(nullptr, nullptr);
    desktop.enter(dsd_qt::DecoderHost::Running);
    publishPaused(search, 31, 9000);
    assert(search.arm().isEmpty());
    assert(startQueued() && submittedSession() == 31);

    publishPaused(search, 32, 9100);
    desktop.enter(dsd_qt::DecoderHost::Stopping);
    dsd_app_frontend_runtime_stop(); // the decoder session's teardown, not yet seen by the model's status tick
    desktop.enter(dsd_qt::DecoderHost::Idle);
    assert(search.arm().isEmpty());
    dsd_app_frontend_runtime_start(nullptr, nullptr);
    desktop.enter(dsd_qt::DecoderHost::Starting);
    desktop.enter(dsd_qt::DecoderHost::Running);
    assert(startQueued());
    const uint64_t restarted = submittedSession();
    assert(restarted != 0 && restarted != 32);
    desktop.enter(dsd_qt::DecoderHost::Idle);
    dsd_app_frontend_runtime_stop();
}

/* Ranges saved when a dynamic squelch took 0 to 2 dB load with the 3 dB margin the receiver applied to them, rather
   than being dropped as invalid; a Level range keeps the margin it does not use. */
void
testStoredMarginBelowMinimum(dsd_qt::KnownFrequencyIndex* index) {
    const QString file = QStringLiteral("signal-search-ranges.json");
    const QJsonArray previous = dsd_qt::json_store_load_array(file);
    QVariantMap noise, level;
    {
        dsd_qt::SignalSearchModel defaults(nullptr, index);
        noise = defaults.defaultRange();
        level = defaults.defaultRange();
    }
    noise[QStringLiteral("name")] = QStringLiteral("Noise 1 dB");
    noise[QStringLiteral("squelchMode")] = 2;
    noise[QStringLiteral("squelchMargin")] = 1;
    level[QStringLiteral("name")] = QStringLiteral("Level 0 dB");
    level[QStringLiteral("squelchMode")] = 0;
    level[QStringLiteral("squelchMargin")] = 0;
    // Only a whole margin the validation took before is lifted: a malformed one is still refused, not rounded into it.
    QJsonArray stored{QJsonObject::fromVariantMap(noise), QJsonObject::fromVariantMap(level)};
    for (double malformed : {1.5, -0.25, 4294967296.0}) {
        QVariantMap bad = noise;
        bad[QStringLiteral("name")] = QStringLiteral("Malformed");
        bad[QStringLiteral("squelchMargin")] = malformed;
        stored.append(QJsonObject::fromVariantMap(bad));
    }
    assert(dsd_qt::json_store_save_array(file, stored));
    dsd_qt::SignalSearchModel search(nullptr, index);
    const QVariantList ranges = search.ranges();
    assert(ranges.size() == 2);
    assert(ranges[0].toMap().value(QStringLiteral("name")) == QStringLiteral("Noise 1 dB"));
    assert(ranges[0].toMap().value(QStringLiteral("squelchMargin")).toInt() == 3);
    assert(ranges[1].toMap().value(QStringLiteral("name")) == QStringLiteral("Level 0 dB"));
    assert(ranges[1].toMap().value(QStringLiteral("squelchMargin")).toInt() == 0);
    assert(dsd_qt::json_store_save_array(file, previous));
}

/* A Resume refused while the session winds down (admission already closed) is kept until that session ends, and
   ends with it. */
void
testResumeWhileWindingDown(dsd_qt::KnownFrequencyIndex* index) {
    SessionHost desktop;
    dsd_qt::SignalSearchModel search(&desktop, index);
    desktop.enter(dsd_qt::DecoderHost::Running);
    assert(search.arm().isEmpty());
    desktop.enter(dsd_qt::DecoderHost::Stopping);
    desktop.enter(dsd_qt::DecoderHost::Idle);
    dsd_app_frontend_runtime_start(nullptr, nullptr);
    desktop.enter(dsd_qt::DecoderHost::Running);
    spin(300);
    assert(!startQueued());
    desktop.enter(dsd_qt::DecoderHost::Idle);
    dsd_app_frontend_runtime_stop();
}
} // namespace

static void
publish(uint64_t id, bool confirmed = false) {
    dsd_signal_search_status s{};
    s.session_id = 17;
    s.revision = id + (confirmed ? 2000 : 0);
    s.phase = DSD_SEARCH_LISTENING;
    auto& h = s.hit;
    h.session_id = 17;
    h.hit_id = id;
    h.frequency_hz = 463552100;
    h.bandwidth_hz = 12500;
    h.power_dbfs = -32.123f;
    h.noise_dbfs = -67.456f;
    h.started_ms = 123456789;
    h.confirmed = confirmed;
    h.unresolved = !confirmed;
    h.gain_tenth_db = 123;
    DSD_SNPRINTF(h.input_spec, sizeof(h.input_spec), "%s", "rtltcp:127.0.0.2:1235:463.55M");
    if (confirmed) {
        DSD_SNPRINTF(h.protocol, sizeof(h.protocol), "%s", "DMR");
        h.source = 123;
    }
    dsd_app_signal_search_publish(&s, &h, nullptr);
}

int
main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("dsd-search-tests"));
    dsd_test_qt_isolate_paths();
    QTemporaryDir temp;
    assert(temp.isValid());
    const QString journal = temp.filePath(QStringLiteral("history"));
    assert(dsd_app_signal_search_store_open(journal.toUtf8().constData()) == 0);
    publish(1);
    dsd_app_signal_search_store_flush();
    const auto before = QFileInfo(journal).size();
    for (int i = 0; i < 50; ++i) {
        publish(1);
    }
    dsd_app_signal_search_store_flush();
    assert(QFileInfo(journal).size() == before);
    publish(1, true);
    std::vector<dsd_signal_search_hit> hits(1000);
    assert(dsd_app_signal_search_history_get(hits.data(), hits.size()) == 1);
    assert(hits[0].confirmed && hits[0].source == 123);
    // Reattaching to an already open store must not end the current detection.
    assert(dsd_app_signal_search_store_open(journal.toUtf8().constData()) == 0);
    assert(dsd_app_signal_search_history_get(hits.data(), hits.size()) == 1 && !hits[0].ended);
    for (int i = 2; i <= 1005; ++i) {
        publish(i);
    }
    dsd_app_signal_search_permanent_avoid(463550000);
    dsd_app_signal_search_permanent_avoid(463550000);
    dsd_app_signal_search_store_flush();
    dsd_app_signal_search_store_close();
    assert(dsd_app_signal_search_store_open(journal.toUtf8().constData()) == 0);
    assert(dsd_app_signal_search_history_get(hits.data(), hits.size()) == 1000);
    assert(hits.front().hit_id == 1005 && hits.back().hit_id == 6 && hits.front().ended);
    uint32_t avoids[4];
    assert(dsd_app_signal_search_avoids_get(avoids, 4) == 1 && avoids[0] == 463550000);
    assert(dsd_app_signal_search_avoid_remove(463550000) == 0);
    dsd_app_signal_search_store_close();
    FILE* f = dsd_fopen_private(journal.toUtf8().constData(), "ab");
    assert(f);
    assert(fwrite("DSS", 1, 3, f) == 3);
    fclose(f);
    assert(dsd_app_signal_search_store_open(journal.toUtf8().constData()) == 0);
    char error[256];
    dsd_app_signal_search_status_get(nullptr, error, sizeof(error));
    assert(error[0]);
    assert(dsd_app_signal_search_history_get(hits.data(), hits.size()) == 1000);
    dsd_app_signal_search_store_close();
    // Failed storage retains the hit, surfaces a failure and retries on an explicit flush.
    const QString missing = temp.filePath(QStringLiteral("missing/history"));
    assert(dsd_app_signal_search_store_open(missing.toUtf8().constData()) == 0);
    publish(1010);
    dsd_app_signal_search_store_flush();
    dsd_app_signal_search_status_get(nullptr, error, sizeof(error));
    assert(error[0]);
    assert(dsd_app_signal_search_history_get(hits.data(), hits.size()) == 1);
    assert(QDir().mkpath(QFileInfo(missing).absolutePath()));
    dsd_app_signal_search_store_flush();
    dsd_app_signal_search_status_get(nullptr, error, sizeof(error));
    assert(!error[0]);
    assert(QFileInfo(missing).size() > 0);
    dsd_app_signal_search_store_close();

    dsd_qt::SavedSystemsModel systems;
    dsd_qt::ScanListsModel lists;
    assert(systems.add(
        {{QStringLiteral("name"), QStringLiteral("Shop")}, {QStringLiteral("freqMhz"), QStringLiteral("463.5500")}}));
    dsd_qt::KnownFrequencyIndex index(&systems, &lists);
    auto matches = index.lookup(463552100, 12500);
    assert(matches.size() == 1 && matches[0].toMap().value(QStringLiteral("name")) == QStringLiteral("Shop"));
    assert(index.lookup(463570000, 12500).isEmpty());
    assert(index.lookup(463555000, 25000.0 / 3).isEmpty());
    assert(systems.add({{QStringLiteral("name"), QStringLiteral("Other shop")},
                        {QStringLiteral("freqMhz"), QStringLiteral("463.5500")}}));
    assert(index.lookup(463552100, 12500).size() == 2);
    assert(systems.add(
        {{QStringLiteral("name"), QStringLiteral("Exact")}, {QStringLiteral("freqMhz"), QStringLiteral("463.5521")}}));
    assert(index.lookup(463552100, 12500).size() == 1);
    const QString channels = temp.filePath(QStringLiteral("channels.csv"));
    f = dsd_fopen_private(channels.toUtf8().constData(), "wb");
    assert(f);
    const char csv[] = "Channel,Frequency\n1,464000000\n2,464025000\n";
    assert(fwrite(csv, 1, sizeof(csv) - 1, f) == sizeof(csv) - 1);
    fclose(f);
    assert(
        systems.add({{QStringLiteral("name"), QStringLiteral("Mapped")}, {QStringLiteral("chanCsvPath"), channels}}));
    assert(index.lookup(464000500, 12500).size() == 1);
    const QString targets = temp.filePath(QStringLiteral("targets.csv"));
    f = dsd_fopen_private(targets.toUtf8().constData(), "wb");
    assert(f);
    const char targetCsv[] = "id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes,options\n"
                             "Tower,am-conventional,118300000,,,,,--am-bandwidth-hz 6000\n"
                             "Trunk,dmr-trunk,464100000,channels.csv,,,,\n";
    assert(fwrite(targetCsv, 1, sizeof(targetCsv) - 1, f) == sizeof(targetCsv) - 1);
    fclose(f);
    assert(lists.add({{QStringLiteral("name"), QStringLiteral("Imported list")},
                      {QStringLiteral("targetSource"), QStringLiteral("csv")},
                      {QStringLiteral("targetsCsvPath"), targets}}));
    assert(index.lookup(118302100, 12500).size() == 1);
    assert(index.lookup(118304000, 12500).isEmpty());
    assert(index.lookup(464100000, 12500).size() == 1);
    assert(index.lookup(464000000, 12500).size() == 2); // saved map and nested target map
    // A channel map row without its own mode or bandwidth takes the saved system's channel width: an NXDN48 system's
    // 6.25 kHz channels match within 3.125 kHz, not the 6.25 kHz a 12.5 kHz default would allow.
    const QString narrowChannels = temp.filePath(QStringLiteral("nxdn48-channels.csv"));
    f = dsd_fopen_private(narrowChannels.toUtf8().constData(), "wb");
    assert(f);
    const char narrowCsv[] = "Channel,Frequency\n1,465000000\n";
    assert(fwrite(narrowCsv, 1, sizeof(narrowCsv) - 1, f) == sizeof(narrowCsv) - 1);
    fclose(f);
    assert(systems.add({{QStringLiteral("name"), QStringLiteral("NXDN48 site")},
                        {QStringLiteral("decodeFlag"), QStringLiteral("-fi")},
                        {QStringLiteral("chanCsvPath"), narrowChannels}}));
    assert(index.lookup(465002000, 12500).size() == 1);
    assert(index.lookup(465005000, 12500).isEmpty());

    // dPMR and D-STAR run 6.25 kHz channels like NXDN48: saved by their decode flags (case-significant: -fm is dPMR,
    // -fM is AM), or named by a channel map's mode column.
    const struct {
        const char* flag;
        const char* freq;
        double hz;
    } narrow[] = {{"-fm", "466.000000", 466000000}, {"-fd", "467.000000", 467000000}};

    for (const auto& n : narrow) {
        assert(systems.add({{QStringLiteral("name"), QString::fromLatin1(n.flag)},
                            {QStringLiteral("decodeFlag"), QString::fromLatin1(n.flag)},
                            {QStringLiteral("freqMhz"), QString::fromLatin1(n.freq)}}));
        assert(index.lookup(n.hz + 2000, 12500).size() == 1);
        assert(index.lookup(n.hz + 5000, 12500).isEmpty());
    }
    const QString narrowModes = temp.filePath(QStringLiteral("narrow-modes.csv"));
    f = dsd_fopen_private(narrowModes.toUtf8().constData(), "wb");
    assert(f);
    const char modesCsv[] = "Channel,Frequency,mode\n1,471000000,dpmr\n2,472000000,dstar\n";
    assert(fwrite(modesCsv, 1, sizeof(modesCsv) - 1, f) == sizeof(modesCsv) - 1);
    fclose(f);
    assert(systems.add(
        {{QStringLiteral("name"), QStringLiteral("Mixed map")}, {QStringLiteral("chanCsvPath"), narrowModes}}));
    for (double hz : {471000000.0, 472000000.0}) {
        assert(index.lookup(hz + 2000, 12500).size() == 1);
        assert(index.lookup(hz + 5000, 12500).isEmpty());
    }

    // An analog system's mode-less map rows take the width its options set for its own kind: an NFM (-fA) system's
    // rows ignore its AM width, and the --name=value form the CLI takes counts like --name value.
    const struct {
        const char* name;
        const char* flag;
        const char* extra;
        const char* csv;
        double hz;
    } analogMaps[] = {
        {"Mixed NFM", "-fA", "-Y --nfm-bandwidth-hz 12500 --am-bandwidth-hz 6000", "analog-nfm.csv", 151000000},
        {"Wide AM", "-fM", "-Y --am-bandwidth-hz=12000", "analog-am.csv", 119300000}};

    for (const auto& a : analogMaps) {
        const QString path = temp.filePath(QString::fromLatin1(a.csv));
        f = dsd_fopen_private(path.toUtf8().constData(), "wb");
        assert(f);
        const QByteArray rows = QByteArray("Channel,Frequency\n1,") + QByteArray::number((qlonglong)a.hz) + "\n";
        assert(fwrite(rows.constData(), 1, (size_t)rows.size(), f) == (size_t)rows.size());
        fclose(f);
        assert(systems.add({{QStringLiteral("name"), QString::fromLatin1(a.name)},
                            {QStringLiteral("decodeFlag"), QString::fromLatin1(a.flag)},
                            {QStringLiteral("extraArgs"), QString::fromLatin1(a.extra)},
                            {QStringLiteral("chanCsvPath"), path}}));
        assert(index.lookup(a.hz + 5000, 12500).size() == 1);
    }

    // A saved system's mode is its decode flag, as the rest of the app reads it: a decoder chosen only through its extra
    // arguments is not read, so such a system is labeled as its decode flag's 12.5 kHz channel (a documented limit).
    assert(systems.add({{QStringLiteral("name"), QStringLiteral("Extra-args AM")},
                        {QStringLiteral("decodeFlag"), QString()},
                        {QStringLiteral("extraArgs"), QStringLiteral("-fM --am-bandwidth-hz 6000")},
                        {QStringLiteral("freqMhz"), QStringLiteral("120.300000")}}));
    assert(index.lookup(120305000, 12500).size() == 1);
    // The live views name the tuned channel at the width the receiver filters, or 12.5 kHz when it publishes none: a
    // 6 kHz AM channel 5 kHz from a saved 12.5 kHz frequency is not that system.
    assert(index.tunedNames(463555100, 0) == QStringList{QStringLiteral("Exact")});
    assert(index.tunedNames(463555100, 12500) == QStringList{QStringLiteral("Exact")});
    assert(index.tunedNames(463557100, 6000).isEmpty());
    assert(index.tunedNames(463557100, 12500) == QStringList{QStringLiteral("Exact")});
    // Cached lookup keeps working without any rendering-time file reads.
    QFile::remove(channels);
    assert(index.lookup(464000500, 12500).size() == 2);

    dsd_qt::SignalSearchModel model(nullptr, &index);
    auto range = model.defaultRange();
    range[QStringLiteral("stepNum")] = 25000;
    range[QStringLiteral("stepDen")] = 3;
    range[QStringLiteral("startMHz")] = QStringLiteral("118.000000");
    range[QStringLiteral("endMHz")] = QStringLiteral("118.100000");
    range[QStringLiteral("am")] = true;
    range[QStringLiteral("digital")] = 0;
    range[QStringLiteral("bandwidth")] = 6000;
    range[QStringLiteral("squelchMode")] = 1;
    assert(model.saveRange(0, range).isEmpty());
    assert(model.ranges().front().toMap().value(QStringLiteral("stepDen")).toInt() == 3);
    auto bad = range;
    bad[QStringLiteral("stepDen")] = 0;
    assert(!model.saveRange(0, bad).isEmpty());
    bad = range;
    bad[QStringLiteral("endMHz")] = QStringLiteral("nan");
    assert(!model.saveRange(0, bad).isEmpty());
    publish(77);
    model.refresh();
    const auto frozen = model.history().front().toMap();
    publish(78, true);
    model.refresh();
    const auto saved = model.systemForHit(frozen, {{QStringLiteral("sourceType"), QStringLiteral("usb")}});
    assert(saved.value(QStringLiteral("freqMhz")) == QStringLiteral("463.552100"));
    assert(saved.value(QStringLiteral("sourceType")) == QStringLiteral("rtltcp"));
    assert(saved.value(QStringLiteral("host")) == QStringLiteral("127.0.0.2"));
    assert(saved.value(QStringLiteral("port")).toInt() == 1235);
    assert(saved.value(QStringLiteral("decodeFlag")) == QStringLiteral("-fA"));
    assert(saved.value(QStringLiteral("searchUnresolved")).toBool());
    const auto confirmed = model.systemForHit(model.history().front().toMap(), {});
    assert(confirmed.value(QStringLiteral("decodeFlag")) == QStringLiteral("-fs"));
    auto nxdn = model.history().front().toMap();
    nxdn[QStringLiteral("protocol")] = QStringLiteral("NXDN48");
    assert(model.systemForHit(nxdn, {}).value(QStringLiteral("decodeFlag")) == QStringLiteral("-fi"));
    nxdn[QStringLiteral("protocol")] = QStringLiteral("NXDN96");
    assert(model.systemForHit(nxdn, {}).value(QStringLiteral("decodeFlag")) == QStringLiteral("-fn"));
    // The captured receiver settings reach the found system in the saved-system shape, each source only its own:
    // PPM text and an explicit bias tee (an Off is 0, never the "follow the app" -1) for an RTL, the native map for
    // an Airspy.
    auto rtl = frozen;
    rtl[QStringLiteral("ppm")] = -3;
    rtl[QStringLiteral("biasTee")] = true;
    const auto rtlSystem = model.systemForHit(rtl, {});
    assert(rtlSystem.value(QStringLiteral("ppm")).typeId() == QMetaType::QString);
    assert(rtlSystem.value(QStringLiteral("ppm")).toString() == QStringLiteral("-3"));
    assert(rtlSystem.value(QStringLiteral("biasTee")).typeId() == QMetaType::Int);
    assert(rtlSystem.value(QStringLiteral("biasTee")).toInt() == 1);
    assert(!rtlSystem.contains(QStringLiteral("airspy")));
    rtl[QStringLiteral("biasTee")] = false;
    assert(model.systemForHit(rtl, {}).value(QStringLiteral("biasTee")).toInt() == 0);
    // The detection's tuner gain becomes the whole-dB gain override a saved session starts with: the -i spec's gain
    // field, where 0 is the tuner's AGC and a manual gain is 1 to 49 dB. AGC stays AGC, and a manual gain stays manual
    // at the nearest whole dB in that range, never truncated: a manual 0 or 0.9 dB must not become AGC.
    assert(rtlSystem.value(QStringLiteral("gainDb")).typeId() == QMetaType::Int);
    assert(rtlSystem.value(QStringLiteral("gainDb")).toInt() == 12); // published as manual 12.3 dB

    const struct {
        double db;
        bool automatic;
        int saved;
    } gains[] = {{0.0, true, 0},    {27.9, true, 0},  {0.0, false, 1},   {0.9, false, 1},
                 {1.4, false, 1},   {1.5, false, 2},  {29.7, false, 30}, {33.8, false, 34},
                 {49.6, false, 49}, {-1.0, false, 1}, {14.4, false, 14}};

    for (const auto& gain : gains) {
        rtl[QStringLiteral("gain")] = gain.db;
        rtl[QStringLiteral("gainAuto")] = gain.automatic;
        const QVariant savedGain = model.systemForHit(rtl, {}).value(QStringLiteral("gainDb"));
        assert(savedGain.typeId() == QMetaType::Int && savedGain.toInt() == gain.saved);
    }
    // The saved session starts the tuner that way: manual 0 dB starts at manual 1 dB, not on the AGC's 0.
    rtl[QStringLiteral("gain")] = 0.0;
    rtl[QStringLiteral("gainAuto")] = false;
    dsd_qt::SessionArgsError startError = dsd_qt::SessionArgsError::None;
    const QStringList startArgs = dsd_qt::session_args_build(model.systemForHit(rtl, {}), {}, &startError);
    assert(startError == dsd_qt::SessionArgsError::None && startArgs.indexOf(QStringLiteral("-i")) >= 0);
    const QStringList spec = startArgs.value(startArgs.indexOf(QStringLiteral("-i")) + 1).split(QLatin1Char(':'));
    assert(spec.value(0) == QStringLiteral("rtltcp") && spec.value(4) == QStringLiteral("1"));
    rtl[QStringLiteral("gainAuto")] = true;
    const QStringList agcArgs = dsd_qt::session_args_build(model.systemForHit(rtl, {}), {}, &startError);
    assert(agcArgs.value(agcArgs.indexOf(QStringLiteral("-i")) + 1).split(QLatin1Char(':')).value(4)
           == QStringLiteral("0"));
    auto airspy = frozen;
    airspy[QStringLiteral("inputSpec")] = QStringLiteral("airspy:463.55M");
    QVariantMap native = airspy.value(QStringLiteral("airspy")).toMap();
    assert(native.size() == 11);
    native[QStringLiteral("serial")] = QStringLiteral("A1B2C3D4E5F60718");
    native[QStringLiteral("gain_mode")] = QStringLiteral("manual");
    native[QStringLiteral("lna_gain")] = 12;
    native[QStringLiteral("bias_tee")] = 1;
    airspy[QStringLiteral("airspy")] = native;
    const auto airspySystem = model.systemForHit(airspy, {});
    assert(airspySystem.value(QStringLiteral("sourceType")) == QStringLiteral("airspy"));
    assert(airspySystem.value(QStringLiteral("airspy")).toMap() == native);
    assert(!airspySystem.contains(QStringLiteral("ppm")) && !airspySystem.contains(QStringLiteral("biasTee")));
    // An Airspy's gains are its native map's; the RTL tuner gain a detection records does not describe it.
    assert(airspySystem.value(QStringLiteral("gainDb")).typeId() == QMetaType::Int);
    assert(airspySystem.value(QStringLiteral("gainDb")).toInt() == -1);

    // History is copied and rebuilt only when the store changed it, not on every status revision.
    int historySignals = 0, statusSignals = 0, avoidSignals = 0;
    QObject::connect(&model, &dsd_qt::SignalSearchModel::historyChanged, [&] { ++historySignals; });
    QObject::connect(&model, &dsd_qt::SignalSearchModel::changed, [&] { ++statusSignals; });
    QObject::connect(&model, &dsd_qt::SignalSearchModel::avoidsChanged, [&] { ++avoidSignals; });
    const uint64_t historyRevision = dsd_app_signal_search_history_revision();
    for (int i = 0; i < 5; ++i) {
        dsd_signal_search_status searching{};
        searching.session_id = 17;
        searching.revision = 5000 + (uint64_t)i;
        searching.phase = DSD_SEARCH_LISTENING;
        searching.owned = 1;
        searching.live_power_valid = 1;
        searching.live_power_dbfs = -42.5f - (float)i;
        dsd_app_signal_search_publish(&searching, nullptr, nullptr);
        model.refresh();
    }
    assert(statusSignals == 5 && historySignals == 0);
    assert(dsd_app_signal_search_history_revision() == historyRevision);
    assert(model.status().value(QStringLiteral("owned")).toBool());
    assert(model.status().value(QStringLiteral("livePowerValid")).toBool());
    assert(std::abs(model.status().value(QStringLiteral("livePower")).toDouble() + 46.5) < 1e-3);
    publish(78, true); // the same record again: coalesced, so the history is unchanged
    model.refresh();
    assert(historySignals == 0 && dsd_app_signal_search_history_revision() == historyRevision);
    publish(79);
    model.refresh();
    assert(historySignals == 1 && model.history().front().toMap().value(QStringLiteral("id")) == QStringLiteral("79"));
    uint64_t snapshotRevision = 0;
    std::vector<dsd_signal_search_hit> newest(2);
    assert(dsd_app_signal_search_history_snapshot(newest.data(), newest.size(), &snapshotRevision) == 2);
    assert(newest[0].hit_id == 79 && snapshotRevision == dsd_app_signal_search_history_revision());

    // Session avoids are mirrored per session, listed apart from permanent ones and cleared when the session stops.
    dsd_app_signal_search_session_avoid_add(17, 463550000);
    dsd_app_signal_search_session_avoid_add(17, 463550000);
    dsd_app_signal_search_session_avoid_add(17, 463575000);
    model.refresh();
    assert(avoidSignals == 1 && model.sessionAvoids().size() == 2 && model.avoids().isEmpty());
    dsd_app_signal_search_session_avoid_remove(463550000);
    model.refresh();
    assert(model.sessionAvoids().size() == 1 && model.sessionAvoids().front().toUInt() == 463575000);
    dsd_app_signal_search_session_avoid_add(18, 463600000); // another session starts its own list
    model.refresh();
    assert(model.sessionAvoids().size() == 1 && model.sessionAvoids().front().toUInt() == 463600000);
    dsd_signal_search_status stopped{};
    stopped.session_id = 18;
    stopped.revision = 6000;
    stopped.phase = DSD_SEARCH_STOPPED;
    dsd_app_signal_search_publish(&stopped, nullptr, nullptr);
    model.refresh();
    assert(model.sessionAvoids().isEmpty() && !model.status().value(QStringLiteral("owned")).toBool());

    // A rejected start owns nothing, and decoder-session teardown retires it while keeping its message.
    dsd_signal_search_status rejected{};
    rejected.session_id = 19;
    rejected.revision = 7000;
    rejected.phase = DSD_SEARCH_ERROR;
    DSD_SNPRINTF(rejected.message, sizeof(rejected.message), "%s", "A search supports up to 100,000 enabled channels");
    dsd_app_signal_search_publish(&rejected, nullptr, nullptr);
    model.refresh();
    assert(model.status().value(QStringLiteral("phase")).toInt() == DSD_SEARCH_ERROR);
    assert(!model.status().value(QStringLiteral("owned")).toBool());
    dsd_app_signal_search_session_ended();
    model.refresh();
    assert(model.status().value(QStringLiteral("phase")).toInt() == DSD_SEARCH_STOPPED);
    assert(model.status().value(QStringLiteral("message")).toString().contains(QStringLiteral("100,000")));

    // The engine's aggregate limit is checked before a start is ever submitted.
    auto large = model.defaultRange();
    large[QStringLiteral("startMHz")] = QStringLiteral("100.000000");
    large[QStringLiteral("endMHz")] = QStringLiteral("850.000000"); // 60,001 channels at 12.5 kHz
    assert(model.saveRange(0, large).isEmpty() && model.saveRange(-1, large).isEmpty());
    assert(model.arm().contains(QStringLiteral("100,000")));
    model.removeRange(1);
    assert(model.arm().isEmpty());
    model.cancelArm();

    // A refused arm leaves nothing armed. It rebuilds the pending configuration as it checks it, so an earlier arm
    // left standing would start that half-built configuration when the session comes up. The decoder session's
    // command admission is open, so a start the model submitted would be queued.
    {
        SessionHost host;
        dsd_qt::SignalSearchModel armed(&host, &index);
        assert(armed.arm().isEmpty());
        assert(armed.saveRange(-1, large).isEmpty());
        assert(armed.arm().contains(QStringLiteral("100,000")));
        dsd_app_frontend_runtime_start(nullptr, nullptr);
        host.enter(dsd_qt::DecoderHost::Running);
        assert(!startQueued() && armed.error().isEmpty());
        host.enter(dsd_qt::DecoderHost::Idle);
        dsd_app_frontend_runtime_stop();
        armed.removeRange(1);
    }

    // A start is submitted once the decoder's command admission takes it, and never outlives its session.
    testRunningBeforeAdmission(&index);
    testInitializedWhileStarting(&index);
    testArmWaitsThroughIdle(&index);
    testArmEndsWithSession(&index, false);
    testArmEndsWithSession(&index, true);
    testRunningEdgeWithoutInitialization(&index);
    testTickRetryWithoutInitialization(&index);
    testResumeWhileWindingDown(&index);
    testRestartAfterPausedStop(&index);
    testStoredMarginBelowMinimum(&index);
    return 0;
}

// NOLINTEND(cert-dcl03-c,misc-static-assert)
