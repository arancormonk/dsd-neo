// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Unit tests: the backup screens' model (issue #580). A configuration the real models build is exported through the
 * model (built and sealed on its worker, saved into a document), inspected and staged from that document, committed,
 * and applied the way the next start applies it; fresh models then load the same rows and files. Also covered: the
 * session and engine guards, quitting once, a host that cannot quit (restorePending), discarding with both failure
 * meanings, every error a restore reports, busy refusals, cancelled exports and inspections (on the worker and after
 * it finished) confined to their own files, the startup outcome from the apply and from the result file, the sweep at
 * construction, that the passphrase is written nowhere, that idle is announced only once a job's outcome is in place,
 * that teardown stops a document copy instead of waiting for it, that a restore an earlier start left committed shows
 * as waiting (and one whose apply is part-way through cannot be discarded), and that the QML tests' stand-in carries
 * the model's exact surface. The log lines the libraries print are captured and checked, so a clean run prints only
 * "OK"; this test's own FAIL and note lines are printed as they happen. Registered only when the Qt frontend is
 * enabled (DSD_ENABLE_QT_UI). */

#include <QByteArray>
#include <QChar>
#include <QCoreApplication>
#include <QDate>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLatin1String>
#include <QList>
#include <QMap>
#include <QMetaMethod>
#include <QObject>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QUrl>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <Qt>
#include <QtGlobal>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <dsd-neo/app_control/signal_search.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/crypto/backup_seal.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/runtime/signal_search.h>
#include <functional>
#include <initializer_list> // IWYU pragma: keep -- braced range-for lists require this header.
#include <mutex>
#include <qflags.h>
#include <qsystemdetection.h>
#include <qtenvironmentvariables.h>
#include <stdint.h>
#include <stdio.h>
#include <thread>
#include <utility>
#include <vector>
#include "../test_support/qt_test_paths.h"
#include "../test_support/test_support.h"
#include "app_prefs.h"
#include "backup_apply.h"
#include "backup_model.h"
#include "decoder_host.h"
#include "decryption_profiles_model.h"
#include "imported_files_model.h"
#include "json_store.h"
#include "qml_backup_recorder.h"
#include "realtime_clock.h"
#include "saved_systems_model.h"
#include "scan_lists_model.h"
#include "signal_search_model.h"
#include "teardown_trace_capture.h"

namespace {

using dsd_qt::BackupModel;
using dsd_qt::backup::ApplyOutcome;
using dsd_qt::backup::ApplyResult;

/* Under a sanitizer stderr stays as it is: a report must reach it even when it ends the process. */
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define BACKUP_MODEL_TEST_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define BACKUP_MODEL_TEST_SANITIZED 1
#endif
#endif

constexpr char kPass[] = "correct horse battery";
constexpr char kRootMarker[] = "dsd-neo-backup-model-";
constexpr char kResultFile[] = "backup-restore-result.json";

int g_failures = 0;
/** Set once a wait times out: later waits only check, so a broken model fails in seconds, not hours. */
bool g_stalled = false;
/**
 * The real stderr while quietly() captures it, else -1. This test's own lines go straight there, so a step that
 * crashes or hangs still shows every FAIL and note it printed before.
 */
int g_realStderr = -1;

/** Print one line of this test's own (a FAIL or a note) where it is seen at once, captured or not. */
void
say(const QByteArray& line) {
    if (g_realStderr >= 0) {
        const QByteArray text = line + '\n';
        (void)dsd_write(g_realStderr, text.constData(), static_cast<size_t>(text.size()));
    } else {
        DSD_FPRINTF(stderr, "%s\n", line.constData());
    }
}

void
expect(const char* what, bool ok) {
    if (!ok) {
        say(QByteArray("FAIL: ") + what);
        g_failures++;
    }
}

void
expect(const QString& what, bool ok) {
    expect(qUtf8Printable(what), ok);
}

/**
 * A host whose session state, engine check and quit the test sets, with the base document copies. The copy from a
 * document can be held at a gate so a job waits on the worker until the test lets it go.
 */
class FakeHost : public dsd_qt::DecoderHost {
  public:
    int quitCalls = 0;
    bool busyEngine = false;
    /** When set, copyDocumentToFile() answers it (after the gate) without copying anything. */
    int forcedCopy = -1;

    bool
    isRunning() const override {
        return m_state == Running;
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
    stop() override {}

    SessionState
    sessionState() const override {
        return m_state;
    }

    void
    setState(SessionState state) {
        m_state = state;
        Q_EMIT sessionStateChanged();
    }

    bool
    quitApp() override {
        ++quitCalls;
        return true;
    }

    bool
    engineBusy() const override {
        return busyEngine;
    }

    int
    copyDocumentToFile(const QString& reference, const QString& destPath, qint64 maxBytes) const override {
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            const int generation = m_aborts;
            ++m_arrived;
            m_changed.notify_all();
            m_changed.wait(lock, [this, generation]() { return m_open || m_aborts != generation; });
            if (m_aborts != generation) {
                return DocumentFailed;
            }
        }
        if (forcedCopy >= 0) {
            return forcedCopy;
        }
        return DecoderHost::copyDocumentToFile(reference, destPath, maxBytes);
    }

    /**
     * Fails the copies held at the gate now. Not the base: one FakeHost serves every test, and the base host's abort
     * lasts for the process (the base host's own abort is tested on hosts of its own).
     */
    void
    abortDocumentTransfers() override {
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            ++m_aborts;
        }
        m_changed.notify_all();
    }

    int
    aborts() const {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_aborts;
    }

    void
    closeGate() {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_open = false;
        m_arrived = 0;
    }

    void
    openGate() {
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_open = true;
        }
        m_changed.notify_all();
    }

    /** Wait until @p count copies have reached the gate. */
    bool
    waitArrived(int count) const {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_changed.wait_for(lock, std::chrono::seconds(g_stalled ? 0 : 20),
                                  [this, count]() { return m_arrived >= count; });
    }

  private:
    SessionState m_state = Idle;
    mutable std::mutex m_mutex;
    mutable std::condition_variable m_changed;
    bool m_open = true;
    mutable int m_arrived = 0;
    int m_aborts = 0;
};

/** A host with the base quitApp(), which cannot close the app. */
class StayingHost : public dsd_qt::DecoderHost {
  public:
    bool
    isRunning() const override {
        return false;
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
    stop() override {}
};

/** Every result the model reports. */
struct Results {
    QList<QVariantMap> prepared, finished, inspected;
    int busyChanges = 0, allowedChanges = 0, pendingChanges = 0, cancellableChanges = 0, outcomeChanges = 0,
        flowChanges = 0;

    explicit Results(BackupModel& model) {
        QObject::connect(&model, &BackupModel::exportPrepared, [this](const QVariantMap& r) { prepared.append(r); });
        QObject::connect(&model, &BackupModel::exportFinished, [this](const QVariantMap& r) { finished.append(r); });
        QObject::connect(&model, &BackupModel::inspectFinished, [this](const QVariantMap& r) { inspected.append(r); });
        QObject::connect(&model, &BackupModel::busyChanged, [this]() { ++busyChanges; });
        QObject::connect(&model, &BackupModel::restoreAllowedChanged, [this]() { ++allowedChanges; });
        QObject::connect(&model, &BackupModel::restorePendingChanged, [this]() { ++pendingChanges; });
        QObject::connect(&model, &BackupModel::restoreCancellableChanged, [this]() { ++cancellableChanges; });
        QObject::connect(&model, &BackupModel::startupOutcomeChanged, [this]() { ++outcomeChanges; });
        QObject::connect(&model, &BackupModel::flowActiveChanged, [this]() { ++flowChanges; });
    }

    int
    total() const {
        return static_cast<int>(prepared.size() + finished.size() + inspected.size());
    }
};

/** Run the event loop until @p done or a generous deadline. */
bool
pump_until(const std::function<bool()>& done) {
    QElapsedTimer timer;
    timer.start();
    while (!g_stalled && !done() && timer.elapsed() < 60000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    QCoreApplication::processEvents();
    if (!done() && !g_stalled) {
        g_stalled = true;
        expect("a wait timed out; later waits only check", false);
    }
    return done();
}

/** Let every queued job finish on the worker, then deliver what it posted. */
void
settle(BackupModel& model) {
    expect("the worker finishes its jobs", model.waitForJobs(60000));
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents();
}

#ifndef BACKUP_MODEL_TEST_SANITIZED
/** "Backup restore applied at start" lines seen, one per apply. */
int g_appliedLines = 0;

/**
 * Whether @p line is one the libraries print while this test runs: a key file loaded, a restore applied, a staging
 * refused for want of a directory (test_errors() makes the data root read-only).
 */
bool
expected_log_line(const QByteArray& line) {
    static const QRegularExpression loaded(
        QStringLiteral("^NOTICE: Loaded \\d+ of \\d+ entries \\(hex file\\) from '.*'\\.$"));
    if (line == "Backup restore applied at start") {
        ++g_appliedLines;
        return true;
    }
    if (line == "Backup restore could not be staged: no staging directory") {
        return true;
    }
    return loaded.match(QString::fromUtf8(line)).hasMatch();
}
#endif

/**
 * Run @p body with stderr captured. The log lines it expects are dropped and any other line fails the test; this
 * test's own lines (FAIL, note) bypass the capture (say()), so none waits on the step finishing. Where the platform
 * allows, the capture file is unlinked while still open, so a step that crashes leaves none behind.
 */
void
quietly(const std::function<void()>& body) {
#ifdef BACKUP_MODEL_TEST_SANITIZED
    body();
#else
    dsd_test_capture_stderr capture;
    if (dsd_test_capture_stderr_begin(&capture, "backup_model_log") != 0) {
        expect("stderr is captured", false);
        body();
        return;
    }
    const QString path = QString::fromUtf8(capture.path);
    QFile log(path);
    const bool readable = log.open(QIODevice::ReadOnly);
#ifdef Q_OS_UNIX
    QFile::remove(path); // the open descriptors keep it readable
#endif
    g_realStderr = capture.saved_fd;
    body();
    g_realStderr = -1;
    (void)dsd_test_capture_stderr_end(&capture);
    const QByteArray text = readable ? log.readAll() : QByteArray();
    log.close();
    QFile::remove(path);
    expect("the captured log is read", readable);
    for (const QByteArray& line : text.split('\n')) {
        if (line.isEmpty() || expected_log_line(line)) {
            continue;
        }
        if (line.startsWith("FAIL: ") || line.startsWith("note: ")) {
            say(line);
        } else {
            say("FAIL: unexpected output: " + line);
            g_failures++;
        }
    }
#endif
}

dsd_backup_kdf_params
fast_kdf() {
    dsd_backup_kdf_params kdf{};
    kdf.log2_n = 14;
    kdf.r = 8;
    kdf.p = 1;
    return kdf;
}

QString
example(const char* name) {
    return QStringLiteral(DSD_NEO_TEST_EXAMPLES_DIR "/") + QLatin1String(name);
}

QString
root() {
    return dsd_qt::json_store_root();
}

QString
at(const QString& relative) {
    return root() + QLatin1Char('/') + relative;
}

QString
cache_dir() {
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/backup");
}

bool
write_file(const QString& path, const QByteArray& contents) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size();
}

QByteArray
read_file(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

bool
guarded(const QString& path, bool strictlyBelow) {
    const QString base = root();
    const bool baseOk = base.size() > 1 && QDir::isAbsolutePath(base) && QDir::cleanPath(base) == base
                        && base.contains(QLatin1String(kRootMarker));
    const bool pathOk = !path.isEmpty() && QDir::isAbsolutePath(path) && QDir::cleanPath(path) == path
                        && ((!strictlyBelow && path == base) || path.startsWith(base + QLatin1Char('/')));
    if (!(baseOk && pathOk)) {
        expect(QStringLiteral("only the isolated data root is removed: '%1'").arg(path), false);
    }
    return baseOk && pathOk;
}

/** The one way this test removes a directory tree: a clean absolute path equal to or below the isolated data root. */
void
remove_tree(const QString& path) {
    if (guarded(path, false)) {
        QDir(path).removeRecursively();
    }
}

/** Entries of the export cache folder. */
QStringList
cache_entries() {
    return QDir(cache_dir()).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
}

/** Staging directories left in the data root. */
QStringList
staging_dirs() {
    return QDir(root()).entryList({QStringLiteral("restore-staging-*")},
                                  QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
}

/* ---- the configuration ---- */

QString
import_path(dsd_qt::ImportedFilesModel& library, const QString& source, const QString& name, const QString& type) {
    const QVariantMap result = library.importFile(source, name, type);
    expect("fixture import succeeds", result.value("ok").toBool());
    return result.value("path").toString();
}

struct Library {
    QString groups, keys, chan, targets;
};

Library
import_library(FakeHost& host) {
    Library lib;
    dsd_qt::ImportedFilesModel library(&host);
    lib.groups = import_path(library, example("group.csv"), "group.csv", "group");
    lib.keys = import_path(library, example("multi_key_hex.csv"), "keys.csv", "keysHex");
    QTemporaryDir source;
    expect("write channel bundle fixture",
           write_file(source.filePath("keys.csv"), "keyid,value\n02,ABCDE\n")
               && write_file(source.filePath("map.csv"), "tg_dec,keyid_hex\n123,02\n")
               && write_file(source.filePath("channels.csv"),
                             "channel,frequency_hz,mode,keys_hex_csv,options\n1,461000000,dmr,keys.csv,"
                             "--dmr-tg-key-csv 'map.csv'\n"));
    QVariantMap result = library.importBundle(source.filePath("channels.csv"), "Bundle.csv", "chan", {});
    expect("channel bundle imports", result.value("ok").toBool());
    lib.chan = result.value("path").toString();
    expect("write target bundle fixture",
           write_file(source.filePath("tkeys.csv"), "id,value\n01,ABCDEF0123\n")
               && write_file(source.filePath("tmap.csv"),
                             "channel,frequency,mode,keys_hex_csv\n1,851012500,p25,tkeys.csv\n")
               && write_file(source.filePath("targets.csv"),
                             "id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes\n"
                             "one,p25-trunk,851012500,tmap.csv,250,,\n"));
    result = library.importFile(source.filePath("targets.csv"), "Targets.csv", "trunkTargets");
    expect("target bundle imports", result.value("ok").toBool());
    lib.targets = result.value("path").toString();
    return lib;
}

/** A profile whose keys the model compiles into decryption/<uid>/. */
QVariantMap
managed_profile(const QString& uid) {
    const QVariantMap key{{"uid", uid + "-key"},
                          {"label", "Dispatch"},
                          {"keyId", 17},
                          {"kind", "aes128"},
                          {"material", "00112233445566778899aabbccddeeff"}};
    return {{"uid", uid}, {"label", uid}, {"protocol", "p25"}, {"mode", "automatic"}, {"keys", QVariantList{key}}};
}

/** The configuration a backup is made of, built by the real models in an empty data root. */
void
build_configuration(FakeHost& host) {
    const Library lib = import_library(host);
    dsd_qt::SavedSystemsModel systems;
    expect("add trunked system", systems.add({{"name", "Metro"},
                                              {"sourceType", "usb"},
                                              {"freqMhz", "851.0125"},
                                              {"decodeFlag", "-ft"},
                                              {"trunking", true},
                                              {"chanCsvPath", lib.chan},
                                              {"groupCsvPath", lib.groups},
                                              {"keyCsvPath", lib.keys},
                                              {"keyCsvHex", true}}));
    expect("add replay system",
           systems.add({{"name", "Replay"}, {"sourceType", "file"}, {"filePath", at("recordings/capture.wav")}}));
    const QString systemUid = systems.get(0).value("uid").toString();
    dsd_qt::ScanListsModel scans;
    expect("add scan list",
           scans.add({{"name", "Patrol"},
                      {"groupCsvPath", lib.groups},
                      {"entries", QVariantList{QVariantMap{{"kind", "system"}, {"systemUid", systemUid}}}}}));
    expect("add target scan list",
           scans.add({{"name", "Targets"}, {"targetSource", "csv"}, {"targetsCsvPath", lib.targets}}));
    dsd_qt::DecryptionProfilesModel profiles;
    profiles.setReferences(&systems, &scans);
    expect("save CSV profile", profiles
                                   .saveProfile({{"uid", "csv-keys"},
                                                 {"label", "CSV keys"},
                                                 {"protocol", "p25"},
                                                 {"mode", "automatic"},
                                                 {"keySource", "csv"},
                                                 {"legacyCsvPath", lib.keys},
                                                 {"legacyCsvHex", true}})
                                   .value("ok")
                                   .toBool());
    expect("save managed-key profile", profiles.saveProfile(managed_profile("managed")).value("ok").toBool());
    {
        dsd_qt::SignalSearchModel search(&host, nullptr);
        QVariantMap range = search.defaultRange();
        range.insert("name", "Marine");
        expect("save search range", search.saveRange(-1, range).isEmpty() && search.ranges().size() == 2);
        expect("avoid 156800000", dsd_app_signal_search_permanent_avoid(156800000U) >= DSD_APP_SEARCH_AVOID_ADDED);
        expect("avoid 162550000", dsd_app_signal_search_permanent_avoid(162550000U) >= DSD_APP_SEARCH_AVOID_ADDED);
    }
    expect("write settings", dsd_qt::AppPrefs::replacePortable(
                                 {{"decode/extraArgs", "-Z"}, {"rr/username", "scanner"}, {"tuner/gainDb", "30"}}));
}

/** What fresh models load from the data root, plus the files, the settings and the avoid list. */
struct Captured {
    QVariantList systems, scans, library, profiles, ranges;
    QMap<QString, QByteArray> files;
    QVariantMap prefs;
    std::vector<uint32_t> avoids;
};

Captured
capture(FakeHost& host) {
    Captured c;
    dsd_qt::ImportedFilesModel library(&host);
    for (int i = 0; i < library.rowCount(); ++i) {
        c.library.append(library.get(i));
    }
    dsd_qt::SavedSystemsModel systems;
    for (int i = 0; i < systems.rowCount(); ++i) {
        c.systems.append(systems.get(i));
    }
    dsd_qt::ScanListsModel scans;
    for (int i = 0; i < scans.rowCount(); ++i) {
        c.scans.append(scans.get(i));
    }
    dsd_qt::DecryptionProfilesModel profiles;
    for (const QVariant& entry : profiles.entries()) {
        QVariantMap row = entry.toMap();
        row.remove("runtimeRef"); // issued again at every load of a stored row without one
        c.profiles.append(row);
    }
    dsd_qt::SignalSearchModel search(&host, nullptr);
    c.ranges = search.ranges();
    c.avoids.resize(DSD_SEARCH_MAX_AVOIDS);
    c.avoids.resize(dsd_app_signal_search_avoids_get(c.avoids.data(), c.avoids.size()));
    QDirIterator it(at("imports"), QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        c.files.insert(path, read_file(path));
    }
    c.prefs = dsd_qt::AppPrefs::readPortable();
    return c;
}

/** Everything a restore replaces is gone, as on a phone the app was just installed on. */
void
wipe_configuration() {
    dsd_app_signal_search_store_close();
    remove_tree(root());
    QDir().mkpath(root());
    expect("clear settings", dsd_qt::AppPrefs::replacePortable({}));
}

/* ---- export ---- */

/** Export the current configuration into @p document through the model; the exportPrepared result. */
QVariantMap
export_to(FakeHost& host, const QString& document) {
    BackupModel model(&host, {});
    model.setKdfParams(fast_kdf());
    Results results(model);
    model.prepareExport(kPass);
    expect("export prepares", pump_until([&]() { return results.prepared.size() == 1; }));
    const QVariantMap prepared = results.prepared.value(0);
    model.saveExport(QUrl::fromLocalFile(document).toString());
    expect("export saves", pump_until([&]() { return results.finished.size() == 1; })
                               && results.finished.value(0).value("ok").toBool());
    return prepared;
}

void
test_export(FakeHost& host, const QString& document) {
    BackupModel model(&host, {});
    model.setKdfParams(fast_kdf());
    Results results(model);
    expect("nothing is busy at first", !model.busy() && model.restoreAllowed() && !model.restorePending());
    expect("the minimum passphrase length", model.minPassphraseLength() == 8);
    expect("no last result at first", model.lastResult().isEmpty());
    model.prepareExport(kPass);
    expect("busy while preparing", model.busy() && !model.restoreAllowed() && results.busyChanges == 1);
    expect("nothing is reported synchronously", results.total() == 0);
    expect("the export is prepared", pump_until([&]() { return results.prepared.size() == 1; }));
    const QVariantMap prepared = results.prepared.value(0);
    const QVariantMap summary = prepared.value("summary").toMap();
    expect("the export succeeded", prepared.value("ok").toBool() && prepared.value("error").toString().isEmpty());
    expect("the export counts the configuration",
           summary.value("systems").toInt() == 2 && summary.value("scanLists").toInt() == 2
               && summary.value("libraryFiles").toInt() == 4 && summary.value("profiles").toInt() == 2
               && summary.value("ranges").toInt() == 2 && summary.value("avoids").toInt() == 2
               && summary.value("recordingsToChoose").toInt() == 1 && summary.value("appExtraArgs").toBool());
    expect("the export lists what it left out",
           prepared.value("notIncluded").toStringList() == summary.value("notIncluded").toStringList());
    expect("idle once prepared", !model.busy() && model.restoreAllowed());
    expect("a prepared export is no card result", model.lastResult().isEmpty());
    const QStringList temp = cache_entries();
    expect("one sealed file waits in the cache",
           temp.size() == 1 && temp.value(0).endsWith(".dsdbak")
               && read_file(cache_dir() + "/" + temp.value(0)).startsWith("DSDNEOBK"));
    expect("the sealed file is private",
           (QFile::permissions(cache_dir() + "/" + temp.value(0)) & (QFileDevice::ReadGroup | QFileDevice::ReadOther))
               == 0);

    model.saveExport(QUrl::fromLocalFile(document).toString());
    expect("busy while saving", model.busy());
    model.cancel();
    expect("saving is not cancellable", model.busy());
    expect("the export is saved", pump_until([&]() { return results.finished.size() == 1; }));
    const QVariantMap finished = results.finished.value(0);
    expect("the save succeeded", finished.value("ok").toBool() && finished.value("error").toString().isEmpty());
    expect("the save carries the export's summary and omissions",
           finished.value("summary").toMap().value("systems").toInt() == 2
               && finished.value("notIncluded") == prepared.value("notIncluded"));
    expect("the document holds the sealed backup", read_file(document).startsWith("DSDNEOBK"));
    expect("the cache file is gone", cache_entries().isEmpty());
    expect("the card shows the save",
           model.lastResult().value("action") == "saveExport" && model.lastResult().value("ok").toBool());
    model.saveExport(QUrl::fromLocalFile(document).toString());
    expect("nothing is left to save", pump_until([&]() { return results.finished.size() == 2; })
                                          && !results.finished.value(1).value("ok").toBool()
                                          && results.finished.value(1).value("error") == "Io");
}

/* ---- restore ---- */

QVariantMap
inspect(BackupModel& model, Results& results, const QString& document, const QString& passphrase = kPass) {
    const int before = static_cast<int>(results.inspected.size());
    model.inspectRestore(document, passphrase);
    expect("the inspection reports", pump_until([&]() { return results.inspected.size() == before + 1; }));
    return results.inspected.value(before);
}

void
test_round_trip(FakeHost& host, const QString& document, const Captured& before) {
    wipe_configuration();
    {
        BackupModel model(&host, {});
        Results results(model);
        const QVariantMap inspected = inspect(model, results, document);
        const QVariantMap summary = inspected.value("summary").toMap();
        expect("the backup inspects", inspected.value("ok").toBool() && inspected.value("error").toString().isEmpty());
        expect("the inspection counts the backup",
               summary.value("systems").toInt() == 2 && summary.value("scanLists").toInt() == 2
                   && summary.value("libraryFiles").toInt() == 4 && summary.value("profiles").toInt() == 2
                   && summary.value("ranges").toInt() == 2 && summary.value("recordingsToChoose").toInt() == 1
                   && summary.value("appExtraArgs").toBool() && summary.value("createdAt").toLongLong() > 0
                   && !summary.value("app").toString().isEmpty() && summary.contains("systemsWithExtraArgs")
                   && summary.contains("notIncluded"));
        expect("the card shows the inspection", model.lastResult().value("action") == "inspectRestore");
        expect("the backup waits unconfirmed",
               dsd_qt::backup::pending_exists() && !dsd_qt::backup::pending_committed());
        expect("no staging directory or cache file is left", staging_dirs().isEmpty() && cache_entries().isEmpty());
        expect("nothing live changed before the commit", !QFileInfo::exists(at("saved_systems.json")));
        const QVariantMap commit = model.commitRestore();
        expect("the commit closes the app", commit.value("ok").toBool() && commit.value("quitting").toBool()
                                                && commit.value("error").toString().isEmpty());
        expect("quitApp is called once", host.quitCalls == 1);
        expect("the restore is confirmed and waits", dsd_qt::backup::pending_committed() && model.restorePending());
        expect("a confirmed restore can be cancelled until it applies",
               model.restoreCancellable() && results.cancellableChanges == 1);
    }
    // The next start.
    const ApplyResult applied = dsd_qt::backup::apply_pending_restore({});
    expect("the restore applies at the next start", applied.outcome == ApplyOutcome::Applied);
    const Captured after = capture(host);
    expect("library rows are identical", after.library == before.library);
    QVariantList systems = before.systems;
    QVariantMap replay = systems.value(1).toMap();
    replay.insert("filePath", QString()); // a backup never carries recordings
    systems.replace(1, replay);
    expect("saved systems are identical but the recording", after.systems == systems);
    expect("scan lists are identical", after.scans == before.scans);
    expect("profiles are identical", after.profiles == before.profiles);
    expect("profiles are compiled again", QFileInfo(at("decryption/managed")).isDir());
    expect("search ranges are identical", after.ranges == before.ranges);
    expect("the avoid list is identical", after.avoids == before.avoids);
    expect("files are byte-identical", after.files == before.files && after.files.size() >= 4);
    expect("settings are identical", after.prefs == before.prefs);
    dsd_app_signal_search_store_close();

    // The outcome card, from the apply in this process and from the result file at a later start.
    {
        BackupModel model(&host, applied);
        const QVariantMap outcome = model.startupOutcome();
        expect("the outcome of this start's apply", outcome.value("kind") == "applied"
                                                        && outcome.value("systems").toInt() == 2
                                                        && outcome.value("recordingsToChoose").toInt() == 1
                                                        && outcome.value("message").toString().isEmpty());
    }
    {
        BackupModel model(&host, {});
        Results results(model);
        expect("the outcome read from the result file",
               model.startupOutcome().value("kind") == "applied"
                   && model.startupOutcome().value("libraryFiles").toInt() == 4);
        model.acknowledgeStartupOutcome();
        expect("acknowledging clears the outcome and the file",
               model.startupOutcome().value("kind").toString().isEmpty() && results.outcomeChanges == 1
                   && !QFileInfo::exists(at(kResultFile)));
    }
    {
        BackupModel model(&host, {});
        expect("an acknowledged outcome stays gone",
               model.startupOutcome().value("kind").toString().isEmpty() && model.startupOutcome().contains("kind"));
    }
}

void
test_session_guards(FakeHost& host, const QString& document) {
    BackupModel model(&host, {});
    model.setKdfParams(fast_kdf());
    Results results(model);

    host.setState(dsd_qt::DecoderHost::Running);
    expect("no restore while listening", !model.restoreAllowed() && results.allowedChanges == 1);
    QVariantMap inspected = inspect(model, results, document);
    expect("inspecting is refused while a session runs",
           !inspected.value("ok").toBool() && inspected.value("error") == "SessionActive");
    expect("a refused inspection stages nothing",
           !dsd_qt::backup::pending_exists() && staging_dirs().isEmpty() && !model.busy());

    model.prepareExport(kPass);
    expect("exporting works during a session", pump_until([&]() { return results.prepared.size() == 1; })
                                                   && results.prepared.value(0).value("ok").toBool());
    model.discardExport();
    expect("a discarded export leaves no file", cache_entries().isEmpty());

    host.setState(dsd_qt::DecoderHost::Idle);
    expect("restore allowed once idle", model.restoreAllowed() && results.allowedChanges == 2);
    inspected = inspect(model, results, document);
    expect("the backup inspects when idle", inspected.value("ok").toBool());

    const int quits = host.quitCalls;
    // A refused commit keeps the staged restore, unconfirmed, for the screen to retry or the user to cancel.
    const auto kept = [&model]() {
        return dsd_qt::backup::pending_exists() && !dsd_qt::backup::pending_committed() && !model.restorePending();
    };
    host.setState(dsd_qt::DecoderHost::Starting);
    QVariantMap commit = model.commitRestore();
    expect("commit refused once a session starts", !commit.value("ok").toBool() && !commit.value("quitting").toBool()
                                                       && commit.value("error") == "SessionActive" && kept());
    host.setState(dsd_qt::DecoderHost::Stopping);
    commit = model.commitRestore();
    expect("commit refused while a session is still stopping",
           !commit.value("ok").toBool() && commit.value("error") == "SessionActive" && kept());
    host.setState(dsd_qt::DecoderHost::Idle);
    host.busyEngine = true;
    commit = model.commitRestore();
    expect("commit refused while the engine is busy",
           !commit.value("ok").toBool() && commit.value("error") == "SessionActive" && kept());
    expect("no quit for a refused commit", host.quitCalls == quits);
    host.busyEngine = false;
    commit = model.commitRestore();
    expect("commit succeeds when idle",
           commit.value("ok").toBool() && commit.value("quitting").toBool() && dsd_qt::backup::pending_committed());
    commit = model.commitRestore();
    expect("quitApp is asked once",
           commit.value("ok").toBool() && commit.value("quitting").toBool() && host.quitCalls == quits + 1);
    expect("clean up the committed restore", dsd_qt::backup::discard_pending());
}

void
test_staying_host(const QString& document) {
    StayingHost host;
    BackupModel model(&host, {});
    Results results(model);
    expect("the backup inspects", inspect(model, results, document).value("ok").toBool());
    const QVariantMap commit = model.commitRestore();
    expect("a host that cannot quit keeps the app open", commit.value("ok").toBool()
                                                             && !commit.value("quitting").toBool()
                                                             && commit.value("error").toString().isEmpty());
    expect("the restore waits for a reopen",
           model.restorePending() && results.pendingChanges == 1 && dsd_qt::backup::pending_committed());

    model.inspectRestore(document, kPass);
    expect("a waiting restore starts no inspection", !model.busy());
    const QVariantMap again =
        pump_until([&]() { return results.inspected.size() == 2; }) ? results.inspected.value(1) : QVariantMap();
    expect("a waiting restore refuses another inspection",
           !again.value("ok").toBool() && again.value("error") == "RestorePending" && staging_dirs().isEmpty()
               && dsd_qt::backup::pending_committed());

    const QVariantMap discarded = model.discardRestore();
    expect("discarding removes it", discarded.value("ok").toBool() && discarded.value("discarded").toBool()
                                        && discarded.value("error").toString().isEmpty());
    expect("nothing waits any more",
           !model.restorePending() && results.pendingChanges == 2 && !dsd_qt::backup::pending_exists());
}

/** Whether this process is refused writes into a read-only directory (not, as root). */
bool
permissions_enforced(const QString& dir) {
    QFile::setPermissions(dir, QFileDevice::ReadOwner | QFileDevice::ExeOwner);
    QFile probe(dir + "/probe");
    const bool refused = !probe.open(QIODevice::WriteOnly);
    probe.close();
    QFile::remove(dir + "/probe");
    QFile::setPermissions(dir, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    return refused;
}

void
test_discard_failures(const QString& document) {
    const QFileDevice::Permissions open = QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner;
    const QFileDevice::Permissions readOnly = QFileDevice::ReadOwner | QFileDevice::ExeOwner;
    StayingHost host;
    BackupModel model(&host, {});
    Results results(model);
    expect("the backup inspects", inspect(model, results, document).value("ok").toBool());
    expect("commit", model.commitRestore().value("ok").toBool() && model.restorePending());
    const QString pending = at("restore-pending");
    if (!permissions_enforced(pending)) {
        say("note: directory permissions are not enforced; discard failures skipped");
        expect("clean up", dsd_qt::backup::discard_pending());
        return;
    }

    QFile::setPermissions(pending, readOnly);
    QVariantMap discarded = model.discardRestore();
    QFile::setPermissions(pending, open);
    expect("a marker that stays: nothing was removed and the restore still applies",
           !discarded.value("ok").toBool() && !discarded.value("discarded").toBool() && discarded.value("error") == "Io"
               && model.restorePending() && dsd_qt::backup::pending_committed());

    QFile::setPermissions(pending + "/files", readOnly);
    discarded = model.discardRestore();
    QFile::setPermissions(pending + "/files", open);
    expect("files that stay: the restore is discarded but not fully removed",
           !discarded.value("ok").toBool() && discarded.value("discarded").toBool() && discarded.value("error") == "Io"
               && !model.restorePending() && dsd_qt::backup::pending_exists() && !dsd_qt::backup::pending_committed());
    expect("clean up", dsd_qt::backup::discard_pending() && !dsd_qt::backup::pending_exists());
}

/** @p sealed with the header byte at @p offset set to @p value. */
QByteArray
with_header_byte(QByteArray sealed, int offset, char value) {
    expect("the sealed backup is long enough to edit", offset < sealed.size());
    if (offset < sealed.size()) {
        sealed[offset] = value;
    }
    return sealed;
}

void
test_errors(FakeHost& host, const QString& document, const QString& scratch) {
    BackupModel model(&host, {});
    model.setKdfParams(fast_kdf());
    Results results(model);
    const QByteArray sealed = read_file(document);

    struct Case {
        const char* name;
        QString document;
        QString passphrase;
        const char* error;
    };

    const QString bad = scratch + "/bad.dsdbak";
    const QString large = scratch + "/large.dsdbak";
    {
        QFile file(large);
        expect("write an oversized document",
               file.open(QIODevice::WriteOnly) && file.resize(dsd_qt::backup::kMaxSealedBytes + 1));
    }
    const QList<Case> cases{
        {"wrong passphrase", document, "a wrong passphrase", "WrongPassphrase"},
        {"too short to be the passphrase", document, "short", "WrongPassphrase"},
        {"empty passphrase", document, "", "WrongPassphrase"},
        {"not a backup", example("group.csv"), kPass, "NotBackup"},
        {"oversized document", large, kPass, "TooLarge"},
        {"missing document", scratch + "/missing.dsdbak", kPass, "Io"},
        {"newer format", bad, kPass, "NewerVersion"},
        {"unusable key parameters", bad, kPass, "Damaged"},
        {"tampered ciphertext", bad, kPass, "WrongPassphrase"},
    };
    for (const Case& c : cases) {
        if (QString(c.name) == "newer format") {
            expect("write a newer format", write_file(bad, with_header_byte(sealed, 9, 2)));
        } else if (QString(c.name) == "unusable key parameters") {
            expect("write bad key parameters", write_file(bad, with_header_byte(sealed, 11, 30)));
        } else if (QString(c.name) == "tampered ciphertext") {
            expect(
                "write tampered ciphertext",
                write_file(bad, with_header_byte(sealed, 70,
                                                 static_cast<char>((sealed.size() > 70 ? sealed.at(70) : 0) ^ 0x01))));
        }
        const QVariantMap result = inspect(model, results, c.document, c.passphrase);
        expect(QStringLiteral("%1 → %2 (got %3)").arg(c.name, c.error, result.value("error").toString()),
               !result.value("ok").toBool() && result.value("error") == c.error);
        expect(QStringLiteral("%1 leaves nothing").arg(c.name), !model.busy() && !dsd_qt::backup::pending_exists()
                                                                    && staging_dirs().isEmpty()
                                                                    && cache_entries().isEmpty());
    }

    // This phone, not the document: no place to copy it to, or to stage it in, a copy it could not write or read
    // back, is StagingFailed, never the document read failure (Io) the screen blames the file for.
    const struct {
        int copy;
        const char* error;
        const char* what;
    } copies[] = {
        {dsd_qt::DecoderHost::DocumentLocalFailed, "StagingFailed", "a copy this phone could not write"},
        {dsd_qt::DecoderHost::DocumentCopied, "StagingFailed", "a copy reported but not readable back"},
        {dsd_qt::DecoderHost::DocumentFailed, "Io", "a document that could not be read"},
    };

    for (const auto& c : copies) {
        host.forcedCopy = c.copy;
        const QVariantMap copied = inspect(model, results, document);
        host.forcedCopy = -1;
        expect(QStringLiteral("%1 → %2 (got %3)").arg(c.what, c.error, copied.value("error").toString()),
               !copied.value("ok").toBool() && copied.value("error") == c.error);
    }
    const QString cache = cache_dir();
    expect("the cache folder is the isolated one", cache.contains(QLatin1String(kRootMarker)));
    if (cache.contains(QLatin1String(kRootMarker))) {
        expect("a file where the cache folder goes", QDir(cache).removeRecursively() && write_file(cache, "x"));
        QVariantMap blocked = inspect(model, results, document);
        expect("no cache folder → StagingFailed",
               !blocked.value("ok").toBool() && blocked.value("error") == "StagingFailed");
        expect("remove the blocking file", QFile::remove(cache) && QDir().mkpath(cache));
    }
    const QString dataRoot = root();
    QFile::setPermissions(dataRoot, QFileDevice::ReadOwner | QFileDevice::ExeOwner);
    QFile probe(dataRoot + "/probe");
    if (probe.open(QIODevice::WriteOnly)) {
        probe.close();
        QFile::remove(probe.fileName());
        say("note: directory permissions are not enforced; staging failure case skipped");
    } else {
        const QVariantMap unstaged = inspect(model, results, document);
        expect("a data root staging cannot write to → StagingFailed",
               !unstaged.value("ok").toBool() && unstaged.value("error") == "StagingFailed");
    }
    QFile::setPermissions(dataRoot, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    expect("a staging failure leaves nothing", staging_dirs().isEmpty() && !dsd_qt::backup::pending_exists());

    model.setFreeBytes([](const QString&) { return qint64{1024}; });
    QVariantMap result = inspect(model, results, document);
    expect("short storage → NoSpace", !result.value("ok").toBool() && result.value("error") == "NoSpace"
                                          && staging_dirs().isEmpty() && !dsd_qt::backup::pending_exists());
    model.setFreeBytes([](const QString&) { return qint64{-1}; });
    result = inspect(model, results, document);
    expect("unknown free space is no shortage", result.value("ok").toBool());
    expect("the card shows the last inspection",
           model.lastResult().value("action") == "inspectRestore" && model.lastResult().value("ok").toBool());
    expect("clean up", model.discardRestore().value("ok").toBool());

    model.prepareExport("seven77");
    expect("a short export passphrase is refused",
           pump_until([&]() { return results.prepared.size() == 1; })
               && results.prepared.value(0).value("error") == "PassphraseTooShort" && cache_entries().isEmpty());
    expect("the card shows the refusal", model.lastResult().value("action") == "prepareExport"
                                             && model.lastResult().value("error") == "PassphraseTooShort");
}

void
test_busy_refusals(FakeHost& host, const QString& document) {
    BackupModel model(&host, {});
    model.setKdfParams(fast_kdf());
    Results results(model);
    host.closeGate();
    model.inspectRestore(document, kPass);
    expect("the inspection holds at the copy", host.waitArrived(1));
    expect("busy: no restore allowed", model.busy() && !model.restoreAllowed());
    model.prepareExport(kPass);
    model.saveExport(QStringLiteral("/nonexistent"));
    model.inspectRestore(document, kPass);
    const QVariantMap commit = model.commitRestore();
    expect("commit refused while busy", !commit.value("ok").toBool() && commit.value("error") == "Busy");
    expect("refusals arrive after the call returns", results.total() == 0);
    expect("refusals are queued", pump_until([&]() { return results.total() == 3; }));
    expect("each refusal says Busy", results.prepared.value(0).value("error") == "Busy"
                                         && results.finished.value(0).value("error") == "Busy"
                                         && results.inspected.value(0).value("error") == "Busy");
    expect("the running job is untouched", model.busy());
    host.openGate();
    expect("the held inspection finishes", pump_until([&]() { return results.inspected.size() == 2; })
                                               && results.inspected.value(1).value("ok").toBool());
    expect("clean up", model.discardRestore().value("ok").toBool());
}

void
test_cancelled_export(FakeHost& host, const QString& document) {
    {
        BackupModel model(&host, {});
        model.setKdfParams(fast_kdf());
        Results results(model);
        model.prepareExport(kPass);
        expect("the export finishes on the worker", model.waitForJobs(60000));
        expect("its file exists before its result arrives", cache_entries().size() == 1);
        model.cancel();
        expect("cancelling frees the model", !model.busy() && model.restoreAllowed());
        settle(model);
        expect("a cancelled export's late result is dropped", results.total() == 0);
        expect("a cancelled export leaves no file", cache_entries().isEmpty());
    }
    {
        BackupModel model(&host, {});
        model.setKdfParams(fast_kdf());
        Results results(model);
        host.closeGate();
        model.inspectRestore(document, kPass);
        expect("the inspection holds at the copy", host.waitArrived(1));
        model.cancel();
        model.prepareExport(kPass); // queued behind the held inspection
        model.cancel();
        host.openGate();
        settle(model);
        expect("cancelled jobs report nothing", results.total() == 0);
        expect("cancelled jobs leave nothing",
               cache_entries().isEmpty() && staging_dirs().isEmpty() && !dsd_qt::backup::pending_exists());
    }
    {
        BackupModel model(&host, {});
        model.setKdfParams(fast_kdf());
        Results results(model);
        model.prepareExport(kPass);
        model.cancel();
        settle(model);
        expect("an export cancelled at once reports nothing and leaves nothing",
               results.total() == 0 && cache_entries().isEmpty());
        model.prepareExport(kPass);
        expect("an export after a cancelled one", pump_until([&]() { return results.prepared.size() == 1; })
                                                      && results.prepared.value(0).value("ok").toBool());
        model.prepareExport(kPass);
        expect("a second export", pump_until([&]() { return results.prepared.size() == 2; }));
        expect("a new export replaces the unsaved one", cache_entries().size() == 1);
        model.discardExport();
        expect("discarding at the picker removes the file", cache_entries().isEmpty());
        model.prepareExport(kPass);
        expect("an export left unsaved",
               pump_until([&]() { return results.prepared.size() == 3; }) && cache_entries().size() == 1);
    }
    expect("the model removes an unsaved export when it goes", cache_entries().isEmpty());
    {
        BackupModel model(&host, {});
        model.setKdfParams(fast_kdf());
        model.prepareExport(kPass);
        expect("an export finishes on the worker", model.waitForJobs(60000) && cache_entries().size() == 1);
    }
    expect("going with a finished export's result undelivered leaves no file", cache_entries().isEmpty());
    {
        BackupModel model(&host, {});
        model.setKdfParams(fast_kdf());
        model.prepareExport(kPass);
        expect("an export finishes on the worker", model.waitForJobs(60000) && cache_entries().size() == 1);
        model.cancel();
    }
    expect("going with a cancelled export's result undelivered leaves no file", cache_entries().isEmpty());
}

/** The pending restore's staged summary. */
QVariantMap
pending_summary() {
    return QJsonDocument::fromJson(read_file(at("restore-pending/manifest.json")))
        .object()
        .value("summary")
        .toObject()
        .toVariantMap();
}

void
test_cancelled_inspect(FakeHost& host, const QString& older, const QString& newer) {
    {
        BackupModel model(&host, {});
        Results results(model);
        model.inspectRestore(older, kPass);
        expect("the inspection finishes on the worker", model.waitForJobs(60000));
        expect("its staging exists before its result arrives", staging_dirs().size() == 1);
        model.cancel();
        settle(model);
        expect("a cancelled inspection's late result is dropped", results.total() == 0);
        expect("it removes only its own staging", staging_dirs().isEmpty() && !dsd_qt::backup::pending_exists());
    }
    {
        BackupModel model(&host, {});
        Results results(model);
        model.inspectRestore(older, kPass);
        expect("the older inspection finishes on the worker", model.waitForJobs(60000));
        model.cancel();
        model.inspectRestore(newer, kPass);
        expect("the newer inspection finishes on the worker", model.waitForJobs(60000));
        expect("both stagings exist", staging_dirs().size() == 2);
        settle(model);
        expect("only the newer inspection reports",
               results.inspected.size() == 1 && results.inspected.value(0).value("ok").toBool()
                   && results.inspected.value(0).value("summary").toMap().value("systems").toInt() == 2);
        expect("the newer staging is the pending restore", dsd_qt::backup::pending_exists()
                                                               && pending_summary().value("systems").toInt() == 2
                                                               && staging_dirs().isEmpty());
        expect("clean up", model.discardRestore().value("ok").toBool());
    }
    {
        BackupModel model(&host, {});
        Results results(model);
        host.closeGate();
        model.inspectRestore(older, kPass);
        expect("the older inspection holds at the copy", host.waitArrived(1));
        model.cancel();
        model.inspectRestore(newer, kPass);
        expect("the newer inspection is busy", model.busy());
        host.openGate();
        settle(model);
        expect("a job cancelled on the worker reports nothing",
               results.inspected.size() == 1 && results.inspected.value(0).value("ok").toBool());
        expect("the newer restore is pending and nothing else is left", pending_summary().value("systems").toInt() == 2
                                                                            && staging_dirs().isEmpty()
                                                                            && cache_entries().isEmpty());
        expect("clean up", model.discardRestore().value("ok").toBool());
    }
    {
        BackupModel model(&host, {});
        Results results(model);
        host.closeGate();
        model.inspectRestore(older, kPass);
        expect("the inspection holds at the copy", host.waitArrived(1));
        const QVariantMap discarded = model.discardRestore();
        expect("discarding cancels an inspection that would stage", discarded.value("ok").toBool() && !model.busy());
        host.openGate();
        settle(model);
        expect("the discarded inspection stages nothing",
               results.total() == 0 && staging_dirs().isEmpty() && !dsd_qt::backup::pending_exists());
    }
}

void
test_teardown_with_undelivered_inspections(FakeHost& host, const QString& document) {
    {
        BackupModel model(&host, {});
        model.inspectRestore(document, kPass);
        expect("an inspection finishes on the worker", model.waitForJobs(60000) && staging_dirs().size() == 1);
    }
    expect("going with a finished inspection's result undelivered leaves no staging",
           staging_dirs().isEmpty() && !dsd_qt::backup::pending_exists());
    {
        BackupModel model(&host, {});
        model.inspectRestore(document, kPass);
        expect("an inspection finishes on the worker", model.waitForJobs(60000) && staging_dirs().size() == 1);
        model.cancel();
    }
    expect("going with a cancelled inspection's result undelivered leaves no staging",
           staging_dirs().isEmpty() && !dsd_qt::backup::pending_exists());
}

/** A backup sealed under a passphrase shorter than today's minimum still opens with it. */
void
test_short_real_passphrase(FakeHost& host, const QString& scratch) {
    auto built = dsd_qt::backup::backup_build_archive(dsd_qt::backup::backup_take_snapshot(), nullptr);
    int status = -100;
    const QByteArray sealed = dsd_qt::backup::backup_seal(built.archive, "short7", fast_kdf(), &status);
    DSD_SECURE_ZERO(built.archive.data(), static_cast<size_t>(built.archive.size()));
    const QString document = scratch + "/short.dsdbak";
    expect("seal under a short passphrase", status == DSD_BACKUP_OK && write_file(document, sealed));
    BackupModel model(&host, {});
    Results results(model);
    const QVariantMap wrong = inspect(model, results, document, "short8");
    expect("another short passphrase is wrong", wrong.value("error") == "WrongPassphrase");
    const QVariantMap opened = inspect(model, results, document, "short7");
    expect("the backup's own short passphrase opens it", opened.value("ok").toBool());
    expect("clean up", model.discardRestore().value("ok").toBool());
}

/** Whoever reacts to busy turning false sees the job's outcome already in place. */
void
test_idle_after_outcome(FakeHost& host, const QString& document, const QString& scratch) {
    BackupModel model(&host, {});
    model.setKdfParams(fast_kdf());
    Results results(model);

    // What was in place each time busy turned false: the card's result and whether a restore is pending.
    struct Idle {
        QVariantMap lastResult;
        bool pending = false;
    };

    QList<Idle> idles;
    bool saveOnIdle = false;
    QObject::connect(&model, &BackupModel::busyChanged, [&]() {
        if (model.busy()) {
            return;
        }
        idles.append({model.lastResult(), dsd_qt::backup::pending_exists()});
        if (saveOnIdle) {
            // A flow that opens the save picker as soon as busy drops can save straight away.
            saveOnIdle = false;
            model.saveExport(QUrl::fromLocalFile(scratch + "/idle.dsdbak").toString());
        }
    });

    const QVariantMap inspected = inspect(model, results, document);
    expect("an inspection is staged", inspected.value("ok").toBool());
    expect("one idle, with the inspection's restore pending and on the card",
           idles.size() == 1 && idles.value(0).pending && idles.value(0).lastResult.value("action") == "inspectRestore"
               && idles.value(0).lastResult.value("ok").toBool());

    idles.clear();
    saveOnIdle = true;
    model.prepareExport(kPass);
    expect("an export prepared and saved from the idle handler",
           pump_until([&]() { return results.finished.size() == 1; }) && results.finished.value(0).value("ok").toBool()
               && results.prepared.size() == 1 && results.prepared.value(0).value("ok").toBool());
    expect("one idle per step, the save's showing it on the card",
           idles.size() == 2 && idles.value(1).lastResult.value("action") == "saveExport"
               && idles.value(1).lastResult.value("ok").toBool());
    expect("the saved export is in its document", read_file(scratch + "/idle.dsdbak").startsWith("DSDNEOBK"));

    host.closeGate();
    model.inspectRestore(document, kPass);
    expect("an inspection holds at the copy", host.waitArrived(1));
    idles.clear();
    const QVariantMap discarded = model.discardRestore();
    expect("discarding cancels it and removes the pending restore", discarded.value("ok").toBool());
    expect("one idle, with the pending restore already gone", idles.size() == 1 && !idles.value(0).pending);
    host.openGate();
    settle(model);
}

/** #782: the Android app's teardown times this destructor, which waits for its worker pool. */
void
test_destructor_marks_its_teardown_stage(FakeHost& host) {
    TeardownCapture::lines().clear();
    { BackupModel model(&host, {}); }
    expect("no teardown marker while markers are off", TeardownCapture::lines().isEmpty());
    {
        const TeardownCapture capture;
        { BackupModel model(&host, {}); }
        expect("~BackupModel marks its teardown stage",
               TeardownCapture::lines() == QStringList{QStringLiteral("teardown: ~BackupModel took 7 ms")});
    }
}

/** The model's teardown stops a copy a document provider holds instead of waiting for it. */
void
test_teardown_stops_a_held_copy(FakeHost& host, const QString& document) {
    const int before = host.aborts();
    {
        BackupModel idle(&host, {});
        (void)idle;
    }
    expect("an idle model's teardown leaves the host's copies alone", host.aborts() == before);
    host.closeGate();
    const int aborts = host.aborts();
    std::mutex mutex;
    std::condition_variable changed;
    bool ended = false;
    std::thread safety;
    QElapsedTimer timer;
    {
        BackupModel model(&host, {});
        model.inspectRestore(document, kPass);
        expect("the inspection holds inside the copy", host.waitArrived(1));
        // Opens the gate after a while, so a teardown that waits for the copy still ends this test.
        safety = std::thread([&]() {
            std::unique_lock<std::mutex> lock(mutex);
            if (!changed.wait_for(lock, std::chrono::seconds(5), [&]() { return ended; })) {
                host.openGate();
            }
        });
        timer.start();
    }
    const qint64 elapsed = timer.elapsed();
    {
        const std::lock_guard<std::mutex> lock(mutex);
        ended = true;
    }
    changed.notify_all();
    safety.join();
    host.openGate();
    expect("the model asks the host to stop its copies", host.aborts() == aborts + 1);
    expect(QStringLiteral("teardown does not wait for the held copy (%1 ms)").arg(elapsed), elapsed < 2000);
    expect("an aborted inspection leaves nothing",
           staging_dirs().isEmpty() && cache_entries().isEmpty() && !dsd_qt::backup::pending_exists());
}

#ifdef Q_OS_LINUX
/** How far the process has read into the open file @p path (its descriptor's offset), or -1 when it is not open. */
qint64
open_file_offset(const QString& path) {
    const QStringList fds = QDir(QStringLiteral("/proc/self/fd")).entryList(QDir::AllEntries | QDir::System);
    for (const QString& fd : fds) {
        if (QFileInfo(QStringLiteral("/proc/self/fd/") + fd).symLinkTarget() != path) {
            continue;
        }
        const QByteArray info = read_file(QStringLiteral("/proc/self/fdinfo/") + fd);
        for (const QByteArray& line : info.split('\n')) {
            if (line.startsWith("pos:")) {
                return line.mid(4).trimmed().toLongLong();
            }
        }
    }
    return -1;
}

/** Run @p copy on a thread, abort @p host once it has read 1 MiB of @p source, and check what is left. */
void
abort_mid_stream(const QString& what, StayingHost& host, const QString& source, const QString& destination,
                 const std::function<bool()>& copy) {
    std::atomic<int> copied{-1};
    std::thread worker([&]() { copied.store(copy() ? 1 : 0); });
    QElapsedTimer timer;
    timer.start();
    qint64 offset = -1;
    while (copied.load() < 0 && (offset = open_file_offset(source)) < (1 << 20) && timer.elapsed() < 30000) {}
    const bool midStream = copied.load() < 0 && offset >= (1 << 20);
    host.abortDocumentTransfers();
    worker.join();
    expect(what + ": aborted while copying", midStream);
    expect(what + ": the copy fails", copied.load() == 0);
    expect(what + ": no destination is left",
           !QFileInfo::exists(destination)
               && QDir(QFileInfo(destination).path())
                      .entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot)
                      .isEmpty());
}
#endif

/** The base host's abort stops a copy in the middle, leaves nothing behind, and lasts. */
void
test_base_host_abort(const QString& scratch) {
#ifdef Q_OS_LINUX
    const QString source = QFileInfo(scratch).canonicalFilePath() + "/big.bin";
    {
        QFile file(source);
        expect("write a large sparse source", file.open(QIODevice::WriteOnly) && file.resize(2LL << 30));
    }
    QDir().mkpath(scratch + "/out1");
    QDir().mkpath(scratch + "/out2");
    StayingHost toDocument;
    const QString document = scratch + "/out1/backup.dsdbak";
    abort_mid_stream("copy into a document", toDocument, source, document,
                     [&]() { return toDocument.copyFileToDocument(source, document); });
    StayingHost fromDocument;
    const QString copy = scratch + "/out2/copy.in";
    abort_mid_stream("copy from a document", fromDocument, source, copy, [&]() {
        return fromDocument.copyDocumentToFile(source, copy, 4LL << 30) == dsd_qt::DecoderHost::DocumentCopied;
    });
    expect("write a small source", write_file(scratch + "/small.bin", "small"));
    expect("an aborted host copies nothing afterwards",
           !toDocument.copyFileToDocument(scratch + "/small.bin", scratch + "/out1/later")
               && fromDocument.copyDocumentToFile(scratch + "/small.bin", scratch + "/out2/later", 1024)
                      == dsd_qt::DecoderHost::DocumentFailed
               && !QFileInfo::exists(scratch + "/out1/later") && !QFileInfo::exists(scratch + "/out2/later"));
    StayingHost fresh;
    expect("another host still copies", fresh.copyFileToDocument(scratch + "/small.bin", scratch + "/out1/fresh")
                                            && read_file(scratch + "/out1/fresh") == "small");
    QFile::remove(source);
#else
    (void)scratch;
#endif
}

void
write_result_file(const QString& outcome, const QString& reason, int systems) {
    const QJsonObject file{{"outcome", outcome}, {"reason", reason}, {"summary", QJsonObject{{"systems", systems}}}};
    expect("write a result file", write_file(at(kResultFile), QJsonDocument(file).toJson()));
}

void
test_startup_outcome(FakeHost& host, const QString& document) {
    write_result_file("stuck", "Restore couldn't finish. Free up storage and reopen DSD-neo", 3);
    {
        BackupModel model(&host, {});
        const QVariantMap outcome = model.startupOutcome();
        expect("a stuck outcome from the file",
               outcome.value("kind") == "stuck" && outcome.value("systems").toInt() == 3
                   && outcome.value("message") == "Restore couldn't finish. Free up storage and reopen DSD-neo");
    }
    {
        ApplyResult failed;
        failed.outcome = ApplyOutcome::Failed;
        failed.reason = "Nothing was changed";
        BackupModel model(&host, failed);
        expect("this start's outcome wins over an older file",
               model.startupOutcome().value("kind") == "failed"
                   && model.startupOutcome().value("message") == "Nothing was changed");
        model.acknowledgeStartupOutcome();
        expect("acknowledging removes the older file too", !QFileInfo::exists(at(kResultFile)));
    }
    write_result_file("bogus", "x", 1);
    {
        BackupModel model(&host, {});
        expect("an unknown outcome in the file shows nothing",
               model.startupOutcome().value("kind").toString().isEmpty());
        model.acknowledgeStartupOutcome();
    }
    {
        // A committed restore the busy engine deferred.
        StayingHost staying;
        BackupModel committer(&staying, {});
        Results results(committer);
        expect("inspect", inspect(committer, results, document).value("ok").toBool());
        expect("commit", committer.commitRestore().value("ok").toBool());
    }
    ApplyResult deferred;
    deferred.outcome = ApplyOutcome::Deferred;
    deferred.reason = "The decoder was busy, so the restore waits for the next start";
    deferred.summary = {{"systems", 2}, {"recordingsToChoose", 1}};
    BackupModel model(&host, deferred);
    Results results(model);
    expect("a deferred outcome", model.startupOutcome().value("kind") == "deferred"
                                     && model.startupOutcome().value("message") == deferred.reason
                                     && model.startupOutcome().value("recordingsToChoose").toInt() == 1);
    expect("a deferred restore waits", model.restorePending());
    const QVariantMap discarded = model.discardDeferredRestore();
    expect("the deferred restore is discarded", discarded.value("ok").toBool() && discarded.value("discarded").toBool()
                                                    && !dsd_qt::backup::pending_exists() && !model.restorePending());
    expect("and its card is gone",
           model.startupOutcome().value("kind").toString().isEmpty() && results.outcomeChanges == 1);
}

/** Inspect @p document and commit it through a host that cannot quit, as a start that deferred it would leave it. */
void
commit_document(const QString& document) {
    StayingHost staying;
    BackupModel model(&staying, {});
    Results results(model);
    expect("the backup inspects", inspect(model, results, document).value("ok").toBool());
    BackupModel unconfirmed(&staying, {});
    expect("an unconfirmed restore does not wait", !unconfirmed.restorePending());
    expect("the restore commits", model.commitRestore().value("ok").toBool());
}

void
test_pending_at_start(const QString& document) {
    StayingHost host;
    {
        BackupModel model(&host, {});
        expect("nothing waits, so nothing can be cancelled", !model.restorePending() && !model.restoreCancellable());
    }
    commit_document(document);
    {
        BackupModel model(&host, {});
        Results results(model);
        expect("a restore an earlier start left committed waits from the start", model.restorePending());
        expect("and can be cancelled", model.restoreCancellable());
        const QVariantMap discarded = model.discardRestore();
        expect("discarding it clears it", discarded.value("ok").toBool() && discarded.value("discarded").toBool()
                                              && !model.restorePending() && results.pendingChanges == 1
                                              && !dsd_qt::backup::pending_exists());
        expect("and nothing is left to cancel", !model.restoreCancellable() && results.cancellableChanges == 1);
    }

    // The apply got as far as a complete journal, then stopped (stuck, or deferred on resuming it).
    commit_document(document);
    const QString journal = at("restore-pending/journal/COMPLETE");
    expect("mark the journal complete", write_file(journal, QByteArray()));
    {
        BackupModel model(&host, {});
        Results results(model);
        expect("an apply part-way through waits", model.restorePending());
        expect("an apply part-way through is not offered for cancelling", !model.restoreCancellable());
        const QVariantMap refused = model.discardRestore();
        expect("an apply part-way through cannot be discarded, and says so",
               !refused.value("ok").toBool() && !refused.value("discarded").toBool()
                   && refused.value("error") == "RestoreInProgress" && model.restorePending()
                   && results.pendingChanges == 0 && dsd_qt::backup::pending_committed());
        expect("its deferred card discards nothing either",
               model.discardDeferredRestore().value("error") == "RestoreInProgress" && model.restorePending());
        expect("still not cancellable, and nothing announced",
               !model.restoreCancellable() && results.cancellableChanges == 0);
    }
    expect("clean up",
           QFile::remove(journal) && dsd_qt::backup::discard_pending() && !dsd_qt::backup::pending_exists());
}

/**
 * A library file this phone cannot read leaves its row out of the restore, not the whole backup: the inspection counts
 * and lists it for the confirmation, and so does the outcome of the start that applies it.
 */
void
test_unreadable_library_row(FakeHost& host, const QString& scratch) {
    wipe_configuration();
    build_configuration(host);
    QString targets, name;
    for (const auto& value : dsd_qt::json_store_load_array("imported_files.json")) {
        if (value.toObject().value("type") == "trunkTargets") {
            targets = value.toObject().value("path").toString();
            name = value.toObject().value("name").toString();
        }
    }
    expect("break the target list on disk", !targets.isEmpty() && write_file(targets, "not,a,target,list\n"));
    const QString document = scratch + "/unreadable.dsdbak";
    expect("the export carries it as it is", export_to(host, document).value("ok").toBool());
    wipe_configuration();
    const QString line = name + ": couldn't be read on this phone";
    {
        BackupModel model(&host, {});
        Results results(model);
        const QVariantMap inspected = inspect(model, results, document);
        const QVariantMap summary = inspected.value("summary").toMap();
        expect("a backup with an unreadable library file still inspects", inspected.value("ok").toBool());
        expect("the inspection leaves the row out and says so",
               summary.value("libraryFiles").toInt() == 3
                   && summary.value("notIncluded").toStringList().contains(line));
        expect("the restore commits", model.commitRestore().value("ok").toBool());
    }
    const ApplyResult applied = dsd_qt::backup::apply_pending_restore({});
    expect("the rest applies", applied.outcome == ApplyOutcome::Applied);
    BackupModel model(&host, applied);
    const QVariantMap outcome = model.startupOutcome();
    expect("the start that applies it lists it too",
           outcome.value("libraryFiles").toInt() == 3 && outcome.value("notIncluded").toStringList().contains(line));
    model.acknowledgeStartupOutcome();
    dsd_app_signal_search_store_close();
}

/** Properties (type, name, notify), invokables and signals (return type and signature) a meta-object adds. */
QStringList
qml_surface(const QMetaObject& meta) {
    QStringList surface;
    for (int i = meta.propertyOffset(); i < meta.propertyCount(); ++i) {
        const QMetaProperty property = meta.property(i);
        surface.append(QStringLiteral("property %1 %2 %3%4")
                           .arg(QLatin1String(property.typeName()), QLatin1String(property.name()),
                                property.isWritable() ? QStringLiteral("writable ") : QString(),
                                property.hasNotifySignal()
                                    ? QStringLiteral("notify %1").arg(QLatin1String(property.notifySignal().name()))
                                    : QStringLiteral("constant")));
    }
    for (int i = meta.methodOffset(); i < meta.methodCount(); ++i) {
        const QMetaMethod method = meta.method(i);
        const bool signal = method.methodType() == QMetaMethod::Signal;
        if (!signal && method.methodType() != QMetaMethod::Method) {
            continue; // slots are the model's own plumbing
        }
        surface.append(QStringLiteral("%1 %2 %3")
                           .arg(signal ? QStringLiteral("signal") : QStringLiteral("invokable"),
                                QLatin1String(method.typeName()), QLatin1String(method.methodSignature())));
    }
    surface.sort();
    return surface;
}

void
test_recorder_parity() {
    const QStringList model = qml_surface(BackupModel::staticMetaObject);
    const QStringList recorder = qml_surface(BackupRecorder::staticMetaObject);
    expect("the model publishes a QML surface", model.size() > 20);
    for (const QString& entry : model) {
        expect(QStringLiteral("the QML tests' backup stand-in has: %1").arg(entry), recorder.contains(entry));
    }
    for (const QString& entry : recorder) {
        expect(QStringLiteral("the model has the stand-in's: %1").arg(entry), model.contains(entry));
    }
}

void
test_sweep(FakeHost& host) {
    expect("write leftovers", write_file(cache_dir() + "/old.dsdbak", "x") && write_file(cache_dir() + "/old.in", "y")
                                  && write_file(at("restore-staging-old/files/imports/a.csv"), "z"));
    expect("write a pending restore", write_file(at("restore-pending/manifest.json"), "{}"));
    {
        BackupModel model(&host, {});
        expect("construction sweeps leftover exports and stagings",
               cache_entries().isEmpty() && staging_dirs().isEmpty());
        expect("a pending restore is not a leftover", dsd_qt::backup::pending_exists());
    }
    expect("clean up", dsd_qt::backup::discard_pending());
    expect("a non-staging directory is never discarded as staging",
           !dsd_qt::backup::discard_staging(at("restore-pending")) && !dsd_qt::backup::discard_staging(root()));
    expect("write a staging dir", write_file(at("restore-staging-x/a"), "z"));
    expect("a staging dir is discarded", dsd_qt::backup::discard_staging(at("restore-staging-x"))
                                             && staging_dirs().isEmpty()
                                             && dsd_qt::backup::discard_staging(at("restore-staging-x")));
}

void
test_small_things(FakeHost& host) {
    BackupModel model(&host, {});
    Results results(model);
    const QString name = model.suggestedFileName();
    expect("the suggested name",
           name == QStringLiteral("dsd-neo-backup-%1.dsdbak").arg(dsd_qt::realtimeCurrentDate().toString(Qt::ISODate))
               && QRegularExpression("^dsd-neo-backup-\\d{4}-\\d{2}-\\d{2}\\.dsdbak$").match(name).hasMatch());
    model.setFlowActive(true);
    model.setFlowActive(true);
    expect("flow active", model.flowActive() && results.flowChanges == 1);
    model.setFlowActive(false);
    expect("flow inactive", !model.flowActive() && results.flowChanges == 2);
}

/** Whether any file below @p dir holds @p needle. */
bool
any_file_holds(const QString& dir, const QByteArray& needle) {
    QDirIterator it(dir, QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        if (read_file(it.next()).contains(needle)) {
            return true;
        }
    }
    return false;
}

void
test_passphrase_written_nowhere(const QString& scratch) {
    const QString isolated = qEnvironmentVariable("XDG_DATA_HOME");
    const QString passphrase = QString::fromLatin1(kPass);
    const QByteArray utf16(reinterpret_cast<const char*>(passphrase.utf16()), passphrase.size() * 2);
    for (const QString& dir : {isolated, scratch}) {
        expect(QStringLiteral("the passphrase is written nowhere under %1").arg(dir),
               !dir.isEmpty() && !any_file_holds(dir, QByteArray(kPass)) && !any_file_holds(dir, utf16));
    }
}

} // namespace

int
main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("dsd-neo-test"));
    QCoreApplication::setApplicationName(QLatin1String(kRootMarker)
                                         + QString::number(QCoreApplication::applicationPid()));
    dsd_test_qt_isolate_paths();
    const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (dataDir != root()) {
        DSD_FPRINTF(stderr, "the data root is not the app data directory\n");
        return 1;
    }
    remove_tree(dataDir);
    QDir().mkpath(dataDir);

    {
        FakeHost host;
        QTemporaryDir scratch;
        const QString full = scratch.filePath("full.dsdbak");
        const QString older = scratch.filePath("older.dsdbak");
        Captured before;
        quietly([&]() {
            build_configuration(host);
            before = capture(host);
        });
        quietly([&]() { test_export(host, full); });
        quietly([&]() {
            dsd_qt::SavedSystemsModel systems;
            systems.remove(1);
            expect("drop a system", systems.rowCount() == 1);
            expect("export the older configuration",
                   export_to(host, older).value("summary").toMap().value("systems").toInt() == 1);
        });
        quietly([&]() { test_round_trip(host, full, before); });
        quietly([&]() { test_session_guards(host, full); });
        quietly([&]() { test_staying_host(full); });
        quietly([&]() { test_discard_failures(full); });
        quietly([&]() { test_errors(host, full, scratch.path()); });
        quietly([&]() { test_short_real_passphrase(host, scratch.path()); });
        quietly([&]() { test_busy_refusals(host, full); });
        quietly([&]() { test_cancelled_export(host, full); });
        quietly([&]() { test_cancelled_inspect(host, older, full); });
        quietly([&]() { test_teardown_with_undelivered_inspections(host, full); });
        quietly([&]() { test_idle_after_outcome(host, full, scratch.path()); });
        quietly([&]() { test_teardown_stops_a_held_copy(host, full); });
        quietly([&]() { test_destructor_marks_its_teardown_stage(host); });
        quietly([&]() { test_base_host_abort(scratch.path()); });
        quietly([&]() { test_startup_outcome(host, full); });
        quietly([&]() { test_pending_at_start(full); });
        quietly([&]() { test_unreadable_library_row(host, scratch.path()); });
        quietly([&]() { test_recorder_parity(); });
        quietly([&]() { test_sweep(host); });
        quietly([&]() { test_small_things(host); });
        test_passphrase_written_nowhere(scratch.path());
    }
    dsd_app_signal_search_store_close();
#ifndef BACKUP_MODEL_TEST_SANITIZED
    expect("each of the two applies logs once", g_appliedLines == 2);
#endif

    remove_tree(dataDir);
    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    DSD_FPRINTF(stderr, "OK\n");
    return 0;
}
