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
#include <QIODevice>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QLatin1String>
#include <QList>
#include <QMap>
#include <QMetaType>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QThread>
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
#include <dsd-neo/runtime/signal_search_avoids.h>
#include <memory>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
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
#include "store_notices.h"
#include "teardown_trace_capture.h"

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

/* #782: the Android app's teardown times this destructor, which flushes the search store. */
void
testDestructorMarksItsTeardownStage(dsd_qt::KnownFrequencyIndex* index) {
    SessionHost android(true);
    TeardownCapture::lines().clear();
    { dsd_qt::SignalSearchModel search(&android, index); }
    assert(TeardownCapture::lines().isEmpty());
    {
        const TeardownCapture capture;
        { dsd_qt::SignalSearchModel search(&android, index); }
        assert(TeardownCapture::lines()
               == QStringList({QStringLiteral("teardown: ~SignalSearchModel started"),
                               QStringLiteral("teardown: ~SignalSearchModel took 7 ms")}));
    }
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

/* Issue #828: a ranges file that could not be read is kept at the first edit instead of being written over. */
void
testRangesKeptAside(dsd_qt::KnownFrequencyIndex* index) {
    const QString file = QStringLiteral("signal-search-ranges.json");
    const QString path = dsd_qt::json_store_path(file);
    const QJsonArray previous = dsd_qt::json_store_load_array(file);
    const QByteArray junk("[{\"name\":");
    {
        QFile broken(path);
        assert(broken.open(QIODevice::WriteOnly | QIODevice::Truncate) && broken.write(junk) == junk.size());
    }
    {
        dsd_qt::SignalSearchModel model(nullptr, index);
        assert(model.ranges().size() == 1); // the default, shown in place of the lost ones
        QVariantMap range = model.defaultRange();
        range[QStringLiteral("name")] = QStringLiteral("Edited");
        assert(model.saveRange(0, range).isEmpty());
        QFile kept(path + QStringLiteral(".unreadable-1"));
        assert(kept.open(QIODevice::ReadOnly) && kept.readAll() == junk);
    }
    assert(QFile::remove(path + QStringLiteral(".unreadable-1")));
    assert(dsd_qt::json_store_save_array(file, previous));
}

/* A range save that failed and then succeeded leaves no failure shown (issue #828). */
void
testRangeSaveFailureClears(dsd_qt::KnownFrequencyIndex* index) {
    const QString file = QStringLiteral("signal-search-ranges.json");
    const QString path = dsd_qt::json_store_path(file);
    const QJsonArray previous = dsd_qt::json_store_load_array(file);
    {
        dsd_qt::SignalSearchModel model(nullptr, index);
        const QVariantMap range = model.ranges().front().toMap();
        assert(QFile::remove(path) && QDir().mkpath(path));
        const QString failed = model.saveRange(0, range);
        assert(!failed.isEmpty() && model.error() == failed);
        assert(QDir().rmdir(path));
        assert(model.saveRange(0, range).isEmpty());
        assert(model.error().isEmpty());
    }
    assert(dsd_qt::json_store_save_array(file, previous));
}

void
writeBytes(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    assert(file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size());
}

QVariantMap
storeFile(const dsd_qt::SignalSearchModel& model, int which) {
    return model.storeFiles().value(which).toMap();
}

bool
noticeShown(const QString& file, const QString& state) {
    for (const QVariant& value : dsd_qt::StoreNotices::instance()->notices()) {
        const QVariantMap notice = value.toMap();
        if (notice.value(QStringLiteral("file")).toString() == file) {
            return notice.value(QStringLiteral("state")).toString() == state;
        }
    }
    return false;
}

/* Issue #828: the store's files as the Signal search screen words them and as Home's card takes them. What the store
   could not read or save is its own line, never the model's error as well. */
void
testStoreFilesWorded(dsd_qt::KnownFrequencyIndex* index) {
    dsd_app_signal_search_store_close();
    const QString v1 = dsd_qt::json_store_path(QStringLiteral("signal-search-history.v1"));
    const QString v2 = dsd_qt::json_store_path(QStringLiteral("signal-search-history.v2"));
    const QString avoids = v1 + QStringLiteral(".avoids.json");
    const QString keptAvoids = QStringLiteral("signal-search-history.v1.avoids.json.unreadable-1");
    assert(QDir().mkpath(QFileInfo(v1).absolutePath()));
    writeBytes(v2, "not a journal\n");
    writeBytes(avoids, "not a list");
    const QString historyLine = QStringLiteral("Signal search history couldn't be read completely, so some entries may "
                                               "be missing. The file won't be overwritten.");
    const QString avoidsUnsaved = QStringLiteral("Signal search permanent avoids couldn't be saved. Check available "
                                                 "storage. DSD-neo tries again with the next change.");
    {
        dsd_qt::SignalSearchModel model(nullptr, index);
        assert(model.storeFiles().size() == 3);
        assert(storeFile(model, 0).value(QStringLiteral("state")) == QStringLiteral("unreadable"));
        assert(storeFile(model, 1).value(QStringLiteral("state")) == QStringLiteral("unreadable"));
        assert(storeFile(model, 2).value(QStringLiteral("state")).toString().isEmpty());
        assert(model.storeMessage()
               == historyLine
                      + QStringLiteral(
                          "\nSignal search permanent avoids couldn't be read completely, so some entries may be "
                          "missing. The file won't be overwritten."));
        assert(model.error().isEmpty());
        assert(noticeShown(QStringLiteral("signal-search-history.v2"), QStringLiteral("unreadable")));
        assert(noticeShown(QStringLiteral("signal-search-history.v1.avoids.json"), QStringLiteral("unreadable")));

        // The first save keeps the file aside, and both surfaces name it.
        assert(dsd_app_signal_search_permanent_avoid(451000000) == DSD_APP_SEARCH_AVOID_ADDED);
        dsd_app_signal_search_store_flush();
        model.refresh();
        assert(storeFile(model, 1).value(QStringLiteral("kept")) == keptAvoids);
        assert(model.storeMessage()
               == historyLine
                      + QStringLiteral(
                          "\nSignal search permanent avoids couldn't be read completely, so some entries may be "
                          "missing. The file was kept as ")
                      + keptAvoids + QStringLiteral("."));
        assert(noticeShown(QStringLiteral("signal-search-history.v1.avoids.json"), QStringLiteral("kept")));

        // A folder at the path: the save is owed, said once, on the store's line only.
        assert(QFile::remove(avoids) && QDir().mkpath(avoids));
        assert(dsd_app_signal_search_avoid_remove(451000000) == 0);
        dsd_app_signal_search_store_flush();
        model.refresh();
        assert(storeFile(model, 1).value(QStringLiteral("writeFailed")).toBool());
        assert(model.storeMessage().endsWith(QStringLiteral("\n") + avoidsUnsaved));
        assert(model.storeMessage().count(avoidsUnsaved) == 1);
        assert(model.error().isEmpty());
        assert(QDir().rmdir(avoids));
        assert(dsd_app_signal_search_permanent_avoid(451000000) == DSD_APP_SEARCH_AVOID_ADDED);
        dsd_app_signal_search_store_flush();
        model.refresh();
        assert(!model.storeMessage().contains(avoidsUnsaved));
        assert(dsd_app_signal_search_avoid_remove(451000000) == 0);
        dsd_app_signal_search_store_flush();
    }
    // No version 2 journal and none kept: the version 1 journal is read, and one that can't be is left in place.
    dsd_app_signal_search_store_close();
    assert(QFile::remove(v2) && QFile::remove(dsd_qt::json_store_path(keptAvoids)));
    writeBytes(v1, "not a journal\n");
    {
        dsd_qt::SignalSearchModel model(nullptr, index);
        assert(storeFile(model, 2).value(QStringLiteral("state")) == QStringLiteral("leftInPlace"));
        assert(
            model.storeMessage()
            == QStringLiteral("Signal search history from an older version couldn't be read. It was left in place."));
        assert(noticeShown(QStringLiteral("signal-search-history.v1"), QStringLiteral("leftInPlace")));
    }
    dsd_app_signal_search_store_close();
    assert(QFile::remove(v1));
    QFile::remove(avoids);
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
        DSD_SNPRINTF(h.site, sizeof(h.site), "%s", "TIII Small \xC2\xB7 Net 12 \xC2\xB7 Site 3 \xC2\xB7 SYS 1183");
        h.slot = 2;
        DSD_SNPRINTF(h.crypt, sizeof(h.crypt), "%s", "ALG 21 \xC2\xB7 KID 0001");
        DSD_SNPRINTF(h.access, sizeof(h.access), "%s", "CC 1");
    } else {
        DSD_SNPRINTF(h.tone, sizeof(h.tone), "%s", "CTCSS 127.3 Hz");
    }
    dsd_app_signal_search_publish(&s, &h, nullptr);
}

static void
writeFile(const QString& path, const char* text) {
    FILE* f = dsd_fopen_private(path.toUtf8().constData(), "wb");
    assert(f);
    const size_t size = strlen(text);
    assert(fwrite(text, 1, size, f) == size);
    fclose(f);
}

/* The one match of @p matches whose name is @p name. */
static QVariantMap
matchNamed(const QVariantList& matches, const QString& name) {
    QVariantMap found;
    for (const QVariant& match : matches) {
        if (match.toMap().value(QStringLiteral("name")).toString() == name) {
            assert(found.isEmpty());
            found = match.toMap();
        }
    }
    assert(!found.isEmpty());
    return found;
}

static void
assertIdentity(const QVariantMap& match, const char* source, const QString& listUid, const QString& targetId, qint64 hz,
               bool selectable) {
    assert(match.value(QStringLiteral("source")).toString() == QLatin1String(source));
    assert(match.value(QStringLiteral("listUid")).toString() == listUid);
    assert(match.value(QStringLiteral("targetId")).toString() == targetId);
    assert(match.value(QStringLiteral("frequencyHz")).toLongLong() == hz);
    assert(match.value(QStringLiteral("selectable")).toBool() == selectable);
}

/* Issue #828: the overload warning reaches the screen as its own fields, never in the message: whether a span was
   skipped because it kept clipping, and the range named at the pass's first skip. A clean pass clears both. */
static void
testOverloadMapped() {
    dsd_qt::SignalSearchModel search(nullptr, nullptr);
    dsd_signal_search_status status{};
    status.session_id = 41;
    status.revision = 9300;
    status.phase = DSD_SEARCH_SEARCHING;
    status.owned = 1;
    status.overloaded = 1;
    DSD_SNPRINTF(status.overload_range, sizeof(status.overload_range), "%s", "Marine");
    DSD_SNPRINTF(status.range_name, sizeof(status.range_name), "%s", "Air band");
    dsd_app_signal_search_publish(&status, nullptr, nullptr);
    search.refresh();
    assert(search.status().value(QStringLiteral("overloaded")).toBool());
    assert(search.status().value(QStringLiteral("overloadRange")).toString() == QStringLiteral("Marine"));
    assert(search.status().value(QStringLiteral("rangeName")).toString() == QStringLiteral("Air band"));
    assert(search.status().value(QStringLiteral("message")).toString().isEmpty());
    status.revision = 9301;
    status.overloaded = 0;
    status.overload_range[0] = '\0';
    dsd_app_signal_search_publish(&status, nullptr, nullptr);
    search.refresh();
    assert(search.status().contains(QStringLiteral("overloaded")));
    assert(!search.status().value(QStringLiteral("overloaded")).toBool());
    assert(search.status().value(QStringLiteral("overloadRange")).toString().isEmpty());
}

/* Issue #809: a detection names its recordings by "<session>:<hit>", the key the recording journal stores, written
   from the integers: a session id above 2^53 (a JS number's exact range) keeps every digit, live and in history. A
   zero id, which the journal never names (the live hit with no detection current), gives no key. */
static void
testRecordingKey() {
    const struct {
        uint64_t session;
        uint64_t hit;
        const char* key;
    } cases[] = {
        {1730000000000000ULL, 7, "1730000000000000:7"},
        {9007199254740993ULL, 18446744073709551615ULL, "9007199254740993:18446744073709551615"},
        {1730000000000000ULL, 0, ""},
        {0, 7, ""},
    };

    dsd_qt::SignalSearchModel search(nullptr, nullptr);
    uint64_t revision = 9100;
    for (const auto& c : cases) {
        dsd_signal_search_status status{};
        status.session_id = c.session;
        status.revision = ++revision;
        status.phase = DSD_SEARCH_LISTENING;
        status.hit.session_id = c.session;
        status.hit.hit_id = c.hit;
        status.hit.frequency_hz = 463552100;
        status.hit.bandwidth_hz = 12500;
        dsd_app_signal_search_publish(&status, &status.hit, nullptr);
        search.refresh();
        const QString key = QString::fromLatin1(c.key);
        assert(search.status().value(QStringLiteral("hit")).toMap().value(QStringLiteral("recordingKey")).toString()
               == key);
        // History keeps only a real detection; a zero id is checked on the live hit alone.
        assert(key.isEmpty()
               || search.history().front().toMap().value(QStringLiteral("recordingKey")).toString() == key);
    }
    // A status with no detection current maps an all-zero hit.
    dsd_signal_search_status idle{};
    idle.session_id = 1730000000000000ULL;
    idle.revision = ++revision;
    idle.phase = DSD_SEARCH_LISTENING;
    dsd_app_signal_search_publish(&idle, nullptr, nullptr);
    search.refresh();
    assert(search.status()
               .value(QStringLiteral("hit"))
               .toMap()
               .value(QStringLiteral("recordingKey"))
               .toString()
               .isEmpty());
}

/* Issue #644: two lists under one name, each with an entry of one uid on one frequency, are two Listen choices, each
   starting its own list: the matches stay apart by list as well as by id. */
static void
testKnownFrequencyListsApart() {
    dsd_qt::SavedSystemsModel systems;
    dsd_qt::ScanListsModel lists;
    const QVariantMap desk{
        {QStringLiteral("uid"), QStringLiteral("entry")},        {QStringLiteral("kind"), QStringLiteral("freq")},
        {QStringLiteral("name"), QStringLiteral("Desk")},        {QStringLiteral("protocol"), QStringLiteral("dmr")},
        {QStringLiteral("freqMhz"), QStringLiteral("450.1250")}, {QStringLiteral("enabled"), true}};
    QStringList uids;
    for (int i = 0; i < 2; ++i) {
        assert(lists.add(
            {{QStringLiteral("name"), QStringLiteral("Gate")}, {QStringLiteral("entries"), QVariantList{desk}}}));
        uids << lists.get(lists.count() - 1).value(QStringLiteral("uid")).toString();
    }
    assert(uids[0] != uids[1]);
    dsd_qt::KnownFrequencyIndex index(&systems, &lists);
    const QVariantList matches = index.tunedMatches(450125000, 12500);
    assert(matches.size() == 2);
    QStringList listUids;
    for (const QVariant& match : matches) {
        assert(match.toMap().value(QStringLiteral("targetId")).toString() == QStringLiteral("entry"));
        assert(match.toMap().value(QStringLiteral("selectable")).toBool());
        listUids << match.toMap().value(QStringLiteral("listUid")).toString();
    }
    listUids.sort();
    uids.sort();
    assert(listUids == uids);
}

/* Issue #644: a target CSV row's id is what the engine loads, so a held start or an in-place go-to names a target the
   session has: the cell trimmed, then one pair of surrounding quotes taken off, which keeps the blanks inside them
   (" fire " is the id " fire "). The name shown is the trimmed text. The saved index and a session's own targets read
   it alike. An id a held start cannot name is not selectable. */
static void
testKnownFrequencyEngineIds(const QString& dir) {
    dsd_qt::SavedSystemsModel systems;
    dsd_qt::ScanListsModel lists;
    const QString targets = QDir(dir).filePath(QStringLiteral("engine-id-targets.csv"));
    writeFile(targets, "id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes,label\n"
                       "\" fire \",nfm-conventional,487000000,,,,,Fire\n"
                       "  \" pump\"  ,dmr-conventional,488000000,,,,,\n"
                       "ta\tb,dmr-conventional,489000000,,,,,Tab\n");
    assert(lists.add({{QStringLiteral("name"), QStringLiteral("Spaced")},
                      {QStringLiteral("targetSource"), QStringLiteral("csv")},
                      {QStringLiteral("targetsCsvPath"), targets}}));
    const QString spaced = lists.get(lists.count() - 1).value(QStringLiteral("uid")).toString();
    dsd_qt::KnownFrequencyIndex index(&systems, &lists);
    assertIdentity(matchNamed(index.tunedMatches(487000000, 12500), QStringLiteral("Spaced / Fire")), "scanTarget",
                   spaced, QStringLiteral(" fire "), 487000000, true);
    assertIdentity(matchNamed(index.tunedMatches(488000000, 12500), QStringLiteral("Spaced / pump")), "scanTarget",
                   spaced, QStringLiteral(" pump"), 488000000, true);
    // The engine loads an id with a tab in it, but a held start cannot name it (session_args_scan_hold_id_valid()):
    // the row names its channel and offers no start that would stop the session and then fail.
    assertIdentity(matchNamed(index.tunedMatches(489000000, 12500), QStringLiteral("Spaced / Tab")), "scanTarget",
                   spaced, QStringLiteral("ta\tb"), 489000000, false);
    const QVariantList session =
        index.sessionTargets({QVariantMap{{"uid", spaced}, {"name", QStringLiteral("Spaced")}, {"path", targets}}});
    assert(session.size() == 3);
    assert(session.at(0).toMap().value(QStringLiteral("targetId")).toString() == QStringLiteral(" fire "));
    assert(session.at(0).toMap().value(QStringLiteral("name")).toString() == QStringLiteral("Spaced / Fire"));
    assert(session.at(1).toMap().value(QStringLiteral("targetId")).toString() == QStringLiteral(" pump"));
    assert(session.at(1).toMap().value(QStringLiteral("name")).toString() == QStringLiteral("Spaced / pump"));
}

/* Issue #644: Qt reads a target CSV and a channel map as the engine's loaders do, so a session's own targets and the
   saved index hold what the session runs. A target CSV: its header is row 1, a blank row is skipped, and nothing is a
   comment, so a `#desk` row is a target (the in-place Listen reaches it); a header off row 1, or a row longer than the
   engine's 4095 characters, makes the engine refuse the file, so nothing of it is a target. A channel map: column 0 is
   the channel key (a row whose key the engine cannot read, `#7`, is no channel), column 1 the frequency, and only
   columns from the third take their names from the header. */
static void
testKnownFrequencyEngineGrammar(const QString& dir) {
    dsd_qt::SavedSystemsModel systems;
    dsd_qt::ScanListsModel lists;
    const QString targets = QDir(dir).filePath(QStringLiteral("grammar-targets.csv"));
    writeFile(targets, "id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes,label\n"
                       "#desk,dmr-conventional,452000000,,,,,Desk\n"
                       "\n"
                       "   \n"
                       "yard,dmr-conventional,452100000,,,,,Yard\n");
    assert(lists.add({{QStringLiteral("name"), QStringLiteral("Grammar")},
                      {QStringLiteral("targetSource"), QStringLiteral("csv")},
                      {QStringLiteral("targetsCsvPath"), targets}}));
    const QString grammar = lists.get(lists.count() - 1).value(QStringLiteral("uid")).toString();
    const QString late = QDir(dir).filePath(QStringLiteral("grammar-late-header.csv"));
    writeFile(late, "\nid,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes,label\n"
                    "late,dmr-conventional,453000000,,,,,Late\n");
    const QString wide = QDir(dir).filePath(QStringLiteral("grammar-long-row.csv"));
    {
        QByteArray text = "id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes\n"
                          "wide,dmr-conventional,453100000,,,,";
        text.append(QByteArray(5000, 'n'));
        text.append("\nnarrow,dmr-conventional,453200000,,,,\n");
        writeFile(wide, text.constData());
    }
    for (const QString& refused : {late, wide}) {
        assert(lists.add({{QStringLiteral("name"), QStringLiteral("Refused")},
                          {QStringLiteral("targetSource"), QStringLiteral("csv")},
                          {QStringLiteral("targetsCsvPath"), refused}}));
    }
    const QString channels = QDir(dir).filePath(QStringLiteral("grammar-channels.csv"));
    writeFile(channels, "chan,freq,name\n#7,454000000,Hash\n7,454100000,#Seven\n0x10,454200000,Hex\n");
    assert(systems.add({{QStringLiteral("name"), QStringLiteral("Metro")},
                        {QStringLiteral("freqMhz"), QStringLiteral("454.9000")},
                        {QStringLiteral("trunking"), true},
                        {QStringLiteral("chanCsvPath"), channels}}));
    dsd_qt::KnownFrequencyIndex index(&systems, &lists);
    assertIdentity(matchNamed(index.tunedMatches(452000000, 12500), QStringLiteral("Grammar / Desk")), "scanTarget",
                   grammar, QStringLiteral("#desk"), 452000000, true);
    assertIdentity(matchNamed(index.tunedMatches(452100000, 12500), QStringLiteral("Grammar / Yard")), "scanTarget",
                   grammar, QStringLiteral("yard"), 452100000, true);
    const QVariantList session =
        index.sessionTargets({QVariantMap{{"uid", grammar}, {"name", QStringLiteral("Grammar")}, {"path", targets}}});
    assert(session.size() == 2);
    assert(session.at(0).toMap().value(QStringLiteral("targetId")).toString() == QStringLiteral("#desk"));
    assert(session.at(1).toMap().value(QStringLiteral("targetId")).toString() == QStringLiteral("yard"));
    for (qint64 hz : {453000000LL, 453100000LL, 453200000LL}) {
        assert(index.tunedMatches(hz, 12500).isEmpty());
    }
    assert(
        index
            .sessionTargets({QVariantMap{{"uid", QStringLiteral("x")}, {"name", QStringLiteral("x")}, {"path", late}},
                             QVariantMap{{"uid", QStringLiteral("y")}, {"name", QStringLiteral("y")}, {"path", wide}}})
            .isEmpty());
    assert(index.tunedMatches(454000000, 12500).isEmpty());
    matchNamed(index.tunedMatches(454100000, 12500), QStringLiteral("Metro / #Seven"));
    matchNamed(index.tunedMatches(454200000, 12500), QStringLiteral("Metro / Hex"));
}

/* Issue #644: an options cell a spreadsheet wrapped in double quotes reads as its loader reads it
   (dsd_scan_options_parse_cell()): a channel map's `"--nfm-bandwidth-hz 8000"` and a target's both give an 8 kHz
   channel, so a signal 5 kHz away is not that channel; a target's quoted type and its spreadsheet-quoted label read as
   the target loader reads them. */
static void
testKnownFrequencyQuotedCells(const QString& dir) {
    dsd_qt::SavedSystemsModel systems;
    dsd_qt::ScanListsModel lists;
    const QString channels = QDir(dir).filePath(QStringLiteral("quoted-channels.csv"));
    writeFile(channels, "chan,freq,name,mode,options\n1,455000000,Quoted,nfm,\"--nfm-bandwidth-hz 8000\"\n");
    assert(systems.add({{QStringLiteral("name"), QStringLiteral("Quoted")},
                        {QStringLiteral("freqMhz"), QStringLiteral("455.9000")},
                        {QStringLiteral("trunking"), true},
                        {QStringLiteral("chanCsvPath"), channels}}));
    const QString targets = QDir(dir).filePath(QStringLiteral("quoted-targets.csv"));
    writeFile(targets, "id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes,options,label\n"
                       "fire,\"nfm-conventional\",456000000,,,,,\"--nfm-bandwidth-hz 8000\",\"Tower \"\"A\"\"\"\n");
    assert(lists.add({{QStringLiteral("name"), QStringLiteral("Quotes")},
                      {QStringLiteral("targetSource"), QStringLiteral("csv")},
                      {QStringLiteral("targetsCsvPath"), targets}}));
    dsd_qt::KnownFrequencyIndex index(&systems, &lists);
    matchNamed(index.tunedMatches(455000000, 12500), QStringLiteral("Quoted / Quoted"));
    assert(index.tunedMatches(455005000, 12500).isEmpty());
    matchNamed(index.tunedMatches(456000000, 12500), QStringLiteral("Quotes / Tower \"A\""));
    assert(index.tunedMatches(456005000, 12500).isEmpty());
}

/* Issue #644: a channel map far past the rows the index keeps from one file: the first 100000 rows are indexed and
   none after them, so a huge map neither grows the index nor the reader without bound. */
static void
testKnownFrequencyRowCap(const QString& dir) {
    dsd_qt::SavedSystemsModel systems;
    dsd_qt::ScanListsModel lists;
    const QString channels = QDir(dir).filePath(QStringLiteral("huge-channels.csv"));
    QByteArray text = "chan,freq,name\n";
    for (int i = 0; i < 100005; ++i) {
        text.append("1," + QByteArray::number(100000000LL + 12500LL * i) + ",C" + QByteArray::number(i) + '\n');
    }
    writeFile(channels, text.constData());
    assert(systems.add({{QStringLiteral("name"), QStringLiteral("Huge")},
                        {QStringLiteral("freqMhz"), QStringLiteral("99.0000")},
                        {QStringLiteral("trunking"), true},
                        {QStringLiteral("chanCsvPath"), channels}}));
    dsd_qt::KnownFrequencyIndex index(&systems, &lists);
    matchNamed(index.tunedMatches(100000000, 12500), QStringLiteral("Huge / C0"));
    matchNamed(index.tunedMatches(100000000 + 12500LL * 99999, 12500), QStringLiteral("Huge / C99999"));
    assert(index.tunedMatches(100000000 + 12500LL * 100000, 12500).isEmpty());
    assert(index.tunedMatches(100000000 + 12500LL * 100004, 12500).isEmpty());
}

/* A scan list over the target CSV @p path, named @p name. */
static void
addTargetList(dsd_qt::ScanListsModel* lists, const QString& name, const QString& path) {
    assert(lists->add({{QStringLiteral("name"), name},
                       {QStringLiteral("targetSource"), QStringLiteral("csv")},
                       {QStringLiteral("targetsCsvPath"), path}}));
}

/* A trunked saved system named @p name whose channel map is @p path. */
static void
addMapSystem(dsd_qt::SavedSystemsModel* systems, const QString& name, const QString& path) {
    assert(systems->add({{QStringLiteral("name"), name},
                         {QStringLiteral("freqMhz"), QStringLiteral("99.5000")},
                         {QStringLiteral("trunking"), true},
                         {QStringLiteral("chanCsvPath"), path}}));
}

/* Spin the event loop until @p index rebuilds past @p revision, at most five seconds; whether it did. */
static bool
waitForRebuild(const dsd_qt::KnownFrequencyIndex& index, qulonglong revision) {
    for (int i = 0; i < 500 && index.revision() == revision; ++i) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(10);
    }
    return index.revision() != revision;
}

static const char kTargetHeader[] = "id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes";

/* Issue #644: a target CSV and a channel map that do not parse when the index is built (an empty file, a bad header)
   are still watched, with their directory, so fixing them rebuilds the index and their rows appear. */
static void
testKnownFrequencyWatchesUnparsedFiles(const QString& dir) {
    dsd_qt::SavedSystemsModel systems;
    dsd_qt::ScanListsModel lists;
    const QString targets = QDir(dir).filePath(QStringLiteral("watch-targets.csv"));
    const QString channels = QDir(dir).filePath(QStringLiteral("watch-channels.csv"));
    writeFile(targets, "not,a,target,header\n");
    writeFile(channels, "");
    addTargetList(&lists, QStringLiteral("Watched"), targets);
    addMapSystem(&systems, QStringLiteral("WatchedMap"), channels);
    dsd_qt::KnownFrequencyIndex index(&systems, &lists);
    assert(index.tunedMatches(1457000000, 12500).isEmpty() && index.tunedMatches(1457100000, 12500).isEmpty());
    qulonglong revision = index.revision();
    writeFile(targets, (QByteArray(kTargetHeader) + "\nfixed,dmr-conventional,1457000000,,,,\n").constData());
    assert(waitForRebuild(index, revision));
    matchNamed(index.tunedMatches(1457000000, 12500), QStringLiteral("Watched / fixed"));
    revision = index.revision();
    writeFile(channels, "chan,freq,name\n1,1457100000,Fixed\n");
    assert(waitForRebuild(index, revision));
    matchNamed(index.tunedMatches(1457100000, 12500), QStringLiteral("WatchedMap / Fixed"));
}

/* Issue #644: a target CSV read exactly as the engine's target loader reads it.
   - The optional columns by any spelling it takes: `relevant_CLI_switches` is the options column, any case, so an NFM
     row's 25 kHz width counts.
   - A file it refuses gives no rows: a duplicated id, a duplicated type and frequency, a row under seven fields, an
     unknown type, an options cell it cannot parse, a header that names an optional column twice.
   - A UTF-8 byte order mark before the header is skipped; a carriage return ends the line where it stands.
   - The label is shown as the loader keeps it, a control character as a space.
   - A row's width is the one its options set as the engine's options parser reads them. */
static void
testKnownFrequencyTargetParity(const QString& dir) {
    dsd_qt::SavedSystemsModel systems;
    dsd_qt::ScanListsModel lists;
    const QString alias = QDir(dir).filePath(QStringLiteral("parity-alias.csv"));
    writeFile(alias, (QByteArray(kTargetHeader)
                      + ",Relevant_CLI_Switches\n"
                        "wide,nfm-conventional,1455000000,,,,,--nfm-bandwidth-hz 25000\n")
                         .constData());
    addTargetList(&lists, QStringLiteral("Alias"), alias);
    const char* const refused[] = {
        "a,dmr-conventional,1458000000,,,,\na,dmr-conventional,1458012500,,,,\n",
        "a,dmr-conventional,1458025000,,,,\nb,dmr-conventional,1458025000,,,,\n",
        "a,dmr-conventional,1458037500,,,\n",
        "a,fm-conventional,1458050000,,,,\n",
        "a,nfm-conventional,1458062500,,,,,--nfm-bandwidth-hz 6000\n",
    };
    for (size_t i = 0; i < sizeof refused / sizeof refused[0]; ++i) {
        const QString path = QDir(dir).filePath(QStringLiteral("parity-refused-%1.csv").arg(i));
        const QByteArray header = QByteArray(kTargetHeader) + (i == 4 ? ",options\n" : "\n");
        writeFile(path, (header + refused[i]).constData());
        addTargetList(&lists, QStringLiteral("Refused"), path);
    }
    const QString twice = QDir(dir).filePath(QStringLiteral("parity-twice.csv"));
    writeFile(twice, (QByteArray(kTargetHeader) + ",label,LABEL\na,dmr-conventional,1458075000,,,,,A,B\n").constData());
    addTargetList(&lists, QStringLiteral("Refused"), twice);
    const QString bom = QDir(dir).filePath(QStringLiteral("parity-bom.csv"));
    writeFile(
        bom,
        (QByteArray("\xEF\xBB\xBF") + kTargetHeader + ",label\nbom,dmr-conventional,1458100000,,,,,Bom\n").constData());
    addTargetList(&lists, QStringLiteral("Bom"), bom);
    const QString cr = QDir(dir).filePath(QStringLiteral("parity-cr.csv"));
    writeFile(cr, (QByteArray(kTargetHeader) + ",label\ncr,dmr-conventional,1458112500,,,,\r,Cut\n").constData());
    addTargetList(&lists, QStringLiteral("Cr"), cr);
    const QString control = QDir(dir).filePath(QStringLiteral("parity-control.csv"));
    writeFile(control, (QByteArray(kTargetHeader)
                        + ",label\nctl,dmr-conventional,1458125000,,,,,Tow\x01"
                          "er\n")
                           .constData());
    addTargetList(&lists, QStringLiteral("Control"), control);
    const QString quoted = QDir(dir).filePath(QStringLiteral("parity-width.csv"));
    writeFile(quoted,
              (QByteArray(kTargetHeader) + ",options\nsq,nfm-conventional,1458200000,,,,,--nfm-bandwidth-hz '20000'\n")
                  .constData());
    addTargetList(&lists, QStringLiteral("Width"), quoted);
    dsd_qt::KnownFrequencyIndex index(&systems, &lists);
    matchNamed(index.tunedMatches(1455010000, 25000), QStringLiteral("Alias / wide"));
    for (qint64 hz :
         {1458000000LL, 1458012500LL, 1458025000LL, 1458037500LL, 1458050000LL, 1458062500LL, 1458075000LL}) {
        assert(index.tunedMatches(hz, 12500).isEmpty());
    }
    matchNamed(index.tunedMatches(1458100000, 12500), QStringLiteral("Bom / Bom"));
    matchNamed(index.tunedMatches(1458112500, 12500), QStringLiteral("Cr / cr"));
    matchNamed(index.tunedMatches(1458125000, 12500), QStringLiteral("Control / Tow er"));
    // The options parser groups a single-quoted argument: a 20 kHz channel, so a signal 9 kHz off is still on it.
    matchNamed(index.tunedMatches(1458209000, 20000), QStringLiteral("Width / sq"));
}

/* Issue #644: a channel map read exactly as the -Y map loader reads it. Its frequency is a whole number of Hz, a sign
   allowed as strtol() takes it (blanks before the sign, none after it), and no MHz or decimal form; a file it refuses gives no rows (a header naming `mode`
   twice, an unknown mode, an options cell it cannot parse); a name is shown as the loader keeps it. */
static void
testKnownFrequencyChannelParity(const QString& dir) {
    dsd_qt::SavedSystemsModel systems;
    dsd_qt::ScanListsModel lists;
    const QString forms = QDir(dir).filePath(QStringLiteral("parity-map-forms.csv"));
    writeFile(
        forms,
        "chan,freq,name\n1,+1459000000,Signed\n2,1459012500.5,Decimal\n3,1459.025M,Mhz\n4,1459037500,Tab\x01Name\n"
        "5,+ 1459050000,SpacedSign\n6, +1459062500,BlankFirst\n");
    addMapSystem(&systems, QStringLiteral("Forms"), forms);
    const char* const refused[] = {
        "chan,freq,mode,mode\n1,1459100000,dmr,dmr\n",
        "chan,freq,mode\n1,1459112500,fm\n",
        "chan,freq,mode,options\n1,1459125000,nfm,--nfm-bandwidth-hz 6000\n",
    };
    for (size_t i = 0; i < sizeof refused / sizeof refused[0]; ++i) {
        const QString path = QDir(dir).filePath(QStringLiteral("parity-map-refused-%1.csv").arg(i));
        writeFile(path, refused[i]);
        addMapSystem(&systems, QStringLiteral("RefusedMap"), path);
    }
    dsd_qt::KnownFrequencyIndex index(&systems, &lists);
    matchNamed(index.tunedMatches(1459000000, 12500), QStringLiteral("Forms / Signed"));
    assert(index.tunedMatches(1459012500, 12500).isEmpty());
    assert(index.tunedMatches(1459025000, 12500).isEmpty());
    matchNamed(index.tunedMatches(1459037500, 12500), QStringLiteral("Forms / Tab Name"));
    // strtol() takes blanks before the sign, never between the sign and its digits.
    assert(index.tunedMatches(1459050000, 12500).isEmpty());
    matchNamed(index.tunedMatches(1459062500, 12500), QStringLiteral("Forms / BlankFirst"));
    for (qint64 hz : {1459100000LL, 1459112500LL, 1459125000LL}) {
        assert(index.tunedMatches(hz, 12500).isEmpty());
    }
}

/* Issue #644: a target list and a channel map whose file names hold an emoji and a CJK character are indexed: the
   engine's validators get the path as the runtime's file opener reads it, UTF-8 (dsd_fopen()), not the local 8-bit
   code page, which on Windows turns what it cannot spell into '?'. */
static void
testKnownFrequencyNonAsciiPaths(const QString& dir) {
    const QString suffix = QString::fromUtf8("-\xF0\x9F\x93\xBB\xE4\xB8\xAD.csv");
    const QString targets = QDir(dir).filePath(QStringLiteral("targets") + suffix);
    const QString channels = QDir(dir).filePath(QStringLiteral("channels") + suffix);
    const QByteArray want =
        QDir(dir).filePath(QStringLiteral("targets")).toUtf8() + "-\xF0\x9F\x93\xBB\xE4\xB8\xAD.csv";
    assert(dsd_qt::runtimeFilePath(targets) == want);
    writeFile(targets, (QByteArray(kTargetHeader) + "\nwide,dmr-conventional,1461000000,,,,\n").constData());
    writeFile(channels, "chan,freq,name\n1,1461100000,Narrow\n");
    dsd_qt::SavedSystemsModel systems;
    dsd_qt::ScanListsModel lists;
    addTargetList(&lists, QStringLiteral("Glyphs"), targets);
    addMapSystem(&systems, QStringLiteral("GlyphMap"), channels);
    dsd_qt::KnownFrequencyIndex index(&systems, &lists);
    matchNamed(index.tunedMatches(1461000000, 12500), QStringLiteral("Glyphs / wide"));
    matchNamed(index.tunedMatches(1461100000, 12500), QStringLiteral("GlyphMap / Narrow"));
}

/* Issue #644: every match says what it is, so a frontend can start it with its own settings. A saved system's own
   frequency and a scan list's row (an entry by its uid, a target CSV row by its id) can be started; a channel map's row
   (a saved system's, or a trunked target's), a disabled entry and a draft list's entry only name the channel. */
static void
testKnownFrequencyIdentity(const QString& dir) {
    dsd_qt::SavedSystemsModel systems;
    dsd_qt::ScanListsModel lists;
    const QString channels = QDir(dir).filePath(QStringLiteral("identity-channels.csv"));
    writeFile(channels, "name,frequency\n1,481000000\n");
    assert(systems.add({{QStringLiteral("uid"), QStringLiteral("county")},
                        {QStringLiteral("name"), QStringLiteral("County")},
                        {QStringLiteral("freqMhz"), QStringLiteral("480.1000")},
                        {QStringLiteral("trunking"), true},
                        {QStringLiteral("chanCsvPath"), channels}}));
    const QString county = systems.get(systems.count() - 1).value(QStringLiteral("uid")).toString();
    const auto entry = [](const char* uid, const char* name, const char* freq, bool enabled) {
        return QVariantMap{
            {QStringLiteral("uid"), QLatin1String(uid)},      {QStringLiteral("kind"), QStringLiteral("freq")},
            {QStringLiteral("name"), QLatin1String(name)},    {QStringLiteral("protocol"), QStringLiteral("dmr")},
            {QStringLiteral("freqMhz"), QLatin1String(freq)}, {QStringLiteral("enabled"), enabled}};
    };
    assert(lists.add({{QStringLiteral("name"), QStringLiteral("Hospital")},
                      {QStringLiteral("entries"), QVariantList{entry("er-desk", "ER desk", "482.0000", true),
                                                               entry("helipad", "Helipad", "482.1000", false)}}}));
    const QString hospital = lists.get(lists.count() - 1).value(QStringLiteral("uid")).toString();
    assert(lists.add({{QStringLiteral("name"), QStringLiteral("Draft")},
                      {QStringLiteral("isDraft"), true},
                      {QStringLiteral("entries"), QVariantList{entry("lobby", "Lobby", "482.2000", true)}}}));
    const QString draft = lists.get(lists.count() - 1).value(QStringLiteral("uid")).toString();
    const QString targets = QDir(dir).filePath(QStringLiteral("identity-targets.csv"));
    writeFile(targets, "id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes,label\n"
                       "tower,am-conventional,483300000,,,,,Tower\n"
                       "trunk,dmr-trunk,484100000,identity-channels.csv,,,,\n"
                       "nxdn-a,nxdn-conventional,485000000,,,,,Gate\n"
                       "nxdn-b,nxdn48-conventional,485000000,,,,,Gate\n");
    /* A row with no id makes the target loader refuse its whole file, so no row of it is a match at all. */
    const QString unnamed = QDir(dir).filePath(QStringLiteral("identity-unnamed.csv"));
    writeFile(unnamed, "id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes,label\n"
                       "named,dmr-conventional,486100000,,,,,Named\n"
                       ",dmr-conventional,486000000,,,,,Unnamed\n");
    assert(lists.add({{QStringLiteral("name"), QStringLiteral("Unnamed")},
                      {QStringLiteral("targetSource"), QStringLiteral("csv")},
                      {QStringLiteral("targetsCsvPath"), unnamed}}));
    assert(lists.add({{QStringLiteral("name"), QStringLiteral("Imported")},
                      {QStringLiteral("targetSource"), QStringLiteral("csv")},
                      {QStringLiteral("targetsCsvPath"), targets}}));
    const QString imported = lists.get(lists.count() - 1).value(QStringLiteral("uid")).toString();
    dsd_qt::KnownFrequencyIndex index(&systems, &lists);

    assertIdentity(matchNamed(index.tunedMatches(480100000, 12500), QStringLiteral("County")), "system", QString(),
                   QString(), 480100000, true);
    assert(matchNamed(index.tunedMatches(480100000, 12500), QStringLiteral("County"))
               .value(QStringLiteral("systemUid"))
               .toString()
           == county);
    /* The map's header names no name column from its third on (column 0 is the channel key, as the engine reads it),
       so its row is named after the system alone. */
    assertIdentity(matchNamed(index.tunedMatches(481000000, 12500), QStringLiteral("County")), "channelMap", QString(),
                   QString(), 481000000, false);
    // A row of the channel: the entry's own frequency is reported, not the one tuned.
    assertIdentity(matchNamed(index.tunedMatches(482002000, 12500), QStringLiteral("Hospital / ER desk")), "scanEntry",
                   hospital, QStringLiteral("er-desk"), 482000000, true);
    assertIdentity(matchNamed(index.tunedMatches(482100000, 12500), QStringLiteral("Hospital / Helipad")), "scanEntry",
                   hospital, QStringLiteral("helipad"), 482100000, false);
    assertIdentity(matchNamed(index.tunedMatches(482200000, 12500), QStringLiteral("Draft / Lobby")), "scanEntry",
                   draft, QStringLiteral("lobby"), 482200000, false);
    assertIdentity(matchNamed(index.tunedMatches(483300000, 0), QStringLiteral("Imported / Tower")), "scanTarget",
                   imported, QStringLiteral("tower"), 483300000, true);
    assertIdentity(matchNamed(index.tunedMatches(484100000, 12500), QStringLiteral("Imported / trunk")), "scanTarget",
                   imported, QStringLiteral("trunk"), 484100000, true);
    // The trunked target's channel map, and the saved system's, both name 481 MHz; neither is a row of its own.
    const auto mapped = index.tunedMatches(481000000, 12500);
    assert(mapped.size() == 2);
    assertIdentity(matchNamed(mapped, QStringLiteral("Imported / trunk")), "channelMap", imported, QString(), 481000000,
                   false);
    assert(index.tunedMatches(486000000, 12500).isEmpty() && index.tunedMatches(486100000, 12500).isEmpty());
    // Two targets on one frequency under one label are two rows, each its own id; the name line says the name once.
    const auto gate = index.tunedMatches(485000000, 12500);
    assert(gate.size() == 2);
    assert(gate[0].toMap().value(QStringLiteral("targetId")).toString() == QStringLiteral("nxdn-a"));
    assert(gate[1].toMap().value(QStringLiteral("targetId")).toString() == QStringLiteral("nxdn-b"));
    assert(index.tunedNames(485000000, 12500) == QStringList{QStringLiteral("Imported / Gate")});
    // Each says its protocol, which tells the two apart: an entry's protocol, a saved system's decode flag's.
    assert(gate[0].toMap().value(QStringLiteral("protocolLabel")).toString() == QStringLiteral("NXDN96"));
    assert(gate[1].toMap().value(QStringLiteral("protocolLabel")).toString() == QStringLiteral("NXDN48"));
    assert(matchNamed(index.tunedMatches(482000000, 12500), QStringLiteral("Hospital / ER desk"))
               .value(QStringLiteral("protocolLabel"))
               .toString()
           == QStringLiteral("DMR"));
    assert(matchNamed(index.tunedMatches(480100000, 12500), QStringLiteral("County"))
               .value(QStringLiteral("protocolLabel"))
               .toString()
               .isEmpty());
    // Signal search names the channel's matches once each, as the tuned-name line does.
    dsd_signal_search_status status{};
    status.session_id = 19;
    status.revision = 9001;
    status.phase = DSD_SEARCH_LISTENING;
    status.hit.session_id = 19;
    status.hit.hit_id = 9001;
    status.hit.frequency_hz = 485000000;
    status.hit.bandwidth_hz = 12500;
    dsd_app_signal_search_publish(&status, &status.hit, nullptr);
    dsd_qt::SignalSearchModel search(nullptr, &index);
    search.refresh();
    const QVariantMap hit = search.status().value(QStringLiteral("hit")).toMap();
    assert(hit.value(QStringLiteral("knownNames")).toStringList() == QStringList{QStringLiteral("Imported / Gate")});
    assert(hit.value(QStringLiteral("knownLabel")).toString() == QStringLiteral("Imported / Gate"));
    // The live views' width rule: 0 takes a 12.5 kHz channel, as tunedNames() does.
    assert(index.tunedMatches(482005000, 0).size() == 1);
    assert(index.tunedMatches(482005000, 6000).isEmpty());
}

int
main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("dsd-search-tests"));
    dsd_test_qt_isolate_paths();
    QTemporaryDir temp;
    assert(temp.isValid());
    const QString journal = temp.filePath(QStringLiteral("history"));
    // The store writes version 2 records beside the version 1 journal it is opened with.
    const QString written = journal + QStringLiteral(".v2");
    assert(dsd_app_signal_search_store_open(journal.toUtf8().constData()) == 0);
    publish(1);
    dsd_app_signal_search_store_flush();
    const auto before = QFileInfo(written).size();
    assert(before > 0);
    for (int i = 0; i < 50; ++i) {
        publish(1);
    }
    dsd_app_signal_search_store_flush();
    assert(QFileInfo(written).size() == before);
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
    assert(dsd_app_signal_search_permanent_avoid(463550000) == DSD_APP_SEARCH_AVOID_ADDED);
    assert(dsd_app_signal_search_permanent_avoid(463550000) == DSD_APP_SEARCH_AVOID_DUPLICATE);
    dsd_app_signal_search_store_flush();
    dsd_app_signal_search_store_close();
    assert(dsd_app_signal_search_store_open(journal.toUtf8().constData()) == 0);
    assert(dsd_app_signal_search_history_get(hits.data(), hits.size()) == 1000);
    assert(hits.front().hit_id == 1005 && hits.back().hit_id == 6 && hits.front().ended);
    uint32_t avoids[4];
    assert(dsd_app_signal_search_avoids_get(avoids, 4) == 1 && avoids[0] == 463550000);
    assert(dsd_app_signal_search_avoid_remove(463550000) == 0);
    dsd_app_signal_search_store_close();
    FILE* f = dsd_fopen_private(written.toUtf8().constData(), "ab");
    assert(f);
    assert(fwrite("DSS", 1, 3, f) == 3);
    fclose(f);
    assert(dsd_app_signal_search_store_open(journal.toUtf8().constData()) == 0);
    // A torn tail is the journal's own load state (issue #828), for the frontend to word; the store's message is only
    // ever a write failure.
    dsd_app_search_file_load load = DSD_APP_SEARCH_FILE_OK;
    assert(dsd_app_signal_search_store_file_state(DSD_APP_SEARCH_STORE_HISTORY, &load, nullptr, nullptr, 0) == 0);
    assert(load == DSD_APP_SEARCH_FILE_UNREADABLE);
    char error[256];
    dsd_app_signal_search_status_get(nullptr, error, sizeof(error));
    assert(!error[0]);
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
    assert(QFileInfo(missing + QStringLiteral(".v2")).size() > 0);
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
    assert(index.tunedNames(118300000, 12500) == QStringList{QStringLiteral("Imported list / Tower")});
    // Issue #782: the tuned-frequency line names an entry as the scan header and the target's calls do: a target
    // CSV's label, an unnamed scan-list entry by its protocol and frequency, an unnamed saved system likewise.
    const QString labelled = temp.filePath(QStringLiteral("labelled.csv"));
    f = dsd_fopen_private(labelled.toUtf8().constData(), "wb");
    assert(f);
    const char labelledCsv[] = "id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes,label\n"
                               "f4c3562f,dmr-conventional,455500000,,,,,\"County, DMR\"\n";
    assert(fwrite(labelledCsv, 1, sizeof(labelledCsv) - 1, f) == sizeof(labelledCsv) - 1);
    fclose(f);
    assert(lists.add({{QStringLiteral("name"), QStringLiteral("Labelled")},
                      {QStringLiteral("targetSource"), QStringLiteral("csv")},
                      {QStringLiteral("targetsCsvPath"), labelled}}));
    assert(index.tunedNames(455500000, 12500) == QStringList{QStringLiteral("Labelled / County, DMR")});
    assert(lists.add({{QStringLiteral("name"), QStringLiteral("Mix6")},
                      {QStringLiteral("entries"),
                       QVariantList{QVariantMap{{QStringLiteral("uid"), QStringLiteral("e1")},
                                                {QStringLiteral("kind"), QStringLiteral("freq")},
                                                {QStringLiteral("protocol"), QStringLiteral("dmr")},
                                                {QStringLiteral("freqMhz"), QStringLiteral("456.5000")}}}}}));
    assert(index.tunedNames(456500000, 12500) == QStringList{QString::fromUtf8("Mix6 / DMR \u00b7 456.5000 MHz")});
    assert(systems.add({{QStringLiteral("decodeFlag"), QStringLiteral("-ft")},
                        {QStringLiteral("freqMhz"), QStringLiteral("457.5000")}}));
    assert(index.tunedNames(457500000, 12500) == QStringList{QString::fromUtf8("P25 \u00b7 457.5000 MHz")});
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
    const auto liveRf = model.status().value(QStringLiteral("hit")).toMap();
    assert(liveRf.value(QStringLiteral("id")) == QStringLiteral("77"));
    assert(liveRf.value(QStringLiteral("tone")) == QStringLiteral("CTCSS 127.3 Hz"));
    assert(liveRf.value(QStringLiteral("access")).toString().isEmpty());
    publish(78, true);
    model.refresh();
    const auto saved = model.systemForHit(frozen, {{QStringLiteral("sourceType"), QStringLiteral("usb")}});
    assert(saved.value(QStringLiteral("freqMhz")) == QStringLiteral("463.552100"));
    assert(saved.value(QStringLiteral("sourceType")) == QStringLiteral("rtltcp"));
    assert(saved.value(QStringLiteral("host")) == QStringLiteral("127.0.0.2"));
    assert(saved.value(QStringLiteral("port")).toInt() == 1235);
    assert(saved.value(QStringLiteral("decodeFlag")) == QStringLiteral("-fA"));
    assert(saved.value(QStringLiteral("searchUnresolved")).toBool());
    // The checked site, slot and encryption, the access code and the received tone reach the screens as they were
    // published, live and from history too (issue #802: a Recent detections row shows the access code and tone).
    const auto shown = model.history().front().toMap();
    assert(shown.value(QStringLiteral("site")) == QStringLiteral("TIII Small · Net 12 · Site 3 · SYS 1183"));
    assert(shown.value(QStringLiteral("slot")).toInt() == 2);
    assert(shown.value(QStringLiteral("crypt")) == QStringLiteral("ALG 21 · KID 0001"));
    assert(shown.value(QStringLiteral("access")) == QStringLiteral("CC 1"));
    assert(shown.value(QStringLiteral("tone")).toString().isEmpty());
    const auto live = model.status().value(QStringLiteral("hit")).toMap();
    assert(live.value(QStringLiteral("id")) == QStringLiteral("78"));
    assert(live.value(QStringLiteral("access")) == QStringLiteral("CC 1"));
    assert(live.value(QStringLiteral("tone")).toString().isEmpty());
    assert(frozen.value(QStringLiteral("site")).toString().isEmpty()
           && frozen.value(QStringLiteral("slot")).toInt() == 0);
    assert(frozen.value(QStringLiteral("crypt")).toString().isEmpty());
    assert(frozen.value(QStringLiteral("access")).toString().isEmpty());
    assert(frozen.value(QStringLiteral("tone")) == QStringLiteral("CTCSS 127.3 Hz"));
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
    // Issue #806: the found system's 48 kHz runs on an Airspy as on an RTL (its capture rate stepped to it), and the
    // width check the session holds a radio to covers the Airspy too: an AM detection at the 6 kHz default starts there
    // even under an app default its channel would not fit.
    auto airspyAm = airspy;
    airspyAm[QStringLiteral("confirmed")] = false;
    airspyAm[QStringLiteral("am")] = true;
    airspyAm[QStringLiteral("width")] = 6000;
    dsd_qt::SessionArgPrefs narrowDefault;
    narrowDefault.bandwidthKhz = 6;
    for (const auto& found : {airspySystem, model.systemForHit(airspyAm, {})}) {
        assert(found.value(QStringLiteral("bandwidthKhz")).toInt() == 48);
        startError = dsd_qt::SessionArgsError::None;
        const QStringList airspyArgs = dsd_qt::session_args_build(found, narrowDefault, &startError);
        assert(startError == dsd_qt::SessionArgsError::None);
        assert(airspyArgs.value(airspyArgs.indexOf(QStringLiteral("-i")) + 1).startsWith(QStringLiteral("airspy:")));
        assert(airspyArgs.value(airspyArgs.indexOf(QStringLiteral("-i")) + 1).split(QLatin1Char(':')).value(2)
               == QStringLiteral("48"));
    }
    assert(model.systemForHit(airspyAm, {}).value(QStringLiteral("decodeFlag")) == QStringLiteral("-fM"));

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
        searching.live_seq = 10 + (uint64_t)i;
        DSD_SNPRINTF(searching.range_name, sizeof(searching.range_name), "%s", "Air band");
        searching.range_start_hz = 118000000;
        searching.range_end_hz = 137012500;
        dsd_app_signal_search_publish(&searching, nullptr, nullptr);
        model.refresh();
    }
    assert(statusSignals == 5 && historySignals == 0);
    assert(dsd_app_signal_search_history_revision() == historyRevision);
    assert(model.status().value(QStringLiteral("owned")).toBool());
    assert(model.status().value(QStringLiteral("livePowerValid")).toBool());
    assert(std::abs(model.status().value(QStringLiteral("livePower")).toDouble() + 46.5) < 1e-3);
    // The running range as the engine names it, bounds at kHz precision or finer; and the live measurement count.
    assert(model.status().value(QStringLiteral("liveSeq")).toULongLong() == 14);
    assert(model.status().value(QStringLiteral("rangeName")).toString() == QStringLiteral("Air band"));
    assert(model.status().value(QStringLiteral("rangeStartMHz")).toString() == QStringLiteral("118.000"));
    assert(model.status().value(QStringLiteral("rangeEndMHz")).toString() == QStringLiteral("137.0125"));
    // An edit to the stored ranges is not the running range: only the engine's next publication changes it.
    assert(!model.ranges().isEmpty());
    QVariantMap renamed = model.ranges().front().toMap();
    renamed[QStringLiteral("name")] = QStringLiteral("Renamed");
    assert(model.saveRange(0, renamed).isEmpty());
    model.refresh();
    assert(model.ranges().front().toMap().value(QStringLiteral("name")) == QStringLiteral("Renamed"));
    assert(model.status().value(QStringLiteral("rangeName")).toString() == QStringLiteral("Air band"));
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

    // Issue #828: a session's own avoids are the engine's runtime list, shown through the store only for the store's
    // running session and apart from the permanent ones. The status published above is session 17's, listening.
    const uint32_t sessionList[] = {463550000, 463575000};
    dsd_signal_search_avoids_session_set(17, sessionList, 2);
    const int avoidSignalsBefore = avoidSignals;
    model.refresh();
    assert(avoidSignals == avoidSignalsBefore + 1 && model.sessionAvoids().size() == 2 && model.avoids().isEmpty());
    model.refresh();
    assert(avoidSignals == avoidSignalsBefore + 1); // nothing changed, nothing copied
    dsd_signal_search_avoids_session_set(17, sessionList + 1, 1);
    model.refresh();
    assert(model.sessionAvoids().size() == 1 && model.sessionAvoids().front().toUInt() == 463575000);
    const uint32_t nextList[] = {463600000};
    dsd_signal_search_avoids_session_set(18, nextList, 1); // another session's list is not this search's
    model.refresh();
    assert(model.sessionAvoids().isEmpty());
    dsd_signal_search_status running{};
    running.session_id = 18;
    running.revision = 5990;
    running.phase = DSD_SEARCH_LISTENING;
    running.owned = 1;
    dsd_app_signal_search_publish(&running, nullptr, nullptr);
    model.refresh();
    assert(model.sessionAvoids().size() == 1 && model.sessionAvoids().front().toUInt() == 463600000);
    // A failed or stopped search shows no session list, even before the engine's clearing reaches the runtime.
    dsd_signal_search_status failed = running;
    failed.revision = 5991;
    failed.phase = DSD_SEARCH_ERROR;
    dsd_app_signal_search_publish(&failed, nullptr, nullptr);
    model.refresh();
    assert(model.sessionAvoids().isEmpty());
    dsd_app_signal_search_publish(&running, nullptr, nullptr);
    model.refresh();
    assert(model.sessionAvoids().size() == 1);
    dsd_signal_search_status stopped = running;
    stopped.revision = 6000;
    stopped.phase = DSD_SEARCH_STOPPED;
    stopped.owned = 0;
    dsd_app_signal_search_publish(&stopped, nullptr, nullptr);
    model.refresh();
    assert(model.sessionAvoids().isEmpty() && !model.status().value(QStringLiteral("owned")).toBool());
    dsd_signal_search_avoids_session_set(0, nullptr, 0);

    // A history row's permanent avoid and its removal change the store alone, here, whether or not a search runs: the
    // store sets the runtime's permanent list, which a running search takes at its next tick. Nothing is queued for
    // the decoder, so a model a tick behind the decoder at the Stop edge (command admission already closed) still
    // saves the avoid and shows no error (issue #828). An avoid for this session only still goes to the decoder thread.
    uint32_t permanent[4];
    uint64_t permanentRevision = 0;
    running.revision = 6001;
    dsd_app_signal_search_publish(&running, nullptr, nullptr);
    model.refresh();
    dsd_app_frontend_runtime_start(nullptr, nullptr);
    assert(model.avoidPermanently(451000000) && !startQueued() && model.error().isEmpty());
    assert(model.avoids().size() == 1 && model.avoids().front().toUInt() == 451000000);
    assert(dsd_signal_search_avoids_permanent_snapshot(permanent, 4, &permanentRevision) == 1
           && permanent[0] == 451000000);
    model.removeAvoid(451000000);
    assert(!startQueued() && model.avoids().isEmpty() && model.error().isEmpty());
    assert(dsd_signal_search_avoids_permanent_snapshot(permanent, 4, &permanentRevision) == 0);
    assert(!model.avoidFrequency(QStringLiteral("17"), 451000000)); // not the running session
    assert(model.avoidFrequency(QStringLiteral("18"), 451000000) && startQueued());
    reopenAdmission();
    dsd_app_frontend_runtime_stop();
    assert(model.avoidPermanently(451000000) && model.error().isEmpty());
    assert(model.avoids().size() == 1 && model.avoids().front().toUInt() == 451000000);
    model.removeAvoid(451000000);
    assert(model.avoids().isEmpty() && model.error().isEmpty());
    // After Stop, a row's permanent avoid leaves the session list alone: the engine is its only writer. A list the
    // runtime still holds for the stopped session, the row's frequency included, keeps every entry and stays hidden.
    stopped.revision = 6002;
    dsd_app_signal_search_publish(&stopped, nullptr, nullptr);
    const uint32_t leftover[] = {463600000, 451000000};
    dsd_signal_search_avoids_session_set(18, leftover, 2);
    const uint64_t sessionRevision = dsd_signal_search_avoids_session_revision();
    model.refresh();
    assert(model.sessionAvoids().isEmpty());
    assert(model.avoidPermanently(451000000));
    uint64_t sessionOwner = 0;
    assert(dsd_signal_search_avoids_session_get(&sessionOwner, permanent, 4) == 2 && sessionOwner == 18);
    assert(permanent[0] == 463600000 && permanent[1] == 451000000);
    assert(dsd_signal_search_avoids_session_revision() == sessionRevision);
    assert(dsd_app_signal_search_session_avoids_get(permanent, 4) == 0);
    assert(model.sessionAvoids().isEmpty() && model.avoids().size() == 1);
    assert(dsd_app_signal_search_avoid_remove(451000000) == 0);
    dsd_signal_search_avoids_session_set(0, nullptr, 0);
    model.refresh();
    assert(model.avoids().isEmpty() && model.sessionAvoids().isEmpty());

    // With no search running, a history row's permanent avoid changes the permanent list alone, at once; a full
    // list is refused with a message, and an avoid for this session only needs a session.
    assert(!model.avoidFrequency(QStringLiteral("18"), 451000000));
    assert(model.avoidPermanently(451000000));
    assert(model.avoids().size() == 1 && model.avoids().front().toUInt() == 451000000 && model.error().isEmpty());
    assert(model.avoidPermanently(451000000)); // already avoided: as asked
    assert(model.avoids().size() == 1 && model.error().isEmpty());
    assert(!model.avoidPermanently(0) && !model.avoidPermanently(-5));
    for (uint32_t i = 1; i < DSD_SEARCH_MAX_AVOIDS; ++i) {
        assert(dsd_app_signal_search_permanent_avoid(451000000U + i * 12500U) == DSD_APP_SEARCH_AVOID_ADDED);
    }
    assert(!model.avoidPermanently(470000000));
    assert(model.error() == QStringLiteral("The permanent avoid list is full"));
    assert((uint32_t)model.avoids().size() == DSD_SEARCH_MAX_AVOIDS);
    // Lifting one through the model makes room, and the refusal goes with it.
    model.removeAvoid(model.avoids().front().toDouble());
    assert(model.error().isEmpty() && (uint32_t)model.avoids().size() == DSD_SEARCH_MAX_AVOIDS - 1);
    for (const auto& avoided : model.avoids()) {
        assert(dsd_app_signal_search_avoid_remove(avoided.toUInt()) == 0);
    }
    assert(model.avoidPermanently(470000000) && model.error().isEmpty());
    assert(dsd_app_signal_search_avoid_remove(470000000) == 0);
    model.refresh();
    assert(model.avoids().isEmpty());
    // A frequency above 2^31 Hz (2.4 GHz) is avoided as itself on every platform, including those whose long is 32 bits.
    assert(model.avoidPermanently(2400000000.0) && model.error().isEmpty());
    assert(model.avoids().size() == 1 && model.avoids().front().toUInt() == 2400000000U);
    assert(dsd_app_signal_search_avoid_remove(2400000000U) == 0);
    model.refresh();
    assert(model.avoids().isEmpty());

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
    testDestructorMarksItsTeardownStage(&index);
    testInitializedWhileStarting(&index);
    testArmWaitsThroughIdle(&index);
    testArmEndsWithSession(&index, false);
    testArmEndsWithSession(&index, true);
    testRunningEdgeWithoutInitialization(&index);
    testTickRetryWithoutInitialization(&index);
    testResumeWhileWindingDown(&index);
    testRestartAfterPausedStop(&index);
    testStoredMarginBelowMinimum(&index);
    testRangesKeptAside(&index);
    testRangeSaveFailureClears(&index);
    testStoreFilesWorded(&index);
    testKnownFrequencyIdentity(temp.path());
    testKnownFrequencyListsApart();
    testKnownFrequencyEngineIds(temp.path());
    testKnownFrequencyEngineGrammar(temp.path());
    testKnownFrequencyQuotedCells(temp.path());
    testKnownFrequencyRowCap(temp.path());
    testKnownFrequencyWatchesUnparsedFiles(temp.path());
    testKnownFrequencyTargetParity(temp.path());
    testKnownFrequencyChannelParity(temp.path());
    testKnownFrequencyNonAsciiPaths(temp.path());
    testRecordingKey();
    testOverloadMapped();
    return 0;
}

// NOLINTEND(cert-dcl03-c,misc-static-assert)
