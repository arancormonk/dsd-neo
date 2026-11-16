// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Turns one LocationSupport.pollResult() record into the host signal it stands for, and one
 * LocationStream.pollStream() record (#786) into the locationStreamUpdate() it stands for.
 *
 * Free of JNI so the host test can drive it directly: DecoderHostAndroid::refreshLocation() and
 * refreshLocationStream() only read the records.
 */

#ifndef DSD_NEO_ANDROID_LOCATION_RECORD_H_
#define DSD_NEO_ANDROID_LOCATION_RECORD_H_

#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantMap>
#include <QtGlobal>
#include <algorithm>
#include <cmath>
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

namespace detail {

/**
 * @brief Whether @p value is a JSON whole number, as LocationStream writes its counters and stamps, read into @p out.
 *
 * toInteger() answers its default for anything but a whole number, so two different defaults tell one apart.
 */
inline bool
json_integer(const QJsonValue& value, qint64* out) {
    const qint64 first = value.toInteger(-1);
    if (first != value.toInteger(0)) {
        return false;
    }
    *out = first;
    return true;
}

/** @brief Whether @p value is a finite JSON number within [@p low, @p high]. */
inline bool
json_number_within(const QJsonValue& value, double low, double high) {
    if (!value.isDouble()) {
        return false;
    }
    const double number = value.toDouble();
    return std::isfinite(number) && number >= low && number <= high;
}

/** @brief Whether @p value is a finite JSON number above 0. */
inline bool
json_positive(const QJsonValue& value) {
    return value.isDouble() && std::isfinite(value.toDouble()) && value.toDouble() > 0.0;
}

/** @brief The fix fields of a stream record: all there with a usable fix, none without one. */
inline const QStringList&
stream_fix_keys() {
    static const QStringList keys = {QStringLiteral("provider"),  QStringLiteral("elapsedRealtimeNanos"),
                                     QStringLiteral("lat"),       QStringLiteral("lon"),
                                     QStringLiteral("accuracyM"), QStringLiteral("ageMs")};
    return keys;
}

/** @brief Read the fix fields of @p record, all of them there, into @p out; false when one does not read. */
inline bool
read_stream_fix(const QJsonObject& record, qint64 now_ms, QVariantMap* out) {
    const QString provider = record.value(QStringLiteral("provider")).toString();
    const QJsonValue lat = record.value(QStringLiteral("lat"));
    const QJsonValue lon = record.value(QStringLiteral("lon"));
    const QJsonValue accuracy = record.value(QStringLiteral("accuracyM"));
    qint64 stamp = 0;
    qint64 age = 0;
    if (provider.isEmpty() || !json_integer(record.value(QStringLiteral("elapsedRealtimeNanos")), &stamp) || stamp <= 0
        || !json_number_within(lat, -90.0, 90.0) || !json_number_within(lon, -180.0, 180.0) || !json_positive(accuracy)
        || !json_integer(record.value(QStringLiteral("ageMs")), &age)) {
        return false;
    }
    out->insert(QStringLiteral("provider"), provider);
    out->insert(QStringLiteral("measurementNs"), stamp);
    out->insert(QStringLiteral("lat"), lat.toDouble());
    out->insert(QStringLiteral("lon"), lon.toDouble());
    out->insert(QStringLiteral("accuracyM"), accuracy.toDouble());
    out->insert(QStringLiteral("fixAtMs"), now_ms - age);
    return true;
}

} // namespace detail

/**
 * @brief Read one LocationStream.pollStream() record (#786) into the locationStreamUpdate() map @p update.
 *
 * @p now_ms is the host's dsd_realtime_mono_ms() at the poll: the fix's @c fixAtMs is @p now_ms less the record's
 * @c ageMs, so it lands on the frontend's monotonic clock. The record's @c elapsedRealtimeNanos is Android's boot-time
 * clock, which keeps counting through deep sleep while that one does not, so it is passed on only as the measurement's
 * identity (@c measurementNs), never as a time.
 *
 * @return false, leaving @p update alone, for a record that does not read: no integer @c generation of 0 or more, a
 * @c state LocationStream never writes, or fix fields that are only partly there or out of range (an empty provider, a
 * stamp of 0 or less, a latitude or longitude off the globe, an accuracy that is not above 0, an age that is not an
 * integer). A record without any fix field reads, with @c hasFix false.
 */
inline bool
location_stream_update(const QJsonObject& record, qint64 now_ms, QVariantMap* update) {
    static const QStringList kStates = {QStringLiteral("ok"), QStringLiteral("noPermission"),
                                        QStringLiteral("approximateOnly"), QStringLiteral("locationOff"),
                                        QStringLiteral("providerUnavailable")};
    qint64 generation = 0;
    const QString state = record.value(QStringLiteral("state")).toString();
    if (!detail::json_integer(record.value(QStringLiteral("generation")), &generation) || generation < 0
        || !kStates.contains(state)) {
        return false;
    }
    const QStringList& keys = detail::stream_fix_keys();
    const auto present = std::count_if(keys.cbegin(), keys.cend(), [&record](const QString& key) {
        return record.contains(key) && !record.value(key).isNull();
    });
    QVariantMap out{{QStringLiteral("generation"), generation},
                    {QStringLiteral("state"), state},
                    {QStringLiteral("hasFix"), present != 0}};
    if (present != 0 && (present != keys.size() || !detail::read_stream_fix(record, now_ms, &out))) {
        return false;
    }
    *update = out;
    return true;
}

} // namespace dsd_android

#endif // DSD_NEO_ANDROID_LOCATION_RECORD_H_
