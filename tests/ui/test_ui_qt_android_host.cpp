// SPDX-License-Identifier: GPL-3.0-or-later
#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QMap>
#include <QObject>
#include <QSemaphore>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QThread>
#include <QUrl>
#include <QVariant>
#include <QVariantMap>
#include <QtGlobal>
#include <atomic>
#include <cstdio>
#include <functional>
#include <initializer_list>
#include <mutex>
#include <stdint.h>
#include <utility>
#include "../../android/decoder_host_android.h"
#include "../../android/local_device_state.h"
#include "../../android/run_status.h"
#include "../../android/session_state_map.h"
#include "decoder_host.h"
#include "diagnostics_log.h"

static QJsonObject&
retained_record() {
    static QJsonObject value;
    return value;
}

static QByteArray&
service_state() {
    static QByteArray value = "IDLE";
    return value;
}

namespace {
/* The platform's document transport: it records each call, and answers as the test arranged. */
struct PlatformCopies {
    int calls = 0;
    QString reference;
    QString destPath;
    QString sourcePath;
    qint64 maxBytes = -1;
    bool destExisted = false;
    int answer = dsd_qt::DecoderHost::DocumentCopied;
    bool writesDest = true; // As the Kotlin side's rename does on a copy.
    bool saves = true;
    bool holdsResolver = true;
};
} // namespace

static PlatformCopies&
platform() {
    static PlatformCopies value;
    return value;
}

namespace {
/* The platform's call recording transport (#708): RecordingShare, RecordingPlayback and RecordingExport as the test
 * arranged them. The transfer runs on the host's worker thread; a held one waits for the test or its own cancel. */
struct PlatformRecordings {
    // The transfer: its id, progress and cancel; the worker's signals to the test (entered, returned) and the test's
    // release of a held call.
    std::atomic<qint64> transferId{0};
    std::atomic<qint64> progress{-1};
    std::atomic<qint64> cancelled{0};
    QSemaphore entered;
    QSemaphore released;
    QSemaphore returned;
    // Playback and roots, read and written on the GUI thread only.
    QStringList roots;
    QString played;
    QStringList commands;
    QString status;
    // What the transfer saw, and its answer: guarded by mutex (a std::mutex, which TSan sees).
    QStringList paths;
    QStringList names;
    QString reference;
    QStringList answer = {QStringLiteral("0")};
    // Shares.
    QString sharedPath;
    QString sharedTitle;
    QStringList shareFailures;
    std::mutex mutex;
    int plays = 0;
    int nextGeneration = 0;
    int statusReads = 0;
    std::atomic<int> transfers{0};
    int shares = 0;
    bool zip = false;
    bool onWorker = false;
    bool shareAnswer = true;
    std::atomic<bool> hold{false};
    // A held call that ignores its cancel, as a write blocked on a provider's pipe does.
    std::atomic<bool> ignoresCancel{false};
};
} // namespace

static PlatformRecordings&
recordings() {
    static PlatformRecordings value;
    return value;
}

/* What the platform's transfer last saw, and the answer it gives, under its lock. */
template <class F>
static auto
with_transfer(F f) {
    PlatformRecordings& r = recordings();
    const std::lock_guard<std::mutex> lock(r.mutex);
    return f(r);
}

static void
reset_recordings() {
    PlatformRecordings& r = recordings();
    r.roots.clear();
    r.plays = 0;
    r.nextGeneration = 0;
    r.played.clear();
    r.commands.clear();
    r.status.clear();
    r.statusReads = 0;
    r.transfers = 0;
    r.transferId = 0;
    r.paths.clear();
    r.names.clear();
    r.reference.clear();
    r.zip = false;
    r.onWorker = false;
    r.answer = {QStringLiteral("0")};
    r.hold = false;
    r.progress = -1;
    r.cancelled = 0;
    r.shares = 0;
    r.shareAnswer = true;
    r.shareFailures.clear();
    r.ignoresCancel = false;
}

static bool running;
static int serviceStops;
// The published phase when the stop intent was dispatched, and the host's real-time clock (#718).
static int phaseAtStopDispatch = -1;
static uint64_t nowMs;
static int hostDiagnostics;
static QString lastHostDiagnostic;
// Every host refresh reads the lifecycle record and refreshes presentation and the local device.
static int lifecycleReads;
static int hostRefreshes;
// The platform's start intents and USB access requests the host dispatched.
static int startDispatches;
static int accessRequests;
// #782: the attempt each start intent carried, why the next dispatch fails ("" hands it over), and the service's side
// of DecoderService.retireAttempts(): each call's bound, the lifecycle reads made before it, and its answer.
static uint64_t lastStartAttempt;
static QString startDispatchFailure;
static QList<uint64_t> retirements;
static int lifecycleReadsAtRetire;
static bool serviceTookAttempt;
// When not empty, the record the service publishes once the retirement returns (gate r5 L1): what happened to the
// service between the host's lifecycle read and its retirement. Its answer is then the service's own rule, taken when
// the record's attempt is the bound and was not cancelled, and the hook is used once.
static QJsonObject recordAtRetire;

// UsbSourceManager.deviceStatus(), which the host's local-device refresh applies.
static QJsonObject&
usb_record() {
    static QJsonObject value;
    return value;
}

// Only platform transport is replaced. The real Android host's refresh, stop,
// phase publication and failureText getter are compiled by this desktop target.
namespace dsd_android {
DecoderHostAndroid::DecoderHostAndroid(QObject* parent) : DecoderHost(parent) {}

QJsonObject
DecoderHostAndroid::readLifecycleStatus() const {
    ++lifecycleReads;
    return retained_record();
}

bool
DecoderHostAndroid::readEngineRunning() {
    return running;
}

uint64_t
DecoderHostAndroid::readRealtimeMs() {
    return nowMs;
}

QByteArray
DecoderHostAndroid::readServiceState() {
    return service_state();
}

QString
DecoderHostAndroid::serviceFailureText() const {
    return retained_record().value("lastError").toString();
}

void
DecoderHostAndroid::requestServiceStop() {
    ++serviceStops;
    phaseAtStopDispatch = static_cast<int>(sessionState());
}

void
DecoderHostAndroid::refreshPresentation() {
    ++hostRefreshes;
}

qreal
DecoderHostAndroid::fontPixelSize(qreal sp) const {
    return sp;
}

void
DecoderHostAndroid::setDarkAppearance(bool) {}

void
DecoderHostAndroid::requestNotificationPermission() {}

void
DecoderHostAndroid::refreshLocation() {}

void
DecoderHostAndroid::refreshLocalDevice() {
    ++hostRefreshes;
    m_usb.apply(*this, usb_record());
}

void
DecoderHostAndroid::requestCurrentLocation(qint64) {}

void
DecoderHostAndroid::requestSiteLocation(qint64) {}

void
DecoderHostAndroid::cancelLocationRequest(qint64) {}

void
DecoderHostAndroid::hostDiagnostic(const QString& line) {
    ++hostDiagnostics;
    lastHostDiagnostic = line;
}

void
DecoderHostAndroid::shareDiagnostics(const QString&, const QString&) {}

QString
DecoderHostAndroid::platformStartDecoder(const QStringList&, uint64_t attempt) {
    if (!startDispatchFailure.isEmpty()) {
        return startDispatchFailure;
    }
    ++startDispatches;
    lastStartAttempt = attempt;
    return {};
}

bool
DecoderHostAndroid::platformRetireAttempts(uint64_t through) {
    retirements.append(through);
    lifecycleReadsAtRetire = lifecycleReads;
    if (!recordAtRetire.isEmpty()) {
        retained_record() = recordAtRetire;
        recordAtRetire = QJsonObject();
        /* DecoderService.retireAttempts(): one still STARTING is cancelled, one past it (RUNNING) is taken. */
        return static_cast<uint64_t>(retained_record().value(QStringLiteral("attemptId")).toInteger()) == through
               && retained_record().value(QStringLiteral("state")).toString() != QStringLiteral("STARTING");
    }
    return serviceTookAttempt;
}

void
DecoderHostAndroid::noteAttempt(const QString&, dsd_qt::DiagnosticsLog::AttemptStage) {}

bool
DecoderHostAndroid::moveToBackground() {
    return false;
}

void
DecoderHostAndroid::platformRequestAccess() {
    ++accessRequests;
}

void
DecoderHostAndroid::platformRequestAccessForSource(const QString&, const QString&) {
    ++accessRequests;
}

/* The service keeps the mark in the record it publishes, as DecoderService.lifecycleStatus() does. */
void
DecoderHostAndroid::platformAcknowledgeReceiverEnding(uint64_t session) {
    QJsonObject& record = retained_record();
    record.insert(QStringLiteral("endingAcknowledged"),
                  qMax(static_cast<qint64>(session), record.value(QStringLiteral("endingAcknowledged")).toInteger()));
}

void
DecoderHostAndroid::noteStartAttempt(const QString&) {}

void
DecoderHostAndroid::setScreenPolicy(int, int) {}

QString
DecoderHostAndroid::importContentUri(const QString&, const QString&) {
    return {};
}

QString
DecoderHostAndroid::importDocument(const QString&, const QString&, const QString&) {
    return {};
}

bool
DecoderHostAndroid::quitApp() {
    return false;
}

bool
DecoderHostAndroid::platformHoldsResolver() {
    return platform().holdsResolver;
}

int
DecoderHostAndroid::platformCopyDocumentToFile(const QString& reference, const QString& destPath, qint64 maxBytes) {
    PlatformCopies& copies = platform();
    ++copies.calls;
    copies.reference = reference;
    copies.destPath = destPath;
    copies.maxBytes = maxBytes;
    copies.destExisted = QFileInfo::exists(destPath);
    if (copies.writesDest) {
        QFile out(destPath);
        if (out.open(QIODevice::WriteOnly)) {
            out.write("copied");
        }
    }
    return copies.answer;
}

bool
DecoderHostAndroid::platformCopyFileToDocument(const QString& sourcePath, const QString& reference) {
    PlatformCopies& copies = platform();
    ++copies.calls;
    copies.sourcePath = sourcePath;
    copies.reference = reference;
    return copies.saves;
}

void
DecoderHostAndroid::abortDocumentTransfers() {
    DecoderHost::abortDocumentTransfers();
}

QStringList
DecoderHostAndroid::platformRecordingRoots() {
    return recordings().roots;
}

int
DecoderHostAndroid::platformPlayRecording(const QString& path) {
    PlatformRecordings& r = recordings();
    ++r.plays;
    r.played = path;
    return r.nextGeneration;
}

void
DecoderHostAndroid::platformPausePlayback() {
    recordings().commands.append(QStringLiteral("pause"));
}

void
DecoderHostAndroid::platformResumePlayback() {
    recordings().commands.append(QStringLiteral("resume"));
}

void
DecoderHostAndroid::platformSeekPlayback(qint64 positionMs) {
    recordings().commands.append(QStringLiteral("seek %1").arg(positionMs));
}

void
DecoderHostAndroid::platformStopPlayback() {
    recordings().commands.append(QStringLiteral("stop"));
}

QString
DecoderHostAndroid::platformPlaybackStatus() {
    ++recordings().statusReads;
    return recordings().status;
}

QStringList
DecoderHostAndroid::platformTransferRecordings(qint64 id, const QStringList& paths, const QStringList& names,
                                               const QString& reference, bool zip) {
    PlatformRecordings& r = recordings();
    {
        const std::lock_guard<std::mutex> lock(r.mutex);
        r.paths = paths;
        r.names = names;
        r.reference = reference;
        r.zip = zip;
        r.onWorker = QThread::currentThread() != QCoreApplication::instance()->thread();
    }
    r.transferId = id;
    ++r.transfers;
    r.entered.release();
    // At most about five seconds, so a host that never cancels fails its checks instead of hanging the test.
    for (int waits = 0;
         r.hold && (r.ignoresCancel || r.cancelled.load() != id) && waits < 1000 && !r.released.tryAcquire(1, 5);
         ++waits) {}
    QStringList answer;
    {
        const std::lock_guard<std::mutex> lock(r.mutex);
        answer = r.answer;
    }
    r.returned.release();
    return answer;
}

qint64
DecoderHostAndroid::platformRecordingTransferProgress(qint64 id) {
    return id == recordings().transferId.load() ? recordings().progress.load() : -1;
}

void
DecoderHostAndroid::platformCancelRecordingTransfer(qint64 id) {
    recordings().cancelled = id;
}

bool
DecoderHostAndroid::platformShareRecording(const QString& path, const QString& title) {
    PlatformRecordings& r = recordings();
    ++r.shares;
    r.sharedPath = path;
    r.sharedTitle = title;
    return r.shareAnswer;
}

QString
DecoderHostAndroid::platformTakeRecordingShareFailure() {
    QStringList& failures = recordings().shareFailures;
    return failures.isEmpty() ? QString() : failures.takeFirst();
}
} // namespace dsd_android

static int failures;

static void
check(bool ok, const char* why) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", why);
        ++failures;
    }
}

/*
 * A restore must not replace the configuration under an engine (#580). engineBusy() asks the service and the engine
 * atomic directly: ui_load asks before the host's first poll, when the published phase still says Idle, and a refresh
 * would deliver a pending USB attachment, which can start a session.
 */
static void
check_engine_busy() {
    using dsd_android::DecoderHostAndroid;

    struct Case {
        const char* state;
        bool engine;
        bool busy;
        const char* why;
    };

    static const Case kCases[] = {
        {"STARTING", false, true, "a service still configuring the engine is busy"},
        {"RUNNING", false, true, "a running service whose engine has not entered its loop is busy"},
        {"RUNNING", true, true, "a running engine is busy"},
        {"STOPPING", false, true, "a stopping service is busy"},
        {"IDLE", true, true, "an engine still running under an idle service is busy"},
        {"IDLE", false, false, "an idle service with no engine is idle"},
        {"", false, true, "a service state that could not be read counts as busy"},
    };
    DecoderHostAndroid host;
    lifecycleReads = 0;
    hostRefreshes = 0;
    for (const Case& c : kCases) {
        service_state() = c.state;
        running = c.engine;
        check(host.engineBusy() == c.busy, c.why);
    }
    check(lifecycleReads == 0 && hostRefreshes == 0, "asking whether the engine is busy never refreshes the host");
    check(host.sessionState() == dsd_qt::DecoderHost::Idle, "asking whether the engine is busy publishes no phase");
    check(dsd_android::engine_busy(nullptr, false), "a failed service state read counts as busy");
    service_state() = "IDLE";
    running = false;
}

static QByteArray
read_file(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

static void
write_file(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    check(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(), "a fixture file is written");
}

/* The backup's document copies (#580): which transport a reference takes, and what the platform's answer means. */
static void
check_document_dispatch() {
    using dsd_android::DecoderHostAndroid;
    using dsd_qt::DecoderHost;
    const QTemporaryDir dir;
    check(dir.isValid(), "a scratch directory exists");
    const QString source = dir.filePath(QStringLiteral("source.bin"));
    const QString dest = dir.filePath(QStringLiteral("dest.in"));
    const QString content = QStringLiteral("content://com.example.documents/document/primary%3Abackup.dsdbak");
    write_file(source, "backup");
    DecoderHostAndroid host;

    platform() = PlatformCopies();
    check(host.copyDocumentToFile(QUrl::fromLocalFile(source).toString(), dest, 1024) == DecoderHost::DocumentCopied
              && read_file(dest) == "backup",
          "a file URL takes the base host's copy");
    const QString saved = dir.filePath(QStringLiteral("saved.bin"));
    check(host.copyFileToDocument(source, saved) && read_file(saved) == "backup", "a plain path takes the base save");
    check(platform().calls == 0, "local references never reach the platform");

    QFile::remove(dest);
    platform() = PlatformCopies();
    check(host.copyDocumentToFile(content, dest, 77) == DecoderHost::DocumentCopied, "a content URI copies");
    check(platform().calls == 1 && platform().reference == content && platform().destPath == dest
              && platform().maxBytes == 77,
          "a content URI goes to the platform with its arguments");

    /* A file already at the destination must not pass for this copy. */
    write_file(dest, "stale");
    platform() = PlatformCopies();
    platform().writesDest = false;
    check(host.copyDocumentToFile(content, dest, 77) == DecoderHost::DocumentLocalFailed,
          "a copy reported without its file (a failed JNI call reads as 0) is the app's failure, not the document's");
    check(!platform().destExisted, "a stale destination is removed before the platform copies");
    check(!QFileInfo::exists(dest), "a failed copy leaves nothing at the destination");

    struct Answer {
        int platform;
        bool writes;
        int result;
        const char* why;
    };

    static const Answer kAnswers[] = {
        {DecoderHost::DocumentTooLarge, false, DecoderHost::DocumentTooLarge, "too large passes through"},
        {DecoderHost::DocumentFailed, false, DecoderHost::DocumentFailed, "a failure passes through"},
        {DecoderHost::DocumentLocalFailed, false, DecoderHost::DocumentLocalFailed,
         "this device's failure passes through"},
        {7, true, DecoderHost::DocumentFailed, "an unknown answer is a failure"},
    };
    for (const Answer& answer : kAnswers) {
        QFile::remove(dest);
        platform() = PlatformCopies();
        platform().answer = answer.platform;
        platform().writesDest = answer.writes;
        check(host.copyDocumentToFile(content, dest, 77) == answer.result, answer.why);
    }

    QDir(dir.path()).mkpath(QStringLiteral("busy/inner"));
    platform() = PlatformCopies();
    check(host.copyDocumentToFile(content, dir.filePath(QStringLiteral("busy")), 77) == DecoderHost::DocumentLocalFailed
              && platform().calls == 0,
          "a destination that cannot be cleared is this device's failure, before the platform is asked");
    check(host.copyDocumentToFile(content, QString(), 77) == DecoderHost::DocumentFailed
              && host.copyDocumentToFile(content, dest, -1) == DecoderHost::DocumentFailed && platform().calls == 0,
          "an empty destination or a negative cap never reaches the platform");

    /* Without the application's ContentResolver no platform transfer can run: the app's failure, never the document's,
     * and the platform is not asked. */
    platform() = PlatformCopies();
    platform().holdsResolver = false;
    check(host.copyDocumentToFile(content, dest, 77) == DecoderHost::DocumentLocalFailed && platform().calls == 0,
          "a copy without a held ContentResolver is the app's failure");
    check(!host.copyFileToDocument(source, content) && platform().calls == 0,
          "a save without a held ContentResolver fails without asking the platform");

    platform() = PlatformCopies();
    check(host.copyFileToDocument(source, content) && platform().calls == 1 && platform().sourcePath == source
              && platform().reference == content,
          "a content URI saves through the platform");
    platform().saves = false;
    check(!host.copyFileToDocument(source, content), "a failed platform save fails");

    host.abortDocumentTransfers();
    platform() = PlatformCopies();
    check(host.copyDocumentToFile(content, dest, 77) == DecoderHost::DocumentFailed
              && !host.copyFileToDocument(source, content) && platform().calls == 0,
          "after an abort no transfer reaches the platform");
    check(host.copyDocumentToFile(QUrl::fromLocalFile(source).toString(), dir.filePath(QStringLiteral("late.in")), 1024)
              == DecoderHost::DocumentFailed,
          "after an abort the base host's copies fail too");
}

/* #708: the host knows its session state only after its first poll of the service; the call recording store holds
 * every journal until then. The known signal comes once, after the polled phase is published, so a session that kept
 * running while the UI was recreated is seen as running when the store hears of it. */
static void
check_session_state_known_after_first_poll() {
    using dsd_android::DecoderHostAndroid;
    using dsd_qt::DecoderHost;
    DecoderHostAndroid host;
    int known = 0;
    bool activeWhenKnown = false;
    QObject::connect(&host, &DecoderHost::sessionStateKnownChanged, [&]() {
        ++known;
        activeWhenKnown = host.sessionStateKnown() && host.sessionActive();
    });
    check(!host.sessionStateKnown() && host.sessionState() == DecoderHost::Idle,
          "before its first poll the host does not know its state");
    nowMs = 20000;
    retained_record() = {{"sessionId", 70}, {"state", "RUNNING"}, {"reason", dsd_android::kRunPending}};
    running = true;
    host.refresh();
    check(host.sessionStateKnown() && known == 1 && activeWhenKnown,
          "the first poll makes it known, once, with the running session already published");
    nowMs += 250;
    host.refresh();
    check(known == 1, "later polls do not announce it again");
    retained_record() = {{"sessionId", 70}, {"state", "IDLE"}, {"reason", dsd_android::kRunCancelled}};
    running = false;
    nowMs += 250;
    host.refresh();
}

/* #718: Stop listening shows Stopping the moment it is tapped, before the service has seen the intent, and holds it
 * through the RUNNING polls that arrive until the service reports STOPPING. */
static void
check_stop_publishes_stopping() {
    using dsd_android::DecoderHostAndroid;
    using dsd_qt::DecoderHost;
    DecoderHostAndroid host;
    int changes = 0;
    QObject::connect(&host, &DecoderHost::sessionStateChanged, [&changes]() { ++changes; });
    serviceStops = 0;
    hostDiagnostics = 0;
    nowMs = 10000;
    retained_record() = {{"sessionId", 60}, {"state", "IDLE"}};
    running = false;
    host.refresh();
    retained_record() = {{"sessionId", 61}, {"state", "RUNNING"}, {"reason", dsd_android::kRunPending}};
    running = true;
    host.refresh();
    check(host.sessionState() == DecoderHost::Running, "the session runs before the stop");

    changes = 0;
    phaseAtStopDispatch = -1;
    host.stop();
    check(serviceStops == 1, "the stop intent is dispatched");
    check(phaseAtStopDispatch == DecoderHost::Stopping, "Stopping is published before the intent is dispatched");
    check(host.sessionState() == DecoderHost::Stopping && host.transitioning() && changes == 1,
          "the stop publishes Stopping at once");
    check(host.statusText() == QStringLiteral("Stopping…"), "the status reads Stopping");
    for (int poll = 1; poll <= 4; ++poll) {
        nowMs = 10000 + 250 * poll;
        host.refresh();
        check(host.sessionState() == DecoderHost::Stopping, "RUNNING polls before the service sees the stop hold");
    }
    retained_record() = {{"sessionId", 61}, {"state", "STOPPING"}, {"reason", dsd_android::kRunPending}};
    nowMs += 250;
    host.refresh();
    check(host.sessionState() == DecoderHost::Stopping, "service STOPPING holds");
    retained_record() = {{"sessionId", 61}, {"state", "IDLE"}, {"reason", dsd_android::kRunCancelled}};
    running = false;
    nowMs += 250;
    host.refresh();
    check(host.sessionState() == DecoderHost::Idle && !host.transitioning(), "the terminal record publishes Idle");
    check(hostDiagnostics == 0, "a delivered stop logs nothing");

    /* A stop the service never saw: two seconds of RUNNING give the user a working Stop button back. */
    retained_record() = {{"sessionId", 62}, {"state", "RUNNING"}, {"reason", dsd_android::kRunPending}};
    running = true;
    nowMs = 20000;
    host.refresh();
    check(host.sessionState() == DecoderHost::Running, "the next session runs");
    host.stop();
    check(host.sessionState() == DecoderHost::Stopping, "the second stop publishes Stopping");
    nowMs = 21999;
    host.refresh();
    check(host.sessionState() == DecoderHost::Stopping && hostDiagnostics == 0, "the grace holds Stopping");
    nowMs = 22000;
    host.refresh();
    check(host.sessionState() == DecoderHost::Running && host.statusText() == QStringLiteral("Decoding"),
          "a stop unanswered for 2 s republishes Running");
    check(hostDiagnostics == 1 && !lastHostDiagnostic.isEmpty(), "the lost stop is logged once");
    nowMs = 22250;
    host.refresh();
    check(host.sessionState() == DecoderHost::Running && hostDiagnostics == 1, "Running stays without more logging");
    check(serviceStops == 2, "each stop dispatched one intent");

    /* Nothing to stop: Idle publishes nothing new. */
    retained_record() = {{"sessionId", 62}, {"state", "IDLE"}, {"reason", dsd_android::kRunCancelled}};
    running = false;
    host.refresh();
    changes = 0;
    host.stop();
    check(host.sessionState() == DecoderHost::Idle && changes == 0, "a stop from Idle publishes no phase");
    retained_record() = QJsonObject();
    serviceStops = 0;
}

/*
 * #708: the call recording store may rewrite a folder's journal only while no decoder can be writing it, which it
 * learns from the session reading inactive. The engine's run opens the journal and closes it before nativeRun returns,
 * and the service reports IDLE only after that (pinned by the session state test). A record that could not be read
 * names no state: while the engine runs it must not read as IDLE.
 */
static void
check_unreadable_record_keeps_the_session() {
    using dsd_android::DecoderHostAndroid;
    using dsd_qt::DecoderHost;
    DecoderHostAndroid host;
    // Each time the session went from active to inactive: what the recording store hears as its end.
    int ended = 0;
    bool active = false;
    QObject::connect(&host, &DecoderHost::sessionStateChanged, [&]() {
        ended += active && !host.sessionActive() ? 1 : 0;
        active = host.sessionActive();
    });
    nowMs = 40000;
    retained_record() = {{"sessionId", 90}, {"state", "RUNNING"}, {"reason", dsd_android::kRunPending}};
    running = true;
    host.refresh();
    check(host.sessionState() == DecoderHost::Running, "the session runs");
    retained_record() = QJsonObject();
    for (int poll = 0; poll < 3; ++poll) {
        nowMs += 250;
        host.refresh();
        check(host.sessionActive() && ended == 0, "an unreadable record while the engine runs keeps the session");
    }
    running = false;
    nowMs += 250;
    host.refresh();
    check(host.sessionState() == DecoderHost::Idle && ended == 1,
          "once the engine has returned (its journal closed), an unreadable record reads Idle");

    /* The first poll of a UI recreated over a running session: known, and active. */
    DecoderHostAndroid recreated;
    running = true;
    recreated.refresh();
    check(recreated.sessionStateKnown() && recreated.sessionActive(),
          "a first poll that cannot read the record while the engine runs knows the session as active");
    running = false;
}

static QString
temp_recording(const QTemporaryDir& dir, const char* name, const QByteArray& bytes) {
    const QString path = dir.filePath(QString::fromLatin1(name));
    write_file(path, bytes);
    return path;
}

/* #708 D1: the roots are the platform's, shared storage first; the base host's when it names none. */
static void
check_recording_roots() {
    dsd_android::DecoderHostAndroid host;
    reset_recordings();
    recordings().roots = {QStringLiteral("/storage/emulated/0/Android/data/app/files/Call recordings"),
                          QStringLiteral("/data/user/0/app/files/Call recordings")};
    check(host.recordingRoots() == recordings().roots, "the platform's roots, in its order");
    recordings().roots.clear();
    const QStringList fallback = host.recordingRoots();
    check(fallback.size() == 1 && fallback.first().endsWith(QStringLiteral("/Call recordings")),
          "the base host's root when the platform names none");
}

/* #708 D6/D10: the player's status reaches the host through its poll, for the play it opened only. */
static void
check_recording_playback() {
    using dsd_android::DecoderHostAndroid;
    using dsd_qt::DecoderHost;
    const QTemporaryDir dir;
    const QString a = temp_recording(dir, "a.wav", "RIFF");
    DecoderHostAndroid host;
    QStringList gone;
    QObject::connect(&host, &DecoderHost::recordingFilesMissing, [&gone](const QStringList& paths) { gone += paths; });
    reset_recordings();
    PlatformRecordings& r = recordings();
    check(host.playbackSupported() && host.property("playbackSupported").toBool(), "the Android host plays");

    r.nextGeneration = 7;
    host.playRecording(a);
    check(r.plays == 1 && r.played == a, "a play reaches the platform's player");
    check(host.playbackState() == DecoderHost::PlaybackPreparing && host.playbackPath() == a, "and reads Preparing");
    r.status = QStringLiteral("6\t2\t900\t60000\t");
    host.refresh();
    check(host.playbackState() == DecoderHost::PlaybackPreparing, "an earlier play's record is not this one's");
    r.status = QStringLiteral("7\tnot a state\t0\t0\t");
    host.refresh();
    check(host.playbackState() == DecoderHost::PlaybackPreparing, "a record that cannot be read says nothing");
    r.status = QStringLiteral("7\t2\t1500\t60000\t");
    host.refresh();
    check(host.playbackState() == DecoderHost::PlaybackPlaying && host.playbackPositionMs() == 1500
              && host.playbackDurationMs() == 60000,
          "the play's record is published");

    host.pausePlayback();
    host.resumePlayback();
    host.seekPlayback(90000);
    check(r.commands == QStringList({QStringLiteral("pause"), QStringLiteral("resume"), QStringLiteral("seek 90000")}),
          "pause, resume and seek reach the player");
    check(host.playbackPositionMs() == 60000, "a seek shows its target at once, inside the recording");

    r.status = QStringLiteral("7\t0\t60000\t60000\t");
    host.refresh();
    check(host.playbackState() == DecoderHost::PlaybackStopped && host.playbackPath() == a
              && host.playbackPositionMs() == 60000,
          "a play that ended reads Stopped at its end, its path kept");
    const int reads = r.statusReads;
    host.refresh();
    host.pausePlayback();
    check(r.statusReads == reads && r.commands.size() == 3, "an ended play is polled and steered no more");

    r.nextGeneration = 8;
    host.playRecording(a);
    r.status = QStringLiteral("8\t4\t0\t0\tmissing");
    host.refresh();
    check(host.playbackState() == DecoderHost::PlaybackFailed && host.playbackError() == QStringLiteral("missing")
              && gone == QStringList({a}),
          "a file the player found gone fails as missing and is reported");

    r.nextGeneration = 9;
    host.playRecording(a);
    r.commands.clear();
    host.stopPlayback();
    check(r.commands == QStringList({QStringLiteral("stop")}) && host.playbackState() == DecoderHost::PlaybackStopped
              && host.playbackPath().isEmpty(),
          "a stop reaches the player and reads Stopped with no path");
    r.status = QStringLiteral("9\t2\t10\t60000\t");
    host.refresh();
    check(host.playbackState() == DecoderHost::PlaybackStopped, "a stopped play's late record is not published");

    r.nextGeneration = 0;
    host.playRecording(a);
    check(host.playbackState() == DecoderHost::PlaybackFailed && host.playbackError() == QStringLiteral("failed"),
          "a play the platform cannot take fails");

    r.commands.clear();
    host.playRecording(dir.filePath(QStringLiteral("gone.wav")));
    check(r.commands == QStringList({QStringLiteral("stop")}) && r.plays == 4,
          "a missing file stops what plays and never reaches the player");
}

/* Pump the GUI thread until the host's transfer ends, for at most about five seconds. */
static bool
wait_for(const std::function<bool()>& done) {
    for (int i = 0; i < 1000 && !done(); ++i) {
        QCoreApplication::processEvents();
        QThread::msleep(5);
    }
    return done();
}

/* #708 D9: exports and saves run on the host's worker, count what is there, and report their end on the GUI thread. */
static void
check_recording_transfers() {
    using dsd_android::DecoderHostAndroid;
    using dsd_qt::DecoderHost;
    const QTemporaryDir dir;
    const QString a = temp_recording(dir, "a.wav", QByteArray(100, 'a'));
    const QString b = temp_recording(dir, "b.wav", QByteArray(250, 'b'));
    const QString gone = dir.filePath(QStringLiteral("gone.wav"));
    const QString document = QStringLiteral("content://com.example.documents/document/calls.zip");
    reset_recordings();
    PlatformRecordings& r = recordings();
    DecoderHostAndroid host;
    check(host.recordingExportSupported(), "the Android host exports ZIPs");
    QList<QList<int>> finished;
    QStringList reported;
    QObject::connect(
        &host, &DecoderHost::recordingTransferFinished,
        [&finished](int selection, int result, int missing) { finished.append({selection, result, missing}); });
    QObject::connect(&host, &DecoderHost::recordingFilesMissing,
                     [&reported](const QStringList& paths) { reported += paths; });

    r.hold = true;
    const QVariantMap selection = {{"selection", 5},
                                   {"paths", QStringList({a, gone, b})},
                                   {"names", QStringList({"S/a.wav", "S/gone.wav", "S/b.wav"})},
                                   {"bytes", 999}};
    check(host.exportRecordings(selection, document), "an export begins");
    check(r.entered.tryAcquire(1, 5000), "the export reaches the platform");
    check(with_transfer([](const PlatformRecordings& seen) { return seen.onWorker; }),
          "on the transfer worker, not the GUI thread");
    check(with_transfer([&](const PlatformRecordings& seen) {
              return seen.paths == QStringList({a, b}) && seen.names == QStringList({"S/a.wav", "S/b.wav"}) && seen.zip
                     && seen.reference == document;
          }),
          "with the files that are there, named in order");
    check(wait_for([&host]() { return host.recordingTransferTotalBytes() == 350; }),
          "its total is what the worker counted");
    r.progress = 120;
    host.refresh();
    check(host.recordingTransferActive() && host.recordingTransferBytes() == 120, "a poll publishes its progress");
    // The held platform call returns once it sees its own cancel, as RecordingExport's does.
    with_transfer([](PlatformRecordings& seen) { return seen.answer = {QStringLiteral("2")}; });
    host.cancelRecordingTransfer();
    check(r.cancelled.load() == r.transferId.load(), "a cancel names the running transfer");
    check(wait_for([&finished]() { return !finished.isEmpty(); }), "the export ends");
    check(finished.value(0) == QList<int>({5, DecoderHost::TransferCancelled, 1}) && reported == QStringList({gone}),
          "cancelled, the file found gone counted and reported");
    check(!host.recordingTransferActive(), "and nothing runs");
    r.hold = false;

    // A save is the same transfer of one file, not zipped; the platform's own finds come back too.
    finished.clear();
    reported.clear();
    with_transfer(
        [](PlatformRecordings& seen) { return seen.answer = {QStringLiteral("0"), QStringLiteral("/vanished.wav")}; });
    const qint64 first = r.transferId.load();
    check(host.saveRecording({{"selection", 6}, {"paths", QStringList({b})}, {"bytes", 250}}, document),
          "a save begins");
    check(wait_for([&finished]() { return !finished.isEmpty(); }), "the save ends");
    check(with_transfer([&](const PlatformRecordings& seen) {
              return !seen.zip && seen.paths == QStringList({b}) && seen.names.isEmpty();
          }) && r.transferId.load() > first,
          "a save is one file, not zipped, under a new id");
    check(finished.value(0) == QList<int>({6, DecoderHost::TransferDone, 1})
              && reported == QStringList({"/vanished.wav"}),
          "done, with what the platform found gone");

    // Nothing there: the platform is never asked.
    finished.clear();
    const int transfers = r.transfers.load();
    host.saveRecording({{"selection", 7}, {"paths", QStringList({gone})}}, document);
    check(wait_for([&finished]() { return !finished.isEmpty(); }), "a save of a gone file ends");
    check(finished.value(0) == QList<int>({7, DecoderHost::TransferMissing, 1}) && r.transfers.load() == transfers,
          "as missing, without the platform");

    // An answer the host cannot read fails.
    for (const QStringList& answer : {QStringList(), QStringList({QStringLiteral("x")}), QStringList({"9"})}) {
        finished.clear();
        with_transfer([&answer](PlatformRecordings& seen) { return seen.answer = answer; });
        host.exportRecordings({{"selection", 8}, {"paths", QStringList({a})}, {"names", QStringList({"S/a.wav"})}},
                              document);
        check(wait_for([&finished]() { return !finished.isEmpty(); }), "the export ends");
        check(finished.value(0).value(1) == DecoderHost::TransferFailed, "an answer that cannot be read fails");
    }

    // A file URL: the base host's copy, still off the GUI thread; no ZIP goes there.
    finished.clear();
    const QString copy = dir.filePath(QStringLiteral("copy.wav"));
    host.saveRecording({{"selection", 9}, {"paths", QStringList({b})}}, QUrl::fromLocalFile(copy).toString());
    check(wait_for([&finished]() { return !finished.isEmpty(); }), "a save to a file ends");
    check(finished.value(0) == QList<int>({9, DecoderHost::TransferDone, 0}) && read_file(copy) == QByteArray(250, 'b'),
          "a save to a file URL copies the recording");
    finished.clear();
    host.exportRecordings({{"selection", 10}, {"paths", QStringList({a})}, {"names", QStringList({"S/a.wav"})}},
                          QUrl::fromLocalFile(dir.filePath(QStringLiteral("x.zip"))).toString());
    check(wait_for([&finished]() { return !finished.isEmpty(); }), "a ZIP to a file ends");
    check(finished.value(0).value(1) == DecoderHost::TransferFailed, "a ZIP goes only into a platform document");

    // Earlier transfers released these without a test waiting on them.
    const auto drain = [&r]() {
        (void)r.entered.tryAcquire(r.entered.available());
        (void)r.returned.tryAcquire(r.returned.available());
    };
    // A host that goes away mid-transfer cancels it and waits for its worker.
    drain();
    {
        DecoderHostAndroid leaving;
        r.hold = true;
        with_transfer([](PlatformRecordings& seen) { return seen.answer = {QStringLiteral("2")}; });
        leaving.exportRecordings({{"selection", 11}, {"paths", QStringList({a})}, {"names", QStringList({"S/a.wav"})}},
                                 document);
        check(r.entered.tryAcquire(1, 5000), "the leaving host's export runs");
    }
    check(r.cancelled.load() == r.transferId.load(), "a host destroyed mid-transfer cancels it first");
    check(r.returned.tryAcquire(1, 5000), "and its job returned before the host was gone");

    drain();
    // A job blocked where a cancel cannot reach (a provider's pipe): the host waits a bounded time, says so, and goes;
    // the job finishes later and touches nothing of it.
    {
        hostDiagnostics = 0;
        r.ignoresCancel = true;
        DecoderHostAndroid stuck;
        stuck.exportRecordings({{"selection", 13}, {"paths", QStringList({a})}, {"names", QStringList({"S/a.wav"})}},
                               document);
        check(r.entered.tryAcquire(1, 5000), "the stuck host's export runs");
    }
    check(hostDiagnostics == 1 && lastHostDiagnostic.contains(QStringLiteral("did not stop")),
          "a host whose transfer would not stop goes after its bound, and logs it");
    check(r.returned.tryAcquire(1, 10000), "the abandoned job returns on its own");
    QCoreApplication::processEvents();
    r.ignoresCancel = false;
    r.hold = false;

    // After the teardown abort nothing begins.
    finished.clear();
    host.abortDocumentTransfers();
    const int before = r.transfers.load();
    check(!host.exportRecordings({{"selection", 12}, {"paths", QStringList({a})}, {"names", QStringList({"S/a.wav"})}},
                                 document),
          "an export after the abort does not begin");
    check(wait_for([&finished]() { return !finished.isEmpty(); }), "and still ends");
    check(finished.value(0).value(1) == DecoderHost::TransferFailed && r.transfers.load() == before,
          "failed, without the platform");
}

/* #708: a share reaches the platform only for a recording the store manages that is there. */
static void
check_recording_share() {
    using dsd_android::DecoderHostAndroid;
    const QTemporaryDir dir;
    const QString a = temp_recording(dir, "a.wav", "RIFF");
    DecoderHostAndroid host;
    reset_recordings();
    host.setRecordingIndex([&a](const QString& path) { return path == a; });
    check(host.shareRecording(a, QStringLiteral("Call recording")) && recordings().shares == 1
              && recordings().sharedPath == a && recordings().sharedTitle == QStringLiteral("Call recording"),
          "an indexed recording is shared through the platform");
    recordings().shareAnswer = false;
    check(!host.shareRecording(a, QStringLiteral("Call recording")), "the platform's refusal is the answer");
    check(!host.shareRecording(dir.filePath(QStringLiteral("other.wav")), QStringLiteral("x"))
              && recordings().shares == 2,
          "a path the store does not manage never reaches the platform");

    // A share the platform took can still fail on its way; the next refresh reports it.
    QStringList failed;
    QObject::connect(&host, &dsd_qt::DecoderHost::recordingShareFailed,
                     [&failed](const QString& path) { failed.append(path); });
    recordings().shareFailures = {a, dir.filePath(QStringLiteral("b.wav"))};
    host.refresh();
    check(failed == QStringList({a, dir.filePath(QStringLiteral("b.wav"))}), "each late share failure is reported");
    host.refresh();
    check(failed.size() == 2, "once");
}

static QString
health_state(const dsd_android::DecoderHostAndroid& host) {
    return host.receiverHealth().value(QStringLiteral("state")).toString();
}

/* A lifecycle record of @p session as DecoderService.lifecycleStatus() writes it. */
static QJsonObject
lifecycle_record(int session, const char* state, int reason, int end_cause, int receiver_source) {
    return {{"sessionId", session},
            {"state", state},
            {"reason", reason},
            {"endCause", end_cause},
            {"receiverSource", receiver_source}};
}

/* #647: why a session's receiver ended, scoped to that session. A receiver ending moves the phase to Stopping at once
 * and holds it through the session's cleanup (the rdio upload drain), however long, until its terminal record. */
static void
check_receiver_health() {
    using dsd_android::DecoderHostAndroid;
    using dsd_qt::DecoderHost;
    using namespace dsd_android;
    DecoderHostAndroid host;
    int results = 0;
    QObject::connect(&host, &DecoderHost::runResultChanged, [&results]() { ++results; });
    hostDiagnostics = 0;
    accessRequests = 0;
    startDispatches = 0;
    nowMs = 50000;
    retained_record() = {{"sessionId", 80}, {"state", "IDLE"}};
    running = false;
    host.refresh();
    check(health_state(host) == QStringLiteral("ok")
              && host.property("receiverHealth").toMap().value(QStringLiteral("state")) == QStringLiteral("ok"),
          "a host with no ending reads healthy, and QML reads it");

    /* An unplug in session 81: the detach broadcast's stop latched "detached" while the engine still drains. */
    retained_record() = lifecycle_record(81, "RUNNING", kRunPending, kEndNone, kReceiverRtl);
    running = true;
    host.refresh();
    check(host.sessionState() == DecoderHost::Running && health_state(host) == QStringLiteral("ok"),
          "a healthy session runs");
    results = 0;
    retained_record() = lifecycle_record(81, "RUNNING", kRunPending, kEndDetached, kReceiverRtl);
    host.refresh();
    const QVariantMap lost = host.receiverHealth();
    check(lost.value(QStringLiteral("state")) == QStringLiteral("lost")
              && lost.value(QStringLiteral("source")) == QStringLiteral("usb")
              && lost.value(QStringLiteral("session")).toLongLong() == 81,
          "a detach reads lost, with the session's receiver and id");
    check(results == 1, "the ending is published once");
    check(host.sessionState() == DecoderHost::Stopping, "the ending moves the phase to Stopping at once");
    for (int poll = 1; poll <= 120 && host.sessionState() == DecoderHost::Stopping; ++poll) {
        nowMs = 50000 + 250 * poll;
        host.refresh();
    }
    check(host.sessionState() == DecoderHost::Stopping && hostDiagnostics == 0,
          "30 s of cleanup hold Stopping, with no stop expiry");
    check(results == 1, "polls of the same ending publish nothing new");

    /* An access request while the ended session still winds down keeps its notice (the Monitor's LOST). */
    host.requestLocalDeviceAccessForSource(QStringLiteral("usb"));
    host.requestLocalDeviceAccess();
    host.refresh();
    check(accessRequests == 2 && health_state(host) == QStringLiteral("lost") && results == 1
              && host.sessionState() == DecoderHost::Stopping,
          "an access request during the hold keeps the notice until the session ends");

    /* Stop now during the hold still reaches the service; the phase and notice do not move. (That the native latch
     * keeps "detached" after it is IO_RTL_RUN_RESULT's "detach then Stop now stays a detach".) */
    const int stopsBefore = serviceStops;
    host.stop();
    check(serviceStops == stopsBefore + 1, "Stop now during the hold dispatches the user's stop");
    check(health_state(host) == QStringLiteral("lost") && host.sessionState() == DecoderHost::Stopping,
          "a host Stop during the hold changes nothing on screen");
    retained_record() = lifecycle_record(81, "IDLE", kRunCancelled, kEndDetached, kReceiverRtl);
    running = false;
    host.refresh();
    check(host.sessionState() == DecoderHost::Idle && health_state(host) == QStringLiteral("lost"),
          "the terminal record ends the hold and keeps the notice");

    /* After an Activity recreation a fresh host still shows the retained session's ending. */
    {
        DecoderHostAndroid recreated;
        recreated.refresh();
        check(health_state(recreated) == QStringLiteral("lost") && recreated.sessionState() == DecoderHost::Idle,
              "a recreated UI shows the ended session's notice");
    }

    /* A new access request acknowledges it, and the same retained record stays acknowledged. */
    results = 0;
    host.requestLocalDeviceAccessForSource(QStringLiteral("usb"));
    check(health_state(host) == QStringLiteral("ok") && accessRequests == 3 && results == 1,
          "an access request after the session ended clears the notice");
    host.refresh();
    check(health_state(host) == QStringLiteral("ok") && results == 1, "the retained record stays acknowledged");

    /* The acknowledgement outlives the Activity: a host recreated over the same retained record starts from it, so the
     * Listen pill does not read LOST over a receiver that is connected again. */
    {
        DecoderHostAndroid recreated;
        recreated.refresh();
        check(health_state(recreated) == QStringLiteral("ok") && recreated.sessionState() == DecoderHost::Idle,
              "a recreated UI does not show an ending the previous one acknowledged");
    }

    /* An Airspy's stream failure in session 82; start() clears it, and the old record stays cleared. */
    retained_record() = lifecycle_record(82, "RUNNING", kRunPending, kEndReceiverFailed, kReceiverAirspy);
    running = true;
    host.refresh();
    const QVariantMap failed = host.receiverHealth();
    check(failed.value(QStringLiteral("state")) == QStringLiteral("failed")
              && failed.value(QStringLiteral("source")) == QStringLiteral("airspy")
              && failed.value(QStringLiteral("session")).toLongLong() == 82,
          "a committed receiver failure reads failed");
    check(host.sessionState() == DecoderHost::Stopping, "a receiver failure holds Stopping too");

    /* A start refused while the session still stops keeps the notice. */
    check(!host.start({}) && health_state(host) == QStringLiteral("failed") && startDispatches == 0,
          "a refused start keeps the notice");
    retained_record() = lifecycle_record(82, "IDLE", kRunCompleted, kEndReceiverFailed, kReceiverAirspy);
    running = false;
    host.refresh();
    /* #782: the refusal is the attempt's failure, and stands past the stopped session's end until a dismissal or the
     * next start; the notice, which Main words ahead of it, stays with it. */
    check(host.sessionState() == DecoderHost::Failed && host.inputFailureKind() == kAttemptPreviousStopping
              && health_state(host) == QStringLiteral("failed"),
          "the failed receiver's notice survives the terminal record");
    check(host.start({}) && startDispatches == 1, "a start is dispatched");
    check(health_state(host) == QStringLiteral("ok"), "start() clears the notice");
    host.refresh();
    check(health_state(host) == QStringLiteral("ok") && host.sessionState() == DecoderHost::Starting,
          "the previous session's record stays cleared while the start is in flight");

    /* The user's Stop, then an unplug during cleanup: the native latch keeps the user's cause, so no notice. */
    retained_record() = lifecycle_record(83, "RUNNING", kRunPending, kEndNone, kReceiverRtl);
    running = true;
    host.refresh();
    check(host.sessionState() == DecoderHost::Running, "the next session runs");
    host.stop();
    for (const char* state : {"RUNNING", "STOPPING"}) {
        retained_record() = lifecycle_record(83, state, kRunPending, kEndUser, kReceiverRtl);
        host.refresh();
        check(health_state(host) == QStringLiteral("ok") && host.sessionState() == DecoderHost::Stopping,
              "a user stop shows no receiver notice");
    }
    retained_record() = lifecycle_record(83, "IDLE", kRunCancelled, kEndUser, kReceiverRtl);
    running = false;
    host.refresh();
    check(health_state(host) == QStringLiteral("ok") && host.sessionState() == DecoderHost::Idle,
          "a user stop ends Idle and healthy");
    retained_record() = lifecycle_record(84, "IDLE", kRunCancelled, kEndLifecycle, kReceiverRtl);
    host.refresh();
    check(health_state(host) == QStringLiteral("ok"), "a lifecycle stop shows no receiver notice");

    /* An access request while a session runs healthy acknowledges nothing it has not shown. */
    const int before = accessRequests;
    retained_record() = lifecycle_record(85, "RUNNING", kRunPending, kEndNone, kReceiverRtl);
    running = true;
    host.refresh();
    host.requestLocalDeviceAccess();
    check(accessRequests == before + 1, "the access request is dispatched");
    retained_record() = lifecycle_record(85, "RUNNING", kRunPending, kEndDetached, kReceiverRtl);
    host.refresh();
    check(health_state(host) == QStringLiteral("lost") && host.sessionState() == DecoderHost::Stopping,
          "a later unplug of that session still shows");
    retained_record() = lifecycle_record(85, "IDLE", kRunCancelled, kEndDetached, kReceiverRtl);
    running = false;
    host.refresh();

    /* A detach that stopped a session with no local receiver (rtl_tcp) names no receiver: no notice and no hold. */
    retained_record() = lifecycle_record(86, "RUNNING", kRunPending, kEndNone, kReceiverNone);
    running = true;
    host.refresh();
    for (const int cause : {kEndDetached, kEndReceiverFailed}) {
        retained_record() = lifecycle_record(86, "RUNNING", kRunPending, cause, kReceiverNone);
        host.refresh();
        check(health_state(host) == QStringLiteral("ok") && host.sessionState() == DecoderHost::Running,
              "an ending without a local receiver shows no notice and holds nothing");
    }
    retained_record() = lifecycle_record(86, "IDLE", kRunCancelled, kEndDetached, kReceiverNone);
    running = false;
    host.refresh();
    check(health_state(host) == QStringLiteral("ok") && host.sessionState() == DecoderHost::Idle, "and ends healthy");

    /* A stream failure ends its run Failed, with the run's own error text. Acknowledging the receiver's ending, by
     * any access request, retires that failure too: one ended session is one notice, never a second failure card. */
    const auto fail_by_receiver = [](int session) {
        QJsonObject record = lifecycle_record(session, "IDLE", kRunFailed, kEndReceiverFailed, kReceiverRtl);
        record.insert("lastError", "Decoder run failed (1); the receiver stopped sending samples");
        return record;
    };
    retained_record() = lifecycle_record(87, "RUNNING", kRunPending, kEndReceiverFailed, kReceiverRtl);
    running = true;
    host.refresh();
    retained_record() = fail_by_receiver(87);
    running = false;
    host.refresh();
    check(host.sessionState() == DecoderHost::Failed && !host.failureText().isEmpty()
              && health_state(host) == QStringLiteral("failed"),
          "a receiver's stream failure ends Failed, with its notice");
    const int stopsBeforeAck = serviceStops;
    for (const bool forSource : {false, true}) {
        if (forSource) {
            retained_record() = lifecycle_record(88, "RUNNING", kRunPending, kEndReceiverFailed, kReceiverRtl);
            running = true;
            host.refresh();
            retained_record() = fail_by_receiver(88);
            running = false;
            host.refresh();
            check(host.sessionState() == DecoderHost::Failed, "the next stream failure ends Failed");
        }
        int phases = 0;
        bool noticeKept = true;
        const auto watch = QObject::connect(&host, &DecoderHost::sessionStateChanged, [&]() {
            ++phases;
            // The failure is retired while the notice still stands, so its text is never shown alone.
            noticeKept = noticeKept && health_state(host) != QStringLiteral("ok");
        });
        if (forSource) {
            host.requestLocalDeviceAccessForSource(QStringLiteral("usb"));
        } else {
            host.requestLocalDeviceAccess();
        }
        QObject::disconnect(watch);
        check(host.sessionState() == DecoderHost::Idle && host.failureText().isEmpty()
                  && health_state(host) == QStringLiteral("ok"),
              "acknowledging the ending retires the session's failure with it");
        check(phases == 1 && noticeKept, "the failure goes first, while the notice still stands");
        host.refresh();
        check(host.sessionState() == DecoderHost::Idle && host.failureText().isEmpty()
                  && health_state(host) == QStringLiteral("ok"),
              "the retained Failed record stays acknowledged");
    }
    check(serviceStops == stopsBeforeAck, "acknowledging stops nothing");

    /* A UI recreated while the session still ran, which then fails: the same holds with no attempt of its own. (A UI
     * recreated after the failure adopts the record as Idle, so it has no failure to retire.) */
    retained_record() = lifecycle_record(89, "RUNNING", kRunPending, kEndNone, kReceiverRtl);
    running = true;
    host.refresh();
    {
        DecoderHostAndroid recreated;
        recreated.refresh();
        check(recreated.sessionState() == DecoderHost::Running, "a recreated UI adopts the running session");
        retained_record() = lifecycle_record(89, "RUNNING", kRunPending, kEndReceiverFailed, kReceiverRtl);
        recreated.refresh();
        retained_record() = fail_by_receiver(89);
        running = false;
        recreated.refresh();
        check(recreated.sessionState() == DecoderHost::Failed && health_state(recreated) == QStringLiteral("failed"),
              "the recreated UI shows the failed session's notice");
        recreated.requestLocalDeviceAccessForSource(QStringLiteral("usb"));
        check(recreated.sessionState() == DecoderHost::Idle && recreated.failureText().isEmpty()
                  && health_state(recreated) == QStringLiteral("ok"),
              "a recreated Reconnect leaves no second card behind");
    }

    /* A Failed session its receiver did not end keeps its failure: Retry only asks for access (WP-D5). */
    retained_record() = lifecycle_record(90, "RUNNING", kRunPending, kEndNone, kReceiverRtl);
    running = true;
    host.refresh();
    QJsonObject openFailed = lifecycle_record(90, "IDLE", kRunFailed, kEndNone, kReceiverRtl);
    openFailed.insert("lastError", "Radio open failed");
    retained_record() = std::move(openFailed);
    running = false;
    host.refresh();
    host.requestLocalDeviceAccess();
    check(host.sessionState() == DecoderHost::Failed && host.failureText() == QStringLiteral("Radio open failed")
              && health_state(host) == QStringLiteral("ok"),
          "an access request leaves another failure in place");

    retained_record() = QJsonObject();
    serviceStops = 0;
    hostDiagnostics = 0;
}

/* B11: a native claim error belongs to the receiver its session used, and shows only while that receiver is ready. */
static void
check_claim_error_scope() {
    using dsd_android::DecoderHostAndroid;
    using dsd_qt::DecoderHost;
    using namespace dsd_android;
    const QJsonObject airspy{
        {"ready", true}, {"source", "airspy"}, {"serial", "A1"}, {"name", "Airspy"}, {"text", "Ready: Airspy"}};
    DecoderHostAndroid host;
    usb_record() = airspy;
    retained_record() = {{"sessionId", 90}, {"state", "IDLE"}};
    running = false;
    host.refresh();
    retained_record() = lifecycle_record(91, "RUNNING", kRunPending, kEndNone, kReceiverAirspy);
    running = true;
    host.refresh();
    QJsonObject failed = lifecycle_record(91, "IDLE", kRunFailed, kEndNone, kReceiverAirspy);
    failed.insert("deviceError", -6);
    failed.insert("lastError", "Decoder run failed (1); USB open/claim error -6");
    retained_record() = std::move(failed);
    running = false;
    host.refresh();
    check(host.localDeviceFailureKind() == DecoderHost::DeviceBusy
              && host.localDeviceStatus().contains(QStringLiteral("could not claim Airspy")),
          "an Airspy session's claim error shows for that Airspy");

    usb_record() = {
        {"ready", true}, {"source", "usb"}, {"serial", "R1"}, {"name", "RTL-SDR"}, {"text", "Ready: RTL-SDR"}};
    host.refresh();
    check(host.localDeviceFailureKind() == DecoderHost::NoDeviceFailure
              && host.localDeviceStatus() == QStringLiteral("Ready: RTL-SDR"),
          "an Airspy session's claim error never shows for a later RTL-SDR");
    QJsonObject other = airspy;
    other.insert("serial", "A2");
    usb_record() = std::move(other);
    host.refresh();
    check(host.localDeviceFailureKind() == DecoderHost::NoDeviceFailure
              && host.localDeviceStatus() == QStringLiteral("Ready: Airspy"),
          "nor for another Airspy");
    usb_record() = airspy;
    host.refresh();
    check(host.localDeviceFailureKind() == DecoderHost::DeviceBusy, "the claim error shows again for its own Airspy");

    usb_record() = QJsonObject();
    retained_record() = QJsonObject();
}

/* A broker record of a ready RTL-SDR connection: its attachment (device name, attach epoch) and connection number. */
static QJsonObject
rtl_connection(const char* device, qint64 epoch, qint64 connection, const char* serial) {
    return {{"ready", true},     {"source", "usb"},          {"serial", serial},
            {"name", "RTL-SDR"}, {"text", "Ready: RTL-SDR"}, {"device", device},
            {"epoch", epoch},    {"connection", connection}};
}

/* A failed RTL-SDR session's terminal record, naming the connection the session took. */
static QJsonObject
claim_failure(int session, const char* device, qint64 epoch, qint64 connection) {
    using namespace dsd_android;
    QJsonObject failed = lifecycle_record(session, "IDLE", kRunFailed, kEndNone, kReceiverRtl);
    failed.insert("deviceError", -6);
    failed.insert("lastError", "Decoder run failed (1); USB open/claim error -6");
    failed.insert("usbDevice", device);
    failed.insert("usbEpoch", epoch);
    failed.insert("usbConnection", connection);
    return failed;
}

/* #782: a session's claim error belongs to the connection that session took, not to whatever shares its kind and
 * serial: two dongles with the same (or no) serial, and a replacement connection to the same dongle, never show it. */
static void
check_claim_error_connection() {
    using dsd_android::DecoderHostAndroid;
    using dsd_qt::DecoderHost;
    using namespace dsd_android;
    const char* kA = "/dev/bus/usb/001/002";
    const char* kB = "/dev/bus/usb/001/003";
    for (const char* serial : {"00000001", ""}) {
        DecoderHostAndroid host;
        usb_record() = rtl_connection(kA, 1, 1, serial);
        retained_record() = {{"sessionId", 120}, {"state", "IDLE"}};
        running = false;
        host.refresh();
        retained_record() = claim_failure(121, kA, 1, 1);
        host.refresh();
        check(host.localDeviceFailureKind() == DecoderHost::DeviceBusy, "the claim error shows for its own dongle");
        // Another dongle of the same kind, with the same serial or none, is connected instead.
        usb_record() = rtl_connection(kB, 2, 2, serial);
        host.refresh();
        check(host.localDeviceFailureKind() == DecoderHost::NoDeviceFailure
                  && host.localDeviceStatus() == QStringLiteral("Ready: RTL-SDR"),
              *serial ? "a dongle with the same serial never shows another's claim error"
                      : "a dongle with no serial never shows another's claim error");
        usb_record() = rtl_connection(kA, 1, 1, serial);
        host.refresh();
        check(host.localDeviceFailureKind() == DecoderHost::DeviceBusy, "it shows again for its own connection");
    }

    /* The session's terminal result arrives after the next start reopened the same dongle (a new connection): it
     * concerns the connection the session took, which is gone. */
    DecoderHostAndroid host;
    usb_record() = rtl_connection(kA, 1, 3, "00000001");
    retained_record() = {{"sessionId", 130}, {"state", "IDLE"}};
    host.refresh();
    QJsonObject run = lifecycle_record(131, "RUNNING", kRunPending, kEndNone, kReceiverRtl);
    run.insert("usbDevice", kA);
    run.insert("usbEpoch", 1);
    run.insert("usbConnection", 3);
    retained_record() = run;
    running = true;
    host.refresh();
    usb_record() = rtl_connection(kA, 1, 4, "00000001");
    retained_record() = claim_failure(131, kA, 1, 3);
    running = false;
    host.refresh();
    check(host.localDeviceFailureKind() == DecoderHost::NoDeviceFailure
              && host.localDeviceStatus() == QStringLiteral("Ready: RTL-SDR"),
          "a late result never lands on the replacement connection");

    usb_record() = QJsonObject();
    retained_record() = QJsonObject();
}

/* #782: the service's own failure of a start that could not take its USB receiver: the attempt's kind and text reach
 * QML, and the broker's record, now not ready, says what the receiver needs. */
static void
check_receiver_take_failures() {
    using dsd_android::DecoderHostAndroid;
    using dsd_qt::DecoderHost;
    using namespace dsd_android;

    struct Case {
        const char* wire;
        int kind;
        const char* text;
        const char* broker_kind;
        int pill;
    };

    const Case cases[] = {
        {"usb_gone", kAttemptUsbGone, "The USB receiver is no longer connected", "detached",
         DecoderHost::DeviceDetached},
        {"usb_permission", kAttemptUsbPermission, "The USB receiver needs permission again", "permission",
         DecoderHost::DevicePermission},
        {"usb_open_failed", kAttemptUsbOpenFailed, "The USB receiver could not be reopened", "open_failed",
         DecoderHost::DeviceOpenFailed},
        {"previous_stopping", kAttemptPreviousStopping, "A previous session is still stopping", "", -1},
    };
    for (const Case& c : cases) {
        DecoderHostAndroid host;
        usb_record() = rtl_connection("/dev/bus/usb/001/002", 1, 1, "00000001");
        retained_record() = {{"sessionId", 140}, {"state", "IDLE"}, {"reason", kRunCompleted}};
        running = false;
        host.refresh();
        check(host.start(QStringList{QStringLiteral("-i"), QStringLiteral("rtl:0")}), "the start is dispatched");
        retained_record() = {{"sessionId", 141},      {"state", "IDLE"},
                             {"reason", kRunFailed},  {"attemptId", (qint64)lastStartAttempt},
                             {"failureKind", c.wire}, {"lastError", c.text}};
        if (c.pill >= 0) {
            usb_record() = {{"ready", false}, {"source", ""}, {"text", "Device detached"}, {"kind", c.broker_kind}};
        }
        host.refresh();
        check(host.sessionState() == DecoderHost::Failed && host.inputFailureKind() == c.kind
                  && host.inputFailureCode() == 0 && host.failureText() == QLatin1String(c.text),
              "the service's take failure is the attempt's failure, with its kind and text");
        if (c.pill >= 0) {
            check(!host.localDeviceReady() && host.localDeviceFailureKind() == c.pill,
                  "the broker reads not ready, with what the receiver needs");
        }
        host.dismissFailure();
    }
    usb_record() = QJsonObject();
    retained_record() = QJsonObject();
}

/* The USB record's outstanding access request reaches QML through the host (#647 WP3). */
static void
check_request_record() {
    using dsd_android::DecoderHostAndroid;
    using dsd_qt::DecoderHost;
    DecoderHostAndroid host;
    int changes = 0;
    QObject::connect(&host, &DecoderHost::localDeviceChanged, [&changes]() { ++changes; });
    host.refresh();
    check(!host.localDeviceRequestPending() && host.localDeviceRequestedSource().isEmpty(),
          "no request before the record names one");
    usb_record() = {{"ready", false},
                    {"text", "Waiting for permission"},
                    {"pending", true},
                    {"requestedSource", "airspy"},
                    {"requestedSerial", "A1"}};
    changes = 0;
    host.refresh();
    check(host.localDeviceRequestPending() && host.localDeviceRequestedSource() == QStringLiteral("airspy")
              && changes == 1,
          "a pending request is published once");
    check(host.property("localDeviceRequestPending").toBool()
              && host.property("localDeviceRequestedSource").toString() == QStringLiteral("airspy"),
          "QML reads the pending request");
    host.refresh();
    check(changes == 1, "the same record publishes nothing new");
    usb_record().insert("pending", false);
    host.refresh();
    check(!host.localDeviceRequestPending() && changes == 2, "the answered request is published");
    usb_record() = QJsonObject();
}

/* #782: the last session failed on the network; the next start, a USB one, never reaches the service. Its card must
 * say the start was not delivered, never the old session's network failure, however often the old record is read. */
static void
check_undelivered_start_after_network_failure() {
    using dsd_android::DecoderHostAndroid;
    using dsd_qt::DecoderHost;
    using namespace dsd_android;
    DecoderHostAndroid host;
    hostDiagnostics = 0;
    serviceStops = 0;
    startDispatches = 0;
    retirements.clear();
    serviceTookAttempt = false;
    nowMs = 100000;
    retained_record() = {{"sessionId", 90}, {"state", "IDLE"}};
    running = false;
    host.refresh();
    retained_record() = {{"sessionId", 91}, {"state", "RUNNING"}, {"reason", kRunPending}};
    running = true;
    host.refresh();
    const QJsonObject network_failure = {{"sessionId", 91},      {"state", "IDLE"},
                                         {"reason", kRunFailed}, {"lastError", "Decoder run failed (1)"},
                                         {"inputFailure", 1},    {"inputError", 111}};
    retained_record() = network_failure;
    running = false;
    host.refresh();
    check(host.sessionState() == DecoderHost::Failed && host.inputFailureKind() == 1,
          "the network session failed with its kind");

    check(host.start(QStringList{QStringLiteral("-i"), QStringLiteral("rtl:0")}), "the USB start is dispatched");
    check(startDispatches == 1, "one start intent");
    for (uint64_t waited = 250; waited < 2000; waited += 250) {
        nowMs = 100000 + waited;
        host.refresh();
        check(host.sessionState() == DecoderHost::Starting, "the start waits out its grace");
    }
    check(hostDiagnostics == 0, "nothing is logged under the grace");
    nowMs = 102000;
    host.refresh();
    check(host.sessionState() == DecoderHost::Failed, "an undelivered start fails after 2 s");
    check(host.inputFailureKind() == kAttemptUndelivered && host.inputFailureCode() == 0,
          "the failure is the undelivered start's, not the old network failure");
    check(host.failureText() == QStringLiteral("The decoder service did not respond to the start"),
          "the failure text is the undelivered start's");
    check(hostDiagnostics == 1
              && lastHostDiagnostic
                     == QStringLiteral(
                         "Start was not delivered to the decoder service within 2000 ms (service state IDLE)"),
          "the expiry is logged with its wait and the service state");
    for (int poll = 1; poll <= 8; ++poll) {
        nowMs = 102000 + 250 * poll;
        host.refresh();
        check(host.sessionState() == DecoderHost::Failed && host.inputFailureKind() == kAttemptUndelivered
                  && host.inputFailureCode() == 0,
              "later polls of the old record never bring the network failure back");
    }
    check(retirements.size() == 2 && retirements.last() == lastStartAttempt,
          "the expiry retired the attempt it gave up on");
    host.stop();
    check(host.sessionState() == DecoderHost::Idle && host.failureText().isEmpty(), "Dismiss acknowledges it");
    check(serviceStops == 0, "acknowledging an undelivered start stops nothing");
    retained_record() = QJsonObject();
}

/* #782: a start refused because the previous session is still stopping says so, and keeps saying so. */
static void
check_refused_start() {
    using dsd_android::DecoderHostAndroid;
    using dsd_qt::DecoderHost;
    using namespace dsd_android;
    DecoderHostAndroid host;
    serviceStops = 0;
    startDispatches = 0;
    nowMs = 200000;
    retained_record() = {{"sessionId", 70}, {"state", "IDLE"}};
    running = false;
    host.refresh();
    /* The service is stopping a session whose receiver failed; the UI has not polled it yet. */
    const QJsonObject stopping = {
        {"sessionId", 71}, {"state", "STOPPING"}, {"reason", kRunPending}, {"inputFailure", 1}, {"inputError", 111}};
    retained_record() = stopping;
    check(!host.start(QStringList{QStringLiteral("-i"), QStringLiteral("rtl:0")}), "the start is refused");
    check(startDispatches == 0, "a refused start dispatches nothing");
    check(host.sessionState() == DecoderHost::Failed, "the refusal is a failure on screen");
    check(host.inputFailureKind() == kAttemptPreviousStopping && host.inputFailureCode() == 0,
          "the refusal has its own kind");
    check(host.failureText() == QStringLiteral("A previous session is still stopping"), "and its own text");
    for (int poll = 1; poll <= 4; ++poll) {
        nowMs = 200000 + 250 * poll;
        host.refresh();
        check(host.sessionState() == DecoderHost::Failed && host.inputFailureKind() == kAttemptPreviousStopping,
              "polls of the stopping session keep the refusal");
    }
    retained_record() = {{"sessionId", 71}, {"state", "IDLE"}, {"reason", kRunCancelled}};
    host.refresh();
    check(host.sessionState() == DecoderHost::Failed && host.inputFailureKind() == kAttemptPreviousStopping,
          "the stopped session's end keeps the refusal until it is dismissed");
    host.stop();
    check(host.sessionState() == DecoderHost::Idle && host.failureText().isEmpty() && host.inputFailureKind() == 0,
          "Retry or Dismiss clears it");
    check(serviceStops == 0, "clearing the refusal stops nothing");
    check(lastHostDiagnostic.startsWith(QStringLiteral("Start attempt "))
              && lastHostDiagnostic.endsWith(QStringLiteral(" refused: the decoder service is STOPPING")),
          "the refusal is logged with the service state");
    retained_record() = QJsonObject();
}

/* #782: every start carries a new attempt id, and retires every earlier attempt before the service state is read. */
static void
check_attempt_identity() {
    using dsd_android::DecoderHostAndroid;
    using dsd_qt::DecoderHost;
    using namespace dsd_android;
    DecoderHostAndroid host;
    int results = 0;
    QObject::connect(&host, &DecoderHost::runResultChanged, [&results]() { ++results; });
    hostDiagnostics = 0;
    serviceStops = 0;
    startDispatches = 0;
    retirements.clear();
    serviceTookAttempt = false;
    nowMs = 300000;
    retained_record() = {{"sessionId", 100}, {"state", "IDLE"}};
    running = false;
    host.refresh();

    const int reads = lifecycleReads;
    check(host.start(QStringList{QStringLiteral("-i"), QStringLiteral("rtl:0")}), "the first start is dispatched");
    const uint64_t first = lastStartAttempt;
    check(first != 0 && retirements == QList<uint64_t>{first - 1}, "a start retires every attempt before it");
    check(lifecycleReadsAtRetire == reads, "before it reads the service state");
    /* The service took it after all: the IDLE record the expiry came from was older than the retirement. */
    serviceTookAttempt = true;
    nowMs = 302000;
    host.refresh();
    check(host.sessionState() == DecoderHost::Starting && host.failureText().isEmpty() && host.inputFailureKind() == 0,
          "an attempt the service took is no failure");
    check(lastHostDiagnostic
              == QStringLiteral("Start attempt %1 was taken by the decoder service after all").arg(first),
          "the late take is logged");
    retained_record() = {
        {"sessionId", 101}, {"state", "RUNNING"}, {"reason", kRunPending}, {"attemptId", (qint64)first}};
    running = true;
    nowMs = 302250;
    host.refresh();
    check(host.sessionState() == DecoderHost::Running, "and it runs");

    /* The service fails the next attempt before its configure, with a kind of its own. */
    retained_record() = {
        {"sessionId", 101}, {"state", "IDLE"}, {"reason", kRunCompleted}, {"attemptId", (qint64)first}};
    running = false;
    host.refresh();
    serviceTookAttempt = false;
    check(host.start(QStringList{QStringLiteral("-i"), QStringLiteral("rtl:0")}), "the second start is dispatched");
    const uint64_t second = lastStartAttempt;
    check(second > first && retirements.last() == second - 1, "the second start supersedes the first");
    const QJsonObject gone = {{"sessionId", 102},          {"state", "IDLE"},
                              {"reason", kRunFailed},      {"attemptId", (qint64)second},
                              {"failureKind", "usb_gone"}, {"lastError", "USB receiver gone"}};
    retained_record() = gone;
    results = 0;
    host.refresh();
    check(host.sessionState() == DecoderHost::Failed && host.inputFailureKind() == kAttemptUsbGone
              && host.failureText() == QStringLiteral("USB receiver gone") && results > 0,
          "the service's failure kind for this attempt is published");
    QJsonObject other = gone;
    other.insert(QStringLiteral("attemptId"), static_cast<qint64>(second + 7));
    other.insert(QStringLiteral("failureKind"), QStringLiteral("usb_permission"));
    retained_record() = other;
    host.refresh();
    check(host.inputFailureKind() == kAttemptUsbGone, "another attempt's failure kind replaces nothing");
    retained_record() = gone;
    const int before_dismiss = retirements.size();
    host.dismissFailure();
    check(retirements.size() == before_dismiss + 1 && retirements.last() == second, "a dismissal retires the attempt");
    check(host.sessionState() == DecoderHost::Idle && host.failureText().isEmpty() && host.inputFailureKind() == 0,
          "and clears its failure");
    host.refresh();
    check(host.sessionState() == DecoderHost::Idle && host.inputFailureKind() == 0,
          "the dismissed attempt's record does not bring it back");

    /* A start the host could not hand over: no kind, and not the old session's. */
    retained_record() = {
        {"sessionId", 103}, {"state", "IDLE"}, {"reason", kRunFailed}, {"inputFailure", 1}, {"inputError", 111}};
    host.refresh();
    host.stop();
    startDispatchFailure = QStringLiteral("No Android context");
    check(!host.start(QStringList{QStringLiteral("-i"), QStringLiteral("rtl:0")}), "an undispatchable start fails");
    startDispatchFailure.clear();
    check(host.sessionState() == DecoderHost::Failed && host.failureText() == QStringLiteral("No Android context")
              && host.inputFailureKind() == 0 && host.inputFailureCode() == 0,
          "with its own text and no kind");
    host.refresh();
    check(host.inputFailureKind() == 0 && host.failureText() == QStringLiteral("No Android context"),
          "the old record's network failure stays out");

    /* Dismissing a failure the service reported retires the attempt and leaves the failure as it was. */
    host.stop();
    check(host.start(QStringList{QStringLiteral("-i"), QStringLiteral("rtl:0")}), "a third start");
    const uint64_t third = lastStartAttempt;
    retained_record() = {{"sessionId", 104},  {"state", "IDLE"},  {"reason", kRunFailed},
                         {"inputFailure", 7}, {"inputError", -6}, {"attemptId", (qint64)third}};
    host.refresh();
    check(host.sessionState() == DecoderHost::Failed && host.inputFailureKind() == 7 && host.inputFailureCode() == -6,
          "a receiver that could not be opened reaches the card as a device failure");
    host.dismissFailure();
    check(retirements.last() == third && host.sessionState() == DecoderHost::Failed && host.inputFailureKind() == 7,
          "a dismissal leaves a run's own failure to the card");
    check(serviceStops == 0, "nothing here stops the service");
    retained_record() = QJsonObject();
    serviceStops = 0;
}

/* #782 (Task 4 minor 1): the grace ran out on a start the service took after all, so the retirement cancels it while it
 * configures. Dismissing or retrying the undelivered start then shows the attempt winding down, never the expiry's
 * stale failure or "Starting…" for a session that will not run, until its cancelled end. */
static void
check_dismissed_undelivered_start_winds_down() {
    using dsd_android::DecoderHostAndroid;
    using dsd_qt::DecoderHost;
    using namespace dsd_android;
    for (const bool retry : {false, true}) {
        DecoderHostAndroid host;
        serviceStops = 0;
        retirements.clear();
        serviceTookAttempt = false;
        nowMs = 400000;
        retained_record() = {{"sessionId", 120}, {"state", "IDLE"}};
        running = false;
        host.refresh();
        check(host.start(QStringList{QStringLiteral("-i"), QStringLiteral("rtl:0")}), "the start is dispatched");
        const uint64_t attempt = lastStartAttempt;
        nowMs = 402000;
        host.refresh();
        check(host.sessionState() == DecoderHost::Failed && host.inputFailureKind() == kAttemptUndelivered,
              "the expired start is undelivered");
        /* The service took it between the IDLE record and the retirement, which cancels it. */
        retained_record() = {
            {"sessionId", 121}, {"state", "STARTING"}, {"reason", kRunPending}, {"attemptId", (qint64)attempt}};
        if (retry) {
            host.stop();
        } else {
            host.dismissFailure();
        }
        check(host.sessionState() == DecoderHost::Stopping && host.inputFailureKind() == 0,
              "a dismissed undelivered start the service still holds winds down");
        check(serviceStops == 0, "dismissing it sends no stop: the retirement cancels it");
        nowMs = 402250;
        host.refresh();
        check(host.sessionState() == DecoderHost::Stopping, "its configuring record is no start on screen");
        retained_record() = {
            {"sessionId", 121}, {"state", "IDLE"}, {"reason", kRunCancelled}, {"attemptId", (qint64)attempt}};
        nowMs = 402500;
        host.refresh();
        check(host.sessionState() == DecoderHost::Idle && host.failureText().isEmpty(), "its cancelled end is idle");
    }
    retained_record() = QJsonObject();
}

/* #782 (Task 4 minors 2 and 3): a start refused while another is on screen, starting or running, through an entry
 * point no QML guard covers, neither retires that attempt nor publishes its refusal over it, and the attempt on screen
 * is still the one the host follows. */
static void
check_refused_start_leaves_the_run_on_screen() {
    using dsd_android::DecoderHostAndroid;
    using dsd_qt::DecoderHost;
    using namespace dsd_android;
    DecoderHostAndroid host;
    serviceStops = 0;
    serviceTookAttempt = false;
    nowMs = 500000;
    retained_record() = {{"sessionId", 130}, {"state", "IDLE"}};
    running = false;
    host.refresh();
    check(host.start(QStringList{QStringLiteral("-i"), QStringLiteral("rtl:0")}), "the first start is dispatched");
    const uint64_t first = lastStartAttempt;
    retained_record() = {
        {"sessionId", 131}, {"state", "STARTING"}, {"reason", kRunPending}, {"attemptId", (qint64)first}};
    nowMs = 500250;
    host.refresh();
    check(host.sessionState() == DecoderHost::Starting, "the first start is starting");
    retirements.clear();
    check(!host.start(QStringList{QStringLiteral("-i"), QStringLiteral("rtl:0")}), "a second start is refused");
    check(retirements.isEmpty(), "a refused start retires nothing while another is starting");
    check(host.sessionState() == DecoderHost::Starting && host.inputFailureKind() == 0 && host.failureText().isEmpty(),
          "the refusal is not published over a starting run");
    /* The attempt on screen is still the one followed: a failure its record names before its configure is its own. */
    retained_record() = {{"sessionId", 131},           {"state", "IDLE"},           {"reason", kRunFailed},
                         {"attemptId", (qint64)first}, {"failureKind", "usb_gone"}, {"lastError", "USB receiver gone"}};
    host.refresh();
    check(host.sessionState() == DecoderHost::Failed && host.inputFailureKind() == kAttemptUsbGone,
          "the starting attempt keeps its own failure");

    host.stop();
    retirements.clear();
    check(host.start(QStringList{QStringLiteral("-i"), QStringLiteral("rtl:0")}), "a start from a settled screen");
    const uint64_t second = lastStartAttempt;
    check(retirements == QList<uint64_t>{second - 1}, "a start from a settled screen supersedes every earlier one");
    retained_record() = {
        {"sessionId", 132}, {"state", "RUNNING"}, {"reason", kRunPending}, {"attemptId", (qint64)second}};
    running = true;
    host.refresh();
    check(host.sessionState() == DecoderHost::Running, "it runs");
    retirements.clear();
    check(!host.start(QStringList{QStringLiteral("-i"), QStringLiteral("rtl:0")}), "a start while it runs is refused");
    check(retirements.isEmpty(), "a refused start retires nothing while another runs");
    check(host.sessionState() == DecoderHost::Running && host.inputFailureKind() == 0 && host.failureText().isEmpty(),
          "the refusal is not published over a running run");
    running = false;
    check(serviceStops == 0, "nothing here stops the service");
    retained_record() = QJsonObject();
}

/* Re-review N1: a start accepted while one is on its way (published Starting, its START not delivered yet, so the
 * service still reads IDLE) supersedes that one once the service state accepted it, never before. Otherwise both
 * STARTs arrive, the first runs, and this one's grace expires as undelivered under a host that follows it. */
static void
check_accepted_start_supersedes_the_one_on_its_way() {
    using dsd_android::DecoderHostAndroid;
    using dsd_qt::DecoderHost;
    using namespace dsd_android;
    DecoderHostAndroid host;
    serviceStops = 0;
    serviceTookAttempt = false;
    nowMs = 600000;
    retained_record() = {{"sessionId", 140}, {"state", "IDLE"}};
    running = false;
    host.refresh();
    check(host.start(QStringList{QStringLiteral("-i"), QStringLiteral("rtl:0")}), "the first start is dispatched");
    const uint64_t first = lastStartAttempt;
    nowMs = 600250;
    host.refresh();
    check(host.sessionState() == DecoderHost::Starting, "it is on its way, its START not delivered");
    retirements.clear();
    const int reads = lifecycleReads;
    check(host.start(QStringList{QStringLiteral("-i"), QStringLiteral("rtl:0")}),
          "a start while one is on its way is accepted while the service is idle");
    const uint64_t second = lastStartAttempt;
    check(second > first && retirements == QList<uint64_t>{second - 1},
          "an accepted start supersedes the one on its way");
    check(lifecycleReadsAtRetire > reads, "once the service state accepted it");
    retained_record() = {
        {"sessionId", 141}, {"state", "RUNNING"}, {"reason", kRunPending}, {"attemptId", (qint64)second}};
    running = true;
    nowMs = 600500;
    host.refresh();
    check(host.sessionState() == DecoderHost::Running && host.inputFailureKind() == 0,
          "the superseding start is the one followed");
    running = false;
    check(serviceStops == 0, "nothing here stops the service");
    retained_record() = QJsonObject();
}

/* Gate r5 L1: a second start supersedes the first while that one is on its way, and the service takes the first
 * between the host's lifecycle read and its retirement, which cancels it while it configures. The first's records
 * (STARTING, then its cancelled end, a newer session than the read) are not the second's: the second keeps Starting
 * and its grace, and either its own records drive the phase or its grace runs out and retires it. */
static void
check_superseded_attempt_never_drives_the_phase() {
    using dsd_android::DecoderHostAndroid;
    using dsd_qt::DecoderHost;
    using namespace dsd_android;
    for (const bool delivered : {false, true}) {
        DecoderHostAndroid host;
        hostDiagnostics = 0;
        serviceStops = 0;
        startDispatches = 0;
        serviceTookAttempt = false;
        nowMs = 700000;
        retained_record() = {{"sessionId", 150}, {"state", "IDLE"}};
        running = false;
        host.refresh();
        check(host.start(QStringList{QStringLiteral("-i"), QStringLiteral("rtl:0")}), "the first start is dispatched");
        const uint64_t first = lastStartAttempt;
        nowMs = 700250;
        host.refresh();
        check(host.sessionState() == DecoderHost::Starting, "the first start is on its way");

        retirements.clear();
        recordAtRetire = {
            {"sessionId", 151}, {"state", "STARTING"}, {"reason", kRunPending}, {"attemptId", (qint64)first}};
        nowMs = 700500;
        check(host.start(QStringList{QStringLiteral("-i"), QStringLiteral("rtl:0")}),
              "the second start is accepted: the first is cancelled while it configures");
        const uint64_t second = lastStartAttempt;
        check(second > first && startDispatches == 2 && retirements == QList<uint64_t>{first},
              "the second start is dispatched and supersedes the first");

        nowMs = 700750;
        host.refresh();
        check(host.sessionState() == DecoderHost::Starting && host.failureText().isEmpty(),
              "the superseded attempt's STARTING record leaves the second start Starting");
        retained_record() = {
            {"sessionId", 151}, {"state", "IDLE"}, {"reason", kRunCancelled}, {"attemptId", (qint64)first}};
        nowMs = 701000;
        host.refresh();
        check(host.sessionState() == DecoderHost::Starting && host.failureText().isEmpty()
                  && host.inputFailureKind() == 0,
              "the superseded attempt's cancelled end is not the second start's end");
        check(host.statusText() == QStringLiteral("Starting…"), "the status still reads Starting");

        if (!delivered) {
            /* The second START is still on its way: its grace runs out and retires it, so it never runs later. */
            for (uint64_t at = 701250; at < 702750; at += 250) {
                nowMs = at;
                host.refresh();
                check(host.sessionState() == DecoderHost::Starting,
                      "the grace runs from the superseded attempt's last STARTING record, which the second START "
                      "waits behind");
            }
            check(hostDiagnostics == 0, "nothing is logged under the grace");
            nowMs = 702750;
            host.refresh();
            check(host.sessionState() == DecoderHost::Failed && host.inputFailureKind() == kAttemptUndelivered,
                  "the delayed second start times out as undelivered");
            check(retirements.last() == second, "and is retired, so it can never start later");
            check(lastHostDiagnostic
                      == QStringLiteral(
                          "Start was not delivered to the decoder service within 2250 ms (service state IDLE)"),
                  "the expiry logs the wait since the request");
            host.stop();
            check(host.sessionState() == DecoderHost::Idle && serviceStops == 0, "Dismiss acknowledges it");
        } else {
            /* The second START arrives once the first is cancelled: its own records drive the phase. */
            retained_record() = {
                {"sessionId", 152}, {"state", "STARTING"}, {"reason", kRunPending}, {"attemptId", (qint64)second}};
            nowMs = 701250;
            host.refresh();
            check(host.sessionState() == DecoderHost::Starting, "the second start's STARTING record is followed");
            retained_record() = {
                {"sessionId", 152}, {"state", "RUNNING"}, {"reason", kRunPending}, {"attemptId", (qint64)second}};
            running = true;
            nowMs = 701500;
            host.refresh();
            check(host.sessionState() == DecoderHost::Running && host.statusText() == QStringLiteral("Decoding"),
                  "the second start's RUNNING record publishes Running");
            retained_record() = {{"sessionId", 152},           {"state", "IDLE"},
                                 {"reason", kRunFailed},       {"lastError", "Radio open failed"},
                                 {"inputFailure", 1},          {"inputError", 111},
                                 {"attemptId", (qint64)second}};
            running = false;
            nowMs = 701750;
            host.refresh();
            check(host.sessionState() == DecoderHost::Failed
                      && host.failureText() == QStringLiteral("Radio open failed") && host.inputFailureKind() == 1,
                  "the second start's own terminal record publishes its failure");
            host.stop();
        }
        check(serviceStops == 0, "nothing here stops the service");
    }
    retained_record() = QJsonObject();
}

/* Gate r5 L1: records of a retired attempt, whatever they say, leave the current start's phase, grace, run result
 * and receiver health untouched; the current attempt's own records still drive them. */
static void
check_retired_attempt_records_are_ignored() {
    using dsd_android::DecoderHostAndroid;
    using dsd_qt::DecoderHost;
    using namespace dsd_android;
    DecoderHostAndroid host;
    int results = 0;
    int initialized = 0;
    QObject::connect(&host, &DecoderHost::runResultChanged, [&results]() { ++results; });
    int devices = 0;
    QObject::connect(&host, &DecoderHost::sessionInitialized, [&initialized]() { ++initialized; });
    QObject::connect(&host, &DecoderHost::localDeviceChanged, [&devices]() { ++devices; });
    serviceStops = 0;
    serviceTookAttempt = false;
    nowMs = 800000;
    retained_record() = {{"sessionId", 160}, {"state", "IDLE"}};
    running = false;
    host.refresh();
    check(host.start(QStringList{QStringLiteral("-i"), QStringLiteral("rtl:0")}), "the first start is dispatched");
    const uint64_t first = lastStartAttempt;
    nowMs = 800250;
    host.refresh();
    check(host.start(QStringList{QStringLiteral("-i"), QStringLiteral("rtl:0")}), "a second start supersedes it");
    const uint64_t second = lastStartAttempt;
    check(retirements.last() == first, "the first is retired");

    results = 0;
    devices = 0;
    retained_record() = {{"sessionId", 161},    {"state", "RUNNING"},        {"reason", kRunPending},
                         {"initialized", true}, {"endCause", kEndDetached},  {"receiverSource", kReceiverRtl},
                         {"deviceError", -6},   {"attemptId", (qint64)first}};
    running = true;
    nowMs = 800500;
    host.refresh();
    check(host.sessionState() == DecoderHost::Starting, "a retired attempt's RUNNING record is not the start's run");
    check(host.receiverHealth().value(QStringLiteral("state")).toString() != QStringLiteral("lost") && initialized == 0
              && devices == 0,
          "nor its receiver ending, device error or initialization");
    retained_record() = {
        {"sessionId", 161},  {"state", "IDLE"},   {"reason", kRunFailed},      {"lastError", "Decoder run failed (1)"},
        {"inputFailure", 1}, {"inputError", 111}, {"attemptId", (qint64)first}};
    running = false;
    nowMs = 800750;
    host.refresh();
    check(host.sessionState() == DecoderHost::Starting && host.failureText().isEmpty(),
          "a retired attempt's terminal record is not the start's end");
    check(host.inputFailureKind() == 0 && host.inputFailureCode() == 0 && host.terminalReason() != kRunFailed
              && results == 0,
          "nor its run result");

    retained_record() = {
        {"sessionId", 162}, {"state", "RUNNING"}, {"reason", kRunPending}, {"attemptId", (qint64)second}};
    running = true;
    nowMs = 801000;
    host.refresh();
    check(host.sessionState() == DecoderHost::Running, "the current attempt's record drives the phase");
    retained_record() = {{"sessionId", 162},
                         {"state", "IDLE"},
                         {"reason", kRunCompleted},
                         {"endCause", kEndDetached},
                         {"receiverSource", kReceiverRtl},
                         {"attemptId", (qint64)second}};
    running = false;
    nowMs = 801250;
    host.refresh();
    check(host.sessionState() == DecoderHost::Idle && host.terminalReason() == kRunCompleted,
          "and its terminal record ends it");
    check(host.receiverHealth().value(QStringLiteral("state")).toString() == QStringLiteral("lost"),
          "with its own receiver ending");
    check(serviceStops == 0, "nothing here stops the service");
    retained_record() = QJsonObject();
}

/* Gate r5 L1: the attempt on its way was taken past the point a retirement can cancel it, between the read that
 * accepted a second start and that start's retirement of it: it runs. The second START would only be rejected as
 * busy, so the second start is refused, and the run on screen is the first, whose records drive the phase. */
static void
check_superseded_attempt_that_ran_keeps_the_screen() {
    using dsd_android::DecoderHostAndroid;
    using dsd_qt::DecoderHost;
    using namespace dsd_android;
    DecoderHostAndroid host;
    serviceStops = 0;
    startDispatches = 0;
    serviceTookAttempt = false;
    nowMs = 900000;
    retained_record() = {{"sessionId", 170}, {"state", "IDLE"}};
    running = false;
    host.refresh();
    check(host.start(QStringList{QStringLiteral("-i"), QStringLiteral("rtl:0")}), "the first start is dispatched");
    const uint64_t first = lastStartAttempt;
    nowMs = 900250;
    host.refresh();
    retirements.clear();
    recordAtRetire = {{"sessionId", 171}, {"state", "RUNNING"}, {"reason", kRunPending}, {"attemptId", (qint64)first}};
    running = true;
    check(!host.start(QStringList{QStringLiteral("-i"), QStringLiteral("rtl:0")}),
          "a start whose predecessor ran meanwhile is refused");
    check(startDispatches == 1 && retirements == QList<uint64_t>{first}, "it dispatches nothing");
    check(host.sessionState() == DecoderHost::Starting && host.failureText().isEmpty() && host.inputFailureKind() == 0,
          "and publishes no refusal over the starting run");
    check(lastHostDiagnostic.endsWith(QStringLiteral(" refused: the decoder service is RUNNING")),
          "the refusal is logged with the service state");
    nowMs = 900500;
    host.refresh();
    check(host.sessionState() == DecoderHost::Running, "the first attempt's run is the one on screen");
    retained_record() = {
        {"sessionId", 171}, {"state", "IDLE"}, {"reason", kRunCompleted}, {"attemptId", (qint64)first}};
    running = false;
    nowMs = 900750;
    host.refresh();
    check(host.sessionState() == DecoderHost::Idle, "and its end ends it");
    check(serviceStops == 0, "nothing here stops the service");
    retained_record() = QJsonObject();
}

int
main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    using namespace dsd_android;
    check_engine_busy();
    check_document_dispatch();
    check_stop_publishes_stopping();
    check_session_state_known_after_first_poll();
    check_receiver_health();
    check_claim_error_scope();
    check_claim_error_connection();
    check_receiver_take_failures();
    check_request_record();
    check_undelivered_start_after_network_failure();
    check_refused_start();
    check_attempt_identity();
    check_dismissed_undelivered_start_winds_down();
    check_refused_start_leaves_the_run_on_screen();
    check_accepted_start_supersedes_the_one_on_its_way();
    check_superseded_attempt_never_drives_the_phase();
    check_retired_attempt_records_are_ignored();
    check_superseded_attempt_that_ran_keeps_the_screen();
    check_unreadable_record_keeps_the_session();
    check_recording_roots();
    check_recording_playback();
    check_recording_transfers();
    check_recording_share();
    DecoderHostAndroid host;
    /* Settings shows the Screen rows only where the host applies them (#574). */
    check(host.screenPolicySupported(), "the Android host applies the screen policy");
    check(host.property("screenPolicySupported").toBool(), "QML reads the screen policy support");
    retained_record() = {{"sessionId", 40}, {"state", "IDLE"}};
    host.refresh();
    for (const int session : {41, 42}) {
        retained_record() = {{"sessionId", session}, {"state", "RUNNING"}, {"reason", kRunPending}};
        running = true;
        host.refresh();
        check(host.sessionState() == dsd_qt::DecoderHost::Running, "new session reaches Running");
        retained_record() = {{"sessionId", session}, {"state", "IDLE"},
                             {"reason", kRunFailed}, {"lastError", "Radio open failed"},
                             {"inputFailure", 1},    {"inputError", 111}};
        running = false;
        host.refresh();
        check(host.inputFailureKind() == 1 && host.inputFailureCode() == 111,
              "structured network error survives terminal lifecycle record");
        check(host.sessionState() == dsd_qt::DecoderHost::Failed && !host.failureText().isEmpty(),
              "new session failure remains visible");
        host.stop();
        check(host.sessionState() == dsd_qt::DecoderHost::Idle && host.failureText().isEmpty(),
              "failure acknowledgement clears phase and text");
        for (int poll = 0; poll < 4; ++poll) {
            host.refresh();
            check(host.sessionState() == dsd_qt::DecoderHost::Idle && host.failureText().isEmpty(),
                  "acknowledged failure stays absent across retained polls");
        }
    }
    check(serviceStops == 0, "inactive failed sessions are acknowledged locally");
    return failures ? 1 : 0;
}
