// SPDX-License-Identifier: GPL-3.0-or-later
#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QUrl>
#include <QVariant>
#include <QtGlobal>
#include <cstdio>
#include <stdint.h>
#include <utility>
#include "../../android/decoder_host_android.h"
#include "../../android/run_status.h"
#include "../../android/session_state_map.h"
#include "decoder_host.h"

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
DecoderHostAndroid::platformStartDecoder(const QStringList&) {
    ++startDispatches;
    return {};
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
    check(host.sessionState() == DecoderHost::Idle && health_state(host) == QStringLiteral("failed"),
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
    retained_record() = openFailed;
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
    retained_record() = failed;
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
    usb_record() = other;
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

int
main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    using namespace dsd_android;
    check_engine_busy();
    check_document_dispatch();
    check_stop_publishes_stopping();
    check_receiver_health();
    check_claim_error_scope();
    check_request_record();
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
