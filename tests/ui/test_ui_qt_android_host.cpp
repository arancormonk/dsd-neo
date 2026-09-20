// SPDX-License-Identifier: GPL-3.0-or-later
#include <QCoreApplication>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>
#include <QtGlobal>
#include <cstdio>
#include <initializer_list>
#include <utility>
#include "../../android/decoder_host_android.h"
#include "../../android/run_status.h"
#include "decoder_host.h"

class QObject;

static QJsonObject&
retained_record() {
    static QJsonObject value;
    return value;
}

static bool running;
static int serviceStops;

// Only platform transport is replaced. The real Android host's refresh, stop,
// phase publication and failureText getter are compiled by this desktop target.
namespace dsd_android {
DecoderHostAndroid::DecoderHostAndroid(QObject* parent) : DecoderHost(parent) {}

QJsonObject
DecoderHostAndroid::readLifecycleStatus() const {
    return retained_record();
}

bool
DecoderHostAndroid::readEngineRunning() {
    return running;
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
DecoderHostAndroid::refreshPresentation() {}

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
DecoderHostAndroid::refreshLocalDevice() {}

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
DecoderHostAndroid::setKeepScreenAwake(bool) {}

QString
DecoderHostAndroid::importContentUri(const QString&, const QString&) {
    return {};
}

QString
DecoderHostAndroid::importDocument(const QString&, const QString&, const QString&) {
    return {};
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

int
main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    using namespace dsd_android;
    DecoderHostAndroid host;
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
