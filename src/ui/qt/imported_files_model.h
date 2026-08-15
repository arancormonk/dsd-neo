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
    enum Roles {
        NameRole = Qt::UserRole + 1,
        PathRole,       // absolute stored path; the row's identity
        TypeRole,       // "chan" | "group" | "keysDec" | "keysHex"
        ImportedAtRole, // seconds since epoch
        AcceptedRole,   // usable rows at the last validation
        SkippedRole     // malformed rows at the last validation
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

    /** @brief Re-pick flow: overwrite the row's stored file in place and re-validate. */
    Q_INVOKABLE QVariantMap updateFile(int row, const QString& reference, const QString& fileName);

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
    };

    static Row rowFromMap(const QVariantMap& map);
    static QVariantMap mapFromRow(const Row& row);

    /** @brief Dry-run validate @p path as @p type; false when it cannot be parsed. */
    static bool validate(const QString& path, const QString& type, int* accepted, int* skipped);

    void load();
    void save() const;

    DecoderHost* m_host = nullptr;
    QList<Row> m_rows;
    QStringList m_pruned_paths;
};

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_IMPORTED_FILES_MODEL_H_ */
