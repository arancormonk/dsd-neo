// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DSD_NEO_UI_QT_AUTO_START_ATTACHMENT_H
#define DSD_NEO_UI_QT_AUTO_START_ATTACHMENT_H

#include <QChar>
#include <QString>

namespace dsd_qt {

/** The receiver kind of a DecoderHost::localDeviceAttached() payload, "<kind>:<device name>" (#647 D3a). */
inline QString
attachedReceiverKind(const QString& attachment) {
    return attachment.section(QLatin1Char(':'), 0, 0);
}

/**
 * Whether a saved system's or scan list's @p source may start on the receiver kind that just attached: it runs on a
 * USB receiver (RTL-SDR "usb" or "airspy") of exactly that kind. What else is on the bus does not count (#647 D3a).
 */
inline bool
autoStartKindMatches(const QString& source, const QString& attachedKind) {
    return (source == QStringLiteral("usb") || source == QStringLiteral("airspy")) && source == attachedKind;
}

} // namespace dsd_qt
#endif
