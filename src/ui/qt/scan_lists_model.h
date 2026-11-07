// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DSD_NEO_UI_QT_SCAN_LISTS_MODEL_H
#define DSD_NEO_UI_QT_SCAN_LISTS_MODEL_H
#include <QAbstractListModel>
#include <QList>
#include <QMap>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantMap>
#include <functional>
#include <utility>

namespace dsd_qt {
class ScanListsModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
  public:
    explicit ScanListsModel(QObject* parent = nullptr);
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int
    count() const {
        return m_rows.size();
    }

    Q_INVOKABLE QVariantMap get(int row) const;
    Q_INVOKABLE int rowForUid(const QString& uid) const;

    Q_INVOKABLE QVariantMap
    getByUid(const QString& uid) const {
        return get(rowForUid(uid));
    }

    Q_INVOKABLE QVariantMap newDraft() const;
    Q_INVOKABLE QString newEntryId() const;
    Q_INVOKABLE bool add(const QVariantMap& list);
    Q_INVOKABLE bool update(int row, const QVariantMap& list);
    Q_INVOKABLE bool remove(int row);
    /** @brief Names of the lists that name @p path in any CSV field: target CSV or a fallback file. */
    Q_INVOKABLE QStringList listsReferencingPath(const QString& path) const;
    /** @brief Names of the lists that run from target CSV @p path; clearing it makes them drafts. */
    Q_INVOKABLE QStringList listsTargetingPath(const QString& path) const;
    /** @brief Names of the lists with @p path as fallback talkgroups or radio IDs; clearing just empties those. */
    Q_INVOKABLE QStringList listsFallingBackOnPath(const QString& path) const;
    /**
     * @brief Blank every CSV field matching @p path, in every list. Persists first.
     *
     * A list that ran from @p path as its target CSV becomes a draft; a fallback file is emptied, and the list runs
     * without it. Returns false, with every list unchanged, when the save fails, and true when nothing named @p path.
     */
    Q_INVOKABLE bool clearCsvPath(const QString& path);
    /** @brief Every CSV path the lists name (target CSVs and fallback files), each once: what clearCsvPath() clears. */
    QStringList csvPaths() const;
    Q_INVOKABLE void touch(int row);
    /**
     * @brief How many targets @p list holds as Home counts them (issue #697): its entries, or for a list over an
     * imported target CSV the rows that file's import accepted (setAcceptedRowsLookup()), 0 when the file is gone.
     */
    Q_INVOKABLE int entryCount(const QVariantMap& list) const;

    /** @brief How entryCount() reads an imported target CSV's accepted rows by path: -1 for no such file. */
    void
    setAcceptedRowsLookup(std::function<int(const QString&)> lookup) {
        m_acceptedRows = std::move(lookup);
    }

    /** @brief Whether the load gave a list a new UID (none stored, or one another list had). What a stored scan
     *  session names by UID (AppPrefs' companions, issue #697) may then be another list, so such a load forgets them
     *  (AppPrefs::forgetScanCompanions()). */
    bool
    uidsRepaired() const {
        return m_uidsRepaired;
    }

  Q_SIGNALS:
    void countChanged();

  private:
    bool save(const QList<QVariantMap>& rows) const;
    QList<QVariantMap> m_rows;
    bool m_uidsRepaired = false;
    std::function<int(const QString&)> m_acceptedRows;
};
} // namespace dsd_qt
#endif
