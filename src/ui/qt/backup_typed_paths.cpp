// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* The typed path table (issue #580): which store fields name files. Kept apart from the rest of the backup so the
 * import library's revision cleanup (#832) reads the same inventory without the backup's model dependencies. */

#include <QDir>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QLatin1String>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>
#include <functional>
#include <iterator>
#include "backup_archive.h"
#include "backup_archive_rules.h"
#include "json_store.h"

namespace dsd_qt {
namespace backup {
namespace detail {

namespace {

constexpr TypedPathField kTypedPathFields[] = {
    {kSavedSystemsStore, "chanCsvPath", PathKind::File},
    {kSavedSystemsStore, "groupCsvPath", PathKind::File},
    {kSavedSystemsStore, "keyCsvPath", PathKind::File},
    {kSavedSystemsStore, "p25BandplanCsvPath", PathKind::File},
    {kSavedSystemsStore, "srcCsvPath", PathKind::File},
    {kSavedSystemsStore, "filePath", PathKind::Recording},
    {kScanListsStore, "groupCsvPath", PathKind::File},
    {kScanListsStore, "srcCsvPath", PathKind::File},
    {kScanListsStore, "targetsCsvPath", PathKind::File},
    {kImportedFilesStore, "path", PathKind::File},
    {kImportedFilesStore, "bundleRoot", PathKind::BundleRoot},
    {kDecryptionProfilesStore, "legacyCsvPath", PathKind::File},
    {kDecryptionProfilesStore, "mappingCsvPath", PathKind::File},
    {kDecryptionProfilesStore, "vendorCsvPath", PathKind::File},
};

} // namespace

const QList<TypedPathField>&
typed_fields() {
    static const QList<TypedPathField> fields(std::begin(kTypedPathFields), std::end(kTypedPathFields));
    return fields;
}

bool
is_typed_field(const QString& store, const TypedPathField& field) {
    return store == QLatin1String(field.store);
}

} // namespace detail

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

QStringList
typed_file_paths(bool* ok) {
    QSet<QString> stores;
    for (const auto& field : detail::typed_fields()) {
        stores.insert(QLatin1String(field.store));
    }
    bool loaded = true;
    QStringList paths;
    for (const QString& store : stores) {
        // A backup only reads the stores, so nothing is marked.
        const JsonStoreRead read = json_store_read(store);
        loaded = loaded && read.status != JsonStoreLoad::Unreadable;
        for (const TypedPathValue& value : typed_path_values(store, read.array)) {
            if (value.field.kind == PathKind::File && QDir::isAbsolutePath(value.value)) {
                paths.append(QDir::cleanPath(value.value));
            }
        }
    }
    if (ok != nullptr) {
        *ok = loaded;
    }
    return paths;
}

} // namespace backup
} // namespace dsd_qt
