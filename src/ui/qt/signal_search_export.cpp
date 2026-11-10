// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "signal_search_export.h"

#include <QByteArray>
#include <QDateTime>
#include <QLatin1String>
#include <QList>
#include <QStringList>
#include <QTimeZone>
#include <Qt>
#include <QtGlobal>
#include <cmath>
#include <dsd-neo/app_control/signal_search.h>
#include <dsd-neo/runtime/signal_search.h>
#include <unordered_set>
#include "csv_text.h"

namespace dsd_qt {
namespace signal_search_export {

namespace {

/* The detections file's columns, in order; `true` marks a text column, which gets the formula guard. */
const struct {
    const char* name;
    bool text;
} kDetectionColumns[] = {
    {"started_utc", false},
    {"ended_utc", false},
    {"ended", false},
    {"frequency_hz", false},
    {"frequency_mhz", false},
    {"listen_mode", false},
    {"identification", false},
    {"protocol", true},
    {"access", true},
    {"tone", true},
    {"site", true},
    {"slot", false},
    {"target", true},
    {"source", true},
    {"encryption", true},
    {"power_dbfs", false},
    {"noise_dbfs", false},
    {"range", true},
    {"bandwidth_hz", false},
    {"saved_names", true},
};

QStringList
detectionHeader() {
    QStringList names;
    for (const auto& column : kDetectionColumns) {
        names.append(QLatin1String(column.name));
    }
    return names;
}

QVector<bool>
detectionTextColumns() {
    QVector<bool> text;
    for (const auto& column : kDetectionColumns) {
        text.append(column.text);
    }
    return text;
}

/* The text in the first `capacity` bytes of `value`, up to its first NUL. It is not a template, so
 * include-what-you-use credits qstrnlen to <QByteArray> instead of asking for Qt's private header. */
QString
boundedText(const char* value, size_t capacity) {
    return QString::fromUtf8(value, (qsizetype)qstrnlen(value, capacity));
}

/* A fixed-size text field of the record, up to its first NUL and never past its end. */
template <size_t N>
QString
text(const char (&value)[N]) {
    return boundedText(value, N);
}

/* A stamp in milliseconds since the Unix epoch as ISO 8601 UTC with milliseconds; empty for 0 (none recorded). */
QString
utc(uint64_t ms) {
    if (ms == 0U) {
        return {};
    }
    return QDateTime::fromMSecsSinceEpoch((qint64)ms, QTimeZone::UTC).toString(Qt::ISODateWithMs);
}

QString
level(float dbfs) {
    return std::isfinite(dbfs) ? QString::number((double)dbfs, 'f', 1) : QString();
}

/* A call party: its received text, else its number, else nothing. */
QString
party(const char (&received)[64], uint32_t number) {
    const QString name = text(received);
    if (!name.isEmpty()) {
        return name;
    }
    return number != 0U ? QString::number(number) : QString();
}

QString
identification(const dsd_signal_search_hit& h) {
    if (h.confirmed != 0) {
        return QStringLiteral("confirmed");
    }
    return h.unresolved != 0 ? QStringLiteral("unresolved") : QStringLiteral("rf-only");
}

QStringList
detectionFields(const dsd_signal_search_hit& h, const QString& savedNames) {
    return {utc(h.started_ms),
            utc(h.ended_ms),
            h.ended != 0 ? QStringLiteral("1") : QStringLiteral("0"),
            QString::number(h.frequency_hz),
            QString::number(h.frequency_hz / 1e6, 'f', 6),
            h.am != 0 ? QStringLiteral("am") : QStringLiteral("nfm"),
            identification(h),
            text(h.protocol),
            text(h.access),
            text(h.tone),
            text(h.site),
            h.slot != 0 ? QString::number(h.slot) : QString(),
            party(h.target_text, h.target),
            party(h.source_text, h.source),
            text(h.crypt),
            level(h.power_dbfs),
            level(h.noise_dbfs),
            text(h.range_name),
            QString::number(h.bandwidth_hz),
            savedNames};
}

std::vector<uint32_t>
readList(const std::function<size_t(uint32_t*, size_t)>& get) {
    std::vector<uint32_t> list(DSD_SEARCH_MAX_AVOIDS);
    const size_t n = get ? get(list.data(), list.size()) : 0U;
    list.resize(n < list.size() ? n : list.size());
    return list;
}

/* The avoids file's three columns hold numbers and a fixed word: none is a text column. */
const QVector<bool>&
avoidColumns() {
    static const QVector<bool> kText{false, false, false};
    return kText;
}

QByteArray
avoidRecord(uint32_t frequency, const char* list) {
    return csv_text::record({QString::number(frequency), QString::number(frequency / 1e6, 'f', 6), QLatin1String(list)},
                            avoidColumns());
}

} // namespace

QByteArray
detections(const dsd_signal_search_hit* hits, size_t count, const SavedNames& names) {
    const QVector<bool> text = detectionTextColumns();
    QByteArray file = csv_text::bom();
    file += csv_text::record(detectionHeader(), QVector<bool>(text.size(), false));
    for (size_t i = 0; hits != nullptr && i < count; ++i) {
        file += csv_text::record(detectionFields(hits[i], names ? names(hits[i]) : QString()), text);
    }
    return file;
}

AvoidSource
storeAvoids() {
    return {dsd_app_signal_search_avoids_revision, dsd_app_signal_search_avoids_get,
            dsd_app_signal_search_session_avoids_get};
}

bool
readAvoids(const AvoidSource& source, std::vector<uint32_t>* permanent, std::vector<uint32_t>* session) {
    if (!source.revision || permanent == nullptr || session == nullptr) {
        return false;
    }
    for (int i = 0; i < kAvoidReadTries; ++i) {
        const uint64_t before = source.revision();
        *permanent = readList(source.permanent);
        *session = readList(source.session);
        if (source.revision() == before) {
            return true;
        }
    }
    permanent->clear();
    session->clear();
    return false;
}

QByteArray
avoids(const std::vector<uint32_t>& permanent, const std::vector<uint32_t>& session, int* rows) {
    QByteArray file = csv_text::bom();
    file += csv_text::record({QStringLiteral("frequency_hz"), QStringLiteral("frequency_mhz"), QStringLiteral("list")},
                             avoidColumns());
    int written = 0;
    const std::unordered_set<uint32_t> saved(permanent.begin(), permanent.end());
    for (const uint32_t frequency : permanent) {
        file += avoidRecord(frequency, "permanent");
        ++written;
    }
    for (const uint32_t frequency : session) {
        if (saved.count(frequency) == 0U) {
            file += avoidRecord(frequency, "session");
            ++written;
        }
    }
    if (rows != nullptr) {
        *rows = written;
    }
    return file;
}

} // namespace signal_search_export
} // namespace dsd_qt
