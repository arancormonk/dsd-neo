// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <QLatin1String>
#include <QList>
#include <QMap>
#include <QVariant>
#include <utility>
#include "decoder_host.h"

#include <QByteArray>
#include <QClipboard>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QIODevice>
#include <QSaveFile>
#include <QUrl>

#include "json_store.h"

#include <Qt>
#include <QtGlobal>

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
 * @brief Copy @p source to @p out in chunks, at most @p maxBytes of it (no limit when negative).
 *
 * Loops on the read result rather than atEnd(): a read error returns an empty
 * chunk without advancing pos(), so an atEnd()-driven loop would spin forever on
 * it, and a readable source QFile reports size 0 for (a FIFO, /proc) claims to be
 * at its end before a byte has been read. The cap is counted on what was read for
 * the same reason, not taken from the source's reported size.
 *
 * @param abort Optional; checked before every chunk. Once raised the copy fails.
 * @return A DecoderHost::DocumentCopy value (a failed write to @p out is DocumentLocalFailed); any failure cancels
 *         @p out.
 */
int
copy_stream_capped(QIODevice& source, QSaveFile& out, qint64 maxBytes, const std::atomic<bool>* abort = nullptr) {
    QByteArray chunk(1 << 16, Qt::Uninitialized);
    qint64 total = 0;
    for (;;) {
        if (abort != nullptr && abort->load()) {
            out.cancelWriting();
            return DecoderHost::DocumentFailed;
        }
        const qint64 n = source.read(chunk.data(), chunk.size());
        if (n < 0) {
            out.cancelWriting();
            return DecoderHost::DocumentFailed;
        }
        if (n == 0) {
            return DecoderHost::DocumentCopied;
        }
        total += n;
        if (maxBytes >= 0 && total > maxBytes) {
            out.cancelWriting();
            return DecoderHost::DocumentTooLarge;
        }
        if (out.write(chunk.constData(), n) != n) {
            out.cancelWriting();
            return DecoderHost::DocumentLocalFailed;
        }
    }
}

/** @brief Copy @p source to @p out in chunks; false on any read or write error. */
bool
copy_stream(QIODevice& source, QSaveFile& out) {
    return copy_stream_capped(source, out, -1) == DecoderHost::DocumentCopied;
}

/** @brief The local path a file:// URL or a plain absolute path names; empty for anything else (a content URI). */
QString
local_document_path(const QString& reference) {
    const QUrl url(reference);
    if (url.isLocalFile()) {
        return url.toLocalFile();
    }
    return QDir::isAbsolutePath(reference) ? reference : QString();
}

/** @brief Open @p out for writing, readable and writable by the owner only. */
bool
open_private(QSaveFile& out) {
    return out.open(QIODevice::WriteOnly) && out.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
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

int
DecoderHost::copyDocumentToFile(const QString& reference, const QString& destPath, qint64 maxBytes) const {
    const QString path = local_document_path(reference);
    if (path.isEmpty() || destPath.isEmpty() || maxBytes < 0 || !QFileInfo(path).isFile()) {
        return DocumentFailed;
    }
    QFile source(path);
    if (!source.open(QIODevice::ReadOnly)) {
        return DocumentFailed;
    }
    if (source.size() > maxBytes) {
        return DocumentTooLarge;
    }
    // QSaveFile writes beside the destination and renames on commit, so no
    // failure, cap included, can leave a partial file there. Every failure on
    // that side is this device's, not the document's.
    QSaveFile out(destPath);
    if (!open_private(out)) {
        return DocumentLocalFailed;
    }
    const int copied = copy_stream_capped(source, out, maxBytes, &m_documentTransfersAborted);
    if (copied != DocumentCopied) {
        return copied;
    }
    return out.commit() ? DocumentCopied : DocumentLocalFailed;
}

bool
DecoderHost::copyFileToDocument(const QString& sourcePath, const QString& reference) const {
    const QString path = local_document_path(reference);
    QFile source(sourcePath);
    if (path.isEmpty() || !QFileInfo(sourcePath).isFile() || !source.open(QIODevice::ReadOnly)) {
        return false;
    }
    QSaveFile out(path);
    return open_private(out) && copy_stream_capped(source, out, -1, &m_documentTransfersAborted) == DocumentCopied
           && out.commit();
}

void
DecoderHost::abortDocumentTransfers() {
    m_documentTransfersAborted.store(true);
}

QVariantMap
DecoderHost::documentInfo(const QString& path) const {
    const QFileInfo info(path);
    return {{"name", info.fileName()},
            {"exists", info.isFile()},
            {"sizeBytes", info.size()},
            {"modified", info.lastModified().toString(Qt::ISODate)}};
}

bool
DecoderHost::copyText(const QString& text) const {
    if (auto* clipboard = QGuiApplication::clipboard()) {
        clipboard->setText(text);
        return true;
    }
    return false;
}

QString
DecoderHost::licenseNotices() const {
    QStringList notices;
    for (const auto* name : {"LICENSE",
                             "COPYRIGHT",
                             "THIRD_PARTY.md",
                             "src/ui/qt/fonts/IBMPlex-LICENSE.txt",
                             "src/third_party/ezpwd/lesser.txt",
                             "pffft-LICENSE.txt",
                             "pffft-NOTICES.txt",
                             "tetra_crypto-LICENSE.txt",
                             "legal/status.md",
                             "legal/source-delivery.md",
                             "LICENSES/ISC.txt",
                             "LICENSES/Unlicense.txt",
                             "LICENSES/gqrx-scanner-MIT.txt",
                             "LICENSES/LicenseRef-BCH3-Noncommercial.txt",
                             "LICENSES/LicenseRef-Simon-Rockliff.txt",
                             "mbelib-neo/LICENSE",
                             "mbelib-neo/GPL-2.0-or-later.txt",
                             "mbelib-neo/ISC.txt",
                             "mbelib-neo/LicenseRef-PFFFT.txt",
                             "mbelib-neo/LicenseRef-ETSI-EN-300-395-2-data.txt",
                             "mbelib-neo/upstream-mbelib-attribution.md",
                             "mbelib-neo/legal/status.md",
                             "mbelib-neo/legal/patents.md",
                             "mbelib-neo/pffft/LICENSE.txt",
                             "mbelib-neo/pffft/PFFFT-NOTICES.txt"}) {
        QFile file(QStringLiteral(":/dsdneo/notices/") + QLatin1String(name));
        if (file.open(QIODevice::ReadOnly)) {
            notices.append(QLatin1String(name) + QStringLiteral("\n\n") + QString::fromUtf8(file.readAll()));
        }
    }
    for (const auto* name :
         {"libusb-LGPL-2.1-or-later.txt", "librtlsdr-GPL-2.0-or-later.txt", "libairspy-LICENSE.txt"}) {
        QFile file(QStringLiteral("assets:/doc/dsd-neo/licenses/") + QLatin1String(name));
        if (file.open(QIODevice::ReadOnly)) {
            notices.append(QString::fromUtf8(file.readAll()));
        }
    }
    return notices.join(QStringLiteral("\n\n"));
}

QStringList
DecoderHost::recordingRoots() const {
    // json_store's root, like the imports: one app data location for everything the app keeps. The store reads a root
    // whose parent folder is missing as storage that is not mounted, and on a first launch nothing has made the app
    // data folder yet.
    (void)QDir().mkpath(json_store_root());
    return {json_store_path(QStringLiteral("Call recordings"))};
}

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
    if (!open_private(out)) {
        return QString();
    }
    if (!copy_stream(source, out) || !out.commit()) {
        return QString();
    }
    return destination;
}

} // namespace dsd_qt
