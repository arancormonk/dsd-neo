// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Rules the backup export and the restore validation share (issue #580). Module-private.
 *
 * The export sanitizes its stores so every backup passes the restore's validation, so both sides read their archive
 * path rules, their carried-file bookkeeping and each store's identity rules from here rather than from two copies.
 * Everything here is free of shared state and safe on a worker thread.
 */

#ifndef DSD_NEO_SRC_UI_QT_BACKUP_ARCHIVE_RULES_H_
#define DSD_NEO_SRC_UI_QT_BACKUP_ARCHIVE_RULES_H_

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QMap>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QtGlobal>
#include "backup_archive.h"

class QByteArray;

namespace dsd_qt {
namespace backup {
namespace detail {

constexpr char kMagic[] = "DNBA";
constexpr qsizetype kHeaderBytes = 8;
constexpr char kFormatName[] = "dsd-neo-backup";
constexpr int kMaxPathComponents = 6;
constexpr int kMaxSourceRoots = 2;
constexpr int kMaxAppTagChars = 256;

/** The typed path table, built once. */
const QList<TypedPathField>& typed_fields();

/** Whether @p field belongs to @p store. */
bool is_typed_field(const QString& store, const TypedPathField& field);

QString new_uid();

/** Wipe @p bytes (DSD_SECURE_ZERO) and empty it. */
void wipe(QByteArray& bytes);

/**
 * Wipe @p text's buffer when it is @p text's own (as a string normalized() just built is), then empty it. A buffer
 * shared with another string (normalized() hands back its input when that is already normalized) or static data is
 * left alone: it belongs to the caller.
 */
void wipe_own(QString& text);

/** A JSON value read the way the models read a stored field: through QVariant, so a number becomes its text. */
QString variant_string(const QJsonValue& value);

/** Whether @p id may name an owned bundle directory (csv_bundle_import.cpp's ownedRoot() rule). */
bool owned_name(const QString& id);

/** Whether @p name is a staging leftover (".staged-*", ".import*") a backup never carries. */
bool staging_name(const QString& name);

/** The comparison key for archive paths: equal for paths equal exactly, by case folding or by normalization. */
QString path_key(const QString& path);

/** Whether @p text holds a backslash or a control character, or is not valid UTF-16. */
bool unsafe_text(const QString& text);

/** Why @p path may not name a carried file, or empty when it may. */
QString archive_path_problem(const QString& path);

/** The bundle directory ("imports/bundles/<id>", exact case) a path lies below, or empty when it lies in none. */
QString bundle_of(const QString& path);

/**
 * The carried file table's paths, refusing anything a restore could not write as one distinct file.
 *
 * Clashes are found on folded keys (a case-insensitive or normalizing filesystem would merge them); which directory a
 * file lies in is answered exactly, as a case-sensitive filesystem lays it out.
 */
class CarriedSet {
  public:
    enum Outcome { Added, Present, Refused };

    Outcome add(const QString& path, QString* why);

    bool
    contains(const QString& path) const {
        return m_exact.contains(path);
    }

    /** Whether some carried file lies below the directory @p path, compared exactly. */
    bool
    holdsFilesUnder(const QString& path) const {
        return m_directories.contains(path);
    }

    const QStringList&
    paths() const {
        return m_paths;
    }

  private:
    QStringList m_paths;
    QSet<QString> m_exact, m_directories;      // exact
    QSet<QString> m_fileKeys, m_directoryKeys; // folded, for clashes only
};

/* Store identity: the loaders' own rules. The export repairs what a loader would repair; the restore refuses it. */
QJsonArray repair_systems(const QJsonArray& rows);
QJsonArray repair_scan_lists(const QJsonArray& rows);
QJsonArray repair_profiles(const QJsonArray& rows);
QJsonArray repair_ranges(const QJsonArray& rows);

/** Drop the compiled form and runtime references from a profile row. */
void strip_compiled(QJsonObject& row);

/** Validate @p rows of @p store against its model's load rules, normalizing them as the model would store them. */
bool validate_store(const QString& store, QJsonArray& rows, QString* why);

/** Counts for @p stores (createdAt, app and notIncluded are left for the caller). */
Summary summarize(const QMap<QString, QJsonArray>& stores, const QVariantMap& prefs, int avoids);

/** A row's name (or label) for display. */
QString row_label(const QJsonObject& row);

} // namespace detail
} // namespace backup
} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_BACKUP_ARCHIVE_RULES_H_ */
