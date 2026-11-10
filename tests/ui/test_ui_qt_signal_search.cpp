// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

// LLVM 22/GCC 16 misclassifies these runtime test oracles as compile-time assertions.
// NOLINTBEGIN(cert-dcl03-c,misc-static-assert)

#include <QByteArray>
#include <QChar>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QLatin1String>
#include <QList>
#include <QLocale>
#include <QLockFile>
#include <QMap>
#include <QMetaObject>
#include <QMetaType>
#include <QObject>
#include <QPair>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>
#include <QtGlobal>
#include <atomic>
#include <cassert>
#include <clocale>
#include <cmath>
#include <condition_variable>
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
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <qflags.h>
#include <qsystemdetection.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <thread>
#include <utility>
#include <vector>
#include "../test_support/qt_test_paths.h"
#include "csv_text.h"
#include "decoder_host.h"
#include "document_export_writer.h"
#include "document_save_transport.h"
#include "json_store.h"
#include "known_frequency_index.h"
#include "saved_systems_model.h"
#include "scan_lists_model.h"
#include "session_args.h"
#include "signal_search_export.h"
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

/* #782: the Android app's teardown times this destructor, which flushes the search store. Its export writer (#826)
   goes inside that stage, timed on its own too, so a save it waits for counts as the model's. */
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
                               QStringLiteral("teardown: ~DocumentExportWriter started"),
                               QStringLiteral("teardown: ~DocumentExportWriter took 7 ms"),
                               QStringLiteral("teardown: ~SignalSearchModel took 21 ms")}));
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

/* Issue #644: two lists under one name, each with a target on one frequency, are two Listen choices, each starting its
   own list: the matches stay apart by list. Issue #697: a manual list given another's entry uid is saved with another
   (the app's lists never share an id), so each match names its own list's entry; two imported lists whose files hold
   one target id stay two matches, one per list, under that one id. */
static void
testKnownFrequencyListsApart(const QString& dir) {
    dsd_qt::SavedSystemsModel systems;
    dsd_qt::ScanListsModel lists;
    const QVariantMap desk{
        {QStringLiteral("uid"), QStringLiteral("entry")},        {QStringLiteral("kind"), QStringLiteral("freq")},
        {QStringLiteral("name"), QStringLiteral("Desk")},        {QStringLiteral("protocol"), QStringLiteral("dmr")},
        {QStringLiteral("freqMhz"), QStringLiteral("450.1250")}, {QStringLiteral("enabled"), true}};
    QStringList uids;
    QStringList entryUids;
    for (int i = 0; i < 2; ++i) {
        assert(lists.add(
            {{QStringLiteral("name"), QStringLiteral("Gate")}, {QStringLiteral("entries"), QVariantList{desk}}}));
        const QVariantMap list = lists.get(lists.count() - 1);
        uids << list.value(QStringLiteral("uid")).toString();
        entryUids
            << list.value(QStringLiteral("entries")).toList().value(0).toMap().value(QStringLiteral("uid")).toString();
    }
    assert(uids[0] != uids[1]);
    assert(entryUids[0] == QStringLiteral("entry") && !entryUids[1].isEmpty() && entryUids[1] != entryUids[0]);
    dsd_qt::KnownFrequencyIndex index(&systems, &lists);
    const QVariantList matches = index.tunedMatches(450125000, 12500);
    assert(matches.size() == 2);
    QStringList pairs;
    for (const QVariant& match : matches) {
        assert(match.toMap().value(QStringLiteral("selectable")).toBool());
        pairs << match.toMap().value(QStringLiteral("listUid")).toString() + QLatin1Char('/')
                     + match.toMap().value(QStringLiteral("targetId")).toString();
    }
    pairs.sort();
    QStringList expected{uids[0] + QLatin1Char('/') + entryUids[0], uids[1] + QLatin1Char('/') + entryUids[1]};
    expected.sort();
    assert(pairs == expected);
    // Every model reads the one saved store, so the manual lists go before the imported ones are indexed.
    for (const QString& uid : uids) {
        assert(lists.remove(lists.rowForUid(uid)));
    }

    dsd_qt::ScanListsModel csvLists;
    QStringList csvUids;
    for (const char* file : {"apart-a.csv", "apart-b.csv"}) {
        const QString targets = QDir(dir).filePath(QString::fromLatin1(file));
        writeFile(targets, "id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes,label\n"
                           "entry,dmr-conventional,450125000,,,,,Desk\n");
        assert(csvLists.add({{QStringLiteral("name"), QStringLiteral("Gate")},
                             {QStringLiteral("targetSource"), QStringLiteral("csv")},
                             {QStringLiteral("targetsCsvPath"), targets}}));
        csvUids << csvLists.get(csvLists.count() - 1).value(QStringLiteral("uid")).toString();
    }
    assert(csvUids[0] != csvUids[1]);
    dsd_qt::KnownFrequencyIndex csvIndex(&systems, &csvLists);
    const QVariantList csvMatches = csvIndex.tunedMatches(450125000, 12500);
    assert(csvMatches.size() == 2);
    QStringList csvListUids;
    for (const QVariant& match : csvMatches) {
        assert(match.toMap().value(QStringLiteral("targetId")).toString() == QStringLiteral("entry"));
        assert(match.toMap().value(QStringLiteral("selectable")).toBool());
        csvListUids << match.toMap().value(QStringLiteral("listUid")).toString();
    }
    csvListUids.sort();
    csvUids.sort();
    assert(csvListUids == csvUids);
    for (const QString& uid : csvUids) {
        assert(csvLists.remove(csvLists.rowForUid(uid)));
    }
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

/* Issue #697: a target id a later list of the session also holds is one engine target, at its first list's place, so
   a session's own targets hold it once, in the engine's order (every new row in list order, then row order): [ROK, A]
   then [B, ROK] is ROK, A, B, each where the engine published its eligibility. One file run as two lists is its
   targets once. */
static void
testKnownFrequencySessionTargetsShared(const QString& dir) {
    dsd_qt::SavedSystemsModel systems;
    dsd_qt::ScanListsModel lists;
    const QString conv = QDir(dir).filePath(QStringLiteral("shared-conv.csv"));
    writeFile(conv, "id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes,label\n"
                    "ROK,dmr-conventional,460100000,,,,,Rock\n"
                    "A,dmr-conventional,460200000,,,,,Alpha\n");
    const QString low = QDir(dir).filePath(QStringLiteral("shared-low.csv"));
    writeFile(low, "id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes,label\n"
                   "B,dmr-conventional,460300000,,,,,Bravo\n"
                   "ROK,dmr-conventional,460100000,,,,,Rock\n");
    const dsd_qt::KnownFrequencyIndex index(&systems, &lists);
    const QVariantMap first{{"uid", QStringLiteral("conv-uid")}, {"name", QStringLiteral("Conv")}, {"path", conv}};
    const QVariantMap second{{"uid", QStringLiteral("low-uid")}, {"name", QStringLiteral("Low")}, {"path", low}};
    const QVariantList session = index.sessionTargets({first, second});

    struct Want {
        const char* id;
        qint64 hz;
        int list;
        const char* name;
    };

    const Want want[] = {
        {"ROK", 460100000, 1, "Conv / Rock"},
        {"A", 460200000, 1, "Conv / Alpha"},
        {"B", 460300000, 2, "Low / Bravo"},
    };
    if (session.size() != 3) {
        DSD_FPRINTF(stderr, "a shared target: %lld session targets, want 3\n", static_cast<long long>(session.size()));
        assert(0);
    }
    for (int i = 0; i < 3; ++i) {
        const QVariantMap target = session.at(i).toMap();
        assert(target.value(QStringLiteral("targetId")).toString() == QLatin1String(want[i].id));
        assert(target.value(QStringLiteral("frequencyHz")).toLongLong() == want[i].hz);
        assert(target.value(QStringLiteral("listNumber")).toInt() == want[i].list);
        assert(target.value(QStringLiteral("name")).toString() == QLatin1String(want[i].name));
    }
    // The other order: the low list's rows first, then the conv list's new one.
    const QVariantList reversed = index.sessionTargets({second, first});
    assert(reversed.size() == 3);
    assert(reversed.at(0).toMap().value(QStringLiteral("targetId")).toString() == QStringLiteral("B"));
    assert(reversed.at(1).toMap().value(QStringLiteral("targetId")).toString() == QStringLiteral("ROK"));
    assert(reversed.at(1).toMap().value(QStringLiteral("listNumber")).toInt() == 1);
    assert(reversed.at(2).toMap().value(QStringLiteral("targetId")).toString() == QStringLiteral("A"));
    assert(reversed.at(2).toMap().value(QStringLiteral("listNumber")).toInt() == 2);
    // One file as two lists: every target is the first list's.
    const QVariantList twice = index.sessionTargets(
        {first, QVariantMap{{"uid", QStringLiteral("again")}, {"name", QStringLiteral("Again")}, {"path", conv}}});
    assert(twice.size() == 2 && twice.at(1).toMap().value(QStringLiteral("listNumber")).toInt() == 1);
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

static void
test_csv_text() {
    using dsd_qt::csv_text::bom;
    using dsd_qt::csv_text::field;
    using dsd_qt::csv_text::record;
    const auto S = [](const char* s) { return QString::fromUtf8(s); };
    assert(bom() == QByteArray("\xEF\xBB\xBF", 3));
    assert(field(S("plain"), true) == S("plain"));
    assert(field(QString(), true).isEmpty());
    assert(field(S("a,b"), true) == S("\"a,b\""));
    assert(field(S("a\"b"), true) == S("\"a\"\"b\""));
    assert(field(S("a\rb"), false) == S("\"a\rb\""));
    assert(field(S("a\nb"), false) == S("\"a\nb\""));
    assert(field(S("a\r\nb"), true) == S("\"a\r\nb\""));
    assert(field(S(" a b "), true) == S(" a b "));
    assert(field(S("=1+2"), true) == S("'=1+2"));
    assert(field(S("=1,2"), true) == S("\"'=1,2\""));
    assert(field(S("\n x"), true) == S("\"'\n x\""));
    const QChar triggers[] = {QLatin1Char('='), QLatin1Char('+'), QLatin1Char('-'), QLatin1Char('@'), QLatin1Char('\t'),
                              QChar(0xFF1D),    QChar(0xFF0B),    QChar(0xFF0D),    QChar(0xFF20)};
    for (const QChar c : triggers) {
        const QString v = QString(c) + QStringLiteral("x");
        assert(field(v, true) == QLatin1Char('\'') + v);
        assert(field(v, false) == v);
    }
    assert(field(QString(QLatin1Char('\r')) + QStringLiteral("x"), true) == S("\"'\rx\""));
    assert(field(S("-47.2"), false) == S("-47.2"));
    assert(field(S("a=b+c-d@e"), true) == S("a=b+c-d@e"));
    const QString dot = QString(QChar(0x00B7)) + QStringLiteral("\u00e9\u4e2d");
    assert(field(dot, true).toUtf8() == dot.toUtf8());
    assert(record({dot}, {true}) == dot.toUtf8() + "\r\n");
    assert(record({S("a"), S("b,c"), S("=x")}, {true, true, true}) == QByteArray("a,\"b,c\",'=x\r\n"));
    assert(record({S("a"), S("-1")}, {true, false}) == QByteArray("a,-1\r\n"));
    assert(record({S("a")}, {true, false}).isEmpty());
    assert(record({}, {}) == QByteArray("\r\n"));
}

/* The bytes at @p path; empty when it cannot be read. */
static QByteArray
read_bytes(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

static void
write_bytes(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    assert(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    assert(file.write(bytes) == bytes.size());
}

/* Whether @p path is readable and writable by its owner and by nobody else (always true off POSIX). */
static bool
owner_only(const QString& path) {
#ifdef Q_OS_UNIX
    const QFileDevice::Permissions others = QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ExeGroup
                                            | QFileDevice::ReadOther | QFileDevice::WriteOther | QFileDevice::ExeOther;
    const QFileDevice::Permissions perms = QFile::permissions(path);
    return (perms & others) == 0 && perms.testFlag(QFileDevice::ReadOwner) && perms.testFlag(QFileDevice::WriteOwner);
#else
    (void)path;
    return true;
#endif
}

/* #826: the base save transport writes a local document, owner-only, and an abort stops it for good. */
static void
test_save_transport_local(const QString& root) {
    QDir(root).mkpath(QStringLiteral("save transport/My exports"));
    const QString dir = root + QStringLiteral("/save transport/My exports");
    const QString source = dir + QStringLiteral("/source.csv");
    const QByteArray bytes = QByteArray("\xEF\xBB\xBF", 3) + QByteArray("frequency_hz\r\n482005000\r\n");
    write_bytes(source, bytes);

    dsd_qt::DocumentSaveTransport transport;
    assert(!transport.aborted());
    const QString plain = dir + QStringLiteral("/plain.csv");
    assert(transport.save(source, plain));
    assert(read_bytes(plain) == bytes && owner_only(plain));

    // A file URL, as the desktop picker hands it, decoded or percent-encoded, to a name with spaces and non-ASCII.
    const QString named = dir + QString::fromUtf8("/S\xC3\xB8k \xC2\xB7.csv");
    assert(transport.save(source, QUrl::fromLocalFile(named).toString()));
    assert(read_bytes(named) == bytes && owner_only(named));
    QFile::remove(named);
    assert(transport.save(source, QString::fromUtf8(QUrl::fromLocalFile(named).toEncoded())));
    assert(read_bytes(named) == bytes);

    // Only local documents: a content URI or a relative path is refused, and a missing source writes nothing.
    assert(!transport.save(source, QStringLiteral("content://com.example.documents/document/1")));
    assert(!transport.save(source, QStringLiteral("relative.csv")));
    const QString fromMissing = dir + QStringLiteral("/from-missing.csv");
    assert(!transport.save(dir + QStringLiteral("/missing.csv"), fromMissing));
    assert(!QFileInfo::exists(fromMissing));
    transport.discardPendingDownload(plain); // the base transport holds no Download-folder rows
    assert(read_bytes(plain) == bytes);

    transport.abort();
    assert(transport.aborted());
    const QString late = dir + QStringLiteral("/late.csv");
    assert(!transport.save(source, late));
    assert(!QFileInfo::exists(late));
    assert(!transport.save(source, plain));
    assert(read_bytes(plain) == bytes);
}

/*
 * An abort from another thread while a large save runs: the save fails and the document keeps what it held. A save
 * that finished before the abort landed proves nothing, so the case tries again (with a fresh transport) a few times.
 */
static void
test_save_transport_abort_mid_save(const QString& root) {
    QDir(root).mkpath(QStringLiteral("save abort"));
    const QString dir = root + QStringLiteral("/save abort");
    const QString source = dir + QStringLiteral("/large.csv");
    write_bytes(source, QByteArray(32 << 20, 'x'));
    const QString dest = dir + QStringLiteral("/dest.csv");
    const QByteArray before("kept\r\n");
    bool stopped = false;
    for (int attempt = 0; attempt < 5 && !stopped; ++attempt) {
        write_bytes(dest, before);
        dsd_qt::DocumentSaveTransport transport;
        std::atomic<bool> started{false};
        bool saved = true;
        std::thread worker([&transport, &started, &saved, &source, &dest]() {
            started.store(true);
            saved = transport.save(source, dest);
        });
        while (!started.load()) {
            QThread::yieldCurrentThread();
        }
        QThread::msleep(1);
        transport.abort();
        worker.join();
        stopped = !saved;
    }
    assert(stopped);
    assert(read_bytes(dest) == before);
    assert(QDir(dir).entryList(QDir::Files | QDir::Hidden | QDir::System)
           == QStringList({QStringLiteral("dest.csv"), QStringLiteral("large.csv")}));
}

/* The host's hand-out: every transport aborted by abortDocumentTransfers(), one handed out after it starts aborted, and
   a transport kept past its host still saves. */
static void
test_save_transport_host(const QString& root) {
    QDir(root).mkpath(QStringLiteral("save host"));
    const QString dir = root + QStringLiteral("/save host");
    const QString source = dir + QStringLiteral("/source.csv");
    write_bytes(source, "a,b\r\n");

    std::shared_ptr<dsd_qt::DocumentSaveTransport> outliving;
    {
        SessionHost shortLived;
        outliving = shortLived.saveTransport();
    }
    assert(outliving != nullptr && !outliving->aborted());
    assert(outliving->save(source, dir + QStringLiteral("/kept.csv")));
    assert(read_bytes(dir + QStringLiteral("/kept.csv")) == "a,b\r\n");

    SessionHost host;
    const std::shared_ptr<dsd_qt::DocumentSaveTransport> kept = host.saveTransport();
    std::shared_ptr<dsd_qt::DocumentSaveTransport> dropped = host.saveTransport();
    assert(kept != nullptr && dropped != nullptr && kept != dropped && !kept->aborted());
    dropped.reset();
    host.abortDocumentTransfers();
    assert(kept->aborted());
    assert(!kept->save(source, dir + QStringLiteral("/after.csv")));
    const std::shared_ptr<dsd_qt::DocumentSaveTransport> after = host.saveTransport();
    assert(after != nullptr && after->aborted());
    assert(!after->save(source, dir + QStringLiteral("/after.csv")));
    assert(!QFileInfo::exists(dir + QStringLiteral("/after.csv")));
    // The host's own copy is unchanged by the transports: sticky after the abort as before.
    assert(!host.copyFileToDocument(source, dir + QStringLiteral("/after.csv")));
    assert(outliving->save(source, dir + QStringLiteral("/still.csv")));
}

namespace {

/* A save transport whose saves wait at a gate (#826), for the export writer's cases. Saves record what they were
   handed; an abort opens the gate unless the transport ignores aborts, as a stuck provider would. */
class GateTransport : public dsd_qt::DocumentSaveTransport {
  public:
    struct Save {
        QString source;
        QString reference;
        QByteArray bytes;
        QThread* thread = nullptr;
    };

    explicit GateTransport(bool open = true, bool honorAbort = true) : m_open(open), m_honorAbort(honorAbort) {}

    bool
    save(const QString& sourcePath, const QString& reference) override {
        const Save save{sourcePath, reference, read_bytes(sourcePath), QThread::currentThread()};
        std::unique_lock<std::mutex> lock(m_mutex);
        m_saves.append(save);
        m_cv.wait(lock, [this]() { return m_open || (m_honorAbort && aborted()); });
        m_returned++;
        return m_result && !aborted();
    }

    void
    discardPendingDownload(const QString& reference) override {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_discards.append(reference);
    }

    void
    openGate() {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_open = true;
        m_cv.notify_all();
    }

    void
    setResult(bool result) {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_result = result;
    }

    QList<Save>
    saves() const {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_saves;
    }

    QStringList
    discards() const {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_discards;
    }

    int
    returned() const {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_returned;
    }

  protected:
    void
    abortPlatform() override {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_cv.notify_all();
    }

  private:
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_open;
    bool m_honorAbort;
    bool m_result = true;
    int m_returned = 0;
    QList<Save> m_saves;
    QStringList m_discards;
};

/* The replies an export writer delivered, and the thread each ran on. */
struct ExportReplies {
    QList<QPair<int, bool>> replies;
    QList<QThread*> threads;

    dsd_qt::DocumentExportWriter::Reply
    reply() {
        return [this](int token, bool ok) {
            replies.append(qMakePair(token, ok));
            threads.append(QThread::currentThread());
        };
    }
};

} // namespace

/* Runs the event loop until @p done holds or about 5 s pass; whether it held. */
static bool
spin_until(const std::function<bool()>& done) {
    for (int i = 0; i < 500 && !done(); ++i) {
        spin(10);
    }
    return done();
}

/* What @p dir holds, hidden entries too, by name. */
static QStringList
entries(const QString& dir) {
    return QDir(dir).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDir::Name);
}

/* @p names, sorted as entries() lists them. */
static QStringList
sorted(QStringList names) {
    names.sort();
    return names;
}

/* The one staging folder under @p cacheRoot whose name is not in @p others. */
static QString
staging_folder(const QString& cacheRoot, const QStringList& others = QStringList()) {
    QStringList found;
    for (const QString& name : QDir(cacheRoot).entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (!others.contains(name)) {
            found.append(name);
        }
    }
    assert(found.size() == 1);
    return cacheRoot + QLatin1Char('/') + found.first();
}

/* #826: a job stages the bytes, hands the staged file to the transport off the owner's thread, removes it, and replies
   on the owner's thread; one job at a time. */
static void
test_export_writer_saves(const QString& root) {
    const QString cacheRoot = root + QStringLiteral("/export saves/signal-search-export");
    auto transport = std::make_shared<GateTransport>();
    QObject owner;
    ExportReplies log;
    const QByteArray bytes = QByteArray("\xEF\xBB\xBF", 3) + QByteArray("frequency_hz\r\n482005000\r\n");
    {
        dsd_qt::DocumentExportWriter writer(transport, cacheRoot, &owner, log.reply());
        const QString folder = staging_folder(cacheRoot);
        const QString name = QFileInfo(folder).fileName();
        assert(entries(cacheRoot) == sorted({name, name + QStringLiteral(".lock")}));
        assert(!writer.busy());
        assert(writer.submit(7, bytes, QStringLiteral("content://picked/7")));
        assert(writer.busy());
        // A second job while one runs is refused, and its picked document lets go of its Download-folder row.
        assert(!writer.submit(8, bytes, QStringLiteral("content://picked/8")));
        assert(transport->discards() == QStringList{QStringLiteral("content://picked/8")});
        assert(spin_until([&log]() { return !log.replies.isEmpty(); }));
        assert(log.replies == (QList<QPair<int, bool>>{qMakePair(7, true)}));
        assert(log.threads.first() == owner.thread());
        assert(!writer.busy());
        const QList<GateTransport::Save> saves = transport->saves();
        assert(saves.size() == 1 && saves[0].source == folder + QStringLiteral("/7.csv"));
        assert(saves[0].reference == QStringLiteral("content://picked/7") && saves[0].bytes == bytes);
        assert(saves[0].thread != owner.thread());
        assert(entries(folder).isEmpty());

        // A failed save replies false and still removes the staged file; the transport let go of the row itself.
        transport->setResult(false);
        assert(writer.submit(9, bytes, QStringLiteral("content://picked/9")));
        assert(spin_until([&log]() { return log.replies.size() == 2; }));
        assert(log.replies.last() == qMakePair(9, false) && log.threads.last() == owner.thread());
        assert(transport->saves().size() == 2 && transport->saves().last().source == folder + QStringLiteral("/9.csv"));
        assert(transport->discards().size() == 1);
        assert(entries(folder).isEmpty() && !writer.busy());
    }
    // An idle writer takes its folder and lock with it, aborts nothing, and nothing replies after it.
    assert(entries(cacheRoot).isEmpty());
    assert(!transport->aborted());
    spin(20);
    assert(log.replies.size() == 2);
}

/* A job that cannot stage its file lets go of the picked document's Download-folder row and replies false. */
static void
test_export_writer_staging_failures(const QString& root) {
    auto transport = std::make_shared<GateTransport>();
    QObject owner;
    ExportReplies log;
    const QByteArray bytes("a,b\r\n");

    // A cache root that cannot be made: under a file.
    QDir(root).mkpath(QStringLiteral("export failures"));
    const QString blocker = root + QStringLiteral("/export failures/file");
    write_bytes(blocker, "x");
    {
        dsd_qt::DocumentExportWriter writer(transport, blocker + QStringLiteral("/cache"), &owner, log.reply());
        assert(writer.submit(1, bytes, QStringLiteral("r1")));
        assert(spin_until([&log]() { return log.replies.size() == 1; }));
        assert(log.replies.last() == qMakePair(1, false) && !writer.busy());
        assert(transport->discards() == QStringList{QStringLiteral("r1")} && transport->saves().isEmpty());
        // Idle again: the next job is taken (and fails the same way).
        assert(writer.submit(2, bytes, QStringLiteral("r2")));
        assert(spin_until([&log]() { return log.replies.size() == 2; }));
        assert(transport->discards().last() == QStringLiteral("r2"));
    }

    const QString cacheRoot = root + QStringLiteral("/export failures/cache");
    dsd_qt::DocumentExportWriter writer(transport, cacheRoot, &owner, log.reply());
    const QString folder = staging_folder(cacheRoot);
    // A commit failure: the job's path is a folder, so the written copy cannot replace it.
    assert(QDir(folder).mkdir(QStringLiteral("3.csv")));
    assert(writer.submit(3, bytes, QStringLiteral("r3")));
    assert(spin_until([&log]() { return log.replies.size() == 3; }));
    assert(log.replies.last() == qMakePair(3, false) && !writer.busy());
    assert(transport->discards().last() == QStringLiteral("r3") && transport->saves().isEmpty());
    assert(entries(folder) == QStringList{QStringLiteral("3.csv")});
    assert(QDir(folder).rmdir(QStringLiteral("3.csv")));

#ifdef Q_OS_UNIX
    // An open failure: a folder nothing can be written into (unless the tests run as a user permissions do not bind).
    QFile::setPermissions(folder, QFileDevice::ReadOwner | QFileDevice::ExeOwner);
    QFile probe(folder + QStringLiteral("/probe"));
    if (!probe.open(QIODevice::WriteOnly)) {
        assert(writer.submit(4, bytes, QStringLiteral("r4")));
        assert(spin_until([&log]() { return log.replies.size() == 4; }));
        assert(log.replies.last() == qMakePair(4, false) && !writer.busy());
        assert(transport->discards().last() == QStringLiteral("r4") && transport->saves().isEmpty());
    } else {
        probe.remove();
    }
    QFile::setPermissions(folder, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
#endif
    assert(entries(folder).isEmpty());

    // A transport aborted at teardown: a new job is refused at once and its row let go of.
    transport->abort();
    const qsizetype before = log.replies.size();
    assert(!writer.submit(5, bytes, QStringLiteral("r5")));
    assert(!writer.busy() && transport->discards().last() == QStringLiteral("r5"));
    spin(20);
    assert(log.replies.size() == before);
}

/* The child mode of this binary: hold the lock file argv[2] until stdin closes or the process is killed. */
static int
hold_export_lock(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QLockFile lock(QString::fromLocal8Bit(argv[2]));
    lock.setStaleLockTime(0);
    if (!lock.tryLock(0)) {
        return 1;
    }
    fputs("locked\n", stdout);
    fflush(stdout);
    char buffer[64];
    while (fread(buffer, 1, sizeof(buffer), stdin) > 0) {}
    return 0;
}

/* Starts this binary holding @p lockPath; it answers once it holds it. */
static void
start_lock_holder(QProcess& child, const QString& lockPath) {
    child.start(QCoreApplication::applicationFilePath(), {QStringLiteral("--hold-export-lock"), lockPath});
    assert(child.waitForStarted(5000));
    assert(child.waitForReadyRead(5000) && child.readAllStandardOutput().startsWith("locked"));
}

static void
kill_lock_holder(QProcess& child) {
    child.kill();
    assert(child.waitForFinished(5000));
}

/* Each writer sweeps the folders no live writer holds: those whose lock it can take (none, or one whose process is
   gone). Age never makes a lock stale. */
static void
test_export_writer_sweep(const QString& root) {
    const QString cacheRoot = root + QStringLiteral("/export sweep");
    QDir(root).mkpath(QStringLiteral("export sweep/free"));
    write_bytes(cacheRoot + QStringLiteral("/free/1.csv"), "left");

    // Held in this process, and last written a day ago.
    QDir(cacheRoot).mkdir(QStringLiteral("live"));
    auto live = std::make_unique<QLockFile>(cacheRoot + QStringLiteral("/live.lock"));
    live->setStaleLockTime(0);
    assert(live->tryLock(0));
    {
        QFile stamp(cacheRoot + QStringLiteral("/live.lock"));
        assert(stamp.open(QIODevice::ReadOnly));
        assert(stamp.setFileTime(QDateTime::currentDateTimeUtc().addDays(-1), QFileDevice::FileModificationTime));
    }

    // Held by another process; and one whose holder is gone.
    QDir(cacheRoot).mkdir(QStringLiteral("child"));
    QProcess child;
    start_lock_holder(child, cacheRoot + QStringLiteral("/child.lock"));
    QDir(cacheRoot).mkdir(QStringLiteral("dead"));
    write_bytes(cacheRoot + QStringLiteral("/dead/2.csv"), "left");
    QProcess dead;
    start_lock_holder(dead, cacheRoot + QStringLiteral("/dead.lock"));
    kill_lock_holder(dead);
    assert(QFileInfo::exists(cacheRoot + QStringLiteral("/dead.lock")));

    auto transport = std::make_shared<GateTransport>();
    QObject owner;
    ExportReplies log;
    {
        dsd_qt::DocumentExportWriter first(transport, cacheRoot, &owner, log.reply());
        const QString own =
            QFileInfo(staging_folder(cacheRoot, {QStringLiteral("child"), QStringLiteral("live")})).fileName();
        const QStringList expected =
            sorted({QStringLiteral("child"), QStringLiteral("child.lock"), QStringLiteral("live"),
                    QStringLiteral("live.lock"), own, own + QStringLiteral(".lock")});
        assert(entries(cacheRoot) == expected);
        // Another writer in this process keeps the first one's folder.
        {
            const dsd_qt::DocumentExportWriter second(transport, cacheRoot, &owner, log.reply());
            assert(QFileInfo::exists(cacheRoot + QLatin1Char('/') + own)
                   && QFileInfo::exists(cacheRoot + QLatin1Char('/') + own + QStringLiteral(".lock")));
        }
        assert(entries(cacheRoot) == expected);
    }

    // Once the other process is gone its folder goes too; the lock held here keeps its own.
    kill_lock_holder(child);
    {
        const dsd_qt::DocumentExportWriter third(transport, cacheRoot, &owner, log.reply());
        const QString own = QFileInfo(staging_folder(cacheRoot, {QStringLiteral("live")})).fileName();
        assert(entries(cacheRoot)
               == sorted({QStringLiteral("live"), QStringLiteral("live.lock"), own, own + QStringLiteral(".lock")}));
    }
    live.reset();
    assert(entries(cacheRoot) == QStringList{QStringLiteral("live")});
    {
        // Free now: the next writer sweeps it.
        const dsd_qt::DocumentExportWriter fourth(transport, cacheRoot, &owner, log.reply());
        assert(!QFileInfo::exists(cacheRoot + QStringLiteral("/live")));
    }
    assert(entries(cacheRoot).isEmpty());
    assert(log.replies.isEmpty());
}

/* Teardown with a save in progress that heeds the abort: it stops at once, nothing replies, and the writer's folder
   and lock go. */
static void
test_export_writer_teardown_stops(const QString& root) {
    const QString cacheRoot = root + QStringLiteral("/export teardown");
    auto transport = std::make_shared<GateTransport>(false, true);
    QObject owner;
    ExportReplies log;
    auto writer = std::make_unique<dsd_qt::DocumentExportWriter>(transport, cacheRoot, &owner, log.reply());
    assert(writer->submit(1, QByteArray("a\r\n"), QStringLiteral("r1")));
    assert(spin_until([&transport]() { return !transport->saves().isEmpty(); }));
    QElapsedTimer timer;
    timer.start();
    writer.reset();
    assert(timer.elapsed() < dsd_qt::DocumentExportWriter::kStopWaitMs);
    assert(transport->aborted() && transport->returned() == 1);
    assert(entries(cacheRoot).isEmpty());
    spin(50);
    assert(log.replies.isEmpty());
}

/* Teardown with a save that ignores the abort: the writer waits about kStopWaitMs, leaves the job behind with its
   folder and lock, and the job's late end never replies. */
static void
test_export_writer_teardown_blocked(const QString& root) {
    const QString cacheRoot = root + QStringLiteral("/export blocked");
    auto transport = std::make_shared<GateTransport>(false, false);
    QObject owner;
    ExportReplies log;
    auto writer = std::make_unique<dsd_qt::DocumentExportWriter>(transport, cacheRoot, &owner, log.reply());
    const QString folder = staging_folder(cacheRoot);
    assert(writer->submit(1, QByteArray("a\r\n"), QStringLiteral("r1")));
    assert(spin_until([&transport]() { return !transport->saves().isEmpty(); }));
    QElapsedTimer timer;
    timer.start();
    writer.reset();
    const qint64 took = timer.elapsed();
    // The upper bound only proves the wait is bounded: a loaded or sanitized runner can be slow to wake.
    assert(took >= dsd_qt::DocumentExportWriter::kStopWaitMs - 100
           && took < qint64{dsd_qt::DocumentExportWriter::kStopWaitMs} * 4);
    assert(transport->aborted() && transport->returned() == 0);
    assert(QFileInfo::exists(folder + QStringLiteral("/1.csv")) && QFileInfo::exists(folder + QStringLiteral(".lock")));

    transport->openGate();
    assert(spin_until([&transport]() { return transport->returned() == 1; }));
    assert(spin_until([&folder]() { return !QFileInfo::exists(folder + QStringLiteral("/1.csv")); }));
    spin(50);
    assert(log.replies.isEmpty());
    // The left-behind job keeps its lock until the process ends, so a new writer keeps its folder.
    {
        const dsd_qt::DocumentExportWriter next(std::make_shared<GateTransport>(), cacheRoot, &owner, log.reply());
        assert(QFileInfo::exists(folder) && QFileInfo::exists(folder + QStringLiteral(".lock")));
    }
    const QString name = QFileInfo(folder).fileName();
    assert(entries(cacheRoot) == sorted({name, name + QStringLiteral(".lock")}));
}

/* #826: a detection with every exported field set, and receiver settings the file must leave out. */
static dsd_signal_search_hit
export_full_hit() {
    dsd_signal_search_hit h{};
    h.session_id = 31;
    h.hit_id = 4;
    h.started_ms = 1794060202418ULL; // 2026-11-07T14:03:22.418Z
    h.ended_ms = 1794060210000ULL;   // a whole second
    h.ended = 1;
    h.frequency_hz = 463552100;
    h.bandwidth_hz = 12500;
    h.step_num = 12500;
    h.step_den = 1;
    h.power_dbfs = -32.123f;
    h.noise_dbfs = -67.456f;
    h.confirmed = 1;
    DSD_SNPRINTF(h.protocol, sizeof(h.protocol), "%s", "DMR");
    DSD_SNPRINTF(h.access, sizeof(h.access), "%s", "CC 1");
    DSD_SNPRINTF(h.tone, sizeof(h.tone), "%s", "CTCSS 127.3 Hz");
    DSD_SNPRINTF(h.site, sizeof(h.site), "%s", "Net 12, Site 3 \xC2\xB7 SYS 1183");
    h.slot = 2;
    h.target = 1234;
    h.source = 99;
    DSD_SNPRINTF(h.source_text, sizeof(h.source_text), "%s", "=HYPERLINK(1)");
    DSD_SNPRINTF(h.crypt, sizeof(h.crypt), "%s", "ALG 21 \xC2\xB7 KID 0001");
    DSD_SNPRINTF(h.range_name, sizeof(h.range_name), "%s", "-Air \"band\"");
    DSD_SNPRINTF(h.input_spec, sizeof(h.input_spec), "%s", "rtltcp:10.9.8.7:1234:463.55M");
    h.ppm = 57;
    h.bias_tee = 1;
    h.gain_tenth_db = 123;
    DSD_SNPRINTF(h.airspy.serial, sizeof(h.airspy.serial), "%s", "A1B2C3D4E5F60718");
    return h;
}

static const char kDetectionHeader[] =
    "started_utc,ended_utc,ended,frequency_hz,frequency_mhz,listen_mode,identification,protocol,access,tone,site,slot,"
    "target,source,encryption,power_dbfs,noise_dbfs,range,bandwidth_hz,saved_names\r\n";

/* The detections file @p hits make, with "Desk / Tower, East" saved at 463.5521 MHz and nothing elsewhere. */
static QByteArray
export_detections(const std::vector<dsd_signal_search_hit>& hits, QList<uint32_t>* asked = nullptr) {
    return dsd_qt::signal_search_export::detections(hits.data(), hits.size(), [asked](const dsd_signal_search_hit& h) {
        if (asked) {
            asked->append(h.frequency_hz);
        }
        return h.frequency_hz == 463552100U ? QStringLiteral("Desk / Tower, East") : QString();
    });
}

/* #826: the detections file's header and each column's format, exclusions and formula guard. */
static void
test_export_detection_rows() {
    std::vector<dsd_signal_search_hit> hits(3);
    hits[0] = export_full_hit();
    // Empty and zero fields: no start, no end, no slot, no parties, levels that are not finite; an analog range.
    hits[1].frequency_hz = 118000000;
    hits[1].bandwidth_hz = 6000;
    hits[1].am = 1;
    hits[1].power_dbfs = std::numeric_limits<float>::quiet_NaN();
    hits[1].noise_dbfs = std::numeric_limits<float>::infinity();
    // Ended with no end time (a row loaded after an abrupt exit), a start on a whole second, a digital range not
    // confirmed, a target's received text over its number.
    hits[2].started_ms = 1794060210000ULL;
    hits[2].ended = 1;
    hits[2].frequency_hz = 155340000;
    hits[2].bandwidth_hz = 12500;
    hits[2].unresolved = 1;
    hits[2].slot = 1;
    hits[2].target = 7;
    DSD_SNPRINTF(hits[2].target_text, sizeof(hits[2].target_text), "%s", "TG 5");
    QList<uint32_t> asked;
    const QByteArray file = export_detections(hits, &asked);
    assert(asked == (QList<uint32_t>{463552100U, 118000000U, 155340000U}));
    const QByteArray expected =
        QByteArray("\xEF\xBB\xBF", 3) + kDetectionHeader
        + "2026-11-07T14:03:22.418Z,2026-11-07T14:03:30.000Z,1,463552100,463.552100,nfm,confirmed,DMR,CC 1,CTCSS 127.3 "
          "Hz,\"Net 12, Site 3 \xC2\xB7 SYS 1183\",2,1234,'=HYPERLINK(1),ALG 21 \xC2\xB7 KID "
          "0001,-32.1,-67.5,\"'-Air \"\"band\"\"\",12500,\"Desk / Tower, East\"\r\n"
        + ",,0,118000000,118.000000,am,rf-only,,,,,,,,,,,,6000,\r\n"
        + "2026-11-07T14:03:30.000Z,,1,155340000,155.340000,nfm,unresolved,,,,,1,TG 5,,,0.0,0.0,,12500,\r\n";
    assert(file == expected);
    // The receiver settings and ids stay out.
    for (const char* left : {"rtltcp", "10.9.8.7", "A1B2C3D4E5F60718", "57", "12.3", "31"}) {
        assert(!file.contains(left));
    }
    // Nothing but the header for no rows.
    assert(export_detections({}) == QByteArray("\xEF\xBB\xBF", 3) + kDetectionHeader);

    // The decimal point stays '.' whatever the locale.
    const QLocale before;
    const QByteArray numeric = QByteArray(setlocale(LC_NUMERIC, nullptr));
    QLocale::setDefault(QLocale(QLocale::German, QLocale::Germany));
    const bool cLocale = setlocale(LC_NUMERIC, "de_DE.UTF-8") != nullptr;
    printf("export locale check: QLocale German%s\n", cLocale ? ", LC_NUMERIC de_DE.UTF-8" : " (no de_DE.UTF-8)");
    assert(export_detections(hits) == expected);
    QLocale::setDefault(before);
    setlocale(LC_NUMERIC, numeric.constData());
}

/* #826: the avoids file lists the permanent avoids in saved order, then the session's; a frequency in both is written
   once, as permanent. */
static void
test_export_avoid_rows() {
    const QByteArray head = QByteArray("\xEF\xBB\xBF", 3) + "frequency_hz,frequency_mhz,list\r\n";
    int rows = -1;
    assert(dsd_qt::signal_search_export::avoids({451000000U, 162550000U}, {}, &rows)
           == head + "451000000,451.000000,permanent\r\n162550000,162.550000,permanent\r\n");
    assert(rows == 2);
    assert(dsd_qt::signal_search_export::avoids({}, {463575000U}, &rows) == head + "463575000,463.575000,session\r\n");
    assert(rows == 1);
    assert(dsd_qt::signal_search_export::avoids({463600000U}, {463575000U, 463600000U}, &rows)
           == head + "463600000,463.600000,permanent\r\n463575000,463.575000,session\r\n");
    assert(rows == 2);
    assert(dsd_qt::signal_search_export::avoids({}, {}, &rows) == head && rows == 0);
}

/* #826: the avoid lists are read as one consistent pair: again while the revision moved between the reads, up to 32
   times, then not at all. */
static void
test_export_avoid_reads() {
    uint64_t revision = 40;
    int calls = 0, permanentReads = 0, moving = 3;
    dsd_qt::signal_search_export::AvoidSource source;
    source.revision = [&]() {
        ++calls;
        // The second read of each try moves while `moving` tries remain.
        if (calls % 2 == 0 && moving > 0) {
            --moving;
            return ++revision;
        }
        return revision;
    };
    source.permanent = [&](uint32_t* out, size_t cap) {
        ++permanentReads;
        assert(cap >= 2);
        out[0] = 451000000U + (uint32_t)permanentReads;
        out[1] = 162550000U;
        return (size_t)2;
    };
    source.session = [](uint32_t* out, size_t cap) {
        assert(cap >= 1);
        out[0] = 463575000U;
        return (size_t)1;
    };
    std::vector<uint32_t> permanent, session;
    assert(dsd_qt::signal_search_export::readAvoids(source, &permanent, &session));
    assert(permanentReads == 4 && calls == 8);
    assert(permanent == (std::vector<uint32_t>{451000004U, 162550000U})
           && session == std::vector<uint32_t>{463575000U});

    // Lists that never stop changing: 32 tries, then nothing.
    permanentReads = 0;
    moving = 1000;
    assert(!dsd_qt::signal_search_export::readAvoids(source, &permanent, &session));
    assert(permanentReads == dsd_qt::signal_search_export::kAvoidReadTries && permanentReads == 32);
    assert(permanent.empty() && session.empty());
}

namespace {
/** A host whose export saves go to @p transport. */
class ExportHost : public SessionHost {
  public:
    explicit ExportHost(std::shared_ptr<dsd_qt::DocumentSaveTransport> transport) : m_transport(std::move(transport)) {}

    std::shared_ptr<dsd_qt::DocumentSaveTransport>
    saveTransport() const override {
        return keepSaveTransport(m_transport);
    }

  private:
    std::shared_ptr<dsd_qt::DocumentSaveTransport> m_transport;
};

/** The exportFinished() signals a model sends while this lives. */
struct ExportFinished {
    QList<QPair<int, bool>> replies;
    QMetaObject::Connection connection;

    explicit ExportFinished(const dsd_qt::SignalSearchModel& model)
        : connection(QObject::connect(&model, &dsd_qt::SignalSearchModel::exportFinished,
                                      [this](int token, bool ok) { replies.append(qMakePair(token, ok)); })) {}

    ~ExportFinished() { QObject::disconnect(connection); }

    ExportFinished(const ExportFinished&) = delete;
    ExportFinished& operator=(const ExportFinished&) = delete;
};
} // namespace

/* An empty store of its own under @p root, for the export cases. A model opens the app's store when made, so this
   follows the model. */
static void
open_export_store(const QString& root, const char* name) {
    dsd_app_signal_search_store_close();
    const QString dir = root + QStringLiteral("/export stores/") + QLatin1String(name);
    assert(QDir().mkpath(dir));
    assert(dsd_app_signal_search_store_open((dir + QStringLiteral("/history")).toUtf8().constData()) == 0);
    std::vector<dsd_signal_search_hit> hits(1);
    assert(dsd_app_signal_search_history_get(hits.data(), hits.size()) == 0);
}

static int
export_token(const QVariantMap& prepared) {
    return prepared.value(QStringLiteral("token")).toInt();
}

static int
export_count(const QVariantMap& prepared) {
    return prepared.value(QStringLiteral("count")).toInt();
}

/* Prepare @p kind and save it through @p transport; the bytes saved, after asserting @p count rows. */
static QByteArray
exported(dsd_qt::SignalSearchModel& model, const GateTransport& transport, const QString& kind, int count) {
    const ExportFinished finished(model);
    const QVariantMap prepared = model.prepareExport(kind);
    assert(export_count(prepared) == count && export_token(prepared) > 0);
    const int saves = transport.saves().size();
    assert(model.saveExport(export_token(prepared), QStringLiteral("content://picked/avoids")));
    assert(spin_until([&finished]() { return !finished.replies.isEmpty(); }));
    assert(finished.replies == (QList<QPair<int, bool>>{qMakePair(export_token(prepared), true)}));
    assert(transport.saves().size() == saves + 1);
    return transport.saves().last().bytes;
}

/* #826: the avoids the model exports: the shown lists, read at the press. */
static void
test_export_model_avoids(const QString& root) {
    auto transport = std::make_shared<GateTransport>();
    ExportHost host(transport);
    dsd_qt::SignalSearchModel model(&host, nullptr);
    open_export_store(root, "avoids");
    const QByteArray head = QByteArray("\xEF\xBB\xBF", 3) + "frequency_hz,frequency_mhz,list\r\n";
    const QVariantMap none{{QStringLiteral("token"), 0}, {QStringLiteral("count"), 0}};
    assert(model.prepareExport(QStringLiteral("avoids")) == none);

    // Permanent only, in saved order.
    assert(dsd_app_signal_search_permanent_avoid(451000000) == DSD_APP_SEARCH_AVOID_ADDED);
    assert(dsd_app_signal_search_permanent_avoid(162550000) == DSD_APP_SEARCH_AVOID_ADDED);
    assert(exported(model, *transport, QStringLiteral("avoids"), 2)
           == head + "451000000,451.000000,permanent\r\n162550000,162.550000,permanent\r\n");
    assert(dsd_app_signal_search_avoid_remove(451000000) == 0 && dsd_app_signal_search_avoid_remove(162550000) == 0);

    // Session only, while the session runs.
    dsd_signal_search_status running{};
    running.session_id = 61;
    running.revision = 8100;
    running.phase = DSD_SEARCH_LISTENING;
    running.owned = 1;
    dsd_app_signal_search_publish(&running, nullptr, nullptr);
    const uint32_t sessionList[] = {463575000, 463600000};
    dsd_signal_search_avoids_session_set(61, sessionList, 2);
    assert(exported(model, *transport, QStringLiteral("avoids"), 2)
           == head + "463575000,463.575000,session\r\n463600000,463.600000,session\r\n");

    // Both; a frequency just made permanent, still in the session list until the engine's next tick, is written once.
    assert(dsd_app_signal_search_permanent_avoid(463600000) == DSD_APP_SEARCH_AVOID_ADDED);
    assert(exported(model, *transport, QStringLiteral("avoids"), 2)
           == head + "463600000,463.600000,permanent\r\n463575000,463.575000,session\r\n");
    assert(dsd_app_signal_search_avoid_remove(463600000) == 0);

    // A stopped search shows no session list, so nothing is left to export.
    dsd_signal_search_status stopped = running;
    stopped.revision = 8101;
    stopped.phase = DSD_SEARCH_STOPPED;
    stopped.owned = 0;
    dsd_app_signal_search_publish(&stopped, nullptr, nullptr);
    assert(model.prepareExport(QStringLiteral("avoids")) == none);
    dsd_signal_search_avoids_session_set(0, nullptr, 0);
    assert(transport->discards().isEmpty());
}

/* #826: one pending export at a time, kept as it was prepared; a token that is not the pending one is refused, and its
   picked document lets go of its Download-folder row. */
static void
test_export_model_tokens(const QString& root) {
    auto transport = std::make_shared<GateTransport>(false);
    ExportHost host(transport);
    dsd_qt::SignalSearchModel model(&host, nullptr);
    open_export_store(root, "tokens");
    const ExportFinished finished(model);
    const QVariantMap unknown{{QStringLiteral("token"), 0}, {QStringLiteral("count"), -1}};
    assert(model.prepareExport(QStringLiteral("everything")) == unknown);
    assert(model.prepareExport(QString()) == unknown);
    assert(dsd_app_signal_search_permanent_avoid(451000000) == DSD_APP_SEARCH_AVOID_ADDED);

    // A second prepare replaces the first: the first token is refused.
    const int first = export_token(model.prepareExport(QStringLiteral("avoids")));
    const int second = export_token(model.prepareExport(QStringLiteral("avoids")));
    assert(first > 0 && second > 0 && second != first);
    assert(!model.saveExport(first, QStringLiteral("content://stale/1")));
    assert(transport->discards() == QStringList{QStringLiteral("content://stale/1")});

    // The list changed after the press: the file is the one prepared.
    assert(dsd_app_signal_search_permanent_avoid(162550000) == DSD_APP_SEARCH_AVOID_ADDED);
    assert(model.saveExport(second, QStringLiteral("content://picked/2")));
    // Saved once: the token is no longer pending.
    assert(!model.saveExport(second, QStringLiteral("content://again/2")));
    // A save while one runs is refused, and the export is no longer pending.
    const QVariantMap third = model.prepareExport(QStringLiteral("avoids"));
    assert(export_count(third) == 2);
    assert(!model.saveExport(export_token(third), QStringLiteral("content://busy/3")));
    assert(!model.saveExport(export_token(third), QStringLiteral("content://busy/3b")));
    assert(transport->discards()
           == (QStringList{QStringLiteral("content://stale/1"), QStringLiteral("content://again/2"),
                           QStringLiteral("content://busy/3"), QStringLiteral("content://busy/3b")}));
    transport->openGate();
    assert(spin_until([&finished]() { return !finished.replies.isEmpty(); }));
    assert(finished.replies == (QList<QPair<int, bool>>{qMakePair(second, true)}));
    assert(transport->saves().size() == 1 && transport->saves()[0].reference == QStringLiteral("content://picked/2"));
    assert(transport->saves()[0].bytes
           == QByteArray("\xEF\xBB\xBF", 3) + "frequency_hz,frequency_mhz,list\r\n451000000,451.000000,permanent\r\n");

    // Discard drops only its own token.
    const int fourth = export_token(model.prepareExport(QStringLiteral("avoids")));
    model.discardExport(fourth + 1);
    model.discardExport(0);
    assert(model.saveExport(fourth, QStringLiteral("content://picked/4")));
    assert(spin_until([&finished]() { return finished.replies.size() == 2; }));
    assert(finished.replies.last() == qMakePair(fourth, true));
    const int fifth = export_token(model.prepareExport(QStringLiteral("avoids")));
    model.discardExport(fifth);
    assert(!model.saveExport(fifth, QStringLiteral("content://gone/5")));
    assert(transport->discards().last() == QStringLiteral("content://gone/5"));

    // A prepare with no rows keeps nothing, and drops the export pending before it.
    const int sixth = export_token(model.prepareExport(QStringLiteral("avoids")));
    assert(model.prepareExport(QStringLiteral("detections"))
           == (QVariantMap{{QStringLiteral("token"), 0}, {QStringLiteral("count"), 0}}));
    assert(!model.saveExport(sixth, QStringLiteral("content://gone/6")));
    assert(!model.saveExport(0, QStringLiteral("content://none")));
    assert(transport->discards().last() == QStringLiteral("content://none"));
    spin(20);
    assert(finished.replies.size() == 2 && transport->saves().size() == 2);
    assert(dsd_app_signal_search_avoid_remove(451000000) == 0 && dsd_app_signal_search_avoid_remove(162550000) == 0);
}

/* #826: a history of 1,000 detections spread over a few frequencies, each saved under many tied names, exports
   newest first with hitMap()'s saved names per row, through the base host's transport into a local file. */
static void
test_export_model_detections(const QString& root) {
    static const uint32_t kFrequencies[] = {463552100U, 463575000U, 451000000U, 162550000U, 155340000U};
    const QString channels = root + QStringLiteral("/export-heavy-channels.csv");
    QByteArray map = "chan,freq,name\n";
    int chan = 0;
    for (const uint32_t frequency : kFrequencies) {
        for (int i = 0; i < 100; ++i, ++chan) {
            map += QByteArray::number(chan) + ',' + QByteArray::number(frequency) + ",Ch" + QByteArray::number(chan)
                   + '\n';
        }
    }
    writeFile(channels, map.constData());
    dsd_qt::SavedSystemsModel systems;
    dsd_qt::ScanListsModel lists;
    addMapSystem(&systems, QStringLiteral("Heavy"), channels);
    dsd_qt::KnownFrequencyIndex index(&systems, &lists);

    SessionHost host;
    dsd_qt::SignalSearchModel model(&host, &index);
    const ExportFinished finished(model);
    open_export_store(root, "detections");
    for (uint64_t id = 1; id <= 1000; ++id) {
        dsd_signal_search_status s{};
        s.session_id = 71;
        s.revision = 9000 + id;
        s.phase = DSD_SEARCH_LISTENING;
        dsd_signal_search_hit& h = s.hit;
        h.session_id = 71;
        h.hit_id = id;
        h.started_ms = 1794060202418ULL + id * 1000U;
        h.frequency_hz = kFrequencies[id % 5];
        h.bandwidth_hz = id % 2 ? 12500 : 6000;
        h.step_num = 12500;
        h.step_den = 1;
        h.confirmed = id % 3 == 0;
        h.unresolved = id % 3 == 1;
        DSD_SNPRINTF(h.input_spec, sizeof(h.input_spec), "%s", "rtltcp:10.9.8.7:1234");
        dsd_app_signal_search_publish(&s, &h, nullptr);
    }
    model.refresh();
    assert(model.history().size() == 1000);
    assert(model.history().front().toMap().value(QStringLiteral("knownLabel")).toString().count(QStringLiteral(" / "))
           >= 99);

    QElapsedTimer timer;
    timer.start();
    const QVariantMap prepared = model.prepareExport(QStringLiteral("detections"));
    printf("export of 1000 detections over a heavy index took %lld ms\n", (long long)timer.elapsed());
    assert(export_count(prepared) == 1000 && export_token(prepared) > 0);

    // The file the snapshot makes with the screen's saved names, row by row.
    std::vector<dsd_signal_search_hit> hits(DSD_SEARCH_HISTORY_LIMIT);
    hits.resize(dsd_app_signal_search_history_snapshot(hits.data(), hits.size(), nullptr));
    assert(hits.size() == 1000 && hits.front().hit_id == 1000 && hits.back().hit_id == 1);
    const QVariantList history = model.history();
    const QByteArray expected = dsd_qt::signal_search_export::detections(
        hits.data(), hits.size(), [&hits, &history](const dsd_signal_search_hit& h) {
            return history.at((qsizetype)(&h - hits.data())).toMap().value(QStringLiteral("knownLabel")).toString();
        });

    const QString folder = root + QStringLiteral("/export saved");
    assert(QDir().mkpath(folder));
    const QString target = folder + QStringLiteral("/Signal search detections.csv");
    assert(model.saveExport(export_token(prepared), target));
    assert(spin_until([&finished]() { return !finished.replies.isEmpty(); }));
    assert(finished.replies == (QList<QPair<int, bool>>{qMakePair(export_token(prepared), true)}));
    const QByteArray saved = read_bytes(target);
    assert(saved == expected);
    const QList<QByteArray> lines = saved.split('\n');
    assert(lines.size() == 1002 && lines.last().isEmpty());
    assert(lines.at(1).startsWith("2026-11-07T14:20:02.418Z,"));    // hit 1000
    assert(lines.at(1000).startsWith("2026-11-07T14:03:23.418Z,")); // hit 1
    assert(lines.at(1).contains("Heavy / Ch") && !saved.contains("rtltcp") && !saved.contains("10.9.8.7"));
    dsd_app_signal_search_store_close();
}

int
main(int argc, char** argv) {
    if (argc == 3 && strcmp(argv[1], "--hold-export-lock") == 0) {
        return hold_export_lock(argc, argv);
    }
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("dsd-search-tests"));
    dsd_test_qt_isolate_paths();
    test_csv_text();
    QTemporaryDir temp;
    assert(temp.isValid());
    test_save_transport_local(temp.path());
    test_save_transport_abort_mid_save(temp.path());
    test_save_transport_host(temp.path());
    test_export_writer_saves(temp.path());
    test_export_writer_staging_failures(temp.path());
    test_export_writer_sweep(temp.path());
    test_export_writer_teardown_stops(temp.path());
    test_export_writer_teardown_blocked(temp.path());
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
    testKnownFrequencyListsApart(temp.path());
    testKnownFrequencyEngineIds(temp.path());
    testKnownFrequencyEngineGrammar(temp.path());
    testKnownFrequencySessionTargetsShared(temp.path());
    testKnownFrequencyQuotedCells(temp.path());
    testKnownFrequencyRowCap(temp.path());
    testKnownFrequencyWatchesUnparsedFiles(temp.path());
    testKnownFrequencyTargetParity(temp.path());
    testKnownFrequencyChannelParity(temp.path());
    testKnownFrequencyNonAsciiPaths(temp.path());
    testRecordingKey();
    testOverloadMapped();
    test_export_detection_rows();
    test_export_avoid_rows();
    test_export_avoid_reads();
    test_export_model_avoids(temp.path());
    test_export_model_tokens(temp.path());
    test_export_model_detections(temp.path());
    return 0;
}

// NOLINTEND(cert-dcl03-c,misc-static-assert)
