// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "decoder_host.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUrl>

namespace dsd_qt {

namespace {

QString
imports_dir_path() {
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/imports");
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
    const QString sourcePath = url.isLocalFile() ? url.toLocalFile() : reference;
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

    QString destination;
    if (!replacePath.isEmpty()) {
        const QString canonicalDir = QFileInfo(importsDir.absolutePath()).canonicalFilePath();
        const QString canonicalTarget = QFileInfo(replacePath).canonicalPath();
        if (!canonicalDir.isEmpty() && canonicalTarget == canonicalDir) {
            destination = replacePath;
        }
    }
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
    while (!source.atEnd()) {
        const QByteArray chunk = source.read(1 << 16);
        if (out.write(chunk) != chunk.size()) {
            out.cancelWriting();
            return QString();
        }
    }
    if (!out.commit()) {
        return QString();
    }
    return destination;
}

} // namespace dsd_qt
