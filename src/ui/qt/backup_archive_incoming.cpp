// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* The backup archive's incoming-document classifier (#831): what a document opened from another app is, from its first
 * bytes and its length. Apart from backup_archive.cpp so the import library (ImportedFilesModel's probes) links it
 * without the snapshot and its stores. */

#include <QByteArray>
#include <QString>
#include <QtGlobal>
#include <dsd-neo/crypto/backup_seal.h>
#include <stddef.h>
#include <stdint.h>
#include "backup_archive.h"

namespace dsd_qt {
namespace backup {

IncomingKind
classify_incoming(const QByteArray& header, qint64 total) {
    // An unknown length never matches header + payload + tag, so a header that passes everything else ends in FORMAT.
    const uint64_t length = total < 0 ? UINT64_MAX : static_cast<uint64_t>(total);
    const int status = dsd_backup_check_header(reinterpret_cast<const uint8_t*>(header.constData()),
                                               static_cast<size_t>(header.size()), length, nullptr, nullptr);
    if (status == DSD_BACKUP_ERR_NOT_BACKUP) {
        return IncomingKind::NotBackup;
    }
    // Before the version, as Restore refuses any file over the cap before it reads the header.
    if (total > kMaxSealedBytes) {
        return IncomingKind::TooLargeBackup;
    }
    if (status == DSD_BACKUP_ERR_VERSION) {
        return IncomingKind::NewerBackup;
    }
    if (status == DSD_BACKUP_ERR_TOO_LARGE) {
        return IncomingKind::TooLargeBackup;
    }
    return status == DSD_BACKUP_OK ? IncomingKind::Backup : IncomingKind::DamagedBackup;
}

QString
incoming_kind_name(IncomingKind kind) {
    switch (kind) {
        case IncomingKind::Backup: return QStringLiteral("backup");
        case IncomingKind::NewerBackup: return QStringLiteral("newerBackup");
        case IncomingKind::DamagedBackup: return QStringLiteral("damagedBackup");
        case IncomingKind::TooLargeBackup: return QStringLiteral("tooLargeBackup");
        case IncomingKind::NotBackup: return QStringLiteral("other");
    }
    return QStringLiteral("other");
}

} // namespace backup
} // namespace dsd_qt
