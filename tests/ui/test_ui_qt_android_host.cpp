// SPDX-License-Identifier: GPL-3.0-or-later
#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QUrl>
#include <QVariant>
#include <QtGlobal>
#include <cstdio>
#include <utility>
#include "../../android/decoder_host_android.h"
#include "../../android/run_status.h"
#include "../../android/session_state_map.h"
#include "decoder_host.h"

class QObject;

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
};
} // namespace

static PlatformCopies&
platform() {
    static PlatformCopies value;
    return value;
}

static bool running;
static int serviceStops;
// Every host refresh reads the lifecycle record and refreshes presentation and the local device.
static int lifecycleReads;
static int hostRefreshes;

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
}

void
DecoderHostAndroid::requestCurrentLocation(qint64) {}

void
DecoderHostAndroid::cancelLocationRequest(qint64) {}

void
DecoderHostAndroid::hostDiagnostic(const QString&) {}

void
DecoderHostAndroid::shareDiagnostics(const QString&, const QString&) {}

bool
DecoderHostAndroid::start(const QStringList&) {
    return false;
}

bool
DecoderHostAndroid::moveToBackground() {
    return false;
}

void
DecoderHostAndroid::requestLocalDeviceAccessForSource(const QString& source, const QString& serial) {
    (void)serial;
    (void)source;
    requestLocalDeviceAccess();
}

void
DecoderHostAndroid::requestLocalDeviceAccess() {}

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
    check(host.copyDocumentToFile(content, dest, 77) == DecoderHost::DocumentFailed,
          "a copy reported without its file fails");
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
    check(host.copyDocumentToFile(content, dir.filePath(QStringLiteral("busy")), 77) == DecoderHost::DocumentFailed
              && platform().calls == 0,
          "a destination that cannot be cleared fails before the platform is asked");
    check(host.copyDocumentToFile(content, QString(), 77) == DecoderHost::DocumentFailed
              && host.copyDocumentToFile(content, dest, -1) == DecoderHost::DocumentFailed && platform().calls == 0,
          "an empty destination or a negative cap never reaches the platform");

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

int
main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    using namespace dsd_android;
    check_engine_busy();
    check_document_dispatch();
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
