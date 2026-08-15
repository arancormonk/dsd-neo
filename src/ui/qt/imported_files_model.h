// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Library of imported CSV files (channel maps, talkgroups, keys), persisted as JSON.
 *
 * The wizard's pickers and the imports screen share this model: importing copies
 * the picked document into durable app storage through DecoderHost::importDocument(),
 * dry-run validates it for row counts, and records one library row per stored file.
 * Saved systems reference stored paths, so one file can serve several systems.
 */

#ifndef DSD_NEO_SRC_UI_QT_IMPORTED_FILES_MODEL_H_
#define DSD_NEO_SRC_UI_QT_IMPORTED_FILES_MODEL_H_

#include <QAbstractListModel>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <Qt>
#include <QtGlobal>

namespace dsd_qt {

class DecoderHost;

class ImportedFilesModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

  public:
    /* Append new roles; never insert. A delegate that binds by number would
     * silently start reading a different column. */
    enum Roles {
        NameRole = Qt::UserRole + 1,
        PathRole,         // absolute stored path; the row's identity
        TypeRole,         // "chan" | "group" | "keysDec" | "keysHex"
        ImportedAtRole,   // seconds since epoch
        AcceptedRole,     // usable rows at the last validation
        SkippedRole,      // malformed rows at the last validation
        OriginRole,       // "" for a picked file, "radioreference" for a generated one
        RrSidRole,        // RadioReference system id
        RrSiteNumberRole, // TrsSite.siteNumber, the RF site - never siteId
        RrKindRole,       // "group" | "chan"
        RrSiteNumbersRole // every selected siteNumber, comma-joined, in selection order
    };

    explicit ImportedFilesModel(DecoderHost* host, QObject* parent = nullptr);
    ~ImportedFilesModel() override;

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int
    count() const {
        return static_cast<int>(m_rows.size());
    }

    /**
     * @brief Copy a picked document into the library, validate it, record a row.
     *
     * @return {ok, path, name, accepted, skipped, error}. error is "" on success,
     *         "open" when the copy or parse failed (no row is added), or "empty"
     *         when the file parsed but no row was usable — the row is kept, so
     *         the user can fix the file and update, but the UI should warn.
     */
    Q_INVOKABLE QVariantMap importFile(const QString& reference, const QString& fileName, const QString& type);

    /**
     * @brief Re-pick flow: replace the row's stored file and re-validate.
     *
     * The pick is staged beside the library and validated BEFORE it is committed
     * over the row's file, so a file that is not parseable as the row's type
     * leaves the stored copy byte-identical. Every saved system referencing that
     * path keeps working.
     */
    Q_INVOKABLE QVariantMap updateFile(int row, const QString& reference, const QString& fileName);

    /**
     * @brief Adopt a file this process generated, recording where it came from.
     *
     * Same validate/rollback/persist flow as importFile(), but the source is a
     * plain path we wrote rather than a picker reference.
     *
     * @param sourcePath Absolute path of the generated file.
     * @param fileName   Display name to store it under.
     * @param type       "chan" | "group" | "keysDec" | "keysHex".
     * @param origin     Provenance: {origin, rrSid, rrSiteNumber, rrSiteNumbers, rrKind}.
     * @return Same shape as importFile().
     */
    Q_INVOKABLE QVariantMap importGeneratedFile(const QString& sourcePath, const QString& fileName, const QString& type,
                                                const QVariantMap& origin);

    /**
     * @brief Replace a generated row's file in place, keeping its stored path.
     *
     * The path is preserved so saved systems pointing at it stay valid, and the
     * staging file is validated before the stored copy is touched: a refresh that
     * fetched a fault page or a truncated body must not destroy working local
     * data.
     *
     * @param row        Library row.
     * @param sourcePath Absolute path of the freshly generated file.
     * @return Same shape as importFile().
     */
    Q_INVOKABLE QVariantMap refreshGeneratedFile(int row, const QString& sourcePath);

    /** @brief Delete the stored file, then the row. Persists immediately. */
    Q_INVOKABLE void remove(int row);

    /** @brief One library row as a field map. */
    Q_INVOKABLE QVariantMap get(int row) const;

    /** @brief Rows of one type, for the wizard's picker sheets. */
    Q_INVOKABLE QVariantList entriesForType(const QString& type) const;

    /** @brief Row index whose stored path matches, or -1. */
    Q_INVOKABLE int rowForPath(const QString& path) const;

    /**
     * @brief Stored paths dropped by load() because the file was gone, then forgets them.
     *
     * Pruning a ghost row is only half the repair: saved systems reference stored
     * paths, and one left pointing at a deleted file makes the session fail to
     * start on `-G <missing>` with a parse error that names the input settings
     * rather than the CSV. The owner has to hand these to
     * SavedSystemsModel::clearCsvPath(), which is what the library's own remove
     * flow does. A signal cannot carry them: load() runs from the constructor,
     * before anything could have connected to one.
     */
    Q_INVOKABLE QStringList takePrunedPaths();

  Q_SIGNALS:
    void countChanged();

  private:
    struct Row {
        QString name;
        QString path;
        QString type;
        qint64 importedAt = 0;
        int accepted = 0;
        int skipped = 0;
        /* Provenance, absent on a picked file. Stores written before this existed
         * load unchanged: rowFromMap reads through QVariantMap::value, which
         * default-constructs a missing key. */
        QString origin;
        int rrSid = 0;
        int rrSiteNumber = 0;
        QString rrKind;
        /* Every selected site, comma-joined in selection order. rrSiteNumber
         * alone records only the first, which is exact for a trunked import but
         * loses the rest of a conventional repeater selection — a refresh driven
         * by it would silently shrink a scan list to one row. */
        QString rrSiteNumbers;
    };

    static Row rowFromMap(const QVariantMap& map);
    static QVariantMap mapFromRow(const Row& row);

    /** @brief Dry-run validate @p path as @p type; false when it cannot be parsed. */
    static bool validate(const QString& path, const QString& type, int* accepted, int* skipped);

    /**
     * @brief Validate an already-stored copy and record a library row for it.
     *
     * Shared tail of importFile() and importGeneratedFile(): on a validation
     * failure the copy is taken back out, because a row that cannot be parsed is
     * a dead entry and nothing else would ever delete the file.
     */
    QVariantMap adoptStoredFile(const QString& path, const QString& type, const QVariantMap& origin);

    /** @brief Shared tail of updateFile() and refreshGeneratedFile(): stamp the
     *         row's new counts, notify, persist, and build the result map. */
    QVariantMap commitReplacedRow(int row, int accepted, int skipped);

    void load();
    void save() const;

    DecoderHost* m_host = nullptr;
    QList<Row> m_rows;
    QStringList m_pruned_paths;
};

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_IMPORTED_FILES_MODEL_H_ */
