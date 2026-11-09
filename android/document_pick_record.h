// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Turns one DocumentPicker.pollResult() record into the host signal it stands for (#829).
 *
 * Free of JNI so the host test can drive it directly: DecoderHostAndroid::refreshDocumentPicks() only reads the
 * records.
 */

#ifndef DSD_NEO_ANDROID_DOCUMENT_PICK_RECORD_H_
#define DSD_NEO_ANDROID_DOCUMENT_PICK_RECORD_H_

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLatin1String>
#include <QString>
#include <QtGlobal>
#include "decoder_host.h"

namespace dsd_android {

/** @brief One DocumentPicker record, read; @c kind is None for a record that is malformed or of an unknown kind. */
struct DocumentPickRecord {
    enum Kind { None, Picked, Cancelled, Unavailable };

    Kind kind = None;
    qint64 id = 0;
    QString uri;
    QString name;
    QString reason;
    bool downloads = false;
};

/**
 * @brief Reads @p text, one JSON record: `{"id","kind":"picked","uri","name","downloads"}`, `{"id","kind":"cancelled"}`
 *        or `{"id","kind":"unavailable","reason"}`. Anything else, or a field missing or of the wrong type, reads as
 *        None.
 */
inline DocumentPickRecord
read_document_pick_record(const QString& text) {
    DocumentPickRecord record;
    const QJsonObject object = QJsonDocument::fromJson(text.toUtf8()).object();
    const QJsonValue id = object.value(QStringLiteral("id"));
    const QString kind = object.value(QStringLiteral("kind")).toString();
    record.id = id.toInteger();
    if (!id.isDouble() || record.id <= 0) {
        return record;
    }
    if (kind == QLatin1String("picked")) {
        const QJsonValue uri = object.value(QStringLiteral("uri"));
        const QJsonValue name = object.value(QStringLiteral("name"));
        const QJsonValue downloads = object.value(QStringLiteral("downloads"));
        if (uri.isString() && !uri.toString().isEmpty() && name.isString() && downloads.isBool()) {
            record.kind = DocumentPickRecord::Picked;
            record.uri = uri.toString();
            record.name = name.toString();
            record.downloads = downloads.toBool();
        }
    } else if (kind == QLatin1String("cancelled")) {
        record.kind = DocumentPickRecord::Cancelled;
    } else if (kind == QLatin1String("unavailable")) {
        const QString reason = object.value(QStringLiteral("reason")).toString();
        if (!reason.isEmpty()) {
            record.kind = DocumentPickRecord::Unavailable;
            record.reason = reason;
        }
    }
    return record;
}

/**
 * @brief Emits the signal for one record on @p host, for the live request @p liveId only, which it then clears.
 *
 * A record for any other id is dropped: its request was cancelled, replaced or already answered. Dropping a
 * Download-folder row hands it to discardPendingDownload(), since nothing will write it. A malformed record, or one
 * of an unknown kind, is ignored.
 */
inline void
deliver_document_pick_record(dsd_qt::DecoderHost& host, qint64& liveId, const QString& text) {
    const DocumentPickRecord record = read_document_pick_record(text);
    if (record.kind == DocumentPickRecord::None) {
        return;
    }
    if (liveId == 0 || record.id != liveId) {
        if (record.kind == DocumentPickRecord::Picked && record.downloads) {
            host.discardPendingDownload(record.uri);
        }
        return;
    }
    liveId = 0;
    switch (record.kind) {
        case DocumentPickRecord::Picked:
            Q_EMIT host.documentPicked(record.id, record.uri, record.name, record.downloads);
            break;
        case DocumentPickRecord::Cancelled: Q_EMIT host.documentPickCancelled(record.id); break;
        case DocumentPickRecord::Unavailable: Q_EMIT host.documentPickUnavailable(record.id, record.reason); break;
        case DocumentPickRecord::None: break;
    }
}

} // namespace dsd_android

#endif // DSD_NEO_ANDROID_DOCUMENT_PICK_RECORD_H_
