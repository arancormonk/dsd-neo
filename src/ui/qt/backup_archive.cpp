// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* The backup archive's public surface (issue #580): the typed path table, error names, the summary map, the snapshot
 * and the passphrase and seal wrappers. Export lives in backup_archive_build.cpp, restore validation in
 * backup_archive_parse.cpp, and the rules both share in backup_archive_rules.cpp. */

#include "backup_archive.h"

#include <QChar>
#include <QFileInfo>
#include <QJsonObject>
#include <QJsonValue>
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
    }
    return QStringLiteral("Io");
}

BackupError
backup_error_from_seal_status(int status) {
    switch (status) {
        case DSD_BACKUP_OK: return BackupError::None;
        case DSD_BACKUP_ERR_FORMAT: return BackupError::NotBackup;
        case DSD_BACKUP_ERR_VERSION: return BackupError::NewerVersion;
        case DSD_BACKUP_ERR_AUTH: return BackupError::WrongPassphrase;
        case DSD_BACKUP_ERR_TOO_LARGE: return BackupError::TooLarge;
        case DSD_BACKUP_ERR_PARAMS: return BackupError::Damaged;
        default: return BackupError::Io;
    }
}

QList<TypedPathField>
typed_path_fields() {
    return detail::typed_fields();
}

QList<TypedPathValue>
typed_path_values(const QString& store, const QJsonArray& rows) {
    QList<TypedPathValue> values;
    for (int i = 0; i < rows.size(); ++i) {
        const QJsonObject row = rows.at(i).toObject();
        for (const auto& field : detail::typed_fields()) {
            const QJsonValue value = row.value(QLatin1String(field.field));
            if (detail::is_typed_field(store, field) && value.isString() && !value.toString().isEmpty()) {
                values.append({i, field, value.toString()});
            }
        }
    }
    return values;
}

QJsonArray
map_typed_path_values(const QString& store, const QJsonArray& rows,
                      const std::function<QString(const TypedPathValue&)>& map) {
    QJsonArray out = rows;
    for (const TypedPathValue& value : typed_path_values(store, rows)) {
        const QString mapped = map(value);
        if (mapped != value.value) {
            QJsonObject row = out.at(value.row).toObject();
            row.insert(QLatin1String(value.field.field), mapped);
            out.replace(value.row, row);
        }
    }
    return out;
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

Snapshot
backup_take_snapshot() {
    Snapshot snapshot;
    snapshot.root = json_store_root();
    snapshot.canonicalRoot = QFileInfo(snapshot.root).canonicalFilePath();
    snapshot.prefs = AppPrefs::readPortable();
    for (const QString& store : backup_store_names()) {
        snapshot.stores.insert(store, json_store_load_array(store));
    }
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
    return passphrase.normalized(QString::NormalizationForm_C).toUtf8();
}

bool
backup_passphrase_ok(const QString& passphrase) {
    const QString composed = passphrase.normalized(QString::NormalizationForm_C);
    qsizetype points = 0;
    for (qsizetype i = 0; i < composed.size(); ++i) {
        if (composed.at(i).isHighSurrogate() && i + 1 < composed.size() && composed.at(i + 1).isLowSurrogate()) {
            ++i;
        }
        ++points;
    }
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
    int rc = dsd_backup_parse_header(reinterpret_cast<const uint8_t*>(sealed.constData()),
                                     static_cast<size_t>(sealed.size()), nullptr, &payload);
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
