// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Turns one LocationSupport.pollResult() record into the host signal it stands for.
 *
 * Free of JNI so the host test can drive it directly: DecoderHostAndroid::refreshLocation() only reads the record.
 */

#ifndef DSD_NEO_ANDROID_LOCATION_RECORD_H_
#define DSD_NEO_ANDROID_LOCATION_RECORD_H_

#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include "decoder_host.h"

namespace dsd_android {

/**
 * @brief Emits the signal for one location record on @p host: locationCancelled for a cancelled request (a permission
 *        prompt Android closed unanswered, #782), locationResult for any other record, and nothing for an empty one
 *        (no result pending). A cancel is never a result: as one it would read as a failed lookup.
 */
inline void
deliver_location_record(dsd_qt::DecoderHost& host, const QJsonObject& location) {
    if (location.isEmpty()) {
        return;
    }
    const qint64 id = location.value(QStringLiteral("id")).toInteger();
    if (location.value(QStringLiteral("cancelled")).toBool()) {
        Q_EMIT host.locationCancelled(id);
        return;
    }
    Q_EMIT host.locationResult(
        id, location.value(QStringLiteral("fixOk")).toBool(), location.value(QStringLiteral("lat")).toDouble(),
        location.value(QStringLiteral("lon")).toDouble(), location.value(QStringLiteral("accuracyM")).toDouble(),
        location.value(QStringLiteral("fixAtMs")).toInteger(), location.value(QStringLiteral("geocodeOk")).toBool(),
        location.value(QStringLiteral("postalCode")).toString(),
        location.value(QStringLiteral("countryCode")).toString(), location.value(QStringLiteral("error")).toString());
}

} // namespace dsd_android

#endif // DSD_NEO_ANDROID_LOCATION_RECORD_H_
