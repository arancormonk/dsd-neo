// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* The backup archive's public surface (issue #580): the typed path table, error names, the summary map, the snapshot
 * and the passphrase and seal wrappers. Export lives in backup_archive_build.cpp, restore validation in
 * backup_archive_parse.cpp, the rules both share in backup_archive_rules.cpp, and the incoming-document classifier in
 * backup_archive_incoming.cpp. */

#include "backup_archive.h"

#include <QChar>
#include <QFileInfo>
#include <QLatin1String>
#include <dsd-neo/app_control/signal_search.h>
#include <dsd-neo/crypto/backup_seal.h>
#include <dsd-neo/runtime/git_ver.h>
#include <dsd-neo/runtime/signal_search.h>
#include <stddef.h>
#include <stdint.h>
#include <utility>
#include <vector>
#include "app_prefs.h"
#include "backup_archive_rules.h"
#include "json_store.h"
#include "realtime_clock.h"
#include "settings_recovery.h"

namespace dsd_qt {
namespace backup {

QStringList
backup_store_names() {
    return {QLatin1String(kSavedSystemsStore), QLatin1String(kScanListsStore), QLatin1String(kImportedFilesStore),
            QLatin1String(kDecryptionProfilesStore), QLatin1String(kSignalSearchRangesStore)};
}

QString
backup_error_name(BackupError error) {
    switch (error) {
        case BackupError::None: return QStringLiteral("None");
        case BackupError::NotBackup: return QStringLiteral("NotBackup");
        case BackupError::NewerVersion: return QStringLiteral("NewerVersion");
        case BackupError::WrongPassphrase: return QStringLiteral("WrongPassphrase");
        case BackupError::TooLarge: return QStringLiteral("TooLarge");
        case BackupError::Damaged: return QStringLiteral("Damaged");
        case BackupError::NoSpace: return QStringLiteral("NoSpace");
        case BackupError::SessionActive: return QStringLiteral("SessionActive");
        case BackupError::Io: return QStringLiteral("Io");
        case BackupError::SourceChanged: return QStringLiteral("SourceChanged");
        case BackupError::StagingFailed: return QStringLiteral("StagingFailed");
        case BackupError::Unreadable: return QStringLiteral("Unreadable");
    }
    return QStringLiteral("Io");
}

BackupError
backup_error_from_open_status(int status) {
    if (status == DSD_BACKUP_ERR_ARGS || status == DSD_BACKUP_ERR_INTERNAL) {
        return BackupError::StagingFailed;
    }
    return backup_error_from_seal_status(status);
}

BackupError
backup_error_from_seal_status(int status) {
    switch (status) {
        case DSD_BACKUP_OK: return BackupError::None;
        case DSD_BACKUP_ERR_NOT_BACKUP: return BackupError::NotBackup;
        case DSD_BACKUP_ERR_VERSION: return BackupError::NewerVersion;
        case DSD_BACKUP_ERR_AUTH: return BackupError::WrongPassphrase;
        case DSD_BACKUP_ERR_TOO_LARGE: return BackupError::TooLarge;
        case DSD_BACKUP_ERR_FORMAT:
        case DSD_BACKUP_ERR_PARAMS: return BackupError::Damaged;
        default: return BackupError::Io;
    }
}

QVariantMap
summary_to_map(const Summary& summary) {
    return {{QStringLiteral("systems"), summary.systems},
            {QStringLiteral("scanLists"), summary.scanLists},
            {QStringLiteral("libraryFiles"), summary.libraryFiles},
            {QStringLiteral("profiles"), summary.profiles},
            {QStringLiteral("ranges"), summary.ranges},
            {QStringLiteral("avoids"), summary.avoids},
            {QStringLiteral("recordingsToChoose"), summary.recordingsToChoose},
            {QStringLiteral("systemsWithExtraArgs"), summary.systemsWithExtraArgs},
            {QStringLiteral("appExtraArgs"), summary.appExtraArgs},
            {QStringLiteral("notIncluded"), summary.notIncluded},
            {QStringLiteral("createdAt"), summary.createdAt},
            {QStringLiteral("app"), summary.app}};
}

namespace {

/* The files a backup carries that are not JSON stores, appended to @p unreadable when they cannot be read now. */
void
append_unreadable_native(QStringList* unreadable) {
    // Only an avoid file still in place unread: once kept aside, the in-memory list is what the store saves.
    dsd_app_search_file_load avoidsLoad = DSD_APP_SEARCH_FILE_OK;
    if (dsd_app_signal_search_store_file_state(DSD_APP_SEARCH_STORE_AVOIDS, &avoidsLoad, nullptr, nullptr, 0) == 0
        && avoidsLoad == DSD_APP_SEARCH_FILE_UNREADABLE) {
        unreadable->append(QLatin1String(kAvoidsFileName));
    }
    if (settings_recovery_scratch(settings_recovery_prefs_file_name())) {
        unreadable->append(settings_recovery_prefs_file_name());
    }
}

} // namespace

QStringList
backup_unreadable_files() {
    QStringList unreadable;
    for (const QString& store : backup_store_names()) {
        if (json_store_read(store).status == JsonStoreLoad::Unreadable) {
            unreadable.append(store);
        }
    }
    append_unreadable_native(&unreadable);
    return unreadable;
}

Snapshot
backup_take_snapshot() {
    Snapshot snapshot;
    snapshot.root = json_store_root();
    snapshot.canonicalRoot = QFileInfo(snapshot.root).canonicalFilePath();
    snapshot.prefs = AppPrefs::readPortable();
    // Read, never loaded: a load would mark a store this snapshot only looks at, and its next save would move a good
    // file aside after a passing read failure.
    for (const QString& store : backup_store_names()) {
        const JsonStoreRead read = json_store_read(store);
        if (read.status == JsonStoreLoad::Unreadable) {
            snapshot.unreadable.append(store);
        }
        snapshot.stores.insert(store, read.array);
    }
    append_unreadable_native(&snapshot.unreadable);
    std::vector<uint32_t> avoids(DSD_SEARCH_MAX_AVOIDS);
    const size_t count = dsd_app_signal_search_avoids_get(avoids.data(), avoids.size());
    for (size_t i = 0; i < count; ++i) {
        snapshot.avoids.append(avoids[i]);
    }
    snapshot.app = QString::fromUtf8(GIT_TAG);
    snapshot.createdAt = realtimeSecsSinceEpoch();
    return snapshot;
}

QByteArray
backup_passphrase_bytes(const QString& passphrase) {
    QString composed = passphrase.normalized(QString::NormalizationForm_C);
    QByteArray bytes = composed.toUtf8();
    detail::wipe_own(composed);
    return bytes;
}

bool
backup_passphrase_ok(const QString& passphrase) {
    QString composed = passphrase.normalized(QString::NormalizationForm_C);
    qsizetype points = 0;
    for (qsizetype i = 0; i < composed.size(); ++i) {
        if (composed.at(i).isHighSurrogate() && i + 1 < composed.size() && composed.at(i + 1).isLowSurrogate()) {
            ++i;
        }
        ++points;
    }
    detail::wipe_own(composed);
    return points >= kMinPassphraseChars;
}

QByteArray
backup_seal(const QByteArray& archive, const QString& passphrase, const dsd_backup_kdf_params& kdf, int* status) {
    QByteArray pass = backup_passphrase_bytes(passphrase);
    const size_t capacity = dsd_backup_sealed_size(static_cast<size_t>(archive.size()));
    QByteArray sealed;
    int rc = DSD_BACKUP_ERR_TOO_LARGE;
    if (capacity != 0U) {
        sealed.resize(static_cast<qsizetype>(capacity));
        size_t length = 0;
        rc = dsd_backup_seal(reinterpret_cast<const uint8_t*>(pass.constData()), static_cast<size_t>(pass.size()), &kdf,
                             reinterpret_cast<const uint8_t*>(archive.constData()), static_cast<size_t>(archive.size()),
                             reinterpret_cast<uint8_t*>(sealed.data()), capacity, &length);
        sealed.resize(rc == DSD_BACKUP_OK ? static_cast<qsizetype>(length) : 0);
    }
    detail::wipe(pass);
    if (status != nullptr) {
        *status = rc;
    }
    return sealed;
}

QByteArray
backup_open(const QByteArray& sealed, const QString& passphrase, int* status) {
    size_t payload = 0;
    // The check that tells a file without the magic (NOT_BACKUP) from a damaged backup (FORMAT).
    int rc = dsd_backup_check_header(reinterpret_cast<const uint8_t*>(sealed.constData()),
                                     static_cast<size_t>(sealed.size()), static_cast<uint64_t>(sealed.size()), nullptr,
                                     &payload);
    QByteArray archive;
    if (rc == DSD_BACKUP_OK) {
        QByteArray pass = backup_passphrase_bytes(passphrase);
        archive.resize(static_cast<qsizetype>(payload));
        size_t length = 0;
        rc = dsd_backup_open(reinterpret_cast<const uint8_t*>(pass.constData()), static_cast<size_t>(pass.size()),
                             reinterpret_cast<const uint8_t*>(sealed.constData()), static_cast<size_t>(sealed.size()),
                             reinterpret_cast<uint8_t*>(archive.data()), payload, &length);
        detail::wipe(pass);
        if (rc == DSD_BACKUP_OK) {
            archive.resize(static_cast<qsizetype>(length));
        } else {
            detail::wipe(archive);
        }
    }
    if (status != nullptr) {
        *status = rc;
    }
    return archive;
}

} // namespace backup
} // namespace dsd_qt
