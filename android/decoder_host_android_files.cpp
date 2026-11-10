// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

/*
 * The in-app file browser's requests and answers (#830), less their JNI transport (platform*File*() and
 * platformListRoots() etc. in decoder_host_android.cpp), so the desktop host test compiles this dispatch as it ships.
 * Qt thread only. The base host keeps the requests, sessions and the exactly-once rules; this file hands each live
 * request to FileBrowser.kt and reads its records back.
 */

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLatin1String>
#include <QList>
#include <QMap>
#include <QObject>
#include <QSet>
#include <QString>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <QtGlobal>
#include <algorithm>
#include <functional>
#include <utility>
#include "decoder_host.h"
#include "decoder_host_android.h"

namespace dsd_android {

namespace {

/* A record member and the JSON type it must have. */
struct FileField {
    enum Type { String, Integer, Bool };

    const char* key;
    Type type;
};

/* Whether @p value is a whole JSON number (one that fits a qint64), read into @p out. toInteger() answers its default
 * for any other value, so two different defaults agree only for a whole number. */
bool
whole_number(const QJsonValue& value, qint64& out) {
    out = value.toInteger(-1);
    return value.isDouble() && out == value.toInteger(0);
}

/* Copies @p field from @p from into @p to, typed: a JSON number must be whole, and is a qint64. False when it is
 * missing or of another type. */
bool
copy_field(const QJsonObject& from, const FileField& field, QVariantMap& to) {
    const QString key = QLatin1String(field.key);
    const QJsonValue value = from.value(key);
    switch (field.type) {
        case FileField::String:
            if (!value.isString()) {
                return false;
            }
            to.insert(key, value.toString());
            return true;
        case FileField::Integer: {
            qint64 number = 0;
            if (!whole_number(value, number)) {
                return false;
            }
            to.insert(key, number);
            return true;
        }
        case FileField::Bool:
            if (!value.isBool()) {
                return false;
            }
            to.insert(key, value.toBool());
            return true;
    }
    return false;
}

bool
copy_fields(const QJsonObject& from, std::initializer_list<FileField> fields, QVariantMap& to) {
    return std::all_of(fields.begin(), fields.end(),
                       [&from, &to](const FileField& field) { return copy_field(from, field, to); });
}

/* Copies the array @p key of objects, each with every one of @p fields, as a list of maps. */
bool
copy_objects(const QJsonObject& from, const char* key, std::initializer_list<FileField> fields, QVariantMap& to) {
    const QJsonValue value = from.value(QLatin1String(key));
    if (!value.isArray()) {
        return false;
    }
    QVariantList list;
    for (const QJsonValue item : value.toArray()) {
        QVariantMap object;
        if (!item.isObject() || !copy_fields(item.toObject(), fields, object)) {
            return false;
        }
        list.append(object);
    }
    to.insert(QLatin1String(key), list);
    return true;
}

/* A member that must be a string that is not empty: a status or an outcome. */
bool
copy_word(const QJsonObject& from, const char* key, QVariantMap& to) {
    return copy_field(from, {key, FileField::String}, to) && !to.value(QLatin1String(key)).toString().isEmpty();
}

bool
read_roots(const QJsonObject& record, QVariantMap& answer) {
    return copy_word(record, "status", answer) && copy_field(record, {"generation", FileField::Integer}, answer)
           && copy_objects(record, "roots",
                           {{"key", FileField::String},
                            {"kind", FileField::String},
                            {"title", FileField::String},
                            {"path", FileField::String},
                            {"state", FileField::String},
                            {"detail", FileField::String}},
                           answer);
}

/* A listing or a page; one that is not ok carries its status only. */
bool
read_listing(const QJsonObject& record, QVariantMap& answer) {
    if (!copy_word(record, "status", answer)) {
        return false;
    }
    if (answer.value(QStringLiteral("status")).toString() != QLatin1String("ok")) {
        return true;
    }
    return copy_fields(record,
                       {{"snapshot", FileField::Integer},
                        {"offset", FileField::Integer},
                        {"more", FileField::Bool},
                        {"hiddenByFilter", FileField::Integer}},
                       answer)
           && copy_objects(record, "entries",
                           {{"name", FileField::String},
                            {"path", FileField::String},
                            {"reference", FileField::String},
                            {"dir", FileField::Bool},
                            {"size", FileField::Integer},
                            {"modifiedMs", FileField::Integer}},
                           answer);
}

/*
 * Reads @p text, one FileBrowser record: {"id","kind":"roots","status","generation","roots"},
 * {"id","kind":"listing","status",…} (a listing or a page), {"id","kind":"handoff","ok","reference","name","reason"}
 * or {"id","kind":"access","outcome"}. On success @p id and @p kind name it and @p answer is its members less those
 * two, typed. False for a record that is malformed, of an unknown kind, or with a member missing or of another type.
 */
bool
read_file_record(const QString& text, qint64& id, QString& kind, QVariantMap& answer) {
    const QJsonObject record = QJsonDocument::fromJson(text.toUtf8()).object();
    kind = record.value(QStringLiteral("kind")).toString();
    if (!whole_number(record.value(QStringLiteral("id")), id) || id <= 0) {
        return false;
    }
    if (kind == QLatin1String("roots")) {
        return read_roots(record, answer);
    }
    if (kind == QLatin1String("listing")) {
        return read_listing(record, answer);
    }
    if (kind == QLatin1String("handoff")) {
        return copy_fields(record,
                           {{"ok", FileField::Bool},
                            {"reference", FileField::String},
                            {"name", FileField::String},
                            {"reason", FileField::String}},
                           answer);
    }
    return kind == QLatin1String("access") && copy_word(record, "outcome", answer);
}

/* Reads @p text, FileBrowser.accessState(), into the host's fileAccess() map; false when it is malformed. */
bool
read_file_access(const QString& text, QVariantMap& access) {
    const QJsonObject state = QJsonDocument::fromJson(text.toUtf8()).object();
    const QJsonValue declared = state.value(QStringLiteral("declared"));
    const QJsonValue granted = state.value(QStringLiteral("granted"));
    const QString route = state.value(QStringLiteral("route")).toString();
    const QJsonValue apiLevel = state.value(QStringLiteral("apiLevel"));
    qint64 generation = 0;
    const bool routeKnown =
        route == QLatin1String("settings") || route == QLatin1String("permission") || route == QLatin1String("none");
    if (!declared.isBool() || !granted.isBool() || !routeKnown || !apiLevel.isDouble() || apiLevel.toInt(-1) < 0
        || !whole_number(state.value(QStringLiteral("rootsGeneration")), generation) || generation < 0) {
        return false;
    }
    // The same types as the base host's reading, so an unchanged reading compares equal.
    access = {{QStringLiteral("declared"), declared.toBool()},
              {QStringLiteral("granted"), granted.toBool()},
              {QStringLiteral("route"), route},
              {QStringLiteral("apiLevel"), apiLevel.toInt()},
              {QStringLiteral("rootsGeneration"), generation}};
    return true;
}

} // namespace

void
DecoderHostAndroid::refreshFileAccess() {
    readFileAccess();
}

void
DecoderHostAndroid::startFileBrowser() {
    platformStartFileBrowser();
    /* FileBrowser is the process's, while ids are this host's and start again in a new one: what a host before this
     * one left waiting is never this host's answer. */
    QString stale;
    do {
        stale = platformPollFileBrowser();
    } while (!stale.isEmpty());
    readFileAccess();
}

void
DecoderHostAndroid::endFileBrowser() {
    // Its work stops, and the records its cancels write wait for the next host's start to discard them.
    QList<qint64> requests = m_file_requests.keys();
    std::sort(requests.begin(), requests.end());
    m_file_requests.clear();
    for (const qint64 id : requests) {
        platformCancelFileRequest(id);
    }
    QList<qint64> sessions = m_file_sessions.values();
    std::sort(sessions.begin(), sessions.end());
    m_file_sessions.clear();
    for (const qint64 session : sessions) {
        platformCloseFileSession(session);
    }
}

qint64
DecoderHostAndroid::openFileBrowserSession() {
    const qint64 session = DecoderHost::openFileBrowserSession();
    m_file_sessions.insert(session);
    return session;
}

void
DecoderHostAndroid::closeFileBrowserSession(qint64 sessionId) {
    m_file_sessions.remove(sessionId);
    // The base host retires each live request first, which cancels it in FileBrowser (fileRequestRetired()).
    DecoderHost::closeFileBrowserSession(sessionId);
    platformCloseFileSession(sessionId);
}

qint64
DecoderHostAndroid::requestFileRoots(qint64 sessionId) {
    return startFileRequest(beginFileRequest(sessionId, FileRequestKind::Roots), FileRequestKind::Roots,
                            [sessionId](qint64 id) { return platformListRoots(id, sessionId); });
}

qint64
DecoderHostAndroid::requestFolderListing(qint64 sessionId, const QString& rootKey, qint64 generation,
                                         const QString& path, const QVariantMap& filter) {
    const QString kind = filter.value(QStringLiteral("kind")).toString();
    const bool showAll = filter.value(QStringLiteral("showAll")).toBool();
    return startFileRequest(
        beginFileRequest(sessionId, FileRequestKind::Listing), FileRequestKind::Listing,
        [&](qint64 id) { return platformListFolder(id, sessionId, rootKey, generation, path, kind, showAll); });
}

qint64
DecoderHostAndroid::requestMorePage(qint64 sessionId, qint64 snapshot, int offset) {
    return startFileRequest(
        beginFileRequest(sessionId, FileRequestKind::Page), FileRequestKind::Page,
        [sessionId, snapshot, offset](qint64 id) { return platformListMore(id, sessionId, snapshot, offset); });
}

qint64
DecoderHostAndroid::requestFileHandoff(qint64 sessionId, const QString& rootKey, qint64 generation,
                                       const QString& reference) {
    return startFileRequest(
        beginFileRequest(sessionId, FileRequestKind::Handoff), FileRequestKind::Handoff,
        [&](qint64 id) { return platformCheckHandoff(id, sessionId, rootKey, generation, reference); });
}

qint64
DecoderHostAndroid::requestFileAccess(qint64 sessionId) {
    const qint64 joined = liveFileAccessRequest();
    const qint64 id = beginFileRequest(sessionId, FileRequestKind::Access);
    if (id == joined) {
        return id; // The live access request, already with FileBrowser.
    }
    return startFileRequest(id, FileRequestKind::Access,
                            [](qint64 request) { return platformRequestFileAccess(request); });
}

void
DecoderHostAndroid::fileRequestRetired(qint64 requestId) {
    if (m_file_requests.remove(requestId)) {
        platformCancelFileRequest(requestId);
    }
}

qint64
DecoderHostAndroid::startFileRequest(qint64 id, FileRequestKind kind, const std::function<bool(qint64)>& handOver) {
    if (!fileRequestLive(id)) {
        return id; // A closed or unknown session's: answered "cancelled" already.
    }
    if (handOver(id)) {
        m_file_requests.insert(id, kind);
        return id;
    }
    settleFileRequestLater(id, fileFailure(kind, QStringLiteral("failed")));
    return id;
}

void
DecoderHostAndroid::readFileAccess() {
    QVariantMap access;
    if (!read_file_access(platformFileAccessState(), access) || access == m_file_access) {
        return; // A malformed reading keeps the last one.
    }
    m_file_access = access;
    Q_EMIT fileAccessChanged();
}

void
DecoderHostAndroid::refreshFileBrowser() {
    if (platformTakeFileAccessRefresh()) {
        readFileAccess();
    }
    for (QString record = platformPollFileBrowser(); !record.isEmpty(); record = platformPollFileBrowser()) {
        deliverFileRecord(record);
    }
}

void
DecoderHostAndroid::deliverFileRecord(const QString& text) {
    qint64 id = 0;
    QString kind;
    QVariantMap answer;
    if (!read_file_record(text, id, kind, answer)) {
        return;
    }
    const auto found = m_file_requests.constFind(id);
    if (found == m_file_requests.constEnd()) {
        return; // Retired, failed or answered already: its answer was given.
    }
    QLatin1String expected("listing"); // A listing's or a page's
    switch (*found) {
        case FileRequestKind::Roots: expected = QLatin1String("roots"); break;
        case FileRequestKind::Handoff: expected = QLatin1String("handoff"); break;
        case FileRequestKind::Access: expected = QLatin1String("access"); break;
        case FileRequestKind::Listing:
        case FileRequestKind::Page: break;
    }
    if (kind != expected) {
        return;
    }
    if (*found == FileRequestKind::Access && platformTakeFileAccessRefresh()) {
        /* FileBrowser raises the flag before it writes an access answer, and both may have come after the take at the
         * top of the drain: read again, so a "granted" answer never finds fileAccess() stale. */
        readFileAccess();
    }
    // Forgotten before the signal, whose slot may make, cancel or close requests.
    m_file_requests.erase(found);
    settleFileRequest(id, answer);
}

} // namespace dsd_android
