// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "decoder_host.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QSaveFile>
#include <QUrl>

#include "json_store.h"

namespace dsd_qt {

namespace {

QString
imports_dir_path() {
    // Same root json_store uses, so the library index and the files it points at
    // can never end up under different app-data locations.
    return json_store_path(QStringLiteral("imports"));
}

/** @brief chan.csv, chan (2).csv, chan (3).csv … first name not already taken. */
QString
unique_destination(const QDir& dir, const QString& fileName) {
    if (!dir.exists(fileName)) {
        return dir.filePath(fileName);
    }
    const QFileInfo info(fileName);
    const QString base = info.completeBaseName();
    const QString suffix = info.suffix();
    for (int i = 2; i < 1000; i++) {
        const QString candidate = suffix.isEmpty() ? QStringLiteral("%1 (%2)").arg(base).arg(i)
                                                   : QStringLiteral("%1 (%2).%3").arg(base).arg(i).arg(suffix);
        if (!dir.exists(candidate)) {
            return dir.filePath(candidate);
        }
    }
    return QString();
}

/**
 * @brief @p replacePath when it names a file directly inside @p dir, else empty.
 *
 * Canonicalizes the *directory*, not the file: QFileInfo::canonicalPath() gives
 * up on a missing leaf and returns ".", which would silently turn an update of a
 * copy that vanished behind the app's back into a fresh unique copy the caller
 * then rejects and never cleans up. The Android host resolves the parent too, so
 * this keeps the two in step.
 */
QString
replace_destination(const QDir& dir, const QString& replacePath) {
    if (replacePath.isEmpty()) {
        return QString();
    }
    const QString canonicalDir = QFileInfo(dir.absolutePath()).canonicalFilePath();
    const QString canonicalTarget = QFileInfo(QFileInfo(replacePath).absolutePath()).canonicalFilePath();
    if (canonicalDir.isEmpty() || canonicalTarget != canonicalDir) {
        return QString();
    }
    return replacePath;
}

/**
 * @brief Copy @p source to @p out in chunks; false on any read or write error.
 *
 * Loops on the read result rather than atEnd(): a read error returns an empty
 * chunk without advancing pos(), so an atEnd()-driven loop would spin forever on
 * it, and a readable source QFile reports size 0 for (a FIFO, /proc) claims to be
 * at its end before a byte has been read.
 */
bool
copy_stream(QIODevice& source, QSaveFile& out) {
    QByteArray chunk(1 << 16, Qt::Uninitialized);
    for (;;) {
        const qint64 n = source.read(chunk.data(), chunk.size());
        if (n < 0) {
            out.cancelWriting();
            return false;
        }
        if (n == 0) {
            return true;
        }
        if (out.write(chunk.constData(), n) != n) {
            out.cancelWriting();
            return false;
        }
    }
}

} // namespace

DecoderHost::DecoderHost(QObject* parent) : QObject(parent) {
    /* sessionState(), and the three properties derived from it, default to a view of
     * isRunning() — whose change signal is runningChanged. A host that leaves that
     * default in place therefore never emits sessionStateChanged, so the monitoring
     * view would never appear and UiController would never clear the models between
     * runs, even though the engine was decoding the whole time. Forward it here so the
     * documented minimal host works. A host that owns a real state machine emits
     * sessionStateChanged itself; the extra emission is absorbed by the value
     * comparison in UiController::onSessionStateChanged and by QML binding equality. */
    connect(this, &DecoderHost::runningChanged, this, &DecoderHost::sessionStateChanged);
}

DecoderHost::~DecoderHost() = default;

QString
DecoderHost::importDocument(const QString& reference, const QString& fileName, const QString& replacePath) {
    const QUrl url(reference);
    return importLocalFile(url.isLocalFile() ? url.toLocalFile() : reference, fileName, replacePath);
}

QString
DecoderHost::importLocalFile(const QString& sourcePath, const QString& fileName, const QString& replacePath) {
    QFile source(sourcePath);
    if (!source.open(QIODevice::ReadOnly)) {
        return QString();
    }

    QDir importsDir(imports_dir_path());
    if (!importsDir.mkpath(QStringLiteral("."))) {
        return QString();
    }

    // The display name comes from the picker; keep only its last component so
    // it cannot navigate out of the imports directory.
    QString name = QFileInfo(fileName).fileName();
    if (name.isEmpty()) {
        name = QFileInfo(sourcePath).fileName();
    }
    if (name.isEmpty()) {
        return QString();
    }

    QString destination = replace_destination(importsDir, replacePath);
    if (destination.isEmpty()) {
        destination = unique_destination(importsDir, name);
    }
    if (destination.isEmpty()) {
        return QString();
    }

    // QSaveFile stages beside the target and renames over it on commit, so an
    // update never leaves a half-written CSV where a saved system points.
    QSaveFile out(destination);
    if (!out.open(QIODevice::WriteOnly)) {
        return QString();
    }
    if (!copy_stream(source, out) || !out.commit()) {
        return QString();
    }
    return destination;
}

} // namespace dsd_qt
