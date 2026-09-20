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

    void
    apply(dsd_qt::DecoderHost& host, const QJsonObject& record) {
        const QString kind = record.value(QStringLiteral("kind")).toString();
        const int failure = kind == QStringLiteral("detached")      ? dsd_qt::DecoderHost::DeviceDetached
                            : kind == QStringLiteral("permission")  ? dsd_qt::DecoderHost::DevicePermission
                            : kind == QStringLiteral("open_failed") ? dsd_qt::DecoderHost::DeviceOpenFailed
                                                                    : dsd_qt::DecoderHost::NoDeviceFailure;
        const QString nextSerial = record.value(QStringLiteral("serial")).toString();
        const QString nextSource = record.value(QStringLiteral("source")).toString(QStringLiteral("usb"));
        const bool nextReady = record.value(QStringLiteral("ready")).toBool();
        const QString nextText = record.value(QStringLiteral("text")).toString();
        const QString nextName = record.value(QStringLiteral("name")).toString(QStringLiteral("RTL-SDR"));
        if (serial == nextSerial && source == nextSource && ready == nextReady && text == nextText
            && failureKind == failure && name == nextName) {
            return;
        }
        serial = nextSerial;
        source = nextSource;
        ready = nextReady;
        text = nextText;
        failureKind = failure;
        name = nextName;
        Q_EMIT host.localDeviceChanged();
    }
};
} // namespace dsd_android
#endif
