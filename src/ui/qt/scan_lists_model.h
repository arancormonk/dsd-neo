// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DSD_NEO_UI_QT_SCAN_LISTS_MODEL_H
#define DSD_NEO_UI_QT_SCAN_LISTS_MODEL_H
#include <QAbstractListModel>
#include <QVariantMap>

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

    Q_INVOKABLE void add(const QVariantMap& list);
    Q_INVOKABLE void update(int row, const QVariantMap& list);
    Q_INVOKABLE void remove(int row);
    Q_INVOKABLE void touch(int row);
  Q_SIGNALS:
    void countChanged();

  private:
    void save() const;
    QList<QVariantMap> m_rows;
};
} // namespace dsd_qt
#endif
