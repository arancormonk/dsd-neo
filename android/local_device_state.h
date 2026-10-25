// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DSD_NEO_ANDROID_LOCAL_DEVICE_STATE_H_
#define DSD_NEO_ANDROID_LOCAL_DEVICE_STATE_H_
#include <QJsonObject>
#include <QString>
#include "decoder_host.h"

namespace dsd_android {
// The complete retained USB record is installed before notifying property readers.
struct LocalDeviceState {
    QString source = QStringLiteral("usb");
    QString serial;
    bool ready = false;
    QString text;
    int failureKind = dsd_qt::DecoderHost::NoDeviceFailure;
    QString name = QStringLiteral("RTL-SDR");
    bool pending = false;
    QString requestedSource;
    QString requestedSerial;

    void
    apply(dsd_qt::DecoderHost& host, const QJsonObject& record) {
        const QString kind = record.value(QStringLiteral("kind")).toString();
        // "refused" (a receiver switch refused mid-session) answers the request; the connection still works (#647 B11).
        const int failure = kind == QStringLiteral("refused")       ? dsd_qt::DecoderHost::NoDeviceFailure
                            : kind == QStringLiteral("detached")    ? dsd_qt::DecoderHost::DeviceDetached
                            : kind == QStringLiteral("permission")  ? dsd_qt::DecoderHost::DevicePermission
                            : kind == QStringLiteral("open_failed") ? dsd_qt::DecoderHost::DeviceOpenFailed
                                                                    : dsd_qt::DecoderHost::NoDeviceFailure;
        const QString nextSerial = record.value(QStringLiteral("serial")).toString();
        const QString nextSource = record.value(QStringLiteral("source")).toString(QStringLiteral("usb"));
        const bool nextReady = record.value(QStringLiteral("ready")).toBool();
        const QString nextText = record.value(QStringLiteral("text")).toString();
        const QString nextName = record.value(QStringLiteral("name")).toString(QStringLiteral("RTL-SDR"));
        // UsbSourceManager's outstanding access request; a record without one has none.
        const bool nextPending = record.value(QStringLiteral("pending")).toBool();
        const QString nextRequestedSource = record.value(QStringLiteral("requestedSource")).toString();
        const QString nextRequestedSerial = record.value(QStringLiteral("requestedSerial")).toString();
        if (serial == nextSerial && source == nextSource && ready == nextReady && text == nextText
            && failureKind == failure && name == nextName && pending == nextPending
            && requestedSource == nextRequestedSource && requestedSerial == nextRequestedSerial) {
            return;
        }
        serial = nextSerial;
        source = nextSource;
        ready = nextReady;
        text = nextText;
        failureKind = failure;
        name = nextName;
        pending = nextPending;
        requestedSource = nextRequestedSource;
        requestedSerial = nextRequestedSerial;
        Q_EMIT host.localDeviceChanged();
    }
};
} // namespace dsd_android
#endif
